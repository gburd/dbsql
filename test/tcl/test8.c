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
#include "dbsql_int.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE

typedef struct echo_vtab echo_vtab;
typedef struct echo_cursor echo_cursor;

/*
** The test module defined in this file uses four global Tcl variables to
** communicate with test-scripts:
**
**     $::echo_module
**     $::echo_module_sync_fail
**     $::echo_module_begin_fail
**     $::echo_module_cost
**
** The variable ::echo_module is a list. Each time one of the following
** methods is called, one or more elements are appended to the list.
** This is used for automated testing of virtual table modules.
**
** The ::echo_module_sync_fail variable is set by test scripts and read
** by code in this file. If it is set to the name of a real table in the
** the database, then all x_sync operations on echo virtual tables that
** use the named table as a backing store will fail.
*/

/*
** Errors can be provoked within the following echo virtual table methods:
**
**   best_index   x_open     filter   next   
**   column      rowid    update   x_sync   
**   begin       rename
**
** This is done by setting the global tcl variable:
**
**   echo_module_fail($method,$tbl)
**
** where $method is set to the name of the virtual table method to fail
** (i.e. "best_index") and $tbl is the name of the table being echoed (not
** the name of the virtual table, the name of the underlying real table).
*/

/* 
** An echo virtual-table object.
**
** echo.vtab.indexes is an array of booleans. The nth entry is true if 
** the nth column of the real table is the left-most column of an index
** (implicit or otherwise). In other words, if dbsql can optimize
** a query like "SELECT * FROM real_table WHERE col = ?".
**
** Member variable cols[] contains copies of the column names of the real
** table.
*/
struct echo_vtab {
  dbsql_vtab_t base;
  Tcl_Interp *interp;     /* Tcl interpreter containing debug variables */
  DBSQL *db;            /* Database connection */

  int is_pattern;
  int in_transaction;      /* True if within a transaction */
  char *z_this;            /* Name of the echo table */
  char *table_name;       /* Name of the real table */
  char *log_name;         /* Name of the log table */
  int n_col;               /* Number of columns in the real table */
  int *indexes;            /* Array of size n_col. True if column has an index */
  char **cols;            /* Array of size n_col. column_t names */
};

/* An echo cursor object */
struct echo_cursor {
  dbsql_vtab_cursor_t base;
  dbsql_stmt_t *stmt;
};

static int __simulate_vtab_error(echo_vtab *p, const char *method){
  const char *err;
  char varname[128];
  varname[127] = '\0';
  dbsql_snprintf(127, varname, "echo_module_fail(%s,%s)", method, p->table_name);
  err = Tcl_GetVar(p->interp, varname, TCL_GLOBAL_ONLY);
  if( err ){
    p->base.errmsg = dbsql_mprintf("echo-vtab-error: %s", err);
  }
  return (err!=0);
}

/*
** Convert an SQL-style quoted string into a normal string by removing
** the quote characters.  The conversion is done in-place.  If the
** input does not begin with a quote character, then this routine
** is a no-op.
**
** Examples:
**
**     "abc"   becomes   abc
**     'xyz'   becomes   xyz
**     [pqr]   becomes   pqr
**     `mno`   becomes   mno
*/
static void __dequote_string(char *z){
  int quote;
  int i, j;
  if( z==0 ) return;
  quote = z[0];
  switch( quote ){
    case '\'':  break;
    case '"':   break;
    case '`':   break;                /* For __my_sql compatibility */
    case '[':   quote = ']';  break;  /* For MS __sql_server compatibility */
    default:    return;
  }
  for(i=1, j=0; z[i]; i++){
    if( z[i]==quote ){
      if( z[i+1]==quote ){
        z[j++] = quote;
        i++;
      }else{
        z[j++] = 0;
        break;
      }
    }else{
      z[j++] = z[i];
    }
  }
}

/*
** Retrieve the column names for the table named tab via database
** connection db. DBSQL_OK is returned on success, or an sqlite error
** code otherwise.
**
** If successful, the number of columns is written to *pn_col. *pa_col is
** set to point at dbsql_malloc()'d space containing the array of
** n_col column names. The caller is responsible for calling dbsql_free
** on *pa_col.
*/
static int __get_column_names(
  DBSQL *db, 
  const char *tab,
  char ***pa_col, 
  int *pn_col
){
  char **cols = 0;
  char *sql;
  dbsql_stmt_t *stmt = 0;
  int rc = DBSQL_OK;
  int n_col = 0;

  /* Prepare the statement "SELECT * FROM <tbl>". The column names
  ** of the result set of the compiled SELECT will be the same as
  ** the column names of table <tbl>.
  */
  sql = dbsql_mprintf("SELECT * FROM %Q", tab);
  if( !sql ){
    rc = DBSQL_NOMEM;
    goto out;
  }
  rc = dbsql_prepare(db, sql, -1, &stmt, 0);
  dbsql_free(sql);

  if( rc==DBSQL_OK ){
    int ii;
    int n_bytes;
    char *space;
    n_col = dbsql_column_count(stmt);

    /* Figure out how much space to allocate for the array of column names 
    ** (including space for the strings themselves). Then allocate it.
    */
    n_bytes = sizeof(char *) * n_col;
    for(ii=0; ii<n_col; ii++){
      const char *name = dbsql_column_name(stmt, ii);
      if( !name ){
        rc = DBSQL_NOMEM;
        goto out;
      }
      n_bytes += (int)strlen(name)+1;
    }
    cols = (char **)__malloc_zero(n_bytes);
    if( !cols ){
      rc = DBSQL_NOMEM;
      goto out;
    }

    /* Copy the column names into the allocated space and set up the
    ** pointers in the cols[] array.
    */
    space = (char *)(&cols[n_col]);
    for(ii=0; ii<n_col; ii++){
      cols[ii] = space;
      dbsql_snprintf(n_bytes, space, "%s", dbsql_column_name(stmt,ii));
      space += (int)strlen(space) + 1;
    }
    assert( (space-n_bytes)==(char *)cols );
  }

  *pa_col = cols;
  *pn_col = n_col;

out:
  dbsql_finalize(stmt);
  return rc;
}

