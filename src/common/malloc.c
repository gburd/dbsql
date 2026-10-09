/*
** 2001 September 15
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
** Memory allocation functions used throughout sqlite.
*/
#include "dbsql_int.h"
#include <stdarg.h>

/*
** Attempt to release up to n bytes of non-essential memory currently
** held by dbsql. An example of non-essential memory is memory used to
** cache database pages that are not currently in use.
*/
int dbsql_release_memory(int n){
#ifdef DBSQL_ENABLE_MEMORY_MANAGEMENT
  return __pcache_release_memory(n);
#else
  /* IMPLEMENTATION-OF: R-34391-24921 The dbsql_release_memory() routine
  ** is a no-op returning zero if dbsql is not compiled with
  ** DBSQL_ENABLE_MEMORY_MANAGEMENT. */
  UNUSED_PARAMETER(n);
  return 0;
#endif
}

/*
** Default value of the hard heap limit.  0 means "no limit".
*/
#ifndef DBSQL_MAX_MEMORY
# define DBSQL_MAX_MEMORY 0
#endif

/*
** State information local to the memory allocation subsystem.
*/
static DBSQL_WSD struct mem0_global_t {
  dbsql_mutex_t *mutex;         /* Mutex to serialize access */
  int64_t alarm_threshold; /* The soft heap limit */
  int64_t hard_limit;      /* The hard upper bound on memory */

  /*
  ** True if heap is nearly "full" where "full" is defined by the
  ** dbsql_soft_heap_limit() setting.
  */
  int nearly_full;
} mem0 = { 0, DBSQL_MAX_MEMORY, DBSQL_MAX_MEMORY, 0 };

#define mem0 GLOBAL(struct mem0_global_t, mem0)

/*
** Return the memory allocator mutex. dbsql_status() needs it.
*/
dbsql_mutex_t *__malloc_mutex(void){
  return mem0.mutex;
}

#ifndef DBSQL_OMIT_DEPRECATED
/*
** Deprecated external interface.  It used to set an alarm callback
** that was invoked when memory usage grew too large.  Now it is a
** no-op.
*/
int dbsql_memory_alarm(
  void(*callback)(void *arg, int64_t used,int N),
  void *arg,
  int64_t i_threshold
){
  (void)callback;
  (void)arg;
  (void)i_threshold;
  return DBSQL_OK;
}
#endif

/*
** Set the soft heap-size limit for the library.  An argument of
** zero disables the limit.  A negative argument is a no-op used to
** obtain the return value.
**
** The return value is the value of the heap limit just before this
** interface was called.
**
** If the hard heap limit is enabled, then the soft heap limit cannot
** be disabled nor raised above the hard heap limit.
*/
int64_t dbsql_soft_heap_limit64(int64_t n){
  int64_t prior_limit;
  int64_t excess;
  int64_t n_used;
#ifndef DBSQL_OMIT_AUTOINIT
  int rc = dbsql_initialize();
  if( rc ) return -1;
#endif
  dbsql_mutex_enter(mem0.mutex);
  prior_limit = mem0.alarm_threshold;
  if( n<0 ){
    dbsql_mutex_leave(mem0.mutex);
    return prior_limit;
  }
  if( mem0.hard_limit>0 && (n>mem0.hard_limit || n==0) ){
    n = mem0.hard_limit;
  }
  mem0.alarm_threshold = n;
  n_used = __status_value(DBSQL_STATUS_MEMORY_USED);
  ATOMIC_STORE(&mem0.nearly_full, n>0 && n<=n_used);
  dbsql_mutex_leave(mem0.mutex);
  excess = dbsql_memory_used() - n;
  if( excess>0 ) dbsql_release_memory((int)(excess & 0x7fffffff));
  return prior_limit;
}
void dbsql_soft_heap_limit(int n){
  if( n<0 ) n = 0;
  dbsql_soft_heap_limit64(n);
}

