/*
** 2008 October 07
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the C functions that implement mutexes.
**
** This implementation in this file does not provide any mutual
** exclusion and is thus suitable for use only in applications
** that use dbsql in a single thread.  The routines defined
** here are place-holders.  Applications can substitute working
** mutex routines at start-time using the
**
**     dbsql_config(DBSQL_CONFIG_MUTEX,...)
**
** interface.
**
** If compiled with DBSQL_DEBUG, then additional logic is inserted
** that does error checking on mutexes to make sure they are being
** called correctly.
*/
#include "dbsql_int.h"

#ifndef DBSQL_MUTEX_OMIT

#ifndef DBSQL_DEBUG
/*
** Stub routines for all mutex methods.
**
** This routines provide no mutual exclusion or error checking.
*/
static int __noop_mutex_init(void){ return DBSQL_OK; }
static int __noop_mutex_end(void){ return DBSQL_OK; }
static dbsql_mutex_t *__noop_mutex_alloc(int id){ 
  UNUSED_PARAMETER(id);
  return (dbsql_mutex_t*)8; 
}
static void __noop_mutex_free(dbsql_mutex_t *p){ UNUSED_PARAMETER(p); return; }
static void __noop_mutex_enter(dbsql_mutex_t *p){ UNUSED_PARAMETER(p); return; }
static int __noop_mutex_try(dbsql_mutex_t *p){
  UNUSED_PARAMETER(p);
  return DBSQL_OK;
}
static void __noop_mutex_leave(dbsql_mutex_t *p){ UNUSED_PARAMETER(p); return; }

dbsql_mutex_methods_t const *__noop_mutex(void){
  static const dbsql_mutex_methods_t s_mutex = {
    __noop_mutex_init,
    __noop_mutex_end,
    __noop_mutex_alloc,
    __noop_mutex_free,
    __noop_mutex_enter,
    __noop_mutex_try,
    __noop_mutex_leave,

    0,
    0,
  };

  return &s_mutex;
}
#endif /* !DBSQL_DEBUG */

#ifdef DBSQL_DEBUG
/*
** In this implementation, error checking is provided for testing
** and debugging purposes.  The mutexes still do not provide any
** mutual exclusion.
*/

/*
** The mutex object
*/
typedef struct dbsql_debug_mutex {
  int id;     /* The mutex type */
  int cnt;    /* Number of entries without a matching leave */
} dbsql_debug_mutex;

/*
** The dbsql_mutex_held() and dbsql_mutex_notheld() routine are
** intended for use inside assert() statements.
*/
static int __debug_mutex_held(dbsql_mutex_t *x){
  dbsql_debug_mutex *p = (dbsql_debug_mutex*)x;
  return p==0 || p->cnt>0;
}
static int __debug_mutex_notheld(dbsql_mutex_t *x){
  dbsql_debug_mutex *p = (dbsql_debug_mutex*)x;
  return p==0 || p->cnt==0;
}

/*
** Initialize and deinitialize the mutex subsystem.
*/
static int __debug_mutex_init(void){ return DBSQL_OK; }
static int __debug_mutex_end(void){ return DBSQL_OK; }

/*
** The dbsql_mutex_alloc() routine allocates a new
** mutex and returns a pointer to it.  If it returns NULL
** that means that a mutex could not be allocated. 
*/
static dbsql_mutex_t *__debug_mutex_alloc(int id){
  static dbsql_debug_mutex statics[DBSQL_MUTEX_STATIC_VFS3 - 1];
  dbsql_debug_mutex *p_new = 0;
  switch( id ){
    case DBSQL_MUTEX_FAST:
    case DBSQL_MUTEX_RECURSIVE: {
      p_new = __dbsql_malloc(sizeof(*p_new));
      if( p_new ){
        p_new->id = id;
        p_new->cnt = 0;
      }
      break;
    }
    default: {
#ifdef DBSQL_ENABLE_API_ARMOR
      if( id-2<0 || id-2>=ARRAY_SIZE(statics) ){
        (void)DBSQL_MISUSE_BKPT;
        return 0;
      }
#endif
      p_new = &statics[id-2];
      p_new->id = id;
      break;
    }
  }
  return (dbsql_mutex_t*)p_new;
}

/*
** This routine deallocates a previously allocated mutex.
*/
static void __debug_mutex_free(dbsql_mutex_t *x){
  dbsql_debug_mutex *p = (dbsql_debug_mutex*)x;
  assert( p->cnt==0 );
  if( p->id==DBSQL_MUTEX_RECURSIVE || p->id==DBSQL_MUTEX_FAST ){
    dbsql_free(p);
  }else{
#ifdef DBSQL_ENABLE_API_ARMOR
    (void)DBSQL_MISUSE_BKPT;
#endif
  }
}

/*
** The dbsql_mutex_enter() and dbsql_mutex_try() routines attempt
** to enter a mutex.  If another thread is already within the mutex,
** dbsql_mutex_enter() will block and dbsql_mutex_try() will return
** DBSQL_BUSY.  The dbsql_mutex_try() interface returns DBSQL_OK
** upon successful entry.  Mutexes created using DBSQL_MUTEX_RECURSIVE can
** be entered multiple times by the same thread.  In such cases the,
** mutex must be exited an equal number of times before another thread
** can enter.  If the same thread tries to enter any other kind of mutex
** more than once, the behavior is undefined.
*/
static void __debug_mutex_enter(dbsql_mutex_t *x){
  dbsql_debug_mutex *p = (dbsql_debug_mutex*)x;
  assert( p->id==DBSQL_MUTEX_RECURSIVE || __debug_mutex_notheld(x) );
  p->cnt++;
}
static int __debug_mutex_try(dbsql_mutex_t *x){
  dbsql_debug_mutex *p = (dbsql_debug_mutex*)x;
  assert( p->id==DBSQL_MUTEX_RECURSIVE || __debug_mutex_notheld(x) );
  p->cnt++;
  return DBSQL_OK;
}

/*
** The dbsql_mutex_leave() routine exits a mutex that was
** previously entered by the same thread.  The behavior
** is undefined if the mutex is not currently entered or
** is not currently allocated.  dbsql will never do either.
*/
static void __debug_mutex_leave(dbsql_mutex_t *x){
  dbsql_debug_mutex *p = (dbsql_debug_mutex*)x;
  assert( __debug_mutex_held(x) );
  p->cnt--;
  assert( p->id==DBSQL_MUTEX_RECURSIVE || __debug_mutex_notheld(x) );
}

dbsql_mutex_methods_t const *__noop_mutex(void){
  static const dbsql_mutex_methods_t s_mutex = {
    __debug_mutex_init,
    __debug_mutex_end,
    __debug_mutex_alloc,
    __debug_mutex_free,
    __debug_mutex_enter,
    __debug_mutex_try,
    __debug_mutex_leave,

    __debug_mutex_held,
    __debug_mutex_notheld
  };

  return &s_mutex;
}
#endif /* DBSQL_DEBUG */

/*
** If compiled with DBSQL_MUTEX_NOOP, then the no-op mutex implementation
** is used regardless of the run-time threadsafety setting.
*/
#ifdef DBSQL_MUTEX_NOOP
dbsql_mutex_methods_t const *__default_mutex(void){
  return __noop_mutex();
}
#endif /* defined(DBSQL_MUTEX_NOOP) */
#endif /* !defined(DBSQL_MUTEX_OMIT) */