/*
** Parameter tab is the name of a table in database db with n_col 
** columns. This function allocates an array of integers n_col in 
** size and populates it according to any implicit or explicit 
** indices on table tab.
**
** If successful, DBSQL_OK is returned and *pa_index set to point 
** at the allocated array. Otherwise, an error code is returned.
**
** See comments associated with the member variable indexes above 
** "struct echo_vtab" for details of the contents of the array.
*/
static int __get_index_array(
  DBSQL *db,             /* Database connection */
  const char *tab,        /* Name of table in database db */
  int n_col,
  int **pa_index
){
  dbsql_stmt_t *stmt = 0;
  int *indexes = 0;
  int rc;
  char *sql;

  /* Allocate space for the index array */
  indexes = (int *)__malloc_zero(sizeof(int) * n_col);
  if( !indexes ){
    rc = DBSQL_NOMEM;
    goto get_index_array_out;
  }

  /* Compile an sqlite pragma to loop through all indices on table tab */
  sql = dbsql_mprintf("PRAGMA index_list(%s)", tab);
  if( !sql ){
    rc = DBSQL_NOMEM;
    goto get_index_array_out;
  }
  rc = dbsql_prepare(db, sql, -1, &stmt, 0);
  dbsql_free(sql);

  /* For each index, figure out the left-most column and set the 
  ** corresponding entry in indexes[] to 1.
  */
  while( stmt && dbsql_step(stmt)==DBSQL_ROW ){
    const char *idx = (const char *)dbsql_column_text(stmt, 1);
    dbsql_stmt_t *stmt2 = 0;
    if( idx==0 ) continue;
    sql = dbsql_mprintf("PRAGMA index_info(%s)", idx);
    if( !sql ){
      rc = DBSQL_NOMEM;
      goto get_index_array_out;
    }
    rc = dbsql_prepare(db, sql, -1, &stmt2, 0);
    dbsql_free(sql);
    if( stmt2 && dbsql_step(stmt2)==DBSQL_ROW ){
      int cid = dbsql_column_int(stmt2, 1);
      assert( cid>=0 && cid<n_col );
      indexes[cid] = 1;
    }
    if( stmt2 ){
      rc = dbsql_finalize(stmt2);
    }
    if( rc!=DBSQL_OK ){
      goto get_index_array_out;
    }
  }


get_index_array_out:
  if( stmt ){
    int rc2 = dbsql_finalize(stmt);
    if( rc==DBSQL_OK ){
      rc = rc2;
    }
  }
  if( rc!=DBSQL_OK ){
    dbsql_free(indexes);
    indexes = 0;
  }
  *pa_index = indexes;
  return rc;
}

/*
** Global Tcl variable $echo_module is a list. This routine appends
** the string element arg to that list in interpreter interp.
*/
static void __append_to_echo_module(Tcl_Interp *interp, const char *arg){
  int flags = (TCL_APPEND_VALUE | TCL_LIST_ELEMENT | TCL_GLOBAL_ONLY);
  Tcl_SetVar(interp, "echo_module", (arg?arg:""), flags);
}

/*
** This function is called from within the echo-modules create and
** connect methods. The argc and argv arguments are copies of those 
** passed to the calling method. This function is responsible for
** calling dbsql_declare_vtab() to declare the schema of the virtual
** table being created or connected.
**
** If the constructor was passed just one argument, i.e.:
**
**   CREATE TABLE t1 AS echo(t2);
**
** Then t2 is assumed to be the name of a *real* database table. The
** schema of the virtual table is declared by passing a copy of the 
** CREATE TABLE statement for the real table to dbsql_declare_vtab().
** Hence, the virtual table should have exactly the same column names and 
** types as the real table.
*/
static int __echo_declare_vtab(
  echo_vtab *vtab, 
  DBSQL *db 
){
  int rc = DBSQL_OK;

  if( vtab->table_name ){
    dbsql_stmt_t *stmt = 0;
    rc = dbsql_prepare(db, 
        "SELECT sql FROM dbsql_schema WHERE type = 'table' AND name = ?",
        -1, &stmt, 0);
    if( rc==DBSQL_OK ){
      dbsql_bind_text(stmt, 1, vtab->table_name, -1, 0);
      if( dbsql_step(stmt)==DBSQL_ROW ){
        int rc2;
        const char *create_table = (const char *)dbsql_column_text(stmt, 0);
        rc = dbsql_declare_vtab(db, create_table);
        rc2 = dbsql_finalize(stmt);
        if( rc==DBSQL_OK ){
          rc = rc2;
        }
      } else {
        rc = dbsql_finalize(stmt);
        if( rc==DBSQL_OK ){ 
          rc = DBSQL_ERROR;
        }
      }
      if( rc==DBSQL_OK ){
        rc = __get_column_names(db, vtab->table_name, &vtab->cols, &vtab->n_col);
      }
      if( rc==DBSQL_OK ){
        rc = __get_index_array(db, vtab->table_name, vtab->n_col, &vtab->indexes);
      }
    }
  }

  return rc;
}

/*
** This function frees all runtime structures associated with the virtual
** table vtab.
*/
static int __echo_destructor(dbsql_vtab_t *vtab){
  echo_vtab *p = (echo_vtab*)vtab;
  dbsql_free(p->indexes);
  dbsql_free(p->cols);
  dbsql_free(p->z_this);
  dbsql_free(p->table_name);
  dbsql_free(p->log_name);
  dbsql_free(p);
  return 0;
}

typedef struct echo_module_t echo_module_t;
struct echo_module_t {
  Tcl_Interp *interp;
  DBSQL *db;
};

