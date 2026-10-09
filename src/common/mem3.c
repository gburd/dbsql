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
** use of malloc(). The dbsql user supplies a block of memory
** before calling dbsql_initialize() from which allocations
** are made and returned by the x_malloc() and x_realloc() 
** implementations. Once dbsql_initialize() has been called,
** the amount of memory available to dbsql is fixed and cannot
** be changed.
**
** This version of the memory allocation subsystem is included
** in the build only if DBSQL_ENABLE_MEMSYS3 is defined.
*/
#include "dbsql_int.h"

/*
** This version of the memory allocator is only built into the library
** DBSQL_ENABLE_MEMSYS3 is defined. Defining this symbol does not
** mean that the library will use a memory-pool by default, just that
** it is available. The mempool allocator is activated by calling
** dbsql_config().
*/
#ifdef DBSQL_ENABLE_MEMSYS3

/*
** Maximum size (in Mem3Blocks) of a "small" chunk.
*/
#define MX_SMALL 10


/*
** Number of freelist hash slots
*/
#define N_HASH  61

/*
** A memory allocation (also called a "chunk") consists of two or 
** more blocks where each block is 8 bytes.  The first 8 bytes are 
** a header that is not returned to the user.
**
** A chunk is two or more blocks that is either checked out or
** free.  The first block has format u.hdr.  u.hdr.size4x is 4 times the
** size of the allocation in blocks if the allocation is free.
** The u.hdr.size4x&1 bit is true if the chunk is checked out and
** false if the chunk is on the freelist.  The u.hdr.size4x&2 bit
** is true if the previous chunk is checked out and false if the
** previous chunk is free.  The u.hdr.prev_size field is the size of
** the previous chunk in blocks if the previous chunk is on the
** freelist. If the previous chunk is checked out, then
** u.hdr.prev_size can be part of the data for that chunk and should
** not be read or written.
**
** We often identify a chunk by its index in mem3.pools[].  When
** this is done, the chunk index refers to the second block of
** the chunk.  In this way, the first chunk has an index of 1.
** A chunk index of 0 means "no such chunk" and is the equivalent
** of a NULL pointer.
**
** The second block of free chunks is of the form u.list.  The
** two fields form a double-linked list of chunks of related sizes.
** Pointers to the head of the list are stored in mem3.smalls[] 
** for smaller chunks and mem3.hashes[] for larger chunks.
**
** The second block of a chunk is user data if the chunk is checked 
** out.  If a chunk is checked out, the user data may extend into
** the u.hdr.prev_size value of the following chunk.
*/
typedef struct mem3_block_t mem3_block_t;
struct mem3_block_t {
  union {
    struct {
      u_int32_t prev_size;   /* Size of previous chunk in mem3_block_t elements */
      u_int32_t size4x;     /* 4x the size of current chunk in mem3_block_t elements */
    } hdr;
    struct {
      u_int32_t next;       /* index_t in mem3.pools[] of next free chunk */
      u_int32_t prev;       /* index_t in mem3.pools[] of previous free chunk */
    } list;
  } u;
};

/*
** All of the static variables used by this module are collected
** into a single structure named "mem3".  This is to keep the
** static variables organized and to reduce namespace pollution
** when this module is combined with other in the amalgamation.
*/
static DBSQL_WSD struct mem3_global_t {
  /*
  ** Memory available for allocation. n_pool is the size of the array
  ** (in Mem3Blocks) pointed to by pools less 2.
  */
  u_int32_t n_pool;
  mem3_block_t *pools;

  /*
  ** True if we are evaluating an out-of-memory callback.
  */
  int alarm_busy;
  
  /*
  ** Mutex to control access to the memory allocation subsystem.
  */
  dbsql_mutex_t *mutex;
  
  /*
  ** The minimum amount of free space that we have seen.
  */
  u_int32_t mn_key_blk;

  /*
  ** i_key_blk is the index of the key chunk.  Most new allocations
  ** occur off of this chunk.  sz_key_blk is the size (in Mem3Blocks)
  ** of the current key chunk.  i_key_blk is 0 if there is no key chunk.
  ** The key chunk is not in either the hashes[] or smalls[].
  */
  u_int32_t i_key_blk;
  u_int32_t sz_key_blk;

  /*
  ** Array of lists of free blocks according to the block size 
  ** for smaller chunks, or a hash on the block size for larger
  ** chunks.
  */
  u_int32_t smalls[MX_SMALL-1];   /* For sizes 2 through MX_SMALL, inclusive */
  u_int32_t hashes[N_HASH];        /* For sizes MX_SMALL+1 and larger */
} mem3 = { 97535575 };

