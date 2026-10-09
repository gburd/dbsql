/*
** 2007 August 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the C functions that implement mutexes for pthreads
*/
#include "dbsql_int.h"

/*
** The code in this file is only used if we are compiling threadsafe
** under unix with pthreads.
**
** Note that this implementation requires a version of pthreads that
** supports recursive mutexes.
*/
#ifdef DBSQL_MUTEX_PTHREADS

#include <pthread.h>

/*
** The dbsql_mutex_t.id, dbsql_mutex_t.n_ref, and dbsql_mutex_t.owner fields
** are necessary under two conditions:  (1) Debug builds and (2) using
** home-grown mutexes.  Encapsulate these conditions into a single #define.
*/
#if defined(DBSQL_DEBUG) || defined(DBSQL_HOMEGROWN_RECURSIVE_MUTEX)
# define DBSQL_MUTEX_NREF 1
#else
# define DBSQL_MUTEX_NREF 0
#endif

/*
** Each recursive mutex is an instance of the following structure.
*/
struct dbsql_mutex_t {
  pthread_mutex_t mutex;     /* Mutex controlling the lock */
#if DBSQL_MUTEX_NREF || defined(DBSQL_ENABLE_API_ARMOR)
  int id;                    /* Mutex type */
#endif
#if DBSQL_MUTEX_NREF
  volatile int n_ref;         /* Number of entrances */
  volatile pthread_t owner;  /* dbsql_thread_t that is within this mutex */
  int trace;                 /* True to trace changes */
#endif
};
#if DBSQL_MUTEX_NREF
# define DBSQL_MUTEX_INITIALIZER(id) \
     {PTHREAD_MUTEX_INITIALIZER,id,0,(pthread_t)0,0}
#elif defined(DBSQL_ENABLE_API_ARMOR)
# define DBSQL_MUTEX_INITIALIZER(id) { PTHREAD_MUTEX_INITIALIZER, id }
#else
#define DBSQL_MUTEX_INITIALIZER(id) { PTHREAD_MUTEX_INITIALIZER }
#endif

/*
** The dbsql_mutex_held() and dbsql_mutex_notheld() routine are
** intended for use only inside assert() statements.  On some platforms,
** there might be race conditions that can cause these routines to
** deliver incorrect results.  In particular, if pthread_equal() is
** not an atomic operation, then these routines might delivery
** incorrect results.  On most platforms, pthread_equal() is a
** comparison of two integers and is therefore atomic.  But we are
** told that HPUX is not such a platform.  If so, then these routines
** will not always work correctly on HPUX.
**
** On those platforms where pthread_equal() is not atomic, dbsql
** should be compiled without -DSQLITE_DEBUG and with -DNDEBUG to
** make sure no assert() statements are evaluated and hence these
** routines are never called.
*/
#if !defined(NDEBUG) || defined(DBSQL_DEBUG)
static int __pthread_mutex_held(dbsql_mutex_t *p){
  return (p->n_ref!=0 && pthread_equal(p->owner, pthread_self()));
}
static int __pthread_mutex_notheld(dbsql_mutex_t *p){
  return p->n_ref==0 || pthread_equal(p->owner, pthread_self())==0;
}
#endif

/*
** Try to provide a memory barrier operation, needed for initialization
** and also for the implementation of shm_barrier in the VFS in cases
** where dbsql is compiled without mutexes.
*/
void __memory_barrier(void){
#if defined(DBSQL_MEMORY_BARRIER)
  DBSQL_MEMORY_BARRIER;
#elif defined(__GNUC__) && GCC_VERSION>=4001000
  __sync_synchronize();
#endif
}

/*
** Initialize and deinitialize the mutex subsystem.
*/
static int __dbsql_pthread_mutex_init(void){ return DBSQL_OK; }
static int __pthread_mutex_end(void){ return DBSQL_OK; }

