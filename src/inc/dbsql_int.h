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
** Internal interface definitions for dbsql.
**
*/
#ifndef DBSQLINT_H
#define DBSQLINT_H

/* Special Comments:
**
** Some comments have special meaning to the tools that measure test
** coverage:
**
**    NO_TEST                     - The branches on this line are not
**                                  measured by branch coverage.  This is
**                                  used on lines of code that actually
**                                  implement parts of coverage testing.
**
**    OPTIMIZATION-IF-TRUE        - This branch is allowed to always be false
**                                  and the correct answer is still obtained,
**                                  though perhaps more slowly.
**
**    OPTIMIZATION-IF-FALSE       - This branch is allowed to always be true
**                                  and the correct answer is still obtained,
**                                  though perhaps more slowly.
**
**    PREVENTS-HARMLESS-OVERREAD  - This branch prevents a buffer overread
**                                  that would be harmless and undetectable
**                                  if it did occur.
**
** In all cases, the special comment must be enclosed in the usual
** slash-asterisk...asterisk-slash comment marks, with no spaces between the
** asterisks and the comment text.
*/

/*
** Make sure the Tcl calling convention macro is defined.  This macro is
** only used by test code and Tcl integration code.
*/
#ifndef DBSQL_TCLAPI
#  define DBSQL_TCLAPI
#endif

/*
** Include the header file used to customize the compiler options for MSVC.
** This should be done first so that it can successfully prevent spurious
** compiler warnings due to subsequent content in this file and other files
** that are included by this file.
*/
#include "msvc.h"

/*
** Special setup for __vx_works
*/
#include "vxworks.h"

/*
** These #defines should enable >2GB file support on POSIX if the
** underlying operating system supports it.  If the OS lacks
** large file support, or if the OS is windows, these should be no-ops.
**
** Ticket #2739:  The _LARGEFILE_SOURCE macro must appear before any
** system #includes.  Hence, this block of code must be the very first
** code in all source files.
**
** Large file support can be disabled using the -DSQLITE_DISABLE_LFS switch
** on the compiler command line.  This is necessary if you are compiling
** on a recent machine (ex: Red Hat 7.2) but you want your code to work
** on an older machine (ex: Red Hat 6.0).  If you compile on Red Hat 7.2
** without this option, LFS is enable.  But LFS does not exist in the kernel
** in Red Hat 6.0, so the code won't work.  Hence, for maximum binary
** portability you should omit LFS.
**
** The previous paragraph was written in 2005.  (This paragraph is written
** on 2008-11-28.) These days, all Linux kernels support large files, so
** you should probably leave LFS enabled.  But some embedded platforms might
** lack LFS in which case the DBSQL_DISABLE_LFS macro might still be useful.
**
** Similar is true for Mac OS X.  LFS is only supported on Mac OS X 9 and later.
*/
#ifndef DBSQL_DISABLE_LFS
# define _LARGE_FILE       1
# ifndef _FILE_OFFSET_BITS
#   define _FILE_OFFSET_BITS 64
# endif
# define _LARGEFILE_SOURCE 1
#endif

/* The GCC_VERSION and MSVC_VERSION macros are used to
** conditionally include optimizations for each of these compilers.  A
** value of 0 means that compiler is not being used.  The
** DBSQL_DISABLE_INTRINSIC macro means do not use any compiler-specific
** optimizations, and hence set all compiler macros to 0
**
** There was once also a CLANG_VERSION macro.  However, we learn that the
** version numbers in clang are for "marketing" only and are inconsistent
** and unreliable.  Fortunately, all versions of clang also recognize the
** gcc version numbers and have reasonable settings for gcc version numbers,
** so the GCC_VERSION macro will be set to a correct non-zero value even
** when compiling with clang.
*/
#if defined(__GNUC__) && !defined(DBSQL_DISABLE_INTRINSIC)
# define GCC_VERSION (__GNUC__*1000000+__GNUC_MINOR__*1000+__GNUC_PATCHLEVEL__)
#else
# define GCC_VERSION 0
#endif
#if defined(_MSC_VER) && !defined(DBSQL_DISABLE_INTRINSIC)
# define MSVC_VERSION _MSC_VER
#else
# define MSVC_VERSION 0
#endif

/*
** Some C99 functions in "math.h" are only present for MSVC when its version
** is associated with Visual Studio 2013 or higher.
*/
#ifndef DBSQL_HAVE_C99_MATH_FUNCS
# if MSVC_VERSION==0 || MSVC_VERSION>=1800
#  define DBSQL_HAVE_C99_MATH_FUNCS (1)
# else
#  define DBSQL_HAVE_C99_MATH_FUNCS (0)
# endif
#endif

/* Needed for various definitions... */
#if defined(__GNUC__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#endif

#if defined(__OpenBSD__) && !defined(_BSD_SOURCE)
# define _BSD_SOURCE
#endif

/*
** Macro to disable warnings about missing "break" at the end of a "case".
*/
#if defined(__has_attribute)
#  if __has_attribute(fallthrough)
#    define deliberate_fall_through __attribute__((fallthrough));
#  endif
#endif
#if !defined(deliberate_fall_through)
#  define deliberate_fall_through
#endif

/*
** For __min_gw, check to see if we can include the header file containing its
** version information, among other things.  Normally, this internal __min_gw
** header file would [only] be included automatically by other __min_gw header
** files; however, the contained version information is now required by this
** header file to work around binary compatibility issues (see below) and
** this is the only known way to reliably obtain it.  This entire #if block
** would be completely unnecessary if there was any other way of detecting
** __min_gw via their preprocessor (e.g. if they customized their GCC to define
** some __min_gw-specific macros).  When compiling for __min_gw, either the
** _HAVE_MINGW_H or _HAVE__MINGW_H (note the extra underscore) macro must be
** defined; otherwise, detection of conditions specific to __min_gw will be
** disabled.
*/
#if defined(_HAVE_MINGW_H)
# include "mingw.h"
#elif defined(_HAVE__MINGW_H)
# include "_mingw.h"
#endif

/*
** For __min_gw version 4.x (and higher), check to see if the _USE_32BIT_TIME_T
** define is required to maintain binary compatibility with the MSVC runtime
** library in use (e.g. for Windows XP).
*/
#if !defined(_USE_32BIT_TIME_T) && !defined(_USE_64BIT_TIME_T) && \
    defined(_WIN32) && !defined(_WIN64) && \
    defined(__MINGW_MAJOR_VERSION) && __MINGW_MAJOR_VERSION >= 4 && \
    defined(__MSVCRT__)
# define _USE_32BIT_TIME_T
#endif

/* Optionally #include a user-defined header, whereby compilation options
** may be set prior to where they take effect, but after platform setup.
** If DBSQL_CUSTOM_INCLUDE=? is defined, its value names the #include
** file.
*/
#ifdef DBSQL_CUSTOM_INCLUDE
# define INC_STRINGIFY_(f) #f
# define INC_STRINGIFY(f) INC_STRINGIFY_(f)
# include INC_STRINGIFY(DBSQL_CUSTOM_INCLUDE)
#endif

/* The public dbsql interface.  The _FILE_OFFSET_BITS macro must appear
** first in QNX.  Also, the _USE_32BIT_TIME_T macro must appear first for
** __min_gw.
*/
#include "dbsql.h"

/*
** Reuse the STATIC_LRU for mutex access to dbsql_temp_directory.
*/
#define DBSQL_MUTEX_STATIC_TEMPDIR DBSQL_MUTEX_STATIC_VFS1

/*
** Include the configuration header output by 'configure' if we're using the
** autoconf-based build
*/
#if defined(_HAVE_DBSQL_CONFIG_H) && !defined(DBSQLCONFIG_H)
#include "dbsql_config.h"
#define DBSQLCONFIG_H 1
#endif

#include "dbsql_limit.h"
#include <sys/types.h>		/* u_int8_t ... u_int64_t */

/* Disable nuisance warnings on Borland compilers */
#if defined(__BORLANDC__)
#pragma warn -rch /* unreachable code */
#pragma warn -ccc /* Condition is always true or false */
#pragma warn -aus /* Assigned value is never used */
#pragma warn -csu /* Comparing signed and unsigned */
#pragma warn -spa /* Suspicious pointer arithmetic */
#endif

/*
** A few places in the code require atomic load/store of aligned
** integer values.
*/
#ifndef __has_extension
# define __has_extension(x) 0     /* compatibility with non-clang compilers */
#endif
#if GCC_VERSION>=4007000 || __has_extension(c_atomic)
# define DBSQL_ATOMIC_INTRINSICS 1
# define ATOMIC_LOAD(PTR)       __atomic_load_n((PTR),__ATOMIC_RELAXED)
# define ATOMIC_STORE(PTR,VAL)  __atomic_store_n((PTR),(VAL),__ATOMIC_RELAXED)
#else
# define DBSQL_ATOMIC_INTRINSICS 0
# define ATOMIC_LOAD(PTR)       (*(PTR))
# define ATOMIC_STORE(PTR,VAL)  (*(PTR) = (VAL))
#endif

/*
** Include standard header files as necessary
*/
#include <stdint.h>
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

/*
** The following macros are used to cast pointers to integers and
** integers to pointers.  The way you do this varies from one compiler
** to the next, so we have developed the following set of #if statements
** to generate appropriate macros for a wide range of compilers.
**
** The correct "ANSI" way to do this is to use the intptr_t type.
** Unfortunately, that typedef is not available on all compilers, or
** if it is available, it requires an #include of specific headers
** that vary from one machine to the next.
**
** Ticket #3860:  The llvm-gcc-4.2 compiler from Apple chokes on
** the ((void*)&((char*)0)[X]) construct.  But MSVC chokes on ((void*)(X)).
** So we have to define the macros in different ways depending on the
** compiler.
*/
#if defined(HAVE_STDINT_H)   /* Use this case if we have ANSI headers */
# define DBSQL_INT_TO_PTR(X)  ((void*)(intptr_t)(X))
# define DBSQL_PTR_TO_INT(X)  ((int)(intptr_t)(X))
#elif defined(__PTRDIFF_TYPE__)  /* This case should work for GCC */
# define DBSQL_INT_TO_PTR(X)  ((void*)(__PTRDIFF_TYPE__)(X))
# define DBSQL_PTR_TO_INT(X)  ((int)(__PTRDIFF_TYPE__)(X))
#elif !defined(__GNUC__)       /* Works for compilers other than LLVM */
# define DBSQL_INT_TO_PTR(X)  ((void*)&((char*)0)[X])
# define DBSQL_PTR_TO_INT(X)  ((int)(((char*)X)-(char*)0))
#else                          /* Generates a warning - but it always works */
# define DBSQL_INT_TO_PTR(X)  ((void*)(X))
# define DBSQL_PTR_TO_INT(X)  ((int)(X))
#endif

/*
** Macros to hint to the compiler that a function should or should not be
** inlined.
*/
#if defined(__GNUC__)
#  define DBSQL_NOINLINE  __attribute__((noinline))
#  define DBSQL_INLINE    __attribute__((always_inline)) inline
#elif defined(_MSC_VER) && _MSC_VER>=1310
#  define DBSQL_NOINLINE  __declspec(noinline)
#  define DBSQL_INLINE    __forceinline
#else
#  define DBSQL_NOINLINE
#  define DBSQL_INLINE
#endif
#if defined(DBSQL_COVERAGE_TEST) || defined(__STRICT_ANSI__)
# undef DBSQL_INLINE
# define DBSQL_INLINE
#endif

/*
** Make sure that the compiler intrinsics we desire are enabled when
** compiling with an appropriate version of MSVC unless prevented by
** the DBSQL_DISABLE_INTRINSIC define.
*/
#if !defined(DBSQL_DISABLE_INTRINSIC)
#  if defined(_MSC_VER) && _MSC_VER>=1400
#    if !defined(_WIN32_WCE)
#      include <intrin.h>
#      pragma intrinsic(_byteswap_ushort)
#      pragma intrinsic(_byteswap_ulong)
#      pragma intrinsic(_byteswap_uint64)
#      pragma intrinsic(read_write_barrier)
#    else
#      include <cmnintrin.h>
#    endif
#  endif
#endif

/*
** Enable DBSQL_USE_SEH by default on MSVC builds.  Only omit
** SEH support if the -DSQLITE_OMIT_SEH option is given.
*/
#if defined(_MSC_VER) && !defined(DBSQL_OMIT_SEH)
# define DBSQL_USE_SEH 1
#else
# undef DBSQL_USE_SEH
#endif

/*
** Enable DBSQL_DIRECT_OVERFLOW_READ, unless the build explicitly
** disables it using -DSQLITE_DIRECT_OVERFLOW_READ=0
*/
#if defined(DBSQL_DIRECT_OVERFLOW_READ) && DBSQL_DIRECT_OVERFLOW_READ+1==1
  /* Disable if -DSQLITE_DIRECT_OVERFLOW_READ=0 */
# undef DBSQL_DIRECT_OVERFLOW_READ
#else
  /* In all other cases, enable */
# define DBSQL_DIRECT_OVERFLOW_READ 1
#endif


/*
** The DBSQL_THREADSAFE macro must be defined as 0, 1, or 2.
** 0 means mutexes are permanently disable and the library is never
** threadsafe.  1 means the library is serialized which is the highest
** level of threadsafety.  2 means the library is multithreaded - multiple
** threads can use dbsql as long as no two threads try to use the same
** database connection at the same time.
**
** Older versions of dbsql used an optional THREADSAFE macro.
** We support that for legacy.
**
** To ensure that the correct value of "THREADSAFE" is reported when querying
** for compile-time options at runtime (e.g. "PRAGMA compile_options"), this
** logic is partially replicated in ctime.c. If it is updated here, it should
** also be updated there.
*/
#if !defined(DBSQL_THREADSAFE)
# if defined(THREADSAFE)
#   define DBSQL_THREADSAFE THREADSAFE
# else
#   define DBSQL_THREADSAFE 1 /* IMP: R-07272-22309 */
# endif
#endif

/*
** Powersafe overwrite is on by default.  But can be turned off using
** the -DSQLITE_POWERSAFE_OVERWRITE=0 command-line option.
*/
#ifndef DBSQL_POWERSAFE_OVERWRITE
# define DBSQL_POWERSAFE_OVERWRITE 1
#endif

/*
** EVIDENCE-OF: R-25715-37072 Memory allocation statistics are enabled by
** default unless dbsql is compiled with DBSQL_DEFAULT_MEMSTATUS=0 in
** which case memory allocation statistics are disabled by default.
*/
#if !defined(DBSQL_DEFAULT_MEMSTATUS)
# define DBSQL_DEFAULT_MEMSTATUS 1
#endif

/*
** Exactly one of the following macros must be defined in order to
** specify which memory allocation subsystem to use.
**
**     DBSQL_SYSTEM_MALLOC          // Use normal system malloc()
**     DBSQL_WIN32_MALLOC           // Use Win32 native heap API
**     DBSQL_ZERO_MALLOC            // Use a stub allocator that always fails
**     DBSQL_MEMDEBUG               // Debugging version of system malloc()
**
** On Windows, if the DBSQL_WIN32_MALLOC_VALIDATE macro is defined and the
** assert() macro is enabled, each call into the Win32 native heap subsystem
** will cause __heap_validate to be called.  If heap validation should fail, an
** assertion will be triggered.
**
** If none of the above are defined, then set DBSQL_SYSTEM_MALLOC as
** the default.
*/
#if defined(DBSQL_SYSTEM_MALLOC) \
  + defined(DBSQL_WIN32_MALLOC) \
  + defined(DBSQL_ZERO_MALLOC) \
  + defined(DBSQL_MEMDEBUG)>1
# error "Two or more of the following compile-time configuration options\
 are defined but at most one is allowed:\
 DBSQL_SYSTEM_MALLOC, DBSQL_WIN32_MALLOC, DBSQL_MEMDEBUG,\
 DBSQL_ZERO_MALLOC"
#endif
#if defined(DBSQL_SYSTEM_MALLOC) \
  + defined(DBSQL_WIN32_MALLOC) \
  + defined(DBSQL_ZERO_MALLOC) \
  + defined(DBSQL_MEMDEBUG)==0
# define DBSQL_SYSTEM_MALLOC 1
#endif

/*
** If DBSQL_MALLOC_SOFT_LIMIT is not zero, then try to keep the
** sizes of memory allocations below this value where possible.
*/
#if !defined(DBSQL_MALLOC_SOFT_LIMIT)
# define DBSQL_MALLOC_SOFT_LIMIT 1024
#endif

/*
** We need to define _XOPEN_SOURCE as follows in order to enable
** recursive mutexes on most Unix systems and fchmod() on __open_bsd.
** But _XOPEN_SOURCE define causes problems for Mac OS X, so omit
** it.
*/
#if !defined(_XOPEN_SOURCE) && !defined(__DARWIN__) && !defined(__APPLE__)
#  define _XOPEN_SOURCE 600
#endif

/*
** NDEBUG and DBSQL_DEBUG are opposites.  It should always be true that
** defined(NDEBUG)==!defined(DBSQL_DEBUG).  If this is not currently true,
** make it true by defining or undefining NDEBUG.
**
** Setting NDEBUG makes the code smaller and faster by disabling the
** assert() statements in the code.  So we want the default action
** to be for NDEBUG to be set and NDEBUG to be undefined only if DBSQL_DEBUG
** is set.  Thus NDEBUG becomes an opt-in rather than an opt-out
** feature.
*/
#if !defined(NDEBUG) && !defined(DBSQL_DEBUG)
# define NDEBUG 1
#endif
#if defined(NDEBUG) && defined(DBSQL_DEBUG)
# undef NDEBUG
#endif

/*
** Enable DBSQL_ENABLE_EXPLAIN_COMMENTS if DBSQL_DEBUG is turned on.
*/
#if !defined(DBSQL_ENABLE_EXPLAIN_COMMENTS) && defined(DBSQL_DEBUG)
# define DBSQL_ENABLE_EXPLAIN_COMMENTS 1
#endif

/*
** The testcase() macro is used to aid in coverage testing.  When
** doing coverage testing, the condition inside the argument to
** testcase() must be evaluated both true and false in order to
** get full branch coverage.  The testcase() macro is inserted
** to help ensure adequate test coverage in places where simple
** condition/decision coverage is inadequate.  For example, testcase()
** can be used to make sure boundary values are tested.  For
** bitmask tests, testcase() can be used to make sure each bit
** is significant and used at least once.  On switch statements
** where multiple cases go to the same block of code, testcase()
** can insure that all cases are evaluated.
*/
#if defined(DBSQL_COVERAGE_TEST) || defined(DBSQL_DEBUG)
# ifndef DBSQL_AMALGAMATION
    extern unsigned int __coverage_counter;
# endif
# define testcase(X)  if( X ){ __coverage_counter += (unsigned)__LINE__; }
#else
# define testcase(X)
#endif

/*
** The TESTONLY macro is used to enclose variable declarations or
** other bits of code that are needed to support the arguments
** within testcase() and assert() macros.
*/
#if !defined(NDEBUG) || defined(DBSQL_COVERAGE_TEST)
# define TESTONLY(X)  X
#else
# define TESTONLY(X)
#endif

/*
** Sometimes we need a small amount of code such as a variable initialization
** to setup for a later assert() statement.  We do not want this code to
** appear when assert() is disabled.  The following macro is therefore
** used to contain that setup code.  The "VVA" acronym stands for
** "Verification, Validation, and Accreditation".  In other words, the
** code within VVA_ONLY() will only run during verification processes.
*/
#ifndef NDEBUG
# define VVA_ONLY(X)  X
#else
# define VVA_ONLY(X)
#endif

/*
** Disable ALWAYS() and NEVER() (make them pass-throughs) for coverage
** and mutation testing
*/
#if defined(DBSQL_COVERAGE_TEST) || defined(DBSQL_MUTATION_TEST)
# define DBSQL_OMIT_AUXILIARY_SAFETY_CHECKS  1
#endif

/*
** The ALWAYS and NEVER macros surround boolean expressions which
** are intended to always be true or false, respectively.  Such
** expressions could be omitted from the code completely.  But they
** are included in a few cases in order to enhance the resilience
** of dbsql to unexpected behavior - to make the code "self-healing"
** or "ductile" rather than being "brittle" and crashing at the first
** hint of unplanned behavior.
**
** In other words, ALWAYS and NEVER are added for defensive code.
**
** When doing coverage testing ALWAYS and NEVER are hard-coded to
** be true and false so that the unreachable code they specify will
** not be counted as untested code.
*/
#if defined(DBSQL_OMIT_AUXILIARY_SAFETY_CHECKS)
# define ALWAYS(X)      (1)
# define NEVER(X)       (0)
#elif !defined(NDEBUG)
# define ALWAYS(X)      ((X)?1:(assert(0),0))
# define NEVER(X)       ((X)?(assert(0),1):0)
#else
# define ALWAYS(X)      (X)
# define NEVER(X)       (X)
#endif

/*
** Some conditionals are optimizations only.  In other words, if the
** conditionals are replaced with a constant 1 (true) or 0 (false) then
** the correct answer is still obtained, though perhaps not as quickly.
**
** The following macros mark these optimizations conditionals.
*/
#if defined(DBSQL_MUTATION_TEST)
# define OK_IF_ALWAYS_TRUE(X)  (1)
# define OK_IF_ALWAYS_FALSE(X) (0)
#else
# define OK_IF_ALWAYS_TRUE(X)  (X)
# define OK_IF_ALWAYS_FALSE(X) (X)
#endif

/*
** Some malloc failures are only possible if DBSQL_TEST_REALLOC_STRESS is
** defined.  We need to defend against those failures when testing with
** DBSQL_TEST_REALLOC_STRESS, but we don't want the unreachable branches
** during a normal build.  The following macro can be used to disable tests
** that are always false except when DBSQL_TEST_REALLOC_STRESS is set.
*/
#if defined(DBSQL_TEST_REALLOC_STRESS)
# define ONLY_IF_REALLOC_STRESS(X)  (X)
#elif !defined(NDEBUG)
# define ONLY_IF_REALLOC_STRESS(X)  ((X)?(assert(0),1):0)
#else
# define ONLY_IF_REALLOC_STRESS(X)  (0)
#endif

/*
** Declarations used for tracing the operating system interfaces.
*/
#if defined(DBSQL_FORCE_OS_TRACE) || defined(DBSQL_TEST) || \
    (defined(DBSQL_DEBUG) && DBSQL_OS_WIN)
  extern int __os_trace;
# define OSTRACE(X)          if( __os_trace ) __debug_printf X
# define DBSQL_HAVE_OS_TRACE
#else
# define OSTRACE(X)
# undef  DBSQL_HAVE_OS_TRACE
#endif

/*
** Is the __err_name() function needed in the build?  Currently,
** it is needed by "mutex_w32.c" (when debugging), "os_win.c" (when
** OSTRACE is enabled), and by several "test*.c" files (which are
** compiled using DBSQL_TEST).
*/
#if defined(DBSQL_HAVE_OS_TRACE) || defined(DBSQL_TEST) || \
    (defined(DBSQL_DEBUG) && DBSQL_OS_WIN)
# define DBSQL_NEED_ERR_NAME
#else
# undef  DBSQL_NEED_ERR_NAME
#endif

/*
** DBSQL_ENABLE_EXPLAIN_COMMENTS is incompatible with DBSQL_OMIT_EXPLAIN
*/
#ifdef DBSQL_OMIT_EXPLAIN
# undef DBSQL_ENABLE_EXPLAIN_COMMENTS
#endif

/*
** DBSQL_OMIT_VIRTUALTABLE implies DBSQL_OMIT_ALTERTABLE
*/
#if defined(DBSQL_OMIT_VIRTUALTABLE) && !defined(DBSQL_OMIT_ALTERTABLE)
# define DBSQL_OMIT_ALTERTABLE
#endif

#define DBSQL_DIGIT_SEPARATOR '_'

/*
** Return true (non-zero) if the input is an integer that is too large
** to fit in 32-bits.  This macro is used inside of various testcase()
** macros to verify that we have tested dbsql for large-file support.
*/
#define IS_BIG_INT(X)  (((X)&~(int64_t)0xffffffff)!=0)

/*
** The macro unlikely() is a hint that surrounds a boolean
** expression that is usually false.  Macro likely() surrounds
** a boolean expression that is usually true.  These hints could,
** in theory, be used by the compiler to generate better code, but
** currently they are just comments for human readers.
*/
#define likely(X)    (X)
#define unlikely(X)  (X)

#include "hash.h"
#include "sql_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>
#include <ctype.h>

/*
** Use a macro to replace memcpy() if compiled with DBSQL_INLINE_MEMCPY.
** This allows better measurements of where memcpy() is used when running
** cachegrind.  But this macro version of memcpy() is very slow so it
** should not be used in production.  This is a performance measurement
** hack only.
*/
#ifdef DBSQL_INLINE_MEMCPY
# define memcpy(D,S,N) {char*xxd=(char*)(D);const char*xxs=(const char*)(S);\
                        int xxn=(N);while(xxn-->0)*(xxd++)=*(xxs++);}
#endif

/*
** If compiling for a processor that lacks floating point support,
** substitute integer for floating-point
*/
#ifdef DBSQL_OMIT_FLOATING_POINT
# define double int64_t
# define float int64_t
# define fabs(X) ((X)<0?-(X):(X))
# define __is_overflow(X) 0
# define INFINITY (9223372036854775807LL)
# ifndef DBSQL_BIG_DBL
#   define DBSQL_BIG_DBL (((int64_t)1)<<50)
# endif
# define DBSQL_OMIT_DATETIME_FUNCS 1
# define DBSQL_OMIT_TRACE 1
# undef DBSQL_MIXED_ENDIAN_64BIT_FLOAT
# undef DBSQL_HAVE_ISNAN
#endif
#ifndef DBSQL_BIG_DBL
# define DBSQL_BIG_DBL (1e99)
#endif

/*
** OMIT_TEMPDB is set to 1 if DBSQL_OMIT_TEMPDB is defined, or 0
** afterward. Having this macro allows us to cause the C compiler
** to omit code used by TEMP tables without messy #ifndef statements.
*/
#ifdef DBSQL_OMIT_TEMPDB
#define OMIT_TEMPDB 1
#else
#define OMIT_TEMPDB 0
#endif

/*
** The "file format" number is an integer that is incremented whenever
** the VDBE-level file format changes.  The following macros define the
** the default file format for new databases and the maximum file format
** that the library can read.
*/
#define DBSQL_MAX_FILE_FORMAT 4
#ifndef DBSQL_DEFAULT_FILE_FORMAT
# define DBSQL_DEFAULT_FILE_FORMAT 4
#endif

/*
** Determine whether triggers are recursive by default.  This can be
** changed at run-time using a pragma.
*/
#ifndef DBSQL_DEFAULT_RECURSIVE_TRIGGERS
# define DBSQL_DEFAULT_RECURSIVE_TRIGGERS 0
#endif

/*
** Provide a default value for DBSQL_TEMP_STORE in case it is not specified
** on the command-line
*/
#ifndef DBSQL_TEMP_STORE
# define DBSQL_TEMP_STORE 1
#endif

/*
** If no value has been provided for DBSQL_MAX_WORKER_THREADS, or if
** DBSQL_TEMP_STORE is set to 3 (never use temporary files), set it
** to zero.
*/
#if DBSQL_TEMP_STORE==3 || DBSQL_THREADSAFE==0
# undef DBSQL_MAX_WORKER_THREADS
# define DBSQL_MAX_WORKER_THREADS 0
#endif
#ifndef DBSQL_MAX_WORKER_THREADS
# define DBSQL_MAX_WORKER_THREADS 8
#endif
#ifndef DBSQL_DEFAULT_WORKER_THREADS
# define DBSQL_DEFAULT_WORKER_THREADS 0
#endif
#if DBSQL_DEFAULT_WORKER_THREADS>DBSQL_MAX_WORKER_THREADS
# undef DBSQL_MAX_WORKER_THREADS
# define DBSQL_MAX_WORKER_THREADS DBSQL_DEFAULT_WORKER_THREADS
#endif

/*
** The default initial allocation for the pagecache when using separate
** pagecaches for each database connection.  A positive number is the
** number of pages.  A negative number N translations means that a buffer
** of -1024*N bytes is allocated and used for as many pages as it will hold.
**
** The default value of "20" was chosen to minimize the run-time of the
** speedtest1 test program with options: --shrink-memory --reprepare
*/
#ifndef DBSQL_DEFAULT_PCACHE_INITSZ
# define DBSQL_DEFAULT_PCACHE_INITSZ 20
#endif

/*
** Default value for the DBSQL_CONFIG_SORTERREF_SIZE option.
*/
#ifndef DBSQL_DEFAULT_SORTERREF_SIZE
# define DBSQL_DEFAULT_SORTERREF_SIZE 0x7fffffff
#endif

/*
** The compile-time options DBSQL_MMAP_READWRITE and
** DBSQL_ENABLE_BATCH_ATOMIC_WRITE are not compatible with one another.
** You must choose one or the other (or neither) but not both.
*/
#if defined(DBSQL_MMAP_READWRITE) && defined(DBSQL_ENABLE_BATCH_ATOMIC_WRITE)
#error Cannot use both DBSQL_MMAP_READWRITE and DBSQL_ENABLE_BATCH_ATOMIC_WRITE
#endif

/*
** GCC does not define the offsetof() macro so we'll have to do it
** ourselves.
*/
#ifndef offsetof
# define offsetof(ST,M) ((size_t)((char*)&((ST*)0)->M - (char*)0))
#endif

/*
** sizeof64() is like sizeof(), but always returns a 64-bit value, even
** on 32-bit builds. This can help to avoid overflow by ensuring 64-bit
** arithmetic is used consistently in both 32-bit and 64-bit builds.
*/
#define sizeof64(X) ((int64_t)sizeof(X))

/*
** Work around C99 "flex-array" syntax for pre-C99 compilers, so as
** to avoid complaints from -fsanitize=strict-bounds.
*/
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
# define FLEXARRAY
#else
# define FLEXARRAY 1
#endif

/*
** Macros to compute minimum and maximum of two numbers.
*/
#ifndef MIN
# define MIN(A,B) ((A)<(B)?(A):(B))
#endif
#ifndef MAX
# define MAX(A,B) ((A)>(B)?(A):(B))
#endif

/*
** Swap two objects of type TYPE.
*/
#define SWAP(TYPE,A,B) {TYPE t=A; A=B; B=t;}

/*
** Check to see if this machine uses EBCDIC.  (Yes, believe it or
** not, there are still machines out there that use EBCDIC.)
*/
#if 'A' == '\301'
# define DBSQL_EBCDIC 1
#else
# define DBSQL_ASCII 1
#endif

/*
** Integers of known sizes.  These typedefs might change for architectures
** where the sizes very.  Preprocessor macros are available so that the
** types can be conveniently redefined at compile-type.  Like this:
**
**         cc '-DUINTPTR_TYPE=long long int' ...
*/
#ifndef UINT32_TYPE
# ifdef HAVE_UINT32_T
#  define UINT32_TYPE uint32_t
# else
#  define UINT32_TYPE unsigned int
# endif
#endif
#ifndef UINT16_TYPE
# ifdef HAVE_UINT16_T
#  define UINT16_TYPE uint16_t
# else
#  define UINT16_TYPE unsigned short int
# endif
#endif
#ifndef INT16_TYPE
# ifdef HAVE_INT16_T
#  define INT16_TYPE int16_t
# else
#  define INT16_TYPE short int
# endif
#endif
#ifndef UINT8_TYPE
# ifdef HAVE_UINT8_T
#  define UINT8_TYPE uint8_t
# else
#  define UINT8_TYPE unsigned char
# endif
#endif
#ifndef INT8_TYPE
# ifdef HAVE_INT8_T
#  define INT8_TYPE int8_t
# else
#  define INT8_TYPE signed char
# endif
#endif








/* A bitfield type for use inside of structures.  Always follow with :N where
** N is the number of bits.
*/
typedef unsigned bft_t;  /* Bit Field Type */

/*
** DBSQL_MAX_U32 is a u64 constant that is the maximum u64 value
** that can be stored in a u32 without loss of data.  The value
** is 0x00000000ffffffff.  But because of quirks of some compilers, we
** have to specify the value in the less intuitive manner shown:
*/
#define DBSQL_MAX_U32  ((((u_int64_t)1)<<32)-1)

/*
** The datatype used to store estimates of the number of rows in a
** table or index.
*/
typedef u_int64_t rowcnt_t;

/*
** Estimated quantities used for query planning are stored as 16-bit
** logarithms.  For quantity X, the value stored is 10*log2(X).  This
** gives a possible range of values of approximately 1.0e986 to 1e-986.
** But the allowed values are "grainy".  Not every value is representable.
** For example, quantities 16 and 17 are both represented by a log_est_t
** of 40.  However, since log_est_t quantities are suppose to be estimates,
** not exact values, this imprecision is not a problem.
**
** "log_est_t" is short for "Logarithmic Estimate".
**
** Examples:
**      1 -> 0              20 -> 43          10000 -> 132
**      2 -> 10             25 -> 46          25000 -> 146
**      3 -> 16            100 -> 66        1000000 -> 199
**      4 -> 20           1000 -> 99        1048576 -> 200
**     10 -> 33           1024 -> 100    4294967296 -> 320
**
** The log_est_t can be negative to indicate fractional values.
** Examples:
**
**    0.5 -> -10           0.1 -> -33        0.0625 -> -40
*/
typedef INT16_TYPE log_est_t;
#define LOGEST_MIN (-32768)
#define LOGEST_MAX (32767)

/*
** Set the DBSQL_PTRSIZE macro to the number of bytes in a pointer
*/
#ifndef DBSQL_PTRSIZE
# if defined(__SIZEOF_POINTER__)
#   define DBSQL_PTRSIZE __SIZEOF_POINTER__
# elif defined(i386)     || defined(__i386__)   || defined(_M_IX86) ||    \
       defined(_M_ARM)   || defined(__arm__)    || defined(__x86)   ||    \
      (defined(__APPLE__) && defined(__ppc__)) ||                         \
      (defined(__TOS_AIX__) && !defined(__64BIT__))
#   define DBSQL_PTRSIZE 4
# else
#   define DBSQL_PTRSIZE 8
# endif
#endif

/* The uptr type is an unsigned integer large enough to hold a pointer
*/
#if defined(HAVE_STDINT_H)
  typedef uintptr_t uptr_t;
#elif DBSQL_PTRSIZE==4
  typedef u_int32_t uptr_t;
#else
  typedef u_int64_t uptr_t;
#endif

/*
** The DBSQL_WITHIN(P,S,E) macro checks to see if pointer P points to
** something between S (inclusive) and E (exclusive).
**
** In other words, S is a buffer and E is a pointer to the first byte after
** the end of buffer S.  This macro returns true if P points to something
** contained within the buffer S.
*/
#define DBSQL_WITHIN(P,S,E)   (((uptr_t)(P)>=(uptr_t)(S))&&((uptr_t)(P)<(uptr_t)(E)))