#define mem3 GLOBAL(struct mem3_global_t, mem3)

/*
** Unlink the chunk at mem3.pools[i] from list it is currently
** on.  *root is the list that i is a member of.
*/
static void __memsys3_unlink_from_list(u_int32_t i, u_int32_t *root){
  u_int32_t next = mem3.pools[i].u.list.next;
  u_int32_t prev = mem3.pools[i].u.list.prev;
  assert( dbsql_mutex_held(mem3.mutex) );
  if( prev==0 ){
    *root = next;
  }else{
    mem3.pools[prev].u.list.next = next;
  }
  if( next ){
    mem3.pools[next].u.list.prev = prev;
  }
  mem3.pools[i].u.list.next = 0;
  mem3.pools[i].u.list.prev = 0;
}

/*
** Unlink the chunk at index i from 
** whatever list is currently a member of.
*/
static void memsys3Unlink(u_int32_t i){
  u_int32_t size, hash;
  assert( dbsql_mutex_held(mem3.mutex) );
  assert( (mem3.pools[i-1].u.hdr.size4x & 1)==0 );
  assert( i>=1 );
  size = mem3.pools[i-1].u.hdr.size4x/4;
  assert( size==mem3.pools[i+size-1].u.hdr.prev_size );
  assert( size>=2 );
  if( size <= MX_SMALL ){
    __memsys3_unlink_from_list(i, &mem3.smalls[size-2]);
  }else{
    hash = size % N_HASH;
    __memsys3_unlink_from_list(i, &mem3.hashes[hash]);
  }
}

/*
** Link the chunk at mem3.pools[i] so that is on the list rooted
** at *root.
*/
static void __memsys3_link_into_list(u_int32_t i, u_int32_t *root){
  assert( dbsql_mutex_held(mem3.mutex) );
  mem3.pools[i].u.list.next = *root;
  mem3.pools[i].u.list.prev = 0;
  if( *root ){
    mem3.pools[*root].u.list.prev = i;
  }
  *root = i;
}

/*
** Link the chunk at index i into either the appropriate
** small chunk list, or into the large chunk hash table.
*/
static void memsys3Link(u_int32_t i){
  u_int32_t size, hash;
  assert( dbsql_mutex_held(mem3.mutex) );
  assert( i>=1 );
  assert( (mem3.pools[i-1].u.hdr.size4x & 1)==0 );
  size = mem3.pools[i-1].u.hdr.size4x/4;
  assert( size==mem3.pools[i+size-1].u.hdr.prev_size );
  assert( size>=2 );
  if( size <= MX_SMALL ){
    __memsys3_link_into_list(i, &mem3.smalls[size-2]);
  }else{
    hash = size % N_HASH;
    __memsys3_link_into_list(i, &mem3.hashes[hash]);
  }
}

/*
** If the STATIC_MEM mutex is not already held, obtain it now. The mutex
** will already be held (obtained by code in malloc.c) if
** __global_config.mem_stat is true.
*/
static void memsys3Enter(void){
  if( __global_config.memstat==0 && mem3.mutex==0 ){
    mem3.mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MEM);
  }
  dbsql_mutex_enter(mem3.mutex);
}
static void memsys3Leave(void){
  dbsql_mutex_leave(mem3.mutex);
}

/*
** Called when we are unable to satisfy an allocation of n_bytes.
*/
static void __memsys3_out_of_memory(int n_byte){
  if( !mem3.alarm_busy ){
    mem3.alarm_busy = 1;
    assert( dbsql_mutex_held(mem3.mutex) );
    dbsql_mutex_leave(mem3.mutex);
    dbsql_release_memory(n_byte);
    dbsql_mutex_enter(mem3.mutex);
    mem3.alarm_busy = 0;
  }
}


