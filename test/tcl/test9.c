/*
** 2007 March 29
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
** This file contains obscure tests of the C-interface required
** for completeness. Test code is written in C for these cases
** as there is not much point in binding to Tcl.
*/
#include "dbsql_int.h"
#include "tcl_dbsql.h"
#include <stdlib.h>
#include <string.h>

/*
** c_collation_test
*/
static int DBSQL_TCLAPI c_collation_test(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  const char *err_function = "N/A";
  DBSQL *db;

  int rc;
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  /* Open a database. */
  rc = dbsql_open(":memory:", &db);
  if( rc!=DBSQL_OK ){
    err_function = "dbsql_open";
    goto error_out;
  }

  rc = dbsql_create_collation(db, "collate", 456, 0, 0);
  if( rc!=DBSQL_MISUSE ){
    dbsql_close(db);
    err_function = "dbsql_create_collation";
    goto error_out;
  }

  dbsql_close(db);
  return TCL_OK;

error_out:
  Tcl_ResetResult(interp);
  Tcl_AppendResult(interp, "Error testing function: ", err_function, NULL);
  return TCL_ERROR;
}

/*
** c_realloc_test
*/
static int DBSQL_TCLAPI c_realloc_test(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  void *p;
  const char *err_function = "N/A";

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  p = dbsql_malloc(5);
  if( !p ){
    err_function = "dbsql_malloc";
    goto error_out;
  }

  /* Test that realloc()ing a block of memory to a negative size is
  ** the same as free()ing that memory.
  */
  p = dbsql_realloc(p, -1);
  if( p ){
    err_function = "dbsql_realloc";
    goto error_out;
  }

  return TCL_OK;

error_out:
  Tcl_ResetResult(interp);
  Tcl_AppendResult(interp, "Error testing function: ", err_function, NULL);
  return TCL_ERROR;
}


/*
** c_misuse_test
*/
static int DBSQL_TCLAPI c_misuse_test(
  ClientData clientData, /* Pointer to dbsql_enable_XXX function */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  const char *err_function = "N/A";
  DBSQL *db = 0;
  dbsql_stmt_t *stmt;
  int rc;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  /* Open a database. Then close it again. We need to do this so that
  ** we have a "closed database handle" to pass to various API functions.
  */
  rc = dbsql_open(":memory:", &db);
  if( rc!=DBSQL_OK ){
    err_function = "dbsql_open";
    goto error_out;
  }
  dbsql_close(db);


  rc = dbsql_errcode(db);
  if( rc!=DBSQL_MISUSE ){
    err_function = "dbsql_errcode";
    goto error_out;
  }

  stmt = (dbsql_stmt_t*)1234;
  rc = dbsql_prepare(db, 0, 0, &stmt, 0);
  if( rc!=DBSQL_MISUSE ){
    err_function = "dbsql_prepare";
    goto error_out;
  }
  assert( stmt==0 ); /* Verify that stmt is zeroed even on a MISUSE error */

  stmt = (dbsql_stmt_t*)1234;
  rc = dbsql_prepare_v2(db, 0, 0, &stmt, 0);
  if( rc!=DBSQL_MISUSE ){
    err_function = "dbsql_prepare_v2";
    goto error_out;
  }
  assert( stmt==0 );

#ifndef DBSQL_OMIT_UTF16
  stmt = (dbsql_stmt_t*)1234;
  rc = dbsql_prepare16(db, 0, 0, &stmt, 0);
  if( rc!=DBSQL_MISUSE ){
    err_function = "dbsql_prepare16";
    goto error_out;
  }
  assert( stmt==0 );
  stmt = (dbsql_stmt_t*)1234;
  rc = dbsql_prepare16_v2(db, 0, 0, &stmt, 0);
  if( rc!=DBSQL_MISUSE ){
    err_function = "dbsql_prepare16_v2";
    goto error_out;
  }
  assert( stmt==0 );
#endif

  return TCL_OK;

error_out:
  Tcl_ResetResult(interp);
  Tcl_AppendResult(interp, "Error testing function: ", err_function, NULL);
  return TCL_ERROR;
}

/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest9_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
     void *clientData;
  } obj_cmds[] = {
     { "c_misuse_test",    c_misuse_test, 0 },
     { "c_realloc_test",   c_realloc_test, 0 },
     { "c_collation_test", c_collation_test, 0 },
  };
  int i;
  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, 
        obj_cmds[i].proc, obj_cmds[i].clientData, 0);
  }
  return TCL_OK;
}
