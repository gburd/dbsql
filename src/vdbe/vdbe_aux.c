/*
** 2003 September 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used for creating, destroying, and populating
** a VDBE (or an "dbsql_stmt_t" as it is known to the outside world.)
*/
#include "dbsql_int.h"
#include "vdbe_int.h"

/* Forward references */
static void __free_ephemeral_function(DBSQL *db, func_def_t *def);
static void __vdbe_free_op_array(DBSQL *, op_t *, int);

/*
** Create a new virtual database engine.
*/
vdbe_t *__vdbe_create(parser_t *parser){
  DBSQL *db = parser->db;
  vdbe_t *p;
  p = __db_malloc_raw_nn(db, sizeof(vdbe_t) );
  if( p==0 ) return 0;
  memset(&p->ops, 0, sizeof(vdbe_t)-offsetof(vdbe_t,ops));
  p->db = db;
  if( db->vdbe ){
    db->vdbe->v_prev = &p->v_next;
  }
  p->v_next = db->vdbe;
  p->v_prev = &db->vdbe;
  db->vdbe = p;
  assert( p->vdbe_state==VDBE_INIT_STATE );
  p->parser = parser;
  parser->vdbe = p;
  assert( parser->labels==0 );
  assert( parser->n_label==0 );
  assert( p->n_op_alloc==0 );
  assert( parser->sz_op_alloc==0 );
  __vdbe_add_op2(p, OP_Init, 0, 1);
  return p;
}

/*
** Return the parser_t object that owns a vdbe_t object.
*/
parser_t *__vdbe_parser(vdbe_t *p){
  return p->parser;
}

/*
** Change the error string stored in vdbe_t.errmsg
*/
void __vdbe_error(vdbe_t *p, const char *format, ...){
  va_list ap;
  __dbsql_db_free(p->db, p->errmsg);
  va_start(ap, format);
  p->errmsg = __vm_printf(p->db, format, ap);
  va_end(ap);
}

/*
** Remember the SQL string for a prepared statement.
*/
void __vdbe_set_sql(vdbe_t *p, const char *z, int n, u_int8_t prep_flags){
  if( p==0 ) return;
  p->prep_flags = prep_flags;
  if( (prep_flags & DBSQL_PREPARE_SAVESQL)==0 ){
    p->expmask = 0;
  }
  assert( p->z_sql==0 );
  p->z_sql = __db_str_n_dup(p->db, z, n);
}

#ifdef DBSQL_ENABLE_NORMALIZE
/*
** Add a new element to the vdbe_t->dbl_str list.
*/
void __vdbe_add_dblquote_str(DBSQL *db, vdbe_t *p, const char *z){
  if( p ){
    int n = __strlen30(z);
    dblquote_str_t *str = __db_malloc_raw_nn(db,
                            sizeof(*str)+n+1-sizeof(str->z));
    if( str ){
      str->next_str = p->dbl_str;
      p->dbl_str = str;
      memcpy(str->z, z, n+1);
    }
  }
}
#endif

#ifdef DBSQL_ENABLE_NORMALIZE
/*
** id of length n_id is a double-quoted identifier.  Check to see if
** that identifier is really used as a string literal.
*/
int __vdbe_uses_double_quoted_string(
  vdbe_t *vdbe,            /* The prepared statement */
  const char *id         /* The double-quoted identifier, already dequoted */
){
  dblquote_str_t *str;
  assert( id!=0 );
  if( vdbe->dbl_str==0 ) return 0;
  for(str=vdbe->dbl_str; str; str=str->next_str){
    if( strcmp(id, str->z)==0 ) return 1;
  }
  return 0;
}
#endif

/*
** Swap byte-code between two VDBE structures.
**
** This happens after b was previously run and returned
** DBSQL_SCHEMA.  The statement was then reprepared in a.
** This routine transfers the new bytecode in a over to b
** so that b can be run again.  The old b byte code is
** moved back to a so that it will be cleaned up when a is
** finalized.
*/
void __vdbe_swap(vdbe_t *a, vdbe_t *b){
  vdbe_t tmp, *p_tmp, **pp_tmp;
  char *z_tmp;
  assert( a->db==b->db );
  tmp = *a;
  *a = *b;
  *b = tmp;
  p_tmp = a->v_next;
  a->v_next = b->v_next;
  b->v_next = p_tmp;
  pp_tmp = a->v_prev;
  a->v_prev = b->v_prev;
  b->v_prev = pp_tmp;
  z_tmp = a->z_sql;
  a->z_sql = b->z_sql;
  b->z_sql = z_tmp;
#ifdef DBSQL_ENABLE_NORMALIZE
  z_tmp = a->norm_sql;
  a->norm_sql = b->norm_sql;
  b->norm_sql = z_tmp;
#endif
  b->expmask = a->expmask;
  b->prep_flags = a->prep_flags;
  memcpy(b->counters, a->counters, sizeof(b->counters));
  b->counters[DBSQL_STMTSTATUS_REPREPARE]++;
}

/*
** Resize the vdbe_t.ops array so that it is at least n_op elements larger
** than its current size. n_op is guaranteed to be less than or equal
** to 1024/sizeof(op_t).
**
** If an out-of-memory error occurs while resizing the array, return
** DBSQL_NOMEM. In this case vdbe_t.ops and vdbe_t.n_op_alloc remain
** unchanged (this is so that any opcodes already allocated can be
** correctly deallocated along with the rest of the vdbe_t).
*/
static int __grow_op_array(vdbe_t *v, int n_op){
  vdbe_op_t *p_new;
  parser_t *p = v->parser;

  /* The DBSQL_TEST_REALLOC_STRESS compile-time option is designed to force
  ** more frequent reallocs and hence provide more opportunities for
  ** simulated OOM faults.  DBSQL_TEST_REALLOC_STRESS is generally used
  ** during testing only.  with_t DBSQL_TEST_REALLOC_STRESS grow the op array
  ** by the minimum* amount required until the size reaches 512.  Normal
  ** operation (without DBSQL_TEST_REALLOC_STRESS) is to double the current
  ** size of the op array or add 1KB of space, whichever is smaller. */
#ifdef DBSQL_TEST_REALLOC_STRESS
  int64_t n_new = (v->n_op_alloc>=512 ? 2*(int64_t)v->n_op_alloc
                        : (int64_t)v->n_op_alloc+n_op);
#else
  int64_t n_new = (v->n_op_alloc ? 2*(int64_t)v->n_op_alloc
                        : (int64_t)(1024/sizeof(op_t)));
  UNUSED_PARAMETER(n_op);
#endif

  /* Ensure that the size of a VDBE does not grow too large */
  if( n_new > p->db->limits[DBSQL_LIMIT_VDBE_OP] ){
    __oom_fault(p->db);
    return DBSQL_NOMEM;
  }

  assert( n_op<=(int)(1024/sizeof(op_t)) );
  assert( n_new>=(v->n_op_alloc+n_op) );
  p_new = __db_realloc(p->db, v->ops, n_new*sizeof(op_t));
  if( p_new ){
    p->sz_op_alloc = __db_malloc_size(p->db, p_new);
    v->n_op_alloc = p->sz_op_alloc/sizeof(op_t);
    v->ops = p_new;
  }
  return (p_new ? DBSQL_OK : DBSQL_NOMEM_BKPT);
}

#ifdef DBSQL_DEBUG
/* This routine is just a convenient place to set a breakpoint that will
** fire after each opcode is inserted and displayed using
** "PRAGMA vdbe_addoptrace=on".  Parameters "pc" (program counter) and
** op are available to make the breakpoint conditional.
**
** Other useful labels for breakpoints include:
**   test_trace_breakpoint(pc,op)
**   __corrupt_error(lineno)
**   __misuse_error(lineno)
**   __cantopen_error(lineno)
*/
static void test_addop_breakpoint(int pc, op_t *op){
  static u_int64_t n = 0;
  (void)pc;
  (void)op;
  n++;
  if( n==LARGEST_UINT64 ) abort(); /* so that n is used, preventing a warning */
}
#endif

/*
** Slow paths for __vdbe_add_op3() and __vdbe_add_op4_int() for the
** unusual case when we need to increase the size of the vdbe_t.ops[] array
** before adding the new opcode.
*/
static DBSQL_NOINLINE int __grow_op3(vdbe_t *p, int op, int p1, int p2, int p3){
  assert( p->n_op_alloc<=p->n_op );
  if( __grow_op_array(p, 1) ) return 1;
  assert( p->n_op_alloc>p->n_op );
  return __vdbe_add_op3(p, op, p1, p2, p3);
}
static DBSQL_NOINLINE int __add_op4_int_slow(
  vdbe_t *p,            /* Add the opcode to this VM */
  int op,             /* The new opcode */
  int p1,             /* The P1 operand */
  int p2,             /* The P2 operand */
  int p3,             /* The P3 operand */
  int p4              /* The P4 operand as an integer */
){
  int addr = __vdbe_add_op3(p, op, p1, p2, p3);
  if( p->db->malloc_failed==0 ){
    vdbe_op_t *p_op = &p->ops[addr];
    p_op->p4type = P4_INT32;
    p_op->p4.i = p4;
  }
  return addr;
}


/*
** Add a new instruction to the list of instructions current in the
** VDBE.  Return the address of the new instruction.
**
** Parameters:
**
**    p               Pointer to the VDBE
**
**    op              The opcode for this instruction
**
**    p1, p2, p3, p4  Operands
*/
int __vdbe_add_op0(vdbe_t *p, int op){
  return __vdbe_add_op3(p, op, 0, 0, 0);
}
int __vdbe_add_op1(vdbe_t *p, int op, int p1){
  return __vdbe_add_op3(p, op, p1, 0, 0);
}
int __vdbe_add_op2(vdbe_t *p, int op, int p1, int p2){
  return __vdbe_add_op3(p, op, p1, p2, 0);
}
int __vdbe_add_op3(vdbe_t *p, int op, int p1, int p2, int p3){
  int i;
  vdbe_op_t *p_op;

  i = p->n_op;
  assert( p->vdbe_state==VDBE_INIT_STATE );
  assert( op>=0 && op<0xff );
  if( p->n_op_alloc<=i ){
    return __grow_op3(p, op, p1, p2, p3);
  }
  assert( p->ops!=0 );
  p->n_op++;
  p_op = &p->ops[i];
  assert( p_op!=0 );
  p_op->opcode = (u_int8_t)op;
  p_op->p5 = 0;
  p_op->p1 = p1;
  p_op->p2 = p2;
  p_op->p3 = p3;
  p_op->p4.p = 0;
  p_op->p4type = P4_NOTUSED;

  /* Replicate this logic in __vdbe_add_op4_int()
  ** vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv   */
#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
  p_op->comment = 0;
#endif
#if defined(DBSQL_ENABLE_STMT_SCANSTATUS) || defined(VDBE_PROFILE)
  p_op->n_exec = 0;
  p_op->n_cycle = 0;
#endif
#ifdef DBSQL_DEBUG
  if( p->db->flags & DBSQL_VdbeAddopTrace ){
    __vdbe_print_op(0, i, &p->ops[i]);
    test_addop_breakpoint(i, &p->ops[i]);
  }
#endif
#ifdef DBSQL_VDBE_COVERAGE
  p_op->i_src_line = 0;
#endif
  /* ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  ** Replicate in __vdbe_add_op4_int() */

  return i;
}
int __vdbe_add_op4_int(
  vdbe_t *p,            /* Add the opcode to this VM */
  int op,             /* The new opcode */
  int p1,             /* The P1 operand */
  int p2,             /* The P2 operand */
  int p3,             /* The P3 operand */
  int p4              /* The P4 operand as an integer */
){
  int i;
  vdbe_op_t *p_op;

  i = p->n_op;
  if( p->n_op_alloc<=i ){
    return __add_op4_int_slow(p, op, p1, p2, p3, p4);
  }
  p->n_op++;
  p_op = &p->ops[i];
  assert( p_op!=0 );
  p_op->opcode = (u_int8_t)op;
  p_op->p5 = 0;
  p_op->p1 = p1;
  p_op->p2 = p2;
  p_op->p3 = p3;
  p_op->p4.i = p4;
  p_op->p4type = P4_INT32;

  /* Replicate this logic in __vdbe_add_op3()
  ** vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv   */
#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
  p_op->comment = 0;
#endif
#if defined(DBSQL_ENABLE_STMT_SCANSTATUS) || defined(VDBE_PROFILE)
  p_op->n_exec = 0;
  p_op->n_cycle = 0;
#endif
#ifdef DBSQL_DEBUG
  if( p->db->flags & DBSQL_VdbeAddopTrace ){
    __vdbe_print_op(0, i, &p->ops[i]);
    test_addop_breakpoint(i, &p->ops[i]);
  }
#endif
#ifdef DBSQL_VDBE_COVERAGE
  p_op->i_src_line = 0;
#endif
  /* ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  ** Replicate in __vdbe_add_op3() */

  return i;
}

/* Generate code for an unconditional jump to instruction i_dest
*/
int __vdbe_goto(vdbe_t *p, int i_dest){
  return __vdbe_add_op3(p, OP_Goto, 0, i_dest, 0);
}

/* Generate code to cause the string str to be loaded into
** register i_dest
*/
int __vdbe_load_string(vdbe_t *p, int i_dest, const char *str){
  return __vdbe_add_op4(p, OP_String8, 0, i_dest, 0, str, 0);
}

/*
** Generate code that initializes multiple registers to string or integer
** constants.  The registers begin with i_dest and increase consecutively.
** One register is initialized for each characgter in types[].  For each
** "s" character in types[], the register is a string if the argument is
** not NULL, or OP_Null if the value is a null pointer.  For each "i" character
** in types[], the register is initialized to an integer.
**
** If the input string does not end with "X" then an OP_ResultRow instruction
** is generated for the values inserted.
*/
void __vdbe_multi_load(vdbe_t *p, int i_dest, const char *types, ...){
  va_list ap;
  int i;
  char c;
  va_start(ap, types);
  for(i=0; (c = types[i])!=0; i++){
    if( c=='s' ){
      const char *z = va_arg(ap, const char*);
      __vdbe_add_op4(p, z==0 ? OP_Null : OP_String8, 0, i_dest+i, 0, z, 0);
    }else if( c=='i' ){
      __vdbe_add_op2(p, OP_Integer, va_arg(ap, int), i_dest+i);
    }else{
      goto skip_op_resultrow;
    }
  }
  __vdbe_add_op2(p, OP_ResultRow, i_dest, i);
skip_op_resultrow:
  va_end(ap);
}

/*
** Add an opcode that includes the p4 value as a pointer.
*/
int __vdbe_add_op4(
  vdbe_t *p,            /* Add the opcode to this VM */
  int op,             /* The new opcode */
  int p1,             /* The P1 operand */
  int p2,             /* The P2 operand */
  int p3,             /* The P3 operand */
  const char *p4,    /* The P4 operand */
  int p4type          /* P4 operand type */
){
  int addr = __vdbe_add_op3(p, op, p1, p2, p3);
  __vdbe_change_p4(p, addr, p4, p4type);
  return addr;
}

/*
** Add an OP_Function or OP_PureFunc opcode.
**
** The call_ctx argument is information (typically taken from expr_t.op2)
** that describes the calling context of the function.  0 means a general
** function call.  NC_IsCheck means called by a check constraint,
** NC_IdxExpr means called as part of an index expression.  NC_PartIdx
** means in the WHERE clause of a partial index.  NC_GenCol means called
** while computing a generated column value.  0 is the usual case.
*/
int __vdbe_add_function_call(
  parser_t *parser,        /* Parsing context */
  int p1,               /* Constant argument mask */
  int p2,               /* First argument register */
  int p3,               /* Register into which results are written */
  int n_arg,             /* Number of argument */
  const func_def_t *func, /* The function to be invoked */
  int call_ctx          /* Calling context */
){
  vdbe_t *v = parser->vdbe;
  int addr;
  dbsql_func_t *ctx;
  assert( v );
  ctx = __db_malloc_raw_nn(parser->db, SZ_CONTEXT(n_arg));
  if( ctx==0 ){
    assert( parser->db->malloc_failed );
    __free_ephemeral_function(parser->db, (func_def_t*)func);
    return 0;
  }
  ctx->out = 0;
  ctx->func = (func_def_t*)func;
  ctx->vdbe = 0;
  ctx->isError = 0;
  ctx->argc = n_arg;
  ctx->i_op = __vdbe_current_addr(v);
  addr = __vdbe_add_op4(v, call_ctx ? OP_PureFunc : OP_Function,
                           p1, p2, p3, (char*)ctx, P4_FUNCCTX);
  __vdbe_change_p5(v, call_ctx & NC_SelfRef);
  __may_abort(parser);
  return addr;
}

/*
** Add an opcode that includes the p4 value with a P4_INT64 or
** P4_REAL type.
*/
int __vdbe_add_op4_dup8(
  vdbe_t *p,            /* Add the opcode to this VM */
  int op,             /* The new opcode */
  int p1,             /* The P1 operand */
  int p2,             /* The P2 operand */
  int p3,             /* The P3 operand */
  const u_int8_t *p4,      /* The P4 operand */
  int p4type          /* P4 operand type */
){
  char *p4copy = __db_malloc_raw_nn(__vdbe_db(p), 8);
  if( p4copy ) memcpy(p4copy, p4, 8);
  return __vdbe_add_op4(p, op, p1, p2, p3, p4copy, p4type);
}

#ifndef DBSQL_OMIT_EXPLAIN
/*
** Return the address of the current EXPLAIN QUERY PLAN baseline.
** 0 means "none".
*/
int __vdbe_explain_parent(parser_t *parser){
  vdbe_op_t *op;
  if( parser->addr_explain==0 ) return 0;
  op = __vdbe_get_op(parser->vdbe, parser->addr_explain);
  return op->p2;
}

/*
** Set a debugger breakpoint on the following routine in order to
** monitor the EXPLAIN QUERY PLAN code generation.
*/
#if defined(DBSQL_DEBUG)
void __explain_breakpoint(const char *z1, const char *z2){
  (void)z1;
  (void)z2;
}
#endif

/*
** Add a new OP_Explain opcode.
**
** If the push flag is true, then make this opcode the parent for
** subsequent Explains until __vdbe_explain_pop() is called.
*/
int __vdbe_explain(parser_t *parser, u_int8_t push, const char *fmt, ...){
  int addr = 0;
#if !defined(DBSQL_DEBUG)
  /* Always include the OP_Explain opcodes if DBSQL_DEBUG is defined.
  ** But omit them (for performance) during production builds */
  if( parser->explain==2 || IS_STMT_SCANSTATUS(parser->db) )
#endif
  {
    char *msg;
    vdbe_t *v;
    va_list ap;
    int i_this;
    va_start(ap, fmt);
    msg = __vm_printf(parser->db, fmt, ap);
    va_end(ap);
    v = parser->vdbe;
    i_this = v->n_op;
    addr = __vdbe_add_op4(v, OP_Explain, i_this, parser->addr_explain, 0,
                      msg, P4_DYNAMIC);
    __explain_breakpoint(push?"PUSH":"", __vdbe_get_last_op(v)->p4.z);
    if( push){
      parser->addr_explain = i_this;
    }
    __vdbe_scan_status(v, i_this, -1, -1, 0, 0);
  }
  return addr;
}

/*
** Pop the EXPLAIN QUERY PLAN stack one level.
*/
void __vdbe_explain_pop(parser_t *parser){
  __explain_breakpoint("POP", 0);
  parser->addr_explain = __vdbe_explain_parent(parser);
}
#endif /* DBSQL_OMIT_EXPLAIN */

/*
** Add an OP_ParseSchema opcode.  This routine is broken out from
** __vdbe_add_op4() since it needs to also needs to mark all btrees
** as having been used.
**
** The where string must have been obtained from dbsql_malloc().
** This routine will take ownership of the allocated memory.
*/
void __vdbe_add_parse_schema_op(vdbe_t *p, int i_db, char *where, u_int16_t p5){
  int j;
  __vdbe_add_op4(p, OP_ParseSchema, i_db, 0, 0, where, P4_DYNAMIC);
  __vdbe_change_p5(p, p5);
  for(j=0; j<p->db->n_db; j++) __vdbe_uses_btree(p, j);
  __may_abort(p->parser);
}

/* Insert the end of a co-routine
*/
void __vdbe_end_coroutine(vdbe_t *v, int reg_yield){
  __vdbe_add_op1(v, OP_EndCoroutine, reg_yield);

  /* Clear the temporary register cache, thereby ensuring that each
  ** co-routine has its own independent set of registers, because co-routines
  ** might expect their registers to be preserved across an OP_Yield, and
  ** that could cause problems if two or more co-routines are using the same
  ** temporary register.
  */
  v->parser->n_temp_reg = 0;
  v->parser->n_range_reg = 0;
}

/*
** Create a new symbolic label for an instruction that has yet to be
** coded.  The symbolic label is really just a negative number.  The
** label can be used as the P2 value of an operation.  Later, when
** the label is resolved to a specific address, the VDBE will scan
** through its operation list and change all values of P2 which match
** the label into the resolved address.
**
** The VDBE knows that a P2 value is a label because labels are
** always negative and P2 values are suppose to be non-negative.
** Hence, a negative P2 value is a label that has yet to be resolved.
** (Later:) This is only true for opcodes that have the OPFLG_JUMP
** property.
**
** Variable usage notes:
**
**     parser_t.labels[x]     Stores the address that the x-th label resolves
**                         into.  For testing (DBSQL_DEBUG), unresolved
**                         labels stores -1, but that is not required.
**     parser_t.n_label_alloc   Number of slots allocated to parser_t.labels[]
**     parser_t.n_label        The *negative* of the number of labels that have
**                         been issued.  The negative is stored because
**                         that gives a performance improvement over storing
**                         the equivalent positive value.
*/
int __vdbe_make_label(parser_t *parser){
  return --parser->n_label;
}

/*
** Resolve label "x" to be the address of the next instruction to
** be inserted.  The parameter "x" must have been obtained from
** a prior call to __vdbe_make_label().
*/
static DBSQL_NOINLINE void __resize_resolve_label(parser_t *p, vdbe_t *v, int j){
  int n_new_size = 10 - p->n_label;
  p->labels = __db_realloc_or_free(p->db, p->labels,
                     n_new_size*sizeof(p->labels[0]));
  if( p->labels==0 ){
    p->n_label_alloc = 0;
  }else{
#ifdef DBSQL_DEBUG
    int i;
    for(i=p->n_label_alloc; i<n_new_size; i++) p->labels[i] = -1;
#endif
    if( n_new_size>=100 && (n_new_size/100)>(p->n_label_alloc/100) ){
      __progress_check(p);
    }
    p->n_label_alloc = n_new_size;
    p->labels[j] = v->n_op;
  }
}
void __vdbe_resolve_label(vdbe_t *v, int x){
  parser_t *p = v->parser;
  int j = ADDR(x);
  assert( v->vdbe_state==VDBE_INIT_STATE );
  assert( j<-p->n_label );
  assert( j>=0 );
#ifdef DBSQL_DEBUG
  if( p->db->flags & DBSQL_VdbeAddopTrace ){
    printf("RESOLVE LABEL %d to %d\n", x, v->n_op);
  }
#endif
  if( p->n_label_alloc + p->n_label < 0 ){
    __resize_resolve_label(p,v,j);
  }else{
    assert( p->labels[j]==(-1) ); /* Labels may only be resolved once */
    p->labels[j] = v->n_op;
  }
}

/*
** Mark the VDBE as one that can only be run one time.
*/
void __vdbe_run_only_once(vdbe_t *p){
  __vdbe_add_op2(p, OP_Expire, 1, 1);
}

/*
** Mark the VDBE as one that can be run multiple times.
*/
void __vdbe_reusable(vdbe_t *p){
  int i;
  for(i=1; ALWAYS(i<p->n_op); i++){
    if( ALWAYS(p->ops[i].opcode==OP_Expire) ){
      p->ops[1].opcode = OP_Noop;
      break;
    }
  }
}

#ifdef DBSQL_DEBUG /* __assert_may_abort() logic */

/*
** The following type and function are used to iterate through all opcodes
** in a vdbe_t main program and each of the sub-programs (triggers) it may
** invoke directly or indirectly. It should be used as follows:
**
**   op_t *op;
**   vdbe_op_iter_t sIter;
**
**   memset(&sIter, 0, sizeof(sIter));
**   sIter.v = v;                            // v is of type vdbe_t*
**   while( (op = __op_iter_next(&sIter)) ){
**     // Do something with op
**   }
**   __dbsql_db_free(v->db, sIter.subs);
**
*/
typedef struct vdbe_op_iter_t vdbe_op_iter_t;
struct vdbe_op_iter_t {
  vdbe_t *v;                   /* vdbe_t to iterate through the opcodes of */
  sub_program_t **subs;        /* Array of subprograms */
  int n_sub;                  /* Number of entries in subs */
  int i_addr;                 /* Address of next instruction to return */
  int i_sub;                  /* 0 = main program, 1 = first sub-program etc. */
};
static op_t *__op_iter_next(vdbe_op_iter_t *p){
  vdbe_t *v = p->v;
  op_t *ret = 0;
  op_t *ops;
  int n_op;

  if( p->i_sub<=p->n_sub ){

    if( p->i_sub==0 ){
      ops = v->ops;
      n_op = v->n_op;
    }else{
      ops = p->subs[p->i_sub-1]->ops;
      n_op = p->subs[p->i_sub-1]->n_op;
    }
    assert( p->i_addr<n_op );

    ret = &ops[p->i_addr];
    p->i_addr++;
    if( p->i_addr==n_op ){
      p->i_sub++;
      p->i_addr = 0;
    }
 
    if( ret->p4type==P4_SUBPROGRAM ){
      int64_t n_byte = (1+(u_int64_t)p->n_sub)*sizeof(sub_program_t*);
      int j;
      for(j=0; j<p->n_sub; j++){
        if( p->subs[j]==ret->p4.program ) break;
      }
      if( j==p->n_sub ){
        p->subs = __db_realloc_or_free(v->db, p->subs, n_byte);
        if( !p->subs ){
          ret = 0;
        }else{
          p->subs[p->n_sub++] = ret->p4.program;
        }
      }
    }
  }

  return ret;
}

