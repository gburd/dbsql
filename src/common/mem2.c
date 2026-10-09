/*
** 2007 August 15
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
** to obtain the memory it needs while adding lots of additional debugging
** information to each allocation in order to help detect and fix memory
** leaks and memory usage errors.
**
** This file contains implementations of the low-level memory allocation
** routines specified in the dbsql_mem_methods_t object.
*/
#include "dbsql_int.h"

/*
** This version of the memory allocator is used only if the
** DBSQL_MEMDEBUG macro is defined
*/
#ifdef DBSQL_MEMDEBUG

/*
** The backtrace functionality is only available with GLIBC
*/
#ifdef __GLIBC__
  extern int backtrace(void**,int);
  extern void backtrace_symbols_fd(void*const*,int,int);
#else
# define backtrace(A,B) 1
# define backtrace_symbols_fd(A,B,C)
#endif
#include <stdio.h>

/*
** Each memory allocation looks like this:
**
**  ------------------------------------------------------------------------
**  | Title |  backtrace pointers |  mem_block_hdr_t |  allocation |  __end_guard |
**  ------------------------------------------------------------------------
**
** The application code sees only a pointer to the allocation.  We have
** to back up from the allocation pointer to find the mem_block_hdr_t.  The
** mem_block_hdr_t tells us the size of the allocation and the number of
** backtrace pointers.  There is also a guard word at the end of the
** mem_block_hdr_t.
*/
struct mem_block_hdr_t {
  int64_t i_size;                          /* Size of this allocation */
  struct mem_block_hdr_t *next, *prev;  /* Linked list of all unfreed memory */
  char n_backtrace;                    /* Number of backtraces on this alloc */
  char n_backtrace_slots;               /* Available backtrace slots */
  u_int8_t n_title;                          /* Bytes of title; includes '\0' */
  u_int8_t type;                           /* Allocation type code */
  int i_fore_guard;                     /* Guard word for sanity */
};

/*
** Guard words
*/
#define FOREGUARD 0x80F5E153
#define REARGUARD 0xE4676B53

/*
** Number of malloc size increments to track.
*/
#define NCSIZE  1000

/*
** All of the static variables used by this module are collected
** into a single structure named "mem".  This is to keep the
** static variables organized and to reduce namespace pollution
** when this module is combined with other in the amalgamation.
*/
static struct {
  
  /*
  ** Mutex to control access to the memory allocation subsystem.
  */
  dbsql_mutex_t *mutex;

  /*
  ** Head and tail of a linked list of all outstanding allocations
  */
  struct mem_block_hdr_t *first;
  struct mem_block_hdr_t *last;
  
  /*
  ** The number of levels of backtrace to save in new allocations.
  */
  int n_backtrace;
  void (*backtrace_fn)(int, int, void **);

  /*
  ** Title text to insert in front of each block
  */
  int n_title;        /* Bytes of title to save.  Includes '\0' and padding */
  char title[100];  /* The title text */

  /* 
  ** __malloc_disallow() increments the following counter.
  ** __malloc_allow() decrements it.
  */
  int disallow; /* Do not allow memory allocation */

  /*
  ** Gather statistics on the sizes of memory allocations.
  ** n_alloc[i] is the number of allocation attempts of i*8
  ** bytes.  i==NCSIZE is the number of allocation attempts for
  ** sizes more than NCSIZE*8 bytes.
  */
  int n_alloc[NCSIZE];      /* Total number of allocations */
  int n_current[NCSIZE];    /* Current number of allocations */
  int mx_current[NCSIZE];   /* Highwater mark for n_current */

} mem;


/*
** Adjust memory usage statistics
*/
static void __adjust_stats(int i_size, int increment){
  int i = ROUND8(i_size)/8;
  if( i>NCSIZE-1 ){
    i = NCSIZE - 1;
  }
  if( increment>0 ){
    mem.n_alloc[i]++;
    mem.n_current[i]++;
    if( mem.n_current[i]>mem.mx_current[i] ){
      mem.mx_current[i] = mem.n_current[i];
    }
  }else{
    mem.n_current[i]--;
    assert( mem.n_current[i]>=0 );
  }
}

