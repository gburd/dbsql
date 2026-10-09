/*
** 2008 May 27
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
** This file contains inline asm code for retrieving "high-performance"
** counters for x86 and x86_64 class CPUs.
*/
#ifndef DBSQL_HWTIME_H
#define DBSQL_HWTIME_H

#if defined(_MSC_VER) && defined(_WIN32)

  #include "windows.h"
  #include <profileapi.h>

  __inline u_int64_t __hwtime(void){
    LARGE_INTEGER tm;
    __query_performance_counter(&tm);
    return (u_int64_t)tm.__quad_part;
  }

#elif !defined(__STRICT_ANSI__) && defined(__GNUC__) && \
    (defined(i386) || defined(__i386__) || defined(_M_IX86))

  __inline__ u_int64_t __hwtime(void){
     unsigned int lo, hi;
     __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
     return (u_int64_t)hi << 32 | lo;
  }

#elif !defined(__STRICT_ANSI__) && (defined(__GNUC__) && defined(__x86_64__))

  __inline__ u_int64_t __hwtime(void){
     unsigned int lo, hi;
     __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
     return (u_int64_t)hi << 32 | lo;
  }

#elif !defined(__STRICT_ANSI__) && defined(__GNUC__) &&  defined(__aarch64__)

  __inline__ u_int64_t __hwtime(void){
     u_int64_t cnt;
     __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (cnt));
     return cnt;
  }
 
#elif !defined(__STRICT_ANSI__) && (defined(__GNUC__) && defined(__ppc__))

  __inline__ u_int64_t __hwtime(void){
      unsigned long long retval;
      unsigned long junk;
      __asm__ __volatile__ ("\n\
          1:      mftbu   %1\n\
                  mftb    %L0\n\
                  mftbu   %0\n\
                  cmpw    %0,%1\n\
                  bne     1b"
                  : "=r" (retval), "=r" (junk));
      return retval;
  }

#else

  /*
  ** asm() is needed for hardware timing support.  Without asm(),
  ** disable the __hwtime() routine.
  **
  ** __hwtime() is only used for some obscure debugging
  ** and analysis configurations, not in any deliverable, so this
  ** should not be a great loss.
  */
  u_int64_t __hwtime(void){ return ((u_int64_t)0); }

#endif

#endif /* !defined(DBSQL_HWTIME_H) */
