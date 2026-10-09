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
**
** This file contains the common header for all mutex implementations.
** The dbsql_int.h header #includes this file so that it is available
** to all source files.  We break it out in an effort to keep the code
** better organized.
**
** NOTE:  source files should *not* #include this header file directly.
** Source files should #include the dbsql_int.h file and let that file
** include this one indirectly.
*/


/*
** Figure out what version of the code to use.  The choices are
**
**   DBSQL_MUTEX_OMIT         No mutex logic.  Not even stubs.  The
**                             mutexes implementation cannot be overridden
**                             at start-time.
**
**   DBSQL_MUTEX_NOOP         For single-threaded applications.  No
**                             mutual exclusion is provided.  But this
**                             implementation can be overridden at
**                             start-time.
**
**   DBSQL_MUTEX_PTHREADS     For multi-threaded applications on Unix.
**
**   DBSQL_MUTEX_W32          For multi-threaded applications on Win32.
*/
#if !DBSQL_THREADSAFE
# define DBSQL_MUTEX_OMIT
#endif
#if DBSQL_THREADSAFE && !defined(DBSQL_MUTEX_NOOP)
#  if DBSQL_OS_UNIX
#    define DBSQL_MUTEX_PTHREADS
#  elif DBSQL_OS_WIN
#    define DBSQL_MUTEX_W32
#  else
#    define DBSQL_MUTEX_NOOP
#  endif
#endif

#ifdef DBSQL_MUTEX_OMIT
/*
** If this is a no-op implementation, implement everything as macros.
*/
#define dbsql_mutex_alloc(X)    ((dbsql_mutex_t*)8)
#define dbsql_mutex_free(X)
#define dbsql_mutex_enter(X)    
#define dbsql_mutex_try(X)      DBSQL_OK
#define dbsql_mutex_leave(X)    
#define dbsql_mutex_held(X)     ((void)(X),1)
#define dbsql_mutex_notheld(X)  ((void)(X),1)
#define __dbsql_mutex_alloc(X)      ((dbsql_mutex_t*)8)
#define __mutex_init()        DBSQL_OK
#define __mutex_end()
#define MUTEX_LOGIC(X)
#else
#define MUTEX_LOGIC(X)            X
int dbsql_mutex_held(dbsql_mutex_t*);
#endif /* defined(DBSQL_MUTEX_OMIT) */
