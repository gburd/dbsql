/*
** 2017-05-31
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
** This file demonstrates an eponymous virtual table that returns information
** about all prepared statements for the database connection.
**
** Usage example:
**
**     .load ./stmt
**     .mode line
**     .header on
**     SELECT * FROM stmt;
*/
#if !defined(DBSQL_CORE) || defined(DBSQL_ENABLE_STMTVTAB)
#if !defined(DBSQLINT_H)
#include "dbsql_ext.h"
#endif
DBSQL_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE


#define STMT_NUM_INTEGER_COLUMN 10
typedef struct stmt_row_t stmt_row_t;
struct stmt_row_t {
  int64_t i_rowid;                /* Rowid value */
  char *z_sql;                          /* column "sql" */
  int cols[STMT_NUM_INTEGER_COLUMN+1]; /* all other column values */
  stmt_row_t *next;                      /* Next row to return */
};

/* stmt_vtab is a subclass of dbsql_vtab_t which will
** serve as the underlying representation of a stmt virtual table
*/
typedef struct stmt_vtab stmt_vtab;
struct stmt_vtab {
  dbsql_vtab_t base;  /* Base class - must be first */
  DBSQL *db;        /* Database connection for this stmt vtab */
};

/* stmt_cursor is a subclass of dbsql_vtab_cursor_t which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct stmt_cursor stmt_cursor;
struct stmt_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  DBSQL *db;               /* Database connection for this cursor */
  stmt_row_t *row;             /* Current row */
};

/*
** The __stmt_connect() method is invoked to create a new
** stmt_vtab that describes the stmt virtual table.
**
** Think of this routine as the constructor for stmt_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the stmt_vtab object and initialize all fields.
**
**    (2) Tell dbsql (via the dbsql_declare_vtab() interface) what the
**        result set of queries against stmt will look like.
*/
static int __stmt_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  stmt_vtab *p_new;
  int rc;

/* column_t numbers */
#define STMT_COLUMN_SQL     0   /* SQL for the statement */
#define STMT_COLUMN_NCOL    1   /* Number of result columns */
#define STMT_COLUMN_RO      2   /* True if read-only */
#define STMT_COLUMN_BUSY    3   /* True if currently busy */
#define STMT_COLUMN_NSCAN   4   /* DBSQL_STMTSTATUS_FULLSCAN_STEP */
#define STMT_COLUMN_NSORT   5   /* DBSQL_STMTSTATUS_SORT */
#define STMT_COLUMN_NAIDX   6   /* DBSQL_STMTSTATUS_AUTOINDEX */
#define STMT_COLUMN_NSTEP   7   /* DBSQL_STMTSTATUS_VM_STEP */
#define STMT_COLUMN_REPREP  8   /* DBSQL_STMTSTATUS_REPREPARE */
#define STMT_COLUMN_RUN     9   /* DBSQL_STMTSTATUS_RUN */
#define STMT_COLUMN_MEM    10   /* DBSQL_STMTSTATUS_MEMUSED */


  (void)aux;
  (void)argc;
  (void)argv;
  (void)err_p;
  rc = dbsql_declare_vtab(db,
     "CREATE TABLE x(sql,ncol,ro,busy,nscan,nsort,naidx,nstep,"
                    "reprep,run,mem)");
  if( rc==DBSQL_OK ){
    p_new = dbsql_malloc64( sizeof(*p_new) );
    *vtab = (dbsql_vtab_t*)p_new;
    if( p_new==0 ) return DBSQL_NOMEM;
    memset(p_new, 0, sizeof(*p_new));
    p_new->db = db;
  }
  return rc;
}

/*
** This method is the destructor for stmt_cursor objects.
*/
static int __stmt_disconnect(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return DBSQL_OK;
}

