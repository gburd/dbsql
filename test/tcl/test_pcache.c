/*
** 2008 November 18
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
** This file contains code used for testing the dbsql system.
** None of the code in this file goes into a deliverable build.
**
** This file contains an application-defined pager cache
** implementation that can be plugged in in place of the
** default pcache.  This alternative pager cache will throw
** some errors that the default cache does not.
**
** This pagecache implementation is designed for simplicity
** not speed.
*/
#include "dbsql.h"
#include <string.h>
#include <assert.h>

/*
** Global data used by this test implementation.  There is no
** mutexing, which means this page cache will not work in a
** multi-threaded test.
*/
typedef struct testpcache_global_type testpcache_global_type;
struct testpcache_global_type {
  void *dummy;             /* Dummy allocation to simulate failures */
  int n_instance;            /* Number of current instances */
  unsigned discard_chance;   /* Chance of discarding on an unpin (0-100) */
  unsigned __prng_seed;        /* Seed for the PRNG */
  unsigned high_stress;      /* Call stress aggressively */
};
static testpcache_global_type __testpcache_global;

/*
** Initializer.
**
** Verify that the initializer is only called when the system is
** uninitialized.  Allocate some memory and report DBSQL_NOMEM if
** the allocation fails.  This provides a means to test the recovery
** from a failed initialization attempt.  It also verifies that the
** the destructor always gets call - otherwise there would be a
** memory leak.
*/
static int __testpcache_init(void *arg){
  assert( arg==(void*)&__testpcache_global );
  assert( __testpcache_global.dummy==0 );
  assert( __testpcache_global.n_instance==0 );
  __testpcache_global.dummy = dbsql_malloc(10);
  return __testpcache_global.dummy==0 ? DBSQL_NOMEM : DBSQL_OK;
}

/*
** Destructor
**
** Verify that this is only called after initialization.
** Free the memory allocated by the initializer.
*/
static void __testpcache_shutdown(void *arg){
  assert( arg==(void*)&__testpcache_global );
  assert( __testpcache_global.dummy!=0 );
  assert( __testpcache_global.n_instance==0 );
  dbsql_free( __testpcache_global.dummy );
  __testpcache_global.dummy = 0;
}

/*
** Number of pages in a cache.
**
** The number of pages is a hard upper bound in this test module.
** If more pages are requested, __pcache_fetch() returns NULL.
**
** If testing with in-memory temp tables, provide a larger pcache.
** Some of the test cases need this.
*/
#if defined(DBSQL_TEMP_STORE) && DBSQL_TEMP_STORE>=2
# define TESTPCACHE_NPAGE    499
#else
# define TESTPCACHE_NPAGE    217
#endif
#define TESTPCACHE_RESERVE   17

/*
** Magic numbers used to determine validity of the page cache.
*/
#define TESTPCACHE_VALID  0x364585fd
#define TESTPCACHE_CLEAR  0xd42670d4

/*
** Private implementation of a page cache.
*/
typedef struct testpcache testpcache;
struct testpcache {
  int64_t sz_page;     /* Size of each page.  Multiple of 8. */
  int sz_extra;              /* Size of extra data that accompanies each page */
  int purgeable;           /* True if the page cache is purgeable */
  int n_free;                /* Number of unused slots in a[] */
  int n_pinned;              /* Number of pinned slots in a[] */
  unsigned i_rand;           /* State of the PRNG */
  unsigned i_magic;          /* Magic number for sanity checking */
  struct testpcache_page {
    dbsql_pcache_page_t page;  /* Base class */
    unsigned key;              /* The key for this page. 0 means unallocated */
    int is_pinned;              /* True if the page is pinned */
  } a[TESTPCACHE_NPAGE];    /* All pages in the cache */
};

/*
** Get a random number using the PRNG in the given page cache.
*/
static unsigned __testpcache_random(testpcache *p){
  unsigned x = 0;
  int i;
  for(i=0; i<4; i++){
    p->i_rand = (p->i_rand*69069 + 5);
    x = (x<<8) | ((p->i_rand>>16)&0xff);
  }
  return x;
}


