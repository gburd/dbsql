/*
** 2006 June 10
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing the virtual table interfaces.  This code
** is not included in the dbsql library.  It is used for automated
** testing of the dbsql library.
*/

/* The code in this file defines a DBSQL virtual-table module that
** provides a read-only view of the current database schema. There is one
** row in the schema table for each column in the database schema.
*/
#define SCHEMA \
"CREATE TABLE x("                                                            \
  "database,"          /* Name of database (i.e. main, temp etc.) */         \
  "tablename,"         /* Name of table */                                   \
  "cid,"               /* column_t number (from left-to-right, 0 upward) */    \
  "name,"              /* column_t name */                                     \
  "type,"              /* Specified type (i.e. VARCHAR(32)) */               \
  "not_null,"          /* Boolean. True if NOT NULL was specified */         \
  "dflt_value,"        /* Default value for this column */                   \
  "pk"                 /* True if this column is part of the primary key */  \
")"

/* If DBSQL_TEST is defined this code is preprocessed for use as part
** of the sqlite test binary "testfixture". Otherwise it is preprocessed
** to be compiled into an sqlite dynamic extension.
*/
#ifdef DBSQL_TEST
#  include "dbsql_int.h"
#  include "tcl_dbsql.h"
#else
#  include "dbsql_ext.h"
  DBSQL_EXTENSION_INIT1
#endif

#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef struct schema_vtab schema_vtab;
typedef struct schema_cursor schema_cursor;

/* A schema table object */
struct schema_vtab {
  dbsql_vtab_t base;
  DBSQL *db;
};

/* A schema table cursor object */
struct schema_cursor {
  dbsql_vtab_cursor_t base;
  dbsql_stmt_t *db_list;
  dbsql_stmt_t *table_list;
  dbsql_stmt_t *column_list;
  int rowid;
};

/*
** None of this works unless we have virtual tables.
*/
#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** table_t destructor for the schema module.
*/
static int __schema_destroy(dbsql_vtab_t *vtab){
  dbsql_free(vtab);
  return 0;
}

/*
** table_t constructor for the schema module.
*/
static int __schema_create(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **pp_vtab,
  char **err_p
){
  int rc = DBSQL_NOMEM;
  schema_vtab *vtab = dbsql_malloc(sizeof(schema_vtab));
  if( vtab ){
    memset(vtab, 0, sizeof(schema_vtab));
    vtab->db = db;
#ifndef DBSQL_OMIT_VIRTUALTABLE
    rc = dbsql_declare_vtab(db, SCHEMA);
#endif
  }
  *pp_vtab = (dbsql_vtab_t *)vtab;
  return rc;
}

/*
** Open a new cursor on the schema table.
*/
static int __schema_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  int rc = DBSQL_NOMEM;
  schema_cursor *cur;
  cur = dbsql_malloc(sizeof(schema_cursor));
  if( cur ){
    memset(cur, 0, sizeof(schema_cursor));
    *cursor = (dbsql_vtab_cursor_t *)cur;
    rc = DBSQL_OK;
  }
  return rc;
}

/*
** Close a schema table cursor.
*/
static int __schema_close(dbsql_vtab_cursor_t *cur){
  schema_cursor *p_cur = (schema_cursor *)cur;
  dbsql_finalize(p_cur->db_list);
  dbsql_finalize(p_cur->table_list);
  dbsql_finalize(p_cur->column_list);
  dbsql_free(p_cur);
  return DBSQL_OK;
}

/*
** Retrieve a column of data.
*/
static int __schema_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  schema_cursor *p_cur = (schema_cursor *)cur;
  switch( i ){
    case 0:
      dbsql_result_value(ctx, dbsql_column_value(p_cur->db_list, 1));
      break;
    case 1:
      dbsql_result_value(ctx, dbsql_column_value(p_cur->table_list, 0));
      break;
    default:
      dbsql_result_value(ctx, dbsql_column_value(p_cur->column_list, i-2));
      break;
  }
  return DBSQL_OK;
}

/*
** Retrieve the current rowid.
*/
static int __schema_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  schema_cursor *p_cur = (schema_cursor *)cur;
  *rowid = p_cur->rowid;
  return DBSQL_OK;
}

static int finalize(dbsql_stmt_t **stmt){
  int rc = dbsql_finalize(*stmt);
  *stmt = 0;
  return rc;
}

static int __schema_eof(dbsql_vtab_cursor_t *cur){
  schema_cursor *p_cur = (schema_cursor *)cur;
  return (p_cur->db_list ? 0 : 1);
}

