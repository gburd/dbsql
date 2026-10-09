/*
** 2013-10-14
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This dbsql extension implements functions tointeger(X) and toreal(X).
**
** If X is an integer, real, or string value that can be
** losslessly represented as an integer, then tointeger(X)
** returns the corresponding integer value.
** If X is an 8-byte BLOB then that blob is interpreted as
** a signed two-compliment little-endian encoding of an integer
** and tointeger(X) returns the corresponding integer value.
** Otherwise tointeger(X) return NULL.
**
** If X is an integer, real, or string value that can be
** convert into a real number, preserving at least 15 digits
** of precision, then toreal(X) returns the corresponding real value.
** If X is an 8-byte BLOB then that blob is interpreted as
** a 64-bit IEEE754 big-endian floating point value
** and toreal(X) returns the corresponding real value.
** Otherwise toreal(X) return NULL.
**
** Note that tointeger(X) of an 8-byte BLOB assumes a little-endian
** encoding whereas toreal(X) of an 8-byte BLOB assumes a big-endian
** encoding.
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

/*
** Determine if this is running on a big-endian or little-endian
** processor
*/
#if defined(i386) || defined(__i386__) || defined(_M_IX86)\
                             || defined(__x86_64) || defined(__x86_64__)
# define TOTYPE_BIGENDIAN    0
# define TOTYPE_LITTLEENDIAN 1
#else
  const int totype_one = 1;
# define TOTYPE_BIGENDIAN    (*(char *)(&totype_one)==0)
# define TOTYPE_LITTLEENDIAN (*(char *)(&totype_one)==1)
#endif

/*
** Constants for the largest and smallest possible 64-bit signed integers.
** These macros are designed to work correctly on both 32-bit and 64-bit
** compilers.
*/
#ifndef LARGEST_INT64
# define LARGEST_INT64   (0xffffffff|(((int64_t)0x7fffffff)<<32))
#endif

#ifndef SMALLEST_INT64
# define SMALLEST_INT64  (((int64_t)-1) - LARGEST_INT64)
#endif

/*
** Return TRUE if character c is a whitespace character
*/
static int __totype_isspace(unsigned char c){
  return c==' ' || c=='\t' || c=='\n' || c=='\v' || c=='\f' || c=='\r';
}

