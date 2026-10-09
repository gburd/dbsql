/*
** 2002 February 23
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the C-language implementations for many of the SQL
** functions of dbsql.  (Some function, and in particular the date and
** time functions, are implemented separately.)
*/
#include "dbsql_int.h"
#include <stdlib.h>
#include <assert.h>
#ifndef DBSQL_OMIT_FLOATING_POINT
#include <math.h>
#endif
#include "vdbe_int.h"

/*
** Return the collating function associated with a function.
*/
static coll_seq_t *__get_func_coll_seq(dbsql_func_t *context){
  vdbe_op_t *op;
  assert( context->vdbe!=0 );
  op = &context->vdbe->ops[context->i_op-1];
  assert( op->opcode==OP_CollSeq );
  assert( op->p4type==P4_COLLSEQ );
  return op->p4.coll;
}

/*
** Indicate that the accumulator load should be skipped on this
** iteration of the aggregate loop.
*/
static void __skip_accumulator_load(dbsql_func_t *context){
  assert( context->isError<=0 );
  context->isError = -1;
  context->skip_flag = 1;
}

/*
** Implementation of the non-aggregate min() and max() functions
*/
static void __minmax_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int i;
  int mask;    /* 0 for min() or 0xffffffff for max() */
  int i_best;
  coll_seq_t *coll;

  assert( argc>1 );
  mask = dbsql_user_data(context)==0 ? 0 : -1;
  coll = __get_func_coll_seq(context);
  assert( coll );
  assert( mask==-1 || mask==0 );
  i_best = 0;
  if( dbsql_value_type(argv[0])==DBSQL_NULL ) return;
  for(i=1; i<argc; i++){
    if( dbsql_value_type(argv[i])==DBSQL_NULL ) return;
    if( (__mem_compare(argv[i_best], argv[i], coll)^mask)>=0 ){
      testcase( mask==0 );
      i_best = i;
    }
  }
  dbsql_result_value(context, argv[i_best]);
}

/*
** Return the type of the argument.
*/
static void __typeof_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **argv
){
  static const char *types[] = { "integer", "real", "text", "blob", "null" };
  int i = dbsql_value_type(argv[0]) - 1;
  UNUSED_PARAMETER(__not_used);
  assert( i>=0 && i<ARRAY_SIZE(types) );
  assert( DBSQL_INTEGER==1 );
  assert( DBSQL_FLOAT==2 );
  assert( DBSQL_TEXT==3 );
  assert( DBSQL_BLOB==4 );
  assert( DBSQL_NULL==5 );
  /* EVIDENCE-OF: R-01470-60482 The dbsql_value_type(V) interface returns
  ** the datatype code for the initial datatype of the dbsql_value_t object
  ** V. The returned value is one of DBSQL_INTEGER, DBSQL_FLOAT,
  ** DBSQL_TEXT, DBSQL_BLOB, or DBSQL_NULL. */
  dbsql_result_text(context, types[i], -1, DBSQL_STATIC);
}

/* subtype(X)
**
** Return the subtype of X
*/
static void __subtype_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  UNUSED_PARAMETER(argc);
  dbsql_result_int(context, dbsql_value_subtype(argv[0]));
}

/*
** Implementation of the length() function
*/
static void __length_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  switch( dbsql_value_type(argv[0]) ){
    case DBSQL_BLOB:
    case DBSQL_INTEGER:
    case DBSQL_FLOAT: {
      dbsql_result_int(context, dbsql_value_bytes(argv[0]));
      break;
    }
    case DBSQL_TEXT: {
      const unsigned char *z = dbsql_value_text(argv[0]);
      const unsigned char *z0;
      unsigned char c;
      if( z==0 ) return;
      z0 = z;
      while( (c = *z)!=0 ){
        z++;
        if( c>=0xc0 ){
          while( (*z & 0xc0)==0x80 ){ z++; z0++; }
        }
      }
      dbsql_result_int(context, (int)(z-z0));
      break;
    }
    default: {
      dbsql_result_null(context);
      break;
    }
  }
}

/*
** Implementation of the octet_length() function
*/
static void __bytelength_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  switch( dbsql_value_type(argv[0]) ){
    case DBSQL_BLOB: {
      dbsql_result_int(context, dbsql_value_bytes(argv[0]));
      break;
    }
    case DBSQL_INTEGER:
    case DBSQL_FLOAT: {
      int64_t m = dbsql_context_db_handle(context)->enc<=DBSQL_UTF8 ? 1 : 2;
      dbsql_result_int64(context, dbsql_value_bytes(argv[0])*m);
      break;
    }
    case DBSQL_TEXT: {
      if( dbsql_value_encoding(argv[0])<=DBSQL_UTF8 ){
        dbsql_result_int(context, dbsql_value_bytes(argv[0]));
      }else{
        dbsql_result_int(context, dbsql_value_bytes16(argv[0]));
      }
      break;
    }
    default: {
      dbsql_result_null(context);
      break;
    }
  }
}

/*
** Implementation of the abs() function.
**
** IMP: R-23979-26855 The abs(X) function returns the absolute value of
** the numeric argument X.
*/
static void __abs_func(dbsql_func_t *context, int argc, dbsql_value_t **argv){
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  switch( dbsql_value_type(argv[0]) ){
    case DBSQL_INTEGER: {
      int64_t i_val = dbsql_value_int64(argv[0]);
      if( i_val<0 ){
        if( i_val==SMALLEST_INT64 ){
          /* IMP: R-31676-45509 If X is the integer -9223372036854775808
          ** then abs(X) throws an integer overflow error since there is no
          ** equivalent positive 64-bit two complement value. */
          dbsql_result_error(context, "integer overflow", -1);
          return;
        }
        i_val = -i_val;
      }
      dbsql_result_int64(context, i_val);
      break;
    }
    case DBSQL_NULL: {
      /* IMP: R-37434-19929 Abs(X) returns NULL if X is NULL. */
      dbsql_result_null(context);
      break;
    }
    default: {
      /* Because dbsql_value_double() returns 0.0 if the argument is not
      ** something that can be converted into a number, we have:
      ** IMP: R-01992-00519 Abs(X) returns 0.0 if X is a string or blob
      ** that cannot be converted to a numeric value.
      */
      double r_val = dbsql_value_double(argv[0]);
      if( r_val<0 ) r_val = -r_val;
      dbsql_result_double(context, r_val);
      break;
    }
  }
}

/*
** Implementation of the instr() function.
**
** instr(haystack,needle) finds the first occurrence of needle
** in haystack and returns the number of previous characters plus 1,
** or 0 if needle does not occur within haystack.
**
** If both haystack and needle are BLOBs, then the result is one more than
** the number of bytes in haystack prior to the first occurrence of needle,
** or 0 if needle never occurs in haystack.
*/
static void __instr_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const unsigned char *haystack;
  const unsigned char *needle;
  int n_haystack;
  int n_needle;
  int type_haystack, type_needle;
  int N = 1;
  int isText;
  unsigned char first_char;
  dbsql_value_t *c1 = 0;
  dbsql_value_t *c2 = 0;

  UNUSED_PARAMETER(argc);
  type_haystack = dbsql_value_type(argv[0]);
  type_needle = dbsql_value_type(argv[1]);
  if( type_haystack==DBSQL_NULL || type_needle==DBSQL_NULL ) return;
  n_haystack = dbsql_value_bytes(argv[0]);
  n_needle = dbsql_value_bytes(argv[1]);
  if( n_needle>0 ){
    if( type_haystack==DBSQL_BLOB && type_needle==DBSQL_BLOB ){
      haystack = dbsql_value_blob(argv[0]);
      needle = dbsql_value_blob(argv[1]);
      isText = 0;
    }else if( type_haystack!=DBSQL_BLOB && type_needle!=DBSQL_BLOB ){
      haystack = dbsql_value_text(argv[0]);
      needle = dbsql_value_text(argv[1]);
      isText = 1;
    }else{
      c1 = dbsql_value_dup(argv[0]);
      haystack = dbsql_value_text(c1);
      if( haystack==0 ) goto end_instr_oom;
      n_haystack = dbsql_value_bytes(c1);
      c2 = dbsql_value_dup(argv[1]);
      needle = dbsql_value_text(c2);
      if( needle==0 ) goto end_instr_oom;
      n_needle = dbsql_value_bytes(c2);
      isText = 1;
    }
    if( needle==0 || (n_haystack && haystack==0) ) goto end_instr_oom;
    first_char = needle[0];
    while( n_needle<=n_haystack
       && (haystack[0]!=first_char || memcmp(haystack, needle, n_needle)!=0)
    ){
      N++;
      do{
        n_haystack--;
        haystack++;
      }while( isText && (haystack[0]&0xc0)==0x80 );
    }
    if( n_needle>n_haystack ) N = 0;
  }
  dbsql_result_int(context, N);
end_instr:
  dbsql_value_free(c1);
  dbsql_value_free(c2);
  return;
end_instr_oom:
  dbsql_result_error_nomem(context);
  goto end_instr;
}

/*
** Implementation of the printf() (a.k.a. format()) SQL function.
*/
static void __printf_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  printf_arguments_t x;
  str_accum_t str;
  const char *format;
  int n;
  DBSQL *db = dbsql_context_db_handle(context);

  if( argc>=1 && (format = (const char*)dbsql_value_text(argv[0]))!=0 ){
    x.n_arg = argc-1;
    x.n_used = 0;
    x.args = argv+1;
    __str_accum_init(&str, db, 0, 0, db->limits[DBSQL_LIMIT_LENGTH]);
    str.printf_flags = DBSQL_PRINTF_SQLFUNC;
    dbsql_str_appendf(&str, format, &x);
    if( str.acc_error==DBSQL_OK ){
      n = str.n_char;
      dbsql_result_text(context, __str_accum_finish(&str), n,
                          DBSQL_DYNAMIC);
    }else{
      if( str.acc_error==DBSQL_NOMEM ){
        dbsql_result_error_nomem(context);
      }else{
        dbsql_result_error_toobig(context);
      }
      dbsql_str_reset(&str);
    }
  }
}

/*
** Implementation of the substr() function.
**
** substr(x,p1,p2)  returns p2 characters of x[] beginning with p1.
** p1 is 1-indexed.  So substr(x,1,1) returns the first character
** of x.  If x is text, then we actually count UTF-8 characters.
** If x is a blob, then we count bytes.
**
** If p1 is negative, then we begin abs(p1) from the end of x[].
**
** If p2 is negative, return the p2 characters preceding p1.
*/
static void __substr_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const unsigned char *z;
  const unsigned char *z2;
  int len;
  int p0type;
  int64_t p1, p2;

  assert( argc==3 || argc==2 );
  p0type = dbsql_value_type(argv[0]);
  p1 = dbsql_value_int64(argv[1]);
  if( p0type==DBSQL_BLOB ){
    len = dbsql_value_bytes(argv[0]);
    z = dbsql_value_blob(argv[0]);
    if( z==0 ) return;
    assert( len==dbsql_value_bytes(argv[0]) );
  }else{
    z = dbsql_value_text(argv[0]);
    if( z==0 ) return;
    len = 0;
    if( p1<0 ){
      for(z2=z; *z2; len++){
        DBSQL_SKIP_UTF8(z2);
      }
    }
  }
  if( argc==3 ){
    p2 = dbsql_value_int64(argv[2]);
    if( p2==0 && dbsql_value_type(argv[2])==DBSQL_NULL ) return;
  }else{
    p2 = dbsql_context_db_handle(context)->limits[DBSQL_LIMIT_LENGTH];
  }
  if( p1==0 ){
#ifdef DBSQL_SUBSTR_COMPATIBILITY
    /* If SUBSTR_COMPATIBILITY is defined then substr(X,0,N) work the same as
    ** as substr(X,1,N) - it returns the first N characters of X.  This
    ** is essentially a back-out of the bug-fix in check-in [5fc125d362df4b8]
    ** from 2009-02-02 for compatibility of applications that exploited the
    ** old buggy behavior. */
    p1 = 1; /* <rdar://problem/6778339> */
#endif
    if( dbsql_value_type(argv[1])==DBSQL_NULL ) return;
  }
  if( p1<0 ){
    p1 += len;
    if( p1<0 ){
      if( p2<0 ){
        p2 = 0;
      }else{
        p2 += p1;
      }
      p1 = 0;
    }
  }else if( p1>0 ){
    p1--;
  }else if( p2>0 ){
    p2--;
  }
  if( p2<0 ){
    if( p2<-p1 ){
      p2 = p1;
    }else{
      p2 = -p2;
    }
    p1 -= p2;
  }
  assert( p1>=0 && p2>=0 );
  if( p0type!=DBSQL_BLOB ){
    while( *z && p1 ){
      DBSQL_SKIP_UTF8(z);
      p1--;
    }
    for(z2=z; *z2 && p2; p2--){
      DBSQL_SKIP_UTF8(z2);
    }
    dbsql_result_text64(context, (char*)z, z2-z, DBSQL_TRANSIENT,
                          DBSQL_UTF8);
  }else{
    if( p1>=len ){
      p1 = p2 = 0;
    }else if( p2>len-p1 ){
      p2 = len-p1;
      assert( p2>0 );
    }
    dbsql_result_blob64(context, (char*)&z[p1], (u_int64_t)p2, DBSQL_TRANSIENT);
  }
}

/*
** Implementation of the round() function
*/
#ifndef DBSQL_OMIT_FLOATING_POINT
static void __round_func(dbsql_func_t *context, int argc, dbsql_value_t **argv){
  int64_t n = 0;
  double r;
  char *buf;
  assert( argc==1 || argc==2 );
  if( argc==2 ){
    if( DBSQL_NULL==dbsql_value_type(argv[1]) ) return;
    n = dbsql_value_int64(argv[1]);
    if( n>30 ) n = 30;
    if( n<0 ) n = 0;
  }
  if( dbsql_value_type(argv[0])==DBSQL_NULL ) return;
  r = dbsql_value_double(argv[0]);
  /* If Y==0 and X will fit in a 64-bit int,
  ** handle the rounding directly,
  ** otherwise use printf.
  */
  if( r<-4503599627370496.0 || r>+4503599627370496.0 ){
    /* The value has no fractional part so there is nothing to round */
  }else if( n==0 ){ 
    r = (double)((int64_t)(r+(r<0?-0.5:+0.5)));
  }else{
    buf = dbsql_mprintf("%!.*f",(int)n,r);
    if( buf==0 ){
      dbsql_result_error_nomem(context);
      return;
    }
    __ato_f(buf, &r);
    dbsql_free(buf);
  }
  dbsql_result_double(context, r);
}
#endif

/*
** Allocate n_byte bytes of space using __dbsql_malloc(). If the
** allocation fails, call dbsql_result_error_nomem() to notify
** the database handle that malloc() has failed and return NULL.
** If n_byte is larger than the maximum string or blob length, then
** raise an DBSQL_TOOBIG exception and return NULL.
*/
static void *__context_malloc(dbsql_func_t *context, int64_t n_byte){
  char *z;
  DBSQL *db = dbsql_context_db_handle(context);
  assert( n_byte>0 );
  testcase( n_byte==db->limits[DBSQL_LIMIT_LENGTH] );
  testcase( n_byte==(int64_t)db->limits[DBSQL_LIMIT_LENGTH]+1 );
  if( n_byte>db->limits[DBSQL_LIMIT_LENGTH] ){
    dbsql_result_error_toobig(context);
    z = 0;
  }else{
    z = __dbsql_malloc(n_byte);
    if( !z ){
      dbsql_result_error_nomem(context);
    }
  }
  return z;
}

