/*
** 2008 February 16
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file implements an object that represents a fixed-length
** bitmap.  Bits are numbered starting with 1.
**
** A bitmap is used to record which pages of a database file have been
** journalled during a transaction, or which pages have the "dont-write"
** property.  Usually only a few pages are meet either condition.
** So the bitmap is usually sparse and has low cardinality.
** But sometimes (for example when during a DROP of a large table) most
** or all of the pages in a database can get journalled.  In those cases,
** the bitmap becomes dense with high cardinality.  The algorithm needs
** to handle both cases well.
**
** The size of the bitmap is fixed when the object is created.
**
** All bits are clear when the bitmap is created.  Individual bits
** may be set or cleared one at a time.
**
** Test operations are about 100 times more common that set operations.
** Clear operations are exceedingly rare.  There are usually between
** 5 and 500 set operations per bitvec_t object, though the number of sets can
** sometimes grow into tens of thousands or larger.  The size of the
** bitvec_t object is the number of pages in the database file at the
** start of a transaction, and is thus usually less than a few thousand,
** but can be as large as 2 billion for a really big database.
*/
#include "dbsql_int.h"

/* Size of the bitvec_t structure in bytes. */
#define BITVEC_SZ        512

/* Round the union size down to the nearest pointer boundary, since that's how
** it will be aligned within the bitvec_t struct. */
#define BITVEC_USIZE \
    (((BITVEC_SZ-(3*sizeof(u_int32_t)))/sizeof(bitvec_t*))*sizeof(bitvec_t*))

/* Type of the array "element" for the bitmap representation.
** Should be a power of 2, and ideally, evenly divide into BITVEC_USIZE.
** Setting this to the "natural word" size of your CPU may improve
** performance. */
#define BITVEC_TELEM     u_int8_t
/* Size, in bits, of the bitmap element. */
#define BITVEC_SZELEM    8
/* Number of elements in a bitmap array. */
#define BITVEC_NELEM     (BITVEC_USIZE/sizeof(BITVEC_TELEM))
/* Number of bits in the bitmap array. */
#define BITVEC_NBIT      (BITVEC_NELEM*BITVEC_SZELEM)

/* Number of u32 values in hash table. */
#define BITVEC_NINT      (BITVEC_USIZE/sizeof(u_int32_t))
/* Maximum number of entries in hash table before
** sub-dividing and re-hashing. */
#define BITVEC_MXHASH    (BITVEC_NINT/2)
/* Hashing function for the hashes representation.
** Empirical testing showed that the *37 multiplier
** (an arbitrary prime)in the hash function provided
** no fewer collisions than the no-op *1. */
#define BITVEC_HASH(X)   (((X)*1)%BITVEC_NINT)

#define BITVEC_NPTR      ((u_int32_t)(BITVEC_USIZE/sizeof(bitvec_t *)))


/*
** A bitmap is an instance of the following structure.
**
** This bitmap records the existence of zero or more bits
** with values between 1 and i_size, inclusive.
**
** There are three possible representations of the bitmap.
** If i_size<=BITVEC_NBIT, then bitvec_t.u.bitmaps[] is a straight
** bitmap.  The least significant bit is bit 1.
**
** If i_size>BITVEC_NBIT and i_divisor==0 then bitvec_t.u.hashes[] is
** a hash table that will hold up to BITVEC_MXHASH distinct values.
**
** Otherwise, the value i is redirected into one of BITVEC_NPTR
** sub-bitmaps pointed to by bitvec_t.u.subs[].  Each subbitmap
** handles up to i_divisor separate values of i.  subs[0] holds
** values between 1 and i_divisor.  subs[1] holds values between
** i_divisor+1 and 2*i_divisor.  subs[N] holds values between
** N*i_divisor+1 and (N+1)*i_divisor.  Each subbitmap is normalized
** to hold deal with values between 1 and i_divisor.
*/
struct bitvec_t {
  u_int32_t i_size;      /* Maximum bit index.  Max i_size is 4,294,967,296. */
  u_int32_t n_set;       /* Number of bits that are set - only valid for hashes
                  ** element.  Max is BITVEC_NINT.  For BITVEC_SZ of 512,
                  ** this would be 125. */
  u_int32_t i_divisor;   /* Number of bits handled by each subs[] entry. */
                  /* Should >=0 for subs element. */
                  /* Max i_divisor is max(u32) / BITVEC_NPTR + 1.  */
                  /* For a BITVEC_SZ of 512, this would be 34,359,739. */
  union {
    BITVEC_TELEM bitmaps[BITVEC_NELEM];    /* Bitmap representation */
    u_int32_t hashes[BITVEC_NINT];      /* hash_t table representation */
    bitvec_t *subs[BITVEC_NPTR];  /* Recursive representation */
  } u;
};