/*
** Set the hard heap-size limit for the library. An argument of zero
** disables the hard heap limit.  A negative argument is a no-op used
** to obtain the return value without affecting the hard heap limit.
**
** The return value is the value of the hard heap limit just prior to
** calling this interface.
**
** Setting the hard heap limit will also activate the soft heap limit
** and constrain the soft heap limit to be no more than the hard heap
** limit.
*/
int64_t dbsql_hard_heap_limit64(int64_t n){
  int64_t prior_limit;
#ifndef DBSQL_OMIT_AUTOINIT
  int rc = dbsql_initialize();
  if( rc ) return -1;
#endif
  dbsql_mutex_enter(mem0.mutex);
  prior_limit = mem0.hard_limit;
  if( n>=0 ){
    mem0.hard_limit = n;
    if( n<mem0.alarm_threshold || mem0.alarm_threshold==0 ){
      mem0.alarm_threshold = n;
    }
  }
  dbsql_mutex_leave(mem0.mutex);
  return prior_limit;
}


/*
** Initialize the memory allocation subsystem.
*/
int __malloc_init(void){
  int rc;
  if( __global_config.m.x_malloc==0 ){
    __mem_set_default();
  }
  mem0.mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MEM);
  if( __global_config.page==0 || __global_config.sz_page<512
      || __global_config.n_page<=0 ){
    __global_config.page = 0;
    __global_config.sz_page = 0;
  }
  rc = __global_config.m.init(__global_config.m.app_data);
  if( rc!=DBSQL_OK ) memset(&mem0, 0, sizeof(mem0));
  return rc;
}

/*
** Return true if the heap is currently under memory pressure - in other
** words if the amount of heap used is close to the limit set by
** dbsql_soft_heap_limit().
*/
int __heap_nearly_full(void){
  return ATOMIC_LOAD(&mem0.nearly_full);
}

/*
** Deinitialize the memory allocation subsystem.
*/
void __malloc_end(void){
  if( __global_config.m.shutdown ){
    __global_config.m.shutdown(__global_config.m.app_data);
  }
  memset(&mem0, 0, sizeof(mem0));
}

/*
** Return the amount of memory currently checked out.
*/
int64_t dbsql_memory_used(void){
  int64_t res, mx;
  dbsql_status64(DBSQL_STATUS_MEMORY_USED, &res, &mx, 0);
  return res;
}

/*
** Return the maximum amount of memory that has ever been
** checked out since either the beginning of this process
** or since the most recent reset.
*/
int64_t dbsql_memory_highwater(int reset_flag){
  int64_t res, mx;
  dbsql_status64(DBSQL_STATUS_MEMORY_USED, &res, &mx, reset_flag);
  return mx;
}

/*
** trigger_t the alarm 
*/
static void __malloc_alarm(int n_byte){
  if( mem0.alarm_threshold<=0 ) return;
  dbsql_mutex_leave(mem0.mutex);
  dbsql_release_memory(n_byte);
  dbsql_mutex_enter(mem0.mutex);
}

#ifdef DBSQL_DEBUG
/*
** This routine is called whenever an out-of-memory condition is seen,
** It's only purpose to to serve as a breakpoint for gdb or similar
** code debuggers when working on out-of-memory conditions, for example
** caused by PRAGMA hard_heap_limit=N.
*/
static DBSQL_NOINLINE void test_oom_breakpoint(u_int64_t n){
  static u_int64_t n_oom_fault = 0;
  n_oom_fault += n;
  /* The assert() is never reached in a human lifetime.  It  is here mostly
  ** to prevent code optimizers from optimizing out this function. */
  assert( (n_oom_fault>>32) < 0xffffffff );
}
#else
# define test_oom_breakpoint(X)   /* No-op for production builds */
#endif

