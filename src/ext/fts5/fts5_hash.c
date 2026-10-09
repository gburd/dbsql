/*
** 2014 August 11
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
*/



#include "fts5_int.h"

typedef struct fts5_hash_entry_t fts5_hash_entry_t;

/*
** This file contains the implementation of an in-memory hash table used
** to accumulate "term -> doclist" content before it is flushed to a level-0
** segment.
*/


struct fts5_hash_t {
  int detail;                    /* Copy of fts5_config_t.detail */
  int *pn_byte;                    /* Pointer to bytes counter */
  int n_entry;                     /* Number of entries currently in hash */
  int n_slot;                      /* Size of slots[] array */
  fts5_hash_entry_t *scan;           /* Current ordered scan item */
  fts5_hash_entry_t **slots;          /* Array of hash slots */
};

/*
** Each entry in the hash table is represented by an object of the 
** following type. Each object, its key, and its current data are stored 
** in a single memory allocation. The key immediately follows the object 
** in memory. The position list data immediately follows the key data 
** in memory.
**
** The key is fts5_hash_entry_t.n_key bytes in size. It consists of a single
** byte identifying the index (either the main term index or a prefix-index),
** followed by the term data. For example: "0token". There is no 
** nul-terminator - in this case n_key=6.
**
** The data that follows the key is in a similar, but not identical format
** to the doclist data stored in the database. It is:
**
**   * Rowid, as a varint
**   * Position list, without 0x00 terminator.
**   * Size of previous position list and rowid, as a 4 byte
**     big-endian integer.
**
** i_rowid_off:
**   Offset of last rowid written to data area. Relative to first byte of
**   structure.
**
** n_data:
**   Bytes of data written since i_rowid_off.
*/
struct fts5_hash_entry_t {
  fts5_hash_entry_t *hash_next;       /* Next hash entry with same hash-key */
  fts5_hash_entry_t *scan_next;       /* Next entry in sorted order */
  
  int n_alloc;                     /* Total size of allocation */
  int i_sz_poslist;                 /* Offset of space for 4-byte poslist size */
  int n_data;                      /* Total bytes of data (incl. structure) */
  int n_key;                       /* Length of key in bytes */
  u_int8_t del;                        /* Set delete-flag @ i_sz_poslist */
  u_int8_t content;                    /* Set content-flag (detail=none mode) */
  int16_t i_col;                       /* column_t of last value written */
  int i_pos;                       /* Position of last value written */
  int64_t i_rowid;                     /* Rowid of last value written */
};

/*
** Equivalent to:
**
**   char *FTS5_ENTRY_KEY(fts5_hash_entry_t *entry){ return key; }
*/
#define FTS5_ENTRY_KEY(p) ( ((char *)(&(p)[1])) )


/*
** Allocate a new hash table.
*/
int __fts5_hash_new(fts5_config_t *config, fts5_hash_t **pp_new, int *pn_byte){
  int rc = DBSQL_OK;
  fts5_hash_t *p_new;

  *pp_new = p_new = (fts5_hash_t*)dbsql_malloc64(sizeof(fts5_hash_t));
  if( p_new==0 ){
    rc = DBSQL_NOMEM;
  }else{
    int64_t n_byte;
    memset(p_new, 0, sizeof(fts5_hash_t));
    p_new->pn_byte = pn_byte;
    p_new->detail = config->detail;

    p_new->n_slot = 1024;
    n_byte = sizeof(fts5_hash_entry_t*) * p_new->n_slot;
    p_new->slots = (fts5_hash_entry_t**)dbsql_malloc64(n_byte);
    if( p_new->slots==0 ){
      dbsql_free(p_new);
      *pp_new = 0;
      rc = DBSQL_NOMEM;
    }else{
      memset(p_new->slots, 0, (size_t)n_byte);
    }
  }
  return rc;
}

/*
** Free a hash table object.
*/
void __fts5_hash_free(fts5_hash_t *hash){
  if( hash ){
    __fts5_hash_clear(hash);
    dbsql_free(hash->slots);
    dbsql_free(hash);
  }
}

