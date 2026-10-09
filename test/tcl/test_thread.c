/*
** 2007 September 9
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
** This file contains the implementation of some Tcl commands used to
** test that DBSQL database handles may be concurrently accessed by 
** multiple threads. Right now this only works on unix.
*/

#include "dbsql_int.h"
#include "tcl_dbsql.h"

#if DBSQL_THREADSAFE

#include <errno.h>

#if !defined(_MSC_VER)
#include <unistd.h>
#endif

/*
** One of these is allocated for each thread created by [sqlthread spawn].
*/
typedef struct sql_thread_t sql_thread_t;
struct sql_thread_t {
  Tcl_ThreadId parent;     /* dbsql_thread_t id of parent thread */
  Tcl_Interp *interp;      /* Parent interpreter */
  char *script;           /* The script to execute. */
  char *varname;          /* Varname in parent script */
};

/*
** A custom Tcl_Event type used by this module. When the event is
** handled, script script is evaluated in interpreter interp. If
** the evaluation throws an exception (returns TCL_ERROR), then the
** error is handled by Tcl_BackgroundError(). If no error occurs,
** the result is simply discarded.
*/
typedef struct eval_event_t eval_event_t;
struct eval_event_t {
  Tcl_Event base;          /* Base class of type Tcl_Event */
  char *script;           /* The script to execute. */
  Tcl_Interp *interp;      /* The interpreter to execute it in. */
};

static Tcl_ObjCmdProc sqlthread_proc;
static Tcl_ObjCmdProc clock_seconds_proc;
#if DBSQL_OS_UNIX && defined(DBSQL_ENABLE_UNLOCK_NOTIFY)
static Tcl_ObjCmdProc blocking_step_proc;
static Tcl_ObjCmdProc blocking_prepare_v2_proc;
#endif
int Dbsqltest1_Init(Tcl_Interp *);
int Dbsql_Init(Tcl_Interp *);

/* Functions from main.c */
extern const char *__err_name(int);

/* Functions from test1.c */
extern void *__test_text_to_ptr(const char *);
extern int __get_db_pointer(Tcl_Interp *, const char *, DBSQL **);
extern int __test_make_pointer_str(Tcl_Interp *, char *, void *);
extern int __test_err_code(Tcl_Interp *, DBSQL *, int);

/*
** Handler for events of type eval_event_t.
*/
static int DBSQL_TCLAPI __tcl_script_event(Tcl_Event *evPtr, int flags){
  int rc;
  eval_event_t *p = (eval_event_t *)evPtr;
  rc = Tcl_Eval(p->interp, p->script);
  if( rc!=TCL_OK ){
    Tcl_BackgroundError(p->interp);
  }
  UNUSED_PARAMETER(flags);
  return 1;
}

/*
** Register an eval_event_t to evaluate the script script in the
** parent interpreter/thread of sql_thread_t p.
*/
static void __post_to_parent(sql_thread_t *p, Tcl_Obj *script){
  eval_event_t *event;
  char *msg;
  TCL_SIZE n_msg;

  msg = Tcl_GetStringFromObj(script, &n_msg); 
  event = (eval_event_t *)ckalloc(sizeof(eval_event_t)+n_msg+1);
  event->base.nextPtr = 0;
  event->base.proc = __tcl_script_event;
  event->script = (char *)&event[1];
  memcpy(event->script, msg, n_msg+1);
  event->interp = p->interp;

  Tcl_ThreadQueueEvent(p->parent, (Tcl_Event *)event, TCL_QUEUE_TAIL);
  Tcl_ThreadAlert(p->parent);
}