/*
** Do a memory allocation with statistics and alarms.  Assume the
** lock is already held.
*/
static void __malloc_with_alarm(int n, void **pp){
  void *p;
  int n_full;
  assert( dbsql_mutex_held(mem0.mutex) );
  assert( n>0 );

  /* In Firefox (circa 2017-02-08), roundup() is remapped to an internal
  ** implementation of malloc_good_size(), which must be called in debug
  ** mode and specifically when the DMD "Dark Matter Detector" is enabled
  ** or else a crash results.  Hence, do not attempt to optimize out the
  ** following roundup() call. */
  n_full = __global_config.m.roundup(n);

  __status_highwater(DBSQL_STATUS_MALLOC_SIZE, n);
  if( mem0.alarm_threshold>0 ){
    int64_t n_used = __status_value(DBSQL_STATUS_MEMORY_USED);
    if( n_used >= mem0.alarm_threshold - n_full ){
      ATOMIC_STORE(&mem0.nearly_full, 1);
      __malloc_alarm(n_full);
      if( mem0.hard_limit ){
        n_used = __status_value(DBSQL_STATUS_MEMORY_USED);
        if( n_used >= mem0.hard_limit - n_full ){
          test_oom_breakpoint(1);
          *pp = 0;
          return;
        }
      }
    }else{
      ATOMIC_STORE(&mem0.nearly_full, 0);
    }
  }
  p = __global_config.m.x_malloc(n_full);
#ifdef DBSQL_ENABLE_MEMORY_MANAGEMENT
  if( p==0 && mem0.alarm_threshold>0 ){
    __malloc_alarm(n_full);
    p = __global_config.m.x_malloc(n_full);
  }
#endif
  if( p ){
    n_full = __malloc_size(p);
    __status_up(DBSQL_STATUS_MEMORY_USED, n_full);
    __status_up(DBSQL_STATUS_MALLOC_COUNT, 1);
  }
  *pp = p;
}

/*
** Allocate memory.  This routine is like dbsql_malloc() except that it
** assumes the memory subsystem has already been initialized.
*/
void *__dbsql_malloc(u_int64_t n){
  void *p;
  if( n==0 || n>DBSQL_MAX_ALLOCATION_SIZE ){
    p = 0;
  }else if( __global_config.memstat ){
    dbsql_mutex_enter(mem0.mutex);
    __malloc_with_alarm((int)n, &p);
    dbsql_mutex_leave(mem0.mutex);
  }else{
    p = __global_config.m.x_malloc((int)n);
  }
  assert( EIGHT_BYTE_ALIGNMENT(p) );  /* IMP: R-11148-40995 */
  return p;
}

/*
** This version of the memory allocation is for use by the application.
** First make sure the memory subsystem is initialized, then do the
** allocation.
*/
void *dbsql_malloc(int n){
#ifndef DBSQL_OMIT_AUTOINIT
  if( dbsql_initialize() ) return 0;
#endif
  return n<=0 ? 0 : __dbsql_malloc(n);
}
void *dbsql_malloc64(u_int64_t n){
#ifndef DBSQL_OMIT_AUTOINIT
  if( dbsql_initialize() ) return 0;
#endif
  return __dbsql_malloc(n);
}

/*
** TRUE if p is a lookaside memory allocation from db
*/
#ifndef DBSQL_OMIT_LOOKASIDE
static int IS_LOOKASIDE(DBSQL *db, const void *p){
  return DBSQL_WITHIN(p, db->lookaside.p_start, db->lookaside.true_end);
}
#else
#define IS_LOOKASIDE(A,B) 0
#endif