/*
** Empty (but do not delete) a hash table.
*/
void __fts5_hash_clear(fts5_hash_t *hash){
  int i;
  for(i=0; i<hash->n_slot; i++){
    fts5_hash_entry_t *next;
    fts5_hash_entry_t *slot;
    for(slot=hash->slots[i]; slot; slot=next){
      next = slot->hash_next;
      dbsql_free(slot);
    }
  }
  memset(hash->slots, 0, hash->n_slot * sizeof(fts5_hash_entry_t*));
  hash->n_entry = 0;
}

static unsigned int __fts5_hash_key(int n_slot, const u_int8_t *p, int n){
  int i;
  unsigned int h = 13;
  for(i=n-1; i>=0; i--){
    h = (h << 3) ^ h ^ p[i];
  }
  return (h % n_slot);
}

static unsigned int __fts5_hash_key2(int n_slot, u_int8_t b, const u_int8_t *p, int n){
  int i;
  unsigned int h = 13;
  for(i=n-1; i>=0; i--){
    h = (h << 3) ^ h ^ p[i];
  }
  h = (h << 3) ^ h ^ b;
  return (h % n_slot);
}

/*
** Resize the hash table by doubling the number of slots.
*/
static int __fts5_hash_resize(fts5_hash_t *hash){
  int n_new = hash->n_slot*2;
  int i;
  fts5_hash_entry_t **news;
  fts5_hash_entry_t **olds = hash->slots;

  news = (fts5_hash_entry_t**)dbsql_malloc64(n_new*sizeof(fts5_hash_entry_t*));
  if( !news ) return DBSQL_NOMEM;
  memset(news, 0, n_new*sizeof(fts5_hash_entry_t*));

  for(i=0; i<hash->n_slot; i++){
    while( olds[i] ){
      unsigned int i_hash;
      fts5_hash_entry_t *p = olds[i];
      olds[i] = p->hash_next;
      i_hash = __fts5_hash_key(n_new, (u_int8_t*)FTS5_ENTRY_KEY(p), p->n_key);
      p->hash_next = news[i_hash];
      news[i_hash] = p;
    }
  }

  dbsql_free(olds);
  hash->n_slot = n_new;
  hash->slots = news;
  return DBSQL_OK;
}

static int __fts5_hash_add_poslist_size(
  fts5_hash_t *hash, 
  fts5_hash_entry_t *p,
  fts5_hash_entry_t *p2
){
  int n_ret = 0;
  if( p->i_sz_poslist ){
    u_int8_t *ptr = p2 ? (u_int8_t*)p2 : (u_int8_t*)p;
    int n_data = p->n_data;
    if( hash->detail==FTS5_DETAIL_NONE ){
      assert( n_data==p->i_sz_poslist );
      if( p->del ){
        ptr[n_data++] = 0x00;
        if( p->content ){
          ptr[n_data++] = 0x00;
        }
      }
    }else{
      int n_sz = (n_data - p->i_sz_poslist - 1);       /* Size in bytes */
      int n_pos = n_sz*2 + p->del;                     /* Value of n_pos field */

      assert( p->del==0 || p->del==1 );
      if( n_pos<=127 ){
        ptr[p->i_sz_poslist] = (u_int8_t)n_pos;
      }else{
        int n_byte = __fts5_get_varint_len((u_int32_t)n_pos);
        memmove(&ptr[p->i_sz_poslist + n_byte], &ptr[p->i_sz_poslist + 1], n_sz);
        __fts5_put_varint(&ptr[p->i_sz_poslist], n_pos);
        n_data += (n_byte-1);
      }
    }

    n_ret = n_data - p->n_data;
    if( p2==0 ){
      p->i_sz_poslist = 0;
      p->del = 0;
      p->content = 0;
      p->n_data = n_data;
    }
  }
  return n_ret;
}

