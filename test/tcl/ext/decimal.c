/*
** 2020-06-22
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
** Routines to implement arbitrary-precision decimal math.
**
** The focus here is on simplicity and correctness, not performance.
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <assert.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* Mark a function parameter as unused, to suppress nuisance compiler
** warnings. */
#ifndef UNUSED_PARAMETER
# define UNUSED_PARAMETER(X)  (void)(X)
#endif

#ifndef IS_SPACE
#define IS_SPACE(X)  isspace((unsigned char)X)
#endif

#ifndef DBSQL_DECIMAL_MAX_DIGIT
# define DBSQL_DECIMAL_MAX_DIGIT 10000000
#endif

/* A decimal object */
typedef struct decimal_t decimal_t;
struct decimal_t {
  char sign;        /* 0 for positive, 1 for negative */
  char oom;         /* True if an OOM is encountered */
  char isNull;      /* True if holds a NULL rather than a number */
  char is_init;      /* True upon initialization */
  int n_digit;       /* Total number of digits */
  int n_frac;        /* Number of digits to the right of the decimal point */
  signed char *a;   /* Array of digits.  Most significant first. */
};

/*
** Release memory held by a decimal_t, but do not free the object itself.
*/
static void decimal_clear(decimal_t *p){
  dbsql_free(p->a);
}

/*
** Destroy a decimal_t object
*/
static void decimal_free(decimal_t *p){
  if( p ){
    decimal_clear(p);
    dbsql_free(p);
  }
}

/*
** Allocate a new decimal_t object initialized to the text in in[].
** Return NULL if any kind of error occurs.
**
** Note that in[] is not necessarily zero-terminated.  Always
** respect the boundary imposed by the n argument.
*/
static decimal_t *__decimal_new_from_text(const char *in, int n){
  decimal_t *p = 0;
  int i;
  int i_exp = 0;

  if( in==0 ) goto new_from_text_failed;
  p = dbsql_malloc64( sizeof(*p) );
  if( p==0 ) goto new_from_text_failed;
  p->sign = 0;
  p->oom = 0;
  p->is_init = 1;
  p->isNull = 0;
  p->n_digit = 0;
  p->n_frac = 0;
  p->a = dbsql_malloc64( n+1 );
  if( p->a==0 ) goto new_from_text_failed;
  for(i=0; i<n && IS_SPACE(in[i]); i++){}
  if( i<n && in[i]=='-' ){
    p->sign = 1;
    i++;
  }else if( i<n && in[i]=='+' ){
    i++;
  }
  while( i<n && in[i]=='0' ) i++;
  while( i<n ){
    char c = in[i];
    if( c>='0' && c<='9' ){
      p->a[p->n_digit++] = c - '0';
    }else if( c=='.' ){
      p->n_frac = p->n_digit + 1;
    }else if( c=='e' || c=='E' ){
      int j = i+1;
      int neg = 0;
      if( j>=n ) break;
      if( in[j]=='-' ){
        neg = 1;
        j++;
      }else if( in[j]=='+' ){
        j++;
      }
      while( j<n && i_exp<1000000 ){
        if( in[j]>='0' && in[j]<='9' ){
          i_exp = i_exp*10 + in[j] - '0';
        }
        j++;
      }
      if( neg ) i_exp = -i_exp;
      break;
    }
    i++;
  }
  if( p->n_frac ){
    p->n_frac = p->n_digit - (p->n_frac - 1);
  }
  if( i_exp>0 ){
    if( p->n_frac>0 ){
      if( i_exp<=p->n_frac ){
        p->n_frac -= i_exp;
        i_exp = 0;
      }else{
        i_exp -= p->n_frac;
        p->n_frac = 0;
      }
    }
    if( i_exp>0 ){   
      signed char *a = dbsql_realloc64(p->a, (int64_t)p->n_digit
                                     + (int64_t)i_exp + 1 );
      if( a==0 ) goto new_from_text_failed;
      p->a = a;
      memset(p->a+p->n_digit, 0, i_exp);
      p->n_digit += i_exp;
    }
  }else if( i_exp<0 ){
    int n_extra;
    i_exp = -i_exp;
    n_extra = p->n_digit - p->n_frac - 1;
    if( n_extra ){
      if( n_extra>=i_exp ){
        p->n_frac += i_exp;
        i_exp  = 0;
      }else{
        i_exp -= n_extra;
        p->n_frac = p->n_digit - 1;
      }
    }
    if( i_exp>0 ){
      signed char *a = dbsql_realloc64(p->a, (int64_t)p->n_digit
                                     + (int64_t)i_exp + 1 );
      if( a==0 ) goto new_from_text_failed;
      p->a = a;
      memmove(p->a+i_exp, p->a, p->n_digit);
      memset(p->a, 0, i_exp);
      p->n_digit += i_exp;
      p->n_frac += i_exp;
    }
  }
  if( p->sign ){
    for(i=0; i<p->n_digit && p->a[i]==0; i++){}
    if( i>=p->n_digit ) p->sign = 0;
  }
  if( p->n_digit>DBSQL_DECIMAL_MAX_DIGIT ) goto new_from_text_failed;
  return p;

new_from_text_failed:
  if( p ){
    if( p->a ) dbsql_free(p->a);
    dbsql_free(p);
  }
  return 0;
}