/*
** Advance the cursor to the next row.
*/
static int __schema_next(dbsql_vtab_cursor_t *cur){
  int rc = DBSQL_OK;
  schema_cursor *p_cur = (schema_cursor *)cur;
  schema_vtab *vtab = (schema_vtab *)(cur->vtab);
  char *sql = 0;

  while( !p_cur->column_list || DBSQL_ROW!=dbsql_step(p_cur->column_list) ){
    if( DBSQL_OK!=(rc = finalize(&p_cur->column_list)) ) goto next_exit;

    while( !p_cur->table_list || DBSQL_ROW!=dbsql_step(p_cur->table_list) ){
      if( DBSQL_OK!=(rc = finalize(&p_cur->table_list)) ) goto next_exit;

      assert(p_cur->db_list);
      while( DBSQL_ROW!=dbsql_step(p_cur->db_list) ){
        rc = finalize(&p_cur->db_list);
        goto next_exit;
      }

      /* Set sql to the SQL to pull the list of tables from the 
      ** dbsql_schema (or dbsql_temp_schema) table of the database
      ** identified by the row pointed to by the SQL statement cur->db_list
      ** (iterating through a "PRAGMA database_list;" statement).
      */
      if( dbsql_column_int(p_cur->db_list, 0)==1 ){
        sql = dbsql_mprintf(
            "SELECT name FROM dbsql_temp_schema WHERE type='table'"
        );
      }else{
        dbsql_stmt_t *db_list = p_cur->db_list;
        sql = dbsql_mprintf(
            "SELECT name FROM %Q.dbsql_schema WHERE type='table'",
             dbsql_column_text(db_list, 1)
        );
      }
      if( !sql ){
        rc = DBSQL_NOMEM;
        goto next_exit;
      }

      rc = dbsql_prepare(vtab->db, sql, -1, &p_cur->table_list, 0);
      dbsql_free(sql);
      if( rc!=DBSQL_OK ) goto next_exit;
    }

    /* Set sql to the SQL to the table_info pragma for the table currently
    ** identified by the rows pointed to by statements cur->db_list and
    ** cur->table_list.
    */
    sql = dbsql_mprintf("PRAGMA %Q.table_info(%Q)", 
        dbsql_column_text(p_cur->db_list, 1),
        dbsql_column_text(p_cur->table_list, 0)
    );

    if( !sql ){
      rc = DBSQL_NOMEM;
      goto next_exit;
    }
    rc = dbsql_prepare(vtab->db, sql, -1, &p_cur->column_list, 0);
    dbsql_free(sql);
    if( rc!=DBSQL_OK ) goto next_exit;
  }
  p_cur->rowid++;

next_exit:
  /* TODO: Handle rc */
  return rc;
}

/*
** Reset a schema table cursor.
*/
static int __schema_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  int rc;
  schema_vtab *vtab = (schema_vtab *)(vtab_cursor->vtab);
  schema_cursor *cur = (schema_cursor *)vtab_cursor;
  cur->rowid = 0;
  finalize(&cur->table_list);
  finalize(&cur->column_list);
  finalize(&cur->db_list);
  rc = dbsql_prepare(vtab->db,"PRAGMA database_list", -1, &cur->db_list, 0);
  return (rc==DBSQL_OK ? __schema_next(vtab_cursor) : rc);
}

/*
** Analyse the WHERE condition.
*/
static int __schema_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  return DBSQL_OK;
}

/*
** A virtual table module that merely echos method calls into TCL
** variables.
*/
static dbsql_module_t __schema_module = {
  0,                           /* i_version */
  __schema_create,
  __schema_create,
  __schema_best_index,
  __schema_destroy,
  __schema_destroy,
  __schema_open,                  /* x_open - open a cursor */
  __schema_close,                 /* x_close - close a cursor */
  __schema_filter,                /* filter - configure scan constraints */
  __schema_next,                  /* next - advance a cursor */
  __schema_eof,                   /* eof */
  __schema_column,                /* column - read data */
  __schema_rowid,                 /* rowid - read data */
  0,                           /* update */
  0,                           /* begin */
  0,                           /* x_sync */
  0,                           /* commit */
  0,                           /* rollback */
  0,                           /* find_method */
  0,                           /* rename */
  0,                           /* savepoint */
  0,                           /* release */
  0,                           /* rollback_to */
  0,                           /* shadow_name */
  0                            /* integrity */
};

#endif /* !defined(DBSQL_OMIT_VIRTUALTABLE) */

#ifdef DBSQL_TEST

/*
** Decode a pointer to an DBSQL object.
*/
extern int __get_db_pointer(Tcl_Interp *interp, const char *a, DBSQL **db);

/*
** Register the schema virtual table module.
*/
static int DBSQL_TCLAPI register_schema_module(
  ClientData clientData, /* Not used */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  DBSQL *db;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
#ifndef DBSQL_OMIT_VIRTUALTABLE
  dbsql_create_module(db, "schema", &__schema_module, 0);
#endif
  return TCL_OK;
}

/*
** Register commands with the TCL interpreter.
*/
int Dbsqltestschema_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
     void *clientData;
  } obj_cmds[] = {
     { "register_schema_module", register_schema_module, 0 },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, 
        obj_cmds[i].proc, obj_cmds[i].clientData, 0);
  }
  return TCL_OK;
}

#else

/*
** Extension load function.
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_schema_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  DBSQL_EXTENSION_INIT2(api);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  dbsql_create_module(db, "schema", &__schema_module, 0);
#endif
  return 0;
}

#endif
