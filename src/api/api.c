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
** Main file for the dbsql library.  The routines in this file
** implement the programmer interface to the library.  Routines in
** other files are for internal use by dbsql and should not be
** accessed by users of the library.
*/
#include "dbsql_int.h"

#ifdef DBSQL_ENABLE_FTS3
# include "fts3.h"
#endif
#ifdef DBSQL_ENABLE_RTREE
# include "rtree.h"
#endif
#if defined(DBSQL_ENABLE_ICU) || defined(DBSQL_ENABLE_ICU_COLLATIONS)
# include "dbsql_icu.h"
#endif

/*
** This is an extension initializer that is a no-op and always
** succeeds, except that it fails if the fault-simulation is set
** to 500.
*/
static int __test_ext_init(DBSQL *db){
  (void)db;
  return __fault_sim(500);
}


/*
** Forward declarations of external module initializer functions
** for modules that need them.
*/
#ifdef DBSQL_ENABLE_FTS5
int __fts5_init(DBSQL*);
#endif
#ifdef DBSQL_ENABLE_STMTVTAB
int __stmt_vtab_init(DBSQL*);
#endif
#ifdef DBSQL_EXTRA_AUTOEXT
int DBSQL_EXTRA_AUTOEXT(DBSQL*);
#endif
/*
** An array of pointers to extension initializer functions for
** built-in extensions.
*/
static int (*const __builtin_extensions[])(DBSQL*) = {
#ifdef DBSQL_ENABLE_FTS3
  __fts3_init,
#endif
#ifdef DBSQL_ENABLE_FTS5
  __fts5_init,
#endif
#if defined(DBSQL_ENABLE_ICU) || defined(DBSQL_ENABLE_ICU_COLLATIONS)
  __icu_init,
#endif
#ifdef DBSQL_ENABLE_RTREE
  __rtree_init,
#endif
#ifdef DBSQL_ENABLE_DBPAGE_VTAB
  __dbpage_register,
#endif
#ifdef DBSQL_ENABLE_DBSTAT_VTAB
  __dbstat_register,
#endif
  __test_ext_init,
#ifdef DBSQL_ENABLE_STMTVTAB
  __stmt_vtab_init,
#endif
#ifdef DBSQL_ENABLE_BYTECODE_VTAB
  __vdbe_bytecode_vtab_init,
#endif
#ifdef DBSQL_EXTRA_AUTOEXT
  DBSQL_EXTRA_AUTOEXT,
#endif
};

#ifndef DBSQL_AMALGAMATION
/* IMPLEMENTATION-OF: R-46656-45156 The dbsql_version[] string constant
** contains the text of DBSQL_VERSION macro.
*/
const char dbsql_version[] = DBSQL_VERSION;
#endif

/* IMPLEMENTATION-OF: R-53536-42575 The dbsql_libversion() function returns
** a pointer to the to the dbsql_version[] string constant.
*/
const char *dbsql_libversion(void){ return dbsql_version; }

/* IMPLEMENTATION-OF: R-25063-23286 The dbsql_sourceid() function returns a
** pointer to a string constant whose value is the same as the
** DBSQL_SOURCE_ID C preprocessor macro. Except if dbsql is built using
** an edited copy of the amalgamation, then the last four characters of
** the hash might be different from DBSQL_SOURCE_ID.
*/
const char *dbsql_sourceid(void){ return DBSQL_SOURCE_ID; }

/* IMPLEMENTATION-OF: R-35210-63508 The dbsql_libversion_number() function
** returns an integer equal to DBSQL_VERSION_NUMBER.
*/
int dbsql_libversion_number(void){ return DBSQL_VERSION_NUMBER; }

/* IMPLEMENTATION-OF: R-20790-14025 The dbsql_threadsafe() function returns
** zero if and only if dbsql was compiled with mutexing code omitted due to
** the DBSQL_THREADSAFE compile-time option being set to 0.
*/
int dbsql_threadsafe(void){ return DBSQL_THREADSAFE; }

/*
** When compiling the test fixture or with debugging enabled (on Win32),
** this variable being set to non-zero will cause OSTRACE macros to emit
** extra diagnostic information.
*/
#ifdef DBSQL_HAVE_OS_TRACE
# ifndef DBSQL_DEBUG_OS_TRACE
#   define DBSQL_DEBUG_OS_TRACE 0
# endif
  int __os_trace = DBSQL_DEBUG_OS_TRACE;
#endif

#if !defined(DBSQL_OMIT_TRACE) && defined(DBSQL_ENABLE_IOTRACE)
/*
** If the following function pointer is not NULL and if
** DBSQL_ENABLE_IOTRACE is enabled, then messages describing
** I/O active are written using this function.  These messages
** are intended for debugging activity only.
*/
DBSQL_API void (DBSQL_CDECL *__io_trace)(const char*, ...) = 0;
#endif

/*
** If the following global variable points to a string which is the
** name of a directory, then that directory will be used to store
** temporary files.
**
** See also the "PRAGMA temp_store_directory" SQL command.
*/
char *dbsql_temp_directory = 0;

/*
** If the following global variable points to a string which is the
** name of a directory, then that directory will be used to store
** all database files specified with a relative pathname.
**
** See also the "PRAGMA data_store_directory" SQL command.
*/
char *dbsql_data_directory = 0;

/*
** Initialize dbsql. 
**
** This routine must be called to initialize the memory allocation,
** VFS, and mutex subsystems prior to doing any serious work with
** dbsql.  But as long as you do not compile with DBSQL_OMIT_AUTOINIT
** this routine will be called automatically by key routines such as
** dbsql_open(). 
**
** This routine is a no-op except on its very first call for the process,
** or for the first call after a call to dbsql_shutdown.
**
** The first thread to call this routine runs the initialization to
** completion.  If subsequent threads call this routine before the first
** thread has finished the initialization process, then the subsequent
** threads must block until the first thread finishes with the initialization.
**
** The first thread might call this routine recursively.  Recursive
** calls to this routine should not block, of course.  Otherwise the
** initialization process would never complete.
**
** Let X be the first thread to enter this routine.  Let Y be some other
** thread.  Then while the initial invocation of this routine by X is
** incomplete, it is required that:
**
**    *  Calls to this routine from Y must block until the outer-most
**       call by X completes.
**
**    *  Recursive calls to this routine from thread X return immediately
**       without blocking.
*/
int dbsql_initialize(void){
  MUTEX_LOGIC( dbsql_mutex_t *main_mtx; )      /* The main static mutex */
  int rc;                                      /* Result code */
#ifdef DBSQL_EXTRA_INIT
  int run_extra_init = 0;                       /* Extra initialization needed */
#endif

#ifdef DBSQL_OMIT_WSD
  rc = dbsql_wsd_init(4096, 24);
  if( rc!=DBSQL_OK ){
    return rc;
  }
#endif

  /* If the following assert() fails on some obscure processor/compiler
  ** combination, the work-around is to set the correct pointer
  ** size at compile-time using -DSQLITE_PTRSIZE=n compile-time option */
  assert( DBSQL_PTRSIZE==sizeof(char*) );

  /* If dbsql is already completely initialized, then this call
  ** to dbsql_initialize() should be a no-op.  But the initialization
  ** must be complete.  So is_init must not be set until the very end
  ** of this routine.
  */
  if( __global_config.is_init ){
    __memory_barrier();
    return DBSQL_OK;
  }

  /* Make sure the mutex subsystem is initialized.  If unable to
  ** initialize the mutex subsystem, return early with the error.
  ** If the system is so sick that we are unable to allocate a mutex,
  ** there is not much dbsql is going to be able to do.
  **
  ** The mutex subsystem must take care of serializing its own
  ** initialization.
  */
  rc = __mutex_init();
  if( rc ) return rc;

  /* Initialize the malloc() system and the recursive init_mutex mutex.
  ** This operation is protected by the STATIC_MAIN mutex.  Note that
  ** __mutex_alloc() is called for a static mutex prior to initializing the
  ** malloc subsystem - this implies that the allocation of a static
  ** mutex must not require support from the malloc subsystem.
  */
  MUTEX_LOGIC( main_mtx = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN); )
  dbsql_mutex_enter(main_mtx);
  __global_config.is_mutex_init = 1;
  if( !__global_config.is_malloc_init ){
    rc = __malloc_init();
  }
  if( rc==DBSQL_OK ){
    __global_config.is_malloc_init = 1;
    if( !__global_config.init_mutex ){
      __global_config.init_mutex =
           __dbsql_mutex_alloc(DBSQL_MUTEX_RECURSIVE);
      if( __global_config.core_mutex && !__global_config.init_mutex ){
        rc = DBSQL_NOMEM_BKPT;
      }
    }
  }
  if( rc==DBSQL_OK ){
    __global_config.n_ref_init_mutex++;
  }
  dbsql_mutex_leave(main_mtx);

  /* If rc is not DBSQL_OK at this point, then either the malloc
  ** subsystem could not be initialized or the system failed to allocate
  ** the init_mutex mutex. Return an error in either case.  */
  if( rc!=DBSQL_OK ){
    return rc;
  }

  /* Do the rest of the initialization under the recursive mutex so
  ** that we will be able to handle recursive calls into
  ** dbsql_initialize().  The recursive calls normally come through
  ** dbsql_os_init() when it invokes dbsql_vfs_register(), but other
  ** recursive calls might also be possible.
  **
  ** IMPLEMENTATION-OF: R-00140-37445 dbsql automatically serializes calls
  ** to the init method, so the init method need not be threadsafe.
  **
  ** The following mutex is what serializes access to the appdef pcache init
  ** methods.  The dbsql_pcache_methods_t.init() all is embedded in the
  ** call to __pcache_initialize().
  */
  dbsql_mutex_enter(__global_config.init_mutex);
  if( __global_config.is_init==0 && __global_config.in_progress==0 ){
    __global_config.in_progress = 1;
#ifdef DBSQL_ENABLE_SQLLOG
    {
      extern void dbsql_init_sqllog(void);
      dbsql_init_sqllog();
    }
#endif
    memset(&__builtin_functions, 0, sizeof(__builtin_functions));
    __register_builtin_functions();
    if( __global_config.is_p_cache_init==0 ){
      rc = __pcache_initialize();
    }
    if( rc==DBSQL_OK ){
      __global_config.is_p_cache_init = 1;
      rc = __dbsql_os_init();
    }
#ifndef DBSQL_OMIT_DESERIALIZE
    if( rc==DBSQL_OK ){
      rc = __memdb_init();
    }
#endif
    if( rc==DBSQL_OK ){
      __p_cache_buffer_setup( __global_config.page,
          __global_config.sz_page, __global_config.n_page);
#ifdef DBSQL_EXTRA_INIT_MUTEXED
      {
        int DBSQL_EXTRA_INIT_MUTEXED(const char*);
        rc = DBSQL_EXTRA_INIT_MUTEXED(0);
      }
#endif
    }
    if( rc==DBSQL_OK ){
      __memory_barrier();
      __global_config.is_init = 1;
#ifdef DBSQL_EXTRA_INIT
      run_extra_init = 1;
#endif
    }
    __global_config.in_progress = 0;
  }
  dbsql_mutex_leave(__global_config.init_mutex);

  /* Go back under the static mutex and clean up the recursive
  ** mutex to prevent a resource leak.
  */
  dbsql_mutex_enter(main_mtx);
  __global_config.n_ref_init_mutex--;
  if( __global_config.n_ref_init_mutex<=0 ){
    assert( __global_config.n_ref_init_mutex==0 );
    dbsql_mutex_free(__global_config.init_mutex);
    __global_config.init_mutex = 0;
  }
  dbsql_mutex_leave(main_mtx);

  /* The following is just a sanity check to make sure dbsql has
  ** been compiled correctly.  It is important to run this code, but
  ** we don't want to run it too often and soak up CPU cycles for no
  ** reason.  So we run it once during initialization.
  */
#ifndef NDEBUG
#ifndef DBSQL_OMIT_FLOATING_POINT
  /* This section of code's only "output" is via assert() statements. */
  if( rc==DBSQL_OK ){
    u_int64_t x = (((u_int64_t)1)<<63)-1;
    double y;
    assert(sizeof(x)==8);
    assert(sizeof(x)==sizeof(y));
    memcpy(&y, &x, 8);
    assert( __is_na_n(y) );
  }
#endif
#endif

  /* Do extra initialization steps requested by the DBSQL_EXTRA_INIT
  ** compile-time option.
  */
#ifdef DBSQL_EXTRA_INIT
  if( run_extra_init ){
    int DBSQL_EXTRA_INIT(const char*);
    rc = DBSQL_EXTRA_INIT(0);
  }
#endif
  return rc;
}

/*
** Undo the effects of dbsql_initialize().  Must not be called while
** there are outstanding database connections or memory allocations or
** while any part of dbsql is otherwise in use in any thread.  This
** routine is not threadsafe.  But it is safe to invoke this routine
** on when dbsql is already shut down.  If dbsql is already shut down
** when this routine is invoked, then this routine is a harmless no-op.
*/
int dbsql_shutdown(void){
#ifdef DBSQL_OMIT_WSD
  int rc = dbsql_wsd_init(4096, 24);
  if( rc!=DBSQL_OK ){
    return rc;
  }
#endif

  if( __global_config.is_init ){
#ifdef DBSQL_EXTRA_SHUTDOWN
    void DBSQL_EXTRA_SHUTDOWN(void);
    DBSQL_EXTRA_SHUTDOWN();
#endif
    dbsql_os_end();
    dbsql_reset_auto_extension();
    __global_config.is_init = 0;
  }
  if( __global_config.is_p_cache_init ){
    __pcache_shutdown();
    __global_config.is_p_cache_init = 0;
  }
  if( __global_config.is_malloc_init ){
    __malloc_end();
    __global_config.is_malloc_init = 0;

#ifndef DBSQL_OMIT_SHUTDOWN_DIRECTORIES
    /* The heap subsystem has now been shutdown and these values are supposed
    ** to be NULL or point to memory that was obtained from dbsql_malloc(),
    ** which would rely on that heap subsystem; therefore, make sure these
    ** values cannot refer to heap memory that was just invalidated when the
    ** heap subsystem was shutdown.  This is only done if the current call to
    ** this function resulted in the heap subsystem actually being shutdown.
    */
    dbsql_data_directory = 0;
    dbsql_temp_directory = 0;
#endif
  }
  if( __global_config.is_mutex_init ){
    __mutex_end();
    __global_config.is_mutex_init = 0;
  }

  return DBSQL_OK;
}

/*
** This API allows applications to modify the global configuration of
** the dbsql library at run-time.
**
** This routine should only be called when there are no outstanding
** database connections or memory allocations.  This routine is not
** threadsafe.  Failure to heed these warnings can lead to unpredictable
** behavior.
*/
int dbsql_config(int op, ...){
  va_list ap;
  int rc = DBSQL_OK;

  /* dbsql_config() normally returns DBSQL_MISUSE if it is invoked while
  ** the dbsql library is in use.  Except, a few selected opcodes
  ** are allowed.
  */
  if( __global_config.is_init ){
    static const u_int64_t m_anytime_config_option = 0
       | MASKBIT64( DBSQL_CONFIG_LOG )
       | MASKBIT64( DBSQL_CONFIG_PCACHE_HDRSZ )
    ;
    if( op<0 || op>63 || (MASKBIT64(op) & m_anytime_config_option)==0 ){
      return DBSQL_MISUSE_BKPT;
    }
    testcase( op==DBSQL_CONFIG_LOG );
    testcase( op==DBSQL_CONFIG_PCACHE_HDRSZ );
  }

  va_start(ap, op);
  switch( op ){

    /* Mutex configuration options are only available in a threadsafe
    ** compile.
    */
#if defined(DBSQL_THREADSAFE) && DBSQL_THREADSAFE>0  /* IMP: R-54466-46756 */
    case DBSQL_CONFIG_SINGLETHREAD: {
      /* EVIDENCE-OF: R-02748-19096 This option sets the threading mode to
      ** Single-thread. */
      __global_config.core_mutex = 0;  /* Disable mutex on core */
      __global_config.full_mutex = 0;  /* Disable mutex on connections */
      break;
    }
#endif
#if defined(DBSQL_THREADSAFE) && DBSQL_THREADSAFE>0 /* IMP: R-20520-54086 */
    case DBSQL_CONFIG_MULTITHREAD: {
      /* EVIDENCE-OF: R-14374-42468 This option sets the threading mode to
      ** Multi-thread. */
      __global_config.core_mutex = 1;  /* Enable mutex on core */
      __global_config.full_mutex = 0;  /* Disable mutex on connections */
      break;
    }
#endif
#if defined(DBSQL_THREADSAFE) && DBSQL_THREADSAFE>0 /* IMP: R-59593-21810 */
    case DBSQL_CONFIG_SERIALIZED: {
      /* EVIDENCE-OF: R-41220-51800 This option sets the threading mode to
      ** Serialized. */
      __global_config.core_mutex = 1;  /* Enable mutex on core */
      __global_config.full_mutex = 1;  /* Enable mutex on connections */
      break;
    }
#endif
#if defined(DBSQL_THREADSAFE) && DBSQL_THREADSAFE>0 /* IMP: R-63666-48755 */
    case DBSQL_CONFIG_MUTEX: {
      /* Specify an alternative mutex implementation */
      __global_config.mutex = *va_arg(ap, dbsql_mutex_methods_t*);
      break;
    }
#endif
#if defined(DBSQL_THREADSAFE) && DBSQL_THREADSAFE>0 /* IMP: R-14450-37597 */
    case DBSQL_CONFIG_GETMUTEX: {
      /* Retrieve the current mutex implementation */
      *va_arg(ap, dbsql_mutex_methods_t*) = __global_config.mutex;
      break;
    }
#endif

    case DBSQL_CONFIG_MALLOC: {
      /* EVIDENCE-OF: R-55594-21030 The DBSQL_CONFIG_MALLOC option takes a
      ** single argument which is a pointer to an instance of the
      ** dbsql_mem_methods_t structure. The argument specifies alternative
      ** low-level memory allocation routines to be used in place of the memory
      ** allocation routines built into dbsql. */
      __global_config.m = *va_arg(ap, dbsql_mem_methods_t*);
      break;
    }
    case DBSQL_CONFIG_GETMALLOC: {
      /* EVIDENCE-OF: R-51213-46414 The DBSQL_CONFIG_GETMALLOC option takes a
      ** single argument which is a pointer to an instance of the
      ** dbsql_mem_methods_t structure. The dbsql_mem_methods_t structure is
      ** filled with the currently defined memory allocation routines. */
      if( __global_config.m.x_malloc==0 ) __mem_set_default();
      *va_arg(ap, dbsql_mem_methods_t*) = __global_config.m;
      break;
    }
    case DBSQL_CONFIG_MEMSTATUS: {
      assert( !__global_config.is_init );  /* Cannot change at runtime */
      /* EVIDENCE-OF: R-61275-35157 The DBSQL_CONFIG_MEMSTATUS option takes
      ** single argument of type int, interpreted as a boolean, which enables
      ** or disables the collection of memory allocation statistics. */
      __global_config.memstat = va_arg(ap, int);
      break;
    }
    case DBSQL_CONFIG_SMALL_MALLOC: {
      __global_config.small_malloc = va_arg(ap, int);
      break;
    }
    case DBSQL_CONFIG_PAGECACHE: {
      /* EVIDENCE-OF: R-18761-36601 There are three arguments to
      ** DBSQL_CONFIG_PAGECACHE: A pointer to 8-byte aligned memory (mem),
      ** the size of each page cache line (sz), and the number of cache lines
      ** (N). */
      __global_config.page = va_arg(ap, void*);
      __global_config.sz_page = va_arg(ap, int);
      __global_config.n_page = va_arg(ap, int);
      break;
    }
    case DBSQL_CONFIG_PCACHE_HDRSZ: {
      /* EVIDENCE-OF: R-39100-27317 The DBSQL_CONFIG_PCACHE_HDRSZ option takes
      ** a single parameter which is a pointer to an integer and writes into
      ** that integer the number of extra bytes per page required for each page
      ** in DBSQL_CONFIG_PAGECACHE. */
      *va_arg(ap, int*) =
          __header_size_btree() +
          __header_size_pcache() +
          __header_size_pcache1();
      break;
    }

    case DBSQL_CONFIG_PCACHE: {
      /* no-op */
      break;
    }
    case DBSQL_CONFIG_GETPCACHE: {
      /* now an error */
      rc = DBSQL_ERROR;
      break;
    }

    case DBSQL_CONFIG_PCACHE2: {
      /* EVIDENCE-OF: R-63325-48378 The DBSQL_CONFIG_PCACHE2 option takes a
      ** single argument which is a pointer to an dbsql_pcache_methods2_t
      ** object. This object specifies the interface to a custom page cache
      ** implementation. */
      __global_config.pcache2 = *va_arg(ap, dbsql_pcache_methods2_t*);
      break;
    }
    case DBSQL_CONFIG_GETPCACHE2: {
      /* EVIDENCE-OF: R-22035-46182 The DBSQL_CONFIG_GETPCACHE2 option takes a
      ** single argument which is a pointer to an dbsql_pcache_methods2_t
      ** object. dbsql copies of the current page cache implementation into
      ** that object. */
      if( __global_config.pcache2.init==0 ){
        __p_cache_set_default();
      }
      *va_arg(ap, dbsql_pcache_methods2_t*) = __global_config.pcache2;
      break;
    }

/* EVIDENCE-OF: R-06626-12911 The DBSQL_CONFIG_HEAP option is only
** available if dbsql is compiled with either DBSQL_ENABLE_MEMSYS3 or
** DBSQL_ENABLE_MEMSYS5 and returns DBSQL_ERROR if invoked otherwise. */
#if defined(DBSQL_ENABLE_MEMSYS3) || defined(DBSQL_ENABLE_MEMSYS5)
    case DBSQL_CONFIG_HEAP: {
      /* EVIDENCE-OF: R-19854-42126 There are three arguments to
      ** DBSQL_CONFIG_HEAP: An 8-byte aligned pointer to the memory, the
      ** number of bytes in the memory buffer, and the minimum allocation size.
      */
      __global_config.heap = va_arg(ap, void*);
      __global_config.n_heap = va_arg(ap, int);
      __global_config.mn_req = va_arg(ap, int);

      if( __global_config.mn_req<1 ){
        __global_config.mn_req = 1;
      }else if( __global_config.mn_req>(1<<12) ){
        /* cap min request size at 2^12 */
        __global_config.mn_req = (1<<12);
      }

      if( __global_config.heap==0 ){
        /* EVIDENCE-OF: R-49920-60189 If the first pointer (the memory pointer)
        ** is NULL, then dbsql reverts to using its default memory allocator
        ** (the system malloc() implementation), undoing any prior invocation of
        ** DBSQL_CONFIG_MALLOC.
        **
        ** Setting __global_config.m to all zeros will cause malloc to
        ** revert to its default implementation when dbsql_initialize() is run
        */
        memset(&__global_config.m, 0, sizeof(__global_config.m));
      }else{
        /* EVIDENCE-OF: R-61006-08918 If the memory pointer is not NULL then the
        ** alternative memory allocator is engaged to handle all of SQLites
        ** memory allocation needs. */
#ifdef DBSQL_ENABLE_MEMSYS3
        __global_config.m = *__mem_get_memsys3();
#endif
#ifdef DBSQL_ENABLE_MEMSYS5
        __global_config.m = *__mem_get_memsys5();
#endif
      }
      break;
    }
#endif

    case DBSQL_CONFIG_LOOKASIDE: {
      __global_config.sz_lookaside = va_arg(ap, int);
      __global_config.n_lookaside = va_arg(ap, int);
      break;
    }
   
    /* Record a pointer to the logger function and its first argument.
    ** The default is NULL.  Logging is disabled if the function pointer is
    ** NULL.
    */
    case DBSQL_CONFIG_LOG: {
      /* MSVC is picky about pulling func ptrs from va lists.
      ** http://support.microsoft.com/kb/47961
      ** __global_config.x_log = va_arg(ap, void(*)(void*,int,const char*));
      */
      typedef void(*LOGFUNC_t)(void*,int,const char*);
      LOGFUNC_t x_log = va_arg(ap, LOGFUNC_t);
      void *log_arg = va_arg(ap, void*);
      ATOMIC_STORE(&__global_config.x_log, x_log);
      ATOMIC_STORE(&__global_config.log_arg, log_arg);
      break;
    }

    /* EVIDENCE-OF: R-55548-33817 The compile-time setting for URI filenames
    ** can be changed at start-time using the
    ** dbsql_config(DBSQL_CONFIG_URI,1) or
    ** dbsql_config(DBSQL_CONFIG_URI,0) configuration calls.
    */
    case DBSQL_CONFIG_URI: {
      /* EVIDENCE-OF: R-25451-61125 The DBSQL_CONFIG_URI option takes a single
      ** argument of type int. If non-zero, then URI handling is globally
      ** enabled. If the parameter is zero, then URI handling is globally
      ** disabled. */
      int open_uri = va_arg(ap, int);
      ATOMIC_STORE(&__global_config.open_uri, open_uri);
      break;
    }

    case DBSQL_CONFIG_COVERING_INDEX_SCAN: {
      /* EVIDENCE-OF: R-36592-02772 The DBSQL_CONFIG_COVERING_INDEX_SCAN
      ** option takes a single integer argument which is interpreted as a
      ** boolean in order to enable or disable the use of covering indices for
      ** full table scans in the query optimizer. */
      __global_config.use_cis = va_arg(ap, int);
      break;
    }

#ifdef DBSQL_ENABLE_SQLLOG
    case DBSQL_CONFIG_SQLLOG: {
      typedef void(*SQLLOGFUNC_t)(void*, DBSQL*, const char*, int);
      __global_config.sqllog = va_arg(ap, SQLLOGFUNC_t);
      __global_config.sqllog_arg = va_arg(ap, void *);
      break;
    }
#endif

    case DBSQL_CONFIG_MMAP_SIZE: {
      /* EVIDENCE-OF: R-58063-38258 DBSQL_CONFIG_MMAP_SIZE takes two 64-bit
      ** integer (int64_t) values that are the default mmap size limit
      ** (the default setting for PRAGMA mmap_size) and the maximum allowed
      ** mmap size limit. */
      int64_t sz_mmap = va_arg(ap, int64_t);
      int64_t mx_mmap = va_arg(ap, int64_t);
      /* EVIDENCE-OF: R-53367-43190 If either argument to this option is
      ** negative, then that argument is changed to its compile-time default.
      **
      ** EVIDENCE-OF: R-34993-45031 The maximum allowed mmap size will be
      ** silently truncated if necessary so that it does not exceed the
      ** compile-time maximum mmap size set by the DBSQL_MAX_MMAP_SIZE
      ** compile-time option.
      */
      if( mx_mmap<0 || mx_mmap>DBSQL_MAX_MMAP_SIZE ){
        mx_mmap = DBSQL_MAX_MMAP_SIZE;
      }
      if( sz_mmap<0 ) sz_mmap = DBSQL_DEFAULT_MMAP_SIZE;
      if( sz_mmap>mx_mmap) sz_mmap = mx_mmap;
      __global_config.mx_mmap = mx_mmap;
      __global_config.sz_mmap = sz_mmap;
      break;
    }

#if DBSQL_OS_WIN && defined(DBSQL_WIN32_MALLOC) /* IMP: R-04780-55815 */
    case DBSQL_CONFIG_WIN32_HEAPSIZE: {
      /* EVIDENCE-OF: R-34926-03360 DBSQL_CONFIG_WIN32_HEAPSIZE takes a 32-bit
      ** unsigned integer value that specifies the maximum size of the created
      ** heap. */
      __global_config.n_heap = va_arg(ap, int);
      break;
    }
#endif

    case DBSQL_CONFIG_PMASZ: {
      __global_config.sz_pma = va_arg(ap, unsigned int);
      break;
    }

    case DBSQL_CONFIG_STMTJRNL_SPILL: {
      __global_config.n_stmt_spill = va_arg(ap, int);
      break;
    }

#ifdef DBSQL_ENABLE_SORTER_REFERENCES
    case DBSQL_CONFIG_SORTERREF_SIZE: {
      int i_val = va_arg(ap, int);
      if( i_val<0 ){
        i_val = DBSQL_DEFAULT_SORTERREF_SIZE;
      }
      __global_config.sz_sorter_ref = (u_int32_t)i_val;
      break;
    }
#endif /* DBSQL_ENABLE_SORTER_REFERENCES */

#ifndef DBSQL_OMIT_DESERIALIZE
    case DBSQL_CONFIG_MEMDB_MAXSIZE: {
      __global_config.mx_memdb_size = va_arg(ap, int64_t);
      break;
    }
#endif /* DBSQL_OMIT_DESERIALIZE */

    case DBSQL_CONFIG_ROWID_IN_VIEW: {
      int *val = va_arg(ap,int*);
#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
      if( 0==*val ) __global_config.m_no_visible_rowid = TF_NoVisibleRowid;
      if( 1==*val ) __global_config.m_no_visible_rowid = 0;
      *val = (__global_config.m_no_visible_rowid==0);
#else
      *val = 0;
#endif
      break;
    }

    default: {
      rc = DBSQL_ERROR;
      break;
    }
  }
  va_end(ap);
  return rc;
}

