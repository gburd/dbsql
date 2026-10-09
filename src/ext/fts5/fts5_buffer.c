/*
** 2014 May 31
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
*/



#include "fts5_int.h"

int __fts5_buffer_size(int *rc, fts5_buffer_t *buf, u_int32_t n_byte){
  if( (u_int32_t)buf->n_space<n_byte ){
    u_int64_t n_new = buf->n_space ? buf->n_space : 64;
    u_int8_t *p_new;
    while( n_new<n_byte ){
      n_new = n_new * 2;
    }
    p_new = dbsql_realloc64(buf->p, n_new);
    if( p_new==0 ){
      *rc = DBSQL_NOMEM;
      return 1;
    }else{
      buf->n_space = (int)n_new;
      buf->p = p_new;
    }
  }
  return 0;
}


/*
** Encode value i_val as an dbsql varint and append it to the buffer object
** buf. If an OOM error occurs, set the error code in p.
*/
void __fts5_buffer_append_varint(int *rc, fts5_buffer_t *buf, int64_t i_val){
  if( FTS5_BUFFER_GROW(rc, buf, 9) ) return;
  buf->n += __fts5_put_varint(&buf->p[buf->n], i_val);
}

void __fts5_put32(u_int8_t *bufs, int i_val){
  bufs[0] = (i_val>>24) & 0x00FF;
  bufs[1] = (i_val>>16) & 0x00FF;
  bufs[2] = (i_val>> 8) & 0x00FF;
  bufs[3] = (i_val>> 0) & 0x00FF;
}

int __fts5_get32(const u_int8_t *bufs){
  return (int)((((u_int32_t)bufs[0])<<24) + (bufs[1]<<16) + (bufs[2]<<8) + bufs[3]);
}

/*
** Append buffer n_data/data to buffer buf. If an OOM error occurs, set 
** the error code in p. If an error has already occurred when this function
** is called, it is a no-op.
*/
void __fts5_buffer_append_blob(
  int *rc,
  fts5_buffer_t *buf, 
  u_int32_t n_data, 
  const u_int8_t *data
){
  if( n_data ){
    if( FTS5_BUFFER_GROW(rc, buf, n_data) ) return;
    assert( buf->p!=0 );
    memcpy(&buf->p[buf->n], data, n_data);
    buf->n += n_data;
  }
}

/*
** Append the nul-terminated string str to the buffer buf. This function
** ensures that the byte following the buffer data is set to 0x00, even 
** though this byte is not included in the buf->n count.
*/
void __fts5_buffer_append_string(
  int *rc,
  fts5_buffer_t *buf, 
  const char *str
){
  int n_str = (int)strlen(str);
  __fts5_buffer_append_blob(rc, buf, n_str+1, (const u_int8_t*)str);
  buf->n--;
}

/*
** Argument fmt is a printf() style format string. This function performs
** the printf() style processing, then appends the results to buffer buf.
**
** Like __fts5_buffer_append_string(), this function ensures that the byte 
** following the buffer data is set to 0x00, even though this byte is not
** included in the buf->n count.
*/ 
void __fts5_buffer_append_printf(
  int *rc,
  fts5_buffer_t *buf, 
  char *fmt, ...
){
  if( *rc==DBSQL_OK ){
    char *tmp;
    va_list ap;
    va_start(ap, fmt);
    tmp = dbsql_vmprintf(fmt, ap);
    va_end(ap);

    if( tmp==0 ){
      *rc = DBSQL_NOMEM;
    }else{
      __fts5_buffer_append_string(rc, buf, tmp);
      dbsql_free(tmp);
    }
  }
}

char *__fts5_mprintf(int *rc, const char *fmt, ...){
  char *ret = 0;
  if( *rc==DBSQL_OK ){
    va_list ap;
    va_start(ap, fmt);
    ret = dbsql_vmprintf(fmt, ap);
    va_end(ap);
    if( ret==0 ){
      *rc = DBSQL_NOMEM; 
    }
  }
  return ret;
}
 

/*
** Free any buffer allocated by buf. Zero the structure before returning.
*/
void __fts5_buffer_free(fts5_buffer_t *buf){
  dbsql_free(buf->p);
  memset(buf, 0, sizeof(fts5_buffer_t));
}

/*
** Zero the contents of the buffer object. But do not free the associated 
** memory allocation.
*/
void __fts5_buffer_zero(fts5_buffer_t *buf){
  buf->n = 0;
}

