/*
** 2007 August 14
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
** This file contains low-level memory allocation drivers for when
** dbsql will use the standard C-library malloc/realloc/free interface
** to obtain the memory it needs.
**
** This file contains implementations of the low-level memory allocation
** routines specified in the dbsql_mem_methods_t object.  The content of
** this file is only used if DBSQL_SYSTEM_MALLOC is defined.  The
** DBSQL_SYSTEM_MALLOC macro is defined automatically if neither the
** DBSQL_MEMDEBUG nor the DBSQL_WIN32_MALLOC macros are defined.  The
** default configuration is to use memory allocation routines in this
** file.
**
** C-preprocessor macro summary:
**
**    HAVE_MALLOC_USABLE_SIZE     The configure script sets this symbol if
**                                the malloc_usable_size() interface exists
**                                on the target platform.  Or, this symbol
**                                can be set manually, if desired.
**                                If an equivalent interface exists by
**                                a different name, using a separate -D
**                                option to rename it.
**
**    DBSQL_WITHOUT_ZONEMALLOC   Some older macs lack support for the zone
**                                memory allocator.  Set this symbol to enable
**                                building on older macs.
**
**    DBSQL_WITHOUT_MSIZE        Set this symbol to disable the use of
**                                _msize() on windows systems.  This might
**                                be necessary when compiling for Delphi,
**                                for example.
*/
#include "dbsql_int.h"

/*
** This version of the memory allocator is the default.  It is
** used when no other memory allocator is specified using compile-time
** macros.
*/
#ifdef DBSQL_SYSTEM_MALLOC
#if defined(__APPLE__) && !defined(DBSQL_WITHOUT_ZONEMALLOC)

/*
** Use the zone allocator available on apple products unless the
** DBSQL_WITHOUT_ZONEMALLOC symbol is defined.
*/
#include <sys/sysctl.h>
#include <malloc/malloc.h>
#ifdef DBSQL_MIGHT_BE_SINGLE_CORE
#include <libkern/OSAtomic.h>
#endif /* DBSQL_MIGHT_BE_SINGLE_CORE */
static malloc_zone_t* dbsql_zone;
#define DBSQL_MALLOC(x) malloc_zone_malloc(dbsql_zone, (x))
#define DBSQL_FREE(x) malloc_zone_free(dbsql_zone, (x));
#define DBSQL_REALLOC(x,y) malloc_zone_realloc(dbsql_zone, (x), (y))
#define DBSQL_MALLOCSIZE(x) \
        (dbsql_zone ? dbsql_zone->size(dbsql_zone,x) : malloc_size(x))

#else /* if not __APPLE__ */

/*
** Use standard C library malloc and free on non-Apple systems.  
** Also used by Apple systems if DBSQL_WITHOUT_ZONEMALLOC is defined.
*/
#define DBSQL_MALLOC(x)             malloc(x)
#define DBSQL_FREE(x)               free(x)
#define DBSQL_REALLOC(x,y)          realloc((x),(y))

/*
** The malloc.h header file is needed for malloc_usable_size() function
** on some systems (e.g. Linux).
*/
#if HAVE_MALLOC_H && HAVE_MALLOC_USABLE_SIZE
#  define DBSQL_USE_MALLOC_H 1
#  define DBSQL_USE_MALLOC_USABLE_SIZE 1
/*
** The MSVCRT has malloc_usable_size(), but it is called _msize().  The
** use of _msize() is automatic, but can be disabled by compiling with
** -DSQLITE_WITHOUT_MSIZE.  Using the _msize() function also requires
** the malloc.h header file.
*/
#elif defined(_MSC_VER) && !defined(DBSQL_WITHOUT_MSIZE)
#  define DBSQL_USE_MALLOC_H
#  define DBSQL_USE_MSIZE
#endif

/*
** Include the malloc.h header file, if necessary.  Also set define macro
** DBSQL_MALLOCSIZE to the appropriate function name, which is _msize()
** for MSVC and malloc_usable_size() for most other systems (e.g. Linux).
** The memory size function can always be overridden manually by defining
** the macro DBSQL_MALLOCSIZE to the desired function name.
*/
#if defined(DBSQL_USE_MALLOC_H)
#  include <malloc.h>
#  if defined(DBSQL_USE_MALLOC_USABLE_SIZE)
#    if !defined(DBSQL_MALLOCSIZE)
#      define DBSQL_MALLOCSIZE(x)   malloc_usable_size(x)
#    endif
#  elif defined(DBSQL_USE_MSIZE)
#    if !defined(DBSQL_MALLOCSIZE)
#      define DBSQL_MALLOCSIZE      _msize
#    endif
#  endif
#endif /* defined(DBSQL_USE_MALLOC_H) */

#endif /* __APPLE__ or not __APPLE__ */