/*
** Return the size of a memory allocation previously obtained from
** __dbsql_malloc() or dbsql_malloc().
*/
int __malloc_size(const void *p){
  assert( __memdebug_has_type(p, MEMTYPE_HEAP) );
  return __global_config.m.size((void*)p);
}
static int __lookaside_malloc_size(DBSQL *db, const void *p){
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE    
  return p<db->lookaside.middle ? db->lookaside.sz_true : LOOKASIDE_SMALL;
#else
  return db->lookaside.sz_true;
#endif  
}
int __db_malloc_size(DBSQL *db, const void *p){
  assert( p!=0 );
#ifdef DBSQL_DEBUG
  if( db==0 ){
    assert( __memdebug_no_type(p, (u_int8_t)~MEMTYPE_HEAP) );
    assert( __memdebug_has_type(p, MEMTYPE_HEAP) );
  }else if( !IS_LOOKASIDE(db,p) ){
    assert( __memdebug_has_type(p, (MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
    assert( __memdebug_no_type(p, (u_int8_t)~(MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
  }
#endif
  if( db ){
    if( ((uptr_t)p)<(uptr_t)(db->lookaside.true_end) ){
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
      if( ((uptr_t)p)>=(uptr_t)(db->lookaside.middle) ){
        assert( dbsql_mutex_held(db->mutex) );
        return LOOKASIDE_SMALL;
      }
#endif
      if( ((uptr_t)p)>=(uptr_t)(db->lookaside.p_start) ){
        assert( dbsql_mutex_held(db->mutex) );
        return db->lookaside.sz_true;
      }
    }
  }
  return __global_config.m.size((void*)p);
}
u_int64_t dbsql_msize(void *p){
  assert( __memdebug_no_type(p, (u_int8_t)~MEMTYPE_HEAP) );
  assert( __memdebug_has_type(p, MEMTYPE_HEAP) );
  return p ? __global_config.m.size(p) : 0;
}

/*
** Free memory previously obtained from __dbsql_malloc().
*/
void dbsql_free(void *p){
  if( p==0 ) return;  /* IMP: R-49053-54554 */
  assert( __memdebug_has_type(p, MEMTYPE_HEAP) );
  assert( __memdebug_no_type(p, (u_int8_t)~MEMTYPE_HEAP) );
  if( __global_config.memstat ){
    dbsql_mutex_enter(mem0.mutex);
    __status_down(DBSQL_STATUS_MEMORY_USED, __malloc_size(p));
    __status_down(DBSQL_STATUS_MALLOC_COUNT, 1);
    __global_config.m.__x_free(p);
    dbsql_mutex_leave(mem0.mutex);
  }else{
    __global_config.m.__x_free(p);
  }
}

/*
** Add the size of memory allocation "p" to the count in
** *db->pn_bytes_freed.
*/
static DBSQL_NOINLINE void __measure_allocation_size(DBSQL *db, void *p){
  *db->pn_bytes_freed += __db_malloc_size(db,p);
}

/*
** Free memory that might be associated with a particular database
** connection.  Calling __dbsql_db_free(D,X) for X==0 is a harmless no-op.
** The __db_free_nn(D,X) version requires that X be non-NULL.
*/
void __db_free_nn(DBSQL *db, void *p){
  assert( db==0 || dbsql_mutex_held(db->mutex) );
  assert( p!=0 );
  if( db ){
    if( ((uptr_t)p)<(uptr_t)(db->lookaside.p_end) ){
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
      if( ((uptr_t)p)>=(uptr_t)(db->lookaside.middle) ){
        lookaside_slot_t *buf = (lookaside_slot_t*)p;
        assert( db->pn_bytes_freed==0 );
#ifdef DBSQL_DEBUG
        memset(p, 0xaa, LOOKASIDE_SMALL);  /* Trash freed content */
#endif
        buf->next = db->lookaside.small_free;
        db->lookaside.small_free = buf;
        return;
      }
#endif /* DBSQL_OMIT_TWOSIZE_LOOKASIDE */
      if( ((uptr_t)p)>=(uptr_t)(db->lookaside.p_start) ){
        lookaside_slot_t *buf = (lookaside_slot_t*)p;
        assert( db->pn_bytes_freed==0 );
#ifdef DBSQL_DEBUG
        memset(p, 0xaa, db->lookaside.sz_true);  /* Trash freed content */
#endif
        buf->next = db->lookaside.p_free;
        db->lookaside.p_free = buf;
        return;
      }
    }
    if( db->pn_bytes_freed ){
      __measure_allocation_size(db, p);
      return;
    }
  }
  assert( __memdebug_has_type(p, (MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
  assert( __memdebug_no_type(p, (u_int8_t)~(MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
  assert( db!=0 || __memdebug_no_type(p, MEMTYPE_LOOKASIDE) );
  __memdebug_set_type(p, MEMTYPE_HEAP);
  dbsql_free(p);
}
void __db_nn_free_nn(DBSQL *db, void *p){
  assert( db!=0 );
  assert( dbsql_mutex_held(db->mutex) );
  assert( p!=0 );
  if( ((uptr_t)p)<(uptr_t)(db->lookaside.p_end) ){
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
    if( ((uptr_t)p)>=(uptr_t)(db->lookaside.middle) ){
      lookaside_slot_t *buf = (lookaside_slot_t*)p;
      assert( db->pn_bytes_freed==0 );
#ifdef DBSQL_DEBUG
      memset(p, 0xaa, LOOKASIDE_SMALL);  /* Trash freed content */
#endif
      buf->next = db->lookaside.small_free;
      db->lookaside.small_free = buf;
      return;
    }
#endif /* DBSQL_OMIT_TWOSIZE_LOOKASIDE */
    if( ((uptr_t)p)>=(uptr_t)(db->lookaside.p_start) ){
      lookaside_slot_t *buf = (lookaside_slot_t*)p;
      assert( db->pn_bytes_freed==0 );
#ifdef DBSQL_DEBUG
      memset(p, 0xaa, db->lookaside.sz_true);  /* Trash freed content */
#endif
      buf->next = db->lookaside.p_free;
      db->lookaside.p_free = buf;
      return;
    }
  }
  if( db->pn_bytes_freed ){
    __measure_allocation_size(db, p);
    return;
  }
  assert( __memdebug_has_type(p, (MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
  assert( __memdebug_no_type(p, (u_int8_t)~(MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
  __memdebug_set_type(p, MEMTYPE_HEAP);
  dbsql_free(p);
}
void __dbsql_db_free(DBSQL *db, void *p){
  assert( db==0 || dbsql_mutex_held(db->mutex) );
  if( p ) __db_free_nn(db, p);
}

/*
** Change the size of an existing memory allocation
*/
void *__dbsql_realloc(void *old, u_int64_t n_bytes){
  int n_old, n_new, n_diff;
  void *p_new;
  assert( __memdebug_has_type(old, MEMTYPE_HEAP) );
  assert( __memdebug_no_type(old, (u_int8_t)~MEMTYPE_HEAP) );
  if( old==0 ){
    return __dbsql_malloc(n_bytes); /* IMP: R-04300-56712 */
  }
  if( n_bytes==0 ){
    dbsql_free(old); /* IMP: R-26507-47431 */
    return 0;
  }
  if( n_bytes>DBSQL_MAX_ALLOCATION_SIZE ){
    return 0;
  }
  n_old = __malloc_size(old);
  /* IMPLEMENTATION-OF: R-46199-30249 dbsql guarantees that the second
  ** argument to x_realloc is always a value returned by a prior call to
  ** roundup. */
  n_new = __global_config.m.roundup((int)n_bytes);
  if( n_old==n_new ){
    p_new = old;
  }else if( __global_config.memstat ){
    int64_t n_used;
    dbsql_mutex_enter(mem0.mutex);
    __status_highwater(DBSQL_STATUS_MALLOC_SIZE, (int)n_bytes);
    n_diff = n_new - n_old;
    if( n_diff>0 && (n_used = __status_value(DBSQL_STATUS_MEMORY_USED)) >= 
          mem0.alarm_threshold-n_diff ){
      __malloc_alarm(n_diff);
      if( mem0.hard_limit>0 && n_used >= mem0.hard_limit - n_diff ){
        dbsql_mutex_leave(mem0.mutex);
        test_oom_breakpoint(1);
        return 0;
      }
    }
    p_new = __global_config.m.x_realloc(old, n_new);
#ifdef DBSQL_ENABLE_MEMORY_MANAGEMENT
    if( p_new==0 && mem0.alarm_threshold>0 ){
      __malloc_alarm((int)n_bytes);
      p_new = __global_config.m.x_realloc(old, n_new);
    }
#endif
    if( p_new ){
      n_new = __malloc_size(p_new);
      __status_up(DBSQL_STATUS_MEMORY_USED, n_new-n_old);
    }
    dbsql_mutex_leave(mem0.mutex);
  }else{
    p_new = __global_config.m.x_realloc(old, n_new);
  }
  assert( EIGHT_BYTE_ALIGNMENT(p_new) ); /* IMP: R-11148-40995 */
  return p_new;
}

/*
** The public interface to __dbsql_realloc.  Make sure that the memory
** subsystem is initialized prior to invoking dbsql_realloc.
*/
void *dbsql_realloc(void *old, int n){
#ifndef DBSQL_OMIT_AUTOINIT
  if( dbsql_initialize() ) return 0;
#endif
  if( n<0 ) n = 0;  /* IMP: R-26507-47431 */
  return __dbsql_realloc(old, n);
}
void *dbsql_realloc64(void *old, u_int64_t n){
#ifndef DBSQL_OMIT_AUTOINIT
  if( dbsql_initialize() ) return 0;
#endif
  return __dbsql_realloc(old, n);
}


/*
** Allocate and zero memory.
*/ 
void *__malloc_zero(u_int64_t n){
  void *p = __dbsql_malloc(n);
  if( p ){
    memset(p, 0, (size_t)n);
  }
  return p;
}

/*
** Allocate and zero memory.  If the allocation fails, make
** the malloc_failed flag in the connection pointer.
*/
void *__db_malloc_zero(DBSQL *db, u_int64_t n){
  void *p;
  testcase( db==0 );
  p = __db_malloc_raw(db, n);
  if( p ) memset(p, 0, (size_t)n);
  return p;
}


/* Finish the work of __db_malloc_raw_nn for the unusual and
** slower case when the allocation cannot be fulfilled using lookaside.
*/
static DBSQL_NOINLINE void *__db_malloc_raw_finish(DBSQL *db, u_int64_t n){
  void *p;
  assert( db!=0 );
  p = __dbsql_malloc(n);
  if( !p ) __oom_fault(db);
  __memdebug_set_type(p, 
         (db->lookaside.disable==0) ? MEMTYPE_LOOKASIDE : MEMTYPE_HEAP);
  return p;
}

/*
** Allocate memory, either lookaside (if possible) or heap.  
** If the allocation fails, set the malloc_failed flag in
** the connection pointer.
**
** If db!=0 and db->malloc_failed is true (indicating a prior malloc
** failure on the same database connection) then always return 0.
** Hence for a particular database connection, once malloc starts
** failing, it fails consistently until malloc_failed is reset.
** This is an important assumption.  There are many places in the
** code that do things like this:
**
**         int *a = (int*)__db_malloc_raw(db, 100);
**         int *b = (int*)__db_malloc_raw(db, 200);
**         if( b ) a[10] = 9;
**
** In other words, if a subsequent malloc (ex: "b") worked, it is assumed
** that all prior mallocs (ex: "a") worked too.
**
** The __malloc_raw_nn() variant guarantees that the "db" parameter is
** not a NULL pointer.
*/
void *__db_malloc_raw(DBSQL *db, u_int64_t n){
  void *p;
  if( db ) return __db_malloc_raw_nn(db, n);
  p = __dbsql_malloc(n);
  __memdebug_set_type(p, MEMTYPE_HEAP);
  return p;
}
void *__db_malloc_raw_nn(DBSQL *db, u_int64_t n){
#ifndef DBSQL_OMIT_LOOKASIDE
  lookaside_slot_t *buf;
  assert( db!=0 );
  assert( dbsql_mutex_held(db->mutex) );
  assert( db->pn_bytes_freed==0 );
  if( n>db->lookaside.sz ){
    if( !db->lookaside.disable ){
      db->lookaside.an_stat[1]++;      
    }else if( db->malloc_failed ){
      return 0;
    }
    return __db_malloc_raw_finish(db, n);
  }
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
  if( n<=LOOKASIDE_SMALL ){
    if( (buf = db->lookaside.small_free)!=0 ){
      db->lookaside.small_free = buf->next;
      db->lookaside.an_stat[0]++;
      return (void*)buf;
    }else if( (buf = db->lookaside.small_init)!=0 ){
      db->lookaside.small_init = buf->next;
      db->lookaside.an_stat[0]++;
      return (void*)buf;
    }
  }
#endif
  if( (buf = db->lookaside.p_free)!=0 ){
    db->lookaside.p_free = buf->next;
    db->lookaside.an_stat[0]++;
    return (void*)buf;
  }else if( (buf = db->lookaside.init)!=0 ){
    db->lookaside.init = buf->next;
    db->lookaside.an_stat[0]++;
    return (void*)buf;
  }else{
    db->lookaside.an_stat[2]++;
  }
#else
  assert( db!=0 );
  assert( dbsql_mutex_held(db->mutex) );
  assert( db->pn_bytes_freed==0 );
  if( db->malloc_failed ){
    return 0;
  }
#endif
  return __db_malloc_raw_finish(db, n);
}

/* Forward declaration */
static DBSQL_NOINLINE void *__db_realloc_finish(DBSQL *db, void *p, u_int64_t n);

/*
** Resize the block of memory pointed to by p to n bytes. If the
** resize fails, set the malloc_failed flag in the connection object.
*/
void *__db_realloc(DBSQL *db, void *p, u_int64_t n){
  assert( db!=0 );
  if( p==0 ) return __db_malloc_raw_nn(db, n);
  assert( dbsql_mutex_held(db->mutex) );
  if( ((uptr_t)p)<(uptr_t)db->lookaside.p_end ){
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
    if( ((uptr_t)p)>=(uptr_t)db->lookaside.middle ){
      if( n<=LOOKASIDE_SMALL ) return p;
    }else
#endif
    if( ((uptr_t)p)>=(uptr_t)db->lookaside.p_start ){
      if( n<=db->lookaside.sz_true ) return p;
    }
  }
  return __db_realloc_finish(db, p, n);
}
static DBSQL_NOINLINE void *__db_realloc_finish(DBSQL *db, void *p, u_int64_t n){
  void *p_new = 0;
  assert( db!=0 );
  assert( p!=0 );
  if( db->malloc_failed==0 ){
    if( IS_LOOKASIDE(db, p) ){
      p_new = __db_malloc_raw_nn(db, n);
      if( p_new ){
        memcpy(p_new, p, __lookaside_malloc_size(db, p));
        __dbsql_db_free(db, p);
      }
    }else{
      assert( __memdebug_has_type(p, (MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
      assert( __memdebug_no_type(p, (u_int8_t)~(MEMTYPE_LOOKASIDE|MEMTYPE_HEAP)) );
      __memdebug_set_type(p, MEMTYPE_HEAP);
      p_new = __dbsql_realloc(p, n);
      if( !p_new ){
        __oom_fault(db);
      }
      __memdebug_set_type(p_new,
            (db->lookaside.disable==0 ? MEMTYPE_LOOKASIDE : MEMTYPE_HEAP));
    }
  }
  return p_new;
}

/*
** Attempt to reallocate p.  If the reallocation fails, then free p
** and set the malloc_failed flag in the database connection.
*/
void *__db_realloc_or_free(DBSQL *db, void *p, u_int64_t n){
  void *p_new;
  p_new = __db_realloc(db, p, n);
  if( !p_new ){
    __dbsql_db_free(db, p);
  }
  return p_new;
}

/*
** Make a copy of a string in memory obtained from dbsql_malloc(). These 
** functions call __malloc_raw() directly instead of dbsql_malloc(). This
** is because when memory debugging is turned on, these two functions are 
** called via macros that record the current file and line number in the
** __thread_data structure.
*/
char *__db_str_dup(DBSQL *db, const char *z){
  char *z_new;
  size_t n;
  if( z==0 ){
    return 0;
  }
  n = strlen(z) + 1;
  z_new = __db_malloc_raw(db, n);
  if( z_new ){
    memcpy(z_new, z, n);
  }
  return z_new;
}
char *__db_str_n_dup(DBSQL *db, const char *z, u_int64_t n){
  char *z_new;
  assert( db!=0 );
  assert( z!=0 || n==0 );
  assert( (n&0x7fffffff)==n );
  z_new = z ? __db_malloc_raw_nn(db, n+1) : 0;
  if( z_new ){
    memcpy(z_new, z, (size_t)n);
    z_new[n] = 0;
  }
  return z_new;
}

/*
** The text between start and end represents a phrase within a larger
** SQL statement.  Make a copy of this phrase in space obtained form
** __db_malloc().  Omit leading and trailing whitespace.
*/
char *__db_span_dup(DBSQL *db, const char *start, const char *end){
  int n;
#ifdef DBSQL_DEBUG
  /* Because of the way the parser works, the span is guaranteed to contain
  ** at least one non-space character */
  for(n=0; __isspace(start[n]); n++){ assert( &start[n]<end ); }
#endif
  while( __isspace(start[0]) ) start++;
  n = (int)(end - start);
  while( __isspace(start[n-1]) ) n--;
  return __db_str_n_dup(db, start, n);
}

/*
** Free any prior content in *pz and replace it with a copy of z_new.
*/
void __set_string(char **pz, DBSQL *db, const char *z_new){
  char *z = __db_str_dup(db, z_new);
  __dbsql_db_free(db, *pz);
  *pz = z;
}

/*
** Call this routine to record the fact that an OOM (out-of-memory) error
** has happened.  This routine will set db->malloc_failed, and also
** temporarily disable the lookaside memory allocator and interrupt
** any running VDBEs.
**
** Always return a NULL pointer so that this routine can be invoked using
**
**      return __oom_fault(db);
**
** and thereby avoid unnecessary stack frame allocations for the overwhelmingly
** common case where no OOM occurs.
*/
void *__oom_fault(DBSQL *db){
  if( db->malloc_failed==0 && db->benign_malloc==0 ){
    db->malloc_failed = 1;
    if( db->n_vdbe_exec>0 ){
      ATOMIC_STORE(&db->u1.is_interrupted, 1);
    }
    DISABLE_LOOKASIDE;
    if( db->parser ){
      parser_t *parser;
      __error_msg(db->parser, "out of memory");
      db->parser->rc = DBSQL_NOMEM_BKPT;
      for(parser=db->parser->outer_parse; parser; parser = parser->outer_parse){
        parser->n_err++;
        parser->rc = DBSQL_NOMEM;
      } 
    }
  }
  return 0;
}

/*
** This routine reactivates the memory allocator and clears the
** db->malloc_failed flag as necessary.
**
** The memory allocator is not restarted if there are running
** VDBEs.
*/
void __oom_clear(DBSQL *db){
  if( db->malloc_failed && db->n_vdbe_exec==0 ){
    db->malloc_failed = 0;
    ATOMIC_STORE(&db->u1.is_interrupted, 0);
    assert( db->lookaside.disable>0 );
    ENABLE_LOOKASIDE;
  }
}

/*
** Take actions at the end of an API call to deal with error codes.
*/
static DBSQL_NOINLINE int __api_handle_error(DBSQL *db, int rc){
  if( db->malloc_failed || rc==DBSQL_IOERR_NOMEM ){
    __oom_clear(db);
    __dbsql_error(db, DBSQL_NOMEM);
    return DBSQL_NOMEM_BKPT;
  }
  return rc & db->err_mask;
}

/*
** This function must be called before exiting any API function (i.e. 
** returning control to the user) that has called dbsql_malloc or
** dbsql_realloc.
**
** The returned value is normally a copy of the second argument to this
** function. However, if a malloc() failure has occurred since the previous
** invocation DBSQL_NOMEM is returned instead. 
**
** If an OOM as occurred, then the connection error-code (the value
** returned by dbsql_errcode()) is set to DBSQL_NOMEM.
*/
int __api_exit(DBSQL* db, int rc){
  /* If the db handle must hold the connection handle mutex here.
  ** Otherwise the read (and possible write) of db->malloc_failed 
  ** is unsafe, as is the call to __dbsql_error().
  */
  assert( db!=0 );
  assert( dbsql_mutex_held(db->mutex) );
  if( db->malloc_failed || rc ){
    return __api_handle_error(db, rc);
  }
  return 0;
}