/*
** Constructor for a new stmt_cursor object.
*/
static int __stmt_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **cursor){
  stmt_cursor *cur;
  cur = dbsql_malloc64( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  cur->db = ((stmt_vtab*)p)->db;
  *cursor = &cur->base;
  return DBSQL_OK;
}

static void __stmt_csr_reset(stmt_cursor *cur){
  stmt_row_t *row = 0;
  stmt_row_t *next = 0;
  for(row=cur->row; row; row=next){
    next = row->next;
    dbsql_free(row);
  }
  cur->row = 0;
}

/*
** Destructor for a stmt_cursor.
*/
static int __stmt_close(dbsql_vtab_cursor_t *cur){
  __stmt_csr_reset((stmt_cursor*)cur);
  dbsql_free(cur);
  return DBSQL_OK;
}


/*
** Advance a stmt_cursor to its next row of output.
*/
static int __stmt_next(dbsql_vtab_cursor_t *cur){
  stmt_cursor *p_cur = (stmt_cursor*)cur;
  stmt_row_t *next = p_cur->row->next;
  dbsql_free(p_cur->row);
  p_cur->row = next;
  return DBSQL_OK;
}

/*
** Return values of columns for the row at which the stmt_cursor
** is currently pointing.
*/
static int __stmt_column(
  dbsql_vtab_cursor_t *cur,   /* The cursor */
  dbsql_func_t *ctx,       /* First argument to dbsql_result_...() */
  int i                       /* Which column to return */
){
  stmt_cursor *p_cur = (stmt_cursor*)cur;
  stmt_row_t *row = p_cur->row;
  if( i==STMT_COLUMN_SQL ){
    dbsql_result_text(ctx, row->z_sql, -1, DBSQL_TRANSIENT);
  }else{
    dbsql_result_int(ctx, row->cols[i]);
  }
  return DBSQL_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int __stmt_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  stmt_cursor *p_cur = (stmt_cursor*)cur;
  *rowid = p_cur->row->i_rowid;
  return DBSQL_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int __stmt_eof(dbsql_vtab_cursor_t *cur){
  stmt_cursor *p_cur = (stmt_cursor*)cur;
  return p_cur->row==0;
}

/*
** This method is called to "rewind" the stmt_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to __stmt_column() or __stmt_rowid() or 
** __stmt_eof().
*/
static int __stmt_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  stmt_cursor *cur = (stmt_cursor *)vtab_cursor;
  dbsql_stmt_t *p = 0;
  int64_t i_rowid = 1;
  stmt_row_t **row = 0;

  (void)idx_num;
  (void)idx_str;
  (void)argc;
  (void)argv;
  __stmt_csr_reset(cur);
  row = &cur->row;
  for(p=dbsql_next_stmt(cur->db, 0); p; p=dbsql_next_stmt(cur->db, p)){
    const char *sql = dbsql_sql(p);
    int64_t n_sql = sql ? strlen(sql)+1 : 0;
    stmt_row_t *p_new = (stmt_row_t*)dbsql_malloc64(sizeof(stmt_row_t) + n_sql);

    if( p_new==0 ) return DBSQL_NOMEM;
    memset(p_new, 0, sizeof(stmt_row_t));
    if( sql ){
      p_new->z_sql = (char*)&p_new[1];
      memcpy(p_new->z_sql, sql, n_sql);
    }
    p_new->cols[STMT_COLUMN_NCOL] = dbsql_column_count(p);
    p_new->cols[STMT_COLUMN_RO] = dbsql_stmt_readonly(p);
    p_new->cols[STMT_COLUMN_BUSY] = dbsql_stmt_busy(p);
    p_new->cols[STMT_COLUMN_NSCAN] = dbsql_stmt_status(
        p, DBSQL_STMTSTATUS_FULLSCAN_STEP, 0
    );
    p_new->cols[STMT_COLUMN_NSORT] = dbsql_stmt_status(
        p, DBSQL_STMTSTATUS_SORT, 0
    );
    p_new->cols[STMT_COLUMN_NAIDX] = dbsql_stmt_status(
        p, DBSQL_STMTSTATUS_AUTOINDEX, 0
    );
    p_new->cols[STMT_COLUMN_NSTEP] = dbsql_stmt_status(
        p, DBSQL_STMTSTATUS_VM_STEP, 0
    );
    p_new->cols[STMT_COLUMN_REPREP] = dbsql_stmt_status(
        p, DBSQL_STMTSTATUS_REPREPARE, 0
    );
    p_new->cols[STMT_COLUMN_RUN] = dbsql_stmt_status(
        p, DBSQL_STMTSTATUS_RUN, 0
    );
    p_new->cols[STMT_COLUMN_MEM] = dbsql_stmt_status(
        p, DBSQL_STMTSTATUS_MEMUSED, 0
    );
    p_new->i_rowid = i_rowid++;
    *row = p_new;
    row = &p_new->next;
  }

  return DBSQL_OK;
}

/*
** dbsql will invoke this method one or more times while planning a query
** that uses the stmt virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int __stmt_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  (void)tab;
  idx_info->estimated_cost = (double)500;
  idx_info->estimated_rows = 500;
  return DBSQL_OK;
}

/*
** This following structure defines all the methods for the 
** stmt virtual table.
*/
static dbsql_module_t __stmt_module = {
  0,                         /* i_version */
  0,                         /* create */
  __stmt_connect,               /* connect */
  __stmt_best_index,             /* best_index */
  __stmt_disconnect,            /* disconnect */
  0,                         /* destroy */
  __stmt_open,                  /* x_open - open a cursor */
  __stmt_close,                 /* x_close - close a cursor */
  __stmt_filter,                /* filter - configure scan constraints */
  __stmt_next,                  /* next - advance a cursor */
  __stmt_eof,                   /* eof - check for end of scan */
  __stmt_column,                /* column - read data */
  __stmt_rowid,                 /* rowid - read data */
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

int __stmt_vtab_init(DBSQL *db){
  int rc = DBSQL_OK;
#ifndef DBSQL_OMIT_VIRTUALTABLE
  rc = dbsql_create_module(db, "dbsql_stmt", &__stmt_module, 0);
#endif
  return rc;
}

#ifndef DBSQL_CORE
#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_stmt_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  rc = __stmt_vtab_init(db);
#endif
  return rc;
}
#endif /* DBSQL_CORE */
#endif /* !defined(DBSQL_CORE) || defined(DBSQL_ENABLE_STMTVTAB) */
