/*
** 2011 April 02
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
** This file implements a virtual table that returns the whole numbers
** between 1 and 4294967295, inclusive.
**
** Example:
**
**     CREATE VIRTUAL TABLE nums USING wholenumber;
**     SELECT value FROM nums WHERE value<10;
**
** Results in:
**
**     1 2 3 4 5 6 7 8 9
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE


/* A wholenumber cursor object */
typedef struct wholenumber_cursor wholenumber_cursor;
struct wholenumber_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  int64_t i_value;      /* Current value */
  int64_t mx_value;     /* Maximum value */
};

/* Methods for the wholenumber module */
static int __wholenumber_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  dbsql_vtab_t *p_new;
  p_new = *vtab = dbsql_malloc64( sizeof(*p_new) );
  if( p_new==0 ) return DBSQL_NOMEM;
  dbsql_declare_vtab(db, "CREATE TABLE x(value)");
  dbsql_vtab_config(db, DBSQL_VTAB_INNOCUOUS);
  memset(p_new, 0, sizeof(*p_new));
  return DBSQL_OK;
}
/* Note that for this virtual table, the create and connect
** methods are identical. */

static int __wholenumber_disconnect(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return DBSQL_OK;
}
/* The disconnect and destroy methods are also the same */


/*
** Open a new wholenumber cursor.
*/
static int __wholenumber_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **cursor){
  wholenumber_cursor *cur;
  cur = dbsql_malloc64( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Close a wholenumber cursor.
*/
static int __wholenumber_close(dbsql_vtab_cursor_t *cur){
  dbsql_free(cur);
  return DBSQL_OK;
}


/*
** Advance a cursor to its next row of output
*/
static int __wholenumber_next(dbsql_vtab_cursor_t *cur){
  wholenumber_cursor *p_cur = (wholenumber_cursor*)cur;
  p_cur->i_value++;
  return DBSQL_OK;
}

/*
** Return the value associated with a wholenumber.
*/
static int __wholenumber_column(
  dbsql_vtab_cursor_t *cur,
  dbsql_func_t *ctx,
  int i
){
  wholenumber_cursor *p_cur = (wholenumber_cursor*)cur;
  dbsql_result_int64(ctx, p_cur->i_value);
  return DBSQL_OK;
}

/*
** The rowid.
*/
static int __wholenumber_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  wholenumber_cursor *p_cur = (wholenumber_cursor*)cur;
  *rowid = p_cur->i_value;
  return DBSQL_OK;
}

/*
** When the wholenumber_cursor.r_limit value is 0 or less, that is a signal
** that the cursor has nothing more to output.
*/
static int __wholenumber_eof(dbsql_vtab_cursor_t *cur){
  wholenumber_cursor *p_cur = (wholenumber_cursor*)cur;
  return p_cur->i_value>p_cur->mx_value || p_cur->i_value==0;
}

/*
** Called to "rewind" a cursor back to the beginning so that
** it starts its output over again.  Always called at least once
** prior to any __wholenumber_column, __wholenumber_rowid, or __wholenumber_eof call.
**
**    idx_num   Constraints
**    ------   ---------------------
**      0      (none)
**      1      value > $argv0
**      2      value >= $argv0
**      4      value < $argv0
**      8      value <= $argv0
**
**      5      value > $argv0 AND value < $argv1
**      6      value >= $argv0 AND value < $argv1
**      9      value > $argv0 AND value <= $argv1
**     10      value >= $argv0 AND value <= $argv1
*/
static int __wholenumber_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  wholenumber_cursor *cur = (wholenumber_cursor *)vtab_cursor;
  int64_t v;
  int i = 0;
  cur->i_value = 1;
  cur->mx_value = 0xffffffff;  /* 4294967295 */
  if( idx_num & 3 ){
    v = dbsql_value_int64(argv[0]) + (idx_num&1);
    if( v>cur->i_value && v<=cur->mx_value ) cur->i_value = v;
    i++;
  }
  if( idx_num & 12 ){
    v = dbsql_value_int64(argv[i]) - ((idx_num>>2)&1);
    if( v>=cur->i_value && v<cur->mx_value ) cur->mx_value = v;
  }
  return DBSQL_OK;
}

/*
** Search for terms of these forms:
**
**  (1)  value > $value
**  (2)  value >= $value
**  (4)  value < $value
**  (8)  value <= $value
**
** idx_num is an ORed combination of 1 or 2 with 4 or 8.
*/
static int __wholenumber_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  int i;
  int idx_num = 0;
  int argv_idx = 1;
  int lt_idx = -1;
  int gt_idx = -1;
  const struct dbsql_index_constraint_t *constraint;
  constraint = idx_info->constraints;
  for(i=0; i<idx_info->n_constraint; i++, constraint++){
    if( constraint->usable==0 ) continue;
    if( (idx_num & 3)==0 && constraint->op==DBSQL_INDEX_CONSTRAINT_GT ){
      idx_num |= 1;
      lt_idx = i;
    }
    if( (idx_num & 3)==0 && constraint->op==DBSQL_INDEX_CONSTRAINT_GE ){
      idx_num |= 2;
      lt_idx = i;
    }
    if( (idx_num & 12)==0 && constraint->op==DBSQL_INDEX_CONSTRAINT_LT ){
      idx_num |= 4;
      gt_idx = i;
    }
    if( (idx_num & 12)==0 && constraint->op==DBSQL_INDEX_CONSTRAINT_LE ){
      idx_num |= 8;
      gt_idx = i;
    }
  }
  idx_info->idx_num = idx_num;
  if( lt_idx>=0 ){
    idx_info->constraint_usages[lt_idx].argv_index = argv_idx++;
    idx_info->constraint_usages[lt_idx].omit = 1;
  }
  if( gt_idx>=0 ){
    idx_info->constraint_usages[gt_idx].argv_index = argv_idx;
    idx_info->constraint_usages[gt_idx].omit = 1;
  }
  if( idx_info->n_order_by==1
   && idx_info->order_bies[0].desc==0
  ){
    idx_info->order_by_consumed = 1;
  }
  if( (idx_num & 12)==0 ){
    idx_info->estimated_cost = 1e99;
  }else if( (idx_num & 3)==0 ){
    idx_info->estimated_cost = (double)5;
  }else{
    idx_info->estimated_cost = (double)1;
  }
  return DBSQL_OK;
}

/*
** A virtual table module that provides read-only access to a
** Tcl global variable namespace.
*/
static dbsql_module_t __wholenumber_module = {
  0,                         /* i_version */
  __wholenumber_connect,
  __wholenumber_connect,
  __wholenumber_best_index,
  __wholenumber_disconnect, 
  __wholenumber_disconnect,
  __wholenumber_open,           /* x_open - open a cursor */
  __wholenumber_close,          /* x_close - close a cursor */
  __wholenumber_filter,         /* filter - configure scan constraints */
  __wholenumber_next,           /* next - advance a cursor */
  __wholenumber_eof,            /* eof - check for end of scan */
  __wholenumber_column,         /* column - read data */
  __wholenumber_rowid,          /* rowid - read data */
  0,                         /* update */
  0,                         /* begin */
  0,                         /* x_sync */
  0,                         /* commit */
  0,                         /* rollback */
  0,                         /* find_method */
  0,                         /* rename */
  0,                         /* savepoint */
  0,                         /* release */
  0,                         /* rollback_to */
  0,                         /* shadow_name */
  0                          /* integrity */
};

#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_wholenumber_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  rc = dbsql_create_module(db, "wholenumber", &__wholenumber_module, 0);
#endif
  return rc;
}