/*
** Given an allocation, find the mem_block_hdr_t for that allocation.
**
** This routine checks the guards at either end of the allocation and
** if they are incorrect it asserts.
*/
static struct mem_block_hdr_t *__memsys_get_header(const void *allocation){
  struct mem_block_hdr_t *p;
  int *p_int;
  u_int8_t *u8;
  int n_reserve;

  p = (struct mem_block_hdr_t*)allocation;
  p--;
  assert( p->i_fore_guard==(int)FOREGUARD );
  n_reserve = ROUND8(p->i_size);
  p_int = (int*)allocation;
  u8 = (u_int8_t*)allocation;
  assert( p_int[n_reserve/sizeof(int)]==(int)REARGUARD );
  /* This checks any of the "extra" bytes allocated due
  ** to rounding up to an 8 byte boundary to ensure 
  ** they haven't been overwritten.
  */
  while( n_reserve-- > p->i_size ) assert( u8[n_reserve]==0x65 );
  return p;
}

/*
** Return the number of bytes currently allocated at address p.
*/
static int __mem_size(void *p){
  struct mem_block_hdr_t *hdr;
  if( !p ){
    return 0;
  }
  hdr = __memsys_get_header(p);
  return (int)hdr->i_size;
}

/*
** Initialize the memory allocation subsystem.
*/
static int __mem_init(void *__not_used){
  UNUSED_PARAMETER(__not_used);
  assert( (sizeof(struct mem_block_hdr_t)&7) == 0 );
  if( !__global_config.memstat ){
    /* If memory status is enabled, then the malloc.c wrapper will already
    ** hold the STATIC_MEM mutex when the routines here are invoked. */
    mem.mutex = __dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MEM);
  }
  return DBSQL_OK;
}

/*
** Deinitialize the memory allocation subsystem.
*/
static void __mem_shutdown(void *__not_used){
  UNUSED_PARAMETER(__not_used);
  mem.mutex = 0;
}

/*
** Round up a request size to the next valid allocation size.
*/
static int __mem_roundup(int n){
  return ROUND8(n);
}

/*
** Fill a buffer with pseudo-random bytes.  This is used to preset
** the content of a new memory allocation to unpredictable values and
** to clear the content of a freed allocation to unpredictable values.
*/
static void __random_fill(char *buf, int n_byte){
  unsigned int x, y, r;
  x = DBSQL_PTR_TO_INT(buf);
  y = n_byte | 1;
  while( n_byte >= 4 ){
    x = (x>>1) ^ (-(int)(x&1) & 0xd0000001);
    y = y*1103515245 + 12345;
    r = x ^ y;
    *(int*)buf = r;
    buf += 4;
    n_byte -= 4;
  }
  while( n_byte-- > 0 ){
    x = (x>>1) ^ (-(int)(x&1) & 0xd0000001);
    y = y*1103515245 + 12345;
    r = x ^ y;
    *(buf++) = r & 0xff;
  }
}

/*
** Allocate n_byte bytes of memory.
*/
static void *__mem_malloc(int n_byte){
  struct mem_block_hdr_t *hdr;
  void **bt;
  char *z;
  int *p_int;
  void *p = 0;
  int total_size;
  int n_reserve;
  dbsql_mutex_enter(mem.mutex);
  assert( mem.disallow==0 );
  n_reserve = ROUND8(n_byte);
  total_size = n_reserve + sizeof(*hdr) + sizeof(int) +
               mem.n_backtrace*sizeof(void*) + mem.n_title;
  p = malloc(total_size);
  if( p ){
    z = p;
    bt = (void**)&z[mem.n_title];
    hdr = (struct mem_block_hdr_t*)&bt[mem.n_backtrace];
    hdr->next = 0;
    hdr->prev = mem.last;
    if( mem.last ){
      mem.last->next = hdr;
    }else{
      mem.first = hdr;
    }
    mem.last = hdr;
    hdr->i_fore_guard = FOREGUARD;
    hdr->type = MEMTYPE_HEAP;
    hdr->n_backtrace_slots = mem.n_backtrace;
    hdr->n_title = mem.n_title;
    if( mem.n_backtrace ){
      void *addrs[40];
      hdr->n_backtrace = backtrace(addrs, mem.n_backtrace+1)-1;
      memcpy(bt, &addrs[1], hdr->n_backtrace*sizeof(void*));
      assert(bt[0]);
      if( mem.backtrace_fn ){
        mem.backtrace_fn(n_byte, hdr->n_backtrace-1, &addrs[1]);
      }
    }else{
      hdr->n_backtrace = 0;
    }
    if( mem.n_title ){
      memcpy(z, mem.title, mem.n_title);
    }
    hdr->i_size = n_byte;
    __adjust_stats(n_byte, +1);
    p_int = (int*)&hdr[1];
    p_int[n_reserve/sizeof(int)] = REARGUARD;
    __random_fill((char*)p_int, n_byte);
    memset(((char*)p_int)+n_byte, 0x65, n_reserve-n_byte);
    p = (void*)p_int;
  }
  dbsql_mutex_leave(mem.mutex);
  return p; 
}

