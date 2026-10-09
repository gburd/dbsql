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
** This file contains code use to implement APIs that are part of the
** VDBE.
*/
#include "dbsql_int.h"
#include "vdbe_int.h"
#include "opcodes.h"

#ifndef DBSQL_OMIT_DEPRECATED
/*
** Return TRUE (non-zero) of the statement supplied as an argument needs
** to be recompiled.  A statement needs to be recompiled whenever the
** execution environment changes in a way that would alter the program
** that dbsql_prepare() generates.  For example, if new functions or
** collating sequences are registered or if an authorizer function is
** added or changed.
*/
int dbsql_expired(dbsql_stmt_t *stmt){
  int i_ret = 1;
  if( stmt ){
    vdbe_t *p = (vdbe_t*)stmt;
    dbsql_mutex_enter(p->db->mutex);
    i_ret = p->expired;
    dbsql_mutex_leave(p->db->mutex);
  }
  return i_ret;
}
#endif

/*
** Check on a vdbe_t to make sure it has not been finalized.  Log
** an error and return true if it has been finalized (or is otherwise
** invalid).  Return false if it is ok.
*/
static int __vdbe_safety(vdbe_t *p){
  if( p->db==0 ){
    dbsql_log(DBSQL_MISUSE, "API called with finalized prepared statement");
    return 1;
  }else{
    return 0;
  }
}
static int __vdbe_safety_not_null(vdbe_t *p){
  if( p==0 ){
    dbsql_log(DBSQL_MISUSE, "API called with NULL prepared statement");
    return 1;
  }else{
    return __vdbe_safety(p);
  }
}

#ifndef DBSQL_OMIT_TRACE
/*
** Invoke the profile callback.  This routine is only called if we already
** know that the profile callback is defined and needs to be invoked.
*/
static DBSQL_NOINLINE void __invoke_profile_callback(DBSQL *db, vdbe_t *p){
  int64_t i_now;
  int64_t i_elapse;
  assert( p->startTime>0 );
  assert( db->init.busy==0 );
  assert( p->z_sql!=0 );
  __dbsql_os_current_time_int64(db->vfs, &i_now);
  i_elapse = (i_now - p->startTime)*1000000;
#ifndef DBSQL_OMIT_DEPRECATED
  if( db->profile ){
    db->profile(db->profile_arg, p->z_sql, i_elapse);
  }
#endif
  if( db->m_trace & DBSQL_TRACE_PROFILE ){
    db->trace.v2(DBSQL_TRACE_PROFILE, db->trace_arg, p, (void*)&i_elapse);
  }
  p->startTime = 0;
}
/*
** The CHECK_PROFILE_CALLBACK(DB,P) macro checks to see if a profile callback
** is needed, and it invokes the callback if it is needed.
*/
# define CHECK_PROFILE_CALLBACK(DB,P) \
   if( ((P)->startTime)>0 ){ __invoke_profile_callback(DB,P); }
#else
# define CHECK_PROFILE_CALLBACK(DB,P)  /*no-op*/
#endif

/*
** The following routine destroys a virtual machine that is created by
** the dbsql_compile() routine. The integer returned is an DBSQL_
** success/failure code that describes the result of executing the virtual
** machine.
**
** This routine sets the error code and string returned by
** dbsql_errcode(), dbsql_errmsg() and dbsql_errmsg16().
*/
int dbsql_finalize(dbsql_stmt_t *stmt){
  int rc;
  if( stmt==0 ){
    /* IMPLEMENTATION-OF: R-57228-12904 Invoking dbsql_finalize() on a NULL
    ** pointer is a harmless no-op. */
    rc = DBSQL_OK;
  }else{
    vdbe_t *v = (vdbe_t*)stmt;
    DBSQL *db = v->db;
    if( __vdbe_safety(v) ) return DBSQL_MISUSE_BKPT;
    dbsql_mutex_enter(db->mutex);
    CHECK_PROFILE_CALLBACK(db, v);
    assert( v->vdbe_state>=VDBE_READY_STATE );
    rc = __vdbe_reset(v);
    __vdbe_delete(v);
    rc = __api_exit(db, rc);
    __leave_mutex_and_close_zombie(db);
  }
  return rc;
}

/*
** Terminate the current execution of an SQL statement and reset it
** back to its starting state so that it can be reused. A success code from
** the prior execution is returned.
**
** This routine sets the error code and string returned by
** dbsql_errcode(), dbsql_errmsg() and dbsql_errmsg16().
*/
int dbsql_reset(dbsql_stmt_t *stmt){
  int rc;
  if( stmt==0 ){
    rc = DBSQL_OK;
  }else{
    vdbe_t *v = (vdbe_t*)stmt;
    DBSQL *db = v->db;
    dbsql_mutex_enter(db->mutex);
    CHECK_PROFILE_CALLBACK(db, v);
    rc = __vdbe_reset(v);
    __vdbe_rewind(v);
    assert( (rc & (db->err_mask))==rc );
    rc = __api_exit(db, rc);
    dbsql_mutex_leave(db->mutex);
  }
  return rc;
}

/*
** Set all the parameters in the compiled SQL statement to NULL.
*/
int dbsql_clear_bindings(dbsql_stmt_t *stmt){
  int i;
  int rc = DBSQL_OK;
  vdbe_t *p = (vdbe_t*)stmt;
#if DBSQL_THREADSAFE
  dbsql_mutex_t *mutex;
#endif
#ifdef DBSQL_ENABLE_API_ARMOR
  if( stmt==0 ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
#if DBSQL_THREADSAFE
  mutex = p->db->mutex;
#endif
  dbsql_mutex_enter(mutex);
  for(i=0; i<p->n_var; i++){
    __vdbe_mem_release(&p->vars[i]);
    p->vars[i].flags = MEM_Null;
  }
  assert( (p->prep_flags & DBSQL_PREPARE_SAVESQL)!=0 || p->expmask==0 );
  if( p->expmask ){
    p->expired = 1;
  }
  dbsql_mutex_leave(mutex);
  return rc;
}


/**************************** dbsql_value_  *******************************
** The following routines extract information from a mem_t or dbsql_value_t
** structure.
*/
const void *dbsql_value_blob(dbsql_value_t *val){
  mem_t *p = (mem_t*)val;
  if( p->flags & (MEM_Blob|MEM_Str) ){
    if( EXPAND_BLOB(p)!=DBSQL_OK ){
      assert( p->flags==MEM_Null && p->z==0 );
      return 0;
    }
    p->flags |= MEM_Blob;
    return p->n ? p->z : 0;
  }else{
    return dbsql_value_text(val);
  }
}
int dbsql_value_bytes(dbsql_value_t *val){
  return __value_bytes(val, DBSQL_UTF8);
}
int dbsql_value_bytes16(dbsql_value_t *val){
  return __value_bytes(val, DBSQL_UTF16NATIVE);
}
double dbsql_value_double(dbsql_value_t *val){
  return __vdbe_real_value((mem_t*)val);
}
int dbsql_value_int(dbsql_value_t *val){
  return (int)__vdbe_int_value((mem_t*)val);
}
int64_t dbsql_value_int64(dbsql_value_t *val){
  return __vdbe_int_value((mem_t*)val);
}
unsigned int dbsql_value_subtype(dbsql_value_t *val){
  mem_t *mem = (mem_t*)val;
  return ((mem->flags & MEM_Subtype) ? mem->subtype : 0);
}
void *dbsql_value_pointer(dbsql_value_t *val, const char *p_type){
  mem_t *p = (mem_t*)val;
  if( (p->flags&(MEM_TypeMask|MEM_Term|MEM_Subtype)) ==
                 (MEM_Null|MEM_Term|MEM_Subtype)
   && p_type!=0
   && p->subtype=='p'
   && strcmp(p->u.p_type, p_type)==0
  ){
    return (void*)p->z;
  }else{
    return 0;
  }
}
const unsigned char *dbsql_value_text(dbsql_value_t *val){
  return (const unsigned char *)__value_text(val, DBSQL_UTF8);
}
#ifndef DBSQL_OMIT_UTF16
const void *dbsql_value_text16(dbsql_value_t* val){
  return __value_text(val, DBSQL_UTF16NATIVE);
}
const void *dbsql_value_text16be(dbsql_value_t *val){
  return __value_text(val, DBSQL_UTF16BE);
}
const void *dbsql_value_text16le(dbsql_value_t *val){
  return __value_text(val, DBSQL_UTF16LE);
}
#endif /* DBSQL_OMIT_UTF16 */
/* EVIDENCE-OF: R-12793-43283 Every value in dbsql has one of five
** fundamental datatypes: 64-bit signed integer 64-bit IEEE floating
** point number string BLOB NULL
*/
int dbsql_value_type(dbsql_value_t* val){
  static const u_int8_t types[] = {
     DBSQL_BLOB,     /* 0x00 (not possible) */
     DBSQL_NULL,     /* 0x01 NULL */
     DBSQL_TEXT,     /* 0x02 TEXT */
     DBSQL_NULL,     /* 0x03 (not possible) */
     DBSQL_INTEGER,  /* 0x04 INTEGER */
     DBSQL_NULL,     /* 0x05 (not possible) */
     DBSQL_INTEGER,  /* 0x06 INTEGER + TEXT */
     DBSQL_NULL,     /* 0x07 (not possible) */
     DBSQL_FLOAT,    /* 0x08 FLOAT */
     DBSQL_NULL,     /* 0x09 (not possible) */
     DBSQL_FLOAT,    /* 0x0a FLOAT + TEXT */
     DBSQL_NULL,     /* 0x0b (not possible) */
     DBSQL_INTEGER,  /* 0x0c (not possible) */
     DBSQL_NULL,     /* 0x0d (not possible) */
     DBSQL_INTEGER,  /* 0x0e (not possible) */
     DBSQL_NULL,     /* 0x0f (not possible) */
     DBSQL_BLOB,     /* 0x10 BLOB */
     DBSQL_NULL,     /* 0x11 (not possible) */
     DBSQL_TEXT,     /* 0x12 (not possible) */
     DBSQL_NULL,     /* 0x13 (not possible) */
     DBSQL_INTEGER,  /* 0x14 INTEGER + BLOB */
     DBSQL_NULL,     /* 0x15 (not possible) */
     DBSQL_INTEGER,  /* 0x16 (not possible) */
     DBSQL_NULL,     /* 0x17 (not possible) */
     DBSQL_FLOAT,    /* 0x18 FLOAT + BLOB */
     DBSQL_NULL,     /* 0x19 (not possible) */
     DBSQL_FLOAT,    /* 0x1a (not possible) */
     DBSQL_NULL,     /* 0x1b (not possible) */
     DBSQL_INTEGER,  /* 0x1c (not possible) */
     DBSQL_NULL,     /* 0x1d (not possible) */
     DBSQL_INTEGER,  /* 0x1e (not possible) */
     DBSQL_NULL,     /* 0x1f (not possible) */
     DBSQL_FLOAT,    /* 0x20 INTREAL */
     DBSQL_NULL,     /* 0x21 (not possible) */
     DBSQL_FLOAT,    /* 0x22 INTREAL + TEXT */
     DBSQL_NULL,     /* 0x23 (not possible) */
     DBSQL_FLOAT,    /* 0x24 (not possible) */
     DBSQL_NULL,     /* 0x25 (not possible) */
     DBSQL_FLOAT,    /* 0x26 (not possible) */
     DBSQL_NULL,     /* 0x27 (not possible) */
     DBSQL_FLOAT,    /* 0x28 (not possible) */
     DBSQL_NULL,     /* 0x29 (not possible) */
     DBSQL_FLOAT,    /* 0x2a (not possible) */
     DBSQL_NULL,     /* 0x2b (not possible) */
     DBSQL_FLOAT,    /* 0x2c (not possible) */
     DBSQL_NULL,     /* 0x2d (not possible) */
     DBSQL_FLOAT,    /* 0x2e (not possible) */
     DBSQL_NULL,     /* 0x2f (not possible) */
     DBSQL_BLOB,     /* 0x30 (not possible) */
     DBSQL_NULL,     /* 0x31 (not possible) */
     DBSQL_TEXT,     /* 0x32 (not possible) */
     DBSQL_NULL,     /* 0x33 (not possible) */
     DBSQL_FLOAT,    /* 0x34 (not possible) */
     DBSQL_NULL,     /* 0x35 (not possible) */
     DBSQL_FLOAT,    /* 0x36 (not possible) */
     DBSQL_NULL,     /* 0x37 (not possible) */
     DBSQL_FLOAT,    /* 0x38 (not possible) */
     DBSQL_NULL,     /* 0x39 (not possible) */
     DBSQL_FLOAT,    /* 0x3a (not possible) */
     DBSQL_NULL,     /* 0x3b (not possible) */
     DBSQL_FLOAT,    /* 0x3c (not possible) */
     DBSQL_NULL,     /* 0x3d (not possible) */
     DBSQL_FLOAT,    /* 0x3e (not possible) */
     DBSQL_NULL,     /* 0x3f (not possible) */
  };
#ifdef DBSQL_DEBUG
  {
    int type = DBSQL_BLOB;
    if( val->flags & MEM_Null ){
      type = DBSQL_NULL;
    }else if( val->flags & (MEM_Real|MEM_IntReal) ){
      type = DBSQL_FLOAT;
    }else if( val->flags & MEM_Int ){
      type = DBSQL_INTEGER;
    }else if( val->flags & MEM_Str ){
      type = DBSQL_TEXT;
    }
    assert( type == types[val->flags&MEM_AffMask] );
  }
#endif
  return types[val->flags&MEM_AffMask];
}
int dbsql_value_encoding(dbsql_value_t *val){
  return val->enc;
}

/* Return true if a parameter to update represents an unchanged column */
int dbsql_value_nochange(dbsql_value_t *val){
  return (val->flags&(MEM_Null|MEM_Zero))==(MEM_Null|MEM_Zero);
}

/* Return true if a parameter value originated from an dbsql_bind() */
int dbsql_value_frombind(dbsql_value_t *val){
  return (val->flags&MEM_FromBind)!=0;
}

/* Make a copy of an dbsql_value_t object
*/
dbsql_value_t *dbsql_value_dup(const dbsql_value_t *orig){
  dbsql_value_t *p_new;
  if( orig==0 ) return 0;
  p_new = dbsql_malloc( sizeof(*p_new) );
  if( p_new==0 ) return 0;
  memset(p_new, 0, sizeof(*p_new));
  memcpy(p_new, orig, MEMCELLSIZE);
  p_new->flags &= ~MEM_Dyn;
  p_new->db = 0;
  if( p_new->flags&(MEM_Str|MEM_Blob) ){
    p_new->flags &= ~(MEM_Static|MEM_Dyn);
    p_new->flags |= MEM_Ephem;
    if( __vdbe_mem_make_writeable(p_new)!=DBSQL_OK ){
      __value_free(p_new);
      p_new = 0;
    }
  }else if( p_new->flags & MEM_Null ){
    /* Do not duplicate pointer values */
    p_new->flags &= ~(MEM_Term|MEM_Subtype);
  }
  return p_new;
}

/* Destroy an dbsql_value_t object previously obtained from
** dbsql_value_dup().
*/
void dbsql_value_free(dbsql_value_t *old){
  __value_free(old);
}
 

/**************************** dbsql_result_  *******************************
** The following routines are used by user-defined functions to specify
** the function result.
**
** The set_str_or_error() function calls __vdbe_mem_set_str() to store the
** result as a string or blob.  Appropriate errors are set if the string/blob
** is too big or if an OOM occurs.
**
** The __invoke_value_destructor(P,X) routine invokes destructor function X()
** on value P if P is not going to be used and need to be destroyed.
*/
static void __set_result_str_or_error(
  dbsql_func_t *ctx,  /* Function context */
  const char *z,          /* String pointer */
  int n,                  /* Bytes in string, or negative */
  u_int8_t enc,                 /* Encoding of z.  0 for BLOBs */
  void (*del)(void*)     /* Destructor function */
){
  mem_t *out = ctx->out;
  int rc;
  if( enc==DBSQL_UTF8 ){
    rc = __vdbe_mem_set_text(out, z, n, del);
  }else if( enc==DBSQL_UTF8_ZT ){
    /* It is usually considered improper to assert() on an input. However,
    ** the following assert() is checking for inputs that are documented
    ** to result in undefined behavior. */
    assert( z==0
         || n<0 
         || n>out->db->limits[DBSQL_LIMIT_LENGTH]
         || z[n]==0
    );
    rc = __vdbe_mem_set_text(out, z, n, del);
    out->flags |= MEM_Term;
  }else{
    rc = __vdbe_mem_set_str(out, z, n, enc, del);
  }
  if( rc ){
    if( rc==DBSQL_TOOBIG ){
      dbsql_result_error_toobig(ctx);
    }else{
      /* The only errors possible from __vdbe_mem_set_str are
      ** DBSQL_TOOBIG and DBSQL_NOMEM */
      assert( rc==DBSQL_NOMEM );
      dbsql_result_error_nomem(ctx);
    }
    return;
  }
  __vdbe_change_encoding(out, ctx->enc);
  if( __vdbe_mem_too_big(out) ){
    dbsql_result_error_toobig(ctx);
  }
}
static int __invoke_value_destructor(
  const void *p,             /* Value to destroy */
  void (*del)(void*),       /* The destructor */
  dbsql_func_t *ctx      /* Set a DBSQL_TOOBIG error if not NULL */
){
  assert( del!=DBSQL_DYNAMIC );
  if( del==0 ){
    /* noop */
  }else if( del==DBSQL_TRANSIENT ){
    /* noop */
  }else{
    del((void*)p);
  }
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx!=0 ){
    dbsql_result_error_toobig(ctx);
  }
#else
  assert( ctx!=0 );
  dbsql_result_error_toobig(ctx);
#endif
  return DBSQL_TOOBIG;
}
void dbsql_result_blob(
  dbsql_func_t *ctx,
  const void *z,
  int n,
  void (*del)(void *)
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 || n<0 ){
    __invoke_value_destructor(z, del, ctx);
    return;
  }
