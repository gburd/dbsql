/*
** 2013-04-16
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
** WARNING:  Experimental and obsolete.  Demonstration and testing only.
**
** This virtual table was created prior to the addition of support for
** common table expressions in dbsql.  Common table expressions are a
** better and more portable solution to any problem that this virtual
** table solves.
**
** Given its experimental and testing-only status, the code here is
** deactivated unless compiled with -DSQLITE_TEST=1
**
** DEMONSTRATION AND TESTING USE ONLY.
**
** This file contains code for a virtual table that finds the transitive
** closure of a parent/child relationship in a real table.  The virtual 
** table is called "transitive_closure".
**
** A transitive_closure virtual table is created like this:
**
**     CREATE VIRTUAL TABLE x USING transitive_closure(
**        tablename=<tablename>,      -- T
**        idcolumn=<columnname>,      -- X
**        parentcolumn=<columnname>   -- P
**     );
**
** When it is created, the new transitive_closure table may be supplied 
** with default values for the name of a table T and columns T.X and T.P.
** The T.X and T.P columns must contain integers.  The ideal case is for 
** T.X to be the INTEGER PRIMARY KEY.  The T.P column should reference
** the T.X column. The row referenced by T.P is the parent of the current row.
**
** The tablename, idcolumn, and parentcolumn supplied by the CREATE VIRTUAL
** TABLE statement may be overridden in individual queries by including
** terms like tablename='newtable', idcolumn='id2', or 
** parentcolumn='parent3' in the WHERE clause of the query.
**
** For efficiency, it is essential that there be an index on the P column:
**
**    CREATE Tidx1 ON T(P)
**
** Suppose a specific instance of the closure table is as follows:
**
**    CREATE VIRTUAL TABLE ct1 USING transitive_closure(
**       tablename='group',
**       idcolumn='group_id',
**       parentcolumn='parent_id'
**    );
**
** Such an instance of the transitive_closure virtual table would be
** appropriate for walking a tree defined using a table like this, for example:
**
**    CREATE TABLE group(
**      group_id INTEGER PRIMARY KEY,
**      parent_id INTEGER REFERENCES group
**    );
**    CREATE INDEX group_idx1 ON group(parent_id);
**
** The group table above would presumably have other application-specific
** fields.  The key point here is that rows of the group table form a
** tree.  The purpose of the ct1 virtual table is to easily extract
** branches of that tree.
**
** Once it has been created, the ct1 virtual table can be queried
** as follows:
**
**    SELECT * FROM element
**     WHERE element.group_id IN (SELECT id FROM ct1 WHERE root=?1);
**
** The above query will return all elements that are part of group ?1
** or children of group ?1 or grand-children of ?1 and so forth for all
** descendents of group ?1.  The same query can be formulated as a join:
**
**    SELECT element.* FROM element, ct1
**     WHERE element.groupid=ct1.id
**       AND ct1.root=?1;
**
** The depth of the transitive_closure (the number of generations of
** parent/child relations to follow) can be limited by setting "depth"
** column in the WHERE clause.  So, for example, the following query
** finds only children and grandchildren but no further descendents:
**
**    SELECT element.* FROM element, ct1
**     WHERE element.groupid=ct1.id
**       AND ct1.root=?1
**       AND ct1.depth<=2;
**
** The "ct1.depth<=2" term could be a strict equality "ct1.depth=2" in
** order to find only the grandchildren of ?1, not ?1 itself or the
** children of ?1.
** 
** The root=?1 term must be supplied in WHERE clause or else the query
** of the ct1 virtual table will return an empty set.  The tablename,
** idcolumn, and parentcolumn attributes can be overridden in the WHERE
** clause if desired.  So, for example, the ct1 table could be repurposed
** to find ancestors rather than descendents by inverting the roles of
** the idcolumn and parentcolumn:
**
**    SELECT element.* FROM element, ct1
**     WHERE element.groupid=ct1.id
**       AND ct1.root=?1
**       AND ct1.idcolumn='parent_id'
**       AND ct1.parentcolumn='group_id';
**
** Multiple calls to ct1 could be combined.  For example, the following
** query finds all elements that "cousins" of group_id ?1.  That is to say
** elements where the group_id is a grandchild of the grandparent of ?1.
** (This definition of "cousins" also includes siblings and self.)
**
**    SELECT element.* FROM element, ct1
**     WHERE element.group_id=ct1.id
**       AND ct1.depth=2
**       AND ct1.root IN (SELECT id FROM ct1
**                         WHERE root=?1
**                           AND depth=2
**                           AND idcolumn='parent_id'
**                           AND parentcolumn='group_id');
**
** In our example, the group.group_id column is unique and thus the
** subquery will return exactly one row.  For that reason, the IN
** operator could be replaced by "=" to get the same result.  But
** in the general case where the idcolumn is not unique, an IN operator
** would be required for this kind of query.
**
** Note that because the tablename, idcolumn, and parentcolumn can
** all be specified in the query, it is possible for an application
** to define a single transitive_closure virtual table for use on lots
** of different hierarchy tables.  One might say:
**
**     CREATE VIRTUAL TABLE temp.closure USING transitive_closure;
**
** As each database connection is being opened.  Then the application
** would always have a "closure" virtual table handy to use for querying.
**
**    SELECT element.* FROM element, closure
**     WHERE element.groupid=ct1.id
**       AND closure.root=?1
**       AND closure.tablename='group'
**       AND closure.idname='group_id'
**       AND closure.parentname='parent_id';
**
** See the documentation at http://sqlite.org/loadext.html for information
** on how to compile and use loadable extensions such as this one.
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <ctype.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** Forward declaration of objects used by this implementation
*/
typedef struct closure_vtab closure_vtab;
typedef struct closure_cursor closure_cursor;
typedef struct closure_queue closure_queue;
typedef struct closure_avl closure_avl;

