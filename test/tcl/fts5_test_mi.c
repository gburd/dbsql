/*
** 2015 Aug 04
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
** This file contains test code only, it is not included in release 
** versions of FTS5. It contains the implementation of an FTS5 auxiliary
** function very similar to the FTS4 function matchinfo():
**
**     https://sqlite.org/fts3.html#matchinfo
**
** Known differences are that:
**
**  1) this function uses the FTS5 definition of "matchable phrase", which
**     excludes any phrases that are part of an expression sub-tree that
**     does not match the current row. This comes up for MATCH queries 
**     such as:
**
**         "a OR (b AND c)"
**
**     In FTS4, if a single row contains instances of tokens "a" and "c", 
**     but not "b", all instances of "c" are considered matches. In FTS5,
**     they are not (as the "b AND c" sub-tree does not match the current
**     row.
**
**  2) For the values returned by 'x' that apply to all rows of the table, 
**     NEAR constraints are not considered. But for the number of hits in
**     the current row, they are.
**     
** This file exports a single function that may be called to register the
** matchinfo() implementation with a database handle:
**
**   int __fts5_test_register_matchinfo(DBSQL *db);
*/


#ifdef DBSQL_ENABLE_FTS5

#include "fts5.h"
#include <assert.h>
#include <string.h>

typedef struct fts5_matchinfo_ctx_t fts5_matchinfo_ctx_t;

#ifndef DBSQL_AMALGAMATION
typedef unsigned int u_int32_t;
#endif

struct fts5_matchinfo_ctx_t {
  int n_col;                       /* Number of cols in FTS5 table */
  int n_phrase;                    /* Number of phrases in FTS5 query */
  char *arg;                     /* nul-term'd copy of 2nd arg */
  int n_ret;                       /* Number of elements in rets[] */
  u_int32_t *rets;                      /* Array of 32-bit unsigned ints to return */
};



/*
** Return a pointer to the fts5_api pointer for database connection db.
** If an error occurs, return NULL and leave an error in the database 
** handle (accessible using dbsql_errcode()/errmsg()).
*/
static int fts5_api_from_db(DBSQL *db, fts5_api **api){
  dbsql_stmt_t *stmt = 0;
  int rc;

  *api = 0;
  rc = dbsql_prepare(db, "SELECT fts5(?1)", -1, &stmt, 0);
  if( rc==DBSQL_OK ){
    dbsql_bind_pointer(stmt, 1, (void*)api, "fts5_api_ptr", 0);
    (void)dbsql_step(stmt);
    rc = dbsql_finalize(stmt);
  }

  return rc;
}


/*
** Argument f should be a flag accepted by matchinfo() (a valid character
** in the string passed as the second argument). If it is not, -1 is 
** returned. Otherwise, if f is a valid matchinfo flag, the value returned
** is the number of 32-bit integers added to the output array if the
** table has n_col columns and the query n_phrase phrases.
*/
static int __fts5_matchinfo_flagsize(int n_col, int n_phrase, char f){
  int ret = -1;
  switch( f ){
    case 'p': ret = 1; break;
    case 'c': ret = 1; break;
    case 'x': ret = 3 * n_col * n_phrase; break;
    case 'y': ret = n_col * n_phrase; break;
    case 'b': ret = ((n_col + 31) / 32) * n_phrase; break;
    case 'n': ret = 1; break;
    case 'a': ret = n_col; break;
    case 'l': ret = n_col; break;
    case 's': ret = n_col; break;
  }
  return ret;
}

static int __fts5_matchinfo_iter(
  const fts5_extension_api_t *api,   /* API offered by current FTS version */
  fts5_context_t *fts,              /* First arg to pass to api functions */
  fts5_matchinfo_ctx_t *p,
  int(*x)(const fts5_extension_api_t*,fts5_context_t*,fts5_matchinfo_ctx_t*,char,u_int32_t*)
){
  int i;
  int n = 0;
  int rc = DBSQL_OK;
  char f;
  for(i=0; (f = p->arg[i]); i++){
    rc = x(api, fts, p, f, &p->rets[n]);
    if( rc!=DBSQL_OK ) break;
    n += __fts5_matchinfo_flagsize(p->n_col, p->n_phrase, f);
  }
  return rc;
}

static int __fts5_matchinfo_x_cb(
  const fts5_extension_api_t *api,
  fts5_context_t *fts,
  void *user_data
){
  fts5_phrase_iter_t iter;
  int i_col, i_off;
  u_int32_t *outs = (u_int32_t*)user_data;
  int i_prev = -1;

  for(api->phrase_first(fts, 0, &iter, &i_col, &i_off); 
      i_col>=0; 
      api->phrase_next(fts, &iter, &i_col, &i_off)
  ){
    outs[i_col*3+1]++;
    if( i_col!=i_prev ) outs[i_col*3 + 2]++;
    i_prev = i_col;
  }

  return DBSQL_OK;
}