/*
** Set up the lookaside buffers for a database connection.
** Return DBSQL_OK on success. 
** If lookaside is already active, return DBSQL_BUSY.
**
** The sz parameter is the number of bytes in each lookaside slot.
** The cnt parameter is the number of slots.  If buf is NULL the
** space for the lookaside memory is obtained from dbsql_malloc()
** or similar.  If buf is not NULL then it is sz*cnt bytes of memory
** to use for the lookaside memory.
*/
static int __setup_lookaside(
  DBSQL *db,    /* Database connection being configured */
  void *buf,     /* Memory to use for lookaside.  May be NULL */
  int sz,         /* Desired size of each lookaside memory slot */
  int cnt         /* Number of slots to allocate */
){
#ifndef DBSQL_OMIT_LOOKASIDE
  void *start;          /* Start of the lookaside buffer */
  int64_t sz_alloc; /* Total space set aside for lookaside memory */
  int n_big;              /* Number of full-size slots */
  int n_sm;               /* Number smaller LOOKASIDE_SMALL-byte slots */
 
  if( __lookaside_used(db,0)>0 ){
    return DBSQL_BUSY;
  }
  /* Free any existing lookaside buffer for this handle before
  ** allocating a new one so we don't have to have space for
  ** both at the same time.
  */
  if( db->lookaside.malloced ){
    dbsql_free(db->lookaside.p_start);
  }
  /* The size of a lookaside slot after ROUNDDOWN8 needs to be larger
  ** than a pointer and small enough to fit in a u16.
  */
  sz = ROUNDDOWN8(sz);
  if( sz<=(int)sizeof(lookaside_slot_t*) ) sz = 0;
  if( sz>65528 ) sz = 65528;
  /* Count must be at least 1 to be useful, but not so large as to use
  ** more than 0x7fff0000 total bytes for lookaside. */
  if( cnt<1 ) cnt = 0;
  if( sz>0 && cnt>(0x7fff0000/sz) ) cnt = 0x7fff0000/sz;
  sz_alloc = (int64_t)sz*(int64_t)cnt;
  if( sz_alloc==0 ){
    sz = 0;
    start = 0;
  }else if( buf==0 ){
    __begin_benign_malloc();
    start = __dbsql_malloc( sz_alloc );
    __end_benign_malloc();
    if( start ) sz_alloc = __malloc_size(start);
  }else{
    start = buf;
  }
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
  if( sz>=LOOKASIDE_SMALL*3 ){
    n_big = sz_alloc/(3*LOOKASIDE_SMALL+sz);
    n_sm = (sz_alloc - (int64_t)sz*(int64_t)n_big)/LOOKASIDE_SMALL;
  }else if( sz>=LOOKASIDE_SMALL*2 ){
    n_big = sz_alloc/(LOOKASIDE_SMALL+sz);
    n_sm = (sz_alloc - (int64_t)sz*(int64_t)n_big)/LOOKASIDE_SMALL;
  }else
#endif /* DBSQL_OMIT_TWOSIZE_LOOKASIDE */
  if( sz>0 ){
    n_big = sz_alloc/sz;
    n_sm = 0;
  }else{
    n_big = n_sm = 0;
  }
  db->lookaside.p_start = start;
  db->lookaside.init = 0;
  db->lookaside.p_free = 0;
  db->lookaside.sz = (u_int16_t)sz;
  db->lookaside.sz_true = (u_int16_t)sz;
  if( start ){
    int i;
    lookaside_slot_t *p;
    assert( sz > (int)sizeof(lookaside_slot_t*) );
    p = (lookaside_slot_t*)start;
    for(i=0; i<n_big; i++){
      p->next = db->lookaside.init;
      db->lookaside.init = p;
      p = (lookaside_slot_t*)&((u_int8_t*)p)[sz];
    }
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
    db->lookaside.small_init = 0;
    db->lookaside.small_free = 0;
    db->lookaside.middle = p;
    for(i=0; i<n_sm; i++){
      p->next = db->lookaside.small_init;
      db->lookaside.small_init = p;
      p = (lookaside_slot_t*)&((u_int8_t*)p)[LOOKASIDE_SMALL];
    }
#endif /* DBSQL_OMIT_TWOSIZE_LOOKASIDE */
    assert( ((uptr_t)p)<=sz_alloc + (uptr_t)start );
    db->lookaside.p_end = p;
    db->lookaside.disable = 0;
    db->lookaside.malloced = buf==0 ?1:0;
    db->lookaside.n_slot = n_big+n_sm;
  }else{
    db->lookaside.p_start = 0;
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
    db->lookaside.small_init = 0;
    db->lookaside.small_free = 0;
    db->lookaside.middle = 0;
#endif /* DBSQL_OMIT_TWOSIZE_LOOKASIDE */
    db->lookaside.p_end = 0;
    db->lookaside.disable = 1;
    db->lookaside.sz = 0;
    db->lookaside.malloced = 0;
    db->lookaside.n_slot = 0;
  }
  db->lookaside.true_end = db->lookaside.p_end;
  assert( __lookaside_used(db,0)==0 );
#endif /* DBSQL_OMIT_LOOKASIDE */
  return DBSQL_OK;
}

/*
** Return the mutex associated with a database connection.
*/
dbsql_mutex_t *dbsql_db_mutex(DBSQL *db){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  return db->mutex;
}

/*
** Free up as much memory as we can from the given database
** connection.
*/
int dbsql_db_release_memory(DBSQL *db){
  int i;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  __sm_enter_all(db);
  for(i=0; i<db->n_db; i++){
    sm_t *bt = db->dbs[i].bt;
    if( bt ){
      pager_t *pager = __sm_pager(bt);
      __sm_pager_shrink(pager);
    }
  }
  __sm_leave_all(db);
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}

/*
** Flush any dirty pages in the pager-cache for any attached database
** to disk.
*/
int dbsql_db_cacheflush(DBSQL *db){
  int i;
  int rc = DBSQL_OK;
  int seen_busy = 0;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  __sm_enter_all(db);
  for(i=0; rc==DBSQL_OK && i<db->n_db; i++){
    sm_t *bt = db->dbs[i].bt;
    if( bt && __sm_txn_state(bt)==DBSQL_TXN_WRITE ){
      pager_t *pager = __sm_pager(bt);
      rc = __sm_pager_flush(pager);
      if( rc==DBSQL_BUSY ){
        seen_busy = 1;
        rc = DBSQL_OK;
      }
    }
  }
  __sm_leave_all(db);
  dbsql_mutex_leave(db->mutex);
  return ((rc==DBSQL_OK && seen_busy) ? DBSQL_BUSY : rc);
}

