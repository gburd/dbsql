/*
** 2014-11-10
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
** This dbsql extension implements SQL function eval() which runs
** SQL statements recursively.
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <string.h>

/*
** Structure used to accumulate the output
*/
struct eval_result_t {
  char *z;               /* Accumulated output */
  const char *sep;      /* Separator */
  int sz_sep;             /* Size of the separator string */
  int64_t n_alloc;  /* Number of bytes allocated for z[] */
  int64_t n_used;   /* Number of bytes of z[] actually used */
};

/*
** Callback from dbsql_exec() for the eval() function.
*/
static int callback(void *ctx, int argc, char **argv, char **colnames){
  struct eval_result_t *p = (struct eval_result_t*)ctx;
  int i; 
  if( argv==0 ) return 0;
  for(i=0; i<argc; i++){
    const char *z = argv[i] ? argv[i] : "";
    size_t sz = strlen(z);
    if( (int64_t)sz+p->n_used+p->sz_sep+1 > p->n_alloc ){
      char *z_new;
      p->n_alloc = p->n_alloc*2 + sz + p->sz_sep + 1;
      /* Using dbsql_realloc64() would be better, but it is a recent
      ** addition and will cause a segfault if loaded by an older version
      ** of dbsql.  */
      z_new = p->n_alloc<=0x7fffffff ? dbsql_realloc64(p->z, p->n_alloc) : 0;
      if( z_new==0 ){
        dbsql_free(p->z);
        memset(p, 0, sizeof(*p));
        return 1;
      }
      p->z = z_new;
    }
    if( p->n_used>0 ){
      memcpy(&p->z[p->n_used], p->sep, p->sz_sep);
      p->n_used += p->sz_sep;
    }
    memcpy(&p->z[p->n_used], z, sz);
    p->n_used += sz;
  }
  return 0;
}

/*
** Implementation of the eval(X) and eval(X,Y) SQL functions.
**
** Evaluate the SQL text in X.  Return the results, using string
** Y as the separator.  If Y is omitted, use a single space character.
*/
static void __sql_eval_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  const char *sql;
  DBSQL *db;
  char *err = 0;
  int rc;
  struct eval_result_t x;

  memset(&x, 0, sizeof(x));
  x.sep = " ";
  sql = (const char*)dbsql_value_text(argv[0]);
  if( sql==0 ) return;
  if( argc>1 ){
    x.sep = (const char*)dbsql_value_text(argv[1]);
    if( x.sep==0 ) return;
  }
  x.sz_sep = (int)strlen(x.sep);
  db = dbsql_context_db_handle(context);
  rc = dbsql_exec(db, sql, callback, &x, &err);
  if( rc!=DBSQL_OK ){
    dbsql_result_error(context, err, -1);
    dbsql_free(err);
  }else if( x.sep==0 ){
    dbsql_result_error_nomem(context);
    dbsql_free(x.z);
  }else{
    dbsql_result_text(context, x.z, (int)x.n_used, dbsql_free);
  }
}


#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_eval_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Unused parameter */
  rc = dbsql_create_function(db, "eval", 1, 
                               DBSQL_UTF8|DBSQL_DIRECTONLY, 0,
                               __sql_eval_func, 0, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "eval", 2,
                                 DBSQL_UTF8|DBSQL_DIRECTONLY, 0,
                                 __sql_eval_func, 0, 0);
  }
  return rc;
}