static int __fts5_matchinfo_global_cb(
  const fts5_extension_api_t *api,
  fts5_context_t *fts,
  fts5_matchinfo_ctx_t *p,
  char f,
  u_int32_t *outs
){
  int rc = DBSQL_OK;
  switch( f ){
    case 'p':
      outs[0] = p->n_phrase; 
      break;

    case 'c':
      outs[0] = p->n_col; 
      break;

    case 'x': {
      int i;
      for(i=0; i<p->n_phrase && rc==DBSQL_OK; i++){
        void *ptr = (void*)&outs[i * p->n_col * 3];
        rc = api->query_phrase(fts, i, ptr, __fts5_matchinfo_x_cb);
      }
      break;
    }

    case 'n': {
      int64_t n_row;
      rc = api->row_count(fts, &n_row);
      outs[0] = (u_int32_t)n_row;
      break;
    }

    case 'a': {
      int64_t n_row = 0;
      rc = api->row_count(fts, &n_row);
      if( n_row==0 ){
        memset(outs, 0, sizeof(u_int32_t) * p->n_col);
      }else{
        int i;
        for(i=0; rc==DBSQL_OK && i<p->n_col; i++){
          int64_t n_token;
          rc = api->column_total_size(fts, i, &n_token);
          if( rc==DBSQL_OK){
            outs[i] = (u_int32_t)((2*n_token + n_row) / (2*n_row));
          }
        }
      }
      break;
    }

  }
  return rc;
}

static int __fts5_matchinfo_local_cb(
  const fts5_extension_api_t *api,
  fts5_context_t *fts,
  fts5_matchinfo_ctx_t *p,
  char f,
  u_int32_t *outs
){
  int i;
  int rc = DBSQL_OK;

  switch( f ){
    case 'b': {
      int i_phrase;
      int n_int = ((p->n_col + 31) / 32) * p->n_phrase;
      for(i=0; i<n_int; i++) outs[i] = 0;

      for(i_phrase=0; i_phrase<p->n_phrase; i_phrase++){
        fts5_phrase_iter_t iter;
        int i_col;
        for(api->phrase_first_column(fts, i_phrase, &iter, &i_col);
            i_col>=0; 
            api->phrase_next_column(fts, &iter, &i_col)
        ){
          outs[i_phrase * ((p->n_col+31)/32) + i_col/32] |= ((u_int32_t)1 << i_col%32);
        }
      }

      break;
    }

    case 'x':
    case 'y': {
      int n_mul = (f=='x' ? 3 : 1);
      int i_phrase;

      for(i=0; i<(p->n_col*p->n_phrase); i++) outs[i*n_mul] = 0;

      for(i_phrase=0; i_phrase<p->n_phrase; i_phrase++){
        fts5_phrase_iter_t iter;
        int i_off, i_col;
        for(api->phrase_first(fts, i_phrase, &iter, &i_col, &i_off); 
            i_off>=0; 
            api->phrase_next(fts, &iter, &i_col, &i_off)
        ){
          outs[n_mul * (i_col + i_phrase * p->n_col)]++;
        }
      }

      break;
    }

    case 'l': {
      for(i=0; rc==DBSQL_OK && i<p->n_col; i++){
        int n_token;
        rc = api->column_size(fts, i, &n_token);
        outs[i] = (u_int32_t)n_token;
      }
      break;
    }

    case 's': {
      int n_inst;

      memset(outs, 0, sizeof(u_int32_t) * p->n_col);

      rc = api->inst_count(fts, &n_inst);
      for(i=0; rc==DBSQL_OK && i<n_inst; i++){
        int i_phrase, i_off, i_col = 0;
        int i_next_phrase;
        int i_next_off;
        u_int32_t n_seq = 1;
        int j;

        rc = api->inst(fts, i, &i_phrase, &i_col, &i_off);
        i_next_phrase = i_phrase+1;
        i_next_off = i_off+api->phrase_size(fts, 0);
        for(j=i+1; rc==DBSQL_OK && j<n_inst; j++){
          int ip, ic, io;
          rc = api->inst(fts, j, &ip, &ic, &io);
          if( ic!=i_col || io>i_next_off ) break;
          if( ip==i_next_phrase && io==i_next_off ){
            n_seq++;
            i_next_phrase = ip+1;
            i_next_off = io + api->phrase_size(fts, ip);
          }
        }

        if( n_seq>outs[i_col] ) outs[i_col] = n_seq;
      }

      break;
    }
  }
  return rc;
}
 