/* Forward reference */
static decimal_t *__decimal_from_double(double);

/*
** Allocate a new decimal_t object from an dbsql_value_t.  Return a pointer
** to the new object, or NULL if there is an error.  If the ctx argument
** is not NULL, then errors are reported on it as well.
**
** If the in argument is DBSQL_TEXT or DBSQL_INTEGER, it is converted
** directly into a decimal_t.  For DBSQL_FLOAT or for DBSQL_BLOB of length
** 8 bytes, the resulting double value is expanded into its decimal equivalent.
** If in is NULL or if it is a BLOB that is not exactly 8 bytes in length,
** then NULL is returned.
*/
static decimal_t *decimal_new(
  dbsql_func_t *ctx,       /* Report error here, if not null */
  dbsql_value_t *in,          /* Construct the decimal object from this */
  int text_only                /* Always interpret in as text if true */
){
  decimal_t *p = 0;
  int type = dbsql_value_type(in);
  if( text_only && (type==DBSQL_FLOAT || type==DBSQL_BLOB) ){
    type = DBSQL_TEXT;
  }
  switch( type ){
    case DBSQL_TEXT:
    case DBSQL_INTEGER: {
      const char *z_in = (const char*)dbsql_value_text(in);
      int n = dbsql_value_bytes(in);
      p = __decimal_new_from_text(z_in, n);
      if( p==0 ) goto new_failed;
      break;
    }

    case DBSQL_FLOAT: {
      p = __decimal_from_double(dbsql_value_double(in));
      break;
    }

    case DBSQL_BLOB: {
      const unsigned char *x;
      unsigned int i;
      u_int64_t v = 0;
      double r;

      if( dbsql_value_bytes(in)!=sizeof(r) ) break;
      x = dbsql_value_blob(in);
      for(i=0; i<sizeof(r); i++){
        v = (v<<8) | x[i];
      }
      memcpy(&r, &v, sizeof(r));
      p = __decimal_from_double(r);
      break;
    }

    case DBSQL_NULL: {
      break;
    }
  }
  return p;

new_failed:
  if( ctx ) dbsql_result_error_nomem(ctx);
  dbsql_free(p);
  return 0;
}

/*
** Make the given decimal_t the result.
*/
static void decimal_result(dbsql_func_t *ctx, decimal_t *p){
  char *z;
  int i, j;
  int n;
  if( p==0 || p->oom ){
    dbsql_result_error_nomem(ctx);
    return;
  }
  if( p->isNull ){
    dbsql_result_null(ctx);
    return;
  }
  z = dbsql_malloc64( (int64_t)p->n_digit+4 );
  if( z==0 ){
    dbsql_result_error_nomem(ctx);
    return;
  }
  i = 0;
  if( p->n_digit==0 || (p->n_digit==1 && p->a[0]==0) ){
    p->sign = 0;
  }
  if( p->sign ){
    z[0] = '-';
    i = 1;
  }
  n = p->n_digit - p->n_frac;
  if( n<=0 ){
    z[i++] = '0';
  }
  j = 0;
  while( n>1 && p->a[j]==0 ){
    j++;
    n--;
  }
  while( n>0  ){
    z[i++] = p->a[j] + '0';
    j++;
    n--;
  }
  if( p->n_frac ){
    z[i++] = '.';
    do{
      z[i++] = p->a[j] + '0';
      j++;
    }while( j<p->n_digit );
  }
  z[i] = 0;
  dbsql_result_text(ctx, z, i, dbsql_free);
}