/*
** P is one byte past the end of a large buffer. Return true if a span of bytes
** between S..E crosses the end of that buffer.  In other words, return true
** if the sub-buffer S..E-1 overflows the buffer whose last byte is P-1.
**
** S is the start of the span.  E is one byte past the end of end of span.
**
**                        P
**     |-----------------|                FALSE
**               |-------|
**               S        E
**
**                        P
**     |-----------------|
**                    |-------|           TRUE
**                    S        E
**
**                        P
**     |-----------------|               
**                        |-------|       FALSE
**                        S        E
*/
#define DBSQL_OVERFLOW(P,S,E) (((uptr_t)(S)<(uptr_t)(P))&&((uptr_t)(E)>(uptr_t)(P)))

/*
** Macros to determine whether the machine is big or little endian,
** and whether or not that determination is run-time or compile-time.
**
** For best performance, an attempt is made to guess at the byte-order
** using C-preprocessor macros.  If that is unsuccessful, or if
** -DSQLITE_BYTEORDER=0 is set, then byte-order is determined
** at run-time.
**
** If you are building dbsql on some obscure platform for which the
** following ifdef magic does not work, you can always include either:
**
**    -DSQLITE_BYTEORDER=1234
**
** or
**
**    -DSQLITE_BYTEORDER=4321
**
** to cause the build to work for little-endian or big-endian processors,
** respectively.
*/
#ifndef DBSQL_BYTEORDER  /* Replicate changes at tag-20230904a */
# if defined(__BYTE_ORDER__) && __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
#   define DBSQL_BYTEORDER 4321
# elif defined(__BYTE_ORDER__) && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
#   define DBSQL_BYTEORDER 1234
# elif defined(__BIG_ENDIAN__) && __BIG_ENDIAN__==1
#   define DBSQL_BYTEORDER 4321
# elif defined(i386)    || defined(__i386__)      || defined(_M_IX86) ||    \
     defined(__x86_64)  || defined(__x86_64__)    || defined(_M_X64)  ||    \
     defined(_M_AMD64)  || defined(_M_ARM)        || defined(__x86)   ||    \
     defined(__ARMEL__) || defined(__AARCH64EL__) || defined(_M_ARM64)
#   define DBSQL_BYTEORDER 1234
# elif defined(sparc)   || defined(__ARMEB__)     || defined(__AARCH64EB__)
#   define DBSQL_BYTEORDER 4321
# else
#   define DBSQL_BYTEORDER 0
# endif
#endif
#if DBSQL_BYTEORDER==4321
# define DBSQL_BIGENDIAN    1
# define DBSQL_LITTLEENDIAN 0
# define DBSQL_UTF16NATIVE  DBSQL_UTF16BE
#elif DBSQL_BYTEORDER==1234
# define DBSQL_BIGENDIAN    0
# define DBSQL_LITTLEENDIAN 1
# define DBSQL_UTF16NATIVE  DBSQL_UTF16LE
#else
# ifdef DBSQL_AMALGAMATION
  const int __dbsqlone = 1;
# else
  extern const int __dbsqlone;
# endif
# define DBSQL_BIGENDIAN    (*(char *)(&__dbsqlone)==0)
# define DBSQL_LITTLEENDIAN (*(char *)(&__dbsqlone)==1)
# define DBSQL_UTF16NATIVE  (DBSQL_BIGENDIAN?DBSQL_UTF16BE:DBSQL_UTF16LE)
#endif

/*
** Constants for the largest and smallest possible 64-bit signed integers.
** These macros are designed to work correctly on both 32-bit and 64-bit
** compilers.
*/
#define LARGEST_INT64  (0xffffffff|(((int64_t)0x7fffffff)<<32))
#define LARGEST_UINT64 (0xffffffff|(((u_int64_t)0xffffffff)<<32))
#define SMALLEST_INT64 (((int64_t)-1) - LARGEST_INT64)

/*
** Macro SMXV(n) return the maximum value that can be held in variable n,
** assuming n is a signed integer type.  UMXV(n) is similar for unsigned
** integer types.
*/
#define SMXV(n) ((((int64_t)1)<<(sizeof(n)*8-1))-1)
#define UMXV(n) ((((int64_t)1)<<(sizeof(n)*8))-1)

/*
** Round up a number to the next larger multiple of 8.  This is used
** to force 8-byte alignment on 64-bit architectures.
**
** ROUND8() always does the rounding, for any argument.
**
** ROUND8P() assumes that the argument is already an integer number of
** pointers in size, and so it is a no-op on systems where the pointer
** size is 8.
*/
#define ROUND8(x)     (((x)+7)&~7)
#if DBSQL_PTRSIZE==8
# define ROUND8P(x)   (x)
#else
# define ROUND8P(x)   (((x)+7)&~7)
#endif

/*
** Round down to the nearest multiple of 8
*/
#define ROUNDDOWN8(x) ((x)&~7)

/*
** Assert that the pointer X is aligned to an 8-byte boundary.  This
** macro is used only within assert() to verify that the code gets
** all alignment restrictions correct.
**
** Except, if DBSQL_4_BYTE_ALIGNED_MALLOC is defined, then the
** underlying malloc() implementation might return us 4-byte aligned
** pointers.  In that case, only verify 4-byte alignment.
*/
#ifdef DBSQL_4_BYTE_ALIGNED_MALLOC
# define EIGHT_BYTE_ALIGNMENT(X)   ((((uptr_t)(X) - (uptr_t)0)&3)==0)
#else
# define EIGHT_BYTE_ALIGNMENT(X)   ((((uptr_t)(X) - (uptr_t)0)&7)==0)
#endif
#define TWO_BYTE_ALIGNMENT(X)      ((((uptr_t)(X) - (uptr_t)0)&1)==0)

/*
** Disable MMAP on platforms where it is known to not work
*/
#if defined(__OpenBSD__) || defined(__QNXNTO__)
# undef DBSQL_MAX_MMAP_SIZE
# define DBSQL_MAX_MMAP_SIZE 0
#endif

/*
** Default maximum size of memory used by memory-mapped I/O in the VFS
*/
#ifdef __APPLE__
# include <TargetConditionals.h>
#endif
#ifndef DBSQL_MAX_MMAP_SIZE
# if defined(__linux__) \
  || defined(_WIN32) \
  || (defined(__APPLE__) && defined(__MACH__)) \
  || defined(__sun) \
  || defined(__FreeBSD__) \
  || defined(dragon_fly)
#   define DBSQL_MAX_MMAP_SIZE 0x7fff0000  /* 2147418112 */
# else
#   define DBSQL_MAX_MMAP_SIZE 0
# endif
#endif

/*
** The default MMAP_SIZE is zero on all platforms.  Or, even if a larger
** default MMAP_SIZE is specified at compile-time, make sure that it does
** not exceed the maximum mmap size.
*/
#ifndef DBSQL_DEFAULT_MMAP_SIZE
# define DBSQL_DEFAULT_MMAP_SIZE 0
#endif
#if DBSQL_DEFAULT_MMAP_SIZE>DBSQL_MAX_MMAP_SIZE
# undef DBSQL_DEFAULT_MMAP_SIZE
# define DBSQL_DEFAULT_MMAP_SIZE DBSQL_MAX_MMAP_SIZE
#endif

/*
** TREETRACE_ENABLED will be either 1 or 0 depending on whether or not
** the Abstract Syntax Tree tracing logic is turned on.
*/
#if !defined(DBSQL_AMALGAMATION)
extern u_int32_t __tree_trace;
#endif
#if defined(DBSQL_DEBUG) \
    && (defined(DBSQL_TEST) || defined(DBSQL_ENABLE_SELECTTRACE) \
                             || defined(DBSQL_ENABLE_TREETRACE))
# define TREETRACE_ENABLED 1
# define TREETRACE(K,P,S,X)  \
  if(__tree_trace&(K))   \
    __debug_printf("%u/%d/%p: ",(S)->sel_id,(P)->addr_explain,(S)),\
    __debug_printf X
#else
# define TREETRACE(K,P,S,X)
# define TREETRACE_ENABLED 0
#endif

/* TREETRACE flag meanings:
**
**   0x00000001     Beginning and end of SELECT processing
**   0x00000002     WHERE clause processing
**   0x00000004     Query flattener
**   0x00000008     Result-set wildcard expansion
**   0x00000010     Query name resolution
**   0x00000020     Aggregate analysis
**   0x00000040     window_t functions
**   0x00000080     Generated column names
**   0x00000100     Move HAVING terms into WHERE
**   0x00000200     Count-of-view optimization
**   0x00000400     Compound SELECT processing
**   0x00000800     Drop superfluous ORDER BY
**   0x00001000     LEFT JOIN simplifies to JOIN
**   0x00002000     Constant propagation
**   0x00004000     Push-down optimization
**   0x00008000     After all FROM-clause analysis
**   0x00010000     Beginning of DELETE/INSERT/UPDATE processing
**   0x00020000     Transform DISTINCT into GROUP BY
**   0x00040000     SELECT tree dump after all code has been generated
**   0x00080000     NOT NULL strength reduction
**   0x00100000     Pointers are all shown as zero
**   0x00200000     EXISTS-to-JOIN optimization
*/

/*
** Macros for "wheretrace"
*/
extern u_int32_t __where_trace;
#if defined(DBSQL_DEBUG) \
    && (defined(DBSQL_TEST) || defined(DBSQL_ENABLE_WHERETRACE))
# define WHERETRACE(K,X)  if(__where_trace&(K)) __debug_printf X
# define WHERETRACE_ENABLED 1
#else
# define WHERETRACE(K,X)
#endif

/*
** Bits for the __where_trace mask:
**
** (---any--)   Top-level block structure
** 0x-------F   High-level debug messages
** 0x----FFF-   More detail
** 0xFFFF----   Low-level debug messages
**
** 0x00000001   Code generation
** 0x00000002   Solver (Use 0x40000 for less detail)
** 0x00000004   Solver costs
** 0x00000008   where_loop_t inserts
**
** 0x00000010   Display dbsql_index_info_t best_index calls
** 0x00000020   Range an equality scan metrics
** 0x00000040   IN operator decisions
** 0x00000080   where_loop_t cost adjustments
** 0x00000100
** 0x00000200   Covering index decisions
** 0x00000400   OR optimization
** 0x00000800   index_t scanner
** 0x00001000   More details associated with code generation
** 0x00002000
** 0x00004000   Show all WHERE terms at key points
** 0x00008000   Show the full SELECT statement at key places
**
** 0x00010000   Show more detail when printing WHERE terms
** 0x00020000   Show WHERE terms returned from __where_scan_next()
** 0x00040000   Solver overview messages
** 0x00080000   Star-query heuristic
** 0x00100000   Pointers are all shown as zero
*/


/*
** An instance of the following structure is used to store the busy-handler
** callback for a given sqlite handle.
**
** The sqlite.busy_handler member of the sqlite struct contains the busy
** callback for the database handle. Each pager opened via the sqlite
** handle is passed a pointer to sqlite.busy_handler. The busy-handler
** callback is currently invoked only from within pager.c.
*/
typedef struct busy_handler_t busy_handler_t;
struct busy_handler_t {
  int (*busy_handler)(void *,int);  /* The busy callback */
  void *busy_arg;                   /* First arg to busy callback */
  int n_busy;                        /* Incremented with each busy call */
};

/*
** Name of table that holds the database schema.
**
** The PREFERRED names are used wherever possible.  But LEGACY is also
** used for backwards compatibility.
**
**  1.  Queries can use either the PREFERRED or the LEGACY names
**  2.  The dbsql_set_authorizer() callback uses the LEGACY name
**  3.  The PRAGMA table_list statement uses the PREFERRED name
**
** The LEGACY names are stored in the internal symbol hash table
** in support of (2).  Names are translated using __preferred_table_name()
** for (3).  The __find_table() function takes care of translating
** names for (1).
**
** Note that "dbsql_temp_schema" can also be called "temp.dbsql_schema".
*/
#define LEGACY_SCHEMA_TABLE          "dbsql_master"
#define LEGACY_TEMP_SCHEMA_TABLE     "dbsql_temp_master"
#define PREFERRED_SCHEMA_TABLE       "dbsql_schema"
#define PREFERRED_TEMP_SCHEMA_TABLE  "dbsql_temp_schema"


/*
** The root-page of the schema table.
*/
#define SCHEMA_ROOT    1

/*
** The name of the schema table.  The name is different for TEMP.
*/
#define SCHEMA_TABLE(x) \
    ((!OMIT_TEMPDB)&&(x==1)?LEGACY_TEMP_SCHEMA_TABLE:LEGACY_SCHEMA_TABLE)

/*
** A convenience macro that returns the number of elements in
** an array.
*/
#define ARRAY_SIZE(X)    ((int)(sizeof(X)/sizeof(X[0])))

/*
** Determine if the argument is a power of two
*/
#define IS_POWER_OF_TWO(X) (((X)&((X)-1))==0)

/*
** The following value as a destructor means to use __dbsql_db_free().
** The __dbsql_db_free() routine requires two parameters instead of the
** one parameter that destructors normally want.  So we have to introduce
** this magic value that the code knows to handle differently.  Any
** pointer will work here as long as it is distinct from DBSQL_STATIC
** and DBSQL_TRANSIENT.
*/
#define DBSQL_DYNAMIC   ((dbsql_destructor_type)__row_set_clear)

/*
** When DBSQL_OMIT_WSD is defined, it means that the target platform does
** not support Writable Static Data (WSD) such as global and static variables.
** All variables must either be on the stack or dynamically allocated from
** the heap.  When WSD is unsupported, the variable declarations scattered
** throughout the dbsql code must become constants instead.  The DBSQL_WSD
** macro is used for this purpose.  And instead of referencing the variable
** directly, we use its constant as a key to lookup the run-time allocated
** buffer that holds real variable.  The constant is also the initializer
** for the run-time allocated buffer.
**
** In the usual case where WSD is supported, the DBSQL_WSD and GLOBAL
** macros become no-ops and have zero performance impact.
*/
#ifdef DBSQL_OMIT_WSD
  #define DBSQL_WSD const
  #define GLOBAL(t,v) (*(t*)dbsql_wsd_find((void*)&(v), sizeof(v)))
  #define __global_config GLOBAL(struct dbsql_config, __config)
  int dbsql_wsd_init(int N, int J);
  void *dbsql_wsd_find(void *K, int L);
#else
  #define DBSQL_WSD
  #define GLOBAL(t,v) v
  #define __global_config __config
#endif

/*
** The following macros are used to suppress compiler warnings and to
** make it clear to human readers when a function parameter is deliberately
** left unused within the body of a function. This usually happens when
** a function is called via a function pointer. For example the
** implementation of an SQL aggregate step callback may not use the
** parameter indicating the number of arguments passed to the aggregate,
** if it knows that this is enforced elsewhere.
**
** When a function parameter is not used at all within the body of a function,
** it is generally named "__not_used" or "__not_used2" to make things even clearer.
** However, these macros may also be used to suppress warnings related to
** parameters that may or may not be used depending on compilation options.
** For example those parameters only used in assert() statements. In these
** cases the parameters are named as per the usual conventions.
*/
#define UNUSED_PARAMETER(x) (void)(x)
#define UNUSED_PARAMETER2(x,y) UNUSED_PARAMETER(x),UNUSED_PARAMETER(y)

/*
** Forward references to structures
*/
typedef struct agg_info_t agg_info_t;
typedef struct auth_context_t auth_context_t;
typedef struct autoinc_info_t autoinc_info_t;
typedef struct bitvec_t bitvec_t;
typedef struct coll_seq_t coll_seq_t;
typedef struct column_t column_t;
typedef struct cte_t cte_t;
typedef struct cte_use_t cte_use_t;
typedef struct db_t db_t;
typedef struct db_client_data_t db_client_data_t;
typedef struct db_fixer_t db_fixer_t;
typedef struct schema_t schema_t;
typedef struct expr_t expr_t;
typedef struct expr_list_t expr_list_t;
typedef struct f_key_t f_key_t;
typedef struct fp_decode_t fp_decode_t;
typedef struct func_destructor_t func_destructor_t;
typedef struct func_def_t func_def_t;
typedef struct func_def_hash_t func_def_hash_t;
typedef struct id_list_t id_list_t;
typedef struct index_t index_t;
typedef struct indexed_expr_t indexed_expr_t;
typedef struct index_sample_t index_sample_t;
typedef struct key_class_t key_class_t;
typedef struct key_info_t key_info_t;
typedef struct lookaside_t lookaside_t;
typedef struct lookaside_slot_t lookaside_slot_t;
typedef struct module_def_t module_def_t;
typedef struct name_context_t name_context_t;
typedef struct on_or_using_t on_or_using_t;
typedef struct parser_t parser_t;
typedef struct parse_cleanup_t parse_cleanup_t;
typedef struct pre_update_t pre_update_t;
typedef struct printf_arguments_t printf_arguments_t;
typedef struct rc_str_t rc_str_t;
typedef struct rename_token_t rename_token_t;
typedef struct returning_t returning_t;
typedef struct row_set_t row_set_t;
typedef struct savepoint_t savepoint_t;
typedef struct select_t select_t;
typedef struct sq_lite_thread_t sq_lite_thread_t;
typedef struct select_dest_t select_dest_t;
typedef struct subquery_t subquery_t;
typedef struct src_item_t src_item_t;
typedef struct src_list_t src_list_t;
typedef struct dbsql_str_t str_accum_t; /* Internal alias for dbsql_str_t */
typedef struct table_t table_t;
typedef struct table_lock_t table_lock_t;
typedef struct Token Token;
typedef struct tree_view_t tree_view_t;
typedef struct trigger_t trigger_t;
typedef struct trigger_prg_t trigger_prg_t;
typedef struct trigger_step_t trigger_step_t;
typedef struct unpacked_record_t unpacked_record_t;
typedef struct upsert_t upsert_t;
typedef struct v_table_t v_table_t;
typedef struct vtab_ctx_t vtab_ctx_t;
typedef struct walker_t walker_t;
typedef struct where_info_t where_info_t;
typedef struct window_t window_t;
typedef struct with_t with_t;


/*
** The bitmask datatype defined below is used for various optimizations.
**
** Changing this from a 64-bit to a 32-bit type limits the number of
** tables in a join to 32 instead of 64.  But it also reduces the size
** of the library by 738 bytes on ix86.
*/
#ifdef DBSQL_BITMASK_TYPE
  typedef DBSQL_BITMASK_TYPE Bitmask;
#else
  typedef u_int64_t Bitmask;
#endif

/*
** The number of bits in a Bitmask.  "BMS" means "__bit_mask Size".
*/
#define BMS  ((int)(sizeof(Bitmask)*8))

/*
** A bit in a Bitmask
*/
#define MASKBIT(n)    (((Bitmask)1)<<(n))
#define MASKBIT64(n)  (((u_int64_t)1)<<(n))
#define MASKBIT32(n)  (((unsigned int)1)<<(n))
#define SMASKBIT32(n) ((n)<=31?((unsigned int)1)<<(n):0)
#define ALLBITS       ((Bitmask)-1)
#define TOPBIT        (((Bitmask)1)<<(BMS-1))

/* A VList object records a mapping between parameters/variables/wildcards
** in the SQL statement (such as $abc, @pqr, or :xyz) and the integer
** variable number associated with that parameter.  See the format description
** on the __v_list_add() routine for more information.  A VList is really
** just an array of integers.
*/
typedef int vlist_t;

/*
** Defer sourcing vdbe.h and btree.h until after the "u8" and
** "busy_handler_t" typedefs. vdbe.h also requires a few of the opaque
** pointer types (i.e. func_def_t) defined above.
*/
#include "os.h"
#include "pager.h"
#include "sm.h"
#include "vdbe.h"
#include "pcache.h"
#include "mutex.h"

/* The DBSQL_EXTRA_DURABLE compile-time option used to set the default
** synchronous setting to EXTRA.  It is no longer supported.
*/
#ifdef DBSQL_EXTRA_DURABLE
# warning Use DBSQL_DEFAULT_SYNCHRONOUS=3 instead of DBSQL_EXTRA_DURABLE
# define DBSQL_DEFAULT_SYNCHRONOUS 3
#endif

/*
** Default synchronous levels.
**
** Note that (for historical reasons) the PAGER_SYNCHRONOUS_* macros differ
** from the DBSQL_DEFAULT_SYNCHRONOUS value by 1.
**
**           PAGER_SYNCHRONOUS       DEFAULT_SYNCHRONOUS
**   OFF           1                         0
**   NORMAL        2                         1
**   FULL          3                         2
**   EXTRA         4                         3
**
** The "PRAGMA synchronous" statement also uses the zero-based numbers.
** In other words, the zero-based numbers are used for all external interfaces
** and the one-based values are used internally.
*/
#ifndef DBSQL_DEFAULT_SYNCHRONOUS
# define DBSQL_DEFAULT_SYNCHRONOUS 2
#endif
#ifndef DBSQL_DEFAULT_WAL_SYNCHRONOUS
# define DBSQL_DEFAULT_WAL_SYNCHRONOUS DBSQL_DEFAULT_SYNCHRONOUS
#endif

/*
** Each database file to be accessed by the system is an instance
** of the following structure.  There are normally two of these structures
** in the sqlite.dbs[] array.  dbs[0] is the main database file and
** dbs[1] is the database file used to hold temporary tables.  Additional
** databases may be attached.
*/
struct db_t {
  char *db_s_name;      /* Name of this database. (schema name, not filename) */
  sm_t *bt;          /* The B*Tree structure for this database file */
  u_int8_t safety_level;     /* How aggressive at syncing data to disk */
  u_int8_t sync_set;         /* True if "PRAGMA synchronous=N" has been run */
  schema_t *schema;     /* Pointer to database schema (possibly shared) */
};

/*
** An instance of the following structure stores a database schema.
**
** Most schema_t objects are associated with a sm_t.  The exception is
** the schema_t for the TEMP database (DBSQL.dbs[1]) which is free-standing.
** In shared cache mode, a single schema_t object can be shared by multiple
** Btrees that refer to the same underlying sm_shared_t object.
**
** schema_t objects are automatically deallocated when the last sm_t that
** references them is destroyed.   The TEMP schema_t is manually freed by
** dbsql_close().
*
** A thread must be holding a mutex on the corresponding sm_t in order
** to access schema_t content.  This implies that the thread must also be
** holding a mutex on the DBSQL connection pointer that owns the sm_t.
** For a TEMP schema_t, only the connection mutex is required.
*/
struct schema_t {
  int schema_cookie;   /* Database schema version number for this file */
  int i_generation;     /* Generation counter.  Incremented with each change */
  hash_t tbl_hash;        /* All tables indexed by name */
  hash_t idx_hash;        /* All (named) indices indexed by name */
  hash_t trig_hash;       /* All triggers indexed by name */
  hash_t fkey_hash;       /* All foreign keys by referenced table name */
  table_t *seq_tab;      /* The dbsql_sequence table used by AUTOINCREMENT */
  u_int8_t file_format;      /* schema_t format version for this file */
  u_int8_t enc;              /* Text encoding used by this database */
  u_int16_t schema_flags;     /* Flags associated with this schema */
  int cache_size;      /* Number of pages to use in the cache */
};

/*
** These macros can be used to test, set, or clear bits in the
** db_t.schema->flags field.
*/
#define DB_HAS_PROPERTY(D,I,P)     (((D)->dbs[I].schema->schema_flags&(P))==(P))
#define DB_HAS_ANY_PROPERTY(D,I,P)  (((D)->dbs[I].schema->schema_flags&(P))!=0)
#define DB_SET_PROPERTY(D,I,P)     (D)->dbs[I].schema->schema_flags|=(P)
#define DB_CLEAR_PROPERTY(D,I,P)   (D)->dbs[I].schema->schema_flags&=~(P)

/*
** Allowed values for the DB.schema->flags field.
**
** The DB_SchemaLoaded flag is set after the database schema has been
** read into internal hash tables.
**
** DB_UnresetViews means that one or more views have column names that
** have been filled out.  If the schema changes, these column names might
** changes and so the view will need to be reset.
*/
#define DB_SchemaLoaded    0x0001  /* The schema has been loaded */
#define DB_UnresetViews    0x0002  /* Some views have defined column names */
#define DB_ResetWanted     0x0008  /* Reset the schema when n_schema_lock==0 */

/*
** The number of different kinds of things that can be limited
** using the dbsql_limit() interface.
*/
#define DBSQL_N_LIMIT (DBSQL_LIMIT_PARSER_DEPTH+1)

/*
** lookaside_t malloc is a set of fixed-size buffers that can be used
** to satisfy small transient memory allocation requests for objects
** associated with a particular database connection.  The use of
** lookaside malloc provides a significant performance enhancement
** (approx 10%) by avoiding numerous malloc/free requests while parsing
** SQL statements.
**
** The lookaside_t structure holds configuration information about the
** lookaside malloc subsystem.  Each available memory allocation in
** the lookaside subsystem is stored on a linked list of lookaside_slot_t
** objects.
**
** lookaside_t allocations are only allowed for objects that are associated
** with a particular database connection.  Hence, schema information cannot
** be stored in lookaside because in shared cache mode the schema information
** is shared by multiple database connections.  Therefore, while parsing
** schema information, the lookaside_t.enabled flag is cleared so that
** lookaside allocations are not used to construct the schema objects.
**
** New lookaside allocations are only allowed if disable==0.  When
** disable is greater than zero, sz is set to zero which effectively
** disables lookaside without adding a new test for the disable flag
** in a performance-critical path.  sz should be set by to sz_true whenever
** disable changes back to zero.
**
** lookaside_t buffers are initially held on the init list.  As they are
** used and freed, they are added back to the p_free list.  New allocations
** come off of p_free first, then init as a fallback.  This dual-list
** allows use to compute a high-water mark - the maximum number of allocations
** outstanding at any point in the past - by subtracting the number of
** allocations on the init list from the total number of allocations.
**
** Enhancement on 2019-12-12:  Two-size-lookaside
** The default lookaside configuration is 100 slots of 1200 bytes each.
** The larger slot sizes are important for performance, but they waste
** a lot of space, as most lookaside allocations are less than 128 bytes.
** The two-size-lookaside enhancement breaks up the lookaside allocation
** into two pools:  One of 128-byte slots and the other of the default size
** (1200-byte) slots.   Allocations are filled from the small-pool first,
** failing over to the full-size pool if that does not work.  Thus more
** lookaside slots are available while also using less memory.
** This enhancement can be omitted by compiling with
** DBSQL_OMIT_TWOSIZE_LOOKASIDE.
*/
struct lookaside_t {
  u_int32_t disable;           /* Only operate the lookaside when zero */
  u_int16_t sz;                 /* Size of each buffer in bytes */
  u_int16_t sz_true;             /* True value of sz, even if disabled */
  u_int8_t malloced;           /* True if start obtained from dbsql_malloc() */
  u_int32_t n_slot;              /* Number of lookaside slots allocated */
  u_int32_t an_stat[3];          /* 0: hits.  1: size misses.  2: full misses */
  lookaside_slot_t *init;   /* List of buffers not previously used */
  lookaside_slot_t *p_free;   /* List of available buffers */
#ifndef DBSQL_OMIT_TWOSIZE_LOOKASIDE
  lookaside_slot_t *small_init; /* List of small buffers not previously used */
  lookaside_slot_t *small_free; /* List of available small buffers */
  void *middle;          /* First byte past end of full-size buffers and
                          ** the first byte of LOOKASIDE_SMALL buffers */
#endif /* DBSQL_OMIT_TWOSIZE_LOOKASIDE */
  void *p_start;           /* First byte of available memory space */
  void *p_end;             /* First byte past end of available space */
  void *true_end;         /* True value of end, when db->pn_bytes_freed!=0 */
};
struct lookaside_slot_t {
  lookaside_slot_t *next;    /* Next buffer in the list of free buffers */
};

#define DISABLE_LOOKASIDE  db->lookaside.disable++;db->lookaside.sz=0
#define ENABLE_LOOKASIDE   db->lookaside.disable--;\
   db->lookaside.sz=db->lookaside.disable?0:db->lookaside.sz_true

/* Size of the smaller allocations in two-size lookaside */
#ifdef DBSQL_OMIT_TWOSIZE_LOOKASIDE
#  define LOOKASIDE_SMALL           0
#else
#  define LOOKASIDE_SMALL         128
#endif

/*
** A hash table for built-in function definitions.  (Application-defined
** functions use a regular table table from hash.h.)
**
** hash_t each func_def_t structure into one of the func_def_hash_t.a[] slots.
** Collisions are on the func_def_t.u.hash chain.  Use the DBSQL_FUNC_HASH()
** macro to compute a hash on the function name.
*/
#define DBSQL_FUNC_HASH_SZ 23
struct func_def_hash_t {
  func_def_t *a[DBSQL_FUNC_HASH_SZ];       /* hash_t table for functions */
};
#define DBSQL_FUNC_HASH(C,L) (((C)+(L))%DBSQL_FUNC_HASH_SZ)

/*
** typedef for the authorization callback function.
*/
typedef int (*dbsql_xauth)(void*,int,const char*,const char*,const char*,
                             const char*);

#ifndef DBSQL_OMIT_DEPRECATED
/* This is an extra DBSQL_TRACE macro that indicates "legacy" tracing
** in the style of dbsql_trace()
*/
#define DBSQL_TRACE_LEGACY          0x40     /* Use the legacy trace */
#define DBSQL_TRACE_XPROFILE        0x80     /* Use the legacy profile */
#else
#define DBSQL_TRACE_LEGACY          0
#define DBSQL_TRACE_XPROFILE        0
#endif /* DBSQL_OMIT_DEPRECATED */
#define DBSQL_TRACE_NONLEGACY_MASK  0x0f     /* Normal flags */

/*
** Maximum number of DBSQL.dbs[] entries.  This is the number of attached
** databases plus 2 for "main" and "temp".
*/
#define DBSQL_MAX_DB (DBSQL_MAX_ATTACHED+2)

/*
** Each database connection is an instance of the following structure.
*/
struct DBSQL {
  dbsql_vfs_t *vfs;            /* OS Interface */
  struct vdbe_t *vdbe;           /* List of active virtual machines */
  coll_seq_t *dflt_coll;           /* BINARY collseq for the database encoding */
  dbsql_mutex_t *mutex;         /* Connection mutex */
  db_t *dbs;                      /* All backends */
  int n_db;                      /* Number of backends currently in use */
  u_int32_t m_db_flags;                 /* flags recording internal state */
  u_int64_t flags;                    /* flags settable by pragmas. See below */
  int64_t last_rowid;                /* ROWID of most recent insert (see above) */
  int64_t sz_mmap;                   /* Default mmap_size setting */
  u_int32_t n_schema_lock;              /* Do not reset the schema when non-zero */
  unsigned int open_flags;       /* Flags passed to dbsql_vfs_t.x_open() */
  int errCode;                  /* Most recent error code (DBSQL_*) */
  int err_byte_offset;            /* Byte offset of error in SQL statement */
  int err_mask;                  /* & result codes with this before returning */
  int i_sys_errno;                /* errno_info_t value from last system error */
  u_int32_t db_opt_flags;               /* Flags to enable/disable optimizations */
  u_int8_t enc;                       /* Text encoding */
  u_int8_t auto_commit;                /* The auto-commit flag. */
  u_int8_t temp_store;                /* 1: file 2: memory 0: default */
  u_int8_t malloc_failed;              /* True if we have seen a malloc failure */
  u_int8_t benign_malloc;             /* Do not require OOMs if true */
  u_int8_t dflt_lock_mode;              /* Default locking-mode for attached dbs */
  signed char next_autovac;      /* Autovac setting after VACUUM if >=0 */
  u_int8_t suppress_err;               /* Do not issue error messages if true */
  u_int8_t vtab_on_conflict;            /* Value to return for s3_vtab_on_conflict() */
  u_int8_t is_transaction_savepoint;    /* True if the outermost savepoint is a TS */
  u_int8_t m_trace;                    /* zero or more DBSQL_TRACE flags */
  u_int8_t no_shared_cache;             /* True if no shared-cache backends */
  u_int8_t n_sql_exec;                  /* Number of pending OP_SqlExec opcodes */
  u_int8_t open_state;                /* Current condition of the connection */
  u_int8_t n_fp_digit;                  /* Significant digits to keep on double->text */
  int next_pagesize;             /* Pagesize after VACUUM if >0 */
  int64_t n_change;                  /* Value returned by dbsql_changes() */
  int64_t n_total_change;             /* Value returned by dbsql_total_changes() */
  int limits[DBSQL_N_LIMIT];   /* Limits */
  int n_max_sorter_mmap;           /* Maximum size of regions mapped by sorter */
  struct __init_info {      /* Information used during initialization */
    pgno_t new_tnum;               /* Rootpage of table being initialized */
    u_int8_t i_db;                     /* Which db file is being initialized */
    u_int8_t busy;                    /* TRUE if currently initializing */
    unsigned orphan_trigger : 1; /* Last statement is orphaned TEMP trigger */
    unsigned imposter_table : 2; /* Building an imposter table */
    unsigned reopen_memdb : 1;   /* ATTACH is really a reopen using __mem_db */
    const char **inits;        /* "type", "name", and "tbl_name" columns */
  } init;
  int n_vdbe_active;              /* Number of VDBEs currently running */
  int n_vdbe_read;                /* Number of active VDBEs that read or write */
  int n_vdbe_write;               /* Number of active VDBEs that read and write */
  int n_vdbe_exec;                /* Number of nested calls to __vdbe_exec() */
  int n_v_destroy;                /* Number of active OP_VDestroy operations */
  int n_extension;               /* Number of loaded extensions */
  void **extensions;            /* Array of shared library handles */
  union {
    void (*legacy)(void*,const char*);   /* m_trace==DBSQL_TRACE_LEGACY */
    int (*v2)(u_int32_t,void*,void*,void*);    /* All other m_trace values */
  } trace;
  void *trace_arg;                        /* Argument to the trace function */
#ifndef DBSQL_OMIT_DEPRECATED
  void (*profile)(void*,const char*,u_int64_t);  /* Profiling function */
  void *profile_arg;                        /* Argument to profile function */
#endif
  void *commit_arg;                 /* Argument to commit_callback() */
  int (*commit_callback)(void*);    /* Invoked at every commit. */
  void *rollback_arg;               /* Argument to rollback_callback() */
  void (*rollback_callback)(void*); /* Invoked at every commit. */
  void *update_arg;
  void (*update_callback)(void*,int, const char*,const char*,int64_t);
  void *autovac_pages_arg;           /* Client argument to autovac_pages */
  void (*autovac_destr)(void*);     /* Destructor for autovac_p_ages_arg */
  unsigned int (*autovac_pages)(void*,const char*,u_int32_t,u_int32_t,u_int32_t);
  parser_t *parser;                /* Current parse */
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
  void *pre_update_arg;          /* First argument to pre_update_callback */
  void (*pre_update_callback)(   /* Registered using dbsql_preupdate_hook() */
    void*,DBSQL*,int,char const*,char const*,int64_t,int64_t
  );
  pre_update_t *pre_update;        /* Context for active pre-update callback */
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */
#ifndef DBSQL_OMIT_WAL
  int (*wal_callback)(void *, DBSQL *, const char *, int);
  void *wal_arg;
#endif
  void(*coll_needed)(void*,DBSQL*,int text_rep,const char*);
  void(*coll_needed16)(void*,DBSQL*,int text_rep,const void*);
  void *coll_needed_arg;
  dbsql_value_t *err;          /* Most recent error message */
  union {
    volatile int is_interrupted; /* True if dbsql_interrupt has been called */
    double not_used1;            /* Spacer */
  } u1;
  lookaside_t lookaside;          /* lookaside_t malloc configuration */
#ifndef DBSQL_OMIT_AUTHORIZATION
  dbsql_xauth auth;          /* Access authorization function */
  void *auth_arg;               /* 1st argument to the access auth function */
#endif
#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
  int (*progress)(void *);     /* The progress callback */
  void *progress_arg;           /* Argument to the progress callback */
  unsigned n_progress_ops;        /* Number of opcodes for progress callback */
#endif
#ifndef DBSQL_OMIT_VIRTUALTABLE
  int n_v_trans;                  /* Allocated size of v_trans */
  hash_t modules;                 /* populated by dbsql_create_module() */
  vtab_ctx_t *vtab_ctx;            /* Context for active vtab connect/create */
  v_table_t **v_trans;             /* Virtual tables with open transactions */
  v_table_t *disconnect;          /* Disconnect these in next dbsql_prepare() */
#endif
  hash_t funcs;                   /* hash_t table of connection functions */
  hash_t coll_seqs;                /* All collating sequences */
  busy_handler_t busy_handler;      /* Busy callback */
  struct sm_txn *sm_txns;          /* libdb txn state per env (sm/) */
  db_t db_statics[2];              /* Static space for the 2 default backends */
  savepoint_t *savepoint;        /* List of active savepoints */
  int n_analysis_limit;           /* Number of index rows to ANALYZE */
  int busy_timeout;              /* Busy handler timeout, in msec */
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
  int setlk_timeout;             /* Blocking lock timeout, in msec. -1 -> inf. */
  int setlk_flags;               /* Flags passed to setlk_timeout() */
#endif
  int n_savepoint;               /* Number of non-transaction savepoints */
  int n_statement;               /* Number of nested statement-transactions  */
  int64_t n_deferred_cons;            /* Net deferred constraints this transaction. */
  int64_t n_deferred_imm_cons;         /* Net deferred immediate constraints */
  int *pn_bytes_freed;            /* If not NULL, increment this in __db_free() */
  db_client_data_t *db_data;        /* dbsql_set_clientdata() content */
  u_int64_t n_spill;                   /* TEMP content spilled to disk */
#ifdef DBSQL_ENABLE_UNLOCK_NOTIFY
  /* The following variables are all protected by the STATIC_MAIN
  ** mutex, not by DBSQL.mutex. They are used by code in notify.c.
  **
  ** When X.unlock_connection==Y, that means that X is waiting for Y to
  ** unlock so that it can proceed.
  **
  ** When X.blocking_connection==Y, that means that something that X tried
  ** tried to do recently failed with an DBSQL_LOCKED error due to locks
  ** held by Y.
  */
  DBSQL *blocking_connection; /* Connection that caused DBSQL_LOCKED */
  DBSQL *unlock_connection;           /* Connection to watch for unlock */
  void *unlock_arg;                     /* Argument to unlock_notify */
  void (*unlock_notify)(void **, int);  /* Unlock notify callback */
  DBSQL *next_blocked;        /* Next in list of all blocked connections */
#endif
};

