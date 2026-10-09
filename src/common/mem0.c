/*
** 2008 October 28
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
** This file contains a no-op memory allocation drivers for use when
** DBSQL_ZERO_MALLOC is defined.  The allocation drivers implemented
** here always fail.  dbsql will not operate with these drivers.  These
** are merely placeholders.  Real drivers must be substituted using
** dbsql_config() before dbsql will operate.
*/
#include "dbsql_int.h"

/*
** This version of the memory allocator is the default.  It is
** used when no other memory allocator is specified using compile-time
** macros.
*/
#ifdef DBSQL_ZERO_MALLOC

/*
** No-op versions of all memory allocation routines
*/
static void *__mem_malloc(int n_byte){ return 0; }
static void __mem_free(void *prior){ return; }
static void *__mem_realloc(void *prior, int n_byte){ return 0; }
static int __mem_size(void *prior){ return 0; }
static int __mem_roundup(int n){ return n; }
static int __mem_init(void *__not_used){ return DBSQL_OK; }
static void __mem_shutdown(void *__not_used){ return; }

/*
** This routine is the only routine in this file with external linkage.
**
** Populate the low-level memory allocation function pointers in
** __global_config.m with pointers to the routines in this file.
*/
void __mem_set_default(void){
  static const dbsql_mem_methods_t default_methods = {
     __mem_malloc,
     __mem_free,
     __mem_realloc,
     __mem_size,
     __mem_roundup,
     __mem_init,
     __mem_shutdown,
     0
  };
  dbsql_config(DBSQL_CONFIG_MALLOC, &default_methods);
}

#endif /* DBSQL_ZERO_MALLOC */