/*
** Configuration settings for an individual database connection
*/
int dbsql_db_config(DBSQL *db, int op, ...){
  va_list ap;
  int rc;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  va_start(ap, op);
  switch( op ){
    case DBSQL_DBCONFIG_MAINDBNAME: {
      /* IMP: R-06824-28531 */
      /* IMP: R-36257-52125 */
      db->dbs[0].db_s_name = va_arg(ap,char*);
      rc = DBSQL_OK;
      break;
    }
    case DBSQL_DBCONFIG_LOOKASIDE: {
      void *buf = va_arg(ap, void*); /* IMP: R-26835-10964 */
      int sz = va_arg(ap, int);       /* IMP: R-47871-25994 */
      int cnt = va_arg(ap, int);      /* IMP: R-04460-53386 */
      rc = __setup_lookaside(db, buf, sz, cnt);
      break;
    }
    case DBSQL_DBCONFIG_FP_DIGITS: {
      int n_in = va_arg(ap, int);
      int *out = va_arg(ap, int*);
      if( n_in>3 && n_in<24 ) db->n_fp_digit = (u_int8_t)n_in;
      if( out ) *out = db->n_fp_digit;
      rc = DBSQL_OK;
      break;
    }
    default: {
      static const struct {
        int op;      /* The opcode */
        u_int64_t mask;    /* Mask of the bit in DBSQL.flags to set/clear */
      } flag_ops[] = {
        { DBSQL_DBCONFIG_ENABLE_FKEY,           DBSQL_ForeignKeys    },
        { DBSQL_DBCONFIG_ENABLE_TRIGGER,        DBSQL_EnableTrigger  },
        { DBSQL_DBCONFIG_ENABLE_VIEW,           DBSQL_EnableView     },
        { DBSQL_DBCONFIG_ENABLE_FTS3_TOKENIZER, DBSQL_Fts3Tokenizer  },
        { DBSQL_DBCONFIG_ENABLE_LOAD_EXTENSION, DBSQL_LoadExtension  },
        { DBSQL_DBCONFIG_NO_CKPT_ON_CLOSE,      DBSQL_NoCkptOnClose  },
        { DBSQL_DBCONFIG_ENABLE_QPSG,           DBSQL_EnableQPSG     },
        { DBSQL_DBCONFIG_TRIGGER_EQP,           DBSQL_TriggerEQP     },
        { DBSQL_DBCONFIG_RESET_DATABASE,        DBSQL_ResetDatabase  },
        { DBSQL_DBCONFIG_DEFENSIVE,             DBSQL_Defensive      },
        { DBSQL_DBCONFIG_WRITABLE_SCHEMA,       DBSQL_WriteSchema|
                                                 DBSQL_NoSchemaError  },
        { DBSQL_DBCONFIG_LEGACY_ALTER_TABLE,    DBSQL_LegacyAlter    },
        { DBSQL_DBCONFIG_DQS_DDL,               DBSQL_DqsDDL         },
        { DBSQL_DBCONFIG_DQS_DML,               DBSQL_DqsDML         },
        { DBSQL_DBCONFIG_LEGACY_FILE_FORMAT,    DBSQL_LegacyFileFmt  },
        { DBSQL_DBCONFIG_TRUSTED_SCHEMA,        DBSQL_TrustedSchema  },
        { DBSQL_DBCONFIG_STMT_SCANSTATUS,       DBSQL_StmtScanStatus },
        { DBSQL_DBCONFIG_REVERSE_SCANORDER,     DBSQL_ReverseOrder   },
        { DBSQL_DBCONFIG_ENABLE_ATTACH_CREATE,  DBSQL_AttachCreate   },
        { DBSQL_DBCONFIG_ENABLE_ATTACH_WRITE,   DBSQL_AttachWrite    },
        { DBSQL_DBCONFIG_ENABLE_COMMENTS,       DBSQL_Comments       },
      };
      unsigned int i;
      rc = DBSQL_ERROR; /* IMP: R-42790-23372 */
      for(i=0; i<ARRAY_SIZE(flag_ops); i++){
        if( flag_ops[i].op==op ){
          int onoff = va_arg(ap, int);
          int *res = va_arg(ap, int*);
          u_int64_t old_flags = db->flags;
          if( onoff>0 ){
            db->flags |= flag_ops[i].mask;
          }else if( onoff==0 ){
            db->flags &= ~(u_int64_t)flag_ops[i].mask;
          }
          if( old_flags!=db->flags ){
            __expire_prepared_statements(db, 0);
          }
          if( res ){
            *res = (db->flags & flag_ops[i].mask)!=0;
          }
          rc = DBSQL_OK;
          break;
        }
      }
      break;
    }
  }
  va_end(ap);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** This is the default collating function named "BINARY" which is always
** available.
*/
static int __bin_coll_func(
  void *__not_used,
  int n_key1, const void *key1,
  int n_key2, const void *key2
){
  int rc, n;
  UNUSED_PARAMETER(__not_used);
  n = n_key1<n_key2 ? n_key1 : n_key2;
  /* EVIDENCE-OF: R-65033-28449 The built-in BINARY collation compares
  ** strings byte by byte using the memcmp() function from the standard C
  ** library. */
  assert( key1 && key2 );
  rc = memcmp(key1, key2, n);
  if( rc==0 ){
    rc = n_key1 - n_key2;
  }
  return rc;
}

/*
** This is the collating function named "RTRIM" which is always
** available.  Ignore trailing spaces.
*/
static int __rtrim_coll_func(
  void *user,
  int n_key1, const void *key1,
  int n_key2, const void *key2
){
  const u_int8_t *k1 = (const u_int8_t*)key1;
  const u_int8_t *k2 = (const u_int8_t*)key2;
  while( n_key1 && k1[n_key1-1]==' ' ) n_key1--;
  while( n_key2 && k2[n_key2-1]==' ' ) n_key2--;
  return __bin_coll_func(user, n_key1, key1, n_key2, key2);
}

/*
** Return true if coll_seq_t is the default built-in BINARY.
*/
int __is_binary(const coll_seq_t *p){
  assert( p==0 || p->cmp!=__bin_coll_func || strcmp(p->name,"BINARY")==0 );
  return p==0 || p->cmp==__bin_coll_func;
}

/*
** Another built-in collating sequence: NOCASE.
**
** This collating sequence is intended to be used for "case independent
** comparison". dbsql's knowledge of upper and lower case equivalents
** extends only to the 26 characters used in the English language.
**
** At the moment there is only a UTF-8 implementation.
*/
static int __nocase_collating_func(
  void *__not_used,
  int n_key1, const void *key1,
  int n_key2, const void *key2
){
  int r = __str_nicmp(
      (const char *)key1, (const char *)key2, (n_key1<n_key2)?n_key1:n_key2);
  UNUSED_PARAMETER(__not_used);
  if( 0==r ){
    r = n_key1-n_key2;
  }
  return r;
}

/*
** Return the ROWID of the most recent insert
*/
int64_t dbsql_last_insert_rowid(DBSQL *db){
  int64_t i_ret;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  i_ret = db->last_rowid;
  dbsql_mutex_leave(db->mutex);
  return i_ret;
}

/*
** Set the value returned by the dbsql_last_insert_rowid() API function.
*/
void dbsql_set_last_insert_rowid(DBSQL *db, int64_t i_rowid){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  db->last_rowid = i_rowid;
  dbsql_mutex_leave(db->mutex);
}

/*
** Return the number of changes in the most recent call to dbsql_exec().
*/
int64_t dbsql_changes64(DBSQL *db){
  int64_t i_ret;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  i_ret = db->n_change;
  dbsql_mutex_leave(db->mutex);
  return i_ret;
}
int dbsql_changes(DBSQL *db){
  return (int)dbsql_changes64(db);
}

/*
** Return the number of changes since the database handle was opened.
*/
int64_t dbsql_total_changes64(DBSQL *db){
  int64_t i_ret;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  i_ret = db->n_total_change;
  dbsql_mutex_leave(db->mutex);
  return i_ret;
}
int dbsql_total_changes(DBSQL *db){
  return (int)dbsql_total_changes64(db);
}

/*
** Close all open savepoints. This function only manipulates fields of the
** database handle object, it does not close any savepoints that may be open
** at the b-tree/pager level.
*/
void __close_savepoints(DBSQL *db){
  while( db->savepoint ){
    savepoint_t *tmp = db->savepoint;
    db->savepoint = tmp->next;
    __dbsql_db_free(db, tmp);
  }
  db->n_savepoint = 0;
  db->n_statement = 0;
  db->is_transaction_savepoint = 0;
}

/*
** Invoke the destructor function associated with func_def_t p, if any. Except,
** if this is not the last copy of the function, do not invoke it. Multiple
** copies of a single function are created when create_function() is called
** with DBSQL_ANY as the encoding.
*/
static void __function_destroy(DBSQL *db, func_def_t *p){
  func_destructor_t *destructor;
  assert( (p->func_flags & DBSQL_FUNC_BUILTIN)==0 );
  destructor = p->u.destructor;
  if( destructor ){
    destructor->n_ref--;
    if( destructor->n_ref==0 ){
      destructor->destroy(destructor->user_data);
      __dbsql_db_free(db, destructor);
    }
  }
}

/*
** Disconnect all dbsql_vtab_t objects that belong to database connection
** db. This is called when db is being closed.
*/
static void __disconnect_all_vtab(DBSQL *db){
#ifndef DBSQL_OMIT_VIRTUALTABLE
  int i;
  hash_elem_t *p;
  __sm_enter_all(db);
  for(i=0; i<db->n_db; i++){
    schema_t *schema = db->dbs[i].schema;
    if( schema ){
      for(p=dbsql_hash_first(&schema->tbl_hash); p; p=dbsql_hash_next(p)){
        table_t *tab = (table_t *)dbsql_hash_data(p);
        if( IS_VIRTUAL(tab) ) __vtab_disconnect(db, tab);
      }
    }
  }
  for(p=dbsql_hash_first(&db->modules); p; p=dbsql_hash_next(p)){
    module_def_t *mod = (module_def_t *)dbsql_hash_data(p);
    if( mod->epo_tab ){
      __vtab_disconnect(db, mod->epo_tab);
    }
  }
  __vtab_unlock_list(db);
  __sm_leave_all(db);
#else
  UNUSED_PARAMETER(db);
#endif
}

/*
** Return TRUE if database connection db has unfinalized prepared
** statements or unfinished dbsql_backup_t objects. 
*/
static int __connection_is_busy(DBSQL *db){
  int j;
  assert( dbsql_mutex_held(db->mutex) );
  if( db->vdbe ) return 1;
  for(j=0; j<db->n_db; j++){
    sm_t *bt = db->dbs[j].bt;
    if( bt && __sm_is_in_backup(bt) ) return 1;
  }
  return 0;
}

/*
** Close an existing dbsql database
*/
static int __close(DBSQL *db, int force_zombie){
  if( !db ){
    /* EVIDENCE-OF: R-63257-11740 Calling dbsql_close() or
    ** dbsql_close_v2() with a NULL pointer argument is a harmless no-op. */
    return DBSQL_OK;
  }
  if( !__safety_check_sick_or_ok(db) ){
    return DBSQL_MISUSE_BKPT;
  }
  dbsql_mutex_enter(db->mutex);
  if( db->m_trace & DBSQL_TRACE_CLOSE ){
    db->trace.v2(DBSQL_TRACE_CLOSE, db->trace_arg, db, 0);
  }

  /* Force disconnect calls on all virtual tables */
  __disconnect_all_vtab(db);

  /* If a transaction is open, the __disconnect_all_vtab() call above
  ** will not have called the disconnect() method on any virtual
  ** tables in the db->v_trans[] array. The following __vtab_rollback()
  ** call will do so. We need to do this before the check for active
  ** SQL statements below, as the v-table implementation may be storing
  ** some prepared statements internally.
  */
  __vtab_rollback(db);

  /* Legacy behavior (dbsql_close() behavior) is to return
  ** DBSQL_BUSY if the connection can not be closed immediately.
  */
  if( !force_zombie && __connection_is_busy(db) ){
    __error_with_msg(db, DBSQL_BUSY, "unable to close due to unfinalized "
       "statements or unfinished backups");
    dbsql_mutex_leave(db->mutex);
    return DBSQL_BUSY;
  }

#ifdef DBSQL_ENABLE_SQLLOG
  if( __global_config.sqllog ){
    /* Closing the handle. Fourth parameter is passed the value 2. */
    __global_config.sqllog(__global_config.sqllog_arg, db, 0, 2);
  }
#endif

  while( db->db_data ){
    db_client_data_t *p = db->db_data;
    db->db_data = p->next;
    assert( p->data!=0 );
    if( p->destructor ) p->destructor(p->data);
    dbsql_free(p);
  }

  /* Convert the connection into a zombie and then close it.
  */
  db->open_state = DBSQL_STATE_ZOMBIE;
  __leave_mutex_and_close_zombie(db);
  return DBSQL_OK;
}

/*
** Return the transaction state for a single databse, or the maximum
** transaction state over all attached databases if schema is null.
*/
int dbsql_txn_state(DBSQL *db, const char *schema){
  int i_db, n_db;
  int i_txn = -1;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return -1;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  if( schema ){
    n_db = i_db = __find_db_name(db, schema);
    if( i_db<0 ) n_db--;
  }else{
    i_db = 0;
    n_db = db->n_db-1;
  }
  for(; i_db<=n_db; i_db++){
    sm_t *bt = db->dbs[i_db].bt;
    int x = bt!=0 ? __sm_txn_state(bt) : DBSQL_TXN_NONE;
    if( x>i_txn ) i_txn = x;
  }
  dbsql_mutex_leave(db->mutex);
  return i_txn;
}

/*
** Two variations on the public interface for closing a database
** connection. The dbsql_close() version returns DBSQL_BUSY and
** leaves the connection open if there are unfinalized prepared
** statements or unfinished dbsql_backups.  The dbsql_close_v2()
** version forces the connection to become a zombie if there are
** unclosed resources, and arranges for deallocation when the last
** prepare statement or dbsql_backup_t closes.
*/
int dbsql_close(DBSQL *db){ return __close(db,0); }
int dbsql_close_v2(DBSQL *db){ return __close(db,1); }


/*
** Close the mutex on database connection db.
**
** Furthermore, if database connection db is a zombie (meaning that there
** has been a prior call to dbsql_close(db) or dbsql_close_v2(db)) and
** every dbsql_stmt_t has now been finalized and every dbsql_backup_t has
** finished, then free all resources.
*/
void __leave_mutex_and_close_zombie(DBSQL *db){
  hash_elem_t *i;                    /* hash_t table iterator */
  int j;

  /* If there are outstanding dbsql_stmt_t or dbsql_backup_t objects
  ** or if the connection has not yet been closed by dbsql_close_v2(),
  ** then just leave the mutex and return.
  */
  if( db->open_state!=DBSQL_STATE_ZOMBIE || __connection_is_busy(db) ){
    dbsql_mutex_leave(db->mutex);
    return;
  }

  /* If we reach this point, it means that the database connection has
  ** closed all dbsql_stmt_t and dbsql_backup_t objects and has been
  ** passed to dbsql_close (meaning that it is a zombie).  Therefore,
  ** go ahead and free all resources.
  */

  /* If a transaction is open, roll it back. This also ensures that if
  ** any database schemas have been modified by an uncommitted transaction
  ** they are reset. And that the required b-tree mutex is held to make
  ** the pager rollback and schema reset an atomic operation. */
  __rollback_all(db, DBSQL_OK);

  /* Free any outstanding savepoint_t structures. */
  __close_savepoints(db);

  /* Close all database connections */
  for(j=0; j<db->n_db; j++){
    struct db_t *p_db = &db->dbs[j];
    if( p_db->bt ){
      __sm_close(p_db->bt);
      p_db->bt = 0;
      if( j!=1 ){
        p_db->schema = 0;
      }
    }
  }
  /* Clear the TEMP schema separately and last */
  if( db->dbs[1].schema ){
    __schema_clear(db->dbs[1].schema);
    assert( db->dbs[1].schema->trig_hash.count==0 );
  }
  __vtab_unlock_list(db);

  /* Free up the array of auxiliary databases */
  __collapse_database_array(db);
  assert( db->n_db<=2 );
  assert( db->dbs==db->db_statics );

  /* Tell the code in notify.c that the connection no longer holds any
  ** locks and does not require any further unlock-notify callbacks.
  */
  __connection_closed(db);

  for(i=dbsql_hash_first(&db->funcs); i; i=dbsql_hash_next(i)){
    func_def_t *next, *p;
    p = dbsql_hash_data(i);
    do{
      __function_destroy(db, p);
      next = p->next;
      __dbsql_db_free(db, p);
      p = next;
    }while( p );
  }
  __hash_clear(&db->funcs);
  for(i=dbsql_hash_first(&db->coll_seqs); i; i=dbsql_hash_next(i)){
    coll_seq_t *coll = (coll_seq_t *)dbsql_hash_data(i);
    /* Invoke any destructors registered for collation sequence user data. */
    for(j=0; j<3; j++){
      if( coll[j].x_del ){
        coll[j].x_del(coll[j].user);
      }
    }
    __dbsql_db_free(db, coll);
  }
  __hash_clear(&db->coll_seqs);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  for(i=dbsql_hash_first(&db->modules); i; i=dbsql_hash_next(i)){
    module_def_t *mod = (module_def_t *)dbsql_hash_data(i);
    __vtab_eponymous_table_clear(db, mod);
    __vtab_module_unref(db, mod);
  }
  __hash_clear(&db->modules);
#endif

  __dbsql_error(db, DBSQL_OK); /* Deallocates any cached error strings. */
  __value_free(db->err);
  __close_extensions(db);

  db->open_state = DBSQL_STATE_ERROR;

  /* The temp-database schema is allocated differently from the other schema
  ** objects (using dbsql_malloc() directly, instead of __sm_schema()).
  ** So it needs to be freed here. Todo: Why not roll the temp schema into
  ** the same dbsql_malloc() as the one that allocates the database
  ** structure?
  */
  __dbsql_db_free(db, db->dbs[1].schema);
  if( db->autovac_destr ){
    db->autovac_destr(db->autovac_pages_arg);
  }
  dbsql_mutex_leave(db->mutex);
  db->open_state = DBSQL_STATE_CLOSED;
  dbsql_mutex_free(db->mutex);
  assert( __lookaside_used(db,0)==0 );
  if( db->lookaside.malloced ){
    dbsql_free(db->lookaside.p_start);
  }
  dbsql_free(db);
}

/*
** Rollback all database files.  If trip_code is not DBSQL_OK, then
** any write cursors are invalidated ("tripped" - as in "tripping a circuit
** breaker") and made to return trip_code if there are any further
** attempts to use that cursor.  Read cursors remain open and valid
** but are "saved" in case the table pages are moved around.
*/
void __rollback_all(DBSQL *db, int trip_code){
  int i;
  int in_trans = 0;
  int schema_change;
  assert( dbsql_mutex_held(db->mutex) );
  __begin_benign_malloc();

  /* Obtain all b-tree mutexes before making any calls to __btree_rollback().
  ** This is important in case the transaction being rolled back has
  ** modified the database schema. If the b-tree mutexes are not taken
  ** here, then another shared-cache connection might sneak in between
  ** the database rollback and schema reset, which can cause false
  ** corruption reports in some cases.  */
  __sm_enter_all(db);
  schema_change = (db->m_db_flags & DBFLAG_SchemaChange)!=0 && db->init.busy==0;

  for(i=0; i<db->n_db; i++){
    sm_t *p = db->dbs[i].bt;
    if( p ){
      if( __sm_txn_state(p)==DBSQL_TXN_WRITE ){
        in_trans = 1;
      }
      __sm_rollback(p, trip_code, !schema_change);
    }
  }
  __vtab_rollback(db);
  __end_benign_malloc();

  if( schema_change ){
    __expire_prepared_statements(db, 0);
    __reset_all_schemas_of_connection(db);
  }
  __sm_leave_all(db);

  /* Any deferred constraint violations have now been resolved. */
  db->n_deferred_cons = 0;
  db->n_deferred_imm_cons = 0;
  db->flags &= ~(u_int64_t)(DBSQL_DeferFKs|DBSQL_CorruptRdOnly);

  /* If one has been configured, invoke the rollback-hook callback */
  if( db->rollback_callback && (in_trans || !db->auto_commit) ){
    db->rollback_callback(db->rollback_arg);
  }
}

/*
** Return a static string containing the name corresponding to the error code
** specified in the argument.
*/
#if defined(DBSQL_NEED_ERR_NAME)
const char *__err_name(int rc){
  const char *name = 0;
  int i, orig_rc = rc;
  for(i=0; i<2 && name==0; i++, rc &= 0xff){
    switch( rc ){
      case DBSQL_OK:                 name = "DBSQL_OK";                break;
      case DBSQL_ERROR:              name = "DBSQL_ERROR";             break;
      case DBSQL_ERROR_SNAPSHOT:     name = "DBSQL_ERROR_SNAPSHOT";    break;
      case DBSQL_ERROR_RETRY:        name = "DBSQL_ERROR_RETRY";       break;
      case DBSQL_ERROR_MISSING_COLLSEQ:
                                name = "DBSQL_ERROR_MISSING_COLLSEQ";   break;
      case DBSQL_INTERNAL:           name = "DBSQL_INTERNAL";          break;
      case DBSQL_PERM:               name = "DBSQL_PERM";              break;
      case DBSQL_ABORT:              name = "DBSQL_ABORT";             break;
      case DBSQL_ABORT_ROLLBACK:     name = "DBSQL_ABORT_ROLLBACK";    break;
      case DBSQL_BUSY:               name = "DBSQL_BUSY";              break;
      case DBSQL_BUSY_RECOVERY:      name = "DBSQL_BUSY_RECOVERY";     break;
      case DBSQL_BUSY_SNAPSHOT:      name = "DBSQL_BUSY_SNAPSHOT";     break;
      case DBSQL_LOCKED:             name = "DBSQL_LOCKED";            break;
      case DBSQL_LOCKED_SHAREDCACHE: name = "DBSQL_LOCKED_SHAREDCACHE";break;
      case DBSQL_NOMEM:              name = "DBSQL_NOMEM";             break;
      case DBSQL_READONLY:           name = "DBSQL_READONLY";          break;
      case DBSQL_READONLY_RECOVERY:  name = "DBSQL_READONLY_RECOVERY"; break;
      case DBSQL_READONLY_CANTINIT:  name = "DBSQL_READONLY_CANTINIT"; break;
      case DBSQL_READONLY_ROLLBACK:  name = "DBSQL_READONLY_ROLLBACK"; break;
      case DBSQL_READONLY_DBMOVED:   name = "DBSQL_READONLY_DBMOVED";  break;
      case DBSQL_READONLY_DIRECTORY: name = "DBSQL_READONLY_DIRECTORY";break;
      case DBSQL_INTERRUPT:          name = "DBSQL_INTERRUPT";         break;
      case DBSQL_IOERR:              name = "DBSQL_IOERR";             break;
      case DBSQL_IOERR_READ:         name = "DBSQL_IOERR_READ";        break;
      case DBSQL_IOERR_SHORT_READ:   name = "DBSQL_IOERR_SHORT_READ";  break;
      case DBSQL_IOERR_WRITE:        name = "DBSQL_IOERR_WRITE";       break;
      case DBSQL_IOERR_FSYNC:        name = "DBSQL_IOERR_FSYNC";       break;
      case DBSQL_IOERR_DIR_FSYNC:    name = "DBSQL_IOERR_DIR_FSYNC";   break;
      case DBSQL_IOERR_TRUNCATE:     name = "DBSQL_IOERR_TRUNCATE";    break;
      case DBSQL_IOERR_FSTAT:        name = "DBSQL_IOERR_FSTAT";       break;
      case DBSQL_IOERR_UNLOCK:       name = "DBSQL_IOERR_UNLOCK";      break;
      case DBSQL_IOERR_RDLOCK:       name = "DBSQL_IOERR_RDLOCK";      break;
      case DBSQL_IOERR_DELETE:       name = "DBSQL_IOERR_DELETE";      break;
      case DBSQL_IOERR_NOMEM:        name = "DBSQL_IOERR_NOMEM";       break;
      case DBSQL_IOERR_ACCESS:       name = "DBSQL_IOERR_ACCESS";      break;
      case DBSQL_IOERR_CHECKRESERVEDLOCK:
                                name = "DBSQL_IOERR_CHECKRESERVEDLOCK"; break;
      case DBSQL_IOERR_LOCK:         name = "DBSQL_IOERR_LOCK";        break;
      case DBSQL_IOERR_CLOSE:        name = "DBSQL_IOERR_CLOSE";       break;
      case DBSQL_IOERR_DIR_CLOSE:    name = "DBSQL_IOERR_DIR_CLOSE";   break;
      case DBSQL_IOERR_SHMOPEN:      name = "DBSQL_IOERR_SHMOPEN";     break;
      case DBSQL_IOERR_SHMSIZE:      name = "DBSQL_IOERR_SHMSIZE";     break;
      case DBSQL_IOERR_SHMLOCK:      name = "DBSQL_IOERR_SHMLOCK";     break;
      case DBSQL_IOERR_SHMMAP:       name = "DBSQL_IOERR_SHMMAP";      break;
      case DBSQL_IOERR_SEEK:         name = "DBSQL_IOERR_SEEK";        break;
      case DBSQL_IOERR_DELETE_NOENT: name = "DBSQL_IOERR_DELETE_NOENT";break;
      case DBSQL_IOERR_MMAP:         name = "DBSQL_IOERR_MMAP";        break;
      case DBSQL_IOERR_GETTEMPPATH:  name = "DBSQL_IOERR_GETTEMPPATH"; break;
      case DBSQL_IOERR_CONVPATH:     name = "DBSQL_IOERR_CONVPATH";    break;
      case DBSQL_CORRUPT:            name = "DBSQL_CORRUPT";           break;
      case DBSQL_CORRUPT_VTAB:       name = "DBSQL_CORRUPT_VTAB";      break;
      case DBSQL_NOTFOUND:           name = "DBSQL_NOTFOUND";          break;
      case DBSQL_FULL:               name = "DBSQL_FULL";              break;
      case DBSQL_CANTOPEN:           name = "DBSQL_CANTOPEN";          break;
      case DBSQL_CANTOPEN_NOTEMPDIR: name = "DBSQL_CANTOPEN_NOTEMPDIR";break;
      case DBSQL_CANTOPEN_ISDIR:     name = "DBSQL_CANTOPEN_ISDIR";    break;
      case DBSQL_CANTOPEN_FULLPATH:  name = "DBSQL_CANTOPEN_FULLPATH"; break;
      case DBSQL_CANTOPEN_CONVPATH:  name = "DBSQL_CANTOPEN_CONVPATH"; break;
      case DBSQL_CANTOPEN_SYMLINK:   name = "DBSQL_CANTOPEN_SYMLINK";  break;
      case DBSQL_PROTOCOL:           name = "DBSQL_PROTOCOL";          break;
      case DBSQL_EMPTY:              name = "DBSQL_EMPTY";             break;
      case DBSQL_SCHEMA:             name = "DBSQL_SCHEMA";            break;
      case DBSQL_TOOBIG:             name = "DBSQL_TOOBIG";            break;
      case DBSQL_CONSTRAINT:         name = "DBSQL_CONSTRAINT";        break;
      case DBSQL_CONSTRAINT_UNIQUE:  name = "DBSQL_CONSTRAINT_UNIQUE"; break;
      case DBSQL_CONSTRAINT_TRIGGER: name = "DBSQL_CONSTRAINT_TRIGGER";break;
      case DBSQL_CONSTRAINT_FOREIGNKEY:
                                name = "DBSQL_CONSTRAINT_FOREIGNKEY";   break;
      case DBSQL_CONSTRAINT_CHECK:   name = "DBSQL_CONSTRAINT_CHECK";  break;
      case DBSQL_CONSTRAINT_PRIMARYKEY:
                                name = "DBSQL_CONSTRAINT_PRIMARYKEY";   break;
      case DBSQL_CONSTRAINT_NOTNULL: name = "DBSQL_CONSTRAINT_NOTNULL";break;
      case DBSQL_CONSTRAINT_COMMITHOOK:
                                name = "DBSQL_CONSTRAINT_COMMITHOOK";   break;
      case DBSQL_CONSTRAINT_VTAB:    name = "DBSQL_CONSTRAINT_VTAB";   break;
      case DBSQL_CONSTRAINT_FUNCTION:
                                name = "DBSQL_CONSTRAINT_FUNCTION";     break;
      case DBSQL_CONSTRAINT_ROWID:   name = "DBSQL_CONSTRAINT_ROWID";  break;
      case DBSQL_MISMATCH:           name = "DBSQL_MISMATCH";          break;
      case DBSQL_MISUSE:             name = "DBSQL_MISUSE";            break;
      case DBSQL_NOLFS:              name = "DBSQL_NOLFS";             break;
      case DBSQL_AUTH:               name = "DBSQL_AUTH";              break;
      case DBSQL_FORMAT:             name = "DBSQL_FORMAT";            break;
      case DBSQL_RANGE:              name = "DBSQL_RANGE";             break;
      case DBSQL_NOTADB:             name = "DBSQL_NOTADB";            break;
      case DBSQL_ROW:                name = "DBSQL_ROW";               break;
      case DBSQL_NOTICE:             name = "DBSQL_NOTICE";            break;
      case DBSQL_NOTICE_RECOVER_WAL: name = "DBSQL_NOTICE_RECOVER_WAL";break;
      case DBSQL_NOTICE_RECOVER_ROLLBACK:
                                name = "DBSQL_NOTICE_RECOVER_ROLLBACK"; break;
      case DBSQL_NOTICE_RBU:         name = "DBSQL_NOTICE_RBU"; break;
      case DBSQL_WARNING:            name = "DBSQL_WARNING";           break;
      case DBSQL_WARNING_AUTOINDEX:  name = "DBSQL_WARNING_AUTOINDEX"; break;
      case DBSQL_DONE:               name = "DBSQL_DONE";              break;
    }
  }
  if( name==0 ){
    static char buf[50];
    dbsql_snprintf(sizeof(buf), buf, "DBSQL_UNKNOWN(%d)", orig_rc);
    name = buf;
  }
  return name;
}
#endif

/*
** Return a static string that describes the kind of error specified in the
** argument.
*/
const char *__err_str(int rc){
  static const char* const msgs[] = {
    /* DBSQL_OK          */ "not an error",
    /* DBSQL_ERROR       */ "SQL logic error",
    /* DBSQL_INTERNAL    */ 0,
    /* DBSQL_PERM        */ "access permission denied",
    /* DBSQL_ABORT       */ "query aborted",
    /* DBSQL_BUSY        */ "database is locked",
    /* DBSQL_LOCKED      */ "database table is locked",
    /* DBSQL_NOMEM       */ "out of memory",
    /* DBSQL_READONLY    */ "attempt to write a readonly database",
    /* DBSQL_INTERRUPT   */ "interrupted",
    /* DBSQL_IOERR       */ "disk I/O error",
    /* DBSQL_CORRUPT     */ "database disk image is malformed",
    /* DBSQL_NOTFOUND    */ "unknown operation",
    /* DBSQL_FULL        */ "database or disk is full",
    /* DBSQL_CANTOPEN    */ "unable to open database file",
    /* DBSQL_PROTOCOL    */ "locking protocol",
    /* DBSQL_EMPTY       */ 0,
    /* DBSQL_SCHEMA      */ "database schema has changed",
    /* DBSQL_TOOBIG      */ "string or blob too big",
    /* DBSQL_CONSTRAINT  */ "constraint failed",
    /* DBSQL_MISMATCH    */ "datatype mismatch",
    /* DBSQL_MISUSE      */ "bad parameter or other API misuse",
#ifdef DBSQL_DISABLE_LFS
    /* DBSQL_NOLFS       */ "large file support is disabled",
#else
    /* DBSQL_NOLFS       */ 0,
#endif
    /* DBSQL_AUTH        */ "authorization denied",
    /* DBSQL_FORMAT      */ 0,
    /* DBSQL_RANGE       */ "column index out of range",
    /* DBSQL_NOTADB      */ "file is not a database",
    /* DBSQL_NOTICE      */ "notification message",
    /* DBSQL_WARNING     */ "warning message",
  };
  const char *err = "unknown error";
  switch( rc ){
    case DBSQL_ABORT_ROLLBACK: {
      err = "abort due to ROLLBACK";
      break;
    }
    case DBSQL_ROW: {
      err = "another row available";
      break;
    }
    case DBSQL_DONE: {
      err = "no more rows available";
      break;
    }
    default: {
      rc &= 0xff;
      if( ALWAYS(rc>=0) && rc<ARRAY_SIZE(msgs) && msgs[rc]!=0 ){
        err = msgs[rc];
      }
      break;
    }
  }
  return err;
}

/*
** This routine implements a busy callback that sleeps and tries
** again until a timeout value is reached.  The timeout value is
** an integer number of milliseconds passed in as the first
** argument.
**
** Return non-zero to retry the lock.  Return zero to stop trying
** and cause dbsql to return DBSQL_BUSY.
*/
static int dbsql_default_busy_callback(
  void *ptr,               /* Database connection */
  int count                /* Number of times table has been busy */
){
#if DBSQL_OS_WIN || !defined(HAVE_NANOSLEEP) || HAVE_NANOSLEEP
  /* This case is for systems that have support for sleeping for fractions of
  ** a second.  Examples:  All windows systems, unix systems with nanosleep() */
  static const u_int8_t delays[] =
     { 1, 2, 5, 10, 15, 20, 25, 25,  25,  50,  50, 100 };
  static const u_int8_t totals[] =
     { 0, 1, 3,  8, 18, 33, 53, 78, 103, 128, 178, 228 };
# define NDELAY ARRAY_SIZE(delays)
  DBSQL *db = (DBSQL *)ptr;
  int tmout = db->busy_timeout;
  int delay, prior;

  assert( count>=0 );
  if( count < NDELAY ){
    delay = delays[count];
    prior = totals[count];
  }else{
    delay = delays[NDELAY-1];
    prior = totals[NDELAY-1] + delay*(count-(NDELAY-1));
  }
  if( prior + delay > tmout ){
    delay = tmout - prior;
    if( delay<=0 ) return 0;
  }
  __dbsql_os_sleep(db->vfs, delay*1000);
  return 1;
#else
  /* This case for unix systems that lack usleep() support.  Sleeping
  ** must be done in increments of whole seconds */
  DBSQL *db = (DBSQL *)ptr;
  int tmout = ((DBSQL *)ptr)->busy_timeout;
  if( (count+1)*1000 > tmout ){
    return 0;
  }
  __dbsql_os_sleep(db->vfs, 1000000);
  return 1;
#endif
}

/*
** Invoke the given busy handler.
**
** This routine is called when an operation failed to acquire a
** lock on VFS file file.
**
** If this routine returns non-zero, the lock is retried.  If it
** returns 0, the operation aborts with an DBSQL_BUSY error.
*/
int __invoke_busy_handler(busy_handler_t *p){
  int rc;
  if( p->busy_handler==0 || p->n_busy<0 ) return 0;
  rc = p->busy_handler(p->busy_arg, p->n_busy);
  if( rc==0 ){
    p->n_busy = -1;
  }else{
    p->n_busy++;
  }
  return rc;
}

/*
** This routine sets the busy callback for an Sqlite database to the
** given callback function with the given argument.
*/
int dbsql_busy_handler(
  DBSQL *db,
  int (*__x_busy)(void*,int),
  void *arg
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  db->busy_handler.busy_handler = __x_busy;
  db->busy_handler.busy_arg = arg;
  db->busy_handler.n_busy = 0;
  db->busy_timeout = 0;
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
  db->setlk_timeout = 0;
#endif
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}

#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
/*
** This routine sets the progress callback for an Sqlite database to the
** given callback function with the given argument. The progress callback will
** be invoked every n_ops opcodes.
*/
void dbsql_progress_handler(
  DBSQL *db,
  int n_ops,
  int (*progress)(void*),
  void *arg
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  if( n_ops>0 ){
    db->progress = progress;
    db->n_progress_ops = (unsigned)n_ops;
    db->progress_arg = arg;
  }else{
    db->progress = 0;
    db->n_progress_ops = 0;
    db->progress_arg = 0;
  }
  dbsql_mutex_leave(db->mutex);
}
#endif


/*
** This routine installs a default busy handler that waits for the
** specified number of milliseconds before returning 0.
*/
int dbsql_busy_timeout(DBSQL *db, int ms){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  if( ms>0 ){
    dbsql_busy_handler(db, (int(*)(void*,int))dbsql_default_busy_callback,
                             (void*)db);
    db->busy_timeout = ms;
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
    db->setlk_timeout = ms;
#endif
  }else{
    dbsql_busy_handler(db, 0, 0);
  }
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}

/*
** Set the setlk timeout value.
*/
int dbsql_setlk_timeout(DBSQL *db, int ms, int flags){
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
  int i_db;
  int boc = ((flags & DBSQL_SETLK_BLOCK_ON_CONNECT) ? 1 : 0);
#endif
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  if( ms<-1 ) return DBSQL_RANGE;
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
  dbsql_mutex_enter(db->mutex);
  db->setlk_timeout = ms;
  db->setlk_flags = flags;
  __sm_enter_all(db);
  for(i_db=0; i_db<db->n_db; i_db++){
    sm_t *bt = db->dbs[i_db].bt;
    if( bt ){
      dbsql_file_t *fd = __sm_pager_file(__sm_pager(bt));
      __dbsql_os_file_control_hint(fd, DBSQL_FCNTL_BLOCK_ON_CONNECT, (void*)&boc);
    }
  }
  __sm_leave_all(db);
  dbsql_mutex_leave(db->mutex);
#endif
#if !defined(DBSQL_ENABLE_API_ARMOR) && !defined(DBSQL_ENABLE_SETLK_TIMEOUT)
  UNUSED_PARAMETER(db);
  UNUSED_PARAMETER(flags);
#endif
  return DBSQL_OK;
}

/*
** Cause any pending operation to stop at its earliest opportunity.
*/
void dbsql_interrupt(DBSQL *db){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db)
   && (db==0 || db->open_state!=DBSQL_STATE_ZOMBIE)
  ){
    (void)DBSQL_MISUSE_BKPT;
    return;
  }