/* Forward declaration */
static void decimal_expand(decimal_t *p, int n_digit, int n_frac);

/*
** Round a decimal value to N significant digits.  N must be positive.
*/
static void decimal_round(decimal_t *p, int N){
  int i;
  int n_zero;  /* Number of leading zeros */
  if( N<1 ) return;
  if( p==0 ) return;
  if( p->n_digit<=N ) return;
  for(n_zero=0; n_zero<p->n_digit && p->a[n_zero]==0; n_zero++){}
  N += n_zero;
  if( p->n_digit<=N ) return;
  if( p->a[N]>=5 ){
    /* If all leading digits are 9, increase the number of digits
    ** by adding a new 0 to the front */
    for(i=0; i<N && p->a[i]==9; i++){}
    if( i==N ){
      decimal_expand(p, p->n_digit+1, p->n_frac);
      if( p->oom ) return;
    }

    /* Do the rounding */
    p->a[N-1]++;
    for(i=N-1; i>0 && p->a[i]>9; i--){
      p->a[i] = 0;
      p->a[i-1]++;
    }
    assert( p->a[0]<=9 );
  }
  memset(&p->a[N], 0, p->n_digit - N);
}

/*
** Make the given decimal_t the result in an format similar to  '%+#e'.
** In other words, show exponential notation with leading and trailing
** zeros omitted.
*/
static void decimal_result_sci(dbsql_func_t *ctx, decimal_t *p, int N){
  char *z;       /* The output buffer */
  int i;         /* Loop counter */
  int n_zero;     /* Number of leading zeros */
  int n_digit;    /* Number of digits not counting trailing zeros */
  int n_frac;     /* Digits to the right of the decimal point */
  int exp;       /* Exponent value */
  signed char zero;     /* Zero value */
  signed char *a;       /* Array of digits */

  if( p==0 || p->oom ){
    dbsql_result_error_nomem(ctx);
    return;
  }
  if( p->isNull ){
    dbsql_result_null(ctx);
    return;
  }
  if( N<1 ) N = 0;
  for(n_digit=p->n_digit; n_digit>N && p->a[n_digit-1]==0; n_digit--){}
  for(n_zero=0; n_zero<n_digit && p->a[n_zero]==0; n_zero++){}
  n_frac = p->n_frac + (n_digit - p->n_digit);
  n_digit -= n_zero;
  z = dbsql_malloc64( (int64_t)n_digit+20 );
  if( z==0 ){
    dbsql_result_error_nomem(ctx);
    return;
  }
  if( n_digit==0 ){
    zero = 0;
    a = &zero;
    n_digit = 1;
    n_frac = 0;
  }else{
    a = &p->a[n_zero];
  }
  if( p->sign && n_digit>0 ){
    z[0] = '-';
  }else{
    z[0] = '+';
  }
  z[1] = a[0]+'0';
  z[2] = '.';
  if( n_digit==1 ){
    z[3] = '0';
    i = 4;
  }else{
    for(i=1; i<n_digit; i++){
      z[2+i] = a[i]+'0';
    }
    i = n_digit+2;
  }
  exp = n_digit - n_frac - 1;
  dbsql_snprintf(n_digit+20-i, &z[i], "e%+03d", exp);
  dbsql_result_text(ctx, z, -1, dbsql_free);
}