/*
** Add an entry to the in-memory hash table. The key is the concatenation
** of byte and (token/n_token). The value is (i_rowid/i_col/i_pos).
**
**     (byte || token) -> (i_rowid,i_col,i_pos)
**
** Or, if i_col is negative, then the value is a delete marker.
*/
int __fts5_hash_write(
  fts5_hash_t *hash,
  int64_t i_rowid,                     /* Rowid for this entry */
  int i_col,                       /* column_t token appears in (-ve -> delete) */
  int i_pos,                       /* Position of token within column */
  char byte,                     /* First byte of token */
  const char *token, int n_token  /* Token to add or remove to or from index */
){
  unsigned int i_hash;
  fts5_hash_entry_t *p;
  u_int8_t *ptr;
  int n_incr = 0;                  /* Amount to increment (*hash->pn_byte) by */
  int b_new;                       /* If non-delete entry should be written */
  
  b_new = (hash->detail==FTS5_DETAIL_FULL);

  /* Attempt to locate an existing hash entry */
  i_hash = __fts5_hash_key2(hash->n_slot, (u_int8_t)byte, (const u_int8_t*)token, n_token);
  for(p=hash->slots[i_hash]; p; p=p->hash_next){
    char *key = FTS5_ENTRY_KEY(p);
    if( key[0]==byte 
     && p->n_key==n_token+1
     && memcmp(&key[1], token, n_token)==0 
    ){
      break;
    }
  }

  /* If an existing hash entry cannot be found, create a new one. */
  if( p==0 ){
    /* Figure out how much space to allocate */
    char *key;
    int64_t n_byte = sizeof(fts5_hash_entry_t) + (n_token+1) + 1 + 64;
    if( n_byte<128 ) n_byte = 128;

    /* Grow the fts5_hash_t.slots[] array if necessary. */
    if( (hash->n_entry*2)>=hash->n_slot ){
      int rc = __fts5_hash_resize(hash);
      if( rc!=DBSQL_OK ) return rc;
      i_hash = __fts5_hash_key2(hash->n_slot, (u_int8_t)byte, (const u_int8_t*)token, n_token);
    }

    /* Allocate new fts5_hash_entry_t and add it to the hash table. */
    p = (fts5_hash_entry_t*)dbsql_malloc64(n_byte);
    if( !p ) return DBSQL_NOMEM;
    memset(p, 0, sizeof(fts5_hash_entry_t));
    p->n_alloc = (int)n_byte;
    key = FTS5_ENTRY_KEY(p);
    key[0] = byte;
    memcpy(&key[1], token, n_token);
    assert( i_hash==__fts5_hash_key(hash->n_slot, (u_int8_t*)key, n_token+1) );
    p->n_key = n_token+1;
    key[n_token+1] = '\0';
    p->n_data = n_token+1 + sizeof(fts5_hash_entry_t);
    p->hash_next = hash->slots[i_hash];
    hash->slots[i_hash] = p;
    hash->n_entry++;

    /* Add the first rowid field to the hash-entry */
    p->n_data += __fts5_put_varint(&((u_int8_t*)p)[p->n_data], i_rowid);
    p->i_rowid = i_rowid;

    p->i_sz_poslist = p->n_data;
    if( hash->detail!=FTS5_DETAIL_NONE ){
      p->n_data += 1;
      p->i_col = (hash->detail==FTS5_DETAIL_FULL ? 0 : -1);
    }

  }else{

    /* Appending to an existing hash-entry. Check that there is enough 
    ** space to append the largest possible new entry. Worst case scenario 
    ** is:
    **
    **     + 9 bytes for a new rowid,
    **     + 4 byte reserved for the "poslist size" varint.
    **     + 1 byte for a "new column" byte,
    **     + 3 bytes for a new column number (16-bit max) as a varint,
    **     + 5 bytes for the new position offset (32-bit max).
    */
    if( (p->n_alloc - p->n_data) < (9 + 4 + 1 + 3 + 5) ){
      int64_t n_new = p->n_alloc * 2;
      fts5_hash_entry_t *p_new;
      fts5_hash_entry_t **pp;
      p_new = (fts5_hash_entry_t*)dbsql_realloc64(p, n_new);
      if( p_new==0 ) return DBSQL_NOMEM;
      p_new->n_alloc = (int)n_new;
      for(pp=&hash->slots[i_hash]; *pp!=p; pp=&(*pp)->hash_next);
      *pp = p_new;
      p = p_new;
    }
    n_incr -= p->n_data;
  }
  assert( (p->n_alloc - p->n_data) >= (9 + 4 + 1 + 3 + 5) );

  ptr = (u_int8_t*)p;

  /* If this is a new rowid, append the 4-byte size field for the previous
  ** entry, and the new rowid for this entry.  */
  if( i_rowid!=p->i_rowid ){
    u_int64_t i_diff = (u_int64_t)i_rowid - (u_int64_t)p->i_rowid;
    __fts5_hash_add_poslist_size(hash, p, 0);
    p->n_data += __fts5_put_varint(&ptr[p->n_data], i_diff);
    p->i_rowid = i_rowid;
    b_new = 1;
    p->i_sz_poslist = p->n_data;
    if( hash->detail!=FTS5_DETAIL_NONE ){
      p->n_data += 1;
      p->i_col = (hash->detail==FTS5_DETAIL_FULL ? 0 : -1);
      p->i_pos = 0;
    }
  }

  if( i_col>=0 ){
    if( hash->detail==FTS5_DETAIL_NONE ){
      p->content = 1;
    }else{
      /* Append a new column value, if necessary */
      assert_nc( i_col>=p->i_col );
      if( i_col!=p->i_col ){
        if( hash->detail==FTS5_DETAIL_FULL ){
          ptr[p->n_data++] = 0x01;
          p->n_data += __fts5_put_varint(&ptr[p->n_data], i_col);
          p->i_col = (int16_t)i_col;
          p->i_pos = 0;
        }else{
          b_new = 1;
          p->i_col = (int16_t)(i_pos = i_col);
        }
      }

      /* Append the new position offset, if necessary */
      if( b_new ){
        p->n_data += __fts5_put_varint(&ptr[p->n_data], i_pos - p->i_pos + 2);
        p->i_pos = i_pos;
      }
    }
  }else{
    /* This is a delete. Set the delete flag. */
    p->del = 1;
  }

  n_incr += p->n_data;
  *hash->pn_byte += n_incr;
  return DBSQL_OK;
}


