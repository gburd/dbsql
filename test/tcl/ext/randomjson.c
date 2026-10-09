/*
** 2023-04-28
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
** This dbsql extension implements a the random_json(SEED) and
** random_json5(SEED) functions.  Given a numeric SEED value, these
** routines generate pseudo-random JSON or JSON5, respectively.  The
** same value is always generated for the same seed.
**
** These SQL functions are intended for testing.  They do not have any
** practical real-world use, that we know of.
**
** COMPILE:
**
**     gcc --shared -f_pic -o randomjson.so -I. ext/misc/randomjson.c
**
** USING FROM THE CLI:
**
**     .load ./randomjson
**     SELECT random_json(1);
**     SELECT random_json5(1);
*/
#ifdef DBSQL_STATIC_RANDOMJSON
#  include "dbsql.h"
#else
#  include "dbsql_ext.h"
   DBSQL_EXTENSION_INIT1
#endif
#include <assert.h>
#include <string.h>
#include <stdlib.h>

/* Pseudo-random number generator */
typedef struct prng_t {
  unsigned int x, y;
} prng_t;

/* Reseed the PRNG */
static void __prng_seed(prng_t *p, unsigned int i_seed){
  p->x = i_seed | 1;
  p->y = i_seed;
}

/* Extract a random number */
static unsigned int __prng_int(prng_t *p){
  p->x = (p->x>>1) ^ ((1+~(p->x&1)) & 0xd0000001);
  p->y = p->y*1103515245 + 12345;
  return p->x ^ p->y;
}

static char *__az_json_atoms[] = {
  /* JSON                    JSON-5 */
  "0",                       "0",
  "1",                       "1",
  "-1",                      "-1",
  "2",                       "+2",
  "3DDDD",                   "3DDDD",
  "2.5DD",                   "2.5DD",
  "0.75",                    ".75",
  "-4.0e2",                  "-4.e2",
  "5.0e-3",                  "+5e-3",
  "6.DDe+0DD",                "6.DDe+0DD",
  "0",                       "0x0",
  "512",                     "0x200",
  "256",                     "+0x100",
  "-2748",                   "-0xabc",
  "true",                    "true",
  "false",                   "false",
  "null",                    "null",
  "9.0e999",                 "Infinity",
  "-9.0e999",                "-Infinity",
  "9.0e999",                 "+Infinity",
  "null",                    "NaN",
  "-0.0005DD",              "-0.0005DD",
  "4.35e-3",                 "+4.35e-3",
  "\"gem\\\"hay\"",          "\"gem\\\"hay\"",
  "\"icy'joy\"",             "'icy\\'joy\'",
  "\"keylog\"",              "\"key\\\nlog\"",
  "\"mix\\\\\\tnet\"",       "\"mix\\\\\\tnet\"",
  "\"oat\\r\\n\"",           "\"oat\\r\\n\"",
  "\"\\fpan\\b\"",           "\"\\fpan\\b\"",
  "{}",                      "{}",
  "[]",                      "[]",
  "[]",                      "[/*empty*/]",
  "{}",                      "{//empty\n}",
  "\"ask\"",                 "\"ask\"",
  "\"bag\"",                 "\"bag\"",
  "\"can\"",                 "\"can\"",
  "\"day\"",                 "\"day\"",
  "\"end\"",                 "'end'",
  "\"fly\"",                 "\"fly\"",
  "\"\\u00XX\\u00XX\"",      "\"\\xXX\\xXX\"",
  "\"y\\uXXXXz\"",           "\"y\\uXXXXz\"",
  "\"\"",                    "\"\"",
};
static char *__az_json_template[] = {
  /* JSON                                      JSON-5 */
  "{\"a\":%,\"b\":%,\"cDD\":%}",               "{a:%,b:%,cDD:%}",
  "{\"a\":%,\"b\":%,\"c\":%,\"d\":%,\"e\":%}", "{a:%,b:%,c:%,d:%,e:%}",
  "{\"a\":%,\"b\":%,\"c\":%,\"d\":%,\"\":%}",  "{a:%,b:%,c:%,d:%,'':%}",
  "{\"d\":%}",                                 "{d:%}",
  "{\"eeee\":%, \"ffff\":%}",                  "{eeee:% /*and*/, ffff:%}",
  "{\"$g\":%,\"_h_\":%,\"a b c d\":%}",        "{$g:%,_h_:%,\"a b c d\":%}",
  "{\"x\":%,\n  \"y\":%}",                     "{\"x\":%,\n  \"y\":%}",
  "{\"\\u00XX\":%,\"\\uXXXX\":%}",             "{\"\\xXX\":%,\"\\uXXXX\":%}",
  "{\"Z\":%}",                                 "{Z:%,}",
  "[%]",                                       "[%,]",
  "[%,%]",                                     "[%,%]",
  "[%,%,%]",                                   "[%,%,%,]",
  "[%,%,%,%]",                                 "[%,%,%,%]",
  "[%,%,%,%,%]",                               "[%,%,%,%,%]",
};

