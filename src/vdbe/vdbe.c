/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** The code in this file implements the function that runs the
** bytecode of a prepared statement.
**
** Various scripts scan this source file in order to generate HTML
** documentation, headers files, or other derived files.  The formatting
** of the code in this file is, therefore, important.  See other comments
** in this file for details.  If in doubt, do not deviate from existing
** commenting and indentation practices when changing or adding code.
*/
#include "dbsql_int.h"
#include "vdbe_int.h"

/*
** High-resolution hardware timer used for debugging and testing only.
*/
#if defined(VDBE_PROFILE)  \
 || defined(DBSQL_PERFORMANCE_TRACE) \
 || defined(DBSQL_ENABLE_STMT_SCANSTATUS)
# include "hwtime.h"
#endif

/*
** Invoke this macro on memory cells just prior to changing the
** value of the cell.  This macro verifies that shallow copies are
** not misused.  A shallow copy of a string or blob just copies a
** pointer to the string or blob, not the content.  If the original
** is changed while the copy is still in use, the string or blob might
** be changed out from under the copy.  This macro verifies that nothing
** like that ever happens.
*/
#ifdef DBSQL_DEBUG
# define MEM_ABOUT_TO_CHANGE(P,M) __vdbe_mem_about_to_change(P,M)
#else
# define MEM_ABOUT_TO_CHANGE(P,M)
#endif

/*
** The following global variable is incremented every time a cursor
** moves, either by the __op_seek_xx, OP_Next, or OP_Prev opcodes.  The test
** procedures use this information to make sure that indices are
** working correctly.  This variable has no function other than to
** help verify the correct operation of the library.
*/
#ifdef DBSQL_TEST
int dbsql_search_count = 0;
#endif

/*
** When this global variable is positive, it gets decremented once before
** each instruction in the VDBE.  When it reaches zero, the u1.is_interrupted
** field of the DBSQL structure is set in order to simulate an interrupt.
**
** This facility is used for testing purposes only.  It does not function
** in an ordinary build.
*/
#ifdef DBSQL_TEST
int dbsql_interrupt_count = 0;
#endif

/*
** The next global variable is incremented each type the OP_Sort opcode
** is executed.  The test procedures use this information to make sure that
** sorting is occurring or not occurring at appropriate times.   This variable
** has no function other than to help verify the correct operation of the
** library.
*/
#ifdef DBSQL_TEST
int dbsql_sort_count = 0;
#endif

/*
** The next global variable records the size of the largest MEM_Blob
** or MEM_Str that has been used by a VDBE opcode.  The test procedures
** use this information to make sure that the zero-blob functionality
** is working correctly.   This variable has no function other than to
** help verify the correct operation of the library.
*/
#ifdef DBSQL_TEST
int dbsql_max_blobsize = 0;
static void __update_max_blobsize(mem_t *p){
  if( (p->flags & (MEM_Str|MEM_Blob))!=0 && p->n>dbsql_max_blobsize ){
    dbsql_max_blobsize = p->n;
  }
}
#endif

/*
** This macro evaluates to true if either the update hook or the preupdate
** hook are enabled for database connect DB.
*/
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
# define HAS_UPDATE_HOOK(DB) ((DB)->pre_update_callback||(DB)->update_callback)
#else
# define HAS_UPDATE_HOOK(DB) ((DB)->update_callback)
#endif

/*
** The next global variable is incremented each time the OP_Found opcode
** is executed. This is used to test whether or not the foreign key
** operation implemented using __op_fk_is_zero is working. This variable
** has no function other than to help verify the correct operation of the
** library.
*/
#ifdef DBSQL_TEST
int dbsql_found_count = 0;
#endif

/*
** Test a register to see if it exceeds the current maximum blob size.
** If it does, record the new maximum blob size.
*/
#if defined(DBSQL_TEST) && !defined(DBSQL_UNTESTABLE)
# define UPDATE_MAX_BLOBSIZE(P)  __update_max_blobsize(P)
#else
# define UPDATE_MAX_BLOBSIZE(P)
#endif

#ifdef DBSQL_DEBUG
/* This routine provides a convenient place to set a breakpoint during
** tracing with PRAGMA vdbe_trace=on.  The breakpoint fires right after
** each opcode is printed.  Variables "pc" (program counter) and op are
** available to add conditionals to the breakpoint.  GDB example:
**
**         break test_trace_breakpoint if pc=22
**
** Other useful labels for breakpoints include:
**   test_addop_breakpoint(pc,op)
**   __corrupt_error(lineno)
**   __misuse_error(lineno)
**   __cantopen_error(lineno)
*/
static void test_trace_breakpoint(int pc, op_t *op, vdbe_t *v){
  static u_int64_t n = 0;
  (void)pc;
  (void)op;
  (void)v;
  n++;
  if( n==LARGEST_UINT64 ) abort(); /* So that n is used, preventing a warning */
}
#endif

/*
** Invoke the VDBE coverage callback, if that callback is defined.  This
** feature is used for test suite validation only and does not appear an
** production builds.
**
** M is the type of branch.  I is the direction taken for this instance of
** the branch.
**
**   M: 2 - two-way branch (I=0: fall-thru   1: jump                )
**      3 - two-way + NULL (I=0: fall-thru   1: jump      2: NULL   )
**      4 - OP_Jump        (I=0: jump p1     1: jump p2   2: jump p3)
**
** In other words, if M is 2, then I is either 0 (for fall-through) or
** 1 (for when the branch is taken).  If M is 3, the I is 0 for an
** ordinary fall-through, I is 1 if the branch was taken, and I is 2
** if the result of comparison is NULL.  For M=3, I=2 the jump may or
** may not be taken, depending on the DBSQL_JUMPIFNULL flags in p5.
** When M is 4, that means that an OP_Jump is being run.  I is 0, 1, or 2
** depending on if the operands are less than, equal, or greater than.
**
** i_src_line is the source code line (from the __LINE__ macro) that
** generated the VDBE instruction combined with flag bits.  The source
** code line number is in the lower 24 bits of i_src_line and the upper
** 8 bytes are flags.  The lower three bits of the flags indicate
** values for I that should never occur.  For example, if the branch is
** always taken, the flags should be 0x05 since the fall-through and
** alternate branch are never taken.  If a branch is never taken then
** flags should be 0x06 since only the fall-through approach is allowed.
**
** Bit 0x08 of the flags indicates an OP_Jump opcode that is only
** interested in equal or not-equal.  In other words, I==0 and I==2
** should be treated as equivalent
**
** Since only a line number is retained, not the filename, this macro
** only works for amalgamation builds.  But that is ok, since these macros
** should be no-ops except for special builds used to measure test coverage.
*/
#if !defined(DBSQL_VDBE_COVERAGE)
# define VDBE_BRANCH_TAKEN(I,M)
#else
# define VDBE_BRANCH_TAKEN(I,M) __vdbe_take_branch(op->i_src_line,I,M)
  static void __vdbe_take_branch(u_int32_t i_src_line, u_int8_t I, u_int8_t M){
    u_int8_t m_never;
    assert( I<=2 );  /* 0: fall through,  1: taken,  2: alternate taken */
    assert( M<=4 );  /* 2: two-way branch, 3: three-way branch, 4: OP_Jump */
    assert( I<M );   /* I can only be 2 if M is 3 or 4 */
    /* Transform I from a integer [0,1,2] into a bitmask of [1,2,4] */
    I = 1<<I;
    /* The upper 8 bits of i_src_line are flags.  The lower three bits of
    ** the flags indicate directions that the branch can never go.  If
    ** a branch really does go in one of those directions, assert right
    ** away. */
    m_never = i_src_line >> 24;
    assert( (I & m_never)==0 );
    if( __global_config.vdbe_branch==0 ) return;  /*NO_TEST*/
    /* Invoke the branch coverage callback with three arguments:
    **    i_src_line - the line number of the VDBE_COVERAGE() macro, with
    **               flags removed.
    **    I        - Mask of bits 0x07 indicating which cases are are
    **               fulfilled by this instance of the jump.  0x01 means
    **               fall-thru, 0x02 means taken, 0x04 means NULL.  Any
    **               impossible cases (ex: if the comparison is never NULL)
    **               are filled in automatically so that the coverage
    **               measurement logic does not flag those impossible cases
    **               as missed coverage.
    **    M        - Type of jump.  Same as M argument above
    */
    I |= m_never;
    if( M==2 ) I |= 0x04;
    if( M==4 ){
      I |= 0x08;
      if( (m_never&0x08)!=0 && (I&0x05)!=0) I |= 0x05; /*NO_TEST*/
    }
    __global_config.vdbe_branch(__global_config.vdbe_branch_arg,
                                    i_src_line&0xffffff, I, M);
  }
#endif

/*
** An ephemeral string value (signified by the MEM_Ephem flag) contains
** a pointer to a dynamically allocated string where some other entity
** is responsible for deallocating that string.  Because the register
** does not control the string, it might be deleted without the register
** knowing it.
**
** This routine converts an ephemeral string into a dynamically allocated
** string that the register itself controls.  In other words, it
** converts an MEM_Ephem string into a string with P.z==P.z_malloc.
*/
#define DEEPHEMERALIZE(P) \
   if( ((P)->flags&MEM_Ephem)!=0 \
       && __vdbe_mem_make_writeable(P) ){ goto no_mem;}

/* Return true if the cursor was opened using the __op_open_sorter opcode. */
#define IS_SORTER(x) ((x)->cur_type==CURTYPE_SORTER)

/*
** Allocate vdbe_cursor_t number i_cur.  Return a pointer to it.  Return NULL
** if we run out of memory.
*/
static vdbe_cursor_t *__allocate_cursor(
  vdbe_t *p,              /* The virtual machine */
  int i_cur,             /* index_t of the new vdbe_cursor_t */
  int n_field,           /* Number of fields in the table or index */
  u_int8_t cur_type           /* Type of the new cursor */
){
  /* Find the memory cell that will be used to store the blob of memory
  ** required for this vdbe_cursor_t structure. It is convenient to use a
  ** vdbe memory cell to manage the memory allocation required for a
  ** vdbe_cursor_t structure for the following reasons:
  **
  **   * Sometimes cursor numbers are used for a couple of different
  **     purposes in a vdbe program. The different uses might require
  **     different sized allocations. Memory cells provide growable
  **     allocations.
  **
  **   * When using ENABLE_MEMORY_MANAGEMENT, memory cell buffers can
  **     be freed lazily via the dbsql_release_memory() API. This
  **     minimizes the number of malloc calls made by the system.
  **
  ** The memory cell for cursor 0 is mems[0]. The rest are allocated from
  ** the top of the register space.  Cursor 1 is at mem_t[p->n_mem-1].
  ** Cursor 2 is at mem_t[p->n_mem-2]. And so forth.
  */
  mem_t *mem = i_cur>0 ? &p->mems[p->n_mem-i_cur] : p->mems;

  int64_t n_byte;
  vdbe_cursor_t *cx = 0;
  n_byte = SZ_VDBECURSOR(n_field);
  assert( ROUND8(n_byte)==n_byte );
  if( cur_type==CURTYPE_BTREE ) n_byte += __sm_cursor_size();

  assert( i_cur>=0 && i_cur<p->n_cursor );
  if( p->csrs[i_cur] ){ /*OPTIMIZATION-IF-FALSE*/
    __vdbe_free_cursor_nn(p, p->csrs[i_cur]);
    p->csrs[i_cur] = 0;
  }

  /* There used to be a call to __vdbe_mem_clear_and_resize() to make sure
  ** the mem used to hold space for the cursor has enough storage available
  ** in mem->z_malloc.  But for the special case of the mems[] entries used
  ** to hold cursors, it is faster to in-line the logic. */
  assert( mem->flags==MEM_Undefined );
  assert( (mem->flags & MEM_Dyn)==0 );
  assert( mem->sz_malloc==0 || mem->z==mem->z_malloc );
  if( mem->sz_malloc<n_byte ){
    if( mem->sz_malloc>0 ){
      __db_free_nn(mem->db, mem->z_malloc);
    }
    mem->z = mem->z_malloc = __db_malloc_raw(mem->db, n_byte);
    if( mem->z_malloc==0 ){
      mem->sz_malloc = 0;
      return 0;
    }
    mem->sz_malloc = (int)n_byte;
  }

  p->csrs[i_cur] = cx = (vdbe_cursor_t*)mem->z_malloc;
  memset(cx, 0, offsetof(vdbe_cursor_t,alt_cursor));
  cx->cur_type = cur_type;
  cx->n_field = n_field;
  cx->offsets = &cx->types[n_field];
  if( cur_type==CURTYPE_BTREE ){
    assert( ROUND8(SZ_VDBECURSOR(n_field))==SZ_VDBECURSOR(n_field) );
    cx->uc.cursor = (sm_cursor_t*)&mem->z[SZ_VDBECURSOR(n_field)];
    __sm_cursor_zero(cx->uc.cursor);
  }
  return cx;
}

/*
** The string in rec is known to look like an integer and to have a
** floating point value of r_value.  Return true and set *pi_value to the
** integer value if the string is in range to be an integer.  Otherwise,
** return false.
*/
static int __also_an_int(mem_t *rec, double r_value, int64_t *pi_value){
  int64_t i_value;
  i_value = __real_to_i64(r_value);
  if( __real_same_as_int(r_value,i_value) ){
    *pi_value = i_value;
    return 1;
  }
  return 0==__atoi64(rec->z, pi_value, rec->n, rec->enc);
}

/*
** Try to convert a value into a numeric representation if we can
** do so without loss of information.  In other words, if the string
** looks like a number, convert it into a number.  If it does not
** look like a number, leave it alone.
**
** If the try_for_int flag is true, then extra effort is made to give
** an integer representation.  Strings that look like floating point
** values but which have no fractional component (example: '48.00')
** will have a MEM_Int representation when try_for_int is true.
**
** If try_for_int is false, then if the input string contains a decimal
** point or exponential notation, the result is only MEM_Real, even
** if there is an exact integer representation of the quantity.
*/
static void __apply_numeric_affinity(mem_t *rec, int try_for_int){
  double r_value;
  int rc;
  assert( (rec->flags & (MEM_Str|MEM_Int|MEM_Real|MEM_IntReal))==MEM_Str );
  rc = __mem_real_value_rc(rec, &r_value);
  if( rc<=0 ) return;
  if( (rc&2)==0 && __also_an_int(rec, r_value, &rec->u.i) ){
    rec->flags |= MEM_Int;
  }else{
    rec->u.r = r_value;
    rec->flags |= MEM_Real;
    if( try_for_int ) __vdbe_integer_affinity(rec);
  }
  /* TEXT->NUMERIC is many->one.  Hence, it is important to invalidate the
  ** string representation after computing a numeric equivalent, because the
  ** string representation might not be the canonical representation for the
  ** numeric value.  Ticket [343634942dd54ab57b7024] 2018-01-31. */
  rec->flags &= ~MEM_Str;
}

/*
** Processing is determine by the affinity parameter:
**
** DBSQL_AFF_INTEGER:
** DBSQL_AFF_REAL:
** DBSQL_AFF_NUMERIC:
**    Try to convert rec to an integer representation or a
**    floating-point representation if an integer representation
**    is not possible.  Note that the integer representation is
**    always preferred, even if the affinity is REAL, because
**    an integer representation is more space efficient on disk.
**
** DBSQL_AFF_FLEXNUM:
**    If the value is text, then try to convert it into a number of
**    some kind (integer or real) but do not make any other changes.
**
** DBSQL_AFF_TEXT:
**    Convert rec to a text representation.
**
** DBSQL_AFF_BLOB:
** DBSQL_AFF_NONE:
**    No-op.  rec is unchanged.
*/
static void __apply_affinity(
  mem_t *rec,          /* The value to apply affinity to */
  char affinity,      /* The affinity to be applied */
  u_int8_t enc              /* Use this text encoding */
){
  if( affinity>=DBSQL_AFF_NUMERIC ){
    assert( affinity==DBSQL_AFF_INTEGER || affinity==DBSQL_AFF_REAL
             || affinity==DBSQL_AFF_NUMERIC || affinity==DBSQL_AFF_FLEXNUM );
    if( (rec->flags & MEM_Int)==0 ){ /*OPTIMIZATION-IF-FALSE*/
      if( (rec->flags & (MEM_Real|MEM_IntReal))==0 ){
        if( rec->flags & MEM_Str ) __apply_numeric_affinity(rec,1);
      }else if( affinity<=DBSQL_AFF_REAL ){
        __vdbe_integer_affinity(rec);
      }
    }
  }else if( affinity==DBSQL_AFF_TEXT ){
    /* Only attempt the conversion to TEXT if there is an integer or real
    ** representation (blob and NULL do not get converted) but no string
    ** representation.  It would be harmless to repeat the conversion if
    ** there is already a string rep, but it is pointless to waste those
    ** CPU cycles. */
    if( 0==(rec->flags&MEM_Str) ){ /*OPTIMIZATION-IF-FALSE*/
      if( (rec->flags&(MEM_Real|MEM_Int|MEM_IntReal)) ){
        testcase( rec->flags & MEM_Int );
        testcase( rec->flags & MEM_Real );
        testcase( rec->flags & MEM_IntReal );
        __vdbe_mem_stringify(rec, enc, 1);
      }
    }
    rec->flags &= ~(MEM_Real|MEM_Int|MEM_IntReal);
  }
}

/*
** Try to convert the type of a function argument or a result column
** into a numeric representation.  Use either INTEGER or REAL whichever
** is appropriate.  But only do the conversion if it is possible without
** loss of information and return the revised type of the argument.
*/
int dbsql_value_numeric_type(dbsql_value_t *val){
  int type = dbsql_value_type(val);
  if( type==DBSQL_TEXT ){
    mem_t *mem = (mem_t*)val;
    assert( mem->db!=0 );
    dbsql_mutex_enter(mem->db->mutex);
    __apply_numeric_affinity(mem, 0);
    dbsql_mutex_leave(mem->db->mutex);
    type = dbsql_value_type(val);
  }
  return type;
}

/*
** Exported version of __apply_affinity(). This one works on dbsql_value_t*,
** not the internal mem_t* type.
*/
void __value_apply_affinity(
  dbsql_value_t *val,
  u_int8_t affinity,
  u_int8_t enc
){
  __apply_affinity((mem_t *)val, affinity, enc);
}

/*
** mem currently only holds a string type (or maybe a BLOB that we can
** interpret as a string if we want to).  Compute its corresponding
** numeric type, if has one.  Set the mem->u.r and mem->u.i fields
** accordingly.
*/
static u_int16_t DBSQL_NOINLINE __compute_numeric_type(mem_t *mem){
  int rc;
  int64_t ix;
  assert( (mem->flags & (MEM_Int|MEM_Real|MEM_IntReal))==0 );
  assert( (mem->flags & (MEM_Str|MEM_Blob))!=0 );
  if( EXPAND_BLOB(mem) ){
    mem->u.i = 0;
    return MEM_Int;
  }
  rc = __mem_real_value_rc(mem, &mem->u.r);
  if( rc<=0 ){
    if( (rc&2)==0 && __atoi64(mem->z, &ix, mem->n, mem->enc)<=1 ){
      mem->u.i = ix;
      return MEM_Int;
    }else{
      return MEM_Real;
    }
  }else if( (rc&2)==0 && __atoi64(mem->z, &ix, mem->n, mem->enc)==0 ){
    mem->u.i = ix;
    return MEM_Int;
  }
  return MEM_Real;
}

/*
** Return the numeric type for mem, either MEM_Int or MEM_Real or both or
** none. 
**
** Unlike __apply_numeric_affinity(), this routine does not modify mem->flags.
** But it does set mem->u.r and mem->u.i appropriately.
*/
static u_int16_t __numeric_type(mem_t *mem){
  assert( (mem->flags & MEM_Null)==0
       || mem->db==0 || mem->db->malloc_failed );
  if( mem->flags & (MEM_Int|MEM_Real|MEM_IntReal|MEM_Null) ){
    testcase( mem->flags & MEM_Int );
    testcase( mem->flags & MEM_Real );
    testcase( mem->flags & MEM_IntReal );
    return mem->flags & (MEM_Int|MEM_Real|MEM_IntReal|MEM_Null);
  }
  assert( mem->flags & (MEM_Str|MEM_Blob) );
  testcase( mem->flags & MEM_Str );
  testcase( mem->flags & MEM_Blob );
  return __compute_numeric_type(mem);
  return 0;
}

#ifdef DBSQL_DEBUG
/*
** Write a nice string representation of the contents of cell mem
** into buffer buf, length n_buf.
*/
void __vdbe_mem_pretty_print(mem_t *mem, str_accum_t *str){
  int f = mem->flags;
  static const char *const encnames[] = {"(X)", "(8)", "(16LE)", "(16BE)"};
  if( f&MEM_Blob ){
    int i;
    char c;
    if( f & MEM_Dyn ){
      c = 'z';
      assert( (f & (MEM_Static|MEM_Ephem))==0 );
    }else if( f & MEM_Static ){
      c = 't';
      assert( (f & (MEM_Dyn|MEM_Ephem))==0 );
    }else if( f & MEM_Ephem ){
      c = 'e';
      assert( (f & (MEM_Static|MEM_Dyn))==0 );
    }else{
      c = 's';
    }
    dbsql_str_appendf(str, "%cx[", c);
    for(i=0; i<25 && i<mem->n; i++){
      dbsql_str_appendf(str, "%02X", ((int)mem->z[i] & 0xFF));
    }
    dbsql_str_appendf(str, "|");
    for(i=0; i<25 && i<mem->n; i++){
      char z = mem->z[i];
      dbsql_str_appendchar(str, 1, (z<32||z>126)?'.':z);
    }
    dbsql_str_appendf(str,"]");
    if( f & MEM_Zero ){
      dbsql_str_appendf(str, "+%dz",mem->u.n_zero);
    }
  }else if( f & MEM_Str ){
    int j;
    u_int8_t c;
    if( f & MEM_Dyn ){
      c = 'z';
      assert( (f & (MEM_Static|MEM_Ephem))==0 );
    }else if( f & MEM_Static ){
      c = 't';
      assert( (f & (MEM_Dyn|MEM_Ephem))==0 );
    }else if( f & MEM_Ephem ){
      c = 'e';
      assert( (f & (MEM_Static|MEM_Dyn))==0 );
    }else{
      c = 's';
    }
    dbsql_str_appendf(str, " %c%d[", c, mem->n);
    for(j=0; j<25 && j<mem->n; j++){
      c = mem->z[j];
      dbsql_str_appendchar(str, 1, (c>=0x20&&c<=0x7f) ? c : '.');
    }
    dbsql_str_appendf(str, "]%s", encnames[mem->enc]);
    if( f & MEM_Term ){
      dbsql_str_appendf(str, "(0-term)");
    }
  }
}
#endif

#ifdef DBSQL_DEBUG
/*
** Print the value of a register for tracing purposes:
*/
static void __mem_trace_print(mem_t *p){
  if( p->flags & MEM_Undefined ){
    printf(" undefined");
  }else if( p->flags & MEM_Null ){
    printf(p->flags & MEM_Zero ? " NULL-nochng" : " NULL");
  }else if( (p->flags & (MEM_Int|MEM_Str))==(MEM_Int|MEM_Str) ){
    printf(" si:%lld", p->u.i);
  }else if( (p->flags & (MEM_IntReal))!=0 ){
    printf(" ir:%lld", p->u.i);
  }else if( p->flags & MEM_Int ){
    printf(" i:%lld", p->u.i);
#ifndef DBSQL_OMIT_FLOATING_POINT
  }else if( p->flags & MEM_Real ){
    printf(" r:%.17g", p->u.r);
#endif
  }else if( __vdbe_mem_is_row_set(p) ){
    printf(" (rowset)");
  }else{
    str_accum_t acc;
    char buf[1000];
    __str_accum_init(&acc, 0, buf, sizeof(buf), 0);
    __vdbe_mem_pretty_print(p, &acc);
    printf(" %s", __str_accum_finish(&acc));
  }
  if( p->flags & MEM_Subtype ) printf(" subtype=0x%02x", p->subtype);
}
static void __register_trace(int i_reg, mem_t *p){
  printf("R[%d] = ", i_reg);
  __mem_trace_print(p);
  if( p->scopy_from ){
    assert( p->scopy_from->scopy );
    printf(" <== R[%d]", (int)(p->scopy_from - &p[-i_reg]));
  }
  printf("\n");
  __vdbe_check_mem_invariants(p);
}
/**/ void __print_mem(mem_t *mem){
  __mem_trace_print(mem);
  printf("\n");
  fflush(stdout);
}
#endif

#ifdef DBSQL_DEBUG
/*
** Show the values of all registers in the virtual machine.  Used for
** interactive debugging.
*/
void __vdbe_register_dump(vdbe_t *v){
  int i;
  for(i=1; i<v->n_mem; i++) __register_trace(i, v->mems+i);
}
#endif /* DBSQL_DEBUG */


#ifdef DBSQL_DEBUG
#  define REGISTER_TRACE(R,M) if(db->flags&DBSQL_VdbeTrace)__register_trace(R,M)
#else
#  define REGISTER_TRACE(R,M)
#endif

#ifndef NDEBUG
/*
** This function is only called from within an assert() expression. It
** checks that the DBSQL.n_transaction variable is correctly set to
** the number of non-transaction savepoints currently in the
** linked list starting at DBSQL.savepoint.
**
** Usage:
**
**     assert( __check_savepoint_count(db) );
*/
static int __check_savepoint_count(DBSQL *db){
  int n = 0;
  savepoint_t *p;
  for(p=db->savepoint; p; p=p->next) n++;
  assert( n==(db->n_savepoint + db->is_transaction_savepoint) );
  return 1;
}
#endif

/*
** Return the register of op->p2 after first preparing it to be
** overwritten with an integer value.
*/
static DBSQL_NOINLINE mem_t *__out2_prerelease_with_clear(mem_t *out){
  __vdbe_mem_set_null(out);
  out->flags = MEM_Int;
  return out;
}
static mem_t *out2Prerelease(vdbe_t *p, vdbe_op_t *op){
  mem_t *out;
  assert( op->p2>0 );
  assert( op->p2<=(p->n_mem+1 - p->n_cursor) );
  out = &p->mems[op->p2];
  MEM_ABOUT_TO_CHANGE(p, out);
  if( VDBE_MEM_DYNAMIC(out) ){ /*OPTIMIZATION-IF-FALSE*/
    return __out2_prerelease_with_clear(out);
  }else{
    out->flags = MEM_Int;
    return out;
  }
}

/*
** Compute a bloom filter hash using op->p4.i registers from mems[] beginning
** with op->p3.  Return the hash.
*/
static u_int64_t __filter_hash(const mem_t *mems, const op_t *op){
  int i, mx;
  u_int64_t h = 0;

  assert( op->p4type==P4_INT32 );
  for(i=op->p3, mx=i+op->p4.i; i<mx; i++){
    const mem_t *p = &mems[i];
    if( p->flags & (MEM_Int|MEM_IntReal) ){
      h += p->u.i;
    }else if( p->flags & MEM_Real ){
      h += __vdbe_int_value(p);
    }else if( p->flags & (MEM_Str|MEM_Blob) ){
      /* All strings have the same hash and all blobs have the same hash,
      ** though, at least, those hashes are different from each other and
      ** from NULL. */
      h += 4093 + (p->flags & (MEM_Str|MEM_Blob));
    }
  }
  return h;
}


/*
** For OP_Column, factor out the case where content is loaded from
** overflow pages, so that the code to implement this case is separate
** the common case where all content fits on the page.  Factoring out
** the code reduces register pressure and helps the common case
** to run faster.
*/
static DBSQL_NOINLINE int __vdbe_column_from_overflow(
  vdbe_cursor_t *c,       /* The BTree cursor from which we are reading */
  int i_col,             /* The column to read */
  u_int32_t t,                /* The serial-type code for the column value */
  int64_t i_offset,          /* Offset to the start of the content value */
  u_int32_t cache_status,      /* Current vdbe_t.cache_ctr value */
  u_int32_t col_cache_ctr,      /* Current value of the column cache counter */
  mem_t *dest            /* Store the value into this register. */
){
  int rc;
  DBSQL *db = dest->db;
  int encoding = dest->enc;
  int len = __vdbe_serial_type_len(t);
  assert( c->cur_type==CURTYPE_BTREE );
  if( len>db->limits[DBSQL_LIMIT_LENGTH] ) return DBSQL_TOOBIG;
  if( len > 4000 && c->key_info==0 ){
    /* Cache large column values that are on overflow pages using
    ** an rc_str_t (reference counted string) so that if they are reloaded,
    ** that do not have to be copied a second time.  The overhead of
    ** creating and managing the cache is such that this is only
    ** profitable for larger TEXT and BLOB values.
    **
    ** Only do this on table-btrees so that writes to index-btrees do not
    ** need to clear the cache.  This buys performance in the common case
    ** in exchange for generality.
    */
    vdbe_txt_blb_cache_t *cache;
    char *buf;
    if( c->col_cache==0 ){
      c->cache = __db_malloc_zero(db, sizeof(vdbe_txt_blb_cache_t) );
      if( c->cache==0 ) return DBSQL_NOMEM;
      c->col_cache = 1;
    }
    cache = c->cache;
    if( cache->c_value==0
     || cache->i_col!=i_col
     || cache->cache_status!=cache_status
     || cache->col_cache_ctr!=col_cache_ctr
     || cache->i_offset!=__sm_offset(c->uc.cursor)
    ){
      if( cache->c_value ) __rc_str_unref(cache->c_value);
      buf = cache->c_value = __rc_str_new( len+3 );
      if( buf==0 ) return DBSQL_NOMEM;
      rc = __sm_payload(c->uc.cursor, i_offset, len, buf);
      if( rc ) return rc;
      buf[len] = 0;
      buf[len+1] = 0;
      buf[len+2] = 0;
      cache->i_col = i_col;
      cache->cache_status = cache_status;
      cache->col_cache_ctr = col_cache_ctr;
      cache->i_offset = __sm_offset(c->uc.cursor);
    }else{
      buf = cache->c_value;
    }
    assert( t>=12 );
    __rc_str_ref(buf);
    if( t&1 ){
      rc = __vdbe_mem_set_str(dest, buf, len, encoding,
                                __rc_str_unref);
      dest->flags |= MEM_Term;
    }else{
      rc = __vdbe_mem_set_str(dest, buf, len, 0,
                                __rc_str_unref);
    }
  }else{
    rc = __vdbe_mem_from_btree(c->uc.cursor, i_offset, len, dest);
    if( rc ) return rc;
    __vdbe_serial_get((const u_int8_t*)dest->z, t, dest);
    if( (t&1)!=0 && encoding==DBSQL_UTF8 ){
      dest->z[len] = 0;
      dest->flags |= MEM_Term;
    }
  }
  dest->flags &= ~MEM_Ephem;
  return rc;
}

/*
** Send a "statement aborts" message to the error log.
*/
static DBSQL_NOINLINE void __vdbe_log_abort(
  vdbe_t *p,     /* The statement that is running at the time of failure */
  int rc,      /* Error code */
  op_t *op,     /* Opcode that filed */
  op_t *ops      /* All opcodes */
){
  const char *sql = p->z_sql;   /* Original SQL text */
  const char *prefix = "";     /* Prefix added to SQL text */
  int pc;                       /* Opcode address */
  char xtra[100];              /* Buffer space to store prefix */

  if( p->frame ){
    assert( ops[0].opcode==OP_Init );
    if( ops[0].p4.z!=0 ){
      assert( ops[0].p4.z[0]=='-' 
           && ops[0].p4.z[1]=='-' 
           && ops[0].p4.z[2]==' ' );
      dbsql_snprintf(sizeof(xtra), xtra,"/* %s */ ",ops[0].p4.z+3);
      prefix = xtra;
    }else{
      prefix = "/* unknown trigger */ ";
    }
  }
  pc = (int)(op - ops);
  dbsql_log(rc, "statement aborts at %d: %s; [%s%s]",
                   pc, p->errmsg, prefix, sql);
}

/*
** Return the symbolic name for the data type of a mem
*/
static const char *__vdbe_mem_type_name(mem_t *mem){
  static const char *types[] = {
      /* DBSQL_INTEGER */ "INT",
      /* DBSQL_FLOAT   */ "REAL",
      /* DBSQL_TEXT    */ "TEXT",
      /* DBSQL_BLOB    */ "BLOB",
      /* DBSQL_NULL    */ "NULL"
  };
  return types[dbsql_value_type(mem)-1];
}

/*
** Execute as much of a VDBE program as we can.
** This is the core of dbsql_step(). 
*/
int __vdbe_exec(
  vdbe_t *p                    /* The VDBE */
){
  op_t *ops = p->ops;          /* Copy of p->ops */
  op_t *p_op = ops;             /* Current operation */
#ifdef DBSQL_DEBUG
  op_t *orig_op;               /* Value of op at the top of the loop */
  int n_extra_delete = 0;      /* Verifies FORDELETE and AUXDELETE flags */
  u_int8_t i_compare_is_init = 0;     /* i_compare is initialized */
#endif
  int rc = DBSQL_OK;        /* Value to return */
  DBSQL *db = p->db;       /* The database */
  u_int8_t reset_schema_on_fault = 0; /* Reset schema after an error if positive */
  u_int8_t encoding = ENC(db);     /* The database encoding */
  int i_compare = 0;          /* Result of last comparison */
  u_int64_t n_vm_step = 0;           /* Number of virtual machine steps */
#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
  u_int64_t n_progress_limit;        /* Invoke progress() when n_vm_step reaches this */
#endif
  mem_t *mems = p->mems;       /* Copy of p->mems */
  mem_t *in1 = 0;             /* 1st input operand */
  mem_t *in2 = 0;             /* 2nd input operand */
  mem_t *in3 = 0;             /* 3rd input operand */
  mem_t *out = 0;             /* Output operand */
  u_int32_t col_cache_ctr = 0;       /* column_t cache counter */
#if defined(DBSQL_ENABLE_STMT_SCANSTATUS) || defined(VDBE_PROFILE)
  u_int64_t *pn_cycle = 0;
  int stmt_scan_status = IS_STMT_SCANSTATUS(db)!=0;
#endif
  /*** INSERT STACK UNION HERE ***/

  assert( p->vdbe_state==VDBE_RUN_STATE );  /* dbsql_step() verifies this */
  if( DB_MASK_NON_ZERO(p->lock_mask) ){
    __vdbe_enter(p);
  }
#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
  if( db->progress ){
    u_int32_t i_prior = p->counters[DBSQL_STMTSTATUS_VM_STEP];
    assert( 0 < db->n_progress_ops );
    n_progress_limit = db->n_progress_ops - (i_prior % db->n_progress_ops);
  }else{
    n_progress_limit = LARGEST_UINT64;
  }
#endif
  if( p->rc==DBSQL_NOMEM ){
    /* This happens if a malloc() inside a call to dbsql_column_text() or
    ** dbsql_column_text16() failed.  */
    goto no_mem;
  }
  assert( p->rc==DBSQL_OK || (p->rc&0xff)==DBSQL_BUSY );
  testcase( p->rc!=DBSQL_OK );
  p->rc = DBSQL_OK;
  assert( p->is_reader || p->readOnly!=0 );
  p->i_current_time = 0;
  assert( p->explain==0 );
  db->busy_handler.n_busy = 0;
  if( ATOMIC_LOAD(&db->u1.is_interrupted) ) goto abort_due_to_interrupt;
  __vdbe_io_trace_sql(p);
#ifdef DBSQL_DEBUG
  __begin_benign_malloc();
  if( p->pc==0
   && (p->db->flags & (DBSQL_VdbeListing|DBSQL_VdbeEQP|DBSQL_VdbeTrace))!=0
  ){
    int i;
    int once = 1;
    __vdbe_print_sql(p);
    if( p->db->flags & DBSQL_VdbeListing ){
      printf("VDBE Program Listing:\n");
      for(i=0; i<p->n_op; i++){
        __vdbe_print_op(stdout, i, &ops[i]);
      }
    }
    if( p->db->flags & DBSQL_VdbeEQP ){
      for(i=0; i<p->n_op; i++){
        if( ops[i].opcode==OP_Explain ){
          if( once ) printf("VDBE Query Plan:\n");
          printf("%s\n", ops[i].p4.z);
          once = 0;
        }
      }
    }
    if( p->db->flags & DBSQL_VdbeTrace )  printf("VDBE Trace:\n");
  }
  __end_benign_malloc();
#endif
  for(p_op=&ops[p->pc]; 1; p_op++){
    /* Errors are detected by individual opcodes, with an immediate
    ** jumps to abort_due_to_error. */
    assert( rc==DBSQL_OK );

    assert( p_op>=ops && p_op<&ops[p->n_op]);
    n_vm_step++;

#if defined(VDBE_PROFILE)
    p_op->n_exec++;
    pn_cycle = &p_op->n_cycle;
    if( __n_profile_cnt==0 ) *pn_cycle -= __hwtime();
#elif defined(DBSQL_ENABLE_STMT_SCANSTATUS)
    if( stmt_scan_status ){
      p_op->n_exec++;
      pn_cycle = &p_op->n_cycle;
      *pn_cycle -= __hwtime();
    }
#endif

    /* Only allow tracing if DBSQL_DEBUG is defined.
    */
#ifdef DBSQL_DEBUG
    if( db->flags & DBSQL_VdbeTrace ){
      __vdbe_print_op(stdout, (int)(p_op - ops), p_op);
      test_trace_breakpoint((int)(p_op - ops),p_op,p);
    }
#endif
     

    /* Check to see if we need to simulate an interrupt.  This only happens
    ** if we have a special test build.
    */
#ifdef DBSQL_TEST
    if( dbsql_interrupt_count>0 ){
      dbsql_interrupt_count--;
      if( dbsql_interrupt_count==0 ){
        dbsql_interrupt(db);
      }
    }
#endif

    /* Sanity checking on other operands */
#ifdef DBSQL_DEBUG
    {
      u_int8_t op_property = __opcode_property[p_op->opcode];
      if( (op_property & OPFLG_IN1)!=0 ){
        assert( p_op->p1>0 );
        assert( p_op->p1<=(p->n_mem+1 - p->n_cursor) );
        assert( MEM_IS_VALID(&mems[p_op->p1]) );
        assert( __vdbe_check_mem_invariants(&mems[p_op->p1]) );
        REGISTER_TRACE(p_op->p1, &mems[p_op->p1]);
      }
      if( (op_property & OPFLG_IN2)!=0 ){
        assert( p_op->p2>0 );
        assert( p_op->p2<=(p->n_mem+1 - p->n_cursor) );
        assert( MEM_IS_VALID(&mems[p_op->p2]) );
        assert( __vdbe_check_mem_invariants(&mems[p_op->p2]) );
        REGISTER_TRACE(p_op->p2, &mems[p_op->p2]);
      }
      if( (op_property & OPFLG_IN3)!=0 ){
        assert( p_op->p3>0 );
        assert( p_op->p3<=(p->n_mem+1 - p->n_cursor) );
        assert( MEM_IS_VALID(&mems[p_op->p3]) );
        assert( __vdbe_check_mem_invariants(&mems[p_op->p3]) );
        REGISTER_TRACE(p_op->p3, &mems[p_op->p3]);
      }
      if( (op_property & OPFLG_OUT2)!=0 ){
        assert( p_op->p2>0 );
        assert( p_op->p2<=(p->n_mem+1 - p->n_cursor) );
        MEM_ABOUT_TO_CHANGE(p, &mems[p_op->p2]);
      }
      if( (op_property & OPFLG_OUT3)!=0 ){
        assert( p_op->p3>0 );
        assert( p_op->p3<=(p->n_mem+1 - p->n_cursor) );
        MEM_ABOUT_TO_CHANGE(p, &mems[p_op->p3]);
      }
    }
#endif
#ifdef DBSQL_DEBUG
    orig_op = p_op;
#endif
 
    switch( p_op->opcode ){

/*****************************************************************************
** What follows is a massive switch statement where each case implements a
** separate instruction in the virtual machine.  If we follow the usual
** indentation conventions, each case should be indented by 6 spaces.  But
** that is a lot of wasted space on the left margin.  So the code within
** the switch statement will break with convention and be flush-left. Another
** big comment (similar to this one) will mark the point in the code where
** we transition back to normal indentation.
**
** The formatting of each case is important.  The makefile for dbsql
** generates two C files "opcodes.h" and "opcodes.c" by scanning this
** file looking for lines that begin with "case OP_".  The opcodes.h files
** will be filled with #defines that give unique integer values to each
** opcode and the opcodes.c file is filled with an array of strings where
** each string is the symbolic name for the corresponding opcode.  If the
** case statement is followed by a comment of the form "/# same as ... #/"
** that comment is used to determine the particular value of the opcode.
**
** Other keywords in the comment that follows each case are used to
** construct the OPFLG_INITIALIZER value that initializes opcode_property[].
** Keywords include: in1, in2, in3, out2, out3.  See
** the mkopcodeh.awk script for additional information.
**
** Documentation about VDBE opcodes is generated by scanning this file
** for lines of that contain "Opcode:".  That line and all subsequent
** comment lines are used in the generation of the opcode.html documentation
** file.
**
** SUMMARY:
**
**     Formatting is important to scripts that scan this file.
**     Do not deviate from the formatting style currently in use.
**
*****************************************************************************/

/* Opcode:  Goto * P2 * * *
**
** An unconditional jump to address P2.
** The next instruction executed will be
** the one at index P2 from the beginning of
** the program.
**
** The P1 parameter is not actually used by this opcode.  However, it
** is sometimes set to 1 instead of 0 as a hint to the command-line shell
** that this Goto is the bottom of a loop and that the lines from P2 down
** to the current line should be indented for EXPLAIN output.
*/
case OP_Goto: {             /* jump */

#ifdef DBSQL_DEBUG
  /* In debugging mode, when the p5 flags is set on an OP_Goto, that
  ** means we should really jump back to the preceding OP_ReleaseReg
  ** instruction. */
  if( p_op->p5 ){
    assert( p_op->p2 < (int)(p_op - ops) );
    assert( p_op->p2 > 1 );
    p_op = &ops[p_op->p2 - 2];
    assert( p_op[1].opcode==OP_ReleaseReg );
    goto check_for_interrupt;
  }
#endif

jump_to_p2_and_check_for_interrupt:
  p_op = &ops[p_op->p2 - 1];

  /* Opcodes that are used as the bottom of a loop (OP_Next, OP_Prev,
  ** OP_VNext, or OP_SorterNext) all jump here upon
  ** completion.  Check to see if dbsql_interrupt() has been called
  ** or if the progress callback needs to be invoked.
  **
  ** This code uses unstructured "goto" statements and does not look clean.
  ** But that is not due to sloppy coding habits. The code is written this
  ** way for performance, to avoid having to run the interrupt and progress
  ** checks on every opcode.  This helps dbsql_step() to run about 1.5%
  ** faster according to "valgrind --tool=cachegrind" */
check_for_interrupt:
  if( ATOMIC_LOAD(&db->u1.is_interrupted) ) goto abort_due_to_interrupt;
#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
  /* Call the progress callback if it is configured and the required number
  ** of VDBE ops have been executed (either since this invocation of
  ** __vdbe_exec() or since last time the progress callback was called).
  ** If the progress callback returns non-zero, exit the virtual machine with
  ** a return code DBSQL_ABORT.
  */
  while( n_vm_step>=n_progress_limit && db->progress!=0 ){
    assert( db->n_progress_ops!=0 );
    n_progress_limit += db->n_progress_ops;
    if( db->progress(db->progress_arg) ){
      n_progress_limit = LARGEST_UINT64;
      rc = DBSQL_INTERRUPT;
      goto abort_due_to_error;
    }
  }
#endif
 
  break;
}

/* Opcode:  Gosub P1 P2 * * *
**
** Write the current address onto register P1
** and then jump to address P2.
*/
case OP_Gosub: {            /* jump */
  assert( p_op->p1>0 && p_op->p1<=(p->n_mem+1 - p->n_cursor) );
  in1 = &mems[p_op->p1];
  assert( VDBE_MEM_DYNAMIC(in1)==0 );
  MEM_ABOUT_TO_CHANGE(p, in1);
  in1->flags = MEM_Int;
  in1->u.i = (int)(p_op-ops);
  REGISTER_TRACE(p_op->p1, in1);
  goto jump_to_p2_and_check_for_interrupt;
}

/* Opcode:  Return P1 P2 P3 * *
**
** Jump to the address stored in register P1.  If P1 is a return address
** register, then this accomplishes a return from a subroutine.
**
** If P3 is 1, then the jump is only taken if register P1 holds an integer
** values, otherwise execution falls through to the next opcode, and the
** OP_Return becomes a no-op. If P3 is 0, then register P1 must hold an
** integer or else an assert() is raised.  P3 should be set to 1 when
** this opcode is used in combination with OP_BeginSubrtn, and set to 0
** otherwise.
**
** The value in register P1 is unchanged by this opcode.
**
** P2 is not used by the byte-code engine.  However, if P2 is positive
** and also less than the current address, then the "EXPLAIN" output
** formatter in the CLI will indent all opcodes from the P2 opcode up
** to be not including the current Return.   P2 should be the first opcode
** in the subroutine from which this opcode is returning.  Thus the P2
** value is a byte-code indentation hint.  See tag-20220407a in
** wherecode.c and shell.c.
*/
case OP_Return: {           /* in1 */
  in1 = &mems[p_op->p1];
  if( in1->flags & MEM_Int ){
    if( p_op->p3 ){ VDBE_BRANCH_TAKEN(1, 2); }
    p_op = &ops[in1->u.i];
  }else if( ALWAYS(p_op->p3) ){
    VDBE_BRANCH_TAKEN(0, 2);
  }
  break;
}

/* Opcode: __init_coroutine P1 P2 P3 * *
**
** Set up register P1 so that it will Yield to the coroutine
** located at address P3.
**
** If P2!=0 then the coroutine implementation immediately follows
** this opcode.  So jump over the coroutine implementation to
** address P2.
**
** See also: __end_coroutine
*/
case OP_InitCoroutine: {     /* jump0 */
  assert( p_op->p1>0 &&  p_op->p1<=(p->n_mem+1 - p->n_cursor) );
  assert( p_op->p2>=0 && p_op->p2<p->n_op );
  assert( p_op->p3>=0 && p_op->p3<p->n_op );
  out = &mems[p_op->p1];
  assert( !VDBE_MEM_DYNAMIC(out) );
  out->u.i = p_op->p3 - 1;
  out->flags = MEM_Int;
  if( p_op->p2==0 ) break;

  /* Most jump operations do a goto to this spot in order to update
  ** the op pointer. */
jump_to_p2:
  assert( p_op->p2>0 );       /* There are never any jumps to instruction 0 */
  assert( p_op->p2<p->n_op );  /* Jumps must be in range */
  p_op = &ops[p_op->p2 - 1];
  break;
}

/* Opcode:  __end_coroutine P1 * * * *
**
** The instruction at the address in register P1 is a Yield.
** Jump to the P2 parameter of that Yield.
** After the jump, the value register P1 is left with a value
** such that subsequent OP_Yields go back to the this same
** OP_EndCoroutine instruction.
**
** See also: __init_coroutine
*/
case OP_EndCoroutine: {           /* in1 */
  vdbe_op_t *caller;
  in1 = &mems[p_op->p1];
  assert( in1->flags==MEM_Int );
  assert( in1->u.i>=0 && in1->u.i<p->n_op );
  caller = &ops[in1->u.i];
  assert( caller->opcode==OP_Yield );
  assert( caller->p2>=0 && caller->p2<p->n_op );
  in1->u.i = (int)(p_op - p->ops) - 1;
  p_op = &ops[caller->p2 - 1];
  break;
}

/* Opcode:  Yield P1 P2 * * *
**
** Swap the program counter with the value in register P1.  This
** has the effect of yielding to a coroutine.
**
** If the coroutine that is launched by this instruction ends with
** Yield or Return then continue to the next instruction.  But if
** the coroutine launched by this instruction ends with
** __end_coroutine, then jump to P2 rather than continuing with the
** next instruction.
**
** See also: __init_coroutine
*/
case OP_Yield: {            /* in1, jump0 */
  int pc_dest;
  in1 = &mems[p_op->p1];
  assert( VDBE_MEM_DYNAMIC(in1)==0 );
  in1->flags = MEM_Int;
  pc_dest = (int)in1->u.i;
  in1->u.i = (int)(p_op - ops);
  REGISTER_TRACE(p_op->p1, in1);
  p_op = &ops[pc_dest];
  break;
}

/* Opcode:  __halt_if_null  P1 P2 P3 P4 P5
** Synopsis: if r[P3]=null halt
**
** Check the value in register P3.  If it is NULL then Halt using
** parameter P1, P2, and P4 as if this were a Halt instruction.  If the
** value in register P3 is not NULL, then this routine is a no-op.
** The P5 parameter should be 1.
*/
case OP_HaltIfNull: {      /* in3 */
  in3 = &mems[p_op->p3];
#ifdef DBSQL_DEBUG
  if( p_op->p2==OE_Abort ){ __vdbe_assert_abortable(p); }
#endif
  if( (in3->flags & MEM_Null)==0 ) break;
  /* Fall through into OP_Halt */
  /* no break */ deliberate_fall_through
}

/* Opcode:  Halt P1 P2 P3 P4 P5
**
** Exit immediately.  All open cursors, etc are closed
** automatically.
**
** P1 is the result code returned by dbsql_exec(), dbsql_reset(),
** or dbsql_finalize().  For a normal halt, this should be DBSQL_OK (0).
** For errors, it can be some other value.  If P1!=0 then P2 will determine
** whether or not to rollback the current transaction.  Do not rollback
** if P2==OE_Fail. Do the rollback if P2==OE_Rollback.  If P2==OE_Abort,
** then back out all changes that have occurred during this execution of the
** VDBE, but do not rollback the transaction.
**
** If P3 is not zero and P4 is NULL, then P3 is a register that holds the
** text of an error message.
**
** If P3 is zero and P4 is not null then the error message string is held
** in P4.
**
** P5 is a value between 1 and 4, inclusive, then the P4 error message
** string is modified as follows:
**
**    1:  NOT NULL constraint failed: P4
**    2:  UNIQUE constraint failed: P4
**    3:  CHECK constraint failed: P4
**    4:  FOREIGN KEY constraint failed: P4
**
** If P3 is zero and P5 is not zero and P4 is NULL, then everything after
** the ":" is omitted.
**
** There is an implied "Halt 0 0 0" instruction inserted at the very end of
** every program.  So a jump past the last instruction of the program
** is the same as executing Halt.
*/
case OP_Halt: {
  vdbe_frame_t *frame;
  int pcx;

#ifdef DBSQL_DEBUG
  if( p_op->p2==OE_Abort ){ __vdbe_assert_abortable(p); }
#endif
  assert( p_op->p4type==P4_NOTUSED
       || p_op->p4type==P4_STATIC
       || p_op->p4type==P4_DYNAMIC );

  /* A deliberately coded "OP_Halt DBSQL_INTERNAL * * * *" opcode indicates
  ** something is wrong with the code generator.  Raise an assertion in order
  ** to bring this to the attention of fuzzers and other testing tools. */
  assert( p_op->p1!=DBSQL_INTERNAL );

  if( p->frame && p_op->p1==DBSQL_OK ){
    /* Halt the sub-program. Return control to the parent frame. */
    frame = p->frame;
    p->frame = frame->parent;
    p->n_frame--;
    __vdbe_set_changes(db, p->n_change);
    pcx = __vdbe_frame_restore(frame);
    if( p_op->p2==OE_Ignore ){
      /* Instruction pcx is the OP_Program that invoked the sub-program
      ** currently being halted. If the p2 instruction of this OP_Halt
      ** instruction is set to OE_Ignore, then the sub-program is throwing
      ** an IGNORE exception. In this case jump to the address specified
      ** as the p2 of the calling OP_Program.  */
      pcx = p->ops[pcx].p2-1;
    }
    ops = p->ops;
    mems = p->mems;
    p_op = &ops[pcx];
    break;
  }
  p->rc = p_op->p1;
  p->error_action = (u_int8_t)p_op->p2;
  assert( p_op->p5<=4 );
  if( p->rc ){
    if( p_op->p3>0 && p_op->p4type==P4_NOTUSED ){
      const char *err;
      assert( p_op->p3<=(p->n_mem + 1 - p->n_cursor) );
      err = __value_text(&mems[p_op->p3], DBSQL_UTF8);
      __vdbe_error(p, "%s", err);
    }else if( p_op->p5 ){
      static const char * const types[] = { "NOT NULL", "UNIQUE", "CHECK",
                                             "FOREIGN KEY" };
      testcase( p_op->p5==1 );
      testcase( p_op->p5==2 );
      testcase( p_op->p5==3 );
      testcase( p_op->p5==4 );
      __vdbe_error(p, "%s constraint failed", types[p_op->p5-1]);
      if( p_op->p4.z ){
        p->errmsg = __m_printf(db, "%z: %s", p->errmsg, p_op->p4.z);
      }
    }else{
      __vdbe_error(p, "%s", p_op->p4.z);
    }
    __vdbe_log_abort(p, p_op->p1, p_op, ops);
  }
  rc = __vdbe_halt(p);
  assert( rc==DBSQL_BUSY || rc==DBSQL_OK || rc==DBSQL_ERROR );
  if( rc==DBSQL_BUSY ){
    p->rc = DBSQL_BUSY;
  }else{
    assert( rc==DBSQL_OK || (p->rc&0xff)==DBSQL_CONSTRAINT );
    assert( rc==DBSQL_OK || db->n_deferred_cons>0 || db->n_deferred_imm_cons>0 );
    rc = p->rc ? DBSQL_ERROR : DBSQL_DONE;
  }
  goto vdbe_return;
}

/* Opcode: Integer P1 P2 * * *
** Synopsis: r[P2]=P1
**
** The 32-bit integer value P1 is written into register P2.
*/
case OP_Integer: {         /* out2 */
  out = out2Prerelease(p, p_op);
  out->u.i = p_op->p1;
  break;
}

/* Opcode: Int64 * P2 * P4 *
** Synopsis: r[P2]=P4
**
** P4 is a pointer to a 64-bit integer value.
** Write that value into register P2.
*/
case OP_Int64: {           /* out2 */
  out = out2Prerelease(p, p_op);
  assert( p_op->p4.i64!=0 );
  out->u.i = *p_op->p4.i64;
  break;
}

#ifndef DBSQL_OMIT_FLOATING_POINT
/* Opcode: Real * P2 * P4 *
** Synopsis: r[P2]=P4
**
** P4 is a pointer to a 64-bit floating point value.
** Write that value into register P2.
*/
case OP_Real: {            /* same as TK_FLOAT, out2 */
  out = out2Prerelease(p, p_op);
  out->flags = MEM_Real;
  assert( !__is_na_n(*p_op->p4.real) );
  out->u.r = *p_op->p4.real;
  break;
}
#endif

/* Opcode: String8 * P2 * P4 *
** Synopsis: r[P2]='P4'
**
** P4 points to a nul terminated UTF-8 string. This opcode is transformed
** into a String opcode before it is executed for the first time.  During
** this transformation, the length of string P4 is computed and stored
** as the P1 parameter.
*/
case OP_String8: {         /* same as TK_STRING, out2 */
  assert( p_op->p4.z!=0 );
  out = out2Prerelease(p, p_op);
  p_op->p1 = __strlen30(p_op->p4.z);

#ifndef DBSQL_OMIT_UTF16
  if( encoding!=DBSQL_UTF8 ){
    rc = __vdbe_mem_set_str(out, p_op->p4.z, -1, DBSQL_UTF8, DBSQL_STATIC);
    assert( rc==DBSQL_OK || rc==DBSQL_TOOBIG );
    if( rc ) goto too_big;
    if( DBSQL_OK!=__vdbe_change_encoding(out, encoding) ) goto no_mem;
    assert( out->sz_malloc>0 && out->z_malloc==out->z );
    assert( VDBE_MEM_DYNAMIC(out)==0 );
    out->sz_malloc = 0;
    out->flags |= MEM_Static;
    if( p_op->p4type==P4_DYNAMIC ){
      __dbsql_db_free(db, p_op->p4.z);
    }
    p_op->p4type = P4_DYNAMIC;
    p_op->p4.z = out->z;
    p_op->p1 = out->n;
  }
#endif
  if( p_op->p1>db->limits[DBSQL_LIMIT_LENGTH] ){
    goto too_big;
  }
  p_op->opcode = OP_String;
  assert( rc==DBSQL_OK );
  /* Fall through to the next case, OP_String */
  /* no break */ deliberate_fall_through
}
 
/* Opcode: String P1 P2 P3 P4 P5
** Synopsis: r[P2]='P4' (len=P1)
**
** The string value P4 of length P1 (bytes) is stored in register P2.
**
** If P3 is not zero and the content of register P3 is equal to P5, then
** the datatype of the register P2 is converted to BLOB.  The content is
** the same sequence of bytes, it is merely interpreted as a BLOB instead
** of a string, as if it had been CAST.  In other words:
**
** if( P3!=0 and reg[P3]==P5 ) reg[P2] := CAST(reg[P2] as BLOB)
*/
case OP_String: {          /* out2 */
  assert( p_op->p4.z!=0 );
  out = out2Prerelease(p, p_op);
  out->flags = MEM_Str|MEM_Static|MEM_Term;
  out->z = p_op->p4.z;
  out->n = p_op->p1;
  out->enc = encoding;
  UPDATE_MAX_BLOBSIZE(out);
#ifndef DBSQL_LIKE_DOESNT_MATCH_BLOBS
  if( p_op->p3>0 ){
    assert( p_op->p3<=(p->n_mem+1 - p->n_cursor) );
    in3 = &mems[p_op->p3];
    assert( in3->flags & MEM_Int );
    if( in3->u.i==p_op->p5 ) out->flags = MEM_Blob|MEM_Static|MEM_Term;
  }
#endif
  break;
}

/* Opcode: __begin_subrtn * P2 * * *
** Synopsis: r[P2]=NULL
**
** Mark the beginning of a subroutine that can be entered in-line
** or that can be called using OP_Gosub.  The subroutine should
** be terminated by an OP_Return instruction that has a P1 operand that
** is the same as the P2 operand to this opcode and that has P3 set to 1.
** If the subroutine is entered in-line, then the OP_Return will simply
** fall through.  But if the subroutine is entered using OP_Gosub, then
** the OP_Return will jump back to the first instruction after the OP_Gosub.
**
** This routine works by loading a NULL into the P2 register.  When the
** return address register contains a NULL, the OP_Return instruction is
** a no-op that simply falls through to the next instruction (assuming that
** the OP_Return opcode has a P3 value of 1).  Thus if the subroutine is
** entered in-line, then the OP_Return will cause in-line execution to
** continue.  But if the subroutine is entered via OP_Gosub, then the
** OP_Return will cause a return to the address following the OP_Gosub.
**
** This opcode is identical to OP_Null.  It has a different name
** only to make the byte code easier to read and verify.
*/
/* Opcode: Null P1 P2 P3 * *
** Synopsis: r[P2..P3]=NULL
**
** Write a NULL into registers P2.  If P3 greater than P2, then also write
** NULL into register P3 and every register in between P2 and P3.  If P3
** is less than P2 (typically P3 is zero) then only register P2 is
** set to NULL.
**
** If the P1 value is non-zero, then also set the MEM_Cleared flag so that
** NULL values will not compare equal even if DBSQL_NULLEQ is set on
** OP_Ne or OP_Eq.
*/
case OP_BeginSubrtn:
case OP_Null: {           /* out2 */
  int cnt;
  u_int16_t null_flag;
  out = out2Prerelease(p, p_op);
  cnt = p_op->p3-p_op->p2;
  assert( p_op->p3<=(p->n_mem+1 - p->n_cursor) );
  out->flags = null_flag = p_op->p1 ? (MEM_Null|MEM_Cleared) : MEM_Null;
  out->n = 0;
#ifdef DBSQL_DEBUG
  out->u_temp = 0;
#endif
  while( cnt>0 ){
    out++;
    MEM_ABOUT_TO_CHANGE(p, out);
    __vdbe_mem_set_null(out);
    out->flags = null_flag;
    out->n = 0;
    cnt--;
  }
  break;
}

/* Opcode: __soft_null P1 * * * *
** Synopsis: r[P1]=NULL
**
** Set register P1 to have the value NULL as seen by the OP_MakeRecord
** instruction, but do not free any string or blob memory associated with
** the register, so that if the value was a string or blob that was
** previously copied using OP_SCopy, the copies will continue to be valid.
*/
case OP_SoftNull: {
  assert( p_op->p1>0 && p_op->p1<=(p->n_mem+1 - p->n_cursor) );
  out = &mems[p_op->p1];
  out->flags = (out->flags&~(MEM_Undefined|MEM_AffMask))|MEM_Null;
  break;
}

/* Opcode: Blob P1 P2 * P4 *
** Synopsis: r[P2]=P4 (len=P1)
**
** P4 points to a blob of data P1 bytes long.  Store this
** blob in register P2.  If P4 is a NULL pointer, then construct
** a zero-filled blob that is P1 bytes long in P2.
*/
case OP_Blob: {                /* out2 */
  assert( p_op->p1 <= DBSQL_MAX_LENGTH );
  out = out2Prerelease(p, p_op);
  if( p_op->p4.z==0 ){
    __vdbe_mem_set_zero_blob(out, p_op->p1);
    if( __vdbe_mem_expand_blob(out) ) goto no_mem;
  }else{
    __vdbe_mem_set_str(out, p_op->p4.z, p_op->p1, 0, 0);
  }
  out->enc = encoding;
  UPDATE_MAX_BLOBSIZE(out);
  break;
}

/* Opcode: Variable P1 P2 * * *
** Synopsis: r[P2]=parameter(P1)
**
** Transfer the values of bound parameter P1 into register P2
*/
case OP_Variable: {            /* out2 */
  mem_t *var;       /* Value being transferred */

  assert( p_op->p1>0 && p_op->p1<=p->n_var );
  var = &p->vars[p_op->p1 - 1];
  if( __vdbe_mem_too_big(var) ){
    goto too_big;
  }
  out = &mems[p_op->p2];
  if( VDBE_MEM_DYNAMIC(out) ) __vdbe_mem_set_null(out);
  memcpy(out, var, MEMCELLSIZE);
  out->flags &= ~(MEM_Dyn|MEM_Ephem);
  out->flags |= MEM_Static|MEM_FromBind;
  UPDATE_MAX_BLOBSIZE(out);
  break;
}

/* Opcode: Move P1 P2 P3 * *
** Synopsis: r[P2@P3]=r[P1@P3]
**
** Move the P3 values in register P1..P1+P3-1 over into
** registers P2..P2+P3-1.  Registers P1..P1+P3-1 are
** left holding a NULL.  It is an error for register ranges
** P1..P1+P3-1 and P2..P2+P3-1 to overlap.  It is an error
** for P3 to be less than 1.
*/
case OP_Move: {
  int n;           /* Number of registers left to copy */
  int p1;          /* Register to copy from */
  int p2;          /* Register to copy to */

  n = p_op->p3;
  p1 = p_op->p1;
  p2 = p_op->p2;
  assert( n>0 && p1>0 && p2>0 );
  assert( p1+n<=p2 || p2+n<=p1 );

  in1 = &mems[p1];
  out = &mems[p2];
  do{
    assert( out<=&mems[(p->n_mem+1 - p->n_cursor)] );
    assert( in1<=&mems[(p->n_mem+1 - p->n_cursor)] );
    assert( MEM_IS_VALID(in1) );
    MEM_ABOUT_TO_CHANGE(p, out);
    __vdbe_mem_move(out, in1);
#ifdef DBSQL_DEBUG
    in1->scopy_from = 0;
    { int i;
      for(i=1; i<p->n_mem; i++){
        if( mems[i].scopy_from==in1 ){
          assert( mems[i].scopy );
          mems[i].scopy_from = out;
        }
      }
    }
#endif
    DEEPHEMERALIZE(out);
    REGISTER_TRACE(p2++, out);
    in1++;
    out++;
  }while( --n );
  break;
}

/* Opcode: Copy P1 P2 P3 * P5
** Synopsis: r[P2@P3+1]=r[P1@P3+1]
**
** Make a copy of registers P1..P1+P3 into registers P2..P2+P3.
**
** If the 0x0002 bit of P5 is set then also clear the MEM_Subtype flag in the
** destination.  The 0x0001 bit of P5 indicates that this Copy opcode cannot
** be merged.  The 0x0001 bit is used by the query planner and does not
** come into play during query execution.
**
** This instruction makes a deep copy of the value.  A duplicate
** is made of any string or blob constant.  See also OP_SCopy.
*/
case OP_Copy: {
  int n;

  n = p_op->p3;
  in1 = &mems[p_op->p1];
  out = &mems[p_op->p2];
  assert( out!=in1 );
  while( 1 ){
    MEM_ABOUT_TO_CHANGE(p, out);
    __vdbe_mem_shallow_copy(out, in1, MEM_Ephem);
    DEEPHEMERALIZE(out);
    if( (out->flags & MEM_Subtype)!=0 &&  (p_op->p5 & 0x0002)!=0 ){
      out->flags &= ~MEM_Subtype;
    }
#ifdef DBSQL_DEBUG
    out->scopy_from = 0;
#endif
    REGISTER_TRACE(p_op->p2+p_op->p3-n, out);
    if( (n--)==0 ) break;
    out++;
    in1++;
  }
  break;
}

/* Opcode: SCopy P1 P2 * * *
** Synopsis: r[P2]=r[P1]
**
** Make a shallow copy of register P1 into register P2.
**
** This instruction makes a shallow copy of the value.  If the value
** is a string or blob, then the copy is only a pointer to the
** original and hence if the original changes so will the copy.
** Worse, if the original is deallocated, the copy becomes invalid.
** Thus the program must guarantee that the original will not change
** during the lifetime of the copy.  Use OP_Copy to make a complete
** copy.
*/
case OP_SCopy: {            /* out2 */
  in1 = &mems[p_op->p1];
  out = &mems[p_op->p2];
  assert( out!=in1 );
  __vdbe_mem_shallow_copy(out, in1, MEM_Ephem);
#ifdef DBSQL_DEBUG
  out->scopy_from = in1;
  out->m_scopy_flags = in1->flags;
  in1->scopy = 1;
#endif
  break;
}

/* Opcode: __int_copy P1 P2 * * *
** Synopsis: r[P2]=r[P1]
**
** Transfer the integer value held in register P1 into register P2.
**
** This is an optimized version of SCopy that works only for integer
** values.
*/
case OP_IntCopy: {            /* out2 */
  in1 = &mems[p_op->p1];
  assert( (in1->flags & MEM_Int)!=0 );
  out = &mems[p_op->p2];
  __vdbe_mem_set_int64(out, in1->u.i);
  break;
}

/* Opcode: __fk_check * * * * *
**
** Halt with an DBSQL_CONSTRAINT error if there are any unresolved
** foreign key constraint violations.  If there are no foreign key
** constraint violations, this is a no-op.
**
** FK constraint violations are also checked when the prepared statement
** exits.  This opcode is used to raise foreign key constraint errors prior
** to returning results such as a row change count or the result of a
** RETURNING clause.
*/
case OP_FkCheck: {
  if( (rc = __vdbe_check_fk_immediate(p))!=DBSQL_OK ){
    goto abort_due_to_error;
  }
  break;
}

/* Opcode: __result_row P1 P2 * * *
** Synopsis: output=r[P1@P2]
**
** The registers P1 through P1+P2-1 contain a single row of
** results. This opcode causes the dbsql_step() call to terminate
** with an DBSQL_ROW return code and it sets up the dbsql_stmt_t
** structure to provide access to the r(P1)..r(P1+P2-1) values as
** the result row.
*/
case OP_ResultRow: {
  assert( p->n_res_column==p_op->p2 );
  assert( p_op->p1>0 || CORRUPT_DB );
  assert( p_op->p1+p_op->p2<=(p->n_mem+1 - p->n_cursor)+1 );

  p->cache_ctr = (p->cache_ctr + 2)|1;
  p->result_row = &mems[p_op->p1];
#ifdef DBSQL_DEBUG
  {
    mem_t *mem = p->result_row;
    int i;
    for(i=0; i<p_op->p2; i++){
      assert( MEM_IS_VALID(&mem[i]) );
      REGISTER_TRACE(p_op->p1+i, &mem[i]);
      /* The registers in the result will not be used again when the
      ** prepared statement restarts.  This is because dbsql_column()
      ** APIs might have caused type conversions of made other changes to
      ** the register values.  Therefore, we can go ahead and break any
      ** OP_SCopy dependencies. */
      mem[i].scopy_from = 0;
    }
  }
#endif
  if( db->malloc_failed ) goto no_mem;
  if( db->m_trace & DBSQL_TRACE_ROW ){
    db->trace.v2(DBSQL_TRACE_ROW, db->trace_arg, p, 0);
  }
  p->pc = (int)(p_op - ops) + 1;
  rc = DBSQL_ROW;
  goto vdbe_return;
}

/* Opcode: Concat P1 P2 P3 * *
** Synopsis: r[P3]=r[P2]+r[P1]
**
** Add the text in register P1 onto the end of the text in
** register P2 and store the result in register P3.
** If either the P1 or P2 text are NULL then store NULL in P3.
**
**   P3 = P2 || P1
**
** It is illegal for P1 and P3 to be the same register. Sometimes,
** if P3 is the same register as P2, the implementation is able
** to avoid a memcpy().
*/
case OP_Concat: {           /* same as TK_CONCAT, in1, in2, out3 */
  int64_t n_byte;          /* Total size of the output string or blob */
  u_int16_t flags1;         /* Initial flags for P1 */
  u_int16_t flags2;         /* Initial flags for P2 */

  in1 = &mems[p_op->p1];
  in2 = &mems[p_op->p2];
  out = &mems[p_op->p3];
  testcase( out==in2 );
  assert( in1!=out );
  flags1 = in1->flags;
  testcase( flags1 & MEM_Null );
  testcase( in2->flags & MEM_Null );
  if( (flags1 | in2->flags) & MEM_Null ){
    __vdbe_mem_set_null(out);
    break;
  }
  if( (flags1 & (MEM_Str|MEM_Blob))==0 ){
    if( __vdbe_mem_stringify(in1,encoding,0) ) goto no_mem;
    flags1 = in1->flags & ~MEM_Str;
  }else if( (flags1 & MEM_Zero)!=0 ){
    if( __vdbe_mem_expand_blob(in1) ) goto no_mem;
    flags1 = in1->flags & ~MEM_Str;
  }
  flags2 = in2->flags;
  if( (flags2 & (MEM_Str|MEM_Blob))==0 ){
    if( __vdbe_mem_stringify(in2,encoding,0) ) goto no_mem;
    flags2 = in2->flags & ~MEM_Str;
  }else if( (flags2 & MEM_Zero)!=0 ){
    if( __vdbe_mem_expand_blob(in2) ) goto no_mem;
    flags2 = in2->flags & ~MEM_Str;
  }
  n_byte = in1->n;
  n_byte += in2->n;
  if( n_byte>db->limits[DBSQL_LIMIT_LENGTH] ){
    goto too_big;
  }
#if DBSQL_MAX_LENGTH>2147483645
  if( n_byte>2147483645 ){ goto too_big; }
#endif
  if( __vdbe_mem_grow(out, (int)n_byte+2, out==in2) ){
    goto no_mem;
  }
  MEM_SET_TYPE_FLAG(out, MEM_Str);
  if( out!=in2 ){
    memcpy(out->z, in2->z, in2->n);
    assert( (in2->flags & MEM_Dyn) == (flags2 & MEM_Dyn) );
    in2->flags = flags2;
  }
  memcpy(&out->z[in2->n], in1->z, in1->n);
  assert( (in1->flags & MEM_Dyn) == (flags1 & MEM_Dyn) );
  in1->flags = flags1;
  if( encoding>DBSQL_UTF8 ) n_byte &= ~1;
  out->z[n_byte]=0;
  out->z[n_byte+1] = 0;
  out->flags |= MEM_Term;
  out->n = (int)n_byte;
  out->enc = encoding;
  UPDATE_MAX_BLOBSIZE(out);
  break;
}

/* Opcode: Add P1 P2 P3 * *
** Synopsis: r[P3]=r[P1]+r[P2]
**
** Add the value in register P1 to the value in register P2
** and store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: Multiply P1 P2 P3 * *
** Synopsis: r[P3]=r[P1]*r[P2]
**
**
** Multiply the value in register P1 by the value in register P2
** and store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: Subtract P1 P2 P3 * *
** Synopsis: r[P3]=r[P2]-r[P1]
**
** Subtract the value in register P1 from the value in register P2
** and store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: Divide P1 P2 P3 * *
** Synopsis: r[P3]=r[P2]/r[P1]
**
** Divide the value in register P1 by the value in register P2
** and store the result in register P3 (P3=P2/P1). If the value in
** register P1 is zero, then the result is NULL. If either input is
** NULL, the result is NULL.
*/
/* Opcode: Remainder P1 P2 P3 * *
** Synopsis: r[P3]=r[P2]%r[P1]
**
** Compute the remainder after integer register P2 is divided by
** register P1 and store the result in register P3.
** If the value in register P1 is zero the result is NULL.
** If either operand is NULL, the result is NULL.
*/
case OP_Add:                   /* same as TK_PLUS, in1, in2, out3 */
case OP_Subtract:              /* same as TK_MINUS, in1, in2, out3 */
case OP_Multiply:              /* same as TK_STAR, in1, in2, out3 */
case OP_Divide:                /* same as TK_SLASH, in1, in2, out3 */
case OP_Remainder: {           /* same as TK_REM, in1, in2, out3 */
  u_int16_t type1;      /* Numeric type of left operand */
  u_int16_t type2;      /* Numeric type of right operand */
  int64_t i_a;         /* Integer value of left operand */
  int64_t i_b;         /* Integer value of right operand */
  double r_a;      /* Real value of left operand */
  double r_b;      /* Real value of right operand */

  in1 = &mems[p_op->p1];
  type1 = in1->flags;
  in2 = &mems[p_op->p2];
  type2 = in2->flags;
  out = &mems[p_op->p3];
  if( (type1 & type2 & MEM_Int)!=0 ){
int_math:
    i_a = in1->u.i;
    i_b = in2->u.i;
    switch( p_op->opcode ){
      case OP_Add:       if( __add_int64(&i_b,i_a) ) goto fp_math;  break;
      case OP_Subtract:  if( __sub_int64(&i_b,i_a) ) goto fp_math;  break;
      case OP_Multiply:  if( __mul_int64(&i_b,i_a) ) goto fp_math;  break;
      case OP_Divide: {
        if( i_a==0 ) goto arithmetic_result_is_null;
        if( i_a==-1 && i_b==SMALLEST_INT64 ) goto fp_math;
        i_b /= i_a;
        break;
      }
      default: {
        if( i_a==0 ) goto arithmetic_result_is_null;
        if( i_a==-1 ) i_a = 1;
        i_b %= i_a;
        break;
      }
    }
    out->u.i = i_b;
    MEM_SET_TYPE_FLAG(out, MEM_Int);
  }else if( ((type1 | type2) & MEM_Null)!=0 ){
    goto arithmetic_result_is_null;
  }else{
    type1 = __numeric_type(in1);
    type2 = __numeric_type(in2);
    if( (type1 & type2 & MEM_Int)!=0 ) goto int_math;
fp_math:
    r_a = __vdbe_real_value(in1);
    r_b = __vdbe_real_value(in2);
    switch( p_op->opcode ){
      case OP_Add:         r_b += r_a;       break;
      case OP_Subtract:    r_b -= r_a;       break;
      case OP_Multiply:    r_b *= r_a;       break;
      case OP_Divide: {
        /* (double)0 In case of DBSQL_OMIT_FLOATING_POINT... */
        if( r_a==(double)0 ) goto arithmetic_result_is_null;
        r_b /= r_a;
        break;
      }
      default: {
        i_a = __vdbe_int_value(in1);
        i_b = __vdbe_int_value(in2);
        if( i_a==0 ) goto arithmetic_result_is_null;
        if( i_a==-1 ) i_a = 1;
        r_b = (double)(i_b % i_a);
        break;
      }
    }
#ifdef DBSQL_OMIT_FLOATING_POINT
    out->u.i = r_b;
    MEM_SET_TYPE_FLAG(out, MEM_Int);
#else
    if( __is_na_n(r_b) ){
      goto arithmetic_result_is_null;
    }
    out->u.r = r_b;
    MEM_SET_TYPE_FLAG(out, MEM_Real);
#endif
  }
  break;

arithmetic_result_is_null:
  __vdbe_mem_set_null(out);
  break;
}

/* Opcode: coll_seq_t P1 * * P4
**
** P4 is a pointer to a coll_seq_t object. If the next call to a user function
** or aggregate calls __get_func_coll_seq(), this collation sequence will
** be returned. This is used by the built-in min(), max() and nullif()
** functions.
**
** If P1 is not zero, then it is a register that a subsequent min() or
** max() aggregate will set to 1 if the current row is not the minimum or
** maximum.  The P1 register is initialized to 0 by this instruction.
**
** The interface used by the implementation of the aforementioned functions
** to retrieve the collation sequence set by this opcode is not available
** publicly.  Only built-in functions have access to this feature.
*/
case OP_CollSeq: {
  assert( p_op->p4type==P4_COLLSEQ );
  if( p_op->p1 ){
    __vdbe_mem_set_int64(&mems[p_op->p1], 0);
  }
  break;
}

/* Opcode: __bit_and P1 P2 P3 * *
** Synopsis: r[P3]=r[P1]&r[P2]
**
** Take the bit-wise AND of the values in register P1 and P2 and
** store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: __bit_or P1 P2 P3 * *
** Synopsis: r[P3]=r[P1]|r[P2]
**
** Take the bit-wise OR of the values in register P1 and P2 and
** store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: __shift_left P1 P2 P3 * *
** Synopsis: r[P3]=r[P2]<<r[P1]
**
** Shift the integer value in register P2 to the left by the
** number of bits specified by the integer in register P1.
** Store the result in register P3.
** If either input is NULL, the result is NULL.
*/
/* Opcode: __shift_right P1 P2 P3 * *
** Synopsis: r[P3]=r[P2]>>r[P1]
**
** Shift the integer value in register P2 to the right by the
** number of bits specified by the integer in register P1.
** Store the result in register P3.
** If either input is NULL, the result is NULL.
*/
case OP_BitAnd:                 /* same as TK_BITAND, in1, in2, out3 */
case OP_BitOr:                  /* same as TK_BITOR, in1, in2, out3 */
case OP_ShiftLeft:              /* same as TK_LSHIFT, in1, in2, out3 */
case OP_ShiftRight: {           /* same as TK_RSHIFT, in1, in2, out3 */
  int64_t i_a;
  u_int64_t u_a;
  int64_t i_b;
  u_int8_t op;

  in1 = &mems[p_op->p1];
  in2 = &mems[p_op->p2];
  out = &mems[p_op->p3];
  if( (in1->flags | in2->flags) & MEM_Null ){
    __vdbe_mem_set_null(out);
    break;
  }
  i_a = __vdbe_int_value(in2);
  i_b = __vdbe_int_value(in1);
  op = p_op->opcode;
  if( op==OP_BitAnd ){
    i_a &= i_b;
  }else if( op==OP_BitOr ){
    i_a |= i_b;
  }else if( i_b!=0 ){
    assert( op==OP_ShiftRight || op==OP_ShiftLeft );

    /* If shifting by a negative amount, shift in the other direction */
    if( i_b<0 ){
      assert( OP_ShiftRight==OP_ShiftLeft+1 );
      op = 2*OP_ShiftLeft + 1 - op;
      i_b = i_b>(-64) ? -i_b : 64;
    }

    if( i_b>=64 ){
      i_a = (i_a>=0 || op==OP_ShiftLeft) ? 0 : -1;
    }else{
      memcpy(&u_a, &i_a, sizeof(u_a));
      if( op==OP_ShiftLeft ){
        u_a <<= i_b;
      }else{
        u_a >>= i_b;
        /* Sign-extend on a right shift of a negative number */
        if( i_a<0 ) u_a |= ((((u_int64_t)0xffffffff)<<32)|0xffffffff) << (64-i_b);
      }
      memcpy(&i_a, &u_a, sizeof(i_a));
    }
  }
  out->u.i = i_a;
  MEM_SET_TYPE_FLAG(out, MEM_Int);
  break;
}

/* Opcode: __add_imm  P1 P2 * * *
** Synopsis: r[P1]=r[P1]+P2
**
** Add the constant P2 to the value in register P1.
** The result is always an integer.
**
** To force any register to be an integer, just add 0.
*/
case OP_AddImm: {            /* in1 */
  in1 = &mems[p_op->p1];
  MEM_ABOUT_TO_CHANGE(p, in1);
  __vdbe_mem_integerify(in1);
  *(u_int64_t*)&in1->u.i += (u_int64_t)p_op->p2;
  break;
}

/* Opcode: __must_be_int P1 P2 * * *
**
** Force the value in register P1 to be an integer.  If the value
** in P1 is not an integer and cannot be converted into an integer
** without data loss, then jump immediately to P2, or if P2==0
** raise an DBSQL_MISMATCH exception.
*/
case OP_MustBeInt: {            /* jump0, in1 */
  in1 = &mems[p_op->p1];
  if( (in1->flags & MEM_Int)==0 ){
    __apply_affinity(in1, DBSQL_AFF_NUMERIC, encoding);
    if( (in1->flags & MEM_Int)==0 ){
      VDBE_BRANCH_TAKEN(1, 2);
      if( p_op->p2==0 ){
        rc = DBSQL_MISMATCH;
        goto abort_due_to_error;
      }else{
        goto jump_to_p2;
      }
    }
  }
  VDBE_BRANCH_TAKEN(0, 2);
  MEM_SET_TYPE_FLAG(in1, MEM_Int);
  break;
}

#ifndef DBSQL_OMIT_FLOATING_POINT
/* Opcode: __real_affinity P1 * * * *
**
** If register P1 holds an integer convert it to a real value.
**
** This opcode is used when extracting information from a column that
** has REAL affinity.  Such column values may still be stored as
** integers, for space efficiency, but after extraction we want them
** to have only a real value.
*/
case OP_RealAffinity: {                  /* in1 */
  in1 = &mems[p_op->p1];
  if( in1->flags & (MEM_Int|MEM_IntReal) ){
    testcase( in1->flags & MEM_Int );
    testcase( in1->flags & MEM_IntReal );
    __vdbe_mem_realify(in1);
    REGISTER_TRACE(p_op->p1, in1);
  }
  break;
}
#endif

#if !defined(DBSQL_OMIT_CAST) || !defined(DBSQL_OMIT_ANALYZE)
/* Opcode: Cast P1 P2 * * *
** Synopsis: affinity(r[P1])
**
** Force the value in register P1 to be the type defined by P2.
**
** <ul>
** <li> P2=='A' &rarr; BLOB
** <li> P2=='B' &rarr; TEXT
** <li> P2=='C' &rarr; NUMERIC
** <li> P2=='D' &rarr; INTEGER
** <li> P2=='E' &rarr; REAL
** </ul>
**
** A NULL value is not changed by this routine.  It remains NULL.
*/
case OP_Cast: {                  /* in1 */
  assert( p_op->p2>=DBSQL_AFF_BLOB && p_op->p2<=DBSQL_AFF_REAL );
  testcase( p_op->p2==DBSQL_AFF_TEXT );
  testcase( p_op->p2==DBSQL_AFF_BLOB );
  testcase( p_op->p2==DBSQL_AFF_NUMERIC );
  testcase( p_op->p2==DBSQL_AFF_INTEGER );
  testcase( p_op->p2==DBSQL_AFF_REAL );
  in1 = &mems[p_op->p1];
  MEM_ABOUT_TO_CHANGE(p, in1);
  rc = EXPAND_BLOB(in1);
  if( rc ) goto abort_due_to_error;
  rc = __vdbe_mem_cast(in1, p_op->p2, encoding);
  if( rc ) goto abort_due_to_error;
  UPDATE_MAX_BLOBSIZE(in1);
  REGISTER_TRACE(p_op->p1, in1);
  break;
}
#endif /* DBSQL_OMIT_CAST */

/* Opcode: Eq P1 P2 P3 P4 P5
** Synopsis: IF r[P3]==r[P1]
**
** Compare the values in register P1 and P3.  If reg(P3)==reg(P1) then
** jump to address P2.
**
** The DBSQL_AFF_MASK portion of P5 must be an affinity character -
** DBSQL_AFF_TEXT, DBSQL_AFF_INTEGER, and so forth. An attempt is made
** to coerce both inputs according to this affinity before the
** comparison is made. If the DBSQL_AFF_MASK is 0x00, then numeric
** affinity is used. Note that the affinity conversions are stored
** back into the input registers P1 and P3.  So this opcode can cause
** persistent changes to registers P1 and P3.
**
** Once any conversions have taken place, and neither value is NULL,
** the values are compared. If both values are blobs then memcmp() is
** used to determine the results of the comparison.  If both values
** are text, then the appropriate collating function specified in
** P4 is used to do the comparison.  If P4 is not specified then
** memcmp() is used to compare text string.  If both values are
** numeric, then a numeric comparison is used. If the two values
** are of different types, then numbers are considered less than
** strings and strings are considered less than blobs.
**
** If DBSQL_NULLEQ is set in P5 then the result of comparison is always either
** true or false and is never NULL.  If both operands are NULL then the result
** of comparison is true.  If either operand is NULL then the result is false.
** If neither operand is NULL the result is the same as it would be if
** the DBSQL_NULLEQ flag were omitted from P5.
**
** This opcode saves the result of comparison for use by the new
** OP_Jump opcode.
*/
/* Opcode: Ne P1 P2 P3 P4 P5
** Synopsis: IF r[P3]!=r[P1]
**
** This works just like the Eq opcode except that the jump is taken if
** the operands in registers P1 and P3 are not equal.  See the Eq opcode for
** additional information.
*/
/* Opcode: Lt P1 P2 P3 P4 P5
** Synopsis: IF r[P3]<r[P1]
**
** Compare the values in register P1 and P3.  If reg(P3)<reg(P1) then
** jump to address P2.
**
** If the DBSQL_JUMPIFNULL bit of P5 is set and either reg(P1) or
** reg(P3) is NULL then the take the jump.  If the DBSQL_JUMPIFNULL
** bit is clear then fall through if either operand is NULL.
**
** The DBSQL_AFF_MASK portion of P5 must be an affinity character -
** DBSQL_AFF_TEXT, DBSQL_AFF_INTEGER, and so forth. An attempt is made
** to coerce both inputs according to this affinity before the
** comparison is made. If the DBSQL_AFF_MASK is 0x00, then numeric
** affinity is used. Note that the affinity conversions are stored
** back into the input registers P1 and P3.  So this opcode can cause
** persistent changes to registers P1 and P3.
**
** Once any conversions have taken place, and neither value is NULL,
** the values are compared. If both values are blobs then memcmp() is
** used to determine the results of the comparison.  If both values
** are text, then the appropriate collating function specified in
** P4 is  used to do the comparison.  If P4 is not specified then
** memcmp() is used to compare text string.  If both values are
** numeric, then a numeric comparison is used. If the two values
** are of different types, then numbers are considered less than
** strings and strings are considered less than blobs.
**
** This opcode saves the result of comparison for use by the new
** OP_Jump opcode.
*/
/* Opcode: Le P1 P2 P3 P4 P5
** Synopsis: IF r[P3]<=r[P1]
**
** This works just like the Lt opcode except that the jump is taken if
** the content of register P3 is less than or equal to the content of
** register P1.  See the Lt opcode for additional information.
*/
/* Opcode: Gt P1 P2 P3 P4 P5
** Synopsis: IF r[P3]>r[P1]
**
** This works just like the Lt opcode except that the jump is taken if
** the content of register P3 is greater than the content of
** register P1.  See the Lt opcode for additional information.
*/
/* Opcode: Ge P1 P2 P3 P4 P5
** Synopsis: IF r[P3]>=r[P1]
**
** This works just like the Lt opcode except that the jump is taken if
** the content of register P3 is greater than or equal to the content of
** register P1.  See the Lt opcode for additional information.
*/
case OP_Eq:               /* same as TK_EQ, jump, in1, in3 */
case OP_Ne:               /* same as TK_NE, jump, in1, in3 */
case OP_Lt:               /* same as TK_LT, jump, in1, in3 */
case OP_Le:               /* same as TK_LE, jump, in1, in3 */
case OP_Gt:               /* same as TK_GT, jump, in1, in3 */
case OP_Ge: {             /* same as TK_GE, jump, in1, in3 */
  int res, res2;      /* Result of the comparison of in1 against in3 */
  char affinity;      /* Affinity to use for comparison */
  u_int16_t flags1;         /* Copy of initial value of in1->flags */
  u_int16_t flags3;         /* Copy of initial value of in3->flags */

  in1 = &mems[p_op->p1];
  in3 = &mems[p_op->p3];
  flags1 = in1->flags;
  flags3 = in3->flags;
  if( (flags1 & flags3 & MEM_Int)!=0 ){
    /* Common case of comparison of two integers */
    if( in3->u.i > in1->u.i ){
      if( __dbsqlaGTb[p_op->opcode] ){
        VDBE_BRANCH_TAKEN(1, (p_op->p5 & DBSQL_NULLEQ)?2:3);
        goto jump_to_p2;
      }
      i_compare = +1;
      VVA_ONLY( i_compare_is_init = 1; )
    }else if( in3->u.i < in1->u.i ){
      if( __dbsqlaLTb[p_op->opcode] ){
        VDBE_BRANCH_TAKEN(1, (p_op->p5 & DBSQL_NULLEQ)?2:3);
        goto jump_to_p2;
      }
      i_compare = -1;
      VVA_ONLY( i_compare_is_init = 1; )
    }else{
      if( __dbsqlaEQb[p_op->opcode] ){
        VDBE_BRANCH_TAKEN(1, (p_op->p5 & DBSQL_NULLEQ)?2:3);
        goto jump_to_p2;
      }
      i_compare = 0;
      VVA_ONLY( i_compare_is_init = 1; )
    }
    VDBE_BRANCH_TAKEN(0, (p_op->p5 & DBSQL_NULLEQ)?2:3);
    break;
  }
  if( (flags1 | flags3)&MEM_Null ){
    /* One or both operands are NULL */
    if( p_op->p5 & DBSQL_NULLEQ ){
      /* If DBSQL_NULLEQ is set (which will only happen if the operator is
      ** OP_Eq or OP_Ne) then take the jump or not depending on whether
      ** or not both operands are null.
      */
      assert( (flags1 & MEM_Cleared)==0 );
      assert( (p_op->p5 & DBSQL_JUMPIFNULL)==0 || CORRUPT_DB );
      testcase( (p_op->p5 & DBSQL_JUMPIFNULL)!=0 );
      if( (flags1&flags3&MEM_Null)!=0
       && (flags3&MEM_Cleared)==0
      ){
        res = 0;  /* Operands are equal */
      }else{
        res = ((flags3 & MEM_Null) ? -1 : +1);  /* Operands are not equal */
      }
    }else{
      /* DBSQL_NULLEQ is clear and at least one operand is NULL,
      ** then the result is always NULL.
      ** The jump is taken if the DBSQL_JUMPIFNULL bit is set.
      */
      VDBE_BRANCH_TAKEN(2,3);
      if( p_op->p5 & DBSQL_JUMPIFNULL ){
        goto jump_to_p2;
      }
      i_compare = 1;    /* Operands are not equal */
      VVA_ONLY( i_compare_is_init = 1; )
      break;
    }
  }else{
    /* Neither operand is NULL and we couldn't do the special high-speed
    ** integer comparison case.  So do a general-case comparison. */
    affinity = p_op->p5 & DBSQL_AFF_MASK;
    if( affinity>=DBSQL_AFF_NUMERIC ){
      if( (flags1 | flags3)&MEM_Str ){
        if( (flags1 & (MEM_Int|MEM_IntReal|MEM_Real|MEM_Str))==MEM_Str ){
          __apply_numeric_affinity(in1,0);
          assert( flags3==in3->flags || CORRUPT_DB );
          flags3 = in3->flags;
        }
        if( (flags3 & (MEM_Int|MEM_IntReal|MEM_Real|MEM_Str))==MEM_Str ){
          __apply_numeric_affinity(in3,0);
        }
      }
    }else if( affinity==DBSQL_AFF_TEXT && ((flags1 | flags3) & MEM_Str)!=0 ){
      if( (flags1 & MEM_Str)!=0 ){
        in1->flags &= ~(MEM_Int|MEM_Real|MEM_IntReal);
      }else if( (flags1&(MEM_Int|MEM_Real|MEM_IntReal))!=0 ){
        testcase( in1->flags & MEM_Int );
        testcase( in1->flags & MEM_Real );
        testcase( in1->flags & MEM_IntReal );
        __vdbe_mem_stringify(in1, encoding, 1);
        testcase( (flags1&MEM_Dyn) != (in1->flags&MEM_Dyn) );
        flags1 = (in1->flags & ~MEM_TypeMask) | (flags1 & MEM_TypeMask);
        if( NEVER(in1==in3) ) flags3 = flags1 | MEM_Str;
      }
      if( (flags3 & MEM_Str)!=0 ){
        in3->flags &= ~(MEM_Int|MEM_Real|MEM_IntReal);
      }else if( (flags3&(MEM_Int|MEM_Real|MEM_IntReal))!=0 ){
        testcase( in3->flags & MEM_Int );
        testcase( in3->flags & MEM_Real );
        testcase( in3->flags & MEM_IntReal );
        __vdbe_mem_stringify(in3, encoding, 1);
        testcase( (flags3&MEM_Dyn) != (in3->flags&MEM_Dyn) );
        flags3 = (in3->flags & ~MEM_TypeMask) | (flags3 & MEM_TypeMask);
      }
    }
    assert( p_op->p4type==P4_COLLSEQ || p_op->p4.coll==0 );
    res = __mem_compare(in3, in1, p_op->p4.coll);
  }

  /* At this point, res is negative, zero, or positive if reg[P1] is
  ** less than, equal to, or greater than reg[P3], respectively.  Compute
  ** the answer to this operator in res2, depending on what the comparison
  ** operator actually is.  The next block of code depends on the fact
  ** that the 6 comparison operators are consecutive integers in this
  ** order:  NE, EQ, GT, LE, LT, GE */
  assert( OP_Eq==OP_Ne+1 ); assert( OP_Gt==OP_Ne+2 ); assert( OP_Le==OP_Ne+3 );
  assert( OP_Lt==OP_Ne+4 ); assert( OP_Ge==OP_Ne+5 );
  if( res<0 ){
    res2 = __dbsqlaLTb[p_op->opcode];
  }else if( res==0 ){
    res2 = __dbsqlaEQb[p_op->opcode];
  }else{
    res2 = __dbsqlaGTb[p_op->opcode];
  }
  i_compare = res;
  VVA_ONLY( i_compare_is_init = 1; )

  /* Undo any changes made by __apply_affinity() to the input registers. */
  assert( (in3->flags & MEM_Dyn) == (flags3 & MEM_Dyn) );
  in3->flags = flags3;
  assert( (in1->flags & MEM_Dyn) == (flags1 & MEM_Dyn) );
  in1->flags = flags1;

  VDBE_BRANCH_TAKEN(res2!=0, (p_op->p5 & DBSQL_NULLEQ)?2:3);
  if( res2 ){
    goto jump_to_p2;
  }
  break;
}

/* Opcode: __else_eq * P2 * * *
**
** This opcode must follow an OP_Lt or OP_Gt comparison operator.  There
** can be zero or more OP_ReleaseReg opcodes intervening, but no other
** opcodes are allowed to occur between this instruction and the previous
** OP_Lt or OP_Gt.
**
** If the result of an OP_Eq comparison on the same two operands as
** the prior OP_Lt or OP_Gt would have been true, then jump to P2.  If
** the result of an OP_Eq comparison on the two previous operands
** would have been false or NULL, then fall through.
*/
case OP_ElseEq: {       /* same as TK_ESCAPE, jump */

#ifdef DBSQL_DEBUG
  /* Verify the preconditions of this opcode - that it follows an OP_Lt or
  ** OP_Gt with zero or more intervening OP_ReleaseReg opcodes */
  int i_addr;
  for(i_addr = (int)(p_op - ops) - 1; ALWAYS(i_addr>=0); i_addr--){
    if( ops[i_addr].opcode==OP_ReleaseReg ) continue;
    assert( ops[i_addr].opcode==OP_Lt || ops[i_addr].opcode==OP_Gt );
    break;
  }
#endif /* DBSQL_DEBUG */
  assert( i_compare_is_init );
  VDBE_BRANCH_TAKEN(i_compare==0, 2);
  if( i_compare==0 ) goto jump_to_p2;
  break;
}


/* Opcode: Permutation * * * P4 *
**
** Set the permutation used by the OP_Compare operator in the next
** instruction.  The permutation is stored in the P4 operand.
**
** The permutation is only valid for the next opcode which must be
** an OP_Compare that has the OPFLAG_PERMUTE bit set in P5.
**
** The first integer in the P4 integer array is the length of the array
** and does not become part of the permutation.
*/
case OP_Permutation: {
  assert( p_op->p4type==P4_INTARRAY );
  assert( p_op->p4.ai );
  assert( p_op[1].opcode==OP_Compare );
  assert( p_op[1].p5 & OPFLAG_PERMUTE );
  break;
}

/* Opcode: Compare P1 P2 P3 P4 P5
** Synopsis: r[P1@P3] <-> r[P2@P3]
**
** Compare two vectors of registers in reg(P1)..reg(P1+P3-1) (call this
** vector "A") and in reg(P2)..reg(P2+P3-1) ("B").  Save the result of
** the comparison for use by the next OP_Jump instruct.
**
** If P5 has the OPFLAG_PERMUTE bit set, then the order of comparison is
** determined by the most recent OP_Permutation operator.  If the
** OPFLAG_PERMUTE bit is clear, then register are compared in sequential
** order.
**
** P4 is a key_info_t structure that defines collating sequences and sort
** orders for the comparison.  The permutation applies to registers
** only.  The key_info_t elements are used sequentially.
**
** The comparison is a sort comparison, so NULLs compare equal,
** NULLs are less than numbers, numbers are less than strings,
** and strings are less than blobs.
**
** This opcode must be immediately followed by an OP_Jump opcode.
*/
case OP_Compare: {
  int n;
  int i;
  int p1;
  int p2;
  const key_info_t *key_info;
  u_int32_t idx;
  coll_seq_t *coll;    /* Collating sequence to use on this term */
  int rev;          /* True for DESCENDING sort order */
  u_int32_t *permutes;     /* The permutation */

  if( (p_op->p5 & OPFLAG_PERMUTE)==0 ){
    permutes = 0;
  }else{
    assert( p_op>ops );
    assert( p_op[-1].opcode==OP_Permutation );
    assert( p_op[-1].p4type==P4_INTARRAY );
    permutes = p_op[-1].p4.ai + 1;
    assert( permutes!=0 );
  }
  n = p_op->p3;
  key_info = p_op->p4.key_info;
  assert( n>0 );
  assert( key_info!=0 );
  assert( key_info->sort_flags!=0 );
  p1 = p_op->p1;
  p2 = p_op->p2;
#ifdef DBSQL_DEBUG
  if( permutes ){
    int k, mx = 0;
    for(k=0; k<n; k++) if( permutes[k]>(u_int32_t)mx ) mx = permutes[k];
    assert( p1>0 && p1+mx<=(p->n_mem+1 - p->n_cursor)+1 );
    assert( p2>0 && p2+mx<=(p->n_mem+1 - p->n_cursor)+1 );
  }else{
    assert( p1>0 && p1+n<=(p->n_mem+1 - p->n_cursor)+1 );
    assert( p2>0 && p2+n<=(p->n_mem+1 - p->n_cursor)+1 );
  }
#endif /* DBSQL_DEBUG */
  for(i=0; i<n; i++){
    idx = permutes ? permutes[i] : (u_int32_t)i;
    assert( MEM_IS_VALID(&mems[p1+idx]) );
    assert( MEM_IS_VALID(&mems[p2+idx]) );
    REGISTER_TRACE(p1+idx, &mems[p1+idx]);
    REGISTER_TRACE(p2+idx, &mems[p2+idx]);
    assert( i<key_info->n_key_field );
    coll = key_info->colls[i];
    rev = (key_info->sort_flags[i] & KEYINFO_ORDER_DESC);
    i_compare = __mem_compare(&mems[p1+idx], &mems[p2+idx], coll);
    VVA_ONLY( i_compare_is_init = 1; )
    if( i_compare ){
      if( (key_info->sort_flags[i] & KEYINFO_ORDER_BIGNULL)
       && ((mems[p1+idx].flags & MEM_Null) || (mems[p2+idx].flags & MEM_Null))
      ){
        i_compare = -i_compare;
      }
      if( rev ) i_compare = -i_compare;
      break;
    }
  }
  assert( p_op[1].opcode==OP_Jump );
  break;
}

/* Opcode: Jump P1 P2 P3 * *
**
** Jump to the instruction at address P1, P2, or P3 depending on whether
** in the most recent OP_Compare instruction the P1 vector was less than,
** equal to, or greater than the P2 vector, respectively.
**
** This opcode must immediately follow an OP_Compare opcode.
*/
case OP_Jump: {             /* jump */
  assert( p_op>ops && p_op[-1].opcode==OP_Compare );
  assert( i_compare_is_init );
  if( i_compare<0 ){
    VDBE_BRANCH_TAKEN(0,4); p_op = &ops[p_op->p1 - 1];
  }else if( i_compare==0 ){
    VDBE_BRANCH_TAKEN(1,4); p_op = &ops[p_op->p2 - 1];
  }else{
    VDBE_BRANCH_TAKEN(2,4); p_op = &ops[p_op->p3 - 1];
  }
  break;
}

/* Opcode: And P1 P2 P3 * *
** Synopsis: r[P3]=(r[P1] && r[P2])
**
** Take the logical AND of the values in registers P1 and P2 and
** write the result into register P3.
**
** If either P1 or P2 is 0 (false) then the result is 0 even if
** the other input is NULL.  A NULL and true or two NULLs give
** a NULL output.
*/
/* Opcode: Or P1 P2 P3 * *
** Synopsis: r[P3]=(r[P1] || r[P2])
**
** Take the logical OR of the values in register P1 and P2 and
** store the answer in register P3.
**
** If either P1 or P2 is nonzero (true) then the result is 1 (true)
** even if the other input is NULL.  A NULL and false or two NULLs
** give a NULL output.
*/
case OP_And:              /* same as TK_AND, in1, in2, out3 */
case OP_Or: {             /* same as TK_OR, in1, in2, out3 */
  int v1;    /* Left operand:  0==FALSE, 1==TRUE, 2==UNKNOWN or NULL */
  int v2;    /* Right operand: 0==FALSE, 1==TRUE, 2==UNKNOWN or NULL */

  v1 = __vdbe_boolean_value(&mems[p_op->p1], 2);
  v2 = __vdbe_boolean_value(&mems[p_op->p2], 2);
  if( p_op->opcode==OP_And ){
    static const unsigned char and_logic[] = { 0, 0, 0, 0, 1, 2, 0, 2, 2 };
    v1 = and_logic[v1*3+v2];
  }else{
    static const unsigned char or_logic[] = { 0, 1, 2, 1, 1, 1, 2, 1, 2 };
    v1 = or_logic[v1*3+v2];
  }
  out = &mems[p_op->p3];
  if( v1==2 ){
    MEM_SET_TYPE_FLAG(out, MEM_Null);
  }else{
    out->u.i = v1;
    MEM_SET_TYPE_FLAG(out, MEM_Int);
  }
  break;
}

/* Opcode: __is_true P1 P2 P3 P4 *
** Synopsis: r[P2] = coalesce(r[P1]==TRUE,P3) ^ P4
**
** This opcode implements the IS TRUE, IS FALSE, IS NOT TRUE, and
** IS NOT FALSE operators.
**
** Interpret the value in register P1 as a boolean value.  Store that
** boolean (a 0 or 1) in register P2.  Or if the value in register P1 is
** NULL, then the P3 is stored in register P2.  Invert the answer if P4
** is 1.
**
** The logic is summarized like this:
**
** <ul>
** <li> If P3==0 and P4==0  then  r[P2] := r[P1] IS TRUE
** <li> If P3==1 and P4==1  then  r[P2] := r[P1] IS FALSE
** <li> If P3==0 and P4==1  then  r[P2] := r[P1] IS NOT TRUE
** <li> If P3==1 and P4==0  then  r[P2] := r[P1] IS NOT FALSE
** </ul>
*/
case OP_IsTrue: {               /* in1, out2 */
  assert( p_op->p4type==P4_INT32 );
  assert( p_op->p4.i==0 || p_op->p4.i==1 );
  assert( p_op->p3==0 || p_op->p3==1 );
  __vdbe_mem_set_int64(&mems[p_op->p2],
      __vdbe_boolean_value(&mems[p_op->p1], p_op->p3) ^ p_op->p4.i);
  break;
}

/* Opcode: Not P1 P2 * * *
** Synopsis: r[P2]= !r[P1]
**
** Interpret the value in register P1 as a boolean value.  Store the
** boolean complement in register P2.  If the value in register P1 is
** NULL, then a NULL is stored in P2.
*/
case OP_Not: {                /* same as TK_NOT, in1, out2 */
  in1 = &mems[p_op->p1];
  out = &mems[p_op->p2];
  if( (in1->flags & MEM_Null)==0 ){
    __vdbe_mem_set_int64(out, !__vdbe_boolean_value(in1,0));
  }else{
    __vdbe_mem_set_null(out);
  }
  break;
}

/* Opcode: __bit_not P1 P2 * * *
** Synopsis: r[P2]= ~r[P1]
**
** Interpret the content of register P1 as an integer.  Store the
** ones-complement of the P1 value into register P2.  If P1 holds
** a NULL then store a NULL in P2.
*/
case OP_BitNot: {             /* same as TK_BITNOT, in1, out2 */
  in1 = &mems[p_op->p1];
  out = &mems[p_op->p2];
  __vdbe_mem_set_null(out);
  if( (in1->flags & MEM_Null)==0 ){
    out->flags = MEM_Int;
    out->u.i = ~__vdbe_int_value(in1);
  }
  break;
}

/* Opcode: Once P1 P2 P3 * *
**
** Fall through to the next instruction the first time this opcode is
** encountered on each invocation of the byte-code program.  Jump to P2
** on the second and all subsequent encounters during the same invocation.
**
** Top-level programs determine first invocation by comparing the P1
** operand against the P1 operand on the OP_Init opcode at the beginning
** of the program.  If the P1 values differ, then fall through and make
** the P1 of this opcode equal to the P1 of OP_Init.  If P1 values are
** the same then take the jump.
**
** For subprograms, there is a bitmask in the vdbe_frame_t that determines
** whether or not the jump should be taken.  The bitmask is necessary
** because the self-altering code trick does not work for recursive
** triggers.
**
** The P3 operand is not used directly by this opcode.  However P3 is
** used by the code generator as follows:  If this opcode is the start
** of a subroutine and that subroutine uses a Bloom filter, then P3 will
** be the register that holds that Bloom filter.  See tag-202407032019
** in the source code for implementation details.
*/
case OP_Once: {             /* jump */
  u_int32_t i_addr;                /* Address of this instruction */
  assert( p->ops[0].opcode==OP_Init );
  if( p->frame ){
    i_addr = (int)(p_op - p->ops);
    if( (p->frame->onces[i_addr/8] & (1<<(i_addr & 7)))!=0 ){
      VDBE_BRANCH_TAKEN(1, 2);
      goto jump_to_p2;
    }
    p->frame->onces[i_addr/8] |= 1<<(i_addr & 7);
  }else{
    if( p->ops[0].p1==p_op->p1 ){
      VDBE_BRANCH_TAKEN(1, 2);
      goto jump_to_p2;
    }
  }
  VDBE_BRANCH_TAKEN(0, 2);
  p_op->p1 = p->ops[0].p1;
  break;
}

/* Opcode: If P1 P2 P3 * *
**
** Jump to P2 if the value in register P1 is true.  The value
** is considered true if it is numeric and non-zero.  If the value
** in P1 is NULL then take the jump if and only if P3 is non-zero.
*/
case OP_If:  {               /* jump, in1 */
  int c;
  c = __vdbe_boolean_value(&mems[p_op->p1], p_op->p3);
  VDBE_BRANCH_TAKEN(c!=0, 2);
  if( c ) goto jump_to_p2;
  break;
}

/* Opcode: __if_not P1 P2 P3 * *
**
** Jump to P2 if the value in register P1 is False.  The value
** is considered false if it has a numeric value of zero.  If the value
** in P1 is NULL then take the jump if and only if P3 is non-zero.
*/
case OP_IfNot: {            /* jump, in1 */
  int c;
  c = !__vdbe_boolean_value(&mems[p_op->p1], !p_op->p3);
  VDBE_BRANCH_TAKEN(c!=0, 2);
  if( c ) goto jump_to_p2;
  break;
}

/* Opcode: __is_null P1 P2 * * *
** Synopsis: if r[P1]==NULL goto P2
**
** Jump to P2 if the value in register P1 is NULL.
*/
case OP_IsNull: {            /* same as TK_ISNULL, jump, in1 */
  in1 = &mems[p_op->p1];
  VDBE_BRANCH_TAKEN( (in1->flags & MEM_Null)!=0, 2);
  if( (in1->flags & MEM_Null)!=0 ){
    goto jump_to_p2;
  }
  break;
}

/* Opcode: __is_type P1 P2 P3 P4 P5
** Synopsis: if typeof(P1.P3) in P5 goto P2
**
** Jump to P2 if the type of a column in a btree is one of the types specified
** by the P5 bitmask.
**
** P1 is normally a cursor on a btree for which the row decode cache is
** valid through at least column P3.  In other words, there should have been
** a prior OP_Column for column P3 or greater.  If the cursor is not valid,
** then this opcode might give spurious results.
** The the btree row has fewer than P3 columns, then use P4 as the
** datatype.
**
** If P1 is -1, then P3 is a register number and the datatype is taken
** from the value in that register.
**
** P5 is a bitmask of data types.  DBSQL_INTEGER is the least significant
** (0x01) bit. DBSQL_FLOAT is the 0x02 bit. DBSQL_TEXT is 0x04.
** DBSQL_BLOB is 0x08.  DBSQL_NULL is 0x10.
**
** WARNING: This opcode does not reliably distinguish between NULL and REAL
** when P1>=0.  If the database contains a __na_n value, this opcode will think
** that the datatype is REAL when it should be NULL.  When P1<0 and the value
** is already stored in register P3, then this opcode does reliably
** distinguish between NULL and REAL.  The problem only arises then P1>=0.
**
** Take the jump to address P2 if and only if the datatype of the
** value determined by P1 and P3 corresponds to one of the bits in the
** P5 bitmask.
**
*/
case OP_IsType: {        /* jump */
  vdbe_cursor_t *p_c;
  u_int16_t type_mask;
  u_int32_t serial_type_;

  assert( p_op->p1>=(-1) && p_op->p1<p->n_cursor );
  assert( p_op->p1>=0 || (p_op->p3>=0 && p_op->p3<=(p->n_mem+1 - p->n_cursor)) );
  if( p_op->p1>=0 ){
    p_c = p->csrs[p_op->p1];
    assert( p_c!=0 );
    assert( p_op->p3>=0 );
    if( p_op->p3<p_c->n_hdr_parsed ){
      serial_type_ = p_c->types[p_op->p3];
      if( serial_type_>=12 ){
        if( serial_type_&1 ){
          type_mask = 0x04;   /* DBSQL_TEXT */
        }else{
          type_mask = 0x08;   /* DBSQL_BLOB */
        }
      }else{
        static const unsigned char masks[] = {
           0x10, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x2,
           0x01, 0x01, 0x10, 0x10
        };
        testcase( serial_type_==0 );
        testcase( serial_type_==1 );
        testcase( serial_type_==2 );
        testcase( serial_type_==3 );
        testcase( serial_type_==4 );
        testcase( serial_type_==5 );
        testcase( serial_type_==6 );
        testcase( serial_type_==7 );
        testcase( serial_type_==8 );
        testcase( serial_type_==9 );
        testcase( serial_type_==10 );
        testcase( serial_type_==11 );
        type_mask = masks[serial_type_];
      }
    }else{
      type_mask = 1 << (p_op->p4.i - 1);
      testcase( type_mask==0x01 );
      testcase( type_mask==0x02 );
      testcase( type_mask==0x04 );
      testcase( type_mask==0x08 );
      testcase( type_mask==0x10 );
    }
  }else{
    assert( MEM_IS_VALID(&mems[p_op->p3]) );
    type_mask = 1 << (dbsql_value_type((dbsql_value_t*)&mems[p_op->p3])-1);
    testcase( type_mask==0x01 );
    testcase( type_mask==0x02 );
    testcase( type_mask==0x04 );
    testcase( type_mask==0x08 );
    testcase( type_mask==0x10 );
  }
  VDBE_BRANCH_TAKEN( (type_mask & p_op->p5)!=0, 2);
  if( type_mask & p_op->p5 ){
    goto jump_to_p2;
  }
  break;
}

/* Opcode: __zero_or_null P1 P2 P3 * *
** Synopsis: r[P2] = 0 OR NULL
**
** If both registers P1 and P3 are NOT NULL, then store a zero in
** register P2.  If either registers P1 or P3 are NULL then put
** a NULL in register P2.
*/
case OP_ZeroOrNull: {            /* in1, in2, out2, in3 */
  if( (mems[p_op->p1].flags & MEM_Null)!=0
   || (mems[p_op->p3].flags & MEM_Null)!=0
  ){
    __vdbe_mem_set_null(mems + p_op->p2);
  }else{
    __vdbe_mem_set_int64(mems + p_op->p2, 0);
  }
  break;
}

/* Opcode: __not_null P1 P2 * * *
** Synopsis: if r[P1]!=NULL goto P2
**
** Jump to P2 if the value in register P1 is not NULL. 
*/
case OP_NotNull: {            /* same as TK_NOTNULL, jump, in1 */
  in1 = &mems[p_op->p1];
  VDBE_BRANCH_TAKEN( (in1->flags & MEM_Null)==0, 2);
  if( (in1->flags & MEM_Null)==0 ){
    goto jump_to_p2;
  }
  break;
}

/* Opcode: __if_null_row P1 P2 P3 * *
** Synopsis: if P1.null_row then r[P3]=NULL, goto P2
**
** Check the cursor P1 to see if it is currently pointing at a NULL row.
** If it is, then set register P3 to NULL and jump immediately to P2.
** If P1 is not on a NULL row, then fall through without making any
** changes.
**
** If P1 is not an open cursor, then this opcode is a no-op.
*/
case OP_IfNullRow: {         /* jump */
  vdbe_cursor_t *p_c;
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  if( p_c && p_c->null_row ){
    __vdbe_mem_set_null(mems + p_op->p3);
    goto jump_to_p2;
  }
  break;
}

#ifdef DBSQL_ENABLE_OFFSET_SQL_FUNC
/* Opcode: Offset P1 P2 P3 * *
** Synopsis: r[P3] = dbsql_offset(P1)
**
** Store in register r[P3] the byte offset into the database file that is the
** start of the payload for the record at which that cursor P1 is currently
** pointing.
**
** P2 is the column number for the argument to the dbsql_offset() function.
** This opcode does not use P2 itself, but the P2 value is used by the
** code generator.  The P1, P2, and P3 operands to this opcode are the
** same as for OP_Column.
**
** This opcode is only available if dbsql is compiled with the
** -DSQLITE_ENABLE_OFFSET_SQL_FUNC option.
*/
case OP_Offset: {          /* out3 */
  vdbe_cursor_t *p_c;    /* The VDBE cursor */
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  out = &p->mems[p_op->p3];
  if( p_c==0 || p_c->cur_type!=CURTYPE_BTREE ){
    __vdbe_mem_set_null(out);
  }else{
    if( p_c->deferred_moveto ){
      rc = __vdbe_finish_moveto(p_c);
      if( rc ) goto abort_due_to_error;
    }
    if( __sm_eof(p_c->uc.cursor) ){
      __vdbe_mem_set_null(out);
    }else{
      __vdbe_mem_set_int64(out, __sm_offset(p_c->uc.cursor));
    }
  }
  break;
}
#endif /* DBSQL_ENABLE_OFFSET_SQL_FUNC */

/* Opcode: column_t P1 P2 P3 P4 P5
** Synopsis: r[P3]=PX cursor P1 column P2
**
** Interpret the data that cursor P1 points to as a structure built using
** the __make_record instruction.  (See the __make_record opcode for additional
** information about the format of the data.)  Extract the P2-th column
** from this record.  If there are less than (P2+1)
** values in the record, extract a NULL.
**
** The value extracted is stored in register P3.
**
** If the record contains fewer than P2 fields, then extract a NULL.  Or,
** if the P4 argument is a P4_MEM use the value of the P4 argument as
** the result.
**
** If the OPFLAG_LENGTHARG bit is set in P5 then the result is guaranteed
** to only be used by the length() function or the equivalent.  The content
** of large blobs is not loaded, thus saving CPU cycles.  If the
** OPFLAG_TYPEOFARG bit is set then the result will only be used by the
** typeof() function or the IS NULL or IS NOT NULL operators or the
** equivalent.  In this case, all content loading can be omitted.
*/
case OP_Column: {            /* ncycle */
  u_int32_t p2;            /* column number to retrieve */
  vdbe_cursor_t *p_c;    /* The VDBE cursor */
  sm_cursor_t *crsr;   /* The B-Tree cursor corresponding to c */
  u_int32_t *offsets;      /* offsets[i] is offset to start of data for i-th column */
  int len;           /* The length of the serialized data for the column */
  int i;             /* Loop counter */
  mem_t *dest;        /* Where to write the extracted value */
  mem_t s_mem;          /* For storing the record being decoded */
  const u_int8_t *z_data;   /* Part of the record being decoded */
  const u_int8_t *hdr;    /* Next unparsed byte of the header */
  const u_int8_t *end_hdr; /* Pointer to first byte after the header */
  u_int64_t offset64;      /* 64-bit offset */
  u_int32_t t;             /* A type code from the record header */
  mem_t *reg;         /* __pseudo_table input register */

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( p_op->p3>0 && p_op->p3<=(p->n_mem+1 - p->n_cursor) );
  p_c = p->csrs[p_op->p1];
  p2 = (u_int32_t)p_op->p2;

op_column_restart:
  assert( p_c!=0 );
  assert( p2<(u_int32_t)p_c->n_field
       || (p_c->cur_type==CURTYPE_PSEUDO && p_c->seek_result==0) );
  offsets = p_c->offsets;
  assert( offsets==p_c->types+p_c->n_field );
  assert( p_c->cur_type!=CURTYPE_VTAB );
  assert( p_c->cur_type!=CURTYPE_PSEUDO || p_c->null_row );
  assert( p_c->cur_type!=CURTYPE_SORTER );

  if( p_c->cache_status!=p->cache_ctr ){                /*OPTIMIZATION-IF-FALSE*/
    if( p_c->null_row ){
      if( p_c->cur_type==CURTYPE_PSEUDO && p_c->seek_result>0 ){
        /* For the special case of as pseudo-cursor, the seek_result field
        ** identifies the register that holds the record */
        reg = &mems[p_c->seek_result];
        assert( reg->flags & MEM_Blob );
        assert( MEM_IS_VALID(reg) );
        p_c->payload_size = p_c->sz_row = reg->n;
        p_c->rows = (u_int8_t*)reg->z;
      }else{
        dest = &mems[p_op->p3];
        MEM_ABOUT_TO_CHANGE(p, dest);
        __vdbe_mem_set_null(dest);
        goto op_column_out;
      }
    }else{
      crsr = p_c->uc.cursor;
      if( p_c->deferred_moveto ){
        u_int32_t i_map;
        assert( !p_c->is_ephemeral );
        if( p_c->ub.alt_maps && (i_map = p_c->ub.alt_maps[1+p2])>0  ){
          p_c = p_c->alt_cursor;
          p2 = i_map - 1;
          goto op_column_restart;
        }
        rc = __vdbe_finish_moveto(p_c);
        if( rc ) goto abort_due_to_error;
      }else if( __sm_cursor_has_moved(crsr) ){
        rc = __vdbe_handle_moved_cursor(p_c);
        if( rc ) goto abort_due_to_error;
        goto op_column_restart;
      }
      assert( p_c->cur_type==CURTYPE_BTREE );
      assert( crsr );
      assert( __sm_cursor_is_valid(crsr) );
      p_c->payload_size = __sm_payload_size(crsr);
      p_c->rows = __sm_payload_fetch(crsr, &p_c->sz_row);
      assert( p_c->sz_row<=p_c->payload_size );
      assert( p_c->sz_row<=65536 );  /* Maximum page size is 64KiB */
    }
    p_c->cache_status = p->cache_ctr;
    if( (offsets[0] = p_c->rows[0])<0x80 ){
      p_c->i_hdr_offset = 1;
    }else{
      p_c->i_hdr_offset = __get_varint32(p_c->rows, offsets);
    }
    p_c->n_hdr_parsed = 0;

    if( p_c->sz_row<offsets[0] ){      /*OPTIMIZATION-IF-FALSE*/
      /* c->rows does not have to hold the entire row, but it does at least
      ** need to cover the header of the record.  If c->rows does not contain
      ** the complete header, then set it to zero, forcing the header to be
      ** dynamically allocated. */
      p_c->rows = 0;
      p_c->sz_row = 0;

      /* Make sure a corrupt database has not given us an oversize header.
      ** Do this now to avoid an oversize memory allocation.
      **
      ** Type entries can be between 1 and 5 bytes each.  But 4 and 5 byte
      ** types use so much data space that there can only be 4096 and 32 of
      ** them, respectively.  So the maximum header length results from a
      ** 3-byte type for each of the maximum of 32768 columns plus three
      ** extra bytes for the header length itself.  32768*3 + 3 = 98307.
      */
      if( offsets[0] > 98307 || offsets[0] > p_c->payload_size ){
        goto op_column_corrupt;
      }
    }else{
      /* This is an optimization.  By skipping over the first few tests
      ** (ex: c->n_hdr_parsed<=p2) in the next section, we achieve a
      ** measurable performance gain.
      **
      ** This branch is taken even if offsets[0]==0.  Such a record is never
      ** generated by dbsql, and could be considered corruption, but we
      ** accept it for historical reasons.  When offsets[0]==0, the code this
      ** branch jumps to reads past the end of the record, but never more
      ** than a few bytes.  Even if the record occurs at the end of the page
      ** content area, the "page header" comes after the page content and so
      ** this overread is harmless.  Similar overreads can occur for a corrupt
      ** database file.
      */
      z_data = p_c->rows;
      assert( p_c->n_hdr_parsed<=p2 );         /* Conditional skipped */
      testcase( offsets[0]==0 );
      goto op_column_read_header;
    }
  }else if( __sm_cursor_has_moved(p_c->uc.cursor) ){
    rc = __vdbe_handle_moved_cursor(p_c);
    if( rc ) goto abort_due_to_error;
    goto op_column_restart;
  }

  /* Make sure at least the first p2+1 entries of the header have been
  ** parsed and valid information is in offsets[] and c->types[].
  */
  if( p_c->n_hdr_parsed<=p2 ){
    /* If there is more header available for parsing in the record, try
    ** to extract additional fields up through the p2+1-th field
    */
    if( p_c->i_hdr_offset<offsets[0] ){
      /* Make sure data points to enough of the record to cover the header. */
      if( p_c->rows==0 ){
        memset(&s_mem, 0, sizeof(s_mem));
        rc = __vdbe_mem_from_btree_zero_offset(p_c->uc.cursor,offsets[0],&s_mem);
        if( rc!=DBSQL_OK ) goto abort_due_to_error;
        z_data = (u_int8_t*)s_mem.z;
      }else{
        z_data = p_c->rows;
      }
 
      /* Fill in c->types[i] and offsets[i] values through the p2-th field. */
    op_column_read_header:
      i = p_c->n_hdr_parsed;
      offset64 = offsets[i];
      hdr = z_data + p_c->i_hdr_offset;
      end_hdr = z_data + offsets[0];
      testcase( hdr>=end_hdr );
      do{
        if( (p_c->types[i] = t = hdr[0])<0x80 ){
          hdr++;
          offset64 += __vdbe_one_byte_serial_type_len(t);
        }else{
          hdr += __get_varint32(hdr, &t);
          p_c->types[i] = t;
          offset64 += __vdbe_serial_type_len(t);
        }
        offsets[++i] = (u_int32_t)(offset64 & 0xffffffff);
      }while( (u_int32_t)i<=p2 && hdr<end_hdr );

      /* The record is corrupt if any of the following are true:
      ** (1) the bytes of the header extend past the declared header size
      ** (2) the entire header was used but not all data was used
      ** (3) the end of the data extends beyond the end of the record.
      */
      if( (hdr>=end_hdr && (hdr>end_hdr || offset64!=p_c->payload_size))
       || (offset64 > p_c->payload_size)
      ){
        if( offsets[0]==0 ){
          i = 0;
          hdr = end_hdr;
        }else{
          if( p_c->rows==0 ) __vdbe_mem_release(&s_mem);
          goto op_column_corrupt;
        }
      }

      p_c->n_hdr_parsed = i;
      p_c->i_hdr_offset = (u_int32_t)(hdr - z_data);
      if( p_c->rows==0 ) __vdbe_mem_release(&s_mem);
    }else{
      t = 0;
    }

    /* If after trying to extract new entries from the header, n_hdr_parsed is
    ** still not up to p2, that means that the record has fewer than p2
    ** columns.  So the result will be either the default value or a NULL.
    */
    if( p_c->n_hdr_parsed<=p2 ){
      dest = &mems[p_op->p3];
      MEM_ABOUT_TO_CHANGE(p, dest);
      if( p_op->p4type==P4_MEM ){
        __vdbe_mem_shallow_copy(dest, p_op->p4.mem, MEM_Static);
      }else{
        __vdbe_mem_set_null(dest);
      }
      goto op_column_out;
    }
  }else{
    t = p_c->types[p2];
  }

  /* Extract the content for the p2+1-th column.  Control can only
  ** reach this point if offsets[p2], offsets[p2+1], and c->types[p2] are
  ** all valid.
  */
  assert( p2<p_c->n_hdr_parsed );
  assert( rc==DBSQL_OK );
  dest = &mems[p_op->p3];
  MEM_ABOUT_TO_CHANGE(p, dest);
  assert( __vdbe_check_mem_invariants(dest) );
  if( VDBE_MEM_DYNAMIC(dest) ){
    __vdbe_mem_set_null(dest);
  }
  assert( t==p_c->types[p2] );
  if( p_c->sz_row>=offsets[p2+1] ){
    /* This is the common case where the desired content fits on the original
    ** page - where the content is not on an overflow page */
    z_data = p_c->rows + offsets[p2];
    if( t<12 ){
      __vdbe_serial_get(z_data, t, dest);
    }else{
      /* If the column value is a string, we need a persistent value, not
      ** a MEM_Ephem value.  This branch is a fast short-cut that is equivalent
      ** to calling __vdbe_serial_get() and __vdbe_deephemeralize().
      */
      static const u_int16_t flags[] = { MEM_Blob, MEM_Str|MEM_Term };
      dest->n = len = (t-12)/2;
      dest->enc = encoding;
      if( dest->sz_malloc < len+2 ){
        if( len>db->limits[DBSQL_LIMIT_LENGTH] ) goto too_big;
        dest->flags = MEM_Null;
        if( __vdbe_mem_grow(dest, len+2, 0) ) goto no_mem;
      }else{
        dest->z = dest->z_malloc;
      }
      memcpy(dest->z, z_data, len);
      dest->z[len] = 0;
      dest->z[len+1] = 0;
      dest->flags = flags[t&1];
    }
  }else{
    u_int8_t p5;
    dest->enc = encoding;
    assert( dest->db==db );
    /* This branch happens only when content is on overflow pages */
    if( ((p5 = (p_op->p5 & OPFLAG_BYTELENARG))!=0
          && (p5==OPFLAG_TYPEOFARG
              || (t>=12 && ((t&1)==0 || p5==OPFLAG_BYTELENARG))
             )
        )
     || __vdbe_serial_type_len(t)==0
    ){
      /* Content is irrelevant for
      **    1. the typeof() function,
      **    2. the length(X) function if X is a blob, and
      **    3. if the content length is zero.
      ** So we might as well use bogus content rather than reading
      ** content from disk.
      **
      ** Although __vdbe_serial_get() may read at most 8 bytes from the
      ** buffer passed to it, debugging function __vdbe_mem_pretty_print() may
      ** read more.  Use the global constant __ctype_map[] as the array,
      ** as that array is 256 bytes long (plenty for __vdbe_mem_pretty_print())
      ** and it begins with a bunch of zeros.
      */
      __vdbe_serial_get((u_int8_t*)__ctype_map, t, dest);
    }else{
      rc = __vdbe_column_from_overflow(p_c, p2, t, offsets[p2],
                p->cache_ctr, col_cache_ctr, dest);
      if( rc ){
        if( rc==DBSQL_NOMEM ) goto no_mem;
        if( rc==DBSQL_TOOBIG ) goto too_big;
        goto abort_due_to_error;
      }
    }
  }

op_column_out:
  UPDATE_MAX_BLOBSIZE(dest);
  REGISTER_TRACE(p_op->p3, dest);
  break;

op_column_corrupt:
  if( ops[0].p3>0 ){
    p_op = &ops[ops[0].p3-1];
    break;
  }else{
    rc = DBSQL_CORRUPT_BKPT;
    goto abort_due_to_error;
  }
}

/* Opcode: __type_check P1 P2 P3 P4 *
** Synopsis: typecheck(r[P1@P2])
**
** Apply affinities to the range of P2 registers beginning with P1.
** Take the affinities from the table_t object in P4.  If any value
** cannot be coerced into the correct type, then raise an error.
**
** If P3==0, then omit checking of VIRTUAL columns.
**
** If P3==1, then omit checking of all generated column, both VIRTUAL
** and STORED.
**
** If P3>=2, then only check column number P3-2 in the table (which will
** be a VIRTUAL column) against the value in reg[P1].  In this case,
** P2 will be 1.
**
** This opcode is similar to OP_Affinity except that this opcode
** forces the register type to the table_t column type.  This is used
** to implement "strict affinity".
**
** GENERATED ALWAYS AS ... STATIC columns are only checked if P3
** is zero.  When P3 is non-zero, no type checking occurs for
** static generated columns.  Virtual columns are computed at query time
** and so they are never checked.
**
** Preconditions:
**
** <ul>
** <li> P2 should be the number of non-virtual columns in the
**      table of P4 unless P3>1, in which case P2 will be 1.
** <li> table_t P4 is a STRICT table.
** </ul>
**
** If any precondition is false, an assertion fault occurs.
*/
case OP_TypeCheck: {
  table_t *tab;
  column_t *cols;
  int i;
  int n_col;

  assert( p_op->p4type==P4_TABLE );
  tab = p_op->p4.tab;
  assert( tab->tab_flags & TF_Strict );
  assert( p_op->p3>=0 && p_op->p3<tab->n_col+2 );
  cols = tab->cols;
  in1 = &mems[p_op->p1];
  if( p_op->p3<2 ){
    assert( tab->n_nv_col==p_op->p2 );
    i = 0;
    n_col = tab->n_col;
  }else{
    i = p_op->p3-2;
    n_col = i+1;
    assert( i<tab->n_col );
    assert( cols[i].col_flags & COLFLAG_VIRTUAL );
    assert( p_op->p2==1 );
  }
  for(; i<n_col; i++){
    if( (cols[i].col_flags & COLFLAG_GENERATED)!=0 && p_op->p3<2 ){
      if( (cols[i].col_flags & COLFLAG_VIRTUAL)!=0 ) continue;
      if( p_op->p3 ){ in1++; continue; }
    }
    assert( in1 < &mems[p_op->p1+p_op->p2] );
    __apply_affinity(in1, cols[i].affinity, encoding);
    if( (in1->flags & MEM_Null)==0 ){
      switch( cols[i].c_type ){
        case COLTYPE_BLOB: {
          if( (in1->flags & MEM_Blob)==0 ) goto vdbe_type_error;
          break;
        }
        case COLTYPE_INTEGER:
        case COLTYPE_INT: {
          if( (in1->flags & MEM_Int)==0 ) goto vdbe_type_error;
          break;
        }
        case COLTYPE_TEXT: {
          if( (in1->flags & MEM_Str)==0 ) goto vdbe_type_error;
          break;
        }
        case COLTYPE_REAL: {
          testcase( (in1->flags & (MEM_Real|MEM_IntReal))==MEM_Real );
          assert( (in1->flags & MEM_IntReal)==0 );
          if( in1->flags & MEM_Int ){
            /* When applying REAL affinity, if the result is still an MEM_Int
            ** that will fit in 6 bytes, then change the type to MEM_IntReal
            ** so that we keep the high-resolution integer value but know that
            ** the type really wants to be REAL. */
            testcase( in1->u.i==140737488355328LL );
            testcase( in1->u.i==140737488355327LL );
            testcase( in1->u.i==-140737488355328LL );
            testcase( in1->u.i==-140737488355329LL );
            if( in1->u.i<=140737488355327LL && in1->u.i>=-140737488355328LL){
              in1->flags |= MEM_IntReal;
              in1->flags &= ~MEM_Int;
            }else{
              in1->u.r = (double)in1->u.i;
              in1->flags |= MEM_Real;
              in1->flags &= ~MEM_Int;
            }
          }else if( (in1->flags & (MEM_Real|MEM_IntReal))==0 ){
            goto vdbe_type_error;
          }
          break;
        }
        default: {
          /* COLTYPE_ANY.  Accept anything. */
          break;
        }
      }
    }
    REGISTER_TRACE((int)(in1-mems), in1);
    in1++;
  }
  assert( in1 == &mems[p_op->p1+p_op->p2] );
  break;

vdbe_type_error:
  __vdbe_error(p, "cannot store %s value in %s column %s.%s",
     __vdbe_mem_type_name(in1), __std_type[cols[i].c_type-1],
     tab->name, cols[i].cn_name);
  rc = DBSQL_CONSTRAINT_DATATYPE;
  goto abort_due_to_error;
}

/* Opcode: Affinity P1 P2 * P4 *
** Synopsis: affinity(r[P1@P2])
**
** Apply affinities to a range of P2 registers starting with P1.
**
** P4 is a string that is P2 characters long. The N-th character of the
** string indicates the column affinity that should be used for the N-th
** memory cell in the range.
*/
case OP_Affinity: {
  const char *z_affinity;   /* The affinity to be applied */

  z_affinity = p_op->p4.z;
  assert( z_affinity!=0 );
  assert( p_op->p2>0 );
  assert( z_affinity[p_op->p2]==0 );
  in1 = &mems[p_op->p1];
  while( 1 /*exit-by-break*/ ){
    assert( in1 <= &p->mems[(p->n_mem+1 - p->n_cursor)] );
    assert( z_affinity[0]==DBSQL_AFF_NONE || MEM_IS_VALID(in1) );
    __apply_affinity(in1, z_affinity[0], encoding);
    if( z_affinity[0]==DBSQL_AFF_REAL && (in1->flags & MEM_Int)!=0 ){
      /* When applying REAL affinity, if the result is still an MEM_Int
      ** that will fit in 6 bytes, then change the type to MEM_IntReal
      ** so that we keep the high-resolution integer value but know that
      ** the type really wants to be REAL. */
      testcase( in1->u.i==140737488355328LL );
      testcase( in1->u.i==140737488355327LL );
      testcase( in1->u.i==-140737488355328LL );
      testcase( in1->u.i==-140737488355329LL );
      if( in1->u.i<=140737488355327LL && in1->u.i>=-140737488355328LL ){
        in1->flags |= MEM_IntReal;
        in1->flags &= ~MEM_Int;
      }else{
        in1->u.r = (double)in1->u.i;
        in1->flags |= MEM_Real;
        in1->flags &= ~(MEM_Int|MEM_Str);
      }
    }
    REGISTER_TRACE((int)(in1-mems), in1);
    z_affinity++;
    if( z_affinity[0]==0 ) break;
    in1++;
  }
  break;
}

/* Opcode: __make_record P1 P2 P3 P4 *
** Synopsis: r[P3]=mkrec(r[P1@P2])
**
** Convert P2 registers beginning with P1 into the [record format]
** use as a data record in a database table or as a key
** in an index.  The OP_Column opcode can decode the record later.
**
** P4 may be a string that is P2 characters long.  The N-th character of the
** string indicates the column affinity that should be used for the N-th
** field of the index key.
**
** The mapping from character to affinity is given by the DBSQL_AFF_
** macros defined in dbsql_int.h.
**
** If P4 is NULL then all index fields have the affinity BLOB.
**
** The meaning of P5 depends on whether or not the DBSQL_ENABLE_NULL_TRIM
** compile-time option is enabled:
**
**   * If DBSQL_ENABLE_NULL_TRIM is enabled, then the P5 is the index
**     of the right-most table that can be null-trimmed.
**
**   * If DBSQL_ENABLE_NULL_TRIM is omitted, then P5 has the value
**     OPFLAG_NOCHNG_MAGIC if the OP_MakeRecord opcode is allowed to
**     accept no-change records with serial_type 10.  This value is
**     only used inside an assert() and does not affect the end result.
*/
case OP_MakeRecord: {
  mem_t *rec;             /* The new record */
  u_int64_t n_data;             /* Number of bytes of data space */
  int n_hdr;              /* Number of bytes of header space */
  int64_t n_byte;             /* Data space required for this record */
  int64_t n_zero;             /* Number of zero bytes at the end of the record */
  int n_varint;           /* Number of bytes in a varint */
  u_int32_t serial_type;       /* Type field */
  mem_t *data0;           /* First field to be combined into the record */
  mem_t *last;            /* Last field of the record */
  int n_field;            /* Number of fields in the record */
  char *z_affinity;       /* The affinity string for the record */
  u_int32_t len;               /* Length of a field */
  u_int8_t *hdr;              /* Where to write next byte of the header */
  u_int8_t *payload;          /* Where to write next byte of the payload */

  /* Assuming the record contains N fields, the record format looks
  ** like this:
  **
  ** ------------------------------------------------------------------------
  ** | hdr-size | type 0 | type 1 | ... | type N-1 | data0 | ... | data N-1 |
  ** ------------------------------------------------------------------------
  **
  ** Data(0) is taken from register P1.  Data(1) comes from register P1+1
  ** and so forth.
  **
  ** Each type field is a varint representing the serial type of the
  ** corresponding data element (see __vdbe_serial_type()). The
  ** hdr-size field is also a varint which is the offset from the beginning
  ** of the record to data0.
  */
  n_data = 0;         /* Number of bytes of data space */
  n_hdr = 0;          /* Number of bytes of header space */
  n_zero = 0;         /* Number of zero bytes at the end of the record */
  n_field = p_op->p1;
  z_affinity = p_op->p4.z;
  assert( n_field>0 && p_op->p2>0 && p_op->p2+n_field<=(p->n_mem+1 - p->n_cursor)+1 );
  data0 = &mems[n_field];
  n_field = p_op->p2;
  last = &data0[n_field-1];

  /* Identify the output register */
  assert( p_op->p3<p_op->p1 || p_op->p3>=p_op->p1+p_op->p2 );
  out = &mems[p_op->p3];
  MEM_ABOUT_TO_CHANGE(p, out);

  /* Apply the requested affinity to all inputs
  */
  assert( data0<=last );
  if( z_affinity ){
    rec = data0;
    do{
      __apply_affinity(rec, z_affinity[0], encoding);
      if( z_affinity[0]==DBSQL_AFF_REAL && (rec->flags & MEM_Int) ){
        rec->flags |= MEM_IntReal;
        rec->flags &= ~(MEM_Int);
      }
      REGISTER_TRACE((int)(rec-mems), rec);
      z_affinity++;
      rec++;
      assert( z_affinity[0]==0 || rec<=last );
    }while( z_affinity[0] );
  }

#ifdef DBSQL_ENABLE_NULL_TRIM
  /* NULLs can be safely trimmed from the end of the record, as long as
  ** as the schema format is 2 or more and none of the omitted columns
  ** have a non-NULL default value.  Also, the record must be left with
  ** at least one field.  If P5>0 then it will be one more than the
  ** index of the right-most column with a non-NULL default value */
  if( p_op->p5 ){
    while( (last->flags & MEM_Null)!=0 && n_field>p_op->p5 ){
      last--;
      n_field--;
    }
  }
#endif

  /* Loop through the elements that will make up the record to figure
  ** out how much space is required for the new record.  After this loop,
  ** the mem_t.u_temp field of each term should hold the serial-type that will
  ** be used for that term in the generated record:
  **
  **   mem_t.u_temp value    type
  **   ---------------    ---------------
  **      0               NULL
  **      1               1-byte signed integer
  **      2               2-byte signed integer
  **      3               3-byte signed integer
  **      4               4-byte signed integer
  **      5               6-byte signed integer
  **      6               8-byte signed integer
  **      7               IEEE float
  **      8               Integer constant 0
  **      9               Integer constant 1
  **     10,11            reserved for expansion
  **    N>=12 and even    BLOB
  **    N>=13 and odd     text
  **
  ** The following additional values are computed:
  **     n_hdr        Number of bytes needed for the record header
  **     n_data       Number of bytes of data space needed for the record
  **     n_zero       Zero bytes at the end of the record
  */
  rec = last;
  do{
    assert( MEM_IS_VALID(rec) );
    if( rec->flags & MEM_Null ){
      if( rec->flags & MEM_Zero ){
        /* Values with MEM_Null and MEM_Zero are created by column virtual
        ** table methods that never invoke dbsql_result_xxxxx() while
        ** computing an unchanging column value in an UPDATE statement.
        ** Give such values a special internal-use-only serial-type of 10
        ** so that they can be passed through to update and have
        ** a true dbsql_value_nochange(). */
#ifndef DBSQL_ENABLE_NULL_TRIM
        assert( p_op->p5==OPFLAG_NOCHNG_MAGIC || CORRUPT_DB );
#endif
        rec->u_temp = 10;
      }else{
        rec->u_temp = 0;
      }
      n_hdr++;
    }else if( rec->flags & (MEM_Int|MEM_IntReal) ){
      /* Figure out whether to use 1, 2, 4, 6 or 8 bytes. */
      int64_t i = rec->u.i;
      u_int64_t uu;
      testcase( rec->flags & MEM_Int );
      testcase( rec->flags & MEM_IntReal );
      if( i<0 ){
        uu = ~i;
      }else{
        uu = i;
      }
      n_hdr++;
      testcase( uu==127 );               testcase( uu==128 );
      testcase( uu==32767 );             testcase( uu==32768 );
      testcase( uu==8388607 );           testcase( uu==8388608 );
      testcase( uu==2147483647 );        testcase( uu==2147483648LL );
      testcase( uu==140737488355327LL ); testcase( uu==140737488355328LL );
      if( uu<=127 ){
        if( (i&1)==i && p->min_write_file_format>=4 ){
          rec->u_temp = 8+(u_int32_t)uu;
        }else{
          n_data++;
          rec->u_temp = 1;
        }
      }else if( uu<=32767 ){
        n_data += 2;
        rec->u_temp = 2;
      }else if( uu<=8388607 ){
        n_data += 3;
        rec->u_temp = 3;
      }else if( uu<=2147483647 ){
        n_data += 4;
        rec->u_temp = 4;
      }else if( uu<=140737488355327LL ){
        n_data += 6;
        rec->u_temp = 5;
      }else{
        n_data += 8;
        if( rec->flags & MEM_IntReal ){
          /* If the value is __int_real and is going to take up 8 bytes to store
          ** as an integer, then we might as well make it an 8-byte floating
          ** point value */
          rec->u.r = (double)rec->u.i;
          rec->flags &= ~MEM_IntReal;
          rec->flags |= MEM_Real;
          rec->u_temp = 7;
        }else{
          rec->u_temp = 6;
        }
      }
    }else if( rec->flags & MEM_Real ){
      n_hdr++;
      n_data += 8;
      rec->u_temp = 7;
    }else{
      assert( db->malloc_failed || rec->flags&(MEM_Str|MEM_Blob) );
      assert( rec->n>=0 );
      len = (u_int32_t)rec->n;
      serial_type = (len*2) + 12 + ((rec->flags & MEM_Str)!=0);
      if( rec->flags & MEM_Zero ){
        serial_type += (u_int32_t)rec->u.n_zero*2;
        if( n_data ){
          if( __vdbe_mem_expand_blob(rec) ) goto no_mem;
          len += rec->u.n_zero;
        }else{
          n_zero += rec->u.n_zero;
        }
      }
      n_data += len;
      n_hdr += __varint_len(serial_type);
      rec->u_temp = serial_type;
    }
    if( rec==data0 ) break;
    rec--;
  }while(1);

  /* EVIDENCE-OF: R-22564-11647 The header begins with a single varint
  ** which determines the total number of bytes in the header. The varint
  ** value is the size of the header in bytes including the size varint
  ** itself. */
  testcase( n_hdr==126 );
  testcase( n_hdr==127 );
  if( n_hdr<=126 ){
    /* The common case */
    n_hdr += 1;
  }else{
    /* Rare case of a really large header */
    n_varint = __varint_len(n_hdr);
    n_hdr += n_varint;
    if( n_varint<__varint_len(n_hdr) ) n_hdr++;
  }
  n_byte = n_hdr+n_data;

  /* Make sure the output register has a buffer large enough to store
  ** the new record. The output register (op->p3) is not allowed to
  ** be one of the input registers (because the following call to
  ** __vdbe_mem_clear_and_resize() could clobber the value before it is used).
  */
  if( n_byte+n_zero<=out->sz_malloc ){
    /* The output register is already large enough to hold the record.
    ** No error checks or buffer enlargement is required */
    out->z = out->z_malloc;
  }else{
    /* Need to make sure that the output is not too big and then enlarge
    ** the output register to hold the full result */
    if( n_byte+n_zero>db->limits[DBSQL_LIMIT_LENGTH] ){
      goto too_big;
    }
    if( __vdbe_mem_clear_and_resize(out, (int)n_byte) ){
      goto no_mem;
    }
  }
  out->n = (int)n_byte;
  out->flags = MEM_Blob;
  if( n_zero ){
    out->u.n_zero = n_zero;
    out->flags |= MEM_Zero;
  }
  UPDATE_MAX_BLOBSIZE(out);
  hdr = (u_int8_t *)out->z;
  payload = hdr + n_hdr;

  /* Write the record */
  if( n_hdr<0x80 ){
    *(hdr++) = n_hdr;
  }else{
    hdr += __put_varint(hdr,n_hdr);
  }
  assert( data0<=last );
  rec = data0;
  while( 1 /*exit-by-break*/ ){
    serial_type = rec->u_temp;
    /* EVIDENCE-OF: R-06529-47362 Following the size varint are one or more
    ** additional varints, one per column.
    ** EVIDENCE-OF: R-64536-51728 The values for each column in the record
    ** immediately follow the header. */
    if( serial_type<=7 ){
      *(hdr++) = serial_type;
      if( serial_type==0 ){
        /* NULL value.  No change in payload */
      }else{
        u_int64_t v;
        if( serial_type==7 ){
          assert( sizeof(v)==sizeof(rec->u.r) );
          memcpy(&v, &rec->u.r, sizeof(v));
          SWAP_MIXED_ENDIAN_FLOAT(v);
        }else{
          v = rec->u.i;
        }
        len = __small_type_sizes[serial_type];
        assert( len>=1 && len<=8 && len!=5 && len!=7 );
        switch( len ){
          default: payload[7] = (u_int8_t)(v&0xff); v >>= 8;
                   payload[6] = (u_int8_t)(v&0xff); v >>= 8;
                   /* no break */ deliberate_fall_through
          case 6:  payload[5] = (u_int8_t)(v&0xff); v >>= 8;
                   payload[4] = (u_int8_t)(v&0xff); v >>= 8;
                   /* no break */ deliberate_fall_through
          case 4:  payload[3] = (u_int8_t)(v&0xff); v >>= 8;
                   /* no break */ deliberate_fall_through
          case 3:  payload[2] = (u_int8_t)(v&0xff); v >>= 8;
                   /* no break */ deliberate_fall_through
          case 2:  payload[1] = (u_int8_t)(v&0xff); v >>= 8;
                   /* no break */ deliberate_fall_through
          case 1:  payload[0] = (u_int8_t)(v&0xff);
        }
        payload += len;
      }
    }else if( serial_type<0x80 ){
      *(hdr++) = serial_type;
      if( serial_type>=14 && rec->n>0 ){
        assert( rec->z!=0 );
        memcpy(payload, rec->z, rec->n);
        payload += rec->n;
      }
    }else{
      hdr += __put_varint(hdr, serial_type);
      if( rec->n ){
        assert( rec->z!=0 );
        assert( rec->z!=(const char*)__ctype_map );
        memcpy(payload, rec->z, rec->n);
        payload += rec->n;
      }
    }
    if( rec==last ) break;
    rec++;
  }
  assert( n_hdr==(int)(hdr - (u_int8_t*)out->z) );
  assert( n_byte==(int)(payload - (u_int8_t*)out->z) );

  assert( p_op->p3>0 && p_op->p3<=(p->n_mem+1 - p->n_cursor) );
  REGISTER_TRACE(p_op->p3, out);
  break;
}

/* Opcode: Count P1 P2 P3 * *
** Synopsis: r[P2]=count()
**
** Store the number of entries (an integer value) in the table or index
** opened by cursor P1 in register P2.
**
** If P3==0, then an exact count is obtained, which involves visiting
** every btree page of the table.  But if P3 is non-zero, an estimate
** is returned based on the current cursor position. 
*/
case OP_Count: {         /* out2 */
  int64_t n_entry;
  sm_cursor_t *crsr;

  assert( p->csrs[p_op->p1]->cur_type==CURTYPE_BTREE );
  crsr = p->csrs[p_op->p1]->uc.cursor;
  assert( crsr );
  if( p_op->p3 ){
    n_entry = __sm_row_count_est(crsr);
  }else{
    n_entry = 0;  /* Not needed.  Only used to silence a warning. */
    rc = __sm_count(db, crsr, &n_entry);
    if( rc ) goto abort_due_to_error;
  }
  out = out2Prerelease(p, p_op);
  out->u.i = n_entry;
  goto check_for_interrupt;
}

/* Opcode: savepoint_t P1 * * P4 *
**
** Open, release or rollback the savepoint named by parameter P4, depending
** on the value of P1. To open a new savepoint set P1==0 (SAVEPOINT_BEGIN).
** To release (commit) an existing savepoint set P1==1 (SAVEPOINT_RELEASE).
** To rollback an existing savepoint set P1==2 (SAVEPOINT_ROLLBACK).
*/
case OP_Savepoint: {
  int p1;                         /* Value of P1 operand */
  char *z_name;                    /* Name of savepoint */
  int n_name;
  savepoint_t *p_new;
  savepoint_t *savepoint;
  savepoint_t *tmp;
  int i_savepoint;
  int ii;

  p1 = p_op->p1;
  z_name = p_op->p4.z;

  /* Assert that the p1 parameter is valid. Also that if there is no open
  ** transaction, then there cannot be any savepoints.
  */
  assert( db->savepoint==0 || db->auto_commit==0 );
  assert( p1==SAVEPOINT_BEGIN||p1==SAVEPOINT_RELEASE||p1==SAVEPOINT_ROLLBACK );
  assert( db->savepoint || db->is_transaction_savepoint==0 );
  assert( __check_savepoint_count(db) );
  assert( p->is_reader );

  if( p1==SAVEPOINT_BEGIN ){
    if( db->n_vdbe_write>0 ){
      /* A new savepoint cannot be created if there are active write
      ** statements (i.e. open read/write incremental blob handles).
      */
      __vdbe_error(p, "cannot open savepoint - SQL statements in progress");
      rc = DBSQL_BUSY;
    }else{
      n_name = __strlen30(z_name);

#ifndef DBSQL_OMIT_VIRTUALTABLE
      /* This call is Ok even if this savepoint is actually a transaction
      ** savepoint (and therefore should not prompt savepoint()) callbacks.
      ** If this is a transaction savepoint being opened, it is guaranteed
      ** that the db->v_trans[] array is empty.  */
      assert( db->auto_commit==0 || db->n_v_trans==0 );
      rc = __vtab_savepoint(db, SAVEPOINT_BEGIN,
                                db->n_statement+db->n_savepoint);
      if( rc!=DBSQL_OK ) goto abort_due_to_error;
#endif

      /* Create a new savepoint structure. */
      p_new = __db_malloc_raw_nn(db, sizeof(savepoint_t)+n_name+1);
      if( p_new ){
        p_new->name = (char *)&p_new[1];
        memcpy(p_new->name, z_name, n_name+1);
   
        /* If there is no open transaction, then mark this as a special
        ** "transaction savepoint". */
        if( db->auto_commit ){
          db->auto_commit = 0;
          db->is_transaction_savepoint = 1;
        }else{
          db->n_savepoint++;
        }

        /* Link the new savepoint into the database handle's list. */
        p_new->next = db->savepoint;
        db->savepoint = p_new;
        p_new->n_deferred_cons = db->n_deferred_cons;
        p_new->n_deferred_imm_cons = db->n_deferred_imm_cons;
      }
    }
  }else{
    assert( p1==SAVEPOINT_RELEASE || p1==SAVEPOINT_ROLLBACK );
    i_savepoint = 0;

    /* Find the named savepoint. If there is no such savepoint, then an
    ** an error is returned to the user.  */
    for(
      savepoint = db->savepoint;
      savepoint && __str_icmp(savepoint->name, z_name);
      savepoint = savepoint->next
    ){
      i_savepoint++;
    }
    if( !savepoint ){
      __vdbe_error(p, "no such savepoint: %s", z_name);
      rc = DBSQL_ERROR;
    }else if( db->n_vdbe_write>0 && p1==SAVEPOINT_RELEASE ){
      /* It is not possible to release (commit) a savepoint if there are
      ** active write statements.
      */
      __vdbe_error(p, "cannot release savepoint - "
                          "SQL statements in progress");
      rc = DBSQL_BUSY;
    }else{

      /* Determine whether or not this is a transaction savepoint. If so,
      ** and this is a RELEASE command, then the current transaction
      ** is committed.
      */
      int is_transaction = savepoint->next==0 && db->is_transaction_savepoint;
      if( is_transaction && p1==SAVEPOINT_RELEASE ){
        if( (rc = __vdbe_check_fk_deferred(p))!=DBSQL_OK ){
          goto vdbe_return;
        }
        db->auto_commit = 1;
        if( __vdbe_halt(p)==DBSQL_BUSY ){
          p->pc = (int)(p_op - ops);
          db->auto_commit = 0;
          p->rc = rc = DBSQL_BUSY;
          goto vdbe_return;
        }
        rc = p->rc;
        if( rc ){
          db->auto_commit = 0;
        }else{
          db->is_transaction_savepoint = 0;
        }
      }else{
        int is_schema_change;
        i_savepoint = db->n_savepoint - i_savepoint - 1;
        if( p1==SAVEPOINT_ROLLBACK ){
          is_schema_change = (db->m_db_flags & DBFLAG_SchemaChange)!=0;
          for(ii=0; ii<db->n_db; ii++){
            rc = __sm_trip_all_cursors(db->dbs[ii].bt,
                                       DBSQL_ABORT_ROLLBACK,
                                       is_schema_change==0);
            if( rc!=DBSQL_OK ) goto abort_due_to_error;
          }
        }else{
          assert( p1==SAVEPOINT_RELEASE );
          is_schema_change = 0;
        }
        for(ii=0; ii<db->n_db; ii++){
          rc = __sm_savepoint(db->dbs[ii].bt, p1, i_savepoint);
          if( rc!=DBSQL_OK ){
            goto abort_due_to_error;
          }
        }
        if( is_schema_change ){
          __expire_prepared_statements(db, 0);
          __reset_all_schemas_of_connection(db);
          db->m_db_flags |= DBFLAG_SchemaChange;
        }
      }
      if( rc ) goto abort_due_to_error;
 
      /* Regardless of whether this is a RELEASE or ROLLBACK, destroy all
      ** savepoints nested inside of the savepoint being operated on. */
      while( db->savepoint!=savepoint ){
        tmp = db->savepoint;
        db->savepoint = tmp->next;
        __dbsql_db_free(db, tmp);
        db->n_savepoint--;
      }

      /* If it is a RELEASE, then destroy the savepoint being operated on
      ** too. If it is a ROLLBACK TO, then set the number of deferred
      ** constraint violations present in the database to the value stored
      ** when the savepoint was created.  */
      if( p1==SAVEPOINT_RELEASE ){
        assert( savepoint==db->savepoint );
        db->savepoint = savepoint->next;
        __dbsql_db_free(db, savepoint);
        if( !is_transaction ){
          db->n_savepoint--;
        }
      }else{
        assert( p1==SAVEPOINT_ROLLBACK );
        db->n_deferred_cons = savepoint->n_deferred_cons;
        db->n_deferred_imm_cons = savepoint->n_deferred_imm_cons;
      }

      if( !is_transaction || p1==SAVEPOINT_ROLLBACK ){
        rc = __vtab_savepoint(db, p1, i_savepoint);
        if( rc!=DBSQL_OK ) goto abort_due_to_error;
      }
    }
  }
  if( rc ) goto abort_due_to_error;
  if( p->vdbe_state==VDBE_HALT_STATE ){
    rc = DBSQL_DONE;
    goto vdbe_return;
  }
  break;
}

/* Opcode: __auto_commit P1 P2 * * *
**
** Set the database auto-commit flag to P1 (1 or 0). If P2 is true, roll
** back any currently active btree transactions. If there are any active
** VMs (apart from this one), then a ROLLBACK fails.  A COMMIT fails if
** there are active writing VMs or active VMs that use shared cache.
**
** This instruction causes the VM to halt.
*/
case OP_AutoCommit: {
  int desired_auto_commit;
  int i_rollback;

  desired_auto_commit = p_op->p1;
  i_rollback = p_op->p2;
  assert( desired_auto_commit==1 || desired_auto_commit==0 );
  assert( desired_auto_commit==1 || i_rollback==0 );
  assert( db->n_vdbe_active>0 );  /* At least this one VM is active */
  assert( p->is_reader );

  if( desired_auto_commit!=db->auto_commit ){
    if( i_rollback ){
      assert( desired_auto_commit==1 );
      __rollback_all(db, DBSQL_ABORT_ROLLBACK);
      db->auto_commit = 1;
    }else if( desired_auto_commit && db->n_vdbe_write>0 ){
      /* If this instruction implements a COMMIT and other VMs are writing
      ** return an error indicating that the other VMs must complete first.
      */
      __vdbe_error(p, "cannot commit transaction - "
                          "SQL statements in progress");
      rc = DBSQL_BUSY;
      goto abort_due_to_error;
    }else if( (rc = __vdbe_check_fk_deferred(p))!=DBSQL_OK ){
      goto vdbe_return;
    }else{
      db->auto_commit = (u_int8_t)desired_auto_commit;
    }
    if( __vdbe_halt(p)==DBSQL_BUSY ){
      p->pc = (int)(p_op - ops);
      db->auto_commit = (u_int8_t)(1-desired_auto_commit);
      p->rc = rc = DBSQL_BUSY;
      goto vdbe_return;
    }
    __close_savepoints(db);
    if( p->rc==DBSQL_OK ){
      rc = DBSQL_DONE;
    }else{
      rc = DBSQL_ERROR;
    }
    goto vdbe_return;
  }else{
    __vdbe_error(p,
        (!desired_auto_commit)?"cannot start a transaction within a transaction":(
        (i_rollback)?"cannot rollback - no transaction is active":
                   "cannot commit - no transaction is active"));
        
    rc = DBSQL_ERROR;
    goto abort_due_to_error;
  }
  /*NOTREACHED*/ assert(0);
}

/* Opcode: Transaction P1 P2 P3 P4 P5
**
** Begin a transaction on database P1 if a transaction is not already
** active.
** If P2 is non-zero, then a write-transaction is started, or if a
** read-transaction is already active, it is upgraded to a write-transaction.
** If P2 is zero, then a read-transaction is started.  If P2 is 2 or more
** then an exclusive transaction is started.
**
** P1 is the index of the database file on which the transaction is
** started.  index_t 0 is the main database file and index 1 is the
** file used for temporary tables.  Indices of 2 or more are used for
** attached databases.
**
** If a write-transaction is started and the vdbe_t.uses_stmt_journal flag is
** true (this flag is set if the vdbe_t may modify more than one row and may
** throw an ABORT exception), a statement transaction may also be opened.
** More specifically, a statement transaction is opened iff the database
** connection is currently not in autocommit mode, or if there are other
** active statements. A statement transaction allows the changes made by this
** VDBE to be rolled back after an error without having to roll back the
** entire transaction. If no error is encountered, the statement transaction
** will automatically commit when the VDBE halts.
**
** If P5!=0 then this opcode also checks the schema cookie against P3
** and the schema generation counter against P4.
** The cookie changes its value whenever the database schema changes.
** This operation is used to detect when that the cookie has changed
** and that the current process needs to reread the schema.  If the schema
** cookie in P3 differs from the schema cookie in the database header or
** if the schema generation counter in P4 differs from the current
** generation counter, then an DBSQL_SCHEMA error is raised and execution
** halts.  The dbsql_step() wrapper function might then reprepare the
** statement and rerun it from the beginning.
*/
case OP_Transaction: {
  sm_t *bt;
  db_t *p_db;
  int i_meta = 0;

  assert( p->is_reader );
  assert( p->readOnly==0 || p_op->p2==0 );
  assert( p_op->p2>=0 && p_op->p2<=2 );
  assert( p_op->p1>=0 && p_op->p1<db->n_db );
  assert( DB_MASK_TEST(p->btree_mask, p_op->p1) );
  assert( rc==DBSQL_OK );
  if( p_op->p2 && (db->flags & (DBSQL_QueryOnly|DBSQL_CorruptRdOnly))!=0 ){
    if( db->flags & DBSQL_QueryOnly ){
      /* Writes prohibited by the "PRAGMA query_only=TRUE" statement */
      rc = DBSQL_READONLY;
    }else{
      /* Writes prohibited due to a prior DBSQL_CORRUPT in the current
      ** transaction */
      rc = DBSQL_CORRUPT;
    }
    goto abort_due_to_error;
  }
  p_db = &db->dbs[p_op->p1];
  bt = p_db->bt;

  if( bt ){
    rc = __sm_begin_trans(bt, p_op->p2, &i_meta);
    testcase( rc==DBSQL_BUSY_SNAPSHOT );
    testcase( rc==DBSQL_BUSY_RECOVERY );
    if( rc!=DBSQL_OK ){
      if( (rc&0xff)==DBSQL_BUSY ){
        p->pc = (int)(p_op - ops);
        p->rc = rc;
        goto vdbe_return;
      }
      goto abort_due_to_error;
    }

    if( p->uses_stmt_journal
     && p_op->p2
     && (db->auto_commit==0 || db->n_vdbe_read>1)
    ){
      assert( __sm_txn_state(bt)==DBSQL_TXN_WRITE );
      if( p->i_statement==0 ){
        assert( db->n_statement>=0 && db->n_savepoint>=0 );
        db->n_statement++;
        p->i_statement = db->n_savepoint + db->n_statement;
      }

      rc = __vtab_savepoint(db, SAVEPOINT_BEGIN, p->i_statement-1);
      if( rc==DBSQL_OK ){
        rc = __sm_begin_stmt(bt, p->i_statement);
      }

      /* Store the current value of the database handles deferred constraint
      ** counter. If the statement transaction needs to be rolled back,
      ** the value of this counter needs to be restored too.  */
      p->n_stmt_def_cons = db->n_deferred_cons;
      p->n_stmt_def_imm_cons = db->n_deferred_imm_cons;
    }
  }
  assert( p_op->p5==0 || p_op->p4type==P4_INT32 );
  if( rc==DBSQL_OK
   && p_op->p5
   && (i_meta!=p_op->p3 || p_db->schema->i_generation!=p_op->p4.i)
  ){
    /*
    ** IMPLEMENTATION-OF: R-03189-51135 As each SQL statement runs, the schema
    ** version is checked to ensure that the schema has not changed since the
    ** SQL statement was prepared.
    */
    __dbsql_db_free(db, p->errmsg);
    p->errmsg = __db_str_dup(db, "database schema has changed");
    /* If the schema-cookie from the database file matches the cookie
    ** stored with the in-memory representation of the schema, do
    ** not reload the schema from the database file.
    **
    ** If virtual-tables are in use, this is not just an optimization.
    ** Often, v-tables store their data in other dbsql tables, which
    ** are queried from within next() and other v-table methods using
    ** prepared queries. If such a query is out-of-date, we do not want to
    ** discard the database schema, as the user code implementing the
    ** v-table would have to be ready for the dbsql_vtab_t structure itself
    ** to be invalidated whenever dbsql_step() is called from within
    ** a v-table method.
    */
    if( db->dbs[p_op->p1].schema->schema_cookie!=i_meta ){
      __reset_one_schema(db, p_op->p1);
    }
    p->expired = 1;
    rc = DBSQL_SCHEMA;

    /* Set change_cnt_on to 0 to prevent the value returned by dbsql_changes()
    ** from being modified in __vdbe_halt(). If this statement is
    ** reprepared, change_cnt_on will be set again. */
    p->change_cnt_on = 0;
  }
  if( rc ) goto abort_due_to_error;
  break;
}

/* Opcode: __read_cookie P1 P2 P3 * *
**
** Read cookie number P3 from database P1 and write it into register P2.
** P3==1 is the schema version.  P3==2 is the database format.
** P3==3 is the recommended pager cache size, and so forth.  P1==0 is
** the main database file and P1==1 is the database file used to store
** temporary tables.
**
** There must be a read-lock on the database (either a transaction
** must be started or there must be an open cursor) before
** executing this instruction.
*/
case OP_ReadCookie: {               /* out2 */
  int i_meta;
  int i_db;
  int i_cookie;

  assert( p->is_reader );
  i_db = p_op->p1;
  i_cookie = p_op->p3;
  assert( p_op->p3<DBSQL_N_BTREE_META );
  assert( i_db>=0 && i_db<db->n_db );
  assert( db->dbs[i_db].bt!=0 );
  assert( DB_MASK_TEST(p->btree_mask, i_db) );

  __sm_get_meta(db->dbs[i_db].bt, i_cookie, (u_int32_t *)&i_meta);
  out = out2Prerelease(p, p_op);
  out->u.i = i_meta;
  break;
}

/* Opcode: __set_cookie P1 P2 P3 * P5
**
** Write the integer value P3 into cookie number P2 of database P1.
** P2==1 is the schema version.  P2==2 is the database format.
** P2==3 is the recommended pager cache
** size, and so forth.  P1==0 is the main database file and P1==1 is the
** database file used to store temporary tables.
**
** A transaction must be started before executing this opcode.
**
** If P2 is the SCHEMA_VERSION cookie (cookie number 1) then the internal
** schema version is set to P3-P5.  The "PRAGMA schema_version=N" statement
** has P5 set to 1, so that the internal schema version will be different
** from the database schema version, resulting in a schema reset.
*/
case OP_SetCookie: {
  db_t *p_db;

  __vdbe_incr_write_counter(p, 0);
  assert( p_op->p2<DBSQL_N_BTREE_META );
  assert( p_op->p1>=0 && p_op->p1<db->n_db );
  assert( DB_MASK_TEST(p->btree_mask, p_op->p1) );
  assert( p->readOnly==0 );
  p_db = &db->dbs[p_op->p1];
  assert( p_db->bt!=0 );
  assert( __schema_mutex_held(db, p_op->p1, 0) );
  /* See note about index shifting on OP_ReadCookie */
  rc = __sm_update_meta(p_db->bt, p_op->p2, p_op->p3);
  if( p_op->p2==BTREE_SCHEMA_VERSION ){
    /* When the schema cookie changes, record the new cookie internally */
    *(u_int32_t*)&p_db->schema->schema_cookie = *(u_int32_t*)&p_op->p3 - p_op->p5;
    db->m_db_flags |= DBFLAG_SchemaChange;
    __fk_clear_trigger_cache(db, p_op->p1);
  }else if( p_op->p2==BTREE_FILE_FORMAT ){
    /* Record changes in the file format */
    p_db->schema->file_format = p_op->p3;
  }
  if( p_op->p1==1 ){
    /* Invalidate all prepared statements whenever the TEMP database
    ** schema is changed.  Ticket #1644 */
    __expire_prepared_statements(db, 0);
    p->expired = 0;
  }
  if( rc ) goto abort_due_to_error;
  break;
}

/* Opcode: __open_read P1 P2 P3 P4 P5
** Synopsis: root=P2 i_db=P3
**
** Open a read-only cursor for the database table whose root page is
** P2 in a database file.  The database file is determined by P3.
** P3==0 means the main database, P3==1 means the database used for
** temporary tables, and P3>1 means used the corresponding attached
** database.  Give the new cursor an identifier of P1.  The P1
** values need not be contiguous but all P1 values should be small integers.
** It is an error for P1 to be negative.
**
** Allowed P5 bits:
** <ul>
** <li>  <b>0x02 OPFLAG_SEEKEQ</b>: This cursor will only be used for
**       equality lookups (implemented as a pair of opcodes OP_SeekGE/OP_IdxGT
**       of OP_SeekLE/OP_IdxLT)
** </ul>
**
** The P4 value may be either an integer (P4_INT32) or a pointer to
** a key_info_t structure (P4_KEYINFO). If it is a pointer to a key_info_t
** object, then table being opened must be an [index b-tree] where the
** key_info_t object defines the content and collating
** sequence of that index b-tree. Otherwise, if P4 is an integer
** value, then the table being opened must be a [table b-tree] with a
** number of columns no less than the value of P4.
**
** See also: __open_write, __reopen_idx
*/
/* Opcode: __reopen_idx P1 P2 P3 P4 P5
** Synopsis: root=P2 i_db=P3
**
** The __reopen_idx opcode works like OP_OpenRead except that it first
** checks to see if the cursor on P1 is already open on the same
** b-tree and if it is this opcode becomes a no-op.  In other words,
** if the cursor is already open, do not reopen it.
**
** The __reopen_idx opcode may only be used with P5==0 or P5==OPFLAG_SEEKEQ
** and with P4 being a P4_KEYINFO object.  Furthermore, the P3 value must
** be the same as every other __reopen_idx or __open_read for the same cursor
** number.
**
** Allowed P5 bits:
** <ul>
** <li>  <b>0x02 OPFLAG_SEEKEQ</b>: This cursor will only be used for
**       equality lookups (implemented as a pair of opcodes OP_SeekGE/OP_IdxGT
**       of OP_SeekLE/OP_IdxLT)
** </ul>
**
** See also: OP_OpenRead, OP_OpenWrite
*/
/* Opcode: __open_write P1 P2 P3 P4 P5
** Synopsis: root=P2 i_db=P3
**
** Open a read/write cursor named P1 on the table or index whose root
** page is P2 (or whose root page is held in register P2 if the
** OPFLAG_P2ISREG bit is set in P5 - see below).
**
** The P4 value may be either an integer (P4_INT32) or a pointer to
** a key_info_t structure (P4_KEYINFO). If it is a pointer to a key_info_t
** object, then table being opened must be an [index b-tree] where the
** key_info_t object defines the content and collating
** sequence of that index b-tree. Otherwise, if P4 is an integer
** value, then the table being opened must be a [table b-tree] with a
** number of columns no less than the value of P4.
**
** Allowed P5 bits:
** <ul>
** <li>  <b>0x02 OPFLAG_SEEKEQ</b>: This cursor will only be used for
**       equality lookups (implemented as a pair of opcodes OP_SeekGE/OP_IdxGT
**       of OP_SeekLE/OP_IdxLT)
** <li>  <b>0x08 OPFLAG_FORDELETE</b>: This cursor is used only to seek
**       and subsequently delete entries in an index btree.  This is a
**       hint to the storage engine that the storage engine is allowed to
**       ignore.  The hint is not used by the official dbsql b*tree storage
**       engine, but is used by COMDB2.
** <li>  <b>0x10 OPFLAG_P2ISREG</b>: Use the content of register P2
**       as the root page, not the value of P2 itself.
** </ul>
**
** This instruction works like __open_read except that it opens the cursor
** in read/write mode.
**
** See also: OP_OpenRead, OP_ReopenIdx
*/
case OP_ReopenIdx: {         /* ncycle */
  int n_field;
  key_info_t *key_info;
  u_int32_t p2;
  int i_db;
  int wr_flag;
  sm_t *p_x;
  vdbe_cursor_t *cur;
  db_t *p_db;

  assert( p_op->p5==0 || p_op->p5==OPFLAG_SEEKEQ );
  assert( p_op->p4type==P4_KEYINFO );
  cur = p->csrs[p_op->p1];
  if( cur && cur->pgno_root==(u_int32_t)p_op->p2 ){
    assert( cur->i_db==p_op->p3 );      /* Guaranteed by the code generator */
    assert( cur->cur_type==CURTYPE_BTREE );
    __sm_clear_cursor(cur->uc.cursor);
    goto open_cursor_set_hints;
  }
  /* If the cursor is not currently open or is open on a different
  ** index, then fall through into OP_OpenRead to force a reopen */
case OP_OpenRead:            /* ncycle */
case OP_OpenWrite:

  assert( p_op->opcode==OP_OpenWrite || p_op->p5==0 || p_op->p5==OPFLAG_SEEKEQ );
  assert( p->is_reader );
  assert( p_op->opcode==OP_OpenRead || p_op->opcode==OP_ReopenIdx
          || p->readOnly==0 );

  if( p->expired==1 ){
    rc = DBSQL_ABORT_ROLLBACK;
    goto abort_due_to_error;
  }

  n_field = 0;
  key_info = 0;
  p2 = (u_int32_t)p_op->p2;
  i_db = p_op->p3;
  assert( i_db>=0 && i_db<db->n_db );
  assert( DB_MASK_TEST(p->btree_mask, i_db) );
  p_db = &db->dbs[i_db];
  p_x = p_db->bt;
  assert( p_x!=0 );
  if( p_op->opcode==OP_OpenWrite ){
    assert( OPFLAG_FORDELETE==BTREE_FORDELETE );
    wr_flag = BTREE_WRCSR | (p_op->p5 & OPFLAG_FORDELETE);
    assert( __schema_mutex_held(db, i_db, 0) );
    if( p_db->schema->file_format < p->min_write_file_format ){
      p->min_write_file_format = p_db->schema->file_format;
    }
    if( p_op->p5 & OPFLAG_P2ISREG ){
      assert( p2>0 );
      assert( p2<=(u_int32_t)(p->n_mem+1 - p->n_cursor) );
      in2 = &mems[p2];
      assert( MEM_IS_VALID(in2) );
      assert( (in2->flags & MEM_Int)!=0 );
      __vdbe_mem_integerify(in2);
      p2 = (int)in2->u.i;
      /* The p2 value always comes from a prior OP_CreateBtree opcode and
      ** that opcode will always set the p2 value to 2 or more or else fail.
      ** If there were a failure, the prepared statement would have halted
      ** before reaching this instruction. */
      assert( p2>=2 );
    }
  }else{
    wr_flag = 0;
    assert( (p_op->p5 & OPFLAG_P2ISREG)==0 );
  }
  if( p_op->p4type==P4_KEYINFO ){
    key_info = p_op->p4.key_info;
    assert( key_info->enc==ENC(db) );
    assert( key_info->db==db );
    n_field = key_info->n_all_field;
  }else if( p_op->p4type==P4_INT32 ){
    n_field = p_op->p4.i;
  }
  assert( p_op->p1>=0 );
  assert( n_field>=0 );
  testcase( n_field==0 );  /* table_t with INTEGER PRIMARY KEY and nothing else */
  cur = __allocate_cursor(p, p_op->p1, n_field, CURTYPE_BTREE);
  if( cur==0 ) goto no_mem;
  cur->i_db = i_db;
  cur->null_row = 1;
  cur->is_ordered = 1;
  cur->pgno_root = p2;
#ifdef DBSQL_DEBUG
  cur->wr_flag = wr_flag;
#endif
  rc = __sm_cursor(p_x, p2, wr_flag, key_info, cur->uc.cursor);
  cur->key_info = key_info;
  /* Set the vdbe_cursor_t.is_table variable. Previous versions of
  ** dbsql used to check if the root-page flags were sane at this point
  ** and report database corruption if they were not, but this check has
  ** since moved into the btree layer.  */ 
  cur->is_table = p_op->p4type!=P4_KEYINFO;

open_cursor_set_hints:
  assert( OPFLAG_BULKCSR==BTREE_BULKLOAD );
  assert( OPFLAG_SEEKEQ==BTREE_SEEK_EQ );
  testcase( p_op->p5 & OPFLAG_BULKCSR );
  testcase( p_op->p2 & OPFLAG_SEEKEQ );
  __sm_cursor_hint_flags(cur->uc.cursor,
                               (p_op->p5 & (OPFLAG_BULKCSR|OPFLAG_SEEKEQ)));
  if( rc ) goto abort_due_to_error;
  break;
}

/* Opcode: __open_dup P1 P2 * * *
**
** Open a new cursor P1 that points to the same ephemeral table as
** cursor P2.  The P2 cursor must have been opened by a prior OP_OpenEphemeral
** opcode.  Only ephemeral cursors may be duplicated.
**
** Duplicate ephemeral cursors are used for self-joins of materialized views.
*/
case OP_OpenDup: {           /* ncycle */
  vdbe_cursor_t *orig;    /* The original cursor to be duplicated */
  vdbe_cursor_t *cx;      /* The new cursor */

  orig = p->csrs[p_op->p2];
  assert( orig );
  assert( orig->is_ephemeral );  /* Only ephemeral cursors can be duplicated */

  cx = __allocate_cursor(p, p_op->p1, orig->n_field, CURTYPE_BTREE);
  if( cx==0 ) goto no_mem;
  cx->null_row = 1;
  cx->is_ephemeral = 1;
  cx->key_info = orig->key_info;
  cx->is_table = orig->is_table;
  cx->pgno_root = orig->pgno_root;
  cx->is_ordered = orig->is_ordered;
  cx->ub.btx = orig->ub.btx;
  cx->no_reuse = 1;
  orig->no_reuse = 1;
  rc = __sm_cursor(cx->ub.btx, cx->pgno_root, BTREE_WRCSR,
                          cx->key_info, cx->uc.cursor);
  /* The __sm_cursor() routine can only fail for the first cursor
  ** opened for a database.  Since there is already an open cursor when this
  ** opcode is run, the __sm_cursor() cannot fail */
  assert( rc==DBSQL_OK );
  break;
}


/* Opcode: __open_ephemeral P1 P2 P3 P4 P5
** Synopsis: n_column=P2
**
** Open a new cursor P1 to a transient table.
** The cursor is always opened read/write even if
** the main database is read-only.  The ephemeral
** table is deleted automatically when the cursor is closed.
**
** If the cursor P1 is already opened on an ephemeral table, the table
** is cleared (all content is erased).
**
** P2 is the number of columns in the ephemeral table.
** The cursor points to a BTree table if P4==0 and to a BTree index
** if P4 is not 0.  If P4 is not NULL, it points to a key_info_t structure
** that defines the format of keys in the index.
**
** The P5 parameter can be a mask of the BTREE_* flags defined
** in btree.h.  These flags control aspects of the operation of
** the btree.  The BTREE_OMIT_JOURNAL and BTREE_SINGLE flags are
** added automatically.
**
** If P3 is positive, then reg[P3] is modified slightly so that it
** can be used as zero-length data for OP_Insert.  This is an optimization
** that avoids an extra OP_Blob opcode to initialize that register.
*/
/* Opcode: __open_autoindex P1 P2 * P4 *
** Synopsis: n_column=P2
**
** This opcode works the same as OP_OpenEphemeral.  It has a
** different name to distinguish its use.  Tables created using
** by this opcode will be used for automatically created transient
** indices in joins.
*/
case OP_OpenAutoindex:       /* ncycle */
case OP_OpenEphemeral: {     /* ncycle */
  vdbe_cursor_t *cx;
  key_info_t *key_info;

  static const int vfs_flags =
      DBSQL_OPEN_READWRITE |
      DBSQL_OPEN_CREATE |
      DBSQL_OPEN_EXCLUSIVE |
      DBSQL_OPEN_DELETEONCLOSE |
      DBSQL_OPEN_TRANSIENT_DB;
  assert( p_op->p1>=0 );
  assert( p_op->p2>=0 );
  if( p_op->p3>0 ){
    /* Make register reg[P3] into a value that can be used as the data
    ** form __sm_insert() where the length of the data is zero. */
    assert( p_op->p2==0 ); /* Only used when number of columns is zero */
    assert( p_op->opcode==OP_OpenEphemeral );
    assert( mems[p_op->p3].flags & MEM_Null );
    mems[p_op->p3].n = 0;
    mems[p_op->p3].z = "";
  }
  cx = p->csrs[p_op->p1];
  if( cx && !cx->no_reuse &&  ALWAYS(p_op->p2<=cx->n_field) ){
    /* If the ephemeral table is already open and has no duplicates from
    ** OP_OpenDup, then erase all existing content so that the table is
    ** empty again, rather than creating a new table. */
    assert( cx->is_ephemeral );
    cx->seq_count = 0;
    cx->cache_status = CACHE_STALE;
    rc = __sm_clear_table(cx->ub.btx, cx->pgno_root, 0);
  }else{
    cx = __allocate_cursor(p, p_op->p1, p_op->p2, CURTYPE_BTREE);
    if( cx==0 ) goto no_mem;
    cx->is_ephemeral = 1;
    rc = __sm_open(db->vfs, 0, db, &cx->ub.btx,
                          BTREE_OMIT_JOURNAL | BTREE_SINGLE | p_op->p5,
                          vfs_flags);
    if( rc==DBSQL_OK ){
      rc = __sm_begin_trans(cx->ub.btx, 1, 0);
      if( rc==DBSQL_OK ){
        /* If a transient index is required, create it by calling
        ** __sm_create_table() with the BTREE_BLOBKEY flag before
        ** opening it. If a transient table is required, just use the
        ** automatically created table with root-page 1 (an BLOB_INTKEY table).
        */
        if( (cx->key_info = key_info = p_op->p4.key_info)!=0 ){
          assert( p_op->p4type==P4_KEYINFO );
          rc = __sm_create_table(cx->ub.btx, &cx->pgno_root,
              BTREE_BLOBKEY | p_op->p5);
          if( rc==DBSQL_OK ){
            assert( cx->pgno_root==SCHEMA_ROOT+1 );
            assert( key_info->db==db );
            assert( key_info->enc==ENC(db) );
            rc = __sm_cursor(cx->ub.btx, cx->pgno_root, BTREE_WRCSR,
                key_info, cx->uc.cursor);
          }
          cx->is_table = 0;
        }else{
          cx->pgno_root = SCHEMA_ROOT;
          rc = __sm_cursor(cx->ub.btx, SCHEMA_ROOT, BTREE_WRCSR,
              0, cx->uc.cursor);
          cx->is_table = 1;
        }
      }
      cx->is_ordered = (p_op->p5!=BTREE_UNORDERED);
      assert( p->csrs[p_op->p1]==cx );
      if( rc ){
        assert( !__sm_closes_with_cursor(cx->ub.btx, cx->uc.cursor) );
        __sm_close(cx->ub.btx);
        p->csrs[p_op->p1] = 0;  /* Not required; helps with static analysis */
      }else{
        assert( __sm_closes_with_cursor(cx->ub.btx, cx->uc.cursor) );
      }
    }
  }
  if( rc ) goto abort_due_to_error;
  cx->null_row = 1;
  break;
}

/* Opcode: __sorter_open P1 P2 P3 P4 *
**
** This opcode works like OP_OpenEphemeral except that it opens
** a transient index that is specifically designed to sort large
** tables using an external merge-sort algorithm.
**
** If argument P3 is non-zero, then it indicates that the sorter may
** assume that a stable sort considering the first P3 fields of each
** key is sufficient to produce the required results.
*/
case OP_SorterOpen: {
  vdbe_cursor_t *cx;

  assert( p_op->p1>=0 );
  assert( p_op->p2>=0 );
  cx = __allocate_cursor(p, p_op->p1, p_op->p2, CURTYPE_SORTER);
  if( cx==0 ) goto no_mem;
  cx->key_info = p_op->p4.key_info;
  assert( cx->key_info->db==db );
  assert( cx->key_info->enc==ENC(db) );
  rc = __vdbe_sorter_init(db, p_op->p3, cx);
  if( rc ) goto abort_due_to_error;
  break;
}

/* Opcode: __sequence_test P1 P2 * * *
** Synopsis: if( cursor[P1].ctr++ ) pc = P2
**
** P1 is a sorter cursor. If the sequence counter is currently zero, jump
** to P2. Regardless of whether or not the jump is taken, increment the
** the sequence value.
*/
case OP_SequenceTest: {
  vdbe_cursor_t *p_c;
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( IS_SORTER(p_c) );
  if( (p_c->seq_count++)==0 ){
    goto jump_to_p2;
  }
  break;
}

/* Opcode: __open_pseudo P1 P2 P3 * *
** Synopsis: P3 columns in r[P2]
**
** Open a new cursor that points to a fake table that contains a single
** row of data.  The content of that one row is the content of memory
** register P2.  In other words, cursor P1 becomes an alias for the
** MEM_Blob content contained in register P2.
**
** A pseudo-table created by this opcode is used to hold a single
** row output from the sorter so that the row can be decomposed into
** individual columns using the OP_Column opcode.  The OP_Column opcode
** is the only cursor opcode that works with a pseudo-table.
**
** P3 is the number of fields in the records that will be stored by
** the pseudo-table.  If P2 is 0 or negative then the pseudo-cursor
** will return NULL for every column.
*/
case OP_OpenPseudo: {
  vdbe_cursor_t *cx;

  assert( p_op->p1>=0 );
  assert( p_op->p3>=0 );
  cx = __allocate_cursor(p, p_op->p1, p_op->p3, CURTYPE_PSEUDO);
  if( cx==0 ) goto no_mem;
  cx->null_row = 1;
  cx->seek_result = p_op->p2;
  cx->is_table = 1;
  /* Give this pseudo-cursor a fake sm_cursor_t pointer so that cx
  ** can be safely passed to __vdbe_cursor_moveto().  This avoids a test
  ** for cx->cur_type==CURTYPE_BTREE inside of __vdbe_cursor_moveto()
  ** which is a performance optimization */
  cx->uc.cursor = __sm_fake_valid_cursor();
  assert( p_op->p5==0 );
  break;
}

/* Opcode: Close P1 * * * *
**
** Close a cursor previously opened as P1.  If P1 is not
** currently open, this instruction is a no-op.
*/
case OP_Close: {             /* ncycle */
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  __vdbe_free_cursor(p, p->csrs[p_op->p1]);
  p->csrs[p_op->p1] = 0;
  break;
}

#ifdef DBSQL_ENABLE_COLUMN_USED_MASK
/* Opcode: __columns_used P1 * * P4 *
**
** This opcode (which only exists if dbsql was compiled with
** DBSQL_ENABLE_COLUMN_USED_MASK) identifies which columns of the
** table or index for cursor P1 are used.  P4 is a 64-bit integer
** (P4_INT64) in which the first 63 bits are one for each of the
** first 63 columns of the table or index that are actually used
** by the cursor.  The high-order bit is set if any column after
** the 64th is used.
*/
case OP_ColumnsUsed: {
  vdbe_cursor_t *p_c;
  p_c = p->csrs[p_op->p1];
  assert( p_c->cur_type==CURTYPE_BTREE );
  p_c->mask_used = *(u_int64_t*)p_op->p4.i64;
  break;
}
#endif

/* Opcode: __seek_ge P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If cursor P1 refers to an SQL table (B-Tree that uses integer keys),
** use the value in register P3 as the key.  If cursor P1 refers
** to an SQL index, then P3 is the first in an array of P4 registers
** that are used as an unpacked index key.
**
** Reposition cursor P1 so that  it points to the smallest entry that
** is greater than or equal to the key value. If there are no records
** greater than or equal to the key and P2 is not zero, then jump to P2.
**
** If the cursor P1 was opened using the OPFLAG_SEEKEQ flag, then this
** opcode will either land on a record that exactly matches the key, or
** else it will cause a jump to P2.  When the cursor is OPFLAG_SEEKEQ,
** this opcode must be followed by an __idx_le opcode with the same arguments.
** The __idx_gt opcode will be skipped if this opcode succeeds, but the
** __idx_gt opcode will be used on subsequent loop iterations.  The
** OPFLAG_SEEKEQ flags is a hint to the btree layer to say that this
** is an equality search.
**
** This opcode leaves the cursor configured to move in forward order,
** from the beginning toward the end.  In other words, the cursor is
** configured to use Next, not Prev.
**
** See also: Found, __not_found, __seek_lt, __seek_gt, __seek_le
*/
/* Opcode: __seek_gt P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If cursor P1 refers to an SQL table (B-Tree that uses integer keys),
** use the value in register P3 as a key. If cursor P1 refers
** to an SQL index, then P3 is the first in an array of P4 registers
** that are used as an unpacked index key.
**
** Reposition cursor P1 so that it points to the smallest entry that
** is greater than the key value. If there are no records greater than
** the key and P2 is not zero, then jump to P2.
**
** This opcode leaves the cursor configured to move in forward order,
** from the beginning toward the end.  In other words, the cursor is
** configured to use Next, not Prev.
**
** See also: Found, __not_found, __seek_lt, __seek_ge, __seek_le
*/
/* Opcode: __seek_lt P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If cursor P1 refers to an SQL table (B-Tree that uses integer keys),
** use the value in register P3 as a key. If cursor P1 refers
** to an SQL index, then P3 is the first in an array of P4 registers
** that are used as an unpacked index key.
**
** Reposition cursor P1 so that  it points to the largest entry that
** is less than the key value. If there are no records less than
** the key and P2 is not zero, then jump to P2.
**
** This opcode leaves the cursor configured to move in reverse order,
** from the end toward the beginning.  In other words, the cursor is
** configured to use Prev, not Next.
**
** See also: Found, __not_found, __seek_gt, __seek_ge, __seek_le
*/
/* Opcode: __seek_le P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If cursor P1 refers to an SQL table (B-Tree that uses integer keys),
** use the value in register P3 as a key. If cursor P1 refers
** to an SQL index, then P3 is the first in an array of P4 registers
** that are used as an unpacked index key.
**
** Reposition cursor P1 so that it points to the largest entry that
** is less than or equal to the key value. If there are no records
** less than or equal to the key and P2 is not zero, then jump to P2.
**
** This opcode leaves the cursor configured to move in reverse order,
** from the end toward the beginning.  In other words, the cursor is
** configured to use Prev, not Next.
**
** If the cursor P1 was opened using the OPFLAG_SEEKEQ flag, then this
** opcode will either land on a record that exactly matches the key, or
** else it will cause a jump to P2.  When the cursor is OPFLAG_SEEKEQ,
** this opcode must be followed by an __idx_le opcode with the same arguments.
** The __idx_ge opcode will be skipped if this opcode succeeds, but the
** __idx_ge opcode will be used on subsequent loop iterations.  The
** OPFLAG_SEEKEQ flags is a hint to the btree layer to say that this
** is an equality search.
**
** See also: Found, __not_found, __seek_gt, __seek_ge, __seek_lt
*/
case OP_SeekLT:         /* jump0, in3, group, ncycle */
case OP_SeekLE:         /* jump0, in3, group, ncycle */
case OP_SeekGE:         /* jump0, in3, group, ncycle */
case OP_SeekGT: {       /* jump0, in3, group, ncycle */
  int res;           /* Comparison result */
  int oc;            /* Opcode */
  vdbe_cursor_t *p_c;    /* The cursor to seek */
  unpacked_record_t r;  /* The key to seek for */
  int n_field;        /* Number of columns or fields in the key */
  int64_t i_key;          /* The rowid we are to seek to */
  int eq_only;        /* Only interested in == results */

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( p_op->p2!=0 );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( OP_SeekLE == OP_SeekLT+1 );
  assert( OP_SeekGE == OP_SeekLT+2 );
  assert( OP_SeekGT == OP_SeekLT+3 );
  assert( p_c->is_ordered );
  assert( p_c->uc.cursor!=0 );
  oc = p_op->opcode;
  eq_only = 0;
  p_c->null_row = 0;
#ifdef DBSQL_DEBUG
  p_c->seek_op = p_op->opcode;
#endif

  p_c->deferred_moveto = 0;
  p_c->cache_status = CACHE_STALE;
  if( p_c->is_table ){
    u_int16_t flags3, new_type;
    /* The OPFLAG_SEEKEQ/BTREE_SEEK_EQ flag is only set on index cursors */
    assert( __sm_cursor_has_hint(p_c->uc.cursor, BTREE_SEEK_EQ)==0
              || CORRUPT_DB );

    /* The input value in P3 might be of any type: integer, real, string,
    ** blob, or NULL.  But it needs to be an integer before we can do
    ** the seek, so convert it. */
    in3 = &mems[p_op->p3];
    flags3 = in3->flags;
    if( (flags3 & (MEM_Int|MEM_Real|MEM_IntReal|MEM_Str))==MEM_Str ){
      __apply_numeric_affinity(in3, 0);
    }
    i_key = __vdbe_int_value(in3); /* Get the integer key value */
    new_type = in3->flags; /* Record the type after applying numeric affinity */
    in3->flags = flags3;  /* But convert the type back to its original */

    /* If the P3 value could not be converted into an integer without
    ** loss of information, then special processing is required... */
    if( (new_type & (MEM_Int|MEM_IntReal))==0 ){
      int c;
      if( (new_type & MEM_Real)==0 ){
        if( (new_type & MEM_Null) || oc>=OP_SeekGE ){
          VDBE_BRANCH_TAKEN(1,2);
          goto jump_to_p2;
        }else{
          rc = __sm_last(p_c->uc.cursor, &res);
          if( rc!=DBSQL_OK ) goto abort_due_to_error;
          goto seek_not_found;
        }
      }
      c = __int_float_compare(i_key, in3->u.r);

      /* If the approximation i_key is larger than the actual real search
      ** term, substitute >= for > and < for <=. e.g. if the search term
      ** is 4.9 and the integer approximation 5:
      **
      **        (x >  4.9)    ->     (x >= 5)
      **        (x <= 4.9)    ->     (x <  5)
      */
      if( c>0 ){
        assert( OP_SeekGE==(OP_SeekGT-1) );
        assert( OP_SeekLT==(OP_SeekLE-1) );
        assert( (OP_SeekLE & 0x0001)==(OP_SeekGT & 0x0001) );
        if( (oc & 0x0001)==(OP_SeekGT & 0x0001) ) oc--;
      }

      /* If the approximation i_key is smaller than the actual real search
      ** term, substitute <= for < and > for >=.  */
      else if( c<0 ){
        assert( OP_SeekLE==(OP_SeekLT+1) );
        assert( OP_SeekGT==(OP_SeekGE+1) );
        assert( (OP_SeekLT & 0x0001)==(OP_SeekGE & 0x0001) );
        if( (oc & 0x0001)==(OP_SeekLT & 0x0001) ) oc++;
      }
    }
    rc = __sm_table_moveto(p_c->uc.cursor, (u_int64_t)i_key, 0, &res);
    p_c->moveto_target = i_key;  /* Used by OP_Delete */
    if( rc!=DBSQL_OK ){
      goto abort_due_to_error;
    }
  }else{
    /* For a cursor with the OPFLAG_SEEKEQ/BTREE_SEEK_EQ hint, only the
    ** OP_SeekGE and OP_SeekLE opcodes are allowed, and these must be
    ** immediately followed by an OP_IdxGT or OP_IdxLT opcode, respectively,
    ** with the same key.
    */
    if( __sm_cursor_has_hint(p_c->uc.cursor, BTREE_SEEK_EQ) ){
      eq_only = 1;
      assert( p_op->opcode==OP_SeekGE || p_op->opcode==OP_SeekLE );
      assert( p_op[1].opcode==OP_IdxLT || p_op[1].opcode==OP_IdxGT );
      assert( p_op->opcode==OP_SeekGE || p_op[1].opcode==OP_IdxLT );
      assert( p_op->opcode==OP_SeekLE || p_op[1].opcode==OP_IdxGT );
      assert( p_op[1].p1==p_op[0].p1 );
      assert( p_op[1].p2==p_op[0].p2 );
      assert( p_op[1].p3==p_op[0].p3 );
      assert( p_op[1].p4.i==p_op[0].p4.i );
    }

    n_field = p_op->p4.i;
    assert( p_op->p4type==P4_INT32 );
    assert( n_field>0 );
    r.key_info = p_c->key_info;
    r.n_field = (u_int16_t)n_field;

    /* The next line of code computes as follows, only faster:
    **   if( oc==OP_SeekGT || oc==OP_SeekLE ){
    **     r.default_rc = -1;
    **   }else{
    **     r.default_rc = +1;
    **   }
    */
    r.default_rc = ((1 & (oc - OP_SeekLT)) ? -1 : +1);
    assert( oc!=OP_SeekGT || r.default_rc==-1 );
    assert( oc!=OP_SeekLE || r.default_rc==-1 );
    assert( oc!=OP_SeekGE || r.default_rc==+1 );
    assert( oc!=OP_SeekLT || r.default_rc==+1 );

    r.mems = &mems[p_op->p3];
#ifdef DBSQL_DEBUG
    {
      int i;
      for(i=0; i<r.n_field; i++){
        assert( MEM_IS_VALID(&r.mems[i]) );
        if( i>0 ) REGISTER_TRACE(p_op->p3+i, &r.mems[i]);
      }
    }
#endif
    r.eq_seen = 0;
    rc = __sm_index_moveto(p_c->uc.cursor, &r, &res);
    if( rc!=DBSQL_OK ){
      goto abort_due_to_error;
    }
    if( eq_only && r.eq_seen==0 ){
      assert( res!=0 );
      goto seek_not_found;
    }
  }
#ifdef DBSQL_TEST
  dbsql_search_count++;
#endif
  if( oc>=OP_SeekGE ){  assert( oc==OP_SeekGE || oc==OP_SeekGT );
    if( res<0 || (res==0 && oc==OP_SeekGT) ){
      res = 0;
      rc = __sm_next(p_c->uc.cursor, 0);
      if( rc!=DBSQL_OK ){
        if( rc==DBSQL_DONE ){
          rc = DBSQL_OK;
          res = 1;
        }else{
          goto abort_due_to_error;
        }
      }
    }else{
      res = 0;
    }
  }else{
    assert( oc==OP_SeekLT || oc==OP_SeekLE );
    if( res>0 || (res==0 && oc==OP_SeekLT) ){
      res = 0;
      rc = __sm_previous(p_c->uc.cursor, 0);
      if( rc!=DBSQL_OK ){
        if( rc==DBSQL_DONE ){
          rc = DBSQL_OK;
          res = 1;
        }else{
          goto abort_due_to_error;
        }
      }
    }else{
      /* res might be negative because the table is empty.  Check to
      ** see if this is the case.
      */
      res = __sm_eof(p_c->uc.cursor);
    }
  }
seek_not_found:
  assert( p_op->p2>0 );
  VDBE_BRANCH_TAKEN(res!=0,2);
  if( res ){
    goto jump_to_p2;
  }else if( eq_only ){
    assert( p_op[1].opcode==OP_IdxLT || p_op[1].opcode==OP_IdxGT );
    p_op++; /* Skip the __op_idx_lt or OP_IdxGT that follows */
  }
  break;
}


/* Opcode: __seek_scan  P1 P2 * * P5
** Synopsis: Scan-ahead up to P1 rows
**
** This opcode is a prefix opcode to OP_SeekGE.  In other words, this
** opcode must be immediately followed by OP_SeekGE. This constraint is
** checked by assert() statements.
**
** This opcode uses the P1 through P4 operands of the subsequent
** OP_SeekGE.  In the text that follows, the operands of the subsequent
** OP_SeekGE opcode are denoted as __seek_op.P1 through __seek_op.P4.   Only
** the P1, P2 and P5 operands of this opcode are also used, and  are called
** This.P1, This.P2 and This.P5.
**
** This opcode helps to optimize IN operators on a multi-column index
** where the IN operator is on the later terms of the index by avoiding
** unnecessary seeks on the btree, substituting steps to the next row
** of the b-tree instead.  A correct answer is obtained if this opcode
** is omitted or is a no-op.
**
** The __seek_ge.P3 and __seek_ge.P4 operands identify an unpacked key which
** is the desired entry that we want the cursor __seek_ge.P1 to be pointing
** to.  Call this __seek_ge.P3/P4 row the "target".
**
** If the __seek_ge.P1 cursor is not currently pointing to a valid row,
** then this opcode is a no-op and control passes through into the OP_SeekGE.
**
** If the __seek_ge.P1 cursor is pointing to a valid row, then that row
** might be the target row, or it might be near and slightly before the
** target row, or it might be after the target row.  If the cursor is
** currently before the target row, then this opcode attempts to position
** the cursor on or after the target row by invoking __sm_step()
** on the cursor between 1 and This.P1 times.
**
** The This.P5 parameter is a flag that indicates what to do if the
** cursor ends up pointing at a valid row that is past the target
** row.  If This.P5 is false (0) then a jump is made to __seek_ge.P2.  If
** This.P5 is true (non-zero) then a jump is made to This.P2.  The P5==0
** case occurs when there are no inequality constraints to the right of
** the IN constraint.  The jump to __seek_ge.P2 ends the loop.  The P5!=0 case
** occurs when there are inequality constraints to the right of the IN
** operator.  In that case, the This.P2 will point either directly to or
** to setup code prior to the OP_IdxGT or OP_IdxGE opcode that checks for
** loop terminate.
**
** Possible outcomes from this opcode:<ol>
**
** <li> If the cursor is initially not pointed to any valid row, then
**      fall through into the subsequent OP_SeekGE opcode.
**
** <li> If the cursor is left pointing to a row that is before the target
**      row, even after making as many as This.P1 calls to
**      __sm_next(), then also fall through into OP_SeekGE.
**
** <li> If the cursor is left pointing at the target row, either because it
**      was at the target row to begin with or because one or more
**      __sm_next() calls moved the cursor to the target row,
**      then jump to This.P2..,
**
** <li> If the cursor started out before the target row and a call to
**      to __sm_next() moved the cursor off the end of the index
**      (indicating that the target row definitely does not exist in the
**      btree) then jump to __seek_ge.P2, ending the loop.
**
** <li> If the cursor ends up on a valid row that is past the target row
**      (indicating that the target row does not exist in the btree) then
**      jump to __seek_op.P2 if This.P5==0 or to This.P2 if This.P5>0.
** </ol>
*/
case OP_SeekScan: {          /* ncycle */
  vdbe_cursor_t *p_c;
  int res;
  int n_step;
  unpacked_record_t r;

  assert( p_op[1].opcode==OP_SeekGE );

  /* If op->p5 is clear, then op->p2 points to the first instruction past the
  ** OP_IdxGT that follows the OP_SeekGE. Otherwise, it points to the first
  ** opcode past the OP_SeekGE itself.  */
  assert( p_op->p2>=(int)(p_op-ops)+2 );
#ifdef DBSQL_DEBUG
  if( p_op->p5==0 ){
    /* There are no inequality constraints following the IN constraint. */
    assert( p_op[1].p1==ops[p_op->p2-1].p1 );
    assert( p_op[1].p2==ops[p_op->p2-1].p2 );
    assert( p_op[1].p3==ops[p_op->p2-1].p3 );
    assert( ops[p_op->p2-1].opcode==OP_IdxGT
         || ops[p_op->p2-1].opcode==OP_IdxGE );
    testcase( ops[p_op->p2-1].opcode==OP_IdxGE );
  }else{
    /* There are inequality constraints.  */
    assert( p_op->p2==(int)(p_op-ops)+2 );
    assert( ops[p_op->p2-1].opcode==OP_SeekGE );
  }
#endif

  assert( p_op->p1>0 );
  p_c = p->csrs[p_op[1].p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( !p_c->is_table );
  if( !__sm_cursor_is_valid_nn(p_c->uc.cursor) ){
#ifdef DBSQL_DEBUG
     if( db->flags&DBSQL_VdbeTrace ){
       printf("... cursor not valid - fall through\n");
     }       
#endif
    break;
  }
  n_step = p_op->p1;
  assert( n_step>=1 );
  r.key_info = p_c->key_info;
  r.n_field = (u_int16_t)p_op[1].p4.i;
  r.default_rc = 0;
  r.mems = &mems[p_op[1].p3];
#ifdef DBSQL_DEBUG
  {
    int i;
    for(i=0; i<r.n_field; i++){
      assert( MEM_IS_VALID(&r.mems[i]) );
      REGISTER_TRACE(p_op[1].p3+i, &mems[p_op[1].p3+i]);
    }
  }
#endif
  res = 0;  /* Not needed.  Only used to silence a warning. */
  while(1){
    rc = __vdbe_idx_key_compare(db, p_c, &r, &res);
    if( rc ) goto abort_due_to_error;
    if( res>0 && p_op->p5==0 ){
      seekscan_search_fail:
      /* Jump to __seek_ge.P2, ending the loop */
#ifdef DBSQL_DEBUG
      if( db->flags&DBSQL_VdbeTrace ){
        printf("... %d steps and then skip\n", p_op->p1 - n_step);
      }       
#endif
      VDBE_BRANCH_TAKEN(1,3);
      p_op++;
      goto jump_to_p2;
    }
    if( res>=0 ){
      /* Jump to This.P2, bypassing the OP_SeekGE opcode */
#ifdef DBSQL_DEBUG
      if( db->flags&DBSQL_VdbeTrace ){
        printf("... %d steps and then success\n", p_op->p1 - n_step);
      }       
#endif
      VDBE_BRANCH_TAKEN(2,3);
      goto jump_to_p2;
      break;
    }
    if( n_step<=0 ){
#ifdef DBSQL_DEBUG
      if( db->flags&DBSQL_VdbeTrace ){
        printf("... fall through after %d steps\n", p_op->p1);
      }       
#endif
      VDBE_BRANCH_TAKEN(0,3);
      break;
    }
    n_step--;
    p_c->cache_status = CACHE_STALE;
    rc = __sm_next(p_c->uc.cursor, 0);
    if( rc ){
      if( rc==DBSQL_DONE ){
        rc = DBSQL_OK;
        goto seekscan_search_fail;
      }else{
        goto abort_due_to_error;
      }
    }
  }
 
  break;
}


/* Opcode: __seek_hit P1 P2 P3 * *
** Synopsis: set P2<=seek_hit<=P3
**
** Increase or decrease the seek_hit value for cursor P1, if necessary,
** so that it is no less than P2 and no greater than P3.
**
** The seek_hit integer represents the maximum of terms in an index for which
** there is known to be at least one match.  If the seek_hit value is smaller
** than the total number of equality terms in an index lookup, then the
** OP_IfNoHope opcode might run to see if the IN loop can be abandoned
** early, thus saving work.  This is part of the IN-early-out optimization.
**
** P1 must be a valid b-tree cursor.
*/
case OP_SeekHit: {           /* ncycle */
  vdbe_cursor_t *p_c;
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_op->p3>=p_op->p2 );
  if( p_c->seek_hit<p_op->p2 ){
#ifdef DBSQL_DEBUG
    if( db->flags&DBSQL_VdbeTrace ){
      printf("seekHit changes from %d to %d\n", p_c->seek_hit, p_op->p2);
    }       
#endif
    p_c->seek_hit = p_op->p2;
  }else if( p_c->seek_hit>p_op->p3 ){
#ifdef DBSQL_DEBUG
    if( db->flags&DBSQL_VdbeTrace ){
      printf("seekHit changes from %d to %d\n", p_c->seek_hit, p_op->p3);
    }       
#endif
    p_c->seek_hit = p_op->p3;
  }
  break;
}

/* Opcode: __if_not_open P1 P2 * * *
** Synopsis: if( !csr[P1] ) goto P2
**
** If cursor P1 is not open or if P1 is set to a NULL row using the
** OP_NullRow opcode, then jump to instruction P2. Otherwise, fall through.
*/
case OP_IfNotOpen: {        /* jump */
  vdbe_cursor_t *cur;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  cur = p->csrs[p_op->p1];
  VDBE_BRANCH_TAKEN(cur==0 || cur->null_row, 2);
  if( cur==0 || cur->null_row ){
    goto jump_to_p2_and_check_for_interrupt;
  }
  break;
}

/* Opcode: Found P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If P4==0 then register P3 holds a blob constructed by __make_record.  If
** P4>0 then register P3 is the first of P4 registers that form an unpacked
** record.
**
** Cursor P1 is on an index btree.  If the record identified by P3 and P4
** is a prefix of any entry in P1 then a jump is made to P2 and
** P1 is left pointing at the matching entry.
**
** This operation leaves the cursor in a state where it can be
** advanced in the forward direction.  The Next instruction will work,
** but not the Prev instruction.
**
** See also: __not_found, __no_conflict, __not_exists. __seek_ge
*/
/* Opcode: __not_found P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If P4==0 then register P3 holds a blob constructed by __make_record.  If
** P4>0 then register P3 is the first of P4 registers that form an unpacked
** record.
**
** Cursor P1 is on an index btree.  If the record identified by P3 and P4
** is not the prefix of any entry in P1 then a jump is made to P2.  If P1
** does contain an entry whose prefix matches the P3/P4 record then control
** falls through to the next instruction and P1 is left pointing at the
** matching entry.
**
** This operation leaves the cursor in a state where it cannot be
** advanced in either direction.  In other words, the Next and Prev
** opcodes do not work after this operation.
**
** See also: Found, __not_exists, __no_conflict, __if_no_hope
*/
/* Opcode: __if_no_hope P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** Register P3 is the first of P4 registers that form an unpacked
** record.  Cursor P1 is an index btree.  P2 is a jump destination.
** In other words, the operands to this opcode are the same as the
** operands to OP_NotFound and OP_IdxGT.
**
** This opcode is an optimization attempt only.  If this opcode always
** falls through, the correct answer is still obtained, but extra work
** is performed.
**
** A value of N in the seek_hit flag of cursor P1 means that there exists
** a key P3:N that will match some record in the index.  We want to know
** if it is possible for a record P3:P4 to match some record in the
** index.  If it is not possible, we can skip some work.  So if seek_hit
** is less than P4, attempt to find out if a match is possible by running
** OP_NotFound.
**
** This opcode is used in IN clause processing for a multi-column key.
** If an IN clause is attached to an element of the key other than the
** left-most element, and if there are no matches on the most recent
** seek over the whole key, then it might be that one of the key element
** to the left is prohibiting a match, and hence there is "no hope" of
** any match regardless of how many IN clause elements are checked.
** In such a case, we abandon the IN clause search early, using this
** opcode.  The opcode name comes from the fact that the
** jump is taken if there is "no hope" of achieving a match.
**
** See also: __not_found, __seek_hit
*/
/* Opcode: __no_conflict P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** If P4==0 then register P3 holds a blob constructed by __make_record.  If
** P4>0 then register P3 is the first of P4 registers that form an unpacked
** record.
**
** Cursor P1 is on an index btree.  If the record identified by P3 and P4
** contains any NULL value, jump immediately to P2.  If all terms of the
** record are not-NULL then a check is done to determine if any row in the
** P1 index btree has a matching key prefix.  If there are no matches, jump
** immediately to P2.  If there is a match, fall through and leave the P1
** cursor pointing to the matching row.
**
** This opcode is similar to OP_NotFound with the exceptions that the
** branch is always taken if any part of the search key input is NULL.
**
** This operation leaves the cursor in a state where it cannot be
** advanced in either direction.  In other words, the Next and Prev
** opcodes do not work after this operation.
**
** See also: __not_found, Found, __not_exists
*/
case OP_IfNoHope: {     /* jump, in3, ncycle */
  vdbe_cursor_t *p_c;
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
#ifdef DBSQL_DEBUG
  if( db->flags&DBSQL_VdbeTrace ){
    printf("seekHit is %d\n", p_c->seek_hit);
  }       
#endif
  if( p_c->seek_hit>=p_op->p4.i ) break;
  /* Fall through into OP_NotFound */
  /* no break */ deliberate_fall_through
}
case OP_NoConflict:     /* jump, in3, ncycle */
case OP_NotFound:       /* jump, in3, ncycle */
case OP_Found: {        /* jump, in3, ncycle */
  int already_exists;
  int ii;
  vdbe_cursor_t *p_c;
  unpacked_record_t *idx_key;
  unpacked_record_t r;

#ifdef DBSQL_TEST
  if( p_op->opcode!=OP_NoConflict ) dbsql_found_count++;
#endif

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( p_op->p4type==P4_INT32 );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
#ifdef DBSQL_DEBUG
  p_c->seek_op = p_op->opcode;
#endif
  r.mems = &mems[p_op->p3];
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( p_c->uc.cursor!=0 );
  assert( p_c->is_table==0 );
  r.n_field = (u_int16_t)p_op->p4.i;
  if( r.n_field>0 ){
    /* Key values in an array of registers */
    r.key_info = p_c->key_info;
    r.default_rc = 0;
#ifdef DBSQL_DEBUG
    (void)__fault_sim(50);  /* For use by --counter in TH3 */
    for(ii=0; ii<r.n_field; ii++){
      assert( MEM_IS_VALID(&r.mems[ii]) );
      assert( (r.mems[ii].flags & MEM_Zero)==0 || r.mems[ii].n==0 );
      if( ii ) REGISTER_TRACE(p_op->p3+ii, &r.mems[ii]);
    }
#endif
    rc = __sm_index_moveto(p_c->uc.cursor, &r, &p_c->seek_result);
  }else{
    /* Composite key generated by OP_MakeRecord */
    assert( r.mems->flags & MEM_Blob );
    assert( p_op->opcode!=OP_NoConflict );
    rc = EXPAND_BLOB(r.mems);
    assert( rc==DBSQL_OK || rc==DBSQL_NOMEM );
    if( rc ) goto no_mem;
    idx_key = __vdbe_alloc_unpacked_record(p_c->key_info);
    if( idx_key==0 ) goto no_mem;
    __vdbe_record_unpack(r.mems->n, r.mems->z, idx_key);
    idx_key->default_rc = 0;
    rc = __sm_index_moveto(p_c->uc.cursor, idx_key, &p_c->seek_result);
    __db_free_nn(db, idx_key);
  }
  if( rc!=DBSQL_OK ){
    goto abort_due_to_error;
  }
  already_exists = (p_c->seek_result==0);
  p_c->null_row = 1-already_exists;
  p_c->deferred_moveto = 0;
  p_c->cache_status = CACHE_STALE;
  if( p_op->opcode==OP_Found ){
    VDBE_BRANCH_TAKEN(already_exists!=0,2);
    if( already_exists ) goto jump_to_p2;
  }else{
    if( !already_exists ){
      VDBE_BRANCH_TAKEN(1,2);
      goto jump_to_p2;
    }
    if( p_op->opcode==OP_NoConflict ){
      /* For the OP_NoConflict opcode, take the jump if any of the
      ** input fields are NULL, since any key with a NULL will not
      ** conflict */
      for(ii=0; ii<r.n_field; ii++){
        if( r.mems[ii].flags & MEM_Null ){
          VDBE_BRANCH_TAKEN(1,2);
          goto jump_to_p2;
        }
      }
    }
    VDBE_BRANCH_TAKEN(0,2);
    if( p_op->opcode==OP_IfNoHope ){
      p_c->seek_hit = p_op->p4.i;
    }
  }
  break;
}

/* Opcode: __seek_rowid P1 P2 P3 * *
** Synopsis: intkey=r[P3]
**
** P1 is the index of a cursor open on an SQL table btree (with integer
** keys).  If register P3 does not contain an integer or if P1 does not
** contain a record with rowid P3 then jump immediately to P2. 
** Or, if P2 is 0, raise an DBSQL_CORRUPT error. If P1 does contain
** a record with rowid P3 then
** leave the cursor pointing at that record and fall through to the next
** instruction.
**
** The OP_NotExists opcode performs the same operation, but with OP_NotExists
** the P3 register must be guaranteed to contain an integer value.  with_t this
** opcode, register P3 might not contain an integer.
**
** The OP_NotFound opcode performs the same operation on index btrees
** (with arbitrary multi-value keys).
**
** This opcode leaves the cursor in a state where it cannot be advanced
** in either direction.  In other words, the Next and Prev opcodes will
** not work following this opcode.
**
** See also: Found, __not_found, __no_conflict, __seek_rowid
*/
/* Opcode: __not_exists P1 P2 P3 * *
** Synopsis: intkey=r[P3]
**
** P1 is the index of a cursor open on an SQL table btree (with integer
** keys).  P3 is an integer rowid.  If P1 does not contain a record with
** rowid P3 then jump immediately to P2.  Or, if P2 is 0, raise an
** DBSQL_CORRUPT error. If P1 does contain a record with rowid P3 then
** leave the cursor pointing at that record and fall through to the next
** instruction.
**
** The OP_SeekRowid opcode performs the same operation but also allows the
** P3 register to contain a non-integer value, in which case the jump is
** always taken.  This opcode requires that P3 always contain an integer.
**
** The OP_NotFound opcode performs the same operation on index btrees
** (with arbitrary multi-value keys).
**
** This opcode leaves the cursor in a state where it cannot be advanced
** in either direction.  In other words, the Next and Prev opcodes will
** not work following this opcode.
**
** See also: Found, __not_found, __no_conflict, __seek_rowid
*/
case OP_SeekRowid: {        /* jump0, in3, ncycle */
  vdbe_cursor_t *p_c;
  sm_cursor_t *crsr;
  int res;
  u_int64_t i_key;

  in3 = &mems[p_op->p3];
  testcase( in3->flags & MEM_Int );
  testcase( in3->flags & MEM_IntReal );
  testcase( in3->flags & MEM_Real );
  testcase( (in3->flags & (MEM_Str|MEM_Int))==MEM_Str );
  if( (in3->flags & (MEM_Int|MEM_IntReal))==0 ){
    /* If in3->u.i does not contain an integer, compute i_key as the
    ** integer value of in3.  Jump to P2 if in3 cannot be converted
    ** into an integer without loss of information.  Take care to avoid
    ** changing the datatype of in3, however, as it is used by other
    ** parts of the prepared statement. */
    mem_t x = in3[0];
    __apply_affinity(&x, DBSQL_AFF_NUMERIC, encoding);
    if( (x.flags & MEM_Int)==0 ) goto jump_to_p2;
    i_key = x.u.i;
    goto not_exists_with_key;
  }
  /* Fall through into OP_NotExists */
  /* no break */ deliberate_fall_through
case OP_NotExists:          /* jump, in3, ncycle */
  in3 = &mems[p_op->p3];
  assert( (in3->flags & MEM_Int)!=0 || p_op->opcode==OP_SeekRowid );
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  i_key = in3->u.i;
not_exists_with_key:
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
#ifdef DBSQL_DEBUG
  if( p_op->opcode==OP_SeekRowid ) p_c->seek_op = OP_SeekRowid;
#endif
  assert( p_c->is_table );
  assert( p_c->cur_type==CURTYPE_BTREE );
  crsr = p_c->uc.cursor;
  assert( crsr!=0 );
  res = 0;
  rc = __sm_table_moveto(crsr, i_key, 0, &res);
  assert( rc==DBSQL_OK || res==0 );
  p_c->moveto_target = i_key;  /* Used by OP_Delete */
  p_c->null_row = 0;
  p_c->cache_status = CACHE_STALE;
  p_c->deferred_moveto = 0;
  VDBE_BRANCH_TAKEN(res!=0,2);
  p_c->seek_result = res;
  if( res!=0 ){
    assert( rc==DBSQL_OK );
    if( p_op->p2==0 ){
      rc = DBSQL_CORRUPT_BKPT;
    }else{
      goto jump_to_p2;
    }
  }
  if( rc ) goto abort_due_to_error;
  break;
}

/* Opcode: Sequence P1 P2 * * *
** Synopsis: r[P2]=cursor[P1].ctr++
**
** Find the next available sequence number for cursor P1.
** Write the sequence number into register P2.
** The sequence number on the cursor is incremented after this
** instruction. 
*/
case OP_Sequence: {           /* out2 */
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( p->csrs[p_op->p1]!=0 );
  assert( p->csrs[p_op->p1]->cur_type!=CURTYPE_VTAB );
  out = out2Prerelease(p, p_op);
  out->u.i = p->csrs[p_op->p1]->seq_count++;
  break;
}


/* Opcode: __new_rowid P1 P2 P3 * *
** Synopsis: r[P2]=rowid
**
** Get a new integer record number (a.k.a "rowid") used as the key to a table.
** The record number is not previously used as a key in the database
** table that cursor P1 points to.  The new record number is written
** written to register P2.
**
** If P3>0 then P3 is a register in the root frame of this VDBE that holds
** the largest previously generated record number. No new record numbers are
** allowed to be less than this value. When this value reaches its maximum,
** an DBSQL_FULL error is generated. The P3 register is updated with the '
** generated record number. This P3 mechanism is used to help implement the
** AUTOINCREMENT feature.
*/
case OP_NewRowid: {           /* out2 */
  int64_t v;                 /* The new rowid */
  vdbe_cursor_t *p_c;        /* Cursor of table to get the new rowid */
  int res;               /* Result of an __sm_last() */
  int cnt;               /* Counter to limit the number of searches */
#ifndef DBSQL_OMIT_AUTOINCREMENT
  mem_t *mem;             /* Register holding largest rowid for AUTOINCREMENT */
  vdbe_frame_t *frame;     /* Root frame of VDBE */
#endif

  v = 0;
  res = 0;
  out = out2Prerelease(p, p_op);
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->is_table );
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( p_c->uc.cursor!=0 );
  {
    /* The next rowid or record number (different terms for the same
    ** thing) is obtained in a two-step algorithm.
    **
    ** First we attempt to find the largest existing rowid and add one
    ** to that.  But if the largest existing rowid is already the maximum
    ** positive integer, we have to fall through to the second
    ** probabilistic algorithm
    **
    ** The second algorithm is to select a rowid at random and see if
    ** it already exists in the table.  If it does not exist, we have
    ** succeeded.  If the random rowid does exist, we select a new one
    ** and try again, up to 100 times.
    */
    assert( p_c->is_table );

#ifdef DBSQL_32BIT_ROWID
#   define MAX_ROWID 0x7fffffff
#else
    /* Some compilers complain about constants of the form 0x7fffffffffffffff.
    ** Others complain about 0x7ffffffffffffffffLL.  The following macro seems
    ** to provide the constant while making all compilers happy.
    */
#   define MAX_ROWID  (int64_t)( (((u_int64_t)0x7fffffff)<<32) | (u_int64_t)0xffffffff )
#endif

    if( !p_c->use_random_rowid ){
      rc = __sm_last(p_c->uc.cursor, &res);
      if( rc!=DBSQL_OK ){
        goto abort_due_to_error;
      }
      if( res ){
        v = 1;   /* IMP: R-61914-48074 */
      }else{
        assert( __sm_cursor_is_valid(p_c->uc.cursor) );
        v = __sm_integer_key(p_c->uc.cursor);
        if( v>=MAX_ROWID ){
          p_c->use_random_rowid = 1;
        }else{
          v++;   /* IMP: R-29538-34987 */
        }
      }
    }

#ifndef DBSQL_OMIT_AUTOINCREMENT
    if( p_op->p3 ){
      /* Assert that P3 is a valid memory cell. */
      assert( p_op->p3>0 );
      if( p->frame ){
        for(frame=p->frame; frame->parent; frame=frame->parent);
        /* Assert that P3 is a valid memory cell. */
        assert( p_op->p3<=frame->n_mem );
        mem = &frame->mems[p_op->p3];
      }else{
        /* Assert that P3 is a valid memory cell. */
        assert( p_op->p3<=(p->n_mem+1 - p->n_cursor) );
        mem = &mems[p_op->p3];
        MEM_ABOUT_TO_CHANGE(p, mem);
      }
      assert( MEM_IS_VALID(mem) );

      REGISTER_TRACE(p_op->p3, mem);
      __vdbe_mem_integerify(mem);
      assert( (mem->flags & MEM_Int)!=0 );  /* mem(P3) holds an integer */
      if( mem->u.i==MAX_ROWID || p_c->use_random_rowid ){
        rc = DBSQL_FULL;   /* IMP: R-17817-00630 */
        goto abort_due_to_error;
      }
      if( v<mem->u.i+1 ){
        v = mem->u.i + 1;
      }
      mem->u.i = v;
    }
#endif
    if( p_c->use_random_rowid ){
      /* IMPLEMENTATION-OF: R-07677-41881 If the largest ROWID is equal to the
      ** largest possible integer (9223372036854775807) then the database
      ** engine starts picking positive candidate ROWIDs at random until
      ** it finds one that is not previously used. */
      assert( p_op->p3==0 );  /* We cannot be in random rowid mode if this is
                             ** an AUTOINCREMENT table. */
      cnt = 0;
      do{
        dbsql_randomness(sizeof(v), &v);
        v &= (MAX_ROWID>>1); v++;  /* Ensure that v is greater than zero */
      }while(  ((rc = __sm_table_moveto(p_c->uc.cursor, (u_int64_t)v,
                                                 0, &res))==DBSQL_OK)
            && (res==0)
            && (++cnt<100));
      if( rc ) goto abort_due_to_error;
      if( res==0 ){
        rc = DBSQL_FULL;   /* IMP: R-38219-53002 */
        goto abort_due_to_error;
      }
      assert( v>0 );  /* EV: R-40812-03570 */
    }
    p_c->deferred_moveto = 0;
    p_c->cache_status = CACHE_STALE;
  }
  out->u.i = v;
  break;
}

/* Opcode: Insert P1 P2 P3 P4 P5
** Synopsis: intkey=r[P3] data=r[P2]
**
** Write an entry into the table of cursor P1.  A new entry is
** created if it doesn't already exist or the data for an existing
** entry is overwritten.  The data is the value MEM_Blob stored in register
** number P2. The key is stored in register P3. The key must
** be a MEM_Int.
**
** If the OPFLAG_NCHANGE flag of P5 is set, then the row change count is
** incremented (otherwise not).  If the OPFLAG_LASTROWID flag of P5 is set,
** then rowid is stored for subsequent return by the
** dbsql_last_insert_rowid() function (otherwise it is unmodified).
**
** If the OPFLAG_USESEEKRESULT flag of P5 is set, the implementation might
** run faster by avoiding an unnecessary seek on cursor P1.  However,
** the OPFLAG_USESEEKRESULT flag must only be set if there have been no prior
** seeks on the cursor or if the most recent seek used a key equal to P3.
**
** If the OPFLAG_ISUPDATE flag is set, then this opcode is part of an
** UPDATE operation.  Otherwise (if the flag is clear) then this opcode
** is part of an INSERT operation.  The difference is only important to
** the update hook.
**
** Parameter P4 may point to a table_t structure, or may be NULL. If it is
** not NULL, then the update-hook (DBSQL.update_callback) is invoked
** following a successful insert.
**
** (WARNING/TODO: If P1 is a pseudo-cursor and P2 is dynamically
** allocated, then ownership of P2 is transferred to the pseudo-cursor
** and register P2 becomes ephemeral.  If the cursor is changed, the
** value of register P2 will then change.  Make sure this does not
** cause any problems.)
**
** This instruction only works on tables.  The equivalent instruction
** for indices is OP_IdxInsert.
*/
case OP_Insert: {
  mem_t *data;       /* MEM cell holding data for the record to be inserted */
  mem_t *key;        /* MEM cell holding key  for the record */
  vdbe_cursor_t *p_c;   /* Cursor to table into which insert is written */
  int seek_result;   /* Result of prior seek or 0 if no USESEEKRESULT flag */
  const char *z_db;  /* database name - used by the update hook */
  table_t *tab;      /* table_t structure - used by update and pre-update hooks */
  sm_payload_t x;   /* Payload to be inserted */

  data = &mems[p_op->p2];
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( MEM_IS_VALID(data) );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( p_c->deferred_moveto==0 );
  assert( p_c->uc.cursor!=0 );
  assert( (p_op->p5 & OPFLAG_ISNOOP) || p_c->is_table );
  assert( p_op->p4type==P4_TABLE || p_op->p4type>=P4_STATIC );
  REGISTER_TRACE(p_op->p2, data);
  __vdbe_incr_write_counter(p, p_c);

  key = &mems[p_op->p3];
  assert( key->flags & MEM_Int );
  assert( MEM_IS_VALID(key) );
  REGISTER_TRACE(p_op->p3, key);
  x.n_key = key->u.i;

  if( p_op->p4type==P4_TABLE && HAS_UPDATE_HOOK(db) ){
    assert( p_c->i_db>=0 );
    z_db = db->dbs[p_c->i_db].db_s_name;
    tab = p_op->p4.tab;
    assert( (p_op->p5 & OPFLAG_ISNOOP) || HAS_ROWID(tab) );
  }else{
    tab = 0;
    z_db = 0;
  }

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
  /* Invoke the pre-update hook, if any */
  if( tab ){
    if( db->pre_update_callback && !(p_op->p5 & OPFLAG_ISUPDATE) ){
      __vdbe_pre_update_hook(p,p_c,DBSQL_INSERT,z_db,tab,x.n_key,p_op->p2,-1);
    }
    if( db->update_callback==0 || tab->cols==0 ){
      /* Prevent post-update hook from running in cases when it should not */
      tab = 0;
    }
  }
  if( p_op->p5 & OPFLAG_ISNOOP ) break;
#endif

  assert( (p_op->p5 & OPFLAG_LASTROWID)==0 || (p_op->p5 & OPFLAG_NCHANGE)!=0 );
  if( p_op->p5 & OPFLAG_NCHANGE ){
    p->n_change++;
    if( p_op->p5 & OPFLAG_LASTROWID ) db->last_rowid = x.n_key;
  }
  assert( (data->flags & (MEM_Blob|MEM_Str))!=0 || data->n==0 );
  x.data = data->z;
  x.n_data = data->n;
  seek_result = ((p_op->p5 & OPFLAG_USESEEKRESULT) ? p_c->seek_result : 0);
  if( data->flags & MEM_Zero ){
    x.n_zero = data->u.n_zero;
  }else{
    x.n_zero = 0;
  }
  x.key = 0;
  assert( BTREE_PREFORMAT==OPFLAG_PREFORMAT );
  rc = __sm_insert(p_c->uc.cursor, &x,
      (p_op->p5 & (OPFLAG_APPEND|OPFLAG_SAVEPOSITION|OPFLAG_PREFORMAT)),
      seek_result
  );
  p_c->deferred_moveto = 0;
  p_c->cache_status = CACHE_STALE;
  col_cache_ctr++;

  /* Invoke the update-hook if required. */
  if( rc ) goto abort_due_to_error;
  if( tab ){
    assert( db->update_callback!=0 );
    assert( tab->cols!=0 );
    db->update_callback(db->update_arg,
           (p_op->p5 & OPFLAG_ISUPDATE) ? DBSQL_UPDATE : DBSQL_INSERT,
           z_db, tab->name, x.n_key);
  }
  break;
}

/* Opcode: __row_cell P1 P2 P3 * *
**
** P1 and P2 are both open cursors. Both must be opened on the same type
** of table - intkey or index. This opcode is used as part of copying
** the current row from P2 into P1. If the cursors are opened on intkey
** tables, register P3 contains the rowid to use with the new record in
** P1. If they are opened on index tables, P3 is not used.
**
** This opcode must be followed by either an Insert or __insert_idx opcode
** with the OPFLAG_PREFORMAT flag set to complete the insert operation.
*/
case OP_RowCell: {
  vdbe_cursor_t *dest;              /* Cursor to write to */
  vdbe_cursor_t *src;               /* Cursor to read from */
  int64_t i_key;                       /* Rowid value to insert with */
  assert( p_op[1].opcode==OP_Insert || p_op[1].opcode==OP_IdxInsert );
  assert( p_op[1].opcode==OP_Insert    || p_op->p3==0 );
  assert( p_op[1].opcode==OP_IdxInsert || p_op->p3>0 );
  assert( p_op[1].p5 & OPFLAG_PREFORMAT );
  dest = p->csrs[p_op->p1];
  src = p->csrs[p_op->p2];
  i_key = p_op->p3 ? mems[p_op->p3].u.i : 0;
  rc = __sm_transfer_row(dest->uc.cursor, src->uc.cursor, i_key);
  if( rc!=DBSQL_OK ) goto abort_due_to_error;
  break;
};

/* Opcode: Delete P1 P2 P3 P4 P5
**
** Delete the record at which the P1 cursor is currently pointing.
**
** If the OPFLAG_SAVEPOSITION bit of the P5 parameter is set, then
** the cursor will be left pointing at  either the next or the previous
** record in the table. If it is left pointing at the next record, then
** the next Next instruction will be a no-op. As a result, in this case
** it is ok to delete a record from within a Next loop. If
** OPFLAG_SAVEPOSITION bit of P5 is clear, then the cursor will be
** left in an undefined state.
**
** If the OPFLAG_AUXDELETE bit is set on P5, that indicates that this
** delete is one of several associated with deleting a table row and
** all its associated index entries.  Exactly one of those deletes is
** the "primary" delete.  The others are all on OPFLAG_FORDELETE
** cursors or else are marked with the AUXDELETE flag.
**
** If the OPFLAG_NCHANGE (0x01) flag of P2 (NB: P2 not P5) is set, then
** the row change count is incremented (otherwise not).
**
** If the OPFLAG_ISNOOP (0x40) flag of P2 (not P5!) is set, then the
** pre-update-hook for deletes is run, but the btree is otherwise unchanged.
** This happens when the OP_Delete is to be shortly followed by an OP_Insert
** with the same key, causing the btree entry to be overwritten.
**
** P1 must not be pseudo-table.  It has to be a real table with
** multiple rows.
**
** If P4 is not NULL then it points to a table_t object. In this case either
** the update or pre-update hook, or both, may be invoked. The P1 cursor must
** have been positioned using OP_NotFound prior to invoking this opcode in
** this case. Specifically, if one is configured, the pre-update hook is
** invoked if P4 is not NULL. The update-hook is invoked if one is configured,
** P4 is not NULL, and the OPFLAG_NCHANGE flag is set in P2.
**
** If the OPFLAG_ISUPDATE flag is set in P2, then P3 contains the address
** of the memory cell that contains the value that the rowid of the row will
** be set to by the update.
*/
case OP_Delete: {
  vdbe_cursor_t *p_c;
  const char *z_db;
  table_t *tab;
  int opflags;

  opflags = p_op->p2;
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( p_c->uc.cursor!=0 );
  assert( p_c->deferred_moveto==0 );
  __vdbe_incr_write_counter(p, p_c);

#ifdef DBSQL_DEBUG
  if( p_op->p4type==P4_TABLE
   && HAS_ROWID(p_op->p4.tab)
   && p_op->p5==0
   && __sm_cursor_is_valid_nn(p_c->uc.cursor)
  ){
    /* If p5 is zero, the seek operation that positioned the cursor prior to
    ** OP_Delete will have also set the c->moveto_target field to the rowid of
    ** the row that is being deleted */
    int64_t i_key = __sm_integer_key(p_c->uc.cursor);
    assert( CORRUPT_DB || p_c->moveto_target==i_key );
  }
#endif

  /* If the update-hook or pre-update-hook will be invoked, set db to
  ** the name of the db to pass as to it. Also set local tab to a copy
  ** of p4.tab. Finally, if p5 is true, indicating that this cursor was
  ** last moved with OP_Next or OP_Prev, not Seek or __not_found, set
  ** vdbe_cursor_t.moveto_target to the current rowid.  */
  if( p_op->p4type==P4_TABLE && HAS_UPDATE_HOOK(db) ){
    assert( p_c->i_db>=0 );
    assert( p_op->p4.tab!=0 );
    z_db = db->dbs[p_c->i_db].db_s_name;
    tab = p_op->p4.tab;
    if( (p_op->p5 & OPFLAG_SAVEPOSITION)!=0 && p_c->is_table ){
      p_c->moveto_target = __sm_integer_key(p_c->uc.cursor);
    }
  }else{
    z_db = 0;
    tab = 0;
  }

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
  /* Invoke the pre-update-hook if required. */
  assert( db->pre_update_callback==0 || tab==p_op->p4.tab );
  if( db->pre_update_callback && tab ){
    assert( !(opflags & OPFLAG_ISUPDATE)
         || HAS_ROWID(tab)==0
         || (mems[p_op->p3].flags & MEM_Int)
    );
    __vdbe_pre_update_hook(p, p_c,
        (opflags & OPFLAG_ISUPDATE) ? DBSQL_UPDATE : DBSQL_DELETE,
        z_db, tab, p_c->moveto_target,
        p_op->p3, -1
    );
  }
  if( opflags & OPFLAG_ISNOOP ) break;
#endif

  /* Only flags that can be set are SAVEPOISTION and AUXDELETE */
  assert( (p_op->p5 & ~(OPFLAG_SAVEPOSITION|OPFLAG_AUXDELETE))==0 );
  assert( OPFLAG_SAVEPOSITION==BTREE_SAVEPOSITION );
  assert( OPFLAG_AUXDELETE==BTREE_AUXDELETE );

#ifdef DBSQL_DEBUG
  if( p->frame==0 ){
    if( p_c->is_ephemeral==0
        && (p_op->p5 & OPFLAG_AUXDELETE)==0
        && (p_c->wr_flag & OPFLAG_FORDELETE)==0
      ){
      n_extra_delete++;
    }
    if( p_op->p2 & OPFLAG_NCHANGE ){
      n_extra_delete--;
    }
  }
#endif

  rc = __sm_delete(p_c->uc.cursor, p_op->p5);
  p_c->cache_status = CACHE_STALE;
  col_cache_ctr++;
  p_c->seek_result = 0;
  if( rc ) goto abort_due_to_error;

  /* Invoke the update-hook if required. */
  if( opflags & OPFLAG_NCHANGE ){
    p->n_change++;
    if( db->update_callback && ALWAYS(tab!=0) && HAS_ROWID(tab) ){
      db->update_callback(db->update_arg, DBSQL_DELETE, z_db, tab->name,
          p_c->moveto_target);
      assert( p_c->i_db>=0 );
    }
  }

  break;
}
/* Opcode: __reset_count * * * * *
**
** The value of the change counter is copied to the database handle
** change counter (returned by subsequent calls to dbsql_changes()).
** Then the VMs internal change counter resets to 0.
** This is used by trigger programs.
*/
case OP_ResetCount: {
  __vdbe_set_changes(db, p->n_change);
  p->n_change = 0;
  break;
}

/* Opcode: __sorter_compare P1 P2 P3 P4
** Synopsis: if key(P1)!=trim(r[P3],P4) goto P2
**
** P1 is a sorter cursor. This instruction compares a prefix of the
** record blob in register P3 against a prefix of the entry that
** the sorter cursor currently points to.  Only the first P4 fields
** of r[P3] and the sorter record are compared.
**
** If either P3 or the sorter contains a NULL in one of their significant
** fields (not counting the P4 fields at the end which are ignored) then
** the comparison is assumed to be equal.
**
** Fall through to next instruction if the two records compare equal to
** each other.  Jump to P2 if they are different.
*/
case OP_SorterCompare: {
  vdbe_cursor_t *p_c;
  int res;
  int n_key_col;

  p_c = p->csrs[p_op->p1];
  assert( IS_SORTER(p_c) );
  assert( p_op->p4type==P4_INT32 );
  in3 = &mems[p_op->p3];
  n_key_col = p_op->p4.i;
  res = 0;
  rc = __vdbe_sorter_compare(p_c, in3, n_key_col, &res);
  VDBE_BRANCH_TAKEN(res!=0,2);
  if( rc ) goto abort_due_to_error;
  if( res ) goto jump_to_p2;
  break;
};

/* Opcode: __sorter_data P1 P2 P3 * *
** Synopsis: r[P2]=data
**
** Write into register P2 the current sorter data for sorter cursor P1.
** Then clear the column header cache on cursor P3.
**
** This opcode is normally used to move a record out of the sorter and into
** a register that is the source for a pseudo-table cursor created using
** __open_pseudo.  That pseudo-table cursor is the one that is identified by
** parameter P3.  Clearing the P3 column cache as part of this opcode saves
** us from having to issue a separate __null_row instruction to clear that cache.
*/
case OP_SorterData: {       /* ncycle */
  vdbe_cursor_t *p_c;

  out = &mems[p_op->p2];
  p_c = p->csrs[p_op->p1];
  assert( IS_SORTER(p_c) );
  rc = __vdbe_sorter_rowkey(p_c, out);
  assert( rc!=DBSQL_OK || (out->flags & MEM_Blob) );
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  if( rc ) goto abort_due_to_error;
  p->csrs[p_op->p3]->cache_status = CACHE_STALE;
  break;
}

/* Opcode: __row_data P1 P2 P3 * *
** Synopsis: r[P2]=data
**
** Write into register P2 the complete row content for the row at
** which cursor P1 is currently pointing.
** There is no interpretation of the data. 
** It is just copied onto the P2 register exactly as
** it is found in the database file.
**
** If cursor P1 is an index, then the content is the key of the row.
** If cursor P2 is a table, then the content extracted is the data.
**
** If the P1 cursor must be pointing to a valid row (not a NULL row)
** of a real table, not a pseudo-table.
**
** If P3!=0 then this opcode is allowed to make an ephemeral pointer
** into the database page.  That means that the content of the output
** register will be invalidated as soon as the cursor moves - including
** moves caused by other cursors that "save" the current cursors
** position in order that they can write to the same table.  If P3==0
** then a copy of the data is made into memory.  P3!=0 is faster, but
** P3==0 is safer.
**
** If P3!=0 then the content of the P2 register is unsuitable for use
** in OP_Result and any OP_Result will invalidate the P2 register content.
** The P2 register content is invalidated by opcodes like OP_Function or
** by any use of another cursor pointing to the same table.
*/
case OP_RowData: {
  vdbe_cursor_t *p_c;
  sm_cursor_t *crsr;
  u_int32_t n;

  out = out2Prerelease(p, p_op);

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( IS_SORTER(p_c)==0 );
  assert( p_c->null_row==0 );
  assert( p_c->uc.cursor!=0 );
  crsr = p_c->uc.cursor;

  /* The OP_RowData opcodes always follow OP_NotExists or
  ** OP_SeekRowid or OP_Rewind/Op_Next with no intervening instructions
  ** that might invalidate the cursor.
  ** If this were not the case, one of the following assert()s
  ** would fail.  Should this ever change (because of changes in the code
  ** generator) then the fix would be to insert a call to
  ** __vdbe_cursor_moveto().
  */
  assert( p_c->deferred_moveto==0 );
  assert( __sm_cursor_is_valid(crsr) );

  n = __sm_payload_size(crsr);
  if( n>(u_int32_t)db->limits[DBSQL_LIMIT_LENGTH] ){
    goto too_big;
  }
  testcase( n==0 );
  rc = __vdbe_mem_from_btree_zero_offset(crsr, n, out);
  if( rc ) goto abort_due_to_error;
  if( !p_op->p3 ) DEEPHEMERALIZE(out);
  UPDATE_MAX_BLOBSIZE(out);
  REGISTER_TRACE(p_op->p2, out);
  break;
}

/* Opcode: Rowid P1 P2 * * *
** Synopsis: r[P2]=PX rowid of P1
**
** Store in register P2 an integer which is the key of the table entry that
** P1 is currently point to.
**
** P1 can be either an ordinary table or a virtual table.  There used to
** be a separate OP_VRowid opcode for use with virtual tables, but this
** one opcode now works for both table types.
*/
case OP_Rowid: {                 /* out2, ncycle */
  vdbe_cursor_t *p_c;
  int64_t v;
  dbsql_vtab_t *vtab;
  const dbsql_module_t *module;

  out = out2Prerelease(p, p_op);
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type!=CURTYPE_PSEUDO || p_c->null_row );
  if( p_c->null_row ){
    out->flags = MEM_Null;
    break;
  }else if( p_c->deferred_moveto ){
    v = p_c->moveto_target;
#ifndef DBSQL_OMIT_VIRTUALTABLE
  }else if( p_c->cur_type==CURTYPE_VTAB ){
    assert( p_c->uc.v_cur!=0 );
    vtab = p_c->uc.v_cur->vtab;
    module = vtab->module;
    assert( module->rowid );
    rc = module->rowid(p_c->uc.v_cur, &v);
    __vtab_import_errmsg(p, vtab);
    if( rc ) goto abort_due_to_error;
#endif /* DBSQL_OMIT_VIRTUALTABLE */
  }else{
    assert( p_c->cur_type==CURTYPE_BTREE );
    assert( p_c->uc.cursor!=0 );
    rc = __vdbe_cursor_restore(p_c);
    if( rc ) goto abort_due_to_error;
    if( p_c->null_row ){
      out->flags = MEM_Null;
      break;
    }
    v = __sm_integer_key(p_c->uc.cursor);
  }
  out->u.i = v;
  break;
}

/* Opcode: __null_row P1 * * * *
**
** Move the cursor P1 to a null row.  Any OP_Column operations
** that occur while the cursor is on the null row will always
** write a NULL.
**
** If cursor P1 is not previously opened, open it now to a special
** pseudo-cursor that always returns NULL for every column.
*/
case OP_NullRow: {
  vdbe_cursor_t *p_c;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  if( p_c==0 ){
    /* If the cursor is not already open, create a special kind of
    ** pseudo-cursor that always gives null rows. */
    p_c = __allocate_cursor(p, p_op->p1, 1, CURTYPE_PSEUDO);
    if( p_c==0 ) goto no_mem;
    p_c->seek_result = 0;
    p_c->is_table = 1;
    p_c->no_reuse = 1;
    p_c->uc.cursor = __sm_fake_valid_cursor();
  }
  p_c->null_row = 1;
  p_c->cache_status = CACHE_STALE;
  if( p_c->cur_type==CURTYPE_BTREE ){
    assert( p_c->uc.cursor!=0 );
    __sm_clear_cursor(p_c->uc.cursor);
  }
#ifdef DBSQL_DEBUG
  if( p_c->seek_op==0 ) p_c->seek_op = OP_NullRow;
#endif
  break;
}

/* Opcode: __seek_end P1 * * * *
**
** Position cursor P1 at the end of the btree for the purpose of
** appending a new entry onto the btree.
**
** It is assumed that the cursor is used only for appending and so
** if the cursor is valid, then the cursor must already be pointing
** at the end of the btree and so no changes are made to
** the cursor.
*/
/* Opcode: Last P1 P2 * * *
**
** The next use of the Rowid or column_t or Prev instruction for P1
** will refer to the last entry in the database table or index.
** If the table or index is empty and P2>0, then jump immediately to P2.
** If P2 is 0 or if the table or index is not empty, fall through
** to the following instruction.
**
** This opcode leaves the cursor configured to move in reverse order,
** from the end toward the beginning.  In other words, the cursor is
** configured to use Prev, not Next.
*/
case OP_SeekEnd:             /* ncycle */
case OP_Last: {              /* jump0, ncycle */
  vdbe_cursor_t *p_c;
  sm_cursor_t *crsr;
  int res;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  crsr = p_c->uc.cursor;
  res = 0;
  assert( crsr!=0 );
#ifdef DBSQL_DEBUG
  p_c->seek_op = p_op->opcode;
#endif
  if( p_op->opcode==OP_SeekEnd ){
    assert( p_op->p2==0 );
    p_c->seek_result = -1;
    if( __sm_cursor_is_valid_nn(crsr) ){
      break;
    }
  }
  rc = __sm_last(crsr, &res);
  p_c->null_row = (u_int8_t)res;
  p_c->deferred_moveto = 0;
  p_c->cache_status = CACHE_STALE;
  if( rc ) goto abort_due_to_error;
  if( p_op->p2>0 ){
    VDBE_BRANCH_TAKEN(res!=0,2);
    if( res ) goto jump_to_p2;
  }
  break;
}

/* Opcode: __if_size_between P1 P2 P3 P4 *
**
** Let N be the approximate number of rows in the table or index
** with cursor P1 and let X be 10*log2(N) if N is positive or -1
** if N is zero.
**
** Jump to P2 if X is in between P3 and P4, inclusive.
*/
case OP_IfSizeBetween: {        /* jump */
  vdbe_cursor_t *p_c;
  sm_cursor_t *crsr;
  int res;
  int64_t sz;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( p_op->p4type==P4_INT32 );
  assert( p_op->p3>=-1 && p_op->p3<=640*2 );
  assert( p_op->p4.i>=-1 && p_op->p4.i<=640*2 );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  crsr = p_c->uc.cursor;
  assert( crsr );
  rc = __sm_first(crsr, &res);
  if( rc ) goto abort_due_to_error;
  if( res!=0 ){
    sz = -1;  /* -Infinity encoding */
  }else{
    sz = __sm_row_count_est(crsr);
    assert( sz>0 );
    sz = __log_est((u_int64_t)sz);
  }
  res = sz>=p_op->p3 && sz<=p_op->p4.i;
  VDBE_BRANCH_TAKEN(res!=0,2);
  if( res ) goto jump_to_p2;
  break;
}


/* Opcode: __sorter_sort P1 P2 * * *
**
** After all records have been inserted into the Sorter object
** identified by P1, invoke this opcode to actually do the sorting.
** Jump to P2 if there are no records to be sorted.
**
** This opcode is an alias for OP_Sort and OP_Rewind that is used
** for Sorter objects.
*/
/* Opcode: Sort P1 P2 * * *
**
** This opcode does exactly the same thing as OP_Rewind except that
** it increments an undocumented global variable used for testing.
**
** Sorting is accomplished by writing records into a sorting index,
** then rewinding that index and playing it back from beginning to
** end.  We use the OP_Sort opcode instead of OP_Rewind to do the
** rewinding so that the global variable will be incremented and
** regression tests can determine whether or not the optimizer is
** correctly optimizing out sorts.
*/
case OP_SorterSort:    /* jump ncycle */
case OP_Sort: {        /* jump ncycle */
#ifdef DBSQL_TEST
  dbsql_sort_count++;
  dbsql_search_count--;
#endif
  p->counters[DBSQL_STMTSTATUS_SORT]++;
  /* Fall through into OP_Rewind */
  /* no break */ deliberate_fall_through
}
/* Opcode: Rewind P1 P2 * * *
**
** The next use of the Rowid or column_t or Next instruction for P1
** will refer to the first entry in the database table or index.
** If the table or index is empty, jump immediately to P2.
** If the table or index is not empty, fall through to the following
** instruction.
**
** If P2 is zero, that is an assertion that the P1 table is never
** empty and hence the jump will never be taken.
**
** This opcode leaves the cursor configured to move in forward order,
** from the beginning toward the end.  In other words, the cursor is
** configured to use Next, not Prev.
*/
case OP_Rewind: {        /* jump0, ncycle */
  vdbe_cursor_t *p_c;
  sm_cursor_t *crsr;
  int res;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( p_op->p5==0 );
  assert( p_op->p2>=0 && p_op->p2<p->n_op );

  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( IS_SORTER(p_c)==(p_op->opcode==OP_SorterSort) );
  res = 1;
#ifdef DBSQL_DEBUG
  p_c->seek_op = OP_Rewind;
#endif
  if( IS_SORTER(p_c) ){
    rc = __vdbe_sorter_rewind(p_c, &res);
  }else{
    assert( p_c->cur_type==CURTYPE_BTREE );
    crsr = p_c->uc.cursor;
    assert( crsr );
    rc = __sm_first(crsr, &res);
    p_c->deferred_moveto = 0;
    p_c->cache_status = CACHE_STALE;
  }
  if( rc ) goto abort_due_to_error;
  p_c->null_row = (u_int8_t)res;
  if( p_op->p2>0 ){
    VDBE_BRANCH_TAKEN(res!=0,2);
    if( res ) goto jump_to_p2;
  }
  break;
}

/* Opcode: __if_empty P1 P2 * * *
** Synopsis: if( empty(P1) ) goto P2
**
** Check to see if the b-tree table that cursor P1 references is empty
** and jump to P2 if it is.
*/
case OP_IfEmpty: {        /* jump */
  vdbe_cursor_t *p_c;
  sm_cursor_t *crsr;
  int res;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( p_op->p2>=0 && p_op->p2<p->n_op );

  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  crsr = p_c->uc.cursor;
  assert( crsr );
  rc = __sm_is_empty(crsr, &res);
  if( rc ) goto abort_due_to_error;
  VDBE_BRANCH_TAKEN(res!=0,2);
  if( res ) goto jump_to_p2;
  break;
}

/* Opcode: Next P1 P2 P3 * P5
**
** Advance cursor P1 so that it points to the next key/data pair in its
** table or index.  If there are no more key/value pairs then fall through
** to the following instruction.  But if the cursor advance was successful,
** jump immediately to P2.
**
** The Next opcode is only valid following an __seek_gt, __seek_ge, or
** OP_Rewind opcode used to position the cursor.  Next is not allowed
** to follow __seek_lt, __seek_le, or OP_Last.
**
** The P1 cursor must be for a real table, not a pseudo-table.  P1 must have
** been opened prior to this opcode or the program will segfault.
**
** The P3 value is a hint to the btree implementation. If P3==1, that
** means P1 is an SQL index and that this instruction could have been
** omitted if that index had been unique.  P3 is usually 0.  P3 is
** always either 0 or 1.
**
** If P5 is positive and the jump is taken, then event counter
** number P5-1 in the prepared statement is incremented.
**
** See also: Prev
*/
/* Opcode: Prev P1 P2 P3 * P5
**
** Back up cursor P1 so that it points to the previous key/data pair in its
** table or index.  If there is no previous key/value pairs then fall through
** to the following instruction.  But if the cursor backup was successful,
** jump immediately to P2.
**
**
** The Prev opcode is only valid following an __seek_lt, __seek_le, or
** OP_Last opcode used to position the cursor.  Prev is not allowed
** to follow __seek_gt, __seek_ge, or OP_Rewind.
**
** The P1 cursor must be for a real table, not a pseudo-table.  If P1 is
** not open then the behavior is undefined.
**
** The P3 value is a hint to the btree implementation. If P3==1, that
** means P1 is an SQL index and that this instruction could have been
** omitted if that index had been unique.  P3 is usually 0.  P3 is
** always either 0 or 1.
**
** If P5 is positive and the jump is taken, then event counter
** number P5-1 in the prepared statement is incremented.
*/
/* Opcode: __sorter_next P1 P2 * * P5
**
** This opcode works just like OP_Next except that P1 must be a
** sorter object for which the OP_SorterSort opcode has been
** invoked.  This opcode advances the cursor to the next sorted
** record, or jumps to P2 if there are no more sorted records.
*/
case OP_SorterNext: {  /* jump */
  vdbe_cursor_t *p_c;

  p_c = p->csrs[p_op->p1];
  assert( IS_SORTER(p_c) );
  rc = __vdbe_sorter_next(db, p_c);
  goto next_tail;

case OP_Prev:          /* jump, ncycle */
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( p_op->p5==0
       || p_op->p5==DBSQL_STMTSTATUS_FULLSCAN_STEP
       || p_op->p5==DBSQL_STMTSTATUS_AUTOINDEX);
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->deferred_moveto==0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( p_c->seek_op==OP_SeekLT || p_c->seek_op==OP_SeekLE
       || p_c->seek_op==OP_Last   || p_c->seek_op==OP_IfNoHope
       || p_c->seek_op==OP_NullRow);
  rc = __sm_previous(p_c->uc.cursor, p_op->p3);
  goto next_tail;

case OP_Next:          /* jump, ncycle */
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( p_op->p5==0
       || p_op->p5==DBSQL_STMTSTATUS_FULLSCAN_STEP
       || p_op->p5==DBSQL_STMTSTATUS_AUTOINDEX);
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->deferred_moveto==0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( p_c->seek_op==OP_SeekGT || p_c->seek_op==OP_SeekGE
       || p_c->seek_op==OP_Rewind || p_c->seek_op==OP_Found
       || p_c->seek_op==OP_NullRow|| p_c->seek_op==OP_SeekRowid
       || p_c->seek_op==OP_IfNoHope);
  rc = __sm_next(p_c->uc.cursor, p_op->p3);

next_tail:
  p_c->cache_status = CACHE_STALE;
  VDBE_BRANCH_TAKEN(rc==DBSQL_OK,2);
  if( rc==DBSQL_OK ){
    p_c->null_row = 0;
    p->counters[p_op->p5]++;
#ifdef DBSQL_TEST
    dbsql_search_count++;
#endif
    goto jump_to_p2_and_check_for_interrupt;
  }
  if( rc!=DBSQL_DONE ) goto abort_due_to_error;
  rc = DBSQL_OK;
  p_c->null_row = 1;
  goto check_for_interrupt;
}

/* Opcode: __idx_insert P1 P2 P3 P4 P5
** Synopsis: key=r[P2]
**
** Register P2 holds an SQL index key made using the
** __make_record instructions.  This opcode writes that key
** into the index P1.  Data for the entry is nil.
**
** If P4 is not zero, then it is the number of values in the unpacked
** key of reg(P2).  In that case, P3 is the index of the first register
** for the unpacked key.  The availability of the unpacked key can sometimes
** be an optimization.
**
** If P5 has the OPFLAG_APPEND bit set, that is a hint to the b-tree layer
** that this insert is likely to be an append.
**
** If P5 has the OPFLAG_NCHANGE bit set, then the change counter is
** incremented by this instruction.  If the OPFLAG_NCHANGE bit is clear,
** then the change counter is unchanged.
**
** If the OPFLAG_USESEEKRESULT flag of P5 is set, the implementation might
** run faster by avoiding an unnecessary seek on cursor P1.  However,
** the OPFLAG_USESEEKRESULT flag must only be set if there have been no prior
** seeks on the cursor or if the most recent seek used a key equivalent
** to P2.
**
** This instruction only works for indices.  The equivalent instruction
** for tables is OP_Insert.
*/
case OP_IdxInsert: {        /* in2 */
  vdbe_cursor_t *p_c;
  sm_payload_t x;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  __vdbe_incr_write_counter(p, p_c);
  assert( p_c!=0 );
  assert( !IS_SORTER(p_c) );
  in2 = &mems[p_op->p2];
  assert( (in2->flags & MEM_Blob) || (p_op->p5 & OPFLAG_PREFORMAT) );
  if( p_op->p5 & OPFLAG_NCHANGE ) p->n_change++;
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( p_c->is_table==0 );
  rc = EXPAND_BLOB(in2);
  if( rc ) goto abort_due_to_error;
  x.n_key = in2->n;
  x.key = in2->z;
  x.mems = mems + p_op->p3;
  x.n_mem = (u_int16_t)p_op->p4.i;
  rc = __sm_insert(p_c->uc.cursor, &x,
       (p_op->p5 & (OPFLAG_APPEND|OPFLAG_SAVEPOSITION|OPFLAG_PREFORMAT)),
      ((p_op->p5 & OPFLAG_USESEEKRESULT) ? p_c->seek_result : 0)
      );
  assert( p_c->deferred_moveto==0 );
  p_c->cache_status = CACHE_STALE;
  if( rc) goto abort_due_to_error;
  break;
}

/* Opcode: __sorter_insert P1 P2 * * *
** Synopsis: key=r[P2]
**
** Register P2 holds an SQL index key made using the
** __make_record instructions.  This opcode writes that key
** into the sorter P1.  Data for the entry is nil.
*/
case OP_SorterInsert: {     /* in2 */
  vdbe_cursor_t *p_c;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  __vdbe_incr_write_counter(p, p_c);
  assert( p_c!=0 );
  assert( IS_SORTER(p_c) );
  in2 = &mems[p_op->p2];
  assert( in2->flags & MEM_Blob );
  assert( p_c->is_table==0 );
  rc = EXPAND_BLOB(in2);
  if( rc ) goto abort_due_to_error;
  rc = __vdbe_sorter_write(p_c, in2);
  if( rc) goto abort_due_to_error;
  break;
}

/* Opcode: __idx_delete P1 P2 P3 P4 *
** Synopsis: key=r[P2@P3]
**
** The content of P3 registers starting at register P2 form
** an unpacked index key. This opcode removes that entry from the
** index opened by cursor P1.
**
** P4 is a pointer to an index_t structure.
**
** Raise an DBSQL_CORRUPT_INDEX error if no matching index entry is found
** and not in writable_schema mode.
*/
case OP_IdxDelete: {
  vdbe_cursor_t *p_c;
  sm_cursor_t *crsr;
  int res;
  unpacked_record_t r;

  assert( p_op->p3>0 );
  assert( p_op->p2>0 && p_op->p2+p_op->p3<=(p->n_mem+1 - p->n_cursor)+1 );
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  __vdbe_incr_write_counter(p, p_c);
  crsr = p_c->uc.cursor;
  assert( crsr!=0 );
  r.key_info = p_c->key_info;
  r.n_field = (u_int16_t)p_op->p3;
  r.default_rc = 0;
  r.mems = &mems[p_op->p2];
  rc = __sm_index_moveto(crsr, &r, &res);
  if( rc ) goto abort_due_to_error;
  if( res!=0 ){
    rc = __vdbe_find_index_key(crsr, p_op->p4.idx, &r, &res, 0);
    if( rc!=DBSQL_OK ) goto abort_due_to_error;
    if( res!=0 ){
      if( !__writable_schema(db) ){
        rc = __report_error(
            DBSQL_CORRUPT_INDEX, __LINE__, "index corruption");
        goto abort_due_to_error;
      }
      p_c->cache_status = CACHE_STALE;
      p_c->seek_result = 0;
      break;
    }
  }
  rc = __sm_delete(crsr, BTREE_AUXDELETE);
  if( rc ) goto abort_due_to_error;
  assert( p_c->deferred_moveto==0 );
  p_c->cache_status = CACHE_STALE;
  p_c->seek_result = 0;
  break;
}

/* Opcode: __deferred_seek P1 * P3 P4 *
** Synopsis: Move P3 to P1.rowid if needed
**
** P1 is an open index cursor and P3 is a cursor on the corresponding
** table.  This opcode does a deferred seek of the P3 table cursor
** to the row that corresponds to the current row of P1.
**
** This is a deferred seek.  Nothing actually happens until
** the cursor is used to read a record.  That way, if no reads
** occur, no unnecessary I/O happens.
**
** P4 may be an array of integers (type P4_INTARRAY) containing
** one entry for each column in the P3 table.  If array entry a(i)
** is non-zero, then reading column a(i)-1 from cursor P3 is
** equivalent to performing the deferred seek and then reading column i
** from P1.  This information is stored in P3 and used to redirect
** reads against P3 over to P1, thus possibly avoiding the need to
** seek and read cursor P3.
*/
/* Opcode: __idx_rowid P1 P2 * * *
** Synopsis: r[P2]=rowid
**
** Write into register P2 an integer which is the last entry in the record at
** the end of the index key pointed to by cursor P1.  This integer should be
** the rowid of the table entry to which this index entry points.
**
** See also: Rowid, __make_record.
*/
case OP_DeferredSeek:         /* ncycle */
case OP_IdxRowid: {           /* out2, ncycle */
  vdbe_cursor_t *p_c;             /* The P1 index cursor */
  vdbe_cursor_t *tab_cur;        /* The P2 table cursor (OP_DeferredSeek only) */
  int64_t rowid;                  /* Rowid that P1 current points to */

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE || IS_NULL_CURSOR(p_c) );
  assert( p_c->uc.cursor!=0 );
  assert( p_c->is_table==0 || IS_NULL_CURSOR(p_c) );
  assert( p_c->deferred_moveto==0 );
  assert( !p_c->null_row || p_op->opcode==OP_IdxRowid );

  /* The __idx_rowid and Seek opcodes are combined because of the commonality
  ** of __vdbe_cursor_restore() and __vdbe_idx_rowid(). */
  rc = __vdbe_cursor_restore(p_c);

  /* __vdbe_cursor_restore() may fail if the cursor has been disturbed
  ** since it was last positioned and an error (e.g. OOM or an IO error)
  ** occurs while trying to reposition it. */
  if( rc!=DBSQL_OK ) goto abort_due_to_error;

  if( !p_c->null_row ){
    rowid = 0;  /* Not needed.  Only used to silence a warning. */
    rc = __vdbe_idx_rowid(db, p_c->uc.cursor, &rowid);
    if( rc!=DBSQL_OK ){
      goto abort_due_to_error;
    }
    if( p_op->opcode==OP_DeferredSeek ){
      assert( p_op->p3>=0 && p_op->p3<p->n_cursor );
      tab_cur = p->csrs[p_op->p3];
      assert( tab_cur!=0 );
      assert( tab_cur->cur_type==CURTYPE_BTREE );
      assert( tab_cur->uc.cursor!=0 );
      assert( tab_cur->is_table );
      tab_cur->null_row = 0;
      tab_cur->moveto_target = rowid;
      tab_cur->deferred_moveto = 1;
      tab_cur->cache_status = CACHE_STALE;
      assert( p_op->p4type==P4_INTARRAY || p_op->p4.ai==0 );
      assert( !tab_cur->is_ephemeral );
      tab_cur->ub.alt_maps = p_op->p4.ai;
      assert( !p_c->is_ephemeral );
      tab_cur->alt_cursor = p_c;
    }else{
      out = out2Prerelease(p, p_op);
      out->u.i = rowid;
    }
  }else{
    assert( p_op->opcode==OP_IdxRowid );
    __vdbe_mem_set_null(&mems[p_op->p2]);
  }
  break;
}

/* Opcode: __finish_seek P1 * * * *
**
** If cursor P1 was previously moved via OP_DeferredSeek, complete that
** seek operation now, without further delay.  If the cursor seek has
** already occurred, this instruction is a no-op.
*/
case OP_FinishSeek: {        /* ncycle */
  vdbe_cursor_t *p_c;            /* The P1 index cursor */

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  if( p_c->deferred_moveto ){
    rc = __vdbe_finish_moveto(p_c);
    if( rc ) goto abort_due_to_error;
  }
  break;
}

/* Opcode: __idx_ge P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** The P4 register values beginning with P3 form an unpacked index
** key that omits the PRIMARY KEY.  Compare this key value against the index
** that P1 is currently pointing to, ignoring the PRIMARY KEY or ROWID
** fields at the end.
**
** If the P1 index entry is greater than or equal to the key value
** then jump to P2.  Otherwise fall through to the next instruction.
*/
/* Opcode: __idx_gt P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** The P4 register values beginning with P3 form an unpacked index
** key that omits the PRIMARY KEY.  Compare this key value against the index
** that P1 is currently pointing to, ignoring the PRIMARY KEY or ROWID
** fields at the end.
**
** If the P1 index entry is greater than the key value
** then jump to P2.  Otherwise fall through to the next instruction.
*/
/* Opcode: __idx_lt P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** The P4 register values beginning with P3 form an unpacked index
** key that omits the PRIMARY KEY or ROWID.  Compare this key value against
** the index that P1 is currently pointing to, ignoring the PRIMARY KEY or
** ROWID on the P1 index.
**
** If the P1 index entry is less than the key value then jump to P2.
** Otherwise fall through to the next instruction.
*/
/* Opcode: __idx_le P1 P2 P3 P4 *
** Synopsis: key=r[P3@P4]
**
** The P4 register values beginning with P3 form an unpacked index
** key that omits the PRIMARY KEY or ROWID.  Compare this key value against
** the index that P1 is currently pointing to, ignoring the PRIMARY KEY or
** ROWID on the P1 index.
**
** If the P1 index entry is less than or equal to the key value then jump
** to P2. Otherwise fall through to the next instruction.
*/
case OP_IdxLE:          /* jump, ncycle */
case OP_IdxGT:          /* jump, ncycle */
case OP_IdxLT:          /* jump, ncycle */
case OP_IdxGE:  {       /* jump, ncycle */
  vdbe_cursor_t *p_c;
  int res;
  unpacked_record_t r;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->is_ordered );
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( p_c->uc.cursor!=0);
  assert( p_c->deferred_moveto==0 );
  assert( p_op->p4type==P4_INT32 );
  r.key_info = p_c->key_info;
  r.n_field = (u_int16_t)p_op->p4.i;
  if( p_op->opcode<OP_IdxLT ){
    assert( p_op->opcode==OP_IdxLE || p_op->opcode==OP_IdxGT );
    r.default_rc = -1;
  }else{
    assert( p_op->opcode==OP_IdxGE || p_op->opcode==OP_IdxLT );
    r.default_rc = 0;
  }
  r.mems = &mems[p_op->p3];
#ifdef DBSQL_DEBUG
  {
    int i;
    for(i=0; i<r.n_field; i++){
      assert( MEM_IS_VALID(&r.mems[i]) );
      REGISTER_TRACE(p_op->p3+i, &mems[p_op->p3+i]);
    }
  }
#endif

  /* Inlined version of __vdbe_idx_key_compare() */
  {
    int64_t n_cell_key = 0;
    sm_cursor_t *cur;
    mem_t m;

    assert( p_c->cur_type==CURTYPE_BTREE );
    cur = p_c->uc.cursor;
    assert( __sm_cursor_is_valid(cur) );
    n_cell_key = __sm_payload_size(cur);
    /* n_cell_key will always be between 0 and 0xffffffff because of the way
    ** that btree_parse_cell_ptr() and __get_varint32() are implemented */
    if( n_cell_key<=0 || n_cell_key>0x7fffffff ){
      rc = DBSQL_CORRUPT_BKPT;
      goto abort_due_to_error;
    }
    __vdbe_mem_init(&m, db, 0);
    rc = __vdbe_mem_from_btree_zero_offset(cur, (u_int32_t)n_cell_key, &m);
    if( rc ) goto abort_due_to_error;
    res = __vdbe_record_compare_with_skip(m.n, m.z, &r, 0);
    __vdbe_mem_release_malloc(&m);
  }
  /* End of inlined __vdbe_idx_key_compare() */

  assert( (OP_IdxLE&1)==(OP_IdxLT&1) && (OP_IdxGE&1)==(OP_IdxGT&1) );
  if( (p_op->opcode&1)==(OP_IdxLT&1) ){
    assert( p_op->opcode==OP_IdxLE || p_op->opcode==OP_IdxLT );
    res = -res;
  }else{
    assert( p_op->opcode==OP_IdxGE || p_op->opcode==OP_IdxGT );
    res++;
  }
  VDBE_BRANCH_TAKEN(res>0,2);
  assert( rc==DBSQL_OK );
  if( res>0 ) goto jump_to_p2;
  break;
}

/* Opcode: Destroy P1 P2 P3 * *
**
** Delete an entire database table or index whose root page in the database
** file is given by P1.
**
** The table being destroyed is in the main database file if P3==0.  If
** P3==1 then the table to be destroyed is in the auxiliary database file
** that is used to store tables create using CREATE TEMPORARY TABLE.
**
** If AUTOVACUUM is enabled then it is possible that another root page
** might be moved into the newly deleted root page in order to keep all
** root pages contiguous at the beginning of the database.  The former
** value of the root page that moved - its value before the move occurred -
** is stored in register P2. If no page movement was required (because the
** table being dropped was already the last one in the database) then a
** zero is stored in register P2.  If AUTOVACUUM is disabled then a zero
** is stored in register P2.
**
** This opcode throws an error if there are any active reader VMs when
** it is invoked. This is done to avoid the difficulty associated with
** updating existing cursors when a root page is moved in an AUTOVACUUM
** database. This error is thrown even if the database is not an AUTOVACUUM
** db in order to avoid introducing an incompatibility between autovacuum
** and non-autovacuum modes.
**
** See also: Clear
*/
case OP_Destroy: {     /* out2 */
  int i_moved;
  int i_db;

  __vdbe_incr_write_counter(p, 0);
  assert( p->readOnly==0 );
  assert( p_op->p1>1 );
  out = out2Prerelease(p, p_op);
  out->flags = MEM_Null;
  if( db->n_vdbe_read > db->n_v_destroy+1 ){
    rc = DBSQL_LOCKED;
    p->error_action = OE_Abort;
    goto abort_due_to_error;
  }else{
    i_db = p_op->p3;
    assert( DB_MASK_TEST(p->btree_mask, i_db) );
    i_moved = 0;  /* Not needed.  Only to silence a warning. */
    rc = __sm_drop_table(db->dbs[i_db].bt, p_op->p1, &i_moved);
    out->flags = MEM_Int;
    out->u.i = i_moved;
    if( rc ) goto abort_due_to_error;
#ifndef DBSQL_OMIT_AUTOVACUUM
    if( i_moved!=0 ){
      __root_page_moved(db, i_db, i_moved, p_op->p1);
      /* All OP_Destroy operations occur on the same btree */
      assert( reset_schema_on_fault==0 || reset_schema_on_fault==i_db+1 );
      reset_schema_on_fault = i_db+1;
    }
#endif
  }
  break;
}

/* Opcode: Clear P1 P2 P3
**
** Delete all contents of the database table or index whose root page
** in the database file is given by P1.  But, unlike Destroy, do not
** remove the table or index from the database file.
**
** The table being cleared is in the main database file if P2==0.  If
** P2==1 then the table to be cleared is in the auxiliary database file
** that is used to store tables create using CREATE TEMPORARY TABLE.
**
** If the P3 value is non-zero, then the row change count is incremented
** by the number of rows in the table being cleared. If P3 is greater
** than zero, then the value stored in register P3 is also incremented
** by the number of rows in the table being cleared.
**
** See also: Destroy
*/
case OP_Clear: {
  int64_t n_change;

  __vdbe_incr_write_counter(p, 0);
  n_change = 0;
  assert( p->readOnly==0 );
  assert( DB_MASK_TEST(p->btree_mask, p_op->p2) );
  rc = __sm_clear_table(db->dbs[p_op->p2].bt, (u_int32_t)p_op->p1, &n_change);
  if( p_op->p3 ){
    p->n_change += n_change;
    if( p_op->p3>0 ){
      assert( MEM_IS_VALID(&mems[p_op->p3]) );
      MEM_ABOUT_TO_CHANGE(p, &mems[p_op->p3]);
      mems[p_op->p3].u.i += n_change;
    }
  }
  if( rc ) goto abort_due_to_error;
  break;
}

/* Opcode: __reset_sorter P1 * * * *
**
** Delete all contents from the ephemeral table or sorter
** that is open on cursor P1.
**
** This opcode only works for cursors used for sorting and
** opened with OP_OpenEphemeral or OP_SorterOpen.
*/
case OP_ResetSorter: {
  vdbe_cursor_t *p_c;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  if( IS_SORTER(p_c) ){
    __vdbe_sorter_reset(db, p_c->uc.sorter);
  }else{
    assert( p_c->cur_type==CURTYPE_BTREE );
    assert( p_c->is_ephemeral );
    rc = __sm_clear_table_of_cursor(p_c->uc.cursor);
    if( rc ) goto abort_due_to_error;
  }
  break;
}

/* Opcode: __create_btree P1 P2 P3 * *
** Synopsis: r[P2]=root i_db=P1 flags=P3
**
** Allocate a new b-tree in the main database file if P1==0 or in the
** TEMP database file if P1==1 or in an attached database if
** P1>1.  The P3 argument must be 1 (BTREE_INTKEY) for a rowid table
** it must be 2 (BTREE_BLOBKEY) for an index or WITHOUT ROWID table.
** The root page number of the new b-tree is stored in register P2.
*/
case OP_CreateBtree: {          /* out2 */
  pgno_t pgno;
  db_t *p_db;

  __vdbe_incr_write_counter(p, 0);
  out = out2Prerelease(p, p_op);
  pgno = 0;
  assert( p_op->p3==BTREE_INTKEY || p_op->p3==BTREE_BLOBKEY );
  assert( p_op->p1>=0 && p_op->p1<db->n_db );
  assert( DB_MASK_TEST(p->btree_mask, p_op->p1) );
  assert( p->readOnly==0 );
  p_db = &db->dbs[p_op->p1];
  assert( p_db->bt!=0 );
  rc = __sm_create_table(p_db->bt, &pgno, p_op->p3);
  if( rc ) goto abort_due_to_error;
  out->u.i = pgno;
  break;
}

/* Opcode: __sql_exec P1 P2 * P4 *
**
** Run the SQL statement or statements specified in the P4 string.
**
** The P1 parameter is a bitmask of options:
**
**    0x0001     Disable Auth and Trace callbacks while the statements
**               in P4 are running.
**
**    0x0002     Set db->n_analysis_limit to P2 while the statements in
**               P4 are running.
**
*/
case OP_SqlExec: {
  char *err;
#ifndef DBSQL_OMIT_AUTHORIZATION
  dbsql_xauth auth;
#endif
  u_int8_t m_trace;
  int saved_analysis_limit;

  __vdbe_incr_write_counter(p, 0);
  db->n_sql_exec++;
  err = 0;
#ifndef DBSQL_OMIT_AUTHORIZATION
  auth = db->auth;
#endif
  m_trace = db->m_trace;
  saved_analysis_limit = db->n_analysis_limit;
  if( p_op->p1 & 0x0001 ){
#ifndef DBSQL_OMIT_AUTHORIZATION
    db->auth = 0;
#endif
    db->m_trace = 0;
  }
  if( p_op->p1 & 0x0002 ){
    db->n_analysis_limit = p_op->p2;
  }
  rc = dbsql_exec(db, p_op->p4.z, 0, 0, &err);
  db->n_sql_exec--;
#ifndef DBSQL_OMIT_AUTHORIZATION
  db->auth = auth;
#endif
  db->m_trace = m_trace;
  db->n_analysis_limit = saved_analysis_limit;
  if( err || rc ){
    __vdbe_error(p, "%s", err);
    dbsql_free(err);
    if( rc==DBSQL_NOMEM ) goto no_mem;
    goto abort_due_to_error;
  }
  break;
}

/* Opcode: __parse_schema P1 * * P4 *
**
** Read and parse all entries from the schema table of database P1
** that match the WHERE clause P4.  If P4 is a NULL pointer, then the
** entire schema for P1 is reparsed.
**
** This opcode invokes the parser to create a new virtual machine,
** then runs the new virtual machine.  It is thus a re-entrant opcode.
*/
case OP_ParseSchema: {
  int i_db;
  const char *schema;
  char *sql;
  __init_data initData;

  /* Any prepared statement that invokes this opcode will hold mutexes
  ** on every btree.  This is a prerequisite for invoking
  ** __init_callback().
  */
#ifdef DBSQL_DEBUG
  for(i_db=0; i_db<db->n_db; i_db++){
    assert( i_db==1 || __sm_holds_mutex(db->dbs[i_db].bt) );
  }
#endif

  i_db = p_op->p1;
  assert( i_db>=0 && i_db<db->n_db );
  assert( DB_HAS_PROPERTY(db, i_db, DB_SchemaLoaded)
           || db->malloc_failed
           || (CORRUPT_DB && (db->flags & DBSQL_NoSchemaError)!=0) );

#ifndef DBSQL_OMIT_ALTERTABLE
  if( p_op->p4.z==0 ){
    __schema_clear(db->dbs[i_db].schema);
    db->m_db_flags &= ~DBFLAG_SchemaKnownOk;
    rc = __init_one(db, i_db, &p->errmsg, p_op->p5);
    db->m_db_flags |= DBFLAG_SchemaChange;
    p->expired = 0;
  }else
#endif
  {
    schema = LEGACY_SCHEMA_TABLE;
    initData.db = db;
    initData.i_db = i_db;
    initData.errmsg_p = &p->errmsg;
    initData.m_init_flags = 0;
    initData.mx_page = __sm_last_page(db->dbs[i_db].bt);
    sql = __m_printf(db,
       "SELECT*FROM\"%w\".%s WHERE %s ORDER BY rowid",
       db->dbs[i_db].db_s_name, schema, p_op->p4.z);
    if( sql==0 ){
      rc = DBSQL_NOMEM_BKPT;
    }else{
      assert( db->init.busy==0 );
      db->init.busy = 1;
      initData.rc = DBSQL_OK;
      initData.n_init_row = 0;
      assert( !db->malloc_failed );
      rc = dbsql_exec(db, sql, __init_callback, &initData, 0);
      if( rc==DBSQL_OK ) rc = initData.rc;
      if( rc==DBSQL_OK && initData.n_init_row==0 ){
        /* The OP_ParseSchema opcode with a non-NULL P4 argument should parse
        ** at least one SQL statement. Any less than that indicates that
        ** the dbsql_schema table is corrupt. */
        rc = DBSQL_CORRUPT_BKPT;
      }
      __db_free_nn(db, sql);
      db->init.busy = 0;
    }
  }
  if( rc ){
    __reset_all_schemas_of_connection(db);
    if( rc==DBSQL_NOMEM ){
      goto no_mem;
    }
    goto abort_due_to_error;
  }
  break; 
}

#if !defined(DBSQL_OMIT_ANALYZE)
/* Opcode: __load_analysis P1 * * * *
**
** Read the dbsql_stat1 table for database P1 and load the content
** of that table into the internal index hash table.  This will cause
** the analysis to be used when preparing all subsequent queries.
*/
case OP_LoadAnalysis: {
  assert( p_op->p1>=0 && p_op->p1<db->n_db );
  rc = __analysis_load(db, p_op->p1);
  if( rc ) goto abort_due_to_error;
  break; 
}
#endif /* !defined(DBSQL_OMIT_ANALYZE) */

/* Opcode: __drop_table P1 * * P4 *
**
** Remove the internal (in-memory) data structures that describe
** the table named P4 in database P1.  This is called after a table
** is dropped from disk (using the Destroy opcode) in order to keep
** the internal representation of the
** schema consistent with what is on disk.
*/
case OP_DropTable: {
  __vdbe_incr_write_counter(p, 0);
  __unlink_and_delete_table(db, p_op->p1, p_op->p4.z);
  break;
}

/* Opcode: __drop_index P1 * * P4 *
**
** Remove the internal (in-memory) data structures that describe
** the index named P4 in database P1.  This is called after an index
** is dropped from disk (using the Destroy opcode)
** in order to keep the internal representation of the
** schema consistent with what is on disk.
*/
case OP_DropIndex: {
  __vdbe_incr_write_counter(p, 0);
  __unlink_and_delete_index(db, p_op->p1, p_op->p4.z);
  break;
}

/* Opcode: __drop_trigger P1 * * P4 *
**
** Remove the internal (in-memory) data structures that describe
** the trigger named P4 in database P1.  This is called after a trigger
** is dropped from disk (using the Destroy opcode) in order to keep
** the internal representation of the
** schema consistent with what is on disk.
*/
case OP_DropTrigger: {
  __vdbe_incr_write_counter(p, 0);
  __unlink_and_delete_trigger(db, p_op->p1, p_op->p4.z);
  break;
}


#ifndef DBSQL_OMIT_INTEGRITY_CHECK
/* Opcode: __integrity_ck P1 P2 P3 P4 P5
**
** Do an analysis of the currently open database.  Store in
** register (P1+1) the text of an error message describing any problems.
** If no problems are found, store a NULL in register (P1+1).
**
** The register (P1) contains one less than the maximum number of allowed
** errors.  At most reg(P1) errors will be reported.
** In other words, the analysis stops as soon as reg(P1) errors are
** seen.  Reg(P1) is updated with the number of errors remaining.
**
** The root page numbers of all tables in the database are integers
** stored in P4_INTARRAY argument.
**
** If P5 is not zero, the check is done on the auxiliary database
** file, not the main database file.
**
** This opcode is used to implement the integrity_check pragma.
*/
case OP_IntegrityCk: {
  int n_root;      /* Number of tables to check.  (Number of root pages.) */
  pgno_t *roots;    /* Array of rootpage numbers for tables to be checked */
  int n_err;       /* Number of errors reported */
  char *z;        /* Text of the error report */
  mem_t *pn_err;     /* Register keeping track of errors remaining */

  assert( p->is_reader );
  assert( p_op->p4type==P4_INTARRAY );
  n_root = p_op->p2;
  roots = p_op->p4.ai;
  assert( n_root>0 );
  assert( roots!=0 );
  assert( roots[0]==(pgno_t)n_root );
  assert( p_op->p1>0 && (p_op->p1+1)<=(p->n_mem+1 - p->n_cursor) );
  pn_err = &mems[p_op->p1];
  assert( (pn_err->flags & MEM_Int)!=0 );
  assert( (pn_err->flags & (MEM_Str|MEM_Blob))==0 );
  in1 = &mems[p_op->p1+1];
  assert( p_op->p5<db->n_db );
  assert( DB_MASK_TEST(p->btree_mask, p_op->p5) );
  rc = __sm_integrity_check(db, db->dbs[p_op->p5].bt, &roots[1], 
      &mems[p_op->p3], n_root, (int)pn_err->u.i+1, &n_err, &z);
  __vdbe_mem_set_null(in1);
  if( n_err==0 ){
    assert( z==0 );
  }else if( rc ){
    dbsql_free(z);
    goto abort_due_to_error;
  }else{
    pn_err->u.i -= n_err-1;
    __vdbe_mem_set_str(in1, z, -1, DBSQL_UTF8, dbsql_free);
  }
  UPDATE_MAX_BLOBSIZE(in1);
  __vdbe_change_encoding(in1, encoding);
  goto check_for_interrupt;
}

/* Opcode: __i_find_key P1 P2 P3 P4 *
**
** This instruction always follows an OP_Found with the same P1, P2 and P3
** values as this instruction and a non-zero P4 value. The P4 value to
** this opcode is of type P4_INDEX and contains a pointer to the index_t
** object of for the index being searched.
**
** This opcode uses __vdbe_find_index_key() to search around the current
** cursor location for an index key that exactly matches all fields that
** are not indexed expressions or references to VIRTUAL generated columns,
** and either exactly match or are real numbers that are within 2 ULPs of
** each other if the don't match.
**
** To put it another way, this opcode looks for nearby index entries that
** are very close to the search key, but which might have small differences
** in floating-point values that come via an expression.
**
** If no nearby alternative entry is found in cursor P1, then jump to P2.
** But if a close match is found, fall through.
**
** This opcode is used by PRAGMA integrity_check to help distinguish
** between truely corrupt indexes and expression indexes that are holding
** floating-point values that are off by one or two ULPs.
*/
case OP_IFindKey: {     /* jump, in3 */
  vdbe_cursor_t *p_c;
  int res;
  unpacked_record_t r;

  assert( p_op[-1].opcode==OP_Found );
  assert( p_op[-1].p1==p_op->p1 );
  assert( p_op[-1].p3==p_op->p3 );
  p_c = p->csrs[p_op->p1];
  assert( p_op->p4type==P4_INDEX );
  assert( p_c->cur_type==CURTYPE_BTREE );
  assert( p_c->uc.cursor!=0 );
  assert( p_c->is_table==0 );

  memset(&r, 0, sizeof(r));
  r.mems = &mems[p_op->p3];
  r.n_field = p_op->p4.idx->n_column;
  r.key_info = p_c->key_info;

  rc = __vdbe_find_index_key(p_c->uc.cursor, p_op->p4.idx, &r, &res, 1);
  if( rc || res!=0 ){
    rc = DBSQL_OK;
    goto jump_to_p2;
  }
  p_c->null_row = 0;
  break;
};
#endif /* DBSQL_OMIT_INTEGRITY_CHECK */

/* Opcode: __row_set_add P1 P2 * * *
** Synopsis: rowset(P1)=r[P2]
**
** Insert the integer value held by register P2 into a row_set_t object
** held in register P1.
**
** An assertion fails if P2 is not an integer.
*/
case OP_RowSetAdd: {       /* in1, in2 */
  in1 = &mems[p_op->p1];
  in2 = &mems[p_op->p2];
  assert( (in2->flags & MEM_Int)!=0 );
  if( (in1->flags & MEM_Blob)==0 ){
    if( __vdbe_mem_set_row_set(in1) ) goto no_mem;
  }
  assert( __vdbe_mem_is_row_set(in1) );
  __row_set_insert((row_set_t*)in1->z, in2->u.i);
  break;
}

/* Opcode: __row_set_read P1 P2 P3 * *
** Synopsis: r[P3]=rowset(P1)
**
** Extract the smallest value from the row_set_t object in P1
** and put that value into register P3.
** Or, if row_set_t object P1 is initially empty, leave P3
** unchanged and jump to instruction P2.
*/
case OP_RowSetRead: {       /* jump, in1, out3 */
  int64_t val;

  in1 = &mems[p_op->p1];
  assert( (in1->flags & MEM_Blob)==0 || __vdbe_mem_is_row_set(in1) );
  if( (in1->flags & MEM_Blob)==0
   || __row_set_next((row_set_t*)in1->z, &val)==0
  ){
    /* The boolean index is empty */
    __vdbe_mem_set_null(in1);
    VDBE_BRANCH_TAKEN(1,2);
    goto jump_to_p2_and_check_for_interrupt;
  }else{
    /* A value was pulled from the index */
    VDBE_BRANCH_TAKEN(0,2);
    __vdbe_mem_set_int64(&mems[p_op->p3], val);
  }
  goto check_for_interrupt;
}

/* Opcode: __row_set_test P1 P2 P3 P4
** Synopsis: if r[P3] in rowset(P1) goto P2
**
** Register P3 is assumed to hold a 64-bit integer value. If register P1
** contains a row_set_t object and that row_set_t object contains
** the value held in P3, jump to register P2. Otherwise, insert the
** integer in P3 into the row_set_t and continue on to the
** next opcode.
**
** The row_set_t object is optimized for the case where sets of integers
** are inserted in distinct phases, which each set contains no duplicates.
** Each set is identified by a unique P4 value. The first set
** must have P4==0, the final set must have P4==-1, and for all other sets
** must have P4>0.
**
** This allows optimizations: (a) when P4==0 there is no need to test
** the row_set_t object for P3, as it is guaranteed not to contain it,
** (b) when P4==-1 there is no need to insert the value, as it will
** never be tested for, and (c) when a value that is part of set X is
** inserted, there is no need to search to see if the same value was
** previously inserted as part of set X (only if it was previously
** inserted as part of some other set).
*/
case OP_RowSetTest: {                     /* jump, in1, in3 */
  int i_set;
  int exists;

  in1 = &mems[p_op->p1];
  in3 = &mems[p_op->p3];
  i_set = p_op->p4.i;
  assert( in3->flags&MEM_Int );

  /* If there is anything other than a rowset object in memory cell P1,
  ** delete it now and initialize P1 with an empty rowset
  */
  if( (in1->flags & MEM_Blob)==0 ){
    if( __vdbe_mem_set_row_set(in1) ) goto no_mem;
  }
  assert( __vdbe_mem_is_row_set(in1) );
  assert( p_op->p4type==P4_INT32 );
  assert( i_set==-1 || i_set>=0 );
  if( i_set ){
    exists = __row_set_test((row_set_t*)in1->z, i_set, in3->u.i);
    VDBE_BRANCH_TAKEN(exists!=0,2);
    if( exists ) goto jump_to_p2;
  }
  if( i_set>=0 ){
    __row_set_insert((row_set_t*)in1->z, in3->u.i);
  }
  break;
}


#ifndef DBSQL_OMIT_TRIGGER

/* Opcode: Program P1 P2 P3 P4 P5
**
** Execute the trigger program passed as P4 (type P4_SUBPROGRAM).
**
** P1 contains the address of the memory cell that contains the first memory
** cell in an array of values used as arguments to the sub-program. P2
** contains the address to jump to if the sub-program throws an IGNORE
** exception using the RAISE() function. P2 might be zero, if there is
** no possibility that an IGNORE exception will be raised.
** Register P3 contains the address
** of a memory cell in this (the parent) VM that is used to allocate the
** memory required by the sub-vdbe at runtime.
**
** P4 is a pointer to the VM containing the trigger program.
**
** If P5 is non-zero, then recursive program invocation is enabled.
*/
case OP_Program: {        /* jump0 */
  int n_mem;               /* Number of memory registers for sub-program */
  int64_t n_byte;              /* Bytes of runtime space required for sub-program */
  mem_t *rt;               /* Register to allocate runtime space */
  mem_t *mem;              /* Used to iterate through memory cells */
  mem_t *end;              /* Last memory cell in new array */
  vdbe_frame_t *frame;      /* New vdbe frame to execute in */
  sub_program_t *program;   /* Sub-program to execute */
  void *t;                /* Token identifying trigger */

  program = p_op->p4.program;
  rt = &mems[p_op->p3];
  assert( program->n_op>0 );
 
  /* If the p5 flag is clear, then recursive invocation of triggers is
  ** disabled for backwards compatibility (p5 is set if this sub-program
  ** is really a trigger, not a foreign key action, and the flag set
  ** and cleared by the "PRAGMA recursive_triggers" command is clear).
  **
  ** It is recursive invocation of triggers, at the SQL level, that is
  ** disabled. In some cases a single trigger may generate more than one
  ** sub_program_t (if the trigger may be executed with more than one different
  ** ON CONFLICT algorithm). sub_program_t structures associated with a
  ** single trigger all have the same value for the sub_program_t.token
  ** variable.  */
  if( p_op->p5 ){
    t = program->token;
    for(frame=p->frame; frame && frame->token!=t; frame=frame->parent);
    if( frame ) break;
  }

  if( p->n_frame>=db->limits[DBSQL_LIMIT_TRIGGER_DEPTH] ){
    rc = DBSQL_ERROR;
    __vdbe_error(p, "too many levels of trigger recursion");
    goto abort_due_to_error;
  }

  /* Register rt is used to store the memory required to save the state
  ** of the current program, and the memory required at runtime to execute
  ** the trigger program. If this trigger has been fired before, then rt
  ** is already allocated. Otherwise, it must be initialized.  */
  if( (rt->flags&MEM_Blob)==0 ){
    /* sub_program_t.n_mem is set to the number of memory cells used by the
    ** program stored in sub_program_t.ops. As well as these, one memory
    ** cell is required for each cursor used by the program. Set local
    ** variable n_mem (and later, vdbe_frame_t.n_child_mem) to this value.
    */
    n_mem = program->n_mem + program->n_csr;
    assert( n_mem>0 );
    if( program->n_csr==0 ) n_mem++;
    n_byte = ROUND8(sizeof(vdbe_frame_t))
              + n_mem * sizeof(mem_t)
              + program->n_csr * sizeof(vdbe_cursor_t*)
              + (7 + (int64_t)program->n_op)/8;
    frame = __db_malloc_zero(db, n_byte);
    if( !frame ){
      goto no_mem;
    }
    __vdbe_mem_release(rt);
    rt->flags = MEM_Blob|MEM_Dyn;
    rt->z = (char*)frame;
    rt->n = (int)n_byte;
    rt->x_del = __vdbe_frame_mem_del;

    frame->v = p;
    frame->n_child_mem = n_mem;
    frame->n_child_csr = program->n_csr;
    frame->pc = (int)(p_op - ops);
    frame->mems = p->mems;
    frame->n_mem = p->n_mem;
    frame->csrs = p->csrs;
    frame->n_cursor = p->n_cursor;
    frame->ops = p->ops;
    frame->n_op = p->n_op;
    frame->token = program->token;
#ifdef DBSQL_DEBUG
    frame->i_frame_magic = DBSQL_FRAME_MAGIC;
#endif

    end = &VDBE_FRAME_MEM(frame)[frame->n_child_mem];
    for(mem=VDBE_FRAME_MEM(frame); mem!=end; mem++){
      mem->flags = MEM_Undefined;
      mem->db = db;
    }
  }else{
    frame = (vdbe_frame_t*)rt->z;
    assert( rt->x_del==__vdbe_frame_mem_del );
    assert( program->n_mem+program->n_csr==frame->n_child_mem
        || (program->n_csr==0 && program->n_mem+1==frame->n_child_mem) );
    assert( program->n_csr==frame->n_child_csr );
    assert( (int)(p_op - ops)==frame->pc );
  }

  p->n_frame++;
  frame->parent = p->frame;
  frame->last_rowid = db->last_rowid;
  frame->n_change = p->n_change;
  frame->n_db_change = p->db->n_change;
  assert( frame->aux_data==0 );
  frame->aux_data = p->aux_data;
  p->aux_data = 0;
  p->n_change = 0;
  p->frame = frame;
  p->mems = mems = VDBE_FRAME_MEM(frame);
  p->n_mem = frame->n_child_mem;
  p->n_cursor = (u_int16_t)frame->n_child_csr;
  p->csrs = (vdbe_cursor_t **)&mems[p->n_mem];
  frame->onces = (u_int8_t*)&p->csrs[program->n_csr];
  memset(frame->onces, 0, (program->n_op + 7)/8);
  p->ops = ops = program->ops;
  p->n_op = program->n_op;
#ifdef DBSQL_DEBUG
  /* Verify that second and subsequent executions of the same trigger do not
  ** try to reuse register values from the first use. */
  {
    int i;
    for(i=0; i<p->n_mem; i++){
      mems[i].scopy_from = 0;  /* Prevent false-positive __about_to_change() errs */
      MEM_SET_TYPE_FLAG(&mems[i], MEM_Undefined); /* Fault if this reg is reused */
    }
  }
#endif
  p_op = &ops[-1];
  goto check_for_interrupt;
}

/* Opcode: Param P1 P2 * * *
**
** This opcode is only ever present in sub-programs called via the
** OP_Program instruction. Copy a value currently stored in a memory
** cell of the calling (parent) frame to cell P2 in the current frames
** address space. This is used by trigger programs to access the new.*
** and old.* values.
**
** The address of the cell in the parent frame is determined by adding
** the value of the P1 argument to the value of the P1 argument to the
** calling OP_Program instruction.
*/
case OP_Param: {           /* out2 */
  vdbe_frame_t *frame;
  mem_t *in;
  out = out2Prerelease(p, p_op);
  frame = p->frame;
  in = &frame->mems[p_op->p1 + frame->ops[frame->pc].p1];  
  __vdbe_mem_shallow_copy(out, in, MEM_Ephem);
  break;
}

#endif /* #ifndef DBSQL_OMIT_TRIGGER */

#ifndef DBSQL_OMIT_FOREIGN_KEY
/* Opcode: __fk_counter P1 P2 * * *
** Synopsis: fkctr[P1]+=P2
**
** Increment a "constraint counter" by P2 (P2 may be negative or positive).
** If P1 is non-zero, the database constraint counter is incremented
** (deferred foreign key constraints). Otherwise, if P1 is zero, the
** statement counter is incremented (immediate foreign key constraints).
*/
case OP_FkCounter: {
  if( p_op->p1 ){
    db->n_deferred_cons += p_op->p2;
  }else{
    if( db->flags & DBSQL_DeferFKs ){
      db->n_deferred_imm_cons += p_op->p2;
    }else{
      p->n_fk_constraint += p_op->p2;
    }
  }
  break;
}

/* Opcode: __fk_if_zero P1 P2 * * *
** Synopsis: if fkctr[P1]==0 goto P2
**
** This opcode tests if a foreign key constraint-counter is currently zero.
** If so, jump to instruction P2. Otherwise, fall through to the next
** instruction.
**
** If P1 is non-zero, then the jump is taken if the database constraint-counter
** is zero (the one that counts deferred constraint violations). If P1 is
** zero, the jump is taken if the statement constraint-counter is zero
** (immediate foreign key constraint violations).
*/
case OP_FkIfZero: {         /* jump */
  if( p_op->p1 ){
    VDBE_BRANCH_TAKEN(db->n_deferred_cons==0 && db->n_deferred_imm_cons==0, 2);
    if( db->n_deferred_cons==0 && db->n_deferred_imm_cons==0 ) goto jump_to_p2;
  }else{
    VDBE_BRANCH_TAKEN(p->n_fk_constraint==0 && db->n_deferred_imm_cons==0, 2);
    if( p->n_fk_constraint==0 && db->n_deferred_imm_cons==0 ) goto jump_to_p2;
  }
  break;
}
#endif /* #ifndef DBSQL_OMIT_FOREIGN_KEY */

#ifndef DBSQL_OMIT_AUTOINCREMENT
/* Opcode: __mem_max P1 P2 * * *
** Synopsis: r[P1]=max(r[P1],r[P2])
**
** P1 is a register in the root frame of this VM (the root frame is
** different from the current frame if this instruction is being executed
** within a sub-program). Set the value of register P1 to the maximum of
** its current value and the value in register P2.
**
** This instruction throws an error if the memory cell is not initially
** an integer.
*/
case OP_MemMax: {        /* in2 */
  vdbe_frame_t *frame;
  if( p->frame ){
    for(frame=p->frame; frame->parent; frame=frame->parent);
    in1 = &frame->mems[p_op->p1];
  }else{
    in1 = &mems[p_op->p1];
  }
  assert( MEM_IS_VALID(in1) );
  __vdbe_mem_integerify(in1);
  in2 = &mems[p_op->p2];
  __vdbe_mem_integerify(in2);
  if( in1->u.i<in2->u.i){
    in1->u.i = in2->u.i;
  }
  break;
}
#endif /* DBSQL_OMIT_AUTOINCREMENT */

/* Opcode: __if_pos P1 P2 P3 * *
** Synopsis: if r[P1]>0 then r[P1]-=P3, goto P2
**
** Register P1 must contain an integer.
** If the value of register P1 is 1 or greater, subtract P3 from the
** value in P1 and jump to P2.
**
** If the initial value of register P1 is less than 1, then the
** value is unchanged and control passes through to the next instruction.
*/
case OP_IfPos: {        /* jump, in1 */
  in1 = &mems[p_op->p1];
  assert( in1->flags&MEM_Int );
  VDBE_BRANCH_TAKEN( in1->u.i>0, 2);
  if( in1->u.i>0 ){
    in1->u.i -= p_op->p3;
    goto jump_to_p2;
  }
  break;
}

/* Opcode: __offset_limit P1 P2 P3 * *
** Synopsis: if r[P1]>0 then r[P2]=r[P1]+max(0,r[P3]) else r[P2]=(-1)
**
** This opcode performs a commonly used computation associated with
** LIMIT and OFFSET processing.  r[P1] holds the limit counter.  r[P3]
** holds the offset counter.  The opcode computes the combined value
** of the LIMIT and OFFSET and stores that value in r[P2].  The r[P2]
** value computed is the total number of rows that will need to be
** visited in order to complete the query.
**
** If r[P3] is zero or negative, that means there is no OFFSET
** and r[P2] is set to be the value of the LIMIT, r[P1].
**
** if r[P1] is zero or negative, that means there is no LIMIT
** and r[P2] is set to -1.
**
** Otherwise, r[P2] is set to the sum of r[P1] and r[P3].
*/
case OP_OffsetLimit: {    /* in1, out2, in3 */
  int64_t x;
  in1 = &mems[p_op->p1];
  in3 = &mems[p_op->p3];
  out = out2Prerelease(p, p_op);
  assert( in1->flags & MEM_Int );
  assert( in3->flags & MEM_Int );
  x = in1->u.i;
  if( x<=0 || __add_int64(&x, in3->u.i>0?in3->u.i:0) ){
    /* If the LIMIT is less than or equal to zero, loop forever.  This
    ** is documented.  But also, if the LIMIT+OFFSET exceeds 2^63 then
    ** also loop forever.  This is undocumented.  In fact, one could argue
    ** that the loop should terminate.  But assuming 1 billion iterations
    ** per second (far exceeding the capabilities of any current hardware)
    ** it would take nearly 300 years to actually reach the limit.  So
    ** looping forever is a reasonable approximation. */
    out->u.i = -1;
  }else{
    out->u.i = x;
  }
  break;
}

/* Opcode: __if_not_zero P1 P2 * * *
** Synopsis: if r[P1]!=0 then r[P1]--, goto P2
**
** Register P1 must contain an integer.  If the content of register P1 is
** initially greater than zero, then decrement the value in register P1.
** If it is non-zero (negative or positive) and then also jump to P2. 
** If register P1 is initially zero, leave it unchanged and fall through.
*/
case OP_IfNotZero: {        /* jump, in1 */
  in1 = &mems[p_op->p1];
  assert( in1->flags&MEM_Int );
  VDBE_BRANCH_TAKEN(in1->u.i<0, 2);
  if( in1->u.i ){
     if( in1->u.i>0 ) in1->u.i--;
     goto jump_to_p2;
  }
  break;
}

/* Opcode: __decr_jump_zero P1 P2 * * *
** Synopsis: if (--r[P1])==0 goto P2
**
** Register P1 must hold an integer.  Decrement the value in P1
** and jump to P2 if the new value is exactly zero.
*/
case OP_DecrJumpZero: {      /* jump, in1 */
  in1 = &mems[p_op->p1];
  assert( in1->flags&MEM_Int );
  if( in1->u.i>SMALLEST_INT64 ) in1->u.i--;
  VDBE_BRANCH_TAKEN(in1->u.i==0, 2);
  if( in1->u.i==0 ) goto jump_to_p2;
  break;
}


/* Opcode: __agg_step * P2 P3 P4 P5
** Synopsis: accum=r[P3] step(r[P2@P5])
**
** Execute the step function for an aggregate.
** The function has P5 arguments.  P4 is a pointer to the
** func_def_t structure that specifies the function.  Register P3 is the
** accumulator.
**
** The P5 arguments are taken from register P2 and its
** successors.
*/
/* Opcode: __agg_inverse * P2 P3 P4 P5
** Synopsis: accum=r[P3] inverse(r[P2@P5])
**
** Execute the inverse function for an aggregate.
** The function has P5 arguments.  P4 is a pointer to the
** func_def_t structure that specifies the function.  Register P3 is the
** accumulator.
**
** The P5 arguments are taken from register P2 and its
** successors.
*/
/* Opcode: __agg_step1 P1 P2 P3 P4 P5
** Synopsis: accum=r[P3] step(r[P2@P5])
**
** Execute the step (if P1==0) or inverse (if P1!=0) function for an
** aggregate.  The function has P5 arguments.  P4 is a pointer to the
** func_def_t structure that specifies the function.  Register P3 is the
** accumulator.
**
** The P5 arguments are taken from register P2 and its
** successors.
**
** This opcode is initially coded as __op_agg_step0.  On first evaluation,
** the func_def_t stored in P4 is converted into an dbsql_func_t and
** the opcode is changed.  In this way, the initialization of the
** dbsql_func_t only happens once, instead of on each call to the
** step function.
*/
case OP_AggInverse:
case OP_AggStep: {
  int n;
  dbsql_func_t *ctx;
  u_int64_t n_alloc;

  assert( p_op->p4type==P4_FUNCDEF );
  n = p_op->p5;
  assert( p_op->p3>0 && p_op->p3<=(p->n_mem+1 - p->n_cursor) );
  assert( n==0 || (p_op->p2>0 && p_op->p2+n<=(p->n_mem+1 - p->n_cursor)+1) );
  assert( p_op->p3<p_op->p2 || p_op->p3>=p_op->p2+n );

  /* Allocate space for (a) the context object and (n-1) extra pointers
  ** to append to the dbsql_func_t.argv[1] array, and (b) a memory
  ** cell in which to store the accumulation. Be careful that the memory
  ** cell is 8-byte aligned, even on platforms where a pointer is 32-bits.
  **
  ** Note: We could avoid this by using a regular memory cell from mems[] for 
  ** the accumulator, instead of allocating one here. */
  n_alloc = ROUND8P( SZ_CONTEXT(n) );
  ctx = __db_malloc_raw_nn(db, n_alloc + sizeof(mem_t));
  if( ctx==0 ) goto no_mem;
  ctx->out = (mem_t*)((u_int8_t*)ctx + n_alloc);
  assert( EIGHT_BYTE_ALIGNMENT(ctx->out) );

  __vdbe_mem_init(ctx->out, db, MEM_Null);
  ctx->mem = 0;
  ctx->func = p_op->p4.func;
  ctx->i_op = (int)(p_op - ops);
  ctx->vdbe = p;
  ctx->skip_flag = 0;
  ctx->isError = 0;
  ctx->enc = encoding;
  ctx->argc = n;
  p_op->p4type = P4_FUNCCTX;
  p_op->p4.ctx = ctx;

  /* OP_AggInverse must have P1==1 and OP_AggStep must have P1==0 */
  assert( p_op->p1==(p_op->opcode==OP_AggInverse) );

  p_op->opcode = OP_AggStep1;
  /* Fall through into OP_AggStep */
  /* no break */ deliberate_fall_through
}
case OP_AggStep1: {
  int i;
  dbsql_func_t *ctx;
  mem_t *mem;

  assert( p_op->p4type==P4_FUNCCTX );
  ctx = p_op->p4.ctx;
  mem = &mems[p_op->p3];

#ifdef DBSQL_DEBUG
  if( p_op->p1 ){
    /* This is an OP_AggInverse call.  Verify that step has always
    ** been called at least once prior to any inverse call. */
    assert( mem->u_temp==0x1122e0e3 );
  }else{
    /* This is an OP_AggStep call.  Mark it as such. */
    mem->u_temp = 0x1122e0e3;
  }
#endif

  /* If this function is inside of a trigger, the register array in mems[]
  ** might change from one evaluation to the next.  The next block of code
  ** checks to see if the register array has changed, and if so it
  ** reinitializes the relevant parts of the dbsql_func_t object */
  if( ctx->mem != mem ){
    ctx->mem = mem;
    for(i=ctx->argc-1; i>=0; i--) ctx->argv[i] = &mems[p_op->p2+i];
  }

#ifdef DBSQL_DEBUG
  for(i=0; i<ctx->argc; i++){
    assert( MEM_IS_VALID(ctx->argv[i]) );
    REGISTER_TRACE(p_op->p2+i, ctx->argv[i]);
  }
#endif

  mem->n++;
  assert( ctx->out->flags==MEM_Null );
  assert( ctx->isError==0 );
  assert( ctx->skip_flag==0 );
#ifndef DBSQL_OMIT_WINDOWFUNC
  if( p_op->p1 ){
    (ctx->func->inverse)(ctx,ctx->argc,ctx->argv);
  }else
#endif
  (ctx->func->s_func)(ctx,ctx->argc,ctx->argv); /* IMP: R-24505-23230 */

  if( ctx->isError ){
    if( ctx->isError>0 ){
      __vdbe_error(p, "%s", dbsql_value_text(ctx->out));
      rc = ctx->isError;
    }
    if( ctx->skip_flag ){
      assert( p_op[-1].opcode==OP_CollSeq );
      i = p_op[-1].p1;
      if( i ) __vdbe_mem_set_int64(&mems[i], 1);
      ctx->skip_flag = 0;
    }
    __vdbe_mem_release(ctx->out);
    ctx->out->flags = MEM_Null;
    ctx->isError = 0;
    if( rc ) goto abort_due_to_error;
  }
  assert( ctx->out->flags==MEM_Null );
  assert( ctx->skip_flag==0 );
  break;
}

/* Opcode: __agg_final P1 P2 * P4 *
** Synopsis: accum=r[P1] N=P2
**
** P1 is the memory location that is the accumulator for an aggregate
** or window function.  Execute the finalizer function
** for an aggregate and store the result in P1.
**
** P2 is the number of arguments that the step function takes and
** P4 is a pointer to the func_def_t for this function.  The P2
** argument is not used by this opcode.  It is only there to disambiguate
** functions that can take varying numbers of arguments.  The
** P4 argument is only needed for the case where
** the step function was not previously called.
*/
/* Opcode: __agg_value * P2 P3 P4 *
** Synopsis: r[P3]=value N=P2
**
** Invoke the value() function and store the result in register P3.
**
** P2 is the number of arguments that the step function takes and
** P4 is a pointer to the func_def_t for this function.  The P2
** argument is not used by this opcode.  It is only there to disambiguate
** functions that can take varying numbers of arguments.  The
** P4 argument is only needed for the case where
** the step function was not previously called.
*/
case OP_AggValue:
case OP_AggFinal: {
  mem_t *mem;
  assert( p_op->p1>0 && p_op->p1<=(p->n_mem+1 - p->n_cursor) );
  assert( p_op->p3==0 || p_op->opcode==OP_AggValue );
  mem = &mems[p_op->p1];
  assert( (mem->flags & ~(MEM_Null|MEM_Agg))==0 );
#ifndef DBSQL_OMIT_WINDOWFUNC
  if( p_op->p3 ){
    MEM_ABOUT_TO_CHANGE(p, &mems[p_op->p3]);
    rc = __vdbe_mem_agg_value(mem, &mems[p_op->p3], p_op->p4.func);
    mem = &mems[p_op->p3];
  }else
#endif
  {
    rc = __vdbe_mem_finalize(mem, p_op->p4.func);
  }
 
  if( rc ){
    __vdbe_error(p, "%s", dbsql_value_text(mem));
    goto abort_due_to_error;
  }
  __vdbe_change_encoding(mem, encoding);
  UPDATE_MAX_BLOBSIZE(mem);
  REGISTER_TRACE((int)(mem-mems), mem);
  break;
}

#ifndef DBSQL_OMIT_WAL
/* Opcode: Checkpoint P1 P2 P3 * *
**
** Checkpoint database P1. This is a no-op if P1 is not currently in
** WAL mode. Parameter P2 is one of DBSQL_CHECKPOINT_PASSIVE, FULL,
** RESTART, or TRUNCATE.  Write 1 or 0 into mem[P3] if the checkpoint returns
** DBSQL_BUSY or not, respectively.  Write the number of pages in the
** WAL after the checkpoint into mem[P3+1] and the number of pages
** in the WAL that have been checkpointed after the checkpoint
** completes into mem[P3+2].  However on an error, mem[P3+1] and
** mem[P3+2] are initialized to -1.
*/
case OP_Checkpoint: {
  int i;                          /* Loop counter */
  int reses[3];                    /* Results */
  mem_t *mem;                      /* Write results here */

  assert( p->readOnly==0 );
  reses[0] = 0;
  reses[1] = reses[2] = -1;
  assert( p_op->p2==DBSQL_CHECKPOINT_PASSIVE
       || p_op->p2==DBSQL_CHECKPOINT_FULL
       || p_op->p2==DBSQL_CHECKPOINT_RESTART
       || p_op->p2==DBSQL_CHECKPOINT_TRUNCATE
       || p_op->p2==DBSQL_CHECKPOINT_NOOP
  );
  rc = __checkpoint(db, p_op->p1, p_op->p2, &reses[1], &reses[2]);
  if( rc ){
    if( rc!=DBSQL_BUSY ) goto abort_due_to_error;
    rc = DBSQL_OK;
    reses[0] = 1;
  }
  for(i=0, mem = &mems[p_op->p3]; i<3; i++, mem++){
    __vdbe_mem_set_int64(mem, (int64_t)reses[i]);
  }   
  break;
}; 
#endif

#ifndef DBSQL_OMIT_PRAGMA
/* Opcode: __journal_mode P1 P2 P3 * *
**
** Change the journal mode of database P1 to P3. P3 must be one of the
** PAGER_JOURNALMODE_XXX values. If changing between the various rollback
** modes (delete, truncate, persist, off and memory), this is a simple
** operation. No IO is required.
**
** If changing into or out of WAL mode the procedure is more complicated.
**
** Write a string containing the final journal-mode to register P2.
*/
case OP_JournalMode: {    /* out2 */
  sm_t *bt;                     /* sm_t to change journal mode of */
  pager_t *pager;                  /* pager_t associated with bt */
  int e_new;                       /* New journal mode */
  int old;                       /* The old journal mode */
#ifndef DBSQL_OMIT_WAL
  const char *filename;          /* Name of database file for pager */
#endif

  out = out2Prerelease(p, p_op);
  e_new = p_op->p3;
  assert( e_new==PAGER_JOURNALMODE_DELETE
       || e_new==PAGER_JOURNALMODE_TRUNCATE
       || e_new==PAGER_JOURNALMODE_PERSIST
       || e_new==PAGER_JOURNALMODE_OFF
       || e_new==PAGER_JOURNALMODE_MEMORY
       || e_new==PAGER_JOURNALMODE_WAL
       || e_new==PAGER_JOURNALMODE_QUERY
  );
  assert( p_op->p1>=0 && p_op->p1<db->n_db );
  assert( p->readOnly==0 );

  bt = db->dbs[p_op->p1].bt;
  pager = __sm_pager(bt);
  old = __sm_pager_get_journal_mode(pager);
  if( e_new==PAGER_JOURNALMODE_QUERY ) e_new = old;
  assert( __sm_holds_mutex(bt) );
  if( !__sm_pager_ok_to_change_journal_mode(pager) ) e_new = old;

#ifndef DBSQL_OMIT_WAL
  filename = __sm_pager_filename(pager, 1);

  /* Do not allow a transition to journal_mode=WAL for a database
  ** in temporary storage or if the VFS does not support shared memory
  */
  if( e_new==PAGER_JOURNALMODE_WAL
   && (__strlen30(filename)==0           /* Temp file */
       || !__sm_pager_wal_supported(pager))   /* No shared-memory support */
  ){
    e_new = old;
  }

  if( (e_new!=old)
   && (old==PAGER_JOURNALMODE_WAL || e_new==PAGER_JOURNALMODE_WAL)
  ){
    if( !db->auto_commit || db->n_vdbe_read>1 ){
      rc = DBSQL_ERROR;
      __vdbe_error(p,
          "cannot change %s wal mode from within a transaction",
          (e_new==PAGER_JOURNALMODE_WAL ? "into" : "out of")
      );
      goto abort_due_to_error;
    }else{

      if( old==PAGER_JOURNALMODE_WAL ){
        /* If leaving WAL mode, close the log file. If successful, the call
        ** to __pager_close_wal() checkpoints and deletes the write-ahead-log
        ** file. An EXCLUSIVE lock may still be held on the database file
        ** after a successful return.
        */
        rc = __sm_pager_close_wal(pager, db);
        if( rc==DBSQL_OK ){
          __sm_pager_set_journal_mode(pager, e_new);
        }
      }else if( old==PAGER_JOURNALMODE_MEMORY ){
        /* Cannot transition directly from MEMORY to WAL.  Use mode OFF
        ** as an intermediate */
        __sm_pager_set_journal_mode(pager, PAGER_JOURNALMODE_OFF);
      }
 
      /* Open a transaction on the database file. Regardless of the journal
      ** mode, this transaction always uses a rollback journal.
      */
      assert( __sm_txn_state(bt)!=DBSQL_TXN_WRITE );
      if( rc==DBSQL_OK ){
        rc = __sm_set_version(bt, (e_new==PAGER_JOURNALMODE_WAL ? 2 : 1));
      }
    }
  }
#endif /* ifndef DBSQL_OMIT_WAL */

  if( rc ) e_new = old;
  e_new = __sm_pager_set_journal_mode(pager, e_new);

  out->flags = MEM_Str|MEM_Static|MEM_Term;
  out->z = (char *)__journal_modename(e_new);
  out->n = __strlen30(out->z);
  out->enc = DBSQL_UTF8;
  __vdbe_change_encoding(out, encoding);
  if( rc ) goto abort_due_to_error;
  break;
};
#endif /* DBSQL_OMIT_PRAGMA */

#if !defined(DBSQL_OMIT_VACUUM) && !defined(DBSQL_OMIT_ATTACH)
/* Opcode: Vacuum P1 P2 * * *
**
** Vacuum the entire database P1.  P1 is 0 for "main", and 2 or more
** for an attached database.  The "temp" database may not be vacuumed.
**
** If P2 is not zero, then it is a register holding a string which is
** the file into which the result of vacuum should be written.  When
** P2 is zero, the vacuum overwrites the original database.
*/
case OP_Vacuum: {
  assert( p->readOnly==0 );
  rc = __run_vacuum(&p->errmsg, db, p_op->p1,
                        p_op->p2 ? &mems[p_op->p2] : 0);
  if( rc ) goto abort_due_to_error;
  break;
}
#endif

#if !defined(DBSQL_OMIT_AUTOVACUUM)
/* Opcode: __incr_vacuum P1 P2 * * *
**
** Perform a single step of the incremental vacuum procedure on
** the P1 database. If the vacuum has finished, jump to instruction
** P2. Otherwise, fall through to the next instruction.
*/
case OP_IncrVacuum: {        /* jump */
  sm_t *bt;

  assert( p_op->p1>=0 && p_op->p1<db->n_db );
  assert( DB_MASK_TEST(p->btree_mask, p_op->p1) );
  assert( p->readOnly==0 );
  bt = db->dbs[p_op->p1].bt;
  rc = __sm_incr_vacuum(bt);
  VDBE_BRANCH_TAKEN(rc==DBSQL_DONE,2);
  if( rc ){
    if( rc!=DBSQL_DONE ) goto abort_due_to_error;
    rc = DBSQL_OK;
    goto jump_to_p2;
  }
  break;
}
#endif

/* Opcode: Expire P1 P2 * * *
**
** Cause precompiled statements to expire.  When an expired statement
** is executed using dbsql_step() it will either automatically
** reprepare itself (if it was originally created using dbsql_prepare_v2())
** or it will fail with DBSQL_SCHEMA.
**
** If P1 is 0, then all SQL statements become expired. If P1 is non-zero,
** then only the currently executing statement is expired.
**
** If P2 is 0, then SQL statements are expired immediately.  If P2 is 1,
** then running SQL statements are allowed to continue to run to completion.
** The P2==1 case occurs when a CREATE INDEX or similar schema change happens
** that might help the statement run faster but which does not affect the
** correctness of operation.
*/
case OP_Expire: {
  assert( p_op->p2==0 || p_op->p2==1 );
  if( !p_op->p1 ){
    __expire_prepared_statements(db, p_op->p2);
  }else{
    p->expired = p_op->p2+1;
  }
  break;
}

/* Opcode: __cursor_lock P1 * * * *
**
** Lock the btree to which cursor P1 is pointing so that the btree cannot be
** written by an other cursor.
*/
case OP_CursorLock: {
  vdbe_cursor_t *p_c;
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  __sm_cursor_pin(p_c->uc.cursor);
  break;
}

/* Opcode: __cursor_unlock P1 * * * *
**
** Unlock the btree to which cursor P1 is pointing so that it can be
** written by other cursors.
*/
case OP_CursorUnlock: {
  vdbe_cursor_t *p_c;
  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  p_c = p->csrs[p_op->p1];
  assert( p_c!=0 );
  assert( p_c->cur_type==CURTYPE_BTREE );
  __sm_cursor_unpin(p_c->uc.cursor);
  break;
}

#ifndef DBSQL_OMIT_SHARED_CACHE
/* Opcode: table_lock_t P1 P2 P3 P4 *
** Synopsis: i_db=P1 root=P2 write=P3
**
** Obtain a lock on a particular table. This instruction is only used when
** the shared-cache feature is enabled.
**
** P1 is the index of the database in DBSQL.dbs[] of the database
** on which the lock is acquired.  A readlock is obtained if P3==0 or
** a write lock if P3==1.
**
** P2 contains the root-page of the table to lock.
**
** P4 contains a pointer to the name of the table being locked. This is only
** used to generate an error message if the lock cannot be obtained.
*/
case OP_TableLock: {
  u_int8_t is_write_lock = (u_int8_t)p_op->p3;
  if( is_write_lock || 0==(db->flags&DBSQL_ReadUncommit) ){
    int p1 = p_op->p1;
    assert( p1>=0 && p1<db->n_db );
    assert( DB_MASK_TEST(p->btree_mask, p1) );
    assert( is_write_lock==0 || is_write_lock==1 );
    rc = __sm_lock_table(db->dbs[p1].bt, p_op->p2, is_write_lock);
    if( rc ){
      if( (rc&0xFF)==DBSQL_LOCKED ){
        const char *z = p_op->p4.z;
        __vdbe_error(p, "database table is locked: %s", z);
      }
      goto abort_due_to_error;
    }
  }
  break;
}
#endif /* DBSQL_OMIT_SHARED_CACHE */

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: VBegin * * * P4 *
**
** P4 may be a pointer to an dbsql_vtab_t structure. If so, call the
** begin method for that table.
**
** Also, whether or not P4 is set, check that this is not being called from
** within a callback to a virtual table x_sync() method. If it is, the error
** code will be set to DBSQL_LOCKED.
*/
case OP_VBegin: {
  v_table_t *v_tab;
  v_tab = p_op->p4.vtab;
  rc = __vtab_begin(db, v_tab);
  if( v_tab ) __vtab_import_errmsg(p, v_tab->vtab);
  if( rc ) goto abort_due_to_error;
  break;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: VCreate P1 P2 * * *
**
** P2 is a register that holds the name of a virtual table in database
** P1. Call the create method for that table.
*/
case OP_VCreate: {
  mem_t s_mem;          /* For storing the record being decoded */
  const char *z_tab;  /* Name of the virtual table */

  memset(&s_mem, 0, sizeof(s_mem));
  s_mem.db = db;
  /* Because P2 is always a static string, it is impossible for the
  ** __vdbe_mem_copy() to fail */
  assert( (mems[p_op->p2].flags & MEM_Str)!=0 );
  assert( (mems[p_op->p2].flags & MEM_Static)!=0 );
  rc = __vdbe_mem_copy(&s_mem, &mems[p_op->p2]);
  assert( rc==DBSQL_OK );
  z_tab = (const char*)dbsql_value_text(&s_mem);
  assert( z_tab || db->malloc_failed );
  if( z_tab ){
    rc = __vtab_call_create(db, p_op->p1, z_tab, &p->errmsg);
  }
  __vdbe_mem_release(&s_mem);
  if( rc ) goto abort_due_to_error;
  break;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: VDestroy P1 * * P4 *
**
** P4 is the name of a virtual table in database P1.  Call the destroy method
** of that table.
*/
case OP_VDestroy: {
  db->n_v_destroy++;
  rc = __vtab_call_destroy(db, p_op->p1, p_op->p4.z);
  db->n_v_destroy--;
  assert( p->error_action==OE_Abort && p->uses_stmt_journal );
  if( rc ) goto abort_due_to_error;
  break;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: VOpen P1 * * P4 *
**
** P4 is a pointer to a virtual table object, an dbsql_vtab_t structure.
** P1 is a cursor number.  This opcode opens a cursor to the virtual
** table and stores that cursor in P1.
*/
case OP_VOpen: {             /* ncycle */
  vdbe_cursor_t *cur;
  dbsql_vtab_cursor_t *v_cur;
  dbsql_vtab_t *vtab;
  const dbsql_module_t *module;

  assert( p->is_reader );
  cur = p->csrs[p_op->p1];
  if( cur!=0
   && ALWAYS( cur->cur_type==CURTYPE_VTAB )
   && ALWAYS( cur->uc.v_cur->vtab==p_op->p4.vtab->vtab )
  ){
    /* This opcode is a no-op if the cursor is already open */
    break;
  }
  v_cur = 0;
  vtab = p_op->p4.vtab->vtab;
  if( vtab==0 || NEVER(vtab->module==0) ){
    rc = DBSQL_LOCKED;
    goto abort_due_to_error;
  }
  module = vtab->module;
  rc = module->x_open(vtab, &v_cur);
  __vtab_import_errmsg(p, vtab);
  if( rc ) goto abort_due_to_error;

  /* Initialize dbsql_vtab_cursor_t base class */
  v_cur->vtab = vtab;

  /* Initialize vdbe cursor object */
  cur = __allocate_cursor(p, p_op->p1, 0, CURTYPE_VTAB);
  if( cur ){
    cur->uc.v_cur = v_cur;
    vtab->n_ref++;
  }else{
    assert( db->malloc_failed );
    module->x_close(v_cur);
    goto no_mem;
  }
  break;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: VCheck P1 P2 P3 P4 *
**
** P4 is a pointer to a table_t object that is a virtual table in schema P1
** that supports the integrity() method.  This opcode runs the integrity()
** method for that virtual table, using P3 as the integer argument.  If
** an error is reported back, the table name is prepended to the error
** message and that message is stored in P2.  If no errors are seen,
** register P2 is set to NULL.
*/
case OP_VCheck: {             /* out2 */
  table_t *tab;
  dbsql_vtab_t *vtab;
  const dbsql_module_t *module;
  char *err = 0;

  out = &mems[p_op->p2];
  __vdbe_mem_set_null(out);  /* Innocent until proven guilty */
  assert( p_op->p4type==P4_TABLEREF );
  tab = p_op->p4.tab;
  assert( tab!=0 );
  assert( tab->n_tab_ref>0 );
  assert( IS_VIRTUAL(tab) );
  if( tab->u.vtab.p==0 ) break;
  vtab = tab->u.vtab.p->vtab;
  assert( vtab!=0 );
  module = vtab->module;
  assert( module!=0 );
  assert( module->i_version>=4 );
  assert( module->integrity!=0 );
  __vtab_lock(tab->u.vtab.p);
  assert( p_op->p1>=0 && p_op->p1<db->n_db );
  rc = module->integrity(vtab, db->dbs[p_op->p1].db_s_name, tab->name,
                           p_op->p3, &err);
  __vtab_unlock(tab->u.vtab.p);
  if( rc ){
    dbsql_free(err);
    goto abort_due_to_error;
  }
  if( err ){
    __vdbe_mem_set_str(out, err, -1, DBSQL_UTF8, dbsql_free);
  }
  break;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: __v_init_in P1 P2 P3 * *
** Synopsis: r[P2]=value_list_t(P1,P3)
**
** Set register P2 to be a pointer to a value_list_t object for cursor P1
** with cache register P3 and output register P3+1.  This value_list_t object
** can be used as the first argument to dbsql_vtab_in_first() and
** dbsql_vtab_in_next() to extract all of the values stored in the P1
** cursor.  Register P3 is used to hold the values returned by
** dbsql_vtab_in_first() and dbsql_vtab_in_next().
*/
case OP_VInitIn: {        /* out2, ncycle */
  vdbe_cursor_t *p_c;         /* The cursor containing the RHS values */
  value_list_t *rhs;        /* New value_list_t object to put in reg[P2] */

  p_c = p->csrs[p_op->p1];
  rhs = dbsql_malloc64( sizeof(*rhs) );
  if( rhs==0 ) goto no_mem;
  rhs->csr = p_c->uc.cursor;
  rhs->out = &mems[p_op->p3];
  out = out2Prerelease(p, p_op);
  out->flags = MEM_Null;
  __vdbe_mem_set_pointer(out, rhs, "ValueList", __vdbe_value_list_free);
  break;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */


#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: VFilter P1 P2 P3 P4 *
** Synopsis: iplan=r[P3] zplan='P4'
**
** P1 is a cursor opened using VOpen.  P2 is an address to jump to if
** the filtered result set is empty.
**
** P4 is either NULL or a string that was generated by the best_index
** method of the module.  The interpretation of the P4 string is left
** to the module implementation.
**
** This opcode invokes the filter method on the virtual table specified
** by P1.  The integer query plan parameter to filter is stored in register
** P3. Register P3+1 stores the argc parameter to be passed to the
** filter method. Registers P3+2..P3+1+argc are the argc
** additional parameters which are passed to
** filter as argv. Register P3+2 becomes argv[0] when passed to filter.
**
** A jump is made to P2 if the result set after filtering would be empty.
*/
case OP_VFilter: {   /* jump, ncycle */
  int n_arg;
  int i_query;
  const dbsql_module_t *module;
  mem_t *query;
  mem_t *argc;
  dbsql_vtab_cursor_t *v_cur;
  dbsql_vtab_t *vtab;
  vdbe_cursor_t *cur;
  int res;
  int i;
  mem_t **args;

  query = &mems[p_op->p3];
  argc = &query[1];
  cur = p->csrs[p_op->p1];
  assert( MEM_IS_VALID(query) );
  REGISTER_TRACE(p_op->p3, query);
  assert( cur!=0 );
  assert( cur->cur_type==CURTYPE_VTAB );
  v_cur = cur->uc.v_cur;
  vtab = v_cur->vtab;
  module = vtab->module;

  /* Grab the index number and argc parameters */
  assert( (query->flags&MEM_Int)!=0 && argc->flags==MEM_Int );
  n_arg = (int)argc->u.i;
  i_query = (int)query->u.i;

  /* Invoke the filter method */
  args = p->args;
  assert( n_arg<=p->nap_arg );
  for(i = 0; i<n_arg; i++){
    args[i] = &argc[i+1];
  }
  rc = module->filter(v_cur, i_query, p_op->p4.z, n_arg, args);
  __vtab_import_errmsg(p, vtab);
  if( rc ) goto abort_due_to_error;
  res = module->eof(v_cur);
  cur->null_row = 0;
  VDBE_BRANCH_TAKEN(res!=0,2);
  if( res ) goto jump_to_p2;
  break;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: VColumn P1 P2 P3 * P5
** Synopsis: r[P3]=vcolumn(P2)
**
** Store in register P3 the value of the P2-th column of
** the current row of the virtual-table of cursor P1.
**
** If the VColumn opcode is being used to fetch the value of
** an unchanging column during an UPDATE operation, then the P5
** value is OPFLAG_NOCHNG.  This will cause the dbsql_vtab_nochange()
** function to return true inside the column method of the virtual
** table implementation.  The P5 column might also contain other
** bits (OPFLAG_LENGTHARG or OPFLAG_TYPEOFARG) but those bits are
** unused by OP_VColumn.
*/
case OP_VColumn: {           /* ncycle */
  dbsql_vtab_t *vtab;
  const dbsql_module_t *module;
  mem_t *dest;
  dbsql_func_t s_context;
  func_def_t null_func;

  vdbe_cursor_t *cur = p->csrs[p_op->p1];
  assert( cur!=0 );
  assert( p_op->p3>0 && p_op->p3<=(p->n_mem+1 - p->n_cursor) );
  dest = &mems[p_op->p3];
  MEM_ABOUT_TO_CHANGE(p, dest);
  if( cur->null_row ){
    __vdbe_mem_set_null(dest);
    break;
  }
  assert( cur->cur_type==CURTYPE_VTAB );
  vtab = cur->uc.v_cur->vtab;
  module = vtab->module;
  assert( module->column );
  memset(&s_context, 0, sizeof(s_context));
  s_context.out = dest;
  s_context.enc = encoding;
  null_func.user_data = 0;
  null_func.func_flags = DBSQL_RESULT_SUBTYPE;
  s_context.func = &null_func;
  assert( p_op->p5==OPFLAG_NOCHNG || p_op->p5==0 );
  if( p_op->p5 & OPFLAG_NOCHNG ){
    __vdbe_mem_set_null(dest);
    dest->flags = MEM_Null|MEM_Zero;
    dest->u.n_zero = 0;
  }else{
    MEM_SET_TYPE_FLAG(dest, MEM_Null);
  }
  rc = module->column(cur->uc.v_cur, &s_context, p_op->p2);
  __vtab_import_errmsg(p, vtab);
  if( s_context.isError>0 ){
    __vdbe_error(p, "%s", dbsql_value_text(dest));
    rc = s_context.isError;
  }
  __vdbe_change_encoding(dest, encoding);
  REGISTER_TRACE(p_op->p3, dest);
  UPDATE_MAX_BLOBSIZE(dest);

  if( rc ) goto abort_due_to_error;
  break;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: VNext P1 P2 * * *
**
** Advance virtual table P1 to the next row in its result set and
** jump to instruction P2.  Or, if the virtual table has reached
** the end of its result set, then fall through to the next instruction.
*/
case OP_VNext: {   /* jump, ncycle */
  dbsql_vtab_t *vtab;
  const dbsql_module_t *module;
  int res;
  vdbe_cursor_t *cur;

  cur = p->csrs[p_op->p1];
  assert( cur!=0 );
  assert( cur->cur_type==CURTYPE_VTAB );
  if( cur->null_row ){
    break;
  }
  vtab = cur->uc.v_cur->vtab;
  module = vtab->module;
  assert( module->next );

  /* Invoke the next() method of the module. There is no way for the
  ** underlying implementation to return an error if one occurs during
  ** next(). Instead, if an error occurs, true is returned (indicating that
  ** data is available) and the error code returned when column or
  ** some other method is next invoked on the save virtual table cursor.
  */
  rc = module->next(cur->uc.v_cur);
  __vtab_import_errmsg(p, vtab);
  if( rc ) goto abort_due_to_error;
  res = module->eof(cur->uc.v_cur);
  VDBE_BRANCH_TAKEN(!res,2);
  if( !res ){
    /* If there is data, jump to P2 */
    goto jump_to_p2_and_check_for_interrupt;
  }
  goto check_for_interrupt;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: VRename P1 * * P4 *
**
** P4 is a pointer to a virtual table object, an dbsql_vtab_t structure.
** This opcode invokes the corresponding rename method. The value
** in register P1 is passed as the name argument to the rename method.
*/
case OP_VRename: {
  dbsql_vtab_t *vtab;
  mem_t *name;
  int is_legacy;
 
  is_legacy = (db->flags & DBSQL_LegacyAlter);
  db->flags |= DBSQL_LegacyAlter;
  vtab = p_op->p4.vtab->vtab;
  name = &mems[p_op->p1];
  assert( vtab->module->rename );
  assert( MEM_IS_VALID(name) );
  assert( p->readOnly==0 );
  REGISTER_TRACE(p_op->p1, name);
  assert( name->flags & MEM_Str );
  testcase( name->enc==DBSQL_UTF8 );
  testcase( name->enc==DBSQL_UTF16BE );
  testcase( name->enc==DBSQL_UTF16LE );
  rc = __vdbe_change_encoding(name, DBSQL_UTF8);
  if( rc ) goto abort_due_to_error;
  rc = vtab->module->rename(vtab, name->z);
  if( is_legacy==0 ) db->flags &= ~(u_int64_t)DBSQL_LegacyAlter;
  __vtab_import_errmsg(p, vtab);
  p->expired = 0;
  if( rc ) goto abort_due_to_error;
  break;
}
#endif

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Opcode: VUpdate P1 P2 P3 P4 P5
** Synopsis: data=r[P3@P2]
**
** P4 is a pointer to a virtual table object, an dbsql_vtab_t structure.
** This opcode invokes the corresponding update method. P2 values
** are contiguous memory cells starting at P3 to pass to the update
** invocation. The value in register (P3+P2-1) corresponds to the
** p2th element of the argv array passed to update.
**
** The update method will do a DELETE or an INSERT or both.
** The argv[0] element (which corresponds to memory cell P3)
** is the rowid of a row to delete.  If argv[0] is NULL then no
** deletion occurs.  The argv[1] element is the rowid of the new
** row.  This can be NULL to have the virtual table select the new
** rowid for itself.  The subsequent elements in the array are
** the values of columns in the new row.
**
** If P2==1 then no insert is performed.  argv[0] is the rowid of
** a row to delete.
**
** P1 is a boolean flag. If it is set to true and the update call
** is successful, then the value returned by dbsql_last_insert_rowid()
** is set to the value of the rowid for the row just inserted.
**
** P5 is the error actions (OE_Replace, OE_Fail, OE_Ignore, etc) to
** apply in the case of a constraint failure on an insert or update.
*/
case OP_VUpdate: {
  dbsql_vtab_t *vtab;
  const dbsql_module_t *module;
  int n_arg;
  int i;
  int64_t rowid = 0;
  mem_t **args;
  mem_t *p_x;

  assert( p_op->p2==1        || p_op->p5==OE_Fail   || p_op->p5==OE_Rollback
       || p_op->p5==OE_Abort || p_op->p5==OE_Ignore || p_op->p5==OE_Replace
  );
  assert( p->readOnly==0 );
  if( db->malloc_failed ) goto no_mem;
  __vdbe_incr_write_counter(p, 0);
  vtab = p_op->p4.vtab->vtab;
  if( vtab==0 || NEVER(vtab->module==0) ){
    rc = DBSQL_LOCKED;
    goto abort_due_to_error;
  }
  module = vtab->module;
  n_arg = p_op->p2;
  assert( p_op->p4type==P4_VTAB );
  if( ALWAYS(module->update) ){
    u_int8_t vtab_on_conflict = db->vtab_on_conflict;
    args = p->args;
    p_x = &mems[p_op->p3];
    assert( n_arg<=p->nap_arg );
    for(i=0; i<n_arg; i++){
      assert( MEM_IS_VALID(p_x) );
      MEM_ABOUT_TO_CHANGE(p, p_x);
      args[i] = p_x;
      p_x++;
    }
    db->vtab_on_conflict = p_op->p5;
    rc = module->update(vtab, n_arg, args, &rowid);
    db->vtab_on_conflict = vtab_on_conflict;
    __vtab_import_errmsg(p, vtab);
    if( rc==DBSQL_OK && p_op->p1 ){
      assert( n_arg>1 && args[0] && (args[0]->flags&MEM_Null) );
      db->last_rowid = rowid;
    }
    if( (rc&0xff)==DBSQL_CONSTRAINT && p_op->p4.vtab->constraint ){
      if( p_op->p5==OE_Ignore ){
        rc = DBSQL_OK;
      }else{
        p->error_action = ((p_op->p5==OE_Replace) ? OE_Abort : p_op->p5);
      }
    }else{
      p->n_change++;
    }
    if( rc ) goto abort_due_to_error;
  }
  break;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifndef  DBSQL_OMIT_PAGER_PRAGMAS
/* Opcode: Pagecount P1 P2 * * *
**
** Write the current number of pages in database P1 to memory cell P2.
*/
case OP_Pagecount: {            /* out2 */
  out = out2Prerelease(p, p_op);
  out->u.i = __sm_last_page(db->dbs[p_op->p1].bt);
  break;
}
#endif


#ifndef  DBSQL_OMIT_PAGER_PRAGMAS
/* Opcode: __max_pgcnt P1 P2 P3 * *
**
** Try to set the maximum page count for database P1 to the value in P3.
** Do not let the maximum page count fall below the current page count and
** do not change the maximum page count value if P3==0.
**
** Store the maximum page count after the change in register P2.
*/
case OP_MaxPgcnt: {            /* out2 */
  unsigned int new_max;
  sm_t *bt;

  out = out2Prerelease(p, p_op);
  bt = db->dbs[p_op->p1].bt;
  new_max = 0;
  if( p_op->p3 ){
    new_max = __sm_last_page(bt);
    if( new_max < (unsigned)p_op->p3 ) new_max = (unsigned)p_op->p3;
  }
  out->u.i = __sm_max_page_count(bt, new_max);
  break;
}
#endif

/* Opcode: Function P1 P2 P3 P4 *
** Synopsis: r[P3]=func(r[P2@NP])
**
** Invoke a user function (P4 is a pointer to an dbsql_func_t object that
** contains a pointer to the function to be run) with arguments taken
** from register P2 and successors.  The number of arguments is in
** the dbsql_func_t object that P4 points to.
** The result of the function is stored
** in register P3.  Register P3 must not be one of the function inputs.
**
** P1 is a 32-bit bitmask indicating whether or not each argument to the
** function was determined to be constant at compile time. If the first
** argument was constant then bit 0 of P1 is set. This is used to determine
** whether meta data associated with a user function argument using the
** dbsql_set_auxdata() API may be safely retained until the next
** invocation of this opcode.
**
** See also: __agg_step, __agg_final, __pure_func
*/
/* Opcode: __pure_func P1 P2 P3 P4 *
** Synopsis: r[P3]=func(r[P2@NP])
**
** Invoke a user function (P4 is a pointer to an dbsql_func_t object that
** contains a pointer to the function to be run) with arguments taken
** from register P2 and successors.  The number of arguments is in
** the dbsql_func_t object that P4 points to.
** The result of the function is stored
** in register P3.  Register P3 must not be one of the function inputs.
**
** P1 is a 32-bit bitmask indicating whether or not each argument to the
** function was determined to be constant at compile time. If the first
** argument was constant then bit 0 of P1 is set. This is used to determine
** whether meta data associated with a user function argument using the
** dbsql_set_auxdata() API may be safely retained until the next
** invocation of this opcode.
**
** This opcode works exactly like OP_Function.  The only difference is in
** its name.  This opcode is used in places where the function must be
** purely non-deterministic.  Some built-in date/time functions can be
** either deterministic of non-deterministic, depending on their arguments.
** When those function are used in a non-deterministic way, they will check
** to see if they were called using OP_PureFunc instead of OP_Function, and
** if they were, they throw an error.
**
** See also: __agg_step, __agg_final, Function
*/
case OP_PureFunc:              /* group */
case OP_Function: {            /* group */
  int i;
  dbsql_func_t *ctx;

  assert( p_op->p4type==P4_FUNCCTX );
  ctx = p_op->p4.ctx;

  /* If this function is inside of a trigger, the register array in mems[]
  ** might change from one evaluation to the next.  The next block of code
  ** checks to see if the register array has changed, and if so it
  ** reinitializes the relevant parts of the dbsql_func_t object */
  out = &mems[p_op->p3];
  if( ctx->out != out ){
    ctx->vdbe = p;
    ctx->out = out;
    ctx->enc = encoding;
    for(i=ctx->argc-1; i>=0; i--) ctx->argv[i] = &mems[p_op->p2+i];
  }
  assert( ctx->vdbe==p );

  MEM_ABOUT_TO_CHANGE(p, out);
#ifdef DBSQL_DEBUG
  for(i=0; i<ctx->argc; i++){
    assert( MEM_IS_VALID(ctx->argv[i]) );
    REGISTER_TRACE(p_op->p2+i, ctx->argv[i]);
  }
#endif
  MEM_SET_TYPE_FLAG(out, MEM_Null);
  assert( ctx->isError==0 );
  (*ctx->func->s_func)(ctx, ctx->argc, ctx->argv);/* IMP: R-24505-23230 */

  /* If the function returned an error, throw an exception */
  if( ctx->isError ){
    if( ctx->isError>0 ){
      __vdbe_error(p, "%s", dbsql_value_text(out));
      rc = ctx->isError;
    }
    __vdbe_delete_aux_data(db, &p->aux_data, ctx->i_op, p_op->p1);
    ctx->isError = 0;
    if( rc ) goto abort_due_to_error;
  }

  assert( (out->flags&MEM_Str)==0
       || out->enc==encoding
       || db->malloc_failed );
  assert( !__vdbe_mem_too_big(out) );

  REGISTER_TRACE(p_op->p3, out);
  UPDATE_MAX_BLOBSIZE(out);
  break;
}

/* Opcode: __clr_subtype P1 * * * *
** Synopsis:  r[P1].subtype = 0
**
** Clear the subtype from register P1.
*/
case OP_ClrSubtype: {   /* in1 */
  in1 = &mems[p_op->p1];
  in1->flags &= ~MEM_Subtype;
  break;
}

/* Opcode: __get_subtype P1 P2 * * *
** Synopsis:  r[P2] = r[P1].subtype
**
** Extract the subtype value from register P1 and write that subtype
** into register P2.  If P1 has no subtype, then P1 gets a NULL.
*/
case OP_GetSubtype: {   /* in1 out2 */
  in1 = &mems[p_op->p1];
  out = &mems[p_op->p2];
  if( in1->flags & MEM_Subtype ){
    __vdbe_mem_set_int64(out, in1->subtype);
  }else{
    __vdbe_mem_set_null(out);
  }
  break;
}

/* Opcode: __set_subtype P1 P2 * * *
** Synopsis:  r[P2].subtype = r[P1]
**
** Set the subtype value of register P2 to the integer from register P1.
** If P1 is NULL, clear the subtype from p2.
*/
case OP_SetSubtype: {   /* in1 out2 */
  in1 = &mems[p_op->p1];
  out = &mems[p_op->p2];
  if( in1->flags & MEM_Null ){
    out->flags &= ~MEM_Subtype;
  }else{
    assert( in1->flags & MEM_Int );
    out->flags |= MEM_Subtype;
    out->subtype = (u_int8_t)(in1->u.i & 0xff);
  }
  break;
}

/* Opcode: __filter_add P1 * P3 P4 *
** Synopsis: filter(P1) += key(P3@P4)
**
** Compute a hash on the P4 registers starting with r[P3] and
** add that hash to the bloom filter contained in r[P1].
*/
case OP_FilterAdd: {
  u_int64_t h;

  assert( p_op->p1>0 && p_op->p1<=(p->n_mem+1 - p->n_cursor) );
  in1 = &mems[p_op->p1];
  assert( in1->flags & MEM_Blob );
  assert( in1->n>0 );
  h = __filter_hash(mems, p_op);
#ifdef DBSQL_DEBUG
  if( db->flags&DBSQL_VdbeTrace ){
    int ii;
    for(ii=p_op->p3; ii<p_op->p3+p_op->p4.i; ii++){
      __register_trace(ii, &mems[ii]);
    }
    printf("hash: %llu modulo %d -> %u\n", h, in1->n, (int)(h%in1->n));
  }
#endif
  h %= (in1->n*8);
  in1->z[h/8] |= 1<<(h&7);
  break;
}

/* Opcode: Filter P1 P2 P3 P4 *
** Synopsis: if key(P3@P4) not in filter(P1) goto P2
**
** Compute a hash on the key contained in the P4 registers starting
** with r[P3].  Check to see if that hash is found in the
** bloom filter hosted by register P1.  If it is not present then
** maybe jump to P2.  Otherwise fall through.
**
** False negatives are harmless.  It is always safe to fall through,
** even if the value is in the bloom filter.  A false negative causes
** more CPU cycles to be used, but it should still yield the correct
** answer.  However, an incorrect answer may well arise from a
** false positive - if the jump is taken when it should fall through.
*/
case OP_Filter: {          /* jump */
  u_int64_t h;

  assert( p_op->p1>0 && p_op->p1<=(p->n_mem+1 - p->n_cursor) );
  in1 = &mems[p_op->p1];
  assert( (in1->flags & MEM_Blob)!=0 );
  assert( in1->n >= 1 );
  h = __filter_hash(mems, p_op);
#ifdef DBSQL_DEBUG
  if( db->flags&DBSQL_VdbeTrace ){
    int ii;
    for(ii=p_op->p3; ii<p_op->p3+p_op->p4.i; ii++){
      __register_trace(ii, &mems[ii]);
    }
    printf("hash: %llu modulo %d -> %u\n", h, in1->n, (int)(h%in1->n));
  }
#endif
  h %= (in1->n*8);
  if( (in1->z[h/8] & (1<<(h&7)))==0 ){
    VDBE_BRANCH_TAKEN(1, 2);
    p->counters[DBSQL_STMTSTATUS_FILTER_HIT]++;
    goto jump_to_p2;
  }else{
    p->counters[DBSQL_STMTSTATUS_FILTER_MISS]++;
    VDBE_BRANCH_TAKEN(0, 2);
  }
  break;
}

/* Opcode: Trace P1 P2 * P4 *
**
** Write P4 on the statement trace output if statement tracing is
** enabled.
**
** Operand P1 must be 0x7fffffff and P2 must positive.
*/
/* Opcode: Init P1 P2 P3 P4 *
** Synopsis: Start at P2
**
** Programs contain a single instance of this opcode as the very first
** opcode.
**
** If tracing is enabled (by the dbsql_trace()) interface, then
** the UTF-8 string contained in P4 is emitted on the trace callback.
** Or if P4 is blank, use the string returned by dbsql_sql().
**
** If P2 is not zero, jump to instruction P2.
**
** Increment the value of P1 so that OP_Once opcodes will jump the
** first time they are evaluated for this run.
**
** If P3 is not zero, then it is an address to jump to if an DBSQL_CORRUPT
** error is encountered.
*/
case OP_Trace:
case OP_Init: {          /* jump0 */
  int i;
#ifndef DBSQL_OMIT_TRACE
  char *trace;
#endif

  /* If the P4 argument is not NULL, then it must be an SQL comment string.
  ** The "--" string is broken up to prevent false-positives with srcck1.c.
  **
  ** This assert() provides evidence for:
  ** EVIDENCE-OF: R-50676-09860 The callback can compute the same text that
  ** would have been returned by the legacy dbsql_trace() interface by
  ** using the X argument when X begins with "--" and invoking
  ** dbsql_expanded_sql(P) otherwise.
  */
  assert( p_op->p4.z==0 || strncmp(p_op->p4.z, "-" "- ", 3)==0 );

  /* OP_Init is always instruction 0 */
  assert( p_op==p->ops || p_op->opcode==OP_Trace );

#ifndef DBSQL_OMIT_TRACE
  if( (db->m_trace & (DBSQL_TRACE_STMT|DBSQL_TRACE_LEGACY))!=0
   && p->min_write_file_format!=254  /* tag-20220401a */
   && (trace = (p_op->p4.z ? p_op->p4.z : p->z_sql))!=0
  ){
#ifndef DBSQL_OMIT_DEPRECATED
    if( db->m_trace & DBSQL_TRACE_LEGACY ){
      char *z = __vdbe_expand_sql(p, trace);
      db->trace.legacy(db->trace_arg, z);
      dbsql_free(z);
    }else
#endif
    if( db->n_vdbe_exec>1 ){
      char *z = __m_printf(db, "-- %s", trace);
      (void)db->trace.v2(DBSQL_TRACE_STMT, db->trace_arg, p, z);
      __dbsql_db_free(db, z);
    }else{
      (void)db->trace.v2(DBSQL_TRACE_STMT, db->trace_arg, p, trace);
    }
  }
#ifdef DBSQL_USE_FCNTL_TRACE
  trace = (p_op->p4.z ? p_op->p4.z : p->z_sql);
  if( trace ){
    int j;
    for(j=0; j<db->n_db; j++){
      if( DB_MASK_TEST(p->btree_mask, j)==0 ) continue;
      dbsql_file_control(db, db->dbs[j].db_s_name, DBSQL_FCNTL_TRACE, trace);
    }
  }
#endif /* DBSQL_USE_FCNTL_TRACE */
#ifdef DBSQL_DEBUG
  if( (db->flags & DBSQL_SqlTrace)!=0
   && (trace = (p_op->p4.z ? p_op->p4.z : p->z_sql))!=0
  ){
    __debug_printf("SQL-trace: %s\n", trace);
  }
#endif /* DBSQL_DEBUG */
#endif /* DBSQL_OMIT_TRACE */
  assert( p_op->p2>0 );
  if( p_op->p1>=__global_config.i_once_reset_threshold ){
    if( p_op->opcode==OP_Trace ) break;
    for(i=1; i<p->n_op; i++){
      if( p->ops[i].opcode==OP_Once ) p->ops[i].p1 = 0;
    }
    p_op->p1 = 0;
  }
  p_op->p1++;
  p->counters[DBSQL_STMTSTATUS_RUN]++;
  goto jump_to_p2;
}

#ifdef DBSQL_ENABLE_CURSOR_HINTS
/* Opcode: __cursor_hint P1 * * P4 *
**
** Provide a hint to cursor P1 that it only needs to return rows that
** satisfy the expr_t in P4.  TK_REGISTER terms in the P4 expression refer
** to values currently held in registers.  TK_COLUMN terms in the P4
** expression refer to columns in the b-tree to which cursor P1 is pointing.
*/
case OP_CursorHint: {
  vdbe_cursor_t *p_c;

  assert( p_op->p1>=0 && p_op->p1<p->n_cursor );
  assert( p_op->p4type==P4_EXPR );
  p_c = p->csrs[p_op->p1];
  if( p_c ){
    assert( p_c->cur_type==CURTYPE_BTREE );
    __sm_cursor_hint(p_c->uc.cursor, BTREE_HINT_RANGE,
                           p_op->p4.expr, mems);
  }
  break;
}
#endif /* DBSQL_ENABLE_CURSOR_HINTS */

#ifdef DBSQL_DEBUG
/* Opcode:  Abortable   * * * * *
**
** Verify that an Abort can happen.  Assert if an Abort at this point
** might cause database corruption.  This opcode only appears in debugging
** builds.
**
** An Abort is safe if either there have been no writes, or if there is
** an active statement journal.
*/
case OP_Abortable: {
  __vdbe_assert_abortable(p);
  break;
}
#endif

#ifdef DBSQL_DEBUG
/* Opcode:  __release_reg   P1 P2 P3 * P5
** Synopsis: release r[P1@P2] mask P3
**
** Release registers from service.  Any content that was in the
** the registers is unreliable after this opcode completes.
**
** The registers released will be the P2 registers starting at P1,
** except if bit ii of P3 set, then do not release register P1+ii.
** In other words, P3 is a mask of registers to preserve.
**
** Releasing a register clears the mem_t.scopy_from pointer.  That means
** that if the content of the released register was set using OP_SCopy,
** a change to the value of the source register for the OP_SCopy will no longer
** generate an assertion fault in __vdbe_mem_about_to_change().
**
** If P5 is set, then all released registers have their type set
** to MEM_Undefined so that any subsequent attempt to read the released
** register (before it is reinitialized) will generate an assertion fault.
**
** P5 ought to be set on every call to this opcode.
** However, there are places in the code generator will release registers
** before their are used, under the (valid) assumption that the registers
** will not be reallocated for some other purpose before they are used and
** hence are safe to release.
**
** This opcode is only available in testing and debugging builds.  It is
** not generated for release builds.  The purpose of this opcode is to help
** validate the generated bytecode.  This opcode does not actually contribute
** to computing an answer.
*/
case OP_ReleaseReg: {
  mem_t *mem;
  int i;
  u_int32_t const_mask;
  assert( p_op->p1>0 );
  assert( p_op->p1+p_op->p2<=(p->n_mem+1 - p->n_cursor)+1 );
  mem = &mems[p_op->p1];
  const_mask = p_op->p3;
  for(i=0; i<p_op->p2; i++, mem++){
    if( i>=32 || (const_mask & MASKBIT32(i))==0 ){
      mem->scopy_from = 0;
      if( i<32 && p_op->p5 ) MEM_SET_TYPE_FLAG(mem, MEM_Undefined);
    }
  }
  break;
}
#endif

/* Opcode: Noop * * * * *
**
** Do nothing.  Continue downward to the next opcode.
*/
/* Opcode: Explain P1 P2 P3 P4 *
**
** This is the same as OP_Noop during normal query execution.  The
** purpose of this opcode is to hold information about the query
** plan for the purpose of EXPLAIN QUERY PLAN output.
**
** The P4 value is human-readable text that describes the query plan
** element.  Something like "SCAN t1" or "SEARCH t2 USING INDEX t2x1".
**
** The P1 value is the ID of the current element and P2 is the parent
** element for the case of nested query plan elements.  If P2 is zero
** then this element is a top-level element.
**
** For loop elements, P3 is the estimated code of each invocation of this
** element.
**
** As with all opcodes, the meanings of the parameters for OP_Explain
** are subject to change from one release to the next.  Applications
** should not attempt to interpret or use any of the information
** contained in the OP_Explain opcode.  The information provided by this
** opcode is intended for testing and debugging use only.
*/
default: {          /* This is really OP_Noop, OP_Explain */
  assert( p_op->opcode==OP_Noop || p_op->opcode==OP_Explain );

  break;
}

/*****************************************************************************
** The cases of the switch statement above this line should all be indented
** by 6 spaces.  But the left-most 6 spaces have been removed to improve the
** readability.  From this point on down, the normal indentation rules are
** restored.
*****************************************************************************/
    }

#if defined(VDBE_PROFILE)
    *pn_cycle += __n_profile_cnt ? __n_profile_cnt : __hwtime();
    pn_cycle = 0;
#elif defined(DBSQL_ENABLE_STMT_SCANSTATUS)
    if( pn_cycle ){
      *pn_cycle += __hwtime();
      pn_cycle = 0;
    }
#endif

    /* The following code adds nothing to the actual functionality
    ** of the program.  It is only here for testing and debugging.
    ** On the other hand, it does burn CPU cycles every time through
    ** the evaluator loop.  So we can leave it out when NDEBUG is defined.
    */
#ifndef NDEBUG
    assert( p_op>=&ops[-1] && p_op<&ops[p->n_op-1] );

#ifdef DBSQL_DEBUG
    if( db->flags & DBSQL_VdbeTrace ){
      u_int8_t op_property = __opcode_property[orig_op->opcode];
      if( rc!=0 ) printf("rc=%d\n",rc);
      if( op_property & (OPFLG_OUT2) ){
        __register_trace(orig_op->p2, &mems[orig_op->p2]);
      }
      if( op_property & OPFLG_OUT3 ){
        __register_trace(orig_op->p3, &mems[orig_op->p3]);
      }
      if( op_property==0xff ){
        /* Never happens.  This code exists to avoid a harmless linkage
        ** warning about __vdbe_register_dump() being defined but not
        ** used. */
        __vdbe_register_dump(p);
      }
    }
#endif  /* DBSQL_DEBUG */
#endif  /* NDEBUG */
  }  /* The end of the for(;;) loop the loops through opcodes */

  /* If we reach this point, it means that execution is finished with
  ** an error of some kind.
  */
abort_due_to_error:
  if( db->malloc_failed ){
    rc = DBSQL_NOMEM_BKPT;
  }else if( rc==DBSQL_IOERR_CORRUPTFS ){
    rc = DBSQL_CORRUPT_BKPT;
  }
  assert( rc );
#ifdef DBSQL_DEBUG
  if( db->flags & DBSQL_VdbeTrace ){
    const char *trace = p->z_sql;
    if( trace==0 ){
      if( ops[0].opcode==OP_Trace ){
        trace = ops[0].p4.z;
      }
      if( trace==0 ) trace = "???";
    }
    printf("ABORT-due-to-error (rc=%d): %s\n", rc, trace);
  }
#endif
  if( p->errmsg==0 && rc!=DBSQL_IOERR_NOMEM ){
    __vdbe_error(p, "%s", __err_str(rc));
  }
  p->rc = rc;
  __system_error(db, rc);
  testcase( __global_config.x_log!=0 );
  __vdbe_log_abort(p, rc, p_op, ops);
  if( p->vdbe_state==VDBE_RUN_STATE ) __vdbe_halt(p);
  if( rc==DBSQL_IOERR_NOMEM ) __oom_fault(db);
  if( rc==DBSQL_CORRUPT && db->auto_commit==0 ){
    db->flags |= DBSQL_CorruptRdOnly;
  }
  rc = DBSQL_ERROR;
  if( reset_schema_on_fault>0 ){
    __reset_one_schema(db, reset_schema_on_fault-1);
  }

  /* This is the only way out of this procedure.  We have to
  ** release the mutexes on btrees that were acquired at the
  ** top. */
vdbe_return:
#if defined(VDBE_PROFILE)
  if( pn_cycle ){
    *pn_cycle += __n_profile_cnt ? __n_profile_cnt : __hwtime();
    pn_cycle = 0;
  }
#elif defined(DBSQL_ENABLE_STMT_SCANSTATUS)
  if( pn_cycle ){
    *pn_cycle += __hwtime();
    pn_cycle = 0;
  }
#endif

#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
  while( n_vm_step>=n_progress_limit && db->progress!=0 ){
    n_progress_limit += db->n_progress_ops;
    if( db->progress(db->progress_arg) ){
      n_progress_limit = LARGEST_UINT64;
      rc = DBSQL_INTERRUPT;
      goto abort_due_to_error;
    }
  }
#endif
  p->counters[DBSQL_STMTSTATUS_VM_STEP] += (int)n_vm_step;
  if( DB_MASK_NON_ZERO(p->lock_mask) ){
    __vdbe_leave(p);
  }
  assert( rc!=DBSQL_OK || n_extra_delete==0
       || dbsql_strlike("DELETE%",p->z_sql,0)!=0
  );
  return rc;

  /* Jump to here if a string or blob larger than DBSQL_MAX_LENGTH
  ** is encountered.
  */
too_big:
  __vdbe_error(p, "string or blob too big");
  rc = DBSQL_TOOBIG;
  goto abort_due_to_error;

  /* Jump to here if a malloc() fails.
  */
no_mem:
  __oom_fault(db);
  __vdbe_error(p, "out of memory");
  rc = DBSQL_NOMEM_BKPT;
  goto abort_due_to_error;

  /* Jump to here if the dbsql_interrupt() API sets the interrupt
  ** flag.
  */
abort_due_to_interrupt:
  assert( ATOMIC_LOAD(&db->u1.is_interrupted) );
  rc = DBSQL_INTERRUPT;
  goto abort_due_to_error;
}