/*
** Check if the program stored in the VM associated with parser may
** throw an ABORT exception (causing the statement, but not entire transaction
** to be rolled back). This condition is true if the main program or any
** sub-programs contains any of the following:
**
**   *  OP_Halt with P1=DBSQL_CONSTRAINT and P2=OE_Abort.
**   *  OP_HaltIfNull with P1=DBSQL_CONSTRAINT and P2=OE_Abort.
**   *  OP_Destroy
**   *  OP_VUpdate
**   *  OP_VCreate
**   *  OP_VRename
**   *  OP_FkCounter with P2==0 (immediate foreign key constraint)
**   *  OP_CreateBtree/BTREE_INTKEY and OP_InitCoroutine
**      (for CREATE TABLE AS SELECT ...)
**
** Then check that the value of parser_t.may_abort is true if an
** ABORT may be thrown, or false otherwise. Return true if it does
** match, or false otherwise. This function is intended to be used as
** part of an assert statement in the compiler. Similar to:
**
**   assert( __vdbe_assert_may_abort(parser->vdbe, parser->may_abort) );
*/
int __vdbe_assert_may_abort(vdbe_t *v, int may_abort){
  int has_abort = 0;
  int has_fk_counter = 0;
  int has_create_table = 0;
  int has_create_index = 0;
  int has_init_coroutine = 0;
  op_t *op;
  vdbe_op_iter_t sIter;

  if( v==0 ) return 0;
  memset(&sIter, 0, sizeof(sIter));
  sIter.v = v;

  while( (op = __op_iter_next(&sIter))!=0 ){
    int opcode = op->opcode;
    if( opcode==OP_Destroy || opcode==OP_VUpdate || opcode==OP_VRename
     || opcode==OP_VDestroy
     || opcode==OP_VCreate
     || opcode==OP_ParseSchema
     || opcode==OP_Function || opcode==OP_PureFunc
     || ((opcode==OP_Halt || opcode==OP_HaltIfNull)
      && ((op->p1)!=DBSQL_OK && op->p2==OE_Abort))
    ){
      has_abort = 1;
      break;
    }
    if( opcode==OP_CreateBtree && op->p3==BTREE_INTKEY ) has_create_table = 1;
    if( may_abort ){
      /* has_create_index may also be set for some DELETE statements that use
      ** OP_Clear. So this routine may end up returning true in the case
      ** where a "DELETE FROM tbl" has a statement-journal but does not
      ** require one. This is not so bad - it is an inefficiency, not a bug. */
      if( opcode==OP_CreateBtree && op->p3==BTREE_BLOBKEY ) has_create_index = 1;
      if( opcode==OP_Clear ) has_create_index = 1;
    }
    if( opcode==OP_InitCoroutine ) has_init_coroutine = 1;
#ifndef DBSQL_OMIT_FOREIGN_KEY
    if( opcode==OP_FkCounter && op->p1==0 && op->p2==1 ){
      has_fk_counter = 1;
    }
#endif
  }
  __dbsql_db_free(v->db, sIter.subs);

  /* Return true if has_abort==may_abort. Or if a malloc failure occurred.
  ** If malloc failed, then the while() loop above may not have iterated
  ** through all opcodes and has_abort may be set incorrectly. Return
  ** true for this case to prevent the assert() in the callers frame
  ** from failing.  */
  return ( v->db->malloc_failed || has_abort==may_abort || has_fk_counter
        || (has_create_table && has_init_coroutine) || has_create_index
  );
}
#endif /* DBSQL_DEBUG - the __assert_may_abort() function */

#ifdef DBSQL_DEBUG
/*
** Increment the n_write counter in the VDBE if the cursor is not an
** ephemeral cursor, or if the cursor argument is NULL.
*/
void __vdbe_incr_write_counter(vdbe_t *p, vdbe_cursor_t *c){
  if( c==0
   || (c->cur_type!=CURTYPE_SORTER
       && c->cur_type!=CURTYPE_PSEUDO
       && !c->is_ephemeral)
  ){
    p->n_write++;
  }
}
#endif

#ifdef DBSQL_DEBUG
/*
** Assert if an Abort at this point in time might result in a corrupt
** database.
*/
void __vdbe_assert_abortable(vdbe_t *p){
  assert( p->n_write==0 || p->uses_stmt_journal );
}
#endif

/*
** This routine is called after all opcodes have been inserted.  It loops
** through all the opcodes and fixes up some details.
**
** (1) For each jump instruction with a negative P2 value (a label)
**     resolve the P2 value to an actual address.
**
** (2) Compute the maximum number of arguments used by the update/filter
**     methods of any virtual table and store that value in *max_vtab_args.
**
** (3) Update the vdbe_t.readOnly and vdbe_t.is_reader flags to accurately
**     indicate what the prepared statement actually does.
**
** (4) (discontinued)
**
** (5) Reclaim the memory allocated for storing labels.
**
** This routine will only function correctly if the mkopcodeh.tcl generator
** script numbers the opcodes correctly.  Changes to this routine must be
** coordinated with changes to mkopcodeh.tcl.
*/
static void __resolve_p2_values(vdbe_t *p, int *max_vtab_args){
  int n_max_vtab_args = *max_vtab_args;
  op_t *op;
  parser_t *parser = p->parser;
  int *labels = parser->labels;

  assert( parser->db->malloc_failed==0 ); /* tag-20230419-1 */
  p->readOnly = 1;
  p->is_reader = 0;
  op = &p->ops[p->n_op-1];
  assert( p->ops[0].opcode==OP_Init );
  while( 1 /* Loop terminates when it reaches the OP_Init opcode */ ){
    /* Only JUMP opcodes and the short list of special opcodes in the switch
    ** below need to be considered.  The mkopcodeh.tcl generator script groups
    ** all these opcodes together near the front of the opcode list.  Skip
    ** any opcode that does not need processing by virtual of the fact that
    ** it is larger than DBSQL_MX_JUMP_OPCODE, as a performance optimization.
    */
    if( op->opcode<=DBSQL_MX_JUMP_OPCODE ){
      /* NOTE: Be sure to update mkopcodeh.tcl when adding or removing
      ** cases from this switch! */
      switch( op->opcode ){
        case OP_Transaction: {
          if( op->p2!=0 ) p->readOnly = 0;
          /* no break */ deliberate_fall_through
        }
        case OP_AutoCommit:
        case OP_Savepoint: {
          p->is_reader = 1;
          break;
        }
#ifndef DBSQL_OMIT_WAL
        case OP_Checkpoint:
#endif
        case OP_Vacuum:
        case OP_JournalMode: {
          p->readOnly = 0;
          p->is_reader = 1;
          break;
        }
        case OP_Init: {
          assert( op->p2>=0 );
          goto resolve_p2_values_loop_exit;
        }
#ifndef DBSQL_OMIT_VIRTUALTABLE
        case OP_VUpdate: {
          if( op->p2>n_max_vtab_args ) n_max_vtab_args = op->p2;
          break;
        }
        case OP_VFilter: {
          int n;
          /* The instruction immediately prior to VFilter will be an
          ** OP_Integer that sets the "argc" value for the VFilter.  See
          ** the code where OP_VFilter is generated at tag-20250207a. */
          assert( (op - p->ops) >= 3 );
          assert( op[-1].opcode==OP_Integer );
          assert( op[-1].p2==op->p3+1 );
          n = op[-1].p1;
          if( n>n_max_vtab_args ) n_max_vtab_args = n;
          /* Fall through into the default case */
          /* no break */ deliberate_fall_through
        }
#endif
        default: {
          if( op->p2<0 ){
            /* The mkopcodeh.tcl script has so arranged things that the only
            ** non-jump opcodes less than DBSQL_MX_JUMP_CODE are guaranteed to
            ** have non-negative values for P2. */
            assert( (__opcode_property[op->opcode] & OPFLG_JUMP)!=0 );
            assert( ADDR(op->p2)<-parser->n_label );
            assert( labels!=0 );  /* True because of tag-20230419-1 */
            op->p2 = labels[ADDR(op->p2)];
          }

          /* OPFLG_JUMP opcodes never have P2==0, though OPFLG_JUMP0 opcodes
          ** might */
          assert( op->p2>0 
                  || (__opcode_property[op->opcode] & OPFLG_JUMP0)!=0 );

          /* Jumps never go off the end of the bytecode array */
          assert( op->p2<p->n_op
                  || (__opcode_property[op->opcode] & OPFLG_JUMP)==0 );
          break;
        }
      }
      /* The mkopcodeh.tcl script has so arranged things that the only
      ** non-jump opcodes less than DBSQL_MX_JUMP_CODE are guaranteed to
      ** have non-negative values for P2. */
      assert( (__opcode_property[op->opcode]&OPFLG_JUMP)==0 || op->p2>=0);
    }
    assert( op>p->ops );   
    op--;
  }
resolve_p2_values_loop_exit:
  if( labels ){
    __db_nn_free_nn(p->db, parser->labels);
    parser->labels = 0;
  }
  parser->n_label = 0;
  *max_vtab_args = n_max_vtab_args;
  assert( p->is_reader!=0 || DB_MASK_ALL_ZERO(p->btree_mask) );
}

#ifdef DBSQL_DEBUG
/*
** Check to see if a subroutine contains a jump to a location outside of
** the subroutine.  If a jump outside the subroutine is detected, add code
** that will cause the program to halt with an error message.
**
** The subroutine consists of opcodes between i_first and i_last.  Jumps to
** locations within the subroutine are acceptable.  i_ret_reg is a register
** that contains the return address.  Jumps to outside the range of i_first
** through i_last are also acceptable as long as the jump destination is
** an OP_Return to i_return_addr.
**
** A jump to an unresolved label means that the jump destination will be
** beyond the current address.  That is normally a jump to an early
** termination and is consider acceptable.
**
** This routine only runs during debug builds.  The purpose is (of course)
** to detect invalid escapes out of a subroutine.  The OP_Halt opcode
** is generated rather than an assert() or other error, so that ".eqp full"
** will still work to show the original bytecode, to aid in debugging.
*/
void __vdbe_no_jumps_outside_subrtn(
  vdbe_t *v,          /* The byte-code program under construction */
  int i_first,       /* First opcode of the subroutine */
  int i_last,        /* Last opcode of the subroutine */
  int i_ret_reg       /* Subroutine return address register */
){
  vdbe_op_t *op;
  parser_t *parser;
  int i;
  dbsql_str_t *p_err = 0;
  assert( v!=0 );
  parser = v->parser;
  assert( parser!=0 );
  if( parser->n_err ) return;
  assert( i_last>=i_first );
  assert( i_last<v->n_op );
  op = &v->ops[i_first];
  for(i=i_first; i<=i_last; i++, op++){
    if( (__opcode_property[op->opcode] & OPFLG_JUMP)!=0 ){
      int i_dest = op->p2;   /* Jump destination */
      if( i_dest==0 ) continue;
      if( op->opcode==OP_Gosub ) continue;
      if( op->p3==20230325 && op->opcode==OP_NotNull ){
        /* This is a deliberately taken illegal branch.  tag-20230325-2 */
        continue;
      }
      if( i_dest<0 ){
        int j = ADDR(i_dest);
        assert( j>=0 );
        if( j>=-parser->n_label || parser->labels[j]<0 ){
          continue;
        }
        i_dest = parser->labels[j];
      }
      if( i_dest<i_first || i_dest>i_last ){
        int j = i_dest;
        for(; j<v->n_op; j++){
          vdbe_op_t *x = &v->ops[j];
          if( x->opcode==OP_Return ){
            if( x->p1==i_ret_reg ) break;
            continue;
          }
          if( x->opcode==OP_Noop ) continue;
          if( x->opcode==OP_Explain ) continue;
          if( p_err==0 ){
            p_err = dbsql_str_new(0);
          }else{
            dbsql_str_appendchar(p_err, 1, '\n');
          }
          dbsql_str_appendf(p_err,
              "Opcode at %d jumps to %d which is outside the "
              "subroutine at %d..%d",
              i, i_dest, i_first, i_last);
          break;
        }
      }
    }
  }
  if( p_err ){
    char *err = dbsql_str_finish(p_err);
    __vdbe_add_op4(v, OP_Halt, DBSQL_INTERNAL, OE_Abort, 0, err, 0);
    dbsql_free(err);
    __may_abort(parser);
  }
}
#endif /* DBSQL_DEBUG */

/*
** Return the address of the next instruction to be inserted.
*/
int __vdbe_current_addr(vdbe_t *p){
  assert( p->vdbe_state==VDBE_INIT_STATE );
  return p->n_op;
}

/*
** Verify that at least N opcode slots are available in p without
** having to malloc for more space (except when compiled using
** DBSQL_TEST_REALLOC_STRESS).  This interface is used during testing
** to verify that certain calls to __vdbe_add_op_list() can never
** fail due to a OOM fault and hence that the return value from
** __vdbe_add_op_list() will always be non-NULL.
*/
#if defined(DBSQL_DEBUG) && !defined(DBSQL_TEST_REALLOC_STRESS)
void __vdbe_verify_no_malloc_required(vdbe_t *p, int N){
  assert( p->n_op + N <= p->n_op_alloc );
}
#endif

/*
** Verify that the VM passed as the only argument does not contain
** an OP_ResultRow opcode. Fail an assert() if it does. This is used
** by code in pragma.c to ensure that the implementation of certain
** pragmas comports with the flags specified in the mkpragmatab.tcl
** script.
*/
#if defined(DBSQL_DEBUG) && !defined(DBSQL_TEST_REALLOC_STRESS)
void __vdbe_verify_no_result_row(vdbe_t *p){
  int i;
  for(i=0; i<p->n_op; i++){
    assert( p->ops[i].opcode!=OP_ResultRow );
  }
}
#endif

/*
** Generate code (a single OP_Abortable opcode) that will
** verify that the VDBE program can safely call Abort in the current
** context.
*/
#if defined(DBSQL_DEBUG)
void __vdbe_verify_abortable(vdbe_t *p, int on_error){
  if( on_error==OE_Abort ) __vdbe_add_op0(p, OP_Abortable);
}
#endif

/*
** This function returns a pointer to the array of opcodes associated with
** the vdbe_t passed as the first argument. It is the callers responsibility
** to arrange for the returned array to be eventually freed using the
** __vdbe_free_op_array() function.
**
** Before returning, *pn_op is set to the number of entries in the returned
** array. Also, *pn_max_arg is set to the larger of its current value and
** the number of entries in the vdbe_t.args[] array required to execute the
** returned program.
*/
vdbe_op_t *__vdbe_take_op_array(vdbe_t *p, int *pn_op, int *pn_max_arg){
  vdbe_op_t *ops = p->ops;
  assert( ops && !p->db->malloc_failed );

  /* Check that __vdbe_uses_btree() was not called on this VM */
  assert( DB_MASK_ALL_ZERO(p->btree_mask) );

  __resolve_p2_values(p, pn_max_arg);
  *pn_op = p->n_op;
  p->ops = 0;
  return ops;
}

/*
** Add a whole list of operations to the operation stack.  Return a
** pointer to the first operation inserted.
**
** Non-zero P2 arguments to jump instructions are automatically adjusted
** so that the jump target is relative to the first operation inserted.
*/
vdbe_op_t *__vdbe_add_op_list(
  vdbe_t *p,                     /* Add opcodes to the prepared statement */
  int n_op,                     /* Number of opcodes to add */
  vdbe_op_list_t const *ops,       /* The opcodes to be added */
  int i_lineno                  /* Source-file line number of first opcode */
){
  int i;
  vdbe_op_t *out, *first;
  assert( n_op>0 );
  assert( p->vdbe_state==VDBE_INIT_STATE );
  if( p->n_op + n_op > p->n_op_alloc && __grow_op_array(p, n_op) ){
    return 0;
  }
  first = out = &p->ops[p->n_op];
  for(i=0; i<n_op; i++, ops++, out++){
    out->opcode = ops->opcode;
    out->p1 = ops->p1;
    out->p2 = ops->p2;
    assert( ops->p2>=0 );
    if( (__opcode_property[ops->opcode] & OPFLG_JUMP)!=0 && ops->p2>0 ){
      out->p2 += p->n_op;
    }
    out->p3 = ops->p3;
    out->p4type = P4_NOTUSED;
    out->p4.p = 0;
    out->p5 = 0;
#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
    out->comment = 0;
#endif
#ifdef DBSQL_VDBE_COVERAGE
    out->i_src_line = i_lineno+i;
#else
    (void)i_lineno;
#endif
#ifdef DBSQL_DEBUG
    if( p->db->flags & DBSQL_VdbeAddopTrace ){
      __vdbe_print_op(0, i+p->n_op, &p->ops[i+p->n_op]);
    }
#endif
  }
  p->n_op += n_op;
  return first;
}

#if defined(DBSQL_ENABLE_STMT_SCANSTATUS)
/*
** Add an entry to the array of counters managed by dbsql_stmt_scanstatus().
*/
void __vdbe_scan_status(
  vdbe_t *p,                        /* VM to add scanstatus() to */
  int addr_explain,                /* Address of OP_Explain (or 0) */
  int addr_loop,                   /* Address of loop counter */
  int addr_visit,                  /* Address of rows visited counter */
  log_est_t n_est,                    /* Estimated number of output rows */
  const char *name               /* Name of table or index being scanned */
){
  if( IS_STMT_SCANSTATUS(p->db) ){
    int64_t n_byte = (1+(int64_t)p->n_scan) * sizeof(scan_status_t);
    scan_status_t *news;
    news = (scan_status_t*)__db_realloc(p->db, p->scans, n_byte);
    if( news ){
      scan_status_t *p_new = &news[p->n_scan++];
      memset(p_new, 0, sizeof(scan_status_t));
      p_new->addr_explain = addr_explain;
      p_new->addr_loop = addr_loop;
      p_new->addr_visit = addr_visit;
      p_new->n_est = n_est;
      p_new->name = __db_str_dup(p->db, name);
      p->scans = news;
    }
  }
}

/*
** Add the range of instructions from addr_start to addr_end (inclusive) to
** the set of those corresponding to the dbsql_stmt_scanstatus() counters
** associated with the OP_Explain instruction at addr_explain. The
** sum of the __hwtime() values for each of these instructions
** will be returned for DBSQL_SCANSTAT_NCYCLE requests.
*/
void __vdbe_scan_status_range(
  vdbe_t *p,
  int addr_explain,
  int addr_start,
  int addr_end
){
  if( IS_STMT_SCANSTATUS(p->db) ){
    scan_status_t *scan = 0;
    int ii;
    for(ii=p->n_scan-1; ii>=0; ii--){
      scan = &p->scans[ii];
      if( scan->addr_explain==addr_explain ) break;
      scan = 0;
    }
    if( scan ){
      if( addr_end<0 ) addr_end = __vdbe_current_addr(p)-1;
      for(ii=0; ii<ARRAY_SIZE(scan->addr_ranges); ii+=2){
        if( scan->addr_ranges[ii]==0 ){
          scan->addr_ranges[ii] = addr_start;
          scan->addr_ranges[ii+1] = addr_end;
          break;
        }
      }
    }
  }
}

/*
** Set the addresses for the DBSQL_SCANSTAT_NLOOP and DBSQL_SCANSTAT_NROW
** counters for the query element associated with the OP_Explain at
** addr_explain.
*/
void __vdbe_scan_status_counters(
  vdbe_t *p,
  int addr_explain,
  int addr_loop,
  int addr_visit
){
  if( IS_STMT_SCANSTATUS(p->db) ){
    scan_status_t *scan = 0;
    int ii;
    for(ii=p->n_scan-1; ii>=0; ii--){
      scan = &p->scans[ii];
      if( scan->addr_explain==addr_explain ) break;
      scan = 0;
    }
    if( scan ){
      if( addr_loop>0 ) scan->addr_loop = addr_loop;
      if( addr_visit>0 ) scan->addr_visit = addr_visit;
    }
  }
}
#endif /* defined(DBSQL_ENABLE_STMT_SCANSTATUS) */


/*
** Change the value of the opcode, or P1, P2, P3, or P5 operands
** for a specific instruction.
*/
void __vdbe_change_opcode(vdbe_t *p, int addr, u_int8_t i_new_opcode){
  assert( addr>=0 );
  __vdbe_get_op(p,addr)->opcode = i_new_opcode;
}
void __vdbe_change_p1(vdbe_t *p, int addr, int val){
  assert( addr>=0 );
  __vdbe_get_op(p,addr)->p1 = val;
}
void __vdbe_change_p2(vdbe_t *p, int addr, int val){
  assert( addr>=0 || p->db->malloc_failed );
  __vdbe_get_op(p,addr)->p2 = val;
}
void __vdbe_change_p3(vdbe_t *p, int addr, int val){
  assert( addr>=0 );
  __vdbe_get_op(p,addr)->p3 = val;
}
void __vdbe_change_p5(vdbe_t *p, u_int16_t p5){
  assert( p->n_op>0 || p->db->malloc_failed );
  if( p->n_op>0 ) p->ops[p->n_op-1].p5 = p5;
}

/*
** If the previous opcode is an OP_Column that delivers results
** into register i_dest, then add the OPFLAG_TYPEOFARG flag to that
** opcode.
*/
void __vdbe_typeof_column(vdbe_t *p, int i_dest){
  vdbe_op_t *op = __vdbe_get_last_op(p);
#ifdef DBSQL_DEBUG
  while( op->opcode==OP_ReleaseReg ) op--;
#endif
  if( op->p3==i_dest && op->opcode==OP_Column ){
    op->p5 |= OPFLAG_TYPEOFARG;
  }
}

/*
** Change the P2 operand of instruction addr so that it points to
** the address of the next instruction to be coded.
*/
void __vdbe_jump_here(vdbe_t *p, int addr){
  __vdbe_change_p2(p, addr, p->n_op);
}

/*
** Change the P2 operand of the jump instruction at addr so that
** the jump lands on the next opcode.  Or if the jump instruction was
** the previous opcode (and is thus a no-op) then simply back up
** the next instruction counter by one slot so that the jump is
** overwritten by the next inserted opcode.
**
** This routine is an optimization of __vdbe_jump_here() that
** strives to omit useless byte-code like this:
**
**        7   Once 0 8 0
**        8   ...
*/
void __vdbe_jump_here_or_pop_inst(vdbe_t *p, int addr){
  if( addr==p->n_op-1 ){
    assert( p->ops[addr].opcode==OP_Once
         || p->ops[addr].opcode==OP_If
         || p->ops[addr].opcode==OP_FkIfZero );
    assert( p->ops[addr].p4type==0 );
#ifdef DBSQL_VDBE_COVERAGE
    __vdbe_get_last_op(p)->i_src_line = 0;  /* Erase VDBE_COVERAGE() macros */
#endif
    p->n_op--;
  }else{
    __vdbe_change_p2(p, addr, p->n_op);
  }
}


/*
** If the input func_def_t structure is ephemeral, then free it.  If
** the func_def_t is not ephemeral, then do nothing.
*/
static void __free_ephemeral_function(DBSQL *db, func_def_t *def){
  assert( db!=0 );
  if( (def->func_flags & DBSQL_FUNC_EPHEM)!=0 ){
    __db_nn_free_nn(db, def);
  }
}

/*
** Delete a P4 value if necessary.
*/
static DBSQL_NOINLINE void __free_p4_mem(DBSQL *db, mem_t *p){
  if( p->sz_malloc ) __dbsql_db_free(db, p->z_malloc);
  __db_nn_free_nn(db, p);
}
static DBSQL_NOINLINE void __free_p4_func_ctx(DBSQL *db, dbsql_func_t *p){
  assert( db!=0 );
  __free_ephemeral_function(db, p->func);
  __db_nn_free_nn(db, p);
}
static void __free_p4(DBSQL *db, int p4type, void *p4){
  assert( db );
  switch( p4type ){
    case P4_FUNCCTX: {
      __free_p4_func_ctx(db, (dbsql_func_t*)p4);
      break;
    }
    case P4_REAL:
    case P4_INT64:
    case P4_DYNAMIC:
    case P4_INTARRAY: {
      if( p4 ) __db_nn_free_nn(db, p4);
      break;
    }
    case P4_KEYINFO: {
      if( db->pn_bytes_freed==0 ) __key_info_unref((key_info_t*)p4);
      break;
    }
#ifdef DBSQL_ENABLE_CURSOR_HINTS
    case P4_EXPR: {
      __expr_delete(db, (expr_t*)p4);
      break;
    }
#endif
    case P4_FUNCDEF: {
      __free_ephemeral_function(db, (func_def_t*)p4);
      break;
    }
    case P4_MEM: {
      if( db->pn_bytes_freed==0 ){
        __value_free((dbsql_value_t*)p4);
      }else{
        __free_p4_mem(db, (mem_t*)p4);
      }
      break;
    }
    case P4_VTAB : {
      if( db->pn_bytes_freed==0 ) __vtab_unlock((v_table_t *)p4);
      break;
    }
    case P4_TABLEREF: {
      if( db->pn_bytes_freed==0 ) __delete_table(db, (table_t*)p4);
      break;
    }
    case P4_SUBRTNSIG: {
      subrtn_sig_t *sig = (subrtn_sig_t*)p4;
      __dbsql_db_free(db, sig->aff);
      __dbsql_db_free(db, sig);
      break;
    }
  }
}

/*
** Free the space allocated for ops and any p4 values allocated for the
** opcodes contained within. If ops is not NULL it is assumed to contain
** n_op entries.
*/
static void __vdbe_free_op_array(DBSQL *db, op_t *ops, int n_op){
  assert( n_op>=0 );
  assert( db!=0 );
  if( ops ){
    op_t *op = &ops[n_op-1];
    while(1){  /* Exit via break */
      if( op->p4type <= P4_FREE_IF_LE ) __free_p4(db, op->p4type, op->p4.p);
#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
      __dbsql_db_free(db, op->comment);
#endif    
      if( op==ops ) break;
      op--;
    }
    __db_nn_free_nn(db, ops);
  }
}

/*
** Link the sub_program_t object passed as the second argument into the linked
** list at vdbe_t.sub_program. This list is used to delete all sub-program
** objects when the VM is no longer required.
*/
void __vdbe_link_sub_program(vdbe_t *vdbe, sub_program_t *p){
  p->next = vdbe->program;
  vdbe->program = p;
}

/*
** Return true if the given vdbe_t has any __sub_programs.
*/
int __vdbe_has_sub_program(vdbe_t *vdbe){
  return vdbe->program!=0;
}

/*
** Change the opcode at addr into OP_Noop
*/
int __vdbe_change_to_noop(vdbe_t *p, int addr){
  vdbe_op_t *op;
  if( p->db->malloc_failed ) return 0;
  assert( addr>=0 && addr<p->n_op );
  op = &p->ops[addr];
  __free_p4(p->db, op->p4type, op->p4.p);
  op->p4type = P4_NOTUSED;
  op->p4.z = 0;
  op->opcode = OP_Noop;
  return 1;
}

/*
** If the last opcode is "op" and it is not a jump destination,
** then remove it.  Return true if and only if an opcode was removed.
*/
int __vdbe_delete_prior_opcode(vdbe_t *p, u_int8_t op){
  if( p->n_op>0 && p->ops[p->n_op-1].opcode==op ){
    return __vdbe_change_to_noop(p, p->n_op-1);
  }else{
    return 0;
  }
}