/*
** Chunk i is a free chunk that has been unlinked.  Adjust its 
** size parameters for check-out and return a pointer to the 
** user portion of the chunk.
*/
static void *memsys3Checkout(u_int32_t i, u_int32_t n_block){
  u_int32_t x;
  assert( dbsql_mutex_held(mem3.mutex) );
  assert( i>=1 );
  assert( mem3.pools[i-1].u.hdr.size4x/4==n_block );
  assert( mem3.pools[i+n_block-1].u.hdr.prev_size==n_block );
  x = mem3.pools[i-1].u.hdr.size4x;
  mem3.pools[i-1].u.hdr.size4x = n_block*4 | 1 | (x&2);
  mem3.pools[i+n_block-1].u.hdr.prev_size = n_block;
  mem3.pools[i+n_block-1].u.hdr.size4x |= 2;
  return &mem3.pools[i];
}

/*
** Carve a piece off of the end of the mem3.i_key_blk free chunk.
** Return a pointer to the new allocation.  Or, if the key chunk
** is not large enough, return 0.
*/
static void *__memsys3_from_key_blk(u_int32_t n_block){
  assert( dbsql_mutex_held(mem3.mutex) );
  assert( mem3.sz_key_blk>=n_block );
  if( n_block>=mem3.sz_key_blk-1 ){
    /* Use the entire key chunk */
    void *p = memsys3Checkout(mem3.i_key_blk, mem3.sz_key_blk);
    mem3.i_key_blk = 0;
    mem3.sz_key_blk = 0;
    mem3.mn_key_blk = 0;
    return p;
  }else{
    /* Split the key block.  Return the tail. */
    u_int32_t newi, x;
    newi = mem3.i_key_blk + mem3.sz_key_blk - n_block;
    assert( newi > mem3.i_key_blk+1 );
    mem3.pools[mem3.i_key_blk+mem3.sz_key_blk-1].u.hdr.prev_size = n_block;
    mem3.pools[mem3.i_key_blk+mem3.sz_key_blk-1].u.hdr.size4x |= 2;
    mem3.pools[newi-1].u.hdr.size4x = n_block*4 + 1;
    mem3.sz_key_blk -= n_block;
    mem3.pools[newi-1].u.hdr.prev_size = mem3.sz_key_blk;
    x = mem3.pools[mem3.i_key_blk-1].u.hdr.size4x & 2;
    mem3.pools[mem3.i_key_blk-1].u.hdr.size4x = mem3.sz_key_blk*4 | x;
    if( mem3.sz_key_blk < mem3.mn_key_blk ){
      mem3.mn_key_blk = mem3.sz_key_blk;
    }
    return (void*)&mem3.pools[newi];
  }
}

/*
** *root is the head of a list of free chunks of the same size
** or same size hash.  In other words, *root is an entry in either
** mem3.smalls[] or mem3.hashes[].  
**
** This routine examines all entries on the given list and tries
** to coalesce each entries with adjacent free chunks.  
**
** If it sees a chunk that is larger than mem3.i_key_blk, it replaces 
** the current mem3.i_key_blk with the new larger chunk.  In order for
** this mem3.i_key_blk replacement to work, the key chunk must be
** linked into the hash tables.  That is not the normal state of
** affairs, of course.  The calling routine must link the key
** chunk before invoking this routine, then must unlink the (possibly
** changed) key chunk once this routine has finished.
*/
static void memsys3Merge(u_int32_t *root){
  u_int32_t i_next, prev, size, i, x;

  assert( dbsql_mutex_held(mem3.mutex) );
  for(i=*root; i>0; i=i_next){
    i_next = mem3.pools[i].u.list.next;
    size = mem3.pools[i-1].u.hdr.size4x;
    assert( (size&1)==0 );
    if( (size&2)==0 ){
      __memsys3_unlink_from_list(i, root);
      assert( i > mem3.pools[i-1].u.hdr.prev_size );
      prev = i - mem3.pools[i-1].u.hdr.prev_size;
      if( prev==i_next ){
        i_next = mem3.pools[prev].u.list.next;
      }
      memsys3Unlink(prev);
      size = i + size/4 - prev;
      x = mem3.pools[prev-1].u.hdr.size4x & 2;
      mem3.pools[prev-1].u.hdr.size4x = size*4 | x;
      mem3.pools[prev+size-1].u.hdr.prev_size = size;
      memsys3Link(prev);
      i = prev;
    }else{
      size /= 4;
    }
    if( size>mem3.sz_key_blk ){
      mem3.i_key_blk = i;
      mem3.sz_key_blk = size;
    }
  }
}

