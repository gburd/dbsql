/*
** 2007 October 14
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the C functions that implement a memory
** allocation subsystem for use by dbsql. 
**
** This version of the memory allocation subsystem omits all
** use of malloc(). The application gives dbsql a block of memory
** before calling dbsql_initialize() from which allocations
** are made and returned by the x_malloc() and x_realloc() 
** implementations. Once dbsql_initialize() has been called,
** the amount of memory available to dbsql is fixed and cannot
** be changed.
**
** This version of the memory allocation subsystem is included
** in the build only if DBSQL_ENABLE_MEMSYS5 is defined.
**
** This memory allocator uses the following algorithm:
**
**   1.  All memory allocation sizes are rounded up to a power of 2.
**
**   2.  If two adjacent free blocks are the halves of a larger block,
**       then the two blocks are coalesced into the single larger block.
**
**   3.  New memory is allocated from the first available free block.
**
** This algorithm is described in: J. M. Robson. "Bounds for Some Functions
** Concerning Dynamic Storage Allocation". Journal of the Association for
** Computing Machinery, Volume 21, Number 8, July 1974, pages 491-499.
** 
** Let n be the size of the largest allocation divided by the minimum
** allocation size (after rounding all sizes up to a power of 2.)  Let M
** be the maximum amount of memory ever outstanding at one time.  Let
** N be the total amount of memory available for allocation.  Robson
** proved that this memory allocator will never breakdown due to 
** fragmentation as long as the following constraint holds:
**
**      N >=  M*(1 + log2(n)/2) - n + 1
**
** The dbsql_status() logic tracks the maximum values of n and M so
** that an application can, at any time, verify this constraint.
*/
#include "dbsql_int.h"

/*
** This version of the memory allocator is used only when 
** DBSQL_ENABLE_MEMSYS5 is defined.
*/
#ifdef DBSQL_ENABLE_MEMSYS5

/*
** A minimum allocation is an instance of the following structure.
** Larger allocations are an array of these structures where the
** size of the array is a power of 2.
**
** The size of this object must be a power of two.  That fact is
** verified in memsys5Init().
*/
typedef struct mem5_link_t mem5_link_t;
struct mem5_link_t {
  int next;       /* index_t of next free chunk */
  int prev;       /* index_t of previous free chunk */
};

/*
** Maximum size of any allocation is ((1<<LOGMAX)*mem5.sz_atom). Since
** mem5.sz_atom is always at least 8 and 32-bit integers are used,
** it is not actually possible to reach this limit.
*/
#define LOGMAX 30

/*
** Masks used for mem5.ctrls[] elements.
*/
#define CTRL_LOGSIZE  0x1f    /* Log2 Size of this block */
#define CTRL_FREE     0x20    /* True if not checked out */

/*
** All of the static variables used by this module are collected
** into a single structure named "mem5".  This is to keep the
** static variables organized and to reduce namespace pollution
** when this module is combined with other in the amalgamation.
*/
static DBSQL_WSD struct mem5_global_t {
  /*
  ** Memory available for allocation
  */
  int sz_atom;      /* Smallest possible allocation in bytes */
  int n_block;      /* Number of sz_atom sized blocks in pool */
  u_int8_t *pool;       /* Memory available to be allocated */
  
  /*
  ** Mutex to control access to the memory allocation subsystem.
  */
  dbsql_mutex_t *mutex;

#if defined(DBSQL_DEBUG) || defined(DBSQL_TEST)
  /*
  ** Performance statistics
  */
  u_int64_t n_alloc;         /* Total number of calls to malloc */
  u_int64_t total_alloc;     /* Total of all malloc calls - includes internal frag */
  u_int64_t total_excess;    /* Total internal fragmentation */
  u_int32_t current_out;     /* Current checkout, including internal fragmentation */
  u_int32_t current_count;   /* Current number of distinct checkouts */
  u_int32_t max_out;         /* Maximum instantaneous current_out */
  u_int32_t max_count;       /* Maximum instantaneous current_count */
  u_int32_t max_request;     /* Largest allocation (exclusive of internal frag) */
#endif
  
  /*
  ** Lists of free blocks.  freelists[0] is a list of free blocks of
  ** size mem5.sz_atom.  freelists[1] holds blocks of size sz_atom*2.
  ** freelists[2] holds free blocks of size sz_atom*4.  And so forth.
  */
  int freelists[LOGMAX+1];

  /*
  ** Space for tracking which blocks are checked out and the size
  ** of each block.  One byte per block.
  */
  u_int8_t *ctrls;

} mem5;