#endif
  assert( n>=0 );
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __set_result_str_or_error(ctx, z, n, 0, del);
}
void dbsql_result_blob64(
  dbsql_func_t *ctx,
  const void *z,
  u_int64_t n,
  void (*del)(void *)
){
  assert( del!=DBSQL_DYNAMIC );
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ){
    __invoke_value_destructor(z, del, 0);
    return;
  }
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  if( n>0x7fffffff ){
    (void)__invoke_value_destructor(z, del, ctx);
  }else{
    __set_result_str_or_error(ctx, z, (int)n, 0, del);
  }
}
void dbsql_result_double(dbsql_func_t *ctx, double r_val){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __vdbe_mem_set_double(ctx->out, r_val);
}
void dbsql_result_error(dbsql_func_t *ctx, const char *z, int n){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  ctx->isError = DBSQL_ERROR;
  __vdbe_mem_set_str(ctx->out, z, n, DBSQL_UTF8, DBSQL_TRANSIENT);
}
#ifndef DBSQL_OMIT_UTF16
void dbsql_result_error16(dbsql_func_t *ctx, const void *z, int n){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  ctx->isError = DBSQL_ERROR;
  __vdbe_mem_set_str(ctx->out, z, n, DBSQL_UTF16NATIVE, DBSQL_TRANSIENT);
}
#endif
void dbsql_result_int(dbsql_func_t *ctx, int i_val){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __vdbe_mem_set_int64(ctx->out, (int64_t)i_val);
}
void dbsql_result_int64(dbsql_func_t *ctx, int64_t i_val){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __vdbe_mem_set_int64(ctx->out, i_val);
}
void dbsql_result_null(dbsql_func_t *ctx){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __vdbe_mem_set_null(ctx->out);
}
void dbsql_result_pointer(
  dbsql_func_t *ctx,
  void *ptr,
  const char *p_type,
  void (*destructor)(void*)
){
  mem_t *out;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ){
    __invoke_value_destructor(ptr, destructor, 0);
    return;
  }
#endif
  out = ctx->out;
  assert( dbsql_mutex_held(out->db->mutex) );
  __vdbe_mem_release(out);
  out->flags = MEM_Null;
  __vdbe_mem_set_pointer(out, ptr, p_type, destructor);
}
void dbsql_result_subtype(dbsql_func_t *ctx, unsigned int subtype){
  mem_t *out;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
#if defined(DBSQL_STRICT_SUBTYPE) && DBSQL_STRICT_SUBTYPE+0!=0
  if( ctx->func!=0
   && (ctx->func->func_flags & DBSQL_RESULT_SUBTYPE)==0
  ){
    char err[200];
    dbsql_snprintf(sizeof(err), err,
                     "misuse of dbsql_result_subtype() by %s()", 
                     ctx->func->name);
    dbsql_result_error(ctx, err, -1);
    return;
  }
#endif /* DBSQL_STRICT_SUBTYPE */
  out = ctx->out;
  assert( dbsql_mutex_held(out->db->mutex) );
  out->subtype = subtype & 0xff;
  out->flags |= MEM_Subtype;
}
void dbsql_result_text(
  dbsql_func_t *ctx,
  const char *z,
  int n,
  void (*del)(void *)
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ){
    __invoke_value_destructor(z, del, 0);
    return;
  }
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __set_result_str_or_error(ctx, z, n, DBSQL_UTF8, del);
}
void dbsql_result_text64(
  dbsql_func_t *ctx,
  const char *z,
  u_int64_t n,
  void (*del)(void *),
  unsigned char enc
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ){
    __invoke_value_destructor(z, del, 0);
    return;
  }
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  assert( del!=DBSQL_DYNAMIC );
  if( enc!=DBSQL_UTF8 && enc!=DBSQL_UTF8_ZT ){
    if( enc==DBSQL_UTF16 ) enc = DBSQL_UTF16NATIVE;
    n &= ~(u_int64_t)1;
  }
  if( n>0x7fffffff ){
    (void)__invoke_value_destructor(z, del, ctx);
  }else{
    __set_result_str_or_error(ctx, z, (int)n, enc, del);
    __vdbe_mem_zero_terminate_if_able(ctx->out);
  }
}
#ifndef DBSQL_OMIT_UTF16
void dbsql_result_text16(
  dbsql_func_t *ctx,
  const void *z,
  int n,
  void (*del)(void *)
){
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __set_result_str_or_error(ctx, z, n & ~(u_int64_t)1, DBSQL_UTF16NATIVE, del);
}
void dbsql_result_text16be(
  dbsql_func_t *ctx,
  const void *z,
  int n,
  void (*del)(void *)
){
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __set_result_str_or_error(ctx, z, n & ~(u_int64_t)1, DBSQL_UTF16BE, del);
}
void dbsql_result_text16le(
  dbsql_func_t *ctx,
  const void *z,
  int n,
  void (*del)(void *)
){
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __set_result_str_or_error(ctx, z, n & ~(u_int64_t)1, DBSQL_UTF16LE, del);
}
#endif /* DBSQL_OMIT_UTF16 */
void dbsql_result_value(dbsql_func_t *ctx, dbsql_value_t *value){
  mem_t *out;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
  if( value==0 ){
    dbsql_result_null(ctx);
    return;
  }
#endif
  out = ctx->out;
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __vdbe_mem_copy(out, value);
  __vdbe_change_encoding(out, ctx->enc);
  if( __vdbe_mem_too_big(out) ){
    dbsql_result_error_toobig(ctx);
  }
}
void dbsql_result_zeroblob(dbsql_func_t *ctx, int n){
  dbsql_result_zeroblob64(ctx, n>0 ? n : 0);
}
int dbsql_result_zeroblob64(dbsql_func_t *ctx, u_int64_t n){
  mem_t *out;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return DBSQL_MISUSE_BKPT;
#endif
  out = ctx->out;
  assert( dbsql_mutex_held(out->db->mutex) );
  if( n>(u_int64_t)out->db->limits[DBSQL_LIMIT_LENGTH] ){
    dbsql_result_error_toobig(ctx);
    return DBSQL_TOOBIG;
  }
#ifndef DBSQL_OMIT_INCRBLOB
  __vdbe_mem_set_zero_blob(ctx->out, (int)n);
  return DBSQL_OK;
#else
  return __vdbe_mem_set_zero_blob(ctx->out, (int)n);
#endif
}
void dbsql_result_error_code(dbsql_func_t *ctx, int errCode){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
  ctx->isError = errCode ? errCode : -1;
#ifdef DBSQL_DEBUG
  if( ctx->vdbe ) ctx->vdbe->rc_app = errCode;
#endif
  if( ctx->out->flags & MEM_Null ){
    __set_result_str_or_error(ctx, __err_str(errCode), -1, DBSQL_UTF8,
                        DBSQL_STATIC);
  }
}