/*
** Implementation of the upper() and lower() SQL functions.
*/
static void __upper_func(dbsql_func_t *context, int argc, dbsql_value_t **argv){
  char *z1;
  const char *z2;
  int i, n;
  UNUSED_PARAMETER(argc);
  z2 = (char*)dbsql_value_text(argv[0]);
  n = dbsql_value_bytes(argv[0]);
  /* Verify that the call to _bytes() does not invalidate the _text() pointer */
  assert( z2==(char*)dbsql_value_text(argv[0]) );
  if( z2 ){
    z1 = __context_malloc(context, ((int64_t)n)+1);
    if( z1 ){
      for(i=0; i<n; i++){
        z1[i] = (char)__dbsql_toupper(z2[i]);
      }
      dbsql_result_text(context, z1, n, dbsql_free);
    }
  }
}
static void __lower_func(dbsql_func_t *context, int argc, dbsql_value_t **argv){
  char *z1;
  const char *z2;
  int i, n;
  UNUSED_PARAMETER(argc);
  z2 = (char*)dbsql_value_text(argv[0]);
  n = dbsql_value_bytes(argv[0]);
  /* Verify that the call to _bytes() does not invalidate the _text() pointer */
  assert( z2==(char*)dbsql_value_text(argv[0]) );
  if( z2 ){
    z1 = __context_malloc(context, ((int64_t)n)+1);
    if( z1 ){
      for(i=0; i<n; i++){
        z1[i] = __dbsql_tolower(z2[i]);
      }
      dbsql_result_text(context, z1, n, dbsql_free);
    }
  }
}

/*
** Some functions like COALESCE() and IFNULL() and UNLIKELY() are implemented
** as VDBE code so that unused argument values do not have to be computed.
** However, we still need some kind of function implementation for this
** routines in the function table.  The NOOP_FUNC macro provides this.
** NOOP_FUNC will never be called so it doesn't matter what the implementation
** is.  We might as well use the "version()" function as a substitute.
*/
#define NOOP_FUNC __version_func   /* Substitute function - never called */

/*
** Implementation of random().  Return a random integer. 
*/
static void __random_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **__not_used2
){
  int64_t r;
  UNUSED_PARAMETER2(__not_used, __not_used2);
  dbsql_randomness(sizeof(r), &r);
  if( r<0 ){
    /* We need to prevent a random number of 0x8000000000000000
    ** (or -9223372036854775808) since when you do abs() of that
    ** number of you get the same value back again.  To do this
    ** in a way that is testable, mask the sign bit off of negative
    ** values, resulting in a positive value.  Then take the
    ** 2s complement of that positive value.  The end result can
    ** therefore be no less than -9223372036854775807.
    */
    r = -(r & LARGEST_INT64);
  }
  dbsql_result_int64(context, r);
}

/*
** Implementation of randomblob(N).  Return a random blob
** that is N bytes long.
*/
static void __random_blob(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int64_t n;
  unsigned char *p;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  n = dbsql_value_int64(argv[0]);
  if( n<1 ){
    n = 1;
  }
  p = __context_malloc(context, n);
  if( p ){
    dbsql_randomness(n, p);
    dbsql_result_blob(context, (char*)p, n, dbsql_free);
  }
}

/*
** Implementation of the last_insert_rowid() SQL function.  The return
** value is the same as the dbsql_last_insert_rowid() API function.
*/
static void last_insert_rowid(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **__not_used2
){
  DBSQL *db = dbsql_context_db_handle(context);
  UNUSED_PARAMETER2(__not_used, __not_used2);
  /* IMP: R-51513-12026 The last_insert_rowid() SQL function is a
  ** wrapper around the dbsql_last_insert_rowid() C/C++ interface
  ** function. */
  dbsql_result_int64(context, dbsql_last_insert_rowid(db));
}

/*
** Implementation of the changes() SQL function.
**
** IMP: R-32760-32347 The changes() SQL function is a wrapper
** around the dbsql_changes64() C/C++ function and hence follows the
** same rules for counting changes.
*/
static void changes(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **__not_used2
){
  DBSQL *db = dbsql_context_db_handle(context);
  UNUSED_PARAMETER2(__not_used, __not_used2);
  dbsql_result_int64(context, dbsql_changes64(db));
}

/*
** Implementation of the total_changes() SQL function.  The return value is
** the same as the dbsql_total_changes64() API function.
*/
static void total_changes(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **__not_used2
){
  DBSQL *db = dbsql_context_db_handle(context);
  UNUSED_PARAMETER2(__not_used, __not_used2);
  /* IMP: R-11217-42568 This function is a wrapper around the
  ** dbsql_total_changes64() C/C++ interface. */
  dbsql_result_int64(context, dbsql_total_changes64(db));
}

/*
** A structure defining how to do GLOB-style comparisons.
*/
struct compare_info {
  u_int8_t match_all;          /* "*" or "%" */
  u_int8_t match_one;          /* "?" or "_" */
  u_int8_t matchSet;          /* "[" or 0 */
  u_int8_t no_case;            /* true to ignore case differences */
};

/*
** For LIKE and GLOB matching on EBCDIC machines, assume that every
** character is exactly one byte in size.  Also, provide the UTF8_READ()
** macro for fast reading of the next character in the common case where
** the next character is ASCII.
*/
#if defined(DBSQL_EBCDIC)
# define __utf8_read(A)        (*((*A)++))
# define UTF8_READ(A)               (*(A++))
#else
# define UTF8_READ(A)               (A[0]<0x80?*(A++):__utf8_read(&A))
#endif

static const struct compare_info __glob_info = { '*', '?', '[', 0 };
/* The correct SQL-92 behavior is for the LIKE operator to ignore
** case.  Thus  'a' LIKE 'A' would be true. */
static const struct compare_info __like_info_norm = { '%', '_',   0, 1 };
/* If DBSQL_CASE_SENSITIVE_LIKE is defined, then the LIKE operator
** is case sensitive causing 'a' LIKE 'A' to be false */
static const struct compare_info __like_info_alt = { '%', '_',   0, 0 };

/*
** Possible error returns from pattern_match()
*/
#define DBSQL_MATCH             0
#define DBSQL_NOMATCH           1
#define DBSQL_NOWILDCARDMATCH   2

/*
** Compare two UTF-8 strings for equality where the first string is
** a GLOB or LIKE expression.  Return values:
**
**    DBSQL_MATCH:            Match
**    DBSQL_NOMATCH:          No match
**    DBSQL_NOWILDCARDMATCH:  No match in spite of having * or % wildcards.
**
** Globbing rules:
**
**      '*'       Matches any sequence of zero or more characters.
**
**      '?'       Matches exactly one character.
**
**     [...]      Matches one character from the enclosed list of
**                characters.
**
**     [^...]     Matches one character not in the enclosed list.
**
** with_t the [...] and [^...] matching, a ']' character can be included
** in the list by making it the first character after '[' or '^'.  A
** range of characters can be specified using '-'.  Example:
** "[a-z]" matches any single lower-case letter.  To match a '-', make
** it the last character in the list.
**
** Like matching rules:
**
**      '%'       Matches any sequence of zero or more characters
**
***     '_'       Matches any one character
**
**      Ec        Where E is the "esc" character and c is any other
**                character, including '%', '_', and esc, match exactly c.
**
** The comments within this routine usually assume glob matching.
**
** This routine is usually quick, but can be N**2 in the worst case.
*/
static int __pattern_compare(
  const u_int8_t *pattern,              /* The glob pattern */
  const u_int8_t *string,               /* The string to compare against the glob */
  const struct compare_info *info, /* Information about how to do the compare */
  u_int32_t match_other                   /* The escape char (LIKE) or '[' (GLOB) */
){
  u_int32_t c, c2;                       /* Next pattern and input string chars */
  u_int32_t match_one = info->match_one;  /* "?" or "_" */
  u_int32_t match_all = info->match_all;  /* "*" or "%" */
  u_int8_t no_case = info->no_case;       /* True if uppercase==lowercase */
  const u_int8_t *escaped = 0;          /* One past the last escaped input char */
 
  while( (c = UTF8_READ(pattern))!=0 ){
    if( c==match_all ){  /* Match "*" */
      /* Skip over multiple "*" characters in the pattern.  If there
      ** are also "?" characters, skip those as well, but consume a
      ** single character of the input string for each "?" skipped */
      while( (c=UTF8_READ(pattern)) == match_all
             || (c == match_one && match_one!=0) ){
        if( c==match_one && __utf8_read(&string)==0 ){
          return DBSQL_NOWILDCARDMATCH;
        }
      }
      if( c==0 ){
        return DBSQL_MATCH;   /* "*" at the end of the pattern matches */
      }else if( c==match_other ){
        if( info->matchSet==0 ){
          c = __utf8_read(&pattern);
          if( c==0 ) return DBSQL_NOWILDCARDMATCH;
        }else{
          /* "[...]" immediately follows the "*".  We have to do a slow
          ** recursive search in this case, but it is an unusual case. */
          assert( match_other<0x80 );  /* '[' is a single-byte character */
          while( *string ){
            int match = __pattern_compare(&pattern[-1],string,info,match_other);
            if( match!=DBSQL_NOMATCH ) return match;
            DBSQL_SKIP_UTF8(string);
          }
          return DBSQL_NOWILDCARDMATCH;
        }
      }

      /* At this point variable c contains the first character of the
      ** pattern string past the "*".  Search in the input string for the
      ** first matching character and recursively continue the match from
      ** that point.
      **
      ** For a case-insensitive search, set variable cx to be the same as
      ** c but in the other case and search the input string for either
      ** c or cx.
      */
      if( c<0x80 ){
        char stop[3];
        int match;
        if( no_case ){
          stop[0] = __dbsql_toupper(c);
          stop[1] = __dbsql_tolower(c);
          stop[2] = 0;
        }else{
          stop[0] = c;
          stop[1] = 0;
        }
        while(1){
          string += strcspn((const char*)string, stop);
          if( string[0]==0 ) break;
          string++;
          match = __pattern_compare(pattern,string,info,match_other);
          if( match!=DBSQL_NOMATCH ) return match;
        }
      }else{
        int match;
        while( (c2 = UTF8_READ(string))!=0 ){
          if( c2!=c ) continue;
          match = __pattern_compare(pattern,string,info,match_other);
          if( match!=DBSQL_NOMATCH ) return match;
        }
      }
      return DBSQL_NOWILDCARDMATCH;
    }
    if( c==match_other ){
      if( info->matchSet==0 ){
        c = __utf8_read(&pattern);
        if( c==0 ) return DBSQL_NOMATCH;
        escaped = pattern;
      }else{
        u_int32_t prior_c = 0;
        int seen = 0;
        int invert = 0;
        c = __utf8_read(&string);
        if( c==0 ) return DBSQL_NOMATCH;
        c2 = __utf8_read(&pattern);
        if( c2=='^' ){
          invert = 1;
          c2 = __utf8_read(&pattern);
        }
        if( c2==']' ){
          if( c==']' ) seen = 1;
          c2 = __utf8_read(&pattern);
        }
        while( c2 && c2!=']' ){
          if( c2=='-' && pattern[0]!=']' && pattern[0]!=0 && prior_c>0 ){
            c2 = __utf8_read(&pattern);
            if( c>=prior_c && c<=c2 ) seen = 1;
            prior_c = 0;
          }else{
            if( c==c2 ){
              seen = 1;
            }
            prior_c = c2;
          }
          c2 = __utf8_read(&pattern);
        }
        if( c2==0 || (seen ^ invert)==0 ){
          return DBSQL_NOMATCH;
        }
        continue;
      }
    }
    c2 = UTF8_READ(string);
    if( c==c2 ) continue;
    if( no_case  && __dbsql_tolower(c)==__dbsql_tolower(c2) && c<0x80 && c2<0x80 ){
      continue;
    }
    if( c==match_one && pattern!=escaped && c2!=0 ) continue;
    return DBSQL_NOMATCH;
  }
  return *string==0 ? DBSQL_MATCH : DBSQL_NOMATCH;
}

/*
** The dbsql_strglob() interface.  Return 0 on a match (like strcmp()) and
** non-zero if there is no match.
*/
int dbsql_strglob(const char *glob_pattern, const char *string){
  if( string==0 ){
    return glob_pattern!=0;
  }else if( glob_pattern==0 ){
    return 1;
  }else {
    return __pattern_compare((u_int8_t*)glob_pattern, (u_int8_t*)string, &__glob_info, '[');
  }
}

/*
** The dbsql_strlike() interface.  Return 0 on a match and non-zero for
** a miss - like strcmp().
*/
int dbsql_strlike(const char *pattern, const char *str, unsigned int esc){
  if( str==0 ){
    return pattern!=0;
  }else if( pattern==0 ){
    return 1;
  }else{
    return __pattern_compare((u_int8_t*)pattern, (u_int8_t*)str, &__like_info_norm, esc);
  }
}

/*
** Count the number of times that the LIKE operator (or GLOB which is
** just a variation of LIKE) gets called.  This is used for testing
** only.
*/
#ifdef DBSQL_TEST
int dbsql_like_count = 0;
#endif


/*
** Implementation of the like() SQL function.  This function implements
** the built-in LIKE operator.  The first argument to the function is the
** pattern and the second argument is the string.  So, the SQL statements:
**
**       A LIKE B
**
** is implemented as like(B,A).
**
** This same function (with a different compare_info structure) computes
** the GLOB operator.
*/
static void __like_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const unsigned char *a, *b;
  u_int32_t escape;
  int n_pat;
  DBSQL *db = dbsql_context_db_handle(context);
  struct compare_info *info = dbsql_user_data(context);
  struct compare_info backup_info;

#ifdef DBSQL_LIKE_DOESNT_MATCH_BLOBS
  if( dbsql_value_type(argv[0])==DBSQL_BLOB
   || dbsql_value_type(argv[1])==DBSQL_BLOB
  ){
#ifdef DBSQL_TEST
    dbsql_like_count++;
#endif
    dbsql_result_int(context, 0);
    return;
  }
#endif

  /* Limit the length of the LIKE or GLOB pattern to avoid problems
  ** of deep recursion and N*N behavior in __pattern_compare().
  */
  n_pat = dbsql_value_bytes(argv[0]);
  testcase( n_pat==db->limits[DBSQL_LIMIT_LIKE_PATTERN_LENGTH] );
  testcase( n_pat==db->limits[DBSQL_LIMIT_LIKE_PATTERN_LENGTH]+1 );
  if( n_pat > db->limits[DBSQL_LIMIT_LIKE_PATTERN_LENGTH] ){
    dbsql_result_error(context, "LIKE or GLOB pattern too complex", -1);
    return;
  }
  if( argc==3 ){
    /* The escape character string must consist of a single UTF-8 character.
    ** Otherwise, return an error.
    */
    const unsigned char *esc = dbsql_value_text(argv[2]);
    if( esc==0 ) return;
    if( __utf8_char_len((char*)esc, -1)!=1 ){
      dbsql_result_error(context,
          "ESCAPE expression must be a single character", -1);
      return;
    }
    escape = __utf8_read(&esc);
    if( escape==info->match_all || escape==info->match_one ){
      memcpy(&backup_info, info, sizeof(backup_info));
      info = &backup_info;
      if( escape==info->match_all ) info->match_all = 0;
      if( escape==info->match_one ) info->match_one = 0;
    }
  }else{
    escape = info->matchSet;
  }
  b = dbsql_value_text(argv[0]);
  a = dbsql_value_text(argv[1]);
  if( a && b ){
#ifdef DBSQL_TEST
    dbsql_like_count++;
#endif
    dbsql_result_int(context,
                      __pattern_compare(b, a, info, escape)==DBSQL_MATCH);
  }
}

