/*
** 2008 March 19
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing all sorts of dbsql interfaces.  This code
** implements new SQL functions used by the test scripts.
*/
#include "dbsql.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "dbsql_int.h"
#include "vdbe_int.h"

/*
** Allocate n_byte bytes of space using dbsql_malloc(). If the
** allocation fails, call dbsql_result_error_nomem() to notify
** the database handle that malloc() has failed.
*/
static void *__test_context_malloc(dbsql_func_t *context, int n_byte){
  char *z = dbsql_malloc(n_byte);
  if( !z && n_byte>0 ){
    dbsql_result_error_nomem(context);
  }
  return z;
}

/*
** This function generates a string of random characters.  Used for
** generating test data.
*/
static void __rand_str(dbsql_func_t *context, int argc, dbsql_value_t **argv){
  static const unsigned char src[] = 
     "abcdefghijklmnopqrstuvwxyz"
     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
     "0123456789"
     ".-!,:*^+=_|?/<> ";
  int i_min, i_max, n, r, i;
  unsigned char buf[1000];

  /* It used to be possible to call randstr() with any number of arguments,
  ** but now it is registered with dbsql as requiring exactly 2.
  */
  assert(argc==2);

  i_min = dbsql_value_int(argv[0]);
  if( i_min<0 ) i_min = 0;
  if( i_min>=sizeof(buf) ) i_min = sizeof(buf)-1;
  i_max = dbsql_value_int(argv[1]);
  if( i_max<i_min ) i_max = i_min;
  if( i_max>=sizeof(buf) ) i_max = sizeof(buf)-1;
  n = i_min;
  if( i_max>i_min ){
    dbsql_randomness(sizeof(r), &r);
    r &= 0x7fffffff;
    n += r%(i_max + 1 - i_min);
  }
  assert( n<sizeof(buf) );
  dbsql_randomness(n, buf);
  for(i=0; i<n; i++){
    buf[i] = src[buf[i]%(sizeof(src)-1)];
  }
  buf[n] = 0;
  dbsql_result_text(context, (char*)buf, n, DBSQL_TRANSIENT);
}

/*
** The following two SQL functions are used to test returning a text
** result with a destructor. Function 'test_destructor' takes one argument
** and returns the same argument interpreted as TEXT. A destructor is
** passed with the dbsql_result_text() call.
**
** SQL function 'test_destructor_count' returns the number of outstanding 
** allocations made by 'test_destructor';
**
** WARNING: Not threadsafe.
*/
static int test_destructor_count_var = 0;
static void destructor(void *p){
  char *val = (char *)p;
  assert(val);
  val--;
  dbsql_free(val);
  test_destructor_count_var--;
}
static void test_destructor(
  dbsql_func_t *ctx, 
  int n_arg,
  dbsql_value_t **argv
){
  char *val;
  int len;
  
  test_destructor_count_var++;
  assert( n_arg==1 );
  if( dbsql_value_type(argv[0])==DBSQL_NULL ) return;
  len = dbsql_value_bytes(argv[0]); 
  val = __test_context_malloc(ctx, len+3);
  if( !val ){
    return;
  }
  val[len+1] = 0;
  val[len+2] = 0;
  val++;
  memcpy(val, dbsql_value_text(argv[0]), len);
  dbsql_result_text(ctx, val, -1, destructor);
}
#ifndef DBSQL_OMIT_UTF16
static void test_destructor16(
  dbsql_func_t *ctx, 
  int n_arg,
  dbsql_value_t **argv
){
  char *val;
  int len;
  
  test_destructor_count_var++;
  assert( n_arg==1 );
  if( dbsql_value_type(argv[0])==DBSQL_NULL ) return;
  len = dbsql_value_bytes16(argv[0]); 
  val = __test_context_malloc(ctx, len+3);
  if( !val ){
    return;
  }
  val[len+1] = 0;
  val[len+2] = 0;
  val++;
  memcpy(val, dbsql_value_text16(argv[0]), len);
  dbsql_result_text16(ctx, val, -1, destructor);
}
#endif
static void test_destructor_count(
  dbsql_func_t *ctx, 
  int n_arg,
  dbsql_value_t **argv
){
  dbsql_result_int(ctx, test_destructor_count_var);
}

