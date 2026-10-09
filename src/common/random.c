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
** This file contains code to implement a pseudo-random number
** generator (PRNG) for dbsql.
**
** Random numbers are used by some of the database backends in order
** to generate random integer keys for tables or random filenames.
*/
#include "dbsql_int.h"


/* All threads share a single random number generator.
** This structure is the current state of the generator.
*/
static DBSQL_WSD struct __prng_type {
  u_int32_t s[16];                 /* 64 bytes of chacha20 state */
  u_int8_t out[64];                /* Output bytes */
  u_int8_t n;                      /* Output bytes remaining */
} __prng;


/* The RFC-7539 __cha_cha20 block function
*/
#define ROTL(a,b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d) ( \
    a += b, d ^= a, d = ROTL(d,16), \
    c += d, b ^= c, b = ROTL(b,12), \
    a += b, d ^= a, d = ROTL(d, 8), \
    c += d, b ^= c, b = ROTL(b, 7))
static void chacha_block(u_int32_t *out, const u_int32_t *in){
  int i;
  u_int32_t x[16];
  memcpy(x, in, 64);
  for(i=0; i<10; i++){
    QR(x[0], x[4], x[ 8], x[12]);
    QR(x[1], x[5], x[ 9], x[13]);
    QR(x[2], x[6], x[10], x[14]);
    QR(x[3], x[7], x[11], x[15]);
    QR(x[0], x[5], x[10], x[15]);
    QR(x[1], x[6], x[11], x[12]);
    QR(x[2], x[7], x[ 8], x[13]);
    QR(x[3], x[4], x[ 9], x[14]);
  }
  for(i=0; i<16; i++) out[i] = x[i]+in[i];
}

/*
** Return N random bytes.
*/
void dbsql_randomness(int N, void *buf){
  unsigned char *z_buf = buf;

  /* The "__wsd_prng" macro will resolve to the pseudo-random number generator
  ** state vector.  If writable static data is unsupported on the target,
  ** we have to locate the state vector at run-time.  In the more common
  ** case where writable static data is supported, __wsd_prng can refer directly
  ** to the "__prng" state vector declared above.
  */
#ifdef DBSQL_OMIT_WSD
  struct __prng_type *p = &GLOBAL(struct __prng_type, __prng);
# define __wsd_prng p[0]
#else
# define __wsd_prng __prng
#endif

#if DBSQL_THREADSAFE
  dbsql_mutex_t *mutex;
#endif

#ifndef DBSQL_OMIT_AUTOINIT
  if( dbsql_initialize() ) return;
#endif

#if DBSQL_THREADSAFE
  mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_PRNG);
#endif

  dbsql_mutex_enter(mutex);
  if( N<=0 || buf==0 ){
    __wsd_prng.s[0] = 0;
    dbsql_mutex_leave(mutex);
    return;
  }

  /* Initialize the state of the random number generator once,
  ** the first time this routine is called.
  */
  if( __wsd_prng.s[0]==0 ){
    dbsql_vfs_t *vfs = dbsql_vfs_find(0);
    static const u_int32_t chacha20_init[] = {
      0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
    };
    memcpy(&__wsd_prng.s[0], chacha20_init, 16);
    if( NEVER(vfs==0) ){
      memset(&__wsd_prng.s[4], 0, 44);
    }else{
      __dbsql_os_randomness(vfs, 44, (char*)&__wsd_prng.s[4]);
    }
    __wsd_prng.s[15] = __wsd_prng.s[12];
    __wsd_prng.s[12] = 0;
    __wsd_prng.n = 0;
  }

  assert( N>0 );
  while( 1 /* exit by break */ ){
    if( N<=__wsd_prng.n ){
      memcpy(z_buf, &__wsd_prng.out[__wsd_prng.n-N], N);
      __wsd_prng.n -= N;
      break;
    }
    if( __wsd_prng.n>0 ){
      memcpy(z_buf, __wsd_prng.out, __wsd_prng.n);
      N -= __wsd_prng.n;
      z_buf += __wsd_prng.n;
    }
    __wsd_prng.s[12]++;
    chacha_block((u_int32_t*)__wsd_prng.out, __wsd_prng.s);
    __wsd_prng.n = 64;
  }
  dbsql_mutex_leave(mutex);
}

#ifndef DBSQL_UNTESTABLE
/*
** For testing purposes, we sometimes want to preserve the state of
** PRNG and restore the PRNG to its saved state at a later time, or
** to reset the PRNG to its initial state.  These routines accomplish
** those tasks.
**
** The dbsql_test_control() interface calls these routines to
** control the PRNG.
*/
static DBSQL_WSD struct __prng_type __saved_prng;
void __prng_save_state(void){
  memcpy(
    &GLOBAL(struct __prng_type, __saved_prng),
    &GLOBAL(struct __prng_type, __prng),
    sizeof(__prng)
  );
}
void __prng_restore_state(void){
  memcpy(
    &GLOBAL(struct __prng_type, __prng),
    &GLOBAL(struct __prng_type, __saved_prng),
    sizeof(__prng)
  );
}
#endif /* DBSQL_UNTESTABLE */
