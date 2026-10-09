/*
** 2008 June 18
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains test logic for the dbsql_mutex_t interfaces.
*/
#include "tcl_dbsql.h"
#include "dbsql.h"
#include "dbsql_int.h"
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define MAX_MUTEXES        (DBSQL_MUTEX_STATIC_VFS3+1)
#define STATIC_MUTEXES     (MAX_MUTEXES-(DBSQL_MUTEX_RECURSIVE+1))

/* defined in main.c */
extern const char *__err_name(int);

static const char *__a_name[MAX_MUTEXES+1] = {
  "fast",        "recursive",   "static_main",   "static_mem",
  "static_open", "static_prng", "static_lru",    "static_pmem",
  "static_app1", "static_app2", "static_app3",   "static_vfs1",
  "static_vfs2", "static_vfs3", 0
};

/* A countable mutex */
struct dbsql_mutex_t {
  dbsql_mutex_t *real;
  int type;
};

/* State variables */
static struct test_mutex_globals {
  int is_installed;           /* True if installed */
  int disable_init;           /* True to cause dbsql_initialize() to fail */
  int disable_try;            /* True to force dbsql_mutex_try() to fail */
  int is_init;                /* True if initialized */
  dbsql_mutex_methods_t m;   /* Interface to "real" mutex system */
  int counters[MAX_MUTEXES]; /* Number of grabs of each type of mutex */
  dbsql_mutex_t statics[STATIC_MUTEXES]; /* The static mutexes */
} g = {0};

/* Return true if the countable mutex is currently held */
static int __counter_mutex_held(dbsql_mutex_t *p){
  return g.m.mutex_held(p->real);
}

/* Return true if the countable mutex is not currently held */
static int __counter_mutex_notheld(dbsql_mutex_t *p){
  return g.m.mutex_notheld(p->real);
}

/* Initialize the countable mutex interface
** Or, if g.disable_init is non-zero, then do not initialize but instead
** return the value of g.disable_init as the result code.  This can be used
** to simulate an initialization failure.
*/
static int __counter_mutex_init(void){ 
  int rc;
  if( g.disable_init ) return g.disable_init;
  rc = g.m.mutex_init();
  g.is_init = 1;
  return rc;
}

/*
** Uninitialize the mutex subsystem
*/
static int __counter_mutex_end(void){ 
  g.is_init = 0;
  return g.m.mutex_end();
}

/*
** Allocate a countable mutex
*/
static dbsql_mutex_t *__counter_mutex_alloc(int type){
  dbsql_mutex_t *real;
  dbsql_mutex_t *ret = 0;

  assert( g.is_init );
  assert( type>=DBSQL_MUTEX_FAST );
  assert( type<=DBSQL_MUTEX_STATIC_VFS3 );

  real = g.m.mutex_alloc(type);
  if( !real ) return 0;

  if( type==DBSQL_MUTEX_FAST || type==DBSQL_MUTEX_RECURSIVE ){
    ret = (dbsql_mutex_t *)malloc(sizeof(dbsql_mutex_t));
  }else{
    int static_type = type - (MAX_MUTEXES - STATIC_MUTEXES);
    assert( static_type>=0 );
    assert( static_type<STATIC_MUTEXES );
    ret = &g.statics[static_type];
  }

  ret->type = type;
  ret->real = real;
  return ret;
}

/*
** Free a countable mutex
*/
static void __counter_mutex_free(dbsql_mutex_t *p){
  assert( g.is_init );
  g.m.mutex_free(p->real);
  if( p->type==DBSQL_MUTEX_FAST || p->type==DBSQL_MUTEX_RECURSIVE ){
    free(p);
  }
}

/*
** Enter a countable mutex.  Block until entry is safe.
*/
static void __counter_mutex_enter(dbsql_mutex_t *p){
  assert( g.is_init );
  assert( p->type>=0 );
  assert( p->type<MAX_MUTEXES );
  g.counters[p->type]++;
  g.m.mutex_enter(p->real);
}

/*
** Try to enter a mutex.  Return true on success.
*/
static int __counter_mutex_try(dbsql_mutex_t *p){
  assert( g.is_init );
  assert( p->type>=0 );
  assert( p->type<MAX_MUTEXES );
  g.counters[p->type]++;
  if( g.disable_try ) return DBSQL_BUSY;
  return g.m.mutex_try(p->real);
}

/* Leave a mutex
*/
static void __counter_mutex_leave(dbsql_mutex_t *p){
  assert( g.is_init );
  g.m.mutex_leave(p->real);
}

