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
** This header file defines the interface that the sqlite B-Tree file
** subsystem.  See comments in the source code for a detailed description
** of what each interface routine does.
*/
#ifndef DBSQL_BTREE_H
#define DBSQL_BTREE_H

/* TODO: This definition is just included so other modules compile. It
** needs to be revisited.
*/
#define DBSQL_N_BTREE_META 16

/*
** If defined as non-zero, auto-vacuum is enabled by default. Otherwise
** it must be turned on for each database using "PRAGMA auto_vacuum = 1".
*/
#ifndef DBSQL_DEFAULT_AUTOVACUUM
  #define DBSQL_DEFAULT_AUTOVACUUM 0
#endif

#define BTREE_AUTOVACUUM_NONE 0        /* Do not do auto-vacuum */
#define BTREE_AUTOVACUUM_FULL 1        /* Do full auto-vacuum */
#define BTREE_AUTOVACUUM_INCR 2        /* Incremental vacuum */

/*
** Forward declarations of structure
*/
typedef struct sm_t sm_t;
typedef struct sm_cursor_t sm_cursor_t;
typedef struct sm_shared_t sm_shared_t;
typedef struct sm_payload_t sm_payload_t;


int __sm_open(
  dbsql_vfs_t *vfs,       /* VFS to use with this b-tree */
  const char *filename,   /* Name of database file to open */
  DBSQL *db,             /* Associated database connection */
  sm_t **btree,         /* Return open sm_t* here */
  int flags,               /* Flags */
  int vfs_flags             /* Flags passed through to VFS open */
);

/* The flags parameter to __sm_open can be the bitwise or of the
** following values.
**
** NOTE:  These values must match the corresponding PAGER_ values in
** pager.h.
*/
#define BTREE_OMIT_JOURNAL  1  /* Do not create or use a rollback journal */
#define BTREE_MEMORY        2  /* This is an in-memory DB */
#define BTREE_SINGLE        4  /* The file contains at most 1 b-tree */
#define BTREE_UNORDERED     8  /* Use of a hash implementation is OK */

int __sm_close(sm_t*);
int __sm_set_cache_size(sm_t*,int);
int __sm_set_spill_size(sm_t*,int);
#if DBSQL_MAX_MMAP_SIZE>0
  int __sm_set_mmap_limit(sm_t*,int64_t);
#endif
int __sm_set_pager_flags(sm_t*,unsigned);
int __sm_set_page_size(sm_t *p, int n_pagesize, int n_reserve, int fix);
int __sm_get_page_size(sm_t*);
pgno_t __sm_max_page_count(sm_t*,pgno_t);
pgno_t __sm_last_page(sm_t*);
int __sm_secure_delete(sm_t*,int);
int __sm_get_requested_reserve(sm_t*);
int __sm_get_reserve_no_mutex(sm_t *p);
int __sm_set_auto_vacuum(sm_t *, int);
int __sm_get_auto_vacuum(sm_t *);
int __sm_begin_trans(sm_t*,int,int*);
int __sm_commit_phase_one(sm_t*, const char*);
int __sm_commit_phase_two(sm_t*, int);
int __sm_commit(sm_t*);
int __sm_rollback(sm_t*,int,int);
int __sm_begin_stmt(sm_t*,int);
int __sm_create_table(sm_t*, pgno_t*, int flags);
int __sm_txn_state(sm_t*);
int __sm_is_in_backup(sm_t*);

void *__sm_schema(sm_t *, int, void(*)(void *));
int __sm_schema_locked(sm_t *btree);
#ifndef DBSQL_OMIT_SHARED_CACHE
int __sm_lock_table(sm_t *btree, int i_tab, u_int8_t is_write_lock);
#endif

/* Savepoints are named, nestable SQL transactions mostly implemented */ 
/* in vdbe.c and pager.c See https://sqlite.org/lang_savepoint.html */
int __sm_savepoint(sm_t *, int, int);

/* "Checkpoint" only refers to WAL. See https://sqlite.org/wal.html#ckpt */
#ifndef DBSQL_OMIT_WAL
  int __sm_checkpoint(sm_t*, int, int *, int *);  
#endif

const char *__sm_get_filename(sm_t *);
const char *__sm_get_journalname(sm_t *);
int __sm_copy_file(sm_t*, sm_t*);

int __sm_incr_vacuum(sm_t *);

