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
** Utility functions used throughout sqlite.
**
** This file contains functions for allocating memory, comparing
** strings, and stuff like that.
**
*/
#include "dbsql_int.h"
#include <stdarg.h>
#ifndef DBSQL_OMIT_FLOATING_POINT
#include <math.h>
#endif

/*
** Calls to __fault_sim() are used to simulate a failure during testing,
** or to bypass normal error detection during testing in order to let
** execute proceed further downstream.
**
** In deployment, __fault_sim() *always* return DBSQL_OK (0).  The
** __fault_sim() function only returns non-zero during testing.
**
** During testing, if the test harness has set a fault-sim callback using
** a call to dbsql_test_control(DBSQL_TESTCTRL_FAULT_INSTALL), then
** each call to __fault_sim() is relayed to that application-supplied
** callback and the integer return value form the application-supplied
** callback is returned by __fault_sim().
**
** The integer argument to __fault_sim() is a code to identify which
** __fault_sim() instance is being invoked. Each call to __fault_sim()
** should have a unique code.  To prevent legacy testing applications from
** breaking, the codes should not be changed or reused.
*/
#ifndef DBSQL_UNTESTABLE
int __fault_sim(int i_test){
  int (*callback)(int) = __global_config.test_callback;
  return callback ? callback(i_test) : DBSQL_OK;
}
#endif

#ifndef DBSQL_OMIT_FLOATING_POINT
/*
** Return true if the floating point value is Not a Number (__na_n).
**
** Use the math library isnan() function if compiled with DBSQL_HAVE_ISNAN.
** Otherwise, we have our own implementation that works on most systems.
*/
int __is_na_n(double x){
  int rc;   /* The value return */
#if !DBSQL_HAVE_ISNAN && !HAVE_ISNAN
  u_int64_t y;
  memcpy(&y,&x,sizeof(y));
  rc = IS_NA_N(y);
#else
  rc = isnan(x);
#endif /* HAVE_ISNAN */
  testcase( rc );
  return rc;
}
#endif /* DBSQL_OMIT_FLOATING_POINT */

#ifndef DBSQL_OMIT_FLOATING_POINT
/*
** Return true if the floating point value is __na_n or +Inf or -Inf.
*/
int __is_overflow(double x){
  int rc;   /* The value return */
  u_int64_t y;
  memcpy(&y,&x,sizeof(y));
  rc = IS_OVFL(y);
  return rc;
}
#endif /* DBSQL_OMIT_FLOATING_POINT */

/*
** Compute a string length that is limited to what can be stored in
** lower 30 bits of a 32-bit signed integer.
**
** The value returned will never be negative.  Nor will it ever be greater
** than the actual length of the string.  For very long strings (greater
** than 1GiB) the value returned might be less than the true string length.
*/
int __strlen30(const char *z){
  if( z==0 ) return 0;
  return 0x3fffffff & (int)strlen(z);
}

/*
** Return the declared type of a column.  Or return dflt if the column
** has no declared type.
**
** The column type is an extra string stored after the zero-terminator on
** the column name if and only if the COLFLAG_HASTYPE flag is set.
*/
char *__column_type(column_t *col, char *dflt){
  if( col->col_flags & COLFLAG_HASTYPE ){
    return col->cn_name + strlen(col->cn_name) + 1;
  }else if( col->c_type ){
    assert( col->c_type<=DBSQL_N_STDTYPE );
    return (char*)__std_type[col->c_type-1];
  }else{
    return dflt;
  }
}

/*
** Helper function for __dbsql_error() - called rarely.  Broken out into
** a separate routine to avoid unnecessary register saves on entry to
** __dbsql_error().
*/
static DBSQL_NOINLINE void  __error_finish(DBSQL *db, int err_code){
  if( db->err ) __value_set_null(db->err);
  __system_error(db, err_code);
}

/*
** Set the current error code to err_code and clear any prior error message.
** Also set i_sys_errno (by calling __system) if the err_code indicates
** that would be appropriate.
*/
void __dbsql_error(DBSQL *db, int err_code){
  assert( db!=0 );
  db->errCode = err_code;
  if( err_code || db->err ){
    __error_finish(db, err_code);
  }else{
    db->err_byte_offset = -1;
  }
}

/*
** The equivalent of __dbsql_error(db, DBSQL_OK).  Clear the error state
** and error message.
*/
void __error_clear(DBSQL *db){
  assert( db!=0 );
  db->errCode = DBSQL_OK;
  db->err_byte_offset = -1;
  if( db->err ) __value_set_null(db->err);
}

/*
** Load the DBSQL.i_sys_errno field if that is an appropriate thing
** to do based on the dbsql error code in rc.
*/
void __system_error(DBSQL *db, int rc){
  if( rc==DBSQL_IOERR_NOMEM ) return;
#if defined(DBSQL_USE_SEH) && !defined(DBSQL_OMIT_WAL)
  if( rc==DBSQL_IOERR_IN_PAGE ){
    int ii;
    int i_err;
    __sm_enter_all(db);
    for(ii=0; ii<db->n_db; ii++){
      if( db->dbs[ii].bt ){
        i_err = __sm_pager_wal_system_errno(__sm_pager(db->dbs[ii].bt));
        if( i_err ){
          db->i_sys_errno = i_err;
        }
      }
    }
    __sm_leave_all(db);
    return;
  }
#endif
  rc &= 0xff;
  if( rc==DBSQL_CANTOPEN || rc==DBSQL_IOERR ){
    db->i_sys_errno = __dbsql_os_get_last_error(db->vfs);
  }
}

/*
** Set the most recent error code and error string for the sqlite
** handle "db". The error code is set to "err_code".
**
** If it is not NULL, string format specifies the format of the
** error string.  format and any string tokens that follow it are
** assumed to be encoded in UTF-8.
**
** To clear the most recent error for sqlite handle "db", __dbsql_error
** should be called with err_code set to DBSQL_OK and format set
** to NULL.
*/
void __error_with_msg(DBSQL *db, int err_code, const char *format, ...){
  assert( db!=0 );
  db->errCode = err_code;
  __system_error(db, err_code);
  if( format==0 ){
    __dbsql_error(db, err_code);
  }else if( db->err || (db->err = __value_new(db))!=0 ){
    char *z;
    va_list ap;
    va_start(ap, format);
    z = __vm_printf(db, format, ap);
    va_end(ap);
    __value_set_str(db->err, -1, z, DBSQL_UTF8, DBSQL_DYNAMIC);
  }
}

/*
** Check for interrupts and invoke progress callback.
*/
void __progress_check(parser_t *p){
  DBSQL *db = p->db;
  if( ATOMIC_LOAD(&db->u1.is_interrupted) ){
    p->n_err++;
    p->rc = DBSQL_INTERRUPT;
  }
#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
  if( db->progress ){
    if( p->rc==DBSQL_INTERRUPT ){
      p->n_progress_steps = 0;
    }else if( (++p->n_progress_steps)>=db->n_progress_ops ){
      if( db->progress(db->progress_arg) ){
        p->n_err++;
        p->rc = DBSQL_INTERRUPT;
      }
      p->n_progress_steps = 0;
    }
  }
#endif
}

/*
** Add an error message to parser->errmsg and increment parser->n_err.
**
** This function should be used to report any error that occurs while
** compiling an SQL statement (i.e. within dbsql_prepare()). The
** last thing the dbsql_prepare() function does is copy the error
** stored by this function into the database handle using __dbsql_error().
** Functions __dbsql_error() or __error_with_msg() should be used
** during statement execution (dbsql_step() etc.).
*/
void __error_msg(parser_t *parser, const char *format, ...){
  char *msg;
  va_list ap;
  DBSQL *db = parser->db;
  assert( db!=0 );
  assert( db->parser==parser || db->parser->toplevel==parser );
  db->err_byte_offset = -2;
  va_start(ap, format);
  msg = __vm_printf(db, format, ap);
  va_end(ap);
  if( db->err_byte_offset<-1 ) db->err_byte_offset = -1;
  if( db->suppress_err ){
    __dbsql_db_free(db, msg);
    if( db->malloc_failed ){
      parser->n_err++;
      parser->rc = DBSQL_NOMEM;
    }
  }else{
    parser->n_err++;
    __dbsql_db_free(db, parser->errmsg);
    parser->errmsg = msg;
    parser->rc = DBSQL_ERROR;
    parser->with = 0;
  }
}

/*
** If database connection db is currently parsing SQL, then transfer
** error code errCode to that parser if the parser has not already
** encountered some other kind of error.
*/
int __error_to_parser(DBSQL *db, int errCode){
  parser_t *parser;
  if( db==0 || (parser = db->parser)==0 ) return errCode;
  parser->rc = errCode;
  parser->n_err++;
  return errCode;
}

/*
** Convert an SQL-style quoted string into a normal string by removing
** the quote characters.  The conversion is done in-place.  If the
** input does not begin with a quote character, then this routine
** is a no-op.
**
** The input string must be zero-terminated.  A new zero-terminator
** is added to the dequoted string.
**
** The return value is -1 if no dequoting occurs or the length of the
** dequoted string, exclusive of the zero terminator, if dequoting does
** occur.
**
** 2002-02-14: This routine is extended to remove MS-Access style
** brackets from around identifiers.  For example:  "[a-b-c]" becomes
** "a-b-c".
*/
void __dequote(char *z){
  char quote;
  int i, j;
  if( z==0 ) return;
  quote = z[0];
  if( !__isquote(quote) ) return;
  if( quote=='[' ) quote = ']';
  for(i=1, j=0;; i++){
    assert( z[i] );
    if( z[i]==quote ){
      if( z[i+1]==quote ){
        z[j++] = quote;
        i++;
      }else{
        break;
      }
    }else{
      z[j++] = z[i];
    }
  }
  z[j] = 0;
}
void __dequote_expr(expr_t *p){
  assert( !EXPR_HAS_PROPERTY(p, EP_IntValue) );
  assert( __isquote(p->u.token[0]) );
  p->flags |= p->u.token[0]=='"' ? EP_Quoted|EP_DblQuoted : EP_Quoted;
  __dequote(p->u.token);
}