/*
** Access the static variable through a macro for DBSQL_OMIT_WSD.
*/
#define mem5 GLOBAL(struct mem5_global_t, mem5)

/*
** Assuming mem5.pool is divided up into an array of mem5_link_t
** structures, return a pointer to the idx-th such link.
*/
#define MEM5LINK(idx) ((mem5_link_t *)(&mem5.pool[(idx)*mem5.sz_atom]))

/*
** Unlink the chunk at mem5.pools[i] from list it is currently
** on.  It should be found on mem5.freelists[i_logsize].
*/
static void memsys5Unlink(int i, int i_logsize){
  int next, prev;
  assert( i>=0 && i<mem5.n_block );
  assert( i_logsize>=0 && i_logsize<=LOGMAX );
  assert( (mem5.ctrls[i] & CTRL_LOGSIZE)==i_logsize );

  next = MEM5LINK(i)->next;
  prev = MEM5LINK(i)->prev;
  if( prev<0 ){
    mem5.freelists[i_logsize] = next;
  }else{
    MEM5LINK(prev)->next = next;
  }
  if( next>=0 ){
    MEM5LINK(next)->prev = prev;
  }
}

/*
** Link the chunk at mem5.pools[i] so that is on the i_logsize
** free list.
*/
static void memsys5Link(int i, int i_logsize){
  int x;
  assert( dbsql_mutex_held(mem5.mutex) );
  assert( i>=0 && i<mem5.n_block );
  assert( i_logsize>=0 && i_logsize<=LOGMAX );
  assert( (mem5.ctrls[i] & CTRL_LOGSIZE)==i_logsize );

  x = MEM5LINK(i)->next = mem5.freelists[i_logsize];
  MEM5LINK(i)->prev = -1;
  if( x>=0 ){
    assert( x<mem5.n_block );
    MEM5LINK(x)->prev = i;
  }
  mem5.freelists[i_logsize] = i;
}

/*
** Obtain or release the mutex needed to access global data structures.
*/
static void memsys5Enter(void){
  dbsql_mutex_enter(mem5.mutex);
}
static void memsys5Leave(void){
  dbsql_mutex_leave(mem5.mutex);
}

/*
** Return the size of an outstanding allocation, in bytes.
** This only works for chunks that are currently checked out.
*/
static int memsys5Size(void *p){
  int i_size, i;
  assert( p!=0 );
  i = (int)(((u_int8_t *)p-mem5.pool)/mem5.sz_atom);
  assert( i>=0 && i<mem5.n_block );
  i_size = mem5.sz_atom * (1 << (mem5.ctrls[i]&CTRL_LOGSIZE));
  return i_size;
}