/*
** Allocate a new page cache instance.
*/
static dbsql_pcache_t *__testpcache_create(
  int sz_page,
  int sz_extra,
  int purgeable
){
  int n_mem;
  char *x;
  testpcache *p;
  int i;
  assert( __testpcache_global.dummy!=0 );
  sz_page = (sz_page+7)&~7;
  sz_extra = (sz_page+7)&~7;
  n_mem = sizeof(testpcache) + TESTPCACHE_NPAGE*(sz_page+sz_extra);
  p = dbsql_malloc( n_mem );
  if( p==0 ) return 0;
  x = (char*)&p[1];
  p->sz_page = sz_page;
  p->sz_extra = sz_extra;
  p->n_free = TESTPCACHE_NPAGE;
  p->n_pinned = 0;
  p->i_rand = __testpcache_global.__prng_seed;
  p->purgeable = purgeable;
  p->i_magic = TESTPCACHE_VALID;
  for(i=0; i<TESTPCACHE_NPAGE; i++, x += (sz_page+sz_extra)){
    p->a[i].key = 0;
    p->a[i].is_pinned = 0;
    p->a[i].page.buf = (void*)x;
    p->a[i].page.extra = (void*)&x[sz_page];
  }
  __testpcache_global.n_instance++;
  return (dbsql_pcache_t*)p;
}

/*
** Set the cache size
*/
static void __testpcache_cachesize(dbsql_pcache_t *cache, int newSize){
  testpcache *p = (testpcache*)cache;
  assert( p->i_magic==TESTPCACHE_VALID );
  assert( __testpcache_global.dummy!=0 );
  assert( __testpcache_global.n_instance>0 );
}

/*
** Return the number of pages in the cache that are being used.
** This includes both pinned and unpinned pages.
*/
static int __testpcache_pagecount(dbsql_pcache_t *cache){
  testpcache *p = (testpcache*)cache;
  assert( p->i_magic==TESTPCACHE_VALID );
  assert( __testpcache_global.dummy!=0 );
  assert( __testpcache_global.n_instance>0 );
  return TESTPCACHE_NPAGE - p->n_free;
}