/*
** Expression p is a QNUMBER (quoted number). Dequote the value in p->u.token
** and set the type to INTEGER or FLOAT. "Quoted" integers or floats are those
** that contain '_' characters that must be removed before further processing.
*/
void __dequote_number(parser_t *parser, expr_t *p){
  assert( p!=0 || parser->db->malloc_failed );
  if( p ){
    const char *in = p->u.token;
    char *out = p->u.token;
    int hex = (in[0]=='0' && (in[1]=='x' || in[1]=='X'));
    int i_value;
    assert( p->op==TK_QNUMBER );
    p->op = TK_INTEGER;
    do {
      if( *in!=DBSQL_DIGIT_SEPARATOR ){
        *out++ = *in;
        if( *in=='e' || *in=='E' || *in=='.' ) p->op = TK_FLOAT;
      }else{
        if( (hex==0 && (!__isdigit(in[-1]) || !__isdigit(in[1])))
         || (hex==1 && (!__isxdigit(in[-1]) || !__isxdigit(in[1])))
        ){
          __error_msg(parser, "unrecognized token: \"%s\"", p->u.token);
        }
      }
    }while( *in++ );
    if( hex ) p->op = TK_INTEGER;

    /* tag-20240227-a: If after dequoting, the number is an integer that
    ** fits in 32 bits, then it must be converted into EP_IntValue.  Other
    ** parts of the code expect this.  See also tag-20240227-b. */
    if( p->op==TK_INTEGER && __get_int32(p->u.token, &i_value) ){
      p->u.i_value = i_value;
      p->flags |= EP_IntValue;
    }
  }
}

/*
** If the input token p is quoted, try to adjust the token to remove
** the quotes.  This is not always possible:
**
**     "abc"     ->   abc
**     "ab""cd"  ->   (not possible because of the interior "")
**
** Remove the quotes if possible.  This is a optimization.  The overall
** system should still return the correct answer even if this routine
** is always a no-op.
*/
void __dequote_token(Token *p){
  unsigned int i;
  if( p->n<2 ) return;
  if( !__isquote(p->z[0]) ) return;
  for(i=1; i<p->n-1; i++){
    if( __isquote(p->z[i]) ) return;
  }
  p->n -= 2;
  p->z++;
}

/*
** Generate a Token object from a string
*/
void __token_init(Token *p, char *z){
  p->z = z;
  p->n = __strlen30(z);
}

/* Convenient short-hand */
#define __upper_to_lower __upper_to_lower

/*
** Some systems have stricmp().  Others have strcasecmp().  Because
** there is no consistency, we will define our own.
**
** IMPLEMENTATION-OF: R-30243-02494 The dbsql_stricmp() and
** dbsql_strnicmp() APIs allow applications and extensions to compare
** the contents of two buffers containing UTF-8 strings in a
** case-independent fashion, using the same definition of "case
** independence" that dbsql uses internally when comparing identifiers.
*/
int dbsql_stricmp(const char *left, const char *right){
  if( left==0 ){
    return right ? -1 : 0;
  }else if( right==0 ){
    return 1;
  }
  return __str_icmp(left, right);
}
int __str_icmp(const char *left, const char *right){
  unsigned char *a, *b;
  int c, x;
  a = (unsigned char *)left;
  b = (unsigned char *)right;
  for(;;){
    c = *a;
    x = *b;
    if( c==x ){
      if( c==0 ) break;
    }else{
      c = (int)__upper_to_lower[c] - (int)__upper_to_lower[x];
      if( c ) break;
    }
    a++;
    b++;
  }
  return c;
}
int dbsql_strnicmp(const char *left, const char *right, int N){
  register unsigned char *a, *b;
  if( left==0 ){
    return right ? -1 : 0;
  }else if( right==0 ){
    return 1;
  }
  a = (unsigned char *)left;
  b = (unsigned char *)right;
  while( N-- > 0 && *a!=0 && __upper_to_lower[*a]==__upper_to_lower[*b]){ a++; b++; }
  return N<0 ? 0 : __upper_to_lower[*a] - __upper_to_lower[*b];
}

/*
** Compute an 8-bit hash on a string that is insensitive to case differences
*/
u_int8_t __str_i_hash(const char *z){
  u_int8_t h = 0;
  if( z==0 ) return 0;
  while( z[0] ){
    h += __upper_to_lower[(unsigned char)z[0]];
    z++;
  }
  return h;
}

#if !defined(DBSQL_DISABLE_INTRINSIC)               \
  && (defined(__GNUC__) || defined(__clang__))       \
  && (defined(__x86_64__) || defined(__aarch64__) || \
      (defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen>32)))
#define DBSQL_USE_UINT128
#endif

/*
** Two inputs are multiplied to get a 128-bit result.  Write the
** lower 64-bits of the result into *lo, and return the high-order
** 64 bits.
*/
static u_int64_t __multiply128(u_int64_t a, u_int64_t b, u_int64_t *lo){
#if defined(DBSQL_USE_UINT128)
  __uint128_t r = (__uint128_t)a * b;
  *lo = (u_int64_t)r;
  return (u_int64_t)(r>>64);
#elif defined(_WIN64) && !defined(DBSQL_DISABLE_INTRINSIC)
  *lo = a*b;
  return __umulh(a, b);
#else
  u_int64_t a0 = (u_int32_t)a;
  u_int64_t a1 = a >> 32;
  u_int64_t b0 = (u_int32_t)b;
  u_int64_t b1 = b >> 32;
  u_int64_t a0b0 = a0 * b0;
  u_int64_t a1b1 = a1 * b1;
  u_int64_t a0b1 = a0 * b1;
  u_int64_t a1b0 = a1 * b0;
  u_int64_t t = (a0b0 >> 32) + (u_int32_t)a0b1 + (u_int32_t)a1b0;
  *lo = (a0b0 & UINT64_C(0xffffffff)) | (t << 32);
  return a1b1 + (a0b1>>32) + (a1b0>>32) + (t>>32);
#endif
}

/*
** A is an unsigned 96-bit integer formed by (a<<32)+los.
** B is an unsigned 64-bit integer.
**
** Compute the upper 96 bits of 160-bit result of A*B.
**
** Write ((A*B)>>64 & 0xffffffff) (the middle 32 bits of A*B)
** into *lo.  Return the upper 64 bits of A*B.
**
** The lower 64 bits of A*B are discarded.
*/
static u_int64_t __multiply160(u_int64_t a, u_int32_t los, u_int64_t b, u_int32_t *lo){
#if defined(DBSQL_USE_UINT128)
  __uint128_t r = (__uint128_t)a * b;
  r += ((__uint128_t)los * b) >> 32;
  *lo = (r>>32)&0xffffffff;
  return r>>64;
#elif defined(_WIN64) && !defined(DBSQL_DISABLE_INTRINSIC)
  u_int64_t r1_hi = __umulh(a,b);
  u_int64_t r1_lo = a*b;
  u_int64_t r2 = (__umulh((u_int64_t)los,b)<<32) + ((los*b)>>32);
  u_int64_t t = r1_lo + r2;
  if( t<r1_lo ) r1_hi++;
  *lo = t>>32;
  return r1_hi;
#else
  u_int64_t x2 = a>>32;
  u_int64_t x1 = a&0xffffffff;
  u_int64_t x0 = los;
  u_int64_t y1 = b>>32;
  u_int64_t y0 = b&0xffffffff;
  u_int64_t x2y1 = x2*y1;
  u_int64_t r4 = x2y1>>32;
  u_int64_t x2y0 = x2*y0;
  u_int64_t x1y1 = x1*y1;
  u_int64_t r3 = (x2y1 & 0xffffffff) + (x2y0 >>32) + (x1y1 >>32);
  u_int64_t x1y0 = x1*y0;
  u_int64_t x0y1 = x0*y1;
  u_int64_t r2 = (x2y0 & 0xffffffff) + (x1y1 & 0xffffffff) +
             (x1y0 >>32) + (x0y1>>32);
  u_int64_t x0y0 = x0*y0;
  u_int64_t r1 = (x1y0 & 0xffffffff) + (x0y1 & 0xffffffff) +
             (x0y0 >>32);
  r2 += r1>>32;
  r3 += r2>>32;
  *lo = r2&0xffffffff;
  return (r4<<32) + r3;
#endif
}

#undef DBSQL_USE_UINT128

/*
** Return a u64 with the N-th bit set.
*/
#define U64_BIT(N)  (((u_int64_t)1)<<(N))

/*
** Range of powers of 10 that we need to deal with when converting
** IEEE754 doubles to and from decimal.
*/
#define POWERSOF10_FIRST (-348)
#define POWERSOF10_LAST  (+347)