/*****************************************************************************
** AVL Tree implementation
*/
/*
** Objects that want to be members of the AVL tree should embedded an
** instance of this structure.
*/
struct closure_avl {
  int64_t id;     /* Id of this entry in the table */
  int i_generation;      /* Which generation is this entry part of */
  closure_avl *list;   /* A linked list of nodes */
  closure_avl *before; /* Other elements less than id */
  closure_avl *after;  /* Other elements greater than id */
  closure_avl *up;     /* Parent element */
  short int height;     /* Height of this node.  Leaf==1 */
  short int imbalance;  /* Height difference between before and after */
};

/* Recompute the closure_avl.height and closure_avl.imbalance fields for p.
** Assume that the children of p have correct heights.
*/
static void __closure_avl_recompute_height(closure_avl *p){
  short int h_before = p->before ? p->before->height : 0;
  short int h_after = p->after ? p->after->height : 0;
  p->imbalance = h_before - h_after;  /* -: after higher.  +: before higher */
  p->height = (h_before>h_after ? h_before : h_after)+1;
}

/*
**     P                B
**    / \              / \
**   B   Z    ==>     X   P
**  / \                  / \
** X   Y                Y   Z
**
*/
static closure_avl *__closure_avl_rotate_before(closure_avl *p){
  closure_avl *b = p->before;
  closure_avl *y = b->after;
  b->up = p->up;
  b->after = p;
  p->up = b;
  p->before = y;
  if( y ) y->up = p;
  __closure_avl_recompute_height(p);
  __closure_avl_recompute_height(b);
  return b;
}

/*
**     P                A
**    / \              / \
**   X   A    ==>     P   Z
**      / \          / \
**     Y   Z        X   Y
**
*/
static closure_avl *__closure_avl_rotate_after(closure_avl *p){
  closure_avl *a = p->after;
  closure_avl *y = a->before;
  a->up = p->up;
  a->before = p;
  p->up = a;
  p->after = y;
  if( y ) y->up = p;
  __closure_avl_recompute_height(p);
  __closure_avl_recompute_height(a);
  return a;
}

/*
** Return a pointer to the before or after pointer in the parent
** of p that points to p.  Or if p is the root node, return pp.
*/
static closure_avl **__closure_avl_from_ptr(closure_avl *p, closure_avl **pp){
  closure_avl *up = p->up;
  if( up==0 ) return pp;
  if( up->after==p ) return &up->after;
  return &up->before;
}