/*
** dbsql_shutdown
*/
static int DBSQL_TCLAPI test_shutdown(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  rc = dbsql_shutdown();
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** dbsql_initialize
*/
static int DBSQL_TCLAPI test_initialize(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  rc = dbsql_initialize();
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** install_mutex_counters BOOLEAN
*/
static int DBSQL_TCLAPI test_install_mutex_counters(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int rc = DBSQL_OK;
  int is_install;

  dbsql_mutex_methods_t counter_methods = {
    __counter_mutex_init,
    __counter_mutex_end,
    __counter_mutex_alloc,
    __counter_mutex_free,
    __counter_mutex_enter,
    __counter_mutex_try,
    __counter_mutex_leave,
    __counter_mutex_held,
    __counter_mutex_notheld
  };

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "BOOLEAN");
    return TCL_ERROR;
  }
  if( TCL_OK!=Tcl_GetBooleanFromObj(interp, objv[1], &is_install) ){
    return TCL_ERROR;
  }

  assert(is_install==0 || is_install==1);
  assert(g.is_installed==0 || g.is_installed==1);
  if( is_install==g.is_installed ){
    Tcl_AppendResult(interp, "mutex counters are ", NULL);
    Tcl_AppendResult(interp, is_install?"already installed":"not installed", NULL);
    return TCL_ERROR;
  }

  if( is_install ){
    assert( g.m.mutex_alloc==0 );
    rc = dbsql_config(DBSQL_CONFIG_GETMUTEX, &g.m);
    if( rc==DBSQL_OK ){
      dbsql_config(DBSQL_CONFIG_MUTEX, &counter_methods);
    }
    g.disable_try = 0;
  }else{
    assert( g.m.mutex_alloc );
    rc = dbsql_config(DBSQL_CONFIG_MUTEX, &g.m);
    memset(&g.m, 0, sizeof(dbsql_mutex_methods_t));
  }

  if( rc==DBSQL_OK ){
    g.is_installed = is_install;
  }

  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
  return TCL_OK;
}

/*
** read_mutex_counters
*/
static int DBSQL_TCLAPI test_read_mutex_counters(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_Obj *ret;
  int ii;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  ret = Tcl_NewObj();
  Tcl_IncrRefCount(ret);
  for(ii=0; ii<MAX_MUTEXES; ii++){
    Tcl_ListObjAppendElement(interp, ret, Tcl_NewStringObj(__a_name[ii], -1));
    Tcl_ListObjAppendElement(interp, ret, Tcl_NewIntObj(g.counters[ii]));
  }
  Tcl_SetObjResult(interp, ret);
  Tcl_DecrRefCount(ret);

  return TCL_OK;
}

/*
** clear_mutex_counters
*/
static int DBSQL_TCLAPI test_clear_mutex_counters(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int ii;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  for(ii=0; ii<MAX_MUTEXES; ii++){
    g.counters[ii] = 0;
  }
  return TCL_OK;
}

/*
** Create and free a mutex.  Return the mutex pointer.  The pointer
** will be invalid since the mutex has already been freed.  The
** return pointer just checks to see if the mutex really was allocated.
*/
static int DBSQL_TCLAPI test_alloc_mutex(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
#if DBSQL_THREADSAFE
  dbsql_mutex_t *p = dbsql_mutex_alloc(DBSQL_MUTEX_FAST);
  char buf[100];
  dbsql_mutex_free(p);
  dbsql_snprintf(sizeof(buf), buf, "%p", p);
  Tcl_AppendResult(interp, buf, (char*)0);
#endif
  return TCL_OK;
}

/*
** dbsql_config OPTION
**
** OPTION can be either one of the keywords:
**
**            DBSQL_CONFIG_SINGLETHREAD
**            DBSQL_CONFIG_MULTITHREAD
**            DBSQL_CONFIG_SERIALIZED
**
** Or OPTION can be an raw integer.
*/
static int DBSQL_TCLAPI test_config(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  struct config_option_t {
    const char *name;
    int i_value;
  } opts[] = {
    {"singlethread", DBSQL_CONFIG_SINGLETHREAD},
    {"multithread",  DBSQL_CONFIG_MULTITHREAD},
    {"serialized",   DBSQL_CONFIG_SERIALIZED},
    {0, 0}
  };
  int s = sizeof(struct config_option_t);
  int i;
  int rc;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  if( Tcl_GetIndexFromObjStruct(interp, objv[1], opts, s, "flag", 0, &i) ){
    if( Tcl_GetIntFromObj(interp, objv[1], &i) ){
      return TCL_ERROR;
    }
  }else{
    i = opts[i].i_value;
  }

  rc = dbsql_config(i);
  Tcl_SetResult(interp, (char *)__err_name(rc), TCL_VOLATILE);
  return TCL_OK;
}

