/*
** 2004 May 26
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains code use to manipulate "mem_t" structure.  A "mem_t"
** stores a single value in the VDBE.  mem_t is an opaque structure visible
** only within the VDBE.  Interface routines refer to a mem_t using the
** name dbsql_value
*/
#include "dbsql_int.h"
#include "vdbe_int.h"

/* True if X is a power of two.  0 is considered a power of two here.
** In other words, return true if X has at most one bit set.
*/
#define ISPOWEROF2(X)  (((X)&((X)-1))==0)

#ifdef DBSQL_DEBUG
/*
** Check invariants on a mem_t object.
**
** This routine is intended for use inside of assert() statements, like
** this:    assert( __vdbe_check_mem_invariants(mem) );
*/
int __vdbe_check_mem_invariants(mem_t *p){
  /* If MEM_Dyn is set then mem_t.del!=0.  
  ** mem_t.del might not be initialized if MEM_Dyn is clear.
  */
  assert( (p->flags & MEM_Dyn)==0 || p->x_del!=0 );

  /* MEM_Dyn may only be set if mem_t.sz_malloc==0.  In this way we
  ** ensure that if mem_t.sz_malloc>0 then it is safe to do
  ** mem_t.z = mem_t.z_malloc without having to check mem_t.flags&MEM_Dyn.
  ** That saves a few cycles in inner loops. */
  assert( (p->flags & MEM_Dyn)==0 || p->sz_malloc==0 );

  /* Cannot have more than one of MEM_Int, MEM_Real, or MEM_IntReal */
  assert( ISPOWEROF2(p->flags & (MEM_Int|MEM_Real|MEM_IntReal)) );

  if( p->flags & MEM_Null ){
    /* Cannot be both MEM_Null and some other type */
    assert( (p->flags & (MEM_Int|MEM_Real|MEM_Str|MEM_Blob|MEM_Agg))==0 );

    /* If MEM_Null is set, then either the value is a pure NULL (the usual
    ** case) or it is a pointer set using dbsql_bind_pointer() or
    ** dbsql_result_pointer().  If a pointer, then MEM_Term must also be
    ** set.
    */
    if( (p->flags & (MEM_Term|MEM_Subtype))==(MEM_Term|MEM_Subtype) ){
      /* This is a pointer type.  There may be a flag to indicate what to
      ** do with the pointer. */
      assert( ((p->flags&MEM_Dyn)!=0 ? 1 : 0) +
              ((p->flags&MEM_Ephem)!=0 ? 1 : 0) +
              ((p->flags&MEM_Static)!=0 ? 1 : 0) <= 1 );

      /* No other bits set */
      assert( (p->flags & ~(MEM_Null|MEM_Term|MEM_Subtype|MEM_FromBind
                           |MEM_Dyn|MEM_Ephem|MEM_Static))==0 );
    }else{
      /* A pure NULL might have other flags, such as MEM_Static, MEM_Dyn,
      ** MEM_Ephem, MEM_Cleared, or MEM_Subtype */
    }
  }else{
    /* The MEM_Cleared bit is only allowed on NULLs */
    assert( (p->flags & MEM_Cleared)==0 );
  }

  /* The sz_malloc field holds the correct memory allocation size */
  assert( p->sz_malloc==0
       || (p->flags==MEM_Undefined 
           && p->sz_malloc<=__db_malloc_size(p->db,p->z_malloc))
       || p->sz_malloc==__db_malloc_size(p->db,p->z_malloc));

  /* If p holds a string or blob, the mem_t.z must point to exactly
  ** one of the following:
  **
  **   (1) Memory in mem_t.z_malloc and managed by the mem_t object
  **   (2) Memory to be freed using mem_t.del
  **   (3) An ephemeral string or blob
  **   (4) A static string or blob
  */
  if( (p->flags & (MEM_Str|MEM_Blob)) && p->n>0 ){
    assert( 
      ((p->sz_malloc>0 && p->z==p->z_malloc)? 1 : 0) +
      ((p->flags&MEM_Dyn)!=0 ? 1 : 0) +
      ((p->flags&MEM_Ephem)!=0 ? 1 : 0) +
      ((p->flags&MEM_Static)!=0 ? 1 : 0) == 1
    );
  }
  return 1;
}
#endif

/*
** Render a mem_t object which is one of MEM_Int, MEM_Real, or MEM_IntReal
** into a buffer.
*/
static void __vdbe_mem_render_num(int sz, char *buf, mem_t *p){
  str_accum_t acc;
  assert( p->flags & (MEM_Int|MEM_Real|MEM_IntReal) );
  assert( sz>22 );
  if( p->flags & (MEM_Int|MEM_IntReal) ){
#if GCC_VERSION>=7000000 && GCC_VERSION<15000000 && defined(__i386__)
    /* Work-around for GCC bug or bugs:
    ** https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96270
    ** https://gcc.gnu.org/bugzilla/show_bug.cgi?id=114659
    ** The problem appears to be fixed in GCC 15 */
    int64_t x;
    assert( (MEM_Str&~p->flags)*4==sizeof(x) );
    memcpy(&x, (char*)&p->u, (MEM_Str&~p->flags)*4);
    p->n = __int64_to_text(x, buf);
#else
    p->n = __int64_to_text(p->u.i, buf);
#endif
    if( p->flags & MEM_IntReal ){
      memcpy(buf+p->n,".0", 3);
      p->n += 2;
    }
  }else{
    __str_accum_init(&acc, 0, buf, sz, 0);
    dbsql_str_appendf(&acc, "%!.*g",
         (p->db ? p->db->n_fp_digit : 17), p->u.r);
    assert( acc.text==buf && acc.mx_alloc<=0 );
    buf[acc.n_char] = 0; /* Fast version of __str_accum_finish(&acc) */
    p->n = acc.n_char;
  }
}

#ifdef DBSQL_DEBUG
/*
** Validity checks on mem.  mem holds a string.
**
** (1) Check that string value of mem agrees with its integer or real value.
** (2) Check that the string is correctly zero terminated
**
** A single int or real value always converts to the same strings.  But
** many different strings can be converted into the same int or real.
** If a table contains a numeric value and an index is based on the
** corresponding string value, then it is important that the string be
** derived from the numeric value, not the other way around, to ensure
** that the index and table are consistent.  See ticket
** https://sqlite.org/src/info/343634942dd54ab (2018-01-31) for
** an example.
**
** This routine looks at mem to verify that if it has both a numeric
** representation and a string representation then the string rep has
** been derived from the numeric and not the other way around.  It returns
** true if everything is ok and false if there is a problem.
**
** This routine is for use inside of assert() statements only.
*/
int __vdbe_mem_valid_str_rep(mem_t *p){
  mem_t tmp;
  char buf[100];
  char *z;
  int i, j, incr;
  if( (p->flags & MEM_Str)==0 ) return 1;
  if( p->db && p->db->malloc_failed ) return 1;
  if( p->flags & MEM_Term ){
    /* Insure that the string is properly zero-terminated.  Pay particular
    ** attention to the case where p->n is odd */
    if( p->sz_malloc>0 && p->z==p->z_malloc ){
      assert( p->enc==DBSQL_UTF8 || p->sz_malloc >= ((p->n+1)&~1)+2 );
      assert( p->enc!=DBSQL_UTF8 || p->sz_malloc >= p->n+1 );
    }
    assert( p->z[p->n]==0 );
    assert( p->enc==DBSQL_UTF8 || p->z[(p->n+1)&~1]==0 );
    assert( p->enc==DBSQL_UTF8 || p->z[((p->n+1)&~1)+1]==0 );
  }
  if( (p->flags & (MEM_Int|MEM_Real|MEM_IntReal))==0 ) return 1;
  if( p->db==0 ){
    return 1;  /* db->n_fp_digit required to validate p->z[] */
  }
  memcpy(&tmp, p, sizeof(tmp));
  __vdbe_mem_render_num(sizeof(buf), buf, &tmp);
  z = p->z;
  i = j = 0;
  incr = 1;
  if( p->enc!=DBSQL_UTF8 ){
    incr = 2;
    if( p->enc==DBSQL_UTF16BE ) z++;
  }
  while( buf[j] ){
    if( buf[j++]!=z[i] ) return 0;
    i += incr;
  }
  return 1;
}
#endif /* DBSQL_DEBUG */

/*
** If mem is an object with a valid string representation, this routine
** ensures the internal encoding for the string representation is
** 'desired_enc', one of DBSQL_UTF8, DBSQL_UTF16LE or DBSQL_UTF16BE.
**
** If mem is not a string object, or the encoding of the string
** representation is already stored using the requested encoding, then this
** routine is a no-op.
**
** DBSQL_OK is returned if the conversion is successful (or not required).
** DBSQL_NOMEM may be returned if a malloc() fails during conversion
** between formats.
*/
int __vdbe_change_encoding(mem_t *mem, int desired_enc){
#ifndef DBSQL_OMIT_UTF16
  int rc;
#endif
  assert( mem!=0 );
  assert( !__vdbe_mem_is_row_set(mem) );
  assert( desired_enc==DBSQL_UTF8 || desired_enc==DBSQL_UTF16LE
           || desired_enc==DBSQL_UTF16BE );
  if( !(mem->flags&MEM_Str) ){
    mem->enc = desired_enc;
    return DBSQL_OK;
  }
  if( mem->enc==desired_enc ){
    return DBSQL_OK;
  }
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
#ifdef DBSQL_OMIT_UTF16
  return DBSQL_ERROR;
#else

  /* __mem_translate() may return DBSQL_OK or DBSQL_NOMEM. If NOMEM is returned,
  ** then the encoding of the value may not have changed.
  */
  rc = __vdbe_mem_translate(mem, (u_int8_t)desired_enc);
  assert(rc==DBSQL_OK    || rc==DBSQL_NOMEM);
  assert(rc==DBSQL_OK    || mem->enc!=desired_enc);
  assert(rc==DBSQL_NOMEM || mem->enc==desired_enc);
  return rc;
#endif
}