/*
** Arguments left and right point to linked-lists of hash-entry objects,
** each sorted in key order. This function merges the two lists into a
** single list and returns a pointer to its first element.
*/
static fts5_hash_entry_t *__fts5_hash_entry_merge(
  fts5_hash_entry_t *left,
  fts5_hash_entry_t *right
){
  fts5_hash_entry_t *p1 = left;
  fts5_hash_entry_t *p2 = right;
  fts5_hash_entry_t *ret = 0;
  fts5_hash_entry_t **out = &ret;

  while( p1 || p2 ){
    if( p1==0 ){
      *out = p2;
      p2 = 0;
    }else if( p2==0 ){
      *out = p1;
      p1 = 0;
    }else{
      char *key1 = FTS5_ENTRY_KEY(p1);
      char *key2 = FTS5_ENTRY_KEY(p2);
      int n_min = MIN(p1->n_key, p2->n_key);

      int cmp = memcmp(key1, key2, n_min);
      if( cmp==0 ){
        cmp = p1->n_key - p2->n_key;
      }
      assert( cmp!=0 );

      if( cmp>0 ){
        /* p2 is smaller */
        *out = p2;
        out = &p2->scan_next;
        p2 = p2->scan_next;
      }else{
        /* p1 is smaller */
        *out = p1;
        out = &p1->scan_next;
        p1 = p1->scan_next;
      }
      *out = 0;
    }
  }

  return ret;
}

/*
** Link all tokens from hash table i_hash into a list in sorted order. The
** tokens are not removed from the hash table.
*/
static int __fts5_hash_entry_sort(
  fts5_hash_t *hash, 
  const char *term, int n_term,   /* Query prefix, if any */
  fts5_hash_entry_t **sorted
){
  const int n_merge_slot = 32;
  fts5_hash_entry_t **ap;
  fts5_hash_entry_t *list;
  int i_slot;
  int i;

  *sorted = 0;
  ap = dbsql_malloc64(sizeof(fts5_hash_entry_t*) * n_merge_slot);
  if( !ap ) return DBSQL_NOMEM;
  memset(ap, 0, sizeof(fts5_hash_entry_t*) * n_merge_slot);

  for(i_slot=0; i_slot<hash->n_slot; i_slot++){
    fts5_hash_entry_t *iter;
    for(iter=hash->slots[i_slot]; iter; iter=iter->hash_next){
      if( term==0 
       || (iter->n_key>=n_term && 0==memcmp(FTS5_ENTRY_KEY(iter), term, n_term))
      ){
        fts5_hash_entry_t *entry = iter;
        entry->scan_next = 0;
        for(i=0; ap[i]; i++){
          entry = __fts5_hash_entry_merge(entry, ap[i]);
          ap[i] = 0;
        }
        ap[i] = entry;
      }
    }
  }

  list = 0;
  for(i=0; i<n_merge_slot; i++){
    list = __fts5_hash_entry_merge(list, ap[i]);
  }

  dbsql_free(ap);
  *sorted = list;
  return DBSQL_OK;
}