/*
** Rebalance all nodes starting with p and working up to the root.
** Return the new root.
*/
static closure_avl *__closure_avl_balance(closure_avl *p){
  closure_avl *top = p;
  closure_avl **pp;
  while( p ){
    __closure_avl_recompute_height(p);
    if( p->imbalance>=2 ){
      closure_avl *b = p->before;
      if( b->imbalance<0 ) p->before = __closure_avl_rotate_after(b);
      pp = __closure_avl_from_ptr(p,&p);
      p = *pp = __closure_avl_rotate_before(p);
    }else if( p->imbalance<=(-2) ){
      closure_avl *a = p->after;
      if( a->imbalance>0 ) p->after = __closure_avl_rotate_before(a);
      pp = __closure_avl_from_ptr(p,&p);
      p = *pp = __closure_avl_rotate_after(p);
    }
    top = p;
    p = p->up;
  }
  return top;
}

/* Search the tree rooted at p for an entry with id.  Return a pointer
** to the entry or return NULL.
*/
static closure_avl *__closure_avl_search(closure_avl *p, int64_t id){
  while( p && id!=p->id ){
    p = (id<p->id) ? p->before : p->after;
  }
  return p;
}

/* Find the first node (the one with the smallest key).
*/
static closure_avl *__closure_avl_first(closure_avl *p){
  if( p ) while( p->before ) p = p->before;
  return p;
}

/* Return the node with the next larger key after p.
*/
closure_avl *__closure_avl_next(closure_avl *p){
  closure_avl *prev = 0;
  while( p && p->after==prev ){
    prev = p;
    p = p->up;
  }
  if( p && prev==0 ){
    p = __closure_avl_first(p->after);
  }
  return p;
}

/* Insert a new node p_new.  Return NULL on success.  If the key is not
** unique, then do not perform the insert but instead leave p_new unchanged
** and return a pointer to an existing node with the same key.
*/
static closure_avl *__closure_avl_insert(
  closure_avl **head,  /* Head of the tree */
  closure_avl *p_new      /* New node to be inserted */
){
  closure_avl *p = *head;
  if( p==0 ){
    p = p_new;
    p_new->up = 0;
  }else{
    while( p ){
      if( p_new->id<p->id ){
        if( p->before ){
          p = p->before;
        }else{
          p->before = p_new;
          p_new->up = p;
          break;
        }
      }else if( p_new->id>p->id ){
        if( p->after ){
          p = p->after;
        }else{
          p->after = p_new;
          p_new->up = p;
          break;
        }
      }else{
        return p;
      }
    }
  }
  p_new->before = 0;
  p_new->after = 0;
  p_new->height = 1;
  p_new->imbalance = 0;
  *head = __closure_avl_balance(p);
  return 0;
}

/* Walk the tree can call destroy on each node
*/
static void __closure_avl_destroy(closure_avl *p, void (*destroy)(closure_avl*)){
  if( p ){
    __closure_avl_destroy(p->before, destroy);
    __closure_avl_destroy(p->after, destroy);
    destroy(p);
  }
}
/*
** End of the AVL Tree implementation
******************************************************************************/

/* 
** A closure virtual-table object 
*/
struct closure_vtab {
  dbsql_vtab_t base;         /* Base class - must be first */
  char *z_db;                 /* Name of database.  (ex: "main") */
  char *self;               /* Name of this virtual table */
  char *table_name;          /* Name of table holding parent/child relation */
  char *id_column;           /* Name of ID column of table_name */
  char *parent_column;       /* Name of PARENT column in table_name */
  DBSQL *db;               /* The database connection */
  int n_cursor;               /* Number of pending cursors */
};

/* A closure cursor object */
struct closure_cursor {
  dbsql_vtab_cursor_t base;  /* Base class - must be first */
  closure_vtab *vtab;       /* The virtual table this cursor belongs to */
  char *table_name;          /* Name of table holding parent/child relation */
  char *id_column;           /* Name of ID column of table_name */
  char *parent_column;       /* Name of PARENT column in table_name */
  closure_avl *current;     /* Current element of output */
  closure_avl *closure;     /* The complete closure tree */
};