/* The flags parameter to __sm_create_table can be the bitwise OR
** of the flags shown below.
**
** Every dbsql table must have either BTREE_INTKEY or BTREE_BLOBKEY set.
** with_t BTREE_INTKEY, the table key is a 64-bit integer and arbitrary data
** is stored in the leaves.  (BTREE_INTKEY is used for SQL tables.)  with_t
** BTREE_BLOBKEY, the key is an arbitrary BLOB and no content is stored
** anywhere - the key is the content.  (BTREE_BLOBKEY is used for SQL
** indices.)
*/
#define BTREE_INTKEY     1    /* table_t has only 64-bit signed integer keys */
#define BTREE_BLOBKEY    2    /* table_t has keys only - no data */

int __sm_drop_table(sm_t*, int, int*);
int __sm_clear_table(sm_t*, int, int64_t*);
int __sm_clear_table_of_cursor(sm_cursor_t*);
int __sm_trip_all_cursors(sm_t*, int, int);

void __sm_get_meta(sm_t *btree, int idx, u_int32_t *value);
int __sm_update_meta(sm_t*, int idx, u_int32_t value);

int __sm_new_db(sm_t *p);

/*
** The second parameter to __sm_get_meta or __sm_update_meta
** should be one of the following values. The integer values are assigned 
** to constants so that the offset of the corresponding field in an
** dbsql database header may be found using the following formula:
**
**   offset = 36 + (idx * 4)
**
** For example, the free-page-count field is located at byte offset 36 of
** the database file header. The incr-vacuum-flag field is located at
** byte offset 64 (== 36+4*7).
**
** The BTREE_DATA_VERSION value is not really a value stored in the header.
** It is a read-only number computed by the pager.  But we merge it with
** the header value access routines since its access pattern is the same.
** Call it a "virtual meta value".
*/
#define BTREE_FREE_PAGE_COUNT     0
#define BTREE_SCHEMA_VERSION      1
#define BTREE_FILE_FORMAT         2
#define BTREE_DEFAULT_CACHE_SIZE  3
#define BTREE_LARGEST_ROOT_PAGE   4
#define BTREE_TEXT_ENCODING       5
#define BTREE_USER_VERSION        6
#define BTREE_INCR_VACUUM         7
#define BTREE_APPLICATION_ID      8
#define BTREE_DATA_VERSION        15  /* A virtual meta-value */

/*
** Kinds of hints that can be passed into the __sm_cursor_hint()
** interface.
**
** BTREE_HINT_RANGE  (arguments: expr_t*, mem_t*)
**
**     The first argument is an expr_t* (which is guaranteed to be constant for
**     the lifetime of the cursor) that defines constraints on which rows
**     might be fetched with this cursor.  The expr_t* tree may contain
**     TK_REGISTER nodes that refer to values stored in the array of registers
**     passed as the second parameter.  In other words, if expr_t.op==TK_REGISTER
**     then the value of the node is the value in mem_t[expr.i_table].  Any
**     TK_COLUMN node in the expression tree refers to the expr_t.i_column-th
**     column of the b-tree of the cursor.  The expr_t tree will not contain
**     any function calls nor subqueries nor references to b-trees other than
**     the cursor being hinted.
**
**     The design of the _RANGE hint is aid b-tree implementations that try
**     to prefetch content from remote machines - to provide those
**     implementations with limits on what needs to be prefetched and thereby
**     reduce network bandwidth.
**
** Note that BTREE_HINT_FLAGS with BTREE_BULKLOAD is the only hint used by
** standard dbsql.  The other hints are provided for extensions that use
** the dbsql parser and code generator but substitute their own storage
** engine.
*/
#define BTREE_HINT_RANGE 0       /* Range constraints on queries */

/*
** Values that may be OR'd together to form the argument to the
** BTREE_HINT_FLAGS hint for __sm_cursor_hint():
**
** The BTREE_BULKLOAD flag is set on index cursors when the index is going
** to be filled with content that is already in sorted order.
**
** The BTREE_SEEK_EQ flag is set on cursors that will get OP_SeekGE or
** OP_SeekLE opcodes for a range search, but where the range of entries
** selected will all have the same key.  In other words, the cursor will
** be used only for equality key searches.
**
*/
#define BTREE_BULKLOAD 0x00000001  /* Used to full index in sorted order */
#define BTREE_SEEK_EQ  0x00000002  /* EQ seeks only - no range seeks */

