/*
** 2003 January 11
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to implement the dbsql_set_authorizer()
** API.  This facility is an optional feature of the library.  Embedded
** systems that do not need this facility may omit it by recompiling
** the library with -DSQLITE_OMIT_AUTHORIZATION=1
*/
#include "dbsql_int.h"

/*
** All of the code in this file may be omitted by defining a single
** macro.
*/
#ifndef DBSQL_OMIT_AUTHORIZATION

/*
** Set or clear the access authorization function.
**
** The access authorization function is be called during the compilation
** phase to verify that the user has read and/or write access permission on
** various fields of the database.  The first argument to the auth function
** is a copy of the 3rd argument to this routine.  The second argument
** to the auth function is one of these constants:
**
**       DBSQL_CREATE_INDEX
**       DBSQL_CREATE_TABLE
**       DBSQL_CREATE_TEMP_INDEX
**       DBSQL_CREATE_TEMP_TABLE
**       DBSQL_CREATE_TEMP_TRIGGER
**       DBSQL_CREATE_TEMP_VIEW
**       DBSQL_CREATE_TRIGGER
**       DBSQL_CREATE_VIEW
**       DBSQL_DELETE
**       DBSQL_DROP_INDEX
**       DBSQL_DROP_TABLE
**       DBSQL_DROP_TEMP_INDEX
**       DBSQL_DROP_TEMP_TABLE
**       DBSQL_DROP_TEMP_TRIGGER
**       DBSQL_DROP_TEMP_VIEW
**       DBSQL_DROP_TRIGGER
**       DBSQL_DROP_VIEW
**       DBSQL_INSERT
**       DBSQL_PRAGMA
**       DBSQL_READ
**       DBSQL_SELECT
**       DBSQL_TRANSACTION
**       DBSQL_UPDATE
**
** The third and fourth arguments to the auth function are the name of
** the table and the column that are being accessed.  The auth function
** should return either DBSQL_OK, DBSQL_DENY, or DBSQL_IGNORE.  If
** DBSQL_OK is returned, it means that access is allowed.  DBSQL_DENY
** means that the SQL statement will never-run - the dbsql_exec() call
** will return with an error.  DBSQL_IGNORE means that the SQL statement
** should run but attempts to read the specified column will return NULL
** and attempts to write the column will be ignored.
**
** Setting the auth function to NULL disables this hook.  The default
** setting of the auth function is NULL.
*/
int dbsql_set_authorizer(
  DBSQL *db,
  int (*auth)(void*,int,const char*,const char*,const char*,const char*),
  void *arg
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  db->auth = (dbsql_xauth)auth;
  db->auth_arg = arg;
  __expire_prepared_statements(db, 1);
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}

/*
** Write an error message into parser->errmsg that explains that the
** user-supplied authorization function returned an illegal value.
*/
static void dbsql_auth_bad_return_code(parser_t *parser){
  __error_msg(parser, "authorizer malfunction");
  parser->rc = DBSQL_ERROR;
}

/*
** Invoke the authorization callback for permission to read column col from
** table tab in database db. This function assumes that an authorization
** callback has been registered (i.e. that DBSQL.auth is not NULL).
**
** If DBSQL_IGNORE is returned and expr is not NULL, then expr is changed
** to an SQL NULL expression. Otherwise, if expr is NULL, then DBSQL_IGNORE
** is treated as DBSQL_DENY. In this case an error is left in parser.
*/
int __auth_read_col(
  parser_t *parser,                  /* The parser context */
  const char *tab,               /* table_t name */
  const char *col,               /* column_t name */
  int i_db                         /* index_t of containing database. */
){
  DBSQL *db = parser->db;          /* Database handle */
  char *z_db = db->dbs[i_db].db_s_name; /* schema_t name of attached database */
  int rc;                            /* Auth callback return code */

  if( db->init.busy ) return DBSQL_OK;
  rc = db->auth(db->auth_arg, DBSQL_READ, tab,col,z_db,parser->auth_context);
  if( rc==DBSQL_DENY ){
    char *z = dbsql_mprintf("%s.%s", tab, col);
    if( db->n_db>2 || i_db!=0 ) z = dbsql_mprintf("%s.%z", z_db, z);
    __error_msg(parser, "access to %z is prohibited", z);
    parser->rc = DBSQL_AUTH;
  }else if( rc!=DBSQL_IGNORE && rc!=DBSQL_OK ){
    dbsql_auth_bad_return_code(parser);
  }
  return rc;
}