/*
** Implementation of the NULLIF(x,y) function.  The result is the first
** argument if the arguments are different.  The result is NULL if the
** arguments are equal to each other.
*/
static void __nullif_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **argv
){
  coll_seq_t *coll = __get_func_coll_seq(context);
  UNUSED_PARAMETER(__not_used);
  if( __mem_compare(argv[0], argv[1], coll)!=0 ){
    dbsql_result_value(context, argv[0]);
  }
}

/*
** Implementation of the dbsql_version() function.  The result is the version
** of the dbsql library that is running.
*/
static void __version_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **__not_used2
){
  UNUSED_PARAMETER2(__not_used, __not_used2);
  /* IMP: R-48699-48617 This function is an SQL wrapper around the
  ** dbsql_libversion() C-interface. */
  dbsql_result_text(context, dbsql_libversion(), -1, DBSQL_STATIC);
}

/*
** Implementation of the dbsql_source_id() function. The result is a string
** that identifies the particular version of the source code used to build
** dbsql.
*/
static void __sourceid_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **__not_used2
){
  UNUSED_PARAMETER2(__not_used, __not_used2);
  /* IMP: R-24470-31136 This function is an SQL wrapper around the
  ** dbsql_sourceid() C interface. */
  dbsql_result_text(context, dbsql_sourceid(), -1, DBSQL_STATIC);
}

/*
** Implementation of the dbsql_log() function.  This is a wrapper around
** dbsql_log().  The return value is NULL.  The function exists purely for
** its side-effects.
*/
static void __errlog_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(context);
  dbsql_log(dbsql_value_int(argv[0]), "%s", dbsql_value_text(argv[1]));
}

/*
** Implementation of the dbsql_compileoption_used() function.
** The result is an integer that identifies if the compiler option
** was used to build dbsql.
*/
#ifndef DBSQL_OMIT_COMPILEOPTION_DIAGS
static void __compileoptionused_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const char *opt_name;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  /* IMP: R-39564-36305 The dbsql_compileoption_used() SQL
  ** function is a wrapper around the dbsql_compileoption_used() C/C++
  ** function.
  */
  if( (opt_name = (const char*)dbsql_value_text(argv[0]))!=0 ){
    dbsql_result_int(context, dbsql_compileoption_used(opt_name));
  }
}
#endif /* DBSQL_OMIT_COMPILEOPTION_DIAGS */

/*
** Implementation of the dbsql_compileoption_get() function.
** The result is a string that identifies the compiler options
** used to build dbsql.
*/
#ifndef DBSQL_OMIT_COMPILEOPTION_DIAGS
static void __compileoptionget_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int n;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  /* IMP: R-04922-24076 The dbsql_compileoption_get() SQL function
  ** is a wrapper around the dbsql_compileoption_get() C/C++ function.
  */
  n = dbsql_value_int(argv[0]);
  dbsql_result_text(context, dbsql_compileoption_get(n), -1, DBSQL_STATIC);
}
#endif /* DBSQL_OMIT_COMPILEOPTION_DIAGS */

/* Array for converting from half-bytes (nybbles) into ASCII hex
** digits. */
static const char hexdigits[] = {
  '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

/*
** Append to str text that is the SQL literal representation of the
** value contained in value.
*/
void __quote_value(str_accum_t *str, dbsql_value_t *value, int escape){
  /* As currently implemented, the string must be initially empty.
  ** we might relax this requirement in the future, but that will
  ** require enhancements to the implementation. */
  assert( str!=0 && str->n_char==0 );

  switch( dbsql_value_type(value) ){
    case DBSQL_FLOAT: {
                             /*    ,---  Show infinity as 9.0e+999
                             **    |   
                             **    | ,--- 17 precision guarantees round-trip
                             **    v v                                       */
      dbsql_str_appendf(str, "%!0.17g", dbsql_value_double(value));
      break;
    }
    case DBSQL_INTEGER: {
      dbsql_str_appendf(str, "%lld", dbsql_value_int64(value));
      break;
    }
    case DBSQL_BLOB: {
      char const *blob = dbsql_value_blob(value);
      int64_t n_blob = dbsql_value_bytes(value);
      assert( blob==dbsql_value_blob(value) ); /* No encoding change */
      __str_accum_enlarge(str, n_blob*2 + 4);
      if( str->acc_error==0 ){
        char *text = str->text;
        int i;
        for(i=0; i<n_blob; i++){
          text[(i*2)+2] = hexdigits[(blob[i]>>4)&0x0F];
          text[(i*2)+3] = hexdigits[(blob[i])&0x0F];
        }
        text[(n_blob*2)+2] = '\'';
        text[(n_blob*2)+3] = '\0';
        text[0] = 'X';
        text[1] = '\'';
        str->n_char = n_blob*2 + 3;
      }
      break;
    }
    case DBSQL_TEXT: {
      const unsigned char *arg = dbsql_value_text(value);
      dbsql_str_appendf(str, escape ? "%#Q" : "%Q", arg);
      break;
    }
    default: {
      assert( dbsql_value_type(value)==DBSQL_NULL );
      dbsql_str_append(str, "NULL", 4);
      break;
    }
  }
}

/*
** Return true if z[] begins with N hexadecimal digits, and write
** a decoding of those digits into *val.  Or return false if any
** one of the first N characters in z[] is not a hexadecimal digit.
*/
static int __is_n_hex(const char *z, int N, u_int32_t *val){
  int i;
  u_int32_t v = 0;
  for(i=0; i<N; i++){
    if( !__isxdigit(z[i]) ) return 0;
    v = (v<<4) + __hex_to_int(z[i]);
  }
  *val = v;
  return 1;
}

/*
** Implementation of the UNISTR() function.
**
** This is intended to be a work-alike of the UNISTR() function in
** __postgre_sql.  Quoting from the PG documentation (__postgre_sql 17 -
** scraped on 2025-02-22):
**
**    Evaluate escaped Unicode characters in the argument. Unicode
**    characters can be specified as \XXXX (4 hexadecimal digits),
**    \+XXXXXX (6 hexadecimal digits), \u_xxxx (4 hexadecimal digits),
**    or \UXXXXXXXX (8 hexadecimal digits). To specify a backslash,
**    write two backslashes. All other characters are taken literally.
*/
static void __unistr_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  char *out;
  const char *in;
  int n_in;
  int i, j, n;
  u_int32_t v;

  assert( argc==1 );
  UNUSED_PARAMETER( argc );
  in = (const char*)dbsql_value_text(argv[0]);
  if( in==0 ) return;
  n_in = dbsql_value_bytes(argv[0]);
  out = dbsql_malloc64(n_in+1);
  if( out==0 ){
    dbsql_result_error_nomem(context);
    return;
  }
  i = j = 0;
  while( i<n_in ){
    const char *z = strchr(&in[i],'\\');
    if( z==0 ){
      n = n_in - i;
      memmove(&out[j], &in[i], n);
      j += n;
      break;
    }
    n = z - &in[i];
    if( n>0 ){
      memmove(&out[j], &in[i], n);
      j += n;
      i += n;
    }
    if( in[i+1]=='\\' ){
      i += 2;
      out[j++] = '\\';
    }else if( __isxdigit(in[i+1]) ){
      if( !__is_n_hex(&in[i+1], 4, &v) ) goto unistr_error;
      i += 5;
      j += __append_one_utf8_character(&out[j], v);
    }else if( in[i+1]=='+' ){
      if( !__is_n_hex(&in[i+2], 6, &v) ) goto unistr_error;
      i += 8;
      j += __append_one_utf8_character(&out[j], v);
    }else if( in[i+1]=='u' ){
      if( !__is_n_hex(&in[i+2], 4, &v) ) goto unistr_error;
      i += 6;
      j += __append_one_utf8_character(&out[j], v);
    }else if( in[i+1]=='U' ){
      if( !__is_n_hex(&in[i+2], 8, &v) ) goto unistr_error;
      i += 10;
      j += __append_one_utf8_character(&out[j], v);
    }else{
      goto unistr_error;
    }
  }
  out[j] = 0;
  dbsql_result_text64(context, out, j, dbsql_free, DBSQL_UTF8_ZT);
  return;

unistr_error:
  dbsql_free(out);
  dbsql_result_error(context, "invalid Unicode escape", -1);
  return;
}


/*
** Implementation of the QUOTE() function. 
**
** The quote(X) function returns the text of an SQL literal which is the
** value of its argument suitable for inclusion into an SQL statement.
** Strings are surrounded by single-quotes with escapes on interior quotes
** as needed. BLOBs are encoded as hexadecimal literals. Strings with
** embedded NUL characters cannot be represented as string literals in SQL
** and hence the returned string literal is truncated prior to the first NUL.
**
** If dbsql_user_data() is non-zero, then the UNISTR_QUOTE() function is
** implemented instead.  The difference is that UNISTR_QUOTE() uses the
** UNISTR() function to escape control characters.
*/
static void __quote_func(dbsql_func_t *context, int argc, dbsql_value_t **argv){
  dbsql_str_t str;
  DBSQL *db = dbsql_context_db_handle(context);
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  __str_accum_init(&str, db, 0, 0, db->limits[DBSQL_LIMIT_LENGTH]);
  __quote_value(&str,argv[0],DBSQL_PTR_TO_INT(dbsql_user_data(context)));
  dbsql_result_text(context, __str_accum_finish(&str), str.n_char,
                      DBSQL_DYNAMIC);
  if( str.acc_error!=DBSQL_OK ){
    dbsql_result_null(context);
    dbsql_result_error_code(context, str.acc_error);
  }
}

/*
** The unicode() function.  Return the integer unicode code-point value
** for the first character of the input string.
*/
static void __unicode_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const unsigned char *z = dbsql_value_text(argv[0]);
  (void)argc;
  if( z && z[0] ) dbsql_result_int(context, __utf8_read(&z));
}

/*
** The char() function takes zero or more arguments, each of which is
** an integer.  It constructs a string where each character of the string
** is the unicode character for the corresponding integer argument.
*/
static void __char_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  unsigned char *z, *out;
  int i;
  out = z = dbsql_malloc64( argc*4+1 );
  if( z==0 ){
    dbsql_result_error_nomem(context);
    return;
  }
  for(i=0; i<argc; i++){
    int64_t x;
    unsigned c;
    x = dbsql_value_int64(argv[i]);
    if( x<0 || x>0x10ffff ) x = 0xfffd;
    c = (unsigned)(x & 0x1fffff);
    if( c<0x00080 ){
      *out++ = (u_int8_t)(c&0xFF);
    }else if( c<0x00800 ){
      *out++ = 0xC0 + (u_int8_t)((c>>6)&0x1F);
      *out++ = 0x80 + (u_int8_t)(c & 0x3F);
    }else if( c<0x10000 ){
      *out++ = 0xE0 + (u_int8_t)((c>>12)&0x0F);
      *out++ = 0x80 + (u_int8_t)((c>>6) & 0x3F);
      *out++ = 0x80 + (u_int8_t)(c & 0x3F);
    }else{
      *out++ = 0xF0 + (u_int8_t)((c>>18) & 0x07);
      *out++ = 0x80 + (u_int8_t)((c>>12) & 0x3F);
      *out++ = 0x80 + (u_int8_t)((c>>6) & 0x3F);
      *out++ = 0x80 + (u_int8_t)(c & 0x3F);
    }                                                    \
  }
  *out = 0;
  dbsql_result_text64(context, (char*)z, out-z,dbsql_free,DBSQL_UTF8_ZT);
}

/*
** The hex() function.  Interpret the argument as a blob.  Return
** a hexadecimal rendering as text.
*/
static void __hex_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int i, n;
  const unsigned char *blob;
  char *hex, *z;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  blob = dbsql_value_blob(argv[0]);
  n = dbsql_value_bytes(argv[0]);
  assert( blob==dbsql_value_blob(argv[0]) );  /* No encoding change */
  z = hex = __context_malloc(context, ((int64_t)n)*2 + 1);
  if( hex ){
    for(i=0; i<n; i++, blob++){
      unsigned char c = *blob;
      *(z++) = hexdigits[(c>>4)&0xf];
      *(z++) = hexdigits[c&0xf];
    }
    *z = 0;
    dbsql_result_text64(context, hex, (u_int64_t)(z-hex),
                          dbsql_free, DBSQL_UTF8_ZT);
  }
}

/*
** Buffer str contains n_str bytes of utf-8 encoded text. Return 1 if str
** contains character ch, or 0 if it does not.
*/
static int __str_contains_char(const u_int8_t *str, int n_str, u_int32_t ch){
  const u_int8_t *end = &str[n_str];
  const u_int8_t *z = str;
  while( z<end ){
    u_int32_t tst = UTF8_READ(z);
    if( tst==ch ) return 1;
  }
  return 0;
}

/*
** The unhex() function. This function may be invoked with either one or
** two arguments. In both cases the first argument is interpreted as text
** a text value containing a set of pairs of hexadecimal digits which are
** decoded and returned as a blob.
**
** If there is only a single argument, then it must consist only of an
** even number of hexadecimal digits. Otherwise, return NULL.
**
** Or, if there is a second argument, then any character that appears in
** the second argument is also allowed to appear between pairs of hexadecimal
** digits in the first argument. If any other character appears in the
** first argument, or if one of the allowed characters appears between
** two hexadecimal digits that make up a single byte, NULL is returned.
**
** The following expressions are all true:
**
**     unhex('ABCD')       IS x'ABCD'
**     unhex('AB CD')      IS NULL
**     unhex('AB CD', ' ') IS x'ABCD'
**     unhex('A BCD', ' ') IS NULL
*/
static void __unhex_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  const u_int8_t *pass = (const u_int8_t*)"";
  int n_pass = 0;
  const u_int8_t *hex = dbsql_value_text(argv[0]);
  int n_hex = dbsql_value_bytes(argv[0]);
#ifdef DBSQL_DEBUG
  const u_int8_t *end = hex ? &hex[n_hex] : 0;