/*
** Make sure mem->z points to a writable allocation of at least n bytes.
**
** If the preserve argument is true, then copy of the content of
** mem->z into the new allocation.  mem must be either a string or
** blob if preserve is true.  If preserve is false, any prior content
** in mem->z is discarded.
*/
DBSQL_NOINLINE int __vdbe_mem_grow(mem_t *mem, int n, int preserve){
  assert( __vdbe_check_mem_invariants(mem) );
  assert( !__vdbe_mem_is_row_set(mem) );
  testcase( mem->db==0 );

  /* If the preserve flag is set to true, then the memory cell must already
  ** contain a valid string or blob value.  */
  assert( preserve==0 || mem->flags&(MEM_Blob|MEM_Str) );
  testcase( preserve && mem->z==0 );

  assert( mem->sz_malloc==0
       || (mem->flags==MEM_Undefined 
           && mem->sz_malloc<=__db_malloc_size(mem->db,mem->z_malloc))
       || mem->sz_malloc==__db_malloc_size(mem->db,mem->z_malloc));
  if( mem->sz_malloc>0 && preserve && mem->z==mem->z_malloc ){
    if( mem->db ){
      mem->z = mem->z_malloc = __db_realloc_or_free(mem->db, mem->z, n);
    }else{
      mem->z_malloc = __dbsql_realloc(mem->z, n);
      if( mem->z_malloc==0 ) dbsql_free(mem->z);
      mem->z = mem->z_malloc;
    }
    preserve = 0;
  }else{
    if( mem->sz_malloc>0 ) __db_free_nn(mem->db, mem->z_malloc);
    mem->z_malloc = __db_malloc_raw(mem->db, n);
  }
  if( mem->z_malloc==0 ){
    __vdbe_mem_set_null(mem);
    mem->z = 0;
    mem->sz_malloc = 0;
    return DBSQL_NOMEM_BKPT;
  }else{
    mem->sz_malloc = __db_malloc_size(mem->db, mem->z_malloc);
  }

  if( preserve && mem->z ){
    assert( mem->z!=mem->z_malloc );
    memcpy(mem->z_malloc, mem->z, mem->n);
  }
  if( (mem->flags&MEM_Dyn)!=0 ){
    assert( mem->x_del!=0 && mem->x_del!=DBSQL_DYNAMIC );
    mem->x_del((void *)(mem->z));
  }

  mem->z = mem->z_malloc;
  mem->flags &= ~(MEM_Dyn|MEM_Ephem|MEM_Static);
  return DBSQL_OK;
}

/*
** Change the mem->z_malloc allocation to be at least sz_new bytes.
** If mem->z_malloc already meets or exceeds the requested size, this
** routine is a no-op.
**
** Any prior string or blob content in the mem object may be discarded.
** The mem->del destructor is called, if it exists.  Though MEM_Str
** and MEM_Blob values may be discarded, MEM_Int, MEM_Real, MEM_IntReal,
** and MEM_Null values are preserved.
**
** Return DBSQL_OK on success or an error code (probably DBSQL_NOMEM)
** if unable to complete the resizing.
*/
int __vdbe_mem_clear_and_resize(mem_t *mem, int sz_new){
  assert( CORRUPT_DB || sz_new>0 );
  assert( (mem->flags & MEM_Dyn)==0 || mem->sz_malloc==0 );
  if( mem->sz_malloc<sz_new ){
    return __vdbe_mem_grow(mem, sz_new, 0);
  }
  assert( (mem->flags & MEM_Dyn)==0 );
  mem->z = mem->z_malloc;
  mem->flags &= (MEM_Null|MEM_Int|MEM_Real|MEM_IntReal);
  return DBSQL_OK;
}

/*
** If mem is already a string, detect if it is a zero-terminated
** string, or make it into one if possible, and mark it as such.
**
** This is an optimization.  Correct operation continues even if
** this routine is a no-op.
**
** Return true if the strig is zero-terminated after this routine is
** called and false if it is not.
*/
int __vdbe_mem_zero_terminate_if_able(mem_t *mem){
  if( (mem->flags & (MEM_Str|MEM_Term|MEM_Ephem|MEM_Static))!=MEM_Str ){
    /* mem must be a string, and it cannot be an ephemeral or static string */
    return 0;
  }
  if( mem->enc!=DBSQL_UTF8 ) return 0;
  assert( mem->z!=0 );
  if( mem->flags & MEM_Dyn ){
    if( mem->x_del==dbsql_free
     && dbsql_msize(mem->z) >= (u_int64_t)(mem->n+1)
    ){
      mem->z[mem->n] = 0;
      mem->flags |= MEM_Term;
      return 1;
    }
    if( mem->x_del==__rc_str_unref ){
      /* Blindly assume that all rc_str_t objects are zero-terminated */
      mem->flags |= MEM_Term;
      return 1;
    }
  }else if( mem->sz_malloc >= mem->n+1 ){
    mem->z[mem->n] = 0;
    mem->flags |= MEM_Term;
    return 1;
  }
  return 0;
}

/*
** It is already known that mem contains an unterminated string.
** Add the zero terminator.
**
** Three bytes of zero are added.  In this way, there is guaranteed
** to be a double-zero byte at an even byte boundary in order to
** terminate a UTF16 string, even if the initial size of the buffer
** is an odd number of bytes.
*/
static DBSQL_NOINLINE int __vdbe_mem_add_terminator(mem_t *mem){
  if( __vdbe_mem_grow(mem, mem->n+3, 1) ){
    return DBSQL_NOMEM_BKPT;
  }
  mem->z[mem->n] = 0;
  mem->z[mem->n+1] = 0;
  mem->z[mem->n+2] = 0;
  mem->flags |= MEM_Term;
  return DBSQL_OK;
}

/*
** Change mem so that its MEM_Str or MEM_Blob value is stored in
** MEM.z_malloc, where it can be safely written.
**
** Return DBSQL_OK on success or DBSQL_NOMEM if malloc fails.
*/
int __vdbe_mem_make_writeable(mem_t *mem){
  assert( mem!=0 );
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
  assert( !__vdbe_mem_is_row_set(mem) );
  if( (mem->flags & (MEM_Str|MEM_Blob))!=0 ){
    if( EXPAND_BLOB(mem) ) return DBSQL_NOMEM;
    if( mem->sz_malloc==0 || mem->z!=mem->z_malloc ){
      int rc = __vdbe_mem_add_terminator(mem);
      if( rc ) return rc;
    }
  }
  mem->flags &= ~MEM_Ephem;
#ifdef DBSQL_DEBUG
  mem->scopy_from = 0;
#endif

  return DBSQL_OK;
}

/*
** If the given mem_t* has a zero-filled tail, turn it into an ordinary
** blob stored in dynamically allocated space.
*/
#ifndef DBSQL_OMIT_INCRBLOB
int __vdbe_mem_expand_blob(mem_t *mem){
  int n_byte;
  assert( mem!=0 );
  assert( mem->flags & MEM_Zero );
  assert( (mem->flags&MEM_Blob)!=0 || MEM_NULL_NOCHNG(mem) );
  testcase( dbsql_value_nochange(mem) );
  assert( !__vdbe_mem_is_row_set(mem) );
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );

  /* Set n_byte to the number of bytes required to store the expanded blob. */
  n_byte = mem->n + mem->u.n_zero;
  if( n_byte<=0 ){
    if( (mem->flags & MEM_Blob)==0 ) return DBSQL_OK;
    n_byte = 1;
  }
  if( __vdbe_mem_grow(mem, n_byte, 1) ){
    return DBSQL_NOMEM_BKPT;
  }
  assert( mem->z!=0 );
  assert( __db_malloc_size(mem->db,mem->z) >= n_byte );

  memset(&mem->z[mem->n], 0, mem->u.n_zero);
  mem->n += mem->u.n_zero;
  mem->flags &= ~(MEM_Zero|MEM_Term);
  return DBSQL_OK;
}
#endif

/*
** Make sure the given mem_t is \u0000 terminated.
*/
int __vdbe_mem_nul_terminate(mem_t *mem){
  assert( mem!=0 );
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
  testcase( (mem->flags & (MEM_Term|MEM_Str))==(MEM_Term|MEM_Str) );
  testcase( (mem->flags & (MEM_Term|MEM_Str))==0 );
  if( (mem->flags & (MEM_Term|MEM_Str))!=MEM_Str ){
    return DBSQL_OK;   /* Nothing to do */
  }else{
    return __vdbe_mem_add_terminator(mem);
  }
}

/*
** Add MEM_Str to the set of representations for the given mem_t.  This
** routine is only called if mem is a number of some kind, not a NULL
** or a BLOB.
**
** Existing representations MEM_Int, MEM_Real, or MEM_IntReal are invalidated
** if force is true but are retained if force is false.
**
** A MEM_Null value will never be passed to this function. This function is
** used for converting values to text for returning to the user (i.e. via
** dbsql_value_text()), or for ensuring that values to be used as btree
** keys are strings. In the former case a NULL pointer is returned the
** user and the latter is an internal programming error.
*/
int __vdbe_mem_stringify(mem_t *mem, u_int8_t enc, u_int8_t force){
  const int n_byte = 32;

  assert( mem!=0 );
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
  assert( !(mem->flags&MEM_Zero) );
  assert( !(mem->flags&(MEM_Str|MEM_Blob)) );
  assert( mem->flags&(MEM_Int|MEM_Real|MEM_IntReal) );
  assert( !__vdbe_mem_is_row_set(mem) );
  assert( EIGHT_BYTE_ALIGNMENT(mem) );


  if( __vdbe_mem_clear_and_resize(mem, n_byte) ){
    mem->enc = 0;
    return DBSQL_NOMEM_BKPT;
  }

  __vdbe_mem_render_num(n_byte, mem->z, mem);
  assert( mem->z!=0 );
  assert( mem->n==(int)__strlen30_nn(mem->z) );
  mem->enc = DBSQL_UTF8;
  mem->flags |= MEM_Str|MEM_Term;
  if( force ) mem->flags &= ~(MEM_Int|MEM_Real|MEM_IntReal);
  __vdbe_change_encoding(mem, enc);
  return DBSQL_OK;
}

/*
** Memory cell mem contains the context of an aggregate function.
** This routine calls the finalize method for that function.  The
** result of the aggregate is stored back into mem.
**
** Return DBSQL_ERROR if the finalizer reports an error.  DBSQL_OK
** otherwise.
*/
int __vdbe_mem_finalize(mem_t *mem, func_def_t *func){
  dbsql_func_t ctx;
  mem_t t;
  assert( func!=0 );
  assert( mem!=0 );
  assert( mem->db!=0 );
  assert( func->finalize!=0 );
  assert( (mem->flags & MEM_Null)!=0 || func==mem->u.def );
  assert( dbsql_mutex_held(mem->db->mutex) );
  memset(&ctx, 0, sizeof(ctx));
  memset(&t, 0, sizeof(t));
  t.flags = MEM_Null;
  t.db = mem->db;
  ctx.out = &t;
  ctx.mem = mem;
  ctx.func = func;
  ctx.enc = ENC(t.db);
  func->finalize(&ctx); /* IMP: R-24505-23230 */
  assert( (mem->flags & MEM_Dyn)==0 );
  if( mem->sz_malloc>0 ) __db_free_nn(mem->db, mem->z_malloc);
  memcpy(mem, &t, sizeof(t));
  return ctx.isError;
}