/*
** Return a block of memory of at least n_bytes in size.
** Return NULL if unable.  Return NULL if n_bytes==0.
**
** The caller guarantees that n_byte is positive.
**
** The caller has obtained a mutex prior to invoking this
** routine so there is never any chance that two or more
** threads can be in this routine at the same time.
*/
static void *__memsys5_malloc_unsafe(int n_byte){
  int i;           /* index_t of a mem5.pools[] slot */
  int i_bin;        /* index_t into mem5.freelists[] */
  int i_full_sz;     /* Size of allocation rounded up to power of 2 */
  int i_logsize;    /* Log2 of i_full_sz/POW2_MIN */

  /* n_byte must be a positive */
  assert( n_byte>0 );

  /* No more than 1GiB per allocation */
  if( n_byte > 0x40000000 ) return 0;

#if defined(DBSQL_DEBUG) || defined(DBSQL_TEST)
  /* Keep track of the maximum allocation request.  Even unfulfilled
  ** requests are counted */
  if( (u_int32_t)n_byte>mem5.max_request ){
    mem5.max_request = n_byte;
  }
#endif


  /* Round n_byte up to the next valid power of two */
  for(i_full_sz=mem5.sz_atom,i_logsize=0; i_full_sz<n_byte; i_full_sz*=2,i_logsize++){}

  /* Make sure mem5.freelists[i_logsize] contains at least one free
  ** block.  If not, then split a block of the next larger power of
  ** two in order to create a new free block of size i_logsize.
  */
  for(i_bin=i_logsize; i_bin<=LOGMAX && mem5.freelists[i_bin]<0; i_bin++){}
  if( i_bin>LOGMAX ){
    testcase( __global_config.x_log!=0 );
    dbsql_log(DBSQL_NOMEM, "failed to allocate %u bytes", n_byte);
    return 0;
  }
  i = mem5.freelists[i_bin];
  memsys5Unlink(i, i_bin);
  while( i_bin>i_logsize ){
    int newSize;

    i_bin--;
    newSize = 1 << i_bin;
    mem5.ctrls[i+newSize] = CTRL_FREE | i_bin;
    memsys5Link(i+newSize, i_bin);
  }
  mem5.ctrls[i] = i_logsize;

#if defined(DBSQL_DEBUG) || defined(DBSQL_TEST)
  /* Update allocator performance statistics. */
  mem5.n_alloc++;
  mem5.total_alloc += i_full_sz;
  mem5.total_excess += i_full_sz - n_byte;
  mem5.current_count++;
  mem5.current_out += i_full_sz;
  if( mem5.max_count<mem5.current_count ) mem5.max_count = mem5.current_count;
  if( mem5.max_out<mem5.current_out ) mem5.max_out = mem5.current_out;
#endif

#ifdef DBSQL_DEBUG
  /* Make sure the allocated memory does not assume that it is set to zero
  ** or retains a value from a previous allocation */
  memset(&mem5.pool[i*mem5.sz_atom], 0xAA, i_full_sz);
#endif

  /* Return a pointer to the allocated memory. */
  return (void*)&mem5.pool[i*mem5.sz_atom];
}

/*
** Free an outstanding memory allocation.
*/
static void __memsys5_free_unsafe(void *old){
  u_int32_t size, i_logsize;
  int i_block;

  /* Set i_block to the index of the block pointed to by old in 
  ** the array of mem5.sz_atom byte blocks pointed to by mem5.pool.
  */
  i_block = (int)(((u_int8_t *)old-mem5.pool)/mem5.sz_atom);

  /* Check that the pointer old points to a valid, non-free block. */
  assert( i_block>=0 && i_block<mem5.n_block );
  assert( ((u_int8_t *)old-mem5.pool)%mem5.sz_atom==0 );
  assert( (mem5.ctrls[i_block] & CTRL_FREE)==0 );

  i_logsize = mem5.ctrls[i_block] & CTRL_LOGSIZE;
  size = 1<<i_logsize;
  assert( i_block+size-1<(u_int32_t)mem5.n_block );

  mem5.ctrls[i_block] |= CTRL_FREE;
  mem5.ctrls[i_block+size-1] |= CTRL_FREE;

#if defined(DBSQL_DEBUG) || defined(DBSQL_TEST)
  assert( mem5.current_count>0 );
  assert( mem5.current_out>=(size*mem5.sz_atom) );
  mem5.current_count--;
  mem5.current_out -= size*mem5.sz_atom;
  assert( mem5.current_out>0 || mem5.current_count==0 );
  assert( mem5.current_count>0 || mem5.current_out==0 );
#endif

  mem5.ctrls[i_block] = CTRL_FREE | i_logsize;
  while( ALWAYS(i_logsize<LOGMAX) ){
    int i_buddy;
    if( (i_block>>i_logsize) & 1 ){
      i_buddy = i_block - size;
      assert( i_buddy>=0 );
    }else{
      i_buddy = i_block + size;
      if( i_buddy>=mem5.n_block ) break;
    }
    if( mem5.ctrls[i_buddy]!=(CTRL_FREE | i_logsize) ) break;
    memsys5Unlink(i_buddy, i_logsize);
    i_logsize++;
    if( i_buddy<i_block ){
      mem5.ctrls[i_buddy] = CTRL_FREE | i_logsize;
      mem5.ctrls[i_block] = 0;
      i_block = i_buddy;
    }else{
      mem5.ctrls[i_block] = CTRL_FREE | i_logsize;
      mem5.ctrls[i_buddy] = 0;
    }
    size *= 2;
  }

#ifdef DBSQL_DEBUG
  /* Overwrite freed memory with the 0x55 bit pattern to verify that it is
  ** not used after being freed */
  memset(&mem5.pool[i_block*mem5.sz_atom], 0x55, size);
#endif

  memsys5Link(i_block, i_logsize);
}