/*
** The following aggregate function, test_agg_errmsg16(), takes zero 
** arguments. It returns the text value returned by the dbsql_errmsg16()
** API function.
*/
#ifndef DBSQL_UNTESTABLE
void __begin_benign_malloc(void);
void __end_benign_malloc(void);
#else
  #define __begin_benign_malloc()
  #define __end_benign_malloc()
#endif
static void test_agg_errmsg16_step(dbsql_func_t *a, int b,dbsql_value_t **c){
}
static void test_agg_errmsg16_final(dbsql_func_t *ctx){
#ifndef DBSQL_OMIT_UTF16
  const void *z;
  DBSQL * db = dbsql_context_db_handle(ctx);
  dbsql_aggregate_context(ctx, 2048);
  z = dbsql_errmsg16(db);
  dbsql_result_text16(ctx, z, -1, DBSQL_TRANSIENT);
#endif
}

/*
** Routines for testing the dbsql_get_auxdata() and dbsql_set_auxdata()
** interface.
**
** The test_auxdata() SQL function attempts to register each of its arguments
** as auxiliary data.  If there are no prior registrations of aux data for
** that argument (meaning the argument is not a constant or this is its first
** call) then the result for that argument is 0.  If there is a prior
** registration, the result for that argument is 1.  The overall result
** is the individual argument results separated by spaces.
*/
static void free_test_auxdata(void *p) {dbsql_free(p);}
static void test_auxdata(
  dbsql_func_t *ctx, 
  int n_arg,
  dbsql_value_t **argv
){
  int i;
  char *ret = __test_context_malloc(ctx, n_arg*2);
  if( !ret ) return;
  memset(ret, 0, n_arg*2);
  for(i=0; i<n_arg; i++){
    char const *z = (char*)dbsql_value_text(argv[i]);
    if( z ){
      int n;
      char *aux = dbsql_get_auxdata(ctx, i);
      if( aux ){
        ret[i*2] = '1';
        assert( strcmp(aux,z)==0 );
      }else {
        ret[i*2] = '0';
      }
      n = (int)strlen(z) + 1;
      aux = __test_context_malloc(ctx, n);
      if( aux ){
        memcpy(aux, z, n);
        dbsql_set_auxdata(ctx, i, aux, free_test_auxdata);
      }
      ret[i*2+1] = ' ';
    }
  }
  dbsql_result_text(ctx, ret, 2*n_arg-1, free_test_auxdata);
}

/*
** A function to test error reporting from user functions. This function
** returns a copy of its first argument as the error message.  If the
** second argument exists, it becomes the error code.
*/
static void test_error(
  dbsql_func_t *ctx, 
  int n_arg,
  dbsql_value_t **argv
){
  dbsql_result_error(ctx, (char*)dbsql_value_text(argv[0]), -1);
  if( n_arg==2 ){
    dbsql_result_error_code(ctx, dbsql_value_int(argv[1]));
  }
}

/*
** Implementation of the counter(X) function.  If X is an integer
** constant, then the first invocation will return X.  The second X+1.
** and so forth.  Can be used (for example) to provide a sequence number
** in a result set.
*/
static void __counter_func(
  dbsql_func_t *ctx,   /* Function context */
  int n_arg,                /* Number of function arguments */
  dbsql_value_t **argv     /* Values for all function arguments */
){
  int *counter = (int*)dbsql_get_auxdata(ctx, 0);
  if( counter==0 ){
    counter = dbsql_malloc( sizeof(*counter) );
    if( counter==0 ){
      dbsql_result_error_nomem(ctx);
      return;
    }
    *counter = dbsql_value_int(argv[0]);
    dbsql_set_auxdata(ctx, 0, counter, dbsql_free);
  }else{
    ++*counter;
  }
  dbsql_result_int(ctx, *counter);
}


