/*
** 2009 August 17
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
** The code in this file is used for testing dbsql. It is not part of
** the source code used in production systems.
**
** Specifically, this file tests the effect of errors while initializing
** the various pluggable sub-systems from within dbsql_initialize().
** If an error occurs in dbsql_initialize() the following should be
** true:
**
**   1) An error code is returned to the user, and
**   2) A subsequent call to dbsql_shutdown() calls the shutdown method
**      of those subsystems that were initialized, and
**   3) A subsequent call to dbsql_initialize() attempts to initialize
**      the remaining, uninitialized, subsystems.
*/

#include "dbsql_int.h"
#include <string.h>
#include "tcl_dbsql.h"

static struct wrapped_t {
  dbsql_pcache_methods2_t pcache;
  dbsql_mem_methods_t     mem;
  dbsql_mutex_methods_t   mutex;

  int mem_init;              /* True if mem subsystem is initialized */
  int mem_fail;              /* True to fail mem subsystem initialization */
  int mutex_init;            /* True if mutex subsystem is initialized */
  int mutex_fail;            /* True to fail mutex subsystem initialization */
  int pcache_init;           /* True if pcache subsystem is initialized */
  int pcache_fail;           /* True to fail pcache subsystem initialization */
} wrapped;

static int __wr_mem_init(void *app_data){
  int rc;
  if( wrapped.mem_fail ){
    rc = DBSQL_ERROR;
  }else{
    rc = wrapped.mem.init(wrapped.mem.app_data);
  }
  if( rc==DBSQL_OK ){
    wrapped.mem_init = 1;
  }
  return rc;
}
static void __wr_mem_shutdown(void *app_data){
  wrapped.mem.shutdown(wrapped.mem.app_data);
  wrapped.mem_init = 0;
}
static void *__wr_mem_malloc(int n)           {return wrapped.mem.x_malloc(n);}
static void __wr_mem_free(void *p)            {wrapped.mem.__x_free(p);}
static void *__wr_mem_realloc(void *p, int n) {return wrapped.mem.x_realloc(p, n);}
static int __wr_mem_size(void *p)             {return wrapped.mem.size(p);}
static int __wr_mem_roundup(int n)            {return wrapped.mem.roundup(n);}


static int __wr_mutex_init(void){
  int rc;
  if( wrapped.mutex_fail ){
    rc = DBSQL_ERROR;
  }else{
    rc = wrapped.mutex.mutex_init();
  }
  if( rc==DBSQL_OK ){
    wrapped.mutex_init = 1;
  }
  return rc;
}
static int __wr_mutex_end(void){
  wrapped.mutex.mutex_end();
  wrapped.mutex_init = 0;
  return DBSQL_OK;
}
static dbsql_mutex_t *__wr_mutex_alloc(int e){
  return wrapped.mutex.mutex_alloc(e);
}
static void __wr_mutex_free(dbsql_mutex_t *p){
  wrapped.mutex.mutex_free(p);
}
static void __wr_mutex_enter(dbsql_mutex_t *p){
  wrapped.mutex.mutex_enter(p);
}
static int __wr_mutex_try(dbsql_mutex_t *p){
  return wrapped.mutex.mutex_try(p);
}
static void __wr_mutex_leave(dbsql_mutex_t *p){
  wrapped.mutex.mutex_leave(p);
}
static int __wr_mutex_held(dbsql_mutex_t *p){
  return wrapped.mutex.mutex_held(p);
}
static int __wr_mutex_notheld(dbsql_mutex_t *p){
  return wrapped.mutex.mutex_notheld(p);
}



static int __wr_p_cache_init(void *arg){
  int rc;
  if( wrapped.pcache_fail ){
    rc = DBSQL_ERROR;
  }else{
    rc = wrapped.pcache.init(wrapped.pcache.arg);
  }
  if( rc==DBSQL_OK ){
    wrapped.pcache_init = 1;
  }
  return rc;
}
static void __wr_p_cache_shutdown(void *arg){
  wrapped.pcache.shutdown(wrapped.pcache.arg);
  wrapped.pcache_init = 0;
}

static dbsql_pcache_t *__wr_p_cache_create(int a, int b, int c){
  return wrapped.pcache.create(a, b, c);
}  
static void __wr_p_cache_cachesize(dbsql_pcache_t *p, int n){
  wrapped.pcache.cachesize(p, n);
}  
static int __wr_p_cache_pagecount(dbsql_pcache_t *p){
  return wrapped.pcache.pagecount(p);
}  
static dbsql_pcache_page_t *__wr_p_cache_fetch(dbsql_pcache_t *p, unsigned a, int b){
  return wrapped.pcache.fetch(p, a, b);
}  
static void __wr_p_cache_unpin(dbsql_pcache_t *p, dbsql_pcache_page_t *a, int b){
  wrapped.pcache.unpin(p, a, b);
}  
static void __wr_p_cache_rekey(
  dbsql_pcache_t *p, 
  dbsql_pcache_page_t *a, 
  unsigned b, 
  unsigned c
){
  wrapped.pcache.rekey(p, a, b, c);
}  
static void __wr_p_cache_truncate(dbsql_pcache_t *p, unsigned a){
  wrapped.pcache.truncate(p, a);
}  
static void __wr_p_cache_destroy(dbsql_pcache_t *p){
  wrapped.pcache.destroy(p);
}  

