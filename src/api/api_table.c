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
** This file contains the dbsql_get_table() and dbsql_free_table()
** interface routines.  These are just wrappers around the main
** interface routine of dbsql_exec().
**
** These routines are in a separate files so that they will not be linked
** if they are not used.
*/
#include "dbsql_int.h"

#ifndef DBSQL_OMIT_GET_TABLE

/*
** This structure is used to pass data from dbsql_get_table() through
** to the callback function is uses to build the result.
*/
typedef struct tab_result_t {
  char **results;   /* Accumulated output */
  char *errmsg;     /* Error message text, if an error occurs */
  u_int32_t n_alloc;        /* Slots allocated for results[] */
  u_int32_t n_row;          /* Number of rows in the result */
  u_int32_t n_column;       /* Number of columns in the result */
  u_int32_t n_data;         /* Slots used in results[].  (n_row+1)*n_column */
  int rc;            /* Return code from dbsql_exec() */
} tab_result_t;

/*
** This routine is called once for each row in the result table.  Its job
** is to fill in the tab_result_t structure appropriately, allocating new
** memory as necessary.
*/
static int dbsql_get_table_cb(void *arg, int n_col, char **argv, char **colv){
  tab_result_t *p = (tab_result_t*)arg;  /* Result accumulator */
  int need;                         /* Slots needed in p->results[] */
  int i;                            /* Loop counter */
  char *z;                          /* A single column of result */

  /* Make sure there is enough space in p->results to hold everything
  ** we need to remember from this invocation of the callback.
  */
  if( p->n_row==0 && argv!=0 ){
    need = n_col*2;
  }else{
    need = n_col;
  }
  if( p->n_data + need > p->n_alloc ){
    char **news;
    p->n_alloc = p->n_alloc*2 + need;
    news = __dbsql_realloc( p->results, sizeof(char*)*p->n_alloc );
    if( news==0 ) goto malloc_failed;
    p->results = news;
  }

  /* If this is the first row, then generate an extra row containing
  ** the names of all columns.
  */
  if( p->n_row==0 ){
    p->n_column = n_col;
    for(i=0; i<n_col; i++){
      z = dbsql_mprintf("%s", colv[i]);
      if( z==0 ) goto malloc_failed;
      p->results[p->n_data++] = z;
    }
  }else if( (int)p->n_column!=n_col ){
    dbsql_free(p->errmsg);
    p->errmsg = dbsql_mprintf(
       "dbsql_get_table() called with two or more incompatible queries"
    );
    p->rc = DBSQL_ERROR;
    return 1;
  }

  /* Copy over the row data
  */
  if( argv!=0 ){
    for(i=0; i<n_col; i++){
      if( argv[i]==0 ){
        z = 0;
      }else{
        int n = __strlen30(argv[i])+1;
        z = dbsql_malloc64( n );
        if( z==0 ) goto malloc_failed;
        memcpy(z, argv[i], n);
      }
      p->results[p->n_data++] = z;
    }
    p->n_row++;
  }
  return 0;

malloc_failed:
  p->rc = DBSQL_NOMEM_BKPT;
  return 1;
}

/*
** Query the database.  But instead of invoking a callback for each row,
** malloc() for space to hold the result and return the entire results
** at the conclusion of the call.
**
** The result that is written to ***paz_result is held in memory obtained
** from malloc().  But the caller cannot free this memory directly.  
** Instead, the entire table should be passed to dbsql_free_table() when
** the calling procedure is finished using it.
*/
int dbsql_get_table(
  DBSQL *db,                /* The database on which the SQL executes */
  const char *sql,           /* The SQL to be executed */
  char ***paz_result,          /* Write the result table here */
  int *pn_row,                 /* Write the number of rows in the result here */
  int *pn_column,              /* Write the number of columns of result here */
  char **errmsg_p             /* Write error messages here */
){
  int rc;
  tab_result_t res;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || paz_result==0 ) return DBSQL_MISUSE_BKPT;
#endif
  *paz_result = 0;
  if( pn_column ) *pn_column = 0;
  if( pn_row ) *pn_row = 0;
  if( errmsg_p ) *errmsg_p = 0;
  res.errmsg = 0;
  res.n_row = 0;
  res.n_column = 0;
  res.n_data = 1;
  res.n_alloc = 20;
  res.rc = DBSQL_OK;
  res.results = dbsql_malloc64(sizeof(char*)*res.n_alloc );
  if( res.results==0 ){
     db->errCode = DBSQL_NOMEM;
     return DBSQL_NOMEM_BKPT;
  }
  res.results[0] = 0;
  rc = dbsql_exec(db, sql, dbsql_get_table_cb, &res, errmsg_p);
  assert( sizeof(res.results[0])>= sizeof(res.n_data) );
  res.results[0] = DBSQL_INT_TO_PTR(res.n_data);
  if( (rc&0xff)==DBSQL_ABORT ){
    dbsql_free_table(&res.results[1]);
    if( res.errmsg ){
      if( errmsg_p ){
        dbsql_free(*errmsg_p);
        *errmsg_p = dbsql_mprintf("%s",res.errmsg);
      }
      dbsql_free(res.errmsg);
    }
    db->errCode = res.rc;  /* Assume 32-bit assignment is atomic */
    return res.rc;
  }
  dbsql_free(res.errmsg);
  if( rc!=DBSQL_OK ){
    dbsql_free_table(&res.results[1]);
    return rc;
  }
  if( res.n_alloc>res.n_data ){
    char **news;
    news = __dbsql_realloc( res.results, sizeof(char*)*res.n_data );
    if( news==0 ){
      dbsql_free_table(&res.results[1]);
      db->errCode = DBSQL_NOMEM;
      return DBSQL_NOMEM_BKPT;
    }
    res.results = news;
  }
  *paz_result = &res.results[1];
  if( pn_column ) *pn_column = res.n_column;
  if( pn_row ) *pn_row = res.n_row;
  return rc;
}

/*
** This routine frees the space the dbsql_get_table() malloced.
*/
void dbsql_free_table(
  char **results            /* Result returned from dbsql_get_table() */
){
  if( results ){
    int i, n;
    results--;
    assert( results!=0 );
    n = DBSQL_PTR_TO_INT(results[0]);
    for(i=1; i<n; i++){ if( results[i] ) dbsql_free(results[i]); }
    dbsql_free(results);
  }
}

#endif /* DBSQL_OMIT_GET_TABLE */
