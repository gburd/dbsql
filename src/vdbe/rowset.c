/*
** 2008 December 3
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
** This module implements an object we call a "row_set_t".
**
** The row_set_t object is a collection of rowids.  Rowids
** are inserted into the row_set_t in an arbitrary order.  Inserts
** can be intermixed with tests to see if a given rowid has been
** previously inserted into the row_set_t.
**
** After all inserts are finished, it is possible to extract the
** elements of the row_set_t in sorted order.  Once this extraction
** process has started, no new elements may be inserted.
**
** Hence, the primitive operations for a row_set_t are:
**
**    CREATE
**    INSERT
**    TEST
**    SMALLEST
**    DESTROY
**
** The CREATE and DESTROY primitives are the constructor and destructor,
** obviously.  The INSERT primitive adds a new element to the row_set_t.
** TEST checks to see if an element is already in the row_set_t.  SMALLEST
** extracts the least value from the row_set_t.
**
** The INSERT primitive might allocate additional memory.  Memory is
** allocated in chunks so most INSERTs do no allocation.  There is an
** upper bound on the size of allocated memory.  No memory is freed
** until DESTROY.
**
** The TEST primitive includes a "batch" number.  The TEST primitive
** will only see elements that were inserted before the last change
** in the batch number.  In other words, if an INSERT occurs between
** two TESTs where the TESTs have the same batch number, then the
** value added by the INSERT will not be visible to the second TEST.
** The initial batch number is zero, so if the very first TEST contains
** a non-zero batch number, it will see all prior INSERTs.
**
** No INSERTs may occurs after a SMALLEST.  An assertion will fail if
** that is attempted.
**
** The cost of an INSERT is roughly constant.  (Sometimes new memory
** has to be allocated on an INSERT.)  The cost of a TEST with a new
** batch number is O(__nlog_n) where N is the number of elements in the row_set_t.
** The cost of a TEST using the same batch number is O(log_n).  The cost
** of the first SMALLEST is O(__nlog_n).  Second and subsequent SMALLEST
** primitives are constant time.  The cost of DESTROY is O(N).
**
** TEST and SMALLEST may not be used by the same row_set_t.  This used to
** be possible, but the feature was not used, so it was removed in order
** to simplify the code.
*/
#include "dbsql_int.h"


/*
** Target size for allocation chunks.
*/
#define ROWSET_ALLOCATION_SIZE 1024

/*
** The number of rowset entries per allocation chunk.
*/
#define ROWSET_ENTRY_PER_CHUNK  \
                       ((ROWSET_ALLOCATION_SIZE-8)/sizeof(struct row_set_entry_t))

/*
** Each entry in a row_set_t is an instance of the following object.
**
** This same object is reused to store a linked list of trees of row_set_entry_t
** objects.  In that alternative use, right points to the next entry
** in the list, left points to the tree, and v is unused.  The
** row_set_t.forest value points to the head of this forest list.
*/
struct row_set_entry_t {           
  int64_t v;                        /* ROWID value for this entry */
  struct row_set_entry_t *right;   /* Right subtree (larger entries) or list */
  struct row_set_entry_t *left;    /* Left subtree (smaller entries) */
};

/*
** row_set_entry_t objects are allocated in large chunks (instances of the
** following structure) to reduce memory allocation overhead.  The
** chunks are kept on a linked list so that they can be deallocated
** when the row_set_t is destroyed.
*/
struct row_set_chunk_t {
  struct row_set_chunk_t *next_chunk;        /* Next chunk on list of them all */
  struct row_set_entry_t entries[ROWSET_ENTRY_PER_CHUNK]; /* Allocated entries */
};

/*
** A row_set_t in an instance of the following structure.
**
** A typedef of this structure if found in dbsql_int.h.
*/
struct row_set_t {
  struct row_set_chunk_t *chunk;    /* List of all chunk allocations */
  DBSQL *db;                   /* The database connection */
  struct row_set_entry_t *entry;    /* List of entries using right */
  struct row_set_entry_t *last;     /* Last entry on the entry list */
  struct row_set_entry_t *fresh;    /* Source of new entry objects */
  struct row_set_entry_t *forest;   /* List of binary trees of entries */
  u_int16_t n_fresh;                    /* Number of objects on fresh */
  u_int16_t rs_flags;                   /* Various flags */
  int i_batch;                    /* Current insert batch */
};