#ifdef DBSQL_DEBUG
/*
** Generate an OP_ReleaseReg opcode to indicate that a range of
** registers, except any identified by mask, are no longer in use.
*/
void __vdbe_release_registers(
  parser_t *parser,       /* Parsing context */
  int i_first,          /* index_t of first register to be released */
  int N,               /* Number of registers to release */
  u_int32_t mask,            /* Mask of registers to NOT release */
  int undefine        /* If true, mark registers as undefined */
){
  if( N==0 || OPTIMIZATION_DISABLED(parser->db, DBSQL_ReleaseReg) ) return;
  assert( parser->vdbe );
  assert( i_first>=1 );
  assert( i_first+N-1<=parser->n_mem );
  if( N<=31 && mask!=0 ){
    while( N>0 && (mask&1)!=0 ){
      mask >>= 1;
      i_first++;
      N--;
    }
    while( N>0 && N<=32 && (mask & MASKBIT32(N-1))!=0 ){
      mask &= ~MASKBIT32(N-1);
      N--;
    }
  }
  if( N>0 ){
    __vdbe_add_op3(parser->vdbe, OP_ReleaseReg, i_first, N, *(int*)&mask);
    if( undefine ) __vdbe_change_p5(parser->vdbe, 1);
  }
}
#endif /* DBSQL_DEBUG */

/*
** Change the value of the P4 operand for a specific instruction.
** This routine is useful when a large program is loaded from a
** static array using __vdbe_add_op_list but we want to make a
** few minor changes to the program.
**
** If n>=0 then the P4 operand is dynamic, meaning that a copy of
** the string is made into memory obtained from dbsql_malloc().
** A value of n==0 means copy bytes of p4 up to and including the
** first null byte.  If n>0 then copy n+1 bytes of p4.
**
** Other values of n (P4_STATIC, P4_COLLSEQ etc.) indicate that p4 points
** to a string or structure that is guaranteed to exist for the lifetime of
** the vdbe_t. In these cases we can just copy the pointer.
**
** If addr<0 then change P4 on the most recently inserted instruction.
*/
static void DBSQL_NOINLINE __vdbe_change_p4_full(
  vdbe_t *p,
  op_t *op,
  const char *p4,
  int n
){
  if( op->p4type ){
    assert( op->p4type > P4_FREE_IF_LE );
    op->p4type = 0;
    op->p4.p = 0;
  }
  if( n<0 ){
    __vdbe_change_p4(p, (int)(op - p->ops), p4, n);
  }else{
    if( n==0 ) n = __strlen30(p4);
    op->p4.z = __db_str_n_dup(p->db, p4, n);
    op->p4type = P4_DYNAMIC;
  }
}
void __vdbe_change_p4(vdbe_t *p, int addr, const char *p4, int n){
  op_t *op;
  DBSQL *db;
  assert( p!=0 );
  db = p->db;
  assert( p->vdbe_state==VDBE_INIT_STATE );
  assert( p->ops!=0 || db->malloc_failed );
  if( db->malloc_failed ){
    if( n!=P4_VTAB ) __free_p4(db, n, (void*)*(char**)&p4);
    return;
  }
  assert( p->n_op>0 );
  assert( addr<p->n_op );
  if( addr<0 ){
    addr = p->n_op - 1;
  }
  op = &p->ops[addr];
  if( n>=0 || op->p4type ){
    __vdbe_change_p4_full(p, op, p4, n);
    return;
  }
  if( n==P4_INT32 ){
    /* Note: this cast is safe, because the origin data point was an int
    ** that was cast to a (const char *). */
    op->p4.i = DBSQL_PTR_TO_INT(p4);
    op->p4type = P4_INT32;
  }else if( p4!=0 ){
    assert( n<0 );
    op->p4.p = (void*)p4;
    op->p4type = (signed char)n;
    if( n==P4_VTAB ) __vtab_lock((v_table_t*)p4);
  }
}

/*
** Change the P4 operand of the most recently coded instruction
** to the value defined by the arguments.  This is a high-speed
** version of __vdbe_change_p4().
**
** The P4 operand must not have been previously defined.  And the new
** P4 must not be P4_INT32.  Use __vdbe_change_p4() in either of
** those cases.
*/
void __vdbe_append_p4(vdbe_t *p, void *p4, int n){
  vdbe_op_t *op;
  assert( n!=P4_INT32 && n!=P4_VTAB );
  assert( n<=0 );
  if( p->db->malloc_failed ){
    __free_p4(p->db, n, p4);
  }else{
    assert( p4!=0 || n==P4_DYNAMIC );
    assert( p->n_op>0 );
    op = &p->ops[p->n_op-1];
    assert( op->p4type==P4_NOTUSED );
    op->p4type = n;
    op->p4.p = p4;
  }
}

/*
** Set the P4 on the most recently added opcode to the key_info_t for the
** index given.
*/
void __vdbe_set_p4_key_info(parser_t *parser, index_t *idx){
  vdbe_t *v = parser->vdbe;
  key_info_t *key_info;
  assert( v!=0 );
  assert( idx!=0 );
  key_info = __key_info_of_index(parser, idx);
  if( key_info ) __vdbe_append_p4(v, key_info, P4_KEYINFO);
}

#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
/*
** Change the comment on the most recently coded instruction.  Or
** insert a No-op and add the comment to that new instruction.  This
** makes the code easier to read during debugging.  None of this happens
** in a production build.
*/
static void __vdbe_v_comment(vdbe_t *p, const char *format, va_list ap){
  assert( p->n_op>0 || p->ops==0 );
  assert( p->ops==0 || p->ops[p->n_op-1].comment==0 || p->parser->n_err>0 );
  if( p->n_op ){
    assert( p->ops );
    __dbsql_db_free(p->db, p->ops[p->n_op-1].comment);
    p->ops[p->n_op-1].comment = __vm_printf(p->db, format, ap);
  }
}
void __vdbe_comment(vdbe_t *p, const char *format, ...){
  va_list ap;
  if( p ){
    va_start(ap, format);
    __vdbe_v_comment(p, format, ap);
    va_end(ap);
  }
}
void __vdbe_noop_comment(vdbe_t *p, const char *format, ...){
  va_list ap;
  if( p ){
    __vdbe_add_op0(p, OP_Noop);
    va_start(ap, format);
    __vdbe_v_comment(p, format, ap);
    va_end(ap);
  }
}
#endif  /* NDEBUG */

#ifdef DBSQL_VDBE_COVERAGE
/*
** Set the value if the i_src_line field for the previously coded instruction.
*/
void __vdbe_set_line_number(vdbe_t *v, int i_line){
  __vdbe_get_last_op(v)->i_src_line = i_line;
}
#endif /* DBSQL_VDBE_COVERAGE */

/*
** Return the opcode for a given address.  The address must be non-negative.
** See __vdbe_get_last_op() to get the most recently added opcode.
**
** If a memory allocation error has occurred prior to the calling of this
** routine, then a pointer to a dummy vdbe_op_t will be returned.  That opcode
** is readable but not writable, though it is cast to a writable value.
** The return of a dummy opcode allows the call to continue functioning
** after an OOM fault without having to check to see if the return from
** this routine is a valid pointer.  But because the dummy.opcode is 0,
** dummy will never be written to.  This is verified by code inspection and
** by running with Valgrind.
*/
vdbe_op_t *__vdbe_get_op(vdbe_t *p, int addr){
  /* C89 specifies that the constant "dummy" will be initialized to all
  ** zeros, which is correct.  MSVC generates a warning, nevertheless. */
  static vdbe_op_t dummy;  /* Ignore the MSVC warning about no initializer */
  assert( p->vdbe_state==VDBE_INIT_STATE );
  assert( (addr>=0 && addr<p->n_op) || p->db->malloc_failed );
  if( p->db->malloc_failed ){
    return (vdbe_op_t*)&dummy;
  }else{
    return &p->ops[addr];
  }
}

/* Return the most recently added opcode
*/
vdbe_op_t *__vdbe_get_last_op(vdbe_t *p){
  return __vdbe_get_op(p, p->n_op - 1);
}

#if defined(DBSQL_ENABLE_EXPLAIN_COMMENTS)
/*
** Return an integer value for one of the parameters to the opcode op
** determined by character c.
*/
static int __translate_p(char c, const op_t *op){
  if( c=='1' ) return op->p1;
  if( c=='2' ) return op->p2;
  if( c=='3' ) return op->p3;
  if( c=='4' ) return op->p4.i;
  return op->p5;
}

/*
** Compute a string for the "comment" field of a VDBE opcode listing.
**
** The Synopsis: field in comments in the vdbe.c source file gets converted
** to an extra string that is appended to the __opcode_name().  In the
** absence of other comments, this synopsis becomes the comment on the opcode.
** Some translation occurs:
**
**       "PX"      ->  "r[X]"
**       "PX@PY"   ->  "r[X..X+Y-1]"  or "r[x]" if y is 0 or 1
**       "PX@PY+1" ->  "r[X..X+Y]"    or "r[x]" if y is 0
**       "PY..PY"  ->  "r[X..Y]"      or "r[x]" if y<=x
*/
char *__vdbe_display_comment(
  DBSQL *db,       /* Optional - Oom error reporting only */
  const op_t *op,     /* The opcode to be commented */
  const char *p4    /* Previously obtained value for P4 */
){
  const char *op_name;
  const char *synopsis;
  int n_op_name;
  int ii;
  char alt[50];
  str_accum_t x;

  __str_accum_init(&x, 0, 0, 0, DBSQL_MAX_LENGTH);
  op_name = __opcode_name(op->opcode);
  n_op_name = __strlen30(op_name);
  if( op_name[n_op_name+1] ){
    int seen_com = 0;
    char c;
    synopsis = op_name + n_op_name + 1;
    if( strncmp(synopsis,"IF ",3)==0 ){
      dbsql_snprintf(sizeof(alt), alt, "if %s goto P2", synopsis+3);
      synopsis = alt;
    }
    for(ii=0; (c = synopsis[ii])!=0; ii++){
      if( c=='P' ){
        c = synopsis[++ii];
        if( c=='4' ){
          dbsql_str_appendall(&x, p4);
        }else if( c=='X' ){
          if( op->comment && op->comment[0] ){
            dbsql_str_appendall(&x, op->comment);
            seen_com = 1;
            break;
          }
        }else{
          int v1 = __translate_p(c, op);
          int v2;
          if( strncmp(synopsis+ii+1, "@P", 2)==0 ){
            ii += 3;
            v2 = __translate_p(synopsis[ii], op);
            if( strncmp(synopsis+ii+1,"+1",2)==0 ){
              ii += 2;
              v2++;
            }
            if( v2<2 ){
              dbsql_str_appendf(&x, "%d", v1);
            }else{
              dbsql_str_appendf(&x, "%d..%d", v1, v1+v2-1);
            }
          }else if( strncmp(synopsis+ii+1, "@NP", 3)==0 ){
            dbsql_func_t *ctx = op->p4.ctx;
            if( op->p4type!=P4_FUNCCTX || ctx->argc==1 ){
              dbsql_str_appendf(&x, "%d", v1);
            }else if( ctx->argc>1 ){
              dbsql_str_appendf(&x, "%d..%d", v1, v1+ctx->argc-1);
            }else if( x.acc_error==0 ){
              assert( x.n_char>2 );
              x.n_char -= 2;
              ii++;
            }
            ii += 3;
          }else{
            dbsql_str_appendf(&x, "%d", v1);
            if( strncmp(synopsis+ii+1, "..P3", 4)==0 && op->p3==0 ){
              ii += 4;
            }
          }
        }
      }else{
        dbsql_str_appendchar(&x, 1, c);
      }
    }
    if( !seen_com && op->comment ){
      dbsql_str_appendf(&x, "; %s", op->comment);
    }
  }else if( op->comment ){
    dbsql_str_appendall(&x, op->comment);
  }
  if( (x.acc_error & DBSQL_NOMEM)!=0 && db!=0 ){
    __oom_fault(db);
  }
  return __str_accum_finish(&x);
}
#endif /* DBSQL_ENABLE_EXPLAIN_COMMENTS */

#if VDBE_DISPLAY_P4 && defined(DBSQL_ENABLE_CURSOR_HINTS)
/*
** Translate the P4.expr value for an OP_CursorHint opcode into text
** that can be displayed in the P4 column of EXPLAIN output.
*/
static void __display_p4_expr(str_accum_t *p, expr_t *expr){
  const char *op = 0;
  switch( expr->op ){
    case TK_STRING:
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      dbsql_str_appendf(p, "%Q", expr->u.token);
      break;
    case TK_INTEGER:
      dbsql_str_appendf(p, "%d", expr->u.i_value);
      break;
    case TK_NULL:
      dbsql_str_appendf(p, "NULL");
      break;
    case TK_REGISTER: {
      dbsql_str_appendf(p, "r[%d]", expr->i_table);
      break;
    }
    case TK_COLUMN: {
      if( expr->i_column<0 ){
        dbsql_str_appendf(p, "rowid");
      }else{
        dbsql_str_appendf(p, "c%d", (int)expr->i_column);
      }
      break;
    }
    case TK_LT:      op = "LT";      break;
    case TK_LE:      op = "LE";      break;
    case TK_GT:      op = "GT";      break;
    case TK_GE:      op = "GE";      break;
    case TK_NE:      op = "NE";      break;
    case TK_EQ:      op = "EQ";      break;
    case TK_IS:      op = "IS";      break;
    case TK_ISNOT:   op = "ISNOT";   break;
    case TK_AND:     op = "AND";     break;
    case TK_OR:      op = "OR";      break;
    case TK_PLUS:    op = "ADD";     break;
    case TK_STAR:    op = "MUL";     break;
    case TK_MINUS:   op = "SUB";     break;
    case TK_REM:     op = "REM";     break;
    case TK_BITAND:  op = "BITAND";  break;
    case TK_BITOR:   op = "BITOR";   break;
    case TK_SLASH:   op = "DIV";     break;
    case TK_LSHIFT:  op = "LSHIFT";  break;
    case TK_RSHIFT:  op = "RSHIFT";  break;
    case TK_CONCAT:  op = "CONCAT";  break;
    case TK_UMINUS:  op = "MINUS";   break;
    case TK_UPLUS:   op = "PLUS";    break;
    case TK_BITNOT:  op = "BITNOT";  break;
    case TK_NOT:     op = "NOT";     break;
    case TK_ISNULL:  op = "ISNULL";  break;
    case TK_NOTNULL: op = "NOTNULL"; break;

    default:
      dbsql_str_appendf(p, "%s", "expr");
      break;
  }

  if( op ){
    dbsql_str_appendf(p, "%s(", op);
    __display_p4_expr(p, expr->left);
    if( expr->right ){
      dbsql_str_append(p, ",", 1);
      __display_p4_expr(p, expr->right);
    }
    dbsql_str_append(p, ")", 1);
  }
}
#endif /* VDBE_DISPLAY_P4 && defined(DBSQL_ENABLE_CURSOR_HINTS) */


#if VDBE_DISPLAY_P4
/*
** Compute a string that describes the P4 parameter for an opcode.
** Use temp for any required temporary buffer space.
*/
char *__vdbe_display_p4(DBSQL *db, op_t *op){
  char *p4 = 0;
  str_accum_t x;

  __str_accum_init(&x, 0, 0, 0, DBSQL_MAX_LENGTH);
  switch( op->p4type ){
    case P4_KEYINFO: {
      int j;
      key_info_t *key_info = op->p4.key_info;
      assert( key_info->sort_flags!=0 );
      dbsql_str_appendf(&x, "k(%d", key_info->n_key_field);
      for(j=0; j<key_info->n_key_field; j++){
        coll_seq_t *coll = key_info->colls[j];
        const char *z_coll = coll ? coll->name : "";
        if( strcmp(z_coll, "BINARY")==0 ) z_coll = "B";
        dbsql_str_appendf(&x, ",%s%s%s",
               (key_info->sort_flags[j] & KEYINFO_ORDER_DESC) ? "-" : "",
               (key_info->sort_flags[j] & KEYINFO_ORDER_BIGNULL)? "N." : "",
               z_coll);
      }
      dbsql_str_append(&x, ")", 1);
      break;
    }
#ifdef DBSQL_ENABLE_CURSOR_HINTS
    case P4_EXPR: {
      __display_p4_expr(&x, op->p4.expr);
      break;
    }
#endif
    case P4_COLLSEQ: {
      static const char *const encnames[] = {"?", "8", "16LE", "16BE"};
      coll_seq_t *coll = op->p4.coll;
      assert( coll->enc<4 );
      dbsql_str_appendf(&x, "%.18s-%s", coll->name,
                          encnames[coll->enc]);
      break;
    }
    case P4_FUNCDEF: {
      func_def_t *def = op->p4.func;
      dbsql_str_appendf(&x, "%s(%d)", def->name, def->n_arg);
      break;
    }
    case P4_FUNCCTX: {
      func_def_t *def = op->p4.ctx->func;
      dbsql_str_appendf(&x, "%s(%d)", def->name, def->n_arg);
      break;
    }
    case P4_INT64: {
      dbsql_str_appendf(&x, "%lld", *op->p4.i64);
      break;
    }
    case P4_INT32: {
      dbsql_str_appendf(&x, "%d", op->p4.i);
      break;
    }
    case P4_REAL: {
      dbsql_str_appendf(&x, "%.16g", *op->p4.real);
      break;
    }
    case P4_MEM: {
      mem_t *mem = op->p4.mem;
      if( mem->flags & MEM_Str ){
        p4 = mem->z;
      }else if( mem->flags & (MEM_Int|MEM_IntReal) ){
        dbsql_str_appendf(&x, "%lld", mem->u.i);
      }else if( mem->flags & MEM_Real ){
        dbsql_str_appendf(&x, "%.16g", mem->u.r);
      }else if( mem->flags & MEM_Null ){
        p4 = "NULL";
      }else{
        assert( mem->flags & MEM_Blob );
        p4 = "(blob)";
      }
      break;
    }
#ifndef DBSQL_OMIT_VIRTUALTABLE
    case P4_VTAB: {
      dbsql_vtab_t *vtab = op->p4.vtab->vtab;
      dbsql_str_appendf(&x, "vtab:%p", vtab);
      break;
    }
#endif
    case P4_INTARRAY: {
      u_int32_t i;
      u_int32_t *ai = op->p4.ai;
      u_int32_t n = ai[0];   /* The first element of an INTARRAY is always the
                       ** count of the number of elements to follow */
      for(i=1; i<=n; i++){
        dbsql_str_appendf(&x, "%c%u", (i==1 ? '[' : ','), ai[i]);
      }
      dbsql_str_append(&x, "]", 1);
      break;
    }
    case P4_SUBPROGRAM: {
      p4 = "program";
      break;
    }
    case P4_TABLE: {
      p4 = op->p4.tab->name;
      break;
    }
    case P4_INDEX: {
      p4 = op->p4.idx->name;
      break;
    }
    case P4_SUBRTNSIG: {
      subrtn_sig_t *sig = op->p4.subrtn_sig;
      dbsql_str_appendf(&x, "subrtnsig:%d,%s", sig->sel_id, sig->aff);
      break;
    }
    default: {
      p4 = op->p4.z;
    }
  }
  if( p4 ) dbsql_str_appendall(&x, p4);
  if( (x.acc_error & DBSQL_NOMEM)!=0 ){
    __oom_fault(db);
  }
  return __str_accum_finish(&x);
}
#endif /* VDBE_DISPLAY_P4 */

/*
** Declare to the vdbe_t that the BTree object at db->dbs[i] is used.
**
** The prepared statements need to know in advance the complete set of
** attached databases that will be use.  A mask of these databases
** is maintained in p->btree_mask.  The p->lock_mask value is the subset of
** p->btree_mask of databases that will require a lock.
*/
void __vdbe_uses_btree(vdbe_t *p, int i){
  assert( i>=0 && i<p->db->n_db && i<(int)sizeof(db_mask_t)*8 );
  assert( i<(int)sizeof(p->btree_mask)*8 );
  DB_MASK_SET(p->btree_mask, i);
  if( i!=1 && __sm_sharable(p->db->dbs[i].bt) ){
    DB_MASK_SET(p->lock_mask, i);
  }
}

#if !defined(DBSQL_OMIT_SHARED_CACHE)
/*
** If dbsql is compiled to support shared-cache mode and to be threadsafe,
** this routine obtains the mutex associated with each sm_shared_t structure
** that may be accessed by the VM passed as an argument. In doing so it also
** sets the sm_shared_t.db member of each of the sm_shared_t structures, ensuring
** that the correct busy-handler callback is invoked if required.
**
** If dbsql is not threadsafe but does support shared-cache mode, then
** __sm_enter() is invoked to set the sm_shared_t.db variables
** of all of sm_shared_t structures accessible via the database handle
** associated with the VM.
**
** If dbsql is not threadsafe and does not support shared-cache mode, this
** function is a no-op.
**
** The p->btree_mask field is a bitmask of all btrees that the prepared
** statement p will ever use.  Let N be the number of bits in p->btree_mask
** corresponding to btrees that use shared cache.  Then the runtime of
** this routine is N*N.  But as N is rarely more than 1, this should not
** be a problem.
*/
void __vdbe_enter(vdbe_t *p){
  int i;
  DBSQL *db;
  db_t *dbs;
  int n_db;
  if( DB_MASK_ALL_ZERO(p->lock_mask) ) return;  /* The common case */
  db = p->db;
  dbs = db->dbs;
  n_db = db->n_db;
  for(i=0; i<n_db; i++){
    if( i!=1 && DB_MASK_TEST(p->lock_mask,i) && ALWAYS(dbs[i].bt!=0) ){
      __sm_enter(dbs[i].bt);
    }
  }
}
#endif

#if !defined(DBSQL_OMIT_SHARED_CACHE) && DBSQL_THREADSAFE>0
/*
** Unlock all of the btrees previously locked by a call to __vdbe_enter().
*/
static DBSQL_NOINLINE void __vdbe_leave_local(vdbe_t *p){
  int i;
  DBSQL *db;
  db_t *dbs;
  int n_db;
  db = p->db;
  dbs = db->dbs;
  n_db = db->n_db;
  for(i=0; i<n_db; i++){
    if( i!=1 && DB_MASK_TEST(p->lock_mask,i) && ALWAYS(dbs[i].bt!=0) ){
      __sm_leave(dbs[i].bt);
    }
  }
}
void __vdbe_leave(vdbe_t *p){
  if( DB_MASK_ALL_ZERO(p->lock_mask) ) return;  /* The common case */
  __vdbe_leave_local(p);
}
#endif

#if defined(VDBE_PROFILE) || defined(DBSQL_DEBUG)
/*
** Print a single opcode.  This routine is used for debugging only.
*/
void __vdbe_print_op(FILE *out, int pc, vdbe_op_t *op){
  char *p4;
  char *com;
  DBSQL dummy_db;
  static const char *format1 = "%4d %-13s %4d %4d %4d %-13s %.2X %s\n";
  if( out==0 ) out = stdout;
  __begin_benign_malloc();
  dummy_db.malloc_failed = 1;
  p4 = __vdbe_display_p4(&dummy_db, op);
#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
  com = __vdbe_display_comment(0, op, p4);
#else
  com = 0;
#endif
  /* NB:  The __opcode_name() function is implemented by code created
  ** by the mkopcodeh.awk and mkopcodec.awk scripts which extract the
  ** information from the vdbe.c source text */
  fprintf(out, format1, pc,
      __opcode_name(op->opcode), op->p1, op->p2, op->p3,
      p4 ? p4 : "", op->p5,
      com ? com : ""
  );
  fflush(out);
  dbsql_free(p4);
  dbsql_free(com);
  __end_benign_malloc();
}
#endif

/*
** Initialize an array of N mem_t element.
**
** This is a high-runner, so only those fields that really do need to
** be initialized are set.  The mem_t structure is organized so that
** the fields that get initialized are nearby and hopefully on the same
** cache line.
**
**    mem_t.flags = flags
**    mem_t.db = db
**    mem_t.sz_malloc = 0
**
** All other fields of mem_t can safely remain uninitialized for now.  They
** will be initialized before use.
*/
static void __init_mem_array(mem_t *p, int N, DBSQL *db, u_int16_t flags){
  assert( db!=0 );
  if( N>0 ){
    do{
      p->flags = flags;
      p->db = db;
      p->sz_malloc = 0;
#ifdef DBSQL_DEBUG
      p->scopy_from = 0;
      p->scopy = 0;
#endif
      p++;
    }while( (--N)>0 );
  }
}

/*
** Release auxiliary memory held in an array of N mem_t elements.
**
** After this routine returns, all mem_t elements in the array will still
** be valid.  Those mem_t elements that were not holding auxiliary resources
** will be unchanged.  mem_t elements which had something freed will be
** set to MEM_Undefined.
*/
static void __release_mem_array(mem_t *p, int N){
  if( p && N ){
    mem_t *end = &p[N];
    DBSQL *db = p->db;
    assert( db!=0 );
    if( db->pn_bytes_freed ){
      do{
        if( p->sz_malloc ) __dbsql_db_free(db, p->z_malloc);
      }while( (++p)<end );
      return;
    }
    do{
      assert( (&p[1])==end || p[0].db==p[1].db );
      assert( __vdbe_check_mem_invariants(p) );

      /* This block is really an inlined version of __vdbe_mem_release()
      ** that takes advantage of the fact that the memory cell value is
      ** being set to NULL after releasing any dynamic resources.
      **
      ** The justification for duplicating code is that according to
      ** callgrind, this causes a certain test case to hit the CPU 4.7
      ** percent less (x86 linux, gcc version 4.1.2, -O6) than if
      ** __mem_release() were called from here. with_t -O2, this jumps
      ** to 6.6 percent. The test case is inserting 1000 rows into a table
      ** with no indexes using a single prepared INSERT statement, bind()
      ** and reset(). Inserts are grouped into a transaction.
      */
      testcase( p->flags & MEM_Agg );
      testcase( p->flags & MEM_Dyn );
      if( p->flags&(MEM_Agg|MEM_Dyn) ){
        testcase( (p->flags & MEM_Dyn)!=0 && p->x_del==__vdbe_frame_mem_del );
        __vdbe_mem_release(p);
        p->flags = MEM_Undefined;
      }else if( p->sz_malloc ){
        __db_nn_free_nn(db, p->z_malloc);
        p->sz_malloc = 0;
        p->flags = MEM_Undefined;
      }
#ifdef DBSQL_DEBUG
      else{
        p->flags = MEM_Undefined;
      }
#endif
    }while( (++p)<end );
  }
}

#ifdef DBSQL_DEBUG
/*
** Verify that frame is a valid vdbe_frame_t pointer.  Return true if it is
** and false if something is wrong.
**
** This routine is intended for use inside of assert() statements only.
*/
int __vdbe_frame_is_valid(vdbe_frame_t *frame){
  if( frame->i_frame_magic!=DBSQL_FRAME_MAGIC ) return 0;
  return 1;
}
#endif


/*
** This is a destructor on a mem_t object (which is really an dbsql_value_t)
** that deletes the Frame object that is attached to it as a blob.
**
** This routine does not delete the Frame right away.  It merely adds the
** frame to a list of frames to be deleted when the vdbe_t halts.
*/
void __vdbe_frame_mem_del(void *arg){
  vdbe_frame_t *frame = (vdbe_frame_t*)arg;
  assert( __vdbe_frame_is_valid(frame) );
  frame->parent = frame->v->del_frame;
  frame->v->del_frame = frame;
}