/* A queue of AVL nodes */
struct closure_queue {
  closure_avl *first;       /* Oldest node on the queue */
  closure_avl *last;        /* Youngest node on the queue */
};

/*
** Add a node to the end of the queue
*/
static void __queue_push(closure_queue *queue, closure_avl *node){
  node->list = 0;
  if( queue->last ){
    queue->last->list = node;
  }else{
    queue->first = node;
  }
  queue->last = node;
}

/*
** Extract the oldest element (the front element) from the queue.
*/
static closure_avl *__queue_pull(closure_queue *queue){
  closure_avl *p = queue->first;
  if( p ){
    queue->first = p->list;
    if( queue->first==0 ) queue->last = 0;
  }
  return p;
}

/*
** This function converts an SQL quoted string into an unquoted string
** and returns a pointer to a buffer allocated using dbsql_malloc() 
** containing the result. The caller should eventually free this buffer
** using dbsql_free.
**
** Examples:
**
**     "abc"   becomes   abc
**     'xyz'   becomes   xyz
**     [pqr]   becomes   pqr
**     `mno`   becomes   mno
*/
static char *__closure_dequote(const char *in){
  int64_t n_in;              /* Size of input string, in bytes */
  char *out;                     /* Output (dequoted) string */

  n_in = strlen(in);
  out = dbsql_malloc64(n_in+1);
  if( out ){
    char q = in[0];              /* Quote character (if any ) */

    if( q!='[' && q!= '\'' && q!='"' && q!='`' ){
      memcpy(out, in, (size_t)(n_in+1));
    }else{
      int i_out = 0;               /* index_t of next byte to write to output */
      int i_in;                    /* index_t of next byte to read from input */

      if( q=='[' ) q = ']';
      for(i_in=1; i_in<n_in; i_in++){
        if( in[i_in]==q ) i_in++;
        out[i_out++] = in[i_in];
      }
      out[i_out] = 0;
    }
    assert( (int)strlen(out)<=n_in );
  }
  return out;
}

/*
** Deallocate an closure_vtab object
*/
static void __closure_free(closure_vtab *p){
  if( p ){
    dbsql_free(p->z_db);
    dbsql_free(p->self);
    dbsql_free(p->table_name);
    dbsql_free(p->id_column);
    dbsql_free(p->parent_column);
    memset(p, 0, sizeof(*p));
    dbsql_free(p);
  }
}

/*
** disconnect/destroy method for the closure module.
*/
static int __closure_disconnect(dbsql_vtab_t *vtab){
  closure_vtab *p = (closure_vtab*)vtab;
  assert( p->n_cursor==0 );
  __closure_free(p);
  return DBSQL_OK;
}

/*
** Check to see if the argument is of the form:
**
**       KEY = VALUE
**
** If it is, return a pointer to the first character of VALUE.
** If not, return NULL.  Spaces around the = are ignored.
*/
static const char *__closure_value_of_key(const char *key, const char *str){
  int n_key = (int)strlen(key);
  int n_str = (int)strlen(str);
  int i;
  if( n_str<n_key+1 ) return 0;
  if( memcmp(str, key, n_key)!=0 ) return 0;
  for(i=n_key; isspace((unsigned char)str[i]); i++){}
  if( str[i]!='=' ) return 0;
  i++;
  while( isspace((unsigned char)str[i]) ){ i++; }
  return str+i;
}

