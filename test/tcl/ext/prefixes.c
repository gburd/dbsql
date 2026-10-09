/*
** 2018-04-19
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file implements a table-valued function:
**
**      prefixes('abcdefg')
**
** The function has a single (non-HIDDEN) column named prefix that takes
** on all prefixes of the string in its argument, including an empty string
** and the input string itself.  The order of prefixes is from longest
** to shortest.
*/
#if !defined(DBSQL_CORE) || !defined(DBSQL_OMIT_VIRTUALTABLE)
#if !defined(DBSQLINT_H)
#include "dbsql_ext.h"
#endif
DBSQL_EXTENSION_INIT1
#include <string.h>
#include <assert.h>

/* prefixes_vtab is a subclass of dbsql_vtab_t which is
** underlying representation of the virtual table
*/
typedef struct prefixes_vtab prefixes_vtab;
struct prefixes_vtab {
  dbsql_vtab_t base;  /* Base class - must be first */
  /* No additional fields are necessary */
};

/* prefixes_cursor is a subclass of dbsql_vtab_cursor_t which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct prefixes_cursor prefixes_cursor;
struct prefixes_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  int64_t i_rowid;      /* The rowid */
  char *str;                /* Original string to be prefixed */
  int n_str;                  /* Length of the string in bytes */
};

/*
** The __prefixes_connect() method is invoked to create a new
** template virtual table.
**
** Think of this routine as the constructor for prefixes_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the prefixes_vtab object and initialize all fields.
**
**    (2) Tell dbsql (via the dbsql_declare_vtab() interface) what the
**        result set of queries against the virtual table will look like.
*/
static int __prefixes_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  prefixes_vtab *p_new;
  int rc;

  rc = dbsql_declare_vtab(db,
           "CREATE TABLE prefixes(prefix TEXT, original_string TEXT HIDDEN)"
       );
  if( rc==DBSQL_OK ){
    p_new = dbsql_malloc64( sizeof(*p_new) );
    *vtab = (dbsql_vtab_t*)p_new;
    if( p_new==0 ) return DBSQL_NOMEM;
    memset(p_new, 0, sizeof(*p_new));
    dbsql_vtab_config(db, DBSQL_VTAB_INNOCUOUS);
  }
  return rc;
}

/*
** This method is the destructor for prefixes_vtab objects.
*/
static int __prefixes_disconnect(dbsql_vtab_t *vtab){
  prefixes_vtab *p = (prefixes_vtab*)vtab;
  dbsql_free(p);
  return DBSQL_OK;
}

