/*
** 2008 August 05
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
** subsystem. 
*/

#ifndef _PCACHE_H_

typedef struct pg_hdr_t pg_hdr_t;
typedef struct p_cache_t p_cache_t;

/*
** Every page in the cache is controlled by an instance of the following
** structure.
*/
struct pg_hdr_t {
  dbsql_pcache_page_t *page;    /* Pcache object page handle */
  void *data;                   /* Page data */
  void *extra;                  /* Extra content */
  p_cache_t *cache;                /* PRIVATE: Cache that owns this page */
  pg_hdr_t *dirty;                 /* Transient list of dirty sorted by pgno */
  pager_t *pager;                 /* The pager this page is part of */
#ifdef DBSQL_CHECK_PAGES
  u_int64_t page_hash;                  /* hash_t of page content */
#endif
  pgno_t pgno;                     /* Page number for this page */
  u_int16_t flags;                     /* PGHDR flags defined below */

  /**********************************************************************
  ** Elements above, except cache, are public.  All that follow are 
  ** private to pcache.c and should not be accessed by other modules.
  ** cache is grouped with the public elements for efficiency.
  */
  int64_t n_ref;                      /* Number of users of this page */
  pg_hdr_t *dirty_next;             /* Next element in list of dirty pages */
  pg_hdr_t *dirty_prev;             /* Previous element in list of dirty pages */
                          /* NB: dirty_next and dirty_prev are undefined if the
                          ** pg_hdr_t object is not dirty */
};

/* Bit values for pg_hdr_t.flags */
#define PGHDR_CLEAN           0x001  /* Page not on the p_cache_t.dirty list */
#define PGHDR_DIRTY           0x002  /* Page is on the p_cache_t.dirty list */
#define PGHDR_WRITEABLE       0x004  /* Journaled and ready to modify */
#define PGHDR_NEED_SYNC       0x008  /* Fsync the rollback journal before
                                     ** writing this page to the database */
#define PGHDR_DONT_WRITE      0x010  /* Do not write content to disk */
#define PGHDR_MMAP            0x020  /* This is an mmap page object */

#define PGHDR_WAL_APPEND      0x040  /* Appended to wal file */

/* Initialize and shutdown the page cache subsystem */
int __pcache_initialize(void);
void __pcache_shutdown(void);

/* Page cache buffer management:
** These routines implement DBSQL_CONFIG_PAGECACHE.
*/
void __p_cache_buffer_setup(void *, int sz, int n);

/* Create a new pager cache.
** Under memory stress, invoke stress to try to make pages clean.
** Only clean and unpinned pages can be reclaimed.
*/
int __pcache_open(
  int sz_page,                    /* Size of every page */
  int sz_extra,                   /* Extra space associated with each page */
  int purgeable,                /* True if pages are on backing store */
  int (*x_stress)(void*, pg_hdr_t*), /* Call to try to make pages clean */
  void *stress,                 /* Argument to stress */
  p_cache_t *to_init                /* Preallocated space for the p_cache_t */
);

/* Modify the page-size after the cache has been created. */
int __pcache_set_page_size(p_cache_t *, int);

/* Return the size in bytes of a p_cache_t object.  Used to preallocate
** storage space.
*/
int __pcache_size(void);

/* One release per successful fetch.  Page is pinned until released.
** Reference counted. 
*/
dbsql_pcache_page_t *__pcache_fetch(p_cache_t*, pgno_t, int create_flag);
int __pcache_fetch_stress(p_cache_t*, pgno_t, dbsql_pcache_page_t**);
pg_hdr_t *__pcache_fetch_finish(p_cache_t*, pgno_t, dbsql_pcache_page_t *page);
void __pcache_release(pg_hdr_t*);

void __pcache_drop(pg_hdr_t*);         /* Remove page from cache */
void __pcache_make_dirty(pg_hdr_t*);    /* Make sure page is marked dirty */
void __pcache_make_clean(pg_hdr_t*);    /* Mark a single page as clean */
void __pcache_clean_all(p_cache_t*);    /* Mark all dirty list pages as clean */
void __pcache_clear_writable(p_cache_t*);

/* Change a page number.  Used by incr-vacuum. */
void __pcache_move(pg_hdr_t*, pgno_t);

/* Remove all pages with pgno>x.  Reset the cache if x==0 */
void __pcache_truncate(p_cache_t*, pgno_t x);

/* Get a list of all dirty pages in the cache, sorted by page number */
pg_hdr_t *__pcache_dirty_list(p_cache_t*);

/* Reset and close the cache object */
void __pcache_close(p_cache_t*);

/* Clear flags from pages of the page cache */
void __pcache_clear_sync_flags(p_cache_t *);

/* Discard the contents of the cache */
void __pcache_clear(p_cache_t*);

/* Return the total number of outstanding page references */
int64_t __pcache_ref_count(p_cache_t*);

/* Increment the reference count of an existing page */
void __pcache_ref(pg_hdr_t*);

int64_t __pcache_page_refcount(pg_hdr_t*);

/* Return the total number of pages stored in the cache */
int __pcache_pagecount(p_cache_t*);

#if defined(DBSQL_CHECK_PAGES) || defined(DBSQL_DEBUG)
/* Iterate through all dirty pages currently stored in the cache. This
** interface is only available if DBSQL_CHECK_PAGES is defined when the 
** library is built.
*/
void __pcache_iterate_dirty(p_cache_t *cache, void (*iter)(pg_hdr_t *));
#endif

#if defined(DBSQL_DEBUG)
/* Check invariants on a pg_hdr_t object */
int __pcache_page_sanity(pg_hdr_t*);
#endif

/* Set and get the suggested cache-size for the specified pager-cache.
**
** If no global maximum is configured, then the system attempts to limit
** the total number of pages cached by purgeable pager-caches to the sum
** of the suggested cache-sizes.
*/
void __pcache_set_cachesize(p_cache_t *, int);
#ifdef DBSQL_TEST
int __pcache_get_cachesize(p_cache_t *);
#endif

/* Set or get the suggested spill-size for the specified pager-cache.
**
** The spill-size is the minimum number of pages in cache before the cache
** will attempt to spill dirty pages by calling stress.
*/
int __pcache_set_spillsize(p_cache_t *, int);

/* Free up as much memory as possible from the page cache */
void __pcache_shrink(p_cache_t*);

#ifdef DBSQL_ENABLE_MEMORY_MANAGEMENT
/* Try to return memory used by the pcache module to the main memory heap */
int __pcache_release_memory(int);
#endif

#ifdef DBSQL_TEST
void __pcache_stats(int*,int*,int*,int*);
#endif

void __p_cache_set_default(void);

/* Return the header size */
int __header_size_pcache(void);
int __header_size_pcache1(void);

/* Number of dirty pages as a percentage of the configured cache size */
int __p_cache_percent_dirty(p_cache_t*);

#ifdef DBSQL_DIRECT_OVERFLOW_READ
int __p_cache_is_dirty(p_cache_t *cache);
#endif

#endif /* _PCACHE_H_ */
