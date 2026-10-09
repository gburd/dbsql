/*
** 2001 September 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This is the implementation of generic hash-tables
** used in dbsql.
*/
#include "dbsql_int.h"
#include <assert.h>

/* Turn bulk memory into a hash table object by initializing the
** fields of the hash_t structure.
**
** "p_new" is a pointer to the hash table that is to be initialized.
*/
void __hash_init(hash_t *p_new){
  assert( p_new!=0 );
  p_new->first = 0;
  p_new->count = 0;
  p_new->htsize = 0;
  p_new->ht = 0;
}

/* Remove all entries from a hash table.  Reclaim all memory.
** Call this routine to delete a hash table or to reset a hash table
** to the empty state.
*/
void __hash_clear(hash_t *h){
  hash_elem_t *elem;         /* For looping over all elements of the table */

  assert( h!=0 );
  elem = h->first;
  h->first = 0;
  dbsql_free(h->ht);
  h->ht = 0;
  h->htsize = 0;
  while( elem ){
    hash_elem_t *next_elem = elem->next;
    dbsql_free(elem);
    elem = next_elem;
  }
  h->count = 0;
}

/*
** The hashing function.
*/
static unsigned int __str_hash(const char *z){
  unsigned int h = 0;
  while( z[0] ){     /*OPTIMIZATION-IF-TRUE*/
    /* Knuth multiplicative hashing.  (Sorting & Searching, p. 510).
    ** 0x9e3779b1 is 2654435761 which is the closest prime number to
    ** (2**32)*golden_ratio, where golden_ratio = (sqrt(5) - 1)/2.
    **
    ** Only bits 0xdf for ASCII and bits 0xbf for EBCDIC each octet are
    ** hashed since the omitted bits determine the upper/lower case difference.
    */
#ifdef DBSQL_EBCDIC
    h += 0xbf & (unsigned char)*(z++);
#else
    h += 0xdf & (unsigned char)*(z++);
#endif
    h *= 0x9e3779b1;
  }
  return h;
}


/* Link p_new element into the hash table h.  If entry!=0 then also
** insert p_new into the entry hash bucket.
*/
static void __insert_element(
  hash_t *h,              /* The complete hash table */
  struct _ht *entry,    /* The entry into which p_new is inserted */
  hash_elem_t *p_new         /* The element to be inserted */
){
  hash_elem_t *head;       /* First element already in entry */
  if( entry ){
    head = entry->count ? entry->chain : 0;
    entry->count++;
    entry->chain = p_new;
  }else{
    head = 0;
  }
  if( head ){
    p_new->next = head;
    p_new->prev = head->prev;
    if( head->prev ){ head->prev->next = p_new; }
    else             { h->first = p_new; }
    head->prev = p_new;
  }else{
    p_new->next = h->first;
    if( h->first ){ h->first->prev = p_new; }
    p_new->prev = 0;
    h->first = p_new;
  }
}


/* Resize the hash table so that it contains "new_size" buckets.
**
** The hash table might fail to resize if dbsql_malloc() fails or
** if the new size is the same as the prior size.
** Return TRUE if the resize occurs and false if not.
*/
static int rehash(hash_t *h, unsigned int new_size){
  struct _ht *new_ht;            /* The new hash table */
  hash_elem_t *elem, *next_elem;    /* For looping over existing elements */

#if DBSQL_MALLOC_SOFT_LIMIT>0
  if( new_size*sizeof(struct _ht)>DBSQL_MALLOC_SOFT_LIMIT ){
    new_size = DBSQL_MALLOC_SOFT_LIMIT/sizeof(struct _ht);
  }
  if( new_size==h->htsize ) return 0;
#endif

  /* The inability to allocates space for a larger hash table is
  ** a performance hit but it is not a fatal error.  So mark the
  ** allocation as a benign. Use __dbsql_malloc()/memset(0) instead of 
  ** __malloc_zero() to make the allocation, as __malloc_zero()
  ** only zeroes the requested number of bytes whereas this module will
  ** use the actual amount of space allocated for the hash table (which
  ** may be larger than the requested amount).
  */
  __begin_benign_malloc();
  new_ht = (struct _ht *)__dbsql_malloc( new_size*sizeof(struct _ht) );
  __end_benign_malloc();

  if( new_ht==0 ) return 0;
  dbsql_free(h->ht);
  h->ht = new_ht;
  h->htsize = new_size = __malloc_size(new_ht)/sizeof(struct _ht);
  memset(new_ht, 0, new_size*sizeof(struct _ht));
  for(elem=h->first, h->first=0; elem; elem = next_elem){
    next_elem = elem->next;
    __insert_element(h, &new_ht[elem->h % new_size], elem);
  }
  return 1;
}