/*
** Fetch a page.
*/
static dbsql_pcache_page_t *__testpcache_fetch(
  dbsql_pcache_t *cache,
  unsigned key,
  int create_flag
){
  testpcache *p = (testpcache*)cache;
  int i, j;
  assert( p->i_magic==TESTPCACHE_VALID );
  assert( __testpcache_global.dummy!=0 );
  assert( __testpcache_global.n_instance>0 );

  /* See if the page is already in cache.  Return immediately if it is */
  for(i=0; i<TESTPCACHE_NPAGE; i++){
    if( p->a[i].key==key ){
      if( !p->a[i].is_pinned ){
        p->n_pinned++;
        assert( p->n_pinned <= TESTPCACHE_NPAGE - p->n_free );
        p->a[i].is_pinned = 1;
      }
      return &p->a[i].page;
    }
  }

  /* If create_flag is 0, never allocate a new page */
  if( create_flag==0 ){
    return 0;
  }

  /* If no pages are available, always fail */
  if( p->n_pinned==TESTPCACHE_NPAGE ){
    return 0;
  }

  /* Do not allocate the last TESTPCACHE_RESERVE pages unless create_flag is 2 */
  if( p->n_pinned>=TESTPCACHE_NPAGE-TESTPCACHE_RESERVE && create_flag<2 ){
    return 0;
  }

  /* Do not allocate if high_stress is enabled and create_flag is not 2.
  **
  ** The high_stress setting causes pager_stress() to be called much more
  ** often, which exercises the pager logic more intensely.
  */
  if( __testpcache_global.high_stress && create_flag<2 ){
    return 0;
  }

  /* Find a free page to allocate if there are any free pages.
  ** Withhold TESTPCACHE_RESERVE free pages until create_flag is 2.
  */
  if( p->n_free>TESTPCACHE_RESERVE || (create_flag==2 && p->n_free>0) ){
    j = __testpcache_random(p) % TESTPCACHE_NPAGE;
    for(i=0; i<TESTPCACHE_NPAGE; i++, j = (j+1)%TESTPCACHE_NPAGE){
      if( p->a[j].key==0 ){
        p->a[j].key = key;
        p->a[j].is_pinned = 1;
        memset(p->a[j].page.buf, 0, p->sz_page);
        memset(p->a[j].page.extra, 0, p->sz_extra);
        p->n_pinned++;
        p->n_free--;
        assert( p->n_pinned <= TESTPCACHE_NPAGE - p->n_free );
        return &p->a[j].page;
      }
    }

    /* The prior loop always finds a freepage to allocate */
    assert( 0 );
  }

  /* If this cache is not purgeable then we have to fail.
  */
  if( p->purgeable==0 ){
    return 0;
  }

  /* If there are no free pages, recycle a page.  The page to
  ** recycle is selected at random from all unpinned pages.
  */
  j = __testpcache_random(p) % TESTPCACHE_NPAGE;
  for(i=0; i<TESTPCACHE_NPAGE; i++, j = (j+1)%TESTPCACHE_NPAGE){
    if( p->a[j].key>0 && p->a[j].is_pinned==0 ){
      p->a[j].key = key;
      p->a[j].is_pinned = 1;
      memset(p->a[j].page.buf, 0, p->sz_page);
      memset(p->a[j].page.extra, 0, p->sz_extra);
      p->n_pinned++;
      assert( p->n_pinned <= TESTPCACHE_NPAGE - p->n_free );
      return &p->a[j].page;
    }
  }

  /* The previous loop always finds a page to recycle. */
  assert(0);
  return 0;
}

/*
** Unpin a page.
*/
static void __testpcache_unpin(
  dbsql_pcache_t *cache,
  dbsql_pcache_page_t *old_page,
  int discard
){
  testpcache *p = (testpcache*)cache;
  int i;
  assert( p->i_magic==TESTPCACHE_VALID );
  assert( __testpcache_global.dummy!=0 );
  assert( __testpcache_global.n_instance>0 );

  /* Randomly discard pages as they are unpinned according to the
  ** discard_chance setting.  If discard_chance is 0, the random discard
  ** never happens.  If discard_chance is 100, it always happens.
  */
  if( p->purgeable
  && (100-__testpcache_global.discard_chance) <= (__testpcache_random(p)%100)
  ){
    discard = 1;
  }

  for(i=0; i<TESTPCACHE_NPAGE; i++){
    if( &p->a[i].page==old_page ){
      /* The old_page pointer always points to a pinned page */
      assert( p->a[i].is_pinned );
      p->a[i].is_pinned = 0;
      p->n_pinned--;
      assert( p->n_pinned>=0 );
      if( discard ){
        p->a[i].key = 0;
        p->n_free++;
        assert( p->n_free<=TESTPCACHE_NPAGE );
      }
      return;
    }
  }

  /* The old_page pointer always points to a valid page */
  assert( 0 );
}


/*
** Rekey a single page.
*/
static void __testpcache_rekey(
  dbsql_pcache_t *cache,
  dbsql_pcache_page_t *old_page,
  unsigned old_key,
  unsigned new_key
){
  testpcache *p = (testpcache*)cache;
  int i;
  assert( p->i_magic==TESTPCACHE_VALID );
  assert( __testpcache_global.dummy!=0 );
  assert( __testpcache_global.n_instance>0 );

  /* If there already exists another page at new_key, verify that
  ** the other page is unpinned and discard it.
  */
  for(i=0; i<TESTPCACHE_NPAGE; i++){
    if( p->a[i].key==new_key ){
      /* The new key is never a page that is already pinned */
      assert( p->a[i].is_pinned==0 );
      p->a[i].key = 0;
      p->n_free++;
      assert( p->n_free<=TESTPCACHE_NPAGE );
      break;
    }
  }

  /* Find the page to be rekeyed and rekey it.
  */
  for(i=0; i<TESTPCACHE_NPAGE; i++){
    if( p->a[i].key==old_key ){
      /* The old_key and old_page parameters match */
      assert( &p->a[i].page==old_page );
      /* Page to be rekeyed must be pinned */
      assert( p->a[i].is_pinned );
      p->a[i].key = new_key;
      return;
    }
  }

  /* Rekey is always given a valid page to work with */
  assert( 0 );
}