/*
** Return a block of memory of at least n_bytes in size.
** Return NULL if unable.
**
** This function assumes that the necessary mutexes, if any, are
** already held by the caller. Hence "Unsafe".
*/
static void *__memsys3_malloc_unsafe(int n_byte){
  u_int32_t i;
  u_int32_t n_block;
  u_int32_t to_free;

  assert( dbsql_mutex_held(mem3.mutex) );
  assert( sizeof(mem3_block_t)==8 );
  if( n_byte<=12 ){
    n_block = 2;
  }else{
    n_block = (n_byte + 11)/8;
  }
  assert( n_block>=2 );

  /* STEP 1:
  ** Look for an entry of the correct size in either the small
  ** chunk table or in the large chunk hash table.  This is
  ** successful most of the time (about 9 times out of 10).
  */
  if( n_block <= MX_SMALL ){
    i = mem3.smalls[n_block-2];
    if( i>0 ){
      __memsys3_unlink_from_list(i, &mem3.smalls[n_block-2]);
      return memsys3Checkout(i, n_block);
    }
  }else{
    int hash = n_block % N_HASH;
    for(i=mem3.hashes[hash]; i>0; i=mem3.pools[i].u.list.next){
      if( mem3.pools[i-1].u.hdr.size4x/4==n_block ){
        __memsys3_unlink_from_list(i, &mem3.hashes[hash]);
        return memsys3Checkout(i, n_block);
      }
    }
  }

  /* STEP 2:
  ** Try to satisfy the allocation by carving a piece off of the end
  ** of the key chunk.  This step usually works if step 1 fails.
  */
  if( mem3.sz_key_blk>=n_block ){
    return __memsys3_from_key_blk(n_block);
  }


  /* STEP 3:  
  ** Loop through the entire memory pool.  Coalesce adjacent free
  ** chunks.  Recompute the key chunk as the largest free chunk.
  ** Then try again to satisfy the allocation by carving a piece off
  ** of the end of the key chunk.  This step happens very
  ** rarely (we hope!)
  */
  for(to_free=n_block*16; to_free<(mem3.n_pool*16); to_free *= 2){
    __memsys3_out_of_memory(to_free);
    if( mem3.i_key_blk ){
      memsys3Link(mem3.i_key_blk);
      mem3.i_key_blk = 0;
      mem3.sz_key_blk = 0;
    }
    for(i=0; i<N_HASH; i++){
      memsys3Merge(&mem3.hashes[i]);
    }
    for(i=0; i<MX_SMALL-1; i++){
      memsys3Merge(&mem3.smalls[i]);
    }
    if( mem3.sz_key_blk ){
      memsys3Unlink(mem3.i_key_blk);
      if( mem3.sz_key_blk>=n_block ){
        return __memsys3_from_key_blk(n_block);
      }
    }
  }

  /* If none of the above worked, then we fail. */
  return 0;
}