/*
** For any p between -348 and +347, return the integer part of
**
**    pow(10,p) * pow(2,63-pow10to2(p))
**
** Or, in other words, for any p in range, return the most significant
** 64 bits of pow(10,p).  The pow(10,p) value is shifted left or right,
** as appropriate so the most significant 64 bits fit exactly into a
** 64-bit unsigned integer.
**
** Write into *lo the next 32 significant bits of the answer after
** the first 64.
**
** Algorithm:
**
** (1) For p between 0 and 26, return the value directly from the bases[]
**     lookup table.
**
** (2) For p outside the range 0 to 26, use scales[] for the initial value
**     then refine that result (if necessary) by a single multiplication
**     against bases[].
**
** The constant tables bases[], scales[], and scale_los[] are generated
** by the C program at ../tool/mkfptab.c run with the --round option.
*/
static u_int64_t __power_of_ten(int p, u_int32_t *p_lo){
  static const u_int64_t bases[] = {
    UINT64_C(0x8000000000000000), /*  0: 1.0e+0 << 63 */
    UINT64_C(0xa000000000000000), /*  1: 1.0e+1 << 60 */
    UINT64_C(0xc800000000000000), /*  2: 1.0e+2 << 57 */
    UINT64_C(0xfa00000000000000), /*  3: 1.0e+3 << 54 */
    UINT64_C(0x9c40000000000000), /*  4: 1.0e+4 << 50 */
    UINT64_C(0xc350000000000000), /*  5: 1.0e+5 << 47 */
    UINT64_C(0xf424000000000000), /*  6: 1.0e+6 << 44 */
    UINT64_C(0x9896800000000000), /*  7: 1.0e+7 << 40 */
    UINT64_C(0xbebc200000000000), /*  8: 1.0e+8 << 37 */
    UINT64_C(0xee6b280000000000), /*  9: 1.0e+9 << 34 */
    UINT64_C(0x9502f90000000000), /* 10: 1.0e+10 << 30 */
    UINT64_C(0xba43b74000000000), /* 11: 1.0e+11 << 27 */
    UINT64_C(0xe8d4a51000000000), /* 12: 1.0e+12 << 24 */
    UINT64_C(0x9184e72a00000000), /* 13: 1.0e+13 << 20 */
    UINT64_C(0xb5e620f480000000), /* 14: 1.0e+14 << 17 */
    UINT64_C(0xe35fa931a0000000), /* 15: 1.0e+15 << 14 */
    UINT64_C(0x8e1bc9bf04000000), /* 16: 1.0e+16 << 10 */
    UINT64_C(0xb1a2bc2ec5000000), /* 17: 1.0e+17 << 7 */
    UINT64_C(0xde0b6b3a76400000), /* 18: 1.0e+18 << 4 */
    UINT64_C(0x8ac7230489e80000), /* 19: 1.0e+19 >> 0 */
    UINT64_C(0xad78ebc5ac620000), /* 20: 1.0e+20 >> 3 */
    UINT64_C(0xd8d726b7177a8000), /* 21: 1.0e+21 >> 6 */
    UINT64_C(0x878678326eac9000), /* 22: 1.0e+22 >> 10 */
    UINT64_C(0xa968163f0a57b400), /* 23: 1.0e+23 >> 13 */
    UINT64_C(0xd3c21bcecceda100), /* 24: 1.0e+24 >> 16 */
    UINT64_C(0x84595161401484a0), /* 25: 1.0e+25 >> 20 */
    UINT64_C(0xa56fa5b99019a5c8), /* 26: 1.0e+26 >> 23 */
  };
  static const u_int64_t scales[] = {
    UINT64_C(0x8049a4ac0c5811ae), /*  0: 1.0e-351 << 1229 */
    UINT64_C(0xcf42894a5dce35ea), /*  1: 1.0e-324 << 1140 */
    UINT64_C(0xa76c582338ed2621), /*  2: 1.0e-297 << 1050 */
    UINT64_C(0x873e4f75e2224e68), /*  3: 1.0e-270 << 960 */
    UINT64_C(0xda7f5bf590966848), /*  4: 1.0e-243 << 871 */
    UINT64_C(0xb080392cc4349dec), /*  5: 1.0e-216 << 781 */
    UINT64_C(0x8e938662882af53e), /*  6: 1.0e-189 << 691 */
    UINT64_C(0xe65829b3046b0afa), /*  7: 1.0e-162 << 602 */
    UINT64_C(0xba121a4650e4ddeb), /*  8: 1.0e-135 << 512 */
    UINT64_C(0x964e858c91ba2655), /*  9: 1.0e-108 << 422 */
    UINT64_C(0xf2d56790ab41c2a2), /* 10: 1.0e-81 << 333 */
    UINT64_C(0xc428d05aa4751e4c), /* 11: 1.0e-54 << 243 */
    UINT64_C(0x9e74d1b791e07e48), /* 12: 1.0e-27 << 153 */
    UINT64_C(0xcccccccccccccccc), /* 13: 1.0e-1 << 67 (special case) */
    UINT64_C(0xcecb8f27f4200f3a), /* 14: 1.0e+27 >> 26 */
    UINT64_C(0xa70c3c40a64e6c51), /* 15: 1.0e+54 >> 116 */
    UINT64_C(0x86f0ac99b4e8dafd), /* 16: 1.0e+81 >> 206 */
    UINT64_C(0xda01ee641a708de9), /* 17: 1.0e+108 >> 295 */
    UINT64_C(0xb01ae745b101e9e4), /* 18: 1.0e+135 >> 385 */
    UINT64_C(0x8e41ade9fbebc27d), /* 19: 1.0e+162 >> 475 */
    UINT64_C(0xe5d3ef282a242e81), /* 20: 1.0e+189 >> 564 */
    UINT64_C(0xb9a74a0637ce2ee1), /* 21: 1.0e+216 >> 654 */
    UINT64_C(0x95f83d0a1fb69cd9), /* 22: 1.0e+243 >> 744 */
    UINT64_C(0xf24a01a73cf2dccf), /* 23: 1.0e+270 >> 833 */
    UINT64_C(0xc3b8358109e84f07), /* 24: 1.0e+297 >> 923 */
    UINT64_C(0x9e19db92b4e31ba9), /* 25: 1.0e+324 >> 1013 */
  };
  static const unsigned int scale_los[] = {
    0x205b896d, /*  0: 1.0e-351 << 1229 */
    0x52064cad, /*  1: 1.0e-324 << 1140 */
    0xaf2af2b8, /*  2: 1.0e-297 << 1050 */
    0x5a7744a7, /*  3: 1.0e-270 << 960 */
    0xaf39a475, /*  4: 1.0e-243 << 871 */
    0xbd8d794e, /*  5: 1.0e-216 << 781 */
    0x547eb47b, /*  6: 1.0e-189 << 691 */
    0x0cb4a5a3, /*  7: 1.0e-162 << 602 */
    0x92f34d62, /*  8: 1.0e-135 << 512 */
    0x3a6a07f9, /*  9: 1.0e-108 << 422 */
    0xfae27299, /* 10: 1.0e-81 << 333 */
    0xaa97e14c, /* 11: 1.0e-54 << 243 */
    0x775ea265, /* 12: 1.0e-27 << 153 */
    0xcccccccc, /* 13: 1.0e-1 << 67 (special case) */
    0x00000000, /* 14: 1.0e+27 >> 26 */
    0x999090b6, /* 15: 1.0e+54 >> 116 */
    0x69a028bb, /* 16: 1.0e+81 >> 206 */
    0xe80e6f48, /* 17: 1.0e+108 >> 295 */
    0x5ec05dd0, /* 18: 1.0e+135 >> 385 */
    0x14588f14, /* 19: 1.0e+162 >> 475 */
    0x8f1668c9, /* 20: 1.0e+189 >> 564 */
    0x6d953e2c, /* 21: 1.0e+216 >> 654 */
    0x4abdaf10, /* 22: 1.0e+243 >> 744 */
    0xbc633b39, /* 23: 1.0e+270 >> 833 */
    0x0a862f81, /* 24: 1.0e+297 >> 923 */
    0x6c07a2c2, /* 25: 1.0e+324 >> 1013 */
  };
  int g, n;
  u_int64_t s, x;
  u_int32_t lo;

  assert( p>=POWERSOF10_FIRST && p<=POWERSOF10_LAST );
  if( p<0 ){
    if( p==(-1) ){
      *p_lo = scale_los[13];
      return scales[13];
    }
    g = p/27;
    n = p%27;
    if( n ){
      g--;
      n += 27;
    }
  }else if( p<27 ){
    *p_lo = 0;
    return bases[p];
  }else{
    g = p/27;
    n = p%27;
  }
  s = scales[g+13];
  if( n==0 ){
    *p_lo = scale_los[g+13];
    return s;
  }
  x = __multiply160(s,scale_los[g+13],bases[n],&lo);
  if( (U64_BIT(63) & x)==0 ){
    x  = x<<1 | ((lo>>31)&1);
    lo = (lo<<1) | 1;
  }
  *p_lo = lo;
  return x;
}

/*
** pow10to2(x) computes floor(log2(pow(10,x))).
** pow2to10(y) computes floor(log10(pow(2,y))).
**
** Conceptually, pow10to2(p) converts a base-10 exponent p into
** a corresponding base-2 exponent, and pow2to10(e) converts a base-2
** exponent into a base-10 exponent.
**
** The conversions are based on the observation that:
**
**     ln(10.0)/ln(2.0) == 108853/32768     (approximately)
**     ln(2.0)/ln(10.0) == 78913/262144     (approximately)
**
** These ratios are approximate, but they are accurate to 5 digits,
** which is close enough for the usage here.  Right-shift is used
** for division so that rounding of negative numbers happens in the
** right direction.
*/
static int pwr10to2(int p){ return (p*108853) >> 15; }
static int pwr2to10(int p){ return (p*78913) >> 18; }

/*
** Count leading zeros for a 64-bit unsigned integer.
*/
static int __count_leading_zeros(u_int64_t m){
#if (defined(__GNUC__) || defined(__clang__)) \
    && !defined(DBSQL_DISABLE_INTRINSIC)
  return __builtin_clzll(m);
#else
  int n = 0;
  if( m <= 0x00000000ffffffffULL) { n += 32; m <<= 32; }
  if( m <= 0x0000ffffffffffffULL) { n += 16; m <<= 16; }
  if( m <= 0x00ffffffffffffffULL) { n += 8;  m <<= 8;  }
  if( m <= 0x0fffffffffffffffULL) { n += 4;  m <<= 4;  }
  if( m <= 0x3fffffffffffffffULL) { n += 2;  m <<= 2;  }
  if( m <= 0x7fffffffffffffffULL) { n += 1;            }
  return n;
#endif
}

/*
** Given m and e, which represent a quantity r == m*pow(2,e),
** return values *d and *p such that r == (*d)*pow(10,*p),
** approximately.  *d should contain at least n significant digits.
**
** The input m is required to have its highest bit set.  In other words,
** m should be left-shifted, and e decremented, to maximize the value of m.
*/
static void __fp2_convert10(u_int64_t m, int e, int n, u_int64_t *d, int *p_p){
  int p;
  u_int64_t h, d1;
  u_int32_t d2;
  assert( n>=1 && n<=18 );
  p = n - 1 - pwr2to10(e+63);
  h = __multiply128(m, __power_of_ten(p,&d2), &d1);
  assert( -(e + pwr10to2(p) + 2) >= 0  );
  assert( -(e + pwr10to2(p) + 1) <= 63 );
  if( n==18 ){
    h >>= -(e + pwr10to2(p) + 2);
    *d = (h + ((h<<1)&2))>>1;
  }else{
    *d = h >> -(e + pwr10to2(p) + 1);
  }
  *p_p = -p;
}