/*
** This function is called to do the work of the connect() method -
** to allocate the required in-memory structures for a newly connected
** virtual table.
*/
static int __echo_constructor(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **pp_vtab,
  char **err_p
){
  int rc;
  int i;
  echo_vtab *vtab;

  /* Allocate the dbsql_vtab_t/echo_vtab structure itself */
  vtab = __malloc_zero( sizeof(*vtab) );
  if( !vtab ){
    return DBSQL_NOMEM;
  }
  vtab->interp = ((echo_module_t *)aux)->interp;
  vtab->db = db;

  /* Allocate echo_vtab.z_this */
  vtab->z_this = dbsql_mprintf("%s", argv[2]);
  if( !vtab->z_this ){
    __echo_destructor((dbsql_vtab_t *)vtab);
    return DBSQL_NOMEM;
  }

  /* Allocate echo_vtab.table_name */
  if( argc>3 ){
    vtab->table_name = dbsql_mprintf("%s", argv[3]);
    __dequote_string(vtab->table_name);
    if( vtab->table_name && vtab->table_name[0]=='*' ){
      char *z = dbsql_mprintf("%s%s", argv[2], &(vtab->table_name[1]));
      dbsql_free(vtab->table_name);
      vtab->table_name = z;
      vtab->is_pattern = 1;
    }
    if( !vtab->table_name ){
      __echo_destructor((dbsql_vtab_t *)vtab);
      return DBSQL_NOMEM;
    }
  }

  /* Log the arguments to this function to Tcl var ::echo_module */
  for(i=0; i<argc; i++){
    __append_to_echo_module(vtab->interp, argv[i]);
  }

  /* Invoke dbsql_declare_vtab and set up other members of the echo_vtab
  ** structure. If an error occurs, delete the dbsql_vtab_t structure and
  ** return an error code.
  */
  rc = __echo_declare_vtab(vtab, db);
  if( rc!=DBSQL_OK ){
    __echo_destructor((dbsql_vtab_t *)vtab);
    return rc;
  }

  /* Success. Set *vtab and return */
  *pp_vtab = &vtab->base;
  return DBSQL_OK;
}

/* 
** Echo virtual table module create method.
*/
static int __echo_create(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **pp_vtab,
  char **err_p
){
  int rc = DBSQL_OK;
  __append_to_echo_module(((echo_module_t *)aux)->interp, "xCreate");
  rc = __echo_constructor(db, aux, argc, argv, pp_vtab, err_p);

  /* If there were two arguments passed to the module at the SQL level 
  ** (i.e. "CREATE VIRTUAL TABLE tbl USING echo(arg1, arg2)"), then 
  ** the second argument is used as a table name. Attempt to create
  ** such a table with a single column, "logmsg". This table will
  ** be used to log calls to the update method. It will be deleted
  ** when the virtual table is DROPed.
  **
  ** Note: The main point of this is to test that we can drop tables
  ** from within an destroy method call.
  */
  if( rc==DBSQL_OK && argc==5 ){
    char *sql;
    echo_vtab *vtab = *(echo_vtab **)pp_vtab;
    vtab->log_name = dbsql_mprintf("%s", argv[4]);
    sql = dbsql_mprintf("CREATE TABLE %Q(logmsg)", vtab->log_name);
    rc = dbsql_exec(db, sql, 0, 0, 0);
    dbsql_free(sql);
    if( rc!=DBSQL_OK ){
      *err_p = dbsql_mprintf("%s", dbsql_errmsg(db));
    }
  }

  if( *pp_vtab && rc!=DBSQL_OK ){
    __echo_destructor(*pp_vtab);
    *pp_vtab = 0;
  }

  if( rc==DBSQL_OK ){
    (*(echo_vtab**)pp_vtab)->in_transaction = 1;
  }

  return rc;
}

/* 
** Echo virtual table module connect method.
*/
static int __echo_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  __append_to_echo_module(((echo_module_t *)aux)->interp, "xConnect");
  return __echo_constructor(db, aux, argc, argv, vtab, err_p);
}

/* 
** Echo virtual table module disconnect method.
*/
static int __echo_disconnect(dbsql_vtab_t *vtab){
  __append_to_echo_module(((echo_vtab *)vtab)->interp, "xDisconnect");
  return __echo_destructor(vtab);
}

/* 
** Echo virtual table module destroy method.
*/
static int __echo_destroy(dbsql_vtab_t *vtab){
  int rc = DBSQL_OK;
  echo_vtab *p = (echo_vtab *)vtab;
  __append_to_echo_module(((echo_vtab *)vtab)->interp, "xDestroy");

  /* Drop the "log" table, if one exists (see __echo_create() for details) */
  if( p && p->log_name ){
    char *sql;
    sql = dbsql_mprintf("DROP TABLE %Q", p->log_name);
    rc = dbsql_exec(p->db, sql, 0, 0, 0);
    dbsql_free(sql);
  }

  if( rc==DBSQL_OK ){
    rc = __echo_destructor(vtab);
  }
  return rc;
}

/* 
** Echo virtual table module x_open method.
*/
static int __echo_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  echo_cursor *cur;
  if( __simulate_vtab_error((echo_vtab *)v_tab, "xOpen") ){
    return DBSQL_ERROR;
  }
  cur = __malloc_zero(sizeof(echo_cursor));
  *cursor = (dbsql_vtab_cursor_t *)cur;
  return (cur ? DBSQL_OK : DBSQL_NOMEM);
}

/* 
** Echo virtual table module x_close method.
*/
static int __echo_close(dbsql_vtab_cursor_t *cur){
  int rc;
  echo_cursor *p_cur = (echo_cursor *)cur;
  dbsql_stmt_t *stmt = p_cur->stmt;
  p_cur->stmt = 0;
  dbsql_free(p_cur);
  rc = dbsql_finalize(stmt);
  return rc;
}