/*
** This function takes two arguments.  It performance UTF-8/16 type
** conversions on the first argument then returns a copy of the second
** argument.
**
** This function is used in cases such as the following:
**
**      SELECT test_isolation(x,x) FROM t1;
**
** We want to verify that the type conversions that occur on the
** first argument do not invalidate the second argument.
*/
static void test_isolation(
  dbsql_func_t *ctx, 
  int n_arg,
  dbsql_value_t **argv
){
#ifndef DBSQL_OMIT_UTF16
  dbsql_value_text16(argv[0]);
  dbsql_value_text(argv[0]);
  dbsql_value_text16(argv[0]);
  dbsql_value_text(argv[0]);
#endif
  dbsql_result_value(ctx, argv[1]);
}

/*
** Invoke an SQL statement recursively.  The function result is the 
** first column of the first row of the result set.
*/
static void test_eval(
  dbsql_func_t *ctx, 
  int n_arg,
  dbsql_value_t **argv
){
  dbsql_stmt_t *stmt;
  int rc;
  DBSQL *db = dbsql_context_db_handle(ctx);
  const char *sql;

  sql = (char*)dbsql_value_text(argv[0]);
  rc = dbsql_prepare_v2(db, sql, -1, &stmt, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_step(stmt);
    if( rc==DBSQL_ROW ){
      dbsql_result_value(ctx, dbsql_column_value(stmt, 0));
    }
    rc = dbsql_finalize(stmt);
  }
  if( rc ){
    char *err;
    assert( stmt==0 );
    err = dbsql_mprintf("dbsql_prepare_v2() error: %s",dbsql_errmsg(db));
    dbsql_result_text(ctx, err, -1, dbsql_free);
    dbsql_result_error_code(ctx, rc);
  }
}


/*
** convert one character from hex to binary
*/
static int __test_hex_char(char c){
  if( c>='0' && c<='9' ){
    return c - '0';
  }else if( c>='a' && c<='f' ){
    return c - 'a' + 10;
  }else if( c>='A' && c<='F' ){
    return c - 'A' + 10;
  }
  return 0;
}

/*
** Convert hex to binary.
*/
static void __test_hex_to_bin_local(const char *in, char *out){
  while( in[0] && in[1] ){
    *(out++) = (__test_hex_char(in[0])<<4) + __test_hex_char(in[1]);
    in += 2;
  }
}

/*
**      hex_to_utf16be(HEX)
**
** Convert the input string from HEX into binary.  Then return the
** result using dbsql_result_text16le().
*/
#ifndef DBSQL_OMIT_UTF16
static void __test_hex_to_utf16be(
  dbsql_func_t *ctx, 
  int n_arg,
  dbsql_value_t **argv
){
  int n;
  const char *in;
  char *out;
  assert( n_arg==1 );
  n = dbsql_value_bytes(argv[0]);
  in = (const char*)dbsql_value_text(argv[0]);
  out = dbsql_malloc( n/2 );
  if( out==0 ){
    dbsql_result_error_nomem(ctx);
  }else{
    __test_hex_to_bin_local(in, out);
    dbsql_result_text16be(ctx, out, n/2, dbsql_free);
  }
}
#endif

/*
**      hex_to_utf8(HEX)
**
** Convert the input string from HEX into binary.  Then return the
** result using dbsql_result_text16le().
*/
static void __test_hex_to_utf8(
  dbsql_func_t *ctx, 
  int n_arg,
  dbsql_value_t **argv
){
  int n;
  const char *in;
  char *out;
  assert( n_arg==1 );
  n = dbsql_value_bytes(argv[0]);
  in = (const char*)dbsql_value_text(argv[0]);
  out = dbsql_malloc( n/2 );
  if( out==0 ){
    dbsql_result_error_nomem(ctx);
  }else{
    __test_hex_to_bin_local(in, out);
    dbsql_result_text(ctx, out, n/2, dbsql_free);
  }
}