/*
** The main function for threads created with [sqlthread spawn].
*/
static Tcl_ThreadCreateType __tcl_script_thread(ClientData sql_thread){
  Tcl_Interp *interp;
  Tcl_Obj *res;
  Tcl_Obj *list;
  int rc;
  sql_thread_t *p = (sql_thread_t *)sql_thread;
  extern int Dbsqltest_mutex_Init(Tcl_Interp*);

  interp = Tcl_CreateInterp();
  Tcl_CreateObjCommand(interp, "clock_seconds", clock_seconds_proc, 0, 0);
  Tcl_CreateObjCommand(interp, "sqlthread", sqlthread_proc, sql_thread, 0);
#if DBSQL_OS_UNIX && defined(DBSQL_ENABLE_UNLOCK_NOTIFY)
  Tcl_CreateObjCommand(interp, "dbsql_blocking_step", blocking_step_proc,0,0);
  Tcl_CreateObjCommand(interp, 
      "dbsql_blocking_prepare_v2", blocking_prepare_v2_proc, (void *)1, 0);
  Tcl_CreateObjCommand(interp, 
      "dbsql_nonblocking_prepare_v2", blocking_prepare_v2_proc, 0, 0);
#endif
  Dbsqltest1_Init(interp);
  Dbsqltest_mutex_Init(interp);
  Dbsql_Init(interp);

  rc = Tcl_Eval(interp, p->script);
  res = Tcl_GetObjResult(interp);
  list = Tcl_NewObj();
  Tcl_IncrRefCount(list);
  Tcl_IncrRefCount(res);

  if( rc!=TCL_OK ){
    Tcl_ListObjAppendElement(interp, list, Tcl_NewStringObj("error", -1));
    Tcl_ListObjAppendElement(interp, list, res);
    __post_to_parent(p, list);
    Tcl_DecrRefCount(list);
    list = Tcl_NewObj();
  }

  Tcl_ListObjAppendElement(interp, list, Tcl_NewStringObj("set", -1));
  Tcl_ListObjAppendElement(interp, list, Tcl_NewStringObj(p->varname, -1));
  Tcl_ListObjAppendElement(interp, list, res);
  __post_to_parent(p, list);

  ckfree((void *)p);
  Tcl_DecrRefCount(list);
  Tcl_DecrRefCount(res);
  Tcl_DeleteInterp(interp);
  while( Tcl_DoOneEvent(TCL_ALL_EVENTS|TCL_DONT_WAIT) );
  Tcl_ExitThread(0);
  TCL_THREAD_CREATE_RETURN;
}

/*
** sqlthread spawn VARNAME SCRIPT
**
**     Spawn a new thread with its own Tcl interpreter and run the
**     specified SCRIPT(s) in it. The thread terminates after running
**     the script. The result of the script is stored in the variable
**     VARNAME.
**
**     The caller can wait for the script to terminate using [vwait VARNAME].
*/
static int DBSQL_TCLAPI sqlthread_spawn(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_ThreadId x;
  sql_thread_t *p_new;
  int rc;

  TCL_SIZE n_varname; char *varname;
  TCL_SIZE n_script; char *script;

  /* Parameters for thread creation */
  const int n_stack = TCL_THREAD_STACK_DEFAULT;
  const int flags = TCL_THREAD_NOFLAGS;

  assert(objc==4);
  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(objc);

  varname = Tcl_GetStringFromObj(objv[2], &n_varname);
  script = Tcl_GetStringFromObj(objv[3], &n_script);

  p_new = (sql_thread_t *)ckalloc(sizeof(sql_thread_t)+n_varname+n_script+2);
  p_new->varname = (char *)&p_new[1];
  p_new->script = (char *)&p_new->varname[n_varname+1];
  memcpy(p_new->varname, varname, n_varname+1);
  memcpy(p_new->script, script, n_script+1);
  p_new->parent = Tcl_GetCurrentThread();
  p_new->interp = interp;

  rc = Tcl_CreateThread(&x, __tcl_script_thread, (void *)p_new, n_stack, flags);
  if( rc!=TCL_OK ){
    Tcl_AppendResult(interp, "Error in Tcl_CreateThread()", NULL);
    ckfree((char *)p_new);
    return TCL_ERROR;
  }

  return TCL_OK;
}

/*
** sqlthread parent SCRIPT
**
**     This can be called by spawned threads only. It sends the specified
**     script back to the parent thread for execution. The result of
**     evaluating the SCRIPT is returned. The parent thread must enter
**     the event loop for this to work - otherwise the caller will
**     block indefinitely.
**
**     NOTE: At the moment, this doesn't work. FIXME.
*/
static int DBSQL_TCLAPI sqlthread_parent(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  eval_event_t *event;
  char *msg;
  TCL_SIZE n_msg;
  sql_thread_t *p = (sql_thread_t *)clientData;

  assert(objc==3);
  UNUSED_PARAMETER(objc);

  if( p==0 ){
    Tcl_AppendResult(interp, "no parent thread", NULL);
    return TCL_ERROR;
  }

  msg = Tcl_GetStringFromObj(objv[2], &n_msg);
  event = (eval_event_t *)ckalloc(sizeof(eval_event_t)+n_msg+1);
  event->base.nextPtr = 0;
  event->base.proc = __tcl_script_event;
  event->script = (char *)&event[1];
  memcpy(event->script, msg, n_msg+1);
  event->interp = p->interp;
  Tcl_ThreadQueueEvent(p->parent, (Tcl_Event *)event, TCL_QUEUE_TAIL);
  Tcl_ThreadAlert(p->parent);

  return TCL_OK;
}

