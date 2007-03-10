/*
 * Code for testing the the SQLite library in a multithreaded environment.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"
#include "tcl.h"

#if defined(OS_UNIX) && OS_UNIX==1 && defined(THREADSAFE) && THREADSAFE==1

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <ctype.h>

/*
** Each thread is controlled by an instance of the following
** structure.
*/
typedef struct Thread Thread;
struct Thread {
  /* The first group of fields are writable by the master and read-only
  ** to the thread. */
  char *zFilename;       /* Name of database file */
  void (*xOp)(Thread*);  /* next operation to do */
  char *zArg;            /* argument usable by xOp */
  int opnum;             /* Operation number */
  int busy;              /* True if this thread is in use */

  /* The next group of fields are writable by the thread but read-only to the
  ** master. */
  int completed;        /* Number of operations completed */
  DBSQL *db;           /* Open database */
  sqlvm_t *vm;        /* Pending operation */
  char *zErr;           /* operation error */
  char *zStaticErr;     /* Static error message */
  int rc;               /* operation return code */
  int argc;             /* number of columns in result */
  const char **argv;    /* result columns */
  const char **colv;    /* result column names */
};

/*
** There can be as many as 26 threads running at once.  Each is named
** by a capital letter: A, B, C, ..., Y, Z.
*/
#define N_THREAD 26
static Thread threadset[N_THREAD];


/*
** The main loop for a thread.  Threads use busy waiting. 
*/
static void *thread_main(void *pArg){
  Thread *p = (Thread*)pArg;
  if( p->db ){
    dbsql_close(p->db);
  }
  p->db = sqlite_open(p->zFilename, 0, &p->zErr);
  p->vm = 0;
  p->completed = 1;
  while( p->opnum<=p->completed ) sched_yield();
  while( p->xOp ){
    if( p->zErr && p->zErr!=p->zStaticErr ){
      dbsql_freemem(p->zErr);
      p->zErr = 0;
    }
    (*p->xOp)(p);
    p->completed++;
    while( p->opnum<=p->completed ) sched_yield();
  }
  if( p->vm ){
    dbsql_close_sqlvm(p->vm, 0);
    p->vm = 0;
  }
  if( p->db ){
    dbsql_close(p->db);
    p->db = 0;
  }
  if( p->zErr && p->zErr!=p->zStaticErr ){
    dbsql_freemem(p->zErr);
    p->zErr = 0;
  }
  p->completed++;
  return 0;
}

/*
** Get a thread ID which is an upper case letter.  Return the index.
** If the argument is not a valid thread ID put an error message in
** the interpreter and return -1.
*/
static int parse_thread_id(Tcl_Interp *interp, const char *zArg){
  if( zArg==0 || zArg[0]==0 || zArg[1]!=0 || !isupper(zArg[0]) ){
    Tcl_AppendResult(interp, "thread ID must be an upper case letter", 0);
    return -1;
  }
  return zArg[0] - 'A';
}

/*
** Usage:    thread_create NAME  FILENAME
**
** NAME should be an upper case letter.  Start the thread running with
** an open connection to the given database.
*/
static int tcl_thread_create(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  pthread_t x;
  int rc;

  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID FILENAME", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( threadset[i].busy ){
    Tcl_AppendResult(interp, "thread ", argv[1], " is already running", 0);
    return TCL_ERROR;
  }
  threadset[i].busy = 1;
  __dbsql_free(threadset[i].zFilename);
  threadset[i].zFilename = strdup(argv[2]);
  threadset[i].opnum = 1;
  threadset[i].completed = 0;
  rc = pthread_create(&x, 0, thread_main, &threadset[i]);
  if( rc ){
    Tcl_AppendResult(interp, "failed to create the thread", 0);
    __dbsql_free(threadset[i].zFilename);
    threadset[i].busy = 0;
    return TCL_ERROR;
  }
  pthread_detach(x);
  return TCL_OK;
}

/*
** Wait for a thread to reach its idle state.
*/
static void thread_wait(Thread *p){
  while( p->opnum>p->completed ) sched_yield();
}

/*
** Usage:  thread_wait ID
**
** Wait on thread ID to reach its idle state.
*/
static int tcl_thread_wait(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  return TCL_OK;
}