/* 
** Flags passed as the third argument to __sm_cursor().
**
** For read-only cursors the wr_flag argument is always zero. For read-write
** cursors it may be set to either (BTREE_WRCSR|BTREE_FORDELETE) or just
** (BTREE_WRCSR). If the BTREE_FORDELETE bit is set, then the cursor will
** only be used by dbsql for the following:
**
**   * to seek to and then delete specific entries, and/or
**
**   * to read values that will be used to create keys that other
**     BTREE_FORDELETE cursors will seek to and delete.
**
** The BTREE_FORDELETE flag is an optimization hint.  It is not used by
** by this, the native b-tree engine of dbsql, but it is available to
** alternative storage engines that might be substituted in place of this
** b-tree system.  For alternative storage engines in which a delete of
** the main table row automatically deletes corresponding index rows,
** the FORDELETE flag hint allows those alternative storage engines to
** skip a lot of work.  Namely:  FORDELETE cursors may treat all SEEK
** and DELETE operations as no-ops, and any READ operation against a
** FORDELETE cursor may return a null row: 0x01 0x00.
*/
#define BTREE_WRCSR     0x00000004     /* read-write cursor */
#define BTREE_FORDELETE 0x00000008     /* Cursor is for seek/delete only */

int __sm_cursor(
  sm_t*,                              /* BTree containing table to open */
  pgno_t i_table,                         /* index_t of root page */
  int wr_flag,                          /* 1 for writing.  0 for read-only */
  struct key_info_t*,                     /* First argument to compare function */
  sm_cursor_t *cursor                    /* Space to write cursor structure */
);
sm_cursor_t *__sm_fake_valid_cursor(void);
int __sm_cursor_size(void);
#ifdef DBSQL_DEBUG
int __sm_closes_with_cursor(sm_t*,sm_cursor_t*);
#endif
void __sm_cursor_zero(sm_cursor_t*);
void __sm_cursor_hint_flags(sm_cursor_t*, unsigned);
#ifdef DBSQL_ENABLE_CURSOR_HINTS
void __sm_cursor_hint(sm_cursor_t*, int, ...);
#endif

int __sm_close_cursor(sm_cursor_t*);
int __sm_table_moveto(
  sm_cursor_t*,
  int64_t int_key,
  int bias,
  int *res
);
int __sm_index_moveto(
  sm_cursor_t*,
  unpacked_record_t *un_key,
  int *res
);
int __sm_cursor_has_moved(sm_cursor_t*);
int __sm_cursor_restore(sm_cursor_t*, int*);
int __sm_delete(sm_cursor_t*, u_int8_t flags);

/* Allowed flags for __sm_delete() and __sm_insert() */
#define BTREE_SAVEPOSITION 0x02  /* Leave cursor pointing at NEXT or PREV */
#define BTREE_AUXDELETE    0x04  /* not the primary delete operation */
#define BTREE_APPEND       0x08  /* Insert is likely an append */
#define BTREE_PREFORMAT    0x80  /* Inserted data is a preformated cell */

/* An instance of the sm_payload_t object describes the content of a single
** entry in either an index or table btree.
**
** index_t btrees (used for indexes and also WITHOUT ROWID tables) contain
** an arbitrary key and no data.  These btrees have key,n_key set to the
** key and the data,n_data,n_zero fields are uninitialized.  The mems,n_mem
** fields give an array of mem_t objects that are a decomposition of the key.
** The n_mem field might be zero, indicating that no decomposition is available.
**
** table_t btrees (used for rowid tables) contain an integer rowid used as
** the key and passed in the n_key field.  The key field is zero.  
** data,n_data hold the content of the new entry.  n_zero extra zero bytes
** are appended to the end of the content when constructing the entry.
** The mems,n_mem fields are uninitialized for table btrees.
**
** Field usage summary:
**
**               table_t BTrees                   index_t Btrees
**
**   key        always NULL                    encoded key
**   n_key        the ROWID                      length of key
**   data       data                           not used
**   mems        not used                       decomposed key value
**   n_mem        not used                       entries in mems
**   n_data       length of data                not used
**   n_zero       extra zeros after data        not used
**
** This object is used to pass information into __sm_insert().  The
** same information used to be passed as five separate parameters.  But placing
** the information into this object helps to keep the interface more 
** organized and understandable, and it also helps the resulting code to
** run a little faster by using fewer registers for parameter passing.
*/
struct sm_payload_t {
  const void *key;       /* Key content for indexes.  NULL for tables */
  int64_t n_key;     /* Size of key for indexes.  PRIMARY KEY for tabs */
  const void *data;      /* Data for tables. */
  dbsql_value_t *mems;    /* First of n_mem value in the unpacked key */
  u_int16_t n_mem;               /* Number of mems[] value.  Might be zero */
  int n_data;              /* Size of data.  0 if none. */
  int n_zero;              /* Extra zero data appended after data,n_data */
};

int __sm_insert(sm_cursor_t*, const sm_payload_t *payload,
                       int flags, int seek_result);