/*
** Return an IEEE754 floating point value that approximates d*pow(10,p).
**
** The (current) algorithm is adapted from the work of Ross Cox at
** https://github.com/rsc/fpfmt
*/
static double __fp10_convert2(u_int64_t d, int p){
  int b, lp, e, adj, s;
  u_int32_t pwr10l, mid1;
  u_int64_t pwr10h, x, hi, lo, sticky, u, m;
  double r;
  if( p<POWERSOF10_FIRST ) return 0.0;
  if( p>POWERSOF10_LAST ) return INFINITY;
  b = 64 - __count_leading_zeros(d);
  lp = pwr10to2(p);
  e = 53 - b - lp;
  if( e > 1074 ){
    if( e>=1130 ) return 0.0;
    e = 1074;
  }
  s = -(e-(64-b) + lp + 3);
  pwr10h = __power_of_ten(p, &pwr10l);
  if( pwr10l!=0 ){
    pwr10h++;
    pwr10l = ~pwr10l;
  }
  x = d<<(64-b);
  hi = __multiply128(x,pwr10h,&lo);
  mid1 = lo>>32;
  sticky = 1;
  if( (hi & (U64_BIT(s)-1))==0 ) {
    u_int32_t mid2 = __multiply128(x,((u_int64_t)pwr10l)<<32,&lo)>>32;
    sticky = (mid1-mid2 > 1);
    hi -= mid1 < mid2;
  }
  u = (hi>>s) | sticky;
  adj = (u >= U64_BIT(55)-2);
  if( adj ){
    u = (u>>adj) | (u&1);
    e -= adj;
  }
  m = (u + 1 + ((u>>2)&1)) >> 2;
  if( e<=(-972) ) return INFINITY;
  if((m & U64_BIT(52)) != 0){
    m = (m & ~U64_BIT(52)) | ((u_int64_t)(1075-e)<<52);
  }
  memcpy(&r,&m,8);
  return r;
}

/*
** The string z[] is an text representation of a real number.
** Convert this string to a double and write it into *result.
**
** z[] must be UTF-8 and zero-terminated.
**
** Return positive if the result is a valid real number (or integer) and
** zero or negative if the string is empty or contains extraneous text.
** Lower bits of the return value contain addition information about the
** parse:
**
**   bit 0       =>   Set if any prefix of the input is valid.  Clear if
**                    there is no prefix of the input that can be seen as
**                    a valid floating point number.
**   bit 1       =>   Set if the input contains a decimal point or nnn
**                    clause.  Zero if the input is an integer.
**   bit 2       =>   The input is exactly 0.0, not an underflow from
**                    some value near zero.
**   bit 3       =>   Set if there are more than about 19 significant
**                    digits in the input.
**
** If the input contains a syntax error but begins with text that might
** be a valid number of some kind, then the result is negative.  The
** result is only zero if no prefix of the input could be interpreted as
** a number.
**
** Leading and trailing whitespace is ignored.  Valid numbers are in
** one of the formats below:
**
**    [+-]digits[E[+-]digits]
**    [+-]digits.[digits][E[+-]digits]
**    [+-].digits[E[+-]digits]
**
** Algorithm sketch:  Compute an unsigned 64-bit integer s and a base-10
** exponent d such that the value encoding by the input is s*pow(10,d).
** Then invoke __fp10_convert2() to calculated the closest possible
** IEEE754 double.  The sign is added back afterwards, if the input string
** starts with a "-".  The use of an unsigned 64-bit s mantissa means that
** only about the first 19 significant digits of the input can contribute
** to the result.  This can result in suboptimal rounding decisions when
** correct rounding requires more than 19 input digits.  For example,
** this routine renders "3500000000000000.2500001" as
** 3500000000000000.0 instead of 3500000000000000.5 because the decision
** to round up instead of using banker's rounding to round down is determined
** by the 23rd significant digit, which this routine ignores. It is not
** possible to do better without some kind of __big_num.
*/
int __ato_f(const char *in, double *result){
#ifndef DBSQL_OMIT_FLOATING_POINT
  const unsigned char *z = (const unsigned char*)in;
  int neg = 0;       /* True for a negative value */
  u_int64_t s = 0;         /* mantissa */
  int d = 0;         /* Value is s * pow(10,d) */
  int m_state = 0;    /* 1: digit seen 2: fp 4: hard-zero */
  unsigned v;        /* Value of a single digit */

  start_of_text:
  if( (v = (unsigned)z[0] - '0')<10 ){
    parse_integer_part:
    m_state = 1;
    s = v;
    z++;
    while( (v = (unsigned)z[0] - '0')<10 ){
      s = s*10 + v;
      z++;
      if( s>=(LARGEST_UINT64-9)/10 ){
        m_state = 9;
        while( __isdigit(z[0]) ){ z++; d++; }
        break;
      }
    }
  }else if( z[0]=='-' ){
    neg = 1;
    z++;
    if( (v = (unsigned)z[0] - '0')<10 ) goto parse_integer_part;
  }else if( z[0]=='+' ){
    z++;
    if( (v = (unsigned)z[0] - '0')<10 ) goto parse_integer_part;
  }else if( __isspace(z[0]) ){
    do{ z++; }while( __isspace(z[0]) );
    goto start_of_text;
  }else{
    s = 0;
  }

  /* if decimal point is present */
  if( *z=='.' ){
    z++;
    if( __isdigit(z[0]) ){
      m_state |= 1;
      do{
        if( s<(LARGEST_UINT64-9)/10 ){
          s = s*10 + z[0] - '0';
          d--;
        }else{
          m_state = 11;
        }
      }while( __isdigit(*++z) );
    }else if( m_state==0 ){
      *result = 0.0;
      return 0;
    }
    m_state |= 2;
  }else if( m_state==0 ){
    *result = 0.0;
    return 0;
  }

  /* if exponent is present */
  if( *z=='e' || *z=='E' ){
    int esign;
    z++;
 
    /* get sign of exponent */
    if( *z=='-' ){
      esign = -1;
      z++;
    }else{
      esign = +1;
      if( *z=='+' ){
        z++;
      }
    }
    /* copy digits to exponent */
    if( (v = (unsigned)z[0] - '0')<10 ){
      int exp = v;
      z++;
      m_state |= 2;
      while( (v = (unsigned)z[0] - '0')<10 ){
        exp = exp<10000 ? (exp*10 + v) : 10000;
        z++;
      }
      d += esign*exp;
    }else{
      z--;  /* Leave z[0] at 'e' or '+' or '-',
            ** so that the return is 0 or -1 */
    }
  }

  /* Convert s*pow(10,d) into real */
  if( s==0 ){
    *result = 0.0;
    m_state |= 4;
  }else{
    *result = __fp10_convert2(s,d);
  }
  if( neg ) *result = -*result;
  assert( !__is_na_n(*result) );

  /* return true if number and no extra non-whitespace characters after */
  if( z[0]==0 ){
    return m_state;
  }
  if( __isspace(z[0]) ){
    do{ z++; }while( __isspace(*z) );
    if( z[0]==0 ){
      return m_state;
    }
  }
  return 0xfffffff0 | m_state;
#else
  return __atoi64(z, result, strlen(z), DBSQL_UTF8)==0;
#endif /* DBSQL_OMIT_FLOATING_POINT */
}

/*
** Digit pairs used to convert a U64 or I64 into text, two digits
** at a time.
*/
static const union {
  char a[201];
  short int force_alignment;
} __digit_pairs = {
  "00010203040506070809"
  "10111213141516171819"
  "20212223242526272829"
  "30313233343536373839"
  "40414243444546474849"
  "50515253545556575859"
  "60616263646566676869"
  "70717273747576777879"
  "80818283848586878889"
  "90919293949596979899"
};

/*
** ARMv6, ARMv7, PPC32 are known to not support hardware u64 division.
*/
#if (defined(__arm__) && !defined(__aarch64__)) || \
    (defined(__ppc__) && !defined(__ppc64__))
# define DBSQL_AVOID_U64_DIVIDE 1
#endif

#ifdef DBSQL_AVOID_U64_DIVIDE
/*
** Render an unsigned 64-bit integer as text onto the end of a 2-byte
** aligned buffer that is DBSQL_U64_DIGIT+1 bytes long.  The last byte
** of the buffer will be filled with a \000 byte.
**
** Return the index into the buffer of the first byte.
**
** This routine is used on platforms where u64-division is slow because
** it is not available in hardware and has to be emulated in software.
** It seeks to minimize the number of u64 divisions and use u32 divisions
** instead.  It is slower on platforms that have hardware u64 division,
** but much faster on platforms that do not.
*/
static int __u_int64_to_text(u_int64_t v, char *out){
  u_int32_t x32, kk;
  int i;
  out[DBSQL_U64_DIGITS] = 0;
  i = DBSQL_U64_DIGITS;
  assert( TWO_BYTE_ALIGNMENT(&__digit_pairs.a[0]) );
  assert( TWO_BYTE_ALIGNMENT(out) );
  while( (v>>32)!=0 ){
    u_int32_t y, x0, x1, y0, y1;
    x32 = v % 100000000;
    v   = v / 100000000;
    y = x32 % 10000;
    x32 /= 10000;
    x1 = x32 / 100;
    x0 = x32 % 100;
    y1 = y / 100;
    y0 = y % 100;
    assert( i>=8 );
    i -= 8;
    *(u_int16_t*)(&out[i]) = *(u_int16_t*)&__digit_pairs.a[x1*2];
    *(u_int16_t*)(&out[i+2]) = *(u_int16_t*)&__digit_pairs.a[x0*2];
    *(u_int16_t*)(&out[i+4]) = *(u_int16_t*)&__digit_pairs.a[y1*2];
    *(u_int16_t*)(&out[i+6]) = *(u_int16_t*)&__digit_pairs.a[y0*2];
  }
  x32 = v;
  while( x32>=10 ){
    kk  = x32 % 100;
    x32 = x32 / 100;
    assert( TWO_BYTE_ALIGNMENT(&__digit_pairs.a[kk*2]) );
    assert( i>=2 );
    i -= 2;
    assert( TWO_BYTE_ALIGNMENT(&out[i]) );
    *(u_int16_t*)(&out[i]) = *(u_int16_t*)&__digit_pairs.a[kk*2];
  }
  if( x32 ){
    assert( i>0 );
    out[--i] = x32 + '0';
  }
  return i;
}
#endif /* defined(DBSQL_AVOID_U64_DIVIDE) */