/*
**      hex_to_utf16le(HEX)
**
** Convert the input string from HEX into binary.  Then return the
** result using dbsql_result_text16le().
*/
#ifndef DBSQL_OMIT_UTF16
static void __test_hex_to_utf16le(
  dbsql_func_t *ctx, 
  int n_arg,
  dbsql_value_t **argv
){
  int n;
  const char *in;
  char *out;
  assert( n_arg==1 );
  n = dbsql_value_bytes(argv[0]);
  in = (const char*)dbsql_value_text(argv[0]);
  out = dbsql_malloc( n/2 );
  if( out==0 ){
    dbsql_result_error_nomem(ctx);
  }else{
    __test_hex_to_bin_local(in, out);
    dbsql_result_text16le(ctx, out, n/2, dbsql_free);
  }
}
#endif

/*
** SQL function:   real2hex(X)
**
** If argument X is a real number, then convert it into a string which is
** the big-endian hexadecimal representation of the ieee754 encoding of
** that number.  If X is not a real number, return NULL.
*/
static void real2hex(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  union {
    u_int64_t i;
    double r;
    unsigned char x[8];
  } v;
  char out[20];
  int i;
  int big_endian;
  v.i = 1;
  big_endian = v.x[0]==0;
  v.r = dbsql_value_double(argv[0]);
  for(i=0; i<8; i++){
    if( big_endian ){
      out[i*2]   = "0123456789abcdef"[v.x[i]>>4];
      out[i*2+1] = "0123456789abcdef"[v.x[i]&0xf];
    }else{
      out[14-i*2]   = "0123456789abcdef"[v.x[i]>>4];
      out[14-i*2+1] = "0123456789abcdef"[v.x[i]&0xf];
    }
  }
  out[16] = 0;
  dbsql_result_text(context, out, -1, DBSQL_TRANSIENT);
}

/*
**     test_extract(record, field)
**
** This function implements an SQL user-function that accepts a blob
** containing a formatted database record as the first argument. The
** second argument is the index of the field within that record to
** extract and return.
*/
static void test_extract(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  DBSQL *db = dbsql_context_db_handle(context);
  u_int8_t *rec;
  u_int8_t *end_hdr;                    /* Points to one byte past record header */
  u_int8_t *hdr;                       /* Current point in record header */
  u_int8_t *body;                      /* Current point in record data */
  u_int64_t n_hdr;                       /* Bytes in record header */
  int i_idx;                       /* Required field */
  int i_current = 0;               /* Current field */

  assert( argc==2 );
  rec = (u_int8_t*)dbsql_value_blob(argv[0]);
  i_idx = dbsql_value_int(argv[1]);

  hdr = rec + __get_varint(rec, &n_hdr);
  body = end_hdr = &rec[n_hdr];

  for(i_current=0; hdr<end_hdr && i_current<=i_idx; i_current++){
    u_int64_t i_serial_type;
    mem_t mem;

    memset(&mem, 0, sizeof(mem));
    mem.db = db;
    mem.enc = ENC(db);
    hdr += __get_varint(hdr, &i_serial_type);
    __vdbe_serial_get(body, (u_int32_t)i_serial_type, &mem);
    body += __vdbe_serial_type_len((u_int32_t)i_serial_type);

    if( i_current==i_idx ){
      dbsql_result_value(context, &mem);
    }

    if( mem.sz_malloc ) __dbsql_db_free(db, mem.z_malloc);
  }
}