#endif
  u_int8_t *blob = 0;
  u_int8_t *p = 0;

  assert( argc==1 || argc==2 );
  if( argc==2 ){
    pass = dbsql_value_text(argv[1]);
    n_pass = dbsql_value_bytes(argv[1]);
  }
  if( !hex || !pass ) return;

  p = blob = __context_malloc(ctx, (n_hex/2)+1);
  if( blob ){
    u_int8_t c;                         /* Most significant digit of next byte */
    u_int8_t d;                         /* Least significant digit of next byte */

    while( (c = *hex)!=0x00 ){
      while( !__isxdigit(c) ){
        u_int32_t ch = UTF8_READ(hex);
        assert( hex<=end );
        if( !__str_contains_char(pass, n_pass, ch) ) goto unhex_null;
        c = *hex;
        if( c==0x00 ) goto unhex_done;
      }
      hex++;
      assert( *end==0x00 );
      assert( hex<=end );
      d = *(hex++);
      if( !__isxdigit(d) ) goto unhex_null;
      *(p++) = (__hex_to_int(c)<<4) | __hex_to_int(d);
    }
  }

 unhex_done:
  dbsql_result_blob(ctx, blob, (p - blob), dbsql_free);
  return;

 unhex_null:
  dbsql_free(blob);
  return;
}


/*
** The zeroblob(N) function returns a zero-filled blob of size N bytes.
*/
static void __zeroblob_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int64_t n;
  int rc;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  n = dbsql_value_int64(argv[0]);
  if( n<0 ) n = 0;
  rc = dbsql_result_zeroblob64(context, n); /* IMP: R-00293-64994 */
  if( rc ){
    dbsql_result_error_code(context, rc);
  }
}

/*
** The replace() function.  Three arguments are all strings: call
** them A, B, and C. The result is also a string which is derived
** from A by replacing every occurrence of B with C.  The match
** must be exact.  Collating sequences are not used.
*/
static void __replace_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const unsigned char *str;        /* The input string A */
  const unsigned char *pattern;    /* The pattern string B */
  const unsigned char *rep;        /* The replacement string C */
  unsigned char *out;              /* The output */
  int n_str;                /* Size of str */
  int n_pattern;            /* Size of pattern */
  int n_rep;                /* Size of rep */
  int64_t n_out;                /* Maximum size of out */
  int loop_limit;           /* Last str[] that might match pattern[] */
  int i, j;                /* Loop counters */
  unsigned cnt_expand;      /* Number out expansions */
  DBSQL *db = dbsql_context_db_handle(context);

  assert( argc==3 );
  UNUSED_PARAMETER(argc);
  str = dbsql_value_text(argv[0]);
  if( str==0 ) return;
  n_str = dbsql_value_bytes(argv[0]);
  assert( str==dbsql_value_text(argv[0]) );  /* No encoding change */
  pattern = dbsql_value_text(argv[1]);
  if( pattern==0 ){
    assert( dbsql_value_type(argv[1])==DBSQL_NULL
            || dbsql_context_db_handle(context)->malloc_failed );
    return;
  }
  if( pattern[0]==0 ){
    assert( dbsql_value_type(argv[1])!=DBSQL_NULL );
    dbsql_result_text(context, (const char*)str, n_str, DBSQL_TRANSIENT);
    return;
  }
  n_pattern = dbsql_value_bytes(argv[1]);
  assert( pattern==dbsql_value_text(argv[1]) );  /* No encoding change */
  rep = dbsql_value_text(argv[2]);
  if( rep==0 ) return;
  n_rep = dbsql_value_bytes(argv[2]);
  assert( rep==dbsql_value_text(argv[2]) );
  n_out = n_str + 1;
  assert( n_out<DBSQL_MAX_LENGTH );
  out = __context_malloc(context, n_out);
  if( out==0 ){
    return;
  }
  loop_limit = n_str - n_pattern; 
  cnt_expand = 0;
  for(i=j=0; i<=loop_limit; i++){
    if( str[i]!=pattern[0] || memcmp(&str[i], pattern, n_pattern) ){
      out[j++] = str[i];
    }else{
      if( n_rep>n_pattern ){
        n_out += n_rep - n_pattern;
        testcase( n_out-1==db->limits[DBSQL_LIMIT_LENGTH] );
        testcase( n_out-2==db->limits[DBSQL_LIMIT_LENGTH] );
        if( n_out-1>db->limits[DBSQL_LIMIT_LENGTH] ){
          dbsql_result_error_toobig(context);
          dbsql_free(out);
          return;
        }
        cnt_expand++;
        if( (cnt_expand&(cnt_expand-1))==0 ){
          /* Grow the size of the output buffer only on substitutions
          ** whose index is a power of two: 1, 2, 4, 8, 16, 32, ... */
          u_int8_t *old;
          old = out;
          out = __dbsql_realloc(out, (int)n_out + (n_out - n_str - 1));
          if( out==0 ){
            dbsql_result_error_nomem(context);
            dbsql_free(old);
            return;
          }
        }
      }
      memcpy(&out[j], rep, n_rep);
      j += n_rep;
      i += n_pattern-1;
    }
  }
  assert( j+n_str-i+1<=n_out );
  memcpy(&out[j], &str[i], n_str-i);
  j += n_str - i;
  assert( j<=n_out );
  out[j] = 0;
  dbsql_result_text(context, (char*)out, j, dbsql_free);
}

/*
** Implementation of the TRIM(), LTRIM(), and RTRIM() functions.
** The userdata is 0x1 for left trim, 0x2 for right trim, 0x3 for both.
*/
static void __trim_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const unsigned char *in;         /* Input string */
  const unsigned char *char_set;    /* Set of characters to trim */
  unsigned int n_in;                 /* Number of bytes in input */
  int flags;                        /* 1: trimleft  2: trimright  3: trim */
  int i;                            /* Loop counter */
  unsigned int *lens = 0;           /* Length of each character in char_set */
  unsigned char **chars = 0;       /* Individual characters in char_set */
  int n_char;                        /* Number of characters in char_set */

  if( dbsql_value_type(argv[0])==DBSQL_NULL ){
    return;
  }
  in = dbsql_value_text(argv[0]);
  if( in==0 ) return;
  n_in = (unsigned)dbsql_value_bytes(argv[0]);
  assert( in==dbsql_value_text(argv[0]) );
  if( argc==1 ){
    static const unsigned len_one[] = { 1 };
    static unsigned char * const ones[] = { (u_int8_t*)" " };
    n_char = 1;
    lens = (unsigned*)len_one;
    chars = (unsigned char **)ones;
    char_set = 0;
  }else if( (char_set = dbsql_value_text(argv[1]))==0 ){
    return;
  }else{
    const unsigned char *z;
    for(z=char_set, n_char=0; *z; n_char++){
      DBSQL_SKIP_UTF8(z);
    }
    if( n_char>0 ){
      chars = __context_malloc(context,
                     ((int64_t)n_char)*(sizeof(char*)+sizeof(unsigned)));
      if( chars==0 ){
        return;
      }
      lens = (unsigned*)&chars[n_char];
      for(z=char_set, n_char=0; *z; n_char++){
        chars[n_char] = (unsigned char *)z;
        DBSQL_SKIP_UTF8(z);
        lens[n_char] = (unsigned)(z - chars[n_char]);
      }
    }
  }
  if( n_char>0 ){
    flags = DBSQL_PTR_TO_INT(dbsql_user_data(context));
    if( flags & 1 ){
      while( n_in>0 ){
        unsigned int len = 0;
        for(i=0; i<n_char; i++){
          len = lens[i];
          if( len<=n_in && memcmp(in, chars[i], len)==0 ) break;
        }
        if( i>=n_char ) break;
        in += len;
        n_in -= len;
      }
    }
    if( flags & 2 ){
      while( n_in>0 ){
        unsigned int len = 0;
        for(i=0; i<n_char; i++){
          len = lens[i];
          if( len<=n_in && memcmp(&in[n_in-len],chars[i],len)==0 ) break;
        }
        if( i>=n_char ) break;
        n_in -= len;
      }
    }
    if( char_set ){
      dbsql_free(chars);
    }
  }
  dbsql_result_text(context, (char*)in, n_in, DBSQL_TRANSIENT);
}

/* The core implementation of the CONCAT(...) and CONCAT_WS(SEP,...)
** functions.
**
** Return a string value that is the concatenation of all non-null
** entries in argv[].  Use sep as the separator.
*/
static void __concat_func_core(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv,
  int n_sep,
  const char *sep
){
  int64_t j, n = 0;
  int i;
  int not_null = 0;   /* True after at least NOT NULL argument seen */
  char *z;
  for(i=0; i<argc; i++){
    n += dbsql_value_bytes(argv[i]);
  }
  n += (argc-1)*(int64_t)n_sep;
  z = dbsql_malloc64(n+1);
  if( z==0 ){
    dbsql_result_error_nomem(context);
    return;
  }
  j = 0;
  for(i=0; i<argc; i++){
    if( dbsql_value_type(argv[i])!=DBSQL_NULL ){
      int k = dbsql_value_bytes(argv[i]);
      const char *v = (const char*)dbsql_value_text(argv[i]);
      if( v!=0 ){
        if( not_null && n_sep>0 ){
          memcpy(&z[j], sep, n_sep);
          j += n_sep;
        }
        memcpy(&z[j], v, k);
        j += k;
        not_null = 1;
      }
    }
  }
  z[j] = 0;
  assert( j<=n );
  dbsql_result_text64(context, z, j, dbsql_free, DBSQL_UTF8_ZT);
}

/*
** The CONCAT(...) function.  Generate a string result that is the
** concatentation of all non-null arguments.
*/
static void __concat_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  __concat_func_core(context, argc, argv, 0, "");
}

/*
** The CONCAT_WS(separator, ...) function.
**
** Generate a string that is the concatenation of 2nd through the Nth
** argument.  Use the first argument (which must be non-NULL) as the
** separator.
*/
static void __concatws_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int n_sep = dbsql_value_bytes(argv[0]);
  const char *sep = (const char*)dbsql_value_text(argv[0]);
  if( sep==0 ) return;
  __concat_func_core(context, argc-1, argv+1, n_sep, sep);
}


#ifdef DBSQL_ENABLE_UNKNOWN_SQL_FUNCTION
/*
** The "unknown" function is automatically substituted in place of
** any unrecognized function name when doing an EXPLAIN or EXPLAIN QUERY PLAN
** when the DBSQL_ENABLE_UNKNOWN_SQL_FUNCTION compile-time option is used.
** When the "DBSQL" command-line shell is built using this functionality,
** that allows an EXPLAIN or EXPLAIN QUERY PLAN for complex queries
** involving application-defined functions to be examined in a generic
** DBSQL shell.
*/
static void __unknown_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  /* no-op */
  (void)context;
  (void)argc;
  (void)argv;
}
#endif /*DBSQL_ENABLE_UNKNOWN_SQL_FUNCTION*/


/* IMP: R-25361-16150 This function is omitted from dbsql by default. It
** is only available if the DBSQL_SOUNDEX compile-time option is used
** when dbsql is built.
*/
#ifdef DBSQL_SOUNDEX
/*
** Compute the soundex encoding of a word.
**
** IMP: R-59782-00072 The soundex(X) function returns a string that is the
** soundex encoding of the string X.
*/
static void __soundex_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  char result[8];
  const u_int8_t *in;
  int i, j;
  static const unsigned char i_code[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 2, 2, 4, 5, 5, 0,
    1, 2, 6, 2, 3, 0, 1, 0, 2, 0, 2, 0, 0, 0, 0, 0,
    0, 0, 1, 2, 3, 0, 1, 2, 0, 0, 2, 2, 4, 5, 5, 0,
    1, 2, 6, 2, 3, 0, 1, 0, 2, 0, 2, 0, 0, 0, 0, 0,
  };
  assert( argc==1 );
  in = (u_int8_t*)dbsql_value_text(argv[0]);
  if( in==0 ) in = (u_int8_t*)"";
  for(i=0; in[i] && !__isalpha(in[i]); i++){}
  if( in[i] ){
    u_int8_t prevcode = i_code[in[i]&0x7f];
    result[0] = __dbsql_toupper(in[i]);
    for(j=1; j<4 && in[i]; i++){
      int code = i_code[in[i]&0x7f];
      if( code>0 ){
        if( code!=prevcode ){
          prevcode = code;
          result[j++] = code + '0';
        }
      }else{
        prevcode = 0;
      }
    }
    while( j<4 ){
      result[j++] = '0';
    }
    result[j] = 0;
    dbsql_result_text(context, result, 4, DBSQL_TRANSIENT);
  }else{
    /* IMP: R-64894-50321 The string "?000" is returned if the argument
    ** is NULL or contains no ASCII alphabetic characters. */
    dbsql_result_text(context, "?000", 4, DBSQL_STATIC);
  }
}
#endif /* DBSQL_SOUNDEX */

#ifndef DBSQL_OMIT_LOAD_EXTENSION
/*
** A function that loads a shared-library extension then returns NULL.
*/
static void __load_ext(dbsql_func_t *context, int argc, dbsql_value_t **argv){
  const char *file = (const char *)dbsql_value_text(argv[0]);
  const char *proc;
  DBSQL *db = dbsql_context_db_handle(context);
  char *errmsg = 0;

  /* Disallow the load_extension() SQL function unless the DBSQL_LoadExtFunc
  ** flag is set.  See the dbsql_enable_load_extension() API.
  */
  if( (db->flags & DBSQL_LoadExtFunc)==0 ){
    dbsql_result_error(context, "not authorized", -1);
    return;
  }

  if( argc==2 ){
    proc = (const char *)dbsql_value_text(argv[1]);
  }else{
    proc = 0;
  }
  if( file && dbsql_load_extension(db, file, proc, &errmsg) ){
    dbsql_result_error(context, errmsg, -1);
    dbsql_free(errmsg);
  }
}
#endif


/*
** An instance of the following structure holds the context of a
** sum() or avg() aggregate computation.
*/
typedef struct sum_ctx_t sum_ctx_t;
struct sum_ctx_t {
  double r_sum;      /* Running sum as as a double */
  double r_err;      /* Error term for Kahan-Babushka-Neumaier summation */
  int64_t i_sum;         /* Running sum as a signed integer */
  int64_t cnt;          /* Number of elements summed */
  u_int8_t approx;        /* True if any non-integer value was input to the sum */
  u_int8_t ovrfl;         /* Integer overflow seen */
};

/*
** Do one step of the Kahan-Babushka-Neumaier summation.
**
** https://en.wikipedia.org/wiki/Kahan_summation_algorithm
**
** Variables are marked "volatile" to defeat c89 x86 floating point
** optimizations can mess up this algorithm.
*/
static void __kahan_babuska_neumaier_step(
  volatile sum_ctx_t *sum,
  volatile double r
){
  volatile double s = sum->r_sum;
  volatile double t = s + r;
  if( fabs(s) > fabs(r) ){
    sum->r_err += (s - t) + r;
  }else{
    sum->r_err += (r - t) + s;
  }
  sum->r_sum = t;
}

/*
** Add a (possibly large) integer to the running sum.
*/
static void __kahan_babuska_neumaier_step_int64(volatile sum_ctx_t *sum, int64_t i_val){
  if( i_val<=-4503599627370496LL || i_val>=+4503599627370496LL ){
    int64_t i_big, i_sm;
    i_sm = i_val % 16384;
    i_big = i_val - i_sm;
    __kahan_babuska_neumaier_step(sum, i_big);
    __kahan_babuska_neumaier_step(sum, i_sm);
  }else{
    __kahan_babuska_neumaier_step(sum, (double)i_val);
  }
}