/*
** connect/create method for the closure module. Arguments are:
**
**   argv[0]    -> module name  ("transitive_closure")
**   argv[1]    -> database name
**   argv[2]    -> table name
**   argv[3...] -> arguments
*/
static int __closure_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  int rc = DBSQL_OK;              /* Return code */
  closure_vtab *p_new = 0;          /* New virtual table */
  const char *z_db = argv[1];
  const char *val;
  int i;

  (void)aux;
  *vtab = 0;
  p_new = dbsql_malloc64( sizeof(*p_new) );
  if( p_new==0 ) return DBSQL_NOMEM;
  rc = DBSQL_NOMEM;
  memset(p_new, 0, sizeof(*p_new));
  p_new->db = db;
  p_new->z_db = dbsql_mprintf("%s", z_db);
  if( p_new->z_db==0 ) goto closure_connect_error;
  p_new->self = dbsql_mprintf("%s", argv[2]);
  if( p_new->self==0 ) goto closure_connect_error;
  for(i=3; i<argc; i++){
    val = __closure_value_of_key("tablename", argv[i]);
    if( val ){
      dbsql_free(p_new->table_name);
      p_new->table_name = __closure_dequote(val);
      if( p_new->table_name==0 ) goto closure_connect_error;
      continue;
    }
    val = __closure_value_of_key("idcolumn", argv[i]);
    if( val ){
      dbsql_free(p_new->id_column);
      p_new->id_column = __closure_dequote(val);
      if( p_new->id_column==0 ) goto closure_connect_error;
      continue;
    }
    val = __closure_value_of_key("parentcolumn", argv[i]);
    if( val ){
      dbsql_free(p_new->parent_column);
      p_new->parent_column = __closure_dequote(val);
      if( p_new->parent_column==0 ) goto closure_connect_error;
      continue;
    }
    *err_p = dbsql_mprintf("unrecognized argument: [%s]\n", argv[i]);
    __closure_free(p_new);
    *vtab = 0;
    return DBSQL_ERROR;
  }
  rc = dbsql_declare_vtab(db,
         "CREATE TABLE x(id,depth,root HIDDEN,tablename HIDDEN,"
                        "idcolumn HIDDEN,parentcolumn HIDDEN)"
       );
#define CLOSURE_COL_ID              0
#define CLOSURE_COL_DEPTH           1
#define CLOSURE_COL_ROOT            2
#define CLOSURE_COL_TABLENAME       3
#define CLOSURE_COL_IDCOLUMN        4
#define CLOSURE_COL_PARENTCOLUMN    5
  if( rc!=DBSQL_OK ){
    __closure_free(p_new);
  }
  *vtab = &p_new->base;
  return rc;

closure_connect_error:
  __closure_free(p_new);
  return rc;
}

/*
** Open a new closure cursor.
*/
static int __closure_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  closure_vtab *p = (closure_vtab*)v_tab;
  closure_cursor *cur;
  cur = dbsql_malloc64( sizeof(*cur) );
  if( cur==0 ) return DBSQL_NOMEM;
  memset(cur, 0, sizeof(*cur));
  cur->vtab = p;
  *cursor = &cur->base;
  p->n_cursor++;
  return DBSQL_OK;
}

/*
** Wrapper around dbsql_free
*/
static void __closure_mem_free(closure_avl *p){ dbsql_free(p); }

/*
** Free up all the memory allocated by a cursor.  Set it r_limit to 0
** to indicate that it is at EOF.
*/
static void __closure_clear_cursor(closure_cursor *cur){
  __closure_avl_destroy(cur->closure, __closure_mem_free);
  dbsql_free(cur->table_name);
  dbsql_free(cur->id_column);
  dbsql_free(cur->parent_column);
  cur->table_name = 0;
  cur->id_column = 0;
  cur->parent_column = 0;
  cur->current = 0;
  cur->closure = 0;
}

/*
** Close a closure cursor.
*/
static int __closure_close(dbsql_vtab_cursor_t *cur){
  closure_cursor *p_cur = (closure_cursor *)cur;
  __closure_clear_cursor(p_cur);
  p_cur->vtab->n_cursor--;
  dbsql_free(p_cur);
  return DBSQL_OK;
}

/*
** Advance a cursor to its next row of output
*/
static int __closure_next(dbsql_vtab_cursor_t *cur){
  closure_cursor *p_cur = (closure_cursor*)cur;
  p_cur->current = __closure_avl_next(p_cur->current);
  return DBSQL_OK;
}

/*
** Allocate and insert a node
*/
static int __closure_insert_node(
  closure_queue *queue,  /* Add new node to this queue */
  closure_cursor *cur,   /* The cursor into which to add the node */
  int64_t id,       /* The node ID */
  int i_generation         /* The generation number for this node */
){
  closure_avl *p_new = dbsql_malloc64( sizeof(*p_new) );
  if( p_new==0 ) return DBSQL_NOMEM;
  memset(p_new, 0, sizeof(*p_new));
  p_new->id = id;
  p_new->i_generation = i_generation;
  __closure_avl_insert(&cur->closure, p_new);
  __queue_push(queue, p_new);
  return DBSQL_OK;
}

