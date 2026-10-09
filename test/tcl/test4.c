/*
** 2003 December 18
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Code for testing the dbsql library in a multithreaded environment.
*/
#include "dbsql_int.h"
#include "tcl_dbsql.h"
#if DBSQL_OS_UNIX && DBSQL_THREADSAFE
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <ctype.h>

extern const char *__err_name(int);

/*
** Each thread is controlled by an instance of the following
** structure.
*/
typedef struct dbsql_thread_t dbsql_thread_t;
struct dbsql_thread_t {
  /* The first group of fields are writable by the leader and read-only
  ** to the thread. */
  char *filename;       /* Name of database file */
  void (*op)(dbsql_thread_t*);  /* next operation to do */
  char *arg;            /* argument usable by op */
  int opnum;             /* Operation number */
  int busy;              /* True if this thread is in use */

  /* The next group of fields are writable by the thread but read-only to the
  ** leader. */
  int completed;        /* Number of operations completed */
  DBSQL *db;           /* Open database */
  dbsql_stmt_t *stmt;     /* Pending operation */
  char *err;           /* operation error */
  char *static_err;     /* Static error message */
  int rc;               /* operation return code */
  int argc;             /* number of columns in result */
  const char *argv[100];    /* result columns */
  const char *colv[100];    /* result column names */
};

/*
** There can be as many as 26 threads running at once.  Each is named
** by a capital letter: A, B, C, ..., Y, Z.
*/
#define N_THREAD 26
static dbsql_thread_t threadset[N_THREAD];

static void test_barrier(){
  dbsql_mutex_t *mutex = dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_APP1);
  dbsql_mutex_enter(mutex);
  dbsql_mutex_leave(mutex);
}

/*
** The main loop for a thread.  Threads use busy waiting. 
*/
static void *test_thread_main(void *arg){
  dbsql_thread_t *p = (dbsql_thread_t*)arg;
  if( p->db ){
    dbsql_close(p->db);
  }
  dbsql_open(p->filename, &p->db);
  if( DBSQL_OK!=dbsql_errcode(p->db) ){
    p->err = strdup(dbsql_errmsg(p->db));
    dbsql_close(p->db);
    p->db = 0;
  }
  p->stmt = 0;
  test_barrier();
  p->completed = 1;
  while( p->opnum<=p->completed ) sched_yield();
  test_barrier();
  while( p->op ){
    if( p->err && p->err!=p->static_err ){
      dbsql_free(p->err);
      p->err = 0;
    }
    (*p->op)(p);
    test_barrier();
    p->completed++;
    while( p->opnum<=p->completed ) sched_yield();
    test_barrier();
  }
  if( p->stmt ){
    dbsql_finalize(p->stmt);
    p->stmt = 0;
  }
  if( p->db ){
    dbsql_close(p->db);
    p->db = 0;
  }
  if( p->err && p->err!=p->static_err ){
    dbsql_free(p->err);
    p->err = 0;
  }
  test_barrier();
  p->completed++;
#ifndef DBSQL_OMIT_DEPRECATED
  dbsql_thread_cleanup();
#endif
  return 0;
}

/*
** Get a thread ID which is an upper case letter.  Return the index.
** If the argument is not a valid thread ID put an error message in
** the interpreter and return -1.
*/
static int parse_thread_id(Tcl_Interp *interp, const char *arg){
  if( arg==0 || arg[0]==0 || arg[1]!=0 || !isupper((unsigned char)arg[0]) ){
    Tcl_AppendResult(interp, "thread ID must be an upper case letter", NULL);
    return -1;
  }
  return arg[0] - 'A';
}

/*
** Usage:    thread_create NAME  FILENAME
**
** NAME should be an upper case letter.  Start the thread running with
** an open connection to the given database.
*/
static int DBSQL_TCLAPI tcl_thread_create(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  pthread_t x;
  int rc;

  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID FILENAME", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( threadset[i].busy ){
    Tcl_AppendResult(interp, "thread ", argv[1], " is already running", NULL);
    return TCL_ERROR;
  }
  threadset[i].busy = 1;
  dbsql_free(threadset[i].filename);
  threadset[i].filename = dbsql_mprintf("%s", argv[2]);
  threadset[i].opnum = 1;
  threadset[i].completed = 0;
  rc = pthread_create(&x, 0, test_thread_main, &threadset[i]);
  if( rc ){
    Tcl_AppendResult(interp, "failed to create the thread", NULL);
    dbsql_free(threadset[i].filename);
    threadset[i].busy = 0;
    return TCL_ERROR;
  }
  pthread_detach(x);
  return TCL_OK;
}

/*
** Wait for a thread to reach its idle state.
*/
static void test_thread_wait(dbsql_thread_t *p){
  test_barrier();
  while( p->opnum>p->completed ) sched_yield();
  test_barrier();
}

/*
** Usage:  thread_wait ID
**
** Wait on thread ID to reach its idle state.
*/
static int DBSQL_TCLAPI tcl_thread_wait(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  return TCL_OK;
}

