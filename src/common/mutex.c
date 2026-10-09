/*
** 2007 August 14
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
** This file contains code that is common across all mutex implementations.
*/
#include "dbsql_int.h"

#if defined(DBSQL_DEBUG) && !defined(DBSQL_MUTEX_OMIT)
/*
** For debugging purposes, record when the mutex subsystem is initialized
** and uninitialized so that we can assert() if there is an attempt to
** allocate a mutex while the system is uninitialized.
*/
static DBSQL_WSD int __mutex_is_init = 0;
#endif /* DBSQL_DEBUG && !defined(DBSQL_MUTEX_OMIT) */


#ifndef DBSQL_MUTEX_OMIT

#ifdef DBSQL_THREAD_MISUSE_WARNINGS
/*
** This block (enclosed by DBSQL_THREAD_MISUSE_WARNINGS) contains
** the implementation of a wrapper around the system default mutex
** implementation (__default_mutex()). 
**
** Most calls are passed directly through to the underlying default
** mutex implementation. Except, if a mutex is configured by calling
** __mutex_warn_on_contention() on it, then if contention is ever
** encountered within mutex_enter() then a warning is emitted via
** dbsql_log().  Furthermore, if DBSQL_THREAD_MISUSE_ABORT is
** defined then abort() is called after the dbsql_log() warning.
**
** This type of mutex is used on the database handle mutex when testing
** apps that usually use DBSQL_CONFIG_MULTITHREAD mode.  A failure
** indicates that the app ought to be using DBSQL_OPEN_FULLMUTEX or
** similar because it is trying to use the same database handle from
** two different connections at the same time.
*/

/* 
** Type for all mutexes used when DBSQL_THREAD_MISUSE_WARNINGS
** is defined. Variable check_mutex_t.mutex is a pointer to the real mutex
** allocated by the system mutex implementation. Variable i_type is usually set
** to the type of mutex requested - DBSQL_MUTEX_RECURSIVE, DBSQL_MUTEX_FAST
** or one of the static mutex identifiers. Or, if this is a recursive mutex
** that has been configured using __mutex_warn_on_contention(), it is
** set to DBSQL_MUTEX_WARNONCONTENTION.
*/
typedef struct check_mutex_t check_mutex_t;
struct check_mutex_t {
  int i_type;
  dbsql_mutex_t *mutex;
};

#define DBSQL_MUTEX_WARNONCONTENTION  (-1)

/* 
** Pointer to real mutex methods object used by the check_mutex_t
** implementation. Set by __check_mutex_init(). 
*/
static DBSQL_WSD const dbsql_mutex_methods_t *__p_global_mutex_methods;

#ifdef DBSQL_DEBUG
static int __check_mutex_held(dbsql_mutex_t *p){
  return __p_global_mutex_methods->mutex_held(((check_mutex_t*)p)->mutex);
}
static int __check_mutex_notheld(dbsql_mutex_t *p){
  return __p_global_mutex_methods->mutex_notheld(((check_mutex_t*)p)->mutex);
}
#endif

/*
** Initialize and deinitialize the mutex subsystem.
*/
static int __check_mutex_init(void){ 
  __p_global_mutex_methods = __default_mutex();
  return __p_global_mutex_methods->mutex_init(); 
}
static int __check_mutex_end(void){ 
  int rc = __p_global_mutex_methods->mutex_end(); 
  __p_global_mutex_methods = 0;
  return rc;
}

/*
** Allocate a mutex.
*/
static dbsql_mutex_t *__check_mutex_alloc(int i_type){
  static check_mutex_t static_mutexes[] = {
    {2, 0}, {3, 0}, {4, 0}, {5, 0},
    {6, 0}, {7, 0}, {8, 0}, {9, 0},
    {10, 0}, {11, 0}, {12, 0}, {13, 0}
  };
  check_mutex_t *p = 0;

  assert( DBSQL_MUTEX_RECURSIVE==1 && DBSQL_MUTEX_FAST==0 );
  if( i_type<2 ){
    p = __malloc_zero(sizeof(check_mutex_t));
    if( p==0 ) return 0;
    p->i_type = i_type;
  }else{
#ifdef DBSQL_ENABLE_API_ARMOR
    if( i_type-2>=ARRAY_SIZE(static_mutexes) ){
      (void)DBSQL_MISUSE_BKPT;
      return 0;
    }
#endif
    p = &static_mutexes[i_type-2];
  }

  if( p->mutex==0 ){
    p->mutex = __p_global_mutex_methods->mutex_alloc(i_type);
    if( p->mutex==0 ){
      if( i_type<2 ){
        dbsql_free(p);
      }
      p = 0;
    }
  }

  return (dbsql_mutex_t*)p;
}