/*
** Render an signed 64-bit integer as text.  Store the result in out[] and
** return the length of the string that was stored, in bytes.  The value
** returned does not include the zero terminator at the end of the output
** string.
**
** The caller must ensure that out[] is at least 21 bytes in size.
*/
int __int64_to_text(int64_t v, char *out){
  int i;
  u_int64_t x;
  union {
    char a[DBSQL_U64_DIGITS+1];
    u_int16_t force_alignment;
  } u;
  if( v>0 ){
    x = v;
  }else if( v==0 ){
    out[0] = '0';
    out[1] = 0;
    return 1;
  }else{
    x = (v==SMALLEST_INT64) ? ((u_int64_t)1)<<63 : (u_int64_t)-v;
  }
#ifdef DBSQL_AVOID_U64_DIVIDE
  i = __u_int64_to_text(x, u.a);
#else
  i = sizeof(u.a)-1;
  u.a[i] = 0;
  while( x>=10 ){
    int kk = (x%100)*2;
    assert( TWO_BYTE_ALIGNMENT(&__digit_pairs.a[kk]) );
    assert( TWO_BYTE_ALIGNMENT(&u.a[i-2]) );
    *(u_int16_t*)(&u.a[i-2]) = *(u_int16_t*)&__digit_pairs.a[kk];
    i -= 2;
    x /= 100;
  }
  if( x ){
    u.a[--i] = x + '0';
  }
#endif /* DBSQL_AVOID_U64_DIVIDE */
  if( v<0 ) u.a[--i] = '-';
  memcpy(out, &u.a[i], sizeof(u.a)-i);
  return sizeof(u.a)-1-i;
}

/*
** Compare the 19-character string num against the text representation
** value 2^63:  9223372036854775808.  Return negative, zero, or positive
** if num is less than, equal to, or greater than the string.
** Note that num must contain exactly 19 characters.
**
** Unlike memcmp() this routine is guaranteed to return the difference
** in the values of the last digit if the only difference is in the
** last digit.  So, for example,
**
**      compare2pow63("9223372036854775800", 1)
**
** will return -8.
*/
static int compare2pow63(const char *num, int incr){
  int c = 0;
  int i;
                    /* 012345678901234567 */
  const char *pow63 = "922337203685477580";
  for(i=0; c==0 && i<18; i++){
    c = (num[i*incr]-pow63[i])*10;
  }
  if( c==0 ){
    c = num[18*incr] - '8';
    testcase( c==(-1) );
    testcase( c==0 );
    testcase( c==(+1) );
  }
  return c;
}

/*
** Convert num to a 64-bit signed integer.  num must be decimal. This
** routine does *not* accept hexadecimal notation.
**
** Returns:
**
**    -1    Not even a prefix of the input text looks like an integer
**     0    Successful transformation.  Fits in a 64-bit signed integer.
**     1    Excess non-space text after the integer value
**     2    Integer too large for a 64-bit signed integer or is malformed
**     3    Special case of 9223372036854775808
**
** length is the number of bytes in the string (bytes, not characters).
** The string is not necessarily zero-terminated.  The encoding is
** given by enc.
*/
int __atoi64(const char *z_num, int64_t *num, int length, u_int8_t enc){
  int incr;
  u_int64_t u = 0;
  int neg = 0; /* assume positive */
  int i, j;
  unsigned int c = 0;
  int non_num = 0;  /* True if input contains UTF16 with high byte non-zero */
  int rc;          /* Baseline return code */
  const char *start;
  const char *end = z_num + length;
  assert( enc==DBSQL_UTF8 || enc==DBSQL_UTF16LE || enc==DBSQL_UTF16BE );
  if( enc==DBSQL_UTF8 ){
    incr = 1;
  }else{
    incr = 2;
    length &= ~1;
    assert( DBSQL_UTF16LE==2 && DBSQL_UTF16BE==3 );
    for(i=3-enc; i<length && z_num[i]==0; i+=2){}
    non_num = i<length;
    end = &z_num[i^1];
    z_num += (enc&1);
  }
  while( z_num<end && __isspace(*z_num) ) z_num+=incr;
  if( z_num<end ){
    if( *z_num=='-' ){
      neg = 1;
      z_num+=incr;
    }else if( *z_num=='+' ){
      z_num+=incr;
    }
  }
  start = z_num;
  while( z_num<end && z_num[0]=='0' ){ z_num+=incr; } /* Skip leading zeros. */
  for(i=0; &z_num[i]<end && (c=(unsigned)z_num[i]-'0')<=9; i+=incr){
    u = u*10 + c;
  }
  testcase( i==18*incr );
  testcase( i==19*incr );
  testcase( i==20*incr );
  if( u>LARGEST_INT64 ){
    /* This test and assignment is needed only to suppress UB warnings
    ** from clang and -fsanitize=undefined.  This test and assignment make
    ** the code a little larger and slower, and no harm comes from omitting
    ** them, but we must appease the undefined-behavior pharisees. */
    *num = neg ? SMALLEST_INT64 : LARGEST_INT64;
  }else if( neg ){
    *num = -(int64_t)u;
  }else{
    *num = (int64_t)u;
  }
  rc = 0;
  if( i==0 && start==z_num ){    /* No digits */
    rc = -1;
  }else if( non_num ){            /* UTF16 with high-order bytes non-zero */
    rc = 1;
  }else if( &z_num[i]<end ){     /* Extra bytes at the end */
    int jj = i;
    do{
      if( !__isspace(z_num[jj]) ){
        rc = 1;          /* Extra non-space text after the integer */
        break;
      }
      jj += incr;
    }while( &z_num[jj]<end );
  }
  if( i<19*incr ){
    /* Less than 19 digits, so we know that it fits in 64 bits */
    assert( u<=LARGEST_INT64 );
    return rc;
  }else{
    /* num is a 19-digit numbers.  Compare it against 9223372036854775808. */
    j = i>19*incr ? 1 : compare2pow63(z_num, incr);
    if( j<0 ){
      /* num is less than 9223372036854775808 so it fits */
      assert( u<=LARGEST_INT64 );
      return rc;
    }else{
      *num = neg ? SMALLEST_INT64 : LARGEST_INT64;
      if( j>0 ){
        /* num is greater than 9223372036854775808 so it overflows */
        return 2;
      }else{
        /* num is exactly 9223372036854775808.  Fits if negative.  The
        ** special case 2 overflow if positive */
        assert( u-1==LARGEST_INT64 );
        return neg ? rc : 3;
      }
    }
  }
}

/*
** Transform a UTF-8 integer literal, in either decimal or hexadecimal,
** into a 64-bit signed integer.  This routine accepts hexadecimal literals,
** whereas __atoi64() does not.
**
** Returns:
**
**     0    Successful transformation.  Fits in a 64-bit signed integer.
**     1    Excess text after the integer value
**     2    Integer too large for a 64-bit signed integer or is malformed
**     3    Special case of 9223372036854775808
*/
int __dec_or_hex_to_i64(const char *z, int64_t *out){
#ifndef DBSQL_OMIT_HEX_INTEGER
  if( z[0]=='0'
   && (z[1]=='x' || z[1]=='X')
  ){
    u_int64_t u = 0;
    int i, k;
    for(i=2; z[i]=='0'; i++){}
    for(k=i; __isxdigit(z[k]); k++){
      u = u*16 + __hex_to_int(z[k]);
    }
    memcpy(out, &u, 8);
    if( k-i>16 ) return 2;
    if( z[k]!=0 ) return 1;
    return 0;
  }else
#endif /* DBSQL_OMIT_HEX_INTEGER */
  {
    int n = (int)(0x3fffffff&strspn(z,"+- \n\t0123456789"));
    if( z[n] ) n++;
    return __atoi64(z, out, n, DBSQL_UTF8);
  }
}

/*
** If num represents an integer that will fit in 32-bits, then set
** *value to that integer and return true.  Otherwise return false.
**
** This routine accepts both decimal and hexadecimal notation for integers.
**
** Any non-numeric characters that following num are ignored.
** This is different from __atoi64() which requires the
** input number to be zero-terminated.
*/
int __get_int32(const char *num, int *value){
  int64_t v = 0;
  int i, c;
  int neg = 0;
  if( num[0]=='-' ){
    neg = 1;
    num++;
  }else if( num[0]=='+' ){
    num++;
  }
#ifndef DBSQL_OMIT_HEX_INTEGER
  else if( num[0]=='0'
        && (num[1]=='x' || num[1]=='X')
        && __isxdigit(num[2])
  ){
    u_int32_t u = 0;
    num += 2;
    while( num[0]=='0' ) num++;
    for(i=0; i<8 && __isxdigit(num[i]); i++){
      u = u*16 + __hex_to_int(num[i]);
    }
    if( (u&0x80000000)==0 && __isxdigit(num[i])==0 ){
      memcpy(value, &u, 4);
      return 1;
    }else{
      return 0;
    }
  }
#endif
  if( !__isdigit(num[0]) ) return 0;
  while( num[0]=='0' ) num++;
  for(i=0; i<11 && (c = num[i] - '0')>=0 && c<=9; i++){
    v = v*10 + c;
  }

  /* The longest decimal representation of a 32 bit integer is 10 digits:
  **
  **             1234567890
  **     2^31 -> 2147483648
  */
  testcase( i==10 );
  if( i>10 ){
    return 0;
  }
  testcase( v-neg==2147483647 );
  if( v-neg>2147483647 ){
    return 0;
  }
  if( neg ){
    v = -v;
  }
  *value = (int)v;
  return 1;
}