/*
** A macro to discover the encoding of a database.
*/
#define SCHEMA_ENC(db) ((db)->dbs[0].schema->enc)
#define ENC(db)        ((db)->enc)

/*
** A u64 constant where the lower 32 bits are all zeros.  Only the
** upper 32 bits are included in the argument.  Necessary because some
** C-compilers still do not accept LL integer literals.
*/
#define HI(X)  ((u_int64_t)(X)<<32)

/*
** Possible values for the DBSQL.flags.
**
** Value constraints (enforced via assert()):
**      DBSQL_FullFSync     == PAGER_FULLFSYNC
**      DBSQL_CkptFullFSync == PAGER_CKPT_FULLFSYNC
**      DBSQL_CacheSpill    == PAGER_CACHE_SPILL
*/
#define DBSQL_WriteSchema    0x00000001  /* OK to update DBSQL_SCHEMA */
#define DBSQL_LegacyFileFmt  0x00000002  /* Create new databases in format 1 */
#define DBSQL_FullColNames   0x00000004  /* Show full column names on SELECT */
#define DBSQL_FullFSync      0x00000008  /* Use full fsync on the backend */
#define DBSQL_CkptFullFSync  0x00000010  /* Use full fsync for checkpoint */
#define DBSQL_CacheSpill     0x00000020  /* OK to spill pager cache */
#define DBSQL_ShortColNames  0x00000040  /* Show short columns names */
#define DBSQL_TrustedSchema  0x00000080  /* Allow unsafe functions and
                                          ** vtabs in the schema definition */
#define DBSQL_NullCallback   0x00000100  /* Invoke the callback once if the */
                                          /*   result set is empty */
#define DBSQL_IgnoreChecks   0x00000200  /* Do not enforce check constraints */
#define DBSQL_StmtScanStatus 0x00000400  /* Enable stmt_scanstats() counters */
#define DBSQL_NoCkptOnClose  0x00000800  /* No checkpoint on close()/DETACH */
#define DBSQL_ReverseOrder   0x00001000  /* Reverse unordered SELECTs */
#define DBSQL_RecTriggers    0x00002000  /* Enable recursive triggers */
#define DBSQL_ForeignKeys    0x00004000  /* Enforce foreign key constraints  */
#define DBSQL_AutoIndex      0x00008000  /* Enable automatic indexes */
#define DBSQL_LoadExtension  0x00010000  /* Enable load_extension */
#define DBSQL_LoadExtFunc    0x00020000  /* Enable load_extension() SQL func */
#define DBSQL_EnableTrigger  0x00040000  /* True to enable triggers */
#define DBSQL_DeferFKs       0x00080000  /* Defer all FK constraints */
#define DBSQL_QueryOnly      0x00100000  /* Disable database changes */
#define DBSQL_CellSizeCk     0x00200000  /* Check btree cell sizes on load */
#define DBSQL_Fts3Tokenizer  0x00400000  /* Enable fts3_tokenizer(2) */
#define DBSQL_EnableQPSG     0x00800000  /* Query Planner Stability Guarantee*/
#define DBSQL_TriggerEQP     0x01000000  /* Show trigger EXPLAIN QUERY PLAN */
#define DBSQL_ResetDatabase  0x02000000  /* Reset the database */
#define DBSQL_LegacyAlter    0x04000000  /* Legacy ALTER TABLE behaviour */
#define DBSQL_NoSchemaError  0x08000000  /* Do not report schema parse errors*/
#define DBSQL_Defensive      0x10000000  /* Input SQL is likely hostile */
#define DBSQL_DqsDDL         0x20000000  /* dbl-quoted strings allowed in DDL*/
#define DBSQL_DqsDML         0x40000000  /* dbl-quoted strings allowed in DML*/
#define DBSQL_EnableView     0x80000000  /* Enable the use of views */
#define DBSQL_CountRows      HI(0x00001) /* Count rows changed by INSERT, */
                                          /*   DELETE, or UPDATE and return */
                                          /*   the count using a callback. */
#define DBSQL_CorruptRdOnly  HI(0x00002) /* Prohibit writes due to error */
#define DBSQL_ReadUncommit   HI(0x00004) /* READ UNCOMMITTED in shared-cache */
#define DBSQL_FkNoAction     HI(0x00008) /* Treat all FK as NO ACTION */
#define DBSQL_AttachCreate   HI(0x00010) /* ATTACH allowed to create new dbs */
#define DBSQL_AttachWrite    HI(0x00020) /* ATTACH allowed to open for write */
#define DBSQL_Comments       HI(0x00040) /* Enable SQL comments */

/* Flags used only if debugging */
#ifdef DBSQL_DEBUG
#define DBSQL_SqlTrace       HI(0x0100000) /* Debug print SQL as it executes */
#define DBSQL_VdbeListing    HI(0x0200000) /* Debug listings of VDBE progs */
#define DBSQL_VdbeTrace      HI(0x0400000) /* True to trace VDBE execution */
#define DBSQL_VdbeAddopTrace HI(0x0800000) /* Trace __vdbe_add_op() calls */
#define DBSQL_VdbeEQP        HI(0x1000000) /* Debug EXPLAIN QUERY PLAN */
#define DBSQL_ParserTrace    HI(0x2000000) /* PRAGMA parser_trace=ON */
#endif

/*
** Allowed values for DBSQL.m_db_flags
*/
#define DBFLAG_SchemaChange   0x0001  /* Uncommitted hash_t table changes */
#define DBFLAG_PreferBuiltin  0x0002  /* Preference to built-in funcs */
#define DBFLAG_Vacuum         0x0004  /* Currently in a VACUUM */
#define DBFLAG_VacuumInto     0x0008  /* Currently running VACUUM INTO */
#define DBFLAG_SchemaKnownOk  0x0010  /* schema_t is known to be valid */
#define DBFLAG_InternalFunc   0x0020  /* Allow use of internal functions */
#define DBFLAG_EncodingFixed  0x0040  /* No longer possible to change enc. */

/*
** Bits of the DBSQL.db_opt_flags field that are used by the
** dbsql_test_control(DBSQL_TESTCTRL_OPTIMIZATIONS,...) interface to
** selectively disable various optimizations.
*/
#define DBSQL_QueryFlattener 0x00000001 /* Query flattening */
#define DBSQL_WindowFunc     0x00000002 /* Use inverse for window functions */
#define DBSQL_GroupByOrder   0x00000004 /* GROUPBY cover of ORDERBY */
#define DBSQL_FactorOutConst 0x00000008 /* Constant factoring */
#define DBSQL_DistinctOpt    0x00000010 /* DISTINCT using indexes */
#define DBSQL_CoverIdxScan   0x00000020 /* Covering index scans */
#define DBSQL_OrderByIdxJoin 0x00000040 /* ORDER BY of joins via index */
#define DBSQL_Transitive     0x00000080 /* Transitive constraints */
#define DBSQL_OmitNoopJoin   0x00000100 /* Omit unused tables in joins */
#define DBSQL_CountOfView    0x00000200 /* The count-of-view optimization */
#define DBSQL_CursorHints    0x00000400 /* Add OP_CursorHint opcodes */
#define DBSQL_Stat4          0x00000800 /* Use STAT4 data */
   /* TH3 expects this value  ^^^^^^^^^^ to be 0x0000800. Don't change it */
#define DBSQL_PushDown       0x00001000 /* WHERE-clause push-down opt */
#define DBSQL_SimplifyJoin   0x00002000 /* Convert LEFT JOIN to JOIN */
#define DBSQL_SkipScan       0x00004000 /* Skip-scans */
#define DBSQL_PropagateConst 0x00008000 /* The constant propagation opt */
#define DBSQL_MinMaxOpt      0x00010000 /* The min/max optimization */
#define DBSQL_SeekScan       0x00020000 /* The OP_SeekScan optimization */
#define DBSQL_OmitOrderBy    0x00040000 /* Omit pointless ORDER BY */
   /* TH3 expects this value  ^^^^^^^^^^ to be 0x40000. Coordinate any change */
#define DBSQL_BloomFilter    0x00080000 /* Use a Bloom filter on searches */
#define DBSQL_BloomPulldown  0x00100000 /* Run Bloom filters early */
#define DBSQL_BalancedMerge  0x00200000 /* Balance multi-way merges */
#define DBSQL_ReleaseReg     0x00400000 /* Use OP_ReleaseReg for testing */
#define DBSQL_FlttnUnionAll  0x00800000 /* Disable the UNION ALL flattener */
   /* TH3 expects this value  ^^^^^^^^^^ See flatten04.test */
#define DBSQL_IndexedExpr    0x01000000 /* Pull exprs from index when able */
#define DBSQL_Coroutines     0x02000000 /* Co-routines for subqueries */
#define DBSQL_NullUnusedCols 0x04000000 /* NULL unused columns in subqueries */
#define DBSQL_OnePass        0x08000000 /* Single-pass DELETE and UPDATE */
#define DBSQL_OrderBySubq    0x10000000 /* ORDER BY in subquery helps outer */
#define DBSQL_StarQuery      0x20000000 /* Heurists for star queries */
#define DBSQL_ExistsToJoin   0x40000000 /* The EXISTS-to-JOIN optimization */
#define DBSQL_AllOpts        0xffffffff /* All optimizations */

/*
** Macros for testing whether or not optimizations are enabled or disabled.
*/
#define OPTIMIZATION_DISABLED(db, mask)  (((db)->db_opt_flags&(mask))!=0)
#define OPTIMIZATION_ENABLED(db, mask)   (((db)->db_opt_flags&(mask))==0)

/*
** Return true if it OK to factor constant expressions into the initialization
** code. The argument is a parser_t object for the code generator.
*/
#define CONST_FACTOR_OK(P) ((P)->ok_const_factor)

/* Possible values for the DBSQL.open_state field.
** The numbers are randomly selected such that a minimum of three bits must
** change to convert any number to another or to zero
*/
#define DBSQL_STATE_OPEN     0x76  /* Database is open */
#define DBSQL_STATE_CLOSED   0xce  /* Database is closed */
#define DBSQL_STATE_SICK     0xba  /* Error and awaiting close */
#define DBSQL_STATE_BUSY     0x6d  /* Database currently in use */
#define DBSQL_STATE_ERROR    0xd5  /* An DBSQL_MISUSE error occurred */
#define DBSQL_STATE_ZOMBIE   0xa7  /* Close with last statement close */

/*
** Each SQL function is defined by an instance of the following
** structure.  For global built-in functions (ex: substr(), max(), count())
** a pointer to this structure is held in the __builtin_functions object.
** For per-connection application-defined functions, a pointer to this
** structure is held in the db->hashes hash table.
**
** The u.hash field is used by the global built-ins.  The u.destructor
** field is used by per-connection app-def functions.
*/
struct func_def_t {
  int16_t n_arg;            /* Number of arguments.  -1 means unlimited */
  u_int32_t func_flags;       /* Some combination of DBSQL_FUNC_* */
  void *user_data;     /* User data parameter */
  func_def_t *next;      /* Next function with same name */
  void (*s_func)(dbsql_func_t*,int,dbsql_value_t**); /* func or agg-step */
  void (*finalize)(dbsql_func_t*);                  /* Agg finalizer */
  void (*value)(dbsql_func_t*);                     /* Current agg value */
  void (*inverse)(dbsql_func_t*,int,dbsql_value_t**); /* inverse agg-step */
  const char *name;   /* SQL name of the function. */
  union {
    func_def_t *hash;      /* Next with a different name but the same hash */
    func_destructor_t *destructor;   /* Reference counted destructor function */
  } u; /* hash if DBSQL_FUNC_BUILTIN, destructor otherwise */
};

/*
** This structure encapsulates a user-function destructor callback (as
** configured using create_function_v2()) and a reference counter. When
** create_function_v2() is called to create a function with a destructor,
** a single object of this type is allocated. func_destructor_t.n_ref is set to
** the number of func_def_t objects created (either 1 or 3, depending on whether
** or not the specified encoding is DBSQL_ANY). The func_def_t.destructor
** member of each of the new func_def_t objects is set to point to the allocated
** func_destructor_t.
**
** Thereafter, when one of the func_def_t objects is deleted, the reference
** count on this object is decremented. When it reaches 0, the destructor
** is invoked and the func_destructor_t structure freed.
*/
struct func_destructor_t {
  int n_ref;
  void (*destroy)(void *);
  void *user_data;
};

/*
** Possible values for func_def_t.flags.  Note that the _LENGTH and _TYPEOF
** values must correspond to OPFLAG_LENGTHARG and OPFLAG_TYPEOFARG.  And
** DBSQL_FUNC_CONSTANT must be the same as DBSQL_DETERMINISTIC.  There
** are assert() statements in the code to verify this.
**
** Value constraints (enforced via assert()):
**     DBSQL_FUNC_MINMAX      ==  NC_MinMaxAgg      == SF_MinMaxAgg
**     DBSQL_FUNC_ANYORDER    ==  NC_OrderAgg       == SF_OrderByReqd
**     DBSQL_FUNC_LENGTH      ==  OPFLAG_LENGTHARG
**     DBSQL_FUNC_TYPEOF      ==  OPFLAG_TYPEOFARG
**     DBSQL_FUNC_BYTELEN     ==  OPFLAG_BYTELENARG
**     DBSQL_FUNC_CONSTANT    ==  DBSQL_DETERMINISTIC from the API
**     DBSQL_FUNC_DIRECT      ==  DBSQL_DIRECTONLY from the API
**     DBSQL_FUNC_UNSAFE      ==  DBSQL_INNOCUOUS  -- opposite meanings!!!
**     DBSQL_FUNC_ENCMASK   depends on DBSQL_UTF* macros in the API
**
** Note that even though DBSQL_FUNC_UNSAFE and DBSQL_INNOCUOUS have the
** same bit value, their meanings are inverted.  DBSQL_FUNC_UNSAFE is
** used internally and if set means that the function has side effects.
** DBSQL_INNOCUOUS is used by application code and means "not unsafe".
** See multiple instances of tag-20230109-1.
*/
#define DBSQL_FUNC_ENCMASK  0x0003 /* DBSQL_UTF8, DBSQL_UTF16BE or UTF16LE */
#define DBSQL_FUNC_LIKE     0x0004 /* Candidate for the LIKE optimization */
#define DBSQL_FUNC_CASE     0x0008 /* Case-sensitive LIKE-type function */
#define DBSQL_FUNC_EPHEM    0x0010 /* Ephemeral.  Delete with VDBE */
#define DBSQL_FUNC_NEEDCOLL 0x0020 /* __get_func_coll_seq() might be called*/
#define DBSQL_FUNC_LENGTH   0x0040 /* Built-in length() function */
#define DBSQL_FUNC_TYPEOF   0x0080 /* Built-in typeof() function */
#define DBSQL_FUNC_BYTELEN  0x00c0 /* Built-in octet_length() function */
#define DBSQL_FUNC_COUNT    0x0100 /* Built-in count(*) aggregate */
/*                           0x0200 -- available for reuse */
#define DBSQL_FUNC_UNLIKELY 0x0400 /* Built-in unlikely() function */
#define DBSQL_FUNC_CONSTANT 0x0800 /* Constant inputs give a constant output */
#define DBSQL_FUNC_MINMAX   0x1000 /* True for min() and max() aggregates */
#define DBSQL_FUNC_SLOCHNG  0x2000 /* "Slow Change". Value constant during a
                                    ** single query - might change over time */
#define DBSQL_FUNC_TEST     0x4000 /* Built-in testing functions */
#define DBSQL_FUNC_RUNONLY  0x8000 /* Cannot be used by VALUE_FROM_FUNCTION */
#define DBSQL_FUNC_WINDOW   0x00010000 /* Built-in window-only function */
#define DBSQL_FUNC_INTERNAL 0x00040000 /* For use by __nested_parse() only */
#define DBSQL_FUNC_DIRECT   0x00080000 /* Not for use in TRIGGERs or VIEWs */
/* DBSQL_SUBTYPE            0x00100000 // Consumer of subtypes */
#define DBSQL_FUNC_UNSAFE   0x00200000 /* Function has side effects */
#define DBSQL_FUNC_INLINE   0x00400000 /* Functions implemented in-line */
#define DBSQL_FUNC_BUILTIN  0x00800000 /* This is a built-in function */
/*  DBSQL_RESULT_SUBTYPE    0x01000000 // Generator of subtypes */
#define DBSQL_FUNC_ANYORDER 0x08000000 /* count/min/max aggregate */

/* Identifier numbers for each in-line function */
#define INLINEFUNC_coalesce             0
#define INLINEFUNC_implies_nonnull_row  1
#define INLINEFUNC_expr_implies_expr    2
#define INLINEFUNC_expr_compare         3
#define INLINEFUNC_affinity             4
#define INLINEFUNC_iif                  5
#define INLINEFUNC_dbsql_offset        6
#define INLINEFUNC_unlikely            99  /* Default case */