/* Force an DBSQL_TOOBIG error. */
void dbsql_result_error_toobig(dbsql_func_t *ctx){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  ctx->isError = DBSQL_TOOBIG;
  __vdbe_mem_set_str(ctx->out, "string or blob too big", -1,
                       DBSQL_UTF8, DBSQL_STATIC);
}

/* An DBSQL_NOMEM error. */
void dbsql_result_error_nomem(dbsql_func_t *ctx){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  __vdbe_mem_set_null(ctx->out);
  ctx->isError = DBSQL_NOMEM_BKPT;
  __oom_fault(ctx->out->db);
}

#ifndef DBSQL_UNTESTABLE
/* Force the INT64 value currently stored as the result to be
** a MEM_IntReal value.  See the DBSQL_TESTCTRL_RESULT_INTREAL
** test-control.
*/
void __result_int_real(dbsql_func_t *ctx){
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
  if( ctx->out->flags & MEM_Int ){
    ctx->out->flags &= ~MEM_Int;
    ctx->out->flags |= MEM_IntReal;
  }
}
#endif


/*
** This function is called after a transaction has been committed. It
** invokes callbacks registered with dbsql_wal_hook() as required.
*/
static int __do_wal_callbacks(DBSQL *db){
  int rc = DBSQL_OK;
#ifndef DBSQL_OMIT_WAL
  int i;
  for(i=0; i<db->n_db; i++){
    sm_t *bt = db->dbs[i].bt;
    if( bt ){
      int n_entry;
      __sm_enter(bt);
      n_entry = __sm_pager_wal_callback(__sm_pager(bt));
      __sm_leave(bt);
      if( n_entry>0 && db->wal_callback && rc==DBSQL_OK ){
        rc = db->wal_callback(db->wal_arg, db, db->dbs[i].db_s_name, n_entry);
      }
    }
  }
#else
  UNUSED_PARAMETER(db);
#endif
  return rc;
}


/*
** Execute the statement stmt, either until a row of data is ready, the
** statement is completely executed or an error occurs.
**
** This routine implements the bulk of the logic behind the dbsql_step()
** API.  The only thing omitted is the automatic recompile if a
** schema change has occurred.  That detail is handled by the
** outer dbsql_step() wrapper procedure.
*/
static int __step(vdbe_t *p){
  DBSQL *db;
  int rc;

  assert(p);
  db = p->db;
  if( p->vdbe_state!=VDBE_RUN_STATE ){
    restart_step:
    if( p->vdbe_state==VDBE_READY_STATE ){
      if( p->expired ){
        p->rc = DBSQL_SCHEMA;
        rc = DBSQL_ERROR;
        if( (p->prep_flags & DBSQL_PREPARE_SAVESQL)!=0 ){
          /* If this statement was prepared using saved SQL and an
          ** error has occurred, then return the error code in p->rc to the
          ** caller. Set the error code in the database handle to the same
          ** value.
          */
          rc = __vdbe_transfer_error(p);
        }
        goto end_of_step;
      }

      /* If there are no other statements currently running, then
      ** reset the interrupt flag.  This prevents a call to dbsql_interrupt
      ** from interrupting a statement that has not yet started.
      */
      if( db->n_vdbe_active==0 ){
        ATOMIC_STORE(&db->u1.is_interrupted, 0);
      }

      assert( db->n_vdbe_write>0 || db->auto_commit==0
          || ((db->n_deferred_cons + db->n_deferred_imm_cons)==0)
      );

#ifndef DBSQL_OMIT_TRACE
      if( (db->m_trace & (DBSQL_TRACE_PROFILE|DBSQL_TRACE_XPROFILE))!=0
          && !db->init.busy && p->z_sql ){
        __dbsql_os_current_time_int64(db->vfs, &p->startTime);
      }else{
        assert( p->startTime==0 );
      }
#endif

      db->n_vdbe_active++;
      if( p->readOnly==0 ) db->n_vdbe_write++;
      if( p->is_reader ) db->n_vdbe_read++;
      p->pc = 0;
      p->vdbe_state = VDBE_RUN_STATE;
    }else

    if( ALWAYS(p->vdbe_state==VDBE_HALT_STATE) ){
      /* We used to require that dbsql_reset() be called before retrying
      ** dbsql_step() after any error or after DBSQL_DONE.  But beginning
      ** with version 3.7.0, we changed this so that dbsql_reset() would
      ** be called automatically instead of throwing the DBSQL_MISUSE error.
      ** This "automatic-reset" change is not technically an incompatibility,
      ** since any application that receives an DBSQL_MISUSE is broken by
      ** definition.
      **
      ** Nevertheless, some published applications that were originally written
      ** for version 3.6.23 or earlier do in fact depend on DBSQL_MISUSE
      ** returns, and those were broken by the automatic-reset change.  As a
      ** a work-around, the DBSQL_OMIT_AUTORESET compile-time restores the
      ** legacy behavior of returning DBSQL_MISUSE for cases where the
      ** previous dbsql_step() returned something other than a DBSQL_LOCKED
      ** or DBSQL_BUSY error.
      */
#ifdef DBSQL_OMIT_AUTORESET
      if( (rc = p->rc&0xff)==DBSQL_BUSY || rc==DBSQL_LOCKED ){
        dbsql_reset((dbsql_stmt_t*)p);
      }else{
        return DBSQL_MISUSE_BKPT;
      }
#else
      dbsql_reset((dbsql_stmt_t*)p);
#endif
      assert( p->vdbe_state==VDBE_READY_STATE );
      goto restart_step;
    }
  }

#ifdef DBSQL_DEBUG
  p->rc_app = DBSQL_OK;
#endif
#ifndef DBSQL_OMIT_EXPLAIN
  if( p->explain ){
    rc = __vdbe_list(p);
  }else
#endif /* DBSQL_OMIT_EXPLAIN */
  {
    db->n_vdbe_exec++;
    rc = __vdbe_exec(p);
    db->n_vdbe_exec--;
  }

  if( rc==DBSQL_ROW ){
    assert( p->rc==DBSQL_OK );
    assert( db->malloc_failed==0 );
    db->errCode = DBSQL_ROW;
    return DBSQL_ROW;
  }else{
#ifndef DBSQL_OMIT_TRACE
    /* If the statement completed successfully, invoke the profile callback */
    CHECK_PROFILE_CALLBACK(db, p);
#endif
    p->result_row = 0;
    if( rc==DBSQL_DONE && db->auto_commit ){
      assert( p->rc==DBSQL_OK );
      p->rc = __do_wal_callbacks(db);
      if( p->rc!=DBSQL_OK ){
        rc = DBSQL_ERROR;
      }
    }else if( rc!=DBSQL_DONE && (p->prep_flags & DBSQL_PREPARE_SAVESQL)!=0 ){
      /* If this statement was prepared using saved SQL and an
      ** error has occurred, then return the error code in p->rc to the
      ** caller. Set the error code in the database handle to the same value.
      */
      rc = __vdbe_transfer_error(p);
    }
  }

  db->errCode = rc;
  if( DBSQL_NOMEM==__api_exit(p->db, p->rc) ){
    p->rc = DBSQL_NOMEM_BKPT;
    if( (p->prep_flags & DBSQL_PREPARE_SAVESQL)!=0 ) rc = p->rc;
  }
end_of_step:
  /* There are only a limited number of result codes allowed from the
  ** statements prepared using the legacy dbsql_prepare() interface */
  assert( (p->prep_flags & DBSQL_PREPARE_SAVESQL)!=0
       || rc==DBSQL_ROW  || rc==DBSQL_DONE   || rc==DBSQL_ERROR
       || (rc&0xff)==DBSQL_BUSY || rc==DBSQL_MISUSE
  );
  return (rc&db->err_mask);
}

/*
** This is the top-level implementation of dbsql_step().  Call
** __step() to do most of the work.  If a schema error occurs,
** call __reprepare() and try again.
*/
int dbsql_step(dbsql_stmt_t *stmt){
  int rc = DBSQL_OK;      /* Result from __step() */
  vdbe_t *v = (vdbe_t*)stmt;  /* the prepared statement */
  int cnt = 0;             /* Counter to prevent infinite loop of reprepares */
  DBSQL *db;             /* The database connection */

  if( __vdbe_safety_not_null(v) ){
    return DBSQL_MISUSE_BKPT;
  }
  db = v->db;
  dbsql_mutex_enter(db->mutex);
  while( (rc = __step(v))==DBSQL_SCHEMA
         && cnt++ < DBSQL_MAX_SCHEMA_RETRY ){
    int saved_pc = v->pc;
    rc = __reprepare(v);
    if( rc!=DBSQL_OK ){
      /* This case occurs after failing to recompile an sql statement.
      ** The error message from the SQL compiler has already been loaded
      ** into the database handle. This block copies the error message
      ** from the database handle into the statement and sets the statement
      ** program counter to 0 to ensure that when the statement is
      ** finalized or reset the parser error message is available via
      ** dbsql_errmsg() and dbsql_errcode().
      */
      const char *err = (const char *)dbsql_value_text(db->err);
      __dbsql_db_free(db, v->errmsg);
      if( !db->malloc_failed ){
        v->errmsg = __db_str_dup(db, err);
        v->rc = rc = __api_exit(db, rc);
      } else {
        v->errmsg = 0;
        v->rc = rc = DBSQL_NOMEM_BKPT;
      }
      break;
    }
    dbsql_reset(stmt);
    if( saved_pc>=0 ){
      /* Setting min_write_file_format to 254 is a signal to the OP_Init and
      ** OP_Trace opcodes to *not* perform DBSQL_TRACE_STMT because it has
      ** already been done once on a prior invocation that failed due to
      ** DBSQL_SCHEMA.   tag-20220401a  */
      v->min_write_file_format = 254;
    }
    assert( v->expired==0 );
  }
  dbsql_mutex_leave(db->mutex);
  return rc;
}


/*
** Extract the user data from a dbsql_func_t structure and return a
** pointer to it.
*/
void *dbsql_user_data(dbsql_func_t *p){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( p==0 ) return 0;
#endif
  assert( p && p->func );
  return p->func->user_data;
}