/*
** Return a 32-bit integer value extracted from a string.  If the
** string is not an integer, just return 0.
*/
int __atoi(const char *z){
  int x = 0;
  __get_int32(z, &x);
  return x;
}

/*
** Decode a floating-point value into an approximate decimal
** representation.
**
** If i_round<=0 then round to -i_round significant digits to the
** the right of the decimal point, or to a maximum of mx_round total
** significant digits.
**
** If i_round>0 round to min(i_round,mx_round) significant digits total.
**
** mx_round must be positive.
**
** The significant digits of the decimal representation are
** stored in p->z[] which is a often (but not always) a pointer
** into the middle of p->buf[].  There are p->n significant digits.
** The p->z[] array is *not* zero-terminated.
*/
void __fp_decode(fp_decode_t *p, double r, int i_round, int mx_round){
  int i;               /* index_t into buf[] where to put next character */
  int n;               /* Number of digits */
  u_int64_t v;               /* mantissa */
  int e, exp = 0;      /* Base-2 and base-10 exponent */
  char *buf;          /* Local alias for p->buf */
  char *z;             /* Local alias for p->z */

  p->is_special = 0;
  assert( mx_round>0 );

  /* Convert negative numbers to positive.  Deal with Infinity, 0.0, and
  ** __na_n. */
  if( r<0.0 ){
    p->sign = '-';
    r = -r;
  }else if( r==0.0 ){
    p->sign = '+';
    p->n = 1;
    p->i_dp = 1;
    p->z = "0";
    return;
  }else{
    p->sign = '+';
  }
  memcpy(&v,&r,8);
  e = (v>>52)&0x7ff;
  if( e==0x7ff ){
    p->is_special = 1 + (v!=0x7ff0000000000000LL);
    p->n = 0;
    p->i_dp = 0;
    p->z = p->buf;
    return;
  }
  v &= 0x000fffffffffffffULL;
  if( e==0 ){
    int nn = __count_leading_zeros(v);
    v <<= nn;
    e = -1074 - nn;
  }else{
    v = (v<<11) | U64_BIT(63);
    e -= 1086;
  }
  __fp2_convert10(v, e, (i_round<=0||i_round>=18)?18:i_round+1, &v, &exp);  

  /* Extract significant digits, start at the right-most slot in p->buf
  ** and working back to the right.  "i" keeps track of the next slot in
  ** which to store a digit. */
  assert( sizeof(p->buf)==DBSQL_U64_DIGITS+1 );
  assert( v>0 );
  buf = p->buf;
#ifdef DBSQL_AVOID_U64_DIVIDE
  i = __u_int64_to_text(v, buf);
#else
  i = DBSQL_U64_DIGITS;
  while( v>=10 ){
    int kk = (v%100)*2;
    assert( TWO_BYTE_ALIGNMENT(&__digit_pairs.a[kk]) );
    assert( TWO_BYTE_ALIGNMENT(&buf[i]) );
    assert( i-2>=0 );
    *(u_int16_t*)(&buf[i-2]) = *(u_int16_t*)&__digit_pairs.a[kk];
    i -= 2;
    v /= 100;
  }
  if( v ){
    assert( v<10 );
    assert( i>0 );
    buf[--i] = v + '0';
  }
#endif /* DBSQL_AVOID_U64_DIVIDE */
  assert( i>=0 && i<DBSQL_U64_DIGITS );
  n = DBSQL_U64_DIGITS - i;  /* Total number of digits extracted */
  assert( n>0 );
  assert( n<=DBSQL_U64_DIGITS );
  p->i_dp = n + exp;
  if( i_round<=0 ){
    i_round = p->i_dp - i_round;
    if( i_round==0 && buf[i]>='5' ){
      i_round = 1;
      buf[--i] = '0';
      n++;
      p->i_dp++;
    }
  }
  z = &buf[i];  /* z points to the first digit */
  if( i_round>0 && (i_round<n || n>mx_round) ){
    if( i_round>mx_round ) i_round = mx_round;
    if( i_round==17 ){
      /* If the precision is exactly 17, which only happens with the "!"
      ** flag (ex: "%!.17g") then try to reduce the precision if that
      ** yields text that will round-trip to the original floating-point.
      ** value.  Thus, for exaple, 49.47 will render as 49.47, rather than
      ** as 49.469999999999999. */
      if( z[15]=='9' && z[14]=='9' ){
        int jj, kk;
        u_int64_t v2;
        for(jj=14; jj>0 && z[jj-1]=='9'; jj--){}
        if( jj==0 ){
          v2 = 1;
        }else{
          v2 = z[0] - '0';
          for(kk=1; kk<jj; kk++) v2 = (v2*10) + z[kk] - '0';
          v2++;
        }
        if( r==__fp10_convert2(v2, exp + n - jj) ){
          i_round = jj+1;
        }
      }else if( p->i_dp>=n || (z[15]=='0' && z[14]=='0' && z[13]=='0') ){
        int jj, kk;
        u_int64_t v2;
        assert( z[0]!='0' );
        for(jj=13; z[jj-1]=='0'; jj--){}
        v2 = z[0] - '0';
        for(kk=1; kk<jj; kk++) v2 = (v2*10) + z[kk] - '0';
        if( r==__fp10_convert2(v2, exp + n - jj) ){
          i_round = jj+1;
        }
      }
    }
    n = i_round;
    if( z[i_round]>='5' ){
      int j = i_round-1;
      while( 1 /*exit-by-break*/ ){
        z[j]++;
        if( z[j]<='9' ) break;
        z[j] = '0';
        if( j==0 ){
          z--;
          z[0] = '1';
          n++;
          p->i_dp++;
          break;
        }else{
          j--;
        }
      }
    }
  }
  assert( n>0 );
  while( z[n-1]=='0' ){
    n--;
    assert( n>0 );
  }
  p->n = n;
  p->z = z;
}

/*
** Try to convert z into an unsigned 32-bit integer.  Return true on
** success and false if there is an error.
**
** Only decimal notation is accepted.
*/
int __get_u_int32(const char *z, u_int32_t *p_i){
  u_int64_t v = 0;
  int i;
  for(i=0; __isdigit(z[i]); i++){
    v = v*10 + z[i] - '0';
    if( v>4294967296LL ){ *p_i = 0; return 0; }
  }
  if( i==0 || z[i]!=0 ){ *p_i = 0; return 0; }
  *p_i = (u_int32_t)v;
  return 1;
}

/*
** The variable-length integer encoding is as follows:
**
** KEY:
**         A = 0xxxxxxx    7 bits of data and one flag bit
**         B = 1xxxxxxx    7 bits of data and one flag bit
**         C = xxxxxxxx    8 bits of data
**
**  7 bits - A
** 14 bits - BA
** 21 bits - BBA
** 28 bits - BBBA
** 35 bits - BBBBA
** 42 bits - BBBBBA
** 49 bits - BBBBBBA
** 56 bits - BBBBBBBA
** 64 bits - BBBBBBBBC
*/

/*
** Write a 64-bit variable-length integer to memory starting at p[0].
** The length of data write will be between 1 and 9 bytes.  The number
** of bytes written is returned.
**
** A variable-length integer consists of the lower 7 bits of each byte
** for all bytes that have the 8th bit set and one byte with the 8th
** bit clear.  Except, if we get to the 9th byte, it stores the full
** 8 bits and is the last byte.
*/
static int DBSQL_NOINLINE __put_varint64(unsigned char *p, u_int64_t v){
  int i, j, n;
  u_int8_t buf[10];
  if( v & (((u_int64_t)0xff000000)<<32) ){
    p[8] = (u_int8_t)v;
    v >>= 8;
    for(i=7; i>=0; i--){
      p[i] = (u_int8_t)((v & 0x7f) | 0x80);
      v >>= 7;
    }
    return 9;
  }   
  n = 0;
  do{
    buf[n++] = (u_int8_t)((v & 0x7f) | 0x80);
    v >>= 7;
  }while( v!=0 );
  buf[0] &= 0x7f;
  assert( n<=9 );
  for(i=0, j=n-1; j>=0; j--, i++){
    p[i] = buf[j];
  }
  return n;
}
int __put_varint(unsigned char *p, u_int64_t v){
  if( v<=0x7f ){
    p[0] = v&0x7f;
    return 1;
  }
  if( v<=0x3fff ){
    p[0] = ((v>>7)&0x7f)|0x80;
    p[1] = v&0x7f;
    return 2;
  }
  return __put_varint64(p,v);
}

/*
** Bitmasks used by __get_varint().  These precomputed constants
** are defined here rather than simply putting the constant expressions
** inline in order to work around bugs in the RVT compiler.
**
** SLOT_2_0     A mask for  (0x7f<<14) | 0x7f
**
** SLOT_4_2_0   A mask for  (0x7f<<28) | SLOT_2_0
*/
#define SLOT_2_0     0x001fc07f
#define SLOT_4_2_0   0xf01fc07f