/*
** Initialize the Kahan-Babaska-Neumaier sum from a 64-bit integer
*/
static void __kahan_babuska_neumaier_init(
  volatile sum_ctx_t *p,
  int64_t i_val
){
  if( i_val<=-4503599627370496LL || i_val>=+4503599627370496LL ){
    int64_t i_sm = i_val % 16384;
    p->r_sum = (double)(i_val - i_sm);
    p->r_err = (double)i_sm;
  }else{
    p->r_sum = (double)i_val;
    p->r_err = 0.0;
  }
}

/*
** Routines used to compute the sum, average, and total.
**
** The SUM() function follows the (broken) SQL standard which means
** that it returns NULL if it sums over no inputs.  TOTAL returns
** 0.0 in that case.  In addition, TOTAL always returns a float where
** SUM might return an integer if it never encounters a floating point
** value.  TOTAL never fails, but SUM might throw an exception if
** it overflows an integer.
*/
static void __sum_step(dbsql_func_t *context, int argc, dbsql_value_t **argv){
  sum_ctx_t *p;
  int type;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  p = dbsql_aggregate_context(context, sizeof(*p));
  type = dbsql_value_numeric_type(argv[0]);
  if( p && type!=DBSQL_NULL ){
    p->cnt++;
    if( p->approx==0 ){
      if( type!=DBSQL_INTEGER ){
        __kahan_babuska_neumaier_init(p, p->i_sum);
        p->approx = 1;
        __kahan_babuska_neumaier_step(p, dbsql_value_double(argv[0]));
      }else{
        int64_t x = p->i_sum;
        if( __add_int64(&x, dbsql_value_int64(argv[0]))==0 ){
          p->i_sum = x;
        }else{
          p->ovrfl = 1;
          __kahan_babuska_neumaier_init(p, p->i_sum);
          p->approx = 1;
          __kahan_babuska_neumaier_step_int64(p, dbsql_value_int64(argv[0]));
        }
      }
    }else{
      if( type==DBSQL_INTEGER ){
        __kahan_babuska_neumaier_step_int64(p, dbsql_value_int64(argv[0]));
      }else{
        p->ovrfl = 0;
        __kahan_babuska_neumaier_step(p, dbsql_value_double(argv[0]));
      }
    }
  }
}
#ifndef DBSQL_OMIT_WINDOWFUNC
static void SUM_INVERSE(dbsql_func_t *context, int argc, dbsql_value_t**argv){
  sum_ctx_t *p;
  int type;
  assert( argc==1 );
  UNUSED_PARAMETER(argc);
  p = dbsql_aggregate_context(context, sizeof(*p));
  type = dbsql_value_numeric_type(argv[0]);
  /* p is always non-NULL because __sum_step() will have been called first
  ** to initialize it */
  if( ALWAYS(p) && type!=DBSQL_NULL ){
    assert( p->cnt>0 );
    p->cnt--;
    if( !p->approx ){
      int64_t x = p->i_sum;
      if( __sub_int64(&x, dbsql_value_int64(argv[0]))==0 ){
        p->i_sum = x;
        return;
      }
      p->ovrfl = 1;
      p->approx = 1;
      __kahan_babuska_neumaier_init(p, p->i_sum);
    }
    if( type==DBSQL_INTEGER ){
      int64_t i_val = dbsql_value_int64(argv[0]);
      if( i_val!=SMALLEST_INT64 ){
        __kahan_babuska_neumaier_step_int64(p, -i_val);
      }else{
        __kahan_babuska_neumaier_step_int64(p, LARGEST_INT64);
        __kahan_babuska_neumaier_step_int64(p, 1);
      }       
    }else{
      __kahan_babuska_neumaier_step(p, -dbsql_value_double(argv[0]));
    }
  }
}
#else
# define SUM_INVERSE 0
#endif /* DBSQL_OMIT_WINDOWFUNC */
static void __sum_finalize(dbsql_func_t *context){
  sum_ctx_t *p;
  p = dbsql_aggregate_context(context, 0);
  if( p && p->cnt>0 ){
    if( p->approx ){
      if( p->ovrfl ){
        dbsql_result_error(context,"integer overflow",-1);
      }else if( !__is_overflow(p->r_err) ){
        dbsql_result_double(context, p->r_sum+p->r_err);
      }else{
        dbsql_result_double(context, p->r_sum);
      }
    }else{
      dbsql_result_int64(context, p->i_sum);
    }
  }
}
static void __avg_finalize(dbsql_func_t *context){
  sum_ctx_t *p;
  p = dbsql_aggregate_context(context, 0);
  if( p && p->cnt>0 ){
    double r;
    if( p->approx ){
      r = p->r_sum;
      if( !__is_overflow(p->r_err) ) r += p->r_err;
    }else{
      r = (double)(p->i_sum);
    }
    dbsql_result_double(context, r/(double)p->cnt);
  }
}
static void __total_finalize(dbsql_func_t *context){
  sum_ctx_t *p;
  double r = 0.0;
  p = dbsql_aggregate_context(context, 0);
  if( p ){
    if( p->approx ){
      r = p->r_sum;
      if( !__is_overflow(p->r_err) ) r += p->r_err;
    }else{
      r = (double)(p->i_sum);
    }
  }
  dbsql_result_double(context, r);
}

/*
** The following structure keeps track of state information for the
** count() aggregate function.
*/
typedef struct count_ctx_t count_ctx_t;
struct count_ctx_t {
  int64_t n;
#ifdef DBSQL_DEBUG
  int inverse;                   /* True if inverse() ever called */
#endif
};

/*
** Routines to implement the count() aggregate function.
*/
static void __count_step(dbsql_func_t *context, int argc, dbsql_value_t **argv){
  count_ctx_t *p;
  p = dbsql_aggregate_context(context, sizeof(*p));
  if( (argc==0 || DBSQL_NULL!=dbsql_value_type(argv[0])) && p ){
    p->n++;
  }

#ifndef DBSQL_OMIT_DEPRECATED
  /* The dbsql_aggregate_count() function is deprecated.  But just to make
  ** sure it still operates correctly, verify that its count agrees with our
  ** internal count when using count(*) and when the total count can be
  ** expressed as a 32-bit integer. */
  assert( argc==1 || p==0 || p->n>0x7fffffff || p->inverse
          || p->n==dbsql_aggregate_count(context) );
#endif
}  
static void __count_finalize(dbsql_func_t *context){
  count_ctx_t *p;
  p = dbsql_aggregate_context(context, 0);
  dbsql_result_int64(context, p ? p->n : 0);
}
#ifndef DBSQL_OMIT_WINDOWFUNC
static void COUNT_INVERSE(dbsql_func_t *ctx, int argc, dbsql_value_t **argv){
  count_ctx_t *p;
  p = dbsql_aggregate_context(ctx, sizeof(*p));
  /* p is always non-NULL since __count_step() will have been called first */
  if( (argc==0 || DBSQL_NULL!=dbsql_value_type(argv[0])) && ALWAYS(p) ){
    p->n--;
#ifdef DBSQL_DEBUG
    p->inverse = 1;
#endif
  }
}  
#else
# define COUNT_INVERSE 0
#endif /* DBSQL_OMIT_WINDOWFUNC */

/*
** Routines to implement min() and max() aggregate functions.
*/
static void __minmax_step(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **argv
){
  mem_t *arg  = (mem_t *)argv[0];
  mem_t *best;
  UNUSED_PARAMETER(__not_used);

  best = (mem_t *)dbsql_aggregate_context(context, sizeof(*best));
  if( !best ) return;

  if( dbsql_value_type(arg)==DBSQL_NULL ){
    if( best->flags ) __skip_accumulator_load(context);
  }else if( best->flags ){
    int max;
    int cmp;
    coll_seq_t *coll = __get_func_coll_seq(context);
    /* This step function is used for both the min() and max() aggregates,
    ** the only difference between the two being that the sense of the
    ** comparison is inverted. For the max() aggregate, the
    ** dbsql_user_data() function returns (void *)-1. For min() it
    ** returns (void *)db, where db is the DBSQL* database pointer.
    ** Therefore the next statement sets variable 'max' to 1 for the max()
    ** aggregate, or 0 for min().
    */
    max = dbsql_user_data(context)!=0;
    cmp = __mem_compare(best, arg, coll);
    if( (max && cmp<0) || (!max && cmp>0) ){
      __vdbe_mem_copy(best, arg);
    }else{
      __skip_accumulator_load(context);
    }
  }else{
    best->db = dbsql_context_db_handle(context);
    __vdbe_mem_copy(best, arg);
  }
}
static void __min_max_value_finalize(dbsql_func_t *context, int value){
  dbsql_value_t *res;
  res = (dbsql_value_t *)dbsql_aggregate_context(context, 0);
  if( res ){
    if( res->flags ){
      dbsql_result_value(context, res);
    }
    if( value==0 ) __vdbe_mem_release(res);
  }
}
#ifndef DBSQL_OMIT_WINDOWFUNC
static void MIN_MAX_VALUE(dbsql_func_t *context){
  __min_max_value_finalize(context, 1);
}
#else
# define MIN_MAX_VALUE 0
#endif /* DBSQL_OMIT_WINDOWFUNC */
static void __min_max_finalize(dbsql_func_t *context){
  __min_max_value_finalize(context, 0);
}

/*
** group_concat(EXPR, ?SEPARATOR?)
** string_agg(EXPR, SEPARATOR)
**
** Content is accumulated in __group_concat_ctx.str with the SEPARATOR
** coming before the EXPR value, except for the first entry which
** omits the SEPARATOR.
**
** It is tragic that the SEPARATOR goes before the EXPR string.  The
** GROUP_CONCAT_INVERSE() implementation would have been easier if the
** SEPARATOR were appended after EXPR.  And the order is undocumented,
** so we could change it, in theory.  But the old behavior has been
** around for so long that we dare not, for fear of breaking something.
*/
typedef struct {
  str_accum_t str;          /* The accumulated concatenation */
#ifndef DBSQL_OMIT_WINDOWFUNC
  int n_accum;            /* Number of strings presently concatenated */
  int n_first_sep_length;   /* Used to detect separator length change */
  /* If pn_sep_lengths!=0, refs an array of inter-string separator lengths,
  ** stored as actually incorporated into presently accumulated result.
  ** (Hence, its slots in use number n_accum-1 between method calls.)
  ** If pn_sep_lengths==0, n_first_sep_length is the length used throughout.
  */
  int *pn_sep_lengths;
#endif
} __group_concat_ctx;

static void __group_concat_step(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const char *val;
  __group_concat_ctx *gcc;
  const char *sep;
  int n_val, n_sep;
  assert( argc==1 || argc==2 );
  if( dbsql_value_type(argv[0])==DBSQL_NULL ) return;
  gcc = (__group_concat_ctx*)dbsql_aggregate_context(context, sizeof(*gcc));
  if( gcc ){
    DBSQL *db = dbsql_context_db_handle(context);
    int first_term = gcc->str.mx_alloc==0;
    gcc->str.mx_alloc = db->limits[DBSQL_LIMIT_LENGTH];
    if( argc==1 ){
      if( !first_term ){
        dbsql_str_appendchar(&gcc->str, 1, ',');
      }
#ifndef DBSQL_OMIT_WINDOWFUNC
      else{
        gcc->n_first_sep_length = 1;
      }
#endif
    }else if( !first_term ){
      sep = (char*)dbsql_value_text(argv[1]);
      n_sep = dbsql_value_bytes(argv[1]);
      if( sep ){
        dbsql_str_append(&gcc->str, sep, n_sep);
      }
#ifndef DBSQL_OMIT_WINDOWFUNC
      else{
        n_sep = 0;
      }
      if( n_sep != gcc->n_first_sep_length || gcc->pn_sep_lengths != 0 ){
        int *pnsl = gcc->pn_sep_lengths;
        if( pnsl == 0 ){
          /* First separator length variation seen, start tracking them. */
          pnsl = (int*)dbsql_malloc64((gcc->n_accum+1) * sizeof(int));
          if( pnsl!=0 ){
            int i = 0, n_a = gcc->n_accum-1;
            while( i<n_a ) pnsl[i++] = gcc->n_first_sep_length;
          }
        }else{
          pnsl = (int*)dbsql_realloc64(pnsl, gcc->n_accum * sizeof(int));
        }
        if( pnsl!=0 ){
          if( ALWAYS(gcc->n_accum>0) ){
            pnsl[gcc->n_accum-1] = n_sep;
          }
          gcc->pn_sep_lengths = pnsl;
        }else{
          __str_accum_set_error(&gcc->str, DBSQL_NOMEM);
        }
      }
#endif
    }
#ifndef DBSQL_OMIT_WINDOWFUNC
    else{
      gcc->n_first_sep_length = dbsql_value_bytes(argv[1]);
    }
    gcc->n_accum += 1;
#endif
    val = (char*)dbsql_value_text(argv[0]);
    n_val = dbsql_value_bytes(argv[0]);
    if( val ) dbsql_str_append(&gcc->str, val, n_val);
  }
}

#ifndef DBSQL_OMIT_WINDOWFUNC
static void GROUP_CONCAT_INVERSE(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  __group_concat_ctx *gcc;
  assert( argc==1 || argc==2 );
  (void)argc;  /* Suppress unused parameter warning */
  if( dbsql_value_type(argv[0])==DBSQL_NULL ) return;
  gcc = (__group_concat_ctx*)dbsql_aggregate_context(context, sizeof(*gcc));
  /* gcc is always non-NULL since __group_concat_step() will have always
  ** run first to initialize it */
  if( ALWAYS(gcc) ){
    int n_vs;  /* Number of characters to remove */
    /* Must call dbsql_value_text() to convert the argument into text prior
    ** to invoking dbsql_value_bytes(), in case the text encoding is UTF16 */
    (void)dbsql_value_text(argv[0]);
    n_vs = dbsql_value_bytes(argv[0]);
    gcc->n_accum -= 1;
    if( gcc->pn_sep_lengths!=0 ){
      assert(gcc->n_accum >= 0);
      if( gcc->n_accum>0 ){
        n_vs += *gcc->pn_sep_lengths;
        memmove(gcc->pn_sep_lengths, gcc->pn_sep_lengths+1,
               (gcc->n_accum-1)*sizeof(int));
      }
    }else{
      /* If removing single accumulated string, harmlessly over-do. */
      n_vs += gcc->n_first_sep_length;
    }
    if( n_vs>=(int)gcc->str.n_char ){
      gcc->str.n_char = 0;
    }else{
      gcc->str.n_char -= n_vs;
      memmove(gcc->str.text, &gcc->str.text[n_vs], gcc->str.n_char);
    }
    if( gcc->str.n_char==0 ){
      gcc->str.mx_alloc = 0;
      dbsql_free(gcc->pn_sep_lengths);
      gcc->pn_sep_lengths = 0;
    }
  }
}
#else
# define GROUP_CONCAT_INVERSE 0
#endif /* DBSQL_OMIT_WINDOWFUNC */
static void __group_concat_finalize(dbsql_func_t *context){
  __group_concat_ctx *gcc
    = (__group_concat_ctx*)dbsql_aggregate_context(context, 0);
  if( gcc ){
    __result_str_accum(context, &gcc->str);
#ifndef DBSQL_OMIT_WINDOWFUNC
    dbsql_free(gcc->pn_sep_lengths);
#endif
  }
}
#ifndef DBSQL_OMIT_WINDOWFUNC
static void GROUP_CONCAT_VALUE(dbsql_func_t *context){
  __group_concat_ctx *gcc
    = (__group_concat_ctx*)dbsql_aggregate_context(context, 0);
  if( gcc ){
    str_accum_t *accum = &gcc->str;
    if( accum->acc_error==DBSQL_TOOBIG ){
      dbsql_result_error_toobig(context);
    }else if( accum->acc_error==DBSQL_NOMEM ){
      dbsql_result_error_nomem(context);
    }else if( gcc->n_accum>0 && accum->n_char==0 ){
      dbsql_result_text(context, "", 1, DBSQL_STATIC);
    }else{   
      const char *text = dbsql_str_value(accum);
      dbsql_result_text(context, text, accum->n_char, DBSQL_TRANSIENT);
    }
  }
}
#else
# define GROUP_CONCAT_VALUE 0
#endif /* DBSQL_OMIT_WINDOWFUNC */

