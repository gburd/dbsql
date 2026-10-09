/*
** 2022-01-19
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
** This file implements a virtual-table that returns information about
** how the query planner called the best_index method.  This virtual table
** is intended for testing and debugging only.
**
** The schema of the virtual table is this:
**
**    CREATE TABLE qpvtab(
**      vn     TEXT,           -- Name of an dbsql_index_info_t field
**      ix     INTEGER,        -- Array index or value
**      cn     TEXT,           -- column_t name
**      op     INTEGER,        -- operator
**      ux     BOOLEAN,        -- "usable" field
**      rhs    TEXT,           -- dbsql_vtab_rhs_value()
**
**      a, b, c, d, e,         -- Extra columns to attach constraints to
**
**      flags    INTEGER HIDDEN  -- control flags
**    );
**
** The virtual table returns a description of the dbsql_index_info_t object
** that was provided to the (successful) best_index method.  There is one
** row in the result table for each field in the dbsql_index_info_t object.
**
** The values of the "a" through "e" columns are one of:
**
**    1.   TEXT - the same as the column name
**    2.   INTEGER - 1 for "a", 2 for "b", and so forth
**
** Option 1 is the default behavior.  2 is use if there is a usable
** constraint on "flags" with an integer right-hand side that where the
** value of the right-hand side has its 0x001 bit set.
**
** All constraints on columns "a" through "e" are marked as "omit".
**
** If there is a usable constraint on "flags" that has a RHS value that
** is an integer and that integer has its 0x02 bit set, then the
** order_by_consumed flag is set.
**
** FLAGS SUMMARY:
**
**   0x001               Columns 'a' through 'e' have INT values
**   0x002               order_by_consumed is set
**   0x004               OFFSET and LIMIT have omit set
**
** COMPILE:
**
**   gcc -Wall -g -shared -f_pic -I. qpvtab.c -o qqvtab.so
**
** EXAMPLE USAGE:
**
**   .load ./qpvtab
**   SELECT rowid, *, flags FROM qpvtab(102)
**    WHERE a=19
**      AND b BETWEEN 4.5 and 'hello'
**      AND c<>x'aabbcc'
**    ORDER BY d, e DESC;
*/
#if !defined(DBSQLINT_H)
#include "dbsql_ext.h"
#endif
DBSQL_EXTENSION_INIT1
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#if !defined(DBSQL_OMIT_VIRTUALTABLE)

/* qpvtab_vtab is a subclass of dbsql_vtab_t which is
** underlying representation of the virtual table
*/
typedef struct qpvtab_vtab qpvtab_vtab;
struct qpvtab_vtab {
  dbsql_vtab_t base;  /* Base class - must be first */
};

/* qpvtab_cursor is a subclass of dbsql_vtab_cursor_t which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct qpvtab_cursor qpvtab_cursor;
struct qpvtab_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  int64_t i_rowid;      /* The rowid */
  const char *data;         /* Data to return */
  int n_data;                 /* Number of bytes of data */
  int flags;                 /* Flags value */
};

/*
** Names of columns
*/
static const char *__az_colname[] = {
  "vn",
  "ix",
  "cn",
  "op",
  "ux",
  "rhs",
  "a", "b", "c", "d", "e",
  "flags",
  ""
};

/*
** The __qpvtab_connect() method is invoked to create a new
** qpvtab virtual table.
*/
static int __qpvtab_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  qpvtab_vtab *p_new;
  int rc;

  rc = dbsql_declare_vtab(db,
         "CREATE TABLE x("
         " vn TEXT,"
         " ix INT,"
         " cn TEXT,"
         " op INT,"
         " ux BOOLEAN,"
         " rhs TEXT,"
         " a, b, c, d, e,"
         " flags INT HIDDEN)"
       );
#define QPVTAB_VN      0
#define QPVTAB_IX      1
#define QPVTAB_CN      2
#define QPVTAB_OP      3
#define QPVTAB_UX      4
#define QPVTAB_RHS     5
#define QPVTAB_A       6
#define QPVTAB_B       7
#define QPVTAB_C       8
#define QPVTAB_D       9
#define QPVTAB_E      10
#define QPVTAB_FLAGS  11
#define QPVTAB_NONE   12
  if( rc==DBSQL_OK ){
    p_new = dbsql_malloc64( sizeof(*p_new) );
    *vtab = (dbsql_vtab_t*)p_new;
    if( p_new==0 ) return DBSQL_NOMEM;
    memset(p_new, 0, sizeof(*p_new));
  }
  return rc;
}

/*
** This method is the destructor for qpvtab_vtab objects.
*/
static int __qpvtab_disconnect(dbsql_vtab_t *vtab){
  qpvtab_vtab *p = (qpvtab_vtab*)vtab;
  dbsql_free(p);
  return DBSQL_OK;
}