static DBSQL *__get_db_pointer(Tcl_Interp *interp, Tcl_Obj *obj){
  DBSQL *db;
  Tcl_CmdInfo info;
  char *cmd = Tcl_GetString(obj);
  if( Tcl_GetCommandInfo(interp, cmd, &info) ){
    db = *((DBSQL **)info.objClientData);
  }else{
    db = (DBSQL*)__test_text_to_ptr(cmd);
  }
  assert( db );
  return db;
}

static dbsql_mutex_t *__get_static_mutex_pointer(
  Tcl_Interp *interp,
  Tcl_Obj *obj
){
  int i_mutex;
  if( Tcl_GetIndexFromObj(interp, obj, __a_name, "mutex name", 0, &i_mutex) ){
    return 0;
  }
  assert( i_mutex!=DBSQL_MUTEX_FAST && i_mutex!=DBSQL_MUTEX_RECURSIVE );
  return __counter_mutex_alloc(i_mutex);
}

static int DBSQL_TCLAPI test_enter_static_mutex(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  dbsql_mutex_t *mutex;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NAME");
    return TCL_ERROR;
  }
  mutex = __get_static_mutex_pointer(interp, objv[1]);
  if( !mutex ){
    return TCL_ERROR;
  }
  dbsql_mutex_enter(mutex);
  return TCL_OK;
}

static int DBSQL_TCLAPI test_leave_static_mutex(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  dbsql_mutex_t *mutex;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "NAME");
    return TCL_ERROR;
  }
  mutex = __get_static_mutex_pointer(interp, objv[1]);
  if( !mutex ){
    return TCL_ERROR;
  }
  dbsql_mutex_leave(mutex);
  return TCL_OK;
}

static int DBSQL_TCLAPI test_enter_db_mutex(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  db = __get_db_pointer(interp, objv[1]);
  if( !db ){
    return TCL_ERROR;
  }
  dbsql_mutex_enter(dbsql_db_mutex(db));
  return TCL_OK;
}

static int DBSQL_TCLAPI test_leave_db_mutex(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db;
  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "DB");
    return TCL_ERROR;
  }
  db = __get_db_pointer(interp, objv[1]);
  if( !db ){
    return TCL_ERROR;
  }
  dbsql_mutex_leave(dbsql_db_mutex(db));
  return TCL_OK;
}

int Dbsqltest_mutex_Init(Tcl_Interp *interp){
  static struct {
    char *name;
    Tcl_ObjCmdProc *proc;
  } cmds[] = {
    { "dbsql_shutdown",        (Tcl_ObjCmdProc*)test_shutdown },
    { "dbsql_initialize",      (Tcl_ObjCmdProc*)test_initialize },
    { "dbsql_config",          (Tcl_ObjCmdProc*)test_config },

    { "enter_static_mutex",      (Tcl_ObjCmdProc*)test_enter_static_mutex },
    { "leave_static_mutex",      (Tcl_ObjCmdProc*)test_leave_static_mutex },

    { "enter_db_mutex",          (Tcl_ObjCmdProc*)test_enter_db_mutex },
    { "leave_db_mutex",          (Tcl_ObjCmdProc*)test_leave_db_mutex },

    { "alloc_dealloc_mutex",     (Tcl_ObjCmdProc*)test_alloc_mutex },
    { "install_mutex_counters",  (Tcl_ObjCmdProc*)test_install_mutex_counters },
    { "read_mutex_counters",     (Tcl_ObjCmdProc*)test_read_mutex_counters },
    { "clear_mutex_counters",    (Tcl_ObjCmdProc*)test_clear_mutex_counters },
  };
  int i;
  for(i=0; i<sizeof(cmds)/sizeof(cmds[0]); i++){
    Tcl_CreateObjCommand(interp, cmds[i].name, cmds[i].proc, 0, 0);
  }

  Tcl_LinkVar(interp, "disable_mutex_init", 
              (char*)&g.disable_init, TCL_LINK_INT);
  Tcl_LinkVar(interp, "disable_mutex_try", 
              (char*)&g.disable_try, TCL_LINK_INT);
  return DBSQL_OK;
}