/*
** Read a 64-bit variable-length integer from memory starting at p[0].
** Return the number of bytes read.  The value is stored in *v.
*/
u_int8_t __get_varint(const unsigned char *p, u_int64_t *v){
  u_int32_t a,b,s;

  if( ((signed char*)p)[0]>=0 ){
    *v = *p;
    return 1;
  }
  if( ((signed char*)p)[1]>=0 ){
    *v = ((u_int32_t)(p[0]&0x7f)<<7) | p[1];
    return 2;
  }

  /* Verify that constants are precomputed correctly */
  assert( SLOT_2_0 == ((0x7f<<14) | (0x7f)) );
  assert( SLOT_4_2_0 == ((0xfU<<28) | (0x7f<<14) | (0x7f)) );

  a = ((u_int32_t)p[0])<<14;
  b = p[1];
  p += 2;
  a |= *p;
  /* a: p0<<14 | p2 (unmasked) */
  if (!(a&0x80))
  {
    a &= SLOT_2_0;
    b &= 0x7f;
    b = b<<7;
    a |= b;
    *v = a;
    return 3;
  }

  /* CSE1 from below */
  a &= SLOT_2_0;
  p++;
  b = b<<14;
  b |= *p;
  /* b: p1<<14 | p3 (unmasked) */
  if (!(b&0x80))
  {
    b &= SLOT_2_0;
    /* moved CSE1 up */
    /* a &= (0x7f<<14)|(0x7f); */
    a = a<<7;
    a |= b;
    *v = a;
    return 4;
  }

  /* a: p0<<14 | p2 (masked) */
  /* b: p1<<14 | p3 (unmasked) */
  /* 1:save off p0<<21 | p1<<14 | p2<<7 | p3 (masked) */
  /* moved CSE1 up */
  /* a &= (0x7f<<14)|(0x7f); */
  b &= SLOT_2_0;
  s = a;
  /* s: p0<<14 | p2 (masked) */

  p++;
  a = a<<14;
  a |= *p;
  /* a: p0<<28 | p2<<14 | p4 (unmasked) */
  if (!(a&0x80))
  {
    /* we can skip these cause they were (effectively) done above
    ** while calculating s */
    /* a &= (0x7f<<28)|(0x7f<<14)|(0x7f); */
    /* b &= (0x7f<<14)|(0x7f); */
    b = b<<7;
    a |= b;
    s = s>>18;
    *v = ((u_int64_t)s)<<32 | a;
    return 5;
  }

  /* 2:save off p0<<21 | p1<<14 | p2<<7 | p3 (masked) */
  s = s<<7;
  s |= b;
  /* s: p0<<21 | p1<<14 | p2<<7 | p3 (masked) */

  p++;
  b = b<<14;
  b |= *p;
  /* b: p1<<28 | p3<<14 | p5 (unmasked) */
  if (!(b&0x80))
  {
    /* we can skip this cause it was (effectively) done above in calc'ing s */
    /* b &= (0x7f<<28)|(0x7f<<14)|(0x7f); */
    a &= SLOT_2_0;
    a = a<<7;
    a |= b;
    s = s>>18;
    *v = ((u_int64_t)s)<<32 | a;
    return 6;
  }

  p++;
  a = a<<14;
  a |= *p;
  /* a: p2<<28 | p4<<14 | p6 (unmasked) */
  if (!(a&0x80))
  {
    a &= SLOT_4_2_0;
    b &= SLOT_2_0;
    b = b<<7;
    a |= b;
    s = s>>11;
    *v = ((u_int64_t)s)<<32 | a;
    return 7;
  }

  /* CSE2 from below */
  a &= SLOT_2_0;
  p++;
  b = b<<14;
  b |= *p;
  /* b: p3<<28 | p5<<14 | p7 (unmasked) */
  if (!(b&0x80))
  {
    b &= SLOT_4_2_0;
    /* moved CSE2 up */
    /* a &= (0x7f<<14)|(0x7f); */
    a = a<<7;
    a |= b;
    s = s>>4;
    *v = ((u_int64_t)s)<<32 | a;
    return 8;
  }

  p++;
  a = a<<15;
  a |= *p;
  /* a: p4<<29 | p6<<15 | p8 (unmasked) */

  /* moved CSE2 up */
  /* a &= (0x7f<<29)|(0x7f<<15)|(0xff); */
  b &= SLOT_2_0;
  b = b<<8;
  a |= b;

  s = s<<4;
  b = p[-4];
  b &= 0x7f;
  b = b>>3;
  s |= b;

  *v = ((u_int64_t)s)<<32 | a;

  return 9;
}

/*
** Read a 32-bit variable-length integer from memory starting at p[0].
** Return the number of bytes read.  The value is stored in *v.
**
** If the varint stored in p[0] is larger than can fit in a 32-bit unsigned
** integer, then set *v to 0xffffffff.
**
** A MACRO version, GET_VARINT32, is provided which inlines the
** single-byte case.  All code should use the MACRO version as
** this function assumes the single-byte case has already been handled.
*/
u_int8_t __get_varint32(const unsigned char *p, u_int32_t *v){
  u_int64_t v64;
  u_int8_t n;

  /* Assume that the single-byte case has already been handled by
  ** the GET_VARINT32() macro */
  assert( (p[0] & 0x80)!=0 );

  if( (p[1] & 0x80)==0 ){
    /* This is the two-byte case */
    *v = ((p[0]&0x7f)<<7) | p[1];
    return 2;
  }
  if( (p[2] & 0x80)==0 ){
    /* This is the three-byte case */
    *v = ((p[0]&0x7f)<<14) | ((p[1]&0x7f)<<7) | p[2];
    return 3;
  }
  /* four or more bytes */
  n = __get_varint(p, &v64);
  assert( n>3 && n<=9 );
  if( (v64 & DBSQL_MAX_U32)!=v64 ){
    *v = 0xffffffff;
  }else{
    *v = (u_int32_t)v64;
  }
  return n;
}

/*
** Return the number of bytes that will be needed to store the given
** 64-bit integer.
*/
int __varint_len(u_int64_t v){
  int i;
  for(i=1; (v >>= 7)!=0; i++){ assert( i<10 ); }
  return i;
}


/*
** Read or write a four-byte big-endian integer value.
*/
u_int32_t __get4byte(const u_int8_t *p){
#if DBSQL_BYTEORDER==4321
  u_int32_t x;
  memcpy(&x,p,4);
  return x;
#elif DBSQL_BYTEORDER==1234 && GCC_VERSION>=4003000
  u_int32_t x;
  memcpy(&x,p,4);
  return __builtin_bswap32(x);
#elif DBSQL_BYTEORDER==1234 && MSVC_VERSION>=1300
  u_int32_t x;
  memcpy(&x,p,4);
  return _byteswap_ulong(x);
#else
  testcase( p[0]&0x80 );
  return ((unsigned)p[0]<<24) | (p[1]<<16) | (p[2]<<8) | p[3];
#endif
}
void __put4byte(unsigned char *p, u_int32_t v){
#if DBSQL_BYTEORDER==4321
  memcpy(p,&v,4);
#elif DBSQL_BYTEORDER==1234 && GCC_VERSION>=4003000
  u_int32_t x = __builtin_bswap32(v);
  memcpy(p,&x,4);
#elif DBSQL_BYTEORDER==1234 && MSVC_VERSION>=1300
  u_int32_t x = _byteswap_ulong(v);
  memcpy(p,&x,4);
#else
  p[0] = (u_int8_t)(v>>24);
  p[1] = (u_int8_t)(v>>16);
  p[2] = (u_int8_t)(v>>8);
  p[3] = (u_int8_t)v;
#endif
}



/*
** Translate a single byte of Hex into an integer.
** This routine only works if h really is a valid hexadecimal
** character:  0..9a..f_a..F
*/
u_int8_t __hex_to_int(int h){
  assert( (h>='0' && h<='9') ||  (h>='a' && h<='f') ||  (h>='A' && h<='F') );
#ifdef DBSQL_ASCII
  h += 9*(1&(h>>6));
#endif
#ifdef DBSQL_EBCDIC
  h += 9*(1&~(h>>4));
#endif
  return (u_int8_t)(h & 0xf);
}

#if !defined(DBSQL_OMIT_BLOB_LITERAL)
/*
** Convert a BLOB literal of the form "x'hhhhhh'" into its binary
** value.  Return a pointer to its binary value.  Space to hold the
** binary value has been obtained from malloc and must be freed by
** the calling routine.
*/
void *__hex_to_blob(DBSQL *db, const char *z, int n){
  char *blob;
  int i;

  blob = (char *)__db_malloc_raw_nn(db, n/2 + 1);
  n--;
  if( blob ){
    for(i=0; i<n; i+=2){
      blob[i/2] = (__hex_to_int(z[i])<<4) | __hex_to_int(z[i+1]);
    }
    blob[i/2] = 0;
  }
  return blob;
}
#endif /* !DBSQL_OMIT_BLOB_LITERAL */

/*
** Log an error that is an API call on a connection pointer that should
** not have been used.  The "type" of connection pointer is given as the
** argument.  The type is a word like "NULL" or "closed" or "invalid".
*/
static void __log_bad_connection(const char *type){
  dbsql_log(DBSQL_MISUSE,
     "API call with %s database connection pointer",
     type
  );
}

/*
** Check to make sure we have a valid db pointer.  This test is not
** foolproof but it does provide some measure of protection against
** misuse of the interface such as passing in db pointers that are
** NULL or which have been previously closed.  If this routine returns
** 1 it means that the db pointer is valid and 0 if it should not be
** dereferenced for any reason.  The calling function should invoke
** DBSQL_MISUSE immediately.
**
** __safety_check_ok() requires that the db pointer be valid for
** use.  __safety_check_sick_or_ok() allows a db pointer that failed to
** open properly and is not fit for general use but which can be
** used as an argument to dbsql_errmsg() or dbsql_close().
*/
int __safety_check_ok(DBSQL *db){
  u_int8_t open_state;
  if( db==0 ){
    __log_bad_connection("NULL");
    return 0;
  }
  open_state = db->open_state;
  if( open_state!=DBSQL_STATE_OPEN ){
    if( __safety_check_sick_or_ok(db) ){
      testcase( __global_config.x_log!=0 );
      __log_bad_connection("unopened");
    }
    return 0;
  }else{
    return 1;
  }
}
int __safety_check_sick_or_ok(DBSQL *db){
  u_int8_t open_state;
  open_state = db->open_state;
  if( open_state!=DBSQL_STATE_SICK &&
      open_state!=DBSQL_STATE_OPEN &&
      open_state!=DBSQL_STATE_BUSY ){
    testcase( __global_config.x_log!=0 );
    __log_bad_connection("invalid");
    return 0;
  }else{
    return 1;
  }
}

