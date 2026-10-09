/*
** 2017 April 07
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
*/

#if defined(DBSQL_TEST)

#include "dbsql_expert.h"
#include <assert.h>
#include <string.h>
#include "tcl_dbsql.h"

#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** Extract an DBSQL* db handle from the object passed as the second
** argument. If successful, set *db to point to the db handle and return
** TCL_OK. Otherwise, return TCL_ERROR.
*/
static int __db_handle_from_obj(Tcl_Interp *interp, Tcl_Obj *obj, DBSQL **db){
  Tcl_CmdInfo info;
  if( 0==Tcl_GetCommandInfo(interp, Tcl_GetString(obj), &info) ){
    Tcl_AppendResult(interp, "no such handle: ", Tcl_GetString(obj), NULL);
    return TCL_ERROR;
  }

  *db = *(DBSQL **)info.objClientData;
  return TCL_OK;
}


/*
** Tclcmd:  $expert sql SQL
**          $expert analyze
**          $expert count
**          $expert report STMT EREPORT
**          $expert destroy
*/
static int DBSQL_TCLAPI __test_expert_cmd(
  void *clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  __dbsqlexpert *expert = (__dbsqlexpert*)clientData;
  struct subcmd_t {
    const char *sub;
    int n_arg;
    const char *msg;
  } subs[] = {
    { "sql",       1, "TABLE",        }, /* 0 */
    { "analyze",   0, "",             }, /* 1 */
    { "count",     0, "",             }, /* 2 */
    { "report",    2, "STMT EREPORT", }, /* 3 */
    { "destroy",   0, "",             }, /* 4 */
    { 0 }
  };
  int i_sub;
  int rc = TCL_OK;
  char *err = 0;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUBCOMMAND ...");
    return TCL_ERROR;
  }
  rc = Tcl_GetIndexFromObjStruct(interp, 
      objv[1], subs, sizeof(subs[0]), "sub-command", 0, &i_sub
  );
  if( rc!=TCL_OK ) return rc;
  if( objc!=2+subs[i_sub].n_arg ){
    Tcl_WrongNumArgs(interp, 2, objv, subs[i_sub].msg);
    return TCL_ERROR;
  }

  switch( i_sub ){
    case 0: {      /* sql */
      char *arg = Tcl_GetString(objv[2]);
      rc = dbsql_expert_sql(expert, arg, &err);
      break;
    }

    case 1: {      /* analyze */
      rc = dbsql_expert_analyze(expert, &err);
      break;
    }

    case 2: {      /* count */
      int n = dbsql_expert_count(expert);
      Tcl_SetObjResult(interp, Tcl_NewIntObj(n));
      break;
    }

    case 3: {      /* report */
      const char *enums[] = {
        "sql", "indexes", "plan", "candidates", 0
      };
      int i_enum;
      int i_stmt;
      const char *report;

      if( Tcl_GetIntFromObj(interp, objv[2], &i_stmt) 
       || Tcl_GetIndexFromObj(interp, objv[3], enums, "report", 0, &i_enum)
      ){
        return TCL_ERROR;
      }

      assert( EXPERT_REPORT_SQL==1 );
      assert( EXPERT_REPORT_INDEXES==2 );
      assert( EXPERT_REPORT_PLAN==3 );
      assert( EXPERT_REPORT_CANDIDATES==4 );
      report = dbsql_expert_report(expert, i_stmt, 1+i_enum);
      Tcl_SetObjResult(interp, Tcl_NewStringObj(report, -1));
      break;
    }

    default:       /* destroy */
      assert( i_sub==4 );     
      Tcl_DeleteCommand(interp, Tcl_GetString(objv[0]));
      break;
  }

  if( rc!=TCL_OK ){
    if( err ){
      Tcl_SetObjResult(interp, Tcl_NewStringObj(err, -1));
    }else{
      extern const char *__err_name(int);
      Tcl_SetObjResult(interp, Tcl_NewStringObj(__err_name(rc), -1));
    }
  }
  dbsql_free(err);
  return rc;
}

static void DBSQL_TCLAPI __test_expert_del(void *clientData){
  __dbsqlexpert *expert = (__dbsqlexpert*)clientData;
  dbsql_expert_destroy(expert);
}

/*
** dbsql_expert_new DB
*/
static int DBSQL_TCLAPI test_dbsql_expert_new(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  static int i_cmd = 0;
  DBSQL *db;
  char *cmd = 0;
  char *err = 0;
  __dbsqlexpert *expert;
  int rc = TCL_OK;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  if( __db_handle_from_obj(interp, objv[1], &db) ){
    return TCL_ERROR;
  }

  cmd = dbsql_mprintf("dbsql3expert%d", ++i_cmd);
  if( cmd==0 ){
    Tcl_AppendResult(interp, "out of memory", (char*)0);
    return TCL_ERROR;
  }

  expert = dbsql_expert_new(db, &err);
  if( expert==0 ){
    Tcl_AppendResult(interp, err, (char*)0);
    rc = TCL_ERROR;
  }else{
    void *p = (void*)expert;
    Tcl_CreateObjCommand(interp, cmd, __test_expert_cmd, p, __test_expert_del);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(cmd, -1));
  }

  dbsql_free(cmd);
  dbsql_free(err);
  return rc;
}

#endif  /* ifndef DBSQL_OMIT_VIRTUALTABLE */

int __test_expert_init(Tcl_Interp *interp){
#ifndef DBSQL_OMIT_VIRTUALTABLE
  struct cmd_t {
    const char *z_cmd;
    Tcl_ObjCmdProc *proc;
  } cmds[] = {
    { "dbsql_expert_new", test_dbsql_expert_new },
  };
  int i;

  for(i=0; i<sizeof(cmds)/sizeof(struct cmd_t); i++){
    struct cmd_t *p = &cmds[i];
    Tcl_CreateObjCommand(interp, p->z_cmd, p->proc, 0, 0);
  }
#endif
  return TCL_OK;
}

#endif