/*
** Allocate n_bytes of memory.
*/
static void *memsys5Malloc(int n_bytes){
  int64_t *p = 0;
  if( n_bytes>0 ){
    memsys5Enter();
    p = __memsys5_malloc_unsafe(n_bytes);
    memsys5Leave();
  }
  return (void*)p; 
}

/*
** Free memory.
**
** The outer layer memory allocator prevents this routine from
** being called with prior==0.
*/
static void memsys5Free(void *prior){
  assert( prior!=0 );
  memsys5Enter();
  __memsys5_free_unsafe(prior);
  memsys5Leave();  
}

/*
** Change the size of an existing memory allocation.
**
** The outer layer memory allocator prevents this routine from
** being called with prior==0.  
**
** n_bytes is always a value obtained from a prior call to
** memsys5Round().  Hence n_bytes is always a non-negative power
** of two.  If n_bytes==0 that means that an oversize allocation
** (an allocation larger than 0x40000000) was requested and this
** routine should return 0 without freeing prior.
*/
static void *memsys5Realloc(void *prior, int n_bytes){
  int n_old;
  void *p;
  assert( prior!=0 );
  assert( (n_bytes&(n_bytes-1))==0 );  /* EV: R-46199-30249 */
  assert( n_bytes>=0 );
  if( n_bytes==0 ){
    return 0;
  }
  n_old = memsys5Size(prior);
  if( n_bytes<=n_old ){
    return prior;
  }
  p = memsys5Malloc(n_bytes);
  if( p ){
    memcpy(p, prior, n_old);
    memsys5Free(prior);
  }
  return p;
}

/*
** Round up a request size to the next valid allocation size.  If
** the allocation is too large to be handled by this allocation system,
** return 0.
**
** All allocations must be a power of two and must be expressed by a
** 32-bit signed integer.  Hence the largest allocation is 0x40000000
** or 1073741824 bytes.
*/
static int memsys5Roundup(int n){
  int i_full_sz;
  if( n<=mem5.sz_atom*2 ){
    if( n<=mem5.sz_atom ) return mem5.sz_atom;
    return mem5.sz_atom*2;
  }
  if( n>0x10000000 ){
    if( n>0x40000000 ) return 0;
    if( n>0x20000000 ) return 0x40000000;
    return 0x20000000;
  }
  for(i_full_sz=mem5.sz_atom*8; i_full_sz<n; i_full_sz *= 4);
  if( (i_full_sz/2)>=(int64_t)n ) return i_full_sz/2;
  return i_full_sz;
}

/*
** Return the ceiling of the logarithm base 2 of i_value.
**
** Examples:   memsys5Log(1) -> 0
**             memsys5Log(2) -> 1
**             memsys5Log(4) -> 2
**             memsys5Log(5) -> 3
**             memsys5Log(8) -> 3
**             memsys5Log(9) -> 4
*/
static int memsys5Log(int i_value){
  int i_log;
  for(i_log=0; (i_log<(int)((sizeof(int)*8)-1)) && (1<<i_log)<i_value; i_log++);
  return i_log;
}