/*
** Free an outstanding memory allocation.
**
** This function assumes that the necessary mutexes, if any, are
** already held by the caller. Hence "Unsafe".
*/
static void __memsys3_free_unsafe(void *old){
  mem3_block_t *p = (mem3_block_t*)old;
  int i;
  u_int32_t size, x;
  assert( dbsql_mutex_held(mem3.mutex) );
  assert( p>mem3.pools && p<&mem3.pools[mem3.n_pool] );
  i = p - mem3.pools;
  assert( (mem3.pools[i-1].u.hdr.size4x&1)==1 );
  size = mem3.pools[i-1].u.hdr.size4x/4;
  assert( i+size<=mem3.n_pool+1 );
  mem3.pools[i-1].u.hdr.size4x &= ~1;
  mem3.pools[i+size-1].u.hdr.prev_size = size;
  mem3.pools[i+size-1].u.hdr.size4x &= ~2;
  memsys3Link(i);

  /* Try to expand the key using the newly freed chunk */
  if( mem3.i_key_blk ){
    while( (mem3.pools[mem3.i_key_blk-1].u.hdr.size4x&2)==0 ){
      size = mem3.pools[mem3.i_key_blk-1].u.hdr.prev_size;
      mem3.i_key_blk -= size;
      mem3.sz_key_blk += size;
      memsys3Unlink(mem3.i_key_blk);
      x = mem3.pools[mem3.i_key_blk-1].u.hdr.size4x & 2;
      mem3.pools[mem3.i_key_blk-1].u.hdr.size4x = mem3.sz_key_blk*4 | x;
      mem3.pools[mem3.i_key_blk+mem3.sz_key_blk-1].u.hdr.prev_size = mem3.sz_key_blk;
    }
    x = mem3.pools[mem3.i_key_blk-1].u.hdr.size4x & 2;
    while( (mem3.pools[mem3.i_key_blk+mem3.sz_key_blk-1].u.hdr.size4x&1)==0 ){
      memsys3Unlink(mem3.i_key_blk+mem3.sz_key_blk);
      mem3.sz_key_blk += mem3.pools[mem3.i_key_blk+mem3.sz_key_blk-1].u.hdr.size4x/4;
      mem3.pools[mem3.i_key_blk-1].u.hdr.size4x = mem3.sz_key_blk*4 | x;
      mem3.pools[mem3.i_key_blk+mem3.sz_key_blk-1].u.hdr.prev_size = mem3.sz_key_blk;
    }
  }
}

/*
** Return the size of an outstanding allocation, in bytes.  The
** size returned omits the 8-byte header overhead.  This only
** works for chunks that are currently checked out.
*/
static int memsys3Size(void *p){
  mem3_block_t *block;
  assert( p!=0 );
  block = (mem3_block_t*)p;
  assert( (block[-1].u.hdr.size4x&1)!=0 );
  return (block[-1].u.hdr.size4x&~3)*2 - 4;
}

/*
** Round up a request size to the next valid allocation size.
*/
static int memsys3Roundup(int n){
  if( n<=12 ){
    return 12;
  }else{
    return ((n+11)&~7) - 4;
  }
}

/*
** Allocate n_bytes of memory.
*/
static void *memsys3Malloc(int n_bytes){
  int64_t *p;
  assert( n_bytes>0 );          /* malloc.c filters out 0 byte requests */
  memsys3Enter();
  p = __memsys3_malloc_unsafe(n_bytes);
  memsys3Leave();
  return (void*)p; 
}

/*
** Free memory.
*/
static void memsys3Free(void *prior){
  assert( prior );
  memsys3Enter();
  __memsys3_free_unsafe(prior);
  memsys3Leave();
}

/*
** Change the size of an existing memory allocation
*/
static void *memsys3Realloc(void *prior, int n_bytes){
  int n_old;
  void *p;
  if( prior==0 ){
    return dbsql_malloc(n_bytes);
  }
  if( n_bytes<=0 ){
    dbsql_free(prior);
    return 0;
  }
  n_old = memsys3Size(prior);
  if( n_bytes<=n_old && n_bytes>=n_old-128 ){
    return prior;
  }
  memsys3Enter();
  p = __memsys3_malloc_unsafe(n_bytes);
  if( p ){
    if( n_old<n_bytes ){
      memcpy(p, prior, n_old);
    }else{
      memcpy(p, prior, n_bytes);
    }
    __memsys3_free_unsafe(prior);
  }
  memsys3Leave();
  return p;
}

/*
** Initialize this module.
*/
static int memsys3Init(void *__not_used){
  UNUSED_PARAMETER(__not_used);
  if( !__global_config.heap ){
    return DBSQL_ERROR;
  }

  /* Store a pointer to the memory block in global structure mem3. */
  assert( sizeof(mem3_block_t)==8 );
  mem3.pools = (mem3_block_t *)__global_config.heap;
  mem3.n_pool = (__global_config.n_heap / sizeof(mem3_block_t)) - 2;

  /* Initialize the key block. */
  mem3.sz_key_blk = mem3.n_pool;
  mem3.mn_key_blk = mem3.sz_key_blk;
  mem3.i_key_blk = 1;
  mem3.pools[0].u.hdr.size4x = (mem3.sz_key_blk<<2) + 2;
  mem3.pools[mem3.n_pool].u.hdr.prev_size = mem3.n_pool;
  mem3.pools[mem3.n_pool].u.hdr.size4x = 1;

  return DBSQL_OK;
}