/*
** Create a new bitmap object able to handle bits between 0 and i_size,
** inclusive.  Return a pointer to the new object.  Return NULL if
** malloc fails.
*/
bitvec_t *__bitvec_create(u_int32_t i_size){
  bitvec_t *p;
  assert( sizeof(*p)==BITVEC_SZ );
  p = __malloc_zero( sizeof(*p) );
  if( p ){
    p->i_size = i_size;
  }
  return p;
}

/*
** Check to see if the i-th bit is set.  Return true or false.
** If p is NULL (if the bitmap has not been created) or if
** i is out of range, then return false.
*/
int __bitvec_test_not_null(bitvec_t *p, u_int32_t i){
  assert( p!=0 );
  i--;
  if( i>=p->i_size ) return 0;
  while( p->i_divisor ){
    u_int32_t bin = i/p->i_divisor;
    i = i%p->i_divisor;
    p = p->u.subs[bin];
    if (!p) {
      return 0;
    }
  }
  if( p->i_size<=BITVEC_NBIT ){
    return (p->u.bitmaps[i/BITVEC_SZELEM] & (1<<(i&(BITVEC_SZELEM-1))))!=0;
  } else{
    u_int32_t h = BITVEC_HASH(i++);
    while( p->u.hashes[h] ){
      if( p->u.hashes[h]==i ) return 1;
      h = (h+1) % BITVEC_NINT;
    }
    return 0;
  }
}
int __bitvec_test(bitvec_t *p, u_int32_t i){
  return p!=0 && __bitvec_test_not_null(p,i);
}

/*
** Set the i-th bit.  Return 0 on success and an error code if
** anything goes wrong.
**
** This routine might cause sub-bitmaps to be allocated.  Failing
** to get the memory needed to hold the sub-bitmap is the only
** that can go wrong with an insert, assuming p and i are valid.
**
** The calling function must ensure that p is a valid bitvec_t object
** and that the value for "i" is within range of the bitvec_t object.
** Otherwise the behavior is undefined.
*/
int __bitvec_set(bitvec_t *p, u_int32_t i){
  u_int32_t h;
  if( p==0 ) return DBSQL_OK;
  assert( i>0 );
  assert( i<=p->i_size );
  i--;
  while((p->i_size > BITVEC_NBIT) && p->i_divisor) {
    u_int32_t bin = i/p->i_divisor;
    i = i%p->i_divisor;
    if( p->u.subs[bin]==0 ){
      p->u.subs[bin] = __bitvec_create( p->i_divisor );
      if( p->u.subs[bin]==0 ) return DBSQL_NOMEM_BKPT;
    }
    p = p->u.subs[bin];
  }
  if( p->i_size<=BITVEC_NBIT ){
    p->u.bitmaps[i/BITVEC_SZELEM] |= 1 << (i&(BITVEC_SZELEM-1));
    return DBSQL_OK;
  }
  h = BITVEC_HASH(i++);
  /* if there wasn't a hash collision, and this doesn't */
  /* completely fill the hash, then just add it without */
  /* worrying about sub-dividing and re-hashing. */
  if( !p->u.hashes[h] ){
    if (p->n_set<(BITVEC_NINT-1)) {
      goto bitvec_set_end;
    } else {
      goto bitvec_set_rehash;
    }
  }
  /* there was a collision, check to see if it's already */
  /* in hash, if not, try to find a spot for it */
  do {
    if( p->u.hashes[h]==i ) return DBSQL_OK;
    h++;
    if( h>=BITVEC_NINT ) h = 0;
  } while( p->u.hashes[h] );
  /* we didn't find it in the hash.  h points to the first */
  /* available free spot. check to see if this is going to */
  /* make our hash too "full".  */
bitvec_set_rehash:
  if( p->n_set>=BITVEC_MXHASH ){
    unsigned int j;
    int rc;
    u_int32_t *values = __stack_alloc_raw(0, sizeof(p->u.hashes));
    if( values==0 ){
      return DBSQL_NOMEM_BKPT;
    }else{
      memcpy(values, p->u.hashes, sizeof(p->u.hashes));
      memset(p->u.subs, 0, sizeof(p->u.subs));
      p->i_divisor = p->i_size/BITVEC_NPTR;
      if( (p->i_size%BITVEC_NPTR)!=0 ) p->i_divisor++;
      if( p->i_divisor<BITVEC_NBIT ) p->i_divisor = BITVEC_NBIT;
      rc = __bitvec_set(p, i);
      for(j=0; j<BITVEC_NINT; j++){
        if( values[j] ) rc |= __bitvec_set(p, values[j]);
      }
      __stack_free(0, values);
      return rc;
    }
  }
bitvec_set_end:
  p->n_set++;
  p->u.hashes[h] = i;
  return DBSQL_OK;
}