/*
** This routine does per-connection function registration.  Most
** of the built-in functions above are part of the global function set.
** This routine only deals with those that are not global.
*/
void __register_per_connection_builtin_functions(DBSQL *db){
  int rc = dbsql_overload_function(db, "MATCH", 2);
  assert( rc==DBSQL_NOMEM || rc==DBSQL_OK );
  if( rc==DBSQL_NOMEM ){
    __oom_fault(db);
  }
}

/*
** Re-register the built-in LIKE functions.  The case_sensitive
** parameter determines whether or not the LIKE operator is case
** sensitive.
*/
void __register_like_functions(DBSQL *db, int case_sensitive){
  func_def_t *def;
  struct compare_info *info;
  int flags;
  int n_arg;
  if( case_sensitive ){
    info = (struct compare_info*)&__like_info_alt;
    flags = DBSQL_FUNC_LIKE | DBSQL_FUNC_CASE;
  }else{
    info = (struct compare_info*)&__like_info_norm;
    flags = DBSQL_FUNC_LIKE;
  }
  for(n_arg=2; n_arg<=3; n_arg++){
    __create_func(db, "like", n_arg, DBSQL_UTF8, info, __like_func, 
                      0, 0, 0, 0, 0);
    def = __find_function(db, "like", n_arg, DBSQL_UTF8, 0);
    assert( def!=0 ); /* The __create_func() call above cannot fail
                       ** because the "like" SQL-function already exists */
    def->func_flags |= flags;
    def->func_flags &= ~DBSQL_FUNC_UNSAFE;
  }
}

/*
** expr points to an expression which implements a function.  If
** it is appropriate to apply the LIKE optimization to that function
** then set wcs[0] through wcs[2] to the wildcard characters and the
** escape character and then return TRUE.  If the function is not a
** LIKE-style function then return FALSE.
**
** The expression "a LIKE b ESCAPE c" is only considered a valid LIKE
** operator if c is a string literal that is exactly one byte in length.
** That one byte is stored in wcs[3].  wcs[3] is set to zero if there is
** no ESCAPE clause.
**
** *is_nocase is set to true if uppercase and lowercase are equivalent for
** the function (default for LIKE).  If the function makes the distinction
** between uppercase and lowercase (as does GLOB) then *is_nocase is set to
** false.
*/
int __is_like_function(DBSQL *db, expr_t *expr, int *is_nocase, char *wcs){
  func_def_t *def;
  int n_expr;
  assert( expr!=0 );
  assert( expr->op==TK_FUNCTION );
  assert( EXPR_USE_X_LIST(expr) );
  if( !expr->x.list ){
    return 0;
  }
  n_expr = expr->x.list->n_expr;
  assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
  def = __find_function(db, expr->u.token, n_expr, DBSQL_UTF8, 0);
#ifdef DBSQL_ENABLE_UNKNOWN_SQL_FUNCTION
  if( def==0 ) return 0;
#endif
  if( NEVER(def==0) || (def->func_flags & DBSQL_FUNC_LIKE)==0 ){
    return 0;
  }

  /* The memcpy() statement assumes that the wildcard characters are
  ** the first three statements in the compare_info structure.  The
  ** asserts() that follow verify that assumption
  */
  memcpy(wcs, def->user_data, 3);
  assert( (char*)&__like_info_alt == (char*)&__like_info_alt.match_all );
  assert( &((char*)&__like_info_alt)[1] == (char*)&__like_info_alt.match_one );
  assert( &((char*)&__like_info_alt)[2] == (char*)&__like_info_alt.matchSet );

  if( n_expr<3 ){
    wcs[3] = 0;
  }else{
    expr_t *escape = expr->x.list->a[2].expr;
    char *z_escape;
    if( escape->op!=TK_STRING ) return 0;
    assert( !EXPR_HAS_PROPERTY(escape, EP_IntValue) );
    z_escape = escape->u.token;
    if( z_escape[0]==0 || z_escape[1]!=0 ) return 0;
    if( z_escape[0]==wcs[0] ) return 0;
    if( z_escape[0]==wcs[1] ) return 0;
    wcs[3] = z_escape[0];
  }

  *is_nocase = (def->func_flags & DBSQL_FUNC_CASE)==0;
  return 1;
}

/* Mathematical Constants */
#ifndef M_PI
# define M_PI   3.141592653589793238462643383279502884
#endif
#ifndef M_LN10
# define M_LN10 2.302585092994045684017991454684364208
#endif
#ifndef M_LN2
# define M_LN2  0.693147180559945309417232121458176568
#endif


/* Extra math functions that require linking with -lm
*/
#ifdef DBSQL_ENABLE_MATH_FUNCTIONS
/*
** Implementation SQL functions:
**
**   ceil(X)
**   ceiling(X)
**   floor(X)
**
** The dbsql_user_data() pointer is a pointer to the libm implementation
** of the underlying C function.
*/
static void __ceiling_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  assert( argc==1 );
  switch( dbsql_value_numeric_type(argv[0]) ){
    case DBSQL_INTEGER: {
       dbsql_result_int64(context, dbsql_value_int64(argv[0]));
       break;
    }
    case DBSQL_FLOAT: {
       double (*x)(double) = (double(*)(double))dbsql_user_data(context);
       dbsql_result_double(context, x(dbsql_value_double(argv[0])));
       break;
    }
    default: {
       break;
    }
  }
}

/*
** On some systems, ceil() and floor() are intrinsic function.  You are
** unable to take a pointer to these functions.  Hence, we here wrap them
** in our own actual functions.
*/
static double __x_ceil(double x){ return ceil(x); }
static double __x_floor(double x){ return floor(x); }

/*
** Some systems do not have log2() and log10() in their standard math
** libraries.
*/
#if defined(HAVE_LOG10) && HAVE_LOG10==0
# define log10(X) (0.4342944819032517867*log(X))
#endif
#if defined(HAVE_LOG2) && HAVE_LOG2==0
# define log2(X) (1.442695040888963456*log(X))
#endif


/*
** Implementation of SQL functions:
**
**   ln(X)       - natural logarithm
**   log(X)      - log X base 10
**   log10(X)    - log X base 10
**   log(B,X)    - log X base B
*/
static void __log_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  double x, b, ans;
  assert( argc==1 || argc==2 );
  switch( dbsql_value_numeric_type(argv[0]) ){
    case DBSQL_INTEGER:
    case DBSQL_FLOAT:
      x = dbsql_value_double(argv[0]);
      if( x<=0.0 ) return;
      break;
    default:
      return;
  }
  if( argc==2 ){
    switch( dbsql_value_numeric_type(argv[0]) ){
      case DBSQL_INTEGER:
      case DBSQL_FLOAT:
        b = log(x);
        if( b<=0.0 ) return;
        x = dbsql_value_double(argv[1]);
        if( x<=0.0 ) return;
        break;
     default:
        return;
    }
    ans = log(x)/b;
  }else{
    switch( DBSQL_PTR_TO_INT(dbsql_user_data(context)) ){
      case 1:
        ans = log10(x);
        break;
      case 2:
        ans = log2(x);
        break;
      default:
        ans = log(x);
        break;
    }
  }
  dbsql_result_double(context, ans);
}

/*
** Functions to converts degrees to radians and radians to degrees.
*/
static double __deg_to_rad(double x){ return x*(M_PI/180.0); }
static double __rad_to_deg(double x){ return x*(180.0/M_PI); }

/*
** Implementation of 1-argument SQL math functions:
**
**   exp(X)  - Compute e to the X-th power
*/
static void math1Func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int type0;
  double v0, ans;
  double (*x)(double);
  assert( argc==1 );
  type0 = dbsql_value_numeric_type(argv[0]);
  if( type0!=DBSQL_INTEGER && type0!=DBSQL_FLOAT ) return;
  v0 = dbsql_value_double(argv[0]);
  x = (double(*)(double))dbsql_user_data(context);
  ans = x(v0);
  dbsql_result_double(context, ans);
}

/*
** Implementation of 2-argument SQL math functions:
**
**   power(X,Y)  - Compute X to the Y-th power
*/
static void math2Func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int type0, type1;
  double v0, v1, ans;
  double (*x)(double,double);
  assert( argc==2 );
  type0 = dbsql_value_numeric_type(argv[0]);
  if( type0!=DBSQL_INTEGER && type0!=DBSQL_FLOAT ) return;
  type1 = dbsql_value_numeric_type(argv[1]);
  if( type1!=DBSQL_INTEGER && type1!=DBSQL_FLOAT ) return;
  v0 = dbsql_value_double(argv[0]);
  v1 = dbsql_value_double(argv[1]);
  x = (double(*)(double,double))dbsql_user_data(context);
  ans = x(v0, v1);
  dbsql_result_double(context, ans);
}

/*
** Implementation of 0-argument pi() function.
*/
static void __pi_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  assert( argc==0 );
  (void)argv;
  dbsql_result_double(context, M_PI);
}

#endif /* DBSQL_ENABLE_MATH_FUNCTIONS */

/*
** Implementation of sign(X) function.
*/
static void __sign_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int type0;
  double x;
  UNUSED_PARAMETER(argc);
  assert( argc==1 );
  type0 = dbsql_value_numeric_type(argv[0]);
  if( type0!=DBSQL_INTEGER && type0!=DBSQL_FLOAT ) return;
  x = dbsql_value_double(argv[0]);
  dbsql_result_int(context, x<0.0 ? -1 : x>0.0 ? +1 : 0);
}

#if defined(DBSQL_ENABLE_PERCENTILE)
/***********************************************************************
** This section implements the percentile(Y,P) SQL function and similar.
** Requirements:
**
**   (1)  The percentile(Y,P) function is an aggregate function taking
**        exactly two arguments.
**
**   (2)  If the P argument to percentile(Y,P) is not the same for every
**        row in the aggregate then an error is thrown.  The word "same"
**        in the previous sentence means that the value differ by less
**        than 0.001.
**
**   (3)  If the P argument to percentile(Y,P) evaluates to anything other
**        than a number in the range of 0.0 to 100.0 inclusive then an
**        error is thrown.
**
**   (4)  If any Y argument to percentile(Y,P) evaluates to a value that
**        is not NULL and is not numeric then an error is thrown.
**
**   (5)  If any Y argument to percentile(Y,P) evaluates to plus or minus
**        infinity then an error is thrown.  (dbsql always interprets __na_n
**        values as NULL.)
**
**   (6)  Both Y and P in percentile(Y,P) can be arbitrary expressions,
**        including CASE WHEN expressions.
**
**   (7)  The percentile(Y,P) aggregate is able to handle inputs of at least
**        one million (1,000,000) rows.
**
**   (8)  If there are no non-NULL values for Y, then percentile(Y,P)
**        returns NULL.
**
**   (9)  If there is exactly one non-NULL value for Y, the percentile(Y,P)
**        returns the one Y value.
**
**  (10)  If there N non-NULL values of Y where N is two or more and
**        the Y values are ordered from least to greatest and a graph is
**        drawn from 0 to N-1 such that the height of the graph at J is
**        the J-th Y value and such that straight lines are drawn between
**        adjacent Y values, then the percentile(Y,P) function returns
**        the height of the graph at P*(N-1)/100.
**
**  (11)  The percentile(Y,P) function always returns either a floating
**        point number or NULL.
**
**  (12)  The percentile(Y,P) is implemented as a single C99 source-code
**        file that compiles into a shared-library or DLL that can be loaded
**        into dbsql using the dbsql_load_extension() interface.
**
**  (13)  A separate median(Y) function is the equivalent percentile(Y,50).
**
**  (14)  A separate percentile_cont(Y,P) function is equivalent to
**        percentile(Y,P/100.0).  In other words, the fraction value in
**        the second argument is in the range of 0 to 1 instead of 0 to 100.
**
**  (15)  A separate percentile_disc(Y,P) function is like
**        percentile_cont(Y,P) except that instead of returning the weighted
**        average of the nearest two input values, it returns the next lower
**        value.  So the percentile_disc(Y,P) will always return a value
**        that was one of the inputs.
**
**  (16)  All of median(), percentile(Y,P), percentile_cont(Y,P) and
**        percentile_disc(Y,P) can be used as window functions.
**
** Differences from standard SQL:
**
**  *  The percentile_cont(X,P) function is equivalent to the following in
**     standard SQL:
**
**         (percentile_cont(P) WITHIN GROUP (ORDER BY X))
**
**     The dbsql syntax is much more compact.  The standard SQL syntax
**     is also supported if dbsql is compiled with the
**     -DSQLITE_ENABLE_ORDERED_SET_AGGREGATES option.
**
**  *  No median(X) function exists in the SQL standard.  App developers
**     are expected to write "percentile_cont(0.5)WITHIN GROUP(ORDER BY X)".
**
**  *  No percentile(Y,P) function exists in the SQL standard.  Instead of
**     percential(Y,P), developers must write this:
**     "percentile_cont(P/100.0) WITHIN GROUP (ORDER BY Y)".  Note that
**     the fraction parameter to percentile() goes from 0 to 100 whereas
**     the fraction parameter in SQL standard percentile_cont() goes from
**     0 to 1.
**
** Implementation notes as of 2024-08-31:
**
**  *  The regular aggregate-function versions of these routines work
**     by accumulating all values in an array of doubles, then sorting
**     that array using quicksort before computing the answer. Thus
**     the runtime is O(__nlog_n) where N is the number of rows of input.
**
**  *  For the window-function versions of these routines, the array of
**     inputs is sorted as soon as the first value is computed.  Thereafter,
**     the array is kept in sorted order using an insert-sort.  This
**     results in O(N*K) performance where K is the size of the window.
**     One can imagine alternative implementations that give O(N*log_n*log_k)
**     performance, but they require more complex logic and data structures.
**     The developers have elected to keep the asymptotically slower
**     algorithm for now, for simplicity, under the theory that window
**     functions are seldom used and when they are, the window size K is
**     often small.  The developers might revisit that decision later,
**     should the need arise.
*/

/* The following object is the group context for a single percentile()
** aggregate.  Remember all input Y values until the very end.
** Those values are accumulated in the percentile_t.a[] array.
*/
typedef struct percentile_t percentile_t;
struct percentile_t {
  u_int64_t n_alloc;          /* Number of slots allocated for a[] */
  u_int64_t n_used;           /* Number of slots actually used in a[] */
  char sorted;        /* True if a[] is already in sorted order */
  char keep_sorted;    /* True if advantageous to keep a[] sorted */
  char pct_valid;      /* True if r_pct is valid */
  double r_pct;         /* Fraction.  0.0 to 1.0 */
  double *a;           /* Array of Y values */
};