/*
** Initialize the memory allocator.
**
** This routine is not threadsafe.  The caller must be holding a mutex
** to prevent multiple threads from entering at the same time.
*/
static int memsys5Init(void *__not_used){
  int ii;            /* Loop counter */
  int n_byte;         /* Number of bytes of memory available to this allocator */
  u_int8_t *byte;         /* Memory usable by this allocator */
  int n_min_log;       /* Log base 2 of minimum allocation size in bytes */
  int i_offset;       /* An offset into mem5.ctrls[] */

  UNUSED_PARAMETER(__not_used);

  /* For the purposes of this routine, disable the mutex */
  mem5.mutex = 0;

  /* The size of a mem5_link_t object must be a power of two.  Verify that
  ** this is case.
  */
  assert( (sizeof(mem5_link_t)&(sizeof(mem5_link_t)-1))==0 );

  n_byte = __global_config.n_heap;
  byte = (u_int8_t*)__global_config.heap;
  assert( byte!=0 );  /* dbsql_config() does not allow otherwise */

  /* boundaries on __global_config.mn_req are enforced in dbsql_config() */
  n_min_log = memsys5Log(__global_config.mn_req);
  mem5.sz_atom = (1<<n_min_log);
  while( (int)sizeof(mem5_link_t)>mem5.sz_atom ){
    mem5.sz_atom = mem5.sz_atom << 1;
  }

  mem5.n_block = (n_byte / (mem5.sz_atom+sizeof(u_int8_t)));
  mem5.pool = byte;
  mem5.ctrls = (u_int8_t *)&mem5.pool[mem5.n_block*mem5.sz_atom];

  for(ii=0; ii<=LOGMAX; ii++){
    mem5.freelists[ii] = -1;
  }

  i_offset = 0;
  for(ii=LOGMAX; ii>=0; ii--){
    int n_alloc = (1<<ii);
    if( (i_offset+n_alloc)<=mem5.n_block ){
      mem5.ctrls[i_offset] = ii | CTRL_FREE;
      memsys5Link(i_offset, ii);
      i_offset += n_alloc;
    }
    assert((i_offset+n_alloc)>mem5.n_block);
  }

  /* If a mutex is required for normal operation, allocate one */
  if( __global_config.memstat==0 ){
    mem5.mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MEM);
  }

  return DBSQL_OK;
}

/*
** Deinitialize this module.
*/
static void memsys5Shutdown(void *__not_used){
  UNUSED_PARAMETER(__not_used);
  mem5.mutex = 0;
  return;
}

#ifdef DBSQL_TEST
/*
** Open the file indicated and write a log of all unfreed memory 
** allocations into that log.
*/
void __memsys5_dump(const char *filename){
  FILE *out;
  int i, j, n;
  int n_min_log;

  if( filename==0 || filename[0]==0 ){
    out = stdout;
  }else{
    out = fopen(filename, "w");
    if( out==0 ){
      fprintf(stderr, "** Unable to output memory debug output log: %s **\n",
                      filename);
      return;
    }
  }
  memsys5Enter();
  n_min_log = memsys5Log(mem5.sz_atom);
  for(i=0; i<=LOGMAX && i+n_min_log<32; i++){
    for(n=0, j=mem5.freelists[i]; j>=0; j = MEM5LINK(j)->next, n++){}
    fprintf(out, "freelist items of size %d: %d\n", mem5.sz_atom << i, n);
  }
  fprintf(out, "mem5.nAlloc       = %llu\n", mem5.n_alloc);
  fprintf(out, "mem5.totalAlloc   = %llu\n", mem5.total_alloc);
  fprintf(out, "mem5.totalExcess  = %llu\n", mem5.total_excess);
  fprintf(out, "mem5.currentOut   = %u\n", mem5.current_out);
  fprintf(out, "mem5.currentCount = %u\n", mem5.current_count);
  fprintf(out, "mem5.maxOut       = %u\n", mem5.max_out);
  fprintf(out, "mem5.maxCount     = %u\n", mem5.max_count);
  fprintf(out, "mem5.maxRequest   = %u\n", mem5.max_request);
  memsys5Leave();
  if( out==stdout ){
    fflush(stdout);
  }else{
    fclose(out);
  }
}
#endif

/*
** This routine is the only routine in this file with external 
** linkage. It returns a pointer to a static dbsql_mem_methods_t
** struct populated with the memsys5 methods.
*/
const dbsql_mem_methods_t *__mem_get_memsys5(void){
  static const dbsql_mem_methods_t memsys5Methods = {
     memsys5Malloc,
     memsys5Free,
     memsys5Realloc,
     memsys5Size,
     memsys5Roundup,
     memsys5Init,
     memsys5Shutdown,
     0
  };
  return &memsys5Methods;
}

#endif /* DBSQL_ENABLE_MEMSYS5 */