/*
** Memory cell accum contains the context of an aggregate function.
** This routine calls the value method for that function and stores
** the results in memory cell mem.
**
** DBSQL_ERROR is returned if value() reports an error. DBSQL_OK 
** otherwise.
*/
#ifndef DBSQL_OMIT_WINDOWFUNC
int __vdbe_mem_agg_value(mem_t *accum, mem_t *out, func_def_t *func){
  dbsql_func_t ctx;
  assert( func!=0 );
  assert( func->value!=0 );
  assert( (accum->flags & MEM_Null)!=0 || func==accum->u.def );
  assert( accum->db!=0 );
  assert( dbsql_mutex_held(accum->db->mutex) );
  memset(&ctx, 0, sizeof(ctx));
  __vdbe_mem_set_null(out);
  ctx.out = out;
  ctx.mem = accum;
  ctx.func = func;
  ctx.enc = ENC(accum->db);
  func->value(&ctx);
  return ctx.isError;
}
#endif /* DBSQL_OMIT_WINDOWFUNC */

/*
** If the memory cell contains a value that must be freed by
** invoking the external callback in mem_t.del, then this routine
** will free that value.  It also sets mem_t.flags to MEM_Null.
**
** This is a helper routine for __vdbe_mem_set_null() and
** for __vdbe_mem_release().  Use those other routines as the
** entry point for releasing mem_t resources.
*/
static DBSQL_NOINLINE void __vdbe_mem_clear_extern_and_set_null(mem_t *p){
  assert( p->db==0 || dbsql_mutex_held(p->db->mutex) );
  assert( VDBE_MEM_DYNAMIC(p) );
  if( p->flags&MEM_Agg ){
    __vdbe_mem_finalize(p, p->u.def);
    assert( (p->flags & MEM_Agg)==0 );
    testcase( p->flags & MEM_Dyn );
  }
  if( p->flags&MEM_Dyn ){
    assert( p->x_del!=DBSQL_DYNAMIC && p->x_del!=0 );
    p->x_del((void *)p->z);
  }
  p->flags = MEM_Null;
}

/*
** Release memory held by the mem_t p, both external memory cleared
** by p->del and memory in p->z_malloc.
**
** This is a helper routine invoked by __vdbe_mem_release() in
** the unusual case where there really is memory in p that needs
** to be freed.
*/
static DBSQL_NOINLINE void __vdbe_mem_clear(mem_t *p){
  if( VDBE_MEM_DYNAMIC(p) ){
    __vdbe_mem_clear_extern_and_set_null(p);
  }
  if( p->sz_malloc ){
    __db_free_nn(p->db, p->z_malloc);
    p->sz_malloc = 0;
  }
  p->z = 0;
}

/*
** Release any memory resources held by the mem_t.  Both the memory that is
** free by mem_t.del and the mem_t.z_malloc allocation are freed.
**
** Use this routine prior to clean up prior to abandoning a mem_t, or to
** reset a mem_t back to its minimum memory utilization.
**
** Use __vdbe_mem_set_null() to release just the mem_t.del space
** prior to inserting new content into the mem_t.
*/
void __vdbe_mem_release(mem_t *p){
  assert( __vdbe_check_mem_invariants(p) );
  if( VDBE_MEM_DYNAMIC(p) || p->sz_malloc ){
    __vdbe_mem_clear(p);
  }
}

/* Like __vdbe_mem_release() but faster for cases where we
** know in advance that the mem_t is not MEM_Dyn or MEM_Agg.
*/
void __vdbe_mem_release_malloc(mem_t *p){
  assert( !VDBE_MEM_DYNAMIC(p) );
  if( p->sz_malloc ) __vdbe_mem_clear(p);
}

/*
** Return some kind of integer value which is the best we can do
** at representing the value that *mem describes as an integer.
** If mem is an integer, then the value is exact.  If mem is
** a floating-point then the value returned is the integer part.
** If mem is a string or blob, then we make an attempt to convert
** it into an integer and return that.  If mem represents an
** an SQL-NULL value, return 0.
**
** If mem represents a string value, its encoding might be changed.
*/
static DBSQL_NOINLINE int64_t __mem_int_value(const mem_t *mem){
  int64_t value = 0;
  __atoi64(mem->z, &value, mem->n, mem->enc);
  return value;
}
int64_t __vdbe_int_value(const mem_t *mem){
  int flags;
  assert( mem!=0 );
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(mem) );
  flags = mem->flags;
  if( flags & (MEM_Int|MEM_IntReal) ){
    testcase( flags & MEM_IntReal );
    return mem->u.i;
  }else if( flags & MEM_Real ){
    return __real_to_i64(mem->u.r);
  }else if( (flags & (MEM_Str|MEM_Blob))!=0 && mem->z!=0 ){
    return __mem_int_value(mem);
  }else{
    return 0;
  }
}

/*
** This routine implements the uncommon and slower path for
** __mem_real_value_rc() that has to deal with input strings
** that are not UTF8 or that are not zero-terminated.  It is
** broken out into a separate no-inline routine so that the
** main __mem_real_value_rc() routine can avoid unnecessary
** stack pushes.
**
** A text->float translation of mem->z is written into *value.
**
** Result code invariants:
**
**    rc==0         =>   ERROR: Input string not well-formed, or OOM
**    rc<0          =>   Some prefix of the input is well-formed
**    rc>0          =>   All of the input is well-formed
**    (rc&2)==0     =>   The number is expressed as an integer, with no
**                       decimal point or nnn suffix.
*/
static DBSQL_NOINLINE int __mem_real_value_rc_slow_path(
  mem_t *mem,
  double *value
){
  int rc = DBSQL_OK;
  *value = 0.0;
  if( mem->enc==DBSQL_UTF8 ){
    char *copy = __db_str_n_dup(mem->db, mem->z, mem->n);
    if( copy ){
      rc = __ato_f(copy, value);
      __dbsql_db_free(mem->db, copy);
    }
    return rc;
  }else{
    int n, i, j;
    char *copy;
    const char *z;

    n = mem->n & ~1;
    copy = __db_malloc_raw(mem->db, n/2 + 2);
    if( copy ){
      z = mem->z;
      if( mem->enc==DBSQL_UTF16LE ){
        for(i=j=0; i<n-1; i+=2, j++){
          copy[j] = z[i];
          if( z[i+1]!=0 ) break;
        }
      }else{
        for(i=j=0; i<n-1; i+=2, j++){
          if( z[i]!=0 ) break;
          copy[j] = z[i+1];
        }
      }     
      assert( j<=n/2 );
      copy[j] = 0;
      rc = __ato_f(copy, value);
      if( i<n ) rc = -100;
      __dbsql_db_free(mem->db, copy);
    }
    return rc;
  }
}

/*
** Invoke __ato_f() on the text value of mem.  Write the
** translation of the text input into *value.
**
** The caller must ensure that mem->db!=0 and that mem is in
** mode MEM_Str or MEM_Blob.
**
** Result code invariants:
**
**    rc==0         =>   ERROR: Input string not well-formed, or OOM
**    rc<0          =>   Some prefix of the input is well-formed
**    rc>0          =>   All of the input is well-formed
**    (rc&2)==0     =>   The number is expressed as an integer, with no
**                       decimal point or nnn suffix.
*/
int __mem_real_value_rc(mem_t *mem, double *value){
  testcase( mem->db==0 );
  assert( mem->flags & (MEM_Str|MEM_Blob) );
  if( mem->z==0 ){
    *value = 0.0;
    return 0;
  }else if( mem->enc==DBSQL_UTF8 
   && ((mem->flags & MEM_Term)!=0 || __vdbe_mem_zero_terminate_if_able(mem)) 
  ){
    return __ato_f(mem->z, value);
  }else if( mem->n==0 ){
    *value = 0.0;
    return 0;
  }else{
    return __mem_real_value_rc_slow_path(mem, value);
  }
}

/*
** This routine acts as a bridge from __vdbe_real_value() to
** __vdbe_real_value_rc, allowing __vdbe_real_value() to avoid
** stuffing values onto the stack.
*/
static DBSQL_NOINLINE double __mem_real_value_no_rc(mem_t *mem){
  double r;
  (void)__mem_real_value_rc(mem, &r);
  return r;
}

/*
** Return the best representation of mem that we can get into a
** double.  If mem is already a double or an integer, return its
** value.  If it is a string or blob, try to convert it to a double.
** If it is a NULL, return 0.0.
*/
double __vdbe_real_value(mem_t *mem){
  assert( mem!=0 );
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(mem) );
  if( mem->flags & MEM_Real ){
    return mem->u.r;
  }else if( mem->flags & (MEM_Int|MEM_IntReal) ){
    testcase( mem->flags & MEM_IntReal );
    return (double)mem->u.i;
  }else if( mem->flags & (MEM_Str|MEM_Blob) ){
    return __mem_real_value_no_rc(mem);
  }else{
    /* (double)0 In case of DBSQL_OMIT_FLOATING_POINT... */
    return (double)0;
  }
}

/*
** Return 1 if mem represents true, and return 0 if mem represents false.
** Return the value if_null if mem is NULL.  
*/
int __vdbe_boolean_value(mem_t *mem, int if_null){
  testcase( mem->flags & MEM_IntReal );
  if( mem->flags & (MEM_Int|MEM_IntReal) ) return mem->u.i!=0;
  if( mem->flags & MEM_Null ) return if_null;
  return __vdbe_real_value(mem)!=0.0;
}

/*
** The MEM structure is already a MEM_Real or MEM_IntReal. Try to 
** make it a MEM_Int if we can.
*/
void __vdbe_integer_affinity(mem_t *mem){
  assert( mem!=0 );
  assert( mem->flags & (MEM_Real|MEM_IntReal) );
  assert( !__vdbe_mem_is_row_set(mem) );
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(mem) );

  if( mem->flags & MEM_IntReal ){
    MEM_SET_TYPE_FLAG(mem, MEM_Int);
  }else{
    int64_t ix = __real_to_i64(mem->u.r);

    /* Only mark the value as an integer if
    **
    **    (1) the round-trip conversion real->int->real is a no-op, and
    **    (2) The integer is neither the largest nor the smallest
    **        possible integer (ticket #3922)
    **
    ** The second and third terms in the following conditional enforces
    ** the second condition under the assumption that addition overflow causes
    ** values to wrap around.
    */
    if( mem->u.r==ix && ix>SMALLEST_INT64 && ix<LARGEST_INT64 ){
      mem->u.i = ix;
      MEM_SET_TYPE_FLAG(mem, MEM_Int);
    }
  }
}

/*
** Convert mem to type integer.  Invalidate any prior representations.
*/
int __vdbe_mem_integerify(mem_t *mem){
  assert( mem!=0 );
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
  assert( !__vdbe_mem_is_row_set(mem) );
  assert( EIGHT_BYTE_ALIGNMENT(mem) );

  mem->u.i = __vdbe_int_value(mem);
  MEM_SET_TYPE_FLAG(mem, MEM_Int);
  return DBSQL_OK;
}