#if defined(DBSQL_ENABLE_BYTECODE_VTAB) || !defined(DBSQL_OMIT_EXPLAIN)
/*
** Locate the next opcode to be displayed in EXPLAIN or EXPLAIN
** QUERY PLAN output.
**
** Return DBSQL_ROW on success.  Return DBSQL_DONE if there are no
** more opcodes to be displayed.
*/
int __vdbe_next_opcode(
  vdbe_t *p,         /* The statement being explained */
  mem_t *sub,       /* Storage for keeping track of subprogram nesting */
  int mode,       /* 0: normal.  1: EQP.  2:  __tables_used */
  int *pi_pc,       /* IN/OUT: Current rowid.  Overwritten with next rowid */
  int *pi_addr,     /* OUT: Write index into (*pa_op)[] here */
  op_t **pa_op        /* OUT: Write the opcode array here */
){
  int n_row;                            /* Stop when row count reaches this */
  int n_sub = 0;                        /* Number of sub-vdbes seen so far */
  sub_program_t **subs = 0;              /* Array of sub-vdbes */
  int i;                               /* Next instruction address */
  int rc = DBSQL_OK;                  /* Result code */
  op_t *ops = 0;                         /* Opcode array */
  int i_pc;                             /* Rowid.  Copy of value in *pi_pc */

  /* When the number of output rows reaches n_row, that means the
  ** listing has finished and dbsql_step() should return DBSQL_DONE.
  ** n_row is the sum of the number of rows in the main program, plus
  ** the sum of the number of rows in all trigger subprograms encountered
  ** so far.  The n_row value will increase as new trigger subprograms are
  ** encountered, but p->pc will eventually catch up to n_row.
  */
  n_row = p->n_op;
  if( sub!=0 ){
    if( sub->flags&MEM_Blob ){
      /* sub is initiallly NULL.  It is initialized to a BLOB by
      ** the P4_SUBPROGRAM processing logic below */
      n_sub = sub->n/sizeof(vdbe_t*);
      subs = (sub_program_t **)sub->z;
    }
    for(i=0; i<n_sub; i++){
      n_row += subs[i]->n_op;
    }
  }
  i_pc = *pi_pc;
  while(1){  /* Loop exits via break */
    i = i_pc++;
    if( i>=n_row ){
      p->rc = DBSQL_OK;
      rc = DBSQL_DONE;
      break;
    }
    if( i<p->n_op ){
      /* The rowid is small enough that we are still in the
      ** main program. */
      ops = p->ops;
    }else{
      /* We are currently listing subprograms.  Figure out which one and
      ** pick up the appropriate opcode. */
      int j;
      i -= p->n_op;
      assert( subs!=0 );
      assert( n_sub>0 );
      for(j=0; i>=subs[j]->n_op; j++){
        i -= subs[j]->n_op;
        assert( i<subs[j]->n_op || j+1<n_sub );
      }
      ops = subs[j]->ops;
    }

    /* When an OP_Program opcode is encounter (the only opcode that has
    ** a P4_SUBPROGRAM argument), expand the size of the array of subprograms
    ** kept in p->mems[9].z to hold the new program - assuming this subprogram
    ** has not already been seen.
    */
    if( sub!=0 && ops[i].p4type==P4_SUBPROGRAM ){
      int n_byte = (n_sub+1)*sizeof(sub_program_t*);
      int j;
      for(j=0; j<n_sub; j++){
        if( subs[j]==ops[i].p4.program ) break;
      }
      if( j==n_sub ){
        p->rc = __vdbe_mem_grow(sub, n_byte, n_sub!=0);
        if( p->rc!=DBSQL_OK ){
          rc = DBSQL_ERROR;
          break;
        }
        subs = (sub_program_t **)sub->z;
        subs[n_sub++] = ops[i].p4.program;
        MEM_SET_TYPE_FLAG(sub, MEM_Blob);
        sub->n = n_sub*sizeof(sub_program_t*);
        n_row += ops[i].p4.program->n_op;
      }
    }
    if( mode==0 ) break;
#ifdef DBSQL_ENABLE_BYTECODE_VTAB
    if( mode==2 ){
      op_t *op = ops + i;
      if( op->opcode==OP_OpenRead ) break;
      if( op->opcode==OP_OpenWrite && (op->p5 & OPFLAG_P2ISREG)==0 ) break;
      if( op->opcode==OP_ReopenIdx ) break;     
    }else
#endif
    {
      assert( mode==1 );
      if( ops[i].opcode==OP_Explain ) break;
      if( ops[i].opcode==OP_Init && i_pc>1 ) break;
    }
  }
  *pi_pc = i_pc;
  *pi_addr = i;
  *pa_op = ops;
  return rc;
}
#endif /* DBSQL_ENABLE_BYTECODE_VTAB || !DBSQL_OMIT_EXPLAIN */


/*
** Delete a vdbe_frame_t object and its contents. vdbe_frame_t objects are
** allocated by the OP_Program opcode in __vdbe_exec().
*/
void __vdbe_frame_delete(vdbe_frame_t *p){
  int i;
  mem_t *mems = VDBE_FRAME_MEM(p);
  vdbe_cursor_t **csrs = (vdbe_cursor_t **)&mems[p->n_child_mem];
  assert( __vdbe_frame_is_valid(p) );
  for(i=0; i<p->n_child_csr; i++){
    if( csrs[i] ) __vdbe_free_cursor_nn(p->v, csrs[i]);
  }
  __release_mem_array(mems, p->n_child_mem);
  __vdbe_delete_aux_data(p->v->db, &p->aux_data, -1, 0);
  __dbsql_db_free(p->v->db, p);
}

#ifndef DBSQL_OMIT_EXPLAIN
/*
** Give a listing of the program in the virtual machine.
**
** The interface is the same as __vdbe_exec().  But instead of
** running the code, it invokes the callback once for each instruction.
** This feature is used to implement "EXPLAIN".
**
** When p->explain==1, each instruction is listed.  When
** p->explain==2, only OP_Explain instructions are listed and these
** are shown in a different format.  p->explain==2 is used to implement
** EXPLAIN QUERY PLAN.
** 2018-04-24:  In p->explain==2 mode, the OP_Init opcodes of triggers
** are also shown, so that the boundaries between the main program and
** each trigger are clear.
**
** When p->explain==1, first the main program is listed, then each of
** the trigger subprograms are listed one by one.
*/
int __vdbe_list(
  vdbe_t *p                   /* The VDBE */
){
  mem_t *sub = 0;                       /* Memory cell hold array of subprogs */
  DBSQL *db = p->db;                 /* The database connection */
  int i;                               /* Loop counter */
  int rc = DBSQL_OK;                  /* Return code */
  mem_t *mem = &p->mems[1];             /* First mem_t of result set */
  int list_subprogs = (p->explain==1 || (db->flags & DBSQL_TriggerEQP)!=0);
  op_t *ops;                             /* Array of opcodes */
  op_t *op;                             /* Current opcode */

  assert( p->explain );
  assert( p->vdbe_state==VDBE_RUN_STATE );
  assert( p->rc==DBSQL_OK || p->rc==DBSQL_BUSY || p->rc==DBSQL_NOMEM );

  /* Even though this opcode does not use dynamic strings for
  ** the result, result columns may become dynamic if the user calls
  ** dbsql_column_text16(), causing a translation to UTF-16 encoding.
  */
  __release_mem_array(mem, 8);

  if( p->rc==DBSQL_NOMEM ){
    /* This happens if a malloc() inside a call to dbsql_column_text() or
    ** dbsql_column_text16() failed.  */
    __oom_fault(db);
    return DBSQL_ERROR;
  }

  if( list_subprogs ){
    /* The first 8 memory cells are used for the result set.  So we will
    ** commandeer the 9th cell to use as storage for an array of pointers
    ** to trigger subprograms.  The VDBE is guaranteed to have at least 9
    ** cells.  */
    assert( p->n_mem>9 );
    sub = &p->mems[9];
  }else{
    sub = 0;
  }

  /* Figure out which opcode is next to display */
  rc = __vdbe_next_opcode(p, sub, p->explain==2, &p->pc, &i, &ops);

  if( rc==DBSQL_OK ){
    op = ops + i;
    if( ATOMIC_LOAD(&db->u1.is_interrupted) ){
      p->rc = DBSQL_INTERRUPT;
      rc = DBSQL_ERROR;
      __vdbe_error(p, __err_str(p->rc));
    }else{
      char *p4 = __vdbe_display_p4(db, op);
      if( p->explain==2 ){
        __vdbe_mem_set_int64(mem, op->p1);
        __vdbe_mem_set_int64(mem+1, op->p2);
        __vdbe_mem_set_int64(mem+2, op->p3);
        __vdbe_mem_set_str(mem+3, p4, -1, DBSQL_UTF8, dbsql_free);
        assert( p->n_res_column==4 );
      }else{
        __vdbe_mem_set_int64(mem+0, i);
        __vdbe_mem_set_str(mem+1, (char*)__opcode_name(op->opcode),
                             -1, DBSQL_UTF8, DBSQL_STATIC);
        __vdbe_mem_set_int64(mem+2, op->p1);
        __vdbe_mem_set_int64(mem+3, op->p2);
        __vdbe_mem_set_int64(mem+4, op->p3);
        /* mem+5 for p4 is done last */
        __vdbe_mem_set_int64(mem+6, op->p5);
#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
        {
          char *com = __vdbe_display_comment(db, op, p4);
          __vdbe_mem_set_str(mem+7, com, -1, DBSQL_UTF8, dbsql_free);
        }
#else
        __vdbe_mem_set_null(mem+7);
#endif
        __vdbe_mem_set_str(mem+5, p4, -1, DBSQL_UTF8, dbsql_free);
        assert( p->n_res_column==8 );
      }
      p->result_row = mem;
      if( db->malloc_failed ){
        p->rc = DBSQL_NOMEM;
        rc = DBSQL_ERROR;
      }else{
        p->rc = DBSQL_OK;
        rc = DBSQL_ROW;
      }
    }
  }
  return rc;
}
#endif /* DBSQL_OMIT_EXPLAIN */

#ifdef DBSQL_DEBUG
/*
** Print the SQL that was used to generate a VDBE program.
*/
void __vdbe_print_sql(vdbe_t *p){
  const char *z = 0;
  if( p->z_sql ){
    z = p->z_sql;
  }else if( p->n_op>=1 ){
    const vdbe_op_t *op = &p->ops[0];
    if( op->opcode==OP_Init && op->p4.z!=0 ){
      z = op->p4.z;
      while( __isspace(*z) ) z++;
    }
  }
  if( z ) printf("SQL: [%s]\n", z);
}
#endif

#if !defined(DBSQL_OMIT_TRACE) && defined(DBSQL_ENABLE_IOTRACE)
/*
** Print an IOTRACE message showing SQL content.
*/
void __vdbe_io_trace_sql(vdbe_t *p){
  int n_op = p->n_op;
  vdbe_op_t *op;
  if( __io_trace==0 ) return;
  if( n_op<1 ) return;
  op = &p->ops[0];
  if( op->opcode==OP_Init && op->p4.z!=0 ){
    int i, j;
    char z[1000];
    dbsql_snprintf(sizeof(z), z, "%s", op->p4.z);
    for(i=0; __isspace(z[i]); i++){}
    for(j=0; z[i]; i++){
      if( __isspace(z[i]) ){
        if( z[i-1]!=' ' ){
          z[j++] = ' ';
        }
      }else{
        z[j++] = z[i];
      }
    }
    z[j] = 0;
    __io_trace("SQL %s\n", z);
  }
}
#endif /* !DBSQL_OMIT_TRACE && DBSQL_ENABLE_IOTRACE */

/* An instance of this object describes bulk memory available for use
** by subcomponents of a prepared statement.  Space is allocated out
** of a reusable_space_t object by the __alloc_space() routine below.
*/
struct reusable_space_t {
  u_int8_t *space;            /* Available memory */
  int64_t n_free;   /* Bytes of available memory */
  int64_t n_needed; /* Total bytes that could not be allocated */
};

/* Try to allocate n_byte bytes of 8-byte aligned bulk memory for buf
** from the reusable_space_t object.  Return a pointer to the allocated
** memory on success.  If insufficient memory is available in the
** reusable_space_t object, increase the reusable_space_t.n_needed
** value by the amount needed and return NULL.
**
** If buf is not initially NULL, that means that the memory has already
** been allocated by a prior call to this routine, so just return a copy
** of buf and leave reusable_space_t unchanged.
**
** This allocator is employed to repurpose unused slots at the end of the
** opcode array of prepared state for other memory needs of the prepared
** statement.
*/
static void *__alloc_space(
  struct reusable_space_t *p,  /* Bulk memory available for allocation */
  void *buf,               /* Pointer to a prior allocation */
  int64_t n_byte       /* Bytes of memory needed. */
){
  assert( EIGHT_BYTE_ALIGNMENT(p->space) );
  if( buf==0 ){
    n_byte = ROUND8P(n_byte);
    if( n_byte <= p->n_free ){
      p->n_free -= n_byte;
      buf = &p->space[p->n_free];
    }else{
      p->n_needed += n_byte;
    }
  }
  assert( EIGHT_BYTE_ALIGNMENT(buf) );
  return buf;
}

/*
** Rewind the VDBE back to the beginning in preparation for
** running it.
*/
void __vdbe_rewind(vdbe_t *p){
#if defined(DBSQL_DEBUG)
  int i;
#endif
  assert( p!=0 );
  assert( p->vdbe_state==VDBE_INIT_STATE
       || p->vdbe_state==VDBE_READY_STATE
       || p->vdbe_state==VDBE_HALT_STATE );

  /* There should be at least one opcode.
  */
  assert( p->n_op>0 );

  p->vdbe_state = VDBE_READY_STATE;

#ifdef DBSQL_DEBUG
  for(i=0; i<p->n_mem; i++){
    assert( p->mems[i].db==p->db );
  }
#endif
  p->pc = -1;
  p->rc = DBSQL_OK;
  p->error_action = OE_Abort;
  p->n_change = 0;
  p->cache_ctr = 1;
  p->min_write_file_format = 255;
  p->i_statement = 0;
  p->n_fk_constraint = 0;
#ifdef VDBE_PROFILE
  for(i=0; i<p->n_op; i++){
    p->ops[i].n_exec = 0;
    p->ops[i].n_cycle = 0;
  }
#endif
}

/*
** Prepare a virtual machine for execution for the first time after
** creating the virtual machine.  This involves things such
** as allocating registers and initializing the program counter.
** After the VDBE has be prepped, it can be executed by one or more
** calls to __vdbe_exec(). 
**
** This function may be called exactly once on each virtual machine.
** After this routine is called the VM has been "packaged" and is ready
** to run.  After this routine is called, further calls to
** __vdbe_add_op() functions are prohibited.  This routine disconnects
** the vdbe_t from the parser_t object that helped generate it so that the
** the vdbe_t becomes an independent entity and the parser_t object can be
** destroyed.
**
** Use the __vdbe_rewind() procedure to restore a virtual machine back
** to its initial state after it has been run.
*/
void __vdbe_make_ready(
  vdbe_t *p,                       /* The VDBE */
  parser_t *parser                  /* Parsing context */
){
  DBSQL *db;                   /* The database connection */
  int n_var;                      /* Number of parameters */
  int n_mem;                      /* Number of VM memory registers */
  int n_cursor;                   /* Number of cursors required */
  int n_arg;                      /* Max number args to filter or update */
  int n;                         /* Loop counter */
  struct reusable_space_t x;        /* Reusable bulk memory */

  assert( p!=0 );
  assert( p->n_op>0 );
  assert( parser!=0 );
  assert( p->vdbe_state==VDBE_INIT_STATE );
  assert( parser==p->parser );
  assert( parser->db==p->db );
  p->v_list = parser->v_list;
  parser->v_list =  0;
  db = p->db;
  assert( db->malloc_failed==0 );
  n_var = parser->n_var;
  n_mem = parser->n_mem;
  n_cursor = parser->n_tab;
  n_arg = parser->n_max_arg;
 
  /* Each cursor uses a memory cell.  The first cursor (cursor 0) can
  ** use mems[0] which is not otherwise used by the VDBE program.  Allocate
  ** space at the end of mems[] for cursors 1 and greater.
  ** See also: __allocate_cursor().
  */
  n_mem += n_cursor;
  if( n_cursor==0 && n_mem>0 ) n_mem++;  /* Space for mems[0] even if not used */

  /* Figure out how much reusable memory is available at the end of the
  ** opcode array.  This extra memory will be reallocated for other elements
  ** of the prepared statement.
  */
  n = ROUND8P(sizeof(op_t)*p->n_op);             /* Bytes of opcode memory used */
  x.space = &((u_int8_t*)p->ops)[n];               /* Unused opcode memory */
  assert( EIGHT_BYTE_ALIGNMENT(x.space) );
  x.n_free = ROUNDDOWN8(parser->sz_op_alloc - n);  /* Bytes of unused memory */
  assert( x.n_free>=0 );
  assert( EIGHT_BYTE_ALIGNMENT(&x.space[x.n_free]) );

  __resolve_p2_values(p, &n_arg);
  p->uses_stmt_journal = (u_int8_t)(parser->is_multi_write && parser->may_abort);
  if( parser->explain ){
    if( n_mem<10 ) n_mem = 10;
    p->explain = parser->explain;
    p->n_res_column = 12 - 4*p->explain;
  }
  p->expired = 0;

  /* Memory for registers, parameters, cursor, etc, is allocated in one or two
  ** passes.  On the first pass, we try to reuse unused memory at the
  ** end of the opcode array.  If we are unable to satisfy all memory
  ** requirements by reusing the opcode array tail, then the second
  ** pass will fill in the remainder using a fresh memory allocation. 
  **
  ** This two-pass approach that reuses as much memory as possible from
  ** the leftover memory at the end of the opcode array.  This can significantly
  ** reduce the amount of memory held by a prepared statement.
  */
  x.n_needed = 0;
  p->mems = __alloc_space(&x, 0, n_mem*sizeof(mem_t));
  p->vars = __alloc_space(&x, 0, n_var*sizeof(mem_t));
  p->args = __alloc_space(&x, 0, n_arg*sizeof(mem_t*));
  p->csrs = __alloc_space(&x, 0, n_cursor*sizeof(vdbe_cursor_t*));
  if( x.n_needed ){
    x.space = p->p_free = __db_malloc_raw_nn(db, x.n_needed);
    x.n_free = x.n_needed;
    if( !db->malloc_failed ){
      p->mems = __alloc_space(&x, p->mems, n_mem*sizeof(mem_t));
      p->vars = __alloc_space(&x, p->vars, n_var*sizeof(mem_t));
      p->args = __alloc_space(&x, p->args, n_arg*sizeof(mem_t*));
      p->csrs = __alloc_space(&x, p->csrs, n_cursor*sizeof(vdbe_cursor_t*));
    }
  }
#ifdef DBSQL_DEBUG
  p->nap_arg = n_arg;
#endif

  if( db->malloc_failed ){
    p->n_var = 0;
    p->n_cursor = 0;
    p->n_mem = 0;
  }else{
    p->n_cursor = n_cursor;
    p->n_var = (yn_var_t)n_var;
    __init_mem_array(p->vars, n_var, db, MEM_Null);
    p->n_mem = n_mem;
    __init_mem_array(p->mems, n_mem, db, MEM_Undefined);
    memset(p->csrs, 0, n_cursor*sizeof(vdbe_cursor_t*));
  }
  __vdbe_rewind(p);
}

/*
** Close a VDBE cursor and release all the resources that cursor
** happens to hold.
*/
void __vdbe_free_cursor(vdbe_t *p, vdbe_cursor_t *cx){
  if( cx ) __vdbe_free_cursor_nn(p,cx);
}
static DBSQL_NOINLINE void __free_cursor_with_cache(vdbe_t *p, vdbe_cursor_t *cx){
  vdbe_txt_blb_cache_t *cache = cx->cache;
  assert( cx->col_cache );
  cx->col_cache = 0;
  cx->cache = 0;
  if( cache->c_value ){
    __rc_str_unref(cache->c_value);
    cache->c_value = 0;
  }
  __dbsql_db_free(p->db, cache);
  __vdbe_free_cursor_nn(p, cx);
}
void __vdbe_free_cursor_nn(vdbe_t *p, vdbe_cursor_t *cx){
  if( cx->col_cache ){
    __free_cursor_with_cache(p, cx);
    return;
  }
  switch( cx->cur_type ){
    case CURTYPE_SORTER: {
      __vdbe_sorter_close(p->db, cx);
      break;
    }
    case CURTYPE_BTREE: {
      assert( cx->uc.cursor!=0 );
      __sm_close_cursor(cx->uc.cursor);
      break;
    }
#ifndef DBSQL_OMIT_VIRTUALTABLE
    case CURTYPE_VTAB: {
      dbsql_vtab_cursor_t *v_cur = cx->uc.v_cur;
      const dbsql_module_t *module = v_cur->vtab->module;
      assert( v_cur->vtab->n_ref>0 );
      v_cur->vtab->n_ref--;
      module->x_close(v_cur);
      break;
    }
#endif
  }
}

/*
** Close all cursors in the current frame.
*/
static void __close_cursors_in_frame(vdbe_t *p){
  int i;
  for(i=0; i<p->n_cursor; i++){
    vdbe_cursor_t *c = p->csrs[i];
    if( c ){
      __vdbe_free_cursor_nn(p, c);
      p->csrs[i] = 0;
    }
  }
}

/*
** Copy the values stored in the vdbe_frame_t structure to its vdbe_t. This
** is used, for example, when a trigger sub-program is halted to restore
** control to the main program.
*/
int __vdbe_frame_restore(vdbe_frame_t *frame){
  vdbe_t *v = frame->v;
  __close_cursors_in_frame(v);
  v->ops = frame->ops;
  v->n_op = frame->n_op;
  v->mems = frame->mems;
  v->n_mem = frame->n_mem;
  v->csrs = frame->csrs;
  v->n_cursor = frame->n_cursor;
  v->db->last_rowid = frame->last_rowid;
  v->n_change = frame->n_change;
  v->db->n_change = frame->n_db_change;
  __vdbe_delete_aux_data(v->db, &v->aux_data, -1, 0);
  v->aux_data = frame->aux_data;
  frame->aux_data = 0;
  return frame->pc;
}

/*
** Close all cursors.
**
** Also release any dynamic memory held by the VM in the vdbe_t.mems memory
** cell array. This is necessary as the memory cell array may contain
** pointers to vdbe_frame_t objects, which may in turn contain pointers to
** open cursors.
*/
static void __close_all_cursors(vdbe_t *p){
  if( p->frame ){
    vdbe_frame_t *frame;
    for(frame=p->frame; frame->parent; frame=frame->parent);
    __vdbe_frame_restore(frame);
    p->frame = 0;
    p->n_frame = 0;
  }
  assert( p->n_frame==0 );
  __close_cursors_in_frame(p);
  __release_mem_array(p->mems, p->n_mem);
  while( p->del_frame ){
    vdbe_frame_t *del = p->del_frame;
    p->del_frame = del->parent;
    __vdbe_frame_delete(del);
  }

  /* Delete any auxdata allocations made by the VM */
  if( p->aux_data ) __vdbe_delete_aux_data(p->db, &p->aux_data, -1, 0);
  assert( p->aux_data==0 );
}

/*
** Set the number of result columns that will be returned by this SQL
** statement. This is now set at compile time, rather than during
** execution of the vdbe program so that dbsql_column_count() can
** be called on an SQL statement before dbsql_step().
*/
void __vdbe_set_num_cols(vdbe_t *p, int n_res_column){
  int n;
  DBSQL *db = p->db;

  if( p->n_res_alloc ){
    __release_mem_array(p->col_names, p->n_res_alloc*COLNAME_N);
    __dbsql_db_free(db, p->col_names);
  }
  n = n_res_column*COLNAME_N;
  p->n_res_column = p->n_res_alloc = (u_int16_t)n_res_column;
  p->col_names = (mem_t*)__db_malloc_raw_nn(db, sizeof(mem_t)*n );
  if( p->col_names==0 ) return;
  __init_mem_array(p->col_names, n, db, MEM_Null);
}

/*
** Set the name of the idx'th column to be returned by the SQL statement.
** name must be a pointer to a nul terminated string.
**
** This call must be made after a call to __vdbe_set_num_cols().
**
** The final parameter, del, must be one of DBSQL_DYNAMIC, DBSQL_STATIC
** or DBSQL_TRANSIENT. If it is DBSQL_DYNAMIC, then the buffer pointed
** to by name will be freed by __dbsql_db_free() when the vdbe is destroyed.
*/
int __vdbe_set_col_name(
  vdbe_t *p,                         /* vdbe_t being configured */
  int idx,                         /* index_t of column name applies to */
  int var,                         /* One of the COLNAME_* constants */
  const char *name,               /* Pointer to buffer containing name */
  void (*del)(void*)              /* Memory management strategy for name */
){
  int rc;
  mem_t *col_name;
  assert( idx<p->n_res_alloc );
  assert( var<COLNAME_N );
  if( p->db->malloc_failed ){
    assert( !name || del!=DBSQL_DYNAMIC );
    return DBSQL_NOMEM_BKPT;
  }
  assert( p->col_names!=0 );
  col_name = &(p->col_names[idx+var*p->n_res_alloc]);
  rc = __vdbe_mem_set_text(col_name, name, -1, del);
  assert( rc!=0 || !name || (col_name->flags&MEM_Term)!=0 );
  return rc;
}