/*
** Attempt to add, subtract, or multiply the 64-bit signed value i_b against
** the other 64-bit signed integer at *a and store the result in *a.
** Return 0 on success.  Or if the operation would have resulted in an
** overflow, leave *a unchanged and return 1.
*/
int __add_int64(int64_t *a, int64_t i_b){
#if GCC_VERSION>=5004000 && !defined(__INTEL_COMPILER)
  return __builtin_add_overflow(*a, i_b, a);
#else
  int64_t i_a = *a;
  testcase( i_a==0 ); testcase( i_a==1 );
  testcase( i_b==-1 ); testcase( i_b==0 );
  if( i_b>=0 ){
    testcase( i_a>0 && LARGEST_INT64 - i_a == i_b );
    testcase( i_a>0 && LARGEST_INT64 - i_a == i_b - 1 );
    if( i_a>0 && LARGEST_INT64 - i_a < i_b ) return 1;
  }else{
    testcase( i_a<0 && -(i_a + LARGEST_INT64) == i_b + 1 );
    testcase( i_a<0 && -(i_a + LARGEST_INT64) == i_b + 2 );
    if( i_a<0 && -(i_a + LARGEST_INT64) > i_b + 1 ) return 1;
  }
  *a += i_b;
  return 0;
#endif
}
int __sub_int64(int64_t *a, int64_t i_b){
#if GCC_VERSION>=5004000 && !defined(__INTEL_COMPILER)
  return __builtin_sub_overflow(*a, i_b, a);
#else
  testcase( i_b==SMALLEST_INT64+1 );
  if( i_b==SMALLEST_INT64 ){
    testcase( (*a)==(-1) ); testcase( (*a)==0 );
    if( (*a)>=0 ) return 1;
    *a -= i_b;
    return 0;
  }else{
    return __add_int64(a, -i_b);
  }
#endif
}
int __mul_int64(int64_t *a, int64_t i_b){
#if GCC_VERSION>=5004000 && !defined(__INTEL_COMPILER)
  return __builtin_mul_overflow(*a, i_b, a);
#else
  int64_t i_a = *a;
  if( i_b>0 ){
    if( i_a>LARGEST_INT64/i_b ) return 1;
    if( i_a<SMALLEST_INT64/i_b ) return 1;
  }else if( i_b<0 ){
    if( i_a>0 ){
      if( i_b<SMALLEST_INT64/i_a ) return 1;
    }else if( i_a<0 ){
      if( i_b==SMALLEST_INT64 ) return 1;
      if( i_a==SMALLEST_INT64 ) return 1;
      if( -i_a>LARGEST_INT64/-i_b ) return 1;
    }
  }
  *a = i_a*i_b;
  return 0;
#endif
}

/*
** Compute the absolute value of a 32-bit signed integer, if possible.  Or
** if the integer has a value of -2147483648, return +2147483647
*/
int __abs_int32(int x){
  if( x>=0 ) return x;
  if( x==(int)0x80000000 ) return 0x7fffffff;
  return -x;
}

#ifdef DBSQL_ENABLE_8_3_NAMES
/*
** If DBSQL_ENABLE_8_3_NAMES is set at compile-time and if the database
** filename in base_filename is a URI with the "8_3_names=1" parameter and
** if filename in z[] has a suffix (a.k.a. "extension") that is longer than
** three characters, then shorten the suffix on z[] to be the last three
** characters of the original suffix.
**
** If DBSQL_ENABLE_8_3_NAMES is set to 2 at compile-time, then always
** do the suffix shortening regardless of URI parameter.
**
** Examples:
**
**     test.db-journal    =>   test.nal
**     test.db-wal        =>   test.wal
**     test.db-shm        =>   test.shm
**     test.db-mj7f3319fa =>   test.9fa
*/
void __file_suffix3(const char *base_filename, char *z){
#if DBSQL_ENABLE_8_3_NAMES<2
  if( dbsql_uri_boolean(base_filename, "8_3_names", 0) )
#endif
  {
    int i, sz;
    sz = __strlen30(z);
    for(i=sz-1; i>0 && z[i]!='/' && z[i]!='.'; i--){}
    if( z[i]=='.' && ALWAYS(sz>i+4) ) memmove(&z[i+1], &z[sz-3], 4);
  }
}
#endif

/*
** Find (an approximate) sum of two log_est_t values.  This computation is
** not a simple "+" operator because log_est_t is stored as a logarithmic
** value.
**
*/
log_est_t __log_est_add(log_est_t a, log_est_t b){
  static const unsigned char x[] = {
     10, 10,                         /* 0,1 */
      9, 9,                          /* 2,3 */
      8, 8,                          /* 4,5 */
      7, 7, 7,                       /* 6,7,8 */
      6, 6, 6,                       /* 9,10,11 */
      5, 5, 5,                       /* 12-14 */
      4, 4, 4, 4,                    /* 15-18 */
      3, 3, 3, 3, 3, 3,              /* 19-24 */
      2, 2, 2, 2, 2, 2, 2,           /* 25-31 */
  };
  if( a>=b ){
    if( a>b+49 ) return a;
    if( a>b+31 ) return a+1;
    return a+x[a-b];
  }else{
    if( b>a+49 ) return b;
    if( b>a+31 ) return b+1;
    return b+x[b-a];
  }
}

/*
** Convert an integer into a log_est_t.  In other words, compute an
** approximation for 10*log2(x).
*/
log_est_t __log_est(u_int64_t x){
  static log_est_t a[] = { 0, 2, 3, 5, 6, 7, 8, 9 };
  log_est_t y = 40;
  if( x<8 ){
    if( x<2 ) return 0;
    while( x<8 ){  y -= 10; x <<= 1; }
  }else{
#if GCC_VERSION>=5004000
    int i = 60 - __builtin_clzll(x);
    y += i*10;
    x >>= i;
#else
    while( x>255 ){ y += 40; x >>= 4; }  /*OPTIMIZATION-IF-TRUE*/
    while( x>15 ){  y += 10; x >>= 1; }
#endif
  }
  return a[x&7] + y - 10;
}

/*
** Convert a double into a log_est_t
** In other words, compute an approximation for 10*log2(x).
*/
log_est_t __log_est_from_double(double x){
  u_int64_t a;
  log_est_t e;
  assert( sizeof(x)==8 && sizeof(a)==8 );
  if( x<=1 ) return 0;
  if( x<=2000000000 ) return __log_est((u_int64_t)x);
  memcpy(&a, &x, 8);
  e = (a>>52) - 1022;
  return e*10;
}

/*
** Convert a log_est_t into an integer.
*/
u_int64_t __log_est_to_int(log_est_t x){
  u_int64_t n;
  n = x%10;
  x /= 10;
  if( n>=5 ) n -= 2;
  else if( n>=1 ) n -= 1;
  if( x>60 ) return (u_int64_t)LARGEST_INT64;
  return x>=3 ? (n+8)<<(x-3) : (n+8)>>(3-x);
}

/*
** Add a new name/number pair to a VList.  This might require that the
** VList object be reallocated, so return the new VList.  If an OOM
** error occurs, the original VList returned and the
** db->malloc_failed flag is set.
**
** A VList is really just an array of integers.  To destroy a VList,
** simply pass it to __dbsql_db_free().
**
** The first integer is the number of integers allocated for the whole
** VList.  The second integer is the number of integers actually used.
** Each name/number pair is encoded by subsequent groups of 3 or more
** integers.
**
** Each name/number pair starts with two integers which are the numeric
** value for the pair and the size of the name/number pair, respectively.
** The text name overlays one or more following integers.  The text name
** is always zero-terminated.
**
** Conceptually:
**
**    struct VList {
**      int n_alloc;   // Number of allocated slots
**      int n_used;    // Number of used slots
**      struct v_list_entry_t {
**        int i_value;    // Value for this entry
**        int n_slot;     // Slots used by this entry
**        // ... variable name goes here
**      } a[0];
**    }
**
** During code generation, pointers to the variable names within the
** VList are taken.  When that happens, n_alloc is set to zero as an
** indication that the VList may never again be enlarged, since the
** accompanying realloc() would invalidate the pointers.
*/
vlist_t *__v_list_add(
  DBSQL *db,           /* The database connection used for malloc() */
  vlist_t *in,            /* The input VList.  Might be NULL */
  const char *name,     /* Name of symbol to add */
  int n_name,             /* Bytes of text in name */
  int i_val               /* Value to associate with name */
){
  int n_int;              /* number of sizeof(int) objects needed for name */
  char *z;               /* Pointer to where name will be stored */
  int i;                 /* index_t in in[] where name is stored */

  n_int = n_name/4 + 3;
  assert( in==0 || in[0]>=3 );  /* Verify ok to add new elements */
  if( in==0 || in[1]+n_int > in[0] ){
    /* Enlarge the allocation */
    int64_t n_alloc = (in ? 2*(int64_t)in[0] : 10) + n_int;
    vlist_t *out = __db_realloc(db, in, n_alloc*sizeof(int));
    if( out==0 ) return in;
    if( in==0 ) out[1] = 2;
    in = out;
    in[0] = n_alloc;
  }
  i = in[1];
  in[i] = i_val;
  in[i+1] = n_int;
  z = (char*)&in[i+2];
  in[1] = i+n_int;
  assert( in[1]<=in[0] );
  memcpy(z, name, n_name);
  z[n_name] = 0;
  return in;
}

/*
** Return a pointer to the name of a variable in the given VList that
** has the value i_val.  Or return a NULL if there is no such variable in
** the list
*/
const char *__v_list_num_to_name(vlist_t *in, int i_val){
  int i, mx;
  if( in==0 ) return 0;
  mx = in[1];
  i = 2;
  do{
    if( in[i]==i_val ) return (char*)&in[i+2];
    i += in[i+1];
  }while( i<mx );
  return 0;
}

/*
** Return the number of the variable named name, if it is in VList.
** or return 0 if there is no such variable.
*/
int __v_list_name_to_num(vlist_t *in, const char *name, int n_name){
  int i, mx;
  if( in==0 ) return 0;
  mx = in[1];
  i = 2;
  do{
    const char *z = (const char*)&in[i+2];
    if( strncmp(z,name,n_name)==0 && z[n_name]==0 ) return in[i];
    i += in[i+1];
  }while( i<mx );
  return 0;
}
