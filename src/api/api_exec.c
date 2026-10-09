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
** Main file for the dbsql library.  The routines in this file
** implement the programmer interface to the library.  Routines in
** other files are for internal use by dbsql and should not be
** accessed by users of the library.
*/

#include "dbsql_int.h"

/*
** Execute SQL code.  Return one of the DBSQL_ success/failure
** codes.  Also write an error message into memory obtained from
** malloc() and make *errmsg_p point to that message.
**
** If the SQL is a query, then for each row in the query result
** the callback() function is called.  arg becomes the first
** argument to callback().  If callback=NULL then no callback
** is invoked, even for queries.
*/
int dbsql_exec(
  DBSQL *db,                /* The database on which the SQL executes */
  const char *sql,           /* The SQL to be executed */
  dbsql_callback callback, /* Invoke this callback routine */
  void *arg,                 /* First argument to callback() */
  char **errmsg_p             /* Write error messages here */
){
  int rc = DBSQL_OK;         /* Return code */
  const char *leftover;      /* Tail of unprocessed SQL */
  dbsql_stmt_t *stmt = 0;    /* The current SQL statement */
  char **cols = 0;          /* Names of result columns */
  int callback_is_init;         /* True if callback data is initialized */

  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
  if( sql==0 ) sql = "";

  dbsql_mutex_enter(db->mutex);
  __dbsql_error(db, DBSQL_OK);
  while( rc==DBSQL_OK && sql[0] ){
    int n_col = 0;
    char **vals = 0;

    stmt = 0;
    rc = dbsql_prepare_v2(db, sql, -1, &stmt, &leftover);
    assert( rc==DBSQL_OK || stmt==0 );
    if( rc!=DBSQL_OK ){
      continue;
    }
    if( !stmt ){
      /* this happens for a comment or white-space */
      sql = leftover;
      continue;
    }
    callback_is_init = 0;

    while( 1 ){
      int i;
      rc = dbsql_step(stmt);

      /* Invoke the callback function if required */
      if( callback && (DBSQL_ROW==rc || 
          (DBSQL_DONE==rc && !callback_is_init
                           && db->flags&DBSQL_NullCallback)) ){
        if( !callback_is_init ){
          n_col = dbsql_column_count(stmt);
          cols = __db_malloc_raw(db, (2*n_col+1)*sizeof(const char*));
          if( cols==0 ){
            goto exec_out;
          }
          for(i=0; i<n_col; i++){
            cols[i] = (char *)dbsql_column_name(stmt, i);
            /* __vdbe_set_col_name() installs column names as UTF8
            ** strings so there is no way for dbsql_column_name() to fail. */
            assert( cols[i]!=0 );
          }
          callback_is_init = 1;
        }
        if( rc==DBSQL_ROW ){
          vals = &cols[n_col];
          for(i=0; i<n_col; i++){
            vals[i] = (char *)dbsql_column_text(stmt, i);
            if( !vals[i] && dbsql_column_type(stmt, i)!=DBSQL_NULL ){
              __oom_fault(db);
              goto exec_out;
            }
          }
          vals[i] = 0;
        }
        if( callback(arg, n_col, vals, cols) ){
          /* EVIDENCE-OF: R-38229-40159 If the callback function to
          ** dbsql_exec() returns non-zero, then dbsql_exec() will
          ** return DBSQL_ABORT. */
          rc = DBSQL_ABORT;
          __vdbe_finalize((vdbe_t *)stmt);
          stmt = 0;
          __dbsql_error(db, DBSQL_ABORT);
          goto exec_out;
        }
      }

      if( rc!=DBSQL_ROW ){
        rc = __vdbe_finalize((vdbe_t *)stmt);
        stmt = 0;
        sql = leftover;
        while( __isspace(sql[0]) ) sql++;
        break;
      }
    }

    __dbsql_db_free(db, cols);
    cols = 0;
  }

exec_out:
  if( stmt ) __vdbe_finalize((vdbe_t *)stmt);
  __dbsql_db_free(db, cols);

  rc = __api_exit(db, rc);
  if( rc!=DBSQL_OK && errmsg_p ){
    *errmsg_p = __db_str_dup(0, dbsql_errmsg(db));
    if( *errmsg_p==0 ){
      rc = DBSQL_NOMEM_BKPT;
      __dbsql_error(db, DBSQL_NOMEM);
    }
  }else if( errmsg_p ){
    *errmsg_p = 0;
  }

  assert( (rc&db->err_mask)==rc );
  dbsql_mutex_leave(db->mutex);
  return rc;
}