/*
** Called to "rewind" a cursor back to the beginning so that
** it starts its output over again.  Always called at least once
** prior to any __closure_column, __closure_rowid, or __closure_eof call.
**
** This routine actually computes the closure.
**
** See the comment at the beginning of __closure_best_index() for a 
** description of the meaning of idx_num.  The idx_str parameter is
** not used.
*/
static int __closure_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  closure_cursor *cur = (closure_cursor *)vtab_cursor;
  closure_vtab *vtab = cur->vtab;
  int64_t i_root;
  int mx_gen = 999999999;
  char *sql;
  dbsql_stmt_t *stmt;
  closure_avl *avl;
  int rc = DBSQL_OK;
  const char *table_name = vtab->table_name;
  const char *id_column = vtab->id_column;
  const char *parent_column = vtab->parent_column;
  closure_queue s_queue;

  (void)idx_str;  /* Unused parameter */
  (void)argc;    /* Unused parameter */
  __closure_clear_cursor(cur);
  memset(&s_queue, 0, sizeof(s_queue));
  if( (idx_num & 1)==0 ){
    /* No root=$root in the WHERE clause.  Return an empty set */
    return DBSQL_OK;
  }
  i_root = dbsql_value_int64(argv[0]);
  if( (idx_num & 0x000f0)!=0 ){
    mx_gen = dbsql_value_int(argv[(idx_num>>4)&0x0f]);
    if( (idx_num & 0x00002)!=0 ) mx_gen--;
  }
  if( (idx_num & 0x00f00)!=0 ){
    table_name = (const char*)dbsql_value_text(argv[(idx_num>>8)&0x0f]);
    cur->table_name = dbsql_mprintf("%s", table_name);
  }
  if( (idx_num & 0x0f000)!=0 ){
    id_column = (const char*)dbsql_value_text(argv[(idx_num>>12)&0x0f]);
    cur->id_column = dbsql_mprintf("%s", id_column);
  }
  if( (idx_num & 0x0f0000)!=0 ){
    parent_column = (const char*)dbsql_value_text(argv[(idx_num>>16)&0x0f]);
    cur->parent_column = dbsql_mprintf("%s", parent_column);
  }

  sql = dbsql_mprintf(
       "SELECT \"%w\".\"%w\" FROM \"%w\" WHERE \"%w\".\"%w\"=?1",
       table_name, id_column, table_name, table_name, parent_column);
  if( sql==0 ){
    return DBSQL_NOMEM;
  }else{
    rc = dbsql_prepare_v2(vtab->db, sql, -1, &stmt, 0);
    dbsql_free(sql);
    if( rc ){
      dbsql_free(vtab->base.errmsg);
      vtab->base.errmsg = dbsql_mprintf("%s", dbsql_errmsg(vtab->db));
      return rc;
    }
  }
  if( rc==DBSQL_OK ){
    rc = __closure_insert_node(&s_queue, cur, i_root, 0);
  }
  while( (avl = __queue_pull(&s_queue))!=0 ){
    if( avl->i_generation>=mx_gen ) continue;
    dbsql_bind_int64(stmt, 1, avl->id);
    while( rc==DBSQL_OK && dbsql_step(stmt)==DBSQL_ROW ){
      if( dbsql_column_type(stmt,0)==DBSQL_INTEGER ){
        int64_t i_new = dbsql_column_int64(stmt, 0);
        if( __closure_avl_search(cur->closure, i_new)==0 ){
          rc = __closure_insert_node(&s_queue, cur, i_new, avl->i_generation+1);
        }
      }
    }
    dbsql_reset(stmt);
  }
  dbsql_finalize(stmt);
  if( rc==DBSQL_OK ){
    cur->current = __closure_avl_first(cur->closure);
  }

  return rc;
}