/*
** Free memory.
*/
static void __mem_free(void *prior){
  struct mem_block_hdr_t *hdr;
  void **bt;
  char *z;
  assert( __global_config.memstat || __global_config.core_mutex==0 
       || mem.mutex!=0 );
  hdr = __memsys_get_header(prior);
  bt = (void**)hdr;
  bt -= hdr->n_backtrace_slots;
  dbsql_mutex_enter(mem.mutex);
  if( hdr->prev ){
    assert( hdr->prev->next==hdr );
    hdr->prev->next = hdr->next;
  }else{
    assert( mem.first==hdr );
    mem.first = hdr->next;
  }
  if( hdr->next ){
    assert( hdr->next->prev==hdr );
    hdr->next->prev = hdr->prev;
  }else{
    assert( mem.last==hdr );
    mem.last = hdr->prev;
  }
  z = (char*)bt;
  z -= hdr->n_title;
  __adjust_stats((int)hdr->i_size, -1);
  __random_fill(z, sizeof(void*)*hdr->n_backtrace_slots + sizeof(*hdr) +
                (int)hdr->i_size + sizeof(int) + hdr->n_title);
  free(z);
  dbsql_mutex_leave(mem.mutex);  
}

/*
** Change the size of an existing memory allocation.
**
** For this debugging implementation, we *always* make a copy of the
** allocation into a new place in memory.  In this way, if the 
** higher level code is using pointer to the old allocation, it is 
** much more likely to break and we are much more liking to find
** the error.
*/
static void *__mem_realloc(void *prior, int n_byte){
  struct mem_block_hdr_t *old_hdr;
  void *p_new;
  assert( mem.disallow==0 );
  assert( (n_byte & 7)==0 );     /* EV: R-46199-30249 */
  old_hdr = __memsys_get_header(prior);
  p_new = __mem_malloc(n_byte);
  if( p_new ){
    memcpy(p_new, prior, (int)(n_byte<old_hdr->i_size ? n_byte : old_hdr->i_size));
    if( n_byte>old_hdr->i_size ){
      __random_fill(&((char*)p_new)[old_hdr->i_size], n_byte - (int)old_hdr->i_size);
    }
    __mem_free(prior);
  }
  return p_new;
}

/*
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

/*
** Set the "type" of an allocation.
*/
void __memdebug_set_type(void *p, u_int8_t type){
  if( p && __global_config.m.__x_free==__mem_free ){
    struct mem_block_hdr_t *hdr;
    hdr = __memsys_get_header(p);
    assert( hdr->i_fore_guard==FOREGUARD );
    hdr->type = type;
  }
}

/*
** Return TRUE if the mask of type in type matches the type of the
** allocation p.  Also return true if p==NULL.
**
** This routine is designed for use within an assert() statement, to
** verify the type of an allocation.  For example:
**
**     assert( __memdebug_has_type(p, MEMTYPE_HEAP) );
*/
int __memdebug_has_type(const void *p, u_int8_t type){
  int rc = 1;
  if( p && __global_config.m.__x_free==__mem_free ){
    struct mem_block_hdr_t *hdr;
    hdr = __memsys_get_header(p);
    assert( hdr->i_fore_guard==FOREGUARD );         /* Allocation is valid */
    if( (hdr->type&type)==0 ){
      rc = 0;
    }
  }
  return rc;
}