/*
** Constructor for a new prefixes_cursor object.
*/
static int __prefixes_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **cursor){
  prefixes_cursor *cur;
  cur = dbsql_malloc64( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Destructor for a prefixes_cursor.
*/
static int __prefixes_close(dbsql_vtab_cursor_t *cur){
  prefixes_cursor *p_cur = (prefixes_cursor*)cur;
  dbsql_free(p_cur->str);
  dbsql_free(p_cur);
  return DBSQL_OK;
}


/*
** Advance a prefixes_cursor to its next row of output.
*/
static int __prefixes_next(dbsql_vtab_cursor_t *cur){
  prefixes_cursor *p_cur = (prefixes_cursor*)cur;
  p_cur->i_rowid++;
  return DBSQL_OK;
}

/*
** Return values of columns for the row at which the prefixes_cursor
** is currently pointing.
*/
static int __prefixes_column(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i                       /* Which column to return */
){
  prefixes_cursor *p_cur = (prefixes_cursor*)cur;
  switch( i ){
    case 0:
      dbsql_result_text(ctx, p_cur->str, p_cur->n_str - (int)p_cur->i_rowid,
                          0); 
      break;
    default:
      dbsql_result_text(ctx, p_cur->str, p_cur->n_str, 0);
      break;
  }
  return DBSQL_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int __prefixes_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  prefixes_cursor *p_cur = (prefixes_cursor*)cur;
  *rowid = p_cur->i_rowid;
  return DBSQL_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int __prefixes_eof(dbsql_vtab_cursor_t *cur){
  prefixes_cursor *p_cur = (prefixes_cursor*)cur;
  return p_cur->i_rowid>p_cur->n_str;
}

/*
** This method is called to "rewind" the prefixes_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to __prefixes_column() or __prefixes_rowid() or 
** __prefixes_eof().
*/
static int __prefixes_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  prefixes_cursor *cur = (prefixes_cursor *)vtab_cursor;
  dbsql_free(cur->str);
  if( argc>0 ){
    cur->str = dbsql_mprintf("%s", dbsql_value_text(argv[0]));
    cur->n_str = cur->str ? (int)strlen(cur->str) : 0;
  }else{
    cur->str = 0;
    cur->n_str = 0;
  }
  cur->i_rowid = 0;
  return DBSQL_OK;
}

/*
** dbsql will invoke this method one or more times while planning a query
** that uses the virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int __prefixes_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  /* Search for a usable equality constraint against column 1 
  ** (original_string) and use it if at all possible */
  int i;
  const struct dbsql_index_constraint_t *p;

  for(i=0, p=idx_info->constraints; i<idx_info->n_constraint; i++, p++){
    if( p->i_column!=1 ) continue;
    if( p->op!=DBSQL_INDEX_CONSTRAINT_EQ ) continue;
    if( !p->usable ) continue;
    idx_info->constraint_usages[i].argv_index = 1;
    idx_info->constraint_usages[i].omit = 1;
    idx_info->estimated_cost = (double)10;
    idx_info->estimated_rows = 10;
    return DBSQL_OK;
  }
  idx_info->estimated_cost = (double)1000000000;
  idx_info->estimated_rows = 1000000000;
  return DBSQL_OK;
}

/*
** This following structure defines all the methods for the 
** virtual table.
*/
static dbsql_module_t __prefixes_module = {
  /* i_version    */ 0,
  /* create     */ 0,
  /* connect    */ __prefixes_connect,
  /* best_index  */ __prefixes_best_index,
  /* disconnect */ __prefixes_disconnect,
  /* destroy    */ 0,
  /* x_open       */ __prefixes_open,
  /* x_close      */ __prefixes_close,
  /* filter     */ __prefixes_filter,
  /* next       */ __prefixes_next,
  /* eof        */ __prefixes_eof,
  /* column     */ __prefixes_column,
  /* rowid      */ __prefixes_rowid,
  /* update     */ 0,
  /* begin      */ 0,
  /* x_sync       */ 0,
  /* commit     */ 0,
  /* rollback   */ 0,
  /* find_method */ 0,
  /* rename     */ 0,
  /* savepoint  */ 0,
  /* release    */ 0,
  /* rollback_to */ 0,
  /* shadow_name */ 0,
  /* integrity  */ 0
};

/*
** This is a copy of the DBSQL_SKIP_UTF8(in) macro in dbsql_int.h.
**
** Assuming in points to the first byte of a UTF-8 character,
** advance in to point to the first byte of the next UTF-8 character.
*/
#define PREFIX_SKIP_UTF8(in) {                        \
  if( (*(in++))>=0xc0 ){                              \
    while( (*in & 0xc0)==0x80 ){ in++; }             \
  }                                                    \
}

/*
** Implementation of function prefix_length(). This function accepts two
** strings as arguments and returns the length in characters (not bytes), 
** of the longest prefix shared by the two strings. For example:
**
**   prefix_length('abcdxxx', 'abcyy') == 3
**   prefix_length('abcdxxx', 'bcyyy') == 0
**   prefix_length('abcdxxx', 'ab')    == 2
**   prefix_length('ab',      'abcd')  == 2
**
** This function assumes the input is well-formed utf-8. If it is not,
** it is possible for this function to return -1.
*/
static void __prefix_length_func(
  dbsql_func_t *ctx,
  int n_val,
  dbsql_value_t **vals
){
  int n_byte;                      /* Number of bytes to compare */
  int n_ret = 0;                   /* Return value */
  const unsigned char *l = dbsql_value_text(vals[0]);
  const unsigned char *r = dbsql_value_text(vals[1]);
  int n_l = dbsql_value_bytes(vals[0]);
  int n_r = dbsql_value_bytes(vals[1]);
  int i;
  if( l==0 || r==0 ){
    dbsql_result_int(ctx, 0);
    return;
  }

  n_byte = (n_l > n_r ? n_l : n_r);
  for(i=0; i<n_byte; i++){
    if( l[i]==0 || l[i]!=r[i] ) break;
    if( (l[i] & 0xC0)!=0x80 ) n_ret++;
  }

  if( (l[i] & 0xC0)==0x80 ) n_ret--;
  dbsql_result_int(ctx, n_ret);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_prefixes_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
  rc = dbsql_create_module(db, "prefixes", &__prefixes_module, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(
        db, "prefix_length", 2, DBSQL_UTF8, 0, __prefix_length_func, 0, 0
    );
  }
  return rc;
}
#endif /* !defined(DBSQL_CORE) || !defined(DBSQL_OMIT_VIRTUALTABLE) */
