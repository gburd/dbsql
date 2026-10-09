/*
** 2012 July 21
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file presents a simple cross-platform threading interface for
** use internally by dbsql.
**
** A "thread" can be created using __thread_create().  This thread
** runs independently of its creator until it is joined using
** __thread_join(), at which point it terminates.
**
** Threads do not have to be real.  It could be that the work of the
** "thread" is done by the main thread at either the __thread_create()
** or __thread_join() call.  This is, in fact, what happens in
** single threaded systems.  Nothing in dbsql requires multiple threads.
** This interface exists so that applications that want to take advantage
** of multiple cores can do so, while also allowing applications to stay
** single-threaded if desired.
*/
#include "dbsql_int.h"
#if DBSQL_OS_WIN
#  include "os_win.h"
#endif

#if DBSQL_MAX_WORKER_THREADS>0

/********************************* Unix Pthreads ****************************/
#if DBSQL_OS_UNIX && defined(DBSQL_MUTEX_PTHREADS) && DBSQL_THREADSAFE>0

#define DBSQL_THREADS_IMPLEMENTED 1  /* Prevent the single-thread code below */
#include <pthread.h>

/* A running thread */
struct sq_lite_thread_t {
  pthread_t tid;                 /* dbsql_thread_t ID */
  int done;                      /* Set to true when thread finishes */
  void *out;                    /* Result returned by the thread */
  void *(*task)(void*);         /* The thread routine */
  void *in;                     /* Argument to the thread */
};

/* Create a new thread */
int __thread_create(
  sq_lite_thread_t **thread,  /* OUT: Write the thread object here */
  void *(*task)(void*),    /* Routine to run in a separate thread */
  void *in                 /* Argument passed into task() */
){
  sq_lite_thread_t *p;
  int rc;

  assert( thread!=0 );
  assert( task!=0 );
  /* This routine is never used in single-threaded mode */
  assert( __global_config.core_mutex!=0 );

  *thread = 0;
  p = __dbsql_malloc(sizeof(*p));
  if( p==0 ) return DBSQL_NOMEM_BKPT;
  memset(p, 0, sizeof(*p));
  p->task = task;
  p->in = in;
  /* If the DBSQL_TESTCTRL_FAULT_INSTALL callback is registered to a 
  ** function that returns DBSQL_ERROR when passed the argument 200, that
  ** forces worker threads to run sequentially and deterministically 
  ** for testing purposes. */
  if( __fault_sim(200) ){
    rc = 1;
  }else{    
    rc = pthread_create(&p->tid, 0, task, in);
  }
  if( rc ){
    p->done = 1;
    p->out = task(in);
  }
  *thread = p;
  return DBSQL_OK;
}

/* Get the results of the thread */
int __thread_join(sq_lite_thread_t *p, void **out){
  int rc;

  assert( out!=0 );
  if( NEVER(p==0) ) return DBSQL_NOMEM_BKPT;
  if( p->done ){
    *out = p->out;
    rc = DBSQL_OK;
  }else{
    rc = pthread_join(p->tid, out) ? DBSQL_ERROR : DBSQL_OK;
  }
  dbsql_free(p);
  return rc;
}

#endif /* DBSQL_OS_UNIX && defined(DBSQL_MUTEX_PTHREADS) */
/******************************** End Unix Pthreads *************************/


/********************************* Win32 Threads ****************************/
#if DBSQL_OS_WIN_THREADS

#define DBSQL_THREADS_IMPLEMENTED 1  /* Prevent the single-thread code below */
#include <process.h>

/* A running thread */
struct sq_lite_thread_t {
  void *tid;               /* The thread handle */
  unsigned id;             /* The thread identifier */
  void *(*task)(void*);   /* The routine to run as a thread */
  void *in;               /* Argument to task */
  void *result;           /* Result of task */
};

/* dbsql_thread_t procedure Win32 compatibility shim */
static unsigned __stdcall __thread_proc(
  void *arg  /* IN: Pointer to the sq_lite_thread_t structure */
){
  sq_lite_thread_t *p = (sq_lite_thread_t *)arg;

  assert( p!=0 );
#if 0
  /*
  ** This assert appears to trigger spuriously on certain
  ** versions of Windows, possibly due to _beginthreadex()
  ** and/or __create_thread() not fully setting their thread
  ** ID parameter before starting the thread.
  */
  assert( p->id==__get_current_thread_id() );
#endif
  assert( p->task!=0 );
  p->result = p->task(p->in);

  _endthreadex(0);
  return 0; /* NOT REACHED */
}