/*
** Constructor for a new qpvtab_cursor object.
*/
static int __qpvtab_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **cursor){
  qpvtab_cursor *cur;
  cur = dbsql_malloc64( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  *cursor = &cur->base;
  return DBSQL_OK;
}

/*
** Destructor for a qpvtab_cursor.
*/
static int __qpvtab_close(dbsql_vtab_cursor_t *cur){
  qpvtab_cursor *p_cur = (qpvtab_cursor*)cur;
  dbsql_free(p_cur);
  return DBSQL_OK;
}


/*
** Advance a qpvtab_cursor to its next row of output.
*/
static int __qpvtab_next(dbsql_vtab_cursor_t *cur){
  qpvtab_cursor *p_cur = (qpvtab_cursor*)cur;
  if( p_cur->i_rowid<p_cur->n_data ){
    const char *z = &p_cur->data[p_cur->i_rowid];
    const char *end = strchr(z, '\n');
    if( end ) end++;
    p_cur->i_rowid = (int)(end - p_cur->data);
  }
  return DBSQL_OK;
}

/*
** Return values of columns for the row at which the qpvtab_cursor
** is currently pointing.
*/
static int __qpvtab_column(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i                       /* Which column to return */
){
  qpvtab_cursor *p_cur = (qpvtab_cursor*)cur;
  if( i>=QPVTAB_VN && i<=QPVTAB_RHS && p_cur->i_rowid<p_cur->n_data ){
    const char *z = &p_cur->data[p_cur->i_rowid];
    const char *end;
    int j;
    j = QPVTAB_VN;
    while(1){
      end = strchr(z, j==QPVTAB_RHS ? '\n' : ',');
      if( j==i || end==0 ) break;
      z = end+1;
      j++;
    }
    if( end==z ){
      dbsql_result_null(ctx);
    }else if( i==QPVTAB_IX || i==QPVTAB_OP || i==QPVTAB_UX ){
      dbsql_result_int(ctx, atoi(z));
    }else{
      dbsql_result_text64(ctx, z, end-z, DBSQL_TRANSIENT, DBSQL_UTF8);
    }
  }else if( i>=QPVTAB_A && i<=QPVTAB_E ){
    if( p_cur->flags & 0x001 ){
      dbsql_result_int(ctx, i-QPVTAB_A+1);
    }else{
      char x = 'a'+i-QPVTAB_A;
      dbsql_result_text64(ctx, &x, 1, DBSQL_TRANSIENT, DBSQL_UTF8);
    }
  }else if( i==QPVTAB_FLAGS ){
    dbsql_result_int(ctx, p_cur->flags);
  }
  return DBSQL_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int __qpvtab_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  qpvtab_cursor *p_cur = (qpvtab_cursor*)cur;
  *rowid = p_cur->i_rowid;
  return DBSQL_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int __qpvtab_eof(dbsql_vtab_cursor_t *cur){
  qpvtab_cursor *p_cur = (qpvtab_cursor*)cur;
  return p_cur->i_rowid>=p_cur->n_data;
}

/*
** This method is called to "rewind" the qpvtab_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to __qpvtab_column() or __qpvtab_rowid() or 
** __qpvtab_eof().
*/
static int __qpvtab_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  qpvtab_cursor *cur = (qpvtab_cursor *)vtab_cursor;
  cur->i_rowid = 0;
  cur->data = idx_str;
  cur->n_data = (int)strlen(idx_str);
  cur->flags = idx_num;
  return DBSQL_OK;
}

/*
** Append the text of a value to str
*/
static void __qpvtab_str_append_value(
  dbsql_str_t *str,
  dbsql_value_t *val
){
  switch( dbsql_value_type(val) ){
    case DBSQL_NULL:
      dbsql_str_appendf(str, "NULL");
      break;
    case DBSQL_INTEGER:
      dbsql_str_appendf(str, "%lld", dbsql_value_int64(val));
      break;
    case DBSQL_FLOAT:
      dbsql_str_appendf(str, "%!f", dbsql_value_double(val));
      break;
    case DBSQL_TEXT: {
      int i;
      const char *a = (const char*)dbsql_value_text(val);
      int n = dbsql_value_bytes(val);
      dbsql_str_append(str, "'", 1);
      for(i=0; i<n; i++){
        char c = a[i];
        if( c=='\n' ) c = ' ';
        dbsql_str_append(str, &c, 1);
        if( c=='\'' ) dbsql_str_append(str, &c, 1);
      }
      dbsql_str_append(str, "'", 1);
      break;
    }
    case DBSQL_BLOB: {
      int i;
      const unsigned char *a = dbsql_value_blob(val);
      int n = dbsql_value_bytes(val);
      dbsql_str_append(str, "x'", 2);
      for(i=0; i<n; i++){
        dbsql_str_appendf(str, "%02x", a[i]);
      }
      dbsql_str_append(str, "'", 1);
      break;
    }
  }
}

/*
** dbsql will invoke this method one or more times while planning a query
** that uses the virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int __qpvtab_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  dbsql_str_t *str = dbsql_str_new(0);
  int i, k = 0;
  int rc;
  dbsql_str_appendf(str, "nConstraint,%d,,,,\n", idx_info->n_constraint);
  for(i=0; i<idx_info->n_constraint; i++){
    dbsql_value_t *val;
    int i_col = idx_info->constraints[i].i_column;
    int op = idx_info->constraints[i].op;
    if( i_col==QPVTAB_FLAGS &&  idx_info->constraints[i].usable ){
      val = 0;
      rc = dbsql_vtab_rhs_value(idx_info, i, &val);
      assert( rc==DBSQL_OK || val==0 );
      if( val ){
        idx_info->idx_num = dbsql_value_int(val);
        if( idx_info->idx_num & 0x002 ) idx_info->order_by_consumed = 1;
      }
    }
    if( op==DBSQL_INDEX_CONSTRAINT_LIMIT
     || op==DBSQL_INDEX_CONSTRAINT_OFFSET
    ){
      i_col = QPVTAB_NONE;
    }
    dbsql_str_appendf(str,"aConstraint,%d,%s,%d,%d,",
       i,
       __az_colname[i_col],
       op,
       idx_info->constraints[i].usable);
    val = 0;
    rc = dbsql_vtab_rhs_value(idx_info, i, &val);
    assert( rc==DBSQL_OK || val==0 );
    if( val ){
      __qpvtab_str_append_value(str, val);
    }
    dbsql_str_append(str, "\n", 1);
  }
  for(i=0; i<idx_info->n_constraint; i++){
    int i_col = idx_info->constraints[i].i_column;
    int op = idx_info->constraints[i].op;
    if( op==DBSQL_INDEX_CONSTRAINT_LIMIT
     || op==DBSQL_INDEX_CONSTRAINT_OFFSET
    ){
      i_col = QPVTAB_NONE;
    }
    if( i_col>=QPVTAB_A && idx_info->constraints[i].usable ){
      idx_info->constraint_usages[i].argv_index = ++k;
      if( i_col<=QPVTAB_FLAGS || (idx_info->idx_num & 0x004)!=0 ){
        idx_info->constraint_usages[i].omit = 1;
      }
    }
  }
  dbsql_str_appendf(str, "nOrderBy,%d,,,,\n", idx_info->n_order_by);
  for(i=0; i<idx_info->n_order_by; i++){
    int i_col = idx_info->order_bies[i].i_column;
    dbsql_str_appendf(str, "aOrderBy,%d,%s,%d,,\n",i,
      i_col>=0 ? __az_colname[i_col] : "rowid",
      idx_info->order_bies[i].desc
    );
  }
  dbsql_str_appendf(str, "dbsql_vtab_distinct,%d,,,,\n", 
                      dbsql_vtab_distinct(idx_info));
  dbsql_str_appendf(str, "idxFlags,%d,,,,\n", idx_info->idx_flags);
  dbsql_str_appendf(str, "colUsed,%d,,,,\n", (int)idx_info->col_used);
  idx_info->estimated_cost = (double)10;
  idx_info->estimated_rows = 10;
  dbsql_str_appendf(str, "idxNum,%d,,,,\n", idx_info->idx_num);
  dbsql_str_appendf(str, "orderByConsumed,%d,,,,\n",
                      idx_info->order_by_consumed);
  idx_info->idx_str = dbsql_str_finish(str);
  idx_info->need_to_free_idx_str = 1;
  return DBSQL_OK;
}

/*
** This following structure defines all the methods for the 
** virtual table.
*/
static dbsql_module_t __qpvtab_module = {
  /* i_version    */ 0,
  /* create     */ 0,
  /* connect    */ __qpvtab_connect,
  /* best_index  */ __qpvtab_best_index,
  /* disconnect */ __qpvtab_disconnect,
  /* destroy    */ 0,
  /* x_open       */ __qpvtab_open,
  /* x_close      */ __qpvtab_close,
  /* filter     */ __qpvtab_filter,
  /* next       */ __qpvtab_next,
  /* eof        */ __qpvtab_eof,
  /* column     */ __qpvtab_column,
  /* rowid      */ __qpvtab_rowid,
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
#endif /* DBSQL_OMIT_VIRTUALTABLE */


#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_qpvtab_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  rc = dbsql_create_module(db, "qpvtab", &__qpvtab_module, 0);
#endif
  return rc;
}