/*
** The dbsql_mutex_alloc() routine allocates a new
** mutex and returns a pointer to it.  If it returns NULL
** that means that a mutex could not be allocated.  dbsql
** will unwind its stack and return an error.  The argument
** to dbsql_mutex_alloc() is one of these integer constants:
**
** <ul>
** <li>  DBSQL_MUTEX_FAST
** <li>  DBSQL_MUTEX_RECURSIVE
** <li>  DBSQL_MUTEX_STATIC_MAIN
** <li>  DBSQL_MUTEX_STATIC_MEM
** <li>  DBSQL_MUTEX_STATIC_OPEN
** <li>  DBSQL_MUTEX_STATIC_PRNG
** <li>  DBSQL_MUTEX_STATIC_LRU
** <li>  DBSQL_MUTEX_STATIC_PMEM
** <li>  DBSQL_MUTEX_STATIC_APP1
** <li>  DBSQL_MUTEX_STATIC_APP2
** <li>  DBSQL_MUTEX_STATIC_APP3
** <li>  DBSQL_MUTEX_STATIC_VFS1
** <li>  DBSQL_MUTEX_STATIC_VFS2
** <li>  DBSQL_MUTEX_STATIC_VFS3
** </ul>
**
** The first two constants cause dbsql_mutex_alloc() to create
** a new mutex.  The new mutex is recursive when DBSQL_MUTEX_RECURSIVE
** is used but not necessarily so when DBSQL_MUTEX_FAST is used.
** The mutex implementation does not need to make a distinction
** between DBSQL_MUTEX_RECURSIVE and DBSQL_MUTEX_FAST if it does
** not want to.  But dbsql will only request a recursive mutex in
** cases where it really needs one.  If a faster non-recursive mutex
** implementation is available on the host platform, the mutex subsystem
** might return such a mutex in response to DBSQL_MUTEX_FAST.
**
** The other allowed parameters to dbsql_mutex_alloc() each return
** a pointer to a static preexisting mutex.  Six static mutexes are
** used by the current version of dbsql.  Future versions of dbsql
** may add additional static mutexes.  Static mutexes are for internal
** use by dbsql only.  Applications that use dbsql mutexes should
** use only the dynamic mutexes returned by DBSQL_MUTEX_FAST or
** DBSQL_MUTEX_RECURSIVE.
**
** Note that if one of the dynamic mutex parameters (DBSQL_MUTEX_FAST
** or DBSQL_MUTEX_RECURSIVE) is used then dbsql_mutex_alloc()
** returns a different mutex on every call.  But for the static
** mutex types, the same mutex is returned on every call that has
** the same type number.
*/
static dbsql_mutex_t *__pthread_mutex_alloc(int i_type){
  static dbsql_mutex_t static_mutexes[] = {
    DBSQL_MUTEX_INITIALIZER(2),
    DBSQL_MUTEX_INITIALIZER(3),
    DBSQL_MUTEX_INITIALIZER(4),
    DBSQL_MUTEX_INITIALIZER(5),
    DBSQL_MUTEX_INITIALIZER(6),
    DBSQL_MUTEX_INITIALIZER(7),
    DBSQL_MUTEX_INITIALIZER(8),
    DBSQL_MUTEX_INITIALIZER(9),
    DBSQL_MUTEX_INITIALIZER(10),
    DBSQL_MUTEX_INITIALIZER(11),
    DBSQL_MUTEX_INITIALIZER(12),
    DBSQL_MUTEX_INITIALIZER(13)
  };
  dbsql_mutex_t *p;
  switch( i_type ){
    case DBSQL_MUTEX_RECURSIVE: {
      p = __malloc_zero( sizeof(*p) );
      if( p ){
#ifdef DBSQL_HOMEGROWN_RECURSIVE_MUTEX
        /* If recursive mutexes are not available, we will have to
        ** build our own.  See below. */
        pthread_mutex_init(&p->mutex, 0);
#else
        /* Use a recursive mutex if it is available */
        pthread_mutexattr_t recursive_attr;
        pthread_mutexattr_init(&recursive_attr);
        pthread_mutexattr_settype(&recursive_attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&p->mutex, &recursive_attr);
        pthread_mutexattr_destroy(&recursive_attr);
#endif
#if DBSQL_MUTEX_NREF || defined(DBSQL_ENABLE_API_ARMOR)
        p->id = DBSQL_MUTEX_RECURSIVE;
#endif
      }
      break;
    }
    case DBSQL_MUTEX_FAST: {
      p = __malloc_zero( sizeof(*p) );
      if( p ){
        pthread_mutex_init(&p->mutex, 0);
#if DBSQL_MUTEX_NREF || defined(DBSQL_ENABLE_API_ARMOR)
        p->id = DBSQL_MUTEX_FAST;
#endif
      }
      break;
    }
    default: {
#ifdef DBSQL_ENABLE_API_ARMOR
      if( i_type-2<0 || i_type-2>=ARRAY_SIZE(static_mutexes) ){
        (void)DBSQL_MISUSE_BKPT;
        return 0;
      }
#endif
      p = &static_mutexes[i_type-2];
      break;
    }
  }
#if DBSQL_MUTEX_NREF || defined(DBSQL_ENABLE_API_ARMOR)
  assert( p==0 || p->id==i_type );
#endif
  return p;
}