/*
** Return TRUE if character c is a digit
*/
static int __totype_isdigit(unsigned char c){
  return c>='0' && c<='9';
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
**      __totype_compare2pow63("9223372036854775800")
**
** will return -8.
*/
static int __totype_compare2pow63(const char *num){
  int c = 0;
  int i;
                    /* 012345678901234567 */
  const char *pow63 = "922337203685477580";
  for(i=0; c==0 && i<18; i++){
    c = (num[i]-pow63[i])*10;
  }
  if( c==0 ){
    c = num[18] - '8';
  }
  return c;
}

/*
** Convert num to a 64-bit signed integer.
**
** If the num value is representable as a 64-bit twos-complement
** integer, then write that value into *num and return 0.
**
** If num is exactly 9223372036854665808, return 2.  This special
** case is broken out because while 9223372036854665808 cannot be a
** signed 64-bit integer, its negative -9223372036854665808 can be.
**
** If num is too big for a 64-bit integer and is not
** 9223372036854665808  or if num contains any non-numeric text,
** then return 1.
**
** The string is not necessarily zero-terminated.
*/
static int __totype_atoi64(const char *z_num, int64_t *num, int length){
  u_int64_t u = 0;
  int neg = 0; /* assume positive */
  int i;
  int c = 0;
  int non_num = 0;
  const char *start;
  const char *end = z_num + length;

  while( z_num<end && __totype_isspace(*z_num) ) z_num++;
  if( z_num<end ){
    if( *z_num=='-' ){
      neg = 1;
      z_num++;
    }else if( *z_num=='+' ){
      z_num++;
    }
  }
  start = z_num;
  while( z_num<end && z_num[0]=='0' ){ z_num++; } /* Skip leading zeros. */
  for(i=0; &z_num[i]<end && (c=z_num[i])>='0' && c<='9'; i++){
    u = u*10 + c - '0';
  }
  if( u>LARGEST_INT64 ){
    *num = SMALLEST_INT64;
  }else if( neg ){
    *num = -(int64_t)u;
  }else{
    *num = (int64_t)u;
  }
  if( (c!=0 && &z_num[i]<end) || (i==0 && start==z_num) || i>19 || non_num ){
    /* num is empty or contains non-numeric text or is longer
    ** than 19 digits (thus guaranteeing that it is too large) */
    return 1;
  }else if( i<19 ){
    /* Less than 19 digits, so we know that it fits in 64 bits */
    assert( u<=LARGEST_INT64 );
    return 0;
  }else{
    /* num is a 19-digit numbers.  Compare it against 9223372036854775808. */
    c = __totype_compare2pow63(z_num);
    if( c<0 ){
      /* num is less than 9223372036854775808 so it fits */
      assert( u<=LARGEST_INT64 );
      return 0;
    }else if( c>0 ){
      /* num is greater than 9223372036854775808 so it overflows */
      return 1;
    }else{
      /* num is exactly 9223372036854775808.  Fits if negative.  The
      ** special case 2 overflow if positive */
      assert( u-1==LARGEST_INT64 );
      assert( (*num)==SMALLEST_INT64 );
      return neg ? 0 : 2;
    }
  }
}

/*
** The string z[] is an text representation of a real number.
** Convert this string to a double and write it into *result.
**
** The string is not necessarily zero-terminated.
**
** Return TRUE if the result is a valid real number (or integer) and FALSE
** if the string is empty or contains extraneous text.  Valid numbers
** are in one of these formats:
**
**    [+-]digits[E[+-]digits]
**    [+-]digits.[digits][E[+-]digits]
**    [+-].digits[E[+-]digits]
**
** Leading and trailing whitespace is ignored for the purpose of determining
** validity.
**
** If some prefix of the input string is a valid number, this routine
** returns FALSE but it still converts the prefix and writes the result
** into *result.
*/
static int __totype_ato_f(const char *z, double *p_result, int length){
  const char *end = z + length;
  /* sign * significand * (10 ^ (esign * exponent)) */
  int sign = 1;    /* sign of significand */
  int64_t s = 0;       /* significand */
  int d = 0;       /* adjust exponent for shifting decimal point */
  int esign = 1;   /* sign of exponent */
  int e = 0;       /* exponent */
  int valid = 1;  /* True exponent is either not used or is well-formed */
  double result;
  int n_digits = 0;
  int non_num = 0;

  *p_result = 0.0;   /* Default return value, in case of an error */

  /* skip leading spaces */
  while( z<end && __totype_isspace(*z) ) z++;
  if( z>=end ) return 0;

  /* get sign of significand */
  if( *z=='-' ){
    sign = -1;
    z++;
  }else if( *z=='+' ){
    z++;
  }

  /* skip leading zeroes */
  while( z<end && z[0]=='0' ) z++, n_digits++;

  /* copy max significant digits to significand */
  while( z<end && __totype_isdigit(*z) && s<((LARGEST_INT64-9)/10) ){
    s = s*10 + (*z - '0');
    z++, n_digits++;
  }

  /* skip non-significant significand digits
  ** (increase exponent by d to shift decimal left) */
  while( z<end && __totype_isdigit(*z) ) z++, n_digits++, d++;
  if( z>=end ) goto totype_atof_calc;

  /* if decimal point is present */
  if( *z=='.' ){
    z++;
    /* copy digits from after decimal to significand
    ** (decrease exponent by d to shift decimal right) */
    while( z<end && __totype_isdigit(*z) && s<((LARGEST_INT64-9)/10) ){
      s = s*10 + (*z - '0');
      z++, n_digits++, d--;
    }
    /* skip non-significant digits */
    while( z<end && __totype_isdigit(*z) ) z++, n_digits++;
  }
  if( z>=end ) goto totype_atof_calc;

  /* if exponent is present */
  if( *z=='e' || *z=='E' ){
    z++;
    valid = 0;
    if( z>=end ) goto totype_atof_calc;
    /* get sign of exponent */
    if( *z=='-' ){
      esign = -1;
      z++;
    }else if( *z=='+' ){
      z++;
    }
    /* copy digits to exponent */
    while( z<end && __totype_isdigit(*z) ){
      e = e<10000 ? (e*10 + (*z - '0')) : 10000;
      z++;
      valid = 1;
    }
  }

  /* skip trailing spaces */
  if( n_digits && valid ){
    while( z<end && __totype_isspace(*z) ) z++;
  }

totype_atof_calc:
  /* adjust exponent by d, and update sign */
  e = (e*esign) + d;
  if( e<0 ) {
    esign = -1;
    e *= -1;
  } else {
    esign = 1;
  }

  /* if 0 significand */
  if( !s ) {
    /* In the IEEE 754 standard, zero is signed.
    ** Add the sign if we've seen at least one digit */
    result = (sign<0 && n_digits) ? -(double)0 : (double)0;
  } else {
    /* attempt to reduce exponent */
    if( esign>0 ){
      while( s<(LARGEST_INT64/10) && e>0 ) e--,s*=10;
    }else{
      while( !(s%10) && e>0 ) e--,s/=10;
    }

    /* adjust the sign of significand */
    s = sign<0 ? -s : s;

    /* if exponent, scale significand as appropriate
    ** and store in result. */
    if( e ){
      double scale = 1.0;
      /* attempt to handle extremely small/large numbers better */
      if( e>307 && e<342 ){
        while( e%308 ) { scale *= 1.0e+1; e -= 1; }
        if( esign<0 ){
          result = s / scale;
          result /= 1.0e+308;
        }else{
          result = s * scale;
          result *= 1.0e+308;
        }
      }else if( e>=342 ){
        if( esign<0 ){
          result = 0.0*s;
        }else{
          result = 1e308*1e308*s;  /* Infinity */
        }
      }else{
        /* 1.0e+22 is the largest power of 10 than can be
        ** represented exactly. */
        while( e%22 ) { scale *= 1.0e+1; e -= 1; }
        while( e>0 ) { scale *= 1.0e+22; e -= 22; }
        if( esign<0 ){
          result = s / scale;
        }else{
          result = s * scale;
        }
      }
    } else {
      result = (double)s;
    }
  }

  /* store the result */
  *p_result = result;

  /* return true if number and no extra non-whitespace characters after */
  return z>=end && n_digits>0 && valid && non_num==0;
}

/* 
** Convert a floating point value to an integer. Or, if this cannot be
** done in a way that avoids 'outside the range of representable values' 
** warnings from UBSAN, return 0.
**
** This function is a modified copy of internal dbsql function
** __real_to_i64().
*/
static int64_t __totype_double_to_int(double r){
  if( r<-9223372036854774784.0 ) return 0;
  if( r>+9223372036854774784.0 ) return 0;
  return (int64_t)r;
}

/*
** tointeger(X):  If X is any value (integer, double, blob, or string) that
** can be losslessly converted into an integer, then make the conversion and
** return the result.  Otherwise, return NULL.
*/
static void __tointeger_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  assert( argc==1 );
  (void)argc;
  switch( dbsql_value_type(argv[0]) ){
    case DBSQL_FLOAT: {
      double r_val = dbsql_value_double(argv[0]);
      int64_t i_val = __totype_double_to_int(r_val);
      if( r_val==(double)i_val ){
        dbsql_result_int64(context, i_val);
      }
      break;
    }
    case DBSQL_INTEGER: {
      dbsql_result_int64(context, dbsql_value_int64(argv[0]));
      break;
    }
    case DBSQL_BLOB: {
      const unsigned char *blob = dbsql_value_blob(argv[0]);
      if( blob ){
        int n_blob = dbsql_value_bytes(argv[0]);
        if( n_blob==sizeof(int64_t) ){
          int64_t i_val;
          if( TOTYPE_BIGENDIAN ){
            int i;
            unsigned char blob_rev[sizeof(int64_t)];
            for(i=0; i<sizeof(int64_t); i++){
              blob_rev[i] = blob[sizeof(int64_t)-1-i];
            }
            memcpy(&i_val, blob_rev, sizeof(int64_t));
          }else{
            memcpy(&i_val, blob, sizeof(int64_t));
          }
          dbsql_result_int64(context, i_val);
        }
      }
      break;
    }
    case DBSQL_TEXT: {
      const unsigned char *str = dbsql_value_text(argv[0]);
      if( str ){
        int n_str = dbsql_value_bytes(argv[0]);
        if( n_str && !__totype_isspace(str[0]) ){
          int64_t i_val;
          if( !__totype_atoi64((const char*)str, &i_val, n_str) ){
            dbsql_result_int64(context, i_val);
          }
        }
      }
      break;
    }
    default: {
      assert( dbsql_value_type(argv[0])==DBSQL_NULL );
      break;
    }
  }
}