/*
** Clear the i-th bit.
**
** buf must be a pointer to at least BITVEC_SZ bytes of temporary storage
** that __bitvec_clear can use to rebuilt its hash table.
*/
void __bitvec_clear(bitvec_t *p, u_int32_t i, void *buf){
  if( p==0 ) return;
  assert( i>0 );
  i--;
  while( p->i_divisor ){
    u_int32_t bin = i/p->i_divisor;
    i = i%p->i_divisor;
    p = p->u.subs[bin];
    if (!p) {
      return;
    }
  }
  if( p->i_size<=BITVEC_NBIT ){
    p->u.bitmaps[i/BITVEC_SZELEM] &= ~(BITVEC_TELEM)(1<<(i&(BITVEC_SZELEM-1)));
  }else{
    unsigned int j;
    u_int32_t *values = buf;
    memcpy(values, p->u.hashes, sizeof(p->u.hashes));
    memset(p->u.hashes, 0, sizeof(p->u.hashes));
    p->n_set = 0;
    for(j=0; j<BITVEC_NINT; j++){
      if( values[j] && values[j]!=(i+1) ){
        u_int32_t h = BITVEC_HASH(values[j]-1);
        p->n_set++;
        while( p->u.hashes[h] ){
          h++;
          if( h>=BITVEC_NINT ) h = 0;
        }
        p->u.hashes[h] = values[j];
      }
    }
  }
}

/*
** Destroy a bitmap object.  Reclaim all memory used.
*/
void __bitvec_destroy(bitvec_t *p){
  if( p==0 ) return;
  if( p->i_divisor ){
    unsigned int i;
    for(i=0; i<BITVEC_NPTR; i++){
      __bitvec_destroy(p->u.subs[i]);
    }
  }
  dbsql_free(p);
}

/*
** Return the value of the i_size parameter specified when bitvec_t *p
** was created.
*/
u_int32_t __bitvec_size(bitvec_t *p){
  return p->i_size;
}

#ifdef DBSQL_DEBUG
/*
** Show the content of a bitvec_t option and its children.  Indent
** everything by n spaces.  Add x to each bitvec value.
**
** From a debugger such as gdb, one can type:
**
**    call __show_bitvec(p)
**
** For some bitvec_t p and see a recursive view of the bitvec_t's content.
*/
static void __show_bitvec_local(bitvec_t *p, int n, unsigned x){
  int i;
  if( p==0 ){
    printf("NULL\n");
    return;
  }
  printf("Bitvec 0x%p iSize=%u", p, p->i_size);
  if( p->i_size<=BITVEC_NBIT ){
    printf(" bitmap\n");
    printf("%*s   bits:", n, "");
    for(i=1; i<=BITVEC_NBIT; i++){
      if( __bitvec_test(p,i) ) printf(" %u", x+(unsigned)i);
    }
    printf("\n");
  }else if( p->i_divisor==0 ){
    printf(" hash with %u entries\n", p->n_set);
    printf("%*s   bits:", n, "");
    for(i=0; i<BITVEC_NINT; i++){
      if( p->u.hashes[i] ) printf(" %u", x+(unsigned)p->u.hashes[i]);
    }
    printf("\n");
  }else{
    printf(" sub-bitvec with iDivisor=%u\n", p->i_divisor);
    for(i=0; i<BITVEC_NPTR; i++){
      if( p->u.subs[i]==0 ) continue;
      printf("%*s   apSub[%d]=", n, "", i);
      __show_bitvec_local(p->u.subs[i], n+4, i*p->i_divisor);
    }
  }
}
void __show_bitvec(bitvec_t *p){
  __show_bitvec_local(p, 0, 0);
}
#endif

#ifndef DBSQL_UNTESTABLE
/*
** Let V[] be an array of unsigned characters sufficient to hold
** up to N bits.  Let I be an integer between 0 and N.  0<=I<N.
** Then the following macros can be used to set, clear, or test
** individual bits within V.
*/
#define SETBIT(V,I)      V[I>>3] |= (1<<(I&7))
#define CLEARBIT(V,I)    V[I>>3] &= ~(BITVEC_TELEM)(1<<(I&7))
#define TESTBIT(V,I)     (V[I>>3]&(1<<(I&7)))!=0