/*
** Stop a thread.
*/
static void test_stop_thread(dbsql_thread_t *p){
  test_thread_wait(p);
  p->op = 0;
  p->opnum++;
  test_thread_wait(p);
  dbsql_free(p->arg);
  p->arg = 0;
  dbsql_free(p->filename);
  p->filename = 0;
  p->busy = 0;
}

/*
** Usage:  thread_halt ID
**
** Cause a thread to shut itself down.  Wait for the shutdown to be
** completed.  If ID is "*" then stop all threads.
*/
static int DBSQL_TCLAPI tcl_thread_halt(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", NULL);
    return TCL_ERROR;
  }
  if( argv[1][0]=='*' && argv[1][1]==0 ){
    for(i=0; i<N_THREAD; i++){
      if( threadset[i].busy ) test_stop_thread(&threadset[i]);
    }
  }else{
    i = parse_thread_id(interp, argv[1]);
    if( i<0 ) return TCL_ERROR;
    if( !threadset[i].busy ){
      Tcl_AppendResult(interp, "no such thread", NULL);
      return TCL_ERROR;
    }
    test_stop_thread(&threadset[i]);
  }
  return TCL_OK;
}

/*
** Usage: thread_argc  ID
**
** Wait on the most recent thread_step to complete, then return the
** number of columns in the result set.
*/
static int DBSQL_TCLAPI tcl_thread_argc(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  char buf[100];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  dbsql_snprintf(sizeof(buf), buf, "%d", threadset[i].argc);
  Tcl_AppendResult(interp, buf, NULL);
  return TCL_OK;
}

/*
** Usage: thread_argv  ID   N
**
** Wait on the most recent thread_step to complete, then return the
** value of the N-th columns in the result set.
*/
static int DBSQL_TCLAPI tcl_thread_argv(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  int n;

  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID N", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[2], &n) ) return TCL_ERROR;
  test_thread_wait(&threadset[i]);
  if( n<0 || n>=threadset[i].argc ){
    Tcl_AppendResult(interp, "column number out of range", NULL);
    return TCL_ERROR;
  }
  Tcl_AppendResult(interp, threadset[i].argv[n], NULL);
  return TCL_OK;
}

/*
** Usage: thread_colname  ID   N
**
** Wait on the most recent thread_step to complete, then return the
** name of the N-th columns in the result set.
*/
static int DBSQL_TCLAPI tcl_thread_colname(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  int n;

  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID N", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[2], &n) ) return TCL_ERROR;
  test_thread_wait(&threadset[i]);
  if( n<0 || n>=threadset[i].argc ){
    Tcl_AppendResult(interp, "column number out of range", NULL);
    return TCL_ERROR;
  }
  Tcl_AppendResult(interp, threadset[i].colv[n], NULL);
  return TCL_OK;
}

/*
** Usage: thread_result  ID
**
** Wait on the most recent operation to complete, then return the
** result code from that operation.
*/
static int DBSQL_TCLAPI tcl_thread_result(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  const char *name;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  name = __err_name(threadset[i].rc);
  Tcl_AppendResult(interp, name, NULL);
  return TCL_OK;
}

/*
** Usage: thread_error  ID
**
** Wait on the most recent operation to complete, then return the
** error string.
*/
static int DBSQL_TCLAPI tcl_thread_error(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  Tcl_AppendResult(interp, threadset[i].err, NULL);
  return TCL_OK;
}

/*
** This procedure runs in the thread to compile an SQL statement.
*/
static void do_compile(dbsql_thread_t *p){
  if( p->db==0 ){
    p->err = p->static_err = "no database is open";
    p->rc = DBSQL_ERROR;
    return;
  }
  if( p->stmt ){
    dbsql_finalize(p->stmt);
    p->stmt = 0;
  }
  p->rc = dbsql_prepare(p->db, p->arg, -1, &p->stmt, 0);
}

/*
** Usage: thread_compile ID SQL
**
** Compile a new virtual machine.
*/
static int DBSQL_TCLAPI tcl_thread_compile(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID SQL", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  threadset[i].op = do_compile;
  dbsql_free(threadset[i].arg);
  threadset[i].arg = dbsql_mprintf("%s", argv[2]);
  test_barrier();
  threadset[i].opnum++;
  return TCL_OK;
}

/*
** This procedure runs in the thread to step the virtual machine.
*/
static void do_step(dbsql_thread_t *p){
  int i;
  if( p->stmt==0 ){
    p->err = p->static_err = "no virtual machine available";
    p->rc = DBSQL_ERROR;
    return;
  }
  p->rc = dbsql_step(p->stmt);
  if( p->rc==DBSQL_ROW ){
    p->argc = dbsql_column_count(p->stmt);
    for(i=0; i<dbsql_data_count(p->stmt); i++){
      p->argv[i] = (char*)dbsql_column_text(p->stmt, i);
    }
    for(i=0; i<p->argc; i++){
      p->colv[i] = dbsql_column_name(p->stmt, i);
    }
  }
}