static int __x_busy(void *arg, int n_busy){
  UNUSED_PARAMETER(arg);
  UNUSED_PARAMETER(n_busy);
  dbsql_sleep(50);
  return 1;             /* Try again... */
}

/*
** sqlthread open
**
**     Open a database handle and return the string representation of
**     the pointer value.
*/
static int DBSQL_TCLAPI sqlthread_open(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  int __test_make_pointer_str(Tcl_Interp *interp, char *ptr, void *p);

  const char *filename;
  DBSQL *db;
  char buf[100];
  extern int Md5_Register(DBSQL*,char**,const dbsql_api_routines_t*);

  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(objc);

  filename = Tcl_GetString(objv[2]);
  dbsql_open(filename, &db);
  Md5_Register(db, 0, 0);
  dbsql_busy_handler(db, __x_busy, 0);
  
  if( __test_make_pointer_str(interp, buf, db) ) return TCL_ERROR;
  Tcl_AppendResult(interp, buf, NULL);

  return TCL_OK;
}


/*
** sqlthread open
**
**     Return the current thread-id (Tcl_GetCurrentThread()) cast to
**     an integer.
*/
static int DBSQL_TCLAPI sqlthread_id(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_ThreadId id = Tcl_GetCurrentThread();
  Tcl_SetObjResult(interp, Tcl_NewIntObj(DBSQL_PTR_TO_INT(id)));
  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(objc);
  UNUSED_PARAMETER(objv);
  return TCL_OK;
}


/*
** Dispatch routine for the sub-commands of [sqlthread].
*/
static int DBSQL_TCLAPI sqlthread_proc(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  struct sub_command_t {
    char *name;
    Tcl_ObjCmdProc *proc;
    int n_arg;
    char *usage;
  } subs[] = {
    {"parent", sqlthread_parent, 1, "SCRIPT"},
    {"spawn",  sqlthread_spawn,  2, "VARNAME SCRIPT"},
    {"open",   sqlthread_open,   1, "DBNAME"},
    {"id",     sqlthread_id,     0, ""},
    {0, 0, 0}
  };
  struct sub_command_t *sub;
  int rc;
  int i_index;

  if( objc<2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "SUB-COMMAND");
    return TCL_ERROR;
  }

  rc = Tcl_GetIndexFromObjStruct(
      interp, objv[1], subs, sizeof(subs[0]), "sub-command", 0, &i_index
  );
  if( rc!=TCL_OK ) return rc;
  sub = &subs[i_index];

  if( objc<(sub->n_arg+2) ){
    Tcl_WrongNumArgs(interp, 2, objv, sub->usage);
    return TCL_ERROR;
  }

  return sub->proc(clientData, interp, objc, objv);
}

/*
** The [clock_seconds] command. This is more or less the same as the
** regular tcl [clock seconds], except that it is available in testfixture
** when linked against both Tcl 8.4 and 8.5. Because [clock seconds] is
** implemented as a script in Tcl 8.5, it is not usually available to
** testfixture.
*/ 
static int DBSQL_TCLAPI clock_seconds_proc(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_Time now;
  Tcl_GetTime(&now);
  Tcl_SetObjResult(interp, Tcl_NewIntObj(now.sec));
  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(objc);
  UNUSED_PARAMETER(objv);
  return TCL_OK;
}

/*
** The [clock_milliseconds] command. This is more or less the same as the
** regular tcl [clock milliseconds]. 
*/ 
static int DBSQL_TCLAPI clock_milliseconds_proc(
  ClientData clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  Tcl_Time now;
  Tcl_GetTime(&now);
  Tcl_SetObjResult(interp, Tcl_NewWideIntObj(
    ((Tcl_WideInt)now.sec * 1000) + (now.usec / 1000)
  ));
  UNUSED_PARAMETER(clientData);
  UNUSED_PARAMETER(objc);
  UNUSED_PARAMETER(objv);
  return TCL_OK;
}

/*************************************************************************
** This block contains the implementation of the [dbsql_blocking_step]
** command available to threads created by [sqlthread spawn] commands. It
** is only available on UNIX for now. This is because pthread condition
** variables are used.
**
** The source code for the C functions dbsql_blocking_step(),
** blocking_step_notify() and the structure unlock_notification_t is
** automatically extracted from this file and used as part of the
** documentation for the dbsql_unlock_notify() API function. This
** should be considered if these functions are to be extended (i.e. to 
** support windows) in the future.
*/ 
#if DBSQL_OS_UNIX && defined(DBSQL_ENABLE_UNLOCK_NOTIFY)