/*
** Free a mutex.
*/
static void __check_mutex_free(dbsql_mutex_t *p){
  assert( DBSQL_MUTEX_RECURSIVE<2 );
  assert( DBSQL_MUTEX_FAST<2 );
  assert( DBSQL_MUTEX_WARNONCONTENTION<2 );

#ifdef DBSQL_ENABLE_API_ARMOR
  if( ((check_mutex_t*)p)->i_type<2 )
#endif
  {
    check_mutex_t *check = (check_mutex_t*)p;
    __p_global_mutex_methods->mutex_free(check->mutex);
    dbsql_free(check);
  }
#ifdef DBSQL_ENABLE_API_ARMOR
  else{
    (void)DBSQL_MISUSE_BKPT;
  }
#endif
}

/*
** Enter the mutex.
*/
static void __check_mutex_enter(dbsql_mutex_t *p){
  check_mutex_t *check = (check_mutex_t*)p;
  if( check->i_type==DBSQL_MUTEX_WARNONCONTENTION ){
    if( DBSQL_OK==__p_global_mutex_methods->mutex_try(check->mutex) ){
      return;
    }
    dbsql_log(DBSQL_MISUSE, 
        "illegal multi-threaded access to database connection"
    );
#if DBSQL_THREAD_MISUSE_ABORT
    abort();
#endif
  }
  __p_global_mutex_methods->mutex_enter(check->mutex);
}

/*
** Enter the mutex (do not block).
*/
static int __check_mutex_try(dbsql_mutex_t *p){
  check_mutex_t *check = (check_mutex_t*)p;
  return __p_global_mutex_methods->mutex_try(check->mutex);
}

/*
** Leave the mutex.
*/
static void __check_mutex_leave(dbsql_mutex_t *p){
  check_mutex_t *check = (check_mutex_t*)p;
  __p_global_mutex_methods->mutex_leave(check->mutex);
}

dbsql_mutex_methods_t const *__multi_threaded_check_mutex(void){
  static const dbsql_mutex_methods_t s_mutex = {
    __check_mutex_init,
    __check_mutex_end,
    __check_mutex_alloc,
    __check_mutex_free,
    __check_mutex_enter,
    __check_mutex_try,
    __check_mutex_leave,
#ifdef DBSQL_DEBUG
    __check_mutex_held,
    __check_mutex_notheld
#else
    0,
    0
#endif
  };
  return &s_mutex;
}

/*
** Mark the DBSQL_MUTEX_RECURSIVE mutex passed as the only argument as
** one on which there should be no contention.
*/
void __mutex_warn_on_contention(dbsql_mutex_t *p){
  if( __global_config.mutex.mutex_alloc==__check_mutex_alloc ){
    check_mutex_t *check = (check_mutex_t*)p;
    assert( check->i_type==DBSQL_MUTEX_RECURSIVE );
    check->i_type = DBSQL_MUTEX_WARNONCONTENTION;
  }
}
#endif   /* ifdef DBSQL_THREAD_MISUSE_WARNINGS */

/*
** Initialize the mutex system.
*/
int __mutex_init(void){ 
  int rc = DBSQL_OK;
  if( !__global_config.mutex.mutex_alloc ){
    /* If the mutex_alloc method has not been set, then the user did not
    ** install a mutex implementation via dbsql_config() prior to 
    ** dbsql_initialize() being called. This block copies pointers to
    ** the default implementation into the __global_config structure.
    */
    dbsql_mutex_methods_t const *from;
    dbsql_mutex_methods_t *to = &__global_config.mutex;

    if( __global_config.core_mutex ){
#ifdef DBSQL_THREAD_MISUSE_WARNINGS
      from = __multi_threaded_check_mutex();
#else
      from = __default_mutex();
#endif
    }else{
      from = __noop_mutex();
    }
    to->mutex_init = from->mutex_init;
    to->mutex_end = from->mutex_end;
    to->mutex_free = from->mutex_free;
    to->mutex_enter = from->mutex_enter;
    to->mutex_try = from->mutex_try;
    to->mutex_leave = from->mutex_leave;
    to->mutex_held = from->mutex_held;
    to->mutex_notheld = from->mutex_notheld;
    __memory_barrier();
    to->mutex_alloc = from->mutex_alloc;
  }
  assert( __global_config.mutex.mutex_init );
  rc = __global_config.mutex.mutex_init();

#ifdef DBSQL_DEBUG
  GLOBAL(int, __mutex_is_init) = 1;
#endif

  __memory_barrier();
  return rc;
}