/*
** Return TRUE if the mask of type in type matches no bits of the type of the
** allocation p.  Also return true if p==NULL.
**
** This routine is designed for use within an assert() statement, to
** verify the type of an allocation.  For example:
**
**     assert( __memdebug_no_type(p, MEMTYPE_LOOKASIDE) );
*/
int __memdebug_no_type(const void *p, u_int8_t type){
  int rc = 1;
  if( p && __global_config.m.__x_free==__mem_free ){
    struct mem_block_hdr_t *hdr;
    hdr = __memsys_get_header(p);
    assert( hdr->i_fore_guard==FOREGUARD );         /* Allocation is valid */
    if( (hdr->type&type)!=0 ){
      rc = 0;
    }
  }
  return rc;
}

/*
** Set the number of backtrace levels kept for each allocation.
** A value of zero turns off backtracing.  The number is always rounded
** up to a multiple of 2.
*/
void __memdebug_backtrace(int depth){
  if( depth<0 ){ depth = 0; }
  if( depth>20 ){ depth = 20; }
  depth = (depth+1)&0xfe;
  mem.n_backtrace = depth;
}

void __memdebug_backtrace_callback(void (*backtrace_fn)(int, int, void **)){
  mem.backtrace_fn = backtrace_fn;
}

/*
** Set the title string for subsequent allocations.
*/
void __memdebug_settitle(const char *title){
  unsigned int n = __strlen30(title) + 1;
  dbsql_mutex_enter(mem.mutex);
  if( n>=sizeof(mem.title) ) n = sizeof(mem.title)-1;
  memcpy(mem.title, title, n);
  mem.title[n] = 0;
  mem.n_title = ROUND8(n);
  dbsql_mutex_leave(mem.mutex);
}

void __memdebug_sync(){
  struct mem_block_hdr_t *hdr;
  for(hdr=mem.first; hdr; hdr=hdr->next){
    void **bt = (void**)hdr;
    bt -= hdr->n_backtrace_slots;
    mem.backtrace_fn((int)hdr->i_size, hdr->n_backtrace-1, &bt[1]);
  }
}

/*
** Open the file indicated and write a log of all unfreed memory 
** allocations into that log.
*/
void __memdebug_dump(const char *filename){
  FILE *out;
  struct mem_block_hdr_t *hdr;
  void **bt;
  int i;
  out = fopen(filename, "w");
  if( out==0 ){
    fprintf(stderr, "** Unable to output memory debug output log: %s **\n",
                    filename);
    return;
  }
  for(hdr=mem.first; hdr; hdr=hdr->next){
    char *z = (char*)hdr;
    z -= hdr->n_backtrace_slots*sizeof(void*) + hdr->n_title;
    fprintf(out, "**** %lld bytes at %p from %s ****\n", 
            hdr->i_size, &hdr[1], hdr->n_title ? z : "???");
    if( hdr->n_backtrace ){
      fflush(out);
      bt = (void**)hdr;
      bt -= hdr->n_backtrace_slots;
      backtrace_symbols_fd(bt, hdr->n_backtrace, fileno(out));
      fprintf(out, "\n");
    }
  }
  fprintf(out, "COUNTS:\n");
  for(i=0; i<NCSIZE-1; i++){
    if( mem.n_alloc[i] ){
      fprintf(out, "   %5d: %10d %10d %10d\n", 
            i*8, mem.n_alloc[i], mem.n_current[i], mem.mx_current[i]);
    }
  }
  if( mem.n_alloc[NCSIZE-1] ){
    fprintf(out, "   %5d: %10d %10d %10d\n",
             NCSIZE*8-8, mem.n_alloc[NCSIZE-1],
             mem.n_current[NCSIZE-1], mem.mx_current[NCSIZE-1]);
  }
  fclose(out);
}

/*
** Return the number of times __mem_malloc() has been called.
*/
int __memdebug_malloc_count(){
  int i;
  int n_total = 0;
  for(i=0; i<NCSIZE; i++){
    n_total += mem.n_alloc[i];
  }
  return n_total;
}


#endif /* DBSQL_MEMDEBUG */