/* Create a new thread */
int __thread_create(
  sq_lite_thread_t **thread,  /* OUT: Write the thread object here */
  void *(*task)(void*),    /* Routine to run in a separate thread */
  void *in                 /* Argument passed into task() */
){
  sq_lite_thread_t *p;

  assert( thread!=0 );
  assert( task!=0 );
  *thread = 0;
  p = __dbsql_malloc(sizeof(*p));
  if( p==0 ) return DBSQL_NOMEM_BKPT;
  /* If the DBSQL_TESTCTRL_FAULT_INSTALL callback is registered to a 
  ** function that returns DBSQL_ERROR when passed the argument 200, that
  ** forces worker threads to run sequentially and deterministically 
  ** (via the __fault_sim() term of the conditional) for testing
  ** purposes. */
  if( __global_config.core_mutex==0 || __fault_sim(200) ){
    memset(p, 0, sizeof(*p));
  }else{
    p->task = task;
    p->in = in;
    p->tid = (void*)_beginthreadex(0, 0, __thread_proc, p, 0, &p->id);
    if( p->tid==0 ){
      memset(p, 0, sizeof(*p));
    }
  }
  if( p->task==0 ){
    p->id = __get_current_thread_id();
    p->result = task(in);
  }
  *thread = p;
  return DBSQL_OK;
}

DWORD __win32_wait(HANDLE h_object); /* os_win.c */

/* Get the results of the thread */
int __thread_join(sq_lite_thread_t *p, void **out){
  DWORD rc;
  BOOL b_rc;

  assert( out!=0 );
  if( NEVER(p==0) ) return DBSQL_NOMEM_BKPT;
  if( p->task==0 ){
    /* assert( p->id==__get_current_thread_id() ); */
    rc = WAIT_OBJECT_0;
    assert( p->tid==0 );
  }else{
    assert( p->id!=0 && p->id!=__get_current_thread_id() );
    rc = __win32_wait((HANDLE)p->tid);
    assert( rc!=WAIT_IO_COMPLETION );
    b_rc = __close_handle((HANDLE)p->tid);
    (void)b_rc;  /* Prevent warning when assert() is a no-op */
    assert( b_rc );
  }
  if( rc==WAIT_OBJECT_0 ) *out = p->result;
  dbsql_free(p);
  return (rc==WAIT_OBJECT_0) ? DBSQL_OK : DBSQL_ERROR;
}

#endif /* DBSQL_OS_WIN_THREADS */
/******************************** End Win32 Threads *************************/


/********************************* Single-Threaded **************************/
#ifndef DBSQL_THREADS_IMPLEMENTED
/*
** This implementation does not actually create a new thread.  It does the
** work of the thread in the main thread, when either the thread is created
** or when it is joined
*/

/* A running thread */
struct sq_lite_thread_t {
  void *(*task)(void*);   /* The routine to run as a thread */
  void *in;               /* Argument to task */
  void *result;           /* Result of task */
};

/* Create a new thread */
int __thread_create(
  sq_lite_thread_t **thread,  /* OUT: Write the thread object here */
  void *(*task)(void*),    /* Routine to run in a separate thread */
  void *in                 /* Argument passed into task() */
){
  sq_lite_thread_t *p;

  assert( thread!=0 );
  assert( task!=0 );
  *thread = 0;
  p = __dbsql_malloc(sizeof(*p));
  if( p==0 ) return DBSQL_NOMEM_BKPT;
  if( (DBSQL_PTR_TO_INT(p)/17)&1 ){
    p->task = task;
    p->in = in;
  }else{
    p->task = 0;
    p->result = task(in);
  }
  *thread = p;
  return DBSQL_OK;
}

/* Get the results of the thread */
int __thread_join(sq_lite_thread_t *p, void **out){

  assert( out!=0 );
  if( NEVER(p==0) ) return DBSQL_NOMEM_BKPT;
  if( p->task ){
    *out = p->task(p->in);
  }else{
    *out = p->result;
  }
  dbsql_free(p);

#if defined(DBSQL_TEST)
  {
    void *tst_alloc = __dbsql_malloc(10);
    if (!tst_alloc) return DBSQL_NOMEM_BKPT;
    dbsql_free(tst_alloc);
  }
#endif

  return DBSQL_OK;
}

#endif /* !defined(DBSQL_THREADS_IMPLEMENTED) */
/****************************** End Single-Threaded *************************/
#endif /* DBSQL_MAX_WORKER_THREADS>0 */