/*
** Allowed values for row_set_t.rs_flags
*/
#define ROWSET_SORTED  0x01   /* True if row_set_t.entry is sorted */
#define ROWSET_NEXT    0x02   /* True if __row_set_next() has been called */

/*
** Allocate a row_set_t object.  Return NULL if a memory allocation
** error occurs.
*/
row_set_t *__row_set_init(DBSQL *db){
  row_set_t *p = __db_malloc_raw_nn(db, sizeof(*p));
  if( p ){
    int N = __db_malloc_size(db, p);
    p->chunk = 0;
    p->db = db;
    p->entry = 0;
    p->last = 0;
    p->forest = 0;
    p->fresh = (struct row_set_entry_t*)(ROUND8(sizeof(*p)) + (char*)p);
    p->n_fresh = (u_int16_t)((N - ROUND8(sizeof(*p)))/sizeof(struct row_set_entry_t));
    p->rs_flags = ROWSET_SORTED;
    p->i_batch = 0;
  }
  return p;
}

/*
** Deallocate all chunks from a row_set_t.  This frees all memory that
** the row_set_t has allocated over its lifetime.  This routine is
** the destructor for the row_set_t.
*/
void __row_set_clear(void *arg){
  row_set_t *p = (row_set_t*)arg;
  struct row_set_chunk_t *chunk, *next_chunk;
  for(chunk=p->chunk; chunk; chunk = next_chunk){
    next_chunk = chunk->next_chunk;
    __dbsql_db_free(p->db, chunk);
  }
  p->chunk = 0;
  p->n_fresh = 0;
  p->entry = 0;
  p->last = 0;
  p->forest = 0;
  p->rs_flags = ROWSET_SORTED;
}

/*
** Deallocate all chunks from a row_set_t.  This frees all memory that
** the row_set_t has allocated over its lifetime.  This routine is
** the destructor for the row_set_t.
*/
void __row_set_delete(void *arg){
  __row_set_clear(arg);
  __dbsql_db_free(((row_set_t*)arg)->db, arg);
}

/*
** Allocate a new row_set_entry_t object that is associated with the
** given row_set_t.  Return a pointer to the new and completely uninitialized
** object.
**
** In an OOM situation, the row_set_t.db->malloc_failed flag is set and this
** routine returns NULL.
*/
static struct row_set_entry_t *__row_set_entry_alloc(row_set_t *p){
  assert( p!=0 );
  if( p->n_fresh==0 ){  /*OPTIMIZATION-IF-FALSE*/
    /* We could allocate a fresh row_set_entry_t each time one is needed, but it
    ** is more efficient to pull a preallocated entry from the pool */
    struct row_set_chunk_t *p_new;
    p_new = __db_malloc_raw_nn(p->db, sizeof(*p_new));
    if( p_new==0 ){
      return 0;
    }
    p_new->next_chunk = p->chunk;
    p->chunk = p_new;
    p->fresh = p_new->entries;
    p->n_fresh = ROWSET_ENTRY_PER_CHUNK;
  }
  p->n_fresh--;
  return p->fresh++;
}

/*
** Insert a new value into a row_set_t.
**
** The malloc_failed flag of the database connection is set if a
** memory allocation fails.
*/
void __row_set_insert(row_set_t *p, int64_t rowid){
  struct row_set_entry_t *entry;  /* The new entry */
  struct row_set_entry_t *last;   /* The last prior entry */

  /* This routine is never called after __row_set_next() */
  assert( p!=0 && (p->rs_flags & ROWSET_NEXT)==0 );

  entry = __row_set_entry_alloc(p);
  if( entry==0 ) return;
  entry->v = rowid;
  entry->right = 0;
  last = p->last;
  if( last ){
    if( rowid<=last->v ){  /*OPTIMIZATION-IF-FALSE*/
      /* Avoid unnecessary sorts by preserving the ROWSET_SORTED flags
      ** where possible */
      p->rs_flags &= ~ROWSET_SORTED;
    }
    last->right = entry;
  }else{
    p->entry = entry;
  }
  p->last = entry;
}