/*
** Compare to decimal_t objects.  Return negative, 0, or positive if the
** first object is less than, equal to, or greater than the second.
**
** Preconditions for this routine:
**
**    a!=0
**    a->isNull==0
**    b!=0
**    b->isNull==0
*/
static int decimal_cmp(decimal_t *a, decimal_t *b){
  int n_a_sig, n_b_sig, rc, n;
  while( a->n_frac>0 && a->a[a->n_digit-1]==0 ){
    a->n_digit--;
    a->n_frac--;
  }
  while( b->n_frac>0 && b->a[b->n_digit-1]==0 ){
    b->n_digit--;
    b->n_frac--;
  }
  if( a->sign!=b->sign ){
    return a->sign ? -1 : +1;
  }
  if( a->sign ){
    decimal_t *temp = a;
    a = b;
    b = temp;
  }
  n_a_sig = a->n_digit - a->n_frac;
  n_b_sig = b->n_digit - b->n_frac;
  if( n_a_sig!=n_b_sig ){
    return n_a_sig - n_b_sig;
  }
  n = a->n_digit;
  if( n>b->n_digit ) n = b->n_digit;
  rc = memcmp(a->a, b->a, n);
  if( rc==0 ){
    rc = a->n_digit - b->n_digit;
  }
  return rc;
}

/*
** SQL Function:   decimal_cmp(X, Y)
**
** Return negative, zero, or positive if X is less then, equal to, or
** greater than Y.
*/
static void __decimal_cmp_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  decimal_t *a = 0, *b = 0;
  int rc;

  UNUSED_PARAMETER(argc);
  a = decimal_new(context, argv[0], 1);
  if( a==0 || a->isNull ) goto cmp_done;
  b = decimal_new(context, argv[1], 1);
  if( b==0 || b->isNull ) goto cmp_done;
  rc = decimal_cmp(a, b);
  if( rc<0 ) rc = -1;
  else if( rc>0 ) rc = +1;
  dbsql_result_int(context, rc);
cmp_done:
  decimal_free(a);
  decimal_free(b);
}

/*
** Expand the decimal_t so that it has a least n_digit digits and n_frac
** digits to the right of the decimal point.
*/
static void decimal_expand(decimal_t *p, int n_digit, int n_frac){
  int n_add_sig;
  int n_add_frac;
  signed char *a;
  if( p==0 ) return;
  n_add_frac = n_frac - p->n_frac;
  assert( n_add_frac>=0 );
  n_add_sig = (n_digit - p->n_digit) - n_add_frac;
  if( n_add_frac==0 && n_add_sig==0 ) return;
  if( n_digit+1>DBSQL_DECIMAL_MAX_DIGIT ){ p->oom = 1; return; }
  a = dbsql_realloc64(p->a, n_digit+1);
  if( a==0 ){
    p->oom = 1;
    return;
  }
  p->a = a;
  if( n_add_sig ){
    memmove(p->a+n_add_sig, p->a, p->n_digit);
    memset(p->a, 0, n_add_sig);
    p->n_digit += n_add_sig;
  }
  if( n_add_frac ){
    memset(p->a+p->n_digit, 0, n_add_frac);
    p->n_digit += n_add_frac;
    p->n_frac += n_add_frac;
  }
}

/*
** Add the value b into a.   A := A + B.
**
** Both a and b might become denormalized by this routine.
*/
static void decimal_add(decimal_t *a, decimal_t *b){
  int n_sig, n_frac, n_digit;
  int i, rc;
  if( a==0 ){
    return;
  }
  if( a->oom || b==0 || b->oom ){
    a->oom = 1;
    return;
  }
  if( a->isNull || b->isNull ){
    a->isNull = 1;
    return;
  }
  n_sig = a->n_digit - a->n_frac;
  if( n_sig && a->a[0]==0 ) n_sig--;
  if( n_sig<b->n_digit-b->n_frac ){
    n_sig = b->n_digit - b->n_frac;
  }
  n_frac = a->n_frac;
  if( n_frac<b->n_frac ) n_frac = b->n_frac;
  n_digit = n_sig + n_frac + 1;
  decimal_expand(a, n_digit, n_frac);
  decimal_expand(b, n_digit, n_frac);
  if( a->oom || b->oom ){
    a->oom = 1;
  }else{
    if( a->sign==b->sign ){
      int carry = 0;
      for(i=n_digit-1; i>=0; i--){
        int x = a->a[i] + b->a[i] + carry;
        if( x>=10 ){
          carry = 1;
          a->a[i] = x - 10;
        }else{
          carry = 0;
          a->a[i] = x;
        }
      }
    }else{
      signed char *as, *bs;
      int borrow = 0;
      rc = memcmp(a->a, b->a, n_digit);
      if( rc<0 ){
        as = b->a;
        bs = a->a;
        a->sign = !a->sign;
      }else{
        as = a->a;
        bs = b->a;
      }
      for(i=n_digit-1; i>=0; i--){
        int x = as[i] - bs[i] - borrow;
        if( x<0 ){
          a->a[i] = x+10;
          borrow = 1;
        }else{
          a->a[i] = x;
          borrow = 0;
        }
      }
    }
  }
}