/*
** Return non-zero if the cursor does not currently point to a valid record
** (i.e if the scan has finished), or zero otherwise.
*/
static int __echo_eof(dbsql_vtab_cursor_t *cur){
  return (((echo_cursor *)cur)->stmt ? 0 : 1);
}

/* 
** Echo virtual table module next method.
*/
static int __echo_next(dbsql_vtab_cursor_t *cur){
  int rc = DBSQL_OK;
  echo_cursor *p_cur = (echo_cursor *)cur;

  if( __simulate_vtab_error((echo_vtab *)(cur->vtab), "xNext") ){
    return DBSQL_ERROR;
  }

  if( p_cur->stmt ){
    rc = dbsql_step(p_cur->stmt);
    if( rc==DBSQL_ROW ){
      rc = DBSQL_OK;
    }else{
      rc = dbsql_finalize(p_cur->stmt);
      p_cur->stmt = 0;
    }
  }

  return rc;
}

/* 
** Echo virtual table module column method.
*/
static int __echo_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  int i_col = i + 1;
  dbsql_stmt_t *stmt = ((echo_cursor *)cur)->stmt;

  if( __simulate_vtab_error((echo_vtab *)(cur->vtab), "xColumn") ){
    return DBSQL_ERROR;
  }

  if( !stmt ){
    dbsql_result_null(ctx);
  }else{
    assert( dbsql_data_count(stmt)>i_col );
    dbsql_result_value(ctx, dbsql_column_value(stmt, i_col));
  }
  return DBSQL_OK;
}

/* 
** Echo virtual table module rowid method.
*/
static int __echo_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  dbsql_stmt_t *stmt = ((echo_cursor *)cur)->stmt;

  if( __simulate_vtab_error((echo_vtab *)(cur->vtab), "xRowid") ){
    return DBSQL_ERROR;
  }

  *rowid = dbsql_column_int64(stmt, 0);
  return DBSQL_OK;
}

/*
** Compute a simple hash of the null terminated string string.
**
** This module uses only dbsql_index_info_t.idx_str, not 
** dbsql_index_info_t.idx_num. So to test idx_num, when idx_str is set
** in __echo_best_index(), idx_num is set to the corresponding hash value.
** In __echo_filter(), code assert()s that the supplied idx_num value is
** indeed the hash of the supplied idx_str.
*/
static int __hash_string(const char *string){
  u_int32_t val = 0;
  int ii;
  for(ii=0; string[ii]; ii++){
    val = (val << 3) + (int)string[ii];
  }
  return (int)(val&0x7fffffff);
}

/* 
** Echo virtual table module filter method.
*/
static int __echo_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  int rc;
  int i;

  echo_cursor *cur = (echo_cursor *)vtab_cursor;
  echo_vtab *vtab = (echo_vtab *)vtab_cursor->vtab;
  DBSQL *db = vtab->db;

  if( __simulate_vtab_error(vtab, "xFilter") ){
    return DBSQL_ERROR;
  }

  /* Check that idx_num matches idx_str */
  assert( idx_num==__hash_string(idx_str) );

  /* Log arguments to the ::echo_module Tcl variable */
  __append_to_echo_module(vtab->interp, "xFilter");
  __append_to_echo_module(vtab->interp, idx_str);
  for(i=0; i<argc; i++){
    __append_to_echo_module(vtab->interp, (const char*)dbsql_value_text(argv[i]));
  }

  dbsql_finalize(cur->stmt);
  cur->stmt = 0;

  /* Prepare the SQL statement created by __echo_best_index and bind the
  ** runtime parameters passed to this function to it.
  */
  rc = dbsql_prepare(db, idx_str, -1, &cur->stmt, 0);
  assert( cur->stmt || rc!=DBSQL_OK );
  for(i=0; rc==DBSQL_OK && i<argc; i++){
    rc = dbsql_bind_value(cur->stmt, i+1, argv[i]);
  }

  /* If everything was successful, advance to the first row of the scan */
  if( rc==DBSQL_OK ){
    rc = __echo_next(vtab_cursor);
  }

  return rc;
}


/*
** A helper function used by __echo_update() and __echo_best_index() for
** manipulating strings in concert with the dbsql_mprintf() function.
**
** Parameter str points to a pointer to a string allocated with
** dbsql_mprintf. The second parameter, append, points to another
** string. The two strings are concatenated together and *str
** set to point at the result. The initial buffer pointed to by *str
** is deallocated via dbsql_free().
**
** If the third argument, do_free, is true, then dbsql_free() is
** also called to free the buffer pointed to by append.
*/
static void string_concat(char **str, char *append, int do_free, int *rc){
  char *in = *str;
  if( !append && do_free && *rc==DBSQL_OK ){
    *rc = DBSQL_NOMEM;
  }
  if( *rc!=DBSQL_OK ){
    dbsql_free(in);
    in = 0;
  }else{
    if( in ){
      char *temp = in;
      in = dbsql_mprintf("%s%s", in, append);
      dbsql_free(temp);
    }else{
      in = dbsql_mprintf("%s", append);
    }
    if( !in ){
      *rc = DBSQL_NOMEM;
    }
  }
  *str = in;
  if( do_free ){
    dbsql_free(append);
  }
}

/*
** This function returns a pointer to an dbsql_malloc()ed buffer 
** containing the select-list (the thing between keywords SELECT and FROM)
** to query the underlying real table with for the scan described by
** argument idx_info.
**
** If the current dbsql version is earlier than 3.10.0, this is just "*"
** (select all columns). Or, for version 3.10.0 and greater, the list of
** columns identified by the idx_info->col_used mask.
*/
static char *__echo_select_list(echo_vtab *tab, dbsql_index_info_t *idx_info){
  char *ret = 0;
  if( dbsql_libversion_number()<3010000 ){
    ret = dbsql_mprintf(", *");
  }else{
    int i;
    for(i=0; i<tab->n_col; i++){
      if( idx_info->col_used & ((u_int64_t)1 << (i>=63 ? 63 : i)) ){
        ret = dbsql_mprintf("%z, %s", ret, tab->cols[i]);
      }else{
        ret = dbsql_mprintf("%z, NULL", ret);
      }
      if( !ret ) break;
    }
  }
  return ret;
}

