/*
** 2009 January 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains test logic for the dbsql_backup_t() interface.
**
*/

#include "tcl_dbsql.h"
#include "dbsql.h"
#include <assert.h>

/* These functions are implemented in main.c. */
extern const char *__err_name(int);

/* These functions are implemented in test1.c. */
extern int __get_db_pointer(Tcl_Interp *, const char *, DBSQL **);

static int DBSQL_TCLAPI __backup_test_cmd(
  ClientData clientData, 
  Tcl_Interp *interp, 
  int objc,
  Tcl_Obj *const*objv
){
  enum __backup_sub_command_enum {
    BACKUP_STEP, BACKUP_FINISH, BACKUP_REMAINING, BACKUP_PAGECOUNT
  };
  struct backup_sub_command_t {
    const char *z_cmd;
    enum __backup_sub_command_enum cmd;
    int n_arg;
    const char *arg;
  } subs[] = {
    {"step",      BACKUP_STEP      , 1, "npage" },
    {"finish",    BACKUP_FINISH    , 0, ""      },
    {"remaining", BACKUP_REMAINING , 0, ""      },
    {"pagecount", BACKUP_PAGECOUNT , 0, ""      },
    {0, 0, 0, 0}
  };

  dbsql_backup_t *p = (dbsql_backup_t *)clientData;
  int i_cmd;
  int rc;

  rc = Tcl_GetIndexFromObjStruct(
      interp, objv[1], subs, sizeof(subs[0]), "option", 0, &i_cmd
  );
  if( rc!=TCL_OK ){
    return rc;
  }
  if( objc!=(2 + subs[i_cmd].n_arg) ){
    Tcl_WrongNumArgs(interp, 2, objv, subs[i_cmd].arg);
    return TCL_ERROR;
  }

  switch( subs[i_cmd].cmd ){

    case BACKUP_FINISH: {
      const char *cmd_name;
      Tcl_CmdInfo cmd_info;
      cmd_name = Tcl_GetString(objv[0]);
      Tcl_GetCommandInfo(interp, cmd_name, &cmd_info);
      cmd_info.deleteProc = 0;
      Tcl_SetCommandInfo(interp, cmd_name, &cmd_info);
      Tcl_DeleteCommand(interp, cmd_name);

      rc = dbsql_backup_finish(p);
      Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);
      break;
    }

    case BACKUP_STEP: {
      int n_page;
      if( TCL_OK!=Tcl_GetIntFromObj(interp, objv[2], &n_page) ){
        return TCL_ERROR;
      }
      rc = dbsql_backup_step(p, n_page);
      Tcl_SetResult(interp, (char *)__err_name(rc), TCL_STATIC);
      break;
    }

    case BACKUP_REMAINING:
      Tcl_SetObjResult(interp, Tcl_NewIntObj(dbsql_backup_remaining(p)));
      break;

    case BACKUP_PAGECOUNT:
      Tcl_SetObjResult(interp, Tcl_NewIntObj(dbsql_backup_pagecount(p)));
      break;
  }

  return TCL_OK;
}

static void DBSQL_TCLAPI __backup_test_finish(ClientData clientData){
  dbsql_backup_t *backup = (dbsql_backup_t *)clientData;
  dbsql_backup_finish(backup);
}

/*
**     dbsql_backup_t CMDNAME DESTHANDLE DESTNAME SRCHANDLE SRCNAME
**
*/
static int DBSQL_TCLAPI __backup_test_init(
  ClientData clientData, 
  Tcl_Interp *interp, 
  int objc,
  Tcl_Obj *const*objv
){
  dbsql_backup_t *backup;
  DBSQL *dest_db;
  DBSQL *src_db;
  const char *dest_name;
  const char *src_name;
  const char *cmd;

  if( objc!=6 ){
    Tcl_WrongNumArgs(
      interp, 1, objv, "CMDNAME DESTHANDLE DESTNAME SRCHANDLE SRCNAME"
    );
    return TCL_ERROR;
  }

  cmd = Tcl_GetString(objv[1]);
  __get_db_pointer(interp, Tcl_GetString(objv[2]), &dest_db);
  dest_name = Tcl_GetString(objv[3]);
  __get_db_pointer(interp, Tcl_GetString(objv[4]), &src_db);
  src_name = Tcl_GetString(objv[5]);

  backup = dbsql_backup_init(dest_db, dest_name, src_db, src_name);
  if( !backup ){
    Tcl_AppendResult(interp, "dbsql_backup_init() failed", NULL);
    return TCL_ERROR;
  }

  Tcl_CreateObjCommand(interp, cmd, __backup_test_cmd, backup, __backup_test_finish);
  Tcl_SetObjResult(interp, objv[1]);
  return TCL_OK;
}

int Dbsqltestbackup_Init(Tcl_Interp *interp){
  Tcl_CreateObjCommand(interp, "dbsql_backup", __backup_test_init, 0, 0);
  return TCL_OK;
}