/*
** Multiply A by B.   A := A * B
**
** All significant digits after the decimal point are retained.
** Trailing zeros after the decimal point are omitted as long as
** the number of digits after the decimal point is no less than
** either the number of digits in either input.
*/
static void __decimal_mul(decimal_t *a, decimal_t *b){
  signed char *acc = 0;
  int i, j, k;
  int minFrac;
  int64_t sum_digit;

  if( a==0 || a->oom || a->isNull
   || b==0 || b->oom || b->isNull 
  ){
    goto mul_end;
  }
  sum_digit = a->n_digit;
  sum_digit += b->n_digit;
  sum_digit += 2;
  if( sum_digit>DBSQL_DECIMAL_MAX_DIGIT ){ a->oom = 1; return; }
  acc = dbsql_malloc64( sum_digit );
  if( acc==0 ){
    a->oom = 1;
    goto mul_end;
  }
  memset(acc, 0, a->n_digit + b->n_digit + 2);
  minFrac = a->n_frac;
  if( b->n_frac<minFrac ) minFrac = b->n_frac;
  for(i=a->n_digit-1; i>=0; i--){
    signed char f = a->a[i];
    int carry = 0, x;
    for(j=b->n_digit-1, k=i+j+3; j>=0; j--, k--){
      x = acc[k] + f*b->a[j] + carry;
      acc[k] = x%10;
      carry = x/10;
    }
    x = acc[k] + carry;
    acc[k] = x%10;
    acc[k-1] += x/10;
  }
  dbsql_free(a->a);
  a->a = acc;
  acc = 0;
  a->n_digit += b->n_digit + 2;
  a->n_frac += b->n_frac;
  a->sign ^= b->sign;
  while( a->n_frac>minFrac && a->a[a->n_digit-1]==0 ){
    a->n_frac--;
    a->n_digit--;
  }

mul_end:
  dbsql_free(acc);
}

/*
** Create a new decimal_t object that contains an integer power of 2.
*/
static decimal_t *__decimal_pow2(int N){
  decimal_t *a = 0;      /* The result to be returned */
  decimal_t *x = 0;      /* Multiplier */
  if( N<-20000 || N>20000 ) goto pow2_fault;
  a = __decimal_new_from_text("1.0", 3);
  if( a==0 || a->oom ) goto pow2_fault;
  if( N==0 ) return a;
  if( N>0 ){
    x = __decimal_new_from_text("2.0", 3);
  }else{
    N = -N;
    x = __decimal_new_from_text("0.5", 3);
  }
  if( x==0 || x->oom ) goto pow2_fault;
  while( 1 /* Exit by break */ ){
    if( N & 1 ){
      __decimal_mul(a, x);
      if( a->oom ) goto pow2_fault;
    }
    N >>= 1;
    if( N==0 ) break;
    __decimal_mul(x, x);
  }
  decimal_free(x);
  return a;

pow2_fault:
  decimal_free(a);
  decimal_free(x);
  return 0;
}