/*
** Only the word and distance columns have values.  All other columns
** return NULL
*/
static int __closure_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  closure_cursor *p_cur = (closure_cursor*)cur;
  switch( i ){
    case CLOSURE_COL_ID: {
      dbsql_result_int64(ctx, p_cur->current->id);
      break;
    }
    case CLOSURE_COL_DEPTH: {
      dbsql_result_int(ctx, p_cur->current->i_generation);
      break;
    }
    case CLOSURE_COL_ROOT: {
      dbsql_result_null(ctx);
      break;
    }
    case CLOSURE_COL_TABLENAME: {
      dbsql_result_text(ctx,
         p_cur->table_name ? p_cur->table_name : p_cur->vtab->table_name,
         -1, DBSQL_TRANSIENT);
      break;
    }
    case CLOSURE_COL_IDCOLUMN: {
      dbsql_result_text(ctx,
         p_cur->id_column ? p_cur->id_column : p_cur->vtab->id_column,
         -1, DBSQL_TRANSIENT);
      break;
    }
    case CLOSURE_COL_PARENTCOLUMN: {
      dbsql_result_text(ctx,
         p_cur->parent_column ? p_cur->parent_column : p_cur->vtab->parent_column,
         -1, DBSQL_TRANSIENT);
      break;
    }
  }
  return DBSQL_OK;
}

/*
** The rowid.  For the closure table, this is the same as the "id" column.
*/
static int __closure_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  closure_cursor *p_cur = (closure_cursor*)cur;
  *rowid = p_cur->current->id;
  return DBSQL_OK;
}

/*
** EOF indicator
*/
static int __closure_eof(dbsql_vtab_cursor_t *cur){
  closure_cursor *p_cur = (closure_cursor*)cur;
  return p_cur->current==0;
}

