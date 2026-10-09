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
** This header file defines the interface that the sqlite page cache
** subsystem.  The page cache subsystem reads and writes a file a page
** at a time and provides a journal for rollback.
*/

#ifndef DBSQL_PAGER_H
#define DBSQL_PAGER_H

/*
** Default maximum size for persistent journal files. A negative 
** value means no limit. This value may be overridden using the 
** __sm_pager_journal_size_limit() API. See also "PRAGMA journal_size_limit".
*/
#ifndef DBSQL_DEFAULT_JOURNAL_SIZE_LIMIT
  #define DBSQL_DEFAULT_JOURNAL_SIZE_LIMIT -1
#endif

/*
** The type used to represent a page number.  The first page in a file
** is called page 1.  0 is used to represent "not a page".
*/
typedef u_int32_t pgno_t;

/*
** Each open file is managed by a separate instance of the "pager_t" structure.
*/
typedef struct pager_t pager_t;

/*
** Handle type for pages.
*/
typedef struct pg_hdr_t db_page_t;

/*
** Page number PAGER_SJ_PGNO is never used in an dbsql database (it is
** reserved for working around a windows/posix incompatibility). It is
** used in the journal to signify that the remainder of the journal file 
** is devoted to storing a super-journal name - there are no more pages to
** roll back. See comments for function write_super_journal() in pager.c 
** for details.
*/
#define PAGER_SJ_PGNO_COMPUTED(x) ((pgno_t)((PENDING_BYTE/((x)->page_size))+1))
#define PAGER_SJ_PGNO(x)          ((x)->lck_pgno)

/*
** Allowed values for the flags parameter to __sm_pager_open().
**
** NOTE: These values must match the corresponding BTREE_ values in btree.h.
*/
#define PAGER_OMIT_JOURNAL  0x0001    /* Do not use a rollback journal */
#define PAGER_MEMORY        0x0002    /* In-memory database */

/*
** Valid values for the second argument to __sm_pager_locking_mode().
*/
#define PAGER_LOCKINGMODE_QUERY      -1
#define PAGER_LOCKINGMODE_NORMAL      0
#define PAGER_LOCKINGMODE_EXCLUSIVE   1

/*
** Numeric constants that encode the journalmode.
**
** The numeric values encoded here (other than PAGER_JOURNALMODE_QUERY)
** are exposed in the API via the "PRAGMA journal_mode" command and
** therefore cannot be changed without a compatibility break.
*/
#define PAGER_JOURNALMODE_QUERY     (-1)  /* Query the value of journalmode */
#define PAGER_JOURNALMODE_DELETE      0   /* Commit by deleting journal file */
#define PAGER_JOURNALMODE_PERSIST     1   /* Commit by zeroing journal header */
#define PAGER_JOURNALMODE_OFF         2   /* Journal omitted.  */
#define PAGER_JOURNALMODE_TRUNCATE    3   /* Commit by truncating journal */
#define PAGER_JOURNALMODE_MEMORY      4   /* In-memory journal file */
#define PAGER_JOURNALMODE_WAL         5   /* Use write-ahead logging */

#define IS_WAL_MODE(x) ((x)==PAGER_JOURNALMODE_WAL)

/*
** The argument to this macro is a file descriptor (type dbsql_file_t*).
** Return 0 if it is not open, or non-zero (but not 1) if it is.
**
** This is so that expressions can be written as:
**
**   if( isOpen(pager->jfd) ){ ...
**
** instead of
**
**   if( pager->jfd->methods ){ ...
*/
#define isOpen(fd) ((fd)->methods!=0)

/*
** Flags that make up the mask passed to __sm_pager_get().
*/
#define PAGER_GET_NOCONTENT     0x01  /* Do not load data from disk */
#define PAGER_GET_READONLY      0x02  /* Read-only page is acceptable */

