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
** This is the header file for the generic hash-table implementation
** used in dbsql.
*/
#ifndef DBSQL_HASH_H
#define DBSQL_HASH_H

/* Forward declarations of structures. */
typedef struct hash_t hash_t;
typedef struct hash_elem_t hash_elem_t;

/* A complete hash table is an instance of the following structure.
** The internals of this structure are intended to be opaque -- client
** code should not attempt to access or modify the fields of this structure
** directly.  Change this structure only by using the routines below.
** However, some of the "procedures" and "functions" for modifying and
** accessing this structure are really macros, so we can't really make
** this structure opaque.
**
** All elements of the hash table are on a single doubly-linked list.
** hash_t.first points to the head of this list.
**
** There are hash_t.htsize buckets.  Each bucket points to a spot in
** the global doubly-linked list.  The contents of the bucket are the
** element pointed to plus the next _ht.count-1 elements in the list.
**
** hash_t.htsize and hash_t.ht may be zero.  In that case lookup is done
** by a linear search of the global list.  For small tables, the 
** hash_t.ht table is never allocated because if there are few elements
** in the table, it is faster to do a linear search than to manage
** the hash table.
*/
struct hash_t {
  unsigned int htsize;      /* Number of buckets in the hash table */
  unsigned int count;       /* Number of entries in this table */
  hash_elem_t *first;          /* The first element of the array */
  struct _ht {              /* the hash table */
    unsigned int count;        /* Number of entries with this hash */
    hash_elem_t *chain;           /* Pointer to first entry with this hash */
  } *ht;
};

/* Each element in the hash table is an instance of the following 
** structure.  All elements are stored on a single doubly-linked list.
**
** Again, this structure is intended to be opaque, but it can't really
** be opaque because it is used by macros.
*/
struct hash_elem_t {
  hash_elem_t *next, *prev;       /* Next and previous elements in the table */
  void *data;                  /* Data associated with this element */
  const char *key;            /* Key associated with this element */
  unsigned int h;              /* hash for key */
};

/*
** Access routines.  To delete, insert a NULL pointer.
*/
void __hash_init(hash_t*);
void *__hash_insert(hash_t*, const char *key, void *data);
void *__hash_find(const hash_t*, const char *key);
void __hash_clear(hash_t*);

/*
** Macros for looping over all elements of a hash table.  The idiom is
** like this:
**
**   hash_t h;
**   hash_elem_t *p;
**   ...
**   for(p=dbsql_hash_first(&h); p; p=dbsql_hash_next(p)){
**     __some_structure *data = dbsql_hash_data(p);
**     // do something with data
**   }
*/
#define dbsql_hash_first(H)  ((H)->first)
#define dbsql_hash_next(E)   ((E)->next)
#define dbsql_hash_data(E)   ((E)->data)
/* #define dbsql_hash_key(E)    ((E)->key) // NOT USED */
/* #define dbsql_hash_keysize(E) ((E)->n_key)  // NOT USED */

/*
** Number of entries in a hash table
*/
#define dbsql_hash_count(H)  ((H)->count)

#endif /* DBSQL_HASH_H */