/*
** Set the buffer to contain n_data/data. If an OOM error occurs, leave an
** the error code in p. If an error has already occurred when this function
** is called, it is a no-op.
*/
void __fts5_buffer_set(
  int *rc,
  fts5_buffer_t *buf, 
  int n_data, 
  const u_int8_t *data
){
  buf->n = 0;
  __fts5_buffer_append_blob(rc, buf, n_data, data);
}

int __fts5_poslist_next64(
  const u_int8_t *a, int n,             /* Buffer containing poslist */
  int *pi,                        /* IN/OUT: Offset within a[] */
  int64_t *pi_off                      /* IN/OUT: Current offset */
){
  int i = *pi;
  assert( a!=0 || i==0 );
  if( i>=n ){
    /* EOF */
    *pi_off = -1;
    return 1;  
  }else{
    int64_t i_off = *pi_off;
    u_int32_t i_val;
    assert( a!=0 );
    FTS5_FAST_GET_VARINT32(a, i, i_val);
    if( i_val<=1 ){
      if( i_val==0 ){
        *pi = i;
        return 0;
      }
      FTS5_FAST_GET_VARINT32(a, i, i_val);
      i_off = ((int64_t)i_val) << 32;
      assert( i_off>=0 );
      FTS5_FAST_GET_VARINT32(a, i, i_val);
      if( i_val<2 ){
        /* This is a corrupt record. So stop parsing it here. */
        *pi_off = -1;
        return 1;
      }
      *pi_off = i_off + ((i_val-2) & 0x7FFFFFFF);
    }else{
      *pi_off = (i_off & (int64_t)0x7FFFFFFF<<32)+((i_off + (i_val-2)) & 0x7FFFFFFF);
    }
    *pi = i;
    assert_nc( *pi_off>=i_off );
    return 0;
  }
}


/*
** Advance the iterator object passed as the only argument. Return true
** if the iterator reaches EOF, or false otherwise.
*/
int __fts5_poslist_reader_next(fts5_poslist_reader_t *iter){
  if( __fts5_poslist_next64(iter->a, iter->n, &iter->i, &iter->i_pos) ){
    iter->eof = 1;
  }
  return iter->eof;
}

int __fts5_poslist_reader_init(
  const u_int8_t *a, int n,             /* Poslist buffer to iterate through */
  fts5_poslist_reader_t *iter        /* Iterator object to initialize */
){
  memset(iter, 0, sizeof(*iter));
  iter->a = a;
  iter->n = n;
  __fts5_poslist_reader_next(iter);
  return iter->eof;
}

/*
** Append position i_pos to the position list being accumulated in buffer
** buf, which must be already be large enough to hold the new data.
** The previous position written to this list is *pi_prev. *pi_prev is set
** to i_pos before returning.
*/
void __fts5_poslist_safe_append(
  fts5_buffer_t *buf, 
  int64_t *pi_prev, 
  int64_t i_pos
){
  if( i_pos>=*pi_prev ){
    static const int64_t colmask = ((int64_t)(0x7FFFFFFF)) << 32;
    if( (i_pos & colmask) != (*pi_prev & colmask) ){
      buf->p[buf->n++] = 1;
      buf->n += __fts5_put_varint(&buf->p[buf->n], (i_pos>>32));
      *pi_prev = (i_pos & colmask);
    }
    buf->n += __fts5_put_varint(&buf->p[buf->n], (i_pos-*pi_prev)+2);
    *pi_prev = i_pos;
  }
}

int __fts5_poslist_writer_append(
  fts5_buffer_t *buf, 
  fts5_poslist_writer_t *writer,
  int64_t i_pos
){
  int rc = 0;   /* Initialized only to suppress erroneous warning from Clang */
  if( FTS5_BUFFER_GROW(&rc, buf, 5+5+5) ) return rc;
  __fts5_poslist_safe_append(buf, &writer->i_prev, i_pos);
  return DBSQL_OK;
}

void *__fts5_malloc_zero(int *rc, int64_t n_byte){
  void *ret = 0;
  if( *rc==DBSQL_OK ){
    ret = dbsql_malloc64(n_byte);
    if( ret==0 ){
      if( n_byte>0 ) *rc = DBSQL_NOMEM;
    }else{
      memset(ret, 0, (size_t)n_byte);
    }
  }
  return ret;
}