/*
** The echo module implements the subset of query constraints and sort
** orders that may take advantage of dbsql indices on the underlying
** real table. For example, if the real table is declared as:
**
**     CREATE TABLE real(a, b, c);
**     CREATE INDEX real_index ON real(b);
**
** then the echo module handles WHERE or ORDER BY clauses that refer
** to the column "b", but not "a" or "c". If a multi-column index is
** present, only its left most column is considered. 
**
** This best_index method encodes the proposed search strategy as
** an SQL query on the real table underlying the virtual echo module 
** table and stores the query in dbsql_index_info_t.idx_str. The SQL
** statement is of the form:
**
**   SELECT rowid, * FROM <real-table> ?<where-clause>? ?<order-by-clause>?
**
** where the <where-clause> and <order-by-clause> are determined
** by the contents of the structure pointed to by the idx_info argument.
*/
static int __echo_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  int ii;
  char *query = 0;
  char *col = 0;
  char *z_new;
  int n_arg = 0;
  const char *sep = "WHERE";
  echo_vtab *vtab = (echo_vtab *)tab;
  dbsql_stmt_t *stmt = 0;
  Tcl_Interp *interp = vtab->interp;

  int n_row = 0;
  int use_idx = 0;
  int rc = DBSQL_OK;
  int use_cost = 0;
  double cost = 0;
  int is_ignore_usable = 0;
  if( Tcl_GetVar(interp, "echo_module_ignore_usable", TCL_GLOBAL_ONLY) ){
    is_ignore_usable = 1;
  }

  if( __simulate_vtab_error(vtab, "xBestIndex") ){
    return DBSQL_ERROR;
  }

  /* Determine the number of rows in the table and store this value in local
  ** variable n_row. The 'estimated-cost' of the scan will be the number of
  ** rows in the table for a linear scan, or the log (base 2) of the 
  ** number of rows if the proposed scan uses an index.  
  */
  if( Tcl_GetVar(interp, "echo_module_cost", TCL_GLOBAL_ONLY) ){
    cost = atof(Tcl_GetVar(interp, "echo_module_cost", TCL_GLOBAL_ONLY));
    use_cost = 1;
  } else {
    query = dbsql_mprintf("SELECT count(*) FROM %Q", vtab->table_name);
    if( !query ){
      return DBSQL_NOMEM;
    }
    rc = dbsql_prepare(vtab->db, query, -1, &stmt, 0);
    dbsql_free(query);
    if( rc!=DBSQL_OK ){
      return rc;
    }
    dbsql_step(stmt);
    n_row = dbsql_column_int(stmt, 0);
    rc = dbsql_finalize(stmt);
    if( rc!=DBSQL_OK ){
      return rc;
    }
  }

  col = __echo_select_list(vtab, idx_info);
  if( !col ) return DBSQL_NOMEM;
  query = dbsql_mprintf("SELECT rowid%z FROM %Q", col, vtab->table_name);
  if( !query ) return DBSQL_NOMEM;

  for(ii=0; ii<idx_info->n_constraint; ii++){
    const struct dbsql_index_constraint_t *constraint;
    struct dbsql_index_constraint_usage_t *usage;
    int i_col;

    constraint = &idx_info->constraints[ii];
    usage = &idx_info->constraint_usages[ii];

    if( !is_ignore_usable && !constraint->usable ) continue;

    i_col = constraint->i_column;
    if( i_col<0 || vtab->indexes[i_col] ){
      char *new_col = i_col>=0 ? vtab->cols[i_col] : "rowid";
      char *op = 0;
      use_idx = 1;
      switch( constraint->op ){
        case DBSQL_INDEX_CONSTRAINT_EQ:
          op = "="; break;
        case DBSQL_INDEX_CONSTRAINT_LT:
          op = "<"; break;
        case DBSQL_INDEX_CONSTRAINT_GT:
          op = ">"; break;
        case DBSQL_INDEX_CONSTRAINT_LE:
          op = "<="; break;
        case DBSQL_INDEX_CONSTRAINT_GE:
          op = ">="; break;
        case DBSQL_INDEX_CONSTRAINT_MATCH:
          /* Purposely translate the MATCH operator into a LIKE, which
          ** will be used by the next block of code to construct a new
          ** query.  It should also be noted here that the next block
          ** of code requires the first letter of this operator to be
          ** in upper-case to trigger the special MATCH handling (i.e.
          ** wrapping the bound parameter with literal '%'s).
          */
          op = "LIKE"; break;
        case DBSQL_INDEX_CONSTRAINT_LIKE:
          op = "like"; break;
        case DBSQL_INDEX_CONSTRAINT_GLOB:
          op = "glob"; break;
        case DBSQL_INDEX_CONSTRAINT_REGEXP:
          op = "regexp"; break;
      }
      if( op ){
        if( op[0]=='L' ){
          z_new = dbsql_mprintf(" %s %s LIKE (SELECT '%%'||?||'%%')", 
              sep, new_col);
        } else {
          z_new = dbsql_mprintf(" %s %s %s ?", sep, new_col, op);
        }
        string_concat(&query, z_new, 1, &rc);
        sep = "AND";
        usage->argv_index = ++n_arg;
        usage->omit = 1;
      }
    }
  }

  /* If there is only one term in the ORDER BY clause, and it is
  ** on a column that this virtual table has an index for, then consume 
  ** the ORDER BY clause.
  */
  if( idx_info->n_order_by==1 && (
        idx_info->order_bies->i_column<0 ||
        vtab->indexes[idx_info->order_bies->i_column]) ){
    int i_col = idx_info->order_bies->i_column;
    char *new_col = i_col>=0 ? vtab->cols[i_col] : "rowid";
    char *dir = idx_info->order_bies->desc?"DESC":"ASC";
    z_new = dbsql_mprintf(" ORDER BY %s %s", new_col, dir);
    string_concat(&query, z_new, 1, &rc);
    idx_info->order_by_consumed = 1;
  }

  __append_to_echo_module(vtab->interp, "xBestIndex");;
  __append_to_echo_module(vtab->interp, query);

  if( !query ){
    return rc;
  }
  idx_info->idx_num = __hash_string(query);
  idx_info->idx_str = query;
  idx_info->need_to_free_idx_str = 1;
  if( use_cost ){
    idx_info->estimated_cost = cost;
  }else if( use_idx ){
    /* Approximation of log2(n_row). */
    for( ii=0; ii<(sizeof(int)*8)-1; ii++ ){
      if( n_row & (1<<ii) ){
        idx_info->estimated_cost = (double)ii;
      }
    }
  }else{
    idx_info->estimated_cost = (double)n_row;
  }
  return rc;
}