#endif
  ATOMIC_STORE(&db->u1.is_interrupted, 1);
}

/*
** Return true or false depending on whether or not an interrupt is
** pending on connection db.
*/
int dbsql_is_interrupted(DBSQL *db){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db)
   && (db==0 || db->open_state!=DBSQL_STATE_ZOMBIE)
  ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  return ATOMIC_LOAD(&db->u1.is_interrupted)!=0;
}

/*
** This function is exactly the same as dbsql_create_function(), except
** that it is designed to be called by internal code. The difference is
** that if a malloc() fails in dbsql_create_function(), an error code
** is returned and the malloc_failed flag cleared.
*/
int __create_func(
  DBSQL *db,
  const char *function_name,
  int n_arg,
  int enc,
  void *user_data,
  void (*s_func)(dbsql_func_t*,int,dbsql_value_t **),
  void (*step)(dbsql_func_t*,int,dbsql_value_t **),
  void (*final)(dbsql_func_t*),
  void (*value)(dbsql_func_t*),
  void (*inverse)(dbsql_func_t*,int,dbsql_value_t **),
  func_destructor_t *destructor
){
  func_def_t *p;
  int extra_flags;

  assert( dbsql_mutex_held(db->mutex) );
  assert( value==0 || s_func==0 );
  if( function_name==0                /* Must have a valid name */
   || (s_func!=0 && final!=0)        /* Not both s_func and final */
   || ((final==0)!=(step==0))       /* Both or neither of final and step */
   || ((value==0)!=(inverse==0))    /* Both or neither of value, inverse */
   || (n_arg<-1 || n_arg>DBSQL_MAX_FUNCTION_ARG)
   || (255<__strlen30(function_name))
  ){
    return DBSQL_MISUSE_BKPT;
  }

  assert( DBSQL_FUNC_CONSTANT==DBSQL_DETERMINISTIC );
  assert( DBSQL_FUNC_DIRECT==DBSQL_DIRECTONLY );
  extra_flags = enc &  (DBSQL_DETERMINISTIC|DBSQL_DIRECTONLY|
                       DBSQL_SUBTYPE|DBSQL_INNOCUOUS|
                       DBSQL_RESULT_SUBTYPE|DBSQL_SELFORDER1);
  enc &= (DBSQL_FUNC_ENCMASK|DBSQL_ANY);

  /* The DBSQL_INNOCUOUS flag is the same bit as DBSQL_FUNC_UNSAFE.  But
  ** the meaning is inverted.  So flip the bit. */
  assert( DBSQL_FUNC_UNSAFE==DBSQL_INNOCUOUS );
  extra_flags ^= DBSQL_FUNC_UNSAFE;  /* tag-20230109-1 */

 
#ifndef DBSQL_OMIT_UTF16
  /* If DBSQL_UTF16 is specified as the encoding type, transform this
  ** to one of DBSQL_UTF16LE or DBSQL_UTF16BE using the
  ** DBSQL_UTF16NATIVE macro. DBSQL_UTF16 is not used internally.
  **
  ** If DBSQL_ANY is specified, add three versions of the function
  ** to the hash table.
  */
  switch( enc ){
    case DBSQL_UTF16:
      enc = DBSQL_UTF16NATIVE;
      break;
    case DBSQL_ANY: {
      int rc;
      rc = __create_func(db, function_name, n_arg,
           (DBSQL_UTF8|extra_flags)^DBSQL_FUNC_UNSAFE, /* tag-20230109-1 */
           user_data, s_func, step, final, value, inverse, destructor);
      if( rc==DBSQL_OK ){
        rc = __create_func(db, function_name, n_arg,
             (DBSQL_UTF16LE|extra_flags)^DBSQL_FUNC_UNSAFE, /* tag-20230109-1*/
             user_data, s_func, step, final, value, inverse, destructor);
      }
      if( rc!=DBSQL_OK ){
        return rc;
      }
      enc = DBSQL_UTF16BE;
      break;
    }
    case DBSQL_UTF8:
    case DBSQL_UTF16LE:
    case DBSQL_UTF16BE:
      break;
    default:
      enc = DBSQL_UTF8;
      break;
  }
#else
  enc = DBSQL_UTF8;
#endif
 
  /* Check if an existing function is being overridden or deleted. If so,
  ** and there are active VMs, then return DBSQL_BUSY. If a function
  ** is being overridden/deleted but there are no active VMs, allow the
  ** operation to continue but invalidate all precompiled statements.
  */
  p = __find_function(db, function_name, n_arg, (u_int8_t)enc, 0);
  if( p && (p->func_flags & DBSQL_FUNC_ENCMASK)==(u_int32_t)enc && p->n_arg==n_arg ){
    if( db->n_vdbe_active ){
      __error_with_msg(db, DBSQL_BUSY,
        "unable to delete/modify user-function due to active statements");
      assert( !db->malloc_failed );
      return DBSQL_BUSY;
    }else{
      __expire_prepared_statements(db, 0);
    }
  }else if( s_func==0 && final==0 ){
    /* Trying to delete a function that does not exist.  This is a no-op.
    ** https://sqlite.org/forum/forumpost/726219164b */
    return DBSQL_OK;
  }

  p = __find_function(db, function_name, n_arg, (u_int8_t)enc, 1);
  assert(p || db->malloc_failed);
  if( !p ){
    return DBSQL_NOMEM_BKPT;
  }

  /* If an older version of the function with a configured destructor is
  ** being replaced invoke the destructor function here. */
  __function_destroy(db, p);

  if( destructor ){
    destructor->n_ref++;
  }
  p->u.destructor = destructor;
  p->func_flags = (p->func_flags & DBSQL_FUNC_ENCMASK) | extra_flags;
  testcase( p->func_flags & DBSQL_DETERMINISTIC );
  testcase( p->func_flags & DBSQL_DIRECTONLY );
  p->s_func = s_func ? s_func : step;
  p->finalize = final;
  p->value = value;
  p->inverse = inverse;
  p->user_data = user_data;
  p->n_arg = (u_int16_t)n_arg;
  return DBSQL_OK;
}

