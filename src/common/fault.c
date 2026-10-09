/*
** 2008 Jan 22
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
** This file contains code to support the concept of "benign" 
** malloc failures (when the x_malloc() or x_realloc() method of the
** dbsql_mem_methods_t structure fails to allocate a block of memory
** and returns 0). 
**
** Most malloc failures are non-benign. After they occur, dbsql
** abandons the current operation and returns an error code (usually
** DBSQL_NOMEM) to the user. However, sometimes a fault is not necessarily
** fatal. For example, if a malloc fails while resizing a hash table, this 
** is completely recoverable simply by not carrying out the resize. The 
** hash table will continue to function normally.  So a malloc failure 
** during a hash table resize is a benign fault.
*/

#include "dbsql_int.h"

#ifndef DBSQL_UNTESTABLE

/*
** Global variables.
*/
typedef struct benign_malloc_hooks_t benign_malloc_hooks_t;
static DBSQL_WSD struct benign_malloc_hooks_t {
  void (*benign_begin)(void);
  void (*benign_end)(void);
} __hooks = { 0, 0 };

/* The "__wsd_hooks" macro will resolve to the appropriate benign_malloc_hooks_t
** structure.  If writable static data is unsupported on the target,
** we have to locate the state vector at run-time.  In the more common
** case where writable static data is supported, __wsd_hooks can refer directly
** to the "__hooks" state vector declared above.
*/
#ifdef DBSQL_OMIT_WSD
# define WSD_HOOKS_INIT \
  benign_malloc_hooks_t *x = &GLOBAL(benign_malloc_hooks_t,__hooks)
# define __wsd_hooks x[0]
#else
# define WSD_HOOKS_INIT
# define __wsd_hooks __hooks
#endif


/*
** Register hooks to call when __begin_benign_malloc() and
** __end_benign_malloc() are called, respectively.
*/
void __benign_malloc_hooks(
  void (*benign_begin)(void),
  void (*benign_end)(void)
){
  WSD_HOOKS_INIT;
  __wsd_hooks.benign_begin = benign_begin;
  __wsd_hooks.benign_end = benign_end;
}

/*
** This (__end_benign_malloc()) is called by dbsql code to indicate that
** subsequent malloc failures are benign. A call to __end_benign_malloc()
** indicates that subsequent malloc failures are non-benign.
*/
void __begin_benign_malloc(void){
  WSD_HOOKS_INIT;
  if( __wsd_hooks.benign_begin ){
    __wsd_hooks.benign_begin();
  }
}
void __end_benign_malloc(void){
  WSD_HOOKS_INIT;
  if( __wsd_hooks.benign_end ){
    __wsd_hooks.benign_end();
  }
}

#endif   /* #ifndef DBSQL_UNTESTABLE */