/*
** Like malloc(), but remember the size of the allocation
** so that we can find it later using __mem_size().
**
** For this low-level routine, we are guaranteed that n_byte>0 because
** cases of n_byte<=0 will be intercepted and dealt with by higher level
** routines.
*/
static void *__mem_malloc(int n_byte){
#ifdef DBSQL_MALLOCSIZE
  void *p;
  testcase( ROUND8(n_byte)==n_byte );
  p = DBSQL_MALLOC( n_byte );
  if( p==0 ){
    testcase( __global_config.x_log!=0 );
    dbsql_log(DBSQL_NOMEM, "failed to allocate %u bytes of memory", n_byte);
  }
  return p;
#else
  int64_t *p;
  assert( n_byte>0 );
  testcase( ROUND8(n_byte)!=n_byte );
  p = DBSQL_MALLOC( n_byte+8 );
  if( p ){
    p[0] = n_byte;
    p++;
  }else{
    testcase( __global_config.x_log!=0 );
    dbsql_log(DBSQL_NOMEM, "failed to allocate %u bytes of memory", n_byte);
  }
  return (void *)p;
#endif
}

/*
** Like free() but works for allocations obtained from __mem_malloc()
** or __mem_realloc().
**
** For this low-level routine, we already know that prior!=0 since
** cases where prior==0 will have been intercepted and dealt with
** by higher-level routines.
*/
static void __mem_free(void *prior){
#ifdef DBSQL_MALLOCSIZE
  DBSQL_FREE(prior);
#else
  int64_t *p = (int64_t*)prior;
  assert( prior!=0 );
  p--;
  DBSQL_FREE(p);
#endif
}

/*
** Report the allocated size of a prior return from x_malloc()
** or x_realloc().
*/
static int __mem_size(void *prior){
#ifdef DBSQL_MALLOCSIZE
  assert( prior!=0 );
  return (int)DBSQL_MALLOCSIZE(prior);
#else
  int64_t *p;
  assert( prior!=0 );
  p = (int64_t*)prior;
  p--;
  return (int)p[0];
#endif
}

/*
** Like realloc().  Resize an allocation previously obtained from
** __mem_malloc().
**
** For this low-level interface, we know that prior!=0.  Cases where
** prior==0 while have been intercepted by higher-level routine and
** redirected to x_malloc.  Similarly, we know that n_byte>0 because
** cases where n_byte<=0 will have been intercepted by higher-level
** routines and redirected to __x_free.
*/
static void *__mem_realloc(void *prior, int n_byte){
#ifdef DBSQL_MALLOCSIZE
  void *p = DBSQL_REALLOC(prior, n_byte);
  if( p==0 ){
    testcase( __global_config.x_log!=0 );
    dbsql_log(DBSQL_NOMEM,
      "failed memory resize %u to %u bytes",
      DBSQL_MALLOCSIZE(prior), n_byte);
  }
  return p;
#else
  int64_t *p = (int64_t*)prior;
  assert( prior!=0 && n_byte>0 );
  assert( n_byte==ROUND8(n_byte) ); /* EV: R-46199-30249 */
  p--;
  p = DBSQL_REALLOC(p, n_byte+8 );
  if( p ){
    p[0] = n_byte;
    p++;
  }else{
    testcase( __global_config.x_log!=0 );
    dbsql_log(DBSQL_NOMEM,
      "failed memory resize %u to %u bytes",
      __mem_size(prior), n_byte);
  }
  return (void*)p;
#endif
}

/*
** Round up a request size to the next valid allocation size.
*/
static int __mem_roundup(int n){
  return ROUND8(n);
}

/*
** Initialize this module.
*/
static int __mem_init(void *__not_used){
#if defined(__APPLE__) && !defined(DBSQL_WITHOUT_ZONEMALLOC)
  int cpu_count;
  size_t len;
  if( dbsql_zone ){
    return DBSQL_OK;
  }
  len = sizeof(cpu_count);
  /* One usually wants to use hw.activecpu for MT decisions, but not here */
  sysctlbyname("hw.ncpu", &cpu_count, &len, NULL, 0);
  if( cpu_count>1 ){
    /* defer MT decisions to system malloc */
    dbsql_zone = malloc_default_zone();
  }else{
    /* only 1 core, use our own zone to contention over global locks,
    ** e.g. we have our own dedicated locks */
    dbsql_zone = malloc_create_zone(4096, 0);
    malloc_set_zone_name(dbsql_zone, "Dbsql_Heap");
  }
#endif /*  defined(__APPLE__) && !defined(DBSQL_WITHOUT_ZONEMALLOC) */
  UNUSED_PARAMETER(__not_used);
  return DBSQL_OK;
}

/*
** Deinitialize this module.
*/
static void __mem_shutdown(void *__not_used){
  UNUSED_PARAMETER(__not_used);
  return;
}

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

#endif /* DBSQL_SYSTEM_MALLOC */