/*
** Worker function used by utf-8 APIs that create new functions:
**
**    dbsql_create_function()
**    dbsql_create_function_v2()
**    dbsql_create_window_function()
*/
static int __create_function_api(
  DBSQL *db,
  const char *func,
  int n_arg,
  int enc,
  void *p,
  void (*s_func)(dbsql_func_t*,int,dbsql_value_t**),
  void (*step)(dbsql_func_t*,int,dbsql_value_t**),
  void (*final)(dbsql_func_t*),
  void (*value)(dbsql_func_t*),
  void (*inverse)(dbsql_func_t*,int,dbsql_value_t**),
  void(*destroy)(void*)
){
  int rc = DBSQL_ERROR;
  func_destructor_t *arg = 0;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  if( destroy ){
    arg = (func_destructor_t *)__dbsql_malloc(sizeof(func_destructor_t));
    if( !arg ){
      __oom_fault(db);
      destroy(p);
      goto out;
    }
    arg->n_ref = 0;
    arg->destroy = destroy;
    arg->user_data = p;
  }
  rc = __create_func(db, func, n_arg, enc, p,
      s_func, step, final, value, inverse, arg
  );
  if( arg && arg->n_ref==0 ){
    assert( rc!=DBSQL_OK || (step==0 && final==0) );
    destroy(p);
    dbsql_free(arg);
  }

 out:
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** Create new user functions.
*/
int dbsql_create_function(
  DBSQL *db,
  const char *func,
  int n_arg,
  int enc,
  void *p,
  void (*s_func)(dbsql_func_t*,int,dbsql_value_t **),
  void (*step)(dbsql_func_t*,int,dbsql_value_t **),
  void (*final)(dbsql_func_t*)
){
  return __create_function_api(db, func, n_arg, enc, p, s_func, step,
                                    final, 0, 0, 0);
}
int dbsql_create_function_v2(
  DBSQL *db,
  const char *func,
  int n_arg,
  int enc,
  void *p,
  void (*s_func)(dbsql_func_t*,int,dbsql_value_t **),
  void (*step)(dbsql_func_t*,int,dbsql_value_t **),
  void (*final)(dbsql_func_t*),
  void (*destroy)(void *)
){
  return __create_function_api(db, func, n_arg, enc, p, s_func, step,
                                    final, 0, 0, destroy);
}
int dbsql_create_window_function(
  DBSQL *db,
  const char *func,
  int n_arg,
  int enc,
  void *p,
  void (*step)(dbsql_func_t*,int,dbsql_value_t **),
  void (*final)(dbsql_func_t*),
  void (*value)(dbsql_func_t*),
  void (*inverse)(dbsql_func_t*,int,dbsql_value_t **),
  void (*destroy)(void *)
){
  return __create_function_api(db, func, n_arg, enc, p, 0, step,
                                    final, value, inverse, destroy);
}

#ifndef DBSQL_OMIT_UTF16
int dbsql_create_function16(
  DBSQL *db,
  const void *function_name,
  int n_arg,
  int text_rep,
  void *p,
  void (*s_func)(dbsql_func_t*,int,dbsql_value_t**),
  void (*step)(dbsql_func_t*,int,dbsql_value_t**),
  void (*final)(dbsql_func_t*)
){
  int rc;
  char *func8;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || function_name==0 ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  assert( !db->malloc_failed );
  func8 = __utf16to8(db, function_name, -1, DBSQL_UTF16NATIVE);
  rc = __create_func(db, func8, n_arg, text_rep, p, s_func,step,final,0,0,0);
  __dbsql_db_free(db, func8);
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}
#endif


/*
** The following is the implementation of an SQL function that always
** fails with an error message stating that the function is used in the
** wrong context.  The dbsql_overload_function() API might construct
** SQL function that use this routine so that the functions will exist
** for name resolution but are actually overloaded by the find_function
** method of virtual tables.
*/
static void __invalid_function(
  dbsql_func_t *context,  /* The function calling context */
  int __not_used,               /* Number of arguments to the function */
  dbsql_value_t **__not_used2   /* Value of each argument */
){
  const char *name = (const char*)dbsql_user_data(context);
  char *err;
  UNUSED_PARAMETER2(__not_used, __not_used2);
  err = dbsql_mprintf(
      "unable to use function %s in the requested context", name);
  dbsql_result_error(context, err, -1);
  dbsql_free(err);
}

/*
** Declare that a function has been overloaded by a virtual table.
**
** If the function already exists as a regular global function, then
** this routine is a no-op.  If the function does not exist, then create
** a new one that always throws a run-time error. 
**
** When virtual tables intend to provide an overloaded function, they
** should call this routine to make sure the global function exists.
** A global function must exist in order for name resolution to work
** properly.
*/
int dbsql_overload_function(
  DBSQL *db,
  const char *name,
  int n_arg
){
  int rc;
  char *copy;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || name==0 || n_arg<-2 ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  rc = __find_function(db, name, n_arg, DBSQL_UTF8, 0)!=0;
  dbsql_mutex_leave(db->mutex);
  if( rc ) return DBSQL_OK;
  copy = dbsql_mprintf("%s", name);
  if( copy==0 ) return DBSQL_NOMEM;
  return dbsql_create_function_v2(db, name, n_arg, DBSQL_UTF8,
                           copy, __invalid_function, 0, 0, dbsql_free);
}

#ifndef DBSQL_OMIT_TRACE
/*
** Register a trace function.  The arg from the previously registered trace
** is returned. 
**
** A NULL trace function means that no tracing is executes.  A non-NULL
** trace is a pointer to a function that is invoked at the start of each
** SQL statement.
*/
#ifndef DBSQL_OMIT_DEPRECATED
void *dbsql_trace(DBSQL *db, void(*trace)(void*,const char*), void *arg){
  void *old;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  old = db->trace_arg;
  db->m_trace = trace ? DBSQL_TRACE_LEGACY : 0;
  db->trace.legacy = trace;
  db->trace_arg = arg;
  dbsql_mutex_leave(db->mutex);
  return old;
}
#endif /* DBSQL_OMIT_DEPRECATED */

/* Register a trace callback using the version-2 interface.
*/
int dbsql_trace_v2(
  DBSQL *db,                               /* Trace this connection */
  unsigned m_trace,                           /* Mask of events to be traced */
  int(*trace)(unsigned,void*,void*,void*),  /* Callback to invoke */
  void *arg                                 /* Context */
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  if( m_trace==0 ) trace = 0;
  if( trace==0 ) m_trace = 0;
  db->m_trace = m_trace;
  db->trace.v2 = trace;
  db->trace_arg = arg;
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}

#ifndef DBSQL_OMIT_DEPRECATED
/*
** Register a profile function.  The arg from the previously registered
** profile function is returned. 
**
** A NULL profile function means that no profiling is executes.  A non-NULL
** profile is a pointer to a function that is invoked at the conclusion of
** each SQL statement that is run.
*/
void *dbsql_profile(
  DBSQL *db,
  void (*profile)(void*,const char*,u_int64_t),
  void *arg
){
  void *old;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  old = db->profile_arg;
  db->profile = profile;
  db->profile_arg = arg;
  db->m_trace &= DBSQL_TRACE_NONLEGACY_MASK;
  if( db->profile ) db->m_trace |= DBSQL_TRACE_XPROFILE;
  dbsql_mutex_leave(db->mutex);
  return old;
}
#endif /* DBSQL_OMIT_DEPRECATED */
#endif /* DBSQL_OMIT_TRACE */

/*
** Register a function to be invoked when a transaction commits.
** If the invoked function returns non-zero, then the commit becomes a
** rollback.
*/
void *dbsql_commit_hook(
  DBSQL *db,              /* Attach the hook to this database */
  int (*callback)(void*),  /* Function to invoke on each commit */
  void *arg                /* Argument to the function */
){
  void *old;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  old = db->commit_arg;
  db->commit_callback = callback;
  db->commit_arg = arg;
  dbsql_mutex_leave(db->mutex);
  return old;
}

/*
** Register a callback to be invoked each time a row is updated,
** inserted or deleted using this database connection.
*/
void *dbsql_update_hook(
  DBSQL *db,              /* Attach the hook to this database */
  void (*callback)(void*,int,char const *,char const *,int64_t),
  void *arg                /* Argument to the function */
){
  void *ret;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  ret = db->update_arg;
  db->update_callback = callback;
  db->update_arg = arg;
  dbsql_mutex_leave(db->mutex);
  return ret;
}

/*
** Register a callback to be invoked each time a transaction is rolled
** back by this database connection.
*/
void *dbsql_rollback_hook(
  DBSQL *db,              /* Attach the hook to this database */
  void (*callback)(void*), /* Callback function */
  void *arg                /* Argument to the function */
){
  void *ret;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  ret = db->rollback_arg;
  db->rollback_callback = callback;
  db->rollback_arg = arg;
  dbsql_mutex_leave(db->mutex);
  return ret;
}

#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
/*
** Register a callback to be invoked each time a row is updated,
** inserted or deleted using this database connection.
*/
void *dbsql_preupdate_hook(
  DBSQL *db,              /* Attach the hook to this database */
  void(*callback)(         /* Callback function */
    void*,DBSQL*,int,char const*,char const*,int64_t,int64_t),
  void *arg                /* First callback argument */
){
  void *ret;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( db==0 ){
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  ret = db->pre_update_arg;
  db->pre_update_callback = callback;
  db->pre_update_arg = arg;
  dbsql_mutex_leave(db->mutex);
  return ret;
}
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */

/*
** Register a function to be invoked prior to each autovacuum that
** determines the number of pages to vacuum.
*/
int dbsql_autovacuum_pages(
  DBSQL *db,                 /* Attach the hook to this database */
  unsigned int (*callback)(void*,const char*,u_int32_t,u_int32_t,u_int32_t),
  void *arg,                  /* Argument to the function */
  void (*destructor)(void*)   /* Destructor for arg */
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    if( destructor ) destructor(arg);
    return DBSQL_MISUSE_BKPT;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  if( db->autovac_destr ){
    db->autovac_destr(db->autovac_pages_arg);
  }
  db->autovac_pages = callback;
  db->autovac_pages_arg = arg;
  db->autovac_destr = destructor;
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}


#ifndef DBSQL_OMIT_WAL
/*
** The dbsql_wal_hook() callback registered by dbsql_wal_autocheckpoint().
** Invoke dbsql_wal_checkpoint if the number of frames in the log file
** is greater than DBSQL.wal_arg cast to an integer (the value configured by
** wal_autocheckpoint()).
*/
int __wal_default_hook(
  void *client_data,     /* Argument */
  DBSQL *db,           /* Connection */
  const char *z_db,       /* Database */
  int n_frame             /* Size of WAL */
){
  if( n_frame>=DBSQL_PTR_TO_INT(client_data) ){
    __begin_benign_malloc();
    dbsql_wal_checkpoint(db, z_db);
    __end_benign_malloc();
  }
  return DBSQL_OK;
}
#endif /* DBSQL_OMIT_WAL */

/*
** Configure an dbsql_wal_hook() callback to automatically checkpoint
** a database after committing a transaction if there are n_frame or
** more frames in the log file. Passing zero or a negative value as the
** n_frame parameter disables automatic checkpoints entirely.
**
** The callback registered by this function replaces any existing callback
** registered using dbsql_wal_hook(). Likewise, registering a callback
** using dbsql_wal_hook() disables the automatic checkpoint mechanism
** configured by this function.
*/
int dbsql_wal_autocheckpoint(DBSQL *db, int n_frame){
#ifdef DBSQL_OMIT_WAL
  UNUSED_PARAMETER(db);
  UNUSED_PARAMETER(n_frame);
#else
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  if( n_frame>0 ){
    dbsql_wal_hook(db, __wal_default_hook, DBSQL_INT_TO_PTR(n_frame));
  }else{
    dbsql_wal_hook(db, 0, 0);
  }
#endif
  return DBSQL_OK;
}

/*
** Register a callback to be invoked each time a transaction is written
** into the write-ahead-log by this database connection.
*/
void *dbsql_wal_hook(
  DBSQL *db,                    /* Attach the hook to this db handle */
  int(*callback)(void *, DBSQL*, const char*, int),
  void *arg                      /* First argument passed to callback() */
){
#ifndef DBSQL_OMIT_WAL
  void *ret;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  ret = db->wal_arg;
  db->wal_callback = callback;
  db->wal_arg = arg;
  dbsql_mutex_leave(db->mutex);
  return ret;
#else
  UNUSED_PARAMETER(db);
  UNUSED_PARAMETER(callback);
  UNUSED_PARAMETER(arg);
  return 0;
#endif
}

/*
** Checkpoint database db.
*/
int dbsql_wal_checkpoint_v2(
  DBSQL *db,                    /* Database handle */
  const char *z_db,                /* Name of attached database (or NULL) */
  int mode,                      /* DBSQL_CHECKPOINT_* value */
  int *pn_log,                     /* OUT: Size of WAL log in frames */
  int *pn_ckpt                     /* OUT: Total number of frames checkpointed */
){
#ifdef DBSQL_OMIT_WAL
  UNUSED_PARAMETER(db);
  UNUSED_PARAMETER(z_db);
  UNUSED_PARAMETER(mode);
  UNUSED_PARAMETER(pn_log);
  UNUSED_PARAMETER(pn_ckpt);
  return DBSQL_OK;
#else
  int rc;                         /* Return code */
  int i_db;                        /* schema_t to checkpoint */

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif

  /* Initialize the output variables to -1 in case an error occurs. */
  if( pn_log ) *pn_log = -1;
  if( pn_ckpt ) *pn_ckpt = -1;

  assert( DBSQL_CHECKPOINT_NOOP==-1 );
  assert( DBSQL_CHECKPOINT_PASSIVE==0 );
  assert( DBSQL_CHECKPOINT_FULL==1 );
  assert( DBSQL_CHECKPOINT_RESTART==2 );
  assert( DBSQL_CHECKPOINT_TRUNCATE==3 );
  if( mode<DBSQL_CHECKPOINT_NOOP || mode>DBSQL_CHECKPOINT_TRUNCATE ){
    /* EVIDENCE-OF: R-03996-12088 The M parameter must be a valid checkpoint
    ** mode: */
    return DBSQL_MISUSE_BKPT;
  }

  dbsql_mutex_enter(db->mutex);
  if( z_db && z_db[0] ){
    i_db = __find_db_name(db, z_db);
  }else{
    i_db = DBSQL_MAX_DB;   /* This means process all schemas */
  }
  if( i_db<0 ){
    rc = DBSQL_ERROR;
    __error_with_msg(db, DBSQL_ERROR, "unknown database: %s", z_db);
  }else{
    db->busy_handler.n_busy = 0;
    rc = __checkpoint(db, i_db, mode, pn_log, pn_ckpt);
    __dbsql_error(db, rc);
  }
  rc = __api_exit(db, rc);

  /* If there are no active statements, clear the interrupt flag at this
  ** point.  */
  if( db->n_vdbe_active==0 ){
    ATOMIC_STORE(&db->u1.is_interrupted, 0);
  }

  dbsql_mutex_leave(db->mutex);
  return rc;
#endif
}


/*
** Checkpoint database db. If db is NULL, or if the buffer db points
** to contains a zero-length string, all attached databases are
** checkpointed.
*/
int dbsql_wal_checkpoint(DBSQL *db, const char *z_db){
  /* EVIDENCE-OF: R-41613-20553 The dbsql_wal_checkpoint(D,X) is equivalent to
  ** dbsql_wal_checkpoint_v2(D,X,DBSQL_CHECKPOINT_PASSIVE,0,0). */
  return dbsql_wal_checkpoint_v2(db,z_db,DBSQL_CHECKPOINT_PASSIVE,0,0);
}

#ifndef DBSQL_OMIT_WAL
/*
** Run a checkpoint on database i_db. This is a no-op if database i_db is
** not currently open in WAL mode.
**
** If a transaction is open on the database being checkpointed, this
** function returns DBSQL_LOCKED and a checkpoint is not attempted. If
** an error occurs while running the checkpoint, an dbsql error code is
** returned (i.e. DBSQL_IOERR). Otherwise, DBSQL_OK.
**
** The mutex on database handle db should be held by the caller. The mutex
** associated with the specific b-tree being checkpointed is taken by
** this function while the checkpoint is running.
**
** If i_db is passed DBSQL_MAX_DB then all attached databases are
** checkpointed. If an error is encountered it is returned immediately -
** no attempt is made to checkpoint any remaining databases.
**
** Parameter mode is one of DBSQL_CHECKPOINT_PASSIVE, FULL, RESTART
** or TRUNCATE.
*/
int __checkpoint(DBSQL *db, int i_db, int mode, int *pn_log, int *pn_ckpt){
  int rc = DBSQL_OK;             /* Return code */
  int i;                          /* Used to iterate through attached dbs */
  int busy = 0;                  /* True if DBSQL_BUSY has been encountered */

  assert( dbsql_mutex_held(db->mutex) );
  assert( !pn_log || *pn_log==-1 );
  assert( !pn_ckpt || *pn_ckpt==-1 );
  testcase( i_db==DBSQL_MAX_ATTACHED ); /* See forum post a006d86f72 */
  testcase( i_db==DBSQL_MAX_DB );

  for(i=0; i<db->n_db && rc==DBSQL_OK; i++){
    if( i==i_db || i_db==DBSQL_MAX_DB ){
      rc = __sm_checkpoint(db->dbs[i].bt, mode, pn_log, pn_ckpt);
      pn_log = 0;
      pn_ckpt = 0;
      if( rc==DBSQL_BUSY ){
        busy = 1;
        rc = DBSQL_OK;
      }
    }
  }

  return (rc==DBSQL_OK && busy) ? DBSQL_BUSY : rc;
}
#endif /* DBSQL_OMIT_WAL */

/*
** This function returns true if main-memory should be used instead of
** a temporary file for transient pager files and statement journals.
** The value returned depends on the value of db->temp_store (runtime
** parameter) and the compile time value of DBSQL_TEMP_STORE. The
** following table describes the relationship between these two values
** and this functions return value.
**
**   DBSQL_TEMP_STORE     db->temp_store     Location of temporary database
**   -----------------     --------------     ------------------------------
**   0                     any                file      (return 0)
**   1                     1                  file      (return 0)
**   1                     2                  memory    (return 1)
**   1                     0                  file      (return 0)
**   2                     1                  file      (return 0)
**   2                     2                  memory    (return 1)
**   2                     0                  memory    (return 1)
**   3                     any                memory    (return 1)
*/
int __temp_in_memory(const DBSQL *db){
#if DBSQL_TEMP_STORE==1
  return ( db->temp_store==2 );
#endif
#if DBSQL_TEMP_STORE==2
  return ( db->temp_store!=1 );
#endif
#if DBSQL_TEMP_STORE==3
  UNUSED_PARAMETER(db);
  return 1;
#endif
#if DBSQL_TEMP_STORE<1 || DBSQL_TEMP_STORE>3
  UNUSED_PARAMETER(db);
  return 0;
#endif
}

/*
** Return UTF-8 encoded English language explanation of the most recent
** error.
*/
const char *dbsql_errmsg(DBSQL *db){
  const char *z;
  if( !db ){
    return __err_str(DBSQL_NOMEM_BKPT);
  }
  if( !__safety_check_sick_or_ok(db) ){
    return __err_str(DBSQL_MISUSE_BKPT);
  }
  dbsql_mutex_enter(db->mutex);
  if( db->malloc_failed ){
    z = __err_str(DBSQL_NOMEM_BKPT);
  }else{
    testcase( db->err==0 );
    z = db->errCode ? (char*)dbsql_value_text(db->err) : 0;
    assert( !db->malloc_failed );
    if( z==0 ){
      z = __err_str(db->errCode);
    }
  }
  dbsql_mutex_leave(db->mutex);
  return z;
}

/*
** Set the error code and error message associated with the database handle.
**
** This routine is intended to be called by outside extensions (ex: the
** Session extension). Internal logic should invoke __dbsql_error() or
** __error_with_msg() directly.
*/
int dbsql_set_errmsg(DBSQL *db, int errcode, const char *msg){
  int rc = DBSQL_OK;
  if( !__safety_check_ok(db) ){
    return DBSQL_MISUSE_BKPT;
  }
  dbsql_mutex_enter(db->mutex);
  if( msg ){
    __error_with_msg(db, errcode, "%s", msg);
  }else{
    __dbsql_error(db, errcode);
  }
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** Return the byte offset of the most recent error
*/
int dbsql_error_offset(DBSQL *db){
  int i_offset = -1;
  if( db && __safety_check_sick_or_ok(db) ){
    dbsql_mutex_enter(db->mutex);
    if( db->errCode ){
      i_offset = db->err_byte_offset;
    }
    dbsql_mutex_leave(db->mutex);
  }
  return i_offset;
}

#ifndef DBSQL_OMIT_UTF16
/*
** Return UTF-16 encoded English language explanation of the most recent
** error.
*/
const void *dbsql_errmsg16(DBSQL *db){
  static const u_int16_t out_of_mem[] = {
    'o', 'u', 't', ' ', 'o', 'f', ' ', 'm', 'e', 'm', 'o', 'r', 'y', 0
  };
  static const u_int16_t misuse[] = {
    'b', 'a', 'd', ' ', 'p', 'a', 'r', 'a', 'm', 'e', 't', 'e', 'r', ' ',
    'o', 'r', ' ', 'o', 't', 'h', 'e', 'r', ' ', 'A', 'P', 'I', ' ',
    'm', 'i', 's', 'u', 's', 'e', 0
  };

  const void *z;
  if( !db ){
    return (void *)out_of_mem;
  }
  if( !__safety_check_sick_or_ok(db) ){
    return (void *)misuse;
  }
  dbsql_mutex_enter(db->mutex);
  if( db->malloc_failed ){
    z = (void *)out_of_mem;
  }else{
    z = dbsql_value_text16(db->err);
    if( z==0 ){
      __error_with_msg(db, db->errCode, __err_str(db->errCode));
      z = dbsql_value_text16(db->err);
    }
    /* A malloc() may have failed within the call to dbsql_value_text16()
    ** above. If this is the case, then the db->malloc_failed flag needs to
    ** be cleared before returning. Do this directly, instead of via
    ** __api_exit(), to avoid setting the database handle error message.
    */
    __oom_clear(db);
  }
  dbsql_mutex_leave(db->mutex);
  return z;
}
#endif /* DBSQL_OMIT_UTF16 */

/*
** Return the most recent error code generated by an dbsql routine. If NULL is
** passed to this function, we assume a malloc() failed during dbsql_open().
*/
int dbsql_errcode(DBSQL *db){
  int i_ret;
  if( !db ) return DBSQL_NOMEM_BKPT;
  if( !__safety_check_sick_or_ok(db) ){
    return DBSQL_MISUSE_BKPT;
  }
  dbsql_mutex_enter(db->mutex);
  if( db->malloc_failed ){
    i_ret = DBSQL_NOMEM_BKPT;
  }else{
    i_ret = db->errCode & db->err_mask;
  }
  dbsql_mutex_leave(db->mutex);
  return i_ret;
}
int dbsql_extended_errcode(DBSQL *db){
  int i_ret;
  if( !db ) return DBSQL_NOMEM_BKPT;
  if( !__safety_check_sick_or_ok(db) ){
    return DBSQL_MISUSE_BKPT;
  }
  dbsql_mutex_enter(db->mutex);
  if( db->malloc_failed ){
    i_ret = DBSQL_NOMEM_BKPT;
  }else{
    i_ret = db->errCode;
  }
  dbsql_mutex_leave(db->mutex);
  return i_ret;
}
int dbsql_system_errno(DBSQL *db){
  int i_ret = 0;
  if( db ){
    dbsql_mutex_enter(db->mutex);
    i_ret = db->i_sys_errno;
    dbsql_mutex_leave(db->mutex);
  }
  return i_ret;
} 

/*
** Return a string that describes the kind of error specified in the
** argument.  For now, this simply calls the internal __err_str()
** function.
*/
const char *dbsql_errstr(int rc){
  return __err_str(rc);
}

/*
** Create a new collating function for database "db".  The name is name
** and the encoding is enc.
*/
static int __create_collation(
  DBSQL* db,
  const char *name,
  u_int8_t enc,
  void* ctx,
  int(*compare)(void*,int,const void*,int,const void*),
  void(*del)(void*)
){
  coll_seq_t *coll;
  int enc2;
 
  assert( dbsql_mutex_held(db->mutex) );

  /* If DBSQL_UTF16 is specified as the encoding type, transform this
  ** to one of DBSQL_UTF16LE or DBSQL_UTF16BE using the
  ** DBSQL_UTF16NATIVE macro. DBSQL_UTF16 is not used internally.
  */
  enc2 = enc;
  testcase( enc2==DBSQL_UTF16 );
  testcase( enc2==DBSQL_UTF16_ALIGNED );
  if( enc2==DBSQL_UTF16 || enc2==DBSQL_UTF16_ALIGNED ){
    enc2 = DBSQL_UTF16NATIVE;
  }
  if( enc2<DBSQL_UTF8 || enc2>DBSQL_UTF16BE ){
    return DBSQL_MISUSE_BKPT;
  }

  /* Check if this call is removing or replacing an existing collation
  ** sequence. If so, and there are active VMs, return busy. If there
  ** are no active VMs, invalidate any pre-compiled statements.
  */
  coll = __find_coll_seq(db, (u_int8_t)enc2, name, 0);
  if( coll && coll->cmp ){
    if( db->n_vdbe_active ){
      __error_with_msg(db, DBSQL_BUSY,
        "unable to delete/modify collation sequence due to active statements");
      return DBSQL_BUSY;
    }
    __expire_prepared_statements(db, 0);

    /* If collation sequence coll was created directly by a call to
    ** dbsql_create_collation, and not generated by __synth_coll_seq(),
    ** then any copies made by __synth_coll_seq() need to be invalidated.
    ** Also, collation destructor - coll_seq_t.del() - function may need
    ** to be called.
    */
    if( (coll->enc & ~DBSQL_UTF16_ALIGNED)==enc2 ){
      coll_seq_t *colls = __hash_find(&db->coll_seqs, name);
      int j;
      for(j=0; j<3; j++){
        coll_seq_t *p = &colls[j];
        if( p->enc==coll->enc ){
          if( p->x_del ){
            p->x_del(p->user);
          }
          p->cmp = 0;
        }
      }
    }
  }

  coll = __find_coll_seq(db, (u_int8_t)enc2, name, 1);
  if( coll==0 ) return DBSQL_NOMEM_BKPT;
  coll->cmp = compare;
  coll->user = ctx;
  coll->x_del = del;
  coll->enc = (u_int8_t)(enc2 | (enc & DBSQL_UTF16_ALIGNED));
  __dbsql_error(db, DBSQL_OK);
  return DBSQL_OK;
}


/*
** This array defines hard upper bounds on limit values.  The
** initializer must be kept in sync with the DBSQL_LIMIT_*
** #defines in DBSQL.h.
*/
static const int __a_hard_limit[] = {
  DBSQL_MAX_LENGTH,
  DBSQL_MAX_SQL_LENGTH,
  DBSQL_MAX_COLUMN,
  DBSQL_MAX_EXPR_DEPTH,
  DBSQL_MAX_COMPOUND_SELECT,
  DBSQL_MAX_VDBE_OP,
  DBSQL_MAX_FUNCTION_ARG,
  DBSQL_MAX_ATTACHED,
  DBSQL_MAX_LIKE_PATTERN_LENGTH,
  DBSQL_MAX_VARIABLE_NUMBER,      /* IMP: R-38091-32352 */
  DBSQL_MAX_TRIGGER_DEPTH,
  DBSQL_MAX_WORKER_THREADS,
  DBSQL_MAX_PARSER_DEPTH,
};

/*
** Make sure the hard limits are set to reasonable values
*/
#if DBSQL_MAX_LENGTH<100
# error DBSQL_MAX_LENGTH must be at least 100
#endif
#if DBSQL_MAX_SQL_LENGTH<100
# error DBSQL_MAX_SQL_LENGTH must be at least 100
#endif
#if DBSQL_MAX_SQL_LENGTH>DBSQL_MAX_LENGTH
# error DBSQL_MAX_SQL_LENGTH must not be greater than DBSQL_MAX_LENGTH
#endif
#if DBSQL_MAX_SQL_LENGTH>2147482624     /* 1024 less than 2^31 */
# error DBSQL_MAX_SQL_LENGTH must not be greater than 2147482624
#endif
#if DBSQL_MAX_COMPOUND_SELECT<2
# error DBSQL_MAX_COMPOUND_SELECT must be at least 2
#endif
#if DBSQL_MAX_VDBE_OP<40
# error DBSQL_MAX_VDBE_OP must be at least 40
#endif
#if DBSQL_MAX_FUNCTION_ARG<0 || DBSQL_MAX_FUNCTION_ARG>32767
# error DBSQL_MAX_FUNCTION_ARG must be between 0 and 32767
#endif
#if DBSQL_MAX_ATTACHED<0 || DBSQL_MAX_ATTACHED>125
# error DBSQL_MAX_ATTACHED must be between 0 and 125
#endif
#if DBSQL_MAX_LIKE_PATTERN_LENGTH<1
# error DBSQL_MAX_LIKE_PATTERN_LENGTH must be at least 1
#endif
#if DBSQL_MAX_COLUMN>32767
# error DBSQL_MAX_COLUMN must not exceed 32767
#endif
#if DBSQL_MAX_TRIGGER_DEPTH<1
# error DBSQL_MAX_TRIGGER_DEPTH must be at least 1
#endif
#if DBSQL_MAX_WORKER_THREADS<0 || DBSQL_MAX_WORKER_THREADS>50
# error DBSQL_MAX_WORKER_THREADS must be between 0 and 50
#endif


/*
** Change the value of a limit.  Report the old value.
** If an invalid limit index is supplied, report -1.
** Make no changes but still report the old value if the
** new limit is negative.
**
** A new lower limit does not shrink existing constructs.
** It merely prevents new constructs that exceed the limit
** from forming.
*/
int dbsql_limit(DBSQL *db, int limit_id, int new_limit){
  int old_limit;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return -1;
  }
#endif

  /* EVIDENCE-OF: R-30189-54097 For each limit category DBSQL_LIMIT_NAME
  ** there is a hard upper bound set at compile-time by a C preprocessor
  ** macro called DBSQL_MAX_NAME. (The "_LIMIT_" in the name is changed to
  ** "_MAX_".)
  */
  assert( __a_hard_limit[DBSQL_LIMIT_LENGTH]==DBSQL_MAX_LENGTH );
  assert( __a_hard_limit[DBSQL_LIMIT_SQL_LENGTH]==DBSQL_MAX_SQL_LENGTH );
  assert( __a_hard_limit[DBSQL_LIMIT_COLUMN]==DBSQL_MAX_COLUMN );
  assert( __a_hard_limit[DBSQL_LIMIT_EXPR_DEPTH]==DBSQL_MAX_EXPR_DEPTH );
  assert( __a_hard_limit[DBSQL_LIMIT_PARSER_DEPTH]==DBSQL_MAX_PARSER_DEPTH );
  assert( __a_hard_limit[DBSQL_LIMIT_COMPOUND_SELECT]==DBSQL_MAX_COMPOUND_SELECT);
  assert( __a_hard_limit[DBSQL_LIMIT_VDBE_OP]==DBSQL_MAX_VDBE_OP );
  assert( __a_hard_limit[DBSQL_LIMIT_FUNCTION_ARG]==DBSQL_MAX_FUNCTION_ARG );
  assert( __a_hard_limit[DBSQL_LIMIT_ATTACHED]==DBSQL_MAX_ATTACHED );
  assert( __a_hard_limit[DBSQL_LIMIT_LIKE_PATTERN_LENGTH]==
                                               DBSQL_MAX_LIKE_PATTERN_LENGTH );
  assert( __a_hard_limit[DBSQL_LIMIT_VARIABLE_NUMBER]==DBSQL_MAX_VARIABLE_NUMBER);
  assert( __a_hard_limit[DBSQL_LIMIT_TRIGGER_DEPTH]==DBSQL_MAX_TRIGGER_DEPTH );
  assert( __a_hard_limit[DBSQL_LIMIT_WORKER_THREADS]==DBSQL_MAX_WORKER_THREADS );
  assert( DBSQL_LIMIT_PARSER_DEPTH==(DBSQL_N_LIMIT-1) );


  if( limit_id<0 || limit_id>=DBSQL_N_LIMIT ){
    return -1;
  }
  dbsql_mutex_enter(db->mutex);
  old_limit = db->limits[limit_id];
  if( new_limit>=0 ){                   /* IMP: R-52476-28732 */
    if( new_limit>__a_hard_limit[limit_id] ){
      new_limit = __a_hard_limit[limit_id];  /* IMP: R-51463-25634 */
    }else if( new_limit<DBSQL_MIN_LENGTH && limit_id==DBSQL_LIMIT_LENGTH ){
      new_limit = DBSQL_MIN_LENGTH;
    }
    db->limits[limit_id] = new_limit;
  }
  dbsql_mutex_leave(db->mutex);
  return old_limit;                     /* IMP: R-53341-35419 */
}

/*
** This function is used to parse both URIs and non-URI filenames passed by the
** user to API functions dbsql_open() or dbsql_open_v2(), and for database
** URIs specified as part of ATTACH statements.
**
** The first argument to this function is the name of the VFS to use (or
** a NULL to signify the default VFS) if the URI does not contain a "vfs=xxx"
** query parameter. The second argument contains the URI (or non-URI filename)
** itself. When this function is called the *flags variable should contain
** the default flags to open the database handle with. The value stored in
** *flags may be updated before returning if the URI filename contains
** "cache=xxx" or "mode=xxx" query parameters.
**
** If successful, DBSQL_OK is returned. In this case *vfs is set to point to
** the VFS that should be used to open the database file. *file is set to
** point to a buffer containing the name of the file to open.  The value
** stored in *file is a database name acceptable to dbsql_uri_parameter()
** and is in the same format as names created using dbsql_create_filename().
** The caller must invoke dbsql_free_filename() (not dbsql_free()!) on
** the value returned in *file to avoid a memory leak.
**
** If an error occurs, then an dbsql error code is returned and *errmsg_p
** may be set to point to a buffer containing an English language error
** message. It is the responsibility of the caller to eventually release
** this buffer by calling dbsql_free().
*/
int __parse_uri(
  const char *default_vfs,        /* VFS to use if no "vfs=xxx" query option */
  const char *uri,               /* Nul-terminated URI to parse */
  unsigned int *p_flags,           /* IN/OUT: DBSQL_OPEN_XXX flags */
  dbsql_vfs_t **pp_vfs,            /* OUT: VFS to use */
  char **pz_file,                  /* OUT: Filename component of URI */
  char **errmsg_p                 /* OUT: Error message (if rc!=DBSQL_OK) */
){
  int rc = DBSQL_OK;
  unsigned int flags = *p_flags;
  const char *vfs = default_vfs;
  char *file;
  char c;
  int64_t n_uri = strlen(uri);

  assert( *errmsg_p==0 );

  if( ((flags & DBSQL_OPEN_URI)                     /* IMP: R-48725-32206 */
       || ATOMIC_LOAD(&__global_config.open_uri)) /* IMP: R-51689-46548 */
   && n_uri>=5 && memcmp(uri, "file:", 5)==0         /* IMP: R-57884-37496 */
  ){
    char *opt;
    int state;                   /* Parser state when parsing URI */
    int64_t i_in;                      /* Input character index */
    int64_t i_out = 0;                 /* Output character index */
    u_int64_t n_byte = n_uri+8;           /* Bytes of space to allocate */

    /* Make sure the DBSQL_OPEN_URI flag is set to indicate to the VFS x_open
    ** method that there may be extra parameters following the file-name.  */
    flags |= DBSQL_OPEN_URI;

    for(i_in=0; i_in<n_uri; i_in++) n_byte += (uri[i_in]=='&');
    file = dbsql_malloc64(n_byte);
    if( !file ) return DBSQL_NOMEM_BKPT;

    memset(file, 0, 4);  /* 4-byte of 0x00 is the start of DB name marker */
    file += 4;

    i_in = 5;
#ifdef DBSQL_ALLOW_URI_AUTHORITY
    if( strncmp(uri+5, "///", 3)==0 ){
      i_in = 7;
      /* The following condition causes URIs with five leading / characters
      ** like file://///host/path to be converted into UNCs like //host/path.
      ** The correct URI for that UNC has only two or four leading / characters
      ** file://host/path or file:////host/path.  But 5 leading slashes is a
      ** common error, we are told, so we handle it as a special case. */
      if( strncmp(uri+7, "///", 3)==0 ){ i_in++; }
    }else if( strncmp(uri+5, "//localhost/", 12)==0 ){
      i_in = 16;
    }
#else
    /* Discard the scheme and authority segments of the URI. */
    if( uri[5]=='/' && uri[6]=='/' ){
      i_in = 7;
      while( uri[i_in] && uri[i_in]!='/' ) i_in++;
      if( i_in!=7 && (i_in!=16 || memcmp("localhost", &uri[7], 9)) ){
        *errmsg_p = dbsql_mprintf("invalid uri authority: %.*s",
            (int)(i_in-7), &uri[7]);
        rc = DBSQL_ERROR;
        goto parse_uri_out;
      }
    }
#endif

    /* Copy the filename and any query parameters into the file buffer.
    ** Decode %HH escape codes along the way.
    **
    ** Within this loop, variable state may be set to 0, 1 or 2, depending
    ** on the parsing context. As follows:
    **
    **   0: Parsing file-name.
    **   1: Parsing name section of a name=value query parameter.
    **   2: Parsing value section of a name=value query parameter.
    */
    state = 0;
    while( (c = uri[i_in])!=0 && c!='#' ){
      i_in++;
      if( c=='%'
       && __isxdigit(uri[i_in])
       && __isxdigit(uri[i_in+1])
      ){
        int octet = (__hex_to_int(uri[i_in++]) << 4);
        octet += __hex_to_int(uri[i_in++]);

        assert( octet>=0 && octet<256 );
        if( octet==0 ){
#ifndef DBSQL_ENABLE_URI_00_ERROR
          /* This branch is taken when "%00" appears within the URI. In this
          ** case we ignore all text in the remainder of the path, name or
          ** value currently being parsed. So ignore the current character
          ** and skip to the next "?", "=" or "&", as appropriate. */
          while( (c = uri[i_in])!=0 && c!='#'
              && (state!=0 || c!='?')
              && (state!=1 || (c!='=' && c!='&'))
              && (state!=2 || c!='&')
          ){
            i_in++;
          }
          continue;
#else
          /* If ENABLE_URI_00_ERROR is defined, "%00" in a URI is an error. */
          *errmsg_p = dbsql_mprintf("unexpected %%00 in uri");
          rc = DBSQL_ERROR;
          goto parse_uri_out;
#endif
        }
        c = octet;
      }else if( state==1 && (c=='&' || c=='=') ){
        if( file[i_out-1]==0 ){
          /* An empty option name. Ignore this option altogether. */
          while( uri[i_in] && uri[i_in]!='#' && uri[i_in-1]!='&' ) i_in++;
          continue;
        }
        if( c=='&' ){
          file[i_out++] = '\0';
        }else{
          state = 2;
        }
        c = 0;
      }else if( (state==0 && c=='?') || (state==2 && c=='&') ){
        c = 0;
        state = 1;
      }
      file[i_out++] = c;
    }
    if( state==1 ) file[i_out++] = '\0';
    memset(file+i_out, 0, 4); /* end-of-options + empty journal filenames */

    /* Check if there were any options specified that should be interpreted
    ** here. Options that are interpreted here include "vfs" and those that
    ** correspond to flags that may be passed to the dbsql_open_v2()
    ** method. */
    opt = &file[strlen(file)+1];
    while( opt[0] ){
      int64_t n_opt = strlen(opt);
      char *val = &opt[n_opt+1];
      int64_t n_val = strlen(val);

      if( n_opt==3 && memcmp("vfs", opt, 3)==0 ){
        vfs = val;
      }else{
        struct open_mode_t {
          const char *z;
          int mode;
        } *modes = 0;
        char *mode_type = 0;
        int mask = 0;
        int limit = 0;

        if( n_opt==5 && memcmp("cache", opt, 5)==0 ){
          static struct open_mode_t cache_modes[] = {
            { "shared",  DBSQL_OPEN_SHAREDCACHE },
            { "private", DBSQL_OPEN_PRIVATECACHE },
            { 0, 0 }
          };

          mask = DBSQL_OPEN_SHAREDCACHE|DBSQL_OPEN_PRIVATECACHE;
          modes = cache_modes;
          limit = mask;
          mode_type = "cache";
        }
        if( n_opt==4 && memcmp("mode", opt, 4)==0 ){
          static struct open_mode_t open_modes[] = {
            { "ro",  DBSQL_OPEN_READONLY },
            { "rw",  DBSQL_OPEN_READWRITE },
            { "rwc", DBSQL_OPEN_READWRITE | DBSQL_OPEN_CREATE },
            { "memory", DBSQL_OPEN_MEMORY },
            { 0, 0 }
          };

          mask = DBSQL_OPEN_READONLY | DBSQL_OPEN_READWRITE
                   | DBSQL_OPEN_CREATE | DBSQL_OPEN_MEMORY;
          modes = open_modes;
          limit = mask & flags;
          mode_type = "access";
        }

        if( modes ){
          int i;
          int mode = 0;
          for(i=0; modes[i].z; i++){
            const char *z = modes[i].z;
            if( n_val==(int64_t)strlen(z) && 0==memcmp(val, z, n_val) ){
              mode = modes[i].mode;
              break;
            }
          }
          if( mode==0 ){
            *errmsg_p = dbsql_mprintf("no such %s mode: %s", mode_type, val);
            rc = DBSQL_ERROR;
            goto parse_uri_out;
          }
          if( (mode & ~DBSQL_OPEN_MEMORY)>limit ){
            *errmsg_p = dbsql_mprintf("%s mode not allowed: %s",
                                        mode_type, val);
            rc = DBSQL_PERM;
            goto parse_uri_out;
          }
          flags = (flags & ~mask) | mode;
        }
      }

      opt = &val[n_val+1];
    }

  }else{
    file = dbsql_malloc64(n_uri+8);
    if( !file ) return DBSQL_NOMEM_BKPT;
    memset(file, 0, 4);
    file += 4;
    if( n_uri ){
      memcpy(file, uri, n_uri);
    }
    memset(file+n_uri, 0, 4);
    flags &= ~DBSQL_OPEN_URI;
  }

  *pp_vfs = dbsql_vfs_find(vfs);
  if( *pp_vfs==0 ){
    *errmsg_p = dbsql_mprintf("no such vfs: %s", vfs);
    rc = DBSQL_ERROR;
  }
 parse_uri_out:
  if( rc!=DBSQL_OK ){
    dbsql_free_filename(file);
    file = 0;
  }
  *p_flags = flags;
  *pz_file = file;
  return rc;
}

/*
** This routine does the core work of extracting URI parameters from a
** database filename for the dbsql_uri_parameter() interface.
*/
static const char *__uri_parameter(const char *filename, const char *param){
  filename += __strlen30(filename) + 1;
  while( ALWAYS(filename!=0) && filename[0] ){
    int x = strcmp(filename, param);
    filename += __strlen30(filename) + 1;
    if( x==0 ) return filename;
    filename += __strlen30(filename) + 1;
  }
  return 0;
}



/*
** This routine does the work of opening a database on behalf of
** dbsql_open() and dbsql_open16(). The database filename "filename" 
** is UTF-8 encoded.
*/
static int __open_database(
  const char *filename, /* Database filename UTF-8 encoded */
  DBSQL **pp_db,        /* OUT: Returned database handle */
  unsigned int flags,    /* Operational flags */
  const char *vfs       /* Name of the VFS to use */
){
  DBSQL *db;                    /* Store allocated handle here */
  int rc;                         /* Return code */
  int is_threadsafe;               /* True for threadsafe connections */
  char *z_open = 0;                /* Filename argument to pass to __btree_open() */
  char *errmsg = 0;              /* Error message from __parse_uri() */
  int i;                          /* Loop counter */

#ifdef DBSQL_ENABLE_API_ARMOR
  if( pp_db==0 ) return DBSQL_MISUSE_BKPT;
#endif
  *pp_db = 0;
#ifndef DBSQL_OMIT_AUTOINIT
  rc = dbsql_initialize();
  if( rc ) return rc;
#endif

  if( __global_config.core_mutex==0 ){
    is_threadsafe = 0;
  }else if( flags & DBSQL_OPEN_NOMUTEX ){
    is_threadsafe = 0;
  }else if( flags & DBSQL_OPEN_FULLMUTEX ){
    is_threadsafe = 1;
  }else{
    is_threadsafe = __global_config.full_mutex;
  }

  if( flags & DBSQL_OPEN_PRIVATECACHE ){
    flags &= ~DBSQL_OPEN_SHAREDCACHE;
  }else if( __global_config.shared_cache_enabled ){
    flags |= DBSQL_OPEN_SHAREDCACHE;
  }

  /* Remove harmful bits from the flags parameter
  **
  ** The DBSQL_OPEN_NOMUTEX and DBSQL_OPEN_FULLMUTEX flags were
  ** dealt with in the previous code block.  Besides these, the only
  ** valid input flags for dbsql_open_v2() are DBSQL_OPEN_READONLY,
  ** DBSQL_OPEN_READWRITE, DBSQL_OPEN_CREATE, DBSQL_OPEN_SHAREDCACHE,
  ** DBSQL_OPEN_PRIVATECACHE, DBSQL_OPEN_EXRESCODE, and some reserved
  ** bits.  Silently mask off all other flags.
  */
  flags &=  ~( DBSQL_OPEN_DELETEONCLOSE |
               DBSQL_OPEN_EXCLUSIVE |
               DBSQL_OPEN_MAIN_DB |
               DBSQL_OPEN_TEMP_DB |
               DBSQL_OPEN_TRANSIENT_DB |
               DBSQL_OPEN_MAIN_JOURNAL |
               DBSQL_OPEN_TEMP_JOURNAL |
               DBSQL_OPEN_SUBJOURNAL |
               DBSQL_OPEN_SUPER_JOURNAL |
               DBSQL_OPEN_NOMUTEX |
               DBSQL_OPEN_FULLMUTEX |
               DBSQL_OPEN_WAL
             );

  /* Allocate the sqlite data structure */
  db = __malloc_zero( sizeof(DBSQL) );
  if( db==0 ) goto opendb_out;
  if( is_threadsafe
#if defined(DBSQL_THREAD_MISUSE_WARNINGS)
   || __global_config.core_mutex
#endif
  ){
    db->mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_RECURSIVE);
    if( db->mutex==0 ){
      dbsql_free(db);
      db = 0;
      goto opendb_out;
    }
    if( is_threadsafe==0 ){
      __mutex_warn_on_contention(db->mutex);
    }
  }
  dbsql_mutex_enter(db->mutex);
  db->err_mask = (flags & DBSQL_OPEN_EXRESCODE)!=0 ? 0xffffffff : 0xff;
  db->n_db = 2;
  db->open_state = DBSQL_STATE_BUSY;
  db->dbs = db->db_statics;
  db->lookaside.disable = 1;
  db->lookaside.sz = 0;
  db->n_fp_digit = 17;

  assert( sizeof(db->limits)==sizeof(__a_hard_limit) );
  memcpy(db->limits, __a_hard_limit, sizeof(db->limits));
  db->limits[DBSQL_LIMIT_WORKER_THREADS] = DBSQL_DEFAULT_WORKER_THREADS;
  db->auto_commit = 1;
  db->next_autovac = -1;
  db->sz_mmap = __global_config.sz_mmap;
  db->next_pagesize = 0;
  db->init.inits = __std_type; /* Any array of string ptrs will do */
#ifdef DBSQL_ENABLE_SORTER_MMAP
  /* Beginning with version 3.37.0, using the VFS fetch() API to memory-map
  ** the temporary files used to do external sorts (see code in vdbesort.c)
  ** is disabled. It can still be used either by defining
  ** DBSQL_ENABLE_SORTER_MMAP at compile time or by using the
  ** DBSQL_TESTCTRL_SORTER_MMAP test-control at runtime. */
  db->n_max_sorter_mmap = 0x7FFFFFFF;
#endif
  db->flags |= DBSQL_ShortColNames
                 | DBSQL_EnableTrigger
                 | DBSQL_EnableView
                 | DBSQL_CacheSpill
                 | DBSQL_AttachCreate
                 | DBSQL_AttachWrite
                 | DBSQL_Comments
#if !defined(DBSQL_TRUSTED_SCHEMA) || DBSQL_TRUSTED_SCHEMA+0!=0
                 | DBSQL_TrustedSchema
#endif
/* The DBSQL_DQS compile-time option determines the default settings
** for DBSQL_DBCONFIG_DQS_DDL and DBSQL_DBCONFIG_DQS_DML.
**
**    DBSQL_DQS     DBSQL_DBCONFIG_DQS_DDL    DBSQL_DBCONFIG_DQS_DML
**    ----------     -----------------------    -----------------------
**     undefined               on                          on
**         3                   on                          on
**         2                   on                         off
**         1                  off                          on
**         0                  off                         off
**
** Legacy behavior is 3 (double-quoted string literals are allowed anywhere)
** and so that is the default.  But developers are encouraged to use
** -DSQLITE_DQS=0 (best) or -DSQLITE_DQS=1 (second choice) if possible.
*/
#if !defined(DBSQL_DQS)
# define DBSQL_DQS 3
#endif
#if (DBSQL_DQS&1)==1
                 | DBSQL_DqsDML
#endif
#if (DBSQL_DQS&2)==2
                 | DBSQL_DqsDDL
#endif

#if !defined(DBSQL_DEFAULT_AUTOMATIC_INDEX) || DBSQL_DEFAULT_AUTOMATIC_INDEX
                 | DBSQL_AutoIndex
#endif
#if DBSQL_DEFAULT_CKPTFULLFSYNC
                 | DBSQL_CkptFullFSync
#endif
#if DBSQL_DEFAULT_FILE_FORMAT<4
                 | DBSQL_LegacyFileFmt
#endif
#ifdef DBSQL_ENABLE_LOAD_EXTENSION
                 | DBSQL_LoadExtension
#endif
#if DBSQL_DEFAULT_RECURSIVE_TRIGGERS
                 | DBSQL_RecTriggers
#endif
#if defined(DBSQL_DEFAULT_FOREIGN_KEYS) && DBSQL_DEFAULT_FOREIGN_KEYS
                 | DBSQL_ForeignKeys
#endif
#if defined(DBSQL_REVERSE_UNORDERED_SELECTS)
                 | DBSQL_ReverseOrder
#endif
#if defined(DBSQL_ENABLE_OVERSIZE_CELL_CHECK)
                 | DBSQL_CellSizeCk
#endif
#if defined(DBSQL_ENABLE_FTS3_TOKENIZER)
                 | DBSQL_Fts3Tokenizer
#endif
#if defined(DBSQL_ENABLE_QPSG)
                 | DBSQL_EnableQPSG
#endif
#if defined(DBSQL_DEFAULT_DEFENSIVE)
                 | DBSQL_Defensive
#endif
#if defined(DBSQL_DEFAULT_LEGACY_ALTER_TABLE)
                 | DBSQL_LegacyAlter
#endif
#if defined(DBSQL_ENABLE_STMT_SCANSTATUS)
                 | DBSQL_StmtScanStatus
#endif
      ;
  __hash_init(&db->coll_seqs);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  __hash_init(&db->modules);
#endif

  /* Add the default collation sequence BINARY. BINARY works for both UTF-8
  ** and UTF-16, so add a version for each to avoid any unnecessary
  ** conversions. The only error that can occur here is a malloc() failure.
  **
  ** EVIDENCE-OF: R-52786-44878 dbsql defines three built-in collating
  ** functions:
  */
  __create_collation(db, __str_binary, DBSQL_UTF8, 0, __bin_coll_func, 0);
  __create_collation(db, __str_binary, DBSQL_UTF16BE, 0, __bin_coll_func, 0);
  __create_collation(db, __str_binary, DBSQL_UTF16LE, 0, __bin_coll_func, 0);
  __create_collation(db, "NOCASE", DBSQL_UTF8, 0, __nocase_collating_func, 0);
  __create_collation(db, "RTRIM", DBSQL_UTF8, 0, __rtrim_coll_func, 0);
  if( db->malloc_failed ){
    goto opendb_out;
  }

#if DBSQL_OS_UNIX && defined(DBSQL_OS_KV_OPTIONAL)
  /* Process magic filenames ":local_storage:" and ":session_storage:" */
  if( filename && filename[0]==':' ){
    if( strcmp(filename, ":localStorage:")==0 ){
      filename = "file:local?vfs=kvvfs";
      flags |= DBSQL_OPEN_URI;
    }else if( strcmp(filename, ":sessionStorage:")==0 ){
      filename = "file:session?vfs=kvvfs";
      flags |= DBSQL_OPEN_URI;
    }
  }
#endif /* DBSQL_OS_UNIX && defined(DBSQL_OS_KV_OPTIONAL) */

  /* parser_t the filename/URI argument
  **
  ** Only allow sensible combinations of bits in the flags argument. 
  ** Throw an error if any non-sense combination is used.  If we
  ** do not block illegal combinations here, it could trigger
  ** assert() statements in deeper layers.  Sensible combinations
  ** are:
  **
  **  1:  DBSQL_OPEN_READONLY
  **  2:  DBSQL_OPEN_READWRITE
  **  6:  DBSQL_OPEN_READWRITE | DBSQL_OPEN_CREATE
  */
  db->open_flags = flags;
  assert( DBSQL_OPEN_READONLY  == 0x01 );
  assert( DBSQL_OPEN_READWRITE == 0x02 );
  assert( DBSQL_OPEN_CREATE    == 0x04 );
  testcase( (1<<(flags&7))==0x02 ); /* READONLY */
  testcase( (1<<(flags&7))==0x04 ); /* READWRITE */
  testcase( (1<<(flags&7))==0x40 ); /* READWRITE | CREATE */
  if( ((1<<(flags&7)) & 0x46)==0 ){
    rc = DBSQL_MISUSE_BKPT;  /* IMP: R-18321-05872 */
  }else{
    if( filename==0 ) filename = ":memory:";
    rc = __parse_uri(vfs, filename, &flags, &db->vfs, &z_open, &errmsg);
  }
  if( rc!=DBSQL_OK ){
    if( rc==DBSQL_NOMEM ) __oom_fault(db);
    __error_with_msg(db, rc, errmsg ? "%s" : 0, errmsg);
    dbsql_free(errmsg);
    goto opendb_out;
  }
  assert( db->vfs!=0 );
#if DBSQL_OS_KV || defined(DBSQL_OS_KV_OPTIONAL)
  if( dbsql_stricmp(db->vfs->name, "kvvfs")==0 ){
    db->temp_store = 2;
  }
#endif

  /* Open the backend database driver */
  rc = __sm_open(db->vfs, z_open, db, &db->dbs[0].bt, 0,
                        flags | DBSQL_OPEN_MAIN_DB);
  if( rc!=DBSQL_OK ){
    if( rc==DBSQL_IOERR_NOMEM ){
      rc = DBSQL_NOMEM_BKPT;
    }
    __dbsql_error(db, rc);
    goto opendb_out;
  }
  __sm_enter(db->dbs[0].bt);
  db->dbs[0].schema = __schema_get(db, db->dbs[0].bt);
  if( !db->malloc_failed ){
    __set_text_encoding(db, SCHEMA_ENC(db));
  }
  __sm_leave(db->dbs[0].bt);
  db->dbs[1].schema = __schema_get(db, 0);

  /* The default safety_level for the main database is FULL; for the temp
  ** database it is OFF. This matches the pager layer defaults. 
  */
  db->dbs[0].db_s_name = "main";
  db->dbs[0].safety_level = DBSQL_DEFAULT_SYNCHRONOUS+1;
  db->dbs[1].db_s_name = "temp";
  db->dbs[1].safety_level = PAGER_SYNCHRONOUS_OFF;

  db->open_state = DBSQL_STATE_OPEN;
  if( db->malloc_failed ){
    goto opendb_out;
  }

  /* Register all built-in functions, but do not attempt to read the
  ** database schema yet. This is delayed until the first time the database
  ** is accessed.
  */
  __dbsql_error(db, DBSQL_OK);
  __register_per_connection_builtin_functions(db);
  rc = dbsql_errcode(db);


  /* Load compiled-in extensions */
  for(i=0; rc==DBSQL_OK && i<ARRAY_SIZE(__builtin_extensions); i++){
    rc = __builtin_extensions[i](db);
  }

  /* Load automatic extensions - extensions that have been registered
  ** using the dbsql_automatic_extension() API.
  */
  if( rc==DBSQL_OK ){
    __auto_load_extensions(db);
    rc = dbsql_errcode(db);
    if( rc!=DBSQL_OK ){
      goto opendb_out;
    }
  }

#ifdef DBSQL_ENABLE_INTERNAL_FUNCTIONS
  /* Testing use only!!! The -DSQLITE_ENABLE_INTERNAL_FUNCTIONS=1 compile-time
  ** option gives access to internal functions by default. 
  ** Testing use only!!! */
  db->m_db_flags |= DBFLAG_InternalFunc;
#endif

  /* -DSQLITE_DEFAULT_LOCKING_MODE=1 makes EXCLUSIVE the default locking
  ** mode.  -DSQLITE_DEFAULT_LOCKING_MODE=0 make NORMAL the default locking
  ** mode.  Doing nothing at all also makes NORMAL the default.
  */
#ifdef DBSQL_DEFAULT_LOCKING_MODE
  db->dflt_lock_mode = DBSQL_DEFAULT_LOCKING_MODE;
  __sm_pager_locking_mode(__sm_pager(db->dbs[0].bt),
                          DBSQL_DEFAULT_LOCKING_MODE);
#endif

  if( rc ) __dbsql_error(db, rc);

  /* Enable the lookaside-malloc subsystem */
  __setup_lookaside(db, 0, __global_config.sz_lookaside,
                        __global_config.n_lookaside);

  dbsql_wal_autocheckpoint(db, DBSQL_DEFAULT_WAL_AUTOCHECKPOINT);

opendb_out:
  if( db ){
    assert( db->mutex!=0 || is_threadsafe==0
           || __global_config.full_mutex==0 );
    dbsql_mutex_leave(db->mutex);
  }
  rc = dbsql_errcode(db);
  assert( db!=0 || (rc&0xff)==DBSQL_NOMEM );
  if( (rc&0xff)==DBSQL_NOMEM ){
    dbsql_close(db);
    db = 0;
  }else if( rc!=DBSQL_OK ){
    db->open_state = DBSQL_STATE_SICK;
  }
  *pp_db = db;
#ifdef DBSQL_ENABLE_SQLLOG
  if( __global_config.sqllog ){
    /* Opening a db handle. Fourth parameter is passed 0. */
    void *arg = __global_config.sqllog_arg;
    __global_config.sqllog(arg, db, filename, 0);
  }
#endif
  dbsql_free_filename(z_open);
  return rc;
}


/*
** Open a new database handle.
*/
int dbsql_open(
  const char *filename,
  DBSQL **db
){
  return __open_database(filename, db,
                      DBSQL_OPEN_READWRITE | DBSQL_OPEN_CREATE, 0);
}
int dbsql_open_v2(
  const char *filename,   /* Database filename (UTF-8) */
  DBSQL **db,         /* OUT: dbsql db handle */
  int flags,              /* Flags */
  const char *vfs        /* Name of VFS module to use */
){
  return __open_database(filename, db, (unsigned int)flags, vfs);
}

#ifndef DBSQL_OMIT_UTF16
/*
** Open a new database handle.
*/
int dbsql_open16(
  const void *filename,
  DBSQL **db
){
  char const *filename8;   /* filename encoded in UTF-8 instead of UTF-16 */
  dbsql_value_t *val;
  int rc;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( db==0 ) return DBSQL_MISUSE_BKPT;
#endif
  *db = 0;
#ifndef DBSQL_OMIT_AUTOINIT
  rc = dbsql_initialize();
  if( rc ) return rc;
#endif
  if( filename==0 ) filename = "\000\000";
  val = __value_new(0);
  __value_set_str(val, -1, filename, DBSQL_UTF16NATIVE, DBSQL_STATIC);
  filename8 = __value_text(val, DBSQL_UTF8);
  if( filename8 ){
    rc = __open_database(filename8, db,
                      DBSQL_OPEN_READWRITE | DBSQL_OPEN_CREATE, 0);
    assert( *db || rc==DBSQL_NOMEM );
    if( rc==DBSQL_OK && !DB_HAS_PROPERTY(*db, 0, DB_SchemaLoaded) ){
      SCHEMA_ENC(*db) = ENC(*db) = DBSQL_UTF16NATIVE;
    }
  }else{
    rc = DBSQL_NOMEM_BKPT;
  }
  __value_free(val);

  return rc & 0xff;
}
#endif /* DBSQL_OMIT_UTF16 */

/*
** Register a new collation sequence with the database handle db.
*/
int dbsql_create_collation(
  DBSQL* db,
  const char *name,
  int enc,
  void* ctx,
  int(*compare)(void*,int,const void*,int,const void*)
){
  return dbsql_create_collation_v2(db, name, enc, ctx, compare, 0);
}

/*
** Register a new collation sequence with the database handle db.
*/
int dbsql_create_collation_v2(
  DBSQL* db,
  const char *name,
  int enc,
  void* ctx,
  int(*compare)(void*,int,const void*,int,const void*),
  void(*del)(void*)
){
  int rc;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || name==0 ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  assert( !db->malloc_failed );
  rc = __create_collation(db, name, (u_int8_t)enc, ctx, compare, del);
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

#ifndef DBSQL_OMIT_UTF16
/*
** Register a new collation sequence with the database handle db.
*/
int dbsql_create_collation16(
  DBSQL* db,
  const void *name,
  int enc,
  void* ctx,
  int(*compare)(void*,int,const void*,int,const void*)
){
  int rc = DBSQL_OK;
  char *name8;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || name==0 ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  assert( !db->malloc_failed );
  name8 = __utf16to8(db, name, -1, DBSQL_UTF16NATIVE);
  if( name8 ){
    rc = __create_collation(db, name8, (u_int8_t)enc, ctx, compare, 0);
    __dbsql_db_free(db, name8);
  }
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}
#endif /* DBSQL_OMIT_UTF16 */

/*
** Register a collation sequence factory callback with the database handle
** db. Replace any previously installed collation sequence factory.
*/
int dbsql_collation_needed(
  DBSQL *db,
  void *coll_needed_arg,
  void(*coll_needed)(void*,DBSQL*,int text_rep,const char*)
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  db->coll_needed = coll_needed;
  db->coll_needed16 = 0;
  db->coll_needed_arg = coll_needed_arg;
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}

#ifndef DBSQL_OMIT_UTF16
/*
** Register a collation sequence factory callback with the database handle
** db. Replace any previously installed collation sequence factory.
*/
int dbsql_collation_needed16(
  DBSQL *db,
  void *coll_needed_arg,
  void(*coll_needed16)(void*,DBSQL*,int text_rep,const void*)
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  db->coll_needed = 0;
  db->coll_needed16 = coll_needed16;
  db->coll_needed_arg = coll_needed_arg;
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}
#endif /* DBSQL_OMIT_UTF16 */

/*
** Find existing client data.
*/
void *dbsql_get_clientdata(DBSQL *db, const char *name){
  db_client_data_t *p;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !name || !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  for(p=db->db_data; p; p=p->next){
    if( strcmp(p->name, name)==0 ){
      void *result = p->data;
      dbsql_mutex_leave(db->mutex);
      return result;
    }
  }
  dbsql_mutex_leave(db->mutex);
  return 0;
}

/*
** Add new client data to a database connection.
*/
int dbsql_set_clientdata(
  DBSQL *db,                   /* Attach client data to this connection */
  const char *name,             /* Name of the client data */
  void *data,                   /* The client data itself */
  void (*destructor)(void*)     /* Destructor */
){
  db_client_data_t *p, **pp;
  dbsql_mutex_enter(db->mutex);
  pp = &db->db_data;
  for(p=db->db_data; p && strcmp(p->name,name); p=p->next){
    pp = &p->next;
  }
  if( p ){
    assert( p->data!=0 );
    if( p->destructor ) p->destructor(p->data);
    if( data==0 ){
      *pp = p->next;
      dbsql_free(p);
      dbsql_mutex_leave(db->mutex);
      return DBSQL_OK;
    }
  }else if( data==0 ){
    dbsql_mutex_leave(db->mutex);
    return DBSQL_OK;
  }else{
    size_t n = strlen(name);
    p = dbsql_malloc64( SZ_DBCLIENTDATA(n+1) );
    if( p==0 ){
      if( destructor ) destructor(data);
      dbsql_mutex_leave(db->mutex);
      return DBSQL_NOMEM;
    }
    memcpy(p->name, name, n+1);
    p->next = db->db_data;
    db->db_data = p;
  }
  p->data = data;
  p->destructor = destructor;
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}


#ifndef DBSQL_OMIT_DEPRECATED
/*
** This function is now an anachronism. It used to be used to recover from a
** malloc() failure, but dbsql now does this automatically.
*/
int dbsql_global_recover(void){
  return DBSQL_OK;
}
#endif

/*
** Test to see whether or not the database connection is in autocommit
** mode.  Return TRUE if it is and FALSE if not.  Autocommit mode is on
** by default.  Autocommit is disabled by a BEGIN statement and reenabled
** by the next COMMIT or ROLLBACK.
*/
int dbsql_get_autocommit(DBSQL *db){
  int i_ret;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  i_ret = db->auto_commit;
  dbsql_mutex_leave(db->mutex);
  return i_ret;
}

/*
** The following routines are substitutes for constants DBSQL_CORRUPT,
** DBSQL_MISUSE, DBSQL_CANTOPEN, DBSQL_NOMEM and possibly other error
** constants.  They serve two purposes:
**
**   1.  Serve as a convenient place to set a breakpoint in a debugger
**       to detect when version error conditions occurs.
**
**   2.  Invoke dbsql_log() to provide the source code location where
**       a low-level error is first detected.
*/
int __report_error(int i_err, int lineno, const char *type){
  dbsql_log(i_err, "%s at line %d of [%.10s]",
              type, lineno, 20+dbsql_sourceid());
  return i_err;
}
int __corrupt_error(int lineno){
  testcase( __global_config.x_log!=0 );
  return __report_error(DBSQL_CORRUPT, lineno, "database corruption");
}
int __misuse_error(int lineno){
  testcase( __global_config.x_log!=0 );
  return __report_error(DBSQL_MISUSE, lineno, "misuse");
}
int __cantopen_error(int lineno){
  testcase( __global_config.x_log!=0 );
  return __report_error(DBSQL_CANTOPEN, lineno, "cannot open file");
}
#if defined(DBSQL_DEBUG) || defined(DBSQL_ENABLE_CORRUPT_PGNO)
int __corrupt_pgno_error(int lineno, pgno_t pgno){
  char msg[100];
  dbsql_snprintf(sizeof(msg), msg, "database corruption page %d", pgno);
  testcase( __global_config.x_log!=0 );
  return __report_error(DBSQL_CORRUPT, lineno, msg);
}
#endif
#ifdef DBSQL_DEBUG
int __nomem_error(int lineno){
  testcase( __global_config.x_log!=0 );
  return __report_error(DBSQL_NOMEM, lineno, "OOM");
}
int __ioerrnomem_error(int lineno){
  testcase( __global_config.x_log!=0 );
  return __report_error(DBSQL_IOERR_NOMEM, lineno, "I/O OOM error");
}
#endif

#ifndef DBSQL_OMIT_DEPRECATED
/*
** This is a convenience routine that makes sure that all thread-specific
** data for this thread has been deallocated.
**
** dbsql no longer uses thread-specific data so this routine is now a
** no-op.  It is retained for historical compatibility.
*/
void dbsql_thread_cleanup(void){
}
#endif

/*
** Return meta information about a specific column of a database table.
** See comment in DBSQL.h (sqlite.h.in) for details.
*/
int dbsql_table_column_metadata(
  DBSQL *db,                /* Connection handle */
  const char *db_name,        /* Database name or NULL */
  const char *table_name,     /* table_t name */
  const char *column_name,    /* column_t name */
  char const **pz_data_type,    /* OUTPUT: Declared data type */
  char const **pz_coll_seq,     /* OUTPUT: Collation sequence name */
  int *not_null,              /* OUTPUT: True if NOT NULL constraint exists */
  int *primary_key,           /* OUTPUT: True if column part of PK */
  int *p_autoinc               /* OUTPUT: True if column is auto-increment */
){
  int rc;
  char *errmsg = 0;
  table_t *tab = 0;
  column_t *col = 0;
  int i_col = 0;
  char const *data_type = 0;
  char const *coll_seq = 0;
  int notnull = 0;
  int primarykey = 0;
  int autoinc = 0;


#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || table_name==0 ){
    return DBSQL_MISUSE_BKPT;
  }
#endif

  /* Ensure the database schema has been loaded */
  dbsql_mutex_enter(db->mutex);
  __sm_enter_all(db);
  rc = __init(db, &errmsg);
  if( DBSQL_OK!=rc ){
    goto error_out;
  }

  /* Locate the table in question */
  tab = __find_table(db, table_name, db_name);
  if( !tab || IsView(tab) ){
    tab = 0;
    goto error_out;
  }

  /* Find the column for which info is requested */
  if( column_name==0 ){
    /* Query for existence of table only */
  }else{
    i_col = __column_index(tab, column_name);
    if( i_col>=0 ){
      col = &tab->cols[i_col];
    }else{
      if( HAS_ROWID(tab) && __is_rowid(column_name) ){
        i_col = tab->i_p_key;
        col = i_col>=0 ? &tab->cols[i_col] : 0;
      }else{
        tab = 0;
        goto error_out;
      }
    }
  }

  /* The following block stores the meta information that will be returned
  ** to the caller in local variables data_type, coll_seq, notnull, primarykey
  ** and autoinc. At this point there are two possibilities:
  **
  **     1. The specified column name was rowid", "oid" or "_rowid_"
  **        and there is no explicitly declared IPK column.
  **
  **     2. The table is not a view and the column name identified an
  **        explicitly declared column. Copy meta information from *col.
  */
  if( col ){
    data_type = __column_type(col,0);
    coll_seq = __column_coll(col);
    notnull = col->not_null!=0;
    primarykey  = (col->col_flags & COLFLAG_PRIMKEY)!=0;
    autoinc = tab->i_p_key==i_col && (tab->tab_flags & TF_Autoincrement)!=0;
  }else{
    data_type = "INTEGER";
    primarykey = 1;
  }
  if( !coll_seq ){
    coll_seq = __str_binary;
  }

error_out:
  __sm_leave_all(db);

  /* Whether the function call succeeded or failed, set the output parameters
  ** to whatever their local counterparts contain. If an error did occur,
  ** this has the effect of zeroing all output parameters.
  */
  if( pz_data_type ) *pz_data_type = data_type;
  if( pz_coll_seq ) *pz_coll_seq = coll_seq;
  if( not_null ) *not_null = notnull;
  if( primary_key ) *primary_key = primarykey;
  if( p_autoinc ) *p_autoinc = autoinc;

  if( DBSQL_OK==rc && !tab ){
    __dbsql_db_free(db, errmsg);
    errmsg = __m_printf(db, "no such table column: %s.%s", table_name,
        column_name);
    rc = DBSQL_ERROR;
  }
  __error_with_msg(db, rc, (errmsg?"%s":0), errmsg);
  __dbsql_db_free(db, errmsg);
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** Sleep for a little while.  Return the amount of time slept.
*/
int dbsql_sleep(int ms){
  dbsql_vfs_t *vfs;
  int rc;
  vfs = dbsql_vfs_find(0);
  if( vfs==0 ) return 0;

  /* This function works in milliseconds, but the underlying __os_sleep()
  ** API uses microseconds. Hence the 1000's.
  */
  rc = (__dbsql_os_sleep(vfs, ms<0 ? 0 : 1000*ms)/1000);
  return rc;
}

/*
** Enable or disable the extended result codes.
*/
int dbsql_extended_result_codes(DBSQL *db, int onoff){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  db->err_mask = onoff ? 0xffffffff : 0xff;
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}

/*
** Invoke the file_control method on a particular database.
*/
int dbsql_file_control(DBSQL *db, const char *db_name, int op, void *arg){
  int rc = DBSQL_ERROR;
  sm_t *btree;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  btree = __db_name_to_btree(db, db_name);
  if( btree ){
    pager_t *pager;
    dbsql_file_t *fd;
    __sm_enter(btree);
    pager = __sm_pager(btree);
    assert( pager!=0 );
    fd = __sm_pager_file(pager);
    assert( fd!=0 );
    if( op==DBSQL_FCNTL_FILE_POINTER ){
      *(dbsql_file_t**)arg = fd;
      rc = DBSQL_OK;
    }else if( op==DBSQL_FCNTL_VFS_POINTER ){
      *(dbsql_vfs_t**)arg = __sm_pager_vfs(pager);
      rc = DBSQL_OK;
    }else if( op==DBSQL_FCNTL_JOURNAL_POINTER ){
      *(dbsql_file_t**)arg = __sm_pager_jrnl_file(pager);
      rc = DBSQL_OK;
    }else if( op==DBSQL_FCNTL_DATA_VERSION ){
      *(unsigned int*)arg = __sm_pager_data_version(pager);
      rc = DBSQL_OK;
    }else if( op==DBSQL_FCNTL_RESERVE_BYTES ){
      int i_new = *(int*)arg;
      *(int*)arg = __sm_get_requested_reserve(btree);
      if( i_new>=0 && i_new<=255 ){
        __sm_set_page_size(btree, 0, i_new, 0);
      }
      rc = DBSQL_OK;
    }else if( op==DBSQL_FCNTL_RESET_CACHE ){
      __sm_clear_cache(btree);
      rc = DBSQL_OK;
    }else{
      int n_save = db->busy_handler.n_busy;
      rc = __dbsql_os_file_control(fd, op, arg);
      db->busy_handler.n_busy = n_save;
    }
    __sm_leave(btree);
  }
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** Interface to the testing logic.
*/
int dbsql_test_control(int op, ...){
  int rc = 0;
#ifdef DBSQL_UNTESTABLE
  UNUSED_PARAMETER(op);
#else
  va_list ap;
  va_start(ap, op);
  switch( op ){

    /*
    ** Save the current state of the PRNG.
    */
    case DBSQL_TESTCTRL_PRNG_SAVE: {
      __prng_save_state();
      break;
    }

    /*
    ** Restore the state of the PRNG to the last state saved using
    ** PRNG_SAVE.  If PRNG_SAVE has never before been called, then
    ** this verb acts like PRNG_RESET.
    */
    case DBSQL_TESTCTRL_PRNG_RESTORE: {
      __prng_restore_state();
      break;
    }

    /*  dbsql_test_control(DBSQL_TESTCTRL_PRNG_SEED, int x, DBSQL *db);
    **
    ** Control the seed for the pseudo-random number generator (PRNG) that
    ** is built into dbsql.  Cases:
    **
    **    x!=0 && db!=0       Seed the PRNG to the current value of the
    **                        schema cookie in the main database for db, or
    **                        x if the schema cookie is zero.  This case
    **                        is convenient to use with database fuzzers
    **                        as it allows the fuzzer some control over the
    **                        the PRNG seed.
    **
    **    x!=0 && db==0       Seed the PRNG to the value of x.
    **
    **    x==0 && db==0       Revert to default behavior of using the
    **                        randomness method on the primary VFS.
    **
    ** This test-control also resets the PRNG so that the new seed will
    ** be used for the next call to dbsql_randomness().
    */
#ifndef DBSQL_OMIT_WSD
    case DBSQL_TESTCTRL_PRNG_SEED: {
      int x = va_arg(ap, int);
      int y;
      DBSQL *db = va_arg(ap, DBSQL*);
      assert( db==0 || db->dbs[0].schema!=0 );
      if( db && (y = db->dbs[0].schema->schema_cookie)!=0 ){ x = y; }
      __config.i_prng_seed = x;
      dbsql_randomness(0,0);
      break;
    }
#endif

    /*  dbsql_test_control(DBSQL_TESTCTRL_FK_NO_ACTION, DBSQL *db, int b);
    **
    ** If b is true, then activate the DBSQL_FkNoAction setting.  If b is
    ** false then clear that setting.  If the DBSQL_FkNoAction setting is
    ** enabled, all foreign key ON DELETE and ON UPDATE actions behave as if
    ** they were NO ACTION, regardless of how they are defined.
    **
    ** NB:  One must usually run "PRAGMA writable_schema=RESET" after
    ** using this test-control, before it will take full effect.  failing
    ** to reset the schema can result in some unexpected behavior.
    */
    case DBSQL_TESTCTRL_FK_NO_ACTION: {
      DBSQL *db = va_arg(ap, DBSQL*);
      int b = va_arg(ap, int);
      if( b ){
        db->flags |= DBSQL_FkNoAction;
      }else{
        db->flags &= ~DBSQL_FkNoAction;
      }
      break;
    }

    /*
    **  dbsql_test_control(BITVEC_TEST, size, program)
    **
    ** Run a test against a bitvec_t object of size.  The program argument
    ** is an array of integers that defines the test.  Return -1 on a
    ** memory allocation error, 0 on success, or non-zero for an error.
    ** See the __bitvec_builtin_test() for additional information.
    */
    case DBSQL_TESTCTRL_BITVEC_TEST: {
      int sz = va_arg(ap, int);
      int *progs = va_arg(ap, int*);
      rc = __bitvec_builtin_test(sz, progs);
      break;
    }

    /*
    **  dbsql_test_control(FAULT_INSTALL, callback)
    **
    ** Arrange to invoke callback() whenever __fault_sim() is called,
    ** if callback is not NULL.
    **
    ** As a test of the fault simulator mechanism itself, __fault_sim(0)
    ** is called immediately after installing the new callback and the return
    ** value from __fault_sim(0) becomes the return from
    ** dbsql_test_control().
    */
    case DBSQL_TESTCTRL_FAULT_INSTALL: {
      /* A bug in MSVC prevents it from understanding pointers to functions
      ** types in the second argument to va_arg().  Work around the problem
      ** using a typedef.
      ** http://support.microsoft.com/kb/47961  <-- dead hyperlink
      ** Search at http://web.archive.org/ to find the 2015-03-16 archive
      ** of the link above to see the original text.
      ** __global_config.test_callback = va_arg(ap, int(*)(int));
      */
      typedef int(*__fault_func_type)(int);
      __global_config.test_callback = va_arg(ap, __fault_func_type);
      rc = __fault_sim(0);
      break;
    }

    /*
    **  dbsql_test_control(BENIGN_MALLOC_HOOKS, begin, end)
    **
    ** Register hooks to call to indicate which malloc() failures
    ** are benign.
    */
    case DBSQL_TESTCTRL_BENIGN_MALLOC_HOOKS: {
      typedef void (*void_function)(void);
      void_function benign_begin;
      void_function benign_end;
      benign_begin = va_arg(ap, void_function);
      benign_end = va_arg(ap, void_function);
      __benign_malloc_hooks(benign_begin, benign_end);
      break;
    }

    /*
    **  dbsql_test_control(DBSQL_TESTCTRL_PENDING_BYTE, unsigned int X)
    **
    ** Set the PENDING byte to the value in the argument, if X>0.
    ** Make no changes if X==0.  Return the value of the pending byte
    ** as it existing before this routine was called.
    **
    ** IMPORTANT:  Changing the PENDING byte from 0x40000000 results in
    ** an incompatible database file format.  Changing the PENDING byte
    ** while any database connection is open results in undefined and
    ** deleterious behavior.
    */
    case DBSQL_TESTCTRL_PENDING_BYTE: {
      rc = PENDING_BYTE;
#ifndef DBSQL_OMIT_WSD
      {
        unsigned int new_val = va_arg(ap, unsigned int);
        if( new_val ) __pending_byte = new_val;
      }
#endif
      break;
    }

    /*
    **  dbsql_test_control(DBSQL_TESTCTRL_ASSERT, int X)
    **
    ** This action provides a run-time test to see whether or not
    ** assert() was enabled at compile-time.  If X is true and assert()
    ** is enabled, then the return value is true.  If X is true and
    ** assert() is disabled, then the return value is zero.  If X is
    ** false and assert() is enabled, then the assertion fires and the
    ** process aborts.  If X is false and assert() is disabled, then the
    ** return value is zero.
    */
    case DBSQL_TESTCTRL_ASSERT: {
      volatile int x = 0;
      assert( /*side-effects-ok*/ (x = va_arg(ap,int))!=0 );
      rc = x;
#if defined(DBSQL_DEBUG)
      /* Invoke these debugging routines so that the compiler does not
      ** issue "defined but not used" warnings. */
      if( x==9999 ){
        __show_expr(0);
        __show_expr_list(0);
        __show_id_list(0);
        __show_src_list(0);
        __show_with(0);
        __show_upsert(0);
#ifndef DBSQL_OMIT_TRIGGER
        __show_trigger_step(0);
        __show_trigger_step_list(0);
        __show_trigger(0);
        __show_trigger_list(0);
#endif
#ifndef DBSQL_OMIT_WINDOWFUNC
        __show_window(0);
        __show_win_func(0);
#endif
        __show_select(0);
      }
#endif
      break;
    }


    /*
    **  dbsql_test_control(DBSQL_TESTCTRL_ALWAYS, int X)
    **
    ** This action provides a run-time test to see how the ALWAYS and
    ** NEVER macros were defined at compile-time.
    **
    ** The return value is ALWAYS(X) if X is true, or 0 if X is false.
    **
    ** The recommended test is X==2.  If the return value is 2, that means
    ** ALWAYS() and NEVER() are both no-op pass-through macros, which is the
    ** default setting.  If the return value is 1, then ALWAYS() is either
    ** hard-coded to true or else it asserts if its argument is false.
    ** The first behavior (hard-coded to true) is the case if
    ** DBSQL_TESTCTRL_ASSERT shows that assert() is disabled and the second
    ** behavior (assert if the argument to ALWAYS() is false) is the case if
    ** DBSQL_TESTCTRL_ASSERT shows that assert() is enabled.
    **
    ** The run-time test procedure might look something like this:
    **
    **    if( dbsql_test_control(DBSQL_TESTCTRL_ALWAYS, 2)==2 ){
    **      // ALWAYS() and NEVER() are no-op pass-through macros
    **    }else if( dbsql_test_control(DBSQL_TESTCTRL_ASSERT, 1) ){
    **      // ALWAYS(x) asserts that x is true. NEVER(x) asserts x is false.
    **    }else{
    **      // ALWAYS(x) is a constant 1.  NEVER(x) is a constant 0.
    **    }
    */
    case DBSQL_TESTCTRL_ALWAYS: {
      int x = va_arg(ap,int);
      rc = x ? ALWAYS(x) : 0;
      break;
    }

    /*
    **   dbsql_test_control(DBSQL_TESTCTRL_BYTEORDER);
    **
    ** The integer returned reveals the byte-order of the computer on which
    ** dbsql is running:
    **
    **       1     big-endian,    determined at run-time
    **      10     little-endian, determined at run-time
    **  432101     big-endian,    determined at compile-time
    **  123410     little-endian, determined at compile-time
    */
    case DBSQL_TESTCTRL_BYTEORDER: {
      rc = DBSQL_BYTEORDER*100 + DBSQL_LITTLEENDIAN*10 + DBSQL_BIGENDIAN;
      break;
    }

    /*  dbsql_test_control(DBSQL_TESTCTRL_OPTIMIZATIONS, DBSQL *db, int N)
    **
    ** Enable or disable various optimizations for testing purposes.  The
    ** argument N is a bitmask of optimizations to be disabled.  For normal
    ** operation N should be 0.  The idea is that a test program (like the
    ** SQL Logic Test or SLT test module) can run the same SQL multiple times
    ** with various optimizations disabled to verify that the same answer
    ** is obtained in every case.
    */
    case DBSQL_TESTCTRL_OPTIMIZATIONS: {
      DBSQL *db = va_arg(ap, DBSQL*);
      db->db_opt_flags = va_arg(ap, u_int32_t);
      break;
    }

    /*  dbsql_test_control(DBSQL_TESTCTRL_GETOPT, DBSQL *db, int *N)
    **
    ** Write the current optimization settings into *N.  A zero bit means that
    ** the optimization is on, and a 1 bit means that the optimization is off.
    */
    case DBSQL_TESTCTRL_GETOPT: {
      DBSQL *db = va_arg(ap, DBSQL*);
      int *n = va_arg(ap, int*);
      *n = db->db_opt_flags;
      break;
    }

    /*   dbsql_test_control(DBSQL_TESTCTRL_LOCALTIME_FAULT, onoff, alt);
    **
    ** If parameter onoff is 1, subsequent calls to localtime() fail.
    ** If 2, then invoke alt() instead of localtime().  If 0, normal
    ** processing.
    **
    ** alt arguments are void pointers, but they really want to be:
    **
    **    int alt(const time_t*, struct tm*);
    **
    ** alt should write results in to struct tm object of its 2nd argument
    ** and return zero on success, or return non-zero on failure.
    */
    case DBSQL_TESTCTRL_LOCALTIME_FAULT: {
      __global_config.localtime_fault = va_arg(ap, int);
      if( __global_config.localtime_fault==2 ){
        typedef int(*__localtime_type)(const void*,void*);
        __global_config.alt_localtime = va_arg(ap, __localtime_type);
      }else{
        __global_config.alt_localtime = 0;
      }
      break;
    }

    /*   dbsql_test_control(DBSQL_TESTCTRL_INTERNAL_FUNCTIONS, DBSQL*);
    **
    ** Toggle the ability to use internal functions on or off for
    ** the database connection given in the argument.
    */
    case DBSQL_TESTCTRL_INTERNAL_FUNCTIONS: {
      DBSQL *db = va_arg(ap, DBSQL*);
      db->m_db_flags ^= DBFLAG_InternalFunc;
      break;
    }

    /*   dbsql_test_control(DBSQL_TESTCTRL_NEVER_CORRUPT, int);
    **
    ** Set or clear a flag that indicates that the database file is always well-
    ** formed and never corrupt.  This flag is clear by default, indicating that
    ** database files might have arbitrary corruption.  Setting the flag during
    ** testing causes certain assert() statements in the code to be activated
    ** that demonstrate invariants on well-formed database files.
    */
    case DBSQL_TESTCTRL_NEVER_CORRUPT: {
      __global_config.never_corrupt = va_arg(ap, int);
      break;
    }

    /*   dbsql_test_control(DBSQL_TESTCTRL_EXTRA_SCHEMA_CHECKS, int);
    **
    ** Set or clear a flag that causes dbsql to verify that type, name,
    ** and tbl_name fields of the dbsql_schema table.  This is normally
    ** on, but it is sometimes useful to turn it off for testing.
    **
    ** 2020-07-22:  Disabling EXTRA_SCHEMA_CHECKS also disables the
    ** verification of rootpage numbers when parsing the schema.  This
    ** is useful to make it easier to reach strange internal error states
    ** during testing.  The EXTRA_SCHEMA_CHECKS setting is always enabled
    ** in production.
    */
    case DBSQL_TESTCTRL_EXTRA_SCHEMA_CHECKS: {
      __global_config.extra_schema_checks = va_arg(ap, int);
      break;
    }

    /* Set the threshold at which OP_Once counters reset back to zero.
    ** By default this is 0x7ffffffe (over 2 billion), but that value is
    ** too big to test in a reasonable amount of time, so this control is
    ** provided to set a small and easily reachable reset value.
    */
    case DBSQL_TESTCTRL_ONCE_RESET_THRESHOLD: {
      __global_config.i_once_reset_threshold = va_arg(ap, int);
      break;
    }

    /*   dbsql_test_control(DBSQL_TESTCTRL_VDBE_COVERAGE, callback, ptr);
    **
    ** Set the VDBE coverage callback function to callback with context
    ** pointer ptr.
    */
    case DBSQL_TESTCTRL_VDBE_COVERAGE: {
#ifdef DBSQL_VDBE_COVERAGE
      typedef void (*branch_callback)(void*,unsigned int,
                                      unsigned char,unsigned char);
      __global_config.vdbe_branch = va_arg(ap,branch_callback);
      __global_config.vdbe_branch_arg = va_arg(ap,void*);
#endif
      break;
    }

    /*   dbsql_test_control(DBSQL_TESTCTRL_SORTER_MMAP, db, n_max); */
    case DBSQL_TESTCTRL_SORTER_MMAP: {
      DBSQL *db = va_arg(ap, DBSQL*);
      db->n_max_sorter_mmap = va_arg(ap, int);
      break;
    }

    /*   dbsql_test_control(DBSQL_TESTCTRL_ISINIT);
    **
    ** Return DBSQL_OK if dbsql has been initialized and DBSQL_ERROR if
    ** not.
    */
    case DBSQL_TESTCTRL_ISINIT: {
      if( __global_config.is_init==0 ) rc = DBSQL_ERROR;
      break;
    }

    /*  dbsql_test_control(DBSQL_TESTCTRL_IMPOSTER, db, db_name, mode, tnum);
    **
    ** This test control is used to create imposter tables.  "db" is a pointer
    ** to the database connection.  db_name is the database name (ex: "main" or
    ** "temp") which will receive the imposter.  "mode" turns imposter mode on
    ** or off.  mode==0 means imposter mode is off.  mode==1 means imposter mode
    ** is on.  mode==2 means imposter mode is on but results in an imposter
    ** table that is read-only unless writable_schema is on.  "tnum" is the
    ** root page of the b-tree to which the imposter table should connect.
    **
    ** Enable imposter mode only when the schema has already been parsed.  Then
    ** run a single CREATE TABLE statement to construct the imposter table in
    ** the parsed schema.  Then turn imposter mode back off again.
    **
    ** If on_off==0 and tnum>0 then reset the schema for all databases, causing
    ** the schema to be reparsed the next time it is needed.  This has the
    ** effect of erasing all imposter tables.
    */
    case DBSQL_TESTCTRL_IMPOSTER: {
      DBSQL *db = va_arg(ap, DBSQL*);
      int i_db;
      dbsql_mutex_enter(db->mutex);
      i_db = __find_db_name(db, va_arg(ap,const char*));
      if( i_db>=0 ){
        db->init.i_db = i_db;
        db->init.busy = db->init.imposter_table = va_arg(ap,int);
        db->init.new_tnum = va_arg(ap,int);
        if( db->init.busy==0 && db->init.new_tnum>0 ){
          __reset_all_schemas_of_connection(db);
        }
      }
      dbsql_mutex_leave(db->mutex);
      break;
    }

#if defined(YYCOVERAGE)
    /*  dbsql_test_control(DBSQL_TESTCTRL_PARSER_COVERAGE, FILE *out)
    **
    ** This test control (only available when dbsql is compiled with
    ** -DYYCOVERAGE) writes a report onto "out" that shows all
    ** state/lookahead combinations in the parser state machine
    ** which are never exercised.  If any state is missed, make the
    ** return code DBSQL_ERROR.
    */
    case DBSQL_TESTCTRL_PARSER_COVERAGE: {
      FILE *out = va_arg(ap, FILE*);
      if( dbsql_parserCoverage(out) ) rc = DBSQL_ERROR;
      break;
    }
#endif /* defined(YYCOVERAGE) */

    /*  dbsql_test_control(DBSQL_TESTCTRL_RESULT_INTREAL, dbsql_func_t*);
    **
    ** This test-control causes the most recent dbsql_result_int64() value
    ** to be interpreted as a MEM_IntReal instead of as an MEM_Int.  Normally,
    ** MEM_IntReal values only arise during an INSERT operation of integer
    ** values into a REAL column, so they can be challenging to test.  This
    ** test-control enables us to write an intreal() SQL function that can
    ** inject an intreal() value at arbitrary places in an SQL statement,
    ** for testing purposes.
    */
    case DBSQL_TESTCTRL_RESULT_INTREAL: {
      dbsql_func_t *ctx = va_arg(ap, dbsql_func_t*);
      __result_int_real(ctx);
      break;
    }

    /*  dbsql_test_control(DBSQL_TESTCTRL_SEEK_COUNT,
    **    DBSQL *db,    // Database connection
    **    u64 *pn_seek     // Write seek count here
    **  );
    **
    ** This test-control queries the seek-counter on the "main" database
    ** file.  The seek-counter is written into *pn_seek and is then reset.
    ** The seek-count is only available if compiled with DBSQL_DEBUG.
    */
    case DBSQL_TESTCTRL_SEEK_COUNT: {
      DBSQL *db = va_arg(ap, DBSQL*);
      u_int64_t *pn = va_arg(ap, u_int64_t*);
      *pn = __sm_seek_count(db->dbs->bt);
      (void)db;  /* Silence harmless unused variable warning */
      break;
    }

    /*  dbsql_test_control(DBSQL_TESTCTRL_TRACEFLAGS, op, ptr)
    **
    **  "ptr" is a pointer to a u32. 
    **
    **   op==0       Store the current __tree_trace in *ptr
    **   op==1       Set __tree_trace to the value *ptr
    **   op==2       Store the current __where_trace in *ptr
    **   op==3       Set __where_trace to the value *ptr
    */
    case DBSQL_TESTCTRL_TRACEFLAGS: {
       int op_trace = va_arg(ap, int);
       u_int32_t *ptr = va_arg(ap, u_int32_t*);
       switch( op_trace ){
         case 0:   *ptr = __tree_trace;      break;
         case 1:   __tree_trace = *ptr;      break;
         case 2:   *ptr = __where_trace;     break;
         case 3:   __where_trace = *ptr;     break;
       }
       break;
    }

    /* dbsql_test_control(DBSQL_TESTCTRL_LOGEST,
    **      double f_in,     // Input value
    **      int *log_est,   // __log_est_from_double(f_in)
    **      u64 *p_int,      // __log_est_to_int(*log_est)
    **      int *log_est2   // __log_est(*p_int)
    ** );
    **
    ** Test access for the log_est_t conversion routines.
    */
    case DBSQL_TESTCTRL_LOGEST: {
      double r_in = va_arg(ap, double);
      log_est_t r_log_est = __log_est_from_double(r_in);
      int *i1 = va_arg(ap,int*);
      u_int64_t *u64 = va_arg(ap,u_int64_t*);
      int *i2 = va_arg(ap,int*);
      *i1 = r_log_est;
      *u64 = __log_est_to_int(r_log_est);
      *i2 = __log_est(*u64);
      break;
    }

    /* dbsql_test_control(DBSQL_TESTCTRL_ATOF, const char *z, double *p);
    **
    ** Test access to the __ato_f() routine.
    */
    case DBSQL_TESTCTRL_ATOF: {
      const char *z = va_arg(ap,const char*);
      double *r = va_arg(ap,double*);
      rc = __ato_f(z,r);
      break;
    }

#if defined(DBSQL_DEBUG) && !defined(DBSQL_OMIT_WSD)
    /* dbsql_test_control(DBSQL_TESTCTRL_TUNE, id, *pi_value)
    **
    ** If "id" is an integer between 1 and DBSQL_NTUNE then set the value
    ** of the id-th tuning parameter to *pi_value.  If "id" is between -1
    ** and -DBSQL_NTUNE, then write the current value of the (-id)-th
    ** tuning parameter into *pi_value.
    **
    ** TUNING parameters are for use during transient development builds,
    ** to help find the best values for constants in the query planner.
    ** Access tuning parameters using the TUNING(ID) macro.  Set the
    ** parameters in the CLI using ".testctrl tune ID VALUE".
    **
    ** Transient use only.  TUNING parameters should not be used in
    ** checked-in code.
    */
    case DBSQL_TESTCTRL_TUNE: {
      int id = va_arg(ap, int);
      int *pi_value = va_arg(ap, int*);
      if( id>0 && id<=DBSQL_NTUNE ){
        TUNING(id) = *pi_value;
      }else if( id<0 && id>=-DBSQL_NTUNE ){
        *pi_value = TUNING(-id);
      }else{
        rc = DBSQL_NOTFOUND;
      }
      break;
    }
#endif

    /* dbsql_test_control(DBSQL_TESTCTRL_JSON_SELFCHECK, &on_off);
    **
    ** Activate or deactivate validation of JSONB that is generated from
    ** text.  Off by default, as the validation is slow.  Validation is
    ** only available if compiled using DBSQL_DEBUG.
    **
    ** If on_off is initially 1, then turn it on.  If on_off is initially
    ** off, turn it off.  If on_off is initially -1, then change on_off
    ** to be the current setting.
    */
    case DBSQL_TESTCTRL_JSON_SELFCHECK: {
#if defined(DBSQL_DEBUG) && !defined(DBSQL_OMIT_WSD)
      int *on_off = va_arg(ap, int*);
      if( *on_off<0 ){
        *on_off = __config.json_selfcheck;
      }else{
        __config.json_selfcheck = (u_int8_t)((*on_off)&0xff);
      }
#endif
      break;
    }
  }
  va_end(ap);
#endif /* DBSQL_UNTESTABLE */
  return rc;
}

/*
** The pager_t stores the Database filename, Journal filename, and WAL filename
** consecutively in memory, in that order.  The database filename is prefixed
** by four zero bytes.  Locate the start of the database filename by searching
** backwards for the first byte following four consecutive zero bytes.
**
** This only works if the filename passed in was obtained from the pager_t.
*/
static const char *__database_name(const char *name){
  while( name[-1]!=0 || name[-2]!=0 || name[-3]!=0 || name[-4]!=0 ){
    name--;
  }
  return name;
}

/*
** Append text z[] to the end of p[].  Return a pointer to the first
** character after then zero terminator on the new text in p[].
*/
static char *__append_text(char *p, const char *z){
  size_t n = strlen(z);
  memcpy(p, z, n+1);
  return p+n+1;
}

/*
** Allocate memory to hold names for a database, journal file, WAL file,
** and query parameters.  The pointer returned is valid for use by
** dbsql_filename_database() and dbsql_uri_parameter() and related
** functions.
**
** Memory layout must be compatible with that generated by the pager
** and expected by dbsql_uri_parameter() and __database_name().
*/
const char *dbsql_create_filename(
  const char *database,
  const char *journal,
  const char *wal,
  int n_param,
  const char **params
){
  int64_t n_byte;
  int i;
  char *result, *p;
  n_byte = strlen(database) + strlen(journal) + strlen(wal) + 10;
  for(i=0; i<n_param*2; i++){
    n_byte += strlen(params[i])+1;
  }
  result = p = dbsql_malloc64( n_byte );
  if( p==0 ) return 0;
  memset(p, 0, 4);
  p += 4;
  p = __append_text(p, database);
  for(i=0; i<n_param*2; i++){
    p = __append_text(p, params[i]);
  }
  *(p++) = 0;
  p = __append_text(p, journal);
  p = __append_text(p, wal);
  *(p++) = 0;
  *(p++) = 0;
  assert( (int64_t)(p - result)==n_byte );
  return result + 4;
}

/*
** Free memory obtained from dbsql_create_filename().  It is a severe
** error to call this routine with any parameter other than a pointer
** previously obtained from dbsql_create_filename() or a NULL pointer.
*/
void dbsql_free_filename(const char *p){
  if( p==0 ) return;
  p = __database_name(p);
  dbsql_free((char*)p - 4);
}


/*
** This is a utility routine, useful to VFS implementations, that checks
** to see if a database file was a URI that contained a specific query
** parameter, and if so obtains the value of the query parameter.
**
** The filename argument is the filename pointer passed into the x_open()
** method of a VFS implementation.  The param argument is the name of the
** query parameter we seek.  This routine returns the value of the param
** parameter if it exists.  If the parameter does not exist, this routine
** returns a NULL pointer.
*/
const char *dbsql_uri_parameter(const char *filename, const char *param){
  if( filename==0 || param==0 ) return 0;
  filename = __database_name(filename);
  return __uri_parameter(filename, param);
}

/*
** Return a pointer to the name of Nth query parameter of the filename.
*/
const char *dbsql_uri_key(const char *filename, int N){
  if( filename==0 || N<0 ) return 0;
  filename = __database_name(filename);
  filename += __strlen30(filename) + 1;
  while( ALWAYS(filename) && filename[0] && (N--)>0 ){
    filename += __strlen30(filename) + 1;
    filename += __strlen30(filename) + 1;
  }
  return filename[0] ? filename : 0;
}

/*
** Return a boolean value for a query parameter.
*/
int dbsql_uri_boolean(const char *filename, const char *param, int dflt){
  const char *z = dbsql_uri_parameter(filename, param);
  dflt = dflt!=0;
  return z ? __get_boolean(z, dflt) : dflt;
}

/*
** Return a 64-bit integer value for a query parameter.
*/
int64_t dbsql_uri_int64(
  const char *filename,    /* Filename as passed to x_open */
  const char *param,       /* URI parameter sought */
  int64_t dflt       /* return if parameter is missing */
){
  const char *z = dbsql_uri_parameter(filename, param);
  int64_t v;
  if( z && __dec_or_hex_to_i64(z, &v)==0 ){
    dflt = v;
  }
  return dflt;
}

/*
** Translate a filename that was handed to a VFS routine into the corresponding
** database, journal, or WAL file.
**
** It is an error to pass this routine a filename string that was not
** passed into the VFS from the dbsql core.  Doing so is similar to
** passing free() a pointer that was not obtained from malloc() - it is
** an error that we cannot easily detect but that will likely cause memory
** corruption.
*/
const char *dbsql_filename_database(const char *filename){
  if( filename==0 ) return 0;
  return __database_name(filename);
}
const char *dbsql_filename_journal(const char *filename){
  if( filename==0 ) return 0;
  filename = __database_name(filename);
  filename += __strlen30(filename) + 1;
  while( ALWAYS(filename) && filename[0] ){
    filename += __strlen30(filename) + 1;
    filename += __strlen30(filename) + 1;
  }
  return filename + 1;
}
const char *dbsql_filename_wal(const char *filename){
#ifdef DBSQL_OMIT_WAL
  UNUSED_PARAMETER(filename);
  return 0;
#else
  filename = dbsql_filename_journal(filename);
  if( filename ) filename += __strlen30(filename) + 1;
  return filename;
#endif
}

/*
** Return the sm_t pointer identified by db_name.  Return NULL if not found.
*/
sm_t *__db_name_to_btree(DBSQL *db, const char *db_name){
  int i_db = db_name ? __find_db_name(db, db_name) : 0;
  return i_db<0 ? 0 : db->dbs[i_db].bt;
}

/*
** Return the name of the N-th database schema.  Return NULL if N is out
** of range.
*/
const char *dbsql_db_name(DBSQL *db, int N){
  const char *ret = 0;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  if( N>=0 && N<db->n_db ){
    ret = db->dbs[N].db_s_name;
  }
  dbsql_mutex_leave(db->mutex);
  return ret;
}

/*
** Return the filename of the database associated with a database
** connection.
*/
const char *dbsql_db_filename(DBSQL *db, const char *db_name){
  sm_t *bt;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif
  bt = __db_name_to_btree(db, db_name);
  return bt ? __sm_get_filename(bt) : 0;
}

/*
** Return 1 if database is read-only or 0 if read/write.  Return -1 if
** no such database exists.
*/
int dbsql_db_readonly(DBSQL *db, const char *db_name){
  sm_t *bt;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    (void)DBSQL_MISUSE_BKPT;
    return -1;
  }
#endif
  bt = __db_name_to_btree(db, db_name);
  return bt ? __sm_is_readonly(bt) : -1;
}

#ifdef DBSQL_ENABLE_SNAPSHOT
/*
** Obtain a snapshot handle for the snapshot of database db currently
** being read by handle db.
*/
int dbsql_snapshot_get(
  DBSQL *db,
  const char *z_db,
  dbsql_snapshot_t **snapshot
){
  int rc = DBSQL_ERROR;
#ifndef DBSQL_OMIT_WAL

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  dbsql_mutex_enter(db->mutex);

  if( db->auto_commit==0 ){
    int i_db = __find_db_name(db, z_db);
    if( i_db==0 || i_db>1 ){
      sm_t *bt = db->dbs[i_db].bt;
      if( DBSQL_TXN_WRITE!=__sm_txn_state(bt) ){
        pager_t *pager = __sm_pager(bt);
        int64_t dummy = 0;
        __sm_pager_snapshot_open(pager, (dbsql_snapshot_t*)&dummy);
        rc = __sm_begin_trans(bt, 0, 0);
        __sm_pager_snapshot_open(pager, 0);
        if( rc==DBSQL_OK ){
          rc = __sm_pager_snapshot_get(__sm_pager(bt), snapshot);
        }
      }
    }
  }

  dbsql_mutex_leave(db->mutex);
#endif   /* DBSQL_OMIT_WAL */
  return rc;
}

/*
** Open a read-transaction on the snapshot identified by snapshot.
*/
int dbsql_snapshot_open(
  DBSQL *db,
  const char *z_db,
  dbsql_snapshot_t *snapshot
){
  int rc = DBSQL_ERROR;
#ifndef DBSQL_OMIT_WAL

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    return DBSQL_MISUSE_BKPT;
  }
#endif
  dbsql_mutex_enter(db->mutex);
  if( db->auto_commit==0 ){
    int i_db;
    i_db = __find_db_name(db, z_db);
    if( i_db==0 || i_db>1 ){
      sm_t *bt = db->dbs[i_db].bt;
      if( __sm_txn_state(bt)!=DBSQL_TXN_WRITE ){
        pager_t *pager = __sm_pager(bt);
        int unlock = 0;
        if( __sm_txn_state(bt)!=DBSQL_TXN_NONE ){
          if( db->n_vdbe_active==0 ){
            rc = __sm_pager_snapshot_check(pager, snapshot);
            if( rc==DBSQL_OK ){
              unlock = 1;
              rc = __sm_commit(bt);
            }
          }
        }else{
          rc = DBSQL_OK;
        }
        if( rc==DBSQL_OK ){
          rc = __sm_pager_snapshot_open(pager, snapshot);
        }
        if( rc==DBSQL_OK ){
          rc = __sm_begin_trans(bt, 0, 0);
          __sm_pager_snapshot_open(pager, 0);
        }
        if( unlock ){
          __sm_pager_snapshot_unlock(pager);
        }
      }
    }
  }

  dbsql_mutex_leave(db->mutex);
#endif   /* DBSQL_OMIT_WAL */
  return rc;
}

/*
** Recover as many snapshots as possible from the wal file associated with
** schema db of database db.
*/
int dbsql_snapshot_recover(DBSQL *db, const char *z_db){
  int rc = DBSQL_ERROR;
#ifndef DBSQL_OMIT_WAL
  int i_db;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ){
    return DBSQL_MISUSE_BKPT;
  }
#endif

  dbsql_mutex_enter(db->mutex);
  i_db = __find_db_name(db, z_db);
  if( i_db==0 || i_db>1 ){
    sm_t *bt = db->dbs[i_db].bt;
    if( DBSQL_TXN_NONE==__sm_txn_state(bt) ){
      rc = __sm_begin_trans(bt, 0, 0);
      if( rc==DBSQL_OK ){
        rc = __sm_pager_snapshot_recover(__sm_pager(bt));
        __sm_commit(bt);
      }
    }
  }
  dbsql_mutex_leave(db->mutex);
#endif   /* DBSQL_OMIT_WAL */
  return rc;
}