/*
** Return TRUE if the input floating-point number is an infinity.
*/
static int __percent_is_infinity(double r){
  u_int64_t u;
  assert( sizeof(u)==sizeof(r) );
  memcpy(&u, &r, sizeof(u));
  return ((u>>52)&0x7ff)==0x7ff;
}

/*
** Return TRUE if two doubles differ by 0.001 or less.
*/
static int __percent_same_value(double a, double b){
  a -= b;
  return a>=-0.001 && a<=0.001;
}

/*
** Search p (which must have p->sorted) looking for an entry with
** value y.  Return the index of that entry.
**
** If exact is true, return -1 if the entry is not found.
**
** If exact is false, return the index at which a new entry with
** value y should be insert in order to keep the values in sorted
** order.  The smallest return value in this case will be 0, and
** the largest return value will be p->n_used.
*/
static int64_t __percent_binary_search(percentile_t *p, double y, int exact){
  int64_t i_first = 0;                   /* First element of search range */
  int64_t i_last = (int64_t)p->n_used - 1;    /* Last element of search range */
  while( i_last>=i_first ){
    int64_t i_mid = (i_first+i_last)/2;
    double x = p->a[i_mid];
    if( x<y ){
      i_first = i_mid + 1;
    }else if( x>y ){
      i_last = i_mid - 1;
    }else{
      return i_mid;
    }
  }
  if( exact ) return -1;
  return i_first;
}

/*
** Generate an error for a percentile function.
**
** The error format string must have exactly one occurrence of "%%s()"
** (with two '%' characters).  That substring will be replaced by the name
** of the function.
*/
static void __percent_error(dbsql_func_t *ctx, const char *format, ...){
  char *msg1;
  char *msg2;
  va_list ap;

  va_start(ap, format);
  msg1 = dbsql_vmprintf(format, ap);
  va_end(ap);
  msg2 = msg1 ? dbsql_mprintf(msg1, __vdbe_func_name(ctx)) : 0;
  dbsql_result_error(ctx, msg2, -1);
  dbsql_free(msg1);
  dbsql_free(msg2);
}

/*
** The "step" function for percentile(Y,P) is called once for each
** input row.
*/
static void __percent_step(dbsql_func_t *ctx, int argc, dbsql_value_t **argv){
  percentile_t *p;
  double r_pct;
  int type;
  double y;
  assert( argc==2 || argc==1 );

  if( argc==1 ){
    /* Requirement 13:  median(Y) is the same as percentile(Y,50). */
    r_pct = 0.5;
  }else{
    /* P must be a number between 0 and 100 for percentile() or between
    ** 0.0 and 1.0 for percentile_cont() and percentile_disc().
    **
    ** The user-data is an integer which is 10 times the upper bound.
    */
    double mx_frac = (DBSQL_PTR_TO_INT(dbsql_user_data(ctx))&2)? 100.0 : 1.0;
    type = dbsql_value_numeric_type(argv[1]);
    r_pct = dbsql_value_double(argv[1])/mx_frac;
    if( (type!=DBSQL_INTEGER && type!=DBSQL_FLOAT)
     || r_pct<0.0 || r_pct>1.0
    ){
      __percent_error(ctx, "the fraction argument to %%s()"
                        " is not between 0.0 and %.1f",
                        (double)mx_frac);
      return;
    }
  }

  /* Allocate the session context. */
  p = (percentile_t*)dbsql_aggregate_context(ctx, sizeof(*p));
  if( p==0 ) return;

  /* Remember the P value.  Throw an error if the P value is different
  ** from any prior row, per Requirement (2). */
  if( !p->pct_valid ){
    p->r_pct = r_pct;
    p->pct_valid = 1;
  }else if( !__percent_same_value(p->r_pct,r_pct) ){
    __percent_error(ctx, "the fraction argument to %%s()"
                      " is not the same for all input rows");
    return;
  }

  /* Ignore rows for which Y is NULL */
  type = dbsql_value_type(argv[0]);
  if( type==DBSQL_NULL ) return;

  /* If not NULL, then Y must be numeric.  Otherwise throw an error.
  ** Requirement 4 */
  if( type!=DBSQL_INTEGER && type!=DBSQL_FLOAT ){
    __percent_error(ctx, "input to %%s() is not numeric");
    return;
  }

  /* Throw an error if the Y value is infinity or __na_n */
  y = dbsql_value_double(argv[0]);
  if( __percent_is_infinity(y) ){
    __percent_error(ctx, "Inf input to %%s()");
    return;
  }

  /* Allocate and store the Y */
  if( p->n_used>=p->n_alloc ){
    u_int64_t n = p->n_alloc*2 + 250;
    double *a = dbsql_realloc64(p->a, sizeof(double)*n);
    if( a==0 ){
      dbsql_free(p->a);
      memset(p, 0, sizeof(*p));
      dbsql_result_error_nomem(ctx);
      return;
    }
    p->n_alloc = n;
    p->a = a;
  }
  if( p->n_used==0 ){
    p->a[p->n_used++] = y;
    p->sorted = 1;
  }else if( !p->sorted || y>=p->a[p->n_used-1] ){
    p->a[p->n_used++] = y;
  }else if( p->keep_sorted ){
    int64_t i;
    i = __percent_binary_search(p, y, 0);
    if( i<(int)p->n_used ){
      memmove(&p->a[i+1], &p->a[i], (p->n_used-i)*sizeof(p->a[0]));
    }
    p->a[i] = y;
    p->n_used++;
  }else{
    p->a[p->n_used++] = y;
    p->sorted = 0;
  }
}

/*
** Interchange two doubles.
*/
#define SWAP_DOUBLE(X,Y)  {double ttt=(X);(X)=(Y);(Y)=ttt;}

/*
** Sort an array of doubles.
**
** Algorithm: quicksort
**
** This is implemented separately rather than using the qsort() routine
** from the standard library because:
**
**    (1)  To avoid a dependency on qsort()
**    (2)  To avoid the function call to the comparison routine for each
**         comparison.
*/
static void __percent_sort(double *a, unsigned int n){
  int i_lt;  /* Entries before a[i_lt] are less than r_pivot */
  int i_gt;  /* Entries at or after a[i_gt] are greater than r_pivot */
  int i;         /* Loop counter */
  double r_pivot; /* The pivot value */

  while( n>=2 ){
    if( a[0]>a[n-1] ){
      SWAP_DOUBLE(a[0],a[n-1])
    }
    if( n==2 ) return;
    i_gt = n-1;
    i = n/2;
    if( a[0]>a[i] ){
      SWAP_DOUBLE(a[0],a[i])
    }else if( a[i]>a[i_gt] ){
      SWAP_DOUBLE(a[i],a[i_gt])
    }
    if( n==3 ) return;
    r_pivot = a[i];
    i_lt = i = 1;
    do{
      if( a[i]<r_pivot ){
        if( i>i_lt ) SWAP_DOUBLE(a[i],a[i_lt])
        i_lt++;
        i++;
      }else if( a[i]>r_pivot ){
        do{
          i_gt--;
        }while( i_gt>i && a[i_gt]>r_pivot );
        SWAP_DOUBLE(a[i],a[i_gt])
      }else{
        i++;
      }
    }while( i<i_gt );
    if( i_lt>(int)(n/2) ){
      if( n-i_gt>=2 ) __percent_sort(a+i_gt, n-i_gt);
      n = i_lt;
    }else{
      if( i_lt>=2 ) __percent_sort(a, i_lt);
      a += i_gt;
      n -= i_gt;
    }
  }
}

/*
** The "inverse" function for percentile(Y,P) is called to remove a
** row that was previously inserted by "step".
*/
static void __percent_inverse(dbsql_func_t *ctx,int argc,dbsql_value_t **argv){
  percentile_t *p;
  int type;
  double y;
  int64_t i;
  assert( argc==2 || argc==1 );

  /* Allocate the session context. */
  p = (percentile_t*)dbsql_aggregate_context(ctx, sizeof(*p));
  assert( p!=0 );

  /* Ignore rows for which Y is NULL */
  type = dbsql_value_type(argv[0]);
  if( type==DBSQL_NULL ) return;

  /* If not NULL, then Y must be numeric.  Otherwise throw an error.
  ** Requirement 4 */
  if( type!=DBSQL_INTEGER && type!=DBSQL_FLOAT ){
    return;
  }

  /* Ignore the Y value if it is infinity or __na_n */
  y = dbsql_value_double(argv[0]);
  if( __percent_is_infinity(y) ){
    return;
  }
  if( p->sorted==0 ){
    assert( p->n_used>1 );
    __percent_sort(p->a, p->n_used);
    p->sorted = 1;
  }
  p->keep_sorted = 1;

  /* Find and remove the row */
  i = __percent_binary_search(p, y, 1);
  if( i>=0 ){
    p->n_used--;
    if( i<(int)p->n_used ){
      memmove(&p->a[i], &p->a[i+1], (p->n_used - i)*sizeof(p->a[0]));
    }
  }
}

/*
** Compute the final output of percentile().  Clean up all allocated
** memory if and only if is_final is true.
*/
static void __percent_compute(dbsql_func_t *ctx, int is_final){
  percentile_t *p;
  int settings = DBSQL_PTR_TO_INT(dbsql_user_data(ctx))&1; /* Discrete? */
  unsigned i1, i2;
  double v1, v2;
  double ix, vx;
  p = (percentile_t*)dbsql_aggregate_context(ctx, 0);
  if( p==0 ) return;
  if( p->a==0 ) return;
  if( p->n_used ){
    if( p->sorted==0 ){
      assert( p->n_used>1 );
      __percent_sort(p->a, p->n_used);
      p->sorted = 1;
    }
    ix = p->r_pct*(p->n_used-1);
    i1 = (unsigned)ix;
    if( settings & 1 ){
      vx = p->a[i1];
    }else{
      i2 = ix==(double)i1 || i1==p->n_used-1 ? i1 : i1+1;
      v1 = p->a[i1];
      v2 = p->a[i2];
      vx = v1 + (v2-v1)*(ix-i1);
    }
    dbsql_result_double(ctx, vx);
  }
  if( is_final ){
    dbsql_free(p->a);
    memset(p, 0, sizeof(*p));
  }else{
    p->keep_sorted = 1;
  }
}
static void __percent_final(dbsql_func_t *ctx){
  __percent_compute(ctx, 1);
}
static void __percent_value(dbsql_func_t *ctx){
  __percent_compute(ctx, 0);
}
/****** End of percentile family of functions ******/
#endif /* DBSQL_ENABLE_PERCENTILE */

#if defined(DBSQL_DEBUG) || defined(DBSQL_ENABLE_FILESTAT)
/*
** Implementation of dbsql_filestat(SCHEMA).
**
** Return JSON text that describes low-level debug/diagnostic information
** about the dbsql_file_t object associated with SCHEMA.
*/
static void __filestat_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  DBSQL *db = dbsql_context_db_handle(context);
  const char *db_name;
  dbsql_str_t *str;
  sm_t *btree;

  db_name = (const char*)dbsql_value_text(argv[0]);
  btree = __db_name_to_btree(db, db_name);
  if( btree ){
    pager_t *pager;
    dbsql_file_t *fd;
    int rc;
    __sm_enter(btree);
    pager = __sm_pager(btree);
    assert( pager!=0 );
    fd = __sm_pager_file(pager);
    str = dbsql_str_new(db);
    if( str==0 ){
      dbsql_result_error_nomem(context);
    }else{
      dbsql_str_append(str, "{\"db\":", 6);
      rc = __dbsql_os_file_control(fd, DBSQL_FCNTL_FILESTAT, str);
      if( rc ) dbsql_str_append(str, "null", 4);
      fd = __sm_pager_jrnl_file(pager);
      if( fd && fd->methods!=0 ){
        dbsql_str_appendall(str, ",\"journal\":");
        rc = __dbsql_os_file_control(fd, DBSQL_FCNTL_FILESTAT, str);
        if( rc ) dbsql_str_append(str, "null", 4);
      }
      dbsql_str_append(str, "}", 1);
      dbsql_result_text(context, dbsql_str_finish(str), -1,
                          dbsql_free);
    }
    __sm_leave(btree);
  }else{
    dbsql_result_text(context, "{}", 2, DBSQL_STATIC);
  }
}
#endif /* DBSQL_DEBUG || DBSQL_ENABLE_FILESTAT */

#ifdef DBSQL_DEBUG
/*
** Implementation of fpdecode(x,y,z) function.
**
** x is a real number that is to be decoded.  y is the precision.
** z is the maximum real precision.  Return a string that shows the
** results of the __fp_decode() function.
**
** Used for testing and debugging only, specifically testing and debugging
** of the __fp_decode() function.  This SQL function does not appear
** in production builds.  This function is not an API and is subject to
** modification or removal in future versions of dbsql.
*/
static void __fpdecode_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  fp_decode_t s;
  double x;
  int y, z;
  char buf[100];
  UNUSED_PARAMETER(argc);
  assert( argc==3 );
  x = dbsql_value_double(argv[0]);
  y = dbsql_value_int(argv[1]);
  z = dbsql_value_int(argv[2]);
  if( z<=0 ) z = 1;
  __fp_decode(&s, x, y, z);
  if( s.is_special==2 ){
    dbsql_snprintf(sizeof(buf), buf, "NaN");
  }else{
    dbsql_snprintf(sizeof(buf), buf, "%c%.*s/%d", s.sign, s.n, s.z, s.i_dp);
  }
  dbsql_result_text(context, buf, -1, DBSQL_TRANSIENT);
}
#endif /* DBSQL_DEBUG */

#ifdef DBSQL_DEBUG
/*
** Implementation of parseuri(uri,flags) function.
**
** Required Arguments:
**    "uri"        The URI to parse.
**    "flags"      Bitmask of flags, as if to dbsql_open_v2().
**
** Additional arguments beyond the first two make calls to
** dbsql_uri_key() for integers and dbsql_uri_parameter for
** anything else.
**
** The result is a string showing the results of calling __parse_uri().
**
** Used for testing and debugging only, specifically testing and debugging
** of the __parse_uri() function.  This SQL function does not appear
** in production builds.  This function is not an API and is subject to
** modification or removal in future versions of dbsql.
*/
static void __parseuri_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  dbsql_str_t *result;
  const char *z_vfs;
  const char *uri;
  unsigned int flgs;
  int rc;
  dbsql_vfs_t *vfs = 0;
  char *file = 0;
  char *err = 0;

  if( argc<2 ) return;
  vfs = dbsql_vfs_find(0);
  assert( vfs );
  z_vfs = vfs->name;
  uri = (const char*)dbsql_value_text(argv[0]);
  if( uri==0 ) return;
  flgs = (unsigned int)dbsql_value_int(argv[1]);
  rc = __parse_uri(z_vfs, uri, &flgs, &vfs, &file, &err);
  result = dbsql_str_new(0);
  if( result ){
    int i;
    dbsql_str_appendf(result, "rc=%d", rc);
    dbsql_str_appendf(result, ", flags=0x%x", flgs);
    dbsql_str_appendf(result, ", vfs=%Q", vfs ? vfs->name: 0);
    dbsql_str_appendf(result, ", err=%Q", err);
    dbsql_str_appendf(result, ", file=%Q", file);
    if( file ){
      const char *z = file;
      z += __strlen30(z)+1;
      while( z[0] ){
        dbsql_str_appendf(result, ", %Q", z);
        z += __strlen30(z)+1;
      }
      for(i=2; i<argc; i++){
        const char *arg;
        if( dbsql_value_type(argv[i])==DBSQL_INTEGER ){
          int k = dbsql_value_int(argv[i]);
          dbsql_str_appendf(result, ", '%d:%q'",k,dbsql_uri_key(file, k));
        }else if( (arg = (const char*)dbsql_value_text(argv[i]))!=0 ){
          dbsql_str_appendf(result, ", '%q:%q'",
                 arg, dbsql_uri_parameter(file,arg));
        }else{
          dbsql_str_appendf(result, ", NULL");
        }
      }
    }
    dbsql_result_text(ctx, dbsql_str_finish(result), -1, dbsql_free);
  }
  dbsql_free_filename(file);
  dbsql_free(err);
}
#endif /* DBSQL_DEBUG */