/* BEGIN_SQLITE_BLOCKING_STEP */
/* This example uses the pthreads API */
#include <pthread.h>

/*
** A pointer to an instance of this structure is passed as the user-context
** pointer when registering for an unlock-notify callback.
*/
typedef struct unlock_notification_t unlock_notification_t;
struct unlock_notification_t {
  int fired;                         /* True after unlock event has occurred */
  pthread_cond_t cond;               /* Condition variable to wait on */
  pthread_mutex_t mutex;             /* Mutex to protect structure */
};

/*
** This function is an unlock-notify callback registered with dbsql.
*/
static void unlock_notify_cb(void **args, int n_arg){
  int i;
  for(i=0; i<n_arg; i++){
    unlock_notification_t *p = (unlock_notification_t *)args[i];
    pthread_mutex_lock(&p->mutex);
    p->fired = 1;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);
  }
}

/*
** This function assumes that an dbsql API call (either dbsql_prepare_v2() 
** or dbsql_step()) has just returned DBSQL_LOCKED. The argument is the
** associated database connection.
**
** This function calls dbsql_unlock_notify() to register for an 
** unlock-notify callback, then blocks until that callback is delivered 
** and returns DBSQL_OK. The caller should then retry the failed operation.
**
** Or, if dbsql_unlock_notify() indicates that to block would deadlock 
** the system, then this function returns DBSQL_LOCKED immediately. In 
** this case the caller should not retry the operation and should roll 
** back the current transaction (if any).
*/
static int wait_for_unlock_notify(DBSQL *db){
  int rc;
  unlock_notification_t un;

  /* Initialize the unlock_notification_t structure. */
  un.fired = 0;
  pthread_mutex_init(&un.mutex, 0);
  pthread_cond_init(&un.cond, 0);

  /* Register for an unlock-notify callback. */
  rc = dbsql_unlock_notify(db, unlock_notify_cb, (void *)&un);
  assert( rc==DBSQL_LOCKED || rc==DBSQL_OK );

  /* The call to dbsql_unlock_notify() always returns either DBSQL_LOCKED 
  ** or DBSQL_OK. 
  **
  ** If DBSQL_LOCKED was returned, then the system is deadlocked. In this
  ** case this function needs to return DBSQL_LOCKED to the caller so 
  ** that the current transaction can be rolled back. Otherwise, block
  ** until the unlock-notify callback is invoked, then return DBSQL_OK.
  */
  if( rc==DBSQL_OK ){
    pthread_mutex_lock(&un.mutex);
    if( !un.fired ){
      pthread_cond_wait(&un.cond, &un.mutex);
    }
    pthread_mutex_unlock(&un.mutex);
  }

  /* Destroy the mutex and condition variables. */
  pthread_cond_destroy(&un.cond);
  pthread_mutex_destroy(&un.mutex);

  return rc;
}

/*
** This function is a wrapper around the dbsql function dbsql_step().
** It functions in the same way as step(), except that if a required
** shared-cache lock cannot be obtained, this function may block waiting for
** the lock to become available. In this scenario the normal API step()
** function always returns DBSQL_LOCKED.
**
** If this function returns DBSQL_LOCKED, the caller should rollback
** the current transaction (if any) and try again later. Otherwise, the
** system may become deadlocked.
*/
int dbsql_blocking_step(dbsql_stmt_t *stmt){
  int rc;
  while( DBSQL_LOCKED==(rc = dbsql_step(stmt)) ){
    rc = wait_for_unlock_notify(dbsql_db_handle(stmt));
    if( rc!=DBSQL_OK ) break;
    dbsql_reset(stmt);
  }
  return rc;
}

/*
** This function is a wrapper around the dbsql function dbsql_prepare_v2().
** It functions in the same way as prepare_v2(), except that if a required
** shared-cache lock cannot be obtained, this function may block waiting for
** the lock to become available. In this scenario the normal API prepare_v2()
** function always returns DBSQL_LOCKED.
**
** If this function returns DBSQL_LOCKED, the caller should rollback
** the current transaction (if any) and try again later. Otherwise, the
** system may become deadlocked.
*/
int dbsql_blocking_prepare_v2(
  DBSQL *db,              /* Database handle. */
  const char *sql,         /* UTF-8 encoded SQL statement. */
  int n_sql,                 /* Length of sql in bytes. */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const char **pz           /* OUT: End of parsed string */
){
  int rc;
  while( DBSQL_LOCKED==(rc = dbsql_prepare_v2(db, sql, n_sql, stmt, pz)) ){
    rc = wait_for_unlock_notify(db);
    if( rc!=DBSQL_OK ) break;
  }
  return rc;
}
/* END_SQLITE_BLOCKING_STEP */