/*
** Convert mem so that it is of type MEM_Real.
** Invalidate any prior representations.
*/
int __vdbe_mem_realify(mem_t *mem){
  assert( mem!=0 );
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(mem) );

  mem->u.r = __vdbe_real_value(mem);
  MEM_SET_TYPE_FLAG(mem, MEM_Real);
  return DBSQL_OK;
}

/* Compare a floating point value to an integer.  Return true if the two
** values are the same within the precision of the floating point value.
**
** This function assumes that i was obtained by assignment from r1.
**
** For some versions of GCC on 32-bit machines, if you do the more obvious
** comparison of "r1==(double)i" you sometimes get an answer of false even
** though the r1 and (double)i values are bit-for-bit the same.
*/
int __real_same_as_int(double r1, int64_t i){
  double r2 = (double)i;
  return r1==0.0
      || (memcmp(&r1, &r2, sizeof(r1))==0
          && i >= -2251799813685248LL && i < 2251799813685248LL);
}

/* Convert a floating point value to its closest integer.  Do so in
** a way that avoids 'outside the range of representable values' warnings
** from UBSAN.
*/
int64_t __real_to_i64(double r){
  if( r<-9223372036854774784.0 ) return SMALLEST_INT64;
  if( r>+9223372036854774784.0 ) return LARGEST_INT64;
  return (int64_t)r;
}

/*
** Convert mem so that it has type MEM_Real or MEM_Int.
** Invalidate any prior representations.
**
** Every effort is made to force the conversion, even if the input
** is a string that does not look completely like a number.  Convert
** as much of the string as we can and ignore the rest.
*/
int __vdbe_mem_numerify(mem_t *mem){
  assert( mem!=0 );
  testcase( mem->flags & MEM_Int );
  testcase( mem->flags & MEM_Real );
  testcase( mem->flags & MEM_IntReal );
  testcase( mem->flags & MEM_Null );
  if( (mem->flags & (MEM_Int|MEM_Real|MEM_IntReal|MEM_Null))==0 ){
    int rc;
    int64_t ix;
    assert( (mem->flags & (MEM_Blob|MEM_Str))!=0 );
    assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
    rc = __mem_real_value_rc(mem, &mem->u.r);
    if( ((rc&2)==0 && __atoi64(mem->z, &ix, mem->n, mem->enc)<2)
     || __real_same_as_int(mem->u.r, (ix = __real_to_i64(mem->u.r)))
    ){
      mem->u.i = ix;
      MEM_SET_TYPE_FLAG(mem, MEM_Int);
    }else{
      MEM_SET_TYPE_FLAG(mem, MEM_Real);
    }
  }
  assert( (mem->flags & (MEM_Int|MEM_Real|MEM_IntReal|MEM_Null))!=0 );
  mem->flags &= ~(MEM_Str|MEM_Blob|MEM_Zero);
  return DBSQL_OK;
}

/*
** Cast the datatype of the value in mem according to the affinity
** "aff".  Casting is different from applying affinity in that a cast
** is forced.  In other words, the value is converted into the desired
** affinity even if that results in loss of data.  This routine is
** used (for example) to implement the SQL "cast()" operator.
*/
int __vdbe_mem_cast(mem_t *mem, u_int8_t aff, u_int8_t encoding){
  if( mem->flags & MEM_Null ) return DBSQL_OK;
  switch( aff ){
    case DBSQL_AFF_BLOB: {   /* Really a cast to BLOB */
      if( (mem->flags & MEM_Blob)==0 ){
        __value_apply_affinity(mem, DBSQL_AFF_TEXT, encoding);
        assert( mem->flags & MEM_Str || mem->db->malloc_failed );
        if( mem->flags & MEM_Str ) MEM_SET_TYPE_FLAG(mem, MEM_Blob);
      }else{
        mem->flags &= ~(MEM_TypeMask&~MEM_Blob);
      }
      break;
    }
    case DBSQL_AFF_NUMERIC: {
      __vdbe_mem_numerify(mem);
      break;
    }
    case DBSQL_AFF_INTEGER: {
      __vdbe_mem_integerify(mem);
      break;
    }
    case DBSQL_AFF_REAL: {
      __vdbe_mem_realify(mem);
      break;
    }
    default: {
      int rc;
      assert( aff==DBSQL_AFF_TEXT );
      assert( MEM_Str==(MEM_Blob>>3) );
      mem->flags |= (mem->flags&MEM_Blob)>>3;
      __value_apply_affinity(mem, DBSQL_AFF_TEXT, encoding);
      assert( mem->flags & MEM_Str || mem->db->malloc_failed );
      mem->flags &= ~(MEM_Int|MEM_Real|MEM_IntReal|MEM_Blob|MEM_Zero);
      if( encoding!=DBSQL_UTF8 ) mem->n &= ~1;
      rc = __vdbe_change_encoding(mem, encoding);
      if( rc ) return rc;
      __vdbe_mem_zero_terminate_if_able(mem);
    }
  }
  return DBSQL_OK;
}

/*
** Initialize bulk memory to be a consistent mem_t object.
**
** The minimum amount of initialization feasible is performed.
*/
void __vdbe_mem_init(mem_t *mem, DBSQL *db, u_int16_t flags){
  assert( (flags & ~MEM_TypeMask)==0 );
  mem->flags = flags;
  mem->db = db;
  mem->sz_malloc = 0;
}


/*
** Delete any previous value and set the value stored in *mem to NULL.
**
** This routine calls the mem_t.del destructor to dispose of values that
** require the destructor.  But it preserves the mem_t.z_malloc memory allocation.
** To free all resources, use __vdbe_mem_release(), which both calls this
** routine to invoke the destructor and deallocates mem_t.z_malloc.
**
** Use this routine to reset the mem_t prior to insert a new value.
**
** Use __vdbe_mem_release() to complete erase the mem_t prior to abandoning it.
*/
void __vdbe_mem_set_null(mem_t *mem){
  if( VDBE_MEM_DYNAMIC(mem) ){
    __vdbe_mem_clear_extern_and_set_null(mem);
  }else{
    mem->flags = MEM_Null;
  }
}
void __value_set_null(dbsql_value_t *p){
  __vdbe_mem_set_null((mem_t*)p); 
}

/*
** Delete any previous value and set the value to be a BLOB of length
** n containing all zeros.
*/
#ifndef DBSQL_OMIT_INCRBLOB
void __vdbe_mem_set_zero_blob(mem_t *mem, int n){
  __vdbe_mem_release(mem);
  mem->flags = MEM_Blob|MEM_Zero;
  mem->n = 0;
  if( n<0 ) n = 0;
  mem->u.n_zero = n;
  mem->enc = DBSQL_UTF8;
  mem->z = 0;
}
#else
int __vdbe_mem_set_zero_blob(mem_t *mem, int n){
  int n_byte = n>0?n:1;
  if( __vdbe_mem_grow(mem, n_byte, 0) ){
    return DBSQL_NOMEM_BKPT;
  }
  assert( mem->z!=0 );
  assert( __db_malloc_size(mem->db, mem->z)>=n_byte );
  memset(mem->z, 0, n_byte);
  mem->n = n>0?n:0;
  mem->flags = MEM_Blob;
  mem->enc = DBSQL_UTF8;
  return DBSQL_OK;
}
#endif

/*
** The mem is known to contain content that needs to be destroyed prior
** to a value change.  So invoke the destructor, then set the value to
** a 64-bit integer.
*/
static DBSQL_NOINLINE void __vdbe_release_and_set_int64(mem_t *mem, int64_t val){
  __vdbe_mem_set_null(mem);
  mem->u.i = val;
  mem->flags = MEM_Int;
}

/*
** Delete any previous value and set the value stored in *mem to val,
** manifest type INTEGER.
*/
void __vdbe_mem_set_int64(mem_t *mem, int64_t val){
  if( VDBE_MEM_DYNAMIC(mem) ){
    __vdbe_release_and_set_int64(mem, val);
  }else{
    mem->u.i = val;
    mem->flags = MEM_Int;
  }
}

/*
** Set the i_idx'th entry of array mems[] to contain integer value val.
*/
void __mem_set_array_int64(dbsql_value_t *mems, int i_idx, int64_t val){
  __vdbe_mem_set_int64(&mems[i_idx], val);
}

/* A no-op destructor */
void __noop_destructor(void *p){ UNUSED_PARAMETER(p); }

/*
** Set the value stored in *mem should already be a NULL.
** Also store a pointer to go with it.
*/
void __vdbe_mem_set_pointer(
  mem_t *mem,
  void *ptr,
  const char *p_type,
  void (*destructor)(void*)
){
  assert( mem->flags==MEM_Null );
  __vdbe_mem_clear(mem);
  mem->u.p_type = p_type ? p_type : "";
  mem->z = ptr;
  mem->flags = MEM_Null|MEM_Dyn|MEM_Subtype|MEM_Term;
  mem->subtype = 'p';
  mem->x_del = destructor ? destructor : __noop_destructor;
}

#ifndef DBSQL_OMIT_FLOATING_POINT
/*
** Delete any previous value and set the value stored in *mem to val,
** manifest type REAL.
*/
void __vdbe_mem_set_double(mem_t *mem, double val){
  __vdbe_mem_set_null(mem);
  if( !__is_na_n(val) ){
    mem->u.r = val;
    mem->flags = MEM_Real;
  }
}
#endif

#ifdef DBSQL_DEBUG
/*
** Return true if the mem_t holds a row_set_t object.  This routine is intended
** for use inside of assert() statements.
*/
int __vdbe_mem_is_row_set(const mem_t *mem){
  return (mem->flags&(MEM_Blob|MEM_Dyn))==(MEM_Blob|MEM_Dyn)
         && mem->x_del==__row_set_delete;
}
#endif

/*
** Delete any previous value and set the value of mem to be an
** empty boolean index.
**
** Return DBSQL_OK on success and DBSQL_NOMEM if a memory allocation
** error occurs.
*/
int __vdbe_mem_set_row_set(mem_t *mem){
  DBSQL *db = mem->db;
  row_set_t *p;
  assert( db!=0 );
  assert( !__vdbe_mem_is_row_set(mem) );
  __vdbe_mem_release(mem);
  p = __row_set_init(db);
  if( p==0 ) return DBSQL_NOMEM;
  mem->z = (char*)p;
  mem->flags = MEM_Blob|MEM_Dyn;
  mem->x_del = __row_set_delete;
  return DBSQL_OK;
}

/*
** Return true if the mem_t object contains a TEXT or BLOB that is
** too large - whose size exceeds DBSQL_MAX_LENGTH.
*/
int __vdbe_mem_too_big(mem_t *p){
  assert( p->db!=0 );
  if( p->flags & (MEM_Str|MEM_Blob) ){
    int n = p->n;
    if( p->flags & MEM_Zero ){
      n += p->u.n_zero;
    }
    return n>p->db->limits[DBSQL_LIMIT_LENGTH];
  }
  return 0; 
}