/*
** Stop a thread.
*/
static void stop_thread(Thread *p){
  thread_wait(p);
  p->xOp = 0;
  p->opnum++;
  thread_wait(p);
  __dbsql_free(p->zArg);
  p->zArg = 0;
  __dbsql_free(p->zFilename);
  p->zFilename = 0;
  p->busy = 0;
}

/*
** Usage:  thread_halt ID
**
** Cause a thread to shut itself down.  Wait for the shutdown to be
** completed.  If ID is "*" then stop all threads.
*/
static int tcl_thread_halt(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", 0);
    return TCL_ERROR;
  }
  if( argv[1][0]=='*' && argv[1][1]==0 ){
    for(i=0; i<N_THREAD; i++){
      if( threadset[i].busy ) stop_thread(&threadset[i]);
    }
  }else{
    i = parse_thread_id(interp, argv[1]);
    if( i<0 ) return TCL_ERROR;
    if( !threadset[i].busy ){
      Tcl_AppendResult(interp, "no such thread", 0);
      return TCL_ERROR;
    }
    stop_thread(&threadset[i]);
  }
  return TCL_OK;
}

/*
** Usage: thread_argc  ID
**
** Wait on the most recent thread_step to complete, then return the
** number of columns in the result set.
*/
static int tcl_thread_argc(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  char zBuf[100];

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  sprintf(zBuf, "%d", threadset[i].argc);
  Tcl_AppendResult(interp, zBuf, 0);
  return TCL_OK;
}

/*
** Usage: thread_argv  ID   N
**
** Wait on the most recent thread_step to complete, then return the
** value of the N-th columns in the result set.
*/
static int tcl_thread_argv(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  int n;

  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID N", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[2], &n) ) return TCL_ERROR;
  thread_wait(&threadset[i]);
  if( n<0 || n>=threadset[i].argc ){
    Tcl_AppendResult(interp, "column number out of range", 0);
    return TCL_ERROR;
  }
  Tcl_AppendResult(interp, threadset[i].argv[n], 0);
  return TCL_OK;
}

/*
** Usage: thread_colname  ID   N
**
** Wait on the most recent thread_step to complete, then return the
** name of the N-th columns in the result set.
*/
static int tcl_thread_colname(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  int n;

  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID N", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  if( Tcl_GetInt(interp, argv[2], &n) ) return TCL_ERROR;
  thread_wait(&threadset[i]);
  if( n<0 || n>=threadset[i].argc ){
    Tcl_AppendResult(interp, "column number out of range", 0);
    return TCL_ERROR;
  }
  Tcl_AppendResult(interp, threadset[i].colv[n], 0);
  return TCL_OK;
}

/*
** Usage: thread_result  ID
**
** Wait on the most recent operation to complete, then return the
** result code from that operation.
*/
static int tcl_thread_result(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  const char *zName;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  switch( threadset[i].rc ){
    case DBSQL_SUCCESS:         zName = "DBSQL_SUCCESS";          break;
    case DBSQL_ERROR:      zName = "DBSQL_ERROR";       break;
    case DBSQL_INTERNAL:   zName = "SQLITE_INTERNAL";    break;
    case DBSQL_PERM:       zName = "SQLITE_PERM";        break;
    case DBSQL_ABORT:      zName = "SQLITE_ABORT";       break;
    case DBSQL_BUSY:       zName = "DBSQL_BUSY";        break;
    case DBSQL_LOCKED:     zName = "SQLITE_LOCKED";      break;
    case DBSQL_NOMEM:      zName = "DBSQL_NOMEM";       break;
    case DBSQL_READONLY:   zName = "DBSQL_READONLY";    break;
    case DBSQL_InterruptED:  zName = "SQLITE_INTERRUPTED";   break;
    case DBSQL_IOERR:      zName = "SQLITE_IOERR";       break;
    case DBSQL_CORRUPT:    zName = "SQLITE_CORRUPT";     break;
    case DBSQL_NOTFOUND:   zName = "SQLITE_NOTFOUND";    break;
    case DBSQL_FULL:       zName = "SQLITE_FULL";        break;
    case DBSQL_CANTOPEN:   zName = "SQLITE_CANTOPEN";    break;
    case DBSQL_PROTOCOL:   zName = "SQLITE_PROTOCOL";    break;
    case DBSQL_EMPTY:      zName = "SQLITE_EMPTY";       break;
    case DBSQL_SCHEMA:     zName = "DBSQL_SCHEMA";      break;
    case DBSQL_TOOBIG:     zName = "SQLITE_TOOBIG";      break;
    case DBSQL_CONSTRAINT: zName = "SQLITE_CONSTRAINT";  break;
    case DBSQL_MISMATCH:   zName = "SQLITE_MISMATCH";    break;
    case DBSQL_MISUSE:     zName = "DBSQL_MISUSE";      break;
    case DBSQL_NOLFS:      zName = "SQLITE_NOLFS";       break;
    case DBSQL_AUTH:       zName = "SQLITE_AUTH";        break;
    case DBSQL_FORMAT:     zName = "DBSQL_FORMAT";      break;
    case DBSQL_RANGE:      zName = "SQLITE_RANGE";       break;
    case DBSQL_ROW:        zName = "DBSQL_ROW";         break;
    case DBSQL_DONE:       zName = "DBSQL_DONE";        break;
    default:                zName = "SQLITE_Unknown";     break;
  }
  Tcl_AppendResult(interp, zName, 0);
  return TCL_OK;
}