/*
** Free a snapshot handle obtained from dbsql_snapshot_get().
*/
void dbsql_snapshot_free(dbsql_snapshot_t *snapshot){
  dbsql_free(snapshot);
}
#endif /* DBSQL_ENABLE_SNAPSHOT */

#ifndef DBSQL_OMIT_COMPILEOPTION_DIAGS
/*
** Given the name of a compile-time option, return true if that option
** was used and false if not.
**
** The name can optionally begin with "DBSQL_" but the "DBSQL_" prefix
** is not required for a match.
*/
int dbsql_compileoption_used(const char *opt_name){
  int i, n;
  int n_opt;
  const char **compile_opts;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( opt_name==0 ){
    (void)DBSQL_MISUSE_BKPT;
    return 0;
  }
#endif

  compile_opts = __compile_options(&n_opt);

  if( __str_nicmp(opt_name, "DBSQL_", 6)==0 ) opt_name += 6;
  n = __strlen30(opt_name);

  /* Since n_opt is normally in single digits, a linear search is
  ** adequate. No need for a binary search. */
  for(i=0; i<n_opt; i++){
    if( __str_nicmp(opt_name, compile_opts[i], n)==0
     && __is_id_char((unsigned char)compile_opts[i][n])==0
    ){
      return 1;
    }
  }
  return 0;
}

/*
** Return the N-th compile-time option string.  If N is out of range,
** return a NULL pointer.
*/
const char *dbsql_compileoption_get(int N){
  int n_opt;
  const char **compile_opts;
  compile_opts = __compile_options(&n_opt);
  if( N>=0 && N<n_opt ){
    return compile_opts[N];
  }
  return 0;
}
#endif /* DBSQL_OMIT_COMPILEOPTION_DIAGS */