/*
** Search for terms of these forms:
**
**   (A)    root = $root
**   (B1)   depth < $depth
**   (B2)   depth <= $depth
**   (B3)   depth = $depth
**   (C)    tablename = $tablename
**   (D)    idcolumn = $idcolumn
**   (E)    parentcolumn = $parentcolumn
**
** 
**
**   idx_num       meaning
**   ----------   ------------------------------------------------------
**   0x00000001   Term of the form (A) found
**   0x00000002   The term of bit-2 is like (B1)
**   0x000000f0   index_t in filter.argv[] of $depth.  0 if not used.
**   0x00000f00   index_t in filter.argv[] of $tablename.  0 if not used.
**   0x0000f000   index_t in filter.argv[] of $idcolumn.  0 if not used
**   0x000f0000   index_t in filter.argv[] of $parentcolumn.  0 if not used.
**
** There must be a term of type (A).  If there is not, then the index type
** is 0 and the query will return an empty set.
*/
static int __closure_best_index(
  dbsql_vtab_t *tab,             /* The virtual table */
  dbsql_index_info_t *idx_info    /* Information about the query */
){
  int i_plan = 0;
  int i;
  int idx = 1;
  const struct dbsql_index_constraint_t *constraint;
  closure_vtab *vtab = (closure_vtab*)tab;
  double r_cost = 10000000.0;

  constraint = idx_info->constraints;
  for(i=0; i<idx_info->n_constraint; i++, constraint++){
    if( constraint->usable==0 ) continue;
    if( (i_plan & 1)==0 
     && constraint->i_column==CLOSURE_COL_ROOT
     && constraint->op==DBSQL_INDEX_CONSTRAINT_EQ
    ){
      i_plan |= 1;
      idx_info->constraint_usages[i].argv_index = 1;
      idx_info->constraint_usages[i].omit = 1;
      r_cost /= 100.0;
    }
    if( (i_plan & 0x0000f0)==0
     && constraint->i_column==CLOSURE_COL_DEPTH
     && (constraint->op==DBSQL_INDEX_CONSTRAINT_LT
           || constraint->op==DBSQL_INDEX_CONSTRAINT_LE
           || constraint->op==DBSQL_INDEX_CONSTRAINT_EQ)
    ){
      i_plan |= idx<<4;
      idx_info->constraint_usages[i].argv_index = ++idx;
      if( constraint->op==DBSQL_INDEX_CONSTRAINT_LT ) i_plan |= 0x000002;
      r_cost /= 5.0;
    }
    if( (i_plan & 0x000f00)==0
     && constraint->i_column==CLOSURE_COL_TABLENAME
     && constraint->op==DBSQL_INDEX_CONSTRAINT_EQ
    ){
      i_plan |= idx<<8;
      idx_info->constraint_usages[i].argv_index = ++idx;
      idx_info->constraint_usages[i].omit = 1;
      r_cost /= 5.0;
    }
    if( (i_plan & 0x00f000)==0
     && constraint->i_column==CLOSURE_COL_IDCOLUMN
     && constraint->op==DBSQL_INDEX_CONSTRAINT_EQ
    ){
      i_plan |= idx<<12;
      idx_info->constraint_usages[i].argv_index = ++idx;
      idx_info->constraint_usages[i].omit = 1;
    }
    if( (i_plan & 0x0f0000)==0
     && constraint->i_column==CLOSURE_COL_PARENTCOLUMN
     && constraint->op==DBSQL_INDEX_CONSTRAINT_EQ
    ){
      i_plan |= idx<<16;
      idx_info->constraint_usages[i].argv_index = ++idx;
      idx_info->constraint_usages[i].omit = 1;
    }
  }
  if( (vtab->table_name==0    && (i_plan & 0x000f00)==0)
   || (vtab->id_column==0     && (i_plan & 0x00f000)==0)
   || (vtab->parent_column==0 && (i_plan & 0x0f0000)==0)
  ){
    /* All of tablename, idcolumn, and parentcolumn must be specified
    ** in either the CREATE VIRTUAL TABLE or in the WHERE clause constraints
    ** or else the result is an empty set. */
    i_plan = 0;
  }
  if( (i_plan&1)==0 ){
    /* If there is no usable "root=?" term, then set the index-type to 0.
    ** Also clear any argv_index variables already set. This is necessary
    ** to prevent the core from throwing an "best_index malfunction error"
    ** error (because the argv_index values are not contiguously assigned
    ** starting from 1).  */
    r_cost *= 1e30;
    for(i=0; i<idx_info->n_constraint; i++, constraint++){
      idx_info->constraint_usages[i].argv_index = 0;
    }
    i_plan = 0;
  }
  idx_info->idx_num = i_plan;
  if( idx_info->n_order_by==1
   && idx_info->order_bies[0].i_column==CLOSURE_COL_ID
   && idx_info->order_bies[0].desc==0
  ){
    idx_info->order_by_consumed = 1;
  }
  idx_info->estimated_cost = r_cost;
   
  return DBSQL_OK;
}

/*
** A virtual table module that implements the "transitive_closure".
*/
static dbsql_module_t __closure_module = {
  0,                      /* i_version */
  __closure_connect,         /* create */
  __closure_connect,         /* connect */
  __closure_best_index,       /* best_index */
  __closure_disconnect,      /* disconnect */
  __closure_disconnect,      /* destroy */
  __closure_open,            /* x_open - open a cursor */
  __closure_close,           /* x_close - close a cursor */
  __closure_filter,          /* filter - configure scan constraints */
  __closure_next,            /* next - advance a cursor */
  __closure_eof,             /* eof - check for end of scan */
  __closure_column,          /* column - read data */
  __closure_rowid,           /* rowid - read data */
  0,                      /* update */
  0,                      /* begin */
  0,                      /* x_sync */
  0,                      /* commit */
  0,                      /* rollback */
  0,                      /* find_method */
  0,                      /* rename */
  0,                      /* savepoint */
  0,                      /* release */
  0,                      /* rollback_to */
  0,                      /* shadow_name */
  0                       /* integrity */
};

#endif /* DBSQL_OMIT_VIRTUALTABLE */

/*
** Register the closure virtual table
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_closure_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;
#if defined(DBSQL_TEST) && !defined(DBSQL_OMIT_VIRTUALTABLE)
  rc = dbsql_create_module(db, "transitive_closure", &__closure_module, 0);
#endif /* DBSQL_TEST && !DBSQL_OMIT_VIRTUALTABLE */
  return rc;
}