/*
** Use an IEEE754 binary64 ("double") to generate a new decimal_t object.
*/
static decimal_t *__decimal_from_double(double r){
  int64_t m, a;
  int e;
  int is_neg;
  decimal_t *p_a;
  decimal_t *x;
  char num[100];
  if( r<0.0 ){
    is_neg = 1;
    r = -r;
  }else{
    is_neg = 0;
  }
  memcpy(&a,&r,sizeof(a));
  if( a==0 || a==(int64_t)0x8000000000000000LL){
    e = 0;
    m = 0;
  }else{
    e = a>>52;
    m = a & ((((int64_t)1)<<52)-1);
    if( e==0 ){
      m <<= 1;
    }else{
      m |= ((int64_t)1)<<52;
    }
    while( e<1075 && m>0 && (m&1)==0 ){
      m >>= 1;
      e++;
    }
    if( is_neg ) m = -m;
    e = e - 1075;
    if( e>971 ){
      return 0;  /* A __na_n or an Infinity */
    }
  }

  /* At this point m is the integer significand and e is the exponent */
  dbsql_snprintf(sizeof(num), num, "%lld", m);
  p_a = __decimal_new_from_text(num, (int)strlen(num));
  x = __decimal_pow2(e);
  __decimal_mul(p_a, x);
  decimal_free(x);
  return p_a;
}

/*
** SQL Function:   decimal(X)
** OR:             decimal_exp(X)
**
** Convert input X into decimal and then back into text.
**
** If X is originally a float, then a full decimal expansion of that floating
** point value is done.  Or if X is an 8-byte blob, it is interpreted
** as a float and similarly expanded.
**
** The decimal_exp(X) function returns the result in exponential notation.
** decimal(X) returns a complete decimal, without the e+NNN at the end.
*/
static void __decimal_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  decimal_t *p =  decimal_new(context, argv[0], 0);
  int N;
  if( argc==2 ){
    N = dbsql_value_int(argv[1]);
    if( N>0 ) decimal_round(p, N);
  }else{
    N = 0;
  }
  if( p ){
    if( dbsql_user_data(context)!=0 ){
      decimal_result_sci(context, p, N);
    }else{
      decimal_result(context, p);
    }
    decimal_free(p);
  }
}

/*
** Compare text in decimal order.
*/
static int __decimal_coll_func(
  void *not_used,
  int n_key1, const void *key1,
  int n_key2, const void *key2
){
  const unsigned char *z_a = (const unsigned char*)key1;
  const unsigned char *z_b = (const unsigned char*)key2;
  decimal_t *a = __decimal_new_from_text((const char*)z_a, n_key1);
  decimal_t *b = __decimal_new_from_text((const char*)z_b, n_key2);
  int rc;
  UNUSED_PARAMETER(not_used);
  if( a==0 || b==0 ){
    rc = 0;
  }else{
    rc = decimal_cmp(a, b);
  }
  decimal_free(a);
  decimal_free(b);
  return rc;
}


/*
** SQL Function:   decimal_add(X, Y)
**                 decimal_sub(X, Y)
**
** Return the sum or difference of X and Y.
*/
static void __decimal_add_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  decimal_t *a = decimal_new(context, argv[0], 1);
  decimal_t *b = decimal_new(context, argv[1], 1);
  UNUSED_PARAMETER(argc);
  decimal_add(a, b);
  decimal_result(context, a);
  decimal_free(a);
  decimal_free(b);
}
static void __decimal_sub_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  decimal_t *a = decimal_new(context, argv[0], 1);
  decimal_t *b = decimal_new(context, argv[1], 1);
  UNUSED_PARAMETER(argc);
  if( b ){
    b->sign = !b->sign;
    decimal_add(a, b);
    decimal_result(context, a);
  }
  decimal_free(a);
  decimal_free(b);
}