/*
** Truncate the page cache.  Every page with a key of i_limit or larger
** is discarded.
*/
static void __testpcache_truncate(dbsql_pcache_t *cache, unsigned i_limit){
  testpcache *p = (testpcache*)cache;
  unsigned int i;
  assert( p->i_magic==TESTPCACHE_VALID );
  assert( __testpcache_global.dummy!=0 );
  assert( __testpcache_global.n_instance>0 );
  for(i=0; i<TESTPCACHE_NPAGE; i++){
    if( p->a[i].key>=i_limit ){
      p->a[i].key = 0;
      if( p->a[i].is_pinned ){
        p->n_pinned--;
        assert( p->n_pinned>=0 );
      }
      p->n_free++;
      assert( p->n_free<=TESTPCACHE_NPAGE );
    }
  }
}

/*
** Destroy a page cache.
*/
static void __testpcache_destroy(dbsql_pcache_t *cache){
  testpcache *p = (testpcache*)cache;
  assert( p->i_magic==TESTPCACHE_VALID );
  assert( __testpcache_global.dummy!=0 );
  assert( __testpcache_global.n_instance>0 );
  p->i_magic = TESTPCACHE_CLEAR;
  dbsql_free(p);
  __testpcache_global.n_instance--;
}


/*
** Invoke this routine to register or unregister the testing pager cache
** implemented by this file.
**
** Install the test pager cache if install_flag is 1 and uninstall it if
** install_flag is 0.
**
** When installing, discard_chance is a number between 0 and 100 that
** indicates the probability of discarding a page when unpinning the
** page.  0 means never discard (unless the discard flag is set).
** 100 means always discard.
*/
void __install_test_p_cache(
  int install_flag,            /* True to install.  False to uninstall. */
  unsigned discard_chance,     /* 0-100.  Chance to discard on unpin */
  unsigned __prng_seed,          /* Seed for the PRNG */
  unsigned high_stress         /* Call stress aggressively */
){
  static const dbsql_pcache_methods2_t test_pcache = {
    1,
    (void*)&__testpcache_global,
    __testpcache_init,
    __testpcache_shutdown,
    __testpcache_create,
    __testpcache_cachesize,
    __testpcache_pagecount,
    __testpcache_fetch,
    __testpcache_unpin,
    __testpcache_rekey,
    __testpcache_truncate,
    __testpcache_destroy,
  };
  static dbsql_pcache_methods2_t default_pcache;
  static int is_installed = 0;

  assert( __testpcache_global.n_instance==0 );
  assert( __testpcache_global.dummy==0 );
  assert( discard_chance<=100 );
  __testpcache_global.discard_chance = discard_chance;
  __testpcache_global.__prng_seed = __prng_seed ^ (__prng_seed<<16);
  __testpcache_global.high_stress = high_stress;
  if( install_flag!=is_installed ){
    if( install_flag ){
      dbsql_config(DBSQL_CONFIG_GETPCACHE2, &default_pcache);
      assert( default_pcache.create!=__testpcache_create );
      /* dbsql: no SQLite pcache (libdb mpool); nothing to swap. */
      if( default_pcache.create==0 ) return;
      dbsql_config(DBSQL_CONFIG_PCACHE2, &test_pcache);
    }else{
      assert( default_pcache.create!=0 );
      dbsql_config(DBSQL_CONFIG_PCACHE2, &default_pcache);
    }
    is_installed = install_flag;
  }
}