/*
** The update method for echo module virtual tables.
** 
**    data[0]  data[1]  data[2..]
**
**    INTEGER                              DELETE            
**
**    INTEGER    NULL       (n_col args)    UPDATE (do not set rowid)
**    INTEGER    INTEGER    (n_col args)    UPDATE (with SET rowid = <arg1>)
**
**    NULL       NULL       (n_col args)    INSERT INTO (automatic rowid value)
**    NULL       INTEGER    (n_col args)    INSERT (incl. rowid value)
**
*/
int __echo_update(
  dbsql_vtab_t *tab, 
  int n_data, 
  dbsql_value_t **data, 
  int64_t *rowid
){
  echo_vtab *vtab = (echo_vtab *)tab;
  DBSQL *db = vtab->db;
  int rc = DBSQL_OK;

  dbsql_stmt_t *stmt = 0;
  char *z = 0;               /* SQL statement to execute */
  int bind_arg_zero = 0;       /* True to bind data[0] to sql var no. n_data */
  int bind_arg_one = 0;        /* True to bind data[1] to sql var no. 1 */
  int i;                     /* Counter variable used by for loops */

  assert( n_data==vtab->n_col+2 || n_data==1 );

  /* Ticket #3083 - make sure we always start a transaction prior to
  ** making any changes to a virtual table */
  assert( vtab->in_transaction );

  if( __simulate_vtab_error(vtab, "xUpdate") ){
    return DBSQL_ERROR;
  }

  /* If data[0] is an integer and n_data>1 then do an UPDATE */
  if( n_data>1 && dbsql_value_type(data[0])==DBSQL_INTEGER ){
    char *sep = " SET";
    z = dbsql_mprintf("UPDATE %Q", vtab->table_name);
    if( !z ){
      rc = DBSQL_NOMEM;
    }

    bind_arg_one = (data[1] && dbsql_value_type(data[1])==DBSQL_INTEGER);
    bind_arg_zero = 1;

    if( bind_arg_one ){
       string_concat(&z, " SET rowid=?1 ", 0, &rc);
       sep = ",";
    }
    for(i=2; i<n_data; i++){
      if( data[i]==0 ) continue;
      string_concat(&z, dbsql_mprintf(
          "%s %Q=?%d", sep, vtab->cols[i-2], i), 1, &rc);
      sep = ",";
    }
    string_concat(&z, dbsql_mprintf(" WHERE rowid=?%d", n_data), 1, &rc);
  }

  /* If data[0] is an integer and n_data==1 then do a DELETE */
  else if( n_data==1 && dbsql_value_type(data[0])==DBSQL_INTEGER ){
    z = dbsql_mprintf("DELETE FROM %Q WHERE rowid = ?1", vtab->table_name);
    if( !z ){
      rc = DBSQL_NOMEM;
    }
    bind_arg_zero = 1;
  }

  /* If the first argument is NULL and there are more than two args, INSERT */
  else if( n_data>2 && dbsql_value_type(data[0])==DBSQL_NULL ){
    int ii;
    char *insert = 0;
    char *values = 0;
  
    insert = dbsql_mprintf("INSERT INTO %Q (", vtab->table_name);
    if( !insert ){
      rc = DBSQL_NOMEM;
    }
    if( dbsql_value_type(data[1])==DBSQL_INTEGER ){
      bind_arg_one = 1;
      values = dbsql_mprintf("?");
      string_concat(&insert, "rowid", 0, &rc);
    }

    assert((vtab->n_col+2)==n_data);
    for(ii=2; ii<n_data; ii++){
      string_concat(&insert, 
          dbsql_mprintf("%s%Q", values?", ":"", vtab->cols[ii-2]), 1, &rc);
      string_concat(&values, 
          dbsql_mprintf("%s?%d", values?", ":"", ii), 1, &rc);
    }

    string_concat(&z, insert, 1, &rc);
    string_concat(&z, ") VALUES(", 0, &rc);
    string_concat(&z, values, 1, &rc);
    string_concat(&z, ")", 0, &rc);
  }

  /* Anything else is an error */
  else{
    assert(0);
    return DBSQL_ERROR;
  }

  if( rc==DBSQL_OK ){
    rc = dbsql_prepare(db, z, -1, &stmt, 0);
  }
  assert( rc!=DBSQL_OK || stmt );
  dbsql_free(z);
  if( rc==DBSQL_OK ) {
    if( bind_arg_zero ){
      dbsql_bind_value(stmt, n_data, data[0]);
    }
    if( bind_arg_one ){
      dbsql_bind_value(stmt, 1, data[1]);
    }
    for(i=2; i<n_data && rc==DBSQL_OK; i++){
      if( data[i] ) rc = dbsql_bind_value(stmt, i, data[i]);
    }
    if( rc==DBSQL_OK ){
      dbsql_step(stmt);
      rc = dbsql_finalize(stmt);
    }else{
      dbsql_finalize(stmt);
    }
  }

  if( rowid && rc==DBSQL_OK ){
    *rowid = dbsql_last_insert_rowid(db);
  }
  if( rc!=DBSQL_OK ){
    tab->errmsg = dbsql_mprintf("echo-vtab-error: %s", dbsql_errmsg(db));
  }

  return rc;
}