/*
** This routine runs an extensive test of the bitvec_t code.
**
** The input is an array of integers that acts as a program
** to test the bitvec_t.  The integers are opcodes followed
** by 0, 1, or 3 operands, depending on the opcode.  Another
** opcode follows immediately after the last operand.
**
** There are opcodes numbered starting with 0.  0 is the
** "halt" opcode and causes the test to end.
**
**    0          Halt and return the number of errors
**    1 N S X    Set N bits beginning with S and incrementing by X
**    2 N S X    Clear N bits beginning with S and incrementing by X
**    3 N        Set N randomly chosen bits
**    4 N        Clear N randomly chosen bits
**    5 N S X    Set N bits from S increment X in array only, not in bitvec
**    6          Invoice __show_bitvec() on the bitvec_t object so far
**    7 X        Show compile-time parameters and the hash of X         
**
** The opcodes 1 through 4 perform set and clear operations are performed
** on both a bitvec_t object and on a linear array of bits obtained from malloc.
** Opcode 5 works on the linear array only, not on the bitvec_t.
** Opcode 5 is used to deliberately induce a fault in order to
** confirm that error detection works.  Opcodes 6 and greater are
** state output opcodes.  Opcodes 6 and greater are no-ops unless
** dbsql has been compiled with DBSQL_DEBUG.
**
** At the conclusion of the test the linear array is compared
** against the bitvec_t object.  If there are any differences,
** an error is returned.  If they are the same, zero is returned.
**
** If a memory allocation error occurs, return -1.
**
** sz is the size of the bitvec_t.  Or if sz is negative, make the size
** 2*(unsigned)(-sz) and disabled the linear vector check.
*/
int __bitvec_builtin_test(int sz, int *ops){
  bitvec_t *bitvec = 0;
  unsigned char *v = 0;
  int rc = -1;
  int i, nx, pc, op;
  void *tmp_space;

  /* Allocate the bitvec_t to be tested and a linear array of
  ** bits to act as the reference */
  if( sz<=0 ){
    bitvec = __bitvec_create( 2*(unsigned)(-sz) );
    v = 0;
  }else{
    bitvec = __bitvec_create( sz );
    v = __malloc_zero( (7+(int64_t)sz)/8 + 1 );
  }
  tmp_space = dbsql_malloc64(BITVEC_SZ);
  if( bitvec==0 || tmp_space==0 || (v==0 && sz>0) ) goto bitvec_end;

  /* NULL bitvec tests */
  __bitvec_set(0, 1);
  __bitvec_clear(0, 1, tmp_space);

  /* Run the program */
  pc = i = 0;
  while( (op = ops[pc])!=0 ){
    if( op>=6 ){
#ifdef DBSQL_DEBUG
      if( op==6 ){
        __show_bitvec(bitvec);
      }else if( op==7 ){
        printf("BITVEC_SZ     = %d (%d by sizeof)\n",
               BITVEC_SZ, (int)sizeof(bitvec_t));
        printf("BITVEC_USIZE  = %d\n", (int)BITVEC_USIZE);
        printf("BITVEC_NELEM  = %d\n", (int)BITVEC_NELEM);
        printf("BITVEC_NBIT   = %d\n", (int)BITVEC_NBIT);
        printf("BITVEC_NINT   = %d\n", (int)BITVEC_NINT);
        printf("BITVEC_MXHASH = %d\n", (int)BITVEC_MXHASH);
        printf("BITVEC_NPTR   = %d\n", (int)BITVEC_NPTR);
      }
#endif
      pc++;
      continue;
    }
    switch( op ){
      case 1:
      case 2:
      case 5: {
        nx = 4;
        i = ops[pc+2] - 1;
        ops[pc+2] += ops[pc+3];
        break;
      }
      case 3:
      case 4:
      default: {
        nx = 2;
        dbsql_randomness(sizeof(i), &i);
        break;
      }
    }
    if( (--ops[pc+1]) > 0 ) nx = 0;
    pc += nx;
    i = (i & 0x7fffffff)%sz;
    if( (op & 1)!=0 ){
      if( v ) SETBIT(v, (i+1));
      if( op!=5 ){
        if( __bitvec_set(bitvec, i+1) ) goto bitvec_end;
      }
    }else{
      if( v ) CLEARBIT(v, (i+1));
      __bitvec_clear(bitvec, i+1, tmp_space);
    }
  }

  /* Test to make sure the linear array exactly matches the
  ** bitvec_t object.  Start with the assumption that they do
  ** match (rc==0).  Change rc to non-zero if a discrepancy
  ** is found.
  */
  if( v ){
    rc = __bitvec_test(0,0) + __bitvec_test(bitvec, sz+1)
            + __bitvec_test(bitvec, 0)
            + (__bitvec_size(bitvec) - sz);
    for(i=1; i<=sz; i++){
      if( (TESTBIT(v,i))!=__bitvec_test(bitvec,i) ){
        rc = i;
        break;
      }
    }
  }else{
    rc = 0;
  }

  /* Free allocated structure */
bitvec_end:
  dbsql_free(tmp_space);
  dbsql_free(v);
  __bitvec_destroy(bitvec);
  return rc;
}
#endif /* DBSQL_UNTESTABLE */