/*
** Merge two lists of row_set_entry_t objects.  Remove duplicates.
**
** The input lists are connected via right pointers and are
** assumed to each already be in sorted order.
*/
static struct row_set_entry_t *__row_set_entry_merge(
  struct row_set_entry_t *a,    /* First sorted list to be merged */
  struct row_set_entry_t *b     /* Second sorted list to be merged */
){
  struct row_set_entry_t head;
  struct row_set_entry_t *tail;

  tail = &head;
  assert( a!=0 && b!=0 );
  for(;;){
    assert( a->right==0 || a->v<=a->right->v );
    assert( b->right==0 || b->v<=b->right->v );
    if( a->v<=b->v ){
      if( a->v<b->v ) tail = tail->right = a;
      a = a->right;
      if( a==0 ){
        tail->right = b;
        break;
      }
    }else{
      tail = tail->right = b;
      b = b->right;
      if( b==0 ){
        tail->right = a;
        break;
      }
    }
  }
  return head.right;
}

/*
** Sort all elements on the list of row_set_entry_t objects into order of
** increasing v.
*/
static struct row_set_entry_t *__row_set_entry_sort(struct row_set_entry_t *in){
  unsigned int i;
  struct row_set_entry_t *next, *buckets[40];

  memset(buckets, 0, sizeof(buckets));
  while( in ){
    next = in->right;
    in->right = 0;
    for(i=0; buckets[i]; i++){
      in = __row_set_entry_merge(buckets[i], in);
      buckets[i] = 0;
    }
    buckets[i] = in;
    in = next;
  }
  in = buckets[0];
  for(i=1; i<sizeof(buckets)/sizeof(buckets[0]); i++){
    if( buckets[i]==0 ) continue;
    in = in ? __row_set_entry_merge(in, buckets[i]) : buckets[i];
  }
  return in;
}


/*
** The input, in, is a binary tree (or subtree) of row_set_entry_t objects.
** Convert this tree into a linked list connected by the right pointers
** and return pointers to the first and last elements of the new list.
*/
static void __row_set_tree_to_list(
  struct row_set_entry_t *in,         /* Root of the input tree */
  struct row_set_entry_t **first,    /* Write head of the output list here */
  struct row_set_entry_t **last      /* Write tail of the output list here */
){
  assert( in!=0 );
  if( in->left ){
    struct row_set_entry_t *p;
    __row_set_tree_to_list(in->left, first, &p);
    p->right = in;
  }else{
    *first = in;
  }
  if( in->right ){
    __row_set_tree_to_list(in->right, &in->right, last);
  }else{
    *last = in;
  }
  assert( (*last)->right==0 );
}


/*
** Convert a sorted list of elements (connected by right) into a binary
** tree with depth of i_depth.  A depth of 1 means the tree contains a single
** node taken from the head of *list.  A depth of 2 means a tree with
** three nodes.  And so forth.
**
** Use as many entries from the input list as required and update the
** *list to point to the unused elements of the list.  If the input
** list contains too few elements, then construct an incomplete tree
** and leave *list set to NULL.
**
** Return a pointer to the root of the constructed binary tree.
*/
static struct row_set_entry_t *__row_set_n_deep_tree(
  struct row_set_entry_t **list,
  int i_depth
){
  struct row_set_entry_t *p;         /* Root of the new tree */
  struct row_set_entry_t *left;     /* Left subtree */
  if( *list==0 ){ /*OPTIMIZATION-IF-TRUE*/
    /* Prevent unnecessary deep recursion when we run out of entries */
    return 0;
  }
  if( i_depth>1 ){   /*OPTIMIZATION-IF-TRUE*/
    /* This branch causes a *balanced* tree to be generated.  A valid tree
    ** is still generated without this branch, but the tree is wildly
    ** unbalanced and inefficient. */
    left = __row_set_n_deep_tree(list, i_depth-1);
    p = *list;
    if( p==0 ){     /*OPTIMIZATION-IF-FALSE*/
      /* It is safe to always return here, but the resulting tree
      ** would be unbalanced */
      return left;
    }
    p->left = left;
    *list = p->right;
    p->right = __row_set_n_deep_tree(list, i_depth-1);
  }else{
    p = *list;
    *list = p->right;
    p->left = p->right = 0;
  }
  return p;
}

/*
** Convert a sorted list of elements into a binary tree. Make the tree
** as deep as it needs to be in order to contain the entire list.
*/
static struct row_set_entry_t *__row_set_list_to_tree(struct row_set_entry_t *list){
  int i_depth;           /* Depth of the tree so far */
  struct row_set_entry_t *p;       /* Current tree root */
  struct row_set_entry_t *left;   /* Left subtree */