int __sm_first(sm_cursor_t*, int *res);
int __sm_is_empty(sm_cursor_t *cur, int *res);
int __sm_last(sm_cursor_t*, int *res);
int __sm_next(sm_cursor_t*, int flags);
int __sm_eof(sm_cursor_t*);
int __sm_previous(sm_cursor_t*, int flags);
int64_t __sm_integer_key(sm_cursor_t*);
void __sm_cursor_pin(sm_cursor_t*);
void __sm_cursor_unpin(sm_cursor_t*);
int64_t __sm_offset(sm_cursor_t*);
int __sm_payload(sm_cursor_t*, u_int32_t offset, u_int32_t amt, void*);
const void *__sm_payload_fetch(sm_cursor_t*, u_int32_t *amt);
u_int32_t __sm_payload_size(sm_cursor_t*);
int64_t __sm_max_record_size(sm_cursor_t*);

int __sm_integrity_check(
  DBSQL *db,  /* Database connection that is running the check */
  sm_t *p,     /* The btree to be checked */
  pgno_t *roots,  /* An array of root pages numbers for individual trees */
  dbsql_value_t *cnts,  /* OUT: entry counts for each btree in roots[] */
  int n_root,    /* Number of entries in roots[] */
  int mx_err,    /* Stop reporting errors after this many */
  int *pn_err,   /* OUT: Write number of errors seen to this variable */
  char **out  /* OUT: Write the error message string here */
);
struct pager_t *__sm_pager(sm_t*);
int64_t __sm_row_count_est(sm_cursor_t*);

#ifndef DBSQL_OMIT_INCRBLOB
int __sm_payload_checked(sm_cursor_t*, u_int32_t offset, u_int32_t amt, void*);
int __sm_put_data(sm_cursor_t*, u_int32_t offset, u_int32_t amt, void*);
void __sm_incrblob_cursor(sm_cursor_t *);
#endif
void __sm_clear_cursor(sm_cursor_t *);
int __sm_set_version(sm_t *bt, int i_version);
int __sm_cursor_has_hint(sm_cursor_t*, unsigned int mask);
int __sm_is_readonly(sm_t *bt);
int __header_size_btree(void);

#ifdef DBSQL_DEBUG
u_int64_t __sm_seek_count(sm_t*);
#else
# define __sm_seek_count(X) 0
#endif

#ifndef NDEBUG
int __sm_cursor_is_valid(sm_cursor_t*);
#endif
int __sm_cursor_is_valid_nn(sm_cursor_t*);

int __sm_count(DBSQL*, sm_cursor_t*, int64_t*);

#ifdef DBSQL_TEST
int __sm_cursor_info(sm_cursor_t*, int*, int);
void __sm_cursor_list(sm_t*);
#endif

#ifndef DBSQL_OMIT_WAL
  int __sm_checkpoint(sm_t*, int, int *, int *);
#endif

int __sm_transfer_row(sm_cursor_t*, sm_cursor_t*, int64_t);

void __sm_clear_cache(sm_t*);

/*
** If we are not using shared cache, then there is no need to
** use mutexes to access the sm_shared_t structures.  So make the
** Enter and Leave procedures no-ops.
*/
#ifndef DBSQL_OMIT_SHARED_CACHE
  void __sm_enter(sm_t*);
  void __sm_enter_all(DBSQL*);
  int __sm_sharable(sm_t*);
  void __sm_enter_cursor(sm_cursor_t*);
  int __sm_connection_count(sm_t*);
#else
# define __sm_enter(X) 
# define __sm_enter_all(X)
# define __sm_sharable(X) 0
# define __sm_enter_cursor(X)
# define __sm_connection_count(X) 1
#endif

#if !defined(DBSQL_OMIT_SHARED_CACHE) && DBSQL_THREADSAFE
  void __sm_leave(sm_t*);
  void __sm_leave_cursor(sm_cursor_t*);
  void __sm_leave_all(DBSQL*);
#ifndef NDEBUG
  /* These routines are used inside assert() statements only. */
  int __sm_holds_mutex(sm_t*);
  int __sm_holds_all_mutexes(DBSQL*);
  int __schema_mutex_held(DBSQL*,int,schema_t*);
#endif
#else

# define __sm_leave(X)
# define __sm_leave_cursor(X)
# define __sm_leave_all(X)

# define __sm_holds_mutex(X) 1
# define __sm_holds_all_mutexes(X) 1
# define __schema_mutex_held(X,Y,Z) 1
#endif


#endif /* DBSQL_BTREE_H */