/*
** Flags for __sm_pager_set_flags()
**
** Value constraints (enforced via assert()):
**    PAGER_FULLFSYNC      == DBSQL_FullFSync
**    PAGER_CKPT_FULLFSYNC == DBSQL_CkptFullFSync
**    PAGER_CACHE_SPILL    == DBSQL_CacheSpill
*/
#define PAGER_SYNCHRONOUS_OFF       0x01  /* PRAGMA synchronous=OFF */
#define PAGER_SYNCHRONOUS_NORMAL    0x02  /* PRAGMA synchronous=NORMAL */
#define PAGER_SYNCHRONOUS_FULL      0x03  /* PRAGMA synchronous=FULL */
#define PAGER_SYNCHRONOUS_EXTRA     0x04  /* PRAGMA synchronous=EXTRA */
#define PAGER_SYNCHRONOUS_MASK      0x07  /* Mask for four values above */
#define PAGER_FULLFSYNC             0x08  /* PRAGMA fullfsync=ON */
#define PAGER_CKPT_FULLFSYNC        0x10  /* PRAGMA checkpoint_fullfsync=ON */
#define PAGER_CACHESPILL            0x20  /* PRAGMA cache_spill=ON */
#define PAGER_FLAGS_MASK            0x38  /* All above except SYNCHRONOUS */

/*
** The remainder of this file contains the declarations of the functions
** that make up the pager_t sub-system API. See source code comments for 
** a detailed description of each routine.
*/

/* Open and close a pager_t connection. */ 
int __sm_pager_open(
  dbsql_vfs_t*,
  pager_t **pager,
  const char*,
  int,
  int,
  int,
  void(*)(db_page_t*)
);
int __sm_pager_close(pager_t *pager, DBSQL*);
int __sm_pager_read_fileheader(pager_t*, int, unsigned char*);

/* Functions used to configure a pager_t object. */
void __sm_pager_set_busy_handler(pager_t*, int(*)(void *), void *);
int __sm_pager_set_pagesize(pager_t*, u_int32_t*, int);
pgno_t __sm_pager_max_page_count(pager_t*, pgno_t);
void __sm_pager_set_cachesize(pager_t*, int);
int __sm_pager_set_spillsize(pager_t*, int);
void __sm_pager_set_mmap_limit(pager_t *, int64_t);
void __sm_pager_shrink(pager_t*);
void __sm_pager_set_flags(pager_t*,unsigned);
int __sm_pager_locking_mode(pager_t *, int);
int __sm_pager_set_journal_mode(pager_t *, int);
int __sm_pager_get_journal_mode(pager_t*);
int __sm_pager_ok_to_change_journal_mode(pager_t*);
int64_t __sm_pager_journal_size_limit(pager_t *, int64_t);
dbsql_backup_t **__sm_pager_backup_ptr(pager_t*);
int __sm_pager_flush(pager_t*);

/* Functions used to obtain and release page references. */ 
int __sm_pager_get(pager_t *pager, pgno_t pgno, db_page_t **page, int clr_flag);
db_page_t *__sm_pager_lookup(pager_t *pager, pgno_t pgno);
void __sm_pager_ref(db_page_t*);
void __sm_pager_unref(db_page_t*);
void __sm_pager_unref_not_null(db_page_t*);
void __sm_pager_unref_page_one(db_page_t*);

/* Operations on page references. */
int __sm_pager_write(db_page_t*);
void __sm_pager_dont_write(db_page_t*);
int __sm_pager_movepage(pager_t*,db_page_t*,pgno_t,int);
int __sm_pager_page_refcount(db_page_t*);
void *__sm_pager_get_data(db_page_t *); 
void *__sm_pager_get_extra(db_page_t *); 