/*
** Usage: thread_step ID
**
** Advance the virtual machine by one step
*/
static int DBSQL_TCLAPI tcl_thread_step(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " IDL", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  threadset[i].op = do_step;
  test_barrier();
  threadset[i].opnum++;
  return TCL_OK;
}

/*
** This procedure runs in the thread to finalize a virtual machine.
*/
static void do_finalize(dbsql_thread_t *p){
  if( p->stmt==0 ){
    p->err = p->static_err = "no virtual machine available";
    p->rc = DBSQL_ERROR;
    return;
  }
  p->rc = dbsql_finalize(p->stmt);
  p->stmt = 0;
}

/*
** Usage: thread_finalize ID
**
** Finalize the virtual machine.
*/
static int DBSQL_TCLAPI tcl_thread_finalize(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " IDL", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  threadset[i].op = do_finalize;
  dbsql_free(threadset[i].arg);
  threadset[i].arg = 0;
  test_barrier();
  threadset[i].opnum++;
  return TCL_OK;
}

/*
** Usage: thread_swap ID ID
**
** Interchange the sqlite* pointer between two threads.
*/
static int DBSQL_TCLAPI tcl_thread_swap(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i, j;
  DBSQL *temp;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID1 ID2", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  j = parse_thread_id(interp, argv[2]);
  if( j<0 ) return TCL_ERROR;
  if( !threadset[j].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[j]);
  temp = threadset[i].db;
  threadset[i].db = threadset[j].db;
  threadset[j].db = temp;
  return TCL_OK;
}

/*
** Usage: thread_db_get ID
**
** Return the database connection pointer for the given thread.  Then
** remove the pointer from the thread itself.  Afterwards, the thread
** can be stopped and the connection can be used by the main thread.
*/
static int DBSQL_TCLAPI tcl_thread_db_get(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  char buf[100];
  extern int __test_make_pointer_str(Tcl_Interp*, char*, void*);
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  __test_make_pointer_str(interp, buf, threadset[i].db);
  threadset[i].db = 0;
  Tcl_AppendResult(interp, buf, (char*)0);
  return TCL_OK;
}

/*
** Usage: thread_db_put ID DB
**
*/
static int DBSQL_TCLAPI tcl_thread_db_put(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  extern int __test_make_pointer_str(Tcl_Interp*, char*, void*);
  extern void *__test_text_to_ptr(const char *);
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID DB", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  assert( !threadset[i].db );
  threadset[i].db = (DBSQL*)__test_text_to_ptr(argv[2]);
  return TCL_OK;
}

/*
** Usage: thread_stmt_get ID
**
** Return the database stmt pointer for the given thread.  Then
** remove the pointer from the thread itself. 
*/
static int DBSQL_TCLAPI tcl_thread_stmt_get(
  void *__not_used,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  char buf[100];
  extern int __test_make_pointer_str(Tcl_Interp*, char*, void*);
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", NULL);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", NULL);
    return TCL_ERROR;
  }
  test_thread_wait(&threadset[i]);
  __test_make_pointer_str(interp, buf, threadset[i].stmt);
  threadset[i].stmt = 0;
  Tcl_AppendResult(interp, buf, (char*)0);
  return TCL_OK;
}

/*
** Register commands with the TCL interpreter.
*/
int Dbsqltest4_Init(Tcl_Interp *interp){
  static struct {
     char *name;
     Tcl_CmdProc *proc;
  } cmds[] = {
     { "thread_create",     (Tcl_CmdProc*)tcl_thread_create     },
     { "thread_wait",       (Tcl_CmdProc*)tcl_thread_wait       },
     { "thread_halt",       (Tcl_CmdProc*)tcl_thread_halt       },
     { "thread_argc",       (Tcl_CmdProc*)tcl_thread_argc       },
     { "thread_argv",       (Tcl_CmdProc*)tcl_thread_argv       },
     { "thread_colname",    (Tcl_CmdProc*)tcl_thread_colname    },
     { "thread_result",     (Tcl_CmdProc*)tcl_thread_result     },
     { "thread_error",      (Tcl_CmdProc*)tcl_thread_error      },
     { "thread_compile",    (Tcl_CmdProc*)tcl_thread_compile    },
     { "thread_step",       (Tcl_CmdProc*)tcl_thread_step       },
     { "thread_finalize",   (Tcl_CmdProc*)tcl_thread_finalize   },
     { "thread_swap",       (Tcl_CmdProc*)tcl_thread_swap       },
     { "thread_db_get",     (Tcl_CmdProc*)tcl_thread_db_get     },
     { "thread_db_put",     (Tcl_CmdProc*)tcl_thread_db_put     },
     { "thread_stmt_get",   (Tcl_CmdProc*)tcl_thread_stmt_get   },
  };
  int i;

  for(i=0; i<sizeof(cmds)/sizeof(cmds[0]); i++){
    Tcl_CreateCommand(interp, cmds[i].name, cmds[i].proc, 0, 0);
  }
  return TCL_OK;
}
#else
int Dbsqltest4_Init(Tcl_Interp *interp){ return TCL_OK; }
#endif /* DBSQL_OS_UNIX */