/*
** Extract the user data from a dbsql_func_t structure and return a
** pointer to it.
**
** IMPLEMENTATION-OF: R-46798-50301 The dbsql_context_db_handle() interface
** returns a copy of the pointer to the database connection (the 1st
** parameter) of the dbsql_create_function() and
** dbsql_create_function16() routines that originally registered the
** application defined function.
*/
DBSQL *dbsql_context_db_handle(dbsql_func_t *p){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( p==0 ) return 0;
#else
  assert( p && p->out );
#endif
  return p->out->db;
}

/*
** If this routine is invoked from within an column method of a virtual
** table, then it returns true if and only if the the call is during an
** UPDATE operation and the value of the column will not be modified
** by the UPDATE.
**
** If this routine is called from any context other than within the
** column method of a virtual table, then the return value is meaningless
** and arbitrary.
**
** Virtual table implements might use this routine to optimize their
** performance by substituting a NULL result, or some other light-weight
** value, as a signal to the update routine that the column is unchanged.
*/
int dbsql_vtab_nochange(dbsql_func_t *p){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( p==0 ) return 0;
#else
  assert( p );
#endif
  return dbsql_value_nochange(p->out);
}

/*
** The destructor function for a value_list_t object.  This needs to be
** a separate function, unknowable to the application, to ensure that
** calls to dbsql_vtab_in_first()/dbsql_vtab_in_next() that are not
** preceded by activation of IN processing via dbsql_vtab_int() do not
** try to access a fake value_list_t object inserted by a hostile extension.
*/
void __vdbe_value_list_free(void *to_delete){
  dbsql_free(to_delete);
}

/*
** Implementation of dbsql_vtab_in_first() (if next==0) and
** dbsql_vtab_in_next() (if next!=0).
*/
static int __value_from_value_list(
  dbsql_value_t *val,        /* Pointer to the value_list_t object */
  dbsql_value_t **pp_out,      /* Store the next value from the list here */
  int next                   /* 1 for _next(). 0 for _first() */
){
  int rc;
  value_list_t *rhs;

  *pp_out = 0;
  if( val==0 ) return DBSQL_MISUSE_BKPT;
  if( (val->flags & MEM_Dyn)==0 || val->x_del!=__vdbe_value_list_free ){
    return DBSQL_ERROR;
  }else{
    assert( (val->flags&(MEM_TypeMask|MEM_Term|MEM_Subtype)) ==
                 (MEM_Null|MEM_Term|MEM_Subtype) );
    assert( val->subtype=='p' );
    assert( val->u.p_type!=0 && strcmp(val->u.p_type,"ValueList")==0 );
    rhs = (value_list_t*)val->z;
  }
  if( next ){
    rc = __sm_next(rhs->csr, 0);
  }else{
    int dummy = 0;
    rc = __sm_first(rhs->csr, &dummy);
    assert( rc==DBSQL_OK || __sm_eof(rhs->csr) );
    if( __sm_eof(rhs->csr) ) rc = DBSQL_DONE;
  }
  if( rc==DBSQL_OK ){
    u_int32_t sz;       /* Size of current row in bytes */
    mem_t s_mem;     /* Raw content of current row */
    memset(&s_mem, 0, sizeof(s_mem));
    sz = __sm_payload_size(rhs->csr);
    rc = __vdbe_mem_from_btree_zero_offset(rhs->csr,sz,&s_mem);
    if( rc==DBSQL_OK ){
      u_int8_t *buf = (u_int8_t*)s_mem.z;
      u_int32_t i_serial;
      dbsql_value_t *out = rhs->out;
      int i_off = 1 + GET_VARINT32(&buf[1], i_serial);
      __vdbe_serial_get(&buf[i_off], i_serial, out);
      out->enc = ENC(out->db);
      if( (out->flags & MEM_Ephem)!=0 && __vdbe_mem_make_writeable(out) ){
        rc = DBSQL_NOMEM;
      }else{
        *pp_out = out;
      }
    }
    __vdbe_mem_release(&s_mem);
  }
  return rc;
}

/*
** Set the iterator value val to point to the first value in the set.
** Set (*out) to point to this value before returning.
*/
int dbsql_vtab_in_first(dbsql_value_t *val, dbsql_value_t **out){
  return __value_from_value_list(val, out, 0);
}

/*
** Set the iterator value val to point to the next value in the set.
** Set (*out) to point to this value before returning.
*/
int dbsql_vtab_in_next(dbsql_value_t *val, dbsql_value_t **out){
  return __value_from_value_list(val, out, 1);
}

/*
** Return the current time for a statement.  If the current time
** is requested more than once within the same run of a single prepared
** statement, the exact same time is returned for each invocation regardless
** of the amount of time that elapses between invocations.  In other words,
** the time returned is always the time of the first call.
*/
int64_t __stmt_current_time(dbsql_func_t *p){
  int rc;
#ifndef DBSQL_ENABLE_STAT4
  int64_t *pi_time = &p->vdbe->i_current_time;
  assert( p->vdbe!=0 );
#else
  int64_t i_time = 0;
  int64_t *pi_time = p->vdbe!=0 ? &p->vdbe->i_current_time : &i_time;
#endif
  if( *pi_time==0 ){
    rc = __dbsql_os_current_time_int64(p->out->db->vfs, pi_time);
    if( rc ) *pi_time = 0;
  }
  return *pi_time;
}

/*
** Create a new aggregate context for p and return a pointer to
** its mem->z element.
*/
static DBSQL_NOINLINE void *__create_agg_context(dbsql_func_t *p, int n_byte){
  mem_t *mem = p->mem;
  assert( (mem->flags & MEM_Agg)==0 );
  if( n_byte<=0 ){
    __vdbe_mem_set_null(mem);
    mem->z = 0;
  }else{
    __vdbe_mem_clear_and_resize(mem, n_byte);
    mem->flags = MEM_Agg;
    mem->u.def = p->func;
    if( mem->z ){
      memset(mem->z, 0, n_byte);
    }
  }
  return (void*)mem->z;
}

/*
** Allocate or return the aggregate context for a user function.  A new
** context is allocated on the first call.  Subsequent calls return the
** same context that was returned on prior calls.
*/
void *dbsql_aggregate_context(dbsql_func_t *p, int n_byte){
  assert( p && p->func && p->func->finalize );
  assert( dbsql_mutex_held(p->out->db->mutex) );
  testcase( n_byte<0 );
  if( (p->mem->flags & MEM_Agg)==0 ){
    return __create_agg_context(p, n_byte);
  }else{
    return (void*)p->mem->z;
  }
}

/*
** Return the auxiliary data pointer, if any, for the i_arg'th argument to
** the user-function defined by ctx.
**
** The left-most argument is 0.
**
** Undocumented behavior:  If i_arg is negative then access a cache of
** auxiliary data pointers that is available to all functions within a
** single prepared statement.  The i_arg values must match.
*/
void *dbsql_get_auxdata(dbsql_func_t *ctx, int i_arg){
  aux_data_t *aux_data;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return 0;
#endif
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
#if DBSQL_ENABLE_STAT4
  if( ctx->vdbe==0 ) return 0;
#else
  assert( ctx->vdbe!=0 );
#endif
  for(aux_data=ctx->vdbe->aux_data; aux_data; aux_data=aux_data->next_aux){
    if(  aux_data->i_aux_arg==i_arg && (aux_data->i_aux_op==ctx->i_op || i_arg<0) ){
      return aux_data->aux;
    }
  }
  return 0;
}

/*
** Set the auxiliary data pointer and delete function, for the i_arg'th
** argument to the user-function defined by ctx. Any previous value is
** deleted by calling the delete function specified when it was set.
**
** The left-most argument is 0.
**
** Undocumented behavior:  If i_arg is negative then make the data available
** to all functions within the current prepared statement using i_arg as an
** access code.
*/
void dbsql_set_auxdata(
  dbsql_func_t *ctx,
  int i_arg,
  void *aux,
  void (*x_delete)(void*)
){
  aux_data_t *aux_data;
  vdbe_t *vdbe;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( ctx==0 ) return;
#endif
  vdbe= ctx->vdbe;
  assert( dbsql_mutex_held(ctx->out->db->mutex) );
#ifdef DBSQL_ENABLE_STAT4
  if( vdbe==0 ) goto failed;
#else
  assert( vdbe!=0 );
#endif

  for(aux_data=vdbe->aux_data; aux_data; aux_data=aux_data->next_aux){
    if( aux_data->i_aux_arg==i_arg && (aux_data->i_aux_op==ctx->i_op || i_arg<0) ){
      break;
    }
  }
  if( aux_data==0 ){
    aux_data = __db_malloc_zero(vdbe->db, sizeof(aux_data_t));
    if( !aux_data ) goto failed;
    aux_data->i_aux_op = ctx->i_op;
    aux_data->i_aux_arg = i_arg;
    aux_data->next_aux = vdbe->aux_data;
    vdbe->aux_data = aux_data;
    if( ctx->isError==0 ) ctx->isError = -1;
  }else if( aux_data->delete_aux ){
    aux_data->delete_aux(aux_data->aux);
  }

  aux_data->aux = aux;
  aux_data->delete_aux = x_delete;
  return;

failed:
  if( x_delete ){
    x_delete(aux);
  }
}

#ifndef DBSQL_OMIT_DEPRECATED
/*
** Return the number of times the Step function of an aggregate has been
** called.
**
** This function is deprecated.  Do not use it for new code.  It is
** provide only to avoid breaking legacy code.  New aggregate function
** implementations should keep their own counts within their aggregate
** context.
*/
int dbsql_aggregate_count(dbsql_func_t *p){
  assert( p && p->mem && p->func && p->func->finalize );
  return p->mem->n;
}
#endif

/*
** Return the number of columns in the result set for the statement stmt.
*/
int dbsql_column_count(dbsql_stmt_t *stmt){
  vdbe_t *vm = (vdbe_t *)stmt;
  if( vm==0 ) return 0;
  return vm->n_res_column;
}

/*
** Return the number of values available from the current row of the
** currently executing statement stmt.
*/
int dbsql_data_count(dbsql_stmt_t *stmt){
  vdbe_t *vm = (vdbe_t *)stmt;
  if( vm==0 || vm->result_row==0 ) return 0;
  return vm->n_res_column;
}

/*
** Return a pointer to static memory containing an SQL NULL value.
*/
static const mem_t *__column_null_value(void){
  /* Even though the mem_t structure contains an element
  ** of type i64, on certain architectures (x86) with certain compiler
  ** switches (-Os), gcc may align this mem_t object on a 4-byte boundary
  ** instead of an 8-byte one. This all works fine, except that when
  ** running with DBSQL_DEBUG defined the dbsql code sometimes assert()s
  ** that a mem_t structure is located on an 8-byte boundary. To prevent
  ** these assert()s from failing, when building with DBSQL_DEBUG defined
  ** using gcc, we force null_mem to be 8-byte aligned using the magical
  ** __attribute__((aligned(8))) macro.  */
  static const mem_t null_mem
#if defined(DBSQL_DEBUG) && defined(__GNUC__)
    __attribute__((aligned(8)))
#endif
    = {
        /* .u          = */ {0},
        /* .z          = */ (char*)0,
        /* .n          = */ (int)0,
        /* .flags      = */ (u_int16_t)MEM_Null,
        /* .enc        = */ (u_int8_t)0,
        /* .subtype   = */ (u_int8_t)0,
        /* .db         = */ (DBSQL*)0,
        /* .sz_malloc   = */ (int)0,
        /* .u_temp      = */ (u_int32_t)0,
        /* .z_malloc    = */ (char*)0,
        /* .del       = */ (void(*)(void*))0,
#ifdef DBSQL_DEBUG
        /* .scopy_from = */ (mem_t*)0,
        /* .m_scopy_flags= */ 0,
        /* .scopy     = */ 0,
#endif
      };
  return &null_mem;
}