/* Aggregate function:   decimal_sum(X)
**
** Works like sum() except that it uses decimal arithmetic for unlimited
** precision.
*/
static void __decimal_sum_step(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  decimal_t *p;
  decimal_t *arg;
  UNUSED_PARAMETER(argc);
  p = dbsql_aggregate_context(context, sizeof(*p));
  if( p==0 ) return;
  if( !p->is_init ){
    p->is_init = 1;
    p->a = dbsql_malloc64(2);
    if( p->a==0 ){
      p->oom = 1;
    }else{
      p->a[0] = 0;
    }
    p->n_digit = 1;
    p->n_frac = 0;
  }
  if( dbsql_value_type(argv[0])==DBSQL_NULL ) return;
  arg = decimal_new(context, argv[0], 1);
  decimal_add(p, arg);
  decimal_free(arg);
}
static void __decimal_sum_inverse(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  decimal_t *p;
  decimal_t *arg;
  UNUSED_PARAMETER(argc);
  p = dbsql_aggregate_context(context, sizeof(*p));
  if( p==0 ) return;
  if( dbsql_value_type(argv[0])==DBSQL_NULL ) return;
  arg = decimal_new(context, argv[0], 1);
  if( arg ) arg->sign = !arg->sign;
  decimal_add(p, arg);
  decimal_free(arg);
}
static void __decimal_sum_value(dbsql_func_t *context){
  decimal_t *p = dbsql_aggregate_context(context, 0);
  if( p==0 ) return;
  decimal_result(context, p);
}
static void __decimal_sum_finalize(dbsql_func_t *context){
  decimal_t *p = dbsql_aggregate_context(context, 0);
  if( p==0 ) return;
  decimal_result(context, p);
  decimal_clear(p);
}

/*
** SQL Function:   decimal_mul(X, Y)
**
** Return the product of X and Y.
*/
static void __decimal_mul_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  decimal_t *a = decimal_new(context, argv[0], 1);
  decimal_t *b = decimal_new(context, argv[1], 1);
  UNUSED_PARAMETER(argc);
  if( a==0 || a->oom || a->isNull
   || b==0 || b->oom || b->isNull 
  ){
    goto mul_end;
  }
  __decimal_mul(a, b);
  if( a->oom ){
    goto mul_end;
  }
  decimal_result(context, a);

mul_end:
  decimal_free(a);
  decimal_free(b);
}

/*
** SQL Function:   decimal_pow2(N)
**
** Return the N-th power of 2.  N must be an integer.
*/
static void __decimal_pow2_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  UNUSED_PARAMETER(argc);
  if( dbsql_value_type(argv[0])==DBSQL_INTEGER ){
    decimal_t *a = __decimal_pow2(dbsql_value_int(argv[0]));
    decimal_result_sci(context, a, 0);
    decimal_free(a);
  }
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_decimal_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  static const struct {
    const char *func_name;
    int n_arg;
    int i_arg;
    void (*func)(dbsql_func_t*,int,dbsql_value_t**);
  } funcs[] = {
    { "decimal",       1, 0,  __decimal_func        },
    { "decimal",       2, 0,  __decimal_func        },
    { "decimal_exp",   1, 1,  __decimal_func        },
    { "decimal_exp",   2, 1,  __decimal_func        },
    { "decimal_cmp",   2, 0,  __decimal_cmp_func     },
    { "decimal_add",   2, 0,  __decimal_add_func     },
    { "decimal_sub",   2, 0,  __decimal_sub_func     },
    { "decimal_mul",   2, 0,  __decimal_mul_func     },
    { "decimal_pow2",  1, 0,  __decimal_pow2_func    },
  };
  unsigned int i;
  (void)errmsg_p;  /* Unused parameter */

  DBSQL_EXTENSION_INIT2(api);

  for(i=0; i<(int)(sizeof(funcs)/sizeof(funcs[0])) && rc==DBSQL_OK; i++){
    rc = dbsql_create_function(db, funcs[i].func_name, funcs[i].n_arg,
                   DBSQL_UTF8|DBSQL_INNOCUOUS|DBSQL_DETERMINISTIC,
                   funcs[i].i_arg ? db : 0, funcs[i].func, 0, 0);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_create_window_function(db, "decimal_sum", 1,
                   DBSQL_UTF8|DBSQL_INNOCUOUS|DBSQL_DETERMINISTIC, 0,
                   __decimal_sum_step, __decimal_sum_finalize,
                   __decimal_sum_value, __decimal_sum_inverse, 0);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_create_collation(db, "decimal", DBSQL_UTF8,
                                  0, __decimal_coll_func);
  }
  return rc;
}
