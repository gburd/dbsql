/*
** 2004 May 22
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
** This file contains macros and a little bit of code that is common to
** all of the platform-specific files (os_*.c) and is #included into those
** files.
**
** This file should be #included by the os_*.c files only.  It is not a
** general purpose header file.
*/
#ifndef _OS_COMMON_H_
#define _OS_COMMON_H_

/*
** At least two bugs have slipped in because we changed the MEMORY_DEBUG
** macro to DBSQL_DEBUG and some older makefiles have not yet made the
** switch.  The following code should catch this problem at compile-time.
*/
#ifdef MEMORY_DEBUG
# error "The MEMORY_DEBUG macro is obsolete.  Use DBSQL_DEBUG instead."
#endif

/*
** Macros for performance tracing.  Normally turned off.  Only works
** on i486 hardware.
*/
#ifdef DBSQL_PERFORMANCE_TRACE

static u_int64_t g_start;
static u_int64_t g_elapsed;
#define TIMER_START       g_start=__hwtime()
#define TIMER_END         g_elapsed=__hwtime()-g_start
#define TIMER_ELAPSED     g_elapsed
#else
#define TIMER_START
#define TIMER_END
#define TIMER_ELAPSED     ((u_int64_t)0)
#endif

/*
** If we compile with the DBSQL_TEST macro set, then the following block
** of code will give us the ability to simulate a disk I/O error.  This
** is used for testing the I/O recovery logic.
*/
#if defined(DBSQL_TEST)
extern int dbsql_io_error_hit;
extern int dbsql_io_error_hardhit;
extern int dbsql_io_error_pending;
extern int dbsql_io_error_persist;
extern int dbsql_io_error_benign;
extern int dbsql_diskfull_pending;
extern int dbsql_diskfull;
#define SIMULATE_IO_ERROR_BENIGN(X) dbsql_io_error_benign=(X)
#define SIMULATE_IO_ERROR(CODE)  \
  if( (dbsql_io_error_persist && dbsql_io_error_hit) \
       || dbsql_io_error_pending-- == 1 )  \
              { local_ioerr(); CODE; }
static void local_ioerr(){
  IOTRACE(("IOERR\n"));
  dbsql_io_error_hit++;
  if( !dbsql_io_error_benign ) dbsql_io_error_hardhit++;
}
#define SIMULATE_DISKFULL_ERROR(CODE) \
   if( dbsql_diskfull_pending ){ \
     if( dbsql_diskfull_pending == 1 ){ \
       local_ioerr(); \
       dbsql_diskfull = 1; \
       dbsql_io_error_hit = 1; \
       CODE; \
     }else{ \
       dbsql_diskfull_pending--; \
     } \
   }
#else
#define SIMULATE_IO_ERROR_BENIGN(X)
#define SIMULATE_IO_ERROR(A)
#define SIMULATE_DISKFULL_ERROR(A)
#endif /* defined(DBSQL_TEST) */

/*
** When testing, keep a count of the number of open files.
*/
#if defined(DBSQL_TEST)
extern int dbsql_open_file_count;
#define OPEN_COUNTER(X)  dbsql_open_file_count+=(X)
#else
#define OPEN_COUNTER(X)
#endif /* defined(DBSQL_TEST) */

#endif /* !defined(_OS_COMMON_H_) */