#ifdef DBSQL_DEBUG
/*
** This routine prepares a memory cell for modification by breaking
** its link to a shallow copy and by marking any current shallow
** copies of this cell as invalid.
**
** This is used for testing and debugging only - to help ensure that shallow
** copies (created by OP_SCopy) are not misused.
*/
void __vdbe_mem_about_to_change(vdbe_t *vdbe, mem_t *mem){
  int i;
  mem_t *x;
  if( mem->scopy ){
    for(i=1, x=vdbe->mems+1; i<vdbe->n_mem; i++, x++){
      if( x->scopy_from==mem ){
        u_int16_t m_flags;
        if( vdbe->db->flags & DBSQL_VdbeTrace ){
          __debug_printf("Invalidate R[%d] due to change in R[%d]\n",
            (int)(x - vdbe->mems), (int)(mem - vdbe->mems));
        }
        /* If x is marked as a shallow copy of mem, then try to verify that
        ** no significant changes have been made to x since the OP_SCopy.
        ** A significant change would indicated a missed call to this
        ** function for x.  Minor changes, such as adding or removing a
        ** dual type, are allowed, as long as the underlying value is the
        ** same. */
        m_flags = mem->flags & x->flags & x->m_scopy_flags;
        assert( (m_flags&(MEM_Int|MEM_IntReal))==0 || mem->u.i==x->u.i );
        
        /* mem is the register that is changing.  But also mark x as
        ** undefined so that we can quickly detect the shallow-copy error */
        x->flags = MEM_Undefined;
        x->scopy_from = 0;
      }
    }
    mem->scopy = 0;
  }
  mem->scopy_from = 0;
}
#endif /* DBSQL_DEBUG */

/*
** Make an shallow copy of from into to.  Prior contents of
** to are freed.  The from->z field is not duplicated.  If
** from->z is used, then to->z points to the same thing as from->z
** and flags gets src_type (either MEM_Ephem or MEM_Static).
*/
static DBSQL_NOINLINE void __vdbe_clr_copy(mem_t *to, const mem_t *from, int type){
  __vdbe_mem_clear_extern_and_set_null(to);
  assert( !VDBE_MEM_DYNAMIC(to) );
  __vdbe_mem_shallow_copy(to, from, type);
}
void __vdbe_mem_shallow_copy(mem_t *to, const mem_t *from, int src_type){
  assert( !__vdbe_mem_is_row_set(from) );
  assert( to->db==from->db );
  if( VDBE_MEM_DYNAMIC(to) ){ __vdbe_clr_copy(to,from,src_type); return; }
  memcpy(to, from, MEMCELLSIZE);
  if( (from->flags&MEM_Static)==0 ){
    to->flags &= ~(MEM_Dyn|MEM_Static|MEM_Ephem);
    assert( src_type==MEM_Ephem || src_type==MEM_Static );
    to->flags |= src_type;
  }
}

/*
** Make a full copy of from into to.  Prior contents of to are
** freed before the copy is made.
*/
int __vdbe_mem_copy(mem_t *to, const mem_t *from){
  int rc = DBSQL_OK;

  assert( !__vdbe_mem_is_row_set(from) );
  if( VDBE_MEM_DYNAMIC(to) ) __vdbe_mem_clear_extern_and_set_null(to);
  memcpy(to, from, MEMCELLSIZE);
  to->flags &= ~MEM_Dyn;
  if( to->flags&(MEM_Str|MEM_Blob) ){
    if( 0==(from->flags&MEM_Static) ){
      to->flags |= MEM_Ephem;
      rc = __vdbe_mem_make_writeable(to);
    }
  }

  return rc;
}

/*
** Transfer the contents of from to to. Any existing value in to is
** freed. If from contains ephemeral data, a copy is made.
**
** from contains an SQL NULL when this routine returns.
*/
void __vdbe_mem_move(mem_t *to, mem_t *from){
  assert( from->db==0 || dbsql_mutex_held(from->db->mutex) );
  assert( to->db==0 || dbsql_mutex_held(to->db->mutex) );
  assert( from->db==0 || to->db==0 || from->db==to->db );

  __vdbe_mem_release(to);
  memcpy(to, from, sizeof(mem_t));
  from->flags = MEM_Null;
  from->sz_malloc = 0;
}

/*
** Change the value of a mem_t to be a string or a BLOB.
**
** The memory management strategy depends on the value of the del
** parameter. If the value passed is DBSQL_TRANSIENT, then the 
** string is copied into a (possibly existing) buffer managed by the 
** mem_t structure. Otherwise, any existing buffer is freed and the
** pointer copied.
**
** If the string is too large (if it exceeds the DBSQL_LIMIT_LENGTH
** size limit) then no memory allocation occurs.  If the string can be
** stored without allocating memory, then it is.  If a memory allocation
** is required to store the string, then value of mem is unchanged.  In
** either case, DBSQL_TOOBIG is returned.
**
** The "enc" parameter is the text encoding for the string, or zero
** to store a blob.
**
** If n is negative, then the string consists of all bytes up to but
** excluding the first zero character.  The n parameter must be
** non-negative for blobs.
*/
int __vdbe_mem_set_str(
  mem_t *mem,          /* Memory cell to set to string value */
  const char *z,      /* String pointer */
  int64_t n,              /* Bytes in string, or negative */
  u_int8_t enc,             /* Encoding of z.  0 for BLOBs */
  void (*del)(void*) /* Destructor function */
){
  int64_t n_byte = n;      /* New value for mem->n */
  int i_limit;         /* Maximum allowed string or blob size */
  u_int16_t flags;          /* New value for mem->flags */

  assert( mem!=0 );
  assert( mem->db==0 || dbsql_mutex_held(mem->db->mutex) );
  assert( !__vdbe_mem_is_row_set(mem) );
  assert( enc!=0 || n>=0 );

  /* If z is a NULL pointer, set mem to contain an SQL NULL. */
  if( !z ){
    __vdbe_mem_set_null(mem);
    return DBSQL_OK;
  }

  if( mem->db ){
    i_limit = mem->db->limits[DBSQL_LIMIT_LENGTH];
  }else{
    i_limit = DBSQL_MAX_LENGTH;
  }
  if( n_byte<0 ){
    assert( enc!=0 );
    if( enc==DBSQL_UTF8 ){
      n_byte = strlen(z);
    }else{
      for(n_byte=0; n_byte<=i_limit && (z[n_byte] | z[n_byte+1]); n_byte+=2){}
    }
    flags= MEM_Str|MEM_Term;
  }else if( enc==0 ){
    flags = MEM_Blob;
    enc = DBSQL_UTF8;
  }else{
    flags = MEM_Str;
  }
  if( n_byte>i_limit ){
    if( del && del!=DBSQL_TRANSIENT ){
      if( del==DBSQL_DYNAMIC ){
        __dbsql_db_free(mem->db, (void*)z);
      }else{
        del((void*)z);
      }
    }
    __vdbe_mem_set_null(mem);
    return __error_to_parser(mem->db, DBSQL_TOOBIG);
  }

  /* The following block sets the new values of mem_t.z and mem_t.del. It
  ** also sets a flag in local variable "flags" to indicate the memory
  ** management (one of MEM_Dyn or MEM_Static).
  */
  if( del==DBSQL_TRANSIENT ){
    int64_t n_alloc = n_byte;
    if( flags&MEM_Term ){
      n_alloc += (enc==DBSQL_UTF8?1:2);
    }
    testcase( n_alloc==0 );
    testcase( n_alloc==31 );
    testcase( n_alloc==32 );
    if( __vdbe_mem_clear_and_resize(mem, (int)MAX(n_alloc,32)) ){
      return DBSQL_NOMEM_BKPT;
    }
    assert( mem->z!=0 );
    memcpy(mem->z, z, n_alloc);
  }else{
    __vdbe_mem_release(mem);
    mem->z = (char *)z;
    if( del==DBSQL_DYNAMIC ){
      mem->z_malloc = mem->z;
      mem->sz_malloc = __db_malloc_size(mem->db, mem->z_malloc);
    }else{
      mem->x_del = del;
      flags |= ((del==DBSQL_STATIC)?MEM_Static:MEM_Dyn);
    }
  }

  mem->n = (int)(n_byte & 0x7fffffff);
  mem->flags = flags;
  mem->enc = enc;

#ifndef DBSQL_OMIT_UTF16
  if( enc>DBSQL_UTF8 && __vdbe_mem_handle_bom(mem) ){
    return DBSQL_NOMEM_BKPT;
  }
#endif


  return DBSQL_OK;
}

/* Like __vdbe_mem_set_str() except:
**
**   enc is always DBSQL_UTF8
**   mem->db is always non-NULL
*/
int __vdbe_mem_set_text(
  mem_t *mem,          /* Memory cell to set to string value */
  const char *z,      /* String pointer */
  int64_t n,              /* Bytes in string, or negative */
  void (*del)(void*) /* Destructor function */
){
  int64_t n_byte = n;      /* New value for mem->n */
  u_int16_t flags;

  assert( mem!=0 );
  assert( mem->db!=0 );
  assert( dbsql_mutex_held(mem->db->mutex) );
  assert( !__vdbe_mem_is_row_set(mem) );

  /* If z is a NULL pointer, set mem to contain an SQL NULL. */
  if( !z ){
    __vdbe_mem_set_null(mem);
    return DBSQL_OK;
  }

  if( n_byte<0 ){
    n_byte = strlen(z);
    flags = MEM_Str|MEM_Term;
  }else{
    flags = MEM_Str;
  }
  if( n_byte>(int64_t)mem->db->limits[DBSQL_LIMIT_LENGTH] ){
    if( del && del!=DBSQL_TRANSIENT ){
      if( del==DBSQL_DYNAMIC ){
        __dbsql_db_free(mem->db, (void*)z);
      }else{
        del((void*)z);
      }
    }
    __vdbe_mem_set_null(mem);
    return __error_to_parser(mem->db, DBSQL_TOOBIG);
  }

  /* The following block sets the new values of mem_t.z and mem_t.del. It
  ** also sets a flag in local variable "flags" to indicate the memory
  ** management (one of MEM_Dyn or MEM_Static).
  */
  if( del==DBSQL_TRANSIENT ){
    int64_t n_alloc = n_byte + 1;
    testcase( n_alloc==31 );
    testcase( n_alloc==32 );
    if( __vdbe_mem_clear_and_resize(mem, (int)MAX(n_alloc,32)) ){
      return DBSQL_NOMEM_BKPT;
    }
    assert( mem->z!=0 );
    memcpy(mem->z, z, n_byte);
    mem->z[n_byte] = 0;
  }else{
    __vdbe_mem_release(mem);
    mem->z = (char *)z;
    if( del==DBSQL_DYNAMIC ){
      mem->z_malloc = mem->z;
      mem->sz_malloc = __db_malloc_size(mem->db, mem->z_malloc);
      mem->x_del = 0;
    }else if( del==DBSQL_STATIC ){
      mem->x_del = del;
      flags |= MEM_Static;
    }else{
      mem->x_del = del;
      flags |= MEM_Dyn;
    }
  }
  mem->flags = flags;
  mem->n = (int)(n_byte & 0x7fffffff);
  mem->enc = DBSQL_UTF8;
  return DBSQL_OK;
}