/*
** All of the func_def_t structures in the builtin_funcs[] array above
** to the global function hash table.  This occurs at start-time (as
** a consequence of calling dbsql_initialize()).
**
** After this routine runs
*/
void __register_builtin_functions(void){
  /*
  ** The following array holds func_def_t structures for all of the functions
  ** defined in this file.
  **
  ** The array cannot be constant since changes are made to the
  ** func_def_t.hash elements at start-time.  The elements of this array
  ** are read-only after initialization is complete.
  **
  ** For peak efficiency, put the most frequently used function last.
  */
  static func_def_t builtin_funcs[] = {
/***** Functions only available with DBSQL_TESTCTRL_INTERNAL_FUNCTIONS *****/
#if !defined(DBSQL_UNTESTABLE)
    TEST_FUNC(implies_nonnull_row, 2, INLINEFUNC_implies_nonnull_row, 0),
    TEST_FUNC(expr_compare,        2, INLINEFUNC_expr_compare,        0),
    TEST_FUNC(expr_implies_expr,   2, INLINEFUNC_expr_implies_expr,   0),
    TEST_FUNC(affinity,            1, INLINEFUNC_affinity,            0),
#endif /* !defined(DBSQL_UNTESTABLE) */
/***** Regular functions *****/
#ifdef DBSQL_SOUNDEX
    FUNCTION(soundex,            1, 0, 0, __soundex_func      ),
#endif
#ifndef DBSQL_OMIT_LOAD_EXTENSION
    SFUNCTION(load_extension,    1, 0, 0, __load_ext          ),
    SFUNCTION(load_extension,    2, 0, 0, __load_ext          ),
#endif
#ifndef DBSQL_OMIT_COMPILEOPTION_DIAGS
    DFUNCTION(dbsql_compileoption_used,1, 0, 0, __compileoptionused_func  ),
    DFUNCTION(dbsql_compileoption_get, 1, 0, 0, __compileoptionget_func  ),
#endif /* DBSQL_OMIT_COMPILEOPTION_DIAGS */
    INLINE_FUNC(unlikely,        1, INLINEFUNC_unlikely, DBSQL_FUNC_UNLIKELY),
    INLINE_FUNC(likelihood,      2, INLINEFUNC_unlikely, DBSQL_FUNC_UNLIKELY),
    INLINE_FUNC(likely,          1, INLINEFUNC_unlikely, DBSQL_FUNC_UNLIKELY),
#ifdef DBSQL_ENABLE_OFFSET_SQL_FUNC
    INLINE_FUNC(dbsql_offset,   1, INLINEFUNC_dbsql_offset, 0 ),
#endif
#if defined(DBSQL_DEBUG) || defined(DBSQL_ENABLE_FILESTAT)
    FUNCTION(dbsql_filestat,    1, 0, 0, __filestat_func     ),
#endif
    FUNCTION(ltrim,              1, 1, 0, __trim_func         ),
    FUNCTION(ltrim,              2, 1, 0, __trim_func         ),
    FUNCTION(rtrim,              1, 2, 0, __trim_func         ),
    FUNCTION(rtrim,              2, 2, 0, __trim_func         ),
    FUNCTION(trim,               1, 3, 0, __trim_func         ),
    FUNCTION(trim,               2, 3, 0, __trim_func         ),
    FUNCTION(min,               -3, 0, 1, __minmax_func       ),
    WAGGREGATE(min, 1, 0, 1, __minmax_step, __min_max_finalize, MIN_MAX_VALUE, 0,
                                 DBSQL_FUNC_MINMAX|DBSQL_FUNC_ANYORDER ),
    FUNCTION(max,               -3, 1, 1, __minmax_func       ),
    WAGGREGATE(max, 1, 1, 1, __minmax_step, __min_max_finalize, MIN_MAX_VALUE, 0,
                                 DBSQL_FUNC_MINMAX|DBSQL_FUNC_ANYORDER ),
    FUNCTION2(typeof,            1, 0, 0, __typeof_func,  DBSQL_FUNC_TYPEOF),
    FUNCTION2(subtype,           1, 0, 0, __subtype_func,
                                           DBSQL_FUNC_TYPEOF|DBSQL_SUBTYPE),
    FUNCTION2(length,            1, 0, 0, __length_func,  DBSQL_FUNC_LENGTH),
    FUNCTION2(octet_length,      1, 0, 0, __bytelength_func,DBSQL_FUNC_BYTELEN),
    FUNCTION(instr,              2, 0, 0, __instr_func        ),
    FUNCTION(printf,            -1, 0, 0, __printf_func       ),
    FUNCTION(format,            -1, 0, 0, __printf_func       ),
    FUNCTION(unicode,            1, 0, 0, __unicode_func      ),
    FUNCTION(char,              -1, 0, 0, __char_func         ),
    FUNCTION(abs,                1, 0, 0, __abs_func          ),
#ifdef DBSQL_DEBUG
    FUNCTION(fpdecode,           3, 0, 0, __fpdecode_func     ),
    FUNCTION(parseuri,          -1, 0, 0, __parseuri_func     ),
#endif
#ifndef DBSQL_OMIT_FLOATING_POINT
    FUNCTION(round,              1, 0, 0, __round_func        ),
    FUNCTION(round,              2, 0, 0, __round_func        ),
#endif
    FUNCTION(upper,              1, 0, 0, __upper_func        ),
    FUNCTION(lower,              1, 0, 0, __lower_func        ),
    FUNCTION(hex,                1, 0, 0, __hex_func          ),
    FUNCTION(unhex,              1, 0, 0, __unhex_func        ),
    FUNCTION(unhex,              2, 0, 0, __unhex_func        ),
    FUNCTION(concat,            -3, 0, 0, __concat_func       ),
    FUNCTION(concat_ws,         -4, 0, 0, __concatws_func     ),
    INLINE_FUNC(ifnull,          2, INLINEFUNC_coalesce, 0 ),
    VFUNCTION(random,            0, 0, 0, __random_func       ),
    VFUNCTION(randomblob,        1, 0, 0, __random_blob       ),
    FUNCTION(nullif,             2, 0, 1, __nullif_func       ),
    DFUNCTION(dbsql_version,    0, 0, 0, __version_func      ),
    DFUNCTION(dbsql_source_id,  0, 0, 0, __sourceid_func     ),
    FUNCTION(dbsql_log,         2, 0, 0, __errlog_func       ),
    FUNCTION(unistr,             1, 0, 0, __unistr_func       ),
    FUNCTION(quote,              1, 0, 0, __quote_func        ),
    FUNCTION(unistr_quote,       1, 1, 0, __quote_func        ),
    VFUNCTION(last_insert_rowid, 0, 0, 0, last_insert_rowid),
    VFUNCTION(changes,           0, 0, 0, changes          ),
    VFUNCTION(total_changes,     0, 0, 0, total_changes    ),
    FUNCTION(replace,            3, 0, 0, __replace_func      ),
    FUNCTION(zeroblob,           1, 0, 0, __zeroblob_func     ),
    FUNCTION(substr,             2, 0, 0, __substr_func       ),
    FUNCTION(substr,             3, 0, 0, __substr_func       ),
    FUNCTION(substring,          2, 0, 0, __substr_func       ),
    FUNCTION(substring,          3, 0, 0, __substr_func       ),
    WAGGREGATE(sum,   1,0,0, __sum_step, __sum_finalize, __sum_finalize, SUM_INVERSE, 0),
    WAGGREGATE(total, 1,0,0, __sum_step,__total_finalize,__total_finalize,SUM_INVERSE, 0),
    WAGGREGATE(avg,   1,0,0, __sum_step, __avg_finalize, __avg_finalize, SUM_INVERSE, 0),
    WAGGREGATE(count, 0,0,0, __count_step,
        __count_finalize, __count_finalize, COUNT_INVERSE,
        DBSQL_FUNC_COUNT|DBSQL_FUNC_ANYORDER  ),
    WAGGREGATE(count, 1,0,0, __count_step,
        __count_finalize, __count_finalize, COUNT_INVERSE, DBSQL_FUNC_ANYORDER ),
    WAGGREGATE(group_concat, 1, 0, 0, __group_concat_step,
        __group_concat_finalize, GROUP_CONCAT_VALUE, GROUP_CONCAT_INVERSE, 0),
    WAGGREGATE(group_concat, 2, 0, 0, __group_concat_step,
        __group_concat_finalize, GROUP_CONCAT_VALUE, GROUP_CONCAT_INVERSE, 0),
    WAGGREGATE(string_agg,   2, 0, 0, __group_concat_step,
        __group_concat_finalize, GROUP_CONCAT_VALUE, GROUP_CONCAT_INVERSE, 0),

#ifdef DBSQL_ENABLE_PERCENTILE
    WAGGREGATE(median,          1,   0,0, __percent_step,
        __percent_final, __percent_value, __percent_inverse,
        DBSQL_INNOCUOUS|DBSQL_SELFORDER1),
    WAGGREGATE(percentile,      2, 0x2,0, __percent_step,
        __percent_final, __percent_value, __percent_inverse,
        DBSQL_INNOCUOUS|DBSQL_SELFORDER1),
    WAGGREGATE(percentile_cont, 2,   0,0, __percent_step,
        __percent_final, __percent_value, __percent_inverse,
        DBSQL_INNOCUOUS|DBSQL_SELFORDER1),
    WAGGREGATE(percentile_disc, 2, 0x1,0, __percent_step,
        __percent_final, __percent_value, __percent_inverse,
        DBSQL_INNOCUOUS|DBSQL_SELFORDER1),
#endif /* DBSQL_ENABLE_PERCENTILE */
 
    LIKEFUNC(glob, 2, &__glob_info, DBSQL_FUNC_LIKE|DBSQL_FUNC_CASE),
#ifdef DBSQL_CASE_SENSITIVE_LIKE
    LIKEFUNC(like, 2, &__like_info_alt, DBSQL_FUNC_LIKE|DBSQL_FUNC_CASE),
    LIKEFUNC(like, 3, &__like_info_alt, DBSQL_FUNC_LIKE|DBSQL_FUNC_CASE),
#else
    LIKEFUNC(like, 2, &__like_info_norm, DBSQL_FUNC_LIKE),
    LIKEFUNC(like, 3, &__like_info_norm, DBSQL_FUNC_LIKE),
#endif
#ifdef DBSQL_ENABLE_UNKNOWN_SQL_FUNCTION
    FUNCTION(unknown,           -1, 0, 0, __unknown_func      ),
#endif
#ifdef DBSQL_ENABLE_MATH_FUNCTIONS
    MFUNCTION(ceil,              1, __x_ceil,     __ceiling_func ),
    MFUNCTION(ceiling,           1, __x_ceil,     __ceiling_func ),
    MFUNCTION(floor,             1, __x_floor,    __ceiling_func ),
#if DBSQL_HAVE_C99_MATH_FUNCS
    MFUNCTION(trunc,             1, trunc,     __ceiling_func ),
#endif
    FUNCTION(ln,                 1, 0, 0,      __log_func     ),
    FUNCTION(log,                1, 1, 0,      __log_func     ),
    FUNCTION(log10,              1, 1, 0,      __log_func     ),
    FUNCTION(log2,               1, 2, 0,      __log_func     ),
    FUNCTION(log,                2, 0, 0,      __log_func     ),
    MFUNCTION(exp,               1, exp,       math1Func   ),
    MFUNCTION(pow,               2, pow,       math2Func   ),
    MFUNCTION(power,             2, pow,       math2Func   ),
    MFUNCTION(mod,               2, fmod,      math2Func   ),
    MFUNCTION(acos,              1, acos,      math1Func   ),
    MFUNCTION(asin,              1, asin,      math1Func   ),
    MFUNCTION(atan,              1, atan,      math1Func   ),
    MFUNCTION(atan2,             2, atan2,     math2Func   ),
    MFUNCTION(cos,               1, cos,       math1Func   ),
    MFUNCTION(sin,               1, sin,       math1Func   ),
    MFUNCTION(tan,               1, tan,       math1Func   ),
    MFUNCTION(cosh,              1, cosh,      math1Func   ),
    MFUNCTION(sinh,              1, sinh,      math1Func   ),
    MFUNCTION(tanh,              1, tanh,      math1Func   ),
#if DBSQL_HAVE_C99_MATH_FUNCS
    MFUNCTION(acosh,             1, acosh,     math1Func   ),
    MFUNCTION(asinh,             1, asinh,     math1Func   ),
    MFUNCTION(atanh,             1, atanh,     math1Func   ),
#endif
    MFUNCTION(sqrt,              1, sqrt,      math1Func   ),
    MFUNCTION(radians,           1, __deg_to_rad,  math1Func   ),
    MFUNCTION(degrees,           1, __rad_to_deg,  math1Func   ),
    MFUNCTION(pi,                0, 0,         __pi_func      ),
#endif /* DBSQL_ENABLE_MATH_FUNCTIONS */
    FUNCTION(sign,               1, 0, 0,      __sign_func    ),
    INLINE_FUNC(coalesce,       -4, INLINEFUNC_coalesce, 0 ),
    INLINE_FUNC(iif,            -4, INLINEFUNC_iif,      0 ),
    INLINE_FUNC(if,             -4, INLINEFUNC_iif,      0 ),
  };
#ifndef DBSQL_OMIT_ALTERTABLE
  __alter_functions();
#endif
  __window_functions();
  __register_date_time_functions();
  __register_json_functions();
  __insert_builtin_funcs(builtin_funcs, ARRAY_SIZE(builtin_funcs));

#if 0  /* Enable to print out how the built-in functions are hashed */
  {
    int i;
    func_def_t *p;
    for(i=0; i<DBSQL_FUNC_HASH_SZ; i++){
      printf("FUNC-HASH %02d:", i);
      for(p=__builtin_functions.a[i]; p; p=p->u.hash){
        int n = __strlen30(p->name);
        int h = p->name[0] + n;
        assert( p->func_flags & DBSQL_FUNC_BUILTIN );
        printf(" %s(%d)", p->name, h);
      }
      printf("\n");
    }
  }
#endif
}