/*
** A read or write transaction may or may not be active on database handle
** db. If a transaction is active, commit it. If there is a
** write-transaction spanning more than one database file, this routine
** takes care of the super-journal trickery.
*/
static int __vdbe_commit(DBSQL *db, vdbe_t *p){
  int i;
  int n_trans = 0;  /* Number of databases with an active write-transaction
                   ** that are candidates for a two-phase commit using a
                   ** super-journal */
  int rc = DBSQL_OK;
  int need_xcommit = 0;

#ifdef DBSQL_OMIT_VIRTUALTABLE
  /* with_t this option, __vtab_sync() is defined to be simply
  ** DBSQL_OK so p is not used.
  */
  UNUSED_PARAMETER(p);
#endif

  /* Before doing anything else, call the x_sync() callback for any
  ** virtual module tables written in this transaction. This has to
  ** be done before determining whether a super-journal file is
  ** required, as an x_sync() callback may add an attached database
  ** to the transaction.
  */
  rc = __vtab_sync(db, p);

  /* This loop determines (a) if the commit hook should be invoked and
  ** (b) how many database files have open write transactions, not
  ** including the temp database. (b) is important because if more than
  ** one database file has an open write transaction, a super-journal
  ** file is required for an atomic commit.
  */
  for(i=0; rc==DBSQL_OK && i<db->n_db; i++){
    sm_t *bt = db->dbs[i].bt;
    if( __sm_txn_state(bt)==DBSQL_TXN_WRITE ){
      /* Whether or not a database might need a super-journal depends upon
      ** its journal mode (among other things).  This matrix determines which
      ** journal modes use a super-journal and which do not */
      static const u_int8_t mj_neededs[] = {
        /* DELETE   */  1,
        /* PERSIST   */ 1,
        /* OFF       */ 0,
        /* TRUNCATE  */ 1,
        /* MEMORY    */ 0,
        /* WAL       */ 0
      };
      pager_t *pager;   /* pager_t associated with bt */
      need_xcommit = 1;
      __sm_enter(bt);
      pager = __sm_pager(bt);
      if( db->dbs[i].safety_level!=PAGER_SYNCHRONOUS_OFF
       && mj_neededs[__sm_pager_get_journal_mode(pager)]
       && __sm_pager_is_memdb(pager)==0
      ){
        assert( i!=1 );
        n_trans++;
      }
      rc = __sm_pager_exclusive_lock(pager);
      __sm_leave(bt);
    }
  }
  if( rc!=DBSQL_OK ){
    return rc;
  }

  /* If there are any write-transactions at all, invoke the commit hook */
  if( need_xcommit && db->commit_callback ){
    rc = db->commit_callback(db->commit_arg);
    if( rc ){
      return DBSQL_CONSTRAINT_COMMITHOOK;
    }
  }

  /* The simple case - no more than one database file (not counting the
  ** TEMP database) has a transaction active.   There is no need for the
  ** super-journal.
  **
  ** If the return value of __sm_get_filename() is a zero length
  ** string, it means the main database is :memory: or a temp file.  In
  ** that case we do not support atomic multi-file commits, so use the
  ** simple case then too.
  */
  if( 0==__strlen30(__sm_get_filename(db->dbs[0].bt))
   || n_trans<=1
  ){
    if( need_xcommit ){
      for(i=0; rc==DBSQL_OK && i<db->n_db; i++){
        sm_t *bt = db->dbs[i].bt;
        if( __sm_txn_state(bt)>=DBSQL_TXN_WRITE ){
          rc = __sm_commit_phase_one(bt, 0);
        }
      }
    }

    /* Do the commit only if all databases successfully complete phase 1.
    ** If one of the __btree_commit_phase_one() calls fails, this indicates an
    ** IO error while deleting or truncating a journal file. It is unlikely,
    ** but could happen. In this case abandon processing and return the error.
    */
    for(i=0; rc==DBSQL_OK && i<db->n_db; i++){
      sm_t *bt = db->dbs[i].bt;
      int txn = __sm_txn_state(bt);
      if( txn!=DBSQL_TXN_NONE ){
        assert( need_xcommit || txn==DBSQL_TXN_READ );
        rc = __sm_commit_phase_two(bt, 0);
      }
    }
    if( rc==DBSQL_OK ){
      __vtab_commit(db);
    }
  }

  /* The complex case - There is a multi-file write-transaction active.
  ** This requires a super-journal file to ensure the transaction is
  ** committed atomically.
  */
#ifndef DBSQL_OMIT_DISKIO
  else{
    dbsql_vfs_t *vfs = db->vfs;
    char *super = 0;   /* File-name for the super-journal */
    char const *main_file = __sm_get_filename(db->dbs[0].bt);
    dbsql_file_t *super_jrnl = 0;
    int64_t offset = 0;
    int res;
    int retry_count = 0;
    int n_main_file;

    /* select_t a super-journal file name */
    n_main_file = __strlen30(main_file);
    super = __m_printf(db, "%.4c%s%.16c", 0,main_file,0);
    if( super==0 ) return DBSQL_NOMEM_BKPT;
    super += 4;
    do {
      u_int32_t i_random;
      if( retry_count ){
        if( retry_count>100 ){
          dbsql_log(DBSQL_FULL, "MJ delete: %s", super);
          __dbsql_os_delete(vfs, super, 0);
          break;
        }else if( retry_count==1 ){
          dbsql_log(DBSQL_FULL, "MJ collide: %s", super);
        }
      }
      retry_count++;
      dbsql_randomness(sizeof(i_random), &i_random);
      dbsql_snprintf(13, &super[n_main_file], "-mj%06X9%02X",
                               (i_random>>8)&0xffffff, i_random&0xff);
      /* The antipenultimate character of the super-journal name must
      ** be "9" to avoid name collisions when using 8+3 filenames. */
      assert( super[__strlen30(super)-3]=='9' );
      __file_suffix3(main_file, super);
      rc = __dbsql_os_access(vfs, super, DBSQL_ACCESS_EXISTS, &res);
    }while( rc==DBSQL_OK && res );
    if( rc==DBSQL_OK ){
      /* Open the super-journal. */
      rc = __dbsql_os_open_malloc(vfs, super, &super_jrnl,
          DBSQL_OPEN_READWRITE|DBSQL_OPEN_CREATE|
          DBSQL_OPEN_EXCLUSIVE|DBSQL_OPEN_SUPER_JOURNAL, 0
      );
    }
    if( rc!=DBSQL_OK ){
      __dbsql_db_free(db, super-4);
      return rc;
    }

    /* Write the name of each database file in the transaction into the new
    ** super-journal file. If an error occurs at this point close
    ** and delete the super-journal file. All the individual journal files
    ** still have 'null' as the super-journal pointer, so they will roll
    ** back independently if a failure occurs.
    */
    for(i=0; i<db->n_db; i++){
      sm_t *bt = db->dbs[i].bt;
      if( __sm_txn_state(bt)==DBSQL_TXN_WRITE ){
        char const *file = __sm_get_journalname(bt);
        if( file==0 ){
          continue;  /* Ignore TEMP and :memory: databases */
        }
        assert( file[0]!=0 );
        rc = __dbsql_os_write(super_jrnl, file, __strlen30(file)+1,offset);
        offset += __strlen30(file)+1;
        if( rc!=DBSQL_OK ){
          __dbsql_os_close_free(super_jrnl);
          __dbsql_os_delete(vfs, super, 0);
          __dbsql_db_free(db, super-4);
          return rc;
        }
      }
    }

    /* Sync the super-journal file. If the IOCAP_SEQUENTIAL device
    ** flag is set this is not required.
    */
    if( 0==(__dbsql_os_device_characteristics(super_jrnl)&DBSQL_IOCAP_SEQUENTIAL)
     && DBSQL_OK!=(rc = __dbsql_os_sync(super_jrnl, DBSQL_SYNC_NORMAL))
    ){
      __dbsql_os_close_free(super_jrnl);
      __dbsql_os_delete(vfs, super, 0);
      __dbsql_db_free(db, super-4);
      return rc;
    }

    /* Sync all the db files involved in the transaction. The same call
    ** sets the super-journal pointer in each individual journal. If
    ** an error occurs here, do not delete the super-journal file.
    **
    ** If the error occurs during the first call to
    ** __sm_commit_phase_one(), then there is a chance that the
    ** super-journal file will be orphaned. But we cannot delete it,
    ** in case the super-journal file name was written into the journal
    ** file before the failure occurred.
    */
    for(i=0; rc==DBSQL_OK && i<db->n_db; i++){
      sm_t *bt = db->dbs[i].bt;
      if( bt ){
        rc = __sm_commit_phase_one(bt, super);
      }
    }
    __dbsql_os_close_free(super_jrnl);
    assert( rc!=DBSQL_BUSY );
    if( rc!=DBSQL_OK ){
      __dbsql_db_free(db, super-4);
      return rc;
    }

    /* Delete the super-journal file. This commits the transaction. After
    ** doing this the directory is synced again before any individual
    ** transaction files are deleted.
    */
    rc = __dbsql_os_delete(vfs, super, 1);
    __dbsql_db_free(db, super-4);
    super = 0;
    if( rc ){
      return rc;
    }

    /* All files and directories have already been synced, so the following
    ** calls to __sm_commit_phase_two() are only closing files and
    ** deleting or truncating journals. If something goes wrong while
    ** this is happening we don't really care. The integrity of the
    ** transaction is already guaranteed, but some stray 'cold' journals
    ** may be lying around. returning_t an error code won't help matters.
    */
    disable_simulated_io_errors();
    __begin_benign_malloc();
    for(i=0; i<db->n_db; i++){
      sm_t *bt = db->dbs[i].bt;
      if( bt ){
        __sm_commit_phase_two(bt, 1);
      }
    }
    __end_benign_malloc();
    enable_simulated_io_errors();

    __vtab_commit(db);
  }
#endif

  return rc;
}

/*
** This routine checks that the DBSQL.n_vdbe_active count variable
** matches the number of vdbe's in the list DBSQL.vdbe that are
** currently active. An assertion fails if the two counts do not match.
** This is an internal self-check only - it is not an essential processing
** step.
**
** This is a no-op if NDEBUG is defined.
*/
#ifndef NDEBUG
static void CHECK_ACTIVE_VDBE_CNT(DBSQL *db){
  vdbe_t *p;
  int cnt = 0;
  int n_write = 0;
  int n_read = 0;
  p = db->vdbe;
  while( p ){
    if( dbsql_stmt_busy((dbsql_stmt_t*)p) ){
      cnt++;
      if( p->readOnly==0 ) n_write++;
      if( p->is_reader ) n_read++;
    }
    p = p->v_next;
  }
  assert( cnt==db->n_vdbe_active );
  assert( n_write==db->n_vdbe_write );
  assert( n_read==db->n_vdbe_read );
}
#else
#define CHECK_ACTIVE_VDBE_CNT(x)
#endif

/*
** If the vdbe_t passed as the first argument opened a statement-transaction,
** close it now. Argument op must be either SAVEPOINT_ROLLBACK or
** SAVEPOINT_RELEASE. If it is SAVEPOINT_ROLLBACK, then the statement
** transaction is rolled back. If op is SAVEPOINT_RELEASE, then the
** statement transaction is committed.
**
** If an IO error occurs, an DBSQL_IOERR_XXX error code is returned.
** Otherwise DBSQL_OK.
*/
static DBSQL_NOINLINE int __vdbe_close_statement_local(vdbe_t *p, int op){
  DBSQL *const db = p->db;
  int rc = DBSQL_OK;
  int i;
  const int i_savepoint = p->i_statement-1;

  assert( op==SAVEPOINT_ROLLBACK || op==SAVEPOINT_RELEASE);
  assert( db->n_statement>0 );
  assert( p->i_statement==(db->n_statement+db->n_savepoint) );

  for(i=0; i<db->n_db; i++){
    int rc2 = DBSQL_OK;
    sm_t *bt = db->dbs[i].bt;
    if( bt ){
      if( op==SAVEPOINT_ROLLBACK ){
        rc2 = __sm_savepoint(bt, SAVEPOINT_ROLLBACK, i_savepoint);
      }
      if( rc2==DBSQL_OK ){
        rc2 = __sm_savepoint(bt, SAVEPOINT_RELEASE, i_savepoint);
      }
      if( rc==DBSQL_OK ){
        rc = rc2;
      }
    }
  }
  db->n_statement--;
  p->i_statement = 0;

  if( rc==DBSQL_OK ){
    if( op==SAVEPOINT_ROLLBACK ){
      rc = __vtab_savepoint(db, SAVEPOINT_ROLLBACK, i_savepoint);
    }
    if( rc==DBSQL_OK ){
      rc = __vtab_savepoint(db, SAVEPOINT_RELEASE, i_savepoint);
    }
  }

  /* If the statement transaction is being rolled back, also restore the
  ** database handles deferred constraint counter to the value it had when
  ** the statement transaction was opened.  */
  if( op==SAVEPOINT_ROLLBACK ){
    db->n_deferred_cons = p->n_stmt_def_cons;
    db->n_deferred_imm_cons = p->n_stmt_def_imm_cons;
  }
  return rc;
}
int __vdbe_close_statement(vdbe_t *p, int op){
  if( p->db->n_statement && p->i_statement ){
    return __vdbe_close_statement_local(p, op);
  }
  return DBSQL_OK;
}


/*
** These functions are called when a transaction opened by the database
** handle associated with the VM passed as an argument is about to be
** committed. If there are outstanding foreign key constraint violations
** return an error code. Otherwise, DBSQL_OK.
**
** If there are outstanding FK violations and this function returns
** non-zero, set the result of the VM to DBSQL_CONSTRAINT_FOREIGNKEY
** and write an error message to it.
*/
#ifndef DBSQL_OMIT_FOREIGN_KEY
static DBSQL_NOINLINE int __vdbe_fk_error(vdbe_t *p){
  p->rc = DBSQL_CONSTRAINT_FOREIGNKEY;
  p->error_action = OE_Abort;
  __vdbe_error(p, "FOREIGN KEY constraint failed");
  if( (p->prep_flags & DBSQL_PREPARE_SAVESQL)==0 ) return DBSQL_ERROR;
  return DBSQL_CONSTRAINT_FOREIGNKEY;
}
int __vdbe_check_fk_immediate(vdbe_t *p){
  if( p->n_fk_constraint==0 ) return DBSQL_OK;
  return __vdbe_fk_error(p);
}
int __vdbe_check_fk_deferred(vdbe_t *p){
  DBSQL *db = p->db;
  if( (db->n_deferred_cons+db->n_deferred_imm_cons)==0 ) return DBSQL_OK;
  return __vdbe_fk_error(p);
}
#endif

/*
** This routine is called the when a VDBE tries to halt.  If the VDBE
** has made changes and is in autocommit mode, then commit those
** changes.  If a rollback is needed, then do the rollback.
**
** This routine is the only way to move the __dbsqleOpenState of a VM from
** DBSQL_STATE_RUN to DBSQL_STATE_HALT.  It is harmless to
** call this on a VM that is in the DBSQL_STATE_HALT state.
**
** Return an error code.  If the commit could not complete because of
** lock contention, return DBSQL_BUSY.  If DBSQL_BUSY is returned, it
** means the close did not happen and needs to be repeated.
*/
int __vdbe_halt(vdbe_t *p){
  int rc;                         /* Used to store transient return codes */
  DBSQL *db = p->db;

  /* This function contains the logic that determines if a statement or
  ** transaction will be committed or rolled back as a result of the
  ** execution of this virtual machine.
  **
  ** If any of the following errors occur:
  **
  **     DBSQL_NOMEM
  **     DBSQL_IOERR
  **     DBSQL_FULL
  **     DBSQL_INTERRUPT
  **
  ** Then the internal cache might have been left in an inconsistent
  ** state.  We need to rollback the statement transaction, if there is
  ** one, or the complete transaction if there is no statement transaction.
  */

  assert( p->vdbe_state==VDBE_RUN_STATE );
  if( db->malloc_failed ){
    p->rc = DBSQL_NOMEM_BKPT;
  }
  __close_all_cursors(p);
  CHECK_ACTIVE_VDBE_CNT(db);

  /* No commit or rollback needed if the program never started or if the
  ** SQL statement does not read or write a database file.  */
  if( p->is_reader ){
    int mrc;   /* Primary error code from p->rc */
    int statement_op = 0;
    int is_special_error;            /* Set to true if a 'special' error */

    /* Lock all btrees used by the statement */
    __vdbe_enter(p);

    /* Check for one of the special errors */
    if( p->rc ){
      mrc = p->rc & 0xff;
      is_special_error = mrc==DBSQL_NOMEM
                    || mrc==DBSQL_IOERR
                    || mrc==DBSQL_INTERRUPT
                    || mrc==DBSQL_FULL;
    }else{
      mrc = is_special_error = 0;
    }
    if( is_special_error ){
      /* If the query was read-only and the error code is DBSQL_INTERRUPT,
      ** no rollback is necessary. Otherwise, at least a savepoint
      ** transaction must be rolled back to restore the database to a
      ** consistent state.
      **
      ** Even if the statement is read-only, it is important to perform
      ** a statement or transaction rollback operation. If the error
      ** occurred while writing to the journal, sub-journal or database
      ** file as part of an effort to free up cache space (see function
      ** pager_stress() in pager.c), the rollback is required to restore
      ** the pager to a consistent state.
      */
      if( !p->readOnly || mrc!=DBSQL_INTERRUPT ){
        if( (mrc==DBSQL_NOMEM || mrc==DBSQL_FULL) && p->uses_stmt_journal ){
          statement_op = SAVEPOINT_ROLLBACK;
        }else{
          /* We are forced to roll back the active transaction. Before doing
          ** so, abort any other statements this handle currently has active.
          */
          __rollback_all(db, DBSQL_ABORT_ROLLBACK);
          __close_savepoints(db);
          db->auto_commit = 1;
          p->n_change = 0;
        }
      }
    }

    /* Check for immediate foreign key violations. */
    if( p->rc==DBSQL_OK || (p->error_action==OE_Fail && !is_special_error) ){
      (void)__vdbe_check_fk_immediate(p);
    }

    /* If the auto-commit flag is set and this is the only active writer
    ** VM, then we do either a commit or rollback of the current transaction.
    **
    ** Note: This block also runs if one of the special errors handled
    ** above has occurred.
    */
    if( !__vtab_in_sync(db)
     && db->auto_commit
     && db->n_vdbe_write==(p->readOnly==0)
    ){
      if( p->rc==DBSQL_OK || (p->error_action==OE_Fail && !is_special_error) ){
        rc = __vdbe_check_fk_deferred(p);
        if( rc!=DBSQL_OK ){
          if( NEVER(p->readOnly) ){
            __vdbe_leave(p);
            return DBSQL_ERROR;
          }
          rc = DBSQL_CONSTRAINT_FOREIGNKEY;
        }else if( db->flags & DBSQL_CorruptRdOnly ){
          rc = DBSQL_CORRUPT;
          db->flags &= ~DBSQL_CorruptRdOnly;
        }else{
          /* The auto-commit flag is true, the vdbe program was successful
          ** or hit an 'OR FAIL' constraint and there are no deferred foreign
          ** key constraints to hold up the transaction. This means a commit
          ** is required. */
          rc = __vdbe_commit(db, p);
        }
        if( rc==DBSQL_BUSY && p->readOnly ){
          __vdbe_leave(p);
          return DBSQL_BUSY;
        }else if( rc!=DBSQL_OK ){
          __system_error(db, rc);
          p->rc = rc;
          __rollback_all(db, DBSQL_OK);
          p->n_change = 0;
        }else{
          db->n_deferred_cons = 0;
          db->n_deferred_imm_cons = 0;
          db->flags &= ~(u_int64_t)DBSQL_DeferFKs;
          __commit_internal_changes(db);
        }
      }else if( p->rc==DBSQL_SCHEMA && db->n_vdbe_active>1 ){
        p->n_change = 0;
      }else{
        __rollback_all(db, DBSQL_OK);
        p->n_change = 0;
      }
      db->n_statement = 0;
    }else if( statement_op==0 ){
      if( p->rc==DBSQL_OK || p->error_action==OE_Fail ){
        statement_op = SAVEPOINT_RELEASE;
      }else if( p->error_action==OE_Abort ){
        statement_op = SAVEPOINT_ROLLBACK;
      }else{
        __rollback_all(db, DBSQL_ABORT_ROLLBACK);
        __close_savepoints(db);
        db->auto_commit = 1;
        p->n_change = 0;
      }
    }
 
    /* If statement_op is non-zero, then a statement transaction needs to
    ** be committed or rolled back. Call __vdbe_close_statement() to
    ** do so. If this operation returns an error, and the current statement
    ** error code is DBSQL_OK or DBSQL_CONSTRAINT, then promote the
    ** current statement error code.
    */
    if( statement_op ){
      rc = __vdbe_close_statement(p, statement_op);
      if( rc ){
        if( p->rc==DBSQL_OK || (p->rc&0xff)==DBSQL_CONSTRAINT ){
          p->rc = rc;
          __dbsql_db_free(db, p->errmsg);
          p->errmsg = 0;
        }
        __rollback_all(db, DBSQL_ABORT_ROLLBACK);
        __close_savepoints(db);
        db->auto_commit = 1;
        p->n_change = 0;
      }
    }
 
    /* If this was an INSERT, UPDATE or DELETE and no statement transaction
    ** has been rolled back, update the database connection change-counter.
    */
    if( p->change_cnt_on ){
      if( statement_op!=SAVEPOINT_ROLLBACK ){
        __vdbe_set_changes(db, p->n_change);
      }else{
        __vdbe_set_changes(db, 0);
      }
      p->n_change = 0;
    }

    /* Release the locks */
    __vdbe_leave(p);
  }

  /* We have successfully halted and closed the VM.  Record this fact. */
  db->n_vdbe_active--;
  if( !p->readOnly ) db->n_vdbe_write--;
  if( p->is_reader ) db->n_vdbe_read--;
  assert( db->n_vdbe_active>=db->n_vdbe_read );
  assert( db->n_vdbe_read>=db->n_vdbe_write );
  assert( db->n_vdbe_write>=0 );
  p->vdbe_state = VDBE_HALT_STATE;
  CHECK_ACTIVE_VDBE_CNT(db);
  if( db->malloc_failed ){
    p->rc = DBSQL_NOMEM_BKPT;
  }

  /* If the auto-commit flag is set to true, then any locks that were held
  ** by connection db have now been released. Call __connection_unlocked()
  ** to invoke any required unlock-notify callbacks.
  */
  if( db->auto_commit ){
    __connection_unlocked(db);
  }

  assert( db->n_vdbe_active>0 || db->auto_commit==0 || db->n_statement==0 );
  return (p->rc==DBSQL_BUSY ? DBSQL_BUSY : DBSQL_OK);
}


/*
** Each VDBE holds the result of the most recent dbsql_step() call
** in p->rc.  This routine sets that result back to DBSQL_OK.
*/
void __vdbe_reset_step_result(vdbe_t *p){
  p->rc = DBSQL_OK;
}

/*
** Copy the error code and error message belonging to the VDBE passed
** as the first argument to its database handle (so that they will be
** returned by calls to dbsql_errcode() and dbsql_errmsg()).
**
** This function does not clear the VDBE error code or message, just
** copies them to the database handle.
*/
int __vdbe_transfer_error(vdbe_t *p){
  DBSQL *db = p->db;
  int rc = p->rc;
  if( p->errmsg ){
    db->benign_malloc++;
    __begin_benign_malloc();
    if( db->err==0 ) db->err = __value_new(db);
    __value_set_str(db->err, -1, p->errmsg, DBSQL_UTF8, DBSQL_TRANSIENT);
    __end_benign_malloc();
    db->benign_malloc--;
  }else if( db->err ){
    __value_set_null(db->err);
  }
  db->errCode = rc;
  db->err_byte_offset = -1;
  return rc;
}

#ifdef DBSQL_ENABLE_SQLLOG
/*
** If an DBSQL_CONFIG_SQLLOG hook is registered and the VM has been run,
** invoke it.
*/
static void VDBE_INVOKE_SQLLOG(vdbe_t *v){
  if( __global_config.sqllog && v->rc==DBSQL_OK && v->z_sql && v->pc>=0 ){
    char *expanded = __vdbe_expand_sql(v, v->z_sql);
    assert( v->db->init.busy==0 );
    if( expanded ){
      __global_config.sqllog(
          __global_config.sqllog_arg, v->db, expanded, 1
      );
      __dbsql_db_free(v->db, expanded);
    }
  }
}
#else
# define VDBE_INVOKE_SQLLOG(x)
#endif

/*
** Clean up a VDBE after execution but do not delete the VDBE just yet.
** Write any error messages into *errmsg_p.  Return the result code.
**
** After this routine is run, the VDBE should be ready to be executed
** again.
**
** To look at it another way, this routine resets the state of the
** virtual machine from VDBE_RUN_STATE or VDBE_HALT_STATE back to
** VDBE_READY_STATE.
*/
int __vdbe_reset(vdbe_t *p){
#if defined(DBSQL_DEBUG) || defined(VDBE_PROFILE)
  int i;
#endif

  DBSQL *db;
  db = p->db;

  /* If the VM did not run to completion or if it encountered an
  ** error, then it might not have been halted properly.  So halt
  ** it now.
  */
  if( p->vdbe_state==VDBE_RUN_STATE ) __vdbe_halt(p);

  /* If the VDBE has been run even partially, then transfer the error code
  ** and error message from the VDBE into the main database structure.  But
  ** if the VDBE has just been set to run but has not actually executed any
  ** instructions yet, leave the main database error information unchanged.
  */
  if( p->pc>=0 ){
    VDBE_INVOKE_SQLLOG(p);
    if( db->err || p->errmsg ){
      __vdbe_transfer_error(p);
    }else{
      db->errCode = p->rc;
    }
  }

  /* Reset register contents and reclaim error message memory.
  */
#ifdef DBSQL_DEBUG
  /* Execute assert() statements to ensure that the vdbe_t.csrs[] and
  ** vdbe_t.mems[] arrays have already been cleaned up.  */
  if( p->csrs ) for(i=0; i<p->n_cursor; i++) assert( p->csrs[i]==0 );
  if( p->mems ){
    for(i=0; i<p->n_mem; i++) assert( p->mems[i].flags==MEM_Undefined );
  }
#endif
  if( p->errmsg ){
    __dbsql_db_free(db, p->errmsg);
    p->errmsg = 0;
  }
  p->result_row = 0;
#ifdef DBSQL_DEBUG
  p->n_write = 0;
#endif

  /* Save profiling information from this VDBE run.
  */
#ifdef VDBE_PROFILE
  {
    FILE *out = fopen("vdbe_profile.out", "a");
    if( out ){
      fprintf(out, "---- ");
      for(i=0; i<p->n_op; i++){
        fprintf(out, "%02x", p->ops[i].opcode);
      }
      fprintf(out, "\n");
      if( p->z_sql ){
        char c, pc = 0;
        fprintf(out, "-- ");
        for(i=0; (c = p->z_sql[i])!=0; i++){
          if( pc=='\n' ) fprintf(out, "-- ");
          putc(c, out);
          pc = c;
        }
        if( pc!='\n' ) fprintf(out, "\n");
      }
      for(i=0; i<p->n_op; i++){
        char hdr[100];
        int64_t cnt = p->ops[i].n_exec;
        int64_t cycles = p->ops[i].n_cycle;
        dbsql_snprintf(sizeof(hdr), hdr, "%6u %12llu %8llu ",
           cnt,
           cycles,
           cnt>0 ? cycles/cnt : 0
        );
        fprintf(out, "%s", hdr);
        __vdbe_print_op(out, i, &p->ops[i]);
      }
      fclose(out);
    }
  }
#endif
  return p->rc & db->err_mask;
}

/*
** Clean up and delete a VDBE after execution.  Return an integer which is
** the result code.  Write any error message text into *errmsg_p.
*/
int __vdbe_finalize(vdbe_t *p){
  int rc = DBSQL_OK;
  assert( VDBE_RUN_STATE>VDBE_READY_STATE );
  assert( VDBE_HALT_STATE>VDBE_READY_STATE );
  assert( VDBE_INIT_STATE<VDBE_READY_STATE );
  if( p->vdbe_state>=VDBE_READY_STATE ){
    rc = __vdbe_reset(p);
    assert( (rc & p->db->err_mask)==rc );
  }
  __vdbe_delete(p);
  return rc;
}