/*
** The expr should be a TK_COLUMN expression.  The table referred to
** is in tab_list or else it is the NEW or OLD table of a trigger.  
** Check to see if it is OK to read this particular column.
**
** If the auth function returns DBSQL_IGNORE, change the TK_COLUMN 
** instruction into a TK_NULL.  If the auth function returns DBSQL_DENY,
** then generate an error.
*/
void __auth_read(
  parser_t *parser,        /* The parser context */
  expr_t *expr,          /* The expression to check authorization on */
  schema_t *schema,      /* The schema of the expression */
  src_list_t *tab_list     /* All table that expr might refer to */
){
  table_t *tab = 0;      /* The table being read */
  const char *col;     /* Name of the column of the table */
  int i_src;             /* index_t in tab_list->a[] of table being read */
  int i_db;              /* The index of the database the expression refers to */
  int i_col;             /* index_t of column in table */

  assert( expr->op==TK_COLUMN || expr->op==TK_TRIGGER );
  assert( !IN_RENAME_OBJECT );
  assert( parser->db->auth!=0 );
  i_db = __schema_to_index(parser->db, schema);
  if( i_db<0 ){
    /* An attempt to read a column out of a subquery or other
    ** temporary table. */
    return;
  }

  if( expr->op==TK_TRIGGER ){
    tab = parser->trigger_tab;
  }else{
    assert( tab_list );
    for(i_src=0; i_src<tab_list->n_src; i_src++){
      if( expr->i_table==tab_list->a[i_src].i_cursor ){
        tab = tab_list->a[i_src].s_tab;
        break;
      }
    }
  }
  i_col = expr->i_column;
  if( tab==0 ) return;

  if( i_col>=0 ){
    assert( i_col<tab->n_col );
    col = tab->cols[i_col].cn_name;
  }else if( tab->i_p_key>=0 ){
    assert( tab->i_p_key<tab->n_col );
    col = tab->cols[tab->i_p_key].cn_name;
  }else{
    col = "ROWID";
  }
  assert( i_db>=0 && i_db<parser->db->n_db );
  if( DBSQL_IGNORE==__auth_read_col(parser, tab->name, col, i_db) ){
    expr->op = TK_NULL;
  }
}

/*
** Do an authorization check using the code and arguments given.  Return
** either DBSQL_OK (zero) or DBSQL_IGNORE or DBSQL_DENY.  If DBSQL_DENY
** is returned, then the error count and error message in parser are
** modified appropriately.
*/
int __auth_check(
  parser_t *parser,
  int code,
  const char *arg1,
  const char *arg2,
  const char *arg3
){
  DBSQL *db = parser->db;
  int rc;

  /* Don't do any authorization checks if the database is initializing
  ** or if the parser is being invoked from within dbsql_declare_vtab.
  */
  assert( !IN_RENAME_OBJECT || db->auth==0 );
  if( db->auth==0 || db->init.busy || IN_SPECIAL_PARSE ){
    return DBSQL_OK;
  }

  /* EVIDENCE-OF: R-43249-19882 The third through sixth parameters to the
  ** callback are either NULL pointers or zero-terminated strings that
  ** contain additional details about the action to be authorized.
  **
  ** The following testcase() macros show that any of the 3rd through 6th
  ** parameters can be either NULL or a string. */
  testcase( arg1==0 );
  testcase( arg2==0 );
  testcase( arg3==0 );
  testcase( parser->auth_context==0 );

  rc = db->auth(db->auth_arg,code,arg1,arg2,arg3,parser->auth_context);
  if( rc==DBSQL_DENY ){
    __error_msg(parser, "not authorized");
    parser->rc = DBSQL_AUTH;
  }else if( rc!=DBSQL_OK && rc!=DBSQL_IGNORE ){
    rc = DBSQL_DENY;
    dbsql_auth_bad_return_code(parser);
  }
  return rc;
}

/*
** Push an authorization context.  After this routine is called, the
** arg3 argument to authorization callbacks will be context until
** popped.  Or if parser==0, this routine is a no-op.
*/
void __auth_context_push(
  parser_t *parser,
  auth_context_t *context, 
  const char *z_context
){
  assert( parser );
  context->parser = parser;
  context->auth_context = parser->auth_context;
  parser->auth_context = z_context;
}

/*
** Pop an authorization context that was previously pushed
** by __auth_context_push
*/
void __auth_context_pop(auth_context_t *context){
  if( context->parser ){
    context->parser->auth_context = context->auth_context;
    context->parser = 0;
  }
}

#endif /* DBSQL_OMIT_AUTHORIZATION */