  assert( list!=0 );
  p = list;
  list = p->right;
  p->left = p->right = 0;
  for(i_depth=1; list; i_depth++){
    left = p;
    p = list;
    list = p->right;
    p->left = left;
    p->right = __row_set_n_deep_tree(&list, i_depth);
  }
  return p;
}

/*
** Extract the smallest element from the row_set_t.
** Write the element into *rowid.  Return 1 on success.  Return
** 0 if the row_set_t is already empty.
**
** After this routine has been called, the __row_set_insert()
** routine may not be called again.
**
** This routine may not be called after __row_set_test() has
** been used.  Older versions of row_set_t allowed that, but as the
** capability was not used by the code generator, it was removed
** for code economy.
*/
int __row_set_next(row_set_t *p, int64_t *rowid){
  assert( p!=0 );
  assert( p->forest==0 );  /* Cannot be used with __row_set_text() */

  /* Merge the forest into a single sorted list on first call */
  if( (p->rs_flags & ROWSET_NEXT)==0 ){  /*OPTIMIZATION-IF-FALSE*/
    if( (p->rs_flags & ROWSET_SORTED)==0 ){  /*OPTIMIZATION-IF-FALSE*/
      p->entry = __row_set_entry_sort(p->entry);
    }
    p->rs_flags |= ROWSET_SORTED|ROWSET_NEXT;
  }

  /* Return the next entry on the list */
  if( p->entry ){
    *rowid = p->entry->v;
    p->entry = p->entry->right;
    if( p->entry==0 ){ /*OPTIMIZATION-IF-TRUE*/
      /* Free memory immediately, rather than waiting on dbsql_finalize() */
      __row_set_clear(p);
    }
    return 1;
  }else{
    return 0;
  }
}

/*
** Check to see if element i_rowid was inserted into the rowset as
** part of any insert batch prior to i_batch.  Return 1 or 0.
**
** If this is the first test of a new batch and if there exist entries
** on row_set->entry, then sort those entries into the forest at
** row_set->forest so that they can be tested.
*/
int __row_set_test(row_set_t *row_set, int i_batch, int64_t i_rowid){
  struct row_set_entry_t *p, *tree;

  /* This routine is never called after __row_set_next() */
  assert( row_set!=0 && (row_set->rs_flags & ROWSET_NEXT)==0 );

  /* Sort entries into the forest on the first test of a new batch.
  ** To save unnecessary work, only do this when the batch number changes.
  */
  if( i_batch!=row_set->i_batch ){  /*OPTIMIZATION-IF-FALSE*/
    p = row_set->entry;
    if( p ){
      struct row_set_entry_t **prev_tree = &row_set->forest;
      if( (row_set->rs_flags & ROWSET_SORTED)==0 ){ /*OPTIMIZATION-IF-FALSE*/
        /* Only sort the current set of entries if they need it */
        p = __row_set_entry_sort(p);
      }
      for(tree = row_set->forest; tree; tree=tree->right){
        prev_tree = &tree->right;
        if( tree->left==0 ){
          tree->left = __row_set_list_to_tree(p);
          break;
        }else{
          struct row_set_entry_t *aux, *tail;
          __row_set_tree_to_list(tree->left, &aux, &tail);
          tree->left = 0;
          p = __row_set_entry_merge(aux, p);
        }
      }
      if( tree==0 ){
        *prev_tree = tree = __row_set_entry_alloc(row_set);
        if( tree ){
          tree->v = 0;
          tree->right = 0;
          tree->left = __row_set_list_to_tree(p);
        }
      }
      row_set->entry = 0;
      row_set->last = 0;
      row_set->rs_flags |= ROWSET_SORTED;
    }
    row_set->i_batch = i_batch;
  }

  /* Test to see if the i_rowid value appears anywhere in the forest.
  ** Return 1 if it does and 0 if not.
  */
  for(tree = row_set->forest; tree; tree=tree->right){
    p = tree->left;
    while( p ){
      if( p->v<i_rowid ){
        p = p->right;
      }else if( p->v>i_rowid ){
        p = p->left;
      }else{
        return 1;
      }
    }
  }
  return 0;
}