/*
** If parameter i_op is less than zero, then invoke the destructor for
** all auxiliary data pointers currently cached by the VM passed as
** the first argument.
**
** Or, if i_op is greater than or equal to zero, then the destructor is
** only invoked for those auxiliary data pointers created by the user
** function invoked by the OP_Function opcode at instruction i_op of
** VM vdbe, and only then if:
**
**    * the associated function parameter is the 32nd or later (counting
**      from left to right), or
**
**    * the corresponding bit in argument mask is clear (where the first
**      function parameter corresponds to bit 0 etc.).
*/
void __vdbe_delete_aux_data(DBSQL *db, aux_data_t **pp, int i_op, int mask){
  while( *pp ){
    aux_data_t *aux = *pp;
    if( (i_op<0)
     || (aux->i_aux_op==i_op
          && aux->i_aux_arg>=0
          && (aux->i_aux_arg>31 || !(mask & MASKBIT32(aux->i_aux_arg))))
    ){
      testcase( aux->i_aux_arg==31 );
      if( aux->delete_aux ){
        aux->delete_aux(aux->aux);
      }
      *pp = aux->next_aux;
      __dbsql_db_free(db, aux);
    }else{
      pp= &aux->next_aux;
    }
  }
}

/*
** Free all memory associated with the vdbe_t passed as the second argument,
** except for object itself, which is preserved.
**
** The difference between this function and __vdbe_delete() is that
** __vdbe_delete() also unlinks the vdbe_t from the list of VMs associated with
** the database connection and frees the object itself.
*/
static void __vdbe_clear_object(DBSQL *db, vdbe_t *p){
  sub_program_t *sub, *next;
  assert( db!=0 );
  assert( p->db==0 || p->db==db );
  if( p->col_names ){
    __release_mem_array(p->col_names, p->n_res_alloc*COLNAME_N);
    __db_nn_free_nn(db, p->col_names);
  }
  for(sub=p->program; sub; sub=next){
    next = sub->next;
    __vdbe_free_op_array(db, sub->ops, sub->n_op);
    __dbsql_db_free(db, sub);
  }
  if( p->vdbe_state!=VDBE_INIT_STATE ){
    __release_mem_array(p->vars, p->n_var);
    if( p->v_list ) __db_nn_free_nn(db, p->v_list);
    if( p->p_free ) __db_nn_free_nn(db, p->p_free);
  }
  __vdbe_free_op_array(db, p->ops, p->n_op);
  if( p->z_sql ) __db_nn_free_nn(db, p->z_sql);
#ifdef DBSQL_ENABLE_NORMALIZE
  __dbsql_db_free(db, p->norm_sql);
  {
    dblquote_str_t *p_this, *nxt;
    for(p_this=p->dbl_str; p_this; p_this=nxt){
      nxt = p_this->next_str;
      __dbsql_db_free(db, p_this);
    }
  }
#endif
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
  {
    int i;
    for(i=0; i<p->n_scan; i++){
      __dbsql_db_free(db, p->scans[i].name);
    }
    __dbsql_db_free(db, p->scans);
  }
#endif
}

/*
** Delete an entire VDBE.
*/
void __vdbe_delete(vdbe_t *p){
  DBSQL *db;

  assert( p!=0 );
  db = p->db;
  assert( db!=0 );
  assert( dbsql_mutex_held(db->mutex) );
  __vdbe_clear_object(db, p);
  if( db->pn_bytes_freed==0 ){
    assert( p->v_prev!=0 );
    *p->v_prev = p->v_next;
    if( p->v_next ){
      p->v_next->v_prev = p->v_prev;
    }
  }
  __db_nn_free_nn(db, p);
}

/*
** The cursor "p" has a pending seek operation that has not yet been
** carried out.  Seek the cursor now.  If an error occurs, return
** the appropriate error code.
*/
int DBSQL_NOINLINE __vdbe_finish_moveto(vdbe_cursor_t *p){
  int res, rc;
#ifdef DBSQL_TEST
  extern int dbsql_search_count;
#endif
  assert( p->deferred_moveto );
  assert( p->is_table );
  assert( p->cur_type==CURTYPE_BTREE );
  rc = __sm_table_moveto(p->uc.cursor, p->moveto_target, 0, &res);
  if( rc ) return rc;
  if( res!=0 ) return DBSQL_CORRUPT_BKPT;
#ifdef DBSQL_TEST
  dbsql_search_count++;
#endif
  p->deferred_moveto = 0;
  p->cache_status = CACHE_STALE;
  return DBSQL_OK;
}

/*
** Something has moved cursor "p" out of place.  Maybe the row it was
** pointed to was deleted out from under it.  Or maybe the btree was
** rebalanced.  Whatever the cause, try to restore "p" to the place it
** is supposed to be pointing.  If the row was deleted out from under the
** cursor, set the cursor to point to a NULL row.
*/
int DBSQL_NOINLINE __vdbe_handle_moved_cursor(vdbe_cursor_t *p){
  int is_different_row, rc;
  assert( p->cur_type==CURTYPE_BTREE );
  assert( p->uc.cursor!=0 );
  assert( __sm_cursor_has_moved(p->uc.cursor) );
  rc = __sm_cursor_restore(p->uc.cursor, &is_different_row);
  p->cache_status = CACHE_STALE;
  if( is_different_row ) p->null_row = 1;
  return rc;
}

/*
** Check to ensure that the cursor is valid.  Restore the cursor
** if need be.  Return any I/O error from the restore operation.
*/
int __vdbe_cursor_restore(vdbe_cursor_t *p){
  assert( p->cur_type==CURTYPE_BTREE || IS_NULL_CURSOR(p) );
  if( __sm_cursor_has_moved(p->uc.cursor) ){
    return __vdbe_handle_moved_cursor(p);
  }
  return DBSQL_OK;
}

/*
** The following functions:
**
** __vdbe_serial_type()
** __vdbe_serial_type_len()
** __vdbe_serial_len()
** __vdbe_serial_put()  <--- in-lined into OP_MakeRecord as of 2022-04-02
** __vdbe_serial_get()
**
** encapsulate the code that serializes values for storage in dbsql
** data and index records. Each serialized value consists of a
** 'serial-type' and a blob of data. The serial type is an 8-byte unsigned
** integer, stored as a varint.
**
** In an dbsql index record, the serial type is stored directly before
** the blob of data that it corresponds to. In a table record, all serial
** types are stored at the start of the record, and the blobs of data at
** the end. Hence these functions allow the caller to handle the
** serial-type and data blob separately.
**
** The following table describes the various storage classes for data:
**
**   serial type        bytes of data      type
**   --------------     ---------------    ---------------
**      0                     0            NULL
**      1                     1            signed integer
**      2                     2            signed integer
**      3                     3            signed integer
**      4                     4            signed integer
**      5                     6            signed integer
**      6                     8            signed integer
**      7                     8            IEEE float
**      8                     0            Integer constant 0
**      9                     0            Integer constant 1
**     10,11                               reserved for expansion
**    N>=12 and even       (N-12)/2        BLOB
**    N>=13 and odd        (N-13)/2        text
**
** The 8 and 9 types were added in 3.3.0, file format 4.  Prior versions
** of dbsql will not understand those serial types.
*/

#if 0 /* Inlined into the OP_MakeRecord opcode */
/*
** Return the serial-type for the value stored in mem.
**
** This routine might convert a large MEM_IntReal value into MEM_Real.
**
** 2019-07-11:  The primary user of this subroutine was the OP_MakeRecord
** opcode in the byte-code engine.  But by moving this routine in-line, we
** can omit some redundant tests and make that opcode a lot faster.  So
** this routine is now only used by the STAT3 logic and STAT3 support has
** ended.  The code is kept here for historical reference only.
*/
u_int32_t __vdbe_serial_type(mem_t *mem, int file_format, u_int32_t *len){
  int flags = mem->flags;
  u_int32_t n;

  assert( len!=0 );
  if( flags&MEM_Null ){
    *len = 0;
    return 0;
  }
  if( flags&(MEM_Int|MEM_IntReal) ){
    /* Figure out whether to use 1, 2, 4, 6 or 8 bytes. */
#   define MAX_6BYTE ((((int64_t)0x00008000)<<32)-1)
    int64_t i = mem->u.i;
    u_int64_t u;
    testcase( flags & MEM_Int );
    testcase( flags & MEM_IntReal );
    if( i<0 ){
      u = ~i;
    }else{
      u = i;
    }
    if( u<=127 ){
      if( (i&1)==i && file_format>=4 ){
        *len = 0;
        return 8+(u_int32_t)u;
      }else{
        *len = 1;
        return 1;
      }
    }
    if( u<=32767 ){ *len = 2; return 2; }
    if( u<=8388607 ){ *len = 3; return 3; }
    if( u<=2147483647 ){ *len = 4; return 4; }
    if( u<=MAX_6BYTE ){ *len = 6; return 5; }
    *len = 8;
    if( flags&MEM_IntReal ){
      /* If the value is __int_real and is going to take up 8 bytes to store
      ** as an integer, then we might as well make it an 8-byte floating
      ** point value */
      mem->u.r = (double)mem->u.i;
      mem->flags &= ~MEM_IntReal;
      mem->flags |= MEM_Real;
      return 7;
    }
    return 6;
  }
  if( flags&MEM_Real ){
    *len = 8;
    return 7;
  }
  assert( mem->db->malloc_failed || flags&(MEM_Str|MEM_Blob) );
  assert( mem->n>=0 );
  n = (u_int32_t)mem->n;
  if( flags & MEM_Zero ){
    n += mem->u.n_zero;
  }
  *len = n;
  return ((n*2) + 12 + ((flags&MEM_Str)!=0));
}
#endif /* inlined into OP_MakeRecord */

/*
** The sizes for serial types less than 128
*/
const u_int8_t __small_type_sizes[128] = {
        /*  0   1   2   3   4   5   6   7   8   9 */  
/*   0 */   0,  1,  2,  3,  4,  6,  8,  8,  0,  0,
/*  10 */   0,  0,  0,  0,  1,  1,  2,  2,  3,  3,
/*  20 */   4,  4,  5,  5,  6,  6,  7,  7,  8,  8,
/*  30 */   9,  9, 10, 10, 11, 11, 12, 12, 13, 13,
/*  40 */  14, 14, 15, 15, 16, 16, 17, 17, 18, 18,
/*  50 */  19, 19, 20, 20, 21, 21, 22, 22, 23, 23,
/*  60 */  24, 24, 25, 25, 26, 26, 27, 27, 28, 28,
/*  70 */  29, 29, 30, 30, 31, 31, 32, 32, 33, 33,
/*  80 */  34, 34, 35, 35, 36, 36, 37, 37, 38, 38,
/*  90 */  39, 39, 40, 40, 41, 41, 42, 42, 43, 43,
/* 100 */  44, 44, 45, 45, 46, 46, 47, 47, 48, 48,
/* 110 */  49, 49, 50, 50, 51, 51, 52, 52, 53, 53,
/* 120 */  54, 54, 55, 55, 56, 56, 57, 57
};

/*
** Return the length of the data corresponding to the supplied serial-type.
*/
u_int32_t __vdbe_serial_type_len(u_int32_t serial_type){
  if( serial_type>=128 ){
    return (serial_type-12)/2;
  }else{
    assert( serial_type<12
            || __small_type_sizes[serial_type]==(serial_type - 12)/2 );
    return __small_type_sizes[serial_type];
  }
}
u_int8_t __vdbe_one_byte_serial_type_len(u_int8_t serial_type){
  assert( serial_type<128 );
  return __small_type_sizes[serial_type]; 
}

/*
** If we are on an architecture with mixed-endian floating
** points (ex: ARM7) then swap the lower 4 bytes with the
** upper 4 bytes.  Return the result.
**
** For most architectures, this is a no-op.
**
** (later):  It is reported to me that the mixed-endian problem
** on ARM7 is an issue with GCC, not with the ARM7 chip.  It seems
** that early versions of GCC stored the two words of a 64-bit
** float in the wrong order.  And that error has been propagated
** ever since.  The blame is not necessarily with GCC, though.
** GCC might have just copying the problem from a prior compiler.
** I am also told that newer versions of GCC that follow a different
** ABI get the byte order right.
**
** Developers using dbsql on an ARM7 should compile and run their
** application using -DSQLITE_DEBUG=1 at least once.  with_t DEBUG
** enabled, some asserts below will ensure that the byte order of
** floating point values is correct.
**
** (2007-08-30)  Frank van Vugt has studied this problem closely
** and has send his findings to the dbsql developers.  Frank
** writes that some Linux kernels offer floating point hardware
** emulation that uses only 32-bit mantissas instead of a full
** 48-bits as required by the IEEE standard.  (This is the
** CONFIG_FPE_FASTFPE option.)  On such systems, floating point
** byte swapping becomes very complicated.  To avoid problems,
** the necessary byte swapping is carried out using a 64-bit integer
** rather than a 64-bit float.  Frank assures us that the code here
** works for him.  We, the developers, have no way to independently
** verify this, but Frank seems to know what he is talking about
** so we trust him.
*/
#ifdef DBSQL_MIXED_ENDIAN_64BIT_FLOAT
u_int64_t __float_swap(u_int64_t in){
  union {
    u_int64_t r;
    u_int32_t i[2];
  } u;
  u_int32_t t;

  u.r = in;
  t = u.i[0];
  u.i[0] = u.i[1];
  u.i[1] = t;
  return u.r;
}
#endif /* DBSQL_MIXED_ENDIAN_64BIT_FLOAT */


/* Input "x" is a sequence of unsigned characters that represent a
** big-endian integer.  Return the equivalent native integer
*/
#define ONE_BYTE_INT(x)    ((int8_t)(x)[0])
#define TWO_BYTE_INT(x)    (256*(int8_t)((x)[0])|(x)[1])
#define THREE_BYTE_INT(x)  (65536*(int8_t)((x)[0])|((x)[1]<<8)|(x)[2])
#define FOUR_BYTE_UINT(x)  (((u_int32_t)(x)[0]<<24)|((x)[1]<<16)|((x)[2]<<8)|(x)[3])
#define FOUR_BYTE_INT(x) (16777216*(int8_t)((x)[0])|((x)[1]<<16)|((x)[2]<<8)|(x)[3])

/*
** Deserialize the data blob pointed to by buf as serial type serial_type
** and store the result in mem.
**
** This function is implemented as two separate routines for performance.
** The few cases that require local variables are broken out into a separate
** routine so that in most cases the overhead of moving the stack pointer
** is avoided.
*/
static void __serial_get(
  const unsigned char *buf,     /* Buffer to deserialize from */
  u_int32_t serial_type,              /* Serial type to deserialize */
  mem_t *mem                     /* Memory cell to write value into */
){
  u_int64_t x = FOUR_BYTE_UINT(buf);
  u_int32_t y = FOUR_BYTE_UINT(buf+4);
  x = (x<<32) + y;
  if( serial_type==6 ){
    /* EVIDENCE-OF: R-29851-52272 Value is a big-endian 64-bit
    ** twos-complement integer. */
    mem->u.i = *(int64_t*)&x;
    mem->flags = MEM_Int;
    testcase( mem->u.i<0 );
  }else{
    /* EVIDENCE-OF: R-57343-49114 Value is a big-endian IEEE 754-2008 64-bit
    ** floating point number. */
#if !defined(NDEBUG) && !defined(DBSQL_OMIT_FLOATING_POINT)
    /* Verify that integers and floating point values use the same
    ** byte order.  Or, that if DBSQL_MIXED_ENDIAN_64BIT_FLOAT is
    ** defined that 64-bit floating point values really are mixed
    ** endian.
    */
    static const u_int64_t t1 = ((u_int64_t)0x3ff00000)<<32;
    static const double r1 = 1.0;
    u_int64_t t2 = t1;
    SWAP_MIXED_ENDIAN_FLOAT(t2);
    assert( sizeof(r1)==sizeof(t2) && memcmp(&r1, &t2, sizeof(r1))==0 );
#endif
    assert( sizeof(x)==8 && sizeof(mem->u.r)==8 );
    SWAP_MIXED_ENDIAN_FLOAT(x);
    memcpy(&mem->u.r, &x, sizeof(x));
    mem->flags = IS_NA_N(x) ? MEM_Null : MEM_Real;
  }
}
static int __serial_get7(
  const unsigned char *buf,     /* Buffer to deserialize from */
  mem_t *mem                     /* Memory cell to write value into */
){
  u_int64_t x = FOUR_BYTE_UINT(buf);
  u_int32_t y = FOUR_BYTE_UINT(buf+4);
  x = (x<<32) + y;
  assert( sizeof(x)==8 && sizeof(mem->u.r)==8 );
  SWAP_MIXED_ENDIAN_FLOAT(x);
  memcpy(&mem->u.r, &x, sizeof(x));
  if( IS_NA_N(x) ){
    mem->flags = MEM_Null;
    return 1;
  }
  mem->flags = MEM_Real;
  return 0;
}
void __vdbe_serial_get(
  const unsigned char *buf,     /* Buffer to deserialize from */
  u_int32_t serial_type,              /* Serial type to deserialize */
  mem_t *mem                     /* Memory cell to write value into */
){
  switch( serial_type ){
    case 10: { /* Internal use only: NULL with virtual table
               ** UPDATE no-change flag set */
      mem->flags = MEM_Null|MEM_Zero;
      mem->n = 0;
      mem->u.n_zero = 0;
      return;
    }
    case 11:   /* Reserved for future use */
    case 0: {  /* Null */
      /* EVIDENCE-OF: R-24078-09375 Value is a NULL. */
      mem->flags = MEM_Null;
      return;
    }
    case 1: {
      /* EVIDENCE-OF: R-44885-25196 Value is an 8-bit twos-complement
      ** integer. */
      mem->u.i = ONE_BYTE_INT(buf);
      mem->flags = MEM_Int;
      testcase( mem->u.i<0 );
      return;
    }
    case 2: { /* 2-byte signed integer */
      /* EVIDENCE-OF: R-49794-35026 Value is a big-endian 16-bit
      ** twos-complement integer. */
      mem->u.i = TWO_BYTE_INT(buf);
      mem->flags = MEM_Int;
      testcase( mem->u.i<0 );
      return;
    }
    case 3: { /* 3-byte signed integer */
      /* EVIDENCE-OF: R-37839-54301 Value is a big-endian 24-bit
      ** twos-complement integer. */
      mem->u.i = THREE_BYTE_INT(buf);
      mem->flags = MEM_Int;
      testcase( mem->u.i<0 );
      return;
    }
    case 4: { /* 4-byte signed integer */
      /* EVIDENCE-OF: R-01849-26079 Value is a big-endian 32-bit
      ** twos-complement integer. */
      mem->u.i = FOUR_BYTE_INT(buf);
#ifdef __HP_cc
      /* Work around a sign-extension bug in the HP compiler for HP/UX */
      if( buf[0]&0x80 ) mem->u.i |= 0xffffffff80000000LL;
#endif
      mem->flags = MEM_Int;
      testcase( mem->u.i<0 );
      return;
    }
    case 5: { /* 6-byte signed integer */
      /* EVIDENCE-OF: R-50385-09674 Value is a big-endian 48-bit
      ** twos-complement integer. */
      mem->u.i = FOUR_BYTE_UINT(buf+2) + (((int64_t)1)<<32)*TWO_BYTE_INT(buf);
      mem->flags = MEM_Int;
      testcase( mem->u.i<0 );
      return;
    }
    case 6:   /* 8-byte signed integer */
    case 7: { /* IEEE floating point */
      /* These use local variables, so do them in a separate routine
      ** to avoid having to move the frame pointer in the common case */
      __serial_get(buf,serial_type,mem);
      return;
    }
    case 8:    /* Integer 0 */
    case 9: {  /* Integer 1 */
      /* EVIDENCE-OF: R-12976-22893 Value is the integer 0. */
      /* EVIDENCE-OF: R-18143-12121 Value is the integer 1. */
      mem->u.i = serial_type-8;
      mem->flags = MEM_Int;
      return;
    }
    default: {
      /* EVIDENCE-OF: R-14606-31564 Value is a BLOB that is (N-12)/2 bytes in
      ** length.
      ** EVIDENCE-OF: R-28401-00140 Value is a string in the text encoding and
      ** (N-13)/2 bytes in length. */
      static const u_int16_t flags[] = { MEM_Blob|MEM_Ephem, MEM_Str|MEM_Ephem };
      mem->z = (char *)buf;
      mem->n = (serial_type-12)/2;
      mem->flags = flags[serial_type&1];
      return;
    }
  }
  return;
}
/*
** Allocate sufficient space for an unpacked_record_t structure large enough
** to hold a decoded index record for key_info.
**
** The space is allocated using __db_malloc_raw().  If an OOM error
** occurs, NULL is returned.
*/
unpacked_record_t *__vdbe_alloc_unpacked_record(
  key_info_t *key_info               /* Description of the record */
){
  unpacked_record_t *p;              /* Unpacked record to return */
  u_int64_t n_byte;                      /* Number of bytes required for *p */
  assert( sizeof(unpacked_record_t) + sizeof(mem_t)*65536 < 0x7fffffff );
  n_byte = ROUND8P(sizeof(unpacked_record_t)) + sizeof(mem_t)*(key_info->n_key_field+1);
  p = (unpacked_record_t *)__db_malloc_raw(key_info->db, n_byte);
  if( !p ) return 0;
  p->mems = (mem_t*)&((char*)p)[ROUND8P(sizeof(unpacked_record_t))];
  p->key_info = key_info;
  p->n_field = key_info->n_key_field + 1;
  return p;
}

/*
** Given the n_key-byte encoding of a record in key[], populate the
** unpacked_record_t structure indicated by the fourth argument with the
** contents of the decoded record.
*/
void __vdbe_record_unpack(
  int n_key,              /* Size of the binary record */
  const void *key,      /* The binary record */
  unpacked_record_t *p      /* Populate this structure before returning. */
){
  const unsigned char *keys = (const unsigned char *)key;
  u_int32_t d;
  u_int32_t idx;                        /* Offset in keys[] to read from */
  u_int16_t u;                          /* Unsigned loop counter */
  u_int32_t sz_hdr;
  mem_t *mem = p->mems;
  key_info_t *key_info = p->key_info;

  p->default_rc = 0;
  assert( EIGHT_BYTE_ALIGNMENT(mem) );
  idx = GET_VARINT32(keys, sz_hdr);
  d = sz_hdr;
  u = 0;
  while( idx<sz_hdr && d<=(u_int32_t)n_key ){
    u_int32_t serial_type;

    idx += GET_VARINT32(&keys[idx], serial_type);
    mem->enc = key_info->enc;
    mem->db = key_info->db;
    /* mem->flags = 0; // __vdbe_serial_get() will set this for us */
    mem->sz_malloc = 0;
    mem->z = 0;
    __vdbe_serial_get(&keys[d], serial_type, mem);
    d += __vdbe_serial_type_len(serial_type);
    if( (++u)>=p->n_field ) break;
    mem++;
  }
  if( d>(u_int32_t)n_key && u ){
    assert( CORRUPT_DB );
    /* In a corrupt record entry, the last mem might have been set up using
    ** uninitialized memory. Overwrite its value with NULL, to prevent
    ** warnings from MSAN. */
    __vdbe_mem_set_null(mem-(u<p->n_field));
  }
  testcase( u == key_info->n_key_field + 1 );
  testcase( u < key_info->n_key_field + 1 );
  assert( u<=key_info->n_key_field + 1 );
  p->n_field = u;
}

#ifdef DBSQL_DEBUG
/*
** This function compares two index or table record keys in the same way
** as the __vdbe_record_compare() routine. Unlike __vdbe_record_compare(),
** this function deserializes and compares values using the
** __vdbe_serial_get() and __mem_compare() functions. It is used
** in assert() statements to ensure that the optimized code in
** __vdbe_record_compare() returns results with these two primitives.
**
** Return true if the result of comparison is equivalent to desired_result.
** Return false if there is a disagreement.
*/
static int __vdbe_record_compare_debug(
  int n_key1, const void *key1, /* Left key */
  const unpacked_record_t *p_key2, /* Right key */
  int desired_result             /* Correct answer */
){
  u_int32_t d1;            /* Offset into keys[] of next data element */
  u_int32_t idx1;          /* Offset into keys[] of next header element */
  u_int32_t sz_hdr1;        /* Number of bytes in header */
  int i = 0;
  int rc = 0;
  const unsigned char *key1s = (const unsigned char *)key1;
  key_info_t *key_info;
  mem_t mem1;

  key_info = p_key2->key_info;
  if( key_info->db==0 ) return 1;
  mem1.enc = key_info->enc;
  mem1.db = key_info->db;
  /* mem1.flags = 0;  // Will be initialized by __vdbe_serial_get() */
  VVA_ONLY( mem1.sz_malloc = 0; ) /* Only needed by assert() statements */

  /* Compilers may complain that mem1.u.i is potentially uninitialized.
  ** We could initialize it, as shown here, to silence those complaints.
  ** But in fact, mem1.u.i will never actually be used uninitialized, and doing
  ** the unnecessary initialization has a measurable negative performance
  ** impact, since this routine is a very high runner.  And so, we choose
  ** to ignore the compiler warnings and leave this variable uninitialized.
  */
  /*  mem1.u.i = 0;  // not needed, here to silence compiler warning */
 
  idx1 = GET_VARINT32(key1s, sz_hdr1);
  if( sz_hdr1>98307 ) return DBSQL_CORRUPT;
  d1 = sz_hdr1;
  assert( key_info->n_all_field>=p_key2->n_field || CORRUPT_DB );
  assert( key_info->sort_flags!=0 );
  assert( key_info->n_key_field>0 );
  assert( idx1<=sz_hdr1 || CORRUPT_DB );
  do{
    u_int32_t serial_type1;

    /* Read the serial types for the next element in each key. */
    idx1 += GET_VARINT32( key1s+idx1, serial_type1 );

    /* Verify that there is enough key space remaining to avoid
    ** a buffer overread.  The "d1+serial_type1+2" subexpression will
    ** always be greater than or equal to the amount of required key space.
    ** Use that approximation to avoid the more expensive call to
    ** __vdbe_serial_type_len() in the common case.
    */
    if( d1+(u_int64_t)serial_type1+2>(u_int64_t)n_key1
     && d1+(u_int64_t)__vdbe_serial_type_len(serial_type1)>(u_int64_t)n_key1
    ){
      if( serial_type1>=1
       && serial_type1<=7
       && d1+(u_int64_t)__vdbe_serial_type_len(serial_type1)<=(u_int64_t)n_key1+8
       && CORRUPT_DB
      ){
        return 1;  /* corrupt record not detected by
                   ** __vdbe_record_compare_with_skip().  Return true
                   ** to avoid firing the assert() */
      }
      break;
    }

    /* Extract the values to be compared.
    */
    __vdbe_serial_get(&key1s[d1], serial_type1, &mem1);
    d1 += __vdbe_serial_type_len(serial_type1);

    /* Do the comparison
    */
    rc = __mem_compare(&mem1, &p_key2->mems[i],
                           key_info->n_all_field>i ? key_info->colls[i] : 0);
    if( rc!=0 ){
      assert( mem1.sz_malloc==0 );  /* See comment below */
      if( (key_info->sort_flags[i] & KEYINFO_ORDER_BIGNULL)
       && ((mem1.flags & MEM_Null) || (p_key2->mems[i].flags & MEM_Null))
      ){
        rc = -rc;
      }
      if( key_info->sort_flags[i] & KEYINFO_ORDER_DESC ){
        rc = -rc;  /* Invert the result for DESC sort order. */
      }
      goto debug_compare_end;
    }
    i++;
  }while( idx1<sz_hdr1 && i<p_key2->n_field );

  /* No memory allocation is ever used on mem1.  Prove this using
  ** the following assert().  If the assert() fails, it indicates a
  ** memory leak and a need to call __vdbe_mem_release(&mem1).
  */
  assert( mem1.sz_malloc==0 );

  /* rc==0 here means that one of the keys ran out of fields and
  ** all the fields up to that point were equal. Return the default_rc
  ** value.  */
  rc = p_key2->default_rc;

debug_compare_end:
  if( desired_result==0 && rc==0 ) return 1;
  if( desired_result<0 && rc<0 ) return 1;
  if( desired_result>0 && rc>0 ) return 1;
  if( CORRUPT_DB ) return 1;
  if( key_info->db->malloc_failed ) return 1;
  return 0;
}
#endif