/*
** begin, x_sync, commit and rollback callbacks for echo module
** virtual tables. Do nothing other than add the name of the callback
** to the $::echo_module Tcl variable.
*/
static int __echo_transaction_call(dbsql_vtab_t *tab, const char *call){
  char *z;
  echo_vtab *vtab = (echo_vtab *)tab;
  z = dbsql_mprintf("echo(%s)", vtab->table_name);
  if( z==0 ) return DBSQL_NOMEM;
  __append_to_echo_module(vtab->interp, call);
  __append_to_echo_module(vtab->interp, z);
  dbsql_free(z);
  return DBSQL_OK;
}
static int __echo_begin(dbsql_vtab_t *tab){
  int rc;
  echo_vtab *vtab = (echo_vtab *)tab;
  Tcl_Interp *interp = vtab->interp;
  const char *val; 

  /* Ticket #3083 - do not start a transaction if we are already in
  ** a transaction */
  assert( !vtab->in_transaction );

  if( __simulate_vtab_error(vtab, "xBegin") ){
    return DBSQL_ERROR;
  }

  rc = __echo_transaction_call(tab, "xBegin");

  if( rc==DBSQL_OK ){
    /* Check if the $::echo_module_begin_fail variable is defined. If it is,
    ** and it is set to the name of the real table underlying this virtual
    ** echo module table, then cause this x_sync operation to fail.
    */
    val = Tcl_GetVar(interp, "echo_module_begin_fail", TCL_GLOBAL_ONLY);
    if( val && 0==strcmp(val, vtab->table_name) ){
      rc = DBSQL_ERROR;
    }
  }
  if( rc==DBSQL_OK ){
    vtab->in_transaction = 1;
  }
  return rc;
}
static int __echo_sync(dbsql_vtab_t *tab){
  int rc;
  echo_vtab *vtab = (echo_vtab *)tab;
  Tcl_Interp *interp = vtab->interp;
  const char *val; 

  /* Ticket #3083 - Only call x_sync if we have previously started a
  ** transaction */
  assert( vtab->in_transaction );

  if( __simulate_vtab_error(vtab, "xSync") ){
    return DBSQL_ERROR;
  }

  rc = __echo_transaction_call(tab, "xSync");

  if( rc==DBSQL_OK ){
    /* Check if the $::echo_module_sync_fail variable is defined. If it is,
    ** and it is set to the name of the real table underlying this virtual
    ** echo module table, then cause this x_sync operation to fail.
    */
    val = Tcl_GetVar(interp, "echo_module_sync_fail", TCL_GLOBAL_ONLY);
    if( val && 0==strcmp(val, vtab->table_name) ){
      rc = -1;
    }
  }
  return rc;
}
static int __echo_commit(dbsql_vtab_t *tab){
  echo_vtab *vtab = (echo_vtab*)tab;
  int rc;

  /* Ticket #3083 - Only call commit if we have previously started
  ** a transaction */
  assert( vtab->in_transaction );

  if( __simulate_vtab_error(vtab, "xCommit") ){
    return DBSQL_ERROR;
  }

  __begin_benign_malloc();
  rc = __echo_transaction_call(tab, "xCommit");
  __end_benign_malloc();
  vtab->in_transaction = 0;
  return rc;
}
static int __echo_rollback(dbsql_vtab_t *tab){
  int rc;
  echo_vtab *vtab = (echo_vtab*)tab;

  /* Ticket #3083 - Only call rollback if we have previously started
  ** a transaction */
  assert( vtab->in_transaction );

  rc = __echo_transaction_call(tab, "xRollback");
  vtab->in_transaction = 0;
  return rc;
}

/*
** Implementation of "GLOB" function on the echo module.  Pass
** all arguments to the ::echo_glob_overload procedure of TCL
** and return the result of that procedure as a string.
*/
static void __overloaded_glob_function(
  dbsql_func_t *context,
  int n_arg,
  dbsql_value_t **args
){
  Tcl_Interp *interp = dbsql_user_data(context);
  Tcl_DString str;
  int i;
  int rc;
  Tcl_DStringInit(&str);
  Tcl_DStringAppendElement(&str, "::echo_glob_overload");
  for(i=0; i<n_arg; i++){
    Tcl_DStringAppendElement(&str, (char*)dbsql_value_text(args[i]));
  }
  rc = Tcl_Eval(interp, Tcl_DStringValue(&str));
  Tcl_DStringFree(&str);
  if( rc ){
    dbsql_result_error(context, Tcl_GetStringResult(interp), -1);
  }else{
    dbsql_result_text(context, Tcl_GetStringResult(interp),
                        -1, DBSQL_TRANSIENT);
  }
  Tcl_ResetResult(interp);
}

/*
** This is the find_function implementation for the echo module.
** dbsql calls this routine when the first argument of a function
** is a column of an echo virtual table.  This routine can optionally
** override the implementation of that function.  It will choose to
** do so if the function is named "glob", and a TCL command named
** ::echo_glob_overload exists.
*/
static int __echo_find_function(
  dbsql_vtab_t *vtab,
  int n_arg,
  const char *func_name,
  void (**px_func)(dbsql_func_t*,int,dbsql_value_t**),
  void **arg
){
  echo_vtab *p_vtab = (echo_vtab *)vtab;
  Tcl_Interp *interp = p_vtab->interp;
  Tcl_CmdInfo info;
  if( strcmp(func_name,"glob")!=0 ){
    return 0;
  }
  if( Tcl_GetCommandInfo(interp, "::echo_glob_overload", &info)==0 ){
    return 0;
  }
  *px_func = __overloaded_glob_function;
  *arg = interp;
  return 1;
}