/*
** Shutdown the mutex system. This call frees resources allocated by
** __mutex_init().
*/
int __mutex_end(void){
  int rc = DBSQL_OK;
  if( __global_config.mutex.mutex_end ){
    rc = __global_config.mutex.mutex_end();
  }

#ifdef DBSQL_DEBUG
  GLOBAL(int, __mutex_is_init) = 0;
#endif

  return rc;
}

/*
** Retrieve a pointer to a static mutex or allocate a new dynamic one.
*/
dbsql_mutex_t *dbsql_mutex_alloc(int id){
#ifndef DBSQL_OMIT_AUTOINIT
  if( id<=DBSQL_MUTEX_RECURSIVE && dbsql_initialize() ) return 0;
  if( id>DBSQL_MUTEX_RECURSIVE && __mutex_init() ) return 0;
#endif
  assert( __global_config.mutex.mutex_alloc );
  return __global_config.mutex.mutex_alloc(id);
}

dbsql_mutex_t *__dbsql_mutex_alloc(int id){
  if( !__global_config.core_mutex ){
    return 0;
  }
  assert( GLOBAL(int, __mutex_is_init) );
  assert( __global_config.mutex.mutex_alloc );
  return __global_config.mutex.mutex_alloc(id);
}

/*
** Free a dynamic mutex.
*/
void dbsql_mutex_free(dbsql_mutex_t *p){
  if( p ){
    assert( __global_config.mutex.mutex_free );
    __global_config.mutex.mutex_free(p);
  }
}

/*
** Obtain the mutex p. If some other thread already has the mutex, block
** until it can be obtained.
*/
void dbsql_mutex_enter(dbsql_mutex_t *p){
  if( p ){
    assert( __global_config.mutex.mutex_enter );
    __global_config.mutex.mutex_enter(p);
  }
}

/*
** Obtain the mutex p. If successful, return DBSQL_OK. Otherwise, if another
** thread holds the mutex and it cannot be obtained, return DBSQL_BUSY.
*/
int dbsql_mutex_try(dbsql_mutex_t *p){
  int rc = DBSQL_OK;
  if( p ){
    assert( __global_config.mutex.mutex_try );
    return __global_config.mutex.mutex_try(p);
  }
  return rc;
}

/*
** The dbsql_mutex_leave() routine exits a mutex that was previously
** entered by the same thread.  The behavior is undefined if the mutex 
** is not currently entered. If a NULL pointer is passed as an argument
** this function is a no-op.
*/
void dbsql_mutex_leave(dbsql_mutex_t *p){
  if( p ){
    assert( __global_config.mutex.mutex_leave );
    __global_config.mutex.mutex_leave(p);
  }
}

#ifndef NDEBUG
/*
** The dbsql_mutex_held() and dbsql_mutex_notheld() routine are
** intended for use inside assert() statements.
**
** Because these routines raise false-positive alerts in TSAN, disable
** them (make them always return 1) when compiling with TSAN.
*/
int dbsql_mutex_held(dbsql_mutex_t *p){
# if defined(__has_feature)
#   if __has_feature(thread_sanitizer)
      p = 0;
#   endif
# endif
  assert( p==0 || __global_config.mutex.mutex_held );
  return p==0 || __global_config.mutex.mutex_held(p);
}
int dbsql_mutex_notheld(dbsql_mutex_t *p){
# if defined(__has_feature)
#   if __has_feature(thread_sanitizer)
      p = 0;
#   endif
# endif
  assert( p==0 || __global_config.mutex.mutex_notheld );
  return p==0 || __global_config.mutex.mutex_notheld(p);
}
#endif /* NDEBUG */

#endif /* !defined(DBSQL_MUTEX_OMIT) */