#ifdef DBSQL_DEBUG
/*
** Count the number of fields (a.k.a. columns) in the record given by
** key,n_key.  The verify that this count is less than or equal to the
** limit given by key_info->n_all_field.
**
** If this constraint is not satisfied, it means that the high-speed
** __vdbe_record_compare_int() and __vdbe_record_compare_string() routines will
** not work correctly.  If this assert() ever fires, it probably means
** that the key_info_t.n_key_field or key_info_t.n_all_field values were computed
** incorrectly.
*/
static void VDBE_ASSERT_FIELD_COUNT_WITHIN_LIMITS(
  int n_key, const void *key,   /* The record to verify */
  const key_info_t *key_info       /* Compare size with this key_info_t */
){
  int n_field = 0;
  u_int32_t sz_hdr;
  u_int32_t idx;
  u_int32_t not_used;
  const unsigned char *keys = (const unsigned char*)key;

  if( CORRUPT_DB ) return;
  idx = GET_VARINT32(keys, sz_hdr);
  assert( n_key>=0 );
  assert( sz_hdr<=(u_int32_t)n_key );
  while( idx<sz_hdr ){
    idx += GET_VARINT32(keys+idx, not_used);
    n_field++;
  }
  assert( n_field <= key_info->n_all_field );
}
#else
# define VDBE_ASSERT_FIELD_COUNT_WITHIN_LIMITS(A,B,C)
#endif

/*
** Both *mem1 and *mem2 contain string values. Compare the two values
** using the collation sequence coll. As usual, return a negative , zero
** or positive value if *mem1 is less than, equal to or greater than
** *mem2, respectively. Similar in spirit to "rc = (*mem1) - (*mem2);".
*/
static DBSQL_NOINLINE int __vdbe_compare_mem_string_with_encoding_change(
  const mem_t *mem1,
  const mem_t *mem2,
  const coll_seq_t *coll,
  u_int8_t *prc_err                      /* If an OOM occurs, set to DBSQL_NOMEM */
){
  int rc;
  const void *v1, *v2;
  mem_t c1;
  mem_t c2;
  __vdbe_mem_init(&c1, mem1->db, MEM_Null);
  __vdbe_mem_init(&c2, mem1->db, MEM_Null);
  __vdbe_mem_shallow_copy(&c1, mem1, MEM_Ephem);
  __vdbe_mem_shallow_copy(&c2, mem2, MEM_Ephem);
  v1 = __value_text((dbsql_value_t*)&c1, coll->enc);
  v2 = __value_text((dbsql_value_t*)&c2, coll->enc);
  if( (v1==0 || v2==0) ){
    if( prc_err ) *prc_err = DBSQL_NOMEM_BKPT;
    rc = 0;
  }else{
    rc = coll->cmp(coll->user, c1.n, v1, c2.n, v2);
  }
  __vdbe_mem_release_malloc(&c1);
  __vdbe_mem_release_malloc(&c2);
  return rc;
}
static int __vdbe_compare_mem_string(
  const mem_t *mem1,
  const mem_t *mem2,
  const coll_seq_t *coll,
  u_int8_t *prc_err                      /* If an OOM occurs, set to DBSQL_NOMEM */
){
  if( mem1->enc==coll->enc ){
    /* The strings are already in the correct encoding.  Call the
     ** comparison function directly */
    return coll->cmp(coll->user,mem1->n,mem1->z,mem2->n,mem2->z);
  }else{
    return __vdbe_compare_mem_string_with_encoding_change(mem1,mem2,coll,prc_err);
  }
}

/*
** The input blob is guaranteed to be a Blob that is not marked
** with MEM_Zero.  Return true if it could be a zero-blob.
*/
static int __is_all_zero(const char *z, int n){
  int i;
  for(i=0; i<n; i++){
    if( z[i] ) return 0;
  }
  return 1;
}

/*
** Compare two blobs.  Return negative, zero, or positive if the first
** is less than, equal to, or greater than the second, respectively.
** If one blob is a prefix of the other, then the shorter is the lessor.
*/
DBSQL_NOINLINE int __blob_compare(const mem_t *b1, const mem_t *b2){
  int c;
  int n1 = b1->n;
  int n2 = b2->n;

  /* It is possible to have a Blob value that has some non-zero content
  ** followed by zero content.  But that only comes up for Blobs formed
  ** by the OP_MakeRecord opcode, and such Blobs never get passed into
  ** __mem_compare(). */
  assert( (b1->flags & MEM_Zero)==0 || n1==0 );
  assert( (b2->flags & MEM_Zero)==0 || n2==0 );

  if( (b1->flags|b2->flags) & MEM_Zero ){
    if( b1->flags & b2->flags & MEM_Zero ){
      return b1->u.n_zero - b2->u.n_zero;
    }else if( b1->flags & MEM_Zero ){
      if( !__is_all_zero(b2->z, b2->n) ) return -1;
      return b1->u.n_zero - n2;
    }else{
      if( !__is_all_zero(b1->z, b1->n) ) return +1;
      return n1 - b2->u.n_zero;
    }
  }
  c = memcmp(b1->z, b2->z, n1>n2 ? n2 : n1);
  if( c ) return c;
  return n1 - n2;
}

/* The following two functions are used only within testcase() to prove
** test coverage.  These functions do no exist for production builds.
** We must use separate DBSQL_NOINLINE functions here, since otherwise
** optimizer code movement causes gcov to become very confused.
*/
#if defined(DBSQL_COVERAGE_TEST) || defined(DBSQL_DEBUG)
static int DBSQL_NOINLINE __double_lt(double a, double b){ return a<b; }
static int DBSQL_NOINLINE __double_eq(double a, double b){ return a==b; }
#endif

/*
** Do a comparison between a 64-bit signed integer and a 64-bit floating-point
** number.  Return negative, zero, or positive if the first (i64) is less than,
** equal to, or greater than the second (double).
*/
int __int_float_compare(int64_t i, double r){
  if( __is_na_n(r) ){
    /* dbsql considers __na_n to be a NULL. And all integer values are greater
    ** than NULL */
    return 1;
  }else{
    int64_t y;
    if( r<-9223372036854775808.0 ) return +1;
    if( r>=9223372036854775808.0 ) return -1;
    y = (int64_t)r;
    if( i<y ) return -1;
    if( i>y ) return +1;
    testcase( __double_lt(((double)i),r) );
    testcase( __double_lt(r,((double)i)) );
    testcase( __double_eq(r,((double)i)) );
    return (((double)i)<r) ? -1 : (((double)i)>r);
  }
}

/*
** Compare the values contained by the two memory cells, returning
** negative, zero or positive if mem1 is less than, equal to, or greater
** than mem2. Sorting order is NULL's first, followed by numbers (integers
** and reals) sorted numerically, followed by text ordered by the collating
** sequence coll and finally blob's ordered by memcmp().
**
** Two NULL values are considered equal by this function.
*/
int __mem_compare(const mem_t *mem1, const mem_t *mem2, const coll_seq_t *coll){
  int f1, f2;
  int combined_flags;

  f1 = mem1->flags;
  f2 = mem2->flags;
  combined_flags = f1|f2;
  assert( !__vdbe_mem_is_row_set(mem1) && !__vdbe_mem_is_row_set(mem2) );

  /* If one value is NULL, it is less than the other. If both values
  ** are NULL, return 0.
  */
  if( combined_flags&MEM_Null ){
    return (f2&MEM_Null) - (f1&MEM_Null);
  }

  /* At least one of the two values is a number
  */
  if( combined_flags&(MEM_Int|MEM_Real|MEM_IntReal) ){
    testcase( combined_flags & MEM_Int );
    testcase( combined_flags & MEM_Real );
    testcase( combined_flags & MEM_IntReal );
    if( (f1 & f2 & (MEM_Int|MEM_IntReal))!=0 ){
      testcase( f1 & f2 & MEM_Int );
      testcase( f1 & f2 & MEM_IntReal );
      if( mem1->u.i < mem2->u.i ) return -1;
      if( mem1->u.i > mem2->u.i ) return +1;
      return 0;
    }
    if( (f1 & f2 & MEM_Real)!=0 ){
      if( mem1->u.r < mem2->u.r ) return -1;
      if( mem1->u.r > mem2->u.r ) return +1;
      return 0;
    }
    if( (f1&(MEM_Int|MEM_IntReal))!=0 ){
      testcase( f1 & MEM_Int );
      testcase( f1 & MEM_IntReal );
      if( (f2&MEM_Real)!=0 ){
        return __int_float_compare(mem1->u.i, mem2->u.r);
      }else if( (f2&(MEM_Int|MEM_IntReal))!=0 ){
        if( mem1->u.i < mem2->u.i ) return -1;
        if( mem1->u.i > mem2->u.i ) return +1;
        return 0;
      }else{
        return -1;
      }
    }
    if( (f1&MEM_Real)!=0 ){
      if( (f2&(MEM_Int|MEM_IntReal))!=0 ){
        testcase( f2 & MEM_Int );
        testcase( f2 & MEM_IntReal );
        return -__int_float_compare(mem2->u.i, mem1->u.r);
      }else{
        return -1;
      }
    }
    return +1;
  }

  /* If one value is a string and the other is a blob, the string is less.
  ** If both are strings, compare using the collating functions.
  */
  if( combined_flags&MEM_Str ){
    if( (f1 & MEM_Str)==0 ){
      return 1;
    }
    if( (f2 & MEM_Str)==0 ){
      return -1;
    }

    assert( mem1->enc==mem2->enc || mem1->db->malloc_failed );
    assert( mem1->enc==DBSQL_UTF8 ||
            mem1->enc==DBSQL_UTF16LE || mem1->enc==DBSQL_UTF16BE );

    /* The collation sequence must be defined at this point, even if
    ** the user deletes the collation sequence after the vdbe program is
    ** compiled (this was not always the case).
    */
    assert( !coll || coll->cmp );

    if( coll ){
      return __vdbe_compare_mem_string(mem1, mem2, coll, 0);
    }
    /* If a NULL pointer was passed as the collate function, fall through
    ** to the blob case and use memcmp().  */
  }

  /* Both values must be blobs.  Compare using memcmp().  */
  return __blob_compare(mem1, mem2);
}


/*
** The first argument passed to this function is a serial-type that
** corresponds to an integer - all values between 1 and 9 inclusive
** except 7. The second points to a buffer containing an integer value
** serialized according to serial_type. This function deserializes
** and returns the value.
*/
static int64_t __vdbe_record_decode_int(u_int32_t serial_type, const u_int8_t *keys){
  u_int32_t y;
  assert( CORRUPT_DB || (serial_type>=1 && serial_type<=9 && serial_type!=7) );
  switch( serial_type ){
    case 0:
    case 1:
      testcase( keys[0]&0x80 );
      return ONE_BYTE_INT(keys);
    case 2:
      testcase( keys[0]&0x80 );
      return TWO_BYTE_INT(keys);
    case 3:
      testcase( keys[0]&0x80 );
      return THREE_BYTE_INT(keys);
    case 4: {
      testcase( keys[0]&0x80 );
      y = FOUR_BYTE_UINT(keys);
      return (int64_t)*(int*)&y;
    }
    case 5: {
      testcase( keys[0]&0x80 );
      return FOUR_BYTE_UINT(keys+2) + (((int64_t)1)<<32)*TWO_BYTE_INT(keys);
    }
    case 6: {
      u_int64_t x = FOUR_BYTE_UINT(keys);
      testcase( keys[0]&0x80 );
      x = (x<<32) | FOUR_BYTE_UINT(keys+4);
      return (int64_t)*(int64_t*)&x;
    }
  }

  return (serial_type - 8);
}

/*
** This function compares the two table rows or index records
** specified by {n_key1, key1} and p_key2.  It returns a negative, zero
** or positive integer if key1 is less than, equal to or
** greater than key2.  The {n_key1, key1} key must be a blob
** created by the OP_MakeRecord opcode of the VDBE.  The p_key2
** key must be a parsed key such as obtained from
** __vdbe_parse_record.
**
** If argument skip is non-zero, it is assumed that the caller has already
** determined that the first fields of the keys are equal.
**
** Key1 and Key2 do not have to contain the same number of fields. If all
** fields that appear in both keys are equal, then p_key2->default_rc is
** returned.
**
** If database corruption is discovered, set p_key2->errCode to
** DBSQL_CORRUPT and return 0. If an OOM error is encountered,
** p_key2->errCode is set to DBSQL_NOMEM and, if it is not NULL, the
** malloc-failed flag set on database handle (p_key2->key_info->db).
*/
int __vdbe_record_compare_with_skip(
  int n_key1, const void *key1,   /* Left key */
  unpacked_record_t *p_key2,         /* Right key */
  int skip                       /* If true, skip the first field */
){
  u_int32_t d1;                         /* Offset into keys[] of next data element */
  int i;                          /* index_t of next field to compare */
  u_int32_t sz_hdr1;                     /* Size of record header in bytes */
  u_int32_t idx1;                       /* Offset of first type in header */
  int rc = 0;                     /* Return value */
  mem_t *p_rhs = p_key2->mems;       /* Next field of p_key2 to compare */
  key_info_t *key_info;
  const unsigned char *key1s = (const unsigned char *)key1;
  mem_t mem1;

  /* If skip is true, then the caller has already determined that the first
  ** two elements in the keys are equal. Fix the various stack variables so
  ** that this routine begins comparing at the second field. */
  if( skip ){
    u_int32_t s1 = key1s[1];
    if( s1<0x80 ){
      idx1 = 2;
    }else{
      idx1 = 1 + __get_varint32(&key1s[1], &s1);
    }
    sz_hdr1 = key1s[0];
    d1 = sz_hdr1 + __vdbe_serial_type_len(s1);
    i = 1;
    p_rhs++;
  }else{
    if( (sz_hdr1 = key1s[0])<0x80 ){
      idx1 = 1;
    }else{
      idx1 = __get_varint32(key1s, &sz_hdr1);
    }
    d1 = sz_hdr1;
    i = 0;
  }
  if( d1>(unsigned)n_key1 ){
    p_key2->errCode = (u_int8_t)DBSQL_CORRUPT_BKPT;
    return 0;  /* Corruption */
  }

  VVA_ONLY( mem1.sz_malloc = 0; ) /* Only needed by assert() statements */
  assert( p_key2->key_info->n_all_field>=p_key2->n_field
       || CORRUPT_DB );
  assert( p_key2->key_info->sort_flags!=0 );
  assert( p_key2->key_info->n_key_field>0 );
  assert( idx1<=sz_hdr1 || CORRUPT_DB );
  while( 1 /*exit-by-break*/ ){
    u_int32_t serial_type;

    /* RHS is an integer */
    if( p_rhs->flags & (MEM_Int|MEM_IntReal) ){
      testcase( p_rhs->flags & MEM_Int );
      testcase( p_rhs->flags & MEM_IntReal );
      serial_type = key1s[idx1];
      testcase( serial_type==12 );
      if( serial_type>=10 ){
        rc = serial_type==10 ? -1 : +1;
      }else if( serial_type==0 ){
        rc = -1;
      }else if( serial_type==7 ){
        __serial_get7(&key1s[d1], &mem1);
        rc = -__int_float_compare(p_rhs->u.i, mem1.u.r);
      }else{
        int64_t lhs = __vdbe_record_decode_int(serial_type, &key1s[d1]);
        int64_t rhs = p_rhs->u.i;
        if( lhs<rhs ){
          rc = -1;
        }else if( lhs>rhs ){
          rc = +1;
        }
      }
    }

    /* RHS is real */
    else if( p_rhs->flags & MEM_Real ){
      serial_type = key1s[idx1];
      if( serial_type>=10 ){
        /* Serial types 12 or greater are strings and blobs (greater than
        ** numbers). Types 10 and 11 are currently "reserved for future
        ** use", so it doesn't really matter what the results of comparing
        ** them to numeric values are.  */
        rc = serial_type==10 ? -1 : +1;
      }else if( serial_type==0 ){
        rc = -1;
      }else{
        if( serial_type==7 ){
          if( __serial_get7(&key1s[d1], &mem1) ){
            rc = -1;  /* mem1 is a __na_n */
          }else if( mem1.u.r<p_rhs->u.r ){
            rc = -1;
          }else if( mem1.u.r>p_rhs->u.r ){
            rc = +1;
          }else{
            assert( rc==0 );
          }
        }else{
          __vdbe_serial_get(&key1s[d1], serial_type, &mem1);
          rc = __int_float_compare(mem1.u.i, p_rhs->u.r);
        }
      }
    }

    /* RHS is a string */
    else if( p_rhs->flags & MEM_Str ){
      GET_VARINT32_NR(&key1s[idx1], serial_type);
      testcase( serial_type==12 );
      if( serial_type<12 ){
        rc = -1;
      }else if( !(serial_type & 0x01) ){
        rc = +1;
      }else{
        mem1.n = (serial_type - 12) / 2;
        testcase( (d1+mem1.n)==(unsigned)n_key1 );
        testcase( (d1+mem1.n+1)==(unsigned)n_key1 );
        if( (d1+mem1.n) > (unsigned)n_key1
         || (key_info = p_key2->key_info)->n_all_field<=i
        ){
          p_key2->errCode = (u_int8_t)DBSQL_CORRUPT_BKPT;
          return 0;                /* Corruption */
        }else if( key_info->colls[i] ){
          mem1.enc = key_info->enc;
          mem1.db = key_info->db;
          mem1.flags = MEM_Str;
          mem1.z = (char*)&key1s[d1];
          rc = __vdbe_compare_mem_string(
              &mem1, p_rhs, key_info->colls[i], &p_key2->errCode
          );
        }else{
          int n_cmp = MIN(mem1.n, p_rhs->n);
          rc = memcmp(&key1s[d1], p_rhs->z, n_cmp);
          if( rc==0 ) rc = mem1.n - p_rhs->n;
        }
      }
    }

    /* RHS is a blob */
    else if( p_rhs->flags & MEM_Blob ){
      assert( (p_rhs->flags & MEM_Zero)==0 || p_rhs->n==0 );
      GET_VARINT32_NR(&key1s[idx1], serial_type);
      testcase( serial_type==12 );
      if( serial_type<12 || (serial_type & 0x01) ){
        rc = -1;
      }else{
        int n_str = (serial_type - 12) / 2;
        testcase( (d1+n_str)==(unsigned)n_key1 );
        testcase( (d1+n_str+1)==(unsigned)n_key1 );
        if( (d1+n_str) > (unsigned)n_key1 ){
          p_key2->errCode = (u_int8_t)DBSQL_CORRUPT_BKPT;
          return 0;                /* Corruption */
        }else if( p_rhs->flags & MEM_Zero ){
          if( !__is_all_zero((const char*)&key1s[d1],n_str) ){
            rc = 1;
          }else{
            rc = n_str - p_rhs->u.n_zero;
          }
        }else{
          int n_cmp = MIN(n_str, p_rhs->n);
          rc = memcmp(&key1s[d1], p_rhs->z, n_cmp);
          if( rc==0 ) rc = n_str - p_rhs->n;
        }
      }
    }

    /* RHS is null */
    else{
      serial_type = key1s[idx1];
      if( serial_type==0
       || serial_type==10
       || (serial_type==7 && __serial_get7(&key1s[d1], &mem1)!=0)
      ){
        assert( rc==0 );
      }else{
        rc = 1;
      }
    }

    if( rc!=0 ){
      int sort_flags = p_key2->key_info->sort_flags[i];
      if( sort_flags ){
        if( (sort_flags & KEYINFO_ORDER_BIGNULL)==0
         || ((sort_flags & KEYINFO_ORDER_DESC)
           !=(serial_type==0 || (p_rhs->flags&MEM_Null)))
        ){
          rc = -rc;
        }
      }
      assert( __vdbe_record_compare_debug(n_key1, key1, p_key2, rc) );
      assert( mem1.sz_malloc==0 );  /* See comment below */
      return rc;
    }

    i++;
    if( i==p_key2->n_field ) break;
    p_rhs++;
    d1 += __vdbe_serial_type_len(serial_type);
    if( d1>(unsigned)n_key1 ) break;
    idx1 += __varint_len(serial_type);
    if( idx1>=(unsigned)sz_hdr1 ){
      p_key2->errCode = (u_int8_t)DBSQL_CORRUPT_BKPT;
      return 0;  /* Corrupt index */
    }
  }

  /* No memory allocation is ever used on mem1.  Prove this using
  ** the following assert().  If the assert() fails, it indicates a
  ** memory leak and a need to call __vdbe_mem_release(&mem1).  */
  assert( mem1.sz_malloc==0 );

  /* rc==0 here means that one or both of the keys ran out of fields and
  ** all the fields up to that point were equal. Return the default_rc
  ** value.  */
  assert( CORRUPT_DB
       || __vdbe_record_compare_debug(n_key1, key1, p_key2, p_key2->default_rc)
       || p_key2->key_info->db->malloc_failed
  );
  p_key2->eq_seen = 1;
  return p_key2->default_rc;
}
int __vdbe_record_compare(
  int n_key1, const void *key1,   /* Left key */
  unpacked_record_t *p_key2          /* Right key */
){
  return __vdbe_record_compare_with_skip(n_key1, key1, p_key2, 0);
}


/*
** This function is an optimized version of __vdbe_record_compare()
** that (a) the first field of p_key2 is an integer, and (b) the
** size-of-header varint at the start of (key1/n_key1) fits in a single
** byte (i.e. is less than 128).
**
** To avoid concerns about buffer overreads, this routine is only used
** on schemas where the maximum valid header size is 63 bytes or less.
*/
static int __vdbe_record_compare_int(
  int n_key1, const void *key1, /* Left key */
  unpacked_record_t *p_key2        /* Right key */
){
  const u_int8_t *keys = &((const u_int8_t*)key1)[*(const u_int8_t*)key1 & 0x3F];
  int serial_type = ((const u_int8_t*)key1)[1];
  int res;
  u_int32_t y;
  u_int64_t x;
  int64_t v;
  int64_t lhs;

  VDBE_ASSERT_FIELD_COUNT_WITHIN_LIMITS(n_key1, key1, p_key2->key_info);
  assert( (*(u_int8_t*)key1)<=0x3F || CORRUPT_DB );
  switch( serial_type ){
    case 1: { /* 1-byte signed integer */
      lhs = ONE_BYTE_INT(keys);
      testcase( lhs<0 );
      break;
    }
    case 2: { /* 2-byte signed integer */
      lhs = TWO_BYTE_INT(keys);
      testcase( lhs<0 );
      break;
    }
    case 3: { /* 3-byte signed integer */
      lhs = THREE_BYTE_INT(keys);
      testcase( lhs<0 );
      break;
    }
    case 4: { /* 4-byte signed integer */
      y = FOUR_BYTE_UINT(keys);
      lhs = (int64_t)*(int*)&y;
      testcase( lhs<0 );
      break;
    }
    case 5: { /* 6-byte signed integer */
      lhs = FOUR_BYTE_UINT(keys+2) + (((int64_t)1)<<32)*TWO_BYTE_INT(keys);
      testcase( lhs<0 );
      break;
    }
    case 6: { /* 8-byte signed integer */
      x = FOUR_BYTE_UINT(keys);
      x = (x<<32) | FOUR_BYTE_UINT(keys+4);
      lhs = *(int64_t*)&x;
      testcase( lhs<0 );
      break;
    }
    case 8:
      lhs = 0;
      break;
    case 9:
      lhs = 1;
      break;

    /* This case could be removed without changing the results of running
    ** this code. Including it causes gcc to generate a faster switch
    ** statement (since the range of switch targets now starts at zero and
    ** is contiguous) but does not cause any duplicate code to be generated
    ** (as gcc is clever enough to combine the two like cases). Other
    ** compilers might be similar.  */
    case 0: case 7:
      return __vdbe_record_compare(n_key1, key1, p_key2);

    default:
      return __vdbe_record_compare(n_key1, key1, p_key2);
  }

  assert( p_key2->u.i == p_key2->mems[0].u.i );
  v = p_key2->u.i;
  if( v>lhs ){
    res = p_key2->r1;
  }else if( v<lhs ){
    res = p_key2->r2;
  }else if( p_key2->n_field>1 ){
    /* The first fields of the two keys are equal. Compare the trailing
    ** fields.  */
    res = __vdbe_record_compare_with_skip(n_key1, key1, p_key2, 1);
  }else{
    /* The first fields of the two keys are equal and there are no trailing
    ** fields. Return p_key2->default_rc in this case. */
    res = p_key2->default_rc;
    p_key2->eq_seen = 1;
  }

  assert( __vdbe_record_compare_debug(n_key1, key1, p_key2, res) );
  return res;
}