/*
** This routine deallocates a previously
** allocated mutex.  dbsql is careful to deallocate every
** mutex that it allocates.
*/
static void __pthread_mutex_free(dbsql_mutex_t *p){
  assert( p->n_ref==0 );
#ifdef DBSQL_ENABLE_API_ARMOR
  if( p->id==DBSQL_MUTEX_FAST || p->id==DBSQL_MUTEX_RECURSIVE )
#endif
  {
    pthread_mutex_destroy(&p->mutex);
    dbsql_free(p);
  }
#ifdef DBSQL_ENABLE_API_ARMOR
  else{
    (void)DBSQL_MISUSE_BKPT;
  }
#endif
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
static void __pthread_mutex_enter(dbsql_mutex_t *p){
  assert( p->id==DBSQL_MUTEX_RECURSIVE || __pthread_mutex_notheld(p) );

#ifdef DBSQL_HOMEGROWN_RECURSIVE_MUTEX
  /* If recursive mutexes are not available, then we have to grow
  ** our own.  This implementation assumes that pthread_equal()
  ** is atomic - that it cannot be deceived into thinking self
  ** and p->owner are equal if p->owner changes between two values
  ** that are not equal to self while the comparison is taking place.
  ** This implementation also assumes a coherent cache - that
  ** separate processes cannot read different values from the same
  ** address at the same time.  If either of these two conditions
  ** are not met, then the mutexes will fail and problems will result.
  */
  {
    pthread_t self = pthread_self();
    if( p->n_ref>0 && pthread_equal(p->owner, self) ){
      p->n_ref++;
    }else{
      pthread_mutex_lock(&p->mutex);
      assert( p->n_ref==0 );
      p->owner = self;
      p->n_ref = 1;
    }
  }
#else
  /* Use the built-in recursive mutexes if they are available.
  */
  pthread_mutex_lock(&p->mutex);
#if DBSQL_MUTEX_NREF
  assert( p->n_ref>0 || p->owner==0 );
  p->owner = pthread_self();
  p->n_ref++;
#endif
#endif

#ifdef DBSQL_DEBUG
  if( p->trace ){
    printf("enter mutex %p (%d) with nRef=%d\n", p, p->trace, p->n_ref);
  }
#endif
}
static int __pthread_mutex_try(dbsql_mutex_t *p){
  int rc;
  assert( p->id==DBSQL_MUTEX_RECURSIVE || __pthread_mutex_notheld(p) );

#ifdef DBSQL_HOMEGROWN_RECURSIVE_MUTEX
  /* If recursive mutexes are not available, then we have to grow
  ** our own.  This implementation assumes that pthread_equal()
  ** is atomic - that it cannot be deceived into thinking self
  ** and p->owner are equal if p->owner changes between two values
  ** that are not equal to self while the comparison is taking place.
  ** This implementation also assumes a coherent cache - that
  ** separate processes cannot read different values from the same
  ** address at the same time.  If either of these two conditions
  ** are not met, then the mutexes will fail and problems will result.
  */
  {
    pthread_t self = pthread_self();
    if( p->n_ref>0 && pthread_equal(p->owner, self) ){
      p->n_ref++;
      rc = DBSQL_OK;
    }else if( pthread_mutex_trylock(&p->mutex)==0 ){
      assert( p->n_ref==0 );
      p->owner = self;
      p->n_ref = 1;
      rc = DBSQL_OK;
    }else{
      rc = DBSQL_BUSY;
    }
  }
#else
  /* Use the built-in recursive mutexes if they are available.
  */
  if( pthread_mutex_trylock(&p->mutex)==0 ){
#if DBSQL_MUTEX_NREF
    p->owner = pthread_self();
    p->n_ref++;
#endif
    rc = DBSQL_OK;
  }else{
    rc = DBSQL_BUSY;
  }
#endif

#ifdef DBSQL_DEBUG
  if( rc==DBSQL_OK && p->trace ){
    printf("enter mutex %p (%d) with nRef=%d\n", p, p->trace, p->n_ref);
  }
#endif
  return rc;
}

/*
** The dbsql_mutex_leave() routine exits a mutex that was
** previously entered by the same thread.  The behavior
** is undefined if the mutex is not currently entered or
** is not currently allocated.  dbsql will never do either.
*/
static void __pthread_mutex_leave(dbsql_mutex_t *p){
  assert( __pthread_mutex_held(p) );
#if DBSQL_MUTEX_NREF
  p->n_ref--;
  if( p->n_ref==0 ) p->owner = 0;
#endif
  assert( p->n_ref==0 || p->id==DBSQL_MUTEX_RECURSIVE );

#ifdef DBSQL_HOMEGROWN_RECURSIVE_MUTEX
  if( p->n_ref==0 ){
    pthread_mutex_unlock(&p->mutex);
  }
#else
  pthread_mutex_unlock(&p->mutex);
#endif

#ifdef DBSQL_DEBUG
  if( p->trace ){
    printf("leave mutex %p (%d) with nRef=%d\n", p, p->trace, p->n_ref);
  }
#endif
}

dbsql_mutex_methods_t const *__default_mutex(void){
  static const dbsql_mutex_methods_t s_mutex = {
    __dbsql_pthread_mutex_init,
    __pthread_mutex_end,
    __pthread_mutex_alloc,
    __pthread_mutex_free,
    __pthread_mutex_enter,
    __pthread_mutex_try,
    __pthread_mutex_leave,
#ifdef DBSQL_DEBUG
    __pthread_mutex_held,
    __pthread_mutex_notheld
#else
    0,
    0
#endif
  };

  return &s_mutex;
}

#endif /* DBSQL_MUTEX_PTHREADS */