/*
** Move data out of a btree key or data field and into a mem_t structure.
** The data is payload from the entry that cur is currently pointing
** to.  offset and amt determine what portion of the data or key to retrieve.
** The result is written into the mem element.
**
** The mem object must have been initialized.  This routine will use
** mem->z_malloc to hold the content from the btree, if possible.  New
** mem->z_malloc space will be allocated if necessary.  The calling routine
** is responsible for making sure that the mem object is eventually
** destroyed.
**
** If this routine fails for any reason (malloc returns NULL or unable
** to read from the disk) then the mem is left in an inconsistent state.
*/
int __vdbe_mem_from_btree(
  sm_cursor_t *cur,   /* Cursor pointing at record to retrieve. */
  u_int32_t offset,       /* Offset from the start of data to return bytes from. */
  u_int32_t amt,          /* Number of bytes to return. */
  mem_t *mem         /* OUT: Return data in this mem_t structure. */
){
  int rc;
  mem->flags = MEM_Null;
  testcase( amt==DBSQL_MAX_ALLOCATION_SIZE-1 );
  testcase( amt==DBSQL_MAX_ALLOCATION_SIZE );
  if( amt>=DBSQL_MAX_ALLOCATION_SIZE ){
    return DBSQL_NOMEM_BKPT;
  }
  if( (u_int64_t)amt + (u_int64_t)offset > (u_int64_t)__sm_max_record_size(cur) ){
    return DBSQL_CORRUPT_BKPT;
  }
  if( DBSQL_OK==(rc = __vdbe_mem_clear_and_resize(mem, amt+1)) ){
    rc = __sm_payload(cur, offset, amt, mem->z);
    if( rc==DBSQL_OK ){
      mem->z[amt] = 0;   /* Overrun area used when reading malformed records */
      mem->flags = MEM_Blob;
      mem->n = (int)amt;
    }else{
      __vdbe_mem_release(mem);
    }
  }
  return rc;
}
int __vdbe_mem_from_btree_zero_offset(
  sm_cursor_t *cur,   /* Cursor pointing at record to retrieve. */
  u_int32_t amt,          /* Number of bytes to return. */
  mem_t *mem         /* OUT: Return data in this mem_t structure. */
){
  u_int32_t available = 0;  /* Number of bytes available on the local btree page */
  int rc = DBSQL_OK; /* Return code */

  assert( __sm_cursor_is_valid(cur) );
  assert( !VDBE_MEM_DYNAMIC(mem) );

  /* Note: the calls to __btree_key_fetch() and __data_fetch() below assert() 
  ** that both the sm_shared_t and database handle mutexes are held. */
  assert( !__vdbe_mem_is_row_set(mem) );
  mem->z = (char *)__sm_payload_fetch(cur, &available);
  assert( mem->z!=0 );

  if( amt<=available ){
    mem->flags = MEM_Blob|MEM_Ephem;
    mem->n = (int)amt;
  }else{
    rc = __vdbe_mem_from_btree(cur, 0, amt, mem);
  }

  return rc;
}

/*
** The val argument is known to be a value other than NULL.
** Convert it into a string with encoding enc and return a pointer
** to a zero-terminated version of that string.
*/
static DBSQL_NOINLINE const void *__value_to_text(dbsql_value_t* val, u_int8_t enc){
  assert( val!=0 );
  assert( val->db==0 || dbsql_mutex_held(val->db->mutex) );
  assert( (enc&3)==(enc&~DBSQL_UTF16_ALIGNED) );
  assert( !__vdbe_mem_is_row_set(val) );
  assert( (val->flags & (MEM_Null))==0 );
  if( val->flags & (MEM_Blob|MEM_Str) ){
    if( EXPAND_BLOB(val) ) return 0;
    val->flags |= MEM_Str;
    if( val->enc != (enc & ~DBSQL_UTF16_ALIGNED) ){
      __vdbe_change_encoding(val, enc & ~DBSQL_UTF16_ALIGNED);
    }
    if( (enc & DBSQL_UTF16_ALIGNED)!=0 && 1==(1&DBSQL_PTR_TO_INT(val->z)) ){
      assert( (val->flags & (MEM_Ephem|MEM_Static))!=0 );
      if( __vdbe_mem_make_writeable(val)!=DBSQL_OK ){
        return 0;
      }
    }
    __vdbe_mem_nul_terminate(val); /* IMP: R-31275-44060 */
  }else{
    __vdbe_mem_stringify(val, enc, 0);
    assert( 0==(1&DBSQL_PTR_TO_INT(val->z)) );
  }
  assert(val->enc==(enc & ~DBSQL_UTF16_ALIGNED) || val->db==0
              || val->db->malloc_failed );
  if( val->enc==(enc & ~DBSQL_UTF16_ALIGNED) ){
    assert( __vdbe_mem_valid_str_rep(val) );
    return val->z;
  }else{
    return 0;
  }
}

/* This function is only available internally, it is not part of the
** external API. It works in a similar way to dbsql_value_text(),
** except the data returned is in the encoding specified by the second
** parameter, which must be one of DBSQL_UTF16BE, DBSQL_UTF16LE or
** DBSQL_UTF8.
**
** (2006-02-16:)  The enc value can be or-ed with DBSQL_UTF16_ALIGNED.
** If that is the case, then the result must be aligned on an even byte
** boundary.
*/
const void *__value_text(dbsql_value_t* val, u_int8_t enc){
  if( !val ) return 0;
  assert( val->db==0 || dbsql_mutex_held(val->db->mutex) );
  assert( (enc&3)==(enc&~DBSQL_UTF16_ALIGNED) );
  assert( !__vdbe_mem_is_row_set(val) );
  if( (val->flags&(MEM_Str|MEM_Term))==(MEM_Str|MEM_Term) && val->enc==enc ){
    assert( __vdbe_mem_valid_str_rep(val) );
    return val->z;
  }
  if( val->flags&MEM_Null ){
    return 0;
  }
  return __value_to_text(val, enc);
}

/* Return true if sqlit3_value object val is a string or blob value
** that uses the destructor specified in the second argument.
**
** TODO:  Maybe someday promote this interface into a published API so
** that third-party extensions can get access to it?
*/
int __value_is_of_class(const dbsql_value_t *val, void(*__x_free)(void*)){
  if( ALWAYS(val!=0)
   && ALWAYS((val->flags & (MEM_Str|MEM_Blob))!=0)
   && (val->flags & MEM_Dyn)!=0
   && val->x_del==__x_free
  ){
    return 1;
  }else{
    return 0;
  }
}

/*
** Create a new dbsql_value_t object.
*/
dbsql_value_t *__value_new(DBSQL *db){
  mem_t *p = __db_malloc_zero(db, sizeof(*p));
  if( p ){
    p->flags = MEM_Null;
    p->db = db;
  }
  return p;
}

/*
** Context object passed by __stat4_probe_set_value() through to 
** __value_new_local(). See comments above __value_new_local() for details.
*/
struct value_new_stat4_ctx_t {
  parser_t *parser;
  index_t *idx;
  unpacked_record_t **rec;
  int i_val;
};

/*
** Allocate and return a pointer to a new dbsql_value_t object. If
** the second argument to this function is NULL, the object is allocated
** by calling __value_new().
**
** Otherwise, if the second argument is non-zero, then this function is 
** being called indirectly by __stat4_probe_set_value(). If it has not
** already been allocated, allocate the unpacked_record_t structure that 
** that function will return to its caller here. Then return a pointer to
** an dbsql_value_t within the unpacked_record_t.a[] array.
*/
static dbsql_value_t *__value_new_local(DBSQL *db, struct value_new_stat4_ctx_t *p){
#ifdef DBSQL_ENABLE_STAT4
  if( p ){
    unpacked_record_t *rec = p->rec[0];

    if( rec==0 ){
      index_t *idx = p->idx;      /* index_t being probed */
      int64_t n_byte;                  /* Bytes of space to allocate */
      int i;                      /* Counter variable */
      int n_col = idx->n_column;   /* Number of index columns including rowid */
  
      n_byte = sizeof(mem_t) * n_col + ROUND8(sizeof(unpacked_record_t));
      rec = (unpacked_record_t*)__db_malloc_zero(db, n_byte);
      if( rec ){
        rec->key_info = __key_info_of_index(p->parser, idx);
        if( rec->key_info ){
          assert( rec->key_info->n_all_field==n_col );
          assert( rec->key_info->enc==ENC(db) );
          rec->mems = (mem_t *)((u_int8_t*)rec + ROUND8(sizeof(unpacked_record_t)));
          for(i=0; i<n_col; i++){
            rec->mems[i].flags = MEM_Null;
            rec->mems[i].db = db;
          }
        }else{
          __db_free_nn(db, rec);
          rec = 0;
        }
      }
      if( rec==0 ) return 0;
      p->rec[0] = rec;
    }
  
    rec->n_field = p->i_val+1;
    __vdbe_mem_set_null(&rec->mems[p->i_val]);
    return &rec->mems[p->i_val];
  }
#else
  UNUSED_PARAMETER(p);
#endif /* defined(DBSQL_ENABLE_STAT4) */
  return __value_new(db);
}