/*
** Check to see if column i_col of the given statement is valid.  If
** it is, return a pointer to the mem_t for the value of that column.
** If i_col is not valid, return a pointer to a mem_t which has a value
** of NULL.
*/
static mem_t *__column_mem(dbsql_stmt_t *stmt, int i){
  vdbe_t *vm;
  mem_t *out;

  vm = (vdbe_t *)stmt;
  if( vm==0 ) return (mem_t*)__column_null_value();
  assert( vm->db );
  dbsql_mutex_enter(vm->db->mutex);
  if( vm->result_row!=0 && i<vm->n_res_column && i>=0 ){
    out = &vm->result_row[i];
  }else{
    __dbsql_error(vm->db, DBSQL_RANGE);
    out = (mem_t*)__column_null_value();
  }
  return out;
}

/*
** This function is called after invoking an dbsql_value_XXX function on a
** column value (i.e. a value returned by evaluating an SQL expression in the
** select list of a SELECT statement) that may cause a malloc() failure. If
** malloc() has failed, the threads malloc_failed flag is cleared and the result
** code of statement stmt set to DBSQL_NOMEM.
**
** Specifically, this is called from within:
**
**     dbsql_column_int()
**     dbsql_column_int64()
**     dbsql_column_text()
**     dbsql_column_text16()
**     dbsql_column_double()
**     dbsql_column_bytes()
**     dbsql_column_bytes16()
**     dbsql_column_blob()
*/
static void __column_malloc_failure(dbsql_stmt_t *stmt)
{
  /* If malloc() failed during an encoding conversion within an
  ** dbsql_column_XXX API, then set the return code of the statement to
  ** DBSQL_NOMEM. The next call to _step() (if any) will return DBSQL_ERROR
  ** and _finalize() will return NOMEM.
  */
  vdbe_t *p = (vdbe_t *)stmt;
  if( p ){
    assert( p->db!=0 );
    assert( dbsql_mutex_held(p->db->mutex) );
    p->rc = __api_exit(p->db, p->rc);
    dbsql_mutex_leave(p->db->mutex);
  }
}

/**************************** dbsql_column_  *******************************
** The following routines are used to access elements of the current row
** in the result set.
*/
const void *dbsql_column_blob(dbsql_stmt_t *stmt, int i){
  const void *val;
  val = dbsql_value_blob( __column_mem(stmt,i) );
  /* Even though there is no encoding conversion, value_blob() might
  ** need to call malloc() to expand the result of a zeroblob()
  ** expression.
  */
  __column_malloc_failure(stmt);
  return val;
}
int dbsql_column_bytes(dbsql_stmt_t *stmt, int i){
  int val = dbsql_value_bytes( __column_mem(stmt,i) );
  __column_malloc_failure(stmt);
  return val;
}
int dbsql_column_bytes16(dbsql_stmt_t *stmt, int i){
  int val = dbsql_value_bytes16( __column_mem(stmt,i) );
  __column_malloc_failure(stmt);
  return val;
}
double dbsql_column_double(dbsql_stmt_t *stmt, int i){
  double val = dbsql_value_double( __column_mem(stmt,i) );
  __column_malloc_failure(stmt);
  return val;
}
int dbsql_column_int(dbsql_stmt_t *stmt, int i){
  int val = dbsql_value_int( __column_mem(stmt,i) );
  __column_malloc_failure(stmt);
  return val;
}
int64_t dbsql_column_int64(dbsql_stmt_t *stmt, int i){
  int64_t val = dbsql_value_int64( __column_mem(stmt,i) );
  __column_malloc_failure(stmt);
  return val;
}
const unsigned char *dbsql_column_text(dbsql_stmt_t *stmt, int i){
  const unsigned char *val = dbsql_value_text( __column_mem(stmt,i) );
  __column_malloc_failure(stmt);
  return val;
}
dbsql_value_t *dbsql_column_value(dbsql_stmt_t *stmt, int i){
  mem_t *out = __column_mem(stmt, i);
  if( out->flags&MEM_Static ){
    out->flags &= ~MEM_Static;
    out->flags |= MEM_Ephem;
  }
  __column_malloc_failure(stmt);
  return (dbsql_value_t *)out;
}
#ifndef DBSQL_OMIT_UTF16
const void *dbsql_column_text16(dbsql_stmt_t *stmt, int i){
  const void *val = dbsql_value_text16( __column_mem(stmt,i) );
  __column_malloc_failure(stmt);
  return val;
}
#endif /* DBSQL_OMIT_UTF16 */
int dbsql_column_type(dbsql_stmt_t *stmt, int i){
  int i_type = dbsql_value_type( __column_mem(stmt,i) );
  __column_malloc_failure(stmt);
  return i_type;
}

/*
** column_t names appropriate for EXPLAIN or EXPLAIN QUERY PLAN.
*/
static const char * const __az_explain_col_names8[] = {
   "addr", "opcode", "p1", "p2", "p3", "p4", "p5", "comment",  /* EXPLAIN */
   "id", "parent", "notused", "detail"                         /* EQP */
};
static const u_int16_t __az_explain_col_names16data[] = {
  /*   0 */  'a', 'd', 'd', 'r',                0,
  /*   5 */  'o', 'p', 'c', 'o', 'd', 'e',      0,
  /*  12 */  'p', '1',                          0, 
  /*  15 */  'p', '2',                          0,
  /*  18 */  'p', '3',                          0,
  /*  21 */  'p', '4',                          0,
  /*  24 */  'p', '5',                          0,
  /*  27 */  'c', 'o', 'm', 'm', 'e', 'n', 't', 0,
  /*  35 */  'i', 'd',                          0,
  /*  38 */  'p', 'a', 'r', 'e', 'n', 't',      0,
  /*  45 */  'n', 'o', 't', 'u', 's', 'e', 'd', 0,
  /*  53 */  'd', 'e', 't', 'a', 'i', 'l',      0
};
static const u_int8_t __i_explain_col_names16[] = {
  0, 5, 12, 15, 18, 21, 24, 27,
  35, 38, 45, 53
};