/*
** Usage: dbsql_blocking_step STMT
**
** Advance the statement to the next row.
*/
static int DBSQL_TCLAPI blocking_step_proc(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){

  dbsql_stmt_t *stmt;
  int rc;

  if( objc!=2 ){
    Tcl_WrongNumArgs(interp, 1, objv, "STMT");
    return TCL_ERROR;
  }

  stmt = (dbsql_stmt_t*)__test_text_to_ptr(Tcl_GetString(objv[1]));
  rc = dbsql_blocking_step(stmt);

  Tcl_SetResult(interp, (char *)__err_name(rc), 0);
  return TCL_OK;
}

/*
** Usage: dbsql_blocking_prepare_v2 DB sql bytes ?tailvar?
** Usage: dbsql_nonblocking_prepare_v2 DB sql bytes ?tailvar?
*/
static int DBSQL_TCLAPI blocking_prepare_v2_proc(
  void * clientData,
  Tcl_Interp *interp,
  int objc,
  Tcl_Obj *CONST objv[]
){
  DBSQL *db;
  const char *sql;
  int bytes;
  const char *tail = 0;
  dbsql_stmt_t *stmt = 0;
  char buf[50];
  int rc;
  int is_blocking = !(clientData==0);

  if( objc!=5 && objc!=4 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", 
       Tcl_GetString(objv[0]), " DB sql bytes tailvar", 0);
    return TCL_ERROR;
  }
  if( __get_db_pointer(interp, Tcl_GetString(objv[1]), &db) ) return TCL_ERROR;
  sql = Tcl_GetString(objv[2]);
  if( Tcl_GetIntFromObj(interp, objv[3], &bytes) ) return TCL_ERROR;

  if( is_blocking ){
    rc = dbsql_blocking_prepare_v2(db, sql, bytes, &stmt, &tail);
  }else{
    rc = dbsql_prepare_v2(db, sql, bytes, &stmt, &tail);
  }

  assert(rc==DBSQL_OK || stmt==0);
  if( tail && objc>=5 ){
    if( bytes>=0 ){
      bytes = bytes - (tail-sql);
    }
    Tcl_ObjSetVar2(interp, objv[4], 0, Tcl_NewStringObj(tail, bytes), 0);
  }
  if( rc!=DBSQL_OK ){
    assert( stmt==0 );
    dbsql_snprintf(sizeof(buf), buf, "%s ", (char *)__err_name(rc));
    Tcl_AppendResult(interp, buf, dbsql_errmsg(db), NULL);
    return TCL_ERROR;
  }

  if( stmt ){
    if( __test_make_pointer_str(interp, buf, stmt) ) return TCL_ERROR;
    Tcl_AppendResult(interp, buf, NULL);
  }
  return TCL_OK;
}

#endif /* DBSQL_OS_UNIX && DBSQL_ENABLE_UNLOCK_NOTIFY */
/*
** End of implementation of [dbsql_blocking_step].
************************************************************************/

/*
** Register commands with the TCL interpreter.
*/
int DbsqltestThread_Init(Tcl_Interp *interp){
  struct tcl_cmd_t {
    int (*proc)(void*, Tcl_Interp*, int, Tcl_Obj*const*);
    const char *name;
    int i_ctx;
  } cmds[] = {
    { sqlthread_proc,           "sqlthread",                      0 },
    { clock_seconds_proc,       "clock_second",                   0 },
    { clock_milliseconds_proc,  "clock_milliseconds",             0 },
#if DBSQL_OS_UNIX && defined(DBSQL_ENABLE_UNLOCK_NOTIFY)
    { blocking_step_proc,       "dbsql_blocking_step",          0 },
    { blocking_prepare_v2_proc, "dbsql_blocking_prepare_v2",    1 },
    { blocking_prepare_v2_proc, "dbsql_nonblocking_prepare_v2", 0 },
#endif
  };
  int ii;

  for(ii=0; ii<sizeof(cmds)/sizeof(cmds[0]); ii++){
    void *p = DBSQL_INT_TO_PTR(cmds[ii].i_ctx);
    Tcl_CreateObjCommand(interp, cmds[ii].name, cmds[ii].proc, p, 0);
  }
  return TCL_OK;
}
#else
int DbsqltestThread_Init(Tcl_Interp *interp){
  return TCL_OK;
}
#endif