/*
** Query the hash table for a doclist associated with term term/n_term.
*/
int __fts5_hash_query(
  fts5_hash_t *hash,                /* hash_t table to query */
  int n_pre,
  const char *term, int n_term,   /* Query term */
  void **out,                   /* OUT: Pointer to new object */
  int *pn_doclist                  /* OUT: Size of doclist in bytes */
){
  unsigned int i_hash = __fts5_hash_key(hash->n_slot, (const u_int8_t*)term, n_term);
  char *key = 0;
  fts5_hash_entry_t *p;

  for(p=hash->slots[i_hash]; p; p=p->hash_next){
    key = FTS5_ENTRY_KEY(p);
    if( n_term==p->n_key && memcmp(key, term, n_term)==0 ) break;
  }

  if( p ){
    int n_hash_pre = sizeof(fts5_hash_entry_t) + n_term;
    int n_list = p->n_data - n_hash_pre;
    u_int8_t *ret = (u_int8_t*)(*out = dbsql_malloc64(n_pre + n_list + 10));
    if( ret ){
      fts5_hash_entry_t *faux = (fts5_hash_entry_t*)&ret[n_pre-n_hash_pre];
      memcpy(&ret[n_pre], &((u_int8_t*)p)[n_hash_pre], n_list);
      n_list += __fts5_hash_add_poslist_size(hash, p, faux);
      *pn_doclist = n_list;
    }else{
      *pn_doclist = 0;
      return DBSQL_NOMEM;
    }
  }else{
    *out = 0;
    *pn_doclist = 0;
  }

  return DBSQL_OK;
}

int __fts5_hash_scan_init(
  fts5_hash_t *p,                    /* hash_t table to query */
  const char *term, int n_term    /* Query prefix */
){
  return __fts5_hash_entry_sort(p, term, n_term, &p->scan);
}

#ifdef DBSQL_DEBUG
static int __fts5_hash_count(fts5_hash_t *hash){
  int n_entry = 0;
  int ii;
  for(ii=0; ii<hash->n_slot; ii++){
    fts5_hash_entry_t *p = 0;
    for(p=hash->slots[ii]; p; p=p->hash_next){
      n_entry++;
    }
  }
  return n_entry;
}
#endif

/*
** Return true if the hash table is empty, false otherwise.
*/
int __fts5_hash_is_empty(fts5_hash_t *hash){
  assert( hash->n_entry==__fts5_hash_count(hash) );
  return hash->n_entry==0;
}

void __fts5_hash_scan_next(fts5_hash_t *p){
  assert( !__fts5_hash_scan_eof(p) );
  p->scan = p->scan->scan_next;
}

int __fts5_hash_scan_eof(fts5_hash_t *p){
  return (p->scan==0);
}

void __fts5_hash_scan_entry(
  fts5_hash_t *hash,
  const char **term,            /* OUT: term (nul-terminated) */
  int *pn_term,                    /* OUT: Size of term in bytes */
  const u_int8_t **doclist,           /* OUT: pointer to doclist */
  int *pn_doclist                  /* OUT: size of doclist in bytes */
){
  fts5_hash_entry_t *p;
  if( (p = hash->scan) ){
    char *key = FTS5_ENTRY_KEY(p);
    int n_term = p->n_key;
    __fts5_hash_add_poslist_size(hash, p, 0);
    *term = key;
    *pn_term = n_term;
    *doclist = (const u_int8_t*)&key[n_term];
    *pn_doclist = p->n_data - (sizeof(fts5_hash_entry_t) + n_term);
  }else{
    *term = 0;
    *pn_term = 0;
    *doclist = 0;
    *pn_doclist = 0;
  }
}