/* This function (for internal use only) locates an element in an
** hash table that matches the given key.  If no element is found,
** a pointer to a static null element with hash_elem_t.data==0 is returned.
** If h is not NULL, then the hash for this key is written to *h.
*/
static hash_elem_t *__find_element_with_hash(
  const hash_t *p_h,     /* The h to be searched */
  const char *key,   /* The key we are searching for */
  unsigned int *hash /* Write the hash value here */
){
  hash_elem_t *elem;                /* Used to loop thru the element list */
  unsigned int count;            /* Number of elements left to test */
  unsigned int h;                /* The computed hash */
  static hash_elem_t null_element = { 0, 0, 0, 0, 0 };

  h = __str_hash(key);
  if( p_h->ht ){   /*OPTIMIZATION-IF-TRUE*/
    struct _ht *entry;
    entry = &p_h->ht[h % p_h->htsize];
    elem = entry->chain;
    count = entry->count;
  }else{
    elem = p_h->first;
    count = p_h->count;
  }
  if( hash ) *hash = h;
  while( count ){
    assert( elem!=0 );
    if( h==elem->h && __str_icmp(elem->key,key)==0 ){ 
      return elem;
    }
    elem = elem->next;
    count--;
  }
  return &null_element;
}

/* Remove a single entry from the hash table given a pointer to that
** element and a hash on the element's key.
*/
static void __remove_element(
  hash_t *h,         /* The h containing "elem" */
  hash_elem_t *elem    /* The element to be removed from the h */
){
  struct _ht *entry;
  if( elem->prev ){
    elem->prev->next = elem->next; 
  }else{
    h->first = elem->next;
  }
  if( elem->next ){
    elem->next->prev = elem->prev;
  }
  if( h->ht ){
    entry = &h->ht[elem->h % h->htsize];
    if( entry->chain==elem ){
      entry->chain = elem->next;
    }
    assert( entry->count>0 );
    entry->count--;
  }
  dbsql_free( elem );
  h->count--;
  if( h->count==0 ){
    assert( h->first==0 );
    assert( h->count==0 );
    __hash_clear(h);
  }
}

/* Attempt to locate an element of the hash table h with a key
** that matches key.  Return the data for this element if it is
** found, or NULL if there is no match.
*/
void *__hash_find(const hash_t *h, const char *key){
  assert( h!=0 );
  assert( key!=0 );
  return __find_element_with_hash(h, key, 0)->data;
}

/* Insert an element into the hash table h.  The key is key
** and the data is "data".
**
** If no element exists with a matching key, then a new
** element is created and NULL is returned.
**
** If another element already exists with the same key, then the
** new data replaces the old data and the old data is returned.
** The key is not copied in this instance.  If a malloc fails, then
** the new data is returned and the hash table is unchanged.
**
** If the "data" parameter to this function is NULL, then the
** element corresponding to "key" is removed from the hash table.
*/
void *__hash_insert(hash_t *p_h, const char *key, void *data){
  unsigned int h;       /* the hash of the key modulo hash table size */
  hash_elem_t *elem;       /* Used to loop thru the element list */
  hash_elem_t *new_elem;   /* New element added to the h */

  assert( p_h!=0 );
  assert( key!=0 );
  elem = __find_element_with_hash(p_h,key,&h);
  if( elem->data ){
    void *old_data = elem->data;
    if( data==0 ){
      __remove_element(p_h,elem);
    }else{
      elem->data = data;
      elem->key = key;
    }
    return old_data;
  }
  if( data==0 ) return 0;
  new_elem = (hash_elem_t*)__dbsql_malloc( sizeof(hash_elem_t) );
  if( new_elem==0 ) return data;
  new_elem->key = key;
  new_elem->h = h;
  new_elem->data = data;
  p_h->count++;
  if( p_h->count>=5 && p_h->count > 2*p_h->htsize ){
    rehash(p_h, p_h->count*3);
  }
  __insert_element(p_h, p_h->ht ? &p_h->ht[new_elem->h % p_h->htsize] : 0, new_elem);
  return 0;
}

