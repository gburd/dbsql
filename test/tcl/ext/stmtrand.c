/*
** 2024-05-24
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
** An SQL function that return pseudo-random non-negative integers.
**
**      SELECT stmtrand(123);
**
** A special feature of this function is that the same sequence of random
** integers is returned for each invocation of the statement.  This makes
** the results repeatable, and hence useful for testing.  The argument is
** an integer which is the seed for the random number sequence.  The seed
** is used by the first invocation of this function only and is ignored
** for all subsequent calls within the same statement.
**
** Resetting a statement (dbsql_reset()) also resets the random number
** sequence.
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

/* State of the pseudo-random number generator */
typedef struct stmtrand_t {
  unsigned int x, y;
} stmtrand_t;

/* auxdata key */
#define STMTRAND_KEY  (-4418371)

/*
** Function:     stmtrand(SEED)
**
** Return a pseudo-random number.
*/
static void __stmtrand_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  stmtrand_t *p;

  p = (stmtrand_t*)dbsql_get_auxdata(context, STMTRAND_KEY);
  if( p==0 ){
    unsigned int seed;
    p = dbsql_malloc64( sizeof(*p) );
    if( p==0 ){
      dbsql_result_error_nomem(context);
      return;
    }
    if( argc>=1 ){
      seed = (unsigned int)dbsql_value_int(argv[0]);
    }else{
      seed = 0;
    }
    p->x = seed | 1;
    p->y = seed;
    dbsql_set_auxdata(context, STMTRAND_KEY, p, dbsql_free);
    p = (stmtrand_t*)dbsql_get_auxdata(context, STMTRAND_KEY);
    if( p==0 ){
      dbsql_result_error_nomem(context);
      return;
    }
  }
  p->x = (p->x>>1) ^ ((1+~(p->x&1)) & 0xd0000001);
  p->y = p->y*1103515245 + 12345;
  dbsql_result_int(context, (int)((p->x ^ p->y)&0x7fffffff));
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_stmtrand_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Unused parameter */
  rc = dbsql_create_function(db, "stmtrand", 1, DBSQL_UTF8, 0,
                               __stmtrand_func, 0, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "stmtrand", 0, DBSQL_UTF8, 0,
                                 __stmtrand_func, 0, 0);
  }
  return rc;
}
