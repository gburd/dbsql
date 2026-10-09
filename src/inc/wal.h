/*
** 2010 February 1
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This header file defines the interface to the write-ahead logging 
** system. Refer to the comments below and the header comment attached to 
** the implementation of each function in log.c for further details.
*/

#ifndef DBSQL_WAL_H
#define DBSQL_WAL_H

#include "dbsql_int.h"

/* Macros for extracting appropriate sync flags for either transaction
** commits (WAL_SYNC_FLAGS(X)) or for checkpoint ops (CKPT_SYNC_FLAGS(X)):
*/
#define WAL_SYNC_FLAGS(X)   ((X)&0x03)
#define CKPT_SYNC_FLAGS(X)  (((X)>>2)&0x03)

#ifdef DBSQL_OMIT_WAL
# define __wal_open(x,y,z)                   0
# define __wal_limit(x,y)
# define __wal_close(v,w,x,y,z)              0
# define __wal_begin_read_transaction(y,z)     0
# define __wal_end_read_transaction(z)
# define __wal_dbsize(y)                     0
# define __wal_begin_write_transaction(y)      0
# define __wal_end_write_transaction(x)        0
# define __wal_undo(x,y,z)                   0
# define __wal_savepoint(y,z)
# define __wal_savepoint_undo(y,z)            0
# define __wal_frames(u,v,w,x,y,z)           0
# define __wal_checkpoint(q,r,s,t,u,v,w,x,y,z) 0
# define __wal_callback(z)                   0
# define __wal_exclusive_mode(y,z)            0
# define __wal_heap_memory(z)                 0
# define __wal_framesize(z)                  0
# define __wal_find_frame(x,y,z)              0
# define __wal_file(x)                       0
# undef DBSQL_USE_SEH
#else

#define WAL_SAVEPOINT_NDATA 4

/* Connection to a write-ahead log (WAL) file. 
** There is one object of this type for each pager. 
*/
typedef struct wal_t wal_t;

/* Open and close a connection to a write-ahead log. */
int __wal_open(dbsql_vfs_t*, dbsql_file_t*, const char *, int, int64_t, wal_t**);
int __wal_close(wal_t *wal, DBSQL*, int sync_flags, int, u_int8_t *);

/* Set the limiting size of a WAL file. */
void __wal_limit(wal_t*, int64_t);

/* Used by readers to open (lock) and close (unlock) a snapshot.  A 
** snapshot is like a read-transaction.  It is the state of the database
** at an instant in time.  __wal_open_snapshot gets a read lock and
** preserves the current state even if the other threads or processes
** write to or checkpoint the WAL.  __wal_close_snapshot() closes the
** transaction and releases the lock.
*/
int __wal_begin_read_transaction(wal_t *wal, int *);
void __wal_end_read_transaction(wal_t *wal);

/* Read a page from the write-ahead log, if it is present. */
int __wal_find_frame(wal_t *, pgno_t, u_int32_t *);
int __wal_read_frame(wal_t *, u_int32_t, int, u_int8_t *);

/* If the WAL is not empty, return the size of the database. */
pgno_t __wal_dbsize(wal_t *wal);

/* Obtain or release the WRITER lock. */
int __wal_begin_write_transaction(wal_t *wal);
int __wal_end_write_transaction(wal_t *wal);

/* Undo any frames written (but not committed) to the log */
int __wal_undo(wal_t *wal, int (*undo)(void *, pgno_t), void *undo_ctx);

/* Return an integer that records the current (uncommitted) write
** position in the WAL */
void __wal_savepoint(wal_t *wal, u_int32_t *wal_data);

/* Move the write position of the WAL back to i_frame.  Called in
** response to a ROLLBACK TO command. */
int __wal_savepoint_undo(wal_t *wal, u_int32_t *wal_data);

/* Write a frame or frames to the log. */
int __wal_frames(wal_t *wal, int, pg_hdr_t *, pgno_t, int, int);

/* Copy pages from the log to the database file */ 
int __wal_checkpoint(
  wal_t *wal,                      /* Write-ahead log connection */
  DBSQL *db,                    /* Check this handle's interrupt flag */
  int mode,                      /* One of PASSIVE, FULL and RESTART */
  int (*__x_busy)(void*),            /* Function to call when busy */
  void *busy_arg,                 /* Context argument for busy_handler */
  int sync_flags,                 /* Flags to sync db file with (or 0) */
  int n_buf,                       /* Size of buffer n_buf */
  u_int8_t *buf,                       /* Temporary buffer to use */
  int *pn_log,                     /* OUT: Number of frames in WAL */
  int *pn_ckpt                     /* OUT: Number of backfilled frames in WAL */
);

/* Return the value to pass to a dbsql_wal_hook callback, the
** number of frames in the WAL at the point of the last commit since
** __wal_callback() was called.  If no commits have occurred since
** the last call, then return 0.
*/
int __wal_callback(wal_t *wal);

/* Tell the wal layer that an EXCLUSIVE lock has been obtained (or released)
** by the pager layer on the database file.
*/
int __wal_exclusive_mode(wal_t *wal, int op);

/* Return true if the argument is non-NULL and the WAL module is using
** heap-memory for the wal-index. Otherwise, if the argument is NULL or the
** WAL module is using shared-memory, return false. 
*/
int __wal_heap_memory(wal_t *wal);

#ifdef DBSQL_ENABLE_SNAPSHOT
int __wal_snapshot_get(wal_t *wal, dbsql_snapshot_t **snapshot);
void __wal_snapshot_open(wal_t *wal, dbsql_snapshot_t *snapshot);
int __wal_snapshot_recover(wal_t *wal);
int __wal_snapshot_check(wal_t *wal, dbsql_snapshot_t *snapshot);
void __wal_snapshot_unlock(wal_t *wal);
#endif

#ifdef DBSQL_ENABLE_ZIPVFS
/* If the WAL file is not empty, return the number of bytes of content
** stored in each frame (i.e. the db page-size when the WAL was created).
*/
int __wal_framesize(wal_t *wal);
#endif

/* Return the dbsql_file_t object for the WAL file */
dbsql_file_t *__wal_file(wal_t *wal);

#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
int __wal_write_lock(wal_t *wal, int lock);
void __wal_db(wal_t *wal, DBSQL *db);
#endif

#ifdef DBSQL_USE_SEH
int __wal_system_errno(wal_t*);
#endif

#endif /* ifndef DBSQL_OMIT_WAL */
#endif /* DBSQL_WAL_H */