/*
** Return a nul-terminated copy of the string indicated by in. If n_in
** is non-negative, then it is the length of the string in bytes. Otherwise,
** the length of the string is determined using strlen().
**
** It is the responsibility of the caller to eventually free the returned
** buffer using dbsql_free(). If an OOM error occurs, NULL is returned. 
*/
char *__fts5_strndup(int *rc, const char *in, int n_in){
  char *ret = 0;
  if( *rc==DBSQL_OK ){
    if( n_in<0 ){
      n_in = (int)strlen(in);
    }
    ret = (char*)dbsql_malloc64((int64_t)n_in+1);
    if( ret ){
      memcpy(ret, in, n_in);
      ret[n_in] = '\0';
    }else{
      *rc = DBSQL_NOMEM;
    }
  }
  return ret;
}


/*
** Return true if character 't' may be part of an FTS5 bareword, or false
** otherwise. Characters that may be part of barewords:
**
**   * All non-ASCII characters,
**   * The 52 upper and lower case ASCII characters, and
**   * The 10 integer ASCII characters.
**   * The underscore character "_" (0x5F).
**   * The unicode "substitute" character (0x1A).
*/
int __fts5_is_bareword(char t){
  u_int8_t barewords[128] = {
    0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0, 0, 0, 0, 0, 0,   /* 0x00 .. 0x0F */
    0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 1, 0, 0, 0, 0, 0,   /* 0x10 .. 0x1F */
    0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0, 0, 0, 0, 0, 0,   /* 0x20 .. 0x2F */
    1, 1, 1, 1, 1, 1, 1, 1,    1, 1, 0, 0, 0, 0, 0, 0,   /* 0x30 .. 0x3F */
    0, 1, 1, 1, 1, 1, 1, 1,    1, 1, 1, 1, 1, 1, 1, 1,   /* 0x40 .. 0x4F */
    1, 1, 1, 1, 1, 1, 1, 1,    1, 1, 1, 0, 0, 0, 0, 1,   /* 0x50 .. 0x5F */
    0, 1, 1, 1, 1, 1, 1, 1,    1, 1, 1, 1, 1, 1, 1, 1,   /* 0x60 .. 0x6F */
    1, 1, 1, 1, 1, 1, 1, 1,    1, 1, 1, 0, 0, 0, 0, 0    /* 0x70 .. 0x7F */
  };

  return (t & 0x80) || barewords[(int)t];
}


/*************************************************************************
*/
typedef struct fts5_termset_entry_t fts5_termset_entry_t;
struct fts5_termset_entry_t {
  char *term;
  int n_term;
  int i_idx;                       /* index_t (main or __a_prefix[] entry) */
  fts5_termset_entry_t *next;
};

struct fts5_termset_t {
  fts5_termset_entry_t *hashes[512];
};

int __fts5_termset_new(fts5_termset_t **pp){
  int rc = DBSQL_OK;
  *pp = __fts5_malloc_zero(&rc, sizeof(fts5_termset_t));
  return rc;
}

int __fts5_termset_add(
  fts5_termset_t *p, 
  int i_idx,
  const char *term, int n_term, 
  int *pb_present
){
  int rc = DBSQL_OK;
  *pb_present = 0;
  if( p ){
    int i;
    u_int32_t hash = 13;
    fts5_termset_entry_t *entry;

    /* Calculate a hash value for this term. This is the same hash checksum
    ** used by the fts5_hash.c module. This is not important for correct
    ** operation of the module, but is necessary to ensure that some tests
    ** designed to produce hash table collisions really do work.  */
    for(i=n_term-1; i>=0; i--){
      hash = (hash << 3) ^ hash ^ term[i];
    }
    hash = (hash << 3) ^ hash ^ i_idx;
    hash = hash % ARRAY_SIZE(p->hashes);

    for(entry=p->hashes[hash]; entry; entry=entry->next){
      if( entry->i_idx==i_idx 
          && entry->n_term==n_term 
          && memcmp(entry->term, term, n_term)==0 
      ){
        *pb_present = 1;
        break;
      }
    }

    if( entry==0 ){
      entry = __fts5_malloc_zero(&rc, sizeof(fts5_termset_entry_t) + n_term);
      if( entry ){
        entry->term = (char*)&entry[1];
        entry->n_term = n_term;
        entry->i_idx = i_idx;
        memcpy(entry->term, term, n_term);
        entry->next = p->hashes[hash];
        p->hashes[hash] = entry;
      }
    }
  }

  return rc;
}

void __fts5_termset_free(fts5_termset_t *p){
  if( p ){
    u_int32_t i;
    for(i=0; i<ARRAY_SIZE(p->hashes); i++){
      fts5_termset_entry_t *entry = p->hashes[i];
      while( entry ){
        fts5_termset_entry_t *del = entry;
        entry = entry->next;
        dbsql_free(del);
      }
    }
    dbsql_free(p);
  }
}