/*
** Deinitialize this module.
*/
static void memsys3Shutdown(void *__not_used){
  UNUSED_PARAMETER(__not_used);
  mem3.mutex = 0;
  return;
}



/*
** Open the file indicated and write a log of all unfreed memory 
** allocations into that log.
*/
void __memsys3_dump(const char *filename){
#ifdef DBSQL_DEBUG
  FILE *out;
  u_int32_t i, j;
  u_int32_t size;
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
  memsys3Enter();
  fprintf(out, "CHUNKS:\n");
  for(i=1; i<=mem3.n_pool; i+=size/4){
    size = mem3.pools[i-1].u.hdr.size4x;
    if( size/4<=1 ){
      fprintf(out, "%p size error\n", &mem3.pools[i]);
      assert( 0 );
      break;
    }
    if( (size&1)==0 && mem3.pools[i+size/4-1].u.hdr.prev_size!=size/4 ){
      fprintf(out, "%p tail size does not match\n", &mem3.pools[i]);
      assert( 0 );
      break;
    }
    if( ((mem3.pools[i+size/4-1].u.hdr.size4x&2)>>1)!=(size&1) ){
      fprintf(out, "%p tail checkout bit is incorrect\n", &mem3.pools[i]);
      assert( 0 );
      break;
    }
    if( size&1 ){
      fprintf(out, "%p %6d bytes checked out\n", &mem3.pools[i], (size/4)*8-8);
    }else{
      fprintf(out, "%p %6d bytes free%s\n", &mem3.pools[i], (size/4)*8-8,
                  i==mem3.i_key_blk ? " **key**" : "");
    }
  }
  for(i=0; i<MX_SMALL-1; i++){
    if( mem3.smalls[i]==0 ) continue;
    fprintf(out, "small(%2d):", i);
    for(j = mem3.smalls[i]; j>0; j=mem3.pools[j].u.list.next){
      fprintf(out, " %p(%d)", &mem3.pools[j],
              (mem3.pools[j-1].u.hdr.size4x/4)*8-8);
    }
    fprintf(out, "\n"); 
  }
  for(i=0; i<N_HASH; i++){
    if( mem3.hashes[i]==0 ) continue;
    fprintf(out, "hash(%2d):", i);
    for(j = mem3.hashes[i]; j>0; j=mem3.pools[j].u.list.next){
      fprintf(out, " %p(%d)", &mem3.pools[j],
              (mem3.pools[j-1].u.hdr.size4x/4)*8-8);
    }
    fprintf(out, "\n"); 
  }
  fprintf(out, "key=%d\n", mem3.i_key_blk);
  fprintf(out, "nowUsed=%d\n", mem3.n_pool*8 - mem3.sz_key_blk*8);
  fprintf(out, "mxUsed=%d\n", mem3.n_pool*8 - mem3.mn_key_blk*8);
  dbsql_mutex_leave(mem3.mutex);
  if( out==stdout ){
    fflush(stdout);
  }else{
    fclose(out);
  }
#else
  UNUSED_PARAMETER(filename);
#endif
}

/*
** This routine is the only routine in this file with external 
** linkage.
**
** Populate the low-level memory allocation function pointers in
** __global_config.m with pointers to the routines in this file. The
** arguments specify the block of memory to manage.
**
** This routine is only called by dbsql_config(), and therefore
** is not required to be threadsafe (it is not).
*/
const dbsql_mem_methods_t *__mem_get_memsys3(void){
  static const dbsql_mem_methods_t mempool_methods = {
     memsys3Malloc,
     memsys3Free,
     memsys3Realloc,
     memsys3Size,
     memsys3Roundup,
     memsys3Init,
     memsys3Shutdown,
     0
  };
  return &mempool_methods;
}

#endif /* DBSQL_ENABLE_MEMSYS3 */