/*
** The expression object indicated by the second argument is guaranteed
** to be a scalar SQL function. If
**
**   * all function arguments are SQL literals,
**   * one of the DBSQL_FUNC_CONSTANT or _SLOCHNG function flags is set, and
**   * the DBSQL_FUNC_NEEDCOLL function flag is not set,
**
** then this routine attempts to invoke the SQL function. Assuming no
** error occurs, output parameter (*val) is set to point to a value 
** object containing the result before returning DBSQL_OK.
**
** Affinity aff is applied to the result of the function before returning.
** If the result is a text value, the dbsql_value_t object uses encoding 
** enc.
**
** If the conditions above are not met, this function returns DBSQL_OK
** and sets (*val) to NULL. Or, if an error occurs, (*val) is set to
** NULL and an dbsql error code returned.
*/
#ifdef DBSQL_ENABLE_STAT4
static int VALUE_FROM_FUNCTION(
  DBSQL *db,                    /* The database connection */
  const expr_t *p,                  /* The expression to evaluate */
  u_int8_t enc,                         /* Encoding to use */
  u_int8_t aff,                         /* Affinity to use */
  dbsql_value_t **pp_val,          /* Write the new value here */
  struct value_new_stat4_ctx_t *p_ctx   /* Second argument for __value_new_local() */
){
  dbsql_func_t ctx;            /* Context object for function invocation */
  dbsql_value_t **vals = 0;      /* Function arguments */
  int n_val = 0;                   /* Number of function arguments */
  func_def_t *func = 0;             /* Function definition */
  dbsql_value_t *val = 0;        /* New value */
  int rc = DBSQL_OK;             /* Return code */
  expr_list_t *list = 0;            /* Function arguments */
  int i;                          /* Iterator variable */

  assert( p_ctx!=0 );
  assert( (p->flags & EP_TokenOnly)==0 );
  assert( EXPR_USE_X_LIST(p) );
  list = p->x.list;
  if( list ) n_val = list->n_expr;
  assert( !EXPR_HAS_PROPERTY(p, EP_IntValue) );
  func = __find_function(db, p->u.token, n_val, enc, 0);
#ifdef DBSQL_ENABLE_UNKNOWN_SQL_FUNCTION
  if( func==0 ) return DBSQL_OK;
#endif
  assert( func );
  if( (func->func_flags & (DBSQL_FUNC_CONSTANT|DBSQL_FUNC_SLOCHNG))==0
   || (func->func_flags & (DBSQL_FUNC_NEEDCOLL|DBSQL_FUNC_RUNONLY))!=0
  ){
    return DBSQL_OK;
  }

  if( list ){
    vals = (dbsql_value_t**)__db_malloc_zero(db, sizeof(vals[0]) * n_val);
    if( vals==0 ){
      rc = DBSQL_NOMEM_BKPT;
      goto value_from_function_out;
    }
    for(i=0; i<n_val; i++){
      rc = __stat4_value_from_expr(p_ctx->parser, list->a[i].expr, aff,
                                     &vals[i]);
      if( vals[i]==0 || rc!=DBSQL_OK ) goto value_from_function_out;
    }
  }

  val = __value_new_local(db, p_ctx);
  if( val==0 ){
    rc = DBSQL_NOMEM_BKPT;
    goto value_from_function_out;
  }

  memset(&ctx, 0, sizeof(ctx));
  ctx.out = val;
  ctx.func = func;
  ctx.enc = ENC(db);
  func->s_func(&ctx, n_val, vals);
  if( ctx.isError ){
    rc = ctx.isError;
    __error_msg(p_ctx->parser, "%s", dbsql_value_text(val));
  }else{
    __value_apply_affinity(val, aff, DBSQL_UTF8);
    assert( rc==DBSQL_OK );
    rc = __vdbe_change_encoding(val, enc);
    if( NEVER(rc==DBSQL_OK && __vdbe_mem_too_big(val)) ){
      rc = DBSQL_TOOBIG;
      p_ctx->parser->n_err++;
    }
  }

 value_from_function_out:
  if( rc!=DBSQL_OK ){
    val = 0;
    p_ctx->parser->rc = rc;
  }
  if( vals ){
    for(i=0; i<n_val; i++){
      __value_free(vals[i]);
    }
    __db_free_nn(db, vals);
  }

  *pp_val = val;
  return rc;
}
#else
# define VALUE_FROM_FUNCTION(a,b,c,d,e,f) DBSQL_OK
#endif /* defined(DBSQL_ENABLE_STAT4) */

/*
** Extract a value from the supplied expression in the manner described
** above __value_from_expr(). Allocate the dbsql_value_t object
** using __value_new_local().
**
** If ctx is NULL and an error occurs after the dbsql_value_t object
** has been allocated, it is freed before returning. Or, if ctx is not
** NULL, it is assumed that the caller will free any allocated object
** in all cases.
*/
static int __value_from_expr_local(
  DBSQL *db,                    /* The database connection */
  const expr_t *expr,              /* The expression to evaluate */
  u_int8_t enc,                         /* Encoding to use */
  u_int8_t affinity,                    /* Affinity to use */
  dbsql_value_t **pp_val,          /* Write the new value here */
  struct value_new_stat4_ctx_t *ctx   /* Second argument for __value_new_local() */
){
  int op;
  char *z_val = 0;
  dbsql_value_t *val = 0;
  int neg_int = 1;
  const char *neg = "";
  int rc = DBSQL_OK;

  assert( expr!=0 );
  while( (op = expr->op)==TK_UPLUS || op==TK_SPAN ) expr = expr->left;
  if( op==TK_REGISTER ) op = expr->op2;

  /* Compressed expressions only appear when parsing the DEFAULT clause
  ** on a table column definition, and hence only when ctx==0.  This
  ** check ensures that an EP_TokenOnly expression is never passed down
  ** into VALUE_FROM_FUNCTION(). */
  assert( (expr->flags & EP_TokenOnly)==0 || ctx==0 );

  if( op==TK_CAST ){
    u_int8_t aff;
    assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
    aff = __affinity_type(expr->u.token,0);
    rc = __value_from_expr_local(db, expr->left, enc, aff, pp_val, ctx);
    testcase( rc!=DBSQL_OK );
    if( *pp_val ){
#ifdef DBSQL_ENABLE_STAT4
      rc = EXPAND_BLOB(*pp_val);
#else
      /* zero-blobs only come from functions, not literal values.  And
      ** functions are only processed under STAT4 */
      assert( (pp_val[0][0].flags & MEM_Zero)==0 );
#endif
      __vdbe_mem_cast(*pp_val, aff, enc);
      __value_apply_affinity(*pp_val, affinity, enc);
    }
    return rc;
  }

  /* Handle negative integers in a single step.  This is needed in the
  ** case when the value is -9223372036854775808. Except - do not do this
  ** for hexadecimal literals.  */
  if( op==TK_UMINUS ){
    expr_t *left = expr->left;
    if( (left->op==TK_INTEGER || left->op==TK_FLOAT) ){
      if( EXPR_HAS_PROPERTY(left, EP_IntValue)
       || left->u.token[0]!='0' || (left->u.token[1] & ~0x20)!='X'
      ){
        expr = left;
        op = expr->op;
        neg_int = -1;
        neg = "-";
      }
    }
  }

  if( op==TK_STRING || op==TK_FLOAT || op==TK_INTEGER ){
    val = __value_new_local(db, ctx);
    if( val==0 ) goto no_mem;
    if( EXPR_HAS_PROPERTY(expr, EP_IntValue) ){
      __vdbe_mem_set_int64(val, (int64_t)expr->u.i_value*neg_int);
    }else{
      int64_t i_val;
      if( op==TK_INTEGER && 0==__dec_or_hex_to_i64(expr->u.token, &i_val) ){
        __vdbe_mem_set_int64(val, i_val*neg_int);
      }else{
        z_val = __m_printf(db, "%s%s", neg, expr->u.token);
        if( z_val==0 ) goto no_mem;
        __value_set_str(val, -1, z_val, DBSQL_UTF8, DBSQL_DYNAMIC);
      }
    }
    if( affinity==DBSQL_AFF_BLOB ){
      if( op==TK_FLOAT ){
        assert( val && val->z && val->flags==(MEM_Str|MEM_Term) );
        __ato_f(val->z, &val->u.r);
        val->flags = MEM_Real;
      }else if( op==TK_INTEGER ){
        /* This case is required by -9223372036854775808 and other strings
        ** that look like integers but cannot be handled by the
        ** __dec_or_hex_to_i64() call above.  */
        __value_apply_affinity(val, DBSQL_AFF_NUMERIC, DBSQL_UTF8);
      }
    }else{
      __value_apply_affinity(val, affinity, DBSQL_UTF8);
    }
    assert( (val->flags & MEM_IntReal)==0 );
    if( val->flags & (MEM_Int|MEM_IntReal|MEM_Real) ){
      testcase( val->flags & MEM_Int );
      testcase( val->flags & MEM_Real );
      val->flags &= ~MEM_Str;
    }
    if( enc!=DBSQL_UTF8 ){
      rc = __vdbe_change_encoding(val, enc);
    }
  }else if( op==TK_UMINUS ) {
    /* This branch happens for multiple negative signs.  Ex: -(-5) */
    if( DBSQL_OK==__value_from_expr_local(db,expr->left,enc,affinity,&val,ctx) 
     && val!=0
    ){
      __vdbe_mem_numerify(val);
      if( val->flags & MEM_Real ){
        val->u.r = -val->u.r;
      }else if( val->u.i==SMALLEST_INT64 ){
#ifndef DBSQL_OMIT_FLOATING_POINT
        val->u.r = -(double)SMALLEST_INT64;
#else
        val->u.r = LARGEST_INT64;
#endif
        MEM_SET_TYPE_FLAG(val, MEM_Real);
      }else{
        val->u.i = -val->u.i;
      }
      __value_apply_affinity(val, affinity, enc);
    }
  }else if( op==TK_NULL ){
    val = __value_new_local(db, ctx);
    if( val==0 ) goto no_mem;
    __vdbe_mem_set_null(val);
  }
#ifndef DBSQL_OMIT_BLOB_LITERAL
  else if( op==TK_BLOB ){
    int n_val;
    assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
    assert( expr->u.token[0]=='x' || expr->u.token[0]=='X' );
    assert( expr->u.token[1]=='\'' );
    val = __value_new_local(db, ctx);
    if( !val ) goto no_mem;
    z_val = &expr->u.token[2];
    n_val = __strlen30(z_val)-1;
    assert( z_val[n_val]=='\'' );
    __vdbe_mem_set_str(val, __hex_to_blob(db, z_val, n_val), n_val/2,
                         0, DBSQL_DYNAMIC);
  }
#endif
#ifdef DBSQL_ENABLE_STAT4
  else if( op==TK_FUNCTION && ctx!=0 ){
    rc = VALUE_FROM_FUNCTION(db, expr, enc, affinity, &val, ctx);
  }
#endif
  else if( op==TK_TRUEFALSE ){
    assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
    val = __value_new_local(db, ctx);
    if( val ){
      val->flags = MEM_Int;
      val->u.i = expr->u.token[4]==0;
      __value_apply_affinity(val, affinity, enc);
    }
  }

  *pp_val = val;
  return rc;

no_mem:
#ifdef DBSQL_ENABLE_STAT4
  if( ctx==0 || NEVER(ctx->parser->n_err==0) )
#endif
    __oom_fault(db);
  __dbsql_db_free(db, z_val);
  assert( *pp_val==0 );
#ifdef DBSQL_ENABLE_STAT4
  if( ctx==0 ) __value_free(val);
#else
  assert( ctx==0 ); __value_free(val);
#endif
  return DBSQL_NOMEM_BKPT;
}