#define count(X)  (sizeof(X)/sizeof(X[0]))

#define STRSZ 10000

static void __json_expand(
  const char *src,
  char *dest,
  prng_t *p,
  int type,            /* 0 for JSON, 1 for JSON5 */
  unsigned int r        /* Growth probability 0..1000.  0 means no growth */
){
  unsigned int i, j, k;
  char *z;
  char *x;
  size_t n;
  char buf[200];

  j = 0;
  if( src==0 ) src = "%";
  if( strlen(src)>=STRSZ/10 ) r = 0;
  for(i=0; src[i]; i++){
    if( src[i]!='%' ){
      if( j<STRSZ ) dest[j++] = src[i];
      continue;
    }
    if( r==0 || (r<1000 && (__prng_int(p)%1000)<=r) ){
      /* Fill in without values without any new % */
      k = __prng_int(p)%(count(__az_json_atoms)/2);
      k = k*2 + type;
      z = __az_json_atoms[k];
    }else{
      /* Add new % terms */
      k = __prng_int(p)%(count(__az_json_template)/2);
      k = k*2 + type;
      z = __az_json_template[k];
    }
    n = strlen(z);
    if( (x = strstr(z,"XX"))!=0 ){
      unsigned int y = __prng_int(p);
      if( (y&0xff)==((y>>8)&0xff) ) y += 0x100;
      while( (y&0xff)==((y>>16)&0xff) || ((y>>8)&0xff)==((y>>16)&0xff) ){
        y += 0x10000;
      }
      memcpy(buf, z, n+1);
      z = buf;
      x = strstr(z,"XX");
      while( x!=0 ){
        x[0] = "0123456789abcdef"[y%16];  y /= 16;
        x[1] = "0123456789abcdef"[y%16];  y /= 16;
        x = strstr(x, "XX");
      }
    }else if( (x = strstr(z,"DD"))!=0 ){
      unsigned int y = __prng_int(p);
      memcpy(buf, z, n+1);
      z = buf;
      x = strstr(z,"DD");
      while( x!=0 ){
        x[0] = "0123456789"[y%10];  y /= 10;
        x[1] = "0123456789"[y%10];  y /= 10;
        x = strstr(x, "DD");
      }
    }
    assert( strstr(z, "XX")==0 );
    assert( strstr(z, "DD")==0 );
    if( j+n<STRSZ ){
      memcpy(&dest[j], z, n);
      j += (int)n;
    }
  }
  dest[STRSZ-1] = 0;
  if( j<STRSZ ) dest[j] = 0;
}

static void __rand_json_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  unsigned int i_seed;
  int type = *(int*)dbsql_user_data(context);
  prng_t prng;
  char z1[STRSZ+1], z2[STRSZ+1];

  i_seed = (unsigned int)dbsql_value_int(argv[0]);
  __prng_seed(&prng, i_seed);
  __json_expand(0, z2, &prng, type, 1000);
  __json_expand(z2, z1, &prng, type, 1000);
  __json_expand(z1, z2, &prng, type, 100);
  __json_expand(z2, z1, &prng, type, 0);
  dbsql_result_text(context, z1, -1, DBSQL_TRANSIENT);
}

#ifdef _WIN32
  __declspec(dllexport)
#endif
int dbsql_randomjson_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  static int c_one = 1;
  static int c_zero = 0;
  int rc = DBSQL_OK;
#ifdef DBSQL_STATIC_RANDOMJSON
  (void)api;      /* Unused parameter */
#else
  DBSQL_EXTENSION_INIT2(api);
#endif
  (void)errmsg_p;  /* Unused parameter */
  rc = dbsql_create_function(db, "random_json", 1,
                   DBSQL_UTF8|DBSQL_INNOCUOUS|DBSQL_DETERMINISTIC,
                   &c_zero, __rand_json_func, 0, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "random_json5", 1,
                   DBSQL_UTF8|DBSQL_INNOCUOUS|DBSQL_DETERMINISTIC,
                   &c_one, __rand_json_func, 0, 0);
  }
  return rc;
}