static fts5_matchinfo_ctx_t *__fts5_matchinfo_new(
  const fts5_extension_api_t *api,   /* API offered by current FTS version */
  fts5_context_t *fts,              /* First arg to pass to api functions */
  dbsql_func_t *ctx,          /* Context for returning error message */
  const char *arg                /* Matchinfo flag string */
){
  fts5_matchinfo_ctx_t *p;
  int n_col;
  int n_phrase;
  int i;
  int n_int;
  int64_t n_byte;
  int rc;

  n_col = api->column_count(fts);
  n_phrase = api->phrase_count(fts);

  n_int = 0;
  for(i=0; arg[i]; i++){
    int n = __fts5_matchinfo_flagsize(n_col, n_phrase, arg[i]);
    if( n<0 ){
      char *err = dbsql_mprintf("unrecognized matchinfo flag: %c", arg[i]);
      dbsql_result_error(ctx, err, -1);
      dbsql_free(err);
      return 0;
    }
    n_int += n;
  }

  n_byte = sizeof(fts5_matchinfo_ctx_t)          /* The struct itself */
         + sizeof(u_int32_t) * n_int               /* The p->rets[] array */
         + (i+1);                           /* The p->arg string */
  p = (fts5_matchinfo_ctx_t*)dbsql_malloc64(n_byte);
  if( p==0 ){
    dbsql_result_error_nomem(ctx);
    return 0;
  }
  memset(p, 0, n_byte);

  p->n_col = n_col;
  p->n_phrase = n_phrase;
  p->rets = (u_int32_t*)&p[1];
  p->n_ret = n_int;
  p->arg = (char*)&p->rets[n_int];
  memcpy(p->arg, arg, i);

  rc = __fts5_matchinfo_iter(api, fts, p, __fts5_matchinfo_global_cb);
  if( rc!=DBSQL_OK ){
    dbsql_result_error_code(ctx, rc);
    dbsql_free(p);
    p = 0;
  }

  return p;
}

static void __fts5_matchinfo_func(
  const fts5_extension_api_t *api,   /* API offered by current FTS version */
  fts5_context_t *fts,              /* First arg to pass to api functions */
  dbsql_func_t *ctx,          /* Context for returning result/error */
  int n_val,                       /* Number of values in vals[] array */
  dbsql_value_t **vals           /* Array of trailing arguments */
){
  const char *arg;
  fts5_matchinfo_ctx_t *p;
  int rc = DBSQL_OK;

  if( n_val>0 ){
    arg = (const char*)dbsql_value_text(vals[0]);
  }else{
    arg = "pcx";
  }

  p = (fts5_matchinfo_ctx_t*)api->get_auxdata(fts, 0);
  if( p==0 || dbsql_stricmp(arg, p->arg) ){
    p = __fts5_matchinfo_new(api, fts, ctx, arg);
    if( p==0 ){
      rc = DBSQL_NOMEM;
    }else{
      rc = api->set_auxdata(fts, p, dbsql_free);
    }
  }

  if( rc==DBSQL_OK ){
    rc = __fts5_matchinfo_iter(api, fts, p, __fts5_matchinfo_local_cb);
  }
  if( rc!=DBSQL_OK ){
    dbsql_result_error_code(ctx, rc);
  }else{
    /* No errors has occured, so return a copy of the array of integers. */
    int n_byte = p->n_ret * sizeof(u_int32_t);
    dbsql_result_blob(ctx, (void*)p->rets, n_byte, DBSQL_TRANSIENT);
  }
}

/*
** Register "matchinfo" with global API object api.
*/
int __fts5_test_register_matchinfo_api(fts5_api *api){
  int rc;

  /* If fts5_api_from_db() returned NULL, then either FTS5 is not registered
  ** with this database handle, or an error (OOM perhaps?) has occurred.
  **
  ** Also check that the fts5_api object is version 2 or newer.  
  */ 
  if( api==0 || api->i_version<2 ){
    return DBSQL_ERROR;
  }

  /* Register the implementation of matchinfo() */
  rc = api->create_function(api, "matchinfo", 0, __fts5_matchinfo_func, 0);

  return rc;
}

/*
** Register "matchinfo" with database handle db.
*/
int __fts5_test_register_matchinfo(DBSQL *db){
  int rc;                         /* Return code */
  fts5_api *api;                 /* FTS5 API functions */

  /* Extract the FTS5 API pointer from the database handle. The 
  ** fts5_api_from_db() function above is copied verbatim from the 
  ** FTS5 documentation. Refer there for details. */
  rc = fts5_api_from_db(db, &api);
  if( rc!=DBSQL_OK ) return rc;

  return __fts5_test_register_matchinfo_api(api);
}

#endif /* DBSQL_ENABLE_FTS5 */
