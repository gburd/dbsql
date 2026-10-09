/*
** 2006 August 23
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Test extension for testing the dbsql_auto_extension() function.
*/
#include "tcl_dbsql.h"
#include "dbsql_ext.h"

#ifndef DBSQL_OMIT_LOAD_EXTENSION
DBSQL_EXTENSION_INIT1

/*
** The sqr() SQL function returns the square of its input value.
*/
static void __sqr_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  double r = dbsql_value_double(argv[0]);
  dbsql_result_double(context, r*r);
}

/*
** This is the entry point to register the extension for the sqr() function.
*/
static int sqr_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  DBSQL_EXTENSION_INIT2(api);
  dbsql_create_function(db, "sqr", 1, DBSQL_ANY, 0, __sqr_func, 0, 0);
  return 0;
}

/*
** The cube() SQL function returns the cube of its input value.
*/
static void __cube_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  double r = dbsql_value_double(argv[0]);
  dbsql_result_double(context, r*r*r);
}

/*
** This is the entry point to register the extension for the cube() function.
*/
static int cube_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  DBSQL_EXTENSION_INIT2(api);
  dbsql_create_function(db, "cube", 1, DBSQL_ANY, 0, __cube_func, 0, 0);
  return 0;
}

/*
** This is a broken extension entry point
*/
static int broken_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  char *err;
  DBSQL_EXTENSION_INIT2(api);
  err = dbsql_mprintf("broken autoext!");
  *errmsg_p = err;
  return 1;
}

/*
** tclcmd:   dbsql_auto_extension_sqr
**
** Register the "sqr" extension to be loaded automatically.
*/
static int DBSQL_TCLAPI __auto_ext_sqr_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc = dbsql_auto_extension((void(*)(void))sqr_init);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return DBSQL_OK;
}

/*
** tclcmd:   dbsql_cancel_auto_extension_sqr
**
** Unregister the "sqr" extension.
*/
static int DBSQL_TCLAPI __cancel_auto_ext_sqr_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc = dbsql_cancel_auto_extension((void(*)(void))sqr_init);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return DBSQL_OK;
}

/*
** tclcmd:   dbsql_auto_extension_cube
**
** Register the "cube" extension to be loaded automatically.
*/
static int DBSQL_TCLAPI __auto_ext_cube_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc = dbsql_auto_extension((void(*)(void))cube_init);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return DBSQL_OK;
}

/*
** tclcmd:   dbsql_cancel_auto_extension_cube
**
** Unregister the "cube" extension.
*/
static int DBSQL_TCLAPI __cancel_auto_ext_cube_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc = dbsql_cancel_auto_extension((void(*)(void))cube_init);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return DBSQL_OK;
}

/*
** tclcmd:   dbsql_auto_extension_broken
**
** Register the broken extension to be loaded automatically.
*/
static int DBSQL_TCLAPI __auto_ext_broken_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc = dbsql_auto_extension((void(*)(void))broken_init);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return DBSQL_OK;
}

/*
** tclcmd:   dbsql_cancel_auto_extension_broken
**
** Unregister the broken extension.
*/
static int DBSQL_TCLAPI __cancel_auto_ext_broken_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc = dbsql_cancel_auto_extension((void(*)(void))broken_init);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
  return DBSQL_OK;
}

#endif /* DBSQL_OMIT_LOAD_EXTENSION */


/*
** tclcmd:   dbsql_reset_auto_extension
**
** Reset all auto-extensions
*/
static int DBSQL_TCLAPI __reset_auto_ext_obj_cmd(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  dbsql_reset_auto_extension();
  return DBSQL_OK;
}


/*
** This procedure registers the TCL procs defined in this file.
*/
int Dbsqltest_autoext_Init(Tcl_Interp *interp){
#ifndef DBSQL_OMIT_LOAD_EXTENSION
  Tcl_CreateObjCommand(interp, "dbsql_auto_extension_sqr",
          __auto_ext_sqr_obj_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "dbsql_auto_extension_cube",
          __auto_ext_cube_obj_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "dbsql_auto_extension_broken",
          __auto_ext_broken_obj_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "dbsql_cancel_auto_extension_sqr",
          __cancel_auto_ext_sqr_obj_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "dbsql_cancel_auto_extension_cube",
          __cancel_auto_ext_cube_obj_cmd, 0, 0);
  Tcl_CreateObjCommand(interp, "dbsql_cancel_auto_extension_broken",
          __cancel_auto_ext_broken_obj_cmd, 0, 0);
#endif
  Tcl_CreateObjCommand(interp, "dbsql_reset_auto_extension",
          __reset_auto_ext_obj_cmd, 0, 0);
  return TCL_OK;
}