/*
** toreal(X): If X is any value (integer, double, blob, or string) that can
** be losslessly converted into a real number, then do so and return that
** real number.  Otherwise return NULL.
*/
#if defined(_MSC_VER)
#pragma warning(disable: 4748)
#pragma optimize("", off)
#endif
static void __toreal_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  assert( argc==1 );
  (void)argc;
  switch( dbsql_value_type(argv[0]) ){
    case DBSQL_FLOAT: {
      dbsql_result_double(context, dbsql_value_double(argv[0]));
      break;
    }
    case DBSQL_INTEGER: {
      int64_t i_val = dbsql_value_int64(argv[0]);
      double r_val = (double)i_val;
      if( i_val==__totype_double_to_int(r_val) ){
        dbsql_result_double(context, r_val);
      }
      break;
    }
    case DBSQL_BLOB: {
      const unsigned char *blob = dbsql_value_blob(argv[0]);
      if( blob ){
        int n_blob = dbsql_value_bytes(argv[0]);
        if( n_blob==sizeof(double) ){
          double r_val;
          if( TOTYPE_LITTLEENDIAN ){
            int i;
            unsigned char blob_rev[sizeof(double)];
            for(i=0; i<sizeof(double); i++){
              blob_rev[i] = blob[sizeof(double)-1-i];
            }
            memcpy(&r_val, blob_rev, sizeof(double));
          }else{
            memcpy(&r_val, blob, sizeof(double));
          }
          dbsql_result_double(context, r_val);
        }
      }
      break;
    }
    case DBSQL_TEXT: {
      const unsigned char *str = dbsql_value_text(argv[0]);
      if( str ){
        int n_str = dbsql_value_bytes(argv[0]);
        if( n_str && !__totype_isspace(str[0]) && !__totype_isspace(str[n_str-1]) ){
          double r_val;
          if( __totype_ato_f((const char*)str, &r_val, n_str) ){
            dbsql_result_double(context, r_val);
            return;
          }
        }
      }
      break;
    }
    default: {
      assert( dbsql_value_type(argv[0])==DBSQL_NULL );
      break;
    }
  }
}
#if defined(_MSC_VER)
#pragma optimize("", on)
#pragma warning(default: 4748)
#endif

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_totype_init(
  DBSQL *db,
  char **errmsg_p,
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Unused parameter */
  rc = dbsql_create_function(db, "tointeger", 1,
        DBSQL_UTF8 | DBSQL_DETERMINISTIC | DBSQL_INNOCUOUS, 0,
        __tointeger_func, 0, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "toreal", 1,
        DBSQL_UTF8 | DBSQL_DETERMINISTIC | DBSQL_INNOCUOUS, 0,
        __toreal_func, 0, 0);
  }
  return rc;
}