/*
** Create a new dbsql_value_t object, containing the value of expr.
**
** This only works for very simple expressions that consist of one constant
** token (i.e. "5", "5.1", "'a string'"). If the expression can
** be converted directly into a value, then the value is allocated and
** a pointer written to *val. The caller is responsible for deallocating
** the value by passing it to __value_free() later on. If the expression
** cannot be converted to a value, then *val is set to NULL.
*/
int __value_from_expr(
  DBSQL *db,              /* The database connection */
  const expr_t *expr,        /* The expression to evaluate */
  u_int8_t enc,                   /* Encoding to use */
  u_int8_t affinity,              /* Affinity to use */
  dbsql_value_t **val     /* Write the new value here */
){
  return expr ? __value_from_expr_local(db, expr, enc, affinity, val, 0) : 0;
}

#ifdef DBSQL_ENABLE_STAT4
/*
** Attempt to extract a value from expr and use it to construct *val.
**
** If alloc is not NULL, then an unpacked_record_t object is created for
** alloc if one does not exist and the new value is added to the
** unpacked_record_t object.
**
** A value is extracted in the following cases:
**
**  * (expr==0). In this case the value is assumed to be an SQL NULL,
**
**  * The expression is a bound variable, and this is a reprepare, or
**
**  * The expression is a literal value.
**
** On success, *val is made to point to the extracted value.  The caller
** is responsible for ensuring that the value is eventually freed.
*/
static int __stat4_value_from_expr_local(
  parser_t *parser,                  /* parser_t context */
  expr_t *expr,                    /* The expression to extract a value from */
  u_int8_t affinity,                    /* Affinity to use */
  struct value_new_stat4_ctx_t *alloc,/* How to allocate space.  Or NULL */
  dbsql_value_t **pp_val           /* OUT: New value object (or NULL) */
){
  int rc = DBSQL_OK;
  dbsql_value_t *val = 0;
  DBSQL *db = parser->db;

  /* Skip over any TK_COLLATE nodes */
  expr = __expr_skip_collate(expr);

  assert( expr==0 || expr->op!=TK_REGISTER || expr->op2!=TK_VARIABLE );
  if( !expr ){
    val = __value_new_local(db, alloc);
    if( val ){
      __vdbe_mem_set_null((mem_t*)val);
    }
  }else if( expr->op==TK_VARIABLE && (db->flags & DBSQL_EnableQPSG)==0 ){
    vdbe_t *v;
    int i_bind_var = expr->i_column;
    __vdbe_set_varmask(parser->vdbe, i_bind_var);
    if( (v = parser->reprepare)!=0 ){
      val = __value_new_local(db, alloc);
      if( val ){
        rc = __vdbe_mem_copy((mem_t*)val, &v->vars[i_bind_var-1]);
        __value_apply_affinity(val, affinity, ENC(db));
        val->db = parser->db;
      }
    }
  }else{
    rc = __value_from_expr_local(db, expr, ENC(db), affinity, &val, alloc);
  }

  assert( val==0 || val->db==db );
  *pp_val = val;
  return rc;
}

/*
** This function is used to allocate and populate unpacked_record_t 
** structures intended to be compared against sample index keys stored 
** in the dbsql_stat4 table.
**
** A single call to this function populates zero or more fields of the
** record starting with field i_val (fields are numbered from left to
** right starting with 0). A single field is populated if:
**
**  * (expr==0). In this case the value is assumed to be an SQL NULL,
**
**  * The expression is a bound variable, and this is a reprepare, or
**
**  * The __value_from_expr() function is able to extract a value 
**    from the expression (i.e. the expression is a literal value).
**
** Or, if expr is a TK_VECTOR, one field is populated for each of the
** vector components that match either of the two latter criteria listed
** above.
**
** Before any value is appended to the record, the affinity of the 
** corresponding column within index idx is applied to it. Before
** this function returns, output parameter *pn_extract is set to the
** number of values appended to the record.
**
** When this function is called, *rec must either point to an object
** allocated by an earlier call to this function, or must be NULL. If it
** is NULL and a value can be successfully extracted, a new unpacked_record_t
** is allocated (and *rec set to point to it) before returning.
**
** Unless an error is encountered, DBSQL_OK is returned. It is not an
** error if a value cannot be extracted from expr. If an error does
** occur, an dbsql error code is returned.
*/
int __stat4_probe_set_value(
  parser_t *parser,                  /* parser_t context */
  index_t *idx,                    /* index_t being probed */
  unpacked_record_t **rec,         /* IN/OUT: Probe record */
  expr_t *expr,                    /* The expression to extract a value from */
  int n_elem,                      /* Maximum number of values to append */
  int i_val,                       /* Array element to populate */
  int *pn_extract                  /* OUT: Values appended to the record */
){
  int rc = DBSQL_OK;
  int n_extract = 0;

  if( expr==0 || expr->op!=TK_SELECT ){
    int i;
    struct value_new_stat4_ctx_t alloc;

    alloc.parser = parser;
    alloc.idx = idx;
    alloc.rec = rec;

    for(i=0; i<n_elem; i++){
      dbsql_value_t *val = 0;
      expr_t *elem = (expr ? __vector_field_subexpr(expr, i) : 0);
      u_int8_t aff = __index_column_affinity(parser->db, idx, i_val+i);
      alloc.i_val = i_val+i;
      rc = __stat4_value_from_expr_local(parser, elem, aff, &alloc, &val);
      if( !val ) break;
      n_extract++;
    }
  }

  *pn_extract = n_extract;
  return rc;
}

/*
** Attempt to extract a value from expression expr using the methods
** as described for __stat4_probe_set_value() above. 
**
** If successful, set *val to point to a new value object and return 
** DBSQL_OK. If no value can be extracted, but no other error occurs
** (e.g. OOM), return DBSQL_OK and set *val to NULL. Or, if an error
** does occur, return an dbsql error code. The final value of *val
** is undefined in this case.
*/
int __stat4_value_from_expr(
  parser_t *parser,                  /* parser_t context */
  expr_t *expr,                    /* The expression to extract a value from */
  u_int8_t affinity,                    /* Affinity to use */
  dbsql_value_t **val           /* OUT: New value object (or NULL) */
){
  return __stat4_value_from_expr_local(parser, expr, affinity, 0, val);
}

/*
** Extract the i_col-th column from the n_rec-byte record in rec.  Write
** the column value into *val.  If *val is initially NULL then a new
** dbsql_value_t object is allocated.
**
** If *val is initially NULL then the caller is responsible for 
** ensuring that the value written into *val is eventually freed.
**
** If the buffer does not contain a well-formed record, this routine may
** read several bytes past the end of the buffer. Callers must therefore
** ensure that any buffer which may contain a corrupt record is padded
** with at least 8 bytes of addressable memory.
*/
int __stat4_column(
  DBSQL *db,                    /* Database handle */
  const void *rec,               /* Pointer to buffer containing record */
  int n_rec,                       /* Size of buffer rec in bytes */
  int i_col,                       /* column_t to extract */
  dbsql_value_t **val           /* OUT: Extracted value */
){
  u_int32_t t = 0;                      /* a column type code */
  u_int32_t n_hdr;                       /* Size of the header in the record */
  u_int32_t i_hdr;                       /* Next unread header byte */
  int64_t i_field;                     /* Next unread data byte */
  u_int32_t sz_field = 0;                /* Size of the current data field */
  int i;                          /* column_t index */
  u_int8_t *a = (u_int8_t*)rec;              /* Typecast byte array */
  mem_t *mem = *val;             /* Write result into this mem_t object */

  assert( i_col>0 );
  i_hdr = GET_VARINT32(a, n_hdr);
  if( n_hdr>(u_int32_t)n_rec || i_hdr>=n_hdr ) return DBSQL_CORRUPT_BKPT;
  i_field = n_hdr;
  for(i=0; i<=i_col; i++){
    i_hdr += GET_VARINT32(&a[i_hdr], t);
    testcase( i_hdr==n_hdr );
    testcase( i_hdr==n_hdr+1 );
    if( i_hdr>n_hdr ) return DBSQL_CORRUPT_BKPT;
    sz_field = __vdbe_serial_type_len(t);
    i_field += sz_field;
  }
  testcase( i_field==n_rec );
  testcase( i_field==n_rec+1 );
  if( i_field>n_rec ) return DBSQL_CORRUPT_BKPT;
  if( mem==0 ){
    mem = *val = __value_new(db);
    if( mem==0 ) return DBSQL_NOMEM_BKPT;
  }
  __vdbe_serial_get(&a[i_field-sz_field], t, mem);
  mem->enc = ENC(db);
  return DBSQL_OK;
}

/*
** Unless it is NULL, the argument must be an unpacked_record_t object returned
** by an earlier call to __stat4_probe_set_value(). This call deletes
** the object.
*/
void __stat4_probe_free(unpacked_record_t *rec){
  if( rec ){
    int i;
    int n_col = rec->key_info->n_all_field;
    mem_t *mems = rec->mems;
    DBSQL *db = mems[0].db;
    for(i=0; i<n_col; i++){
      __vdbe_mem_release(&mems[i]);
    }
    __key_info_unref(rec->key_info);
    __db_free_nn(db, rec);
  }
}
#endif /* ifdef DBSQL_ENABLE_STAT4 */

/*
** Change the string value of an dbsql_value_t object
*/
void __value_set_str(
  dbsql_value_t *v,     /* Value to be set */
  int n,                /* Length of string z */
  const void *z,        /* Text of the new string */
  u_int8_t enc,               /* Encoding to use */
  void (*del)(void*)   /* Destructor for the string */
){
  if( v ) __vdbe_mem_set_str((mem_t *)v, z, n, enc, del);
}

/*
** Free an dbsql_value_t object
*/
void __value_free(dbsql_value_t *v){
  if( !v ) return;
  __vdbe_mem_release((mem_t *)v);
  __db_free_nn(((mem_t*)v)->db, v);
}

/*
** The __value_bytes() routine returns the number of bytes in the
** dbsql_value_t object assuming that it uses the encoding "enc".
** The __value_bytes_local() routine is a helper function.
*/
static DBSQL_NOINLINE int __value_bytes_local(dbsql_value_t *val, u_int8_t enc){
  return __value_to_text(val, enc)!=0 ? val->n : 0;
}
int __value_bytes(dbsql_value_t *val, u_int8_t enc){
  mem_t *p = (mem_t*)val;
  assert( (p->flags & MEM_Null)==0 || (p->flags & (MEM_Str|MEM_Blob))==0 );
  if( (p->flags & MEM_Str)!=0 && val->enc==enc ){
    return p->n;
  }
  if( (p->flags & MEM_Str)!=0 && enc!=DBSQL_UTF8 && val->enc!=DBSQL_UTF8 ){
    return p->n;
  }
  if( (p->flags & MEM_Blob)!=0 ){
    if( p->flags & MEM_Zero ){
      return p->n + p->u.n_zero;
    }else{
      return p->n;
    }
  }
  if( p->flags & MEM_Null ) return 0;
  return __value_bytes_local(val, enc);
}