/*
** Usage: thread_error  ID
**
** Wait on the most recent operation to complete, then return the
** error string.
*/
static int tcl_thread_error(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;

  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  Tcl_AppendResult(interp, threadset[i].zErr, 0);
  return TCL_OK;
}

/*
** This procedure runs in the thread to compile an SQL statement.
*/
static void do_compile(Thread *p){
  if( p->db==0 ){
    p->zErr = p->zStaticErr = "no database is open";
    p->rc = DBSQL_ERROR;
    return;
  }
  if( p->vm ){
    dbsql_close_sqlvm(p->vm, 0);
    p->vm = 0;
  }
  p->rc = dbsql_compile(p->db, p->zArg, 0, &p->vm, &p->zErr);
}

/*
** Usage: thread_compile ID SQL
**
** Compile a new virtual machine.
*/
static int tcl_thread_compile(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " ID SQL", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  threadset[i].xOp = do_compile;
  __dbsql_free(threadset[i].zArg);
  threadset[i].zArg = strdup(argv[2]);
  threadset[i].opnum++;
  return TCL_OK;
}

/*
** This procedure runs in the thread to step the virtual machine.
*/
static void do_step(Thread *p){
  if( p->vm==0 ){
    p->zErr = p->zStaticErr = "no virtual machine available";
    p->rc = DBSQL_ERROR;
    return;
  }
  p->rc = dbsql_step(p->vm, &p->argc, &p->argv, &p->colv);
}

/*
** Usage: thread_step ID
**
** Advance the virtual machine by one step
*/
static int tcl_thread_step(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " IDL", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  threadset[i].xOp = do_step;
  threadset[i].opnum++;
  return TCL_OK;
}

/*
** This procedure runs in the thread to finalize a virtual machine.
*/
static void do_finalize(Thread *p){
  if( p->vm==0 ){
    p->zErr = p->zStaticErr = "no virtual machine available";
    p->rc = DBSQL_ERROR;
    return;
  }
  p->rc = dbsql_close_sqlvm(p->vm, &p->zErr);
  p->vm = 0;
}

/*
** Usage: thread_finalize ID
**
** Finalize the virtual machine.
*/
static int tcl_thread_finalize(
  void *NotUsed,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  const char **argv      /* Text of each argument */
){
  int i;
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
       " IDL", 0);
    return TCL_ERROR;
  }
  i = parse_thread_id(interp, argv[1]);
  if( i<0 ) return TCL_ERROR;
  if( !threadset[i].busy ){
    Tcl_AppendResult(interp, "no such thread", 0);
    return TCL_ERROR;
  }
  thread_wait(&threadset[i]);
  threadset[i].xOp = do_finalize;
  __dbsql_free(threadset[i].zArg);
  threadset[i].zArg = 0;
  threadset[i].opnum++;
  return TCL_OK;
}

/*
** Register commands with the TCL interpreter.
*/
int __testset_4_init(Tcl_Interp *interp){
  static struct {
     char *zName;
     Tcl_CmdProc *xProc;
  } aCmd[] = {
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
  };
  int i;

  for(i=0; i<sizeof(aCmd)/sizeof(aCmd[0]); i++){
    Tcl_CreateCommand(interp, aCmd[i].zName, aCmd[i].xProc, 0, 0);
  }
  return TCL_OK;
}
#else
int __testset_4_init(Tcl_Interp *interp){ return TCL_OK; }
#endif /* OS_UNIX */