/*
**      test_decode(record)
**
** This function implements an SQL user-function that accepts a blob
** containing a formatted database record as its only argument. It returns
** a tcl list (type DBSQL_TEXT) containing each of the values stored
** in the record.
*/
static void test_decode(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  DBSQL *db = dbsql_context_db_handle(context);
  u_int8_t *rec;
  u_int8_t *end_hdr;                    /* Points to one byte past record header */
  u_int8_t *hdr;                       /* Current point in record header */
  u_int8_t *body;                      /* Current point in record data */
  u_int64_t n_hdr;                       /* Bytes in record header */
  Tcl_Obj *ret;                  /* Return value */

  ret = Tcl_NewObj();
  Tcl_IncrRefCount(ret);

  assert( argc==1 );
  rec = (u_int8_t*)dbsql_value_blob(argv[0]);

  hdr = rec + __get_varint(rec, &n_hdr);
  body = end_hdr = &rec[n_hdr];
  while( hdr<end_hdr ){
    Tcl_Obj *val = 0;
    u_int64_t i_serial_type;
    mem_t mem;

    memset(&mem, 0, sizeof(mem));
    mem.db = db;
    mem.enc = ENC(db);
    hdr += __get_varint(hdr, &i_serial_type);
    __vdbe_serial_get(body, (u_int32_t)i_serial_type, &mem);
    body += __vdbe_serial_type_len((u_int32_t)i_serial_type);

    switch( dbsql_value_type(&mem) ){
      case DBSQL_TEXT:
        val = Tcl_NewStringObj((const char*)dbsql_value_text(&mem), -1);
        break;

      case DBSQL_BLOB: {
        char hexdigit[] = {
          '0', '1', '2', '3', '4', '5', '6', '7',
          '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
        };
        int n = dbsql_value_bytes(&mem);
        u_int8_t *z = (u_int8_t*)dbsql_value_blob(&mem);
        int i;
        val = Tcl_NewStringObj("x'", -1);
        for(i=0; i<n; i++){
          char hex[3];
          hex[0] = hexdigit[((z[i] >> 4) & 0x0F)];
          hex[1] = hexdigit[(z[i] & 0x0F)];
          hex[2] = '\0';
          Tcl_AppendStringsToObj(val, hex, 0);
        }
        Tcl_AppendStringsToObj(val, "'", 0);
        break;
      }

      case DBSQL_FLOAT:
        val = Tcl_NewDoubleObj(dbsql_value_double(&mem));
        break;

      case DBSQL_INTEGER:
        val = Tcl_NewWideIntObj(dbsql_value_int64(&mem));
        break;

      case DBSQL_NULL:
        val = Tcl_NewStringObj("NULL", -1);
        break;

      default:
        assert( 0 );
    }

    Tcl_ListObjAppendElement(0, ret, val);

    if( mem.sz_malloc ){
      __dbsql_db_free(db, mem.z_malloc);
    }
  }

  dbsql_result_text(context, Tcl_GetString(ret), -1, DBSQL_TRANSIENT);
  Tcl_DecrRefCount(ret);
}

/*
**       test_zeroblob(N)
**
** The implementation of scalar SQL function "test_zeroblob()". This is
** similar to the built-in zeroblob() function, except that it does not
** check that the integer parameter is within range before passing it
** to dbsql_result_zeroblob().
*/
static void test_zeroblob(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  int n_zero = dbsql_value_int(argv[0]);
  dbsql_result_zeroblob(context, n_zero);
}

/*         test_getsubtype(V)
**
** Return the subtype for value V.
*/
static void test_getsubtype(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  dbsql_result_int(context, (int)dbsql_value_subtype(argv[0]));
}

/*         test_frombind(A,B,C,...)
**
** Return an integer bitmask that has a bit set for every argument
** (up to the first 63 arguments) that originates from a bind a parameter.
*/
static void test_frombind(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  u_int64_t m = 0;
  int i;
  for(i=0; i<argc && i<63; i++){
    if( dbsql_value_frombind(argv[i]) ) m |= ((u_int64_t)1)<<i;
  }
  dbsql_result_int64(context, (int64_t)m);
}