/*
** Convert the N-th element of stmt->col_name[] into a string using
** func() then return that string.  If N is out of range, return 0.
**
** There are up to 5 names for each column.  use_type determines which
** name is returned.  Here are the names:
**
**    0      The column name as it should be displayed for output
**    1      The datatype name for the column
**    2      The name of the database that the column derives from
**    3      The name of the table that the column derives from
**    4      The name of the table column that the result column derives from
**
** If the result is not a simple column reference (if it is an expression
** or a constant) then use_types 2, 3, and 4 return NULL.
*/
static const void *__column_name(
  dbsql_stmt_t *stmt,     /* The statement */
  int N,                   /* Which column to get the name for */
  int use_utf16,            /* True to return the name as UTF16 */
  int use_type              /* What type of name */
){
  const void *ret;
  vdbe_t *p;
  int n;
  DBSQL *db;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( stmt==0 ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  if( N<0 ) return 0;
  ret = 0;
  p = (vdbe_t *)stmt;
  db = p->db;
  assert( db!=0 );
  dbsql_mutex_enter(db->mutex);

  if( p->explain ){
    if( use_type>0 ) goto column_name_end;
    n = p->explain==1 ? 8 : 4;
    if( N>=n ) goto column_name_end;
    if( use_utf16 ){
      int i = __i_explain_col_names16[N + 8*p->explain - 8];
      ret = (void*)&__az_explain_col_names16data[i];
    }else{
      ret = (void*)__az_explain_col_names8[N + 8*p->explain - 8];
    }
    goto column_name_end;
  }
  n = p->n_res_column;
  if( N<n ){
    u_int8_t prior_malloc_failed = db->malloc_failed;
    N += use_type*n;
#ifndef DBSQL_OMIT_UTF16
    if( use_utf16 ){
      ret = dbsql_value_text16((dbsql_value_t*)&p->col_names[N]);
    }else
#endif
    {
      ret = dbsql_value_text((dbsql_value_t*)&p->col_names[N]);
    }
    /* A malloc may have failed inside of the _text() call. If this
    ** is the case, clear the malloc_failed flag and return NULL.
    */
    assert( db->malloc_failed==0 || db->malloc_failed==1 );
    if( db->malloc_failed > prior_malloc_failed ){
      __oom_clear(db);
      ret = 0;
    }
  }
column_name_end:
  dbsql_mutex_leave(db->mutex);
  return ret;
}

/*
** Return the name of the Nth column of the result set returned by SQL
** statement stmt.
*/
const char *dbsql_column_name(dbsql_stmt_t *stmt, int N){
  return __column_name(stmt, N, 0, COLNAME_NAME);
}
#ifndef DBSQL_OMIT_UTF16
const void *dbsql_column_name16(dbsql_stmt_t *stmt, int N){
  return __column_name(stmt, N, 1, COLNAME_NAME);
}
#endif

/*
** Constraint:  If you have ENABLE_COLUMN_METADATA then you must
** not define OMIT_DECLTYPE.
*/
#if defined(DBSQL_OMIT_DECLTYPE) && defined(DBSQL_ENABLE_COLUMN_METADATA)
# error "Must not define both DBSQL_OMIT_DECLTYPE \
         and DBSQL_ENABLE_COLUMN_METADATA"
#endif

#ifndef DBSQL_OMIT_DECLTYPE
/*
** Return the column declaration type (if applicable) of the 'i'th column
** of the result set of SQL statement stmt.
*/
const char *dbsql_column_decltype(dbsql_stmt_t *stmt, int N){
  return __column_name(stmt, N, 0, COLNAME_DECLTYPE);
}
#ifndef DBSQL_OMIT_UTF16
const void *dbsql_column_decltype16(dbsql_stmt_t *stmt, int N){
  return __column_name(stmt, N, 1, COLNAME_DECLTYPE);
}
#endif /* DBSQL_OMIT_UTF16 */
#endif /* DBSQL_OMIT_DECLTYPE */

#ifdef DBSQL_ENABLE_COLUMN_METADATA
/*
** Return the name of the database from which a result column derives.
** NULL is returned if the result column is an expression or constant or
** anything else which is not an unambiguous reference to a database column.
*/
const char *dbsql_column_database_name(dbsql_stmt_t *stmt, int N){
  return __column_name(stmt, N, 0, COLNAME_DATABASE);
}
#ifndef DBSQL_OMIT_UTF16
const void *dbsql_column_database_name16(dbsql_stmt_t *stmt, int N){
  return __column_name(stmt, N, 1, COLNAME_DATABASE);
}
#endif /* DBSQL_OMIT_UTF16 */

/*
** Return the name of the table from which a result column derives.
** NULL is returned if the result column is an expression or constant or
** anything else which is not an unambiguous reference to a database column.
*/
const char *dbsql_column_table_name(dbsql_stmt_t *stmt, int N){
  return __column_name(stmt, N, 0, COLNAME_TABLE);
}
#ifndef DBSQL_OMIT_UTF16
const void *dbsql_column_table_name16(dbsql_stmt_t *stmt, int N){
  return __column_name(stmt, N, 1, COLNAME_TABLE);
}
#endif /* DBSQL_OMIT_UTF16 */

/*
** Return the name of the table column from which a result column derives.
** NULL is returned if the result column is an expression or constant or
** anything else which is not an unambiguous reference to a database column.
*/
const char *dbsql_column_origin_name(dbsql_stmt_t *stmt, int N){
  return __column_name(stmt, N, 0, COLNAME_COLUMN);
}
#ifndef DBSQL_OMIT_UTF16
const void *dbsql_column_origin_name16(dbsql_stmt_t *stmt, int N){
  return __column_name(stmt, N, 1, COLNAME_COLUMN);
}
#endif /* DBSQL_OMIT_UTF16 */
#endif /* DBSQL_ENABLE_COLUMN_METADATA */


/******************************* dbsql_bind_  ***************************
**
** Routines used to attach values to wildcards in a compiled SQL statement.
*/
/*
** Unbind the value bound to variable i in virtual machine p. This is the
** the same as binding a NULL value to the column. If the "i" parameter is
** out of range, then DBSQL_RANGE is returned. Otherwise DBSQL_OK.
**
** A successful evaluation of this routine acquires the mutex on p.
** the mutex is released if any kind of error occurs.
**
** The error code stored in database p->db is overwritten with the return
** value in any case.
**
** (tag-20240917-01) If  __vdbe_unbind(p,(u32)(i-1))  returns DBSQL_OK,
** that means all of the the following will be true:
**
**     p!=0
**     p->var!=0
**     i>0
**     i<=p->n_var
**
** An assert() is normally added after __vdbe_unbind() to help static analyzers
** realize this.
*/
static int __vdbe_unbind(vdbe_t *p, unsigned int i){
  mem_t *var;
  if( __vdbe_safety_not_null(p) ){
    return DBSQL_MISUSE_BKPT;
  }
  dbsql_mutex_enter(p->db->mutex);
  if( p->vdbe_state!=VDBE_READY_STATE ){
    __dbsql_error(p->db, DBSQL_MISUSE_BKPT);
    dbsql_mutex_leave(p->db->mutex);
    dbsql_log(DBSQL_MISUSE,
        "bind on a busy prepared statement: [%s]", p->z_sql);
    return DBSQL_MISUSE_BKPT;
  }
  if( i>=(unsigned int)p->n_var ){
    __dbsql_error(p->db, DBSQL_RANGE);
    dbsql_mutex_leave(p->db->mutex);
    return DBSQL_RANGE;
  }
  var = &p->vars[i];
  __vdbe_mem_release(var);
  var->flags = MEM_Null;
  p->db->errCode = DBSQL_OK;

  /* If the bit corresponding to this variable in vdbe_t.expmask is set, then
  ** binding a new value to this variable invalidates the current query plan.
  **
  ** IMPLEMENTATION-OF: R-57496-20354 If the specific value bound to a host
  ** parameter in the WHERE clause might influence the choice of query plan
  ** for a statement, then the statement will be automatically recompiled,
  ** as if there had been a schema change, on the first dbsql_step() call
  ** following any change to the bindings of that parameter.
  */
  assert( (p->prep_flags & DBSQL_PREPARE_SAVESQL)!=0 || p->expmask==0 );
  if( p->expmask!=0 && (p->expmask & (i>=31 ? 0x80000000 : (u_int32_t)1<<i))!=0 ){
    p->expired = 1;
  }
  return DBSQL_OK;
}

/*
** Bind a text or BLOB value.
*/
static int __bind_text(
  dbsql_stmt_t *stmt,   /* The statement to bind against */
  int i,                 /* index_t of the parameter to bind */
  const void *data,     /* Pointer to the data to be bound */
  int64_t n_data,             /* Number of bytes of data to be bound */
  void (*del)(void*),   /* Destructor for the data */
  u_int8_t encoding            /* Encoding for the data */
){
  vdbe_t *p = (vdbe_t *)stmt;
  mem_t *var;
  int rc;

  rc = __vdbe_unbind(p, (u_int32_t)(i-1));
  if( rc==DBSQL_OK ){
    assert( p!=0 && p->vars!=0 && i>0 && i<=p->n_var ); /* tag-20240917-01 */
    if( data!=0 ){
      var = &p->vars[i-1];
      if( encoding==DBSQL_UTF8 ){
        rc = __vdbe_mem_set_text(var, data, n_data, del);
      }else if( encoding==DBSQL_UTF8_ZT ){
        /* It is usually consider improper to assert() on an input.
        ** However, the following assert() is checking for inputs
        ** that are documented to result in undefined behavior. */
        assert( data==0
             || n_data<0 
             || n_data>var->db->limits[DBSQL_LIMIT_LENGTH]
             || ((u_int8_t*)data)[n_data]==0
        );
        rc = __vdbe_mem_set_text(var, data, n_data, del);
        var->flags |= MEM_Term;
      }else{
        rc = __vdbe_mem_set_str(var, data, n_data, encoding, del);
        if( encoding==0 ) var->enc = ENC(p->db);
      }
      if( rc==DBSQL_OK && encoding!=0 ){
        rc = __vdbe_change_encoding(var, ENC(p->db));
      }
      if( rc ){
        __dbsql_error(p->db, rc);
        rc = __api_exit(p->db, rc);
      }
    }
    dbsql_mutex_leave(p->db->mutex);
  }else if( del!=DBSQL_STATIC && del!=DBSQL_TRANSIENT ){
    del((void*)data);
  }
  return rc;
}


/*
** Bind a blob value to an SQL statement variable.
*/
int dbsql_bind_blob(
  dbsql_stmt_t *stmt,
  int i,
  const void *data,
  int n_data,
  void (*del)(void*)
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( n_data<0 ) return DBSQL_MISUSE_BKPT;
#endif
  return __bind_text(stmt, i, data, n_data, del, 0);
}
int dbsql_bind_blob64(
  dbsql_stmt_t *stmt,
  int i,
  const void *data,
  u_int64_t n_data,
  void (*del)(void*)
){
  assert( del!=DBSQL_DYNAMIC );
  return __bind_text(stmt, i, data, n_data, del, 0);
}
int dbsql_bind_double(dbsql_stmt_t *stmt, int i, double r_value){
  int rc;
  vdbe_t *p = (vdbe_t *)stmt;
  rc = __vdbe_unbind(p, (u_int32_t)(i-1));
  if( rc==DBSQL_OK ){
    assert( p!=0 && p->vars!=0 && i>0 && i<=p->n_var ); /* tag-20240917-01 */
    __vdbe_mem_set_double(&p->vars[i-1], r_value);
    dbsql_mutex_leave(p->db->mutex);
  }
  return rc;
}
int dbsql_bind_int(dbsql_stmt_t *p, int i, int i_value){
  return dbsql_bind_int64(p, i, (int64_t)i_value);
}
int dbsql_bind_int64(dbsql_stmt_t *stmt, int i, int64_t i_value){
  int rc;
  vdbe_t *p = (vdbe_t *)stmt;
  rc = __vdbe_unbind(p, (u_int32_t)(i-1));
  if( rc==DBSQL_OK ){
    assert( p!=0 && p->vars!=0 && i>0 && i<=p->n_var ); /* tag-20240917-01 */
    __vdbe_mem_set_int64(&p->vars[i-1], i_value);
    dbsql_mutex_leave(p->db->mutex);
  }
  return rc;
}
int dbsql_bind_null(dbsql_stmt_t *stmt, int i){
  int rc;
  vdbe_t *p = (vdbe_t*)stmt;
  rc = __vdbe_unbind(p, (u_int32_t)(i-1));
  if( rc==DBSQL_OK ){
    assert( p!=0 && p->vars!=0 && i>0 && i<=p->n_var ); /* tag-20240917-01 */
    dbsql_mutex_leave(p->db->mutex);
  }
  return rc;
}
int dbsql_bind_pointer(
  dbsql_stmt_t *stmt,
  int i,
  void *ptr,
  const char *p_ttype,
  void (*destructor)(void*)
){
  int rc;
  vdbe_t *p = (vdbe_t*)stmt;
  rc = __vdbe_unbind(p, (u_int32_t)(i-1));
  if( rc==DBSQL_OK ){
    assert( p!=0 && p->vars!=0 && i>0 && i<=p->n_var ); /* tag-20240917-01 */
    __vdbe_mem_set_pointer(&p->vars[i-1], ptr, p_ttype, destructor);
    dbsql_mutex_leave(p->db->mutex);
  }else if( destructor ){
    destructor(ptr);
  }
  return rc;
}
int dbsql_bind_text(
  dbsql_stmt_t *stmt,
  int i,
  const char *data,
  int n_data,
  void (*del)(void*)
){
  return __bind_text(stmt, i, data, n_data, del, DBSQL_UTF8);
}
int dbsql_bind_text64(
  dbsql_stmt_t *stmt,
  int i,
  const char *data,
  u_int64_t n_data,
  void (*del)(void*),
  unsigned char enc
){
  assert( del!=DBSQL_DYNAMIC );
  if( enc!=DBSQL_UTF8 && enc!=DBSQL_UTF8_ZT ){
    if( enc==DBSQL_UTF16 ) enc = DBSQL_UTF16NATIVE;
    n_data &= ~(u_int64_t)1;
  }
  return __bind_text(stmt, i, data, n_data, del, enc);
}
#ifndef DBSQL_OMIT_UTF16
int dbsql_bind_text16(
  dbsql_stmt_t *stmt,
  int i,
  const void *data,
  int n,
  void (*del)(void*)
){
  return __bind_text(stmt, i, data, n & ~(u_int64_t)1, del, DBSQL_UTF16NATIVE);
}
#endif /* DBSQL_OMIT_UTF16 */
int dbsql_bind_value(dbsql_stmt_t *stmt, int i, const dbsql_value_t *value){
  int rc;
  switch( dbsql_value_type((dbsql_value_t*)value) ){
    case DBSQL_INTEGER: {
      rc = dbsql_bind_int64(stmt, i, value->u.i);
      break;
    }
    case DBSQL_FLOAT: {
      assert( value->flags & (MEM_Real|MEM_IntReal) );
      rc = dbsql_bind_double(stmt, i,
          (value->flags & MEM_Real) ? value->u.r : (double)value->u.i
      );
      break;
    }
    case DBSQL_BLOB: {
      if( value->flags & MEM_Zero ){
        rc = dbsql_bind_zeroblob(stmt, i, value->u.n_zero);
      }else{
        rc = dbsql_bind_blob(stmt, i, value->z, value->n,DBSQL_TRANSIENT);
      }
      break;
    }
    case DBSQL_TEXT: {
      rc = __bind_text(stmt,i,  value->z, value->n, DBSQL_TRANSIENT,
                              value->enc);
      break;
    }
    default: {
      rc = dbsql_bind_null(stmt, i);
      break;
    }
  }
  return rc;
}
int dbsql_bind_zeroblob(dbsql_stmt_t *stmt, int i, int n){
  int rc;
  vdbe_t *p = (vdbe_t *)stmt;
  rc = __vdbe_unbind(p, (u_int32_t)(i-1));
  if( rc==DBSQL_OK ){
    assert( p!=0 && p->vars!=0 && i>0 && i<=p->n_var ); /* tag-20240917-01 */
#ifndef DBSQL_OMIT_INCRBLOB
    __vdbe_mem_set_zero_blob(&p->vars[i-1], n);
#else
    rc = __vdbe_mem_set_zero_blob(&p->vars[i-1], n);
#endif
    dbsql_mutex_leave(p->db->mutex);
  }
  return rc;
}
int dbsql_bind_zeroblob64(dbsql_stmt_t *stmt, int i, u_int64_t n){
  int rc;
  vdbe_t *p = (vdbe_t *)stmt;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( p==0 ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(p->db->mutex);
  if( n>(u_int64_t)p->db->limits[DBSQL_LIMIT_LENGTH] ){
    rc = DBSQL_TOOBIG;
  }else{
    assert( (n & 0x7FFFFFFF)==n );
    rc = dbsql_bind_zeroblob(stmt, i, n);
  }
  rc = __api_exit(p->db, rc);
  dbsql_mutex_leave(p->db->mutex);
  return rc;
}

/*
** Return the number of wildcards that can be potentially bound to.
** This routine is added to support DBD::dbsql. 
*/
int dbsql_bind_parameter_count(dbsql_stmt_t *stmt){
  vdbe_t *p = (vdbe_t*)stmt;
  return p ? p->n_var : 0;
}

/*
** Return the name of a wildcard parameter.  Return NULL if the index
** is out of range or if the wildcard is unnamed.
**
** The result is always UTF-8.
*/
const char *dbsql_bind_parameter_name(dbsql_stmt_t *stmt, int i){
  vdbe_t *p = (vdbe_t*)stmt;
  if( p==0 ) return 0;
  return __v_list_num_to_name(p->v_list, i);
}

/*
** Given a wildcard parameter name, return the index of the variable
** with that name.  If there is no variable with the given name,
** return 0.
*/
int __vdbe_parameter_index(vdbe_t *p, const char *name, int n_name){
  if( p==0 || name==0 ) return 0;
  return __v_list_name_to_num(p->v_list, name, n_name);
}
int dbsql_bind_parameter_index(dbsql_stmt_t *stmt, const char *name){
  return __vdbe_parameter_index((vdbe_t*)stmt, name, __strlen30(name));
}

/*
** Transfer all bindings from the first statement over to the second.
*/
int __transfer_bindings(dbsql_stmt_t *from_stmt, dbsql_stmt_t *to_stmt){
  vdbe_t *from = (vdbe_t*)from_stmt;
  vdbe_t *to = (vdbe_t*)to_stmt;
  int i;
  assert( to->db==from->db );
  assert( to->n_var==from->n_var );
  dbsql_mutex_enter(to->db->mutex);
  for(i=0; i<from->n_var; i++){
    __vdbe_mem_move(&to->vars[i], &from->vars[i]);
  }
  dbsql_mutex_leave(to->db->mutex);
  return DBSQL_OK;
}

#ifndef DBSQL_OMIT_DEPRECATED
/*
** Deprecated external interface.  Internal/core dbsql code
** should call __transfer_bindings.
**
** It is misuse to call this routine with statements from different
** database connections.  But as this is a deprecated interface, we
** will not bother to check for that condition.
**
** If the two statements contain a different number of bindings, then
** an DBSQL_ERROR is returned.  Nothing else can go wrong, so otherwise
** DBSQL_OK is returned.
*/
int dbsql_transfer_bindings(dbsql_stmt_t *from_stmt, dbsql_stmt_t *to_stmt){
  vdbe_t *from = (vdbe_t*)from_stmt;
  vdbe_t *to = (vdbe_t*)to_stmt;
  if( from->n_var!=to->n_var ){
    return DBSQL_ERROR;
  }
  assert( (to->prep_flags & DBSQL_PREPARE_SAVESQL)!=0 || to->expmask==0 );
  if( to->expmask ){
    to->expired = 1;
  }
  assert( (from->prep_flags & DBSQL_PREPARE_SAVESQL)!=0 || from->expmask==0 );
  if( from->expmask ){
    from->expired = 1;
  }
  return __transfer_bindings(from_stmt, to_stmt);
}
#endif

/*
** Return the DBSQL* database handle to which the prepared statement given
** in the argument belongs.  This is the same database handle that was
** the first argument to the dbsql_prepare() that was used to create
** the statement in the first place.
*/
DBSQL *dbsql_db_handle(dbsql_stmt_t *stmt){
  return stmt ? ((vdbe_t*)stmt)->db : 0;
}

/*
** Return true if the prepared statement is guaranteed to not modify the
** database.
*/
int dbsql_stmt_readonly(dbsql_stmt_t *stmt){
  return stmt ? ((vdbe_t*)stmt)->readOnly : 1;
}

/*
** Return 1 if the statement is an EXPLAIN and return 2 if the
** statement is an EXPLAIN QUERY PLAN
*/
int dbsql_stmt_isexplain(dbsql_stmt_t *stmt){
  return stmt ? ((vdbe_t*)stmt)->explain : 0;
}

/*
** Set the explain mode for a statement.
*/
int dbsql_stmt_explain(dbsql_stmt_t *stmt, int mode){
  vdbe_t *v = (vdbe_t*)stmt;
  int rc;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( stmt==0 ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(v->db->mutex);
  if( ((int)v->explain)==mode ){
    rc = DBSQL_OK;
  }else if( mode<0 || mode>2 ){
    rc = DBSQL_ERROR;
  }else if( (v->prep_flags & DBSQL_PREPARE_SAVESQL)==0 ){
    rc = DBSQL_ERROR;
  }else if( v->vdbe_state!=VDBE_READY_STATE ){
    rc = DBSQL_BUSY;
  }else if( v->n_mem>=10 && (mode!=2 || v->have_eqp_ops) ){
    /* No reprepare necessary */
    v->explain = mode;
    rc = DBSQL_OK;
  }else{
    v->explain = mode;
    rc = __reprepare(v);
    v->have_eqp_ops = mode==2;
  }
  if( v->explain ){
    v->n_res_column = 12 - 4*v->explain;
  }else{
    v->n_res_column = v->n_res_alloc;
  }
  dbsql_mutex_leave(v->db->mutex);
  return rc;
}

/*
** Return true if the prepared statement is in need of being reset.
*/
int dbsql_stmt_busy(dbsql_stmt_t *stmt){
  vdbe_t *v = (vdbe_t*)stmt;
  return v!=0 && v->vdbe_state==VDBE_RUN_STATE;
}

/*
** Return a pointer to the next prepared statement after stmt associated
** with database connection db.  If stmt is NULL, return the first
** prepared statement for the database connection.  Return NULL if there
** are no more.
*/
dbsql_stmt_t *dbsql_next_stmt(DBSQL *db, dbsql_stmt_t *stmt){
  dbsql_stmt_t *next;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  if( stmt==0 ){
    next = (dbsql_stmt_t*)db->vdbe;
  }else{
    next = (dbsql_stmt_t*)((vdbe_t*)stmt)->v_next;
  }
  dbsql_mutex_leave(db->mutex);
  return next;
}

/*
** Return the value of a status counter for a prepared statement
*/
int dbsql_stmt_status(dbsql_stmt_t *stmt, int op, int reset_flag){
  vdbe_t *vdbe = (vdbe_t*)stmt;
  u_int32_t v;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !stmt
   || (op!=DBSQL_STMTSTATUS_MEMUSED && (op<0||op>=ARRAY_SIZE(vdbe->counters)))
  ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  if( op==DBSQL_STMTSTATUS_MEMUSED ){
    DBSQL *db = vdbe->db;
    dbsql_mutex_enter(db->mutex);
    v = 0;
    db->pn_bytes_freed = (int*)&v;
    assert( db->lookaside.p_end==db->lookaside.true_end );
    db->lookaside.p_end = db->lookaside.p_start;
    __vdbe_delete(vdbe);
    db->pn_bytes_freed = 0;
    db->lookaside.p_end = db->lookaside.true_end;
    dbsql_mutex_leave(db->mutex);
  }else{
    v = vdbe->counters[op];
    if( reset_flag ) vdbe->counters[op] = 0;
  }
  return (int)v;
}

/*
** Return the SQL associated with a prepared statement
*/
const char *dbsql_sql(dbsql_stmt_t *stmt){
  vdbe_t *p = (vdbe_t *)stmt;
  return p ? p->z_sql : 0;
}

/*
** Return the SQL associated with a prepared statement with
** bound parameters expanded.  Space to hold the returned string is
** obtained from dbsql_malloc().  The caller is responsible for
** freeing the returned string by passing it to dbsql_free().
**
** The DBSQL_TRACE_SIZE_LIMIT puts an upper bound on the size of
** expanded bound parameters.
*/
char *dbsql_expanded_sql(dbsql_stmt_t *stmt){
#ifdef DBSQL_OMIT_TRACE
  return 0;
#else
  char *z = 0;
  const char *sql = dbsql_sql(stmt);
  if( sql ){
    vdbe_t *p = (vdbe_t *)stmt;
    dbsql_mutex_enter(p->db->mutex);
    z = __vdbe_expand_sql(p, sql);
    dbsql_mutex_leave(p->db->mutex);
  }
  return z;
#endif
}

#ifdef DBSQL_ENABLE_NORMALIZE
/*
** Return the normalized SQL associated with a prepared statement.
*/
const char *dbsql_normalized_sql(dbsql_stmt_t *stmt){
  vdbe_t *p = (vdbe_t *)stmt;
  if( p==0 ) return 0;
  if( p->norm_sql==0 && ALWAYS(p->z_sql!=0) ){
    dbsql_mutex_enter(p->db->mutex);
    p->norm_sql = __normalize(p, p->z_sql);
    dbsql_mutex_leave(p->db->mutex);
  }
  return p->norm_sql;
}
#endif /* DBSQL_ENABLE_NORMALIZE */

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
/*
** Allocate and populate an unpacked_record_t structure based on the serialized
** record in n_key/key. Return a pointer to the new unpacked_record_t structure
** if successful, or a NULL pointer if an OOM error is encountered.
*/
static unpacked_record_t *__vdbe_unpack_record(
  key_info_t *key_info,
  int n_key,
  const void *key
){
  unpacked_record_t *ret;           /* Return value */

  ret = __vdbe_alloc_unpacked_record(key_info);
  if( ret ){
    memset(ret->mems, 0, sizeof(mem_t)*(key_info->n_key_field+1));
    __vdbe_record_unpack(n_key, key, ret);
  }
  return ret;
}

/*
** This function is called from within a pre-update callback to retrieve
** a field of the row currently being updated or deleted.
*/
int dbsql_preupdate_old(DBSQL *db, int i_idx, dbsql_value_t **value){
  pre_update_t *p;
  mem_t *mem;
  int rc = DBSQL_OK;
  int i_store = 0;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( db==0 || value==0 ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  p = db->pre_update;
  /* Test that this call is being made from within an DBSQL_DELETE or
  ** DBSQL_UPDATE pre-update callback, and that i_idx is within range. */
  if( !p || p->op==DBSQL_INSERT ){
    rc = DBSQL_MISUSE_BKPT;
    goto preupdate_old_out;
  }
  if( p->pk ){
    i_store = __table_column_to_index(p->pk, i_idx);
  }else if( i_idx >= p->tab->n_col ){
    rc = DBSQL_MISUSE_BKPT;
    goto preupdate_old_out;
  }else{
    i_store = __table_column_to_storage(p->tab, i_idx);
  }
  if( i_store>=p->csr->n_field || i_store<0 ){
    rc = DBSQL_RANGE;
    goto preupdate_old_out;
  }

  if( i_idx==p->tab->i_p_key ){
    *value = mem = &p->oldipk;
    __vdbe_mem_set_int64(mem, p->i_key1);
  }else{

    /* If the old.* record has not yet been loaded into memory, do so now. */
    if( p->unpacked==0 ){
      u_int32_t n_rec;
      u_int8_t *recs;

      assert( p->csr->cur_type==CURTYPE_BTREE );
      n_rec = __sm_payload_size(p->csr->uc.cursor);
      recs = __db_malloc_raw(db, n_rec);
      if( !recs ) goto preupdate_old_out;
      rc = __sm_payload(p->csr->uc.cursor, 0, n_rec, recs);
      if( rc==DBSQL_OK ){
        p->unpacked = __vdbe_unpack_record(p->keyinfo, n_rec, recs);
        if( !p->unpacked ) rc = DBSQL_NOMEM;
      }
      if( rc!=DBSQL_OK ){
        __dbsql_db_free(db, recs);
        goto preupdate_old_out;
      }
      p->records = recs;
    }

    mem = *value = &p->unpacked->mems[i_store];
    if( i_store>=p->unpacked->n_field ){
      /* This occurs when the table has been extended using ALTER TABLE
      ** ADD COLUMN. The value to return is the default value of the column. */
      column_t *col = &p->tab->cols[i_idx];
      if( col->i_dflt>0 ){
        if( p->dflts==0 ){
          int n_byte;
          assert( sizeof(dbsql_value_t*)*UMXV(p->tab->n_col) < 0x7fffffff );
          n_byte = sizeof(dbsql_value_t*)*p->tab->n_col;
          p->dflts = (dbsql_value_t**)__db_malloc_zero(db, n_byte);
          if( p->dflts==0 ) goto preupdate_old_out;
        }
        if( p->dflts[i_idx]==0 ){
          dbsql_value_t *val = 0;
          expr_t *dflt;
          assert( p->tab!=0 && IS_ORDINARY_TABLE(p->tab) );
          dflt = p->tab->u.tab.dflt_list->a[col->i_dflt-1].expr;
          rc = __value_from_expr(db, dflt, ENC(db), col->affinity, &val);
          if( rc==DBSQL_OK && val==0 ){
            rc = DBSQL_CORRUPT_BKPT;
          }
          p->dflts[i_idx] = val;
        }
        *value = p->dflts[i_idx];
      }else{
        *value = (dbsql_value_t *)__column_null_value();
      }
    }else if( p->tab->cols[i_idx].affinity==DBSQL_AFF_REAL ){
      if( mem->flags & (MEM_Int|MEM_IntReal) ){
        testcase( mem->flags & MEM_Int );
        testcase( mem->flags & MEM_IntReal );
        __vdbe_mem_realify(mem);
      }
    }
  }

 preupdate_old_out:
  __dbsql_error(db, rc);
  return __api_exit(db, rc);
}
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
/*
** This function is called from within a pre-update callback to retrieve
** the number of columns in the row being updated, deleted or inserted.
*/
int dbsql_preupdate_count(DBSQL *db){
  pre_update_t *p;
#ifdef DBSQL_ENABLE_API_ARMOR
  p = db!=0 ? db->pre_update : 0;
#else
  p = db->pre_update;
#endif
  return (p ? p->keyinfo->n_key_field : 0);
}
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
/*
** This function is designed to be called from within a pre-update callback
** only. It returns zero if the change that caused the callback was made
** immediately by a user SQL statement. Or, if the change was made by a
** trigger program, it returns the number of trigger programs currently
** on the stack (1 for a top-level trigger, 2 for a trigger fired by a
** top-level trigger etc.).
**
** For the purposes of the previous paragraph, a foreign key CASCADE, SET NULL
** or SET DEFAULT action is considered a trigger.
*/
int dbsql_preupdate_depth(DBSQL *db){
  pre_update_t *p;
#ifdef DBSQL_ENABLE_API_ARMOR
  p = db!=0 ? db->pre_update : 0;
#else
  p = db->pre_update;
#endif
  return (p ? p->v->n_frame : 0);
}
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
/*
** This function is designed to be called from within a pre-update callback
** only.
*/
int dbsql_preupdate_blobwrite(DBSQL *db){
  pre_update_t *p;
#ifdef DBSQL_ENABLE_API_ARMOR
  p = db!=0 ? db->pre_update : 0;
#else
  p = db->pre_update;
#endif
  return (p ? p->i_blob_write : -1);
}
#endif

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
/*
** This function is called from within a pre-update callback to retrieve
** a field of the row currently being updated or inserted.
*/
int dbsql_preupdate_new(DBSQL *db, int i_idx, dbsql_value_t **value){
  pre_update_t *p;
  int rc = DBSQL_OK;
  mem_t *mem;
  int i_store = 0;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( db==0 || value==0 ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  p = db->pre_update;
  if( !p || p->op==DBSQL_DELETE ){
    rc = DBSQL_MISUSE_BKPT;
    goto preupdate_new_out;
  }
  if( p->pk && p->op!=DBSQL_UPDATE ){
    i_store = __table_column_to_index(p->pk, i_idx);
  }else if( i_idx >= p->tab->n_col ){
    return DBSQL_MISUSE_BKPT;
  }else{
    i_store = __table_column_to_storage(p->tab, i_idx);
  }

  if( i_store>=p->csr->n_field || i_store<0 ){
    rc = DBSQL_RANGE;
    goto preupdate_new_out;
  }

  if( p->op==DBSQL_INSERT ){
    /* For an INSERT, memory cell p->i_new_reg contains the serialized record
    ** that is being inserted. Deserialize it. */
    unpacked_record_t *unpack = p->new_unpacked;
    if( !unpack ){
      mem_t *data = &p->v->mems[p->i_new_reg];
      rc = EXPAND_BLOB(data);
      if( rc!=DBSQL_OK ) goto preupdate_new_out;
      unpack = __vdbe_unpack_record(p->keyinfo, data->n, data->z);
      if( !unpack ){
        rc = DBSQL_NOMEM;
        goto preupdate_new_out;
      }
      p->new_unpacked = unpack;
    }
    mem = &unpack->mems[i_store];
    if( i_idx==p->tab->i_p_key ){
      __vdbe_mem_set_int64(mem, p->i_key2);
    }else if( i_store>=unpack->n_field ){
      mem = (dbsql_value_t *)__column_null_value();
    }
  }else{
    /* For an UPDATE, memory cell (p->i_new_reg+1+i_store) contains the required
    ** value. Make a copy of the cell contents and return a pointer to it.
    ** It is not safe to return a pointer to the memory cell itself as the
    ** caller may modify the value text encoding.
    */
    assert( p->op==DBSQL_UPDATE );
    if( !p->news ){
      assert( sizeof(mem_t)*UMXV(p->csr->n_field) < 0x7fffffff );
      p->news = (mem_t *)__db_malloc_zero(db, sizeof(mem_t)*p->csr->n_field);
      if( !p->news ){
        rc = DBSQL_NOMEM;
        goto preupdate_new_out;
      }
    }
    assert( i_store>=0 && i_store<p->csr->n_field );
    mem = &p->news[i_store];
    if( mem->flags==0 ){
      if( i_idx==p->tab->i_p_key ){
        __vdbe_mem_set_int64(mem, p->i_key2);
      }else{
        rc = __vdbe_mem_copy(mem, &p->v->mems[p->i_new_reg+1+i_store]);
        if( rc!=DBSQL_OK ) goto preupdate_new_out;
      }
    }
  }
  *value = mem;

 preupdate_new_out:
  __dbsql_error(db, rc);
  return __api_exit(db, rc);
}
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */

#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
/*
** Return status data for a single loop within query stmt.
*/
int dbsql_stmt_scanstatus_v2(
  dbsql_stmt_t *stmt,            /* Prepared statement being queried */
  int i_scan,                      /* index_t of loop to report on */
  int i_scan_status_op,              /* Which metric to return */
  int flags,
  void *out                      /* OUT: Write the answer here */
){
  vdbe_t *p = (vdbe_t*)stmt;
  vdbe_op_t *ops;
  int n_op;
  scan_status_t *scan = 0;
  int idx;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( p==0 || out==0
      || i_scan_status_op<DBSQL_SCANSTAT_NLOOP
      || i_scan_status_op>DBSQL_SCANSTAT_NCYCLE ){
    return 1;
  }
#endif
  ops = p->ops;
  n_op = p->n_op;
  if( p->frame ){
    vdbe_frame_t *frame;
    for(frame=p->frame; frame->parent; frame=frame->parent);
    ops = frame->ops;
    n_op = frame->n_op;
  }

  if( i_scan<0 ){
    int ii;
    if( i_scan_status_op==DBSQL_SCANSTAT_NCYCLE ){
      int64_t res = 0;
      for(ii=0; ii<n_op; ii++){
        res += ops[ii].n_cycle;
      }
      *(int64_t*)out = res;
      return 0;
    }
    return 1;
  }
  if( flags & DBSQL_SCANSTAT_COMPLEX ){
    idx = i_scan;
  }else{
    /* If the COMPLEX flag is clear, then this function must ignore any
    ** scan_status_t structures with scan_status_t.addr_loop set to 0. */
    for(idx=0; idx<p->n_scan; idx++){
      scan = &p->scans[idx];
      if( scan->name ){
        i_scan--;
        if( i_scan<0 ) break;
      }
    }
  }
  if( idx>=p->n_scan ) return 1;
  assert( scan==0 || scan==&p->scans[idx] );
  scan = &p->scans[idx];

  switch( i_scan_status_op ){
    case DBSQL_SCANSTAT_NLOOP: {
      if( scan->addr_loop>0 ){
        *(int64_t*)out = ops[scan->addr_loop].n_exec;
      }else{
        *(int64_t*)out = -1;
      }
      break;
    }
    case DBSQL_SCANSTAT_NVISIT: {
      if( scan->addr_visit>0 ){
        *(int64_t*)out = ops[scan->addr_visit].n_exec;
      }else{
        *(int64_t*)out = -1;
      }
      break;
    }
    case DBSQL_SCANSTAT_EST: {
      double r = 1.0;
      log_est_t x = scan->n_est;
      while( x<100 ){
        x += 10;
        r *= 0.5;
      }
      *(double*)out = r*__log_est_to_int(x);
      break;
    }
    case DBSQL_SCANSTAT_NAME: {
      *(const char**)out = scan->name;
      break;
    }
    case DBSQL_SCANSTAT_EXPLAIN: {
      if( scan->addr_explain ){
        *(const char**)out = ops[ scan->addr_explain ].p4.z;
      }else{
        *(const char**)out = 0;
      }
      break;
    }
    case DBSQL_SCANSTAT_SELECTID: {
      if( scan->addr_explain ){
        *(int*)out = ops[ scan->addr_explain ].p1;
      }else{
        *(int*)out = -1;
      }
      break;
    }
    case DBSQL_SCANSTAT_PARENTID: {
      if( scan->addr_explain ){
        *(int*)out = ops[ scan->addr_explain ].p2;
      }else{
        *(int*)out = -1;
      }
      break;
    }
    case DBSQL_SCANSTAT_NCYCLE: {
      int64_t res = 0;
      if( scan->addr_ranges[0]==0 ){
        res = -1;
      }else{
        int ii;
        for(ii=0; ii<ARRAY_SIZE(scan->addr_ranges); ii+=2){
          int i_ins = scan->addr_ranges[ii];
          int i_end = scan->addr_ranges[ii+1];
          if( i_ins==0 ) break;
          if( i_ins>0 ){
            while( i_ins<=i_end ){
              res += ops[i_ins].n_cycle;
              i_ins++;
            }
          }else{
            int i_op;
            for(i_op=0; i_op<n_op; i_op++){
              op_t *op = &ops[i_op];
              if( op->p1!=i_end ) continue;
              if( (__opcode_property[op->opcode] & OPFLG_NCYCLE)==0 ){
                continue;
              }
              res += ops[i_op].n_cycle;
            }
          }
        }
      }
      *(int64_t*)out = res;
      break;
    }
    default: {
      return 1;
    }
  }
  return 0;
}

/*
** Return status data for a single loop within query stmt.
*/
int dbsql_stmt_scanstatus(
  dbsql_stmt_t *stmt,            /* Prepared statement being queried */
  int i_scan,                      /* index_t of loop to report on */
  int i_scan_status_op,              /* Which metric to return */
  void *out                      /* OUT: Write the answer here */
){
  return dbsql_stmt_scanstatus_v2(stmt, i_scan, i_scan_status_op, 0, out);
}

/*
** Zero all counters associated with the dbsql_stmt_scanstatus() data.
*/
void dbsql_stmt_scanstatus_reset(dbsql_stmt_t *stmt){
  vdbe_t *p = (vdbe_t*)stmt;
  int ii;
  for(ii=0; p!=0 && ii<p->n_op; ii++){
    op_t *op = &p->ops[ii];
    op->n_exec = 0;
    op->n_cycle = 0;
  }
}
#endif /* DBSQL_ENABLE_STMT_SCANSTATUS */