static int __echo_rename(dbsql_vtab_t *vtab, const char *new_name){
  int rc = DBSQL_OK;
  echo_vtab *p = (echo_vtab *)vtab;

  if( __simulate_vtab_error(p, "xRename") ){
    return DBSQL_ERROR;
  }

  if( p->is_pattern ){
    int n_this = (int)strlen(p->z_this);
    char *sql = dbsql_mprintf("ALTER TABLE %s RENAME TO %s%s", 
        p->table_name, new_name, &p->table_name[n_this]
    );
    rc = dbsql_exec(p->db, sql, 0, 0, 0);
    dbsql_free(sql);
  }

  return rc;
}

static int __echo_savepoint(dbsql_vtab_t *v_tab, int i_savepoint){
  assert( v_tab );
  return DBSQL_OK;
}

static int __echo_release(dbsql_vtab_t *v_tab, int i_savepoint){
  assert( v_tab );
  return DBSQL_OK;
}

static int __echo_rollback_to(dbsql_vtab_t *v_tab, int i_savepoint){
  assert( v_tab );
  return DBSQL_OK;
}

/*
** A virtual table module that merely "echos" the contents of another
** table (like an SQL VIEW).
*/
static dbsql_module_t __echo_module = {
  1,                         /* i_version */
  __echo_create,
  __echo_connect,
  __echo_best_index,
  __echo_disconnect, 
  __echo_destroy,
  __echo_open,                  /* x_open - open a cursor */
  __echo_close,                 /* x_close - close a cursor */
  __echo_filter,                /* filter - configure scan constraints */
  __echo_next,                  /* next - advance a cursor */
  __echo_eof,                   /* eof */
  __echo_column,                /* column - read data */
  __echo_rowid,                 /* rowid - read data */
  __echo_update,                /* update - write data */
  __echo_begin,                 /* begin - begin transaction */
  __echo_sync,                  /* x_sync - sync transaction */
  __echo_commit,                /* commit - commit transaction */
  __echo_rollback,              /* rollback - rollback transaction */
  __echo_find_function,          /* find_function - function overloading */
  __echo_rename,                /* rename - rename the table */
  0,                         /* savepoint */
  0,                         /* release */
  0,                         /* rollback_to */
  0,                         /* shadow_name */
  0                          /* integrity */
};

static dbsql_module_t __echo_module_v2 = {
  2,                         /* i_version */
  __echo_create,
  __echo_connect,
  __echo_best_index,
  __echo_disconnect, 
  __echo_destroy,
  __echo_open,                  /* x_open - open a cursor */
  __echo_close,                 /* x_close - close a cursor */
  __echo_filter,                /* filter - configure scan constraints */
  __echo_next,                  /* next - advance a cursor */
  __echo_eof,                   /* eof */
  __echo_column,                /* column - read data */
  __echo_rowid,                 /* rowid - read data */
  __echo_update,                /* update - write data */
  __echo_begin,                 /* begin - begin transaction */
  __echo_sync,                  /* x_sync - sync transaction */
  __echo_commit,                /* commit - commit transaction */
  __echo_rollback,              /* rollback - rollback transaction */
  __echo_find_function,          /* find_function - function overloading */
  __echo_rename,                /* rename - rename the table */
  __echo_savepoint,
  __echo_release,
  __echo_rollback_to,
  0,                         /* shadow_name */
  0                          /* integrity  */
};

/*
** Decode a pointer to an DBSQL object.
*/
extern int __get_db_pointer(Tcl_Interp *interp, const char *a, DBSQL **db);
extern const char *__err_name(int);

static void __module_destroy(void *p){
  echo_module_t *mod = (echo_module_t*)p;
  dbsql_create_function(mod->db, "function_that_does_not_exist_0982ma98",
                          DBSQL_ANY, 1, 0, 0, 0, 0);
  dbsql_free(p);
}

/*
** Register the echo virtual table module.
*/
static int DBSQL_TCLAPI register_echo_module(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  int rc;
  DBSQL *db;
  echo_module_t *mod;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;

  /* Virtual table module "echo" */
  mod = dbsql_malloc(sizeof(echo_module_t));
  mod->interp = interp;
  mod->db = db;
  rc = dbsql_create_module_v2(
      db, "echo", &__echo_module, (void*)mod, __module_destroy
  );

  /* Virtual table module "echo_v2" */
  if( rc==DBSQL_OK ){
    mod = dbsql_malloc(sizeof(echo_module_t));
    mod->interp = interp;
    mod->db = db;
    rc = dbsql_create_module_v2(db, "echo_v2", 
        &__echo_module_v2, (void*)mod, __module_destroy
    );
  }

  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);
  return TCL_OK;
}

/*
** Tcl interface to dbsql_declare_vtab, invoked as follows from Tcl:
**
** dbsql_declare_vtab DB SQL
*/
static int DBSQL_TCLAPI declare_vtab(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  DBSQL *db;
  int rc;
  if( objc!=3 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB SQL");
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  rc = dbsql_declare_vtab(db, Tcl_GetString(objv[2]));
  if( rc!=DBSQL_OK ){
    Tcl_SetResult(interp, (char *)dbsql_errmsg(db), TCL_VOLATILE);
    return TCL_ERROR;
  }
  return TCL_OK;
}

#endif /* ifndef DBSQL_OMIT_VIRTUALTABLE */

/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest8_Init(Tcl_Interp *interp){
#ifndef DBSQL_OMIT_VIRTUALTABLE
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
     void *clientData;
  } obj_cmds[] = {
     { "register_echo_module",       register_echo_module, 0 },
     { "dbsql_declare_vtab",       declare_vtab, 0 },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, 
        obj_cmds[i].proc, obj_cmds[i].clientData, 0);
  }
#endif
  return TCL_OK;
}
