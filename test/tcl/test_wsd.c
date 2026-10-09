/*
** 2008 September 1
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
** The code in this file contains sample implementations of the 
** dbsql_wsd_init() and dbsql_wsd_find() functions required if the
** DBSQL_OMIT_WSD symbol is defined at build time.
*/

#if defined(DBSQL_OMIT_WSD) && defined(DBSQL_TEST)

#include "dbsql_int.h"

#define PLS_HASHSIZE 43

typedef struct process_local_storage_t process_local_storage_t;
typedef struct process_local_var_t process_local_var_t;

struct process_local_storage_t {
  process_local_var_t *data[PLS_HASHSIZE];
  int n_free;
  u_int8_t *p_free;
};

struct process_local_var_t {
  void *key;
  process_local_var_t *next;
};

static process_local_storage_t *__p_global = 0;

int dbsql_wsd_init(int N, int J){
  if( !__p_global ){
    int n_malloc = N + sizeof(process_local_storage_t) + J*sizeof(process_local_var_t);
    __p_global = (process_local_storage_t *)malloc(n_malloc);
    if( __p_global ){
      memset(__p_global, 0, sizeof(process_local_storage_t));
      __p_global->n_free = n_malloc - sizeof(process_local_storage_t);
      __p_global->p_free = (u_int8_t *)&__p_global[1];
    }
  }

  return __p_global ? DBSQL_OK : DBSQL_NOMEM;
}

void *dbsql_wsd_find(void *K, int L){
  int i;
  int i_hash = 0;
  process_local_var_t *var;

  /* Calculate a hash of K */
  for(i=0; i<sizeof(void*); i++){
    i_hash = (i_hash<<3) + ((unsigned char *)&K)[i];
  }
  i_hash = i_hash%PLS_HASHSIZE;

  /* Search the hash table for K. */
  for(var=__p_global->data[i_hash]; var && var->key!=K; var=var->next);

  /* If no entry for K was found, create and populate a new one. */
  if( !var ){
    int n_byte = ROUND8(sizeof(process_local_var_t) + L);
    assert( __p_global->n_free>=n_byte );
    var = (process_local_var_t *)__p_global->p_free;
    var->key = K;
    var->next = __p_global->data[i_hash];
    __p_global->data[i_hash] = var;
    __p_global->n_free -= n_byte;
    __p_global->p_free += n_byte;
    memcpy(&var[1], K, L);
  }

  return (void *)&var[1];
}

#endif