/*
** The following three macros, FUNCTION(), LIKEFUNC() and AGGREGATE() are
** used to create the initializers for the func_def_t structures.
**
**   FUNCTION(name, n_arg, i_arg, nc, func)
**     Used to create a scalar function definition of a function name
**     implemented by C function func that accepts n_arg arguments. The
**     value passed as i_arg is cast to a (void*) and made available
**     as the user-data (dbsql_user_data()) for the function. If
**     argument nc is true, then the DBSQL_FUNC_NEEDCOLL flag is set.
**
**   VFUNCTION(name, n_arg, i_arg, nc, func)
**     Like FUNCTION except it omits the DBSQL_FUNC_CONSTANT flag.
**
**   SFUNCTION(name, n_arg, i_arg, nc, func)
**     Like FUNCTION except it omits the DBSQL_FUNC_CONSTANT flag and
**     adds the DBSQL_DIRECTONLY flag.
**
**   INLINE_FUNC(name, n_arg, i_func_id, m_flags)
**     name is the name of a function that is implemented by in-line
**     byte code rather than by the usual callbacks. The i_func_id
**     parameter determines the function id.  The m_flags parameter is
**     optional DBSQL_FUNC_ flags for this function.
**
**   TEST_FUNC(name, n_arg, i_func_id, m_flags)
**     name is the name of a test-only function implemented by in-line
**     byte code rather than by the usual callbacks. The i_func_id
**     parameter determines the function id.  The m_flags parameter is
**     optional DBSQL_FUNC_ flags for this function.
**
**   DFUNCTION(name, n_arg, i_arg, nc, func)
**     Like FUNCTION except it omits the DBSQL_FUNC_CONSTANT flag and
**     adds the DBSQL_FUNC_SLOCHNG flag.  Used for date & time functions
**     and functions like dbsql_version() that can change, but not during
**     a single query.  The i_arg is ignored.  The user-data is always set
**     to a NULL pointer.  The nc parameter is not used.
**
**   MFUNCTION(name, n_arg, ptr, func)
**     For math-library functions.  ptr is an arbitrary pointer.
**
**   PURE_DATE(name, n_arg, i_arg, nc, func)
**     Used for "pure" date/time functions, this macro is like DFUNCTION
**     except that it does set the DBSQL_FUNC_CONSTANT flags.  i_arg is
**     ignored and the user-data for these functions is set to an
**     arbitrary non-NULL pointer.  The nc parameter is not used.
**
**   AGGREGATE(name, n_arg, i_arg, nc, step, final)
**     Used to create an aggregate function definition implemented by
**     the C functions step and final. The first four parameters
**     are interpreted in the same way as the first 4 parameters to
**     FUNCTION().
**
**   WAGGREGATE(name, n_arg, i_arg, step, final, value, inverse)
**     Used to create an aggregate function definition implemented by
**     the C functions step and final. The first four parameters
**     are interpreted in the same way as the first 4 parameters to
**     FUNCTION().
**
**   LIKEFUNC(name, n_arg, arg, flags)
**     Used to create a scalar function definition of a function name
**     that accepts n_arg arguments and is implemented by a call to C
**     function __like_func. Argument arg is cast to a (void *) and made
**     available as the function user-data (dbsql_user_data()). The
**     func_def_t.flags variable is set to the value passed as the flags
**     parameter.
*/
#define FUNCTION(name, n_arg, i_arg, nc, func) \
  {n_arg, DBSQL_FUNC_BUILTIN|\
   DBSQL_FUNC_CONSTANT|DBSQL_UTF8|(nc*DBSQL_FUNC_NEEDCOLL), \
   DBSQL_INT_TO_PTR(i_arg), 0, func, 0, 0, 0, #name, {0} }
#define VFUNCTION(name, n_arg, i_arg, nc, func) \
  {n_arg, DBSQL_FUNC_BUILTIN|DBSQL_UTF8|(nc*DBSQL_FUNC_NEEDCOLL), \
   DBSQL_INT_TO_PTR(i_arg), 0, func, 0, 0, 0, #name, {0} }
#define SFUNCTION(name, n_arg, i_arg, nc, func) \
  {n_arg, DBSQL_FUNC_BUILTIN|DBSQL_UTF8|DBSQL_DIRECTONLY|DBSQL_FUNC_UNSAFE, \
   DBSQL_INT_TO_PTR(i_arg), 0, func, 0, 0, 0, #name, {0} }
#define MFUNCTION(name, n_arg, ptr, func) \
  {n_arg, DBSQL_FUNC_BUILTIN|DBSQL_FUNC_CONSTANT|DBSQL_UTF8, \
   ptr, 0, func, 0, 0, 0, #name, {0} }
#define JFUNCTION(name, n_arg, use_cache, ws, rs, json_b, i_arg, func) \
  {n_arg, DBSQL_FUNC_BUILTIN|DBSQL_DETERMINISTIC|DBSQL_FUNC_CONSTANT|\
   DBSQL_UTF8|((use_cache)*DBSQL_FUNC_RUNONLY)|\
   ((rs)*DBSQL_SUBTYPE)|((ws)*DBSQL_RESULT_SUBTYPE), \
   DBSQL_INT_TO_PTR(i_arg|((json_b)*JSON_BLOB)),0,func,0, 0, 0, #name, {0} }
#define INLINE_FUNC(name, n_arg, i_arg, m_flags) \
  {n_arg, DBSQL_FUNC_BUILTIN|\
   DBSQL_UTF8|DBSQL_FUNC_INLINE|DBSQL_FUNC_CONSTANT|(m_flags), \
   DBSQL_INT_TO_PTR(i_arg), 0, NOOP_FUNC, 0, 0, 0, #name, {0} }
#define TEST_FUNC(name, n_arg, i_arg, m_flags) \
  {n_arg, DBSQL_FUNC_BUILTIN|\
         DBSQL_UTF8|DBSQL_FUNC_INTERNAL|DBSQL_FUNC_TEST| \
         DBSQL_FUNC_INLINE|DBSQL_FUNC_CONSTANT|(m_flags), \
   DBSQL_INT_TO_PTR(i_arg), 0, NOOP_FUNC, 0, 0, 0, #name, {0} }
#define DFUNCTION(name, n_arg, i_arg, nc, func) \
  {n_arg, DBSQL_FUNC_BUILTIN|DBSQL_FUNC_SLOCHNG|DBSQL_UTF8, \
   0, 0, func, 0, 0, 0, #name, {0} }
#define PURE_DATE(name, n_arg, i_arg, nc, func) \
  {n_arg, DBSQL_FUNC_BUILTIN|\
         DBSQL_FUNC_SLOCHNG|DBSQL_UTF8|DBSQL_FUNC_CONSTANT, \
   (void*)&__config, 0, func, 0, 0, 0, #name, {0} }
#define FUNCTION2(name, n_arg, i_arg, nc, func, extra_flags) \
  {n_arg, DBSQL_FUNC_BUILTIN|\
   DBSQL_FUNC_CONSTANT|DBSQL_UTF8|(nc*DBSQL_FUNC_NEEDCOLL)|extra_flags,\
   DBSQL_INT_TO_PTR(i_arg), 0, func, 0, 0, 0, #name, {0} }
#define STR_FUNCTION(name, n_arg, arg, nc, func) \
  {n_arg, DBSQL_FUNC_BUILTIN|\
   DBSQL_FUNC_SLOCHNG|DBSQL_UTF8|(nc*DBSQL_FUNC_NEEDCOLL), \
   arg, 0, func, 0, 0, 0, #name, {0} }
#define LIKEFUNC(name, n_arg, arg, flags) \
  {n_arg, DBSQL_FUNC_BUILTIN|DBSQL_FUNC_CONSTANT|DBSQL_UTF8|flags, \
   (void *)arg, 0, __like_func, 0, 0, 0, #name, {0} }
#define WAGGREGATE(name, n_arg, arg, nc, step, final, value, inverse, f) \
  {n_arg, DBSQL_FUNC_BUILTIN|DBSQL_UTF8|(nc*DBSQL_FUNC_NEEDCOLL)|f, \
   DBSQL_INT_TO_PTR(arg), 0, step,final,value,inverse,#name, {0}}
#define INTERNAL_FUNCTION(name, n_arg, func) \
  {n_arg, DBSQL_FUNC_BUILTIN|\
   DBSQL_FUNC_INTERNAL|DBSQL_UTF8|DBSQL_FUNC_CONSTANT, \
   0, 0, func, 0, 0, 0, #name, {0} }


/*
** All current savepoints are stored in a linked list starting at
** DBSQL.savepoint. The first element in the list is the most recently
** opened savepoint. Savepoints are added to the list by the vdbe
** OP_Savepoint instruction.
*/
struct savepoint_t {
  char *name;                        /* savepoint_t name (nul-terminated) */
  int64_t n_deferred_cons;                  /* Number of deferred fk violations */
  int64_t n_deferred_imm_cons;               /* Number of deferred imm fk. */
  savepoint_t *next;                   /* Parent savepoint (if any) */
};

/*
** The following are used as the second parameter to __savepoint(),
** and as the P1 argument to the OP_Savepoint instruction.
*/
#define SAVEPOINT_BEGIN      0
#define SAVEPOINT_RELEASE    1
#define SAVEPOINT_ROLLBACK   2


/*
** Each dbsql module (virtual table definition) is defined by an
** instance of the following structure, stored in the DBSQL.modules
** hash table.
*/
struct module_def_t {
  const dbsql_module_t *module;       /* Callback pointers */
  const char *name;                   /* Name passed to create_module() */
  int n_ref_module;                      /* Number of pointers to this object */
  void *aux;                          /* aux passed to create_module() */
  void (*destroy)(void *);            /* module_def_t destructor function */
  table_t *epo_tab;                      /* Eponymous table for this module */
};

/*
** Information about each column of an SQL table is held in an instance
** of the column_t structure, in the table_t.cols[] array.
**
** Definitions:
**
**   "table column index"     This is the index of the column in the
**                            table_t.cols[] array, and also the index of
**                            the column in the original CREATE TABLE stmt.
**
**   "storage column index"   This is the index of the column in the
**                            record BLOB generated by the OP_MakeRecord
**                            opcode.  The storage column index is less than
**                            or equal to the table column index.  It is
**                            equal if and only if there are no VIRTUAL
**                            columns to the left.
**
** Notes on cn_name:
** The cn_name field stores the name of the column, the datatype of the
** column, and the collating sequence for the column, in that order, all in
** a single allocation.  Each string is 0x00 terminated.  The datatype
** is only included if the COLFLAG_HASTYPE bit of col_flags is set and the
** collating sequence name is only included if the COLFLAG_HASCOLL bit is
** set.
*/
struct column_t {
  char *cn_name;        /* Name of this column */
  unsigned not_null :4;  /* An OE_ code for handling a NOT NULL constraint */
  unsigned c_type :4;   /* One of the standard types */
  char affinity;        /* One of the DBSQL_AFF_... values */
  u_int8_t sz_est;             /* Est size of value in this column. sizeof(INT)==1 */
  u_int8_t h_name;             /* column_t name hash for faster lookup */
  u_int16_t i_dflt;            /* 1-based index of DEFAULT.  0 means "none" */
  u_int16_t col_flags;         /* Boolean properties.  See COLFLAG_ defines below */
};

/* Allowed values for column_t.c_type.
**
** Values must match entries in the global constant arrays
** __std_type_len[] and __std_type[].  Each value is one more
** than the offset into these arrays for the corresponding name.
** Adjust the DBSQL_N_STDTYPE value if adding or removing entries.
*/
#define COLTYPE_CUSTOM      0   /* Type appended to name */
#define COLTYPE_ANY         1
#define COLTYPE_BLOB        2
#define COLTYPE_INT         3
#define COLTYPE_INTEGER     4
#define COLTYPE_REAL        5
#define COLTYPE_TEXT        6
#define DBSQL_N_STDTYPE    6  /* Number of standard types */

/* Allowed values for column_t.col_flags.
**
** Constraints:
**         TF_HasVirtual == COLFLAG_VIRTUAL
**         TF_HasStored  == COLFLAG_STORED
**         TF_HasHidden  == COLFLAG_HIDDEN
*/
#define COLFLAG_PRIMKEY   0x0001   /* column_t is part of the primary key */
#define COLFLAG_HIDDEN    0x0002   /* A hidden column in a virtual table */
#define COLFLAG_HASTYPE   0x0004   /* Type name follows column name */
#define COLFLAG_UNIQUE    0x0008   /* column_t def contains "UNIQUE" or "PK" */
#define COLFLAG_SORTERREF 0x0010   /* Use sorter-refs with this column */
#define COLFLAG_VIRTUAL   0x0020   /* GENERATED ALWAYS AS ... VIRTUAL */
#define COLFLAG_STORED    0x0040   /* GENERATED ALWAYS AS ... STORED */
#define COLFLAG_NOTAVAIL  0x0080   /* STORED column not yet calculated */
#define COLFLAG_BUSY      0x0100   /* Blocks recursion on GENERATED columns */
#define COLFLAG_HASCOLL   0x0200   /* Has collating sequence name in cn_name */
#define COLFLAG_NOEXPAND  0x0400   /* Omit this column when expanding "*" */
#define COLFLAG_GENERATED 0x0060   /* Combo: _STORED, _VIRTUAL */
#define COLFLAG_NOINSERT  0x0062   /* Combo: _HIDDEN, _STORED, _VIRTUAL */

/*
** A "Collating Sequence" is defined by an instance of the following
** structure. Conceptually, a collating sequence consists of a name and
** a comparison routine that defines the order of that sequence.
**
** If coll_seq_t.cmp is NULL, it means that the
** collating sequence is undefined.  Indices built on an undefined
** collating sequence may not be read or written.
*/
struct coll_seq_t {
  char *name;          /* Name of the collating sequence, UTF-8 encoded */
  u_int8_t enc;               /* Text encoding handled by cmp() */
  void *user;          /* First argument to cmp() */
  int (*cmp)(void*,int, const void*, int, const void*);
  void (*x_del)(void*);  /* Destructor for user */
};

/*
** A sort order can be either ASC or DESC.
*/
#define DBSQL_SO_ASC       0  /* Sort in ascending order */
#define DBSQL_SO_DESC      1  /* Sort in ascending order */
#define DBSQL_SO_UNDEFINED -1 /* No sort order specified */

/*
** column_t affinity types.
**
** These used to have mnemonic name like 'i' for DBSQL_AFF_INTEGER and
** 't' for DBSQL_AFF_TEXT.  But we can save a little space and improve
** the speed a little by numbering the values consecutively.
**
** But rather than start with 0 or 1, we begin with 'A'.  That way,
** when multiple affinity types are concatenated into a string and
** used as the P4 operand, they will be more readable.
**
** Note also that the numeric types are grouped together so that testing
** for a numeric type is a single comparison.  And the BLOB type is first.
*/
#define DBSQL_AFF_NONE     0x40  /* '@' */
#define DBSQL_AFF_BLOB     0x41  /* 'A' */
#define DBSQL_AFF_TEXT     0x42  /* 'B' */
#define DBSQL_AFF_NUMERIC  0x43  /* 'C' */
#define DBSQL_AFF_INTEGER  0x44  /* 'D' */
#define DBSQL_AFF_REAL     0x45  /* 'E' */
#define DBSQL_AFF_FLEXNUM  0x46  /* 'F' */
#define DBSQL_AFF_DEFER    0x58  /* 'X'  - defer computation until later */

#define __is_numeric_affinity(X)  ((X)>=DBSQL_AFF_NUMERIC)

/*
** The DBSQL_AFF_MASK values masks off the significant bits of an
** affinity value.
*/
#define DBSQL_AFF_MASK     0x47

/*
** Additional bit values that can be ORed with an affinity without
** changing the affinity.
**
** The DBSQL_NOTNULL flag is a combination of NULLEQ and JUMPIFNULL.
** It causes an assert() to fire if either operand to a comparison
** operator is NULL.  It is added to certain comparison operators to
** prove that the operands are always NOT NULL.
*/
#define DBSQL_JUMPIFNULL   0x10  /* jumps if either operand is NULL */
#define DBSQL_NULLEQ       0x80  /* NULL=NULL */
#define DBSQL_NOTNULL      0x90  /* Assert that operands are never NULL */

/*
** An object of this type is created for each virtual table present in
** the database schema.
**
** If the database schema is shared, then there is one instance of this
** structure for each database connection (DBSQL*) that uses the shared
** schema. This is because each database connection requires its own unique
** instance of the dbsql_vtab_t* handle used to access the virtual table
** implementation. dbsql_vtab_t* handles can not be shared between
** database connections, even when the rest of the in-memory database
** schema is shared, as the implementation often stores the database
** connection handle passed to it via the connect() or create() method
** during initialization internally. This database connection handle may
** then be used by the virtual table implementation to access real tables
** within the database. So that they appear as part of the callers
** transaction, these accesses need to be made via the same database
** connection as that used to execute SQL operations on the virtual table.
**
** All v_table_t objects that correspond to a single table in a shared
** database schema are initially stored in a linked-list pointed to by
** the table_t.v_table member variable of the corresponding table_t object.
** When an dbsql_prepare() operation is required to access the virtual
** table, it searches the list for the v_table_t that corresponds to the
** database connection doing the preparing so as to use the correct
** dbsql_vtab_t* handle in the compiled query.
**
** When an in-memory table_t object is deleted (for example when the
** schema is being reloaded for some reason), the v_table_t objects are not
** deleted and the dbsql_vtab_t* handles are not disconnect()ed
** immediately. Instead, they are moved from the table_t.v_table list to
** another linked list headed by the DBSQL.disconnect member of the
** corresponding DBSQL structure. They are then deleted/disconnected
** next time a statement is prepared using said DBSQL*. This is done
** to avoid deadlock issues involving multiple DBSQL.mutex mutexes.
** Refer to comments above function __vtab_unlock_list() for an
** explanation as to why it is safe to add an entry to an DBSQL.disconnect
** list without holding the corresponding DBSQL.mutex mutex.
**
** The memory for objects of this type is always allocated by
** __db_malloc(), using the connection handle stored in v_table_t.db as
** the first argument.
*/
struct v_table_t {
  DBSQL *db;              /* Database connection associated with this table */
  module_def_t *mod;             /* Pointer to module implementation */
  dbsql_vtab_t *vtab;      /* Pointer to vtab instance */
  int n_ref;                 /* Number of pointers to this structure */
  u_int8_t constraint;           /* True if constraints are supported */
  u_int8_t all_schemas;           /* True if might use any attached schema */
  u_int8_t vtab_risk;             /* Riskiness of allowing hacker access */
  int i_savepoint;           /* Depth of the SAVEPOINT stack */
  v_table_t *next;            /* Next in linked list (see above) */
};

/* Allowed values for v_table_t.vtab_risk
*/
#define DBSQL_VTABRISK_Low          0
#define DBSQL_VTABRISK_Normal       1
#define DBSQL_VTABRISK_High         2

/*
** The schema for each SQL table, virtual table, and view is represented
** in memory by an instance of the following structure.
*/
struct table_t {
  char *name;         /* Name of the table or view */
  column_t *cols;        /* Information about each column */
  index_t *p_index;       /* List of SQL indexes on this table. */
  char *col_aff;       /* String defining the affinity of each column */
  expr_list_t *check;    /* All CHECK constraints */
                       /*   ... also used as column name list in a VIEW */
  pgno_t tnum;           /* Root BTree page for this table */
  u_int32_t n_tab_ref;         /* Number of pointers to this table_t */
  u_int32_t tab_flags;        /* Mask of TF_* values */
  int16_t i_p_key;           /* If not negative, use cols[i_p_key] as the rowid */
  int16_t n_col;            /* Number of columns in this table */
  int16_t n_nv_col;          /* Number of columns that are not VIRTUAL */
  log_est_t n_row_log_est;   /* Estimated rows in table - from dbsql_stat1 table */
  log_est_t sz_tab_row;     /* Estimated size of each table row in bytes */
#ifdef DBSQL_ENABLE_COSTMULT
  log_est_t cost_mult;     /* Cost multiplier for using this table */
#endif
  u_int8_t key_conf;          /* What to do in case of uniqueness conflict on i_p_key */
  u_int8_t tab_type;         /* 0: normal, 1: virtual, 2: view */
  union {
    struct {             /* Used by ordinary tables: */
      int add_col_offset;    /* Offset in CREATE TABLE stmt to add a new column */
      f_key_t *f_key;         /* Linked list of all foreign keys in this table */
      expr_list_t *dflt_list; /* DEFAULT clauses on various columns.
                           ** Or the AS clause for generated columns. */
    } tab;
    struct {             /* Used by views: */
      select_t *p_select;     /* View definition */
    } view;
    struct {             /* Used by virtual tables only: */
      int n_arg;            /* Number of arguments to the module */
      char **args;        /* 0: module 1: schema 2: vtab name 3...: args */
      v_table_t *p;           /* List of v_table_t objects. */
    } vtab;
  } u;
  trigger_t *P_TRIGGER;   /* List of triggers on this object */
  schema_t *schema;     /* schema_t that contains this table */
  u_int8_t hxes[16];          /* column_t hts[K%sizeof(hts)] might have hash K */
};

/*
** Allowed values for table_t.tab_flags.
**
** TF_OOOHidden applies to tables or view that have hidden columns that are
** followed by non-hidden columns.  Example:  "CREATE VIRTUAL TABLE x USING
** vtab1(a HIDDEN, b);".  Since "b" is a non-hidden column but "a" is hidden,
** the TF_OOOHidden attribute would apply in this case.  Such tables require
** special handling during INSERT processing. The "OOO" means "Out Of Order".
**
** Constraints:
**
**         TF_HasVirtual == COLFLAG_VIRTUAL
**         TF_HasStored  == COLFLAG_STORED
**         TF_HasHidden  == COLFLAG_HIDDEN
*/
#define TF_Readonly       0x00000001 /* Read-only system table */
#define TF_HasHidden      0x00000002 /* Has one or more hidden columns */
#define TF_HasPrimaryKey  0x00000004 /* table_t has a primary key */
#define TF_Autoincrement  0x00000008 /* Integer primary key is autoincrement */
#define TF_HasStat1       0x00000010 /* n_row_log_est set from dbsql_stat1 */
#define TF_HasVirtual     0x00000020 /* Has one or more VIRTUAL columns */
#define TF_HasStored      0x00000040 /* Has one or more STORED columns */
#define TF_HasGenerated   0x00000060 /* Combo: __has_virtual + __has_stored */
#define TF_WithoutRowid   0x00000080 /* No rowid.  PRIMARY KEY is the key */
#define TF_MaybeReanalyze 0x00000100 /* Maybe run ANALYZE on this table */
#define TF_NoVisibleRowid 0x00000200 /* No user-visible "rowid" column */
#define TF_OOOHidden      0x00000400 /* Out-of-Order hidden columns */
#define TF_HasNotNull     0x00000800 /* Contains NOT NULL constraints */
#define TF_Shadow         0x00001000 /* True for a shadow table */
#define TF_HasStat4       0x00002000 /* STAT4 info available for this table */
#define TF_Ephemeral      0x00004000 /* An ephemeral table */
#define TF_Eponymous      0x00008000 /* An eponymous virtual table */
#define TF_Strict         0x00010000 /* STRICT mode */
#define TF_Imposter       0x00020000 /* An imposter table */

/*
** Allowed values for table_t.tab_type
*/
#define TABTYP_NORM      0     /* Ordinary table */
#define TABTYP_VTAB      1     /* Virtual table */
#define TABTYP_VIEW      2     /* A view */

#define IsView(X)           ((X)->tab_type==TABTYP_VIEW)
#define IS_ORDINARY_TABLE(X)  ((X)->tab_type==TABTYP_NORM)

/*
** Test to see whether or not a table is a virtual table.  This is
** done as a macro so that it will be optimized out when virtual
** table support is omitted from the build.
*/
#ifndef DBSQL_OMIT_VIRTUALTABLE
#  define IS_VIRTUAL(X)      ((X)->tab_type==TABTYP_VTAB)
#  define EXPR_IS_VTAB(X)  \
   ((X)->op==TK_COLUMN && (X)->y.tab->tab_type==TABTYP_VTAB)
#else
#  define IS_VIRTUAL(X)      0
#  define EXPR_IS_VTAB(X)     0
#endif

/*
** Macros to determine if a column is hidden.  IS_ORDINARY_HIDDEN_COLUMN()
** only works for non-virtual tables (ordinary tables and views) and is
** always false unless DBSQL_ENABLE_HIDDEN_COLUMNS is defined.  The
** IS_HIDDEN_COLUMN() macro is general purpose.
*/
#if defined(DBSQL_ENABLE_HIDDEN_COLUMNS)
#  define IS_HIDDEN_COLUMN(X)         (((X)->col_flags & COLFLAG_HIDDEN)!=0)
#  define IS_ORDINARY_HIDDEN_COLUMN(X) (((X)->col_flags & COLFLAG_HIDDEN)!=0)
#elif !defined(DBSQL_OMIT_VIRTUALTABLE)
#  define IS_HIDDEN_COLUMN(X)         (((X)->col_flags & COLFLAG_HIDDEN)!=0)
#  define IS_ORDINARY_HIDDEN_COLUMN(X) 0
#else
#  define IS_HIDDEN_COLUMN(X)         0
#  define IS_ORDINARY_HIDDEN_COLUMN(X) 0
#endif


/* Does the table have a rowid */
#define HAS_ROWID(X)     (((X)->tab_flags & TF_WithoutRowid)==0)
#define VISIBLE_ROWID(X) (((X)->tab_flags & TF_NoVisibleRowid)==0)

/* Macro is true if the DBSQL_ALLOW_ROWID_IN_VIEW (mis-)feature is
** available.  By default, this macro is false
*/
#ifndef DBSQL_ALLOW_ROWID_IN_VIEW
# define VIEW_CAN_HAVE_ROWID     0
#else
# define VIEW_CAN_HAVE_ROWID     (__config.m_no_visible_rowid==0)
#endif

/*
** Each foreign key constraint is an instance of the following structure.
**
** A foreign key is associated with two tables.  The "from" table is
** the table that contains the REFERENCES clause that creates the foreign
** key.  The "to" table is the table that is named in the REFERENCES clause.
** Consider this example:
**
**     CREATE TABLE ex1(
**       a INTEGER PRIMARY KEY,
**       b INTEGER CONSTRAINT fk1 REFERENCES ex2(x)
**     );
**
** For foreign key "fk1", the from-table is "ex1" and the to-table is "ex2".
** Equivalent names:
**
**     from-table == child-table
**       to-table == parent-table
**
** Each REFERENCES clause generates an instance of the following structure
** which is attached to the from-table.  The to-table need not exist when
** the from-table is created.  The existence of the to-table is not checked.
**
** The list of all parents for child table_t X is held at X.f_key.
**
** A list of all children for a table named Z (which might not even exist)
** is held in schema_t.fkey_hash with a hash key of Z.
*/
struct f_key_t {
  table_t *from;     /* table_t containing the REFERENCES clause (aka: Child) */
  f_key_t *next_from;  /* Next f_key_t with the same in from. Next parent of from */
  char *to;        /* Name of table that the key points to (aka: Parent) */
  f_key_t *next_to;    /* Next with the same to. Next child of to. */
  f_key_t *prev_to;    /* Previous with the same to */
  int n_col;         /* Number of columns in this key */
  /* EV: R-30323-21917 */
  u_int8_t is_deferred;       /* True if constraint checking is deferred till COMMIT */
  u_int8_t actions[2];        /* ON DELETE and ON UPDATE actions, respectively */
  trigger_t *triggers[2];/* Triggers for actions[] actions */
  struct s_col_map {      /* Mapping of columns in from to columns in to */
    int i_from;            /* index_t of column in from */
    char *col;           /* Name of column in to.  If NULL use PRIMARY KEY */
  } cols[FLEXARRAY];      /* One entry for each of n_col columns */
};

/* The size (in bytes) of an f_key_t object holding N columns.  The answer
** does NOT include space to hold the to name. */
#define SZ_FKEY(N)  (offsetof(f_key_t,cols)+(N)*sizeof(struct s_col_map))

/*
** dbsql supports many different ways to resolve a constraint
** error.  ROLLBACK processing means that a constraint violation
** causes the operation in process to fail and for the current transaction
** to be rolled back.  ABORT processing means the operation in process
** fails and any prior changes from that one operation are backed out,
** but the transaction is not rolled back.  FAIL processing means that
** the operation in progress stops and returns an error code.  But prior
** changes due to the same operation are not backed out and no rollback
** occurs.  IGNORE means that the particular row that caused the constraint
** error is not inserted or updated.  Processing continues and no error
** is returned.  REPLACE means that preexisting database rows that caused
** a UNIQUE constraint violation are removed so that the new insert or
** update can proceed.  Processing continues and no error is reported.
** UPDATE applies to insert operations only and means that the insert
** is omitted and the DO UPDATE clause of an upsert is run instead.
**
** RESTRICT, SETNULL, SETDFLT, and CASCADE actions apply only to foreign keys.
** RESTRICT is the same as ABORT for IMMEDIATE foreign keys and the
** same as ROLLBACK for DEFERRED keys.  SETNULL means that the foreign
** key is set to NULL.  SETDFLT means that the foreign key is set
** to its default value.  CASCADE means that a DELETE or UPDATE of the
** referenced table row is propagated into the row that holds the
** foreign key.
**
** The OE_Default value is a place holder that means to use whatever
** conflict resolution algorithm is required from context.
**
** The following symbolic values are used to record which type
** of conflict resolution action to take.
*/
#define OE_None     0   /* There is no constraint to check */
#define OE_Rollback 1   /* Fail the operation and rollback the transaction */
#define OE_Abort    2   /* Back out changes but do no rollback transaction */
#define OE_Fail     3   /* Stop the operation but leave all prior changes */
#define OE_Ignore   4   /* Ignore the error. Do not do the INSERT or UPDATE */
#define OE_Replace  5   /* Delete existing record, then do INSERT or UPDATE */
#define OE_Update   6   /* Process as a DO UPDATE in an upsert */
#define OE_Restrict 7   /* OE_Abort for IMMEDIATE, OE_Rollback for DEFERRED */
#define OE_SetNull  8   /* Set the foreign key value to NULL */
#define OE_SetDflt  9   /* Set the foreign key value to its default */
#define OE_Cascade  10  /* Cascade the changes */
#define OE_Default  11  /* Do whatever the default action is */


/*
** An instance of the following structure is passed as the first
** argument to __vdbe_key_compare and is used to control the
** comparison of the two index keys.
**
** The sort_orders[] and colls[] arrays have n_all_field slots each. There
** are n_key_field slots for the columns of an index then extra slots
** for the rowid or key at the end.  The sort_orders array is located after
** the colls[] array.
**
** If DBSQL_ENABLE_PREUPDATE_HOOK is defined, then sort_flags might be NULL
** to indicate that this object is for use by a preupdate hook.  When sort_flags
** is NULL, then n_all_field is uninitialized and no space is allocated for
** colls[], so those fields may not be used.
*/
struct key_info_t {
  u_int32_t n_ref;           /* Number of references to this key_info_t object */
  u_int8_t enc;             /* Text encoding - one of the DBSQL_UTF* values */
  u_int16_t n_key_field;      /* Number of key columns in the index */
  u_int16_t n_all_field;      /* Total columns, including key plus others */
  DBSQL *db;        /* The database connection */
  u_int8_t *sort_flags;     /* Sort order for each column. */
  coll_seq_t *colls[FLEXARRAY]; /* Collating sequence for each term of the key */
};

/* The size (in bytes) of a key_info_t object with up to N fields.  This includes
** the main body of the key_info_t object and the colls[] array of N elements,
** but does not count the memory used to hold sort_flags[]. */
#define SZ_KEYINFO(N)  (offsetof(key_info_t,colls) + (N)*sizeof(coll_seq_t*))

/* The size of a bare key_info_t with no colls[] entries */
#if FLEXARRAY+1 > 1
# define SZ_KEYINFO_0   offsetof(key_info_t,colls)
#else
# define SZ_KEYINFO_0   sizeof(key_info_t)
#endif

/*
** Allowed bit values for entries in the key_info_t.sort_flags[] array.
*/
#define KEYINFO_ORDER_DESC    0x01    /* DESC sort order */
#define KEYINFO_ORDER_BIGNULL 0x02    /* NULL is larger than any other value */

/*
** This object holds a record which has been parsed out into individual
** fields, for the purposes of doing a comparison.
**
** A record is an object that contains one or more fields of data.
** Records are used to store the content of a table row and to store
** the key of an index.  A blob encoding of a record is created by
** the OP_MakeRecord opcode of the VDBE and is disassembled by the
** OP_Column opcode.
**
** An instance of this object serves as a "key" for doing a search on
** an index b+tree. The goal of the search is to find the entry that
** is closest to the key described by this object.  This object might hold
** just a prefix of the key.  The number of fields is given by n_field.
**
** The r1 and r2 fields are the values to return if this key is less than
** or greater than a key in the btree, respectively.  These are normally
** -1 and +1 respectively, but might be inverted to +1 and -1 if the b-tree
** is in DESC order.
**
** The key comparison functions actually return default_rc when they find
** an equals comparison.  default_rc can be -1, 0, or +1.  If there are
** multiple entries in the b-tree with the same key (when only looking
** at the first n_field elements) then default_rc can be set to -1 to
** cause the search to find the last match, or +1 to cause the search to
** find the first match.
**
** The key comparison functions will set eq_seen to true if they ever
** get and equal results when comparing this structure to a b-tree record.
** When default_rc!=0, the search might end up on the record immediately
** before the first match or immediately after the last match.  The
** eq_seen field will indicate whether or not an exact match exists in the
** b-tree.
*/
struct unpacked_record_t {
  key_info_t *key_info;  /* Comparison info for the index that is unpacked */
  mem_t *mems;          /* Values for columns of the index */
  union {
    char *z;            /* Cache of mems[0].z for __vdbe_record_compare_string() */
    int64_t i;              /* Cache of mems[0].u.i for __vdbe_record_compare_int() */
  } u;
  int n;              /* Cache of mems[0].n used by __vdbe_record_compare_string() */
  u_int16_t n_field;         /* Number of entries in mems[] */
  int8_t default_rc;      /* Comparison result if keys are equal */
  u_int8_t errCode;         /* Error detected by record_compare (CORRUPT or NOMEM) */
  int8_t r1;              /* Value to return if (lhs < rhs) */
  int8_t r2;              /* Value to return if (lhs > rhs) */
  u_int8_t eq_seen;          /* True if an equality comparison has been seen */
};


/*
** Each SQL index is represented in memory by an
** instance of the following structure.
**
** The columns of the table that are to be indexed are described
** by the columns[] field of this structure.  For example, suppose
** we have the following table and index:
**
**     CREATE TABLE Ex1(c1 int, c2 int, c3 text);
**     CREATE INDEX Ex2 ON Ex1(c3,c1);
**
** In the table_t structure describing Ex1, n_col==3 because there are
** three columns in the table.  In the index_t structure describing
** Ex2, n_column==2 since 2 of the 3 columns of Ex1 are indexed.
** The value of columns is {2, 0}.  columns[0]==2 because the
** first column to be indexed (c3) has an index of 2 in Ex1.cols[].
** The second column to be indexed (c1) has an index of 0 in
** Ex1.cols[], hence Ex2.columns[1]==0.
**
** The index_t.on_error field determines whether or not the indexed columns
** must be unique and what to do if they are not.  When index_t.on_error=OE_None,
** it means this is not a unique index.  Otherwise it is a unique index
** and the value of index_t.on_error indicates which conflict resolution
** algorithm to employ when an attempt is made to insert a non-unique
** element.
**
** The col_not_idxed bitmask is used in combination with src_item_t.col_used
** for a fast test to see if an index can serve as a covering index.
** col_not_idxed has a 1 bit for every column of the original table that
** is *not* available in the index.  Thus the expression
** "col_used & col_not_idxed" will be non-zero if the index is not a
** covering index.  The most significant bit of of col_not_idxed will always
** be true (note-20221022-a).  If a column beyond the 63rd column of the
** table is used, the "col_used & col_not_idxed" test will always be non-zero
** and we have to assume either that the index is not covering, or use
** an alternative (slower) algorithm to determine whether or not
** the index is covering.
**
** While parsing a CREATE TABLE or CREATE INDEX statement in order to
** generate VDBE code (as opposed to parsing one read from an dbsql_schema
** table as part of parsing an existing database schema), transient instances
** of this structure may be created. In this case the index_t.tnum variable is
** used to store the address of a VDBE instruction, not a database page
** number (it cannot - the database page is not allocated until the VDBE
** program is executed). See __convert_to_without_rowid_table() for details.
*/
struct index_t {
  char *name;             /* Name of this index */
  int16_t *columns;           /* Which columns are used by this index.  1st is 0 */
  log_est_t *row_log_ests;     /* From ANALYZE: Est. rows selected by each column */
  table_t *table;           /* The SQL table being indexed */
  char *col_aff;           /* String defining the affinity of each column */
  index_t *next;            /* The next index associated with the same table */
  schema_t *schema;         /* schema_t containing this index */
  u_int8_t *sort_orders;          /* for each column: True==DESC, False==ASC */
  const char **colls;     /* Array of collation sequence names for index */
  expr_t *part_idx_where;     /* WHERE clause for partial indices */
  expr_list_t *col_exprs;      /* column_t expressions */
  pgno_t tnum;               /* DB Page containing root of this index */
  log_est_t sz_idx_row;         /* Estimated average row size in bytes */
  u_int16_t n_key_col;             /* Number of columns forming the key */
  u_int16_t n_column;             /* Nr columns in btree. Can be 2*table_t.n_col */
  u_int8_t on_error;              /* OE_Abort, OE_Ignore, OE_Replace, or OE_None */
  unsigned idx_type:2;      /* 0:Normal 1:UNIQUE, 2:PRIMARY KEY, 3:IPK */
  unsigned unordered:1;   /* Use this index for == or IN queries only */
  unsigned uniq_not_null:1;  /* True if UNIQUE and NOT NULL for all columns */
  unsigned is_resized:1;    /* True if __resize_index_object() has been called */
  unsigned is_covering:1;   /* True if this is a covering index */
  unsigned no_skip_scan:1;   /* Do not try to use skip-scan if true */
  unsigned has_stat1:1;     /* row_log_ests values come from dbsql_stat1 */
  unsigned no_query:1;     /* Do not use this index to optimize queries */
  unsigned asc_key_bug:1;   /* True if the bba7b69f9849b5bf bug applies */
  unsigned has_v_col:1;     /* index_t references one or more VIRTUAL columns */
  unsigned has_expr:1;     /* index_t contains an expression, either a literal
                           ** expression, or a reference to a VIRTUAL column */
#ifdef DBSQL_ENABLE_STAT4
  int n_sample;             /* Number of elements in samples[] */
  int mx_sample;            /* Number of slots allocated to samples[] */
  int n_sample_col;          /* Size of index_sample_t.an_eq[] and so on */
  rowcnt_t *avg_eqs;         /* Average n_eq values for keys not in samples */
  index_sample_t *samples;    /* Samples of the left-most key */
  rowcnt_t *row_ests;       /* Non-logarithmic stat1 data for this index */
  rowcnt_t n_row_est0;        /* Non-logarithmic number of rows in the index */
#endif
  Bitmask col_not_idxed;     /* Unindexed columns in tab */
};

/*
** Allowed values for index_t.idx_type
*/
#define DBSQL_IDXTYPE_APPDEF      0   /* Created using CREATE INDEX */
#define DBSQL_IDXTYPE_UNIQUE      1   /* Implements a UNIQUE constraint */
#define DBSQL_IDXTYPE_PRIMARYKEY  2   /* Is the PRIMARY KEY for the table */
#define DBSQL_IDXTYPE_IPK         3   /* INTEGER PRIMARY KEY index */

/* Return true if index X is a PRIMARY KEY index */
#define IS_PRIMARY_KEY_INDEX(X)  ((X)->idx_type==DBSQL_IDXTYPE_PRIMARYKEY)

/* Return true if index X is a UNIQUE index */
#define IS_UNIQUE_INDEX(X)      ((X)->on_error!=OE_None)

/* The index_t.columns[] values are normally positive integer.  But
** there are some negative values that have special meaning:
*/
#define XN_ROWID     (-1)     /* Indexed column is the rowid */
#define XN_EXPR      (-2)     /* Indexed column is an expression */

/*
** Each sample stored in the dbsql_stat4 table is represented in memory
** using a structure of this type.  See documentation at the top of the
** analyze.c source file for additional information.
*/
struct index_sample_t {
  void *p;          /* Pointer to sampled record */
  int n;            /* Size of record in bytes */
  rowcnt_t *an_eq;    /* Est. number of rows where the key equals this sample */
  rowcnt_t *an_lt;    /* Est. number of rows where key is less than this sample */
  rowcnt_t *an_d_lt;   /* Est. number of distinct keys less than this sample */
};

/*
** Possible values to use within the flags argument to __get_token().
*/
#define DBSQL_TOKEN_QUOTED    0x1 /* Token is a quoted identifier. */
#define DBSQL_TOKEN_KEYWORD   0x2 /* Token is a keyword. */

/*
** Each token coming out of the lexer is an instance of
** this structure.  Tokens are also used as part of an expression.
**
** The memory that "z" points to is owned by other objects.  Take care
** that the owner of the "z" string does not deallocate the string before
** the Token goes out of scope!  Very often, the "z" points to some place
** in the middle of the parser_t.sql text.  But it might also point to a
** static string.
*/
struct Token {
  const char *z;     /* Text of the token.  Not NULL-terminated! */
  unsigned int n;    /* Number of characters in this token */
};

/*
** An instance of this structure contains information needed to generate
** code for a SELECT that contains aggregate functions.
**
** If expr_t.op==TK_AGG_COLUMN or TK_AGG_FUNCTION then expr_t.agg_info is a
** pointer to this structure.  The expr_t.i_agg field is the index in
** agg_info_t.cols[] or agg_info_t.funcs[] of information needed to generate
** code for that node.
**
** agg_info_t.group_by and agg_info_t.funcs.expr point to fields within the
** original select_t structure that describes the SELECT statement.  These
** fields do not need to be freed when deallocating the agg_info_t structure.
*/
struct agg_info_t {
  u_int8_t direct_mode;          /* Direct rendering mode means take data directly
                          ** from source tables rather than from accumulators */
  u_int8_t use_sorting_idx;       /* In direct mode, reference the sorting index rather
                          ** than the source table */
  u_int32_t n_sorting_column;     /* Number of columns in the sorting index */
  int sorting_idx;         /* Cursor number of the sorting index */
  int sorting_idx_p_tab;     /* Cursor number of pseudo-table */
  int i_first_reg;          /* First register in range for cols[] and funcs[] */
  expr_list_t *group_by;     /* The group by clause */
  struct agg_info_col_t {    /* For each column used in source tables */
    table_t *tab;             /* Source table */
    expr_t *c_expr;            /* The original expression */
    int i_table;              /* Cursor number of the source table */
    int i_column;             /* column_t number within the source table */
    int i_sorter_column;       /* column_t number in the sorting index */
  } *cols;
  int n_column;            /* Number of used entries in cols[] */
  int n_accumulator;       /* Number of columns that show through to the output.
                          ** Additional columns are used only as parameters to
                          ** aggregate functions */
  struct agg_info_func_t {   /* For each aggregate function */
    expr_t *f_expr;            /* Expression encoding the function */
    func_def_t *func;          /* The aggregate function implementation */
    int i_distinct;           /* Ephemeral table used to enforce DISTINCT */
    int i_dist_addr;           /* Address of OP_OpenEphemeral */
    int i_ob_tab;              /* Ephemeral table to implement ORDER BY */
    u_int8_t ob_payload;           /* i_ob_tab has payload columns separate from key */
    u_int8_t ob_unique;            /* Enforce uniqueness on i_ob_tab keys */
    u_int8_t use_subtype;          /* Transfer subtype info through sorter */
  } *funcs;
  int n_func;              /* Number of entries in funcs[] */
  u_int32_t sel_id;              /* select_t to which this agg_info_t belongs */
#ifdef DBSQL_DEBUG
  select_t *p_select;        /* SELECT statement that this agg_info_t supports */
#endif
};

/*
** Macros to compute cols[] and funcs[] register numbers.
**
** These macros should not be used prior to the call to
** __assign_aggregate_registers() that computes the value of agg_info->i_first_reg.
** The assert()s that are part of this macro verify that constraint.
*/
#ifndef NDEBUG
#define AGG_INFO_COLUMN_REG(A,I)  (assert((A)->i_first_reg),(A)->i_first_reg+(I))
#define AGG_INFO_FUNC_REG(A,I)    \
                      (assert((A)->i_first_reg),(A)->i_first_reg+(A)->n_column+(I))
#else
#define AGG_INFO_COLUMN_REG(A,I)  ((A)->i_first_reg+(I))
#define AGG_INFO_FUNC_REG(A,I)    \
                      ((A)->i_first_reg+(A)->n_column+(I))
#endif

/*
** The datatype yn_var_t is a signed integer, either 16-bit or 32-bit.
** Usually it is 16-bits.  But if DBSQL_MAX_VARIABLE_NUMBER is greater
** than 32767 we have to make it 32-bit.  16-bit is preferred because
** it uses less memory in the expr_t object, which is a big memory user
** in systems with lots of prepared statements.  And few applications
** need more than about 10 or 20 variables.  But some extreme users want
** to have prepared statements with over 32766 variables, and for them
** the option is available (at compile-time).
*/
#if DBSQL_MAX_VARIABLE_NUMBER<32767
typedef int16_t yn_var_t;
#else
typedef int yn_var_t;
#endif

/*
** Each node of an expression in the parse tree is an instance
** of this structure.
**
** expr_t.op is the opcode. The integer parser token codes are reused
** as opcodes here. For example, the parser defines TK_GE to be an integer
** code representing the ">=" operator. This same integer code is reused
** to represent the greater-than-or-equal-to operator in the expression
** tree.
**
** If the expression is an SQL literal (TK_INTEGER, TK_FLOAT, TK_BLOB,
** or TK_STRING), then expr_t.u.token contains the text of the SQL literal. If
** the expression is a variable (TK_VARIABLE), then expr_t.u.token contains the
** variable name. Finally, if the expression is an SQL function (TK_FUNCTION),
** then expr_t.u.token contains the name of the function.
**
** expr_t.right and expr_t.left are the left and right subexpressions of a
** binary operator. Either or both may be NULL.
**
** expr_t.x.list is a list of arguments if the expression is an SQL function,
** a CASE expression or an IN expression of the form "<lhs> IN (<y>, <z>...)".
** expr_t.x.p_select is used if the expression is a sub-select or an expression of
** the form "<lhs> IN (SELECT ...)". If the EP_xIsSelect bit is set in the
** expr_t.flags mask, then expr_t.x.p_select is valid. Otherwise, expr_t.x.list is
** valid.
**
** An expression of the form ID or ID.ID refers to a column in a table.
** For such expressions, expr_t.op is set to TK_COLUMN and expr_t.i_table is
** the integer cursor number of a VDBE cursor pointing to that table and
** expr_t.i_column is the column number for the specific column.  If the
** expression is used as a result in an aggregate SELECT, then the
** value is also stored in the expr_t.i_agg column in the aggregate so that
** it can be accessed after all aggregates are computed.
**
** If the expression is an unbound variable marker (a question mark
** character '?' in the original SQL) then the expr_t.i_table holds the index
** number for that variable.
**
** If the expression is a subquery then expr_t.i_column holds an integer
** register number containing the result of the subquery.  If the
** subquery gives a constant result, then i_table is -1.  If the subquery
** gives a different answer at different times during statement processing
** then i_table is the address of a subroutine that computes the subquery.
**
** If the expr_t is of type OP_Column, and the table it is selecting from
** is a disk table or the "old.*" pseudo-table, then tab points to the
** corresponding table definition.
**
** ALLOCATION NOTES:
**
** expr_t objects can use a lot of memory space in database schema.  To
** help reduce memory requirements, sometimes an expr_t object will be
** truncated.  And to reduce the number of memory allocations, sometimes
** two or more expr_t objects will be stored in a single memory allocation,
** together with expr_t.u.token strings.
**
** If the EP_Reduced and EP_TokenOnly flags are set when
** an expr_t object is truncated.  When EP_Reduced is set, then all
** the child expr_t objects in the expr_t.left and expr_t.right subtrees
** are contained within the same memory allocation.  Note, however, that
** the subtrees in expr_t.x.list or expr_t.x.p_select are always separately
** allocated, regardless of whether or not EP_Reduced is set.
*/
struct expr_t {
  u_int8_t op;                 /* Operation performed by this node */
  char aff_expr;          /* affinity, or RAISE type */
  u_int8_t op2;                /* TK_REGISTER/TK_TRUTH: original value of expr_t.op
                         ** TK_COLUMN: the value of p5 for OP_Column
                         ** TK_AGG_FUNCTION: nesting depth
                         ** TK_FUNCTION: NC_SelfRef flag if needs OP_PureFunc */
#ifdef DBSQL_DEBUG
  u_int8_t vva_flags;           /* Verification flags. */
#endif
  u_int32_t flags;             /* Various flags.  EP_* See below */
  union {
    char *token;          /* Token value. Zero terminated and dequoted */
    int i_value;            /* Non-negative integer value if EP_IntValue */
  } u;

  /* If the EP_TokenOnly flag is set in the expr_t.flags mask, then no
  ** space is allocated for the fields below this point. An attempt to
  ** access them will result in a segfault or malfunction.
  *********************************************************************/

  expr_t *left;           /* Left subnode */
  expr_t *right;          /* Right subnode */
  union {
    expr_list_t *list;     /* op = IN, EXISTS, SELECT, CASE, FUNCTION, BETWEEN */
    select_t *p_select;     /* EP_xIsSelect and op = IN, EXISTS, SELECT */
  } x;

  /* If the EP_Reduced flag is set in the expr_t.flags mask, then no
  ** space is allocated for the fields below this point. An attempt to
  ** access them will result in a segfault or malfunction.
  *********************************************************************/

#if DBSQL_MAX_EXPR_DEPTH>0
  int n_height;           /* Height of the tree headed by this node */
#endif
  int i_table;            /* TK_COLUMN: cursor number of table holding column
                         ** TK_REGISTER: register number
                         ** TK_TRIGGER: 1 -> new, 0 -> old
                         ** EP_Unlikely:  134217728 times likelihood
                         ** TK_IN: ephemeral table holding RHS
                         ** TK_SELECT_COLUMN: Number of columns on the LHS
                         ** TK_SELECT: 1st register of result vector */
  yn_var_t i_column;         /* TK_COLUMN: column index.  -1 for rowid.
                         ** TK_VARIABLE: variable number (always >= 1).
                         ** TK_SELECT_COLUMN: column of the result vector */
  int16_t i_agg;              /* Which entry in agg_info->cols[] or ->funcs[] */
  union {
    int i_join;             /* If EP_OuterON or EP_InnerON, the right table */
    int i_ofst;             /* else: start of token from start of statement */
  } w;
  agg_info_t *agg_info;     /* Used by TK_AGG_COLUMN and TK_AGG_FUNCTION */
  union {
    table_t *tab;           /* TK_COLUMN: table_t containing column. Can be NULL
                           ** for a column of an index on an expression */
    window_t *win;          /* EP_WinFunc: window_t/Filter defn for a function */
    int n_reg;              /* TK_NULLS: Number of registers to NULL out */
    struct {               /* TK_IN, TK_SELECT, and TK_EXISTS */
      int i_addr;             /* Subroutine entry address */
      int reg_return;         /* Register used to hold return address */
    } sub;
  } y;
};

/* The following are the meanings of bits in the expr_t.flags field.
** Value restrictions:
**
**          EP_Agg == NC_HasAgg == SF_HasAgg
**          EP_Win == NC_HasWin
*/
#define EP_OuterON    0x000001 /* Originates in ON/USING clause of outer join */
#define EP_InnerON    0x000002 /* Originates in ON/USING of an inner join */
#define EP_Distinct   0x000004 /* Aggregate function with DISTINCT keyword */
#define EP_HasFunc    0x000008 /* Contains one or more functions of any kind */
#define EP_Agg        0x000010 /* Contains one or more aggregate functions */
#define EP_FixedCol   0x000020 /* TK_Column with a known fixed value */
#define EP_VarSelect  0x000040 /* p_select is correlated, not constant */
#define EP_DblQuoted  0x000080 /* token.z was originally in "..." */
#define EP_InfixFunc  0x000100 /* True for an infix function: LIKE, GLOB, etc */
#define EP_Collate    0x000200 /* Tree contains a TK_COLLATE operator */
#define EP_Commuted   0x000400 /* Comparison operator has been commuted */
#define EP_IntValue   0x000800 /* Integer value contained in u.i_value */
#define EP_xIsSelect  0x001000 /* x.p_select is valid (otherwise x.list is) */
#define EP_Skip       0x002000 /* Operator does not contribute to affinity */
#define EP_Reduced    0x004000 /* expr_t struct EXPR_REDUCEDSIZE bytes only */
#define EP_Win        0x008000 /* Contains window functions */
#define EP_TokenOnly  0x010000 /* expr_t struct EXPR_TOKENONLYSIZE bytes only */
#define EP_FullSize   0x020000 /* expr_t structure must remain full sized */
#define EP_IfNullRow  0x040000 /* The TK_IF_NULL_ROW opcode */
#define EP_Unlikely   0x080000 /* unlikely() or likelihood() function */
#define EP_ConstFunc  0x100000 /* A DBSQL_FUNC_CONSTANT or _SLOCHNG function */
#define EP_CanBeNull  0x200000 /* Can be null despite NOT NULL constraint */
#define EP_Subquery   0x400000 /* Tree contains a TK_SELECT operator */
#define EP_Leaf       0x800000 /* expr_t.left, .right, .u.p_select all NULL */
#define EP_WinFunc   0x1000000 /* TK_FUNCTION with expr_t.y.win set */
#define EP_Subrtn    0x2000000 /* Uses expr_t.y.sub. TK_IN, _SELECT, or _EXISTS */
#define EP_Quoted    0x4000000 /* TK_ID was originally quoted */
#define EP_Static    0x8000000 /* Held in memory not obtained from malloc() */
#define EP_IsTrue   0x10000000 /* Always has boolean value of TRUE */
#define EP_IsFalse  0x20000000 /* Always has boolean value of FALSE */
#define EP_FromDDL  0x40000000 /* Originates from dbsql_schema */
#define EP_SubtArg  0x80000000 /* Is argument to DBSQL_SUBTYPE function */

/* The EP_Propagate mask is a set of properties that automatically propagate
** upwards into parent nodes.
*/
#define EP_Propagate (EP_Collate|EP_Subquery|EP_HasFunc)

/* Macros can be used to test, set, or clear bits in the
** expr_t.flags field.
*/
#define EXPR_HAS_PROPERTY(E,P)     (((E)->flags&(u_int32_t)(P))!=0)
#define EXPR_HAS_ALL_PROPERTY(E,P)  (((E)->flags&(u_int32_t)(P))==(u_int32_t)(P))
#define EXPR_SET_PROPERTY(E,P)     (E)->flags|=(u_int32_t)(P)
#define EXPR_CLEAR_PROPERTY(E,P)   (E)->flags&=~(u_int32_t)(P)
#define EXPR_ALWAYS_TRUE(E)   (((E)->flags&(EP_OuterON|EP_IsTrue))==EP_IsTrue)
#define EXPR_ALWAYS_FALSE(E)  (((E)->flags&(EP_OuterON|EP_IsFalse))==EP_IsFalse)
#define EXPR_IS_FULL_SIZE(E)   (((E)->flags&(EP_Reduced|EP_TokenOnly))==0)

/* Macros used to ensure that the correct members of unions are accessed
** in expr_t.
*/
#define EXPR_USE_U_TOKEN(E)    (((E)->flags&EP_IntValue)==0)
#define EXPR_USE_U_VALUE(E)    (((E)->flags&EP_IntValue)!=0)
#define EXPR_USE_W_OFST(E)     (((E)->flags&(EP_InnerON|EP_OuterON))==0)
#define EXPR_USE_W_JOIN(E)     (((E)->flags&(EP_InnerON|EP_OuterON))!=0)
#define EXPR_USE_X_LIST(E)     (((E)->flags&EP_xIsSelect)==0)
#define EXPR_USE_X_SELECT(E)   (((E)->flags&EP_xIsSelect)!=0)
#define EXPR_USE_Y_TAB(E)      (((E)->flags&(EP_WinFunc|EP_Subrtn))==0)
#define EXPR_USE_Y_WIN(E)      (((E)->flags&EP_WinFunc)!=0)
#define EXPR_USE_Y_SUB(E)      (((E)->flags&EP_Subrtn)!=0)

/* Flags for use with expr_t.vva_flags
*/
#define EP_NoReduce   0x01  /* Cannot EXPRDUP_REDUCE this expr_t */
#define EP_Immutable  0x02  /* Do not change this expr_t node */

/* The EXPR_SET_VVA_PROPERTY() macro is used for Verification, Validation,
** and Accreditation only.  It works like EXPR_SET_PROPERTY() during VVA
** processes but is a no-op for delivery.
*/
#ifdef DBSQL_DEBUG
# define EXPR_SET_VVA_PROPERTY(E,P)   (E)->vva_flags|=(P)
# define EXPR_HAS_VVA_PROPERTY(E,P)   (((E)->vva_flags&(P))!=0)
# define EXPR_CLEAR_VVA_PROPERTIES(E) (E)->vva_flags = 0
#else
# define EXPR_SET_VVA_PROPERTY(E,P)
# define EXPR_HAS_VVA_PROPERTY(E,P)   0
# define EXPR_CLEAR_VVA_PROPERTIES(E)
#endif

/*
** Macros to determine the number of bytes required by a normal expr_t
** struct, an expr_t struct with the EP_Reduced flag set in expr_t.flags
** and an expr_t struct with the EP_TokenOnly flag set.
*/
#define EXPR_FULLSIZE           sizeof(expr_t)           /* Full size */
#define EXPR_REDUCEDSIZE        offsetof(expr_t,i_table)  /* Common features */
#define EXPR_TOKENONLYSIZE      offsetof(expr_t,left)   /* Fewer features */

/*
** Flags passed to the __expr_dup() function. See the header comment
** above __expr_dup() for details.
*/
#define EXPRDUP_REDUCE         0x0001  /* Used reduced-size expr_t nodes */

/*
** True if the expression passed as an argument was a function with
** an OVER() clause (a window function).
*/
#ifdef DBSQL_OMIT_WINDOWFUNC
# define IS_WINDOW_FUNC(p) 0
#else
# define IS_WINDOW_FUNC(p) ( \
    EXPR_HAS_PROPERTY((p), EP_WinFunc) && p->y.win->frm_type!=TK_FILTER \
 )
#endif

/*
** A list of expressions.  Each expression may optionally have a
** name.  An expr/name combination can be used in several ways, such
** as the list of "expr AS ID" fields following a "SELECT" or in the
** list of "ID = expr" items in an UPDATE.  A list of expressions can
** also be used as the argument to a function, in which case the a.name
** field is not used.
**
** In order to try to keep memory usage down, the expr_t.a.e_name field
** is used for multiple purposes:
**
**     e_name          Usage
**    ----------       -------------------------
**    ENAME_NAME       (1) the AS of result set column
**                     (2) COLUMN= of an UPDATE
**
**    ENAME_TAB        DB.TABLE.NAME used to resolve names
**                     of subqueries
**
**    ENAME_SPAN       Text of the original result set
**                     expression.
*/
struct expr_list_t {
  int n_expr;             /* Number of expressions on the list */
  int n_alloc;            /* Number of a[] slots allocated */
  struct expr_list_item_t { /* For each expression in the list */
    expr_t *expr;            /* The parse tree for this expression */
    char *e_name;           /* Token associated with this expression */
    struct {
      u_int8_t sort_flags;           /* Mask of KEYINFO_ORDER_* flags */
      unsigned e_name :2;     /* Meaning of e_name */
      unsigned done :1;       /* Indicates when processing is finished */
      unsigned reusable :1;   /* Constant expression is reusable */
      unsigned sorter_ref :1; /* Defer evaluation until after sorting */
      unsigned nulls :1;     /* True if explicit "NULLS FIRST/LAST" */
      unsigned used :1;      /* This column used in a SF_NestedFrom subquery */
      unsigned using_term:1;  /* Term from the USING clause of a __nested_from */
      unsigned no_expand: 1;  /* Term is an auxiliary in __nested_from and should
                              ** not be expanded by "*" in parent queries */
    } fg;
    union {
      struct {             /* Used by any expr_list_t other than parser_t.cons_expr */
        u_int16_t i_order_by_col;      /* For ORDER BY, column number in result set */
        u_int16_t i_alias;           /* index_t into parser_t.alias[] for name */
      } x;
      int i_const_expr_reg;   /* Register in which expr_t value is cached. Used only
                           ** by parser_t.const_expr */
    } u;
  } a[FLEXARRAY];          /* One slot for each expression in the list */
};

/* The size (in bytes) of an expr_list_t object that is big enough to hold
** as many as N expressions. */
#define SZ_EXPRLIST(N)  \
             (offsetof(expr_list_t,a) + (N)*sizeof(struct expr_list_item_t))

/*
** Allowed values for expr_t.a.e_name
*/
#define ENAME_NAME  0       /* The AS clause of a result set */
#define ENAME_SPAN  1       /* Complete text of the result set expression */
#define ENAME_TAB   2       /* "DB.TABLE.NAME" for the result set */
#define ENAME_ROWID 3       /* "DB.TABLE._rowid_" for * expansion of rowid */

/*
** An instance of this structure can hold a simple list of identifiers,
** such as the list "a,b,c" in the following statements:
**
**      INSERT INTO t(a,b,c) VALUES ...;
**      CREATE INDEX idx ON t(a,b,c);
**      CREATE TRIGGER trig BEFORE UPDATE ON t(a,b,c) ...;
**
** The id_list_t.a.idx field is used when the id_list_t represents the list of
** column names after a table name in an INSERT statement.  In the statement
**
**     INSERT INTO t(a,b,c) ...
**
** If "a" is the k-th column of table "t", then id_list_t.a[0].idx==k.
*/
struct id_list_t {
  int n_id;         /* Number of identifiers on the list */
  struct id_list_item_t {
    char *name;      /* Name of the identifier */
  } a[FLEXARRAY];
};

/* The size (in bytes) of an id_list_t object that can hold up to N IDs. */
#define SZ_IDLIST(N)  (offsetof(id_list_t,a)+(N)*sizeof(struct id_list_item_t))

/*
** Allowed values for id_list_t.type, which determines which value of the a.u4
** is valid.
*/
#define EU4_NONE   0   /* Does not use id_list_t.a.u4 */
#define EU4_IDX    1   /* Uses id_list_t.a.u4.idx */
#define EU4_EXPR   2   /* Uses id_list_t.a.u4.expr -- NOT CURRENTLY USED */

/*
** Details of the implementation of a subquery.
*/
struct subquery_t {
  select_t *p_select;  /* A SELECT statement used in place of a table name */
  int addr_fill_sub;  /* Address of subroutine to initialize a subquery */
  int reg_return;    /* Register holding return address of addr_fill_sub */
  int reg_result;    /* Registers holding results of a co-routine */
};

/*
** The src_item_t object represents a single term in the FROM clause of a query.
** The src_list_t object is mostly an array of __src_items.
**
** The jointype starts out showing the join type between the current table
** and the next table on the list.  The parser builds the list this way.
** But __src_list_shift_join_type() later shifts the jointypes so that each
** jointype expresses the join between the table and the previous table.
**
** In the col_used field, the high-order bit (bit 63) is set if the table
** contains more than 63 columns and the 64-th or later column is used.
**
** Aggressive use of "union" helps keep the size of the object small.  This
** has been shown to boost performance, in addition to saving memory.
** Access to union elements is gated by the following rules which should
** always be checked, either by an if-statement or by an assert().
**
**    Field              Only access if this is true
**    ---------------    -----------------------------------
**    u1.indexed_by      fg.is_indexed_by
**    u1.func_arg        fg.is_tab_func
**    u1.n_row            !fg.is_tab_func  && !fg.is_indexed_by
**
**    u2.ib_index        fg.is_indexed_by
**    u2.cte_use         fg.is_cte
**
**    u3.on             !fg.is_using
**    u3.p_using          fg.is_using
**
**    u4.database       !fg.fixed_schema && !fg.is_subquery
**    u4.schema         fg.fixed_schema
**    u4.subq           fg.is_subquery
**
** See also the __src_list_delete() routine for assert() statements that
** check invariants on the fields of this object, especially the flags
** inside the fg struct.
*/
struct src_item_t {
  char *name;      /* Name of the table */
  char *alias;     /* The "B" part of a "A AS B" phrase.  name is the "A" */
  table_t *s_tab;     /* table_t object for name. Mnemonic: Srcitem-TABle */
  struct {
    u_int8_t jointype;      /* Type of join between this table and the previous */
    unsigned not_indexed :1;    /* True if there is a NOT INDEXED clause */
    unsigned is_indexed_by :1;   /* True if there is an INDEXED BY clause */
    unsigned is_subquery :1;    /* True if this term is a subquery */
    unsigned is_tab_func :1;     /* True if table-valued-function syntax */
    unsigned is_correlated :1;  /* True if sub-query is correlated */
    unsigned is_materialized:1; /* This is a materialized view */
    unsigned via_coroutine :1;  /* Implemented as a co-routine */
    unsigned is_recursive :1;   /* True for recursive reference in WITH */
    unsigned from_ddl :1;       /* Comes from dbsql_schema */
    unsigned is_cte :1;         /* This is a CTE */
    unsigned not_cte :1;        /* This item may not match a CTE */
    unsigned is_using :1;       /* u3.p_using is valid */
    unsigned is_on :1;          /* u3.on was once valid and non-NULL */
    unsigned is_synth_using :1;  /* u3.p_using is synthesized from NATURAL */
    unsigned is_nested_from :1;  /* p_select is a SF_NestedFrom subquery */
    unsigned rowid_used :1;     /* The ROWID of this table is referenced */
    unsigned fixed_schema :1;   /* Uses u4.schema, not u4.database */
    unsigned had_schema :1;     /* Had u4.database before u4.schema */
    unsigned from_exists :1;    /* Comes from WHERE EXISTS(...) */
  } fg;
  int i_cursor;      /* The VDBE cursor number used to access this table */
  Bitmask col_used;  /* Bit N set if column N used. Details above for N>62 */
  union {
    char *indexed_by;    /* Identifier from "INDEXED BY <z_index>" clause */
    expr_list_t *func_arg;  /* Arguments to table-valued-function */
    u_int32_t n_row;            /* Number of rows in a VALUES clause */
  } u1;
  union {
    index_t *ib_index;  /* index_t structure corresponding to u1.indexed_by */
    cte_use_t *cte_use;  /* CTE Usage info when fg.is_cte is true */
  } u2;
  union {
    expr_t *on;        /* fg.is_using==0 =>  The ON clause of a join */
    id_list_t *p_using;   /* fg.is_using==1 =>  The USING clause of a join */
  } u3;
  union {
    schema_t *schema;  /* schema_t to which this item is fixed */
    char *database;  /* Name of database holding this table */
    subquery_t *subq;  /* Description of a subquery */
  } u4;
};

/*
** The on_or_using_t object represents either an ON clause or a USING clause.
** It can never be both at the same time, but it can be neither.
*/
struct on_or_using_t {
  expr_t *on;         /* The ON clause of a join */
  id_list_t *p_using;    /* The USING clause of a join */
};

/*
** This object represents one or more tables that are the source of
** content for an SQL statement.  For example, a single src_list_t object
** is used to hold the FROM clause of a SELECT statement.  src_list_t also
** represents the target tables for DELETE, INSERT, and UPDATE statements.
**
*/
struct src_list_t {
  int n_src;             /* Number of tables or subqueries in the FROM clause */
  u_int32_t n_alloc;           /* Number of entries allocated in a[] below */
  src_item_t a[FLEXARRAY]; /* One entry for each identifier on the list */
};

/* Size (in bytes) of a src_list_t object that can hold as many as N
** src_item_t objects. */
#define SZ_SRCLIST(N) (offsetof(src_list_t,a)+(N)*sizeof(src_item_t))

/* Size (in bytes( of a src_list_t object that holds 1 src_item_t.  This is a
** special case of SZ_SRCITEM(1) that comes up often. */
#define SZ_SRCLIST_1  (offsetof(src_list_t,a)+sizeof(src_item_t))

/*
** Permitted values of the src_list_t.a.jointype field
*/
#define JT_INNER     0x01    /* Any kind of inner or cross join */
#define JT_CROSS     0x02    /* Explicit use of the CROSS keyword */
#define JT_NATURAL   0x04    /* True for a "natural" join */
#define JT_LEFT      0x08    /* Left outer join */
#define JT_RIGHT     0x10    /* Right outer join */
#define JT_OUTER     0x20    /* The "OUTER" keyword is present */
#define JT_LTORJ     0x40    /* One of the LEFT operands of a RIGHT JOIN
                             ** Mnemonic: Left table_t Of Right Join */
#define JT_ERROR     0x80    /* unknown or unsupported join type */

/*
** Flags appropriate for the wctrl_flags parameter of __where_begin()
** and the where_info_t.wctrl_flags member.
**
** Value constraints (enforced via assert()):
**     WHERE_USE_LIMIT  == SF_FixedLimit
*/
#define WHERE_ORDERBY_NORMAL   0x0000 /* No-op */
#define WHERE_ORDERBY_MIN      0x0001 /* ORDER BY processing for min() func */
#define WHERE_ORDERBY_MAX      0x0002 /* ORDER BY processing for max() func */
#define WHERE_ONEPASS_DESIRED  0x0004 /* Want to do one-pass UPDATE/DELETE */
#define WHERE_ONEPASS_MULTIROW 0x0008 /* ONEPASS is ok with multiple rows */
#define WHERE_DUPLICATES_OK    0x0010 /* Ok to return a row more than once */
#define WHERE_OR_SUBCLAUSE     0x0020 /* Processing a sub-WHERE as part of
                                      ** the OR optimization  */
#define WHERE_GROUPBY          0x0040 /* order_by is really a GROUP BY */
#define WHERE_DISTINCTBY       0x0080 /* orderby is really a DISTINCT clause */
#define WHERE_WANT_DISTINCT    0x0100 /* All output needs to be distinct */
#define WHERE_SORTBYGROUP      0x0200 /* Support __where_is_sorted() */
#define WHERE_AGG_DISTINCT     0x0400 /* Query is "SELECT agg(DISTINCT ...)" */
#define WHERE_ORDERBY_LIMIT    0x0800 /* ORDERBY+LIMIT on the inner loop */
#define WHERE_RIGHT_JOIN       0x1000 /* Processing a RIGHT JOIN */
#define WHERE_KEEP_ALL_JOINS   0x2000 /* Do not do the omit-noop-join opt */
#define WHERE_USE_LIMIT        0x4000 /* Use the LIMIT in cost estimates */
                        /*     0x8000    not currently used */

/* Allowed return values from __where_is_distinct()
*/
#define WHERE_DISTINCT_NOOP      0  /* DISTINCT keyword not used */
#define WHERE_DISTINCT_UNIQUE    1  /* No duplicates */
#define WHERE_DISTINCT_ORDERED   2  /* All duplicates are adjacent */
#define WHERE_DISTINCT_UNORDERED 3  /* Duplicates are scattered */

/*
** A name_context_t defines a context in which to resolve table and column
** names.  The context consists of a list of tables (the src_list) field and
** a list of named expression (e_list).  The named expression list may
** be NULL.  The src corresponds to the FROM clause of a SELECT or
** to the table being operated on by INSERT, UPDATE, or DELETE.  The
** e_list corresponds to the result set of a SELECT and is NULL for
** other statements.
**
** __name_contexts can be nested.  When resolving names, the inner-most
** context is searched first.  If no match is found, the next outer
** context is checked.  If there is still no match, the next context
** is checked.  This process continues until either a match is found
** or all contexts are check.  When a match is found, the n_ref member of
** the context containing the match is incremented.
**
** Each subquery gets a new name_context_t.  The next field points to the
** name_context_t in the parent query.  Thus the process of scanning the
** name_context_t list corresponds to searching through successively outer
** subqueries looking for a match.
*/
struct name_context_t {
  parser_t *parser;       /* The parser */
  src_list_t *src_list;   /* One or more tables used to resolve names */
  union {
    expr_list_t *e_list;    /* Optional list of result-set columns */
    agg_info_t *agg_info;   /* Information about aggregates at this level */
    upsert_t *upsert;     /* ON CONFLICT clause information from an upsert */
    int i_base_reg;        /* For TK_REGISTER when parsing RETURNING */
  } u_nc;
  name_context_t *next;  /* Next outer name context.  NULL for outermost */
  int n_ref;            /* Number of names resolved by this context */
  int n_nc_err;          /* Number of errors encountered while resolving names */
  int nc_flags;         /* Zero or more NC_* flags defined below */
  u_int32_t n_nested_select;   /* Number of nested selects using this NC */
  select_t *win_select;  /* SELECT statement for any window functions */
};

/*
** Allowed values for the name_context_t, nc_flags field.
**
** Value constraints (all checked via assert()):
**    NC_HasAgg    == SF_HasAgg       == EP_Agg
**    NC_MinMaxAgg == SF_MinMaxAgg    == DBSQL_FUNC_MINMAX
**    NC_OrderAgg  == SF_OrderByReqd  == DBSQL_FUNC_ANYORDER
**    NC_HasWin    == EP_Win
**
*/
#define NC_AllowAgg  0x000001 /* Aggregate functions are allowed here */
#define NC_PartIdx   0x000002 /* True if resolving a partial index WHERE */
#define NC_IsCheck   0x000004 /* True if resolving a CHECK constraint */
#define NC_GenCol    0x000008 /* True for a GENERATED ALWAYS AS clause */
#define NC_HasAgg    0x000010 /* One or more aggregate functions seen */
#define NC_IdxExpr   0x000020 /* True if resolving columns of CREATE INDEX */
#define NC_SelfRef   0x00002e /* Combo: __part_idx, is_check, __gen_col, and __idx_expr */
#define NC_Subquery  0x000040 /* A subquery has been seen */
#define NC_UEList    0x000080 /* True if u_nc.e_list is used */
#define NC_UAggInfo  0x000100 /* True if u_nc.agg_info is used */
#define NC_UUpsert   0x000200 /* True if u_nc.upsert is used */
#define NC_UBaseReg  0x000400 /* True if u_nc.i_base_reg is used */
#define NC_MinMaxAgg 0x001000 /* min/max aggregates seen.  See note above */
/*                   0x002000 // available for reuse */
#define NC_AllowWin  0x004000 /* window_t functions are allowed here */
#define NC_HasWin    0x008000 /* One or more window functions seen */
#define NC_IsDDL     0x010000 /* Resolving names in a CREATE statement */
#define NC_InAggFunc 0x020000 /* True if analyzing arguments to an agg func */
#define NC_FromDDL   0x040000 /* SQL text comes from dbsql_schema */
#define NC_NoSelect  0x080000 /* Do not descend into sub-selects */
#define NC_Where     0x100000 /* Processing WHERE clause of a SELECT */
#define NC_OrderAgg 0x8000000 /* Has an aggregate other than count/min/max */

/*
** An instance of the following object describes a single ON CONFLICT
** clause in an upsert.
**
** The upsert_target field is only set if the ON CONFLICT clause includes
** conflict-target clause.  (In "ON CONFLICT(a,b)" the "(a,b)" is the
** conflict-target clause.)  The upsert_target_where is the optional
** WHERE clause used to identify partial unique indexes.
**
** upsert_set is the list of column=expr terms of the UPDATE statement.
** The upsert_set field is NULL for a ON CONFLICT DO NOTHING.  The
** upsert_where is the WHERE clause for the UPDATE and is NULL if the
** WHERE clause is omitted.
*/
struct upsert_t {
  expr_list_t *upsert_target;  /* Optional description of conflict target */
  expr_t *upsert_target_where; /* WHERE clause for partial index targets */
  expr_list_t *upsert_set;     /* The SET clause from an ON CONFLICT UPDATE */
  expr_t *upsert_where;       /* WHERE clause for the ON CONFLICT UPDATE */
  upsert_t *next_upsert;      /* Next ON CONFLICT clause in the list */
  u_int8_t is_do_update;            /* True for DO UPDATE.  False for DO NOTHING */
  u_int8_t is_dup;                 /* True if 2nd or later with same upsert_idx */
  /* Above this point is the parse tree for the ON CONFLICT clauses.
  ** The next group of fields stores intermediate data. */
  void *to_free;            /* Free memory when deleting the upsert_t object */
  /* All fields above are owned by the upsert_t object and must be freed
  ** when the upsert_t is destroyed.  The fields below are used to transfer
  ** information from the INSERT processing down into the UPDATE processing
  ** while generating code.  The fields below are owned by the INSERT
  ** statement and will be freed by INSERT processing. */
  index_t *upsert_idx;        /* UNIQUE constraint specified by upsert_target */
  src_list_t *upsert_src;      /* table_t to be updated */
  int reg_data;              /* First register holding array of VALUES */
  int i_data_cur;             /* index_t of the data cursor */
  int i_idx_cur;              /* index_t of the first index cursor */
};

/*
** An instance of the following structure contains all information
** needed to generate code for a single SELECT statement.
*/
struct select_t {
  u_int8_t op;                 /* One of: TK_UNION TK_ALL TK_INTERSECT TK_EXCEPT */
  log_est_t n_select_row;     /* Estimated number of result rows */
  u_int32_t sel_flags;          /* Various SF_* values */
  int i_limit, i_offset;   /* Memory registers holding LIMIT & OFFSET counters */
  u_int32_t sel_id;             /* Unique identifier number for this SELECT */
  expr_list_t *e_list;      /* The fields of the result */
  src_list_t *src;         /* The FROM clause */
  expr_t *where;          /* The WHERE clause */
  expr_list_t *group_by;    /* The GROUP BY clause */
  expr_t *having;         /* The HAVING clause */
  expr_list_t *order_by;    /* The ORDER BY clause */
  select_t *prior;        /* Prior select in a compound select statement */
  select_t *next;         /* Next select to the left in a compound */
  expr_t *limit;          /* LIMIT expression. NULL means not used. */
  with_t *with;           /* WITH clause attached to this select. Or NULL. */
#ifndef DBSQL_OMIT_WINDOWFUNC
  window_t *win;          /* List of window functions */
  window_t *win_defn;      /* List of named window definitions */
#endif
};

/*
** Allowed values for select_t.sel_flags.  The "SF" prefix stands for
** "select_t Flag".
**
** Value constraints (all checked via assert())
**     SF_HasAgg      == NC_HasAgg
**     SF_MinMaxAgg   == NC_MinMaxAgg     == DBSQL_FUNC_MINMAX
**     SF_OrderByReqd == NC_OrderAgg      == DBSQL_FUNC_ANYORDER
**     SF_FixedLimit  == WHERE_USE_LIMIT
*/
#define SF_Distinct      0x0000001 /* Output should be DISTINCT */
#define SF_All           0x0000002 /* Includes the ALL keyword */
#define SF_Resolved      0x0000004 /* Identifiers have been resolved */
#define SF_Aggregate     0x0000008 /* Contains agg functions or a GROUP BY */
#define SF_HasAgg        0x0000010 /* Contains aggregate functions */
#define SF_ClonedRhsIn   0x0000020 /* Cloned RHS of an IN operator */
#define SF_Expanded      0x0000040 /* __select_expand() called on this */
#define SF_HasTypeInfo   0x0000080 /* FROM subqueries have table_t metadata */
#define SF_Compound      0x0000100 /* Part of a compound query */
#define SF_Values        0x0000200 /* Synthesized from VALUES clause */
#define SF_MultiValue    0x0000400 /* Single VALUES term with multiple rows */
#define SF_NestedFrom    0x0000800 /* Part of a parenthesized FROM clause */
#define SF_MinMaxAgg     0x0001000 /* Aggregate containing min() or max() */
#define SF_Recursive     0x0002000 /* The recursive part of a recursive CTE */
#define SF_FixedLimit    0x0004000 /* n_select_row set by a constant LIMIT */
/*                       0x0008000 // available for reuse */
#define SF_Converted     0x0010000 /* By __convert_compound_select_to_subquery() */
#define SF_IncludeHidden 0x0020000 /* Include hidden columns in output */
#define SF_ComplexResult 0x0040000 /* Result contains subquery or function */
#define SF_WhereBegin    0x0080000 /* Really a __where_begin() call.  Debug Only */
#define SF_WinRewrite    0x0100000 /* window_t function rewrite accomplished */
#define SF_View          0x0200000 /* SELECT statement is a view */
/*                       0x0400000 // available for reuse */
#define SF_UFSrcCheck    0x0800000 /* Check src as required by UPDATE...FROM */
#define SF_PushDown      0x1000000 /* Modified by WHERE-clause push-down opt */
#define SF_MultiPart     0x2000000 /* Has multiple incompatible PARTITIONs */
#define SF_CopyCte       0x4000000 /* SELECT statement is a copy of a CTE */
#define SF_OrderByReqd   0x8000000 /* The ORDER BY clause may not be omitted */
#define SF_UpdateFrom   0x10000000 /* Query originates with UPDATE FROM */
#define SF_Correlated   0x20000000 /* True if references the outer context */
#define SF_OnToWhere    0x40000000 /* One or more ON clauses moved to WHERE */

/* True if src_item_t X is a subquery that has SF_NestedFrom */
#define IS_NESTED_FROM(X) \
   ((X)->fg.is_subquery && \
    ((X)->u4.subq->p_select->sel_flags&SF_NestedFrom)!=0)

/*
** The results of a SELECT can be distributed in several ways, as defined
** by one of the following macros.  The "SRT" prefix means "SELECT Result
** Type".
**
**     SRT_Exists      Store a 1 in memory cell dest->i_sd_parm if the result
**                     set is not empty.
**
**     SRT_Discard     Throw the results away.  This is used by SELECT
**                     statements within triggers whose only purpose is
**                     the side-effects of functions.
**
**     SRT_Output      Generate a row of output (using the OP_ResultRow
**                     opcode) for each row in the result set.
**
**     SRT_Mem         Only valid if the result is a single column.
**                     Store the first column of the first result row
**                     in register dest->i_sd_parm then abandon the rest
**                     of the query.  This destination implies "LIMIT 1".
**
**     SRT_Set         The result must be a single column.  Store each
**                     row of result as the key in table dest->i_sd_parm.
**                     Apply the affinity dest->aff_sdst before storing
**                     results.  if dest->i_sd_parm2 is positive, then it is
**                     a register holding a Bloom filter for the IN operator
**                     that should be populated in addition to the
**                     dest->i_sd_parm table.  This SRT is used to
**                     implement "IN (SELECT ...)".
**
**     SRT_EphemTab    Create an temporary table dest->i_sd_parm and store
**                     the result there. The cursor is left open after
**                     returning.  This is like SRT_Table except that
**                     this destination uses OP_OpenEphemeral to create
**                     the table first.
**
**     SRT_Coroutine   Generate a co-routine that returns a new row of
**                     results each time it is invoked.  The entry point
**                     of the co-routine is stored in register dest->i_sd_parm
**                     and the result row is stored in dest->n_dest registers
**                     starting with dest->i_sdst.
**
**     SRT_Table       Store results in temporary table dest->i_sd_parm.
**     SRT_Fifo        This is like SRT_EphemTab except that the table
**                     is assumed to already be open.  SRT_Fifo has
**                     the additional property of being able to ignore
**                     the ORDER BY clause.
**
**     SRT_DistFifo    Store results in a temporary table dest->i_sd_parm.
**                     But also use temporary table dest->i_sd_parm+1 as
**                     a record of all prior results and ignore any duplicate
**                     rows.  Name means:  "Distinct Fifo".
**
**     SRT_Queue       Store results in priority queue dest->i_sd_parm (really
**                     an index).  Append a sequence number so that all entries
**                     are distinct.
**
**     SRT_DistQueue   Store results in priority queue dest->i_sd_parm only if
**                     the same record has never been stored before.  The
**                     index at dest->i_sd_parm+1 hold all prior stores.
**
**     SRT_Upfrom      Store results in the temporary table already opened by
**                     dest->i_sd_parm. If (dest->i_sd_parm<0), then the temp
**                     table is an intkey table - in this case the first
**                     column returned by the SELECT is used as the integer
**                     key. If (dest->i_sd_parm>0), then the table is an index
**                     table. (dest->i_sd_parm) is the number of key columns in
**                     each index record in this case.
*/
#define SRT_Exists       1  /* Store 1 if the result is not empty */
#define SRT_Discard      2  /* Do not save the results anywhere */
#define SRT_DistFifo     3  /* Like SRT_Fifo, but unique results only */
#define SRT_DistQueue    4  /* Like SRT_Queue, but unique results only */

/* The DISTINCT clause is ignored for all of the above.  Not that
** IGNORABLE_DISTINCT() implies IGNORABLE_ORDERBY() */
#define IGNORABLE_DISTINCT(X) ((X->dest)<=SRT_DistQueue)

#define SRT_Queue        5  /* Store result in an queue */
#define SRT_Fifo         6  /* Store result as data with an automatic rowid */

/* The ORDER BY clause is ignored for all of the above */
#define IGNORABLE_ORDERBY(X) ((X->dest)<=SRT_Fifo)

#define SRT_Output       7  /* Output each row of result */
#define SRT_Mem          8  /* Store result in a memory cell */
#define SRT_Set          9  /* Store results as keys in an index */
#define SRT_EphemTab    10  /* Create transient tab and store like SRT_Table */
#define SRT_Coroutine   11  /* Generate a single row of result */
#define SRT_Table       12  /* Store result as data with an automatic rowid */
#define SRT_Upfrom      13  /* Store result as data with rowid */

/*
** An instance of this object describes where to put of the results of
** a SELECT statement.
*/
struct select_dest_t {
  u_int8_t dest;            /* How to dispose of the results.  One of SRT_* above. */
  int i_sd_parm;         /* A parameter used by the dest disposal method */
  int i_sd_parm2;        /* A second parameter for the dest disposal method */
  int i_sdst;           /* Base register where results are written */
  int n_sdst;           /* Number of registers allocated */
  char *aff_sdst;      /* Affinity used for SRT_Set */
  expr_list_t *order_by;  /* Key columns for SRT_Queue and SRT_DistQueue */
};

/*
** During code generation of statements that do inserts into AUTOINCREMENT
** tables, the following information is attached to the table_t.u.auto_inc.p
** pointer of each autoincrement table to record some side information that
** the code generator needs.  We have to keep per-table autoincrement
** information in case inserts are done within triggers.  Triggers do not
** normally coordinate their activities, but we do need to coordinate the
** loading and saving of autoincrement information.
*/
struct autoinc_info_t {
  autoinc_info_t *next;   /* Next info block in a list of them all */
  table_t *tab;          /* table_t this info block refers to */
  int i_db;              /* index_t in DBSQL.dbs[] of database holding tab */
  int reg_ctr;           /* Memory register holding the rowid counter */
};

/*
** At least one instance of the following structure is created for each
** trigger that may be fired while parsing an INSERT, UPDATE or DELETE
** statement. All such objects are stored in the linked list headed at
** parser_t.trigger_prg and deleted once statement compilation has been
** completed.
**
** A vdbe_t sub-program that implements the body and WHEN clause of trigger
** trigger_prg_t.P_TRIGGER, assuming a default ON CONFLICT clause of
** trigger_prg_t.orconf, is stored in the trigger_prg_t.program variable.
** The parser_t.trigger_prg list never contains two entries with the same
** values for both P_TRIGGER and orconf.
**
** The trigger_prg_t.colmasks[0] variable is set to a mask of old.* columns
** accessed (or set to 0 for triggers fired as a result of INSERT
** statements). Similarly, the trigger_prg_t.colmasks[1] variable is set to
** a mask of new.* columns used by the program.
*/
struct trigger_prg_t {
  trigger_t *P_TRIGGER;      /* trigger_t this program was coded from */
  trigger_prg_t *next;      /* Next entry in parser_t.trigger_prg list */
  sub_program_t *program;   /* Program implementing P_TRIGGER/orconf */
  int orconf;             /* Default ON CONFLICT policy */
  u_int32_t colmasks[2];        /* Masks of old.*, new.* columns accessed */
};

/*
** The db_mask_t datatype for the bitmask of all attached databases.
*/
#if DBSQL_MAX_ATTACHED>30
  typedef unsigned char db_mask_t[(DBSQL_MAX_ATTACHED+9)/8];
# define DB_MASK_TEST(M,I)    (((M)[(I)/8]&(1<<((I)&7)))!=0)
# define DB_MASK_ZERO(M)      memset((M),0,sizeof(M))
# define DB_MASK_SET(M,I)     (M)[(I)/8]|=(1<<((I)&7))
# define DB_MASK_ALL_ZERO(M)   __db_mask_all_zero(M)
# define DB_MASK_NON_ZERO(M)   (__db_mask_all_zero(M)==0)
#else
  typedef unsigned int db_mask_t;
# define DB_MASK_TEST(M,I)    (((M)&(((db_mask_t)1)<<(I)))!=0)
# define DB_MASK_ZERO(M)      ((M)=0)
# define DB_MASK_SET(M,I)     ((M)|=(((db_mask_t)1)<<(I)))
# define DB_MASK_ALL_ZERO(M)   ((M)==0)
# define DB_MASK_NON_ZERO(M)   ((M)!=0)
#endif

/*
** For each index X that has as one of its arguments either an expression
** or the name of a virtual generated column, and if X is in scope such that
** the value of the expression can simply be read from the index, then
** there is an instance of this object on the parser_t.idx_expr list.
**
** During code generation, while generating code to evaluate expressions,
** this list is consulted and if a matching expression is found, the value
** is read from the index rather than being recomputed.
*/
struct indexed_expr_t {
  expr_t *expr;            /* The expression contained in the index */
  int i_data_cur;           /* The data cursor associated with the index */
  int i_idx_cur;            /* The index cursor */
  int i_idx_col;            /* The index column that contains value of expr */
  u_int8_t maybe_null_row;       /* True if we need an OP_IfNullRow check */
  u_int8_t aff;                 /* Affinity of the expr expression */
  indexed_expr_t *ie_next;   /* Next in a list of all indexed expressions */
#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
  const char *idx_name;   /* Name of index, used only for bytecode comments */
#endif
};

/*
** An instance of the parse_cleanup_t object specifies an operation that
** should be performed after parsing to deallocation resources obtained
** during the parse and which are no longer needed.
*/
struct parse_cleanup_t {
  parse_cleanup_t *next;               /* Next cleanup task */
  void *ptr;                        /* Pointer to object to deallocate */
  void (*cleanup)(DBSQL*,void*);  /* Deallocation routine */
};

/*
** An SQL parser context.  A copy of this structure is passed through
** the parser and down into all the parser action routine in order to
** carry around information that is global to the entire parse.
**
** The structure is divided into two parts.  When the parser and code
** generate call themselves recursively, the first part of the structure
** is constant but the second part is reset at the beginning and end of
** each recursion.
**
** The n_table_lock and table_locks variables are only used if the shared-cache
** feature is enabled (if __tsd()->use_shared_data is true). They are
** used to store the set of table-locks required by the statement being
** compiled. Function __table_lock() is used to add entries to the
** list.
*/
struct parser_t {
  DBSQL *db;         /* The main database structure */
  char *errmsg;       /* An error message */
  vdbe_t *vdbe;         /* An engine for executing database bytecode */
  int rc;              /* Return code from execution */
  log_est_t n_query_loop;   /* Est number of iterations of a query (10*log2(N)) */
  u_int8_t nested;           /* Number of nested calls to the parser/code generator */
  u_int8_t n_temp_reg;         /* Number of temporary registers in temp_regs[] */
  u_int8_t is_multi_write;     /* True if statement may modify/insert multiple rows */
  u_int8_t __disable_lookaside; /* Number of times lookaside has been disabled */
  u_int8_t prep_flags;        /* DBSQL_PREPARE_* flags */
  u_int8_t within_rj_subrtn;   /* Nesting level for RIGHT JOIN body subroutines */
  u_int8_t m_subrtn_sig;       /* mini Bloom filter on available subrtn_sig_t.sel_id */
  u_int8_t trigger_op;       /* TK_UPDATE, TK_INSERT or TK_DELETE */
  u_int8_t orconf;          /* Default ON CONFLICT policy for trigger steps */
#if defined(DBSQL_DEBUG) || defined(DBSQL_COVERAGE_TEST)
  u_int8_t early_cleanup;     /* OOM inside __parser_add_cleanup() */
#endif
#ifdef DBSQL_DEBUG
  u_int8_t if_not_exists;      /* Might be true if IF NOT EXISTS.  Assert()s only */
  u_int8_t is_create;         /* CREATE TABLE, INDEX, or VIEW (but not TRIGGER)
                       ** and ALTER TABLE ADD COLUMN. */
#endif
  bft_t disable_triggers:1; /* True to disable triggers */
  bft_t may_abort :1;     /* True if statement may throw an ABORT exception */
  bft_t has_compound :1;  /* Need to invoke __convert_compound_select_to_subquery() */
  bft_t returning :1;   /* Coding a RETURNING trigger */
  bft_t has_exists :1;   /* Has a correlated "EXISTS (SELECT ....)" expression */
  bft_t col_names_set :1;  /* TRUE after __op_column_name has been issued to vdbe */
  bft_t has_with :1;     /* True if statement contains WITH */
  bft_t ok_const_factor:1; /* OK to factor out constants */
  bft_t check_schema :1;  /* Causes schema cookie check after an error */
  int n_range_reg;       /* Size of the temporary register block */
  int i_range_reg;       /* First register in temporary register block */
  int n_err;            /* Number of errors seen */
  int n_tab;            /* Number of previously allocated VDBE cursors */
  int n_mem;            /* Number of memory cells used so far */
  int sz_op_alloc;       /* Bytes of memory space allocated for vdbe_t.ops[] */
  int i_self_tab;        /* table_t associated with an index on expr, or negative
                       ** of the base register during check-constraint eval */
  int n_nest_sel;        /* Number of nested SELECT statements and/or VIEWs */
  int n_label;          /* The *negative* of the number of labels used */
  int n_label_alloc;     /* Number of slots in labels */
  int *labels;         /* Space to hold the labels */
  expr_list_t *const_expr;/* Constant expressions */
  indexed_expr_t *idx_epr;/* List of expressions used by active indexes */
  indexed_expr_t *idx_part_expr; /* Exprs constrained by index WHERE clauses */
  db_mask_t write_mask;   /* Start a write transaction on these databases */
  db_mask_t cookie_mask;  /* Bitmask of schema verified databases */
  int n_max_arg;         /* Max args to update and filter vtab methods */
  int n_select;         /* Number of SELECT stmts. Counter for select_t.sel_id */
#ifndef DBSQL_OMIT_PROGRESS_CALLBACK
  u_int32_t n_progress_steps;  /* progress steps taken during dbsql_prepare() */
#endif
#ifndef DBSQL_OMIT_SHARED_CACHE
  int n_table_lock;        /* Number of locks in table_locks */
  table_lock_t *table_locks; /* Required table locks for shared-cache mode */
#endif
  autoinc_info_t *ainc;  /* Information about AUTOINCREMENT counters */
  parser_t *toplevel;    /* parser_t structure for main program (or NULL) */
  table_t *trigger_tab;  /* table_t triggers are being coded for */
  trigger_prg_t *trigger_prg;  /* Linked list of coded triggers */
  parse_cleanup_t *cleanup;   /* List of cleanup operations to run after parse */

  /**************************************************************************
  ** Fields above must be initialized to zero.  The fields that follow,
  ** down to the beginning of the recursive section, do not need to be
  ** initialized as they will be set before being used.  The boundary is
  ** determined by offsetof(parser_t,temp_regs).
  **************************************************************************/

  int temp_regs[8];        /* Holding area for temporary registers */
  parser_t *outer_parse;     /* Outer parser_t object when nested */
  Token s_name_token;       /* Token with unqualified schema object name */
  u_int32_t oldmask;            /* Mask of old.* columns referenced */
  u_int32_t newmask;            /* Mask of new.* columns referenced */
  union {
    struct {  /* These fields available when is_create is true */
      int addr_cr_tab;        /* Address of OP_CreateBtree on CREATE TABLE */
      int reg_rowid;         /* Register holding rowid of CREATE TABLE entry */
      int reg_root;          /* Register holding root page for new objects */
      Token constraint_name; /* Name of the constraint currently being parsed */
    } cr;
    struct {  /* These fields available to all other statements */
      returning_t *returning; /* The RETURNING clause */
    } d;
  } u1;

  /************************************************************************
  ** Above is constant between recursions.  Below is reset before and after
  ** each recursion.  The boundary between these two regions is determined
  ** using offsetof(parser_t,s_last_token) so the s_last_token field must be the
  ** first field in the recursive region.
  ************************************************************************/

  Token s_last_token;       /* The last token parsed */
  yn_var_t n_var;               /* Number of '?' variables seen in the SQL so far */
  u_int8_t i_pk_sort_order;          /* ASC or DESC for INTEGER PRIMARY KEY */
  u_int8_t explain;               /* True if the EXPLAIN flag is found on the query */
  u_int8_t parse_mode;            /* PARSE_MODE_XXX constant */
#ifndef DBSQL_OMIT_VIRTUALTABLE
  int n_vtab_lock;            /* Number of virtual tables to lock */
#endif
  int n_height;              /* Expression tree height of current sub-select */
  int addr_explain;          /* Address of current OP_Explain opcode */
  vlist_t *v_list;            /* Mapping between variable names and numbers */
  vdbe_t *reprepare;         /* VM being reprepared (__reprepare()) */
  const char *tail;        /* All SQL text past the last semicolon parsed */
  table_t *new_table;         /* A table being constructed by CREATE TABLE */
  index_t *new_index;         /* An index being constructed by CREATE INDEX.
                            ** Also used to hold redundant UNIQUE constraints
                            ** during a RENAME COLUMN */
  trigger_t *new_trigger;     /* trigger_t under construct by a CREATE TRIGGER */
  const char *auth_context; /* The 6th parameter to db->auth callbacks */
#ifndef DBSQL_OMIT_VIRTUALTABLE
  Token s_arg;               /* Complete text of a module argument */
  table_t **vtab_locks;       /* Pointer to virtual tables needing locking */
#endif
  with_t *with;              /* Current WITH clause, or NULL */
#ifndef DBSQL_OMIT_ALTERTABLE
  rename_token_t *rename;     /* Tokens subject to renaming by ALTER TABLE */
#endif
};

/* Allowed values for parser_t.parse_mode
*/
#define PARSE_MODE_NORMAL        0
#define PARSE_MODE_DECLARE_VTAB  1
#define PARSE_MODE_RENAME        2
#define PARSE_MODE_UNMAP         3

/*
** Sizes and pointers of various parts of the parser_t object.
*/
#define PARSE_HDR(X)  (((char*)(X))+offsetof(parser_t,errmsg))
#define PARSE_HDR_SZ (offsetof(parser_t,temp_regs)-offsetof(parser_t,errmsg)) /* Recursive part w/o col_caches*/
#define PARSE_RECURSE_SZ offsetof(parser_t,s_last_token)    /* Recursive part */
#define PARSE_TAIL_SZ (sizeof(parser_t)-PARSE_RECURSE_SZ) /* Non-recursive part */
#define PARSE_TAIL(X) (((char*)(X))+PARSE_RECURSE_SZ)  /* Pointer to tail */

/*
** Return true if currently inside an dbsql_declare_vtab() call.
*/
#ifdef DBSQL_OMIT_VIRTUALTABLE
  #define IN_DECLARE_VTAB 0
#else
  #define IN_DECLARE_VTAB (parser->parse_mode==PARSE_MODE_DECLARE_VTAB)
#endif

#if defined(DBSQL_OMIT_ALTERTABLE)
  #define IN_RENAME_OBJECT 0
#else
  #define IN_RENAME_OBJECT (parser->parse_mode>=PARSE_MODE_RENAME)
#endif

#if defined(DBSQL_OMIT_VIRTUALTABLE) && defined(DBSQL_OMIT_ALTERTABLE)
  #define IN_SPECIAL_PARSE 0
#else
  #define IN_SPECIAL_PARSE (parser->parse_mode!=PARSE_MODE_NORMAL)
#endif

/*
** An instance of the following structure can be declared on a stack and used
** to save the parser_t.auth_context value so that it can be restored later.
*/
struct auth_context_t {
  const char *auth_context;   /* Put saved parser_t.auth_context here */
  parser_t *parser;              /* The parser_t structure */
};

/*
** Bitfield flags for P5 value in various opcodes.
**
** Value constraints (enforced via assert()):
**    OPFLAG_LENGTHARG    == DBSQL_FUNC_LENGTH
**    OPFLAG_TYPEOFARG    == DBSQL_FUNC_TYPEOF
**    OPFLAG_BULKCSR      == BTREE_BULKLOAD
**    OPFLAG_SEEKEQ       == BTREE_SEEK_EQ
**    OPFLAG_FORDELETE    == BTREE_FORDELETE
**    OPFLAG_SAVEPOSITION == BTREE_SAVEPOSITION
**    OPFLAG_AUXDELETE    == BTREE_AUXDELETE
*/
#define OPFLAG_NCHANGE       0x01    /* OP_Insert: Set to update db->n_change */
                                     /* Also used in P2 (not P5) of OP_Delete */
#define OPFLAG_NOCHNG        0x01    /* OP_VColumn nochange for UPDATE */
#define OPFLAG_EPHEM         0x01    /* OP_Column: Ephemeral output is ok */
#define OPFLAG_LASTROWID     0x20    /* Set to update db->last_rowid */
#define OPFLAG_ISUPDATE      0x04    /* This OP_Insert is an sql UPDATE */
#define OPFLAG_APPEND        0x08    /* This is likely to be an append */
#define OPFLAG_USESEEKRESULT 0x10    /* Try to avoid a seek in __btree_insert() */
#define OPFLAG_ISNOOP        0x40    /* OP_Delete does pre-update-hook only */
#define OPFLAG_LENGTHARG     0x40    /* OP_Column only used for length() */
#define OPFLAG_TYPEOFARG     0x80    /* OP_Column only used for typeof() */
#define OPFLAG_BYTELENARG    0xc0    /* OP_Column only for octet_length() */
#define OPFLAG_BULKCSR       0x01    /* OP_Open** used to open bulk cursor */
#define OPFLAG_SEEKEQ        0x02    /* OP_Open** cursor uses EQ seek only */
#define OPFLAG_FORDELETE     0x08    /* OP_Open should use BTREE_FORDELETE */
#define OPFLAG_P2ISREG       0x10    /* P2 to OP_Open** is a register number */
#define OPFLAG_PERMUTE       0x01    /* OP_Compare: use the permutation */
#define OPFLAG_SAVEPOSITION  0x02    /* OP_Delete/Insert: save cursor pos */
#define OPFLAG_AUXDELETE     0x04    /* OP_Delete: index in a DELETE op */
#define OPFLAG_NOCHNG_MAGIC  0x6d    /* OP_MakeRecord: serialtype 10 is ok */
#define OPFLAG_PREFORMAT     0x80    /* OP_Insert uses preformatted cell */

/*
** Each trigger present in the database schema is stored as an instance of
** struct trigger_t.
**
** Pointers to instances of struct trigger_t are stored in two ways.
** 1. In the "trig_hash" hash table (part of the DBSQL* that represents the
**    database). This allows trigger_t structures to be retrieved by name.
** 2. All triggers associated with a single table form a linked list, using the
**    next member of struct trigger_t. A pointer to the first element of the
**    linked list is stored as the "P_TRIGGER" member of the associated
**    struct table_t.
**
** The "step_list" member points to the first element of a linked list
** containing the SQL statements specified as the trigger program.
*/
struct trigger_t {
  char *name;            /* The name of the trigger                        */
  char *table;            /* The table or view to which the trigger applies */
  u_int8_t op;                  /* One of TK_DELETE, TK_UPDATE, TK_INSERT         */
  u_int8_t tr_tm;               /* One of TRIGGER_BEFORE, TRIGGER_AFTER */
  u_int8_t returning;          /* This trigger implements a RETURNING clause */
  expr_t *when;            /* The WHEN clause of the expression (may be NULL) */
  id_list_t *columns;       /* If this is an UPDATE OF <column-list> trigger,
                             the <column-list> is stored here */
  schema_t *schema;        /* schema_t containing the trigger */
  schema_t *tab_schema;     /* schema_t containing the table */
  trigger_step_t *step_list; /* Link list of trigger program steps             */
  trigger_t *next;         /* Next trigger associated with the table */
};

/*
** A trigger is either a BEFORE or an AFTER trigger.  The following constants
** determine which.
**
** If there are multiple triggers, you might of some BEFORE and some AFTER.
** In that cases, the constants below can be ORed together.
*/
#define TRIGGER_BEFORE  1
#define TRIGGER_AFTER   2

/*
** An instance of struct trigger_step_t is used to store a single SQL statement
** that is a part of a trigger-program.
**
** Instances of struct trigger_step_t are stored in a singly linked list (linked
** using the "next" member) referenced by the "step_list" member of the
** associated struct trigger_t instance. The first element of the linked list is
** the first step of the trigger-program.
**
** The "op" member indicates whether this is a "DELETE", "INSERT", "UPDATE" or
** "SELECT" statement. The meanings of the other members is determined by the
** value of "op" as follows:
**
** (op == TK_INSERT)
** orconf    -> stores the ON CONFLICT algorithm
** p_select   -> The content to be inserted - either a SELECT statement or
**              a VALUES clause.
** src      -> table_t to insert into.
** id_list   -> If this is an INSERT INTO ... (<column-names>) VALUES ...
**              statement, then this stores the column-names to be
**              inserted into.
** upsert   -> The ON CONFLICT clauses for an upsert_t
**
** (op == TK_DELETE)
** src      -> table_t to delete from
** where    -> The WHERE clause of the DELETE statement if one is specified.
**              Otherwise NULL.
**
** (op == TK_UPDATE)
** src      -> table_t to update, followed by any FROM clause tables.
** where    -> The WHERE clause of the UPDATE statement if one is specified.
**              Otherwise NULL.
** expr_list -> A list of the columns to update and the expressions to update
**              them to. See __update() documentation of "changes"
**              argument.
**
** (op == TK_SELECT)
** p_select   -> The SELECT statement
**
** (op == TK_RETURNING)
** expr_list -> The list of expressions that follow the RETURNING keyword.
**
*/
struct trigger_step_t {
  u_int8_t op;               /* One of TK_DELETE, TK_UPDATE, TK_INSERT, TK_SELECT,
                       ** or TK_RETURNING */
  u_int8_t orconf;           /* OE_Rollback etc. */
  trigger_t *trig;      /* The trigger that this step is a part of */
  select_t *p_select;     /* SELECT statement or RHS of INSERT INTO SELECT ... */
  src_list_t *src;       /* table_t to insert/update/delete */
  expr_t *where;        /* The WHERE clause for DELETE or UPDATE steps */
  expr_list_t *expr_list; /* SET clause for UPDATE, or RETURNING clause */
  id_list_t *id_list;     /* column_t names for INSERT */
  upsert_t *upsert;     /* upsert_t clauses on an INSERT */
  char *span;         /* Original SQL text of this command */
  trigger_step_t *next;  /* Next in the link-list */
  trigger_step_t *last;  /* Last element in link-list. Valid for 1st elem only */
};

/*
** Information about a RETURNING clause
*/
struct returning_t {
  parser_t *parser;        /* The parse that includes the RETURNING clause */
  expr_list_t *return_el;  /* List of expressions to return */
  trigger_t ret_trig;      /* The transient trigger that implements RETURNING */
  trigger_step_t ret_t_step; /* The trigger step */
  int i_ret_cur;          /* Transient table holding RETURNING results */
  int n_ret_col;          /* Number of in return_el after expansion */
  int i_ret_reg;          /* Register array for holding a row of RETURNING */
  char name[40];       /* Name of trigger: "dbsql_returning_%p" */
};

/*
** An object used to accumulate the text of a string where we
** do not necessarily know how big the string will be in the end.
*/
struct dbsql_str_t {
  DBSQL *db;         /* Optional database for lookaside.  Can be NULL */
  char *text;         /* The string collected so far */
  u_int32_t  n_alloc;         /* Amount of space allocated in text */
  u_int32_t  mx_alloc;        /* Maximum allowed allocation.  0 for no malloc usage */
  u_int32_t  n_char;          /* Length of the string so far */
  u_int8_t   acc_error;       /* DBSQL_NOMEM or DBSQL_TOOBIG */
  u_int8_t   printf_flags;    /* DBSQL_PRINTF flags below */
};
#define DBSQL_PRINTF_INTERNAL 0x01  /* Internal-use-only converters allowed */
#define DBSQL_PRINTF_SQLFUNC  0x02  /* SQL function arguments to VXPrintf */
#define DBSQL_PRINTF_MALLOCED 0x04  /* True if text is allocated space */

#define IS_MALLOCED(X)  (((X)->printf_flags & DBSQL_PRINTF_MALLOCED)!=0)

/*
** The following object is the header for an "rc_str_t" or "reference-counted
** string".  An rc_str_t is passed around and used like any other char*
** that has been dynamically allocated.  The important interface
** differences:
**
**   1.  rc_str_t strings are reference counted.  They are deallocated
**       when the reference count reaches zero.
**
**   2.  Use __rc_str_unref() to free an rc_str_t string rather than
**       dbsql_free()
**
**   3.  Make a (read-only) copy of a read-only rc_str_t string using
**       __rc_str_ref().
**
** "String" is in the name, but an rc_str_t object can also be used to hold
** binary data.
*/
struct rc_str_t {
  u_int64_t n_rc_ref;            /* Number of references */
  /* Total structure size should be a multiple of 8 bytes for alignment */
};

/*
** A pointer to this structure is used to communicate information
** from __init and OP_ParseSchema into the __init_callback.
*/
typedef struct {
  DBSQL *db;        /* The database being initialized */
  char **errmsg_p;    /* Error message stored here */
  int i_db;            /* 0 for main database.  1 for TEMP, 2.. for ATTACHed */
  int rc;             /* Result code stored here */
  u_int32_t m_init_flags;     /* Flags controlling error messages */
  u_int32_t n_init_row;       /* Number of rows processed */
  pgno_t mx_page;        /* Maximum page number.  0 for no limit. */
} __init_data;

/*
** Allowed values for m_init_flags
*/
#define INITFLAG_AlterMask     0x0007  /* Types of ALTER */
#define INITFLAG_AlterRename   0x0001  /* Reparse after a RENAME */
#define INITFLAG_AlterDrop     0x0002  /* Reparse after a DROP COLUMN */
#define INITFLAG_AlterAdd      0x0003  /* Reparse after an ADD COLUMN */
#define INITFLAG_AlterDropCons 0x0004  /* Reparse after an ADD COLUMN */

/* TUNING parameters are set using DBSQL_TESTCTRL_TUNE and are controlled
** on debug-builds of the CLI using ".testctrl tune ID VALUE".  TUNING
** parameters are for temporary use during development, to help find
** optimal values for parameters in the query planner.  The should not
** be used on trunk check-ins.  They are a temporary mechanism available
** for transient development builds only.
**
** TUNING parameters are numbered starting with 1.
*/
#define DBSQL_NTUNE  6             /* Should be zero for all trunk check-ins */
#ifdef DBSQL_DEBUG
# define TUNING(X)  (__config.tunes[(X)-1])
#else
# define TUNING(X)  0
#endif

/*
** Structure containing global configuration data for the dbsql library.
**
** This structure also contains some state information.
*/
struct dbsql_config {
  int memstat;                     /* True to enable memory status */
  u_int8_t core_mutex;                    /* True to enable core mutexing */
  u_int8_t full_mutex;                    /* True to enable full mutexing */
  u_int8_t open_uri;                      /* True to interpret filenames as URIs */
  u_int8_t use_cis;                       /* Use covering indices for full-scans */
  u_int8_t small_malloc;                  /* Avoid large memory allocations if true */
  u_int8_t extra_schema_checks;            /* Verify type,name,tbl_name in schema */
#ifdef DBSQL_DEBUG
  u_int8_t json_selfcheck;                /* Double-check JSON parsing */
#endif
  int mx_strlen;                     /* Maximum string length */
  int never_corrupt;                 /* Database is always well-formed */
  int sz_lookaside;                  /* Default lookaside buffer size */
  int n_lookaside;                   /* Default lookaside buffer count */
  int n_stmt_spill;                   /* Stmt-journal spill-to-disk threshold */
  dbsql_mem_methods_t m;            /* Low-level memory allocation interface */
  dbsql_mutex_methods_t mutex;      /* Low-level mutex interface */
  dbsql_pcache_methods2_t pcache2;  /* Low-level page-cache interface */
  void *heap;                      /* Heap storage space */
  int n_heap;                        /* Size of heap[] */
  int mn_req, mx_req;                 /* Min and max heap requests sizes */
  int64_t sz_mmap;             /* mmap() space per open file */
  int64_t mx_mmap;             /* Maximum value for sz_mmap */
  void *page;                      /* Page cache memory */
  int sz_page;                       /* Size of each page in page[] */
  int n_page;                        /* Number of pages in page[] */
  int mx_parser_stack;                /* maximum depth of the parser stack */
  int shared_cache_enabled;           /* true if shared-cache mode enabled */
  u_int32_t sz_pma;                        /* Maximum Sorter PMA size */
  /* The above might be initialized to non-zero.  The following need to always
  ** initially be zero, however. */
  int is_init;                       /* True after initialization has finished */
  int in_progress;                   /* True while initialization in progress */
  int is_mutex_init;                  /* True after mutexes are initialized */
  int is_malloc_init;                 /* True after malloc is initialized */
  int is_p_cache_init;                 /* True after malloc is initialized */
  int n_ref_init_mutex;                /* Number of users of init_mutex */
  dbsql_mutex_t *init_mutex;        /* Mutex used by dbsql_initialize() */
  void (*x_log)(void*,int,const char*); /* Function for logging */
  void *log_arg;                       /* First argument to x_log() */
#ifdef DBSQL_ENABLE_SQLLOG
  void(*sqllog)(void*,DBSQL*,const char*, int);
  void *sqllog_arg;
#endif
#ifdef DBSQL_VDBE_COVERAGE
  /* The following callback (if not NULL) is invoked on every VDBE branch
  ** operation.  Set the callback using DBSQL_TESTCTRL_VDBE_COVERAGE.
  */
  void (*vdbe_branch)(void*,unsigned i_src_line,u_int8_t e_this,u_int8_t mx);  /* Callback */
  void *vdbe_branch_arg;                                     /* 1st argument */
#endif
#ifndef DBSQL_OMIT_DESERIALIZE
  int64_t mx_memdb_size;        /* Default max memdb size */
#endif
#ifndef DBSQL_UNTESTABLE
  int (*test_callback)(int);        /* Invoked by __fault_sim() */
#endif
#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
  u_int32_t m_no_visible_rowid;              /* TF_NoVisibleRowid if the ROWID_IN_VIEW
                                    ** feature is disabled.  0 if rowids can
                                    ** occur in views. */
#endif
  int localtime_fault;              /* True to fail localtime() calls */
  int (*alt_localtime)(const void*,void*); /* Alternative localtime() routine */
  int i_once_reset_threshold;          /* When to reset OP_Once counters */
  u_int32_t sz_sorter_ref;                  /* Min size in bytes to use sorter-refs */
  unsigned int i_prng_seed;           /* Alternative fixed seed for the PRNG */
  /* vvvv--- must be last ---vvv */
#ifdef DBSQL_DEBUG
  int64_t tunes[DBSQL_NTUNE]; /* TUNING parameters */
#endif
};

/*
** This macro is used inside of assert() statements to indicate that
** the assert is only valid on a well-formed database.  Instead of:
**
**     assert( X );
**
** One writes:
**
**     assert( X || CORRUPT_DB );
**
** CORRUPT_DB is true during normal operation.  CORRUPT_DB does not indicate
** that the database is definitely corrupt, only that it might be corrupt.
** For most test cases, CORRUPT_DB is set to false using a special
** dbsql_test_control().  This enables assert() statements to prove
** things that are always true for well-formed databases.
*/
#define CORRUPT_DB  (__config.never_corrupt==0)

/*
** Context pointer passed down through the tree-walk.
*/
struct walker_t {
  parser_t *parser;                            /* Parser context.  */
  int (*expr_callback)(walker_t*, expr_t*);     /* Callback for expressions */
  int (*select_callback)(walker_t*,select_t*);  /* Callback for SELECTs */
  void (*select_callback2)(walker_t*,select_t*);/* Second callback for SELECTs */
  int walker_depth;                          /* Number of subqueries */
  u_int16_t code;                                /* A small processing code */
  u_int16_t m_w_flags;                              /* Use-dependent flags */
  union {                                   /* Extra data for callback */
    name_context_t *nc;                         /* Naming context */
    int n;                                    /* A counter */
    int i_cur;                                 /* A cursor number */
    int sz;                                   /* String literal length */
    src_list_t *src_list;                        /* FROM clause */
    struct c_cur_hint_t *c_cur_hint;               /* Used by CODE_CURSOR_HINT() */
    struct ref_src_list_t *ref_src_list;           /* __references_src_list() */
    int *cols;                               /* array of column indexes */
    struct idx_cover_t *idx_cover;               /* Check for index coverage */
    expr_list_t *group_by;                       /* GROUP BY clause */
    select_t *p_select;                          /* HAVING to WHERE clause ctx */
    struct window_rewrite_t *rewrite;           /* window_t rewrite context */
    struct where_const_t *p_const;                /* WHERE clause constants */
    struct rename_ctx_t *rename;                /* RENAME COLUMN context */
    struct table_t *tab;                       /* table_t of generated column */
    struct covering_index_check_t *cov_idx_ck;     /* Check for covering index */
    src_item_t *src_item;                        /* A single FROM clause item */
    db_fixer_t *fix;                            /* See __fix_select() */
    mem_t *mems;                                /* See __sm_cursor_hint() */
    struct check_on_ctx_t *check_on_ctx;           /* See select_check_on_clauses() */
  } u;
};

/*
** The following structure contains information used by the dbsql_fix...
** routines as they walk the parse tree to make database references
** explicit.
*/
struct db_fixer_t {
  parser_t *parser;      /* The parsing context.  Error messages written here */
  walker_t w;           /* walker_t object */
  schema_t *schema;    /* Fix items to this schema */
  u_int8_t temp;           /* True for TEMP schema entries */
  const char *z_db;    /* Make sure all objects are contained in this database */
  const char *type;  /* Type of the container - used for error messages */
  const Token *name; /* Name of the container - used for error messages */
};

/* Forward declarations */
int __walk_expr(walker_t*, expr_t*);
int __walk_expr_nn(walker_t*, expr_t*);
int __walk_expr_list(walker_t*, expr_list_t*);
int __walk_select(walker_t*, select_t*);
int __walk_select_expr(walker_t*, select_t*);
int __walk_select_from(walker_t*, select_t*);
int __expr_walk_noop(walker_t*, expr_t*);
int __select_walk_noop(walker_t*, select_t*);
int __select_walk_fail(walker_t*, select_t*);
int __walker_depth_increase(walker_t*,select_t*);
void __walker_depth_decrease(walker_t*,select_t*);
void __walk_win_defn_dummy_callback(walker_t*,select_t*);

#ifdef DBSQL_DEBUG
void __select_walk_assert2(walker_t*, select_t*);
#endif

#ifndef DBSQL_OMIT_CTE
void __select_pop_with(walker_t*, select_t*);
#else
# define __select_pop_with 0
#endif

/*
** Return code from the parse-tree walking primitives and their
** callbacks.
*/
#define WRC_Continue    0   /* Continue down into children */
#define WRC_Prune       1   /* Omit children but continue walking siblings */
#define WRC_Abort       2   /* Abandon the tree walk */

/*
** A single common table expression
*/
struct cte_t {
  char *name;            /* Name of this CTE */
  expr_list_t *cols;        /* List of explicit column names, or NULL */
  select_t *p_select;        /* The definition of this CTE */
  const char *cte_err;    /* Error message for circular references */
  cte_use_t *use;           /* Usage information for this CTE */
  u_int8_t m10d;               /* The MATERIALIZED flag */
};

/*
** Allowed values for the materialized flag (m10d):
*/
#define M10D_YES       0  /* AS MATERIALIZED */
#define M10D_ANY       1  /* Not specified.  Query planner's choice */
#define M10D_NO        2  /* AS NOT MATERIALIZED */

/*
** An instance of the with_t object represents a WITH clause containing
** one or more CTEs (common table expressions).
*/
struct with_t {
  int n_cte;               /* Number of CTEs in the WITH clause */
  int view;              /* Belongs to the outermost select_t of a view */
  with_t *outer;           /* Containing WITH clause, or NULL */
  cte_t a[FLEXARRAY];       /* For each CTE in the WITH clause.... */
};

/* The size (in bytes) of a with_t object that can hold as many
** as N different CTEs. */
#define SZ_WITH(N)  (offsetof(with_t,a) + (N)*sizeof(cte_t))

/*
** The cte_t object is not guaranteed to persist for the entire duration
** of code generation.  (The query flattener or other parser tree
** edits might delete it.)  The following object records information
** about each Common table_t Expression that must be preserved for the
** duration of the parse.
**
** The cte_use_t objects are freed using __parser_add_cleanup() rather
** than __select_delete(), which is what enables them to persist
** until the end of code generation.
*/
struct cte_use_t {
  int n_use;              /* Number of users of this CTE */
  int addr_m9e;           /* Start of subroutine to compute materialization */
  int reg_rtn;            /* Return address register for addr_m9e subroutine */
  int i_cur;              /* Ephemeral table holding the materialization */
  log_est_t n_row_est;        /* Estimated number of rows in the table */
  u_int8_t m10d;              /* The MATERIALIZED flag */
};


/* Client data associated with dbsql_set_clientdata() and
** dbsql_get_clientdata().
*/
struct db_client_data_t {
  db_client_data_t *next;        /* Next in a linked list */
  void *data;                /* The data */
  void (*destructor)(void*); /* Destructor.  Might be NULL */
  char name[FLEXARRAY];      /* Name of this client data. MUST BE LAST */
};

/* The size (in bytes) of a db_client_data_t object that can has a name
** that is N bytes long, including the zero-terminator. */
#define SZ_DBCLIENTDATA(N) (offsetof(db_client_data_t,name)+(N))

#ifdef DBSQL_DEBUG
/*
** An instance of the tree_view_t object is used for printing the content of
** data structures on __debug_printf() using a tree-like view.
*/
struct tree_view_t {
  int i_level;             /* Which level of the tree we are on */
  u_int8_t  line[100];         /* Draw vertical in column i if line[i] is true */
};
#endif /* DBSQL_DEBUG */

/*
** This object is used in various ways, most (but not all) related to window
** functions.
**
**   (1) A single instance of this structure is attached to the
**       the expr_t.y.win field for each window function in an expression tree.
**       This object holds the information contained in the OVER clause,
**       plus additional fields used during code generation.
**
**   (2) All window functions in a single SELECT form a linked-list
**       attached to select_t.win.  The window_t.func and window_t.expr
**       fields point back to the expression that is the window function.
**
**   (3) The terms of the WINDOW clause of a SELECT are instances of this
**       object on a linked list attached to select_t.win_defn.
**
**   (4) For an aggregate function with a FILTER clause, an instance
**       of this object is stored in expr_t.y.win with frm_type set to
**       TK_FILTER. In this case the only field used is window_t.filter.
**
** The uses (1) and (2) are really the same window_t object that just happens
** to be accessible in two different ways.  Use case (3) are separate objects.
*/
struct window_t {
  char *name;            /* Name of window (may be NULL) */
  char *z_base;            /* Name of base window for chaining (may be NULL) */
  expr_list_t *partition;   /* PARTITION BY clause */
  expr_list_t *order_by;     /* ORDER BY clause */
  u_int8_t frm_type;            /* TK_RANGE, TK_GROUPS, TK_ROWS, or 0 */
  u_int8_t start;              /* UNBOUNDED, CURRENT, PRECEDING or FOLLOWING */
  u_int8_t end;                /* UNBOUNDED, CURRENT, PRECEDING or FOLLOWING */
  u_int8_t implicit_frame;      /* True if frame was implicitly specified */
  u_int8_t exclude;            /* TK_NO, TK_CURRENT, TK_TIES, TK_GROUP, or 0 */
  expr_t *p_start;           /* Expression for "<expr> PRECEDING" */
  expr_t *p_end;             /* Expression for "<expr> FOLLOWING" */
  window_t **pp_this;        /* Pointer to this object in select_t.win list */
  window_t *next_win;       /* Next window function belonging to this SELECT */
  expr_t *filter;          /* The FILTER expression */
  func_def_t *w_func;        /* The function */
  int i_eph_csr;            /* Partition buffer or Peer buffer */
  int reg_accum;           /* Accumulator */
  int reg_result;          /* Interim result */
  int csr_app;             /* Function cursor (used by min/max) */
  int reg_app;             /* Function register (also used by min/max) */
  int reg_part;            /* Array of registers for PARTITION BY values */
  expr_t *owner;           /* Expression object this window is attached to */
  int n_buffer_col;         /* Number of columns in buffer table */
  int i_arg_col;            /* Offset of first argument for this function */
  int reg_one;             /* Register containing constant value 1 */
  int reg_start_rowid;
  int reg_end_rowid;
  u_int8_t expr_args;           /* Defer evaluation of window function arguments
                          ** due to the DBSQL_SUBTYPE flag */
};

select_t *__multi_values(parser_t *parser, select_t *left, expr_list_t *row);
void __multi_values_end(parser_t *parser, select_t *val);

#ifndef DBSQL_OMIT_WINDOWFUNC
void __window_delete(DBSQL*, window_t*);
void __window_unlink_from_select(window_t*);
void __window_list_delete(DBSQL *db, window_t *p);
window_t *__window_alloc(parser_t*, int, int, expr_t*, int , expr_t*, u_int8_t);
void __window_attach(parser_t*, expr_t*, window_t*);
void __window_link(select_t *sel, window_t *win);
int __window_compare(const parser_t*, const window_t*, const window_t*, int);
void __window_code_init(parser_t*, select_t*);
void __window_code_step(parser_t*, select_t*, where_info_t*, int, int);
int __window_rewrite(parser_t*, select_t*);
void __window_update(parser_t*, window_t*, window_t*, func_def_t*);
window_t *__window_dup(DBSQL *db, expr_t *owner, window_t *p);
window_t *__window_list_dup(DBSQL *db, window_t *p);
void __window_functions(void);
void __window_chain(parser_t*, window_t*, window_t*);
window_t *__window_assemble(parser_t*, window_t*, expr_list_t*, expr_list_t*, Token*);
#else
# define __window_delete(a,b)
# define __window_functions()
# define __window_attach(a,b,c)
#endif

/*
** Assuming in points to the first byte of a UTF-8 character,
** advance in to point to the first byte of the next UTF-8 character.
*/
#define DBSQL_SKIP_UTF8(in) {                        \
  if( (*(in++))>=0xc0 ){                              \
    while( (*in & 0xc0)==0x80 ){ in++; }             \
  }                                                    \
}

/*
** The DBSQL_*_BKPT macros are substitutes for the error codes with
** the same name but without the _BKPT suffix.  These macros invoke
** routines that report the line-number on which the error originated
** using dbsql_log().  The routines also provide a convenient place
** to set a debugger breakpoint.
*/
int __report_error(int i_err, int lineno, const char *type);
int __corrupt_error(int);
int __misuse_error(int);
int __cantopen_error(int);
#define DBSQL_CORRUPT_BKPT __corrupt_error(__LINE__)
#define DBSQL_MISUSE_BKPT __misuse_error(__LINE__)
#define DBSQL_CANTOPEN_BKPT __cantopen_error(__LINE__)
#ifdef DBSQL_DEBUG
  int __nomem_error(int);
  int __ioerrnomem_error(int);
# define DBSQL_NOMEM_BKPT __nomem_error(__LINE__)
# define DBSQL_IOERR_NOMEM_BKPT __ioerrnomem_error(__LINE__)
#else
# define DBSQL_NOMEM_BKPT DBSQL_NOMEM
# define DBSQL_IOERR_NOMEM_BKPT DBSQL_IOERR_NOMEM
#endif
#if defined(DBSQL_DEBUG) || defined(DBSQL_ENABLE_CORRUPT_PGNO)
  int __corrupt_pgno_error(int,pgno_t);
# define DBSQL_CORRUPT_PGNO(P) __corrupt_pgno_error(__LINE__,(P))
#else
# define DBSQL_CORRUPT_PGNO(P) __corrupt_error(__LINE__)
#endif

/*
** FTS3 and FTS4 both require virtual table support
*/
#if defined(DBSQL_OMIT_VIRTUALTABLE)
# undef DBSQL_ENABLE_FTS3
# undef DBSQL_ENABLE_FTS4
#endif

/*
** FTS4 is really an extension for FTS3.  It is enabled using the
** DBSQL_ENABLE_FTS3 macro.  But to avoid confusion we also call
** the DBSQL_ENABLE_FTS4 macro to serve as an alias for DBSQL_ENABLE_FTS3.
*/
#if defined(DBSQL_ENABLE_FTS4) && !defined(DBSQL_ENABLE_FTS3)
# define DBSQL_ENABLE_FTS3 1
#endif

/*
** The following macros mimic the standard library functions toupper(),
** isspace(), isalnum(), isdigit() and isxdigit(), respectively. The
** sqlite versions only work for ASCII characters, regardless of locale.
*/
#ifdef DBSQL_ASCII
# define __dbsql_toupper(x)  ((x)&~(__ctype_map[(unsigned char)(x)]&0x20))
# define __isspace(x)   (__ctype_map[(unsigned char)(x)]&0x01)
# define __isalnum(x)   (__ctype_map[(unsigned char)(x)]&0x06)
# define __isalpha(x)   (__ctype_map[(unsigned char)(x)]&0x02)
# define __isdigit(x)   (__ctype_map[(unsigned char)(x)]&0x04)
# define __isxdigit(x)  (__ctype_map[(unsigned char)(x)]&0x08)
# define __dbsql_tolower(x)   (__upper_to_lower[(unsigned char)(x)])
# define __isquote(x)   (__ctype_map[(unsigned char)(x)]&0x80)
# define __json_id1(x)   (__ctype_map[(unsigned char)(x)]&0x42)
# define __json_id2(x)   (__ctype_map[(unsigned char)(x)]&0x46)
#else
# define __dbsql_toupper(x)   toupper((unsigned char)(x))
# define __isspace(x)   isspace((unsigned char)(x))
# define __isalnum(x)   isalnum((unsigned char)(x))
# define __isalpha(x)   isalpha((unsigned char)(x))
# define __isdigit(x)   isdigit((unsigned char)(x))
# define __isxdigit(x)  isxdigit((unsigned char)(x))
# define __dbsql_tolower(x)   tolower((unsigned char)(x))
# define __isquote(x)   ((x)=='"'||(x)=='\''||(x)=='['||(x)=='`')
# define __json_id1(x)   (__is_id_char(x)&&(x)<'0')
# define __json_id2(x)   __is_id_char(x)
#endif
int __is_id_char(u_int8_t);

/*
** Internal function prototypes
*/
int __str_icmp(const char*,const char*);
int __strlen30(const char*);
#define __strlen30_nn(C) (strlen(C)&0x3fffffff)
char *__column_type(column_t*,char*);
#define __str_nicmp dbsql_strnicmp

int __malloc_init(void);
void __malloc_end(void);
void *__dbsql_malloc(u_int64_t);
void *__malloc_zero(u_int64_t);
void *__db_malloc_zero(DBSQL*, u_int64_t);
void *__db_malloc_raw(DBSQL*, u_int64_t);
void *__db_malloc_raw_nn(DBSQL*, u_int64_t);
char *__db_str_dup(DBSQL*,const char*);
char *__db_str_n_dup(DBSQL*,const char*, u_int64_t);
char *__db_span_dup(DBSQL*,const char*,const char*);
void *__dbsql_realloc(void*, u_int64_t);
void *__db_realloc_or_free(DBSQL *, void *, u_int64_t);
void *__db_realloc(DBSQL *, void *, u_int64_t);
void __dbsql_db_free(DBSQL*, void*);
void __db_free_nn(DBSQL*, void*);
void __db_nn_free_nn(DBSQL*, void*);
int __malloc_size(const void*);
int __db_malloc_size(DBSQL*, const void*);
void *__page_malloc(int);
void __page_free(void*);
void __mem_set_default(void);
#ifndef DBSQL_UNTESTABLE
void __benign_malloc_hooks(void (*)(void), void (*)(void));
#endif
int __heap_nearly_full(void);

/*
** On systems with ample stack space and that support alloca(), make
** use of alloca() to obtain space for large automatic objects.  By default,
** obtain space from malloc().
**
** The alloca() routine never returns NULL.  This will cause code paths
** that deal with __stack_alloc() failures to be unreachable.
*/
#ifdef DBSQL_USE_ALLOCA
# define __stack_alloc_raw(D,N)   alloca(N)
# define __stack_alloc_raw_nn(D,N) alloca(N)
# define __stack_free(D,P)
# define __stack_free_nn(D,P)
#else
# define __stack_alloc_raw(D,N)   __db_malloc_raw(D,N)
# define __stack_alloc_raw_nn(D,N) __db_malloc_raw_nn(D,N)
# define __stack_free(D,P)       __dbsql_db_free(D,P)
# define __stack_free_nn(D,P)     __db_free_nn(D,P)
#endif

/* Do not allow both MEMSYS5 and MEMSYS3 to be defined together.  If they
** are, disable MEMSYS3
*/
#ifdef DBSQL_ENABLE_MEMSYS5
const dbsql_mem_methods_t *__mem_get_memsys5(void);
#undef DBSQL_ENABLE_MEMSYS3
#endif
#ifdef DBSQL_ENABLE_MEMSYS3
const dbsql_mem_methods_t *__mem_get_memsys3(void);
#endif


#ifndef DBSQL_MUTEX_OMIT
  dbsql_mutex_methods_t const *__default_mutex(void);
  dbsql_mutex_methods_t const *__noop_mutex(void);
  dbsql_mutex_t *__dbsql_mutex_alloc(int);
  int __mutex_init(void);
  int __mutex_end(void);
#endif
#if !defined(DBSQL_MUTEX_OMIT) && !defined(DBSQL_MUTEX_NOOP)
  void __memory_barrier(void);
#else
# define __memory_barrier()
#endif

int64_t __status_value(int);
void __status_up(int, int);
void __status_down(int, int);
void __status_highwater(int, int);
int __lookaside_used(DBSQL*,int*);

/* Access to mutexes used by dbsql_status() */
dbsql_mutex_t *__pcache1_mutex(void);
dbsql_mutex_t *__malloc_mutex(void);


/* The DBSQL_THREAD_MISUSE_WARNINGS compile-time option used to be called
** DBSQL_ENABLE_MULTITHREADED_CHECKS.  Keep that older macro for backwards
** compatibility, at least for a while... */
#ifdef DBSQL_ENABLE_MULTITHREADED_CHECKS
# define DBSQL_THREAD_MISUSE_WARNINGS 1
#endif

/* DBSQL_THREAD_MISUSE_ABORT implies DBSQL_THREAD_MISUSE_WARNINGS */
#ifdef DBSQL_THREAD_MISUSE_ABORT
# define DBSQL_THREAD_MISUSE_WARNINGS 1
#endif

#if defined(DBSQL_THREAD_MISUSE_WARNINGS) && !defined(DBSQL_MUTEX_OMIT)
void __mutex_warn_on_contention(dbsql_mutex_t*);
#else
# define __mutex_warn_on_contention(x)
#endif

#ifndef DBSQL_OMIT_FLOATING_POINT
# define EXP754 (((u_int64_t)0x7ff)<<52)
# define MAN754 ((((u_int64_t)1)<<52)-1)
# define IS_NA_N(X) (((X)&EXP754)==EXP754 && ((X)&MAN754)!=0)
# define IS_OVFL(X) (((X)&EXP754)==EXP754)
  int __is_na_n(double);
  int __is_overflow(double);
#else
# define IS_NA_N(X)             0
# define __is_na_n(X)      0
# define __is_o_verflow(X) 0
#endif

/*
** An instance of the following structure holds information about SQL
** functions arguments that are the parameters to the printf() function.
*/
struct printf_arguments_t {
  int n_arg;                /* Total number of arguments */
  int n_used;               /* Number of arguments used so far */
  dbsql_value_t **args;   /* The argument values */
};

/*
** Maxium number of base-10 digits in an unsigned 64-bit integer
*/
#define DBSQL_U64_DIGITS 20

/*
** An instance of this object receives the decoding of a floating point
** value into an approximate decimal representation.
*/
struct fp_decode_t {
  int n;                           /* Significant digits in the decode */
  int i_dp;                         /* Location of the decimal point */
  char *z;                         /* Start of significant digits */
  char buf[DBSQL_U64_DIGITS+1];  /* Storage for significant digits */
  char sign;                       /* '+' or '-' */
  char is_special;                  /* 1: Infinity  2: __na_n */
};

void __fp_decode(fp_decode_t*,double,int,int);
char *__m_printf(DBSQL*,const char*, ...);
char *__vm_printf(DBSQL*,const char*, va_list);
#if defined(DBSQL_DEBUG) || defined(DBSQL_HAVE_OS_TRACE)
  void __debug_printf(const char*, ...);
#endif
#if defined(DBSQL_TEST)
  void *__test_text_to_ptr(const char*);
#endif

#if defined(DBSQL_DEBUG)
  void __tree_view_line(tree_view_t*, const char *format, ...);
  void __tree_view_expr(tree_view_t*, const expr_t*, u_int8_t);
  void __tree_view_bare_expr_list(tree_view_t*, const expr_list_t*, const char*);
  void __tree_view_expr_list(tree_view_t*, const expr_list_t*, u_int8_t, const char*);
  void __tree_view_bare_id_list(tree_view_t*, const id_list_t*, const char*);
  void __tree_view_id_list(tree_view_t*, const id_list_t*, u_int8_t, const char*);
  void __tree_view_column_list(tree_view_t*, const column_t*, int, u_int8_t);
  void __tree_view_src_list(tree_view_t*, const src_list_t*);
  void __tree_view_select(tree_view_t*, const select_t*, u_int8_t);
  void __tree_view_with(tree_view_t*, const with_t*, u_int8_t);
  void __tree_view_upsert(tree_view_t*, const upsert_t*, u_int8_t);
#if TREETRACE_ENABLED
  void __tree_view_delete(const with_t*, const src_list_t*, const expr_t*,
                             const expr_list_t*,const expr_t*, const trigger_t*);
  void __tree_view_insert(const with_t*, const src_list_t*,
                             const id_list_t*, const select_t*, const expr_list_t*,
                             int, const upsert_t*, const trigger_t*);
  void __tree_view_update(const with_t*, const src_list_t*, const expr_list_t*,
                             const expr_t*, int, const expr_list_t*, const expr_t*,
                             const upsert_t*, const trigger_t*);
#endif
#ifndef DBSQL_OMIT_TRIGGER
  void __tree_view_trigger_step(tree_view_t*, const trigger_step_t*, u_int8_t, u_int8_t);
  void __tree_view_trigger(tree_view_t*, const trigger_t*, u_int8_t, u_int8_t);
#endif
#ifndef DBSQL_OMIT_WINDOWFUNC
  void __tree_view_window(tree_view_t*, const window_t*, u_int8_t);
  void __tree_view_win_func(tree_view_t*, const window_t*, u_int8_t);
#endif
  void __show_expr(const expr_t*);
  void __show_expr_list(const expr_list_t*);
  void __show_id_list(const id_list_t*);
  void __show_src_list(const src_list_t*);
  void __show_select(const select_t*);
  void __show_with(const with_t*);
  void __show_upsert(const upsert_t*);
#ifndef DBSQL_OMIT_TRIGGER
  void __show_trigger_step(const trigger_step_t*);
  void __show_trigger_step_list(const trigger_step_t*);
  void __show_trigger(const trigger_t*);
  void __show_trigger_list(const trigger_t*);
#endif
#ifndef DBSQL_OMIT_WINDOWFUNC
  void __show_window(const window_t*);
  void __show_win_func(const window_t*);
#endif
  void __show_bitvec(bitvec_t*);
#endif

void __set_string(char **, DBSQL*, const char*);
void __progress_check(parser_t*);
void __error_msg(parser_t*, const char*, ...);
int __error_to_parser(DBSQL*,int);
void __dequote(char*);
void __dequote_expr(expr_t*);
void __dequote_token(Token*);
void __dequote_number(parser_t*, expr_t*);
void __token_init(Token*,char*);
int __keyword_code(const unsigned char*, int);
int __run_parser(parser_t*, const char*);
void __finish_coding(parser_t*);
int __get_temp_reg(parser_t*);
void __release_temp_reg(parser_t*,int);
int __get_temp_range(parser_t*,int);
void __release_temp_range(parser_t*,int,int);
void __clear_temp_reg_cache(parser_t*);
void __touch_register(parser_t*,int);
#if defined(DBSQL_ENABLE_STAT4) || defined(DBSQL_DEBUG)
int __first_available_register(parser_t*,int);
#endif
#ifdef DBSQL_DEBUG
int __no_temps_in_range(parser_t*,int,int);
#endif
expr_t *__expr_alloc(DBSQL*,int,const Token*,int);
expr_t *__expr(DBSQL*,int,const char*);
expr_t *__expr_int32(DBSQL*,int);
void __expr_attach_subtrees(DBSQL*,expr_t*,expr_t*,expr_t*);
expr_t *__p_expr(parser_t*, int, expr_t*, expr_t*);
void __p_expr_add_select(parser_t*, expr_t*, select_t*);
expr_t *__expr_and(parser_t*,expr_t*, expr_t*);
expr_t *__expr_simplified_and_or(expr_t*);
expr_t *__expr_function(parser_t*,expr_list_t*, const Token*, int);
void __expr_add_function_order_by(parser_t*,expr_t*,expr_list_t*);
void __expr_order_by_aggregate_error(parser_t*,expr_t*);
void __expr_function_usable(parser_t*,const expr_t*,const func_def_t*);
void __expr_assign_var_number(parser_t*, expr_t*, u_int32_t);
void __expr_delete(DBSQL*, expr_t*);
void __expr_delete_generic(DBSQL*,void*);
int __expr_deferred_delete(parser_t*, expr_t*);
void __expr_unmap_and_delete(parser_t*, expr_t*);
expr_list_t *__expr_list_append(parser_t*,expr_list_t*,expr_t*);
expr_list_t *__expr_list_append_vector(parser_t*,expr_list_t*,id_list_t*,expr_t*);
select_t *__expr_list_to_values(parser_t*, int, expr_list_t*);
void __expr_list_set_sort_order(expr_list_t*,int,int);
void __expr_list_set_name(parser_t*,expr_list_t*,const Token*,int);
void __expr_list_set_span(parser_t*,expr_list_t*,const char*,const char*);
void __expr_list_delete(DBSQL*, expr_list_t*);
void __expr_list_delete_generic(DBSQL*,void*);
u_int32_t __expr_list_flags(const expr_list_t*);
int __index_has_duplicate_root_page(index_t*);
int __init(DBSQL*, char**);
int __init_callback(void*, int, char**, char**);
int __init_one(DBSQL*, int, char**, u_int32_t);
void __pragma(parser_t*,Token*,Token*,Token*,int);
#ifndef DBSQL_OMIT_VIRTUALTABLE
module_def_t *__pragma_vtab_register(DBSQL*,const char *name);
#endif
void __reset_all_schemas_of_connection(DBSQL*);
void __reset_one_schema(DBSQL*,int);
void __collapse_database_array(DBSQL*);
void __commit_internal_changes(DBSQL*);
void __column_set_expr(parser_t*,table_t*,column_t*,expr_t*);
expr_t *__column_expr(table_t*,column_t*);
void __column_set_coll(DBSQL*,column_t*,const char*coll);
const char *__column_coll(column_t*);
void __delete_column_names(DBSQL*,table_t*);
void __generate_column_names(parser_t *parser, select_t *p_select);
int __columns_from_expr_list(parser_t*,expr_list_t*,int16_t*,column_t**);
void __subquery_column_types(parser_t*,table_t*,select_t*,char);
table_t *__result_set_of_select(parser_t*,select_t*,char);
void __open_schema_table(parser_t *, int);
index_t *__primary_key_index(table_t*);
int __table_column_to_index(index_t*, int);
#ifdef DBSQL_OMIT_GENERATED_COLUMNS
# define __table_column_to_storage(T,X) (X)  /* No-op pass-through */
# define __storage_column_to_table(T,X) (X)  /* No-op pass-through */
#else
  int16_t __table_column_to_storage(table_t*, int16_t);
  int16_t __storage_column_to_table(table_t*, int16_t);
#endif
void __start_table(parser_t*,Token*,Token*,int,int,int,int);
#if DBSQL_ENABLE_HIDDEN_COLUMNS
  void __column_properties_from_name(table_t*, column_t*);
#else
# define __column_properties_from_name(T,C) /* no-op */
#endif
void __add_column(parser_t*,Token,Token);
void __add_not_null(parser_t*, int);
void __add_primary_key(parser_t*, expr_list_t*, int, int, int);
void __add_check_constraint(parser_t*, expr_t*, const char*, const char*);
void __add_default_value(parser_t*,expr_t*,const char*,const char*);
void __add_collate_type(parser_t*, Token*);
void __add_generated(parser_t*,expr_t*,Token*);
void __end_table(parser_t*,Token*,Token*,u_int32_t,select_t*);
void __add_returning(parser_t*,expr_list_t*);
int __parse_uri(const char*,const char*,unsigned int*,
                    dbsql_vfs_t**,char**,char **);
#define __codec_query_parameters(A,B,C) 0
sm_t *__db_name_to_btree(DBSQL*,const char*);

#ifdef DBSQL_UNTESTABLE
# define __fault_sim(X) DBSQL_OK
#else
  int __fault_sim(int);
#endif

bitvec_t *__bitvec_create(u_int32_t);
int __bitvec_test(bitvec_t*, u_int32_t);
int __bitvec_test_not_null(bitvec_t*, u_int32_t);
int __bitvec_set(bitvec_t*, u_int32_t);
void __bitvec_clear(bitvec_t*, u_int32_t, void*);
void __bitvec_destroy(bitvec_t*);
u_int32_t __bitvec_size(bitvec_t*);
#ifndef DBSQL_UNTESTABLE
int __bitvec_builtin_test(int,int*);
#endif

row_set_t *__row_set_init(DBSQL*);
void __row_set_delete(void*);
void __row_set_clear(void*);
void __row_set_insert(row_set_t*, int64_t);
int __row_set_test(row_set_t*, int i_batch, int64_t);
int __row_set_next(row_set_t*, int64_t*);

void __create_view(parser_t*,Token*,Token*,Token*,expr_list_t*,select_t*,int,int);

#if !defined(DBSQL_OMIT_VIEW) || !defined(DBSQL_OMIT_VIRTUALTABLE)
  int __view_get_column_names(parser_t*,table_t*);
#else
# define __view_get_column_names(A,B) 0
#endif

#if DBSQL_MAX_ATTACHED>30
  int __db_mask_all_zero(db_mask_t);
#endif
void __drop_table(parser_t*, src_list_t*, int, int);
void __code_drop_table(parser_t*, table_t*, int, int);
void __delete_table(DBSQL*, table_t*);
void __delete_table_generic(DBSQL*, void*);
void __free_index(DBSQL*, index_t*);
#ifndef DBSQL_OMIT_AUTOINCREMENT
  void __autoincrement_begin(parser_t *parser);
  void __autoincrement_end(parser_t *parser);
#else
# define __autoincrement_begin(X)
# define __autoincrement_end(X)
#endif
void __insert(parser_t*, src_list_t*, select_t*, id_list_t*, int, upsert_t*);
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
  void __compute_generated_columns(parser_t*, int, table_t*);
#endif
void *__array_allocate(DBSQL*,void*,int,int*,int*);
id_list_t *__id_list_append(parser_t*, id_list_t*, Token*);
int __id_list_index(id_list_t*,const char*);
src_list_t *__src_list_enlarge(parser_t*, src_list_t*, int, int);
src_list_t *__src_list_append_list(parser_t *parser, src_list_t *p1, src_list_t *p2);
src_list_t *__src_list_append(parser_t*, src_list_t*, Token*, Token*);
void __subquery_delete(DBSQL*,subquery_t*);
select_t *__subquery_detach(DBSQL*,src_item_t*);
int __src_item_attach_subquery(parser_t*, src_item_t*, select_t*, int);
src_list_t *__src_list_append_from_term(parser_t*, src_list_t*, Token*, Token*,
                                      Token*, select_t*, on_or_using_t*);
void __src_list_indexed_by(parser_t *, src_list_t *, Token *);
void __src_list_func_args(parser_t*, src_list_t*, expr_list_t*);
int __indexed_by_lookup(parser_t *, src_item_t *);
void __src_list_shift_join_type(parser_t*,src_list_t*);
void __src_list_assign_cursors(parser_t*, src_list_t*);
void __id_list_delete(DBSQL*, id_list_t*);
void __clear_on_or_using(DBSQL*, on_or_using_t*);
void __src_list_delete(DBSQL*, src_list_t*);
index_t *__allocate_index_object(DBSQL*,int,int,char**);
void __create_index(parser_t*,Token*,Token*,src_list_t*,expr_list_t*,int,Token*,
                          expr_t*, int, int, u_int8_t);
void __drop_index(parser_t*, src_list_t*, int);
int __select(parser_t*, select_t*, select_dest_t*);
select_t *__select_new(parser_t*,expr_list_t*,src_list_t*,expr_t*,expr_list_t*,
                         expr_t*,expr_list_t*,u_int32_t,expr_t*);
void __select_delete(DBSQL*, select_t*);
void __select_delete_generic(DBSQL*,void*);
void __select_check_on_clauses(parser_t *parser, select_t *p_select);
table_t *__src_list_lookup(parser_t*, src_list_t*);
int __is_read_only(parser_t*, table_t*, trigger_t*);
void __open_table(parser_t*, int i_cur, int i_db, table_t*, int);
#if defined(DBSQL_ENABLE_UPDATE_DELETE_LIMIT) && !defined(DBSQL_OMIT_SUBQUERY)
expr_t *__limit_where(parser_t*,src_list_t*,expr_t*,expr_list_t*,expr_t*,char*);
#endif
void __code_change_count(vdbe_t*,int,const char*);
void __delete_from(parser_t*, src_list_t*, expr_t*, expr_list_t*, expr_t*);
void __update(parser_t*, src_list_t*, expr_list_t*,expr_t*,int,expr_list_t*,expr_t*,
                   upsert_t*);
where_info_t *__where_begin(parser_t*,src_list_t*,expr_t*,expr_list_t*,
                             expr_list_t*,select_t*,u_int16_t,int);
void __where_end(where_info_t*);
log_est_t __where_output_row_count(where_info_t*);
int __where_is_distinct(where_info_t*);
int __where_is_ordered(where_info_t*);
int __where_order_by_limit_opt_label(where_info_t*);
void __where_min_max_opt_early_out(vdbe_t*,where_info_t*);
int __where_is_sorted(where_info_t*);
int __where_continue_label(where_info_t*);
int __where_break_label(where_info_t*);
int __where_ok_one_pass(where_info_t*, int*);
#define ONEPASS_OFF      0        /* Use of ONEPASS not allowed */
#define ONEPASS_SINGLE   1        /* ONEPASS valid for a single row update */
#define ONEPASS_MULTI    2        /* ONEPASS is valid for multiple rows */
int __where_uses_deferred_seek(where_info_t*);
void __expr_code_load_index_column(parser_t*, index_t*, int, int, int);
int __expr_code_get_column(parser_t*, table_t*, int, int, int, u_int8_t);
void __expr_code_get_column_of_table(vdbe_t*, table_t*, int, int, int);
void __expr_code_move(parser_t*, int, int, int);
void __expr_to_register(expr_t *expr, int i_reg);
void __expr_code(parser_t*, expr_t*, int);
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
void __expr_code_generated_column(parser_t*, table_t*, column_t*, int);
#endif
void __expr_code_copy(parser_t*, expr_t*, int);
void __expr_code_factorable(parser_t*, expr_t*, int);
int __expr_code_run_just_once(parser_t*, expr_t*, int);
void __expr_null_register_range(parser_t*, int, int);
int __expr_code_temp(parser_t*, expr_t*, int*);
int __expr_code_target(parser_t*, expr_t*, int);
int __expr_code_expr_list(parser_t*, expr_list_t*, int, int, u_int8_t);
#define DBSQL_ECEL_DUP      0x01  /* Deep, not shallow copies */
#define DBSQL_ECEL_FACTOR   0x02  /* Factor out constant terms */
#define DBSQL_ECEL_REF      0x04  /* Use expr_list_t.u.x.i_order_by_col */
#define DBSQL_ECEL_OMITREF  0x08  /* Omit if expr_list_t.u.x.i_order_by_col */
void __expr_if_true(parser_t*, expr_t*, int, int);
void __expr_if_false(parser_t*, expr_t*, int, int);
void __expr_if_false_dup(parser_t*, expr_t*, int, int);
table_t *__find_table(DBSQL*,const char*, const char*);
#define LOCATE_VIEW    0x01
#define LOCATE_NOERR   0x02
table_t *__locate_table(parser_t*,u_int32_t flags,const char*, const char*);
const char *__preferred_table_name(const char*);
table_t *__locate_table_item(parser_t*,u_int32_t flags,src_item_t *);
index_t *__find_index(DBSQL*,const char*, const char*);
void __unlink_and_delete_table(DBSQL*,int,const char*);
void __unlink_and_delete_index(DBSQL*,int,const char*);
void __vacuum(parser_t*,Token*,expr_t*);
int __run_vacuum(char**, DBSQL*, int, dbsql_value_t*);
char *__name_from_token(DBSQL*, const Token*);
int __expr_compare(const parser_t*,const expr_t*,const expr_t*, int);
int __expr_compare_skip(expr_t*,expr_t*,int);
int __expr_list_compare(const expr_list_t*,const expr_list_t*, int);
int __expr_implies_expr(const parser_t*,const expr_t*,const expr_t*, int);
int __expr_implies_non_null_row(expr_t*,int,int);
void __agg_info_persist_walker_init(walker_t*,parser_t*);
void __expr_analyze_aggregates(name_context_t*, expr_t*);
void __expr_analyze_agg_list(name_context_t*,expr_list_t*);
int __expr_covered_by_index(expr_t*, int i_cur, index_t *idx);
int __references_src_list(parser_t*, expr_t*, src_list_t*);
vdbe_t *__get_vdbe(parser_t*);
#ifndef DBSQL_UNTESTABLE
void __prng_save_state(void);
void __prng_restore_state(void);
#endif
void __rollback_all(DBSQL*,int);
void __code_verify_schema(parser_t*, int);
void __code_verify_named_schema(parser_t*, const char *db);
void __begin_transaction(parser_t*, int);
void __end_transaction(parser_t*,int);
void __savepoint(parser_t*, int, Token*);
void __close_savepoints(DBSQL *);
void __leave_mutex_and_close_zombie(DBSQL*);
u_int32_t __is_true_or_false(const char*);
int __expr_id_to_true_false(expr_t*);
int __expr_truth_value(const expr_t*);
int __expr_is_constant(parser_t*,expr_t*);
int __expr_is_constant_or_function(expr_t*, u_int8_t);
int __expr_is_constant_or_group_by(parser_t*, expr_t*, expr_list_t*);
int __expr_is_single_table_constraint(expr_t*,const src_list_t*,int,int);
#ifdef DBSQL_ENABLE_CURSOR_HINTS
int __expr_contains_subquery(expr_t*);
#endif
int __expr_is_integer(const expr_t*, int*, parser_t*);
int __expr_can_be_null(const expr_t*);
int __expr_needs_no_affinity_change(const expr_t*, char);
int __expr_is_like_operator(const expr_t*);
int __is_rowid(const char*);
const char *__rowid_alias(table_t *tab);
void __generate_row_delete(
    parser_t*,table_t*,trigger_t*,int,int,int,int16_t,u_int8_t,u_int8_t,u_int8_t,int);
void __generate_row_index_delete(parser_t*, table_t*, int, int, int*, int);
int __generate_index_key(parser_t*, index_t*, int, int, int, int*,index_t*,int);
void __resolve_part_idx_label(parser_t*,int);
int __expr_references_updated_column(expr_t*,int*,int);
void __generate_constraint_checks(parser_t*,table_t*,int*,int,int,int,int,
                                     u_int8_t,u_int8_t,int,int*,int*,upsert_t*);
#ifdef DBSQL_ENABLE_NULL_TRIM
  void __set_make_record_p5(vdbe_t*,table_t*);
#else
# define __set_make_record_p5(A,B)
#endif
void __complete_insertion(parser_t*,table_t*,int,int,int,int*,int,int,int);
int __open_table_and_indices(parser_t*, table_t*, int, u_int8_t, int, u_int8_t*, int*, int*);
void __begin_write_operation(parser_t*, int, int);
void __multi_write(parser_t*);
void __may_abort(parser_t*);
void __halt_constraint(parser_t*, int, int, char*, int8_t, u_int8_t);
void __unique_constraint(parser_t*, int, index_t*);
void __rowid_constraint(parser_t*, int, table_t*);
expr_t *__expr_dup(DBSQL*,const expr_t*,int);
expr_list_t *__expr_list_dup(DBSQL*,const expr_list_t*,int);
src_list_t *__src_list_dup(DBSQL*,const src_list_t*,int);
id_list_t *__id_list_dup(DBSQL*,const id_list_t*);
select_t *__select_dup(DBSQL*,const select_t*,int);
func_def_t *__function_search(int,const char*);
void __insert_builtin_funcs(func_def_t*,int);
func_def_t *__find_function(DBSQL*,const char*,int,u_int8_t,u_int8_t);
void __quote_value(str_accum_t*,dbsql_value_t*,int);
int __append_one_utf8_character(char*, u_int32_t);
void __register_builtin_functions(void);
void __register_date_time_functions(void);
void __register_json_functions(void);
void __register_per_connection_builtin_functions(DBSQL*);
#if !defined(DBSQL_OMIT_VIRTUALTABLE) && !defined(DBSQL_OMIT_JSON)
  module_def_t *__json_vtab_register(DBSQL*,const char*);
#endif
int __safety_check_ok(DBSQL*);
int __safety_check_sick_or_ok(DBSQL*);
void __change_cookie(parser_t*, int);
with_t *__with_dup(DBSQL *db, with_t *p);

#if !defined(DBSQL_OMIT_VIRTUALTABLE) && defined(DBSQL_ENABLE_CARRAY)
  module_def_t *__carray_register(DBSQL*);
#endif

#if !defined(DBSQL_OMIT_VIEW) && !defined(DBSQL_OMIT_TRIGGER)
void __materialize_view(parser_t*, table_t*, expr_t*, expr_list_t*,expr_t*,int);
#endif

#ifndef DBSQL_OMIT_TRIGGER
  void __begin_trigger(parser_t*, Token*,Token*,int,int,id_list_t*,src_list_t*,
                           expr_t*,int, int);
  void __finish_trigger(parser_t*, trigger_step_t*, Token*);
  void __drop_trigger(parser_t*, src_list_t*, int);
  void __drop_trigger_ptr(parser_t*, trigger_t*);
  trigger_t *__triggers_exist(parser_t *, table_t*, int, expr_list_t*, int *mask);
  trigger_t *__trigger_list(parser_t *, table_t *);
  void __code_row_trigger(parser_t*, trigger_t *, int, expr_list_t*, int, table_t *,
                            int, int, int);
  void __code_row_trigger_direct(parser_t *, trigger_t *, table_t *, int, int, int);
  void dbsql_view_triggers(parser_t*, table_t*, expr_t*, int, expr_list_t*);
  void __delete_trigger_step(DBSQL*, trigger_step_t*);
  trigger_step_t *__trigger_select_step(DBSQL*,select_t*,
                                        const char*,const char*);
  trigger_step_t *__trigger_insert_step(parser_t*,src_list_t*, id_list_t*,
                                        select_t*,u_int8_t,upsert_t*,
                                        const char*,const char*);
  trigger_step_t *__trigger_update_step(parser_t*,src_list_t*,src_list_t*,expr_list_t*,
                                        expr_t*, u_int8_t, const char*,const char*);
  trigger_step_t *__trigger_delete_step(parser_t*,src_list_t*, expr_t*,
                                        const char*,const char*);
  void __delete_trigger(DBSQL*, trigger_t*);
  void __unlink_and_delete_trigger(DBSQL*,int,const char*);
  u_int32_t __trigger_colmask(parser_t*,trigger_t*,expr_list_t*,int,int,table_t*,int);
# define __parse_toplevel(p) ((p)->toplevel ? (p)->toplevel : (p))
# define __is_toplevel(p) ((p)->toplevel==0)
#else
# define __triggers_exist(B,C,D,E,F) 0
# define __delete_trigger(A,B)
# define __drop_trigger_ptr(A,B)
# define __unlink_and_delete_trigger(A,B,C)
# define __code_row_trigger(A,B,C,D,E,F,G,H,I)
# define __code_row_trigger_direct(A,B,C,D,E,F)
# define __trigger_list(X, Y) 0
# define __parse_toplevel(p) p
# define __is_toplevel(p) 1
# define __trigger_colmask(A,B,C,D,E,F,G) 0
#endif

int __join_type(parser_t*, Token*, Token*, Token*);
int __column_index(table_t *tab, const char *col);
void __src_item_column_used(src_item_t*,int);
void __set_join_expr(expr_t*,int,u_int32_t);
void __create_foreign_key(parser_t*, expr_list_t*, Token*, expr_list_t*, int);
void __defer_foreign_key(parser_t*, int);
#ifndef DBSQL_OMIT_AUTHORIZATION
  void __auth_read(parser_t*,expr_t*,schema_t*,src_list_t*);
  int __auth_check(parser_t*,int, const char*, const char*, const char*);
  void __auth_context_push(parser_t*, auth_context_t*, const char*);
  void __auth_context_pop(auth_context_t*);
  int __auth_read_col(parser_t*, const char *, const char *, int);
#else
# define __auth_read(a,b,c,d)
# define __auth_check(a,b,c,d,e)    DBSQL_OK
# define __auth_context_push(a,b,c)
# define __auth_context_pop(a)  ((void)(a))
#endif
int __db_is_named(DBSQL *db, int i_db, const char *name);
void __attach(parser_t*, expr_t*, expr_t*, expr_t*);
void __detach(parser_t*, expr_t*);
void __fix_init(db_fixer_t*, parser_t*, int, const char*, const Token*);
int __fix_src_list(db_fixer_t*, src_list_t*);
int __fix_select(db_fixer_t*, select_t*);
int __fix_expr(db_fixer_t*, expr_t*);
int __fix_trigger_step(db_fixer_t*, trigger_step_t*);

int __real_same_as_int(double,int64_t);
int64_t __real_to_i64(double);
int __int64_to_text(int64_t,char*);
int __ato_f(const char *z, double*);
int __get_int32(const char *, int*);
int __get_u_int32(const char*, u_int32_t*);
int __atoi(const char*);
#ifndef DBSQL_OMIT_UTF16
int __utf16_byte_len(const void *data, int n_byte, int n_char);
#endif
int __utf8_char_len(const char *data, int n_byte);
u_int32_t __utf8_read(const u_int8_t**);
int __utf8_read_limited(const u_int8_t*, int, u_int32_t*);
log_est_t __log_est(u_int64_t);
log_est_t __log_est_add(log_est_t,log_est_t);
log_est_t __log_est_from_double(double);
u_int64_t __log_est_to_int(log_est_t);
vlist_t *__v_list_add(DBSQL*,vlist_t*,const char*,int,int);
const char *__v_list_num_to_name(vlist_t*,int);
int __v_list_name_to_num(vlist_t*,const char*,int);

/*
** Routines to read and write variable-length integers.  These used to
** be defined locally, but now we use the varint routines in the util.c
** file.
*/
int __put_varint(unsigned char*, u_int64_t);
u_int8_t __get_varint(const unsigned char *, u_int64_t *);
u_int8_t __get_varint32(const unsigned char *, u_int32_t *);
int __varint_len(u_int64_t v);

/*
** The common case is for a varint to be a single byte.  They following
** macros handle the common case without a procedure call, but then call
** the procedure for larger varints.
*/
#define GET_VARINT32(A,B)  \
  (u_int8_t)((*(A)<(u_int8_t)0x80)?((B)=(u_int32_t)*(A)),1:__get_varint32((A),(u_int32_t *)&(B)))
#define GET_VARINT32_NR(A,B) \
  B=(u_int32_t)*(A);if(B>=0x80)__get_varint32((A),(u_int32_t*)&(B))
#define PUT_VARINT32(A,B)  \
  (u_int8_t)(((u_int32_t)(B)<(u_int32_t)0x80)?(*(A)=(unsigned char)(B)),1:\
  __put_varint((A),(B)))
#define __get_varint    __get_varint
#define __put_varint    __put_varint


const char *__index_affinity_str(DBSQL*, index_t*);
char *__table_affinity_str(DBSQL*,const table_t*);
void __table_affinity(vdbe_t*, table_t*, int);
char __compare_affinity(const expr_t *expr, char aff2);
int __index_affinity_ok(const expr_t *expr, char idx_affinity);
char __table_column_affinity(const table_t*,int);
char __expr_affinity(const expr_t *expr);
int __expr_data_type(const expr_t *expr);
int __atoi64(const char*, int64_t*, int, u_int8_t);
int __dec_or_hex_to_i64(const char*, int64_t*);
void __error_with_msg(DBSQL*, int, const char*,...);
void __dbsql_error(DBSQL*,int);
void __error_clear(DBSQL*);
void __system_error(DBSQL*,int);
#if !defined(DBSQL_OMIT_BLOB_LITERAL)
void *__hex_to_blob(DBSQL*, const char *z, int n);
#endif
u_int8_t __hex_to_int(int h);
int __two_part_name(parser_t *, Token *, Token *, Token **);

#if defined(DBSQL_NEED_ERR_NAME)
const char *__err_name(int);
#endif

#ifndef DBSQL_OMIT_DESERIALIZE
int __memdb_init(void);
int __is_memdb(const dbsql_vfs_t*);
#else
# define __is_memdb(X) 0
#endif

const char *__err_str(int);
int __read_schema(parser_t *parser);
coll_seq_t *__find_coll_seq(DBSQL*,u_int8_t enc, const char*,int);
int __is_binary(const coll_seq_t*);
coll_seq_t *__locate_coll_seq(parser_t *parser, const char*name);
void __set_text_encoding(DBSQL *db, u_int8_t);
coll_seq_t *__expr_coll_seq(parser_t *parser, const expr_t *expr);
coll_seq_t *__expr_nn_coll_seq(parser_t *parser, const expr_t *expr);
int __expr_coll_seq_match(parser_t*,const expr_t*,const expr_t*);
expr_t *__expr_add_collate_token(const parser_t *parser, expr_t*, const Token*, int);
expr_t *__expr_add_collate_string(const parser_t*,expr_t*,const char*);
expr_t *__expr_skip_collate(expr_t*);
expr_t *__expr_skip_collate_and_likely(expr_t*);
int __check_coll_seq(parser_t *, coll_seq_t *);
int __writable_schema(DBSQL*);
int __check_object_name(parser_t*, const char*,const char*,const char*);
void __vdbe_set_changes(DBSQL *, int64_t);
int __add_int64(int64_t*,int64_t);
int __sub_int64(int64_t*,int64_t);
int __mul_int64(int64_t*,int64_t);
int __abs_int32(int);
#ifdef DBSQL_ENABLE_8_3_NAMES
void __file_suffix3(const char*, char*);
#else
# define __file_suffix3(X,Y)
#endif
u_int8_t __get_boolean(const char *z,u_int8_t);

const void *__value_text(dbsql_value_t*, u_int8_t);
int __value_is_of_class(const dbsql_value_t*, void(*)(void*));
int __value_bytes(dbsql_value_t*, u_int8_t);
void __value_set_str(dbsql_value_t*, int, const void *,u_int8_t,
                        void(*)(void*));
void __value_set_null(dbsql_value_t*);
void __value_free(dbsql_value_t*);
#ifndef DBSQL_UNTESTABLE
void __result_int_real(dbsql_func_t*);
#endif
dbsql_value_t *__value_new(DBSQL *);
#ifndef DBSQL_OMIT_UTF16
char *__utf16to8(DBSQL *, const void*, int, u_int8_t);
#endif
int __value_from_expr(DBSQL *, const expr_t *, u_int8_t, u_int8_t, dbsql_value_t **);
void __value_apply_affinity(dbsql_value_t *, u_int8_t, u_int8_t);
#ifndef DBSQL_AMALGAMATION
extern const unsigned char __opcode_property[];
extern const char __str_binary[];
extern const unsigned char __std_type_len[];
extern const char __std_type_affinity[];
extern const char *__std_type[];
extern const unsigned char __upper_to_lower[];
extern const unsigned char *__dbsqlaLTb;
extern const unsigned char *__dbsqlaEQb;
extern const unsigned char *__dbsqlaGTb;
extern const unsigned char __ctype_map[];
extern DBSQL_WSD struct dbsql_config __config;
extern func_def_hash_t __builtin_functions;
#ifndef DBSQL_OMIT_WSD
extern int __pending_byte;
#endif
#endif /* DBSQL_AMALGAMATION */
#ifdef VDBE_PROFILE
extern u_int64_t __n_profile_cnt;
#endif
void __root_page_moved(DBSQL*, int, pgno_t, pgno_t);
void __reindex(parser_t*, Token*, Token*);
void __alter_functions(void);
void __alter_rename_table(parser_t*, src_list_t*, Token*);
void __alter_rename_column(parser_t*, src_list_t*, Token*, Token*);
void __alter_drop_constraint(parser_t*,src_list_t*,Token*,Token*);
void __alter_add_constraint(
  parser_t *parser,           /* parser_t context */
  src_list_t *src,           /* table_t to add constraint to */
  Token *first,           /* First token of new constraint */
  Token *name,            /* Name of new constraint. NULL if name omitted. */
  const char *z_expr,       /* Text of CHECK expression */
  int n_expr,               /* Size of expr in bytes */
  expr_t *expr              /* The parsed CHECK expression */
);
void __alter_set_not_null(parser_t*, src_list_t*, Token*, Token*);
int64_t __get_token(const unsigned char *, int *);
void __nested_parse(parser_t*, const char*, ...);
void __expire_prepared_statements(DBSQL*, int);
void __code_rhs_of_in(parser_t*, expr_t*, int, int);
int __code_subselect(parser_t*, expr_t*);
void __select_prep(parser_t*, select_t*, name_context_t*);
int __expand_subquery(parser_t*, src_item_t*);
void __select_wrong_num_terms_error(parser_t *parser, select_t *p);
int __match_e_name(
  const struct expr_list_item_t*,
  const char*,
  const char*,
  const char*,
  int*
);
Bitmask __expr_col_used(expr_t*);
u_int8_t __str_i_hash(const char*);
int __resolve_expr_names(name_context_t*, expr_t*);
int __resolve_expr_list_names(name_context_t*, expr_list_t*);
void __resolve_select_names(parser_t*, select_t*, name_context_t*);
int __resolve_self_reference(parser_t*,table_t*,int,expr_t*,expr_list_t*);
int __resolve_order_group_by(parser_t*, select_t*, expr_list_t*, const char*);
void __column_default(vdbe_t *, table_t *, int, int);
void __alter_finish_add_column(parser_t *, Token *);
void __alter_begin_add_column(parser_t *, src_list_t *);
void __alter_drop_column(parser_t*, src_list_t*, const Token*);
const void *__rename_token_map(parser_t*, const void*, const Token*);
void __rename_token_remap(parser_t*, const void *to, const void *from);
void __rename_expr_unmap(parser_t*, expr_t*);
void __rename_exprlist_unmap(parser_t*, expr_list_t*);
coll_seq_t *__get_coll_seq(parser_t*, u_int8_t, coll_seq_t *, const char*);
char __affinity_type(const char*, column_t*);
void __analyze(parser_t*, Token*, Token*);
int __invoke_busy_handler(busy_handler_t*);
int __find_db(DBSQL*, Token*);
int __find_db_name(DBSQL *, const char *);
int __analysis_load(DBSQL*,int i_db);
void __delete_index_samples(DBSQL*,index_t*);
void __default_row_est(index_t*);
void __register_like_functions(DBSQL*, int);
int __is_like_function(DBSQL*,expr_t*,int*,char*);
void __schema_clear(void *);
schema_t *__schema_get(DBSQL *, sm_t *);
int __schema_to_index(DBSQL *db, schema_t *);
key_info_t *__key_info_alloc(DBSQL*,int,int);
void __key_info_unref(key_info_t*);
key_info_t *__key_info_ref(key_info_t*);
key_info_t *__key_info_of_index(parser_t*, index_t*);
key_info_t *__key_info_from_expr_list(parser_t*, expr_list_t*, int, int);
const char *__select_op_name(int);
int __has_explicit_nulls(parser_t*, expr_list_t*);

#ifdef DBSQL_DEBUG
int __key_info_is_writeable(key_info_t*);
#endif
int __create_func(DBSQL *, const char *, int, int, void *,
  void (*)(dbsql_func_t*,int,dbsql_value_t **),
  void (*)(dbsql_func_t*,int,dbsql_value_t **),
  void (*)(dbsql_func_t*),
  void (*)(dbsql_func_t*),
  void (*)(dbsql_func_t*,int,dbsql_value_t **),
  func_destructor_t *destructor
);
void __noop_destructor(void*);
void *__oom_fault(DBSQL*);
void __oom_clear(DBSQL*);
int __api_exit(DBSQL *db, int);
int __open_temp_database(parser_t *);

char *__rc_str_ref(char*);
void __rc_str_unref(void*);
char *__rc_str_new(u_int64_t);
char *__rc_str_resize(char*,u_int64_t);

void __str_accum_init(str_accum_t*, DBSQL*, char*, int, int);
int __str_accum_enlarge(str_accum_t*, int64_t);
int __str_accum_enlarge_if_needed(str_accum_t*, int64_t);
char *__str_accum_finish(str_accum_t*);
void __str_accum_set_error(str_accum_t*, u_int8_t);
void __result_str_accum(dbsql_func_t*,str_accum_t*);
void __select_dest_init(select_dest_t*,int,int);
expr_t *__create_column_expr(DBSQL *, src_list_t *, int, int);
void __record_error_byte_offset(DBSQL*,const char*);
void __record_error_offset_of_expr(DBSQL*,const expr_t*);

void __backup_restart(dbsql_backup_t *);
void __backup_update(dbsql_backup_t *, pgno_t, const u_int8_t *);

#ifndef DBSQL_OMIT_SUBQUERY
int __expr_check_in(parser_t*, expr_t*);
#else
# define __expr_check_in(x,y) DBSQL_OK
#endif

#ifdef DBSQL_ENABLE_STAT4
int __stat4_probe_set_value(
    parser_t*,index_t*,unpacked_record_t**,expr_t*,int,int,int*);
int __stat4_value_from_expr(parser_t*, expr_t*, u_int8_t, dbsql_value_t**);
void __stat4_probe_free(unpacked_record_t*);
int __stat4_column(DBSQL*, const void*, int, int, dbsql_value_t**);
char __index_column_affinity(DBSQL*, index_t*, int);
#endif

/*
** The interface to the LEMON-generated parser
*/
#ifndef DBSQL_AMALGAMATION
  void *dbsql_parserAlloc(void*(*)(u_int64_t), parser_t*);
  void dbsql_parserFree(void*, void(*)(void*));
#endif
void dbsql_parser(void*, int, Token);
int dbsql_parserFallback(int);
#ifdef YYTRACKMAXSTACKDEPTH
  int dbsql_parserStackPeak(void*);
#endif

void __auto_load_extensions(DBSQL*);
#ifndef DBSQL_OMIT_LOAD_EXTENSION
  void __close_extensions(DBSQL*);
#else
# define __close_extensions(X)
#endif

#ifndef DBSQL_OMIT_SHARED_CACHE
  void __table_lock(parser_t *, int, pgno_t, u_int8_t, const char *);
#else
  #define __table_lock(v,w,x,y,z)
#endif

#ifdef DBSQL_TEST
  int __utf8_to8(unsigned char*);
#endif

#ifdef DBSQL_OMIT_VIRTUALTABLE
#  define __vtab_clear(D,T)
#  define __vtab_sync(X,Y) DBSQL_OK
#  define __vtab_rollback(X)
#  define __vtab_commit(X)
#  define __vtab_in_sync(db) 0
#  define __vtab_lock(X)
#  define __vtab_unlock(X)
#  define __vtab_module_unref(D,X)
#  define __vtab_unlock_list(X)
#  define __vtab_savepoint(X, Y, Z) DBSQL_OK
#  define __get_v_table(X,Y)  ((v_table_t*)0)
#else
   void __vtab_clear(DBSQL *db, table_t*);
   void __vtab_disconnect(DBSQL *db, table_t *p);
   int __vtab_sync(DBSQL *db, vdbe_t*);
   int __vtab_rollback(DBSQL *db);
   int __vtab_commit(DBSQL *db);
   void __vtab_lock(v_table_t *);
   void __vtab_unlock(v_table_t *);
   void __vtab_module_unref(DBSQL*,module_def_t*);
   void __vtab_unlock_list(DBSQL*);
   int __vtab_savepoint(DBSQL *, int, int);
   void __vtab_import_errmsg(vdbe_t*, dbsql_vtab_t*);
   v_table_t *__get_v_table(DBSQL*, table_t*);
   module_def_t *__vtab_create_module(
     DBSQL*,
     const char*,
     const dbsql_module_t*,
     void*,
     void(*)(void*)
   );
#  define __vtab_in_sync(db) ((db)->n_v_trans>0 && (db)->v_trans==0)
#endif
int __read_only_shadow_tables(DBSQL *db);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  int __shadow_table_name(DBSQL *db, const char *name);
  int __is_shadow_table_of(DBSQL*,table_t*,const char*);
  void __mark_all_shadow_tables_of(DBSQL*, table_t*);
#else
# define __shadow_table_name(A,B) 0
# define __is_shadow_table_of(A,B,C) 0
# define __mark_all_shadow_tables_of(A,B)
#endif
int __vtab_eponymous_table_init(parser_t*,module_def_t*);
void __vtab_eponymous_table_clear(DBSQL*,module_def_t*);
void __vtab_make_writable(parser_t*,table_t*);
void __vtab_begin_parse(parser_t*, Token*, Token*, Token*, int);
void __vtab_finish_parse(parser_t*, Token*);
void __vtab_arg_init(parser_t*);
void __vtab_arg_extend(parser_t*, Token*);
int __vtab_call_create(DBSQL*, int, const char *, char **);
int __vtab_call_connect(parser_t*, table_t*);
int __vtab_call_destroy(DBSQL*, int, const char *);
int __vtab_begin(DBSQL *, v_table_t *);

func_def_t *__vtab_overload_function(DBSQL *,func_def_t*, int n_arg, expr_t*);
void __vtab_uses_all_schemas(parser_t*);
int64_t __stmt_current_time(dbsql_func_t*);
int __vdbe_parameter_index(vdbe_t*, const char*, int);
int __transfer_bindings(dbsql_stmt_t *, dbsql_stmt_t *);
void __parse_object_init(parser_t*,DBSQL*);
void __parse_object_reset(parser_t*);
void *__parser_add_cleanup(parser_t*,void(*)(DBSQL*,void*),void*);
#ifdef DBSQL_ENABLE_NORMALIZE
char *__normalize(vdbe_t*, const char*);
#endif
int __reprepare(vdbe_t*);
void __expr_list_check_length(parser_t*, expr_list_t*, const char*);
coll_seq_t *__expr_compare_coll_seq(parser_t*,const expr_t*);
coll_seq_t *__binary_compare_coll_seq(parser_t *, const expr_t*, const expr_t*);
int __temp_in_memory(const DBSQL*);
const char *__journal_modename(int);
#ifndef DBSQL_OMIT_WAL
  int __checkpoint(DBSQL*, int, int, int*, int*);
  int __wal_default_hook(void*,DBSQL*,const char*,int);
#endif
#ifndef DBSQL_OMIT_CTE
  cte_t *__cte_new(parser_t*,Token*,expr_list_t*,select_t*,u_int8_t);
  void __cte_delete(DBSQL*,cte_t*);
  with_t *__with_add(parser_t*,with_t*,cte_t*);
  void __with_delete(DBSQL*,with_t*);
  void __with_delete_generic(DBSQL*,void*);
  with_t *__with_push(parser_t*, with_t*, u_int8_t);
#else
# define __cte_new(P,T,E,S)   ((void*)0)
# define __cte_delete(D,C)
# define __cte_with_add(P,W,C) ((void*)0)
# define __with_delete(x,y)
# define __with_push(x,y,z) ((void*)0)
#endif
#ifndef DBSQL_OMIT_UPSERT
  upsert_t *__upsert_new(DBSQL*,expr_list_t*,expr_t*,expr_list_t*,expr_t*,upsert_t*);
  void __upsert_delete(DBSQL*,upsert_t*);
  upsert_t *__upsert_dup(DBSQL*,upsert_t*);
  int __upsert_analyze_target(parser_t*,src_list_t*,upsert_t*,upsert_t*);
  void __upsert_do_update(parser_t*,upsert_t*,table_t*,index_t*,int);
  upsert_t *__upsert_of_index(upsert_t*,index_t*);
  int __upsert_next_is_ipk(upsert_t*);
#else
#define __upsert_new(u,v,w,x,y,z) ((upsert_t*)0)
#define __upsert_delete(x,y)
#define __upsert_dup(x,y)         ((upsert_t*)0)
#define __upsert_of_index(x,y)     ((upsert_t*)0)
#define __upsert_next_is_ipk(x)     0
#endif


/* Declarations for functions in fkey.c. All of these are replaced by
** no-op macros if OMIT_FOREIGN_KEY is defined. In this case no foreign
** key functionality is available. If OMIT_TRIGGER is defined but
** OMIT_FOREIGN_KEY is not, only some of the functions are no-oped. In
** this case foreign keys are parsed, but no other functionality is
** provided (enforcement of FK constraints requires the triggers sub-system).
*/
#if !defined(DBSQL_OMIT_FOREIGN_KEY) && !defined(DBSQL_OMIT_TRIGGER)
  void __fk_check(parser_t*, table_t*, int, int, int*, int);
  void __fk_drop_table(parser_t*, src_list_t *, table_t*);
  void __fk_actions(parser_t*, table_t*, expr_list_t*, int, int*, int);
  int __fk_required(parser_t*, table_t*, int*, int);
  u_int32_t __fk_oldmask(parser_t*, table_t*);
  f_key_t *__fk_references(table_t *);
  void __fk_clear_trigger_cache(DBSQL*,int);
#else
  #define __fk_actions(a,b,c,d,e,f)
  #define __fk_check(a,b,c,d,e,f)
  #define __fk_drop_table(a,b,c)
  #define __fk_oldmask(a,b)         0
  #define __fk_required(a,b,c,d)    0
  #define __fk_references(a)        0
  #define __fk_clear_trigger_cache(a,b)
#endif
#ifndef DBSQL_OMIT_FOREIGN_KEY
  void __fk_delete(DBSQL *, table_t*);
  int __fk_locate_index(parser_t*,table_t*,f_key_t*,index_t**,int**);
#else
  #define __fk_delete(a,b)
  #define __fk_locate_index(a,b,c,d,e)
#endif


/*
** Available fault injectors.  Should be numbered beginning with 0.
*/
#define DBSQL_FAULTINJECTOR_MALLOC     0
#define DBSQL_FAULTINJECTOR_COUNT      1

/*
** The interface to the code in fault.c used for identifying "benign"
** malloc failures. This is only present if DBSQL_UNTESTABLE
** is not defined.
*/
#ifndef DBSQL_UNTESTABLE
  void __begin_benign_malloc(void);
  void __end_benign_malloc(void);
#else
  #define __begin_benign_malloc()
  #define __end_benign_malloc()
#endif

/*
** Allowed return values from __find_in_index()
*/
#define IN_INDEX_ROWID        1   /* Search the rowid of the table */
#define IN_INDEX_EPH          2   /* Search an ephemeral b-tree */
#define IN_INDEX_INDEX_ASC    3   /* Existing index ASCENDING */
#define IN_INDEX_INDEX_DESC   4   /* Existing index DESCENDING */
#define IN_INDEX_NOOP         5   /* No table available. Use comparisons */
/*
** Allowed flags for the 3rd parameter to __find_in_index().
*/
#define IN_INDEX_NOOP_OK     0x0001  /* OK to return IN_INDEX_NOOP */
#define IN_INDEX_MEMBERSHIP  0x0002  /* IN operator used for membership test */
#define IN_INDEX_LOOP        0x0004  /* IN operator used as a loop */
int __find_in_index(parser_t *, expr_t *, u_int32_t, int*, int*, int*);

int __journal_open(dbsql_vfs_t *, const char *, dbsql_file_t *, int, int);
int __journal_size(dbsql_vfs_t *);
#if defined(DBSQL_ENABLE_ATOMIC_WRITE) \
 || defined(DBSQL_ENABLE_BATCH_ATOMIC_WRITE)
  int __journal_create(dbsql_file_t *);
#endif

int __journal_is_in_memory(dbsql_file_t *p);
void __mem_journal_open(dbsql_file_t *);

void __expr_set_height_and_flags(parser_t *parser, expr_t *p);
#if DBSQL_MAX_EXPR_DEPTH>0
  int __select_expr_height(const select_t *);
  int __expr_check_height(parser_t*, int);
#else
  #define __select_expr_height(x) 0
  #define __expr_check_height(x,y)
#endif
void __expr_set_error_offset(expr_t*,int);

u_int32_t __get4byte(const u_int8_t*);
void __put4byte(u_int8_t*, u_int32_t);

#ifdef DBSQL_ENABLE_UNLOCK_NOTIFY
  void __connection_blocked(DBSQL *, DBSQL *);
  void __connection_unlocked(DBSQL *db);
  void __connection_closed(DBSQL *db);
#else
  #define __connection_blocked(x,y)
  #define __connection_unlocked(x)
  #define __connection_closed(x)
#endif

#ifdef DBSQL_DEBUG
  void dbsql_parserTrace(FILE*, char *);
#endif
#if defined(YYCOVERAGE)
  int dbsql_parserCoverage(FILE*);
#endif

/*
** If the DBSQL_ENABLE IOTRACE exists then the global variable
** __io_trace is a pointer to a printf-like routine used to
** print I/O tracing messages.
*/
#ifdef DBSQL_ENABLE_IOTRACE
# define IOTRACE(A)  if( __io_trace ){ __io_trace A; }
  void __vdbe_io_trace_sql(vdbe_t*);
DBSQL_API DBSQL_EXTERN void (DBSQL_CDECL *__io_trace)(const char*,...);
#else
# define IOTRACE(A)
# define __vdbe_io_trace_sql(X)
#endif

/*
** These routines are available for the mem2.c debugging memory allocator
** only.  They are used to verify that different "types" of memory
** allocations are properly tracked by the system.
**
** __memdebug_set_type() sets the "type" of an allocation to one of
** the MEMTYPE_* macros defined below.  The type must be a bitmask with
** a single bit set.
**
** __memdebug_has_type() returns true if any of the bits in its second
** argument match the type set by the previous __memdebug_set_type().
** __memdebug_has_type() is intended for use inside assert() statements.
**
** __memdebug_no_type() returns true if none of the bits in its second
** argument match the type set by the previous __memdebug_set_type().
**
** Perhaps the most important point is the difference between MEMTYPE_HEAP
** and MEMTYPE_LOOKASIDE.  If an allocation is MEMTYPE_LOOKASIDE, that means
** it might have been allocated by lookaside, except the allocation was
** too large or lookaside was already full.  It is important to verify
** that allocations that might have been satisfied by lookaside are not
** passed back to non-lookaside free() routines.  Asserts such as the
** example above are placed on the non-lookaside free() routines to verify
** this constraint.
**
** All of this is no-op for a production build.  It only comes into
** play when the DBSQL_MEMDEBUG compile-time option is used.
*/
#ifdef DBSQL_MEMDEBUG
  void __memdebug_set_type(void*,u_int8_t);
  int __memdebug_has_type(const void*,u_int8_t);
  int __memdebug_no_type(const void*,u_int8_t);
#else
# define __memdebug_set_type(X,Y)  /* no-op */
# define __memdebug_has_type(X,Y)  1
# define __memdebug_no_type(X,Y)   1
#endif
#define MEMTYPE_HEAP       0x01  /* General heap allocations */
#define MEMTYPE_LOOKASIDE  0x02  /* Heap that might have been lookaside */
#define MEMTYPE_PCACHE     0x04  /* Page cache allocations */

/*
** Threading interface
*/
#if DBSQL_MAX_WORKER_THREADS>0
int __thread_create(sq_lite_thread_t**,void*(*)(void*),void*);
int __thread_join(sq_lite_thread_t*, void**);
#endif

#if defined(DBSQL_ENABLE_DBPAGE_VTAB) || defined(DBSQL_TEST)
int __dbpage_register(DBSQL*);
#endif
#if defined(DBSQL_ENABLE_DBSTAT_VTAB) || defined(DBSQL_TEST)
int __dbstat_register(DBSQL*);
#endif

int __expr_vector_size(const expr_t *expr);
int __expr_is_vector(const expr_t *expr);
expr_t *__vector_field_subexpr(expr_t*, int);
expr_t *__expr_for_vector_field(parser_t*,expr_t*,int,int);
void __vector_error_msg(parser_t*, expr_t*);

#ifndef DBSQL_OMIT_COMPILEOPTION_DIAGS
const char **__compile_options(int *pn_opt);
#endif

#if DBSQL_OS_UNIX && defined(DBSQL_OS_KV_OPTIONAL)
int __kvvfs_init(void);
#endif

#if defined(VDBE_PROFILE) \
 || defined(DBSQL_PERFORMANCE_TRACE) \
 || defined(DBSQL_ENABLE_STMT_SCANSTATUS)
u_int64_t __hwtime(void);
#endif

#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
# define IS_STMT_SCANSTATUS(db) (db->flags & DBSQL_StmtScanStatus)
#else
# define IS_STMT_SCANSTATUS(db) 0
#endif

#endif /* DBSQLINT_H */