/*
** This function is an optimized version of __vdbe_record_compare()
** that (a) the first field of p_key2 is a string, that (b) the first field
** uses the collation sequence BINARY and (c) that the size-of-header varint
** at the start of (key1/n_key1) fits in a single byte.
*/
static int __vdbe_record_compare_string(
  int n_key1, const void *key1, /* Left key */
  unpacked_record_t *p_key2        /* Right key */
){
  const u_int8_t *key1s = (const u_int8_t*)key1;
  int serial_type;
  int res;

  assert( p_key2->mems[0].flags & MEM_Str );
  assert( p_key2->mems[0].n == p_key2->n );
  assert( p_key2->mems[0].z == p_key2->u.z );
  VDBE_ASSERT_FIELD_COUNT_WITHIN_LIMITS(n_key1, key1, p_key2->key_info);
  serial_type = (signed char)(key1s[1]);

vrcs_restart:
  if( serial_type<12 ){
    if( serial_type<0 ){
      __get_varint32(&key1s[1], (u_int32_t*)&serial_type);
      if( serial_type>=12 ) goto vrcs_restart;
      assert( CORRUPT_DB );
    }
    res = p_key2->r1;      /* (key1/n_key1) is a number or a null */
  }else if( !(serial_type & 0x01) ){
    res = p_key2->r2;      /* (key1/n_key1) is a blob */
  }else{
    int n_cmp;
    int n_str;
    int sz_hdr = key1s[0];

    n_str = (serial_type-12) / 2;
    if( (sz_hdr + n_str) > n_key1 ){
      p_key2->errCode = (u_int8_t)DBSQL_CORRUPT_BKPT;
      return 0;    /* Corruption */
    }
    n_cmp = MIN( p_key2->n, n_str );
    res = memcmp(&key1s[sz_hdr], p_key2->u.z, n_cmp);

    if( res>0 ){
      res = p_key2->r2;
    }else if( res<0 ){
      res = p_key2->r1;
    }else{
      res = n_str - p_key2->n;
      if( res==0 ){
        if( p_key2->n_field>1 ){
          res = __vdbe_record_compare_with_skip(n_key1, key1, p_key2, 1);
        }else{
          res = p_key2->default_rc;
          p_key2->eq_seen = 1;
        }
      }else if( res>0 ){
        res = p_key2->r2;
      }else{
        res = p_key2->r1;
      }
    }
  }

  assert( __vdbe_record_compare_debug(n_key1, key1, p_key2, res)
       || CORRUPT_DB
       || p_key2->key_info->db->malloc_failed
  );
  return res;
}

/*
** Return a pointer to an __vdbe_record_compare() compatible function
** suitable for comparing serialized records to the unpacked record passed
** as the only argument.
*/
__record_compare __vdbe_find_compare(unpacked_record_t *p){
  /* varint_record_compare_int() and varint_record_compare_string() both assume
  ** that the size-of-header varint that occurs at the start of each record
  ** fits in a single byte (i.e. is 127 or less). varint_record_compare_int()
  ** also assumes that it is safe to overread a buffer by at least the
  ** maximum possible legal header size plus 8 bytes. Because there is
  ** guaranteed to be at least 74 (but not 136) bytes of padding following each
  ** buffer passed to varint_record_compare_int() this makes it convenient to
  ** limit the size of the header to 64 bytes in cases where the first field
  ** is an integer.
  **
  ** The easiest way to enforce this limit is to consider only records with
  ** 13 fields or less. If the first field is an integer, the maximum legal
  ** header size is (12*5 + 1 + 1) bytes.  */
  assert( p->key_info->sort_flags!=0 );
  if( p->key_info->n_all_field<=13 ){
    int flags = p->mems[0].flags;
    if( p->key_info->sort_flags[0] ){
      if( p->key_info->sort_flags[0] & KEYINFO_ORDER_BIGNULL ){
        return __vdbe_record_compare;
      }
      p->r1 = 1;
      p->r2 = -1;
    }else{
      p->r1 = -1;
      p->r2 = 1;
    }
    if( (flags & MEM_Int) ){
      p->u.i = p->mems[0].u.i;
      return __vdbe_record_compare_int;
    }
    testcase( flags & MEM_Real );
    testcase( flags & MEM_Null );
    testcase( flags & MEM_Blob );
    if( (flags & (MEM_Real|MEM_IntReal|MEM_Null|MEM_Blob))==0
     && p->key_info->colls[0]==0
    ){
      assert( flags & MEM_Str );
      p->u.z = p->mems[0].z;
      p->n = p->mems[0].n;
      return __vdbe_record_compare_string;
    }
  }

  return __vdbe_record_compare;
}

/*
** cur points at an index entry created using the OP_MakeRecord opcode.
** Read the rowid (the last field in the record) and store it in *rowid.
** Return DBSQL_OK if everything works, or an error code otherwise.
**
** cur might be pointing to text obtained from a corrupt database file.
** So the content cannot be trusted.  Do appropriate checks on the content.
*/
int __vdbe_idx_rowid(DBSQL *db, sm_cursor_t *cur, int64_t *rowid){
  int64_t n_cell_key = 0;
  int rc;
  u_int32_t sz_hdr;        /* Size of the header */
  u_int32_t type_rowid;    /* Serial type of the rowid */
  u_int32_t len_rowid;     /* Size of the rowid */
  mem_t m, v;

  /* Get the size of the index entry.  Only indices entries of less
  ** than 2GiB are support - anything large must be database corruption.
  ** Any corruption is detected in __sm_parse_cell_ptr(), though, so
  ** this code can safely assume that n_cell_key is 32-bits 
  */
  assert( __sm_cursor_is_valid(cur) );
  n_cell_key = __sm_payload_size(cur);
  assert( (n_cell_key & DBSQL_MAX_U32)==(u_int64_t)n_cell_key );

  /* Read in the complete content of the index entry */
  __vdbe_mem_init(&m, db, 0);
  rc = __vdbe_mem_from_btree_zero_offset(cur, (u_int32_t)n_cell_key, &m);
  if( rc ){
    return rc;
  }

  /* The index entry must begin with a header size */
  GET_VARINT32_NR((u_int8_t*)m.z, sz_hdr);
  testcase( sz_hdr==3 );
  testcase( sz_hdr==(u_int32_t)m.n );
  testcase( sz_hdr>0x7fffffff );
  assert( m.n>=0 );
  if( unlikely(sz_hdr<3 || sz_hdr>(unsigned)m.n) ){
    goto idx_rowid_corruption;
  }

  /* The last field of the index should be an integer - the ROWID.
  ** Verify that the last entry really is an integer. */
  GET_VARINT32_NR((u_int8_t*)&m.z[sz_hdr-1], type_rowid);
  testcase( type_rowid==1 );
  testcase( type_rowid==2 );
  testcase( type_rowid==3 );
  testcase( type_rowid==4 );
  testcase( type_rowid==5 );
  testcase( type_rowid==6 );
  testcase( type_rowid==8 );
  testcase( type_rowid==9 );
  if( unlikely(type_rowid<1 || type_rowid>9 || type_rowid==7) ){
    goto idx_rowid_corruption;
  }
  len_rowid = __small_type_sizes[type_rowid];
  testcase( (u_int32_t)m.n==sz_hdr+len_rowid );
  if( unlikely((u_int32_t)m.n<sz_hdr+len_rowid) ){
    goto idx_rowid_corruption;
  }

  /* Fetch the integer off the end of the index record */
  __vdbe_serial_get((u_int8_t*)&m.z[m.n-len_rowid], type_rowid, &v);
  *rowid = v.u.i;
  __vdbe_mem_release_malloc(&m);
  return DBSQL_OK;

  /* Jump here if database corruption is detected after m has been
  ** allocated.  Free the m object and return DBSQL_CORRUPT. */
idx_rowid_corruption:
  testcase( m.sz_malloc!=0 );
  __vdbe_mem_release_malloc(&m);
  return DBSQL_CORRUPT_BKPT;
}

/*
** Compare the key of the index entry that cursor c is pointing to against
** the key string in unpacked.  Write into *res a number
** that is negative, zero, or positive if c is less than, equal to,
** or greater than unpacked.  Return DBSQL_OK on success.
**
** unpacked is either created without a rowid or is truncated so that it
** omits the rowid at the end.  The rowid at the end of the index entry
** is ignored as well.  Hence, this routine only compares the prefixes
** of the keys prior to the final rowid, not the entire key.
*/
int __vdbe_idx_key_compare(
  DBSQL *db,                     /* Database connection */
  vdbe_cursor_t *c,                  /* The cursor to compare against */
  unpacked_record_t *unpacked,       /* Unpacked version of key */
  int *res                         /* Write the comparison result here */
){
  int64_t n_cell_key = 0;
  int rc;
  sm_cursor_t *cur;
  mem_t m;

  assert( c->cur_type==CURTYPE_BTREE );
  cur = c->uc.cursor;
  assert( __sm_cursor_is_valid(cur) );
  n_cell_key = __sm_payload_size(cur);
  /* n_cell_key will always be between 0 and 0xffffffff because of the way
  ** that btree_parse_cell_ptr() and __get_varint32() are implemented */
  if( n_cell_key<=0 || n_cell_key>0x7fffffff ){
    *res = 0;
    return DBSQL_CORRUPT_BKPT;
  }
  __vdbe_mem_init(&m, db, 0);
  rc = __vdbe_mem_from_btree_zero_offset(cur, (u_int32_t)n_cell_key, &m);
  if( rc ){
    return rc;
  }
  *res = __vdbe_record_compare_with_skip(m.n, m.z, unpacked, 0);
  __vdbe_mem_release_malloc(&m);
  return DBSQL_OK;
}

/*
** This routine sets the value to be returned by subsequent calls to
** dbsql_changes() on the database handle 'db'.
*/
void __vdbe_set_changes(DBSQL *db, int64_t n_change){
  assert( dbsql_mutex_held(db->mutex) );
  db->n_change = n_change;
  db->n_total_change += n_change;
}

/*
** Set a flag in the vdbe to update the change counter when it is finalised
** or reset.
*/
void __vdbe_count_changes(vdbe_t *v){
  v->change_cnt_on = 1;
}

/*
** Mark every prepared statement associated with a database connection
** as expired.
**
** An expired statement means that recompilation of the statement is
** recommend.  Statements expire when things happen that make their
** programs obsolete.  Removing user-defined functions or collating
** sequences, or changing an authorization function are the types of
** things that make prepared statements obsolete.
**
** If i_code is 1, then expiration is advisory.  The statement should
** be reprepared before being restarted, but if it is already running
** it is allowed to run to completion.
**
** Internally, this function just sets the vdbe_t.expired flag on all
** prepared statements.  The flag is set to 1 for an immediate expiration
** and set to 2 for an advisory expiration.
*/
void __expire_prepared_statements(DBSQL *db, int i_code){
  vdbe_t *p;
  for(p = db->vdbe; p; p=p->v_next){
    p->expired = i_code+1;
  }
}

/*
** Return the database associated with the vdbe_t.
*/
DBSQL *__vdbe_db(vdbe_t *v){
  return v->db;
}

/*
** Return the DBSQL_PREPARE flags for a vdbe_t.
*/
u_int8_t __vdbe_prepare_flags(vdbe_t *v){
  return v->prep_flags;
}

/*
** Return a pointer to an dbsql_value_t structure containing the value bound
** parameter i_var of VM v. Except, if the value is an SQL NULL, return
** 0 instead. Unless it is NULL, apply affinity aff (one of the DBSQL_AFF_*
** constants) to the value before returning it.
**
** The returned value must be freed by the caller using __value_free().
*/
dbsql_value_t *__vdbe_get_bound_value(vdbe_t *v, int i_var, u_int8_t aff){
  assert( i_var>0 );
  if( v ){
    mem_t *mem = &v->vars[i_var-1];
    assert( (v->db->flags & DBSQL_EnableQPSG)==0 
         || (v->db->m_db_flags & DBFLAG_InternalFunc)!=0 );
    if( 0==(mem->flags & MEM_Null) ){
      dbsql_value_t *ret = __value_new(v->db);
      if( ret ){
        __vdbe_mem_copy((mem_t *)ret, mem);
        __value_apply_affinity(ret, aff, DBSQL_UTF8);
      }
      return ret;
    }
  }
  return 0;
}

/*
** Configure SQL variable i_var so that binding a new value to it signals
** to dbsql_reoptimize() that re-preparing the statement may result
** in a better query plan.
*/
void __vdbe_set_varmask(vdbe_t *v, int i_var){
  assert( i_var>0 );
  assert( (v->db->flags & DBSQL_EnableQPSG)==0 
       || (v->db->m_db_flags & DBFLAG_InternalFunc)!=0 );
  if( i_var>=32 ){
    v->expmask |= 0x80000000;
  }else{
    v->expmask |= ((u_int32_t)1 << (i_var-1));
  }
}

/*
** Helper function for __vdbe_is_matching_index_key(). Return true if column
** i_col should be ignored when comparing a record with a record from 
** an index on disk. The field should be ignored if:
**
**   * the corresponding bit in mask is set, and
**   * either:
**       - integrity is false, or
**       - the two mem_t values are both real values that differ by 
**         BTREE_ULPDISTORTION or fewer ULPs.
*/
static int __vdbe_skip_field(
  Bitmask mask,                   /* Mask of indexed expression fields */
  int i_col,                       /* column_t of index being considered */
  mem_t *mem1,                     /* Expected index value */
  mem_t *mem2,                     /* Actual indexed value */
  int integrity                  /* True if running PRAGMA integrity_check */
){
#define BTREE_ULPDISTORTION 2
  if( i_col>=BMS || (mask & MASKBIT(i_col))==0 ) return 0;
  if( integrity==0 ) return 1;
  if( (mem1->flags & MEM_Real) && (mem2->flags & MEM_Real) ){
    u_int64_t m1, m2;
    memcpy(&m1,&mem1->u.r,8);
    memcpy(&m2,&mem2->u.r,8);
    if( (m1<m2 ? m2-m1 : m1-m2) <= BTREE_ULPDISTORTION ){
      return 1;
    }
  }
  return 0;
}

/*
** This function compares the unpacked record with the current key that
** cursor cur points to. If b_int is false, all fields for which the 
** corresponding bit in parameter "mask" is set are ignored. Or, if
** b_int is true, then a difference of BTREE_ULPDISTORTION or fewer ULPs
** in real values is overlooked for fields with the corresponding bit
** set in mask.  
**
** Return the usual less than zero, zero, or greater than zero if the 
** remaining fields of the cursor cursor key are less than, equal to or 
** greater than those in (*p).
*/
static int __vdbe_is_matching_index_key(
  sm_cursor_t *cur,            /* Cursor open on index */
  int b_int,                  /* True for integrity_check-style search */
  Bitmask mask,              /* Mask of columns to skip */
  unpacked_record_t *p,         /* index_t key being deleted */
  int *pi_res                 /* 0 for a match, non-zero for not a match */
){
  u_int8_t *recs = 0;
  u_int32_t n_rec = 0;
  mem_t mem;
  int rc = DBSQL_OK;

  memset(&mem, 0, sizeof(mem));
  mem.enc = p->key_info->enc;
  mem.db = p->key_info->db;
  n_rec = __sm_payload_size(cur);
  if( n_rec>0x7fffffff ){
    return DBSQL_CORRUPT_BKPT;
  }

  /* Allocate 5 extra bytes at the end of the buffer. This allows the
  ** GET_VARINT32() call below to read slightly past the end of the buffer 
  ** if the record is corrupt. */
  recs = __malloc_zero(n_rec+5);
  if( recs==0 ){
    rc = DBSQL_NOMEM_BKPT;
  }else{
    rc = __sm_payload(cur, 0, n_rec, recs);
  }

  if( rc==DBSQL_OK ){
    u_int32_t sz_hdr = 0;                /* Size of record header in bytes */
    u_int32_t idx_hdr = 0;               /* Current index in header */

    idx_hdr = GET_VARINT32(recs, sz_hdr);
    if( sz_hdr>98307 ){
      rc = DBSQL_CORRUPT;
    }else{
      int res = 0;                /* Result of this function call */
      u_int32_t idx_rec = sz_hdr;         /* index_t of next field in record body */
      int ii = 0;                 /* Iterator variable */

      int n_col = p->key_info->n_all_field;
      for(ii=0; ii<n_col && rc==DBSQL_OK; ii++){
        u_int32_t i_serial = 0;
        int n_serial = 0;

        if( idx_hdr>=sz_hdr ){
          rc = DBSQL_CORRUPT_BKPT;
          break;
        }
        idx_hdr += GET_VARINT32(&recs[idx_hdr], i_serial);
        n_serial = __vdbe_serial_type_len(i_serial);
        if( (idx_rec+n_serial)>n_rec ){
          rc = DBSQL_CORRUPT_BKPT;
        }else{
          __vdbe_serial_get(&recs[idx_rec], i_serial, &mem);
          if( __vdbe_skip_field(mask, ii, &p->mems[ii], &mem, b_int)==0 ){
            res = __mem_compare(&mem, &p->mems[ii], p->key_info->colls[ii]);
            if( res!=0 ) break;
          }
        }
        idx_rec += __vdbe_serial_type_len(i_serial);
      }

      *pi_res = res;
    }
  }

  dbsql_free(recs);
  return rc;
}

/*
** This is called when the record in (*p) should be found in the index 
** opened by cursor cur, but was not. This may happen as part of a DELETE
** operation or an integrity check.
**
** One reason that an exact match was not found may be the EIIB bug - that
** a text-to-float conversion may have caused a real value in record (*p)
** to be slightly different from its counterpart on disk. This function
** attempts to find the right index record. If it does find the right
** record, it leaves *cur pointing to it and sets (*res) to 0 before
** returning. Otherwise, (*res) is set to non-zero and an dbsql error
** code returned.
**
** The algorithm used to find the correct record is:
**
**   * Scan up to BTREE_FDK_RANGE entries either side of the current entry.
**     If parameter integrity is false, then all fields that are indexed
**     expressions or virtual table columns are omitted from the comparison.
**     If integrity is true, then small differences in real values in
**     such fields are overlooked, but they are not omitted from the comparison
**     altogether.
**
**   * If the above fails to find an entry and integrity is false, search 
**     the entire index.
*/
int __vdbe_find_index_key(
  sm_cursor_t *cur, 
  index_t *idx,
  unpacked_record_t *p, 
  int *p_res,
  int integrity
){
#define BTREE_FDK_RANGE 10
  int n_step = 0;
  int res = 1;
  int rc = DBSQL_OK;
  int ii = 0;

  /* Calculate a mask based on the first 64 columns of the index. The mask
  ** bit is set if the corresponding index field is either an expression
  ** or a virtual column of the table.  */
  Bitmask mask = 0;
  for(ii=0; ii<MIN(idx->n_column, BMS); ii++){
    int i_col = idx->columns[ii];
    if( (i_col==XN_EXPR)
     || (i_col>=0 && (idx->table->cols[i_col].col_flags & COLFLAG_VIRTUAL))
    ){
      mask |= MASKBIT(ii);
    }
  }

  /* If the mask is 0 at this point, then the index contains no expressions
  ** or virtual columns. So do not search for a match - return so that the
  ** caller may declare the db corrupt immediately. Or, if mask is non-zero,
  ** proceed.  */
  if( mask!=0 ){

    /* Move the cursor back BTREE_FDK_RANGE entries. If this hits an EOF, 
    ** position the cursor at the first entry in the index and set n_step
    ** to -1 so that the first loop below scans the entire index. Otherwise,
    ** set n_step to BTREE_FDK_RANGE*2 so that the first loop below scans
    ** just that many entries.  */
    for(ii=0; __sm_eof(cur)==0 && ii<BTREE_FDK_RANGE; ii++){
      rc = __sm_previous(cur, 0);
    }
    if( rc==DBSQL_DONE ){
      rc = __sm_first(cur, &res);
      n_step = -1;
    }else{
      n_step = BTREE_FDK_RANGE*2;
    }
  
    /* This loop runs at most twice to search for a key with matching PK 
    ** fields in the index. The second iteration always searches the entire 
    ** index. The first iteration searches n_step entries starting with the
    ** current cursor entry if (n_step>=0), or the entire index if (n_step<0).  */
    while( __sm_cursor_is_valid_nn(cur) ){
      for(ii=0; rc==DBSQL_OK && (ii<n_step || n_step<0); ii++){
        rc = __vdbe_is_matching_index_key(cur, integrity, mask, p, &res);
        if( res==0 || rc!=DBSQL_OK ) break;
        rc = __sm_next(cur, 0);
      }
      if( rc==DBSQL_DONE ){
        rc = DBSQL_OK;
        assert( res!=0 );
      }
      if( n_step<0 || rc!=DBSQL_OK || res==0 || integrity ) break;
  
      /* The first, non-exhaustive, search failed to find an entry with 
      ** matching PK fields. So restart for an exhaustive search of the 
      ** entire index.  */
      n_step = -1;
      rc = __sm_first(cur, &res);
    }
  }

  *p_res = res;
  return rc;
}

#ifndef DBSQL_OMIT_DATETIME_FUNCS
/*
** Cause a function to throw an error if it was call from OP_PureFunc
** rather than OP_Function.
**
** OP_PureFunc means that the function must be deterministic, and should
** throw an error if it is given inputs that would make it non-deterministic.
** This routine is invoked by date/time functions that use non-deterministic
** features such as 'now'.
*/
int __not_pure_func(dbsql_func_t *ctx){
  const vdbe_op_t *op;
#ifdef DBSQL_ENABLE_STAT4
  if( ctx->vdbe==0 ) return 1;
#endif
  op = ctx->vdbe->ops + ctx->i_op;
  if( op->opcode==OP_PureFunc ){
    const char *context;
    char *msg;
    if( op->p5 & NC_IsCheck ){
      context = "a CHECK constraint";
    }else if( op->p5 & NC_GenCol ){
      context = "a generated column";
    }else{
      context = "an index";
    }
    msg = dbsql_mprintf("non-deterministic use of %s() in %s",
                           ctx->func->name, context);
    dbsql_result_error(ctx, msg, -1);
    dbsql_free(msg);
    return 0;
  }
  return 1;
}
#endif /* DBSQL_OMIT_DATETIME_FUNCS */

#if defined(DBSQL_ENABLE_CURSOR_HINTS) && defined(DBSQL_DEBUG)
/*
** This walker_t callback is used to help verify that calls to
** __sm_cursor_hint() with opcode BTREE_HINT_RANGE have
** byte-code register values correctly initialized.
*/
int __cursor_range_hint_expr_check(walker_t *walker, expr_t *expr){
  if( expr->op==TK_REGISTER ){
    assert( (walker->u.mems[expr->i_table].flags & MEM_Undefined)==0 );
  }
  return WRC_Continue;
}
#endif /* DBSQL_ENABLE_CURSOR_HINTS && DBSQL_DEBUG */

#ifndef DBSQL_OMIT_VIRTUALTABLE
/*
** Transfer error message text from an dbsql_vtab_t.errmsg (text stored
** in memory obtained from dbsql_malloc) into a vdbe_t.errmsg (text stored
** in memory obtained from __db_malloc).
*/
void __vtab_import_errmsg(vdbe_t *p, dbsql_vtab_t *vtab){
  if( vtab->errmsg ){
    DBSQL *db = p->db;
    __dbsql_db_free(db, p->errmsg);
    p->errmsg = __db_str_dup(db, vtab->errmsg);
    dbsql_free(vtab->errmsg);
    vtab->errmsg = 0;
  }
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK

/*
** If the second argument is not NULL, release any allocations associated
** with the memory cells in the p->mems[] array. Also free the unpacked_record_t
** structure itself, using __dbsql_db_free().
**
** This function is used to free unpacked_record_t structures allocated by
** the __vdbe_unpack_record() function found in vdbeapi.c.
*/
static void __vdbe_free_unpacked(DBSQL *db, int n_field, unpacked_record_t *p){
  assert( db!=0 );
  if( p ){
    int i;
    for(i=0; i<n_field; i++){
      mem_t *mem = &p->mems[i];
      if( mem->z_malloc ) __vdbe_mem_release_malloc(mem);
    }
    __db_nn_free_nn(db, p);
  }
}
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
/*
** Invoke the pre-update hook. If this is an UPDATE or DELETE pre-update call,
** then cursor passed as the second argument should point to the row about
** to be update or deleted. If the application calls dbsql_preupdate_old(),
** the required value will be read from the row the cursor points to.
*/
void __vdbe_pre_update_hook(
  vdbe_t *v,                        /* vdbe_t pre-update hook is invoked by */
  vdbe_cursor_t *csr,               /* Cursor to grab old.* values from */
  int op,                         /* DBSQL_INSERT, UPDATE or DELETE */
  const char *z_db,                /* Database name */
  table_t *tab,                    /* Modified table */
  int64_t i_key1,                      /* Initial key value */
  int i_reg,                       /* Register for new.* record */
  int i_blob_write
){
  DBSQL *db = v->db;
  int64_t i_key2;
  pre_update_t preupdate;
  const char *tbl = tab->name;
#ifdef DBSQL_DEBUG
  int n_real_col;
  if( tab->tab_flags & TF_WithoutRowid ){
    n_real_col = __primary_key_index(tab)->n_column;
  }else if( tab->tab_flags & TF_HasVirtual ){
    n_real_col = tab->n_nv_col;
  }else{
    n_real_col = tab->n_col;
  }
#endif

  assert( db->pre_update==0 );
  memset(&preupdate, 0, sizeof(pre_update_t));
  if( HAS_ROWID(tab)==0 ){
    i_key1 = i_key2 = 0;
    preupdate.pk = __primary_key_index(tab);
  }else{
    if( op==DBSQL_UPDATE ){
      i_key2 = v->mems[i_reg].u.i;
    }else{
      i_key2 = i_key1;
    }
  }

  assert( csr!=0 );
  assert( csr->cur_type==CURTYPE_BTREE );
  assert( csr->n_field==n_real_col
       || (csr->n_field==n_real_col+1 && op==DBSQL_DELETE && i_reg==-1)
  );

  preupdate.v = v;
  preupdate.csr = csr;
  preupdate.op = op;
  preupdate.i_new_reg = i_reg;
  preupdate.keyinfo = (key_info_t*)&preupdate.u_key;
  preupdate.keyinfo->db = db;
  preupdate.keyinfo->enc = ENC(db);
  preupdate.keyinfo->n_key_field = tab->n_col;
  preupdate.keyinfo->sort_flags = 0; /* Indicate .colls, .n_all_field uninit */
  preupdate.i_key1 = i_key1;
  preupdate.i_key2 = i_key2;
  preupdate.tab = tab;
  preupdate.i_blob_write = i_blob_write;

  db->pre_update = &preupdate;
  db->pre_update_callback(db->pre_update_arg, db, op, z_db, tbl, i_key1, i_key2);
  db->pre_update = 0;
  __dbsql_db_free(db, preupdate.records);
  __vdbe_free_unpacked(db, preupdate.keyinfo->n_key_field+1,preupdate.unpacked);
  __vdbe_free_unpacked(db, preupdate.keyinfo->n_key_field+1,preupdate.new_unpacked);
  __vdbe_mem_release(&preupdate.oldipk);
  if( preupdate.news ){
    int i;
    for(i=0; i<csr->n_field; i++){
      __vdbe_mem_release(&preupdate.news[i]);
    }
    __db_nn_free_nn(db, preupdate.news);
  }
  if( preupdate.dflts ){
    int i;
    for(i=0; i<tab->n_col; i++){
      __value_free(preupdate.dflts[i]);
    }
    __dbsql_db_free(db, preupdate.dflts);
  }
}
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */

#ifdef DBSQL_ENABLE_PERCENTILE
/*
** Return the name of an SQL function associated with the dbsql_func_t.
*/
const char *__vdbe_func_name(const dbsql_func_t *ctx){
  assert( ctx!=0 );
  assert( ctx->func!=0 );
  return ctx->func->name;
}
#endif /* DBSQL_ENABLE_PERCENTILE */