/*         test_setsubtype(V, T)
**
** Return the value V with its subtype changed to T
*/
static void test_setsubtype(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  dbsql_result_value(context, argv[0]);
  dbsql_result_subtype(context, (unsigned int)dbsql_value_int(argv[1]));
}

static int __register_test_functions(
  DBSQL *db,
  char **errmsg_p,
  const dbsql_api_routines_t *thunk
){
  static const struct {
     char *name;
     signed char n_arg;
     unsigned int text_rep; /* 1: UTF-16.  0: UTF-8 */
     void (*func)(dbsql_func_t*,int,dbsql_value_t **);
  } funcs[] = {
    { "randstr",               2, DBSQL_UTF8, __rand_str    },
    { "test_destructor",       1, DBSQL_UTF8, test_destructor},
#ifndef DBSQL_OMIT_UTF16
    { "test_destructor16",     1, DBSQL_UTF8, test_destructor16},
    { "hex_to_utf16be",        1, DBSQL_UTF8, __test_hex_to_utf16be},
    { "hex_to_utf16le",        1, DBSQL_UTF8, __test_hex_to_utf16le},
#endif
    { "hex_to_utf8",           1, DBSQL_UTF8, __test_hex_to_utf8},
    { "test_destructor_count", 0, DBSQL_UTF8, test_destructor_count},
    { "test_auxdata",         -1, DBSQL_UTF8, test_auxdata},
    { "test_error",            1, DBSQL_UTF8, test_error},
    { "test_error",            2, DBSQL_UTF8, test_error},
    { "test_eval",             1, DBSQL_UTF8, test_eval},
    { "test_isolation",        2, DBSQL_UTF8, test_isolation},
    { "test_counter",          1, DBSQL_UTF8, __counter_func},
    { "real2hex",              1, DBSQL_UTF8, real2hex},
    { "test_decode",           1, DBSQL_UTF8, test_decode},
    { "test_extract",          2, DBSQL_UTF8, test_extract},
    { "test_zeroblob",  1, DBSQL_UTF8|DBSQL_DETERMINISTIC, test_zeroblob},
    { "test_getsubtype",       1, DBSQL_UTF8, test_getsubtype},
    { "test_setsubtype",       2, DBSQL_UTF8|DBSQL_RESULT_SUBTYPE,
                                               test_setsubtype},
    { "test_frombind",        -1, DBSQL_UTF8, test_frombind},
  };
  int i;

  for(i=0; i<sizeof(funcs)/sizeof(funcs[0]); i++){
    dbsql_create_function(db, funcs[i].name, funcs[i].n_arg,
        funcs[i].text_rep, 0, funcs[i].func, 0, 0);
  }

  dbsql_create_function(db, "test_agg_errmsg16", 0, DBSQL_ANY, 0, 0, 
      test_agg_errmsg16_step, test_agg_errmsg16_final);
      
  return DBSQL_OK;
}

/*
** TCLCMD:  autoinstall_test_functions
**
** Invoke this TCL command to use dbsql_auto_extension() to cause
** the standard set of test functions to be loaded into each new
** database connection.
*/
static int DBSQL_TCLAPI autoinstall_test_funcs(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int Md5_Register(DBSQL *, char **, const dbsql_api_routines_t *);
  int rc = dbsql_auto_extension((void(*)(void))__register_test_functions);
  if( rc==DBSQL_OK ){
    rc = dbsql_auto_extension((void(*)(void))Md5_Register);
  }
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return TCL_OK;
}

/*
** A bogus step function and finalizer function.
*/
static void __t_step(dbsql_func_t *a, int b, dbsql_value_t **c){}
static void __t_final(dbsql_func_t *a){}


