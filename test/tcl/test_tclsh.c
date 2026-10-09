/*
** 2017-10-13
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
** This file contains extensions to the the "tclsqlite.c" module used for
** testing.  Basically, all of the other "test_*.c" modules are linked
** into the enhanced tclsh used for testing (and named "testfixture" or
** "testfixture.exe") using logic encoded by this file.
**
** The code in this file used to be found in tclsqlite3.c, contained within
** #if DBSQL_TEST ... #endif.  It is factored out into this separate module
** in an effort to keep the tclsqlite.c file pure.
*/
#include "dbsql.h"
#include "tcl_dbsql.h"

/* Needed for the setrlimit() system call on unix */
#if defined(unix)
#include <sys/resource.h>
#endif

/* Forward declaration */
static int DBSQL_TCLAPI load_testfixture_extensions(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
);

/*
** This routine is the primary export of this file.
**
** Configure the interpreter passed as the first argument to have access
** to the commands and linked variables that make up:
**
**   * the [DBSQL] extension itself,
**
**   * If DBSQL_TCLMD5 or DBSQL_TEST is defined, the Md5 commands, and
**
**   * If DBSQL_TEST is set, the various test interfaces used by the Tcl
**     test suite.
*/
const char *__test_init(Tcl_Interp *interp){
  extern int Dbsql_Init(Tcl_Interp*);
  extern int Dbsqlconfig_Init(Tcl_Interp*);
  extern int Dbsqltest1_Init(Tcl_Interp*);
  extern int Dbsqltest2_Init(Tcl_Interp*);
  extern int Dbsqltest3_Init(Tcl_Interp*);
  extern int Dbsqltest4_Init(Tcl_Interp*);
  extern int Dbsqltest5_Init(Tcl_Interp*);
  extern int Dbsqltest6_Init(Tcl_Interp*);
  extern int Dbsqltest8_Init(Tcl_Interp*);
  extern int Dbsqltest9_Init(Tcl_Interp*);
  extern int Dbsqltest_autoext_Init(Tcl_Interp*);
  extern int Dbsqltest_blob_Init(Tcl_Interp*);
  extern int Dbsqltest_demovfs_Init(Tcl_Interp *);
  extern int Dbsqltest_func_Init(Tcl_Interp*);
  extern int Dbsqltest_hexio_Init(Tcl_Interp*);
  extern int Dbsqltest_init_Init(Tcl_Interp*);
  extern int Dbsqltest_malloc_Init(Tcl_Interp*);
  extern int Dbsqltest_mutex_Init(Tcl_Interp*);
  extern int Dbsqltestschema_Init(Tcl_Interp*);
  extern int Dbsqltestsse_Init(Tcl_Interp*);
  extern int Dbsqltesttclvar_Init(Tcl_Interp*);
  extern int Dbsqltestfs_Init(Tcl_Interp*);
  extern int DbsqltestThread_Init(Tcl_Interp*);
  extern int DbsqltestOnefile_Init();
  extern int DbsqltestOsinst_Init(Tcl_Interp*);
  extern int Dbsqltestbackup_Init(Tcl_Interp*);
  extern int Dbsqltestintarray_Init(Tcl_Interp*);
  extern int Dbsqltestvfs_Init(Tcl_Interp *);
  extern int Dbsqltestrtree_Init(Tcl_Interp*);
  extern int Dbsqltestrtreedoc_Init(Tcl_Interp*);
  extern int Dbsqlquota_Init(Tcl_Interp*);
  extern int Dbsqlmultiplex_Init(Tcl_Interp*);
  extern int DbsqlSuperlock_Init(Tcl_Interp*);
  extern int DbsqltestSyscall_Init(Tcl_Interp*);
#if defined(DBSQL_ENABLE_SESSION) && defined(DBSQL_ENABLE_PREUPDATE_HOOK)
  extern int __test_session_init(Tcl_Interp*);
#endif
  extern int Md5_Init(Tcl_Interp*);
  extern int Fts5tcl_Init(Tcl_Interp *);
  extern int DbsqlRbu_Init(Tcl_Interp*);
  extern int Dbsqltesttcl_Init(Tcl_Interp*);
#if defined(DBSQL_ENABLE_FTS3) || defined(DBSQL_ENABLE_FTS4)
  extern int Dbsqltestfts3_Init(Tcl_Interp *interp);
#endif
#ifdef DBSQL_ENABLE_ZIPVFS
  extern int Zipvfs_Init(Tcl_Interp*);
#endif
  extern int __test_expert_init(Tcl_Interp*);
  extern int Dbsqltest_window_Init(Tcl_Interp *);
  extern int Dbsqltestvdbecov_Init(Tcl_Interp *);
  extern int __test_recover_init(Tcl_Interp*);
  extern int Dbsqltestintck_Init(Tcl_Interp*);

  Tcl_CmdInfo cmd_info;

  /* Since the primary use case for this binary is testing of dbsql,
  ** be sure to generate core files if we crash */
#if defined(unix)
  { struct rlimit x;
    getrlimit(RLIMIT_CORE, &x);
    x.rlim_cur = x.rlim_max;
    setrlimit(RLIMIT_CORE, &x);
  }
#endif /* unix */

  if( Tcl_GetCommandInfo(interp, "dbsql", &cmd_info)==0 ){
    Dbsql_Init(interp);
  }
#ifdef DBSQL_ENABLE_ZIPVFS
  Zipvfs_Init(interp);
#endif
  Md5_Init(interp);
  Dbsqlconfig_Init(interp);
  Dbsqltest1_Init(interp);
  Dbsqltest2_Init(interp);
  Dbsqltest3_Init(interp);
  Dbsqltest4_Init(interp);
  Dbsqltest5_Init(interp);
  Dbsqltest6_Init(interp);
  Dbsqltest8_Init(interp);
  Dbsqltest9_Init(interp);
  Dbsqltest_autoext_Init(interp);
  Dbsqltest_blob_Init(interp);
  Dbsqltest_demovfs_Init(interp);
  Dbsqltest_func_Init(interp);
  Dbsqltest_hexio_Init(interp);
  Dbsqltest_init_Init(interp);
  Dbsqltest_malloc_Init(interp);
  Dbsqltest_mutex_Init(interp);
  Dbsqltestschema_Init(interp);
  Dbsqltesttclvar_Init(interp);
  Dbsqltestfs_Init(interp);
  DbsqltestThread_Init(interp);
  DbsqltestOnefile_Init();
  DbsqltestOsinst_Init(interp);
  Dbsqltestbackup_Init(interp);
  Dbsqltestintarray_Init(interp);
  Dbsqltestvfs_Init(interp);
  Dbsqltestrtree_Init(interp);
  Dbsqltestrtreedoc_Init(interp);
  Dbsqlquota_Init(interp);
  Dbsqlmultiplex_Init(interp);
  DbsqlSuperlock_Init(interp);
  DbsqltestSyscall_Init(interp);
#if defined(DBSQL_ENABLE_SESSION) && defined(DBSQL_ENABLE_PREUPDATE_HOOK)
  __test_session_init(interp);
#endif
  Fts5tcl_Init(interp);
  DbsqlRbu_Init(interp);
  Dbsqltesttcl_Init(interp);

#if defined(DBSQL_ENABLE_FTS3) || defined(DBSQL_ENABLE_FTS4)
  Dbsqltestfts3_Init(interp);
#endif
  __test_expert_init(interp);
  Dbsqltest_window_Init(interp);
  Dbsqltestvdbecov_Init(interp);
  __test_recover_init(interp);
  Dbsqltestintck_Init(interp);

  Tcl_CreateObjCommand(
      interp, "load_testfixture_extensions", load_testfixture_extensions,0,0
  );
  return 0;
}

/* tclcmd:   load_testfixture_extensions
*/
static int DBSQL_TCLAPI load_testfixture_extensions(
  ClientData cd,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){

  Tcl_Interp *slave;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SLAVE");
    return TCL_ERROR;
  }

  slave = Tcl_GetSlave(interp, Tcl_GetString(objv[1]));
  if( !slave ){
    return TCL_ERROR;
  }

  (void)__test_init(slave);
  return TCL_OK;
}