/* Functions used to manage pager transactions and savepoints. */
void __sm_pager_pagecount(pager_t*, int*);
int __sm_pager_begin(pager_t*, int ex_flag, int);
int __sm_pager_commit_phase_one(pager_t*,const char *super, int);
int __sm_pager_exclusive_lock(pager_t*);
int __sm_pager_sync(pager_t *pager, const char *super);
int __sm_pager_commit_phase_two(pager_t*);
int __sm_pager_rollback(pager_t*);
int __sm_pager_open_savepoint(pager_t *pager, int n);
int __sm_pager_savepoint(pager_t *pager, int op, int i_savepoint);
int __sm_pager_shared_lock(pager_t *pager);

#ifndef DBSQL_OMIT_WAL
  int __sm_pager_checkpoint(pager_t *pager, DBSQL*, int, int*, int*);
  int __sm_pager_wal_supported(pager_t *pager);
  int __sm_pager_wal_callback(pager_t *pager);
  int __sm_pager_open_wal(pager_t *pager, int *pis_open);
  int __sm_pager_close_wal(pager_t *pager, DBSQL*);
# ifdef DBSQL_ENABLE_SNAPSHOT
  int __sm_pager_snapshot_get(pager_t*, dbsql_snapshot_t **snapshot);
  int __sm_pager_snapshot_open(pager_t*, dbsql_snapshot_t *snapshot);
  int __sm_pager_snapshot_recover(pager_t *pager);
  int __sm_pager_snapshot_check(pager_t *pager, dbsql_snapshot_t *snapshot);
  void __sm_pager_snapshot_unlock(pager_t *pager);
# endif
#endif

#if !defined(DBSQL_OMIT_WAL) && defined(DBSQL_ENABLE_SETLK_TIMEOUT)
  int __sm_pager_wal_write_lock(pager_t*, int);
  void __sm_pager_wal_db(pager_t*, DBSQL*);
#else
# define __sm_pager_wal_write_lock(y,z) DBSQL_OK
# define __sm_pager_wal_db(x,y)
#endif

#ifdef DBSQL_DIRECT_OVERFLOW_READ
  int __sm_pager_direct_read_ok(pager_t *pager, pgno_t pgno);
#endif

#ifdef DBSQL_ENABLE_ZIPVFS
  int __sm_pager_wal_framesize(pager_t *pager);
#endif

/* Functions used to query pager state and configuration. */
u_int8_t __sm_pager_isreadonly(pager_t*);
u_int32_t __sm_pager_data_version(pager_t*);
#ifdef DBSQL_DEBUG
  int __sm_pager_refcount(pager_t*);
#endif
int __sm_pager_mem_used(pager_t*);
const char *__sm_pager_filename(const pager_t*, int);
dbsql_vfs_t *__sm_pager_vfs(pager_t*);
dbsql_file_t *__sm_pager_file(pager_t*);
dbsql_file_t *__sm_pager_jrnl_file(pager_t*);
const char *__sm_pager_journalname(pager_t*);
void *__sm_pager_temp_space(pager_t*);
int __sm_pager_is_memdb(pager_t*);
void __sm_pager_cache_stat(pager_t *, int, int, u_int64_t*);
void __sm_pager_clear_cache(pager_t*);
int __sector_size(dbsql_file_t *);

/* Functions used to truncate the database file. */
void __sm_pager_truncate_image(pager_t*,pgno_t);

void __sm_pager_rekey(db_page_t*, pgno_t, u_int16_t);

/* Functions to support testing and debugging. */
#if !defined(NDEBUG) || defined(DBSQL_TEST)
  pgno_t __sm_pager_pagenumber(db_page_t*);
  int __sm_pager_iswriteable(db_page_t*);
#endif
#ifdef DBSQL_TEST
  int *__sm_pager_stats(pager_t*);
  void __sm_pager_refdump(pager_t*);
  void disable_simulated_io_errors(void);
  void enable_simulated_io_errors(void);
#else
# define disable_simulated_io_errors()
# define enable_simulated_io_errors()
#endif

#if defined(DBSQL_USE_SEH) && !defined(DBSQL_OMIT_WAL)
int __sm_pager_wal_system_errno(pager_t*);
#endif

#endif /* DBSQL_PAGER_H */