/*
** tclcmd:  abuse_create_function
**
** Make various calls to dbsql_create_function that do not have valid
** parameters.  Verify that the error condition is detected and reported.
*/
static int DBSQL_TCLAPI abuse_create_function(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int __get_db_pointer(Tcl_Interp*, const char*, DBSQL**);
  DBSQL *db;
  int rc;
  int mx_arg;

  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;

  rc = dbsql_create_function(db, "tx", 1, DBSQL_UTF8, 0, __t_step,__t_step,__t_final);
  if( rc!=DBSQL_MISUSE ) goto abuse_err;

  rc = dbsql_create_function(db, "tx", 1, DBSQL_UTF8, 0, __t_step, __t_step, 0);
  if( rc!=DBSQL_MISUSE ) goto abuse_err;

  rc = dbsql_create_function(db, "tx", 1, DBSQL_UTF8, 0, __t_step, 0, __t_final);
  if( rc!=DBSQL_MISUSE) goto abuse_err;

  rc = dbsql_create_function(db, "tx", 1, DBSQL_UTF8, 0, 0, 0, __t_final);
  if( rc!=DBSQL_MISUSE ) goto abuse_err;

  rc = dbsql_create_function(db, "tx", 1, DBSQL_UTF8, 0, 0, __t_step, 0);
  if( rc!=DBSQL_MISUSE ) goto abuse_err;

  rc = dbsql_create_function(db, "tx", -2, DBSQL_UTF8, 0, __t_step, 0, 0);
  if( rc!=DBSQL_MISUSE ) goto abuse_err;

  rc = dbsql_create_function(db, "tx", 32768, DBSQL_UTF8, 0, __t_step, 0, 0);
  if( rc!=DBSQL_MISUSE ) goto abuse_err;

  rc = dbsql_create_function(db, "funcxx"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789",
       1, DBSQL_UTF8, 0, __t_step, 0, 0);
  if( rc!=DBSQL_MISUSE ) goto abuse_err;

  /* This last function registration should actually work.  Generate
  ** a no-op function (that always returns NULL) and which has the
  ** maximum-length function name and the maximum number of parameters.
  */
  dbsql_limit(db, DBSQL_LIMIT_FUNCTION_ARG, 1000000);
  mx_arg = dbsql_limit(db, DBSQL_LIMIT_FUNCTION_ARG, -1);
  rc = dbsql_create_function(db, "nullx"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789"
       "_123456789_123456789_123456789_123456789_123456789",
       mx_arg, DBSQL_UTF8, 0, __t_step, 0, 0);
  if( rc!=DBSQL_OK ) goto abuse_err;
                                
  return TCL_OK;

abuse_err:
  Tcl_AppendResult(interp, "dbsql_create_function abused test failed", 
                   (char*)0);
  return TCL_ERROR;
}