static void __install_init_wrappers(void){
  dbsql_mutex_methods_t mutexmethods = {
    __wr_mutex_init,  __wr_mutex_end,   __wr_mutex_alloc,
    __wr_mutex_free,  __wr_mutex_enter, __wr_mutex_try,
    __wr_mutex_leave, __wr_mutex_held,  __wr_mutex_notheld
  };
  dbsql_pcache_methods2_t pcachemethods = {
    1, 0,
    __wr_p_cache_init,      __wr_p_cache_shutdown,  __wr_p_cache_create, 
    __wr_p_cache_cachesize, __wr_p_cache_pagecount, __wr_p_cache_fetch,
    __wr_p_cache_unpin,     __wr_p_cache_rekey,     __wr_p_cache_truncate,  
    __wr_p_cache_destroy
  };
  dbsql_mem_methods_t memmethods = {
    __wr_mem_malloc,   __wr_mem_free,    __wr_mem_realloc,
    __wr_mem_size,     __wr_mem_roundup, __wr_mem_init,
    __wr_mem_shutdown,
    0
  };

  memset(&wrapped, 0, sizeof(wrapped));

  dbsql_shutdown();
  dbsql_config(DBSQL_CONFIG_GETMUTEX, &wrapped.mutex);
  dbsql_config(DBSQL_CONFIG_GETMALLOC, &wrapped.mem);
  dbsql_config(DBSQL_CONFIG_GETPCACHE2, &wrapped.pcache);
  dbsql_config(DBSQL_CONFIG_MUTEX, &mutexmethods);
  dbsql_config(DBSQL_CONFIG_MALLOC, &memmethods);
  dbsql_config(DBSQL_CONFIG_PCACHE2, &pcachemethods);
}

static int DBSQL_TCLAPI init_wrapper_install(
  ClientData clientData, /* Unused */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  int i;
  __install_init_wrappers();
  for(i=1; i<objc; i++){
    char *z = Tcl_GetString(objv[i]);
    if( strcmp(z, "mem")==0 ){
      wrapped.mem_fail = 1;
    }else if( strcmp(z, "mutex")==0 ){
      wrapped.mutex_fail = 1;
    }else if( strcmp(z, "pcache")==0 ){
      wrapped.pcache_fail = 1;
    }else{
      Tcl_AppendResult(interp, "Unknown argument: \"", z, "\"", NULL);
      return TCL_ERROR;
    }
  }
  return TCL_OK;
}

static int DBSQL_TCLAPI init_wrapper_uninstall(
  ClientData clientData, /* Unused */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  dbsql_shutdown();
  dbsql_config(DBSQL_CONFIG_MUTEX, &wrapped.mutex);
  dbsql_config(DBSQL_CONFIG_MALLOC, &wrapped.mem);
  dbsql_config(DBSQL_CONFIG_PCACHE2, &wrapped.pcache);
  return TCL_OK;
}

static int DBSQL_TCLAPI init_wrapper_clear(
  ClientData clientData, /* Unused */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  wrapped.mem_fail = 0;
  wrapped.mutex_fail = 0;
  wrapped.pcache_fail = 0;
  return TCL_OK;
}

static int DBSQL_TCLAPI init_wrapper_query(
  ClientData clientData, /* Unused */
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int objc,              /* Number of arguments */
  Tcl_Obj *CONST objv[]  /* Command arguments */
){
  Tcl_Obj *ret;

  if( objc!=1 ){
    Tcl_WrongNumArgs(interp, 1, objv, "");
    return TCL_ERROR;
  }

  ret = Tcl_NewObj();
  if( wrapped.mutex_init ){
    Tcl_ListObjAppendElement(interp, ret, Tcl_NewStringObj("mutex", -1));
  }
  if( wrapped.mem_init ){
    Tcl_ListObjAppendElement(interp, ret, Tcl_NewStringObj("mem", -1));
  }
  if( wrapped.pcache_init ){
    Tcl_ListObjAppendElement(interp, ret, Tcl_NewStringObj("pcache", -1));
  }

  Tcl_SetObjResult(interp, ret);
  return TCL_OK;
}

int Dbsqltest_init_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_ObjCmdProc *proc;
  } obj_cmds[] = {
    {"init_wrapper_install",   init_wrapper_install},
    {"init_wrapper_query",     init_wrapper_query  },
    {"init_wrapper_uninstall", init_wrapper_uninstall},
    {"init_wrapper_clear",     init_wrapper_clear}
  };
  int i;

  for(i=0; i<sizeof(obj_cmds)/sizeof(obj_cmds[0]); i++){
    Tcl_CreateObjCommand(interp, obj_cmds[i].name, obj_cmds[i].proc, 0, 0);
  }

  return TCL_OK;
}