/*
** dbsql user defined function to use with matchinfo() to calculate the
** relevancy of an FTS match. The value returned is the relevancy score
** (a real value greater than or equal to zero). A larger value indicates 
** a more relevant document.
**
** The overall relevancy returned is the sum of the relevancies of each 
** column value in the FTS table. The relevancy of a column value is the
** sum of the following for each reportable phrase in the FTS query:
**
**   (<hit count> / <global hit count>) * <column weight>
**
** where <hit count> is the number of instances of the phrase in the
** column value of the current row and <global hit count> is the number
** of instances of the phrase in the same column of all rows in the FTS
** table. The <column weight> is a weighting factor assigned to each
** column by the caller (see below).
**
** The first argument to this function must be the return value of the FTS 
** matchinfo() function. Following this must be one argument for each column 
** of the FTS table containing a numeric weight factor for the corresponding 
** column. Example:
**
**     CREATE VIRTUAL TABLE documents USING fts3(title, content)
**
** The following query returns the docids of documents that match the full-text
** query <query> sorted from most to least relevant. When calculating
** relevance, query term instances in the 'title' column are given twice the
** weighting of those in the 'content' column.
**
**     SELECT docid FROM documents 
**     WHERE documents MATCH <query> 
**     ORDER BY rank(matchinfo(documents), 1.0, 0.5) DESC
*/
static void rankfunc(dbsql_func_t *ctx, int n_val, dbsql_value_t **vals){
  int *matchinfos;                /* Return value of matchinfo() */
  int n_matchinfo;                 /* Number of elements in matchinfos[] */
  int n_col = 0;                   /* Number of columns in the table */
  int n_phrase = 0;                /* Number of phrases in the query */
  int i_phrase;                    /* Current phrase */
  double score = 0.0;             /* Value to return */

  assert( sizeof(int)==4 );

  /* Check that the number of arguments passed to this function is correct.
  ** If not, jump to wrong_number_args. Set matchinfos to point to the array
  ** of unsigned integer values returned by FTS function matchinfo. Set
  ** n_phrase to contain the number of reportable phrases in the users full-text
  ** query, and n_col to the number of columns in the table. Then check that the
  ** size of the matchinfo blob is as expected. Return an error if it is not.
  */
  if( n_val<1 ) goto wrong_number_args;
  matchinfos = (int*)dbsql_value_blob(vals[0]);
  n_matchinfo = dbsql_value_bytes(vals[0]) / sizeof(int);
  if( n_matchinfo>=2 ){
    n_phrase = matchinfos[0];
    n_col = matchinfos[1];
  }
  if( n_matchinfo!=(2+3*n_col*n_phrase) ){
    dbsql_result_error(ctx,
        "invalid matchinfo blob passed to function rank()", -1);
    return;
  }
  if( n_val!=(1+n_col) ) goto wrong_number_args;

  /* Iterate through each phrase in the users query. */
  for(i_phrase=0; i_phrase<n_phrase; i_phrase++){
    int i_col;                     /* Current column */

    /* Now iterate through each column in the users query. For each column,
    ** increment the relevancy score by:
    **
    **   (<hit count> / <global hit count>) * <column weight>
    **
    ** phraseinfos[] points to the start of the data for phrase i_phrase. So
    ** the hit count and global hit counts for each column are found in 
    ** phraseinfos[i_col*3] and phraseinfos[i_col*3+1], respectively.
    */
    int *phraseinfos = &matchinfos[2 + i_phrase*n_col*3];
    for(i_col=0; i_col<n_col; i_col++){
      int n_hit_count = phraseinfos[3*i_col];
      int n_global_hit_count = phraseinfos[3*i_col+1];
      double weight = dbsql_value_double(vals[i_col+1]);
      if( n_hit_count>0 ){
        score += ((double)n_hit_count / (double)n_global_hit_count) * weight;
      }
    }
  }

  dbsql_result_double(ctx, score);
  return;

  /* Jump here if the wrong number of arguments are passed to this function */
wrong_number_args:
  dbsql_result_error(ctx, "wrong number of arguments to function rank()", -1);
}

static int DBSQL_TCLAPI install_fts3_rank_function(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  extern int __get_db_pointer(Tcl_Interp*, const char*, DBSQL**);
  DBSQL *db;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }

  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  dbsql_create_function(db, "rank", -1, DBSQL_UTF8, 0, rankfunc, 0, 0);
  return TCL_OK;
}


/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest_func_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
  } obj_cmds[] = {
     { "autoinstall_test_functions",    autoinstall_test_funcs },
     { "abuse_create_function",         abuse_create_function  },
     { "install_fts3_rank_function",    install_fts3_rank_function  },
  };
  int i;
  extern int Md5_Register(DBSQL *, char **, const dbsql_api_routines_t *);

  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, obj_cmds[i].proc, 0, 0);
  }
  dbsql_initialize();
  dbsql_auto_extension((void(*)(void))__register_test_functions);
  dbsql_auto_extension((void(*)(void))Md5_Register);
  return TCL_OK;
}
