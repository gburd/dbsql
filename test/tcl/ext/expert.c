/*
** 2017 April 09
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
*/
#include "dbsql_expert.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

#if !defined(DBSQL_AMALGAMATION)
#if defined(DBSQL_COVERAGE_TEST) || defined(DBSQL_MUTATION_TEST)
# define DBSQL_OMIT_AUXILIARY_SAFETY_CHECKS 1
#endif
#if defined(DBSQL_OMIT_AUXILIARY_SAFETY_CHECKS)
# define ALWAYS(X)      (1)
# define NEVER(X)       (0)
#elif !defined(NDEBUG)
# define ALWAYS(X)      ((X)?1:(assert(0),0))
# define NEVER(X)       ((X)?(assert(0),1):0)
#else
# define ALWAYS(X)      (X)
# define NEVER(X)       (X)
#endif
#endif /* !defined(DBSQL_AMALGAMATION) */


#ifndef DBSQL_OMIT_VIRTUALTABLE

typedef int64_t int64_t;
typedef u_int64_t u_int64_t;

typedef struct idx_column_t idx_column_t;
typedef struct idx_constraint_t idx_constraint_t;
typedef struct idx_scan_t idx_scan_t;
typedef struct idx_statement_t idx_statement_t;
typedef struct idx_table_t idx_table_t;
typedef struct idx_write_t idx_write_t;

#define STRLEN  (int)strlen

/*
** A temp table name that we assume no user database will actually use.
** If this assumption proves incorrect triggers on the table with the
** conflicting name will be ignored.
*/
#define UNIQUE_TABLE_NAME "t592690916721053953805701627921227776"

/*
** A single constraint. Equivalent to either "col = ?" or "col < ?" (or
** any other type of single-ended range constraint on a column).
**
** link:
**   Used to temporarily link idx_constraint_t objects into lists while
**   creating candidate indexes.
*/
struct idx_constraint_t {
  char *coll;                    /* Collation sequence */
  int range;                     /* True for range, false for eq */
  int i_col;                       /* Constrained table column */
  int flag;                      /* Used by __idx_find_compatible() */
  int desc;                      /* True if ORDER BY <expr> DESC */
  idx_constraint_t *next;           /* Next constraint in eq or range list */
  idx_constraint_t *link;           /* See above */
};

/*
** A single scan of a single table.
*/
struct idx_scan_t {
  idx_table_t *tab;                 /* Associated table object */
  int i_db;                        /* Database containing table table */
  int64_t covering;                   /* Mask of columns required for cov. index */
  idx_constraint_t *order;          /* ORDER BY columns */
  idx_constraint_t *eq;             /* List of == constraints */
  idx_constraint_t *range;          /* List of < constraints */
  idx_scan_t *next_scan;             /* Next idx_scan_t object for same analysis */
};

/*
** Information regarding a single database table. Extracted from 
** "PRAGMA table_info" by function __idx_get_table_info().
*/
struct idx_column_t {
  char *name;
  char *coll;
  int i_pk;
};
struct idx_table_t {
  int n_col;
  char *name;                    /* table_t name */
  idx_column_t *cols;
  idx_table_t *next;                /* Next table in linked list of all tables */
};

/*
** An object of the following type is created for each unique table/write-op
** seen. The objects are stored in a singly-linked list beginning at
** sqlite3expert.p_write.
*/
struct idx_write_t {
  idx_table_t *tab;
  int op;                        /* DBSQL_UPDATE, DELETE or INSERT */
  idx_write_t *next;
};

/*
** Each statement being analyzed is represented by an instance of this
** structure.
*/
struct idx_statement_t {
  int i_id;                        /* Statement number */
  char *z_sql;                     /* SQL statement */
  char *idx;                     /* Indexes */
  char *eqp;                     /* Plan */
  idx_statement_t *next;
};


/*
** A hash table for storing strings. with_t space for a payload string
** with each entry. Methods are:
**
**   __idx_hash_init()
**   __idx_hash_clear()
**   __idx_hash_add()
**   __idx_hash_search()
*/
#define IDX_HASH_SIZE 1023
typedef struct idx_hash_entry_t idx_hash_entry_t;
typedef struct idx_hash_t idx_hash_t;
struct idx_hash_entry_t {
  char *key;                     /* nul-terminated key */
  char *val;                     /* nul-terminated value string */
  char *val2;                    /* nul-terminated value string 2 */
  idx_hash_entry_t *hash_next;        /* Next entry in same hash bucket */
  idx_hash_entry_t *next;            /* Next entry in hash */
};
struct idx_hash_t {
  idx_hash_entry_t *first;
  idx_hash_entry_t *hashes[IDX_HASH_SIZE];
};

/*
** sqlite3expert object.
*/
struct __dbsqlexpert {
  int i_sample;                    /* Percentage of tables to sample for stat1 */
  DBSQL *db;                    /* User database */
  DBSQL *dbm;                   /* In-memory db for this analysis */
  DBSQL *dbv;                   /* Vtab schema for this analysis */
  idx_table_t *table;               /* List of all idx_table_t objects */
  idx_scan_t *scan;                 /* List of scan objects */
  idx_write_t *p_write;               /* List of write objects */
  idx_statement_t *statement;       /* List of idx_statement_t objects */
  int run;                       /* True once analysis has run */
  char **errmsg;
  int rc;                         /* Error code from whereinfo hook */
  idx_hash_t h_idx;                   /* hash_t containing all candidate indexes */
  char *candidates;              /* For EXPERT_REPORT_CANDIDATES */
};


/*
** Allocate and return n_byte bytes of zeroed memory using dbsql_malloc(). 
** If the allocation fails, set *rc to DBSQL_NOMEM and return NULL.
*/
static void *__idx_malloc(int *rc, int64_t n_byte){
  void *ret;
  assert( *rc==DBSQL_OK );
  assert( n_byte>0 );
  ret = dbsql_malloc64(n_byte);
  if( ret ){
    memset(ret, 0, n_byte);
  }else{
    *rc = DBSQL_NOMEM;
  }
  return ret;
}

/*
** Initialize an idx_hash_t hash table.
*/
static void __idx_hash_init(idx_hash_t *hash){
  memset(hash, 0, sizeof(idx_hash_t));
}

/*
** Reset an idx_hash_t hash table.
*/
static void __idx_hash_clear(idx_hash_t *hash){
  int i;
  for(i=0; i<IDX_HASH_SIZE; i++){
    idx_hash_entry_t *entry;
    idx_hash_entry_t *next;
    for(entry=hash->hashes[i]; entry; entry=next){
      next = entry->hash_next;
      dbsql_free(entry->val2);
      dbsql_free(entry);
    }
  }
  memset(hash, 0, sizeof(idx_hash_t));
}

/*
** Return the index of the hash bucket that the string specified by the
** arguments to this function belongs.
*/
static int __idx_hash_string(const char *z, int n){
  unsigned int ret = 0;
  int i;
  for(i=0; i<n; i++){
    ret += (ret<<3) + (unsigned char)(z[i]);
  }
  return (int)(ret % IDX_HASH_SIZE);
}

/*
** If key is already present in the hash table, return non-zero and do
** nothing. Otherwise, add an entry with key key and payload string val to
** the hash table passed as the second argument. 
*/
static int __idx_hash_add(
  int *rc, 
  idx_hash_t *hash, 
  const char *key,
  const char *val
){
  int n_key = STRLEN(key);
  int i_hash = __idx_hash_string(key, n_key);
  int n_val = (val ? STRLEN(val) : 0);
  idx_hash_entry_t *entry;
  assert( i_hash>=0 );
  for(entry=hash->hashes[i_hash]; entry; entry=entry->hash_next){
    if( STRLEN(entry->key)==n_key && 0==memcmp(entry->key, key, n_key) ){
      return 1;
    }
  }
  entry = __idx_malloc(rc, sizeof(idx_hash_entry_t) + (int64_t)n_key+1 + (int64_t)n_val+1);
  if( entry ){
    entry->key = (char*)&entry[1];
    memcpy(entry->key, key, n_key);
    if( val ){
      entry->val = &entry->key[n_key+1];
      memcpy(entry->val, val, n_val);
    }
    entry->hash_next = hash->hashes[i_hash];
    hash->hashes[i_hash] = entry;

    entry->next = hash->first;
    hash->first = entry;
  }
  return 0;
}

/*
** If key/n_key is present in the hash table, return a pointer to the 
** hash-entry object.
*/
static idx_hash_entry_t *__idx_hash_find(idx_hash_t *hash, const char *key, int n_key){
  int i_hash;
  idx_hash_entry_t *entry;
  if( n_key<0 ) n_key = STRLEN(key);
  i_hash = __idx_hash_string(key, n_key);
  assert( i_hash>=0 );
  for(entry=hash->hashes[i_hash]; entry; entry=entry->hash_next){
    if( STRLEN(entry->key)==n_key && 0==memcmp(entry->key, key, n_key) ){
      return entry;
    }
  }
  return 0;
}

/*
** If the hash table contains an entry with a key equal to the string
** passed as the final two arguments to this function, return a pointer
** to the payload string. Otherwise, if key/n_key is not present in the
** hash table, return NULL.
*/
static const char *__idx_hash_search(idx_hash_t *hash, const char *key, int n_key){
  idx_hash_entry_t *entry = __idx_hash_find(hash, key, n_key);
  if( entry ) return entry->val;
  return 0;
}

/*
** Allocate and return a new idx_constraint_t object. Set the idx_constraint_t.coll
** variable to point to a copy of nul-terminated string coll.
*/
static idx_constraint_t *__idx_new_constraint(int *rc, const char *coll){
  idx_constraint_t *p_new;
  int n_coll = STRLEN(coll);

  assert( *rc==DBSQL_OK );
  p_new = (idx_constraint_t*)__idx_malloc(rc, sizeof(idx_constraint_t) * n_coll + 1);
  if( p_new ){
    p_new->coll = (char*)&p_new[1];
    memcpy(p_new->coll, coll, n_coll+1);
  }
  return p_new;
}

/*
** An error associated with database handle db has just occurred. Pass
** the error message to callback function out.
*/
static void __idx_database_error(
  DBSQL *db,                    /* Database handle */
  char **errmsg                 /* Write error here */
){
  *errmsg = dbsql_mprintf("%s", dbsql_errmsg(db));
}

/*
** Prepare an SQL statement.
*/
static int __idx_prepare_stmt(
  DBSQL *db,                    /* Database handle to compile against */
  dbsql_stmt_t **stmt,          /* OUT: Compiled SQL statement */
  char **errmsg,                /* OUT: dbsql_malloc()ed error message */
  const char *sql                /* SQL statement to compile */
){
  int rc = dbsql_prepare_v2(db, sql, -1, stmt, 0);
  if( rc!=DBSQL_OK ){
    *stmt = 0;
    __idx_database_error(db, errmsg);
  }
  return rc;
}

/*
** Prepare an SQL statement using the results of a printf() formatting.
*/
static int __idx_printf_prepare_stmt(
  DBSQL *db,                    /* Database handle to compile against */
  dbsql_stmt_t **stmt,          /* OUT: Compiled SQL statement */
  char **errmsg,                /* OUT: dbsql_malloc()ed error message */
  const char *fmt,               /* printf() format of SQL statement */
  ...                             /* Trailing printf() arguments */
){
  va_list ap;
  int rc;
  char *sql;
  va_start(ap, fmt);
  sql = dbsql_vmprintf(fmt, ap);
  if( sql==0 ){
    rc = DBSQL_NOMEM;
  }else{
    rc = __idx_prepare_stmt(db, stmt, errmsg, sql);
    dbsql_free(sql);
  }
  va_end(ap);
  return rc;
}


/*************************************************************************
** Beginning of virtual table implementation.
*/
typedef struct expert_vtab_t expert_vtab_t;
struct expert_vtab_t {
  dbsql_vtab_t base;
  idx_table_t *tab;
  __dbsqlexpert *expert;
};

typedef struct expert_csr_t expert_csr_t;
struct expert_csr_t {
  dbsql_vtab_cursor_t base;
  dbsql_stmt_t *data;
};

static char *__expert_dequote(const char *in){
  int64_t n = STRLEN(in);
  char *ret = dbsql_malloc64(n);

  assert( in[0]=='\'' );
  assert( in[n-1]=='\'' );

  if( ret ){
    int64_t i_out = 0;
    int64_t i_in = 0;
    for(i_in=1; i_in<(n-1); i_in++){
      if( in[i_in]=='\'' ){
        assert( in[i_in+1]=='\'' );
        i_in++;
      }
      ret[i_out++] = in[i_in];
    }
    ret[i_out] = '\0';
  }

  return ret;
}

/* 
** This function is the implementation of both the connect and create
** methods of the r-tree virtual table.
**
**   argv[0]   -> module name
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> column names...
*/
static int __expert_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  __dbsqlexpert *expert = (__dbsqlexpert*)aux;
  expert_vtab_t *p = 0;
  int rc;

  if( argc!=4 ){
    *err_p = dbsql_mprintf("internal error!");
    rc = DBSQL_ERROR;
  }else{
    char *create_table = __expert_dequote(argv[3]);
    if( create_table ){
      rc = dbsql_declare_vtab(db, create_table);
      if( rc==DBSQL_OK ){
        p = __idx_malloc(&rc, sizeof(expert_vtab_t));
      }
      if( rc==DBSQL_OK ){
        p->expert = expert;
        p->tab = expert->table;
        assert( dbsql_stricmp(p->tab->name, argv[2])==0 );
      }
      dbsql_free(create_table);
    }else{
      rc = DBSQL_NOMEM;
    }
  }

  *vtab = (dbsql_vtab_t*)p;
  return rc;
}

static int __expert_disconnect(dbsql_vtab_t *vtab){
  expert_vtab_t *p = (expert_vtab_t*)vtab;
  dbsql_free(p);
  return DBSQL_OK;
}

static int __expert_best_index(dbsql_vtab_t *vtab, dbsql_index_info_t *idx_info){
  expert_vtab_t *p = (expert_vtab_t*)vtab;
  int rc = DBSQL_OK;
  int n = 0;
  idx_scan_t *scan;
  const int opmask = 
    DBSQL_INDEX_CONSTRAINT_EQ | DBSQL_INDEX_CONSTRAINT_GT |
    DBSQL_INDEX_CONSTRAINT_LT | DBSQL_INDEX_CONSTRAINT_GE |
    DBSQL_INDEX_CONSTRAINT_LE;

  scan = __idx_malloc(&rc, sizeof(idx_scan_t));
  if( scan ){
    int i;

    /* Link the new scan object into the list */
    scan->tab = p->tab;
    scan->next_scan = p->expert->scan;
    p->expert->scan = scan;

    /* Add the constraints to the idx_scan_t object */
    for(i=0; i<idx_info->n_constraint; i++){
      struct dbsql_index_constraint_t *cons = &idx_info->constraints[i];
      if( cons->usable 
       && cons->i_column>=0 
       && p->tab->cols[cons->i_column].i_pk==0
       && (cons->op & opmask) 
      ){
        idx_constraint_t *p_new;
        const char *coll = dbsql_vtab_collation(idx_info, i);
        p_new = __idx_new_constraint(&rc, coll);
        if( p_new ){
          p_new->i_col = cons->i_column;
          if( cons->op==DBSQL_INDEX_CONSTRAINT_EQ ){
            p_new->next = scan->eq;
            scan->eq = p_new;
          }else{
            p_new->range = 1;
            p_new->next = scan->range;
            scan->range = p_new;
          }
        }
        n++;
        idx_info->constraint_usages[i].argv_index = n;
      }
    }

    /* Add the ORDER BY to the idx_scan_t object */
    for(i=idx_info->n_order_by-1; i>=0; i--){
      int i_col = idx_info->order_bies[i].i_column;
      if( i_col>=0 ){
        idx_constraint_t *p_new = __idx_new_constraint(&rc, p->tab->cols[i_col].coll);
        if( p_new ){
          p_new->i_col = i_col;
          p_new->desc = idx_info->order_bies[i].desc;
          p_new->next = scan->order;
          p_new->link = scan->order;
          scan->order = p_new;
          n++;
        }
      }
    }
  }

  idx_info->estimated_cost = 1000000.0 / (n+1);
  return rc;
}

static int __expert_update(
  dbsql_vtab_t *vtab, 
  int n_data, 
  dbsql_value_t **data, 
  int64_t *rowid
){
  (void)vtab;
  (void)n_data;
  (void)data;
  (void)rowid;
  return DBSQL_OK;
}

/* 
** Virtual table module x_open method.
*/
static int __expert_open(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **cursor){
  int rc = DBSQL_OK;
  expert_csr_t *csr;
  (void)v_tab;
  csr = __idx_malloc(&rc, sizeof(expert_csr_t));
  *cursor = (dbsql_vtab_cursor_t*)csr;
  return rc;
}

/* 
** Virtual table module x_close method.
*/
static int __expert_close(dbsql_vtab_cursor_t *cur){
  expert_csr_t *csr = (expert_csr_t*)cur;
  dbsql_finalize(csr->data);
  dbsql_free(csr);
  return DBSQL_OK;
}

/*
** Virtual table module eof method.
**
** Return non-zero if the cursor does not currently point to a valid 
** record (i.e if the scan has finished), or zero otherwise.
*/
static int __expert_eof(dbsql_vtab_cursor_t *cur){
  expert_csr_t *csr = (expert_csr_t*)cur;
  return csr->data==0;
}

/* 
** Virtual table module next method.
*/
static int __expert_next(dbsql_vtab_cursor_t *cur){
  expert_csr_t *csr = (expert_csr_t*)cur;
  int rc = DBSQL_OK;

  assert( csr->data );
  rc = dbsql_step(csr->data);
  if( rc!=DBSQL_ROW ){
    rc = dbsql_finalize(csr->data);
    csr->data = 0;
  }else{
    rc = DBSQL_OK;
  }

  return rc;
}

/* 
** Virtual table module rowid method.
*/
static int __expert_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  (void)cur;
  *rowid = 0;
  return DBSQL_OK;
}

/* 
** Virtual table module column method.
*/
static int __expert_column(dbsql_vtab_cursor_t *cur, dbsql_func_t *ctx, int i){
  expert_csr_t *csr = (expert_csr_t*)cur;
  dbsql_value_t *val;
  val = dbsql_column_value(csr->data, i);
  if( val ){
    dbsql_result_value(ctx, val);
  }
  return DBSQL_OK;
}

/* 
** Virtual table module filter method.
*/
static int __expert_filter(
  dbsql_vtab_cursor_t *cur, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  expert_csr_t *csr = (expert_csr_t*)cur;
  expert_vtab_t *vtab = (expert_vtab_t*)(cur->vtab);
  __dbsqlexpert *expert = vtab->expert;
  int rc;

  (void)idx_num;
  (void)idx_str;
  (void)argc;
  (void)argv;
  rc = dbsql_finalize(csr->data);
  csr->data = 0;
  if( rc==DBSQL_OK ){
    rc = __idx_printf_prepare_stmt(expert->db, &csr->data, &vtab->base.errmsg,
        "SELECT * FROM main.%Q WHERE dbsql_expert_sample()", vtab->tab->name
    );
  }

  if( rc==DBSQL_OK ){
    rc = __expert_next(cur);
  }
  return rc;
}

static int __idx_register_vtab(__dbsqlexpert *p){
  static dbsql_module_t expert_module = {
    2,                            /* i_version */
    __expert_connect,                /* create - create a table */
    __expert_connect,                /* connect - connect to an existing table */
    __expert_best_index,              /* best_index - Determine search strategy */
    __expert_disconnect,             /* disconnect - Disconnect from a table */
    __expert_disconnect,             /* destroy - Drop a table */
    __expert_open,                   /* x_open - open a cursor */
    __expert_close,                  /* x_close - close a cursor */
    __expert_filter,                 /* filter - configure scan constraints */
    __expert_next,                   /* next - advance a cursor */
    __expert_eof,                    /* eof */
    __expert_column,                 /* column - read data */
    __expert_rowid,                  /* rowid - read data */
    __expert_update,                 /* update - write data */
    0,                            /* begin - begin transaction */
    0,                            /* x_sync - sync transaction */
    0,                            /* commit - commit transaction */
    0,                            /* rollback - rollback transaction */
    0,                            /* find_function - function overloading */
    0,                            /* rename - rename the table */
    0,                            /* savepoint */
    0,                            /* release */
    0,                            /* rollback_to */
    0,                            /* shadow_name */
    0,                            /* integrity */
  };

  return dbsql_create_module(p->dbv, "expert", &expert_module, (void*)p);
}
/*
** End of virtual table implementation.
*************************************************************************/
/*
** Finalize SQL statement stmt. If (*rc) is DBSQL_OK when this function
** is called, set it to the return value of dbsql_finalize() before
** returning. Otherwise, discard the dbsql_finalize() return value.
*/
static void __idx_finalize(int *p_rc, dbsql_stmt_t *stmt){
  int rc = dbsql_finalize(stmt);
  if( *p_rc==DBSQL_OK ) *p_rc = rc;
}

/*
** Attempt to allocate an idx_table_t structure corresponding to table tab
** in the main database of connection db. If successful, set (*out) to
** point to the new object and return DBSQL_OK. Otherwise, return an
** dbsql error code and set (*out) to NULL. In this case *errmsg may be
** set to point to an error string.
**
** It is the responsibility of the caller to eventually free either the
** idx_table_t object or error message using dbsql_free().
*/
static int __idx_get_table_info(
  DBSQL *db,                    /* Database connection to read details from */
  const char *tab,               /* table_t name */
  idx_table_t **out,               /* OUT: New object (if successful) */
  char **errmsg                 /* OUT: Error message (if not) */
){
  dbsql_stmt_t *p1 = 0;
  int n_col = 0;
  int n_tab;
  int64_t n_byte;
  idx_table_t *p_new = 0;
  int rc, rc2;
  char *csr = 0;
  int n_pk = 0;

  *out = 0;
  if( tab==0 ) return DBSQL_ERROR;
  n_tab = STRLEN(tab);
  n_byte = sizeof(idx_table_t) + n_tab + 1;
  rc = __idx_printf_prepare_stmt(db, &p1, errmsg, "PRAGMA table_xinfo=%Q", tab);
  while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(p1) ){
    const char *col = (const char*)dbsql_column_text(p1, 1);
    const char *col_seq = 0;
    if( col==0 ){
      rc = DBSQL_ERROR;
      break;
    }
    n_byte += 1 + STRLEN(col);
    rc = dbsql_table_column_metadata(
        db, "main", tab, col, 0, &col_seq, 0, 0, 0
    );
    if( col_seq==0 ) col_seq = "binary";
    n_byte += 1 + STRLEN(col_seq);
    n_col++;
    n_pk += (dbsql_column_int(p1, 5)>0);
  }
  rc2 = dbsql_reset(p1);
  if( rc==DBSQL_OK ) rc = rc2;

  n_byte += sizeof(idx_column_t) * n_col;
  if( rc==DBSQL_OK ){
    p_new = __idx_malloc(&rc, n_byte);
  }
  if( rc==DBSQL_OK ){
    p_new->cols = (idx_column_t*)&p_new[1];
    p_new->n_col = n_col;
    csr = (char*)&p_new->cols[n_col];
  }

  n_col = 0;
  while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(p1) ){
    const char *col = (const char*)dbsql_column_text(p1, 1);
    const char *col_seq = 0;
    int n_copy;
    if( col==0 ) continue;
    n_copy = STRLEN(col) + 1;
    p_new->cols[n_col].name = csr;
    p_new->cols[n_col].i_pk = (dbsql_column_int(p1, 5)==1 && n_pk==1);
    memcpy(csr, col, n_copy);
    csr += n_copy;

    rc = dbsql_table_column_metadata(
        db, "main", tab, col, 0, &col_seq, 0, 0, 0
    );
    if( rc==DBSQL_OK ){
      if( col_seq==0 ) col_seq = "binary";
      n_copy = STRLEN(col_seq) + 1;
      p_new->cols[n_col].coll = csr;
      memcpy(csr, col_seq, n_copy);
      csr += n_copy;
    }

    n_col++;
  }
  __idx_finalize(&rc, p1);

  if( rc!=DBSQL_OK ){
    dbsql_free(p_new);
    p_new = 0;
  }else if( ALWAYS(p_new!=0) ){
    p_new->name = csr;
    if( ALWAYS(p_new->name!=0) ) memcpy(p_new->name, tab, n_tab+1);
  }

  *out = p_new;
  return rc;
}

/*
** This function is a no-op if *rc is set to anything other than 
** DBSQL_OK when it is called.
**
** If *rc is initially set to DBSQL_OK, then the text specified by
** the printf() style arguments is appended to in and the result returned
** in a buffer allocated by dbsql_malloc(). dbsql_free() is called on
** in before returning.
*/
static char *__idx_append_text(int *rc, char *in, const char *fmt, ...){
  va_list ap;
  char *append = 0;
  char *ret = 0;
  int64_t n_in = in ? STRLEN(in) : 0;
  int64_t n_append = 0;
  va_start(ap, fmt);
  if( *rc==DBSQL_OK ){
    append = dbsql_vmprintf(fmt, ap);
    if( append ){
      n_append = STRLEN(append);
      ret = (char*)dbsql_malloc64(n_in + n_append + 1);
    }
    if( append && ret ){
      if( n_in ) memcpy(ret, in, n_in);
      memcpy(&ret[n_in], append, n_append+1);
    }else{
      dbsql_free(ret);
      ret = 0;
      *rc = DBSQL_NOMEM;
    }
    dbsql_free(append);
    dbsql_free(in);
  }
  va_end(ap);
  return ret;
}

/*
** Return true if id must be quoted in order to use it as an SQL
** identifier, or false otherwise.
*/
static int __idx_identifier_requires_quotes(const char *id){
  int i;
  int n_id = STRLEN(id);
  
  if( dbsql_keyword_check(id, n_id) ) return 1;

  for(i=0; id[i]; i++){
    if( !(id[i]=='_')
     && !(id[i]>='0' && id[i]<='9')
     && !(id[i]>='a' && id[i]<='z')
     && !(id[i]>='A' && id[i]<='Z')
    ){
      return 1;
    }
  }
  return 0;
}

/*
** This function appends an index column definition suitable for constraint
** cons to the string passed as in and returns the result.
*/
static char *__idx_append_col_defn(
  int *rc,                       /* IN/OUT: Error code */
  char *in,                      /* column_t defn accumulated so far */
  idx_table_t *tab,                 /* table_t index will be created on */
  idx_constraint_t *cons
){
  char *ret = in;
  idx_column_t *p = &tab->cols[cons->i_col];
  if( ret ) ret = __idx_append_text(rc, ret, ", ");

  if( __idx_identifier_requires_quotes(p->name) ){
    ret = __idx_append_text(rc, ret, "%Q", p->name);
  }else{
    ret = __idx_append_text(rc, ret, "%s", p->name);
  }

  if( dbsql_stricmp(p->coll, cons->coll) ){
    if( __idx_identifier_requires_quotes(cons->coll) ){
      ret = __idx_append_text(rc, ret, " COLLATE %Q", cons->coll);
    }else{
      ret = __idx_append_text(rc, ret, " COLLATE %s", cons->coll);
    }
  }

  if( cons->desc ){
    ret = __idx_append_text(rc, ret, " DESC");
  }
  return ret;
}

/*
** Search database dbm for an index compatible with the one __idx_create_from_cons()
** would create from arguments scan, eq and tail. If no error occurs and 
** such an index is found, return non-zero. Or, if no such index is found,
** return zero.
**
** If an error occurs, set *rc to an dbsql error code and return zero.
*/
static int __idx_find_compatible(
  int *p_rc,                       /* OUT: Error code */
  DBSQL* dbm,                   /* Database to search */
  idx_scan_t *scan,                 /* Scan for table to search for index on */
  idx_constraint_t *eq,             /* List of == constraints */
  idx_constraint_t *tail            /* List of range constraints */
){
  const char *tbl = scan->tab->name;
  dbsql_stmt_t *idx_list = 0;
  idx_constraint_t *iter;
  int n_eq = 0;                    /* Number of elements in eq */
  int rc;

  /* Count the elements in list eq */
  for(iter=eq; iter; iter=iter->link) n_eq++;

  rc = __idx_printf_prepare_stmt(dbm, &idx_list, 0, "PRAGMA index_list=%Q", tbl);
  while( rc==DBSQL_OK && dbsql_step(idx_list)==DBSQL_ROW ){
    int match = 1;
    idx_constraint_t *t = tail;
    dbsql_stmt_t *info = 0;
    const char *idx = (const char*)dbsql_column_text(idx_list, 1);
    if( idx==0 ) continue;

    /* Zero the idx_constraint_t.flag values in the eq list */
    for(iter=eq; iter; iter=iter->link) iter->flag = 0;

    rc = __idx_printf_prepare_stmt(dbm, &info, 0, "PRAGMA index_xInfo=%Q", idx);
    while( rc==DBSQL_OK && dbsql_step(info)==DBSQL_ROW ){
      int i_idx = dbsql_column_int(info, 0);
      int i_col = dbsql_column_int(info, 1);
      const char *coll = (const char*)dbsql_column_text(info, 4);

      if( i_idx<n_eq ){
        for(iter=eq; iter; iter=iter->link){
          if( iter->flag ) continue;
          if( iter->i_col!=i_col ) continue;
          if( dbsql_stricmp(iter->coll, coll) ) continue;
          iter->flag = 1;
          break;
        }
        if( iter==0 ){
          match = 0;
          break;
        }
      }else{
        if( t ){
          if( t->i_col!=i_col || dbsql_stricmp(t->coll, coll) ){
            match = 0;
            break;
          }
          t = t->link;
        }
      }
    }
    __idx_finalize(&rc, info);

    if( rc==DBSQL_OK && match ){
      dbsql_finalize(idx_list);
      return 1;
    }
  }
  __idx_finalize(&rc, idx_list);

  *p_rc = rc;
  return 0;
}

/* Callback for dbsql_exec() with query with leading count(*) column.
 * The first argument is expected to be an int*, referent to be incremented
 * if that leading column is not exactly '0'.
 */
static int __count_nonzeros(void* count, int nc,
                         char* results[], char* columns[]){
  (void)columns;  /* Suppress unused parameter warning */
  if( nc>0 && (results[0][0]!='0' || results[0][1]!=0) ){
    *((int *)count) += 1;
  }
  return 0;
}

static int __idx_create_from_cons(
  __dbsqlexpert *p,
  idx_scan_t *scan,
  idx_constraint_t *eq, 
  idx_constraint_t *tail
){
  DBSQL *dbm = p->dbm;
  int rc = DBSQL_OK;
  if( (eq || tail) && 0==__idx_find_compatible(&rc, dbm, scan, eq, tail) ){
    idx_table_t *tab = scan->tab;
    char *cols = 0;
    char *idx = 0;
    idx_constraint_t *cons;
    unsigned int h = 0;
    const char *fmt;

    for(cons=eq; cons; cons=cons->link){
      cols = __idx_append_col_defn(&rc, cols, tab, cons);
    }
    for(cons=tail; cons; cons=cons->link){
      cols = __idx_append_col_defn(&rc, cols, tab, cons);
    }

    if( rc==DBSQL_OK ){
      /* hash_t the list of columns to come up with a name for the index */
      const char *table = scan->tab->name;
      int quote_table = __idx_identifier_requires_quotes(table);
      char *name = 0;          /* index_t name */
      int collisions = 0;
      do{
        int i;
        char *find;
        for(i=0; cols[i]; i++){
          h += ((h<<3) + cols[i]);
        }
        dbsql_free(name);
        name = dbsql_mprintf("%s_idx_%08x", table, h);
        if( name==0 ) break;
        /* Is is unique among table, view and index names? */
        fmt = "SELECT count(*) FROM dbsql_schema WHERE name=%Q"
          " AND type in ('index','table','view')";
        find = dbsql_mprintf(fmt, name);
        i = 0;
        rc = dbsql_exec(dbm, find, __count_nonzeros, &i, 0);
        assert(rc==DBSQL_OK);
        dbsql_free(find);
        if( i==0 ){
          collisions = 0;
          break;
        }
        ++collisions;
      }while( collisions<50 && name!=0 );
      if( collisions ){
        /* This return means "Gave up trying to find a unique index name." */
        rc = DBSQL_BUSY_TIMEOUT;
      }else if( name==0 ){
        rc = DBSQL_NOMEM;
      }else{
        if( quote_table ){
          fmt = "CREATE INDEX \"%w\" ON \"%w\"(%s)";
        }else{
          fmt = "CREATE INDEX %s ON %s(%s)";
        }
        idx = dbsql_mprintf(fmt, name, table, cols);
        if( !idx ){
          rc = DBSQL_NOMEM;
        }else{
          rc = dbsql_exec(dbm, idx, 0, 0, p->errmsg);
          if( rc!=DBSQL_OK ){
            rc = DBSQL_BUSY_TIMEOUT;
          }else{
            __idx_hash_add(&rc, &p->h_idx, name, idx);
          }
        }
        dbsql_free(name);
        dbsql_free(idx);
      }
    }

    dbsql_free(cols);
  }
  return rc;
}

/*
** Return true if list list (linked by idx_constraint_t.link) contains
** a constraint compatible with *p. Otherwise return false.
*/
static int __idx_find_constraint(idx_constraint_t *list, idx_constraint_t *p){
  idx_constraint_t *cmp;
  for(cmp=list; cmp; cmp=cmp->link){
    if( p->i_col==cmp->i_col ) return 1;
  }
  return 0;
}

static int __idx_create_from_where(
  __dbsqlexpert *p, 
  idx_scan_t *scan,                 /* Create indexes for this scan */
  idx_constraint_t *tail            /* range/ORDER BY constraints for inclusion */
){
  idx_constraint_t *p1 = 0;
  idx_constraint_t *con;
  int rc;

  /* Gather up all the == constraints. */
  for(con=scan->eq; con; con=con->next){
    if( !__idx_find_constraint(p1, con) && !__idx_find_constraint(tail, con) ){
      con->link = p1;
      p1 = con;
    }
  }

  /* Create an index using the == constraints collected above. And the
  ** range constraint/ORDER BY terms passed in by the caller, if any. */
  rc = __idx_create_from_cons(p, scan, p1, tail);

  /* If no range/ORDER BY passed by the caller, create a version of the
  ** index for each range constraint.  */
  if( tail==0 ){
    for(con=scan->range; rc==DBSQL_OK && con; con=con->next){
      assert( con->link==0 );
      if( !__idx_find_constraint(p1, con) && !__idx_find_constraint(tail, con) ){
        rc = __idx_create_from_cons(p, scan, p1, con);
      }
    }
  }

  return rc;
}

/*
** Create candidate indexes in database [dbm] based on the data in 
** linked-list scan.
*/
static int __idx_create_candidates(__dbsqlexpert *p){
  int rc = DBSQL_OK;
  idx_scan_t *iter;

  for(iter=p->scan; iter && rc==DBSQL_OK; iter=iter->next_scan){
    rc = __idx_create_from_where(p, iter, 0);
    if( rc==DBSQL_OK && iter->order ){
      rc = __idx_create_from_where(p, iter, iter->order);
    }
  }

  return rc;
}

/*
** Free all elements of the linked list starting at constraint.
*/
static void __idx_constraint_free(idx_constraint_t *constraint){
  idx_constraint_t *next;
  idx_constraint_t *p;

  for(p=constraint; p; p=next){
    next = p->next;
    dbsql_free(p);
  }
}

/*
** Free all elements of the linked list starting from scan up until last
** (last is not freed).
*/
static void __idx_scan_free(idx_scan_t *scan, idx_scan_t *last){
  idx_scan_t *p;
  idx_scan_t *next;
  for(p=scan; p!=last; p=next){
    next = p->next_scan;
    __idx_constraint_free(p->order);
    __idx_constraint_free(p->eq);
    __idx_constraint_free(p->range);
    dbsql_free(p);
  }
}

/*
** Free all elements of the linked list starting from statement up 
** until last (last is not freed).
*/
static void __idx_statement_free(idx_statement_t *statement, idx_statement_t *last){
  idx_statement_t *p;
  idx_statement_t *next;
  for(p=statement; p!=last; p=next){
    next = p->next;
    dbsql_free(p->eqp);
    dbsql_free(p->idx);
    dbsql_free(p);
  }
}

/*
** Free the linked list of idx_table_t objects starting at tab.
*/
static void __idx_table_free(idx_table_t *tab){
  idx_table_t *iter;
  idx_table_t *next;
  for(iter=tab; iter; iter=next){
    next = iter->next;
    dbsql_free(iter);
  }
}

/*
** Free the linked list of idx_write_t objects starting at tab.
*/
static void __idx_write_free(idx_write_t *tab){
  idx_write_t *iter;
  idx_write_t *next;
  for(iter=tab; iter; iter=next){
    next = iter->next;
    dbsql_free(iter);
  }
}



/*
** This function is called after candidate indexes have been created. It
** runs all the queries to see which indexes they prefer, and populates
** idx_statement_t.idx and idx_statement_t.eqp with the results.
*/
static int __idx_find_indexes(
  __dbsqlexpert *p,
  char **err_p                         /* OUT: Error message (dbsql_malloc) */
){
  idx_statement_t *stmt;
  DBSQL *dbm = p->dbm;
  int rc = DBSQL_OK;

  idx_hash_t h_idx;
  __idx_hash_init(&h_idx);

  for(stmt=p->statement; rc==DBSQL_OK && stmt; stmt=stmt->next){
    idx_hash_entry_t *entry;
    dbsql_stmt_t *explain = 0;
    __idx_hash_clear(&h_idx);
    rc = __idx_printf_prepare_stmt(dbm, &explain, err_p,
        "EXPLAIN QUERY PLAN %s", stmt->z_sql
    );
    while( rc==DBSQL_OK && dbsql_step(explain)==DBSQL_ROW ){
      /* int i_id = dbsql_column_int(explain, 0); */
      /* int i_parent = dbsql_column_int(explain, 1); */
      /* int i_not_used = dbsql_column_int(explain, 2); */
      const char *detail = (const char*)dbsql_column_text(explain, 3);
      int n_detail;
      int i;

      if( !detail ) continue;
      n_detail = STRLEN(detail);

      for(i=0; i<n_detail; i++){
        const char *idx = 0;
        if( i+13<n_detail && memcmp(&detail[i], " USING INDEX ", 13)==0 ){
          idx = &detail[i+13];
        }else if( i+22<n_detail 
            && memcmp(&detail[i], " USING COVERING INDEX ", 22)==0 
        ){
          idx = &detail[i+22];
        }
        if( idx ){
          const char *sql;
          int n_idx = 0;
          while( idx[n_idx]!='\0' && (idx[n_idx]!=' ' || idx[n_idx+1]!='(') ){
            n_idx++;
          }
          sql = __idx_hash_search(&p->h_idx, idx, n_idx);
          if( sql ){
            __idx_hash_add(&rc, &h_idx, sql, 0);
            if( rc ) goto find_indexes_out;
          }
          break;
        }
      }

      if( detail[0]!='-' ){
        stmt->eqp = __idx_append_text(&rc, stmt->eqp, "%s\n", detail);
      }
    }

    for(entry=h_idx.first; entry; entry=entry->next){
      stmt->idx = __idx_append_text(&rc, stmt->idx, "%s;\n", entry->key);
    }

    __idx_finalize(&rc, explain);
  }

 find_indexes_out:
  __idx_hash_clear(&h_idx);
  return rc;
}

static int __idx_auth_callback(
  void *ctx,
  int op,
  const char *z3,
  const char *z4,
  const char *db,
  const char *trigger
){
  int rc = DBSQL_OK;
  (void)z4;
  (void)trigger;
  if( op==DBSQL_INSERT || op==DBSQL_UPDATE || op==DBSQL_DELETE ){
    if( dbsql_stricmp(db, "main")==0 ){
      __dbsqlexpert *p = (__dbsqlexpert*)ctx;
      idx_table_t *tab;
      for(tab=p->table; tab; tab=tab->next){
        if( 0==dbsql_stricmp(z3, tab->name) ) break;
      }
      if( tab ){
        idx_write_t *p_write;
        for(p_write=p->p_write; p_write; p_write=p_write->next){
          if( p_write->tab==tab && p_write->op==op ) break;
        }
        if( p_write==0 ){
          p_write = __idx_malloc(&rc, sizeof(idx_write_t));
          if( rc==DBSQL_OK ){
            p_write->tab = tab;
            p_write->op = op;
            p_write->next = p->p_write;
            p->p_write = p_write;
          }
        }
      }
    }
  }
  return rc;
}

static int __idx_process_one_trigger(
  __dbsqlexpert *p, 
  idx_write_t *p_write, 
  char **err_p
){
  static const char *z_int = UNIQUE_TABLE_NAME;
  static const char *drop = "DROP TABLE " UNIQUE_TABLE_NAME;
  idx_table_t *tab = p_write->tab;
  const char *z_tab = tab->name;
  const char *sql = 
    "SELECT 'CREATE TEMP' || substr(sql, 7) FROM dbsql_schema "
    "WHERE tbl_name = %Q AND type IN ('table', 'trigger') "
    "ORDER BY type;";
  dbsql_stmt_t *p_select = 0;
  int rc = DBSQL_OK;
  char *z_write = 0;

  /* Create the table and its triggers in the temp schema */
  rc = __idx_printf_prepare_stmt(p->db, &p_select, err_p, sql, z_tab, z_tab);
  while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(p_select) ){
    const char *create = (const char*)dbsql_column_text(p_select, 0);
    if( create==0 ) continue;
    rc = dbsql_exec(p->dbv, create, 0, 0, err_p);
  }
  __idx_finalize(&rc, p_select);

  /* Rename the table in the temp schema to z_int */
  if( rc==DBSQL_OK ){
    char *z = dbsql_mprintf("ALTER TABLE temp.%Q RENAME TO %Q", z_tab, z_int);
    if( z==0 ){
      rc = DBSQL_NOMEM;
    }else{
      rc = dbsql_exec(p->dbv, z, 0, 0, err_p);
      dbsql_free(z);
    }
  }

  switch( p_write->op ){
    case DBSQL_INSERT: {
      int i;
      z_write = __idx_append_text(&rc, z_write, "INSERT INTO %Q VALUES(", z_int);
      for(i=0; i<tab->n_col; i++){
        z_write = __idx_append_text(&rc, z_write, "%s?", i==0 ? "" : ", ");
      }
      z_write = __idx_append_text(&rc, z_write, ")");
      break;
    }
    case DBSQL_UPDATE: {
      int i;
      z_write = __idx_append_text(&rc, z_write, "UPDATE %Q SET ", z_int);
      for(i=0; i<tab->n_col; i++){
        z_write = __idx_append_text(&rc, z_write, "%s%Q=?", i==0 ? "" : ", ", 
            tab->cols[i].name
        );
      }
      break;
    }
    default: {
      assert( p_write->op==DBSQL_DELETE );
      if( rc==DBSQL_OK ){
        z_write = dbsql_mprintf("DELETE FROM %Q", z_int);
        if( z_write==0 ) rc = DBSQL_NOMEM;
      }
    }
  }

  if( rc==DBSQL_OK ){
    dbsql_stmt_t *x = 0;
    rc = dbsql_prepare_v2(p->dbv, z_write, -1, &x, 0);
    __idx_finalize(&rc, x);
    if( rc!=DBSQL_OK ){
      __idx_database_error(p->dbv, err_p);
    }
  }
  dbsql_free(z_write);

  if( rc==DBSQL_OK ){
    rc = dbsql_exec(p->dbv, drop, 0, 0, err_p);
  }

  return rc;
}

static int __idx_process_triggers(__dbsqlexpert *p, char **err_p){
  int rc = DBSQL_OK;
  idx_write_t *end = 0;
  idx_write_t *first = p->p_write;

  while( rc==DBSQL_OK && first!=end ){
    idx_write_t *iter;
    for(iter=first; rc==DBSQL_OK && iter!=end; iter=iter->next){
      rc = __idx_process_one_trigger(p, iter, err_p);
    }
    end = first;
    first = p->p_write;
  }

  return rc;
}

/*
** This function tests if the schema of the main database of database handle
** db contains an object named tab. Assuming no error occurs, output parameter
** (*pb_contains) is set to true if tab exists, or false if it does not.
**
** Or, if an error occurs, an dbsql error code is returned. The final value
** of (*pb_contains) is undefined in this case.
*/
static int __expert_db_contains_object(
  DBSQL *db, 
  const char *tab, 
  int *pb_contains                 /* OUT: True if object exists */
){
  const char *z_sql = "SELECT 1 FROM dbsql_schema WHERE name = ?";
  dbsql_stmt_t *sql = 0;
  int rc = DBSQL_OK;
  int ret = 0;

  rc = dbsql_prepare_v2(db, z_sql, -1, &sql, 0);
  if( rc==DBSQL_OK ){
    dbsql_bind_text(sql, 1, tab, -1, DBSQL_STATIC);
    if( DBSQL_ROW==dbsql_step(sql) ){
      ret = 1;
    }
    rc = dbsql_finalize(sql);
  }

  *pb_contains = ret;
  return rc;
}

/*
** Execute SQL command sql using database handle db. If no error occurs,
** set (*err_p) to NULL and return DBSQL_OK. 
**
** If an error does occur, return an dbsql error code and set (*err_p) to
** point to a buffer containing an English language error message. Except,
** if the error message begins with "no such module:", then ignore the
** error and return as if the SQL statement had succeeded.
**
** This is used to copy as much of the database schema as possible while 
** ignoring any errors related to missing virtual table modules.
*/
static int __expert_schema_sql(DBSQL *db, const char *sql, char **err_p){
  int rc = DBSQL_OK;
  char *err = 0;

  rc = dbsql_exec(db, sql, 0, 0, &err);
  if( rc!=DBSQL_OK && err ){
    int n_err = STRLEN(err);
    if( n_err>=15 && memcmp(err, "no such module:", 15)==0 ){
      dbsql_free(err);
      rc = DBSQL_OK;
      err = 0;
    }
  }

  *err_p = err;
  return rc;
}

static int __idx_create_vtab_schema(__dbsqlexpert *p, char **errmsg){
  int rc = __idx_register_vtab(p);
  dbsql_stmt_t *schema = 0;

  /* For each table in the main db schema:
  **
  **   1) Add an entry to the p->table list, and
  **   2) Create the equivalent virtual table in dbv.
  */
  rc = __idx_prepare_stmt(p->db, &schema, errmsg,
      "SELECT type, name, sql, 1, "
      "       substr(sql,1,14)=='create virtual' COLLATE nocase "
      "FROM dbsql_schema "
      "WHERE type IN ('table','view') AND "
      "      substr(name,1,7)!='dbsql_' COLLATE nocase "
      " UNION ALL "
      "SELECT type, name, sql, 2, 0 FROM dbsql_schema "
      "WHERE type = 'trigger'"
      "  AND tbl_name IN(SELECT name FROM dbsql_schema WHERE type = 'view') "
      "ORDER BY 4, 5 DESC, 1"
  );
  while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(schema) ){
    const char *type = (const char*)dbsql_column_text(schema, 0);
    const char *name = (const char*)dbsql_column_text(schema, 1);
    const char *sql = (const char*)dbsql_column_text(schema, 2);
    int b_virtual = dbsql_column_int(schema, 4);
    int exists = 0;

    if( type==0 || name==0 ) continue;
    rc = __expert_db_contains_object(p->dbv, name, &exists);
    if( rc || exists ) continue;

    if( type[0]=='v' || type[1]=='r' || b_virtual ){
      /* A view. Or a trigger on a view. */
      if( sql ) rc = __expert_schema_sql(p->dbv, sql, errmsg);
    }else{
      idx_table_t *tab;
      rc = __idx_get_table_info(p->db, name, &tab, errmsg);
      if( rc==DBSQL_OK && ALWAYS(tab!=0) ){
        int i;
        char *inner = 0;
        char *outer = 0;
        tab->next = p->table;
        p->table = tab;

        /* The statement the vtab will pass to dbsql_declare_vtab() */
        inner = __idx_append_text(&rc, 0, "CREATE TABLE x(");
        for(i=0; i<tab->n_col; i++){
          inner = __idx_append_text(&rc, inner, "%s%Q COLLATE %s", 
              (i==0 ? "" : ", "), tab->cols[i].name, tab->cols[i].coll
          );
        }
        inner = __idx_append_text(&rc, inner, ")");

        /* The CVT statement to create the vtab */
        outer = __idx_append_text(&rc, 0, 
            "CREATE VIRTUAL TABLE %Q USING expert(%Q)", name, inner
        );
        if( rc==DBSQL_OK ){
          rc = dbsql_exec(p->dbv, outer, 0, 0, errmsg);
        }
        dbsql_free(inner);
        dbsql_free(outer);
      }
    }
  }
  __idx_finalize(&rc, schema);
  return rc;
}

struct idx_sample_ctx_t {
  int i_target;
  double target;                  /* Target n_ret/n_row value */
  double n_row;                    /* Number of rows seen */
  double n_ret;                    /* Number of rows returned */
};

static void __idx_sample_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  struct idx_sample_ctx_t *p = (struct idx_sample_ctx_t*)dbsql_user_data(ctx);
  int ret;

  (void)argv;
  assert( argc==0 );
  if( p->n_row==0.0 ){
    ret = 1;
  }else{
    ret = (p->n_ret / p->n_row) <= p->target;
    if( ret==0 ){
      unsigned short rnd;
      dbsql_randomness(2, (void*)&rnd);
      ret = ((int)rnd % 100) <= p->i_target;
    }
  }

  dbsql_result_int(ctx, ret);
  p->n_row += 1.0;
  p->n_ret += (double)ret;
}

struct idx_rem_ctx_t {
  int n_slot;
  struct idx_rem_slot_t {
    int type;                    /* DBSQL_NULL, INTEGER, REAL, TEXT, BLOB */
    int64_t i_val;                     /* DBSQL_INTEGER value */
    double r_val;                  /* DBSQL_FLOAT value */
    int64_t n_byte;                    /* Bytes of space allocated at z */
    int64_t n;                        /* Size of buffer z */
    char *z;                      /* DBSQL_TEXT/BLOB value */
  } slots[1];
};

/*
** Implementation of scalar function dbsql_expert_rem().
*/
static void __idx_rem_func(
  dbsql_func_t *ctx,
  int argc,
  dbsql_value_t **argv
){
  struct idx_rem_ctx_t *p = (struct idx_rem_ctx_t*)dbsql_user_data(ctx);
  struct idx_rem_slot_t *slot;
  int i_slot;
  assert( argc==2 );

  i_slot = dbsql_value_int(argv[0]);
  assert( i_slot<p->n_slot );
  slot = &p->slots[i_slot];

  switch( slot->type ){
    case DBSQL_NULL:
      /* no-op */
      break;

    case DBSQL_INTEGER:
      dbsql_result_int64(ctx, slot->i_val);
      break;

    case DBSQL_FLOAT:
      dbsql_result_double(ctx, slot->r_val);
      break;

    case DBSQL_BLOB:
      assert( slot->n <= 0x7fffffff );
      dbsql_result_blob(ctx, slot->z, (int)slot->n, DBSQL_TRANSIENT);
      break;

    case DBSQL_TEXT:
      assert( slot->n <= 0x7fffffff );
      dbsql_result_text(ctx, slot->z, (int)slot->n, DBSQL_TRANSIENT);
      break;
  }

  slot->type = dbsql_value_type(argv[1]);
  switch( slot->type ){
    case DBSQL_NULL:
      /* no-op */
      break;

    case DBSQL_INTEGER:
      slot->i_val = dbsql_value_int64(argv[1]);
      break;

    case DBSQL_FLOAT:
      slot->r_val = dbsql_value_double(argv[1]);
      break;

    case DBSQL_BLOB:
    case DBSQL_TEXT: {
      int64_t n_byte = dbsql_value_bytes(argv[1]);
      const void *data = 0;
      if( n_byte>slot->n_byte ){
        char *z_new = (char*)dbsql_realloc64(slot->z, n_byte*2);
        if( z_new==0 ){
          dbsql_result_error_nomem(ctx);
          return;
        }
        slot->n_byte = n_byte*2;
        slot->z = z_new;
      }
      slot->n = n_byte;
      if( slot->type==DBSQL_BLOB ){
        data = dbsql_value_blob(argv[1]);
        if( data ) memcpy(slot->z, data, n_byte);
      }else{
        data = dbsql_value_text(argv[1]);
        memcpy(slot->z, data, n_byte);
      }
      break;
    }
  }
}

static int __idx_largest_index(DBSQL *db, int *pn_max, char **err_p){
  int rc = DBSQL_OK;
  const char *z_max = 
    "SELECT max(i.seqno) FROM "
    "  dbsql_schema AS s, "
    "  pragma_index_list(s.name) AS l, "
    "  pragma_index_info(l.name) AS i "
    "WHERE s.type = 'table'";
  dbsql_stmt_t *max = 0;

  *pn_max = 0;
  rc = __idx_prepare_stmt(db, &max, err_p, z_max);
  if( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(max) ){
    *pn_max = dbsql_column_int(max, 0) + 1;
  }
  __idx_finalize(&rc, max);

  return rc;
}

static int __idx_populate_one_stat1(
  __dbsqlexpert *p,
  dbsql_stmt_t *index_x_info,
  dbsql_stmt_t *write_stat,
  const char *tab,
  const char *idx,
  char **err_p
){
  char *cols = 0;
  char *order = 0;
  char *z_query = 0;
  int n_col = 0;
  int i;
  dbsql_stmt_t *query = 0;
  int64_t *stats = 0;
  int rc = DBSQL_OK;

  assert( p->i_sample>0 );

  /* Formulate the query text */
  dbsql_bind_text(index_x_info, 1, idx, -1, DBSQL_STATIC);
  while( DBSQL_OK==rc && DBSQL_ROW==dbsql_step(index_x_info) ){
    const char *comma = cols==0 ? "" : ", ";
    const char *name = (const char*)dbsql_column_text(index_x_info, 0);
    const char *coll = (const char*)dbsql_column_text(index_x_info, 1);
    if( name==0 ){
      /* This index contains an expression. Ignore it. */
      dbsql_free(cols);
      dbsql_free(order);
      return dbsql_reset(index_x_info);
    }
    cols = __idx_append_text(&rc, cols, 
        "%sx.%Q IS dbsql_expert_rem(%d, x.%Q) COLLATE %s", 
        comma, name, n_col, name, coll
    );
    order = __idx_append_text(&rc, order, "%s%d", comma, ++n_col);
  }
  dbsql_reset(index_x_info);
  if( rc==DBSQL_OK ){
    if( p->i_sample==100 ){
      z_query = dbsql_mprintf(
          "SELECT %s FROM %Q x ORDER BY %s", cols, tab, order
      );
    }else{
      z_query = dbsql_mprintf(
          "SELECT %s FROM temp."UNIQUE_TABLE_NAME" x ORDER BY %s", cols, order
      );
    }
  }
  dbsql_free(cols);
  dbsql_free(order);

  /* Formulate the query text */
  if( rc==DBSQL_OK ){
    DBSQL *dbrem = (p->i_sample==100 ? p->db : p->dbv);
    rc = __idx_prepare_stmt(dbrem, &query, err_p, z_query);
  }
  dbsql_free(z_query);

  if( rc==DBSQL_OK ){
    stats = (int64_t*)__idx_malloc(&rc, sizeof(int64_t)*(n_col+1));
  }
  if( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(query) ){
    idx_hash_entry_t *entry;
    char *z_stat = 0;
    for(i=0; i<=n_col; i++) stats[i] = 1;
    while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(query) ){
      stats[0]++;
      for(i=0; i<n_col; i++){
        if( dbsql_column_int(query, i)==0 ) break;
      }
      for(/*no-op*/; i<n_col; i++){
        stats[i+1]++;
      }
    }

    if( rc==DBSQL_OK ){
      int64_t s0 = stats[0];
      z_stat = dbsql_mprintf("%lld", s0);
      if( z_stat==0 ) rc = DBSQL_NOMEM;
      for(i=1; rc==DBSQL_OK && i<=n_col; i++){
        z_stat = __idx_append_text(&rc, z_stat, " %lld", (s0+stats[i]/2) / stats[i]);
      }
    }

    if( rc==DBSQL_OK ){
      dbsql_bind_text(write_stat, 1, tab, -1, DBSQL_STATIC);
      dbsql_bind_text(write_stat, 2, idx, -1, DBSQL_STATIC);
      dbsql_bind_text(write_stat, 3, z_stat, -1, DBSQL_STATIC);
      dbsql_step(write_stat);
      rc = dbsql_reset(write_stat);
    }

    entry = __idx_hash_find(&p->h_idx, idx, STRLEN(idx));
    if( entry ){
      assert( entry->val2==0 );
      entry->val2 = z_stat;
    }else{
      dbsql_free(z_stat);
    }
  }
  dbsql_free(stats);
  __idx_finalize(&rc, query);

  return rc;
}

static int __idx_build_sample_table(__dbsqlexpert *p, const char *tab){
  int rc;
  char *sql;

  rc = dbsql_exec(p->dbv,"DROP TABLE IF EXISTS temp."UNIQUE_TABLE_NAME,0,0,0);
  if( rc!=DBSQL_OK ) return rc;

  sql = dbsql_mprintf(
      "CREATE TABLE temp." UNIQUE_TABLE_NAME " AS SELECT * FROM %Q", tab
  );
  if( sql==0 ) return DBSQL_NOMEM;
  rc = dbsql_exec(p->dbv, sql, 0, 0, 0);
  dbsql_free(sql);

  return rc;
}

/*
** This function is called as part of dbsql_expert_analyze(). Candidate
** indexes have already been created in database sqlite3expert.dbm, this
** function populates dbsql_stat1 table in the same database.
**
** The stat1 data is generated by querying the 
*/
static int __idx_populate_stat1(__dbsqlexpert *p, char **err_p){
  int rc = DBSQL_OK;
  int n_max =0;
  struct idx_rem_ctx_t *ctx = 0;
  struct idx_sample_ctx_t samplectx; 
  int i;
  int64_t i_prev = -100000;
  dbsql_stmt_t *all_index = 0;
  dbsql_stmt_t *index_x_info = 0;
  dbsql_stmt_t *p_write = 0;

  const char *z_all_index =
    "SELECT s.rowid, s.name, l.name FROM "
    "  dbsql_schema AS s, "
    "  pragma_index_list(s.name) AS l "
    "WHERE s.type = 'table'";
  const char *z_index_x_info = 
    "SELECT name, coll FROM pragma_index_xinfo(?) WHERE key";
  const char *z_write = "INSERT INTO dbsql_stat1 VALUES(?, ?, ?)";

  /* If i_sample==0, no dbsql_stat1 data is required. */
  if( p->i_sample==0 ) return DBSQL_OK;

  rc = __idx_largest_index(p->dbm, &n_max, err_p);
  if( n_max<=0 || rc!=DBSQL_OK ) return rc;

  rc = dbsql_exec(p->dbm, "ANALYZE; PRAGMA writable_schema=1", 0, 0, 0);

  if( rc==DBSQL_OK ){
    int64_t n_byte = sizeof(struct idx_rem_ctx_t) + (sizeof(struct idx_rem_slot_t) * n_max);
    ctx = (struct idx_rem_ctx_t*)__idx_malloc(&rc, n_byte);
  }

  if( rc==DBSQL_OK ){
    DBSQL *dbrem = (p->i_sample==100 ? p->db : p->dbv);
    rc = dbsql_create_function(dbrem, "dbsql_expert_rem", 
        2, DBSQL_UTF8, (void*)ctx, __idx_rem_func, 0, 0
    );
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(p->db, "dbsql_expert_sample", 
        0, DBSQL_UTF8, (void*)&samplectx, __idx_sample_func, 0, 0
    );
  }

  if( rc==DBSQL_OK ){
    ctx->n_slot = (int64_t)n_max+1;
    rc = __idx_prepare_stmt(p->dbm, &all_index, err_p, z_all_index);
  }
  if( rc==DBSQL_OK ){
    rc = __idx_prepare_stmt(p->dbm, &index_x_info, err_p, z_index_x_info);
  }
  if( rc==DBSQL_OK ){
    rc = __idx_prepare_stmt(p->dbm, &p_write, err_p, z_write);
  }

  while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(all_index) ){
    int64_t i_rowid = dbsql_column_int64(all_index, 0);
    const char *tab = (const char*)dbsql_column_text(all_index, 1);
    const char *idx = (const char*)dbsql_column_text(all_index, 2);
    if( tab==0 || idx==0 ) continue;
    if( p->i_sample<100 && i_prev!=i_rowid ){
      samplectx.target = (double)p->i_sample / 100.0;
      samplectx.i_target = p->i_sample;
      samplectx.n_row = 0.0;
      samplectx.n_ret = 0.0;
      rc = __idx_build_sample_table(p, tab);
      if( rc!=DBSQL_OK ) break;
    }
    rc = __idx_populate_one_stat1(p, index_x_info, p_write, tab, idx, err_p);
    i_prev = i_rowid;
  }
  if( rc==DBSQL_OK && p->i_sample<100 ){
    rc = dbsql_exec(p->dbv, 
        "DROP TABLE IF EXISTS temp." UNIQUE_TABLE_NAME, 0,0,0
    );
  }

  __idx_finalize(&rc, all_index);
  __idx_finalize(&rc, index_x_info);
  __idx_finalize(&rc, p_write);

  if( ctx ){
    for(i=0; i<ctx->n_slot; i++){
      dbsql_free(ctx->slots[i].z);
    }
    dbsql_free(ctx);
  }

  if( rc==DBSQL_OK ){
    rc = dbsql_exec(p->dbm, "ANALYZE dbsql_schema", 0, 0, 0);
  }

  dbsql_create_function(p->db, "dbsql_expert_rem", 2, DBSQL_UTF8, 0,0,0,0);
  dbsql_create_function(p->db, "dbsql_expert_sample", 0,DBSQL_UTF8,0,0,0,0);

  dbsql_exec(p->db, "DROP TABLE IF EXISTS temp."UNIQUE_TABLE_NAME,0,0,0);
  return rc;
}

/*
** Define and possibly pretend to use a useless collation sequence.
** This pretense allows expert to accept SQL using custom collations.
*/
int __dummy_compare(void *up1, int up2, const void *up3, int up4, const void *up5){
  (void)up1;
  (void)up2;
  (void)up3;
  (void)up4;
  (void)up5;
  assert(0); /* VDBE should never be run. */
  return 0;
}
/* And a callback to register above upon actual need */
void __use_dummy_cs(void *up1, DBSQL *db, int etr, const char *name){
  (void)up1;
  dbsql_create_collation_v2(db, name, etr, 0, __dummy_compare, 0);
}

#if !defined(DBSQL_OMIT_SCHEMA_PRAGMAS) \
  && !defined(DBSQL_OMIT_INTROSPECTION_PRAGMAS)
/*
** dummy functions for no-op implementation of UDFs during expert's work
*/
void __dummy_udf(dbsql_func_t *up1, int up2, dbsql_value_t **up3){
  (void)up1;
  (void)up2;
  (void)up3;
  assert(0); /* VDBE should never be run. */
}
void __dummy_ud_fvalue(dbsql_func_t *up1){
  (void)up1;
  assert(0); /* VDBE should never be run. */
}

/*
** Register UDFs from user database with another.
*/
int __register_ud_fs(DBSQL *db_src, DBSQL *db_dst){
  dbsql_stmt_t *stmt;
  int rc = dbsql_prepare_v2(db_src,
            "SELECT name,type,enc,narg,flags "
            "FROM pragma_function_list() "
            "WHERE builtin==0", -1, &stmt, 0);
  if( rc==DBSQL_OK ){
    while( DBSQL_ROW==(rc = dbsql_step(stmt)) ){
      int nargs = dbsql_column_int(stmt,3);
      int flags = dbsql_column_int(stmt,4);
      const char *name = (char*)dbsql_column_text(stmt,0);
      const char *type = (char*)dbsql_column_text(stmt,1);
      const char *enc = (char*)dbsql_column_text(stmt,2);
      if( name==0 || type==0 || enc==0 ){
        /* no-op.  Only happens on OOM */
      }else{
        int ienc = DBSQL_UTF8;
        int rcf = DBSQL_ERROR;
        if( strcmp(enc,"utf16le")==0 ) ienc = DBSQL_UTF16LE;
        else if( strcmp(enc,"utf16be")==0 ) ienc = DBSQL_UTF16BE;
        ienc |= (flags & (DBSQL_DETERMINISTIC|DBSQL_DIRECTONLY));
        if( strcmp(type,"w")==0 ){
          rcf = dbsql_create_window_function(db_dst,name,nargs,ienc,0,
                                               __dummy_udf,__dummy_ud_fvalue,0,0,0);
        }else if( strcmp(type,"a")==0 ){
          rcf = dbsql_create_function(db_dst,name,nargs,ienc,0,
                                        0,__dummy_udf,__dummy_ud_fvalue);
        }else if( strcmp(type,"s")==0 ){
          rcf = dbsql_create_function(db_dst,name,nargs,ienc,0,
                                        __dummy_udf,0,0);
        }
        if( rcf!=DBSQL_OK ){
          rc = rcf;
          break;
        }
      }
    }
    dbsql_finalize(stmt);
    if( rc==DBSQL_DONE ) rc = DBSQL_OK;
  }
  return rc;
}
#endif

/*
** Allocate a new sqlite3expert object.
*/
__dbsqlexpert *dbsql_expert_new(DBSQL *db, char **errmsg){
  int rc = DBSQL_OK;
  __dbsqlexpert *p_new;

  p_new = (__dbsqlexpert*)__idx_malloc(&rc, sizeof(__dbsqlexpert));

  /* Open two in-memory databases to work with. The "vtab database" (dbv)
  ** will contain a virtual table corresponding to each real table in
  ** the user database schema, and a copy of each view. It is used to
  ** collect information regarding the WHERE, ORDER BY and other clauses
  ** of the user's query.
  */
  if( rc==DBSQL_OK ){
    p_new->db = db;
    p_new->i_sample = 100;
    rc = dbsql_open(":memory:", &p_new->dbv);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_open(":memory:", &p_new->dbm);
    if( rc==DBSQL_OK ){
      dbsql_db_config(p_new->dbm, DBSQL_DBCONFIG_TRIGGER_EQP, 1, (int*)0);
    }
  }

  /* Allow custom collations to be dealt with through prepare. */
  if( rc==DBSQL_OK ) rc = dbsql_collation_needed(p_new->dbm,0,__use_dummy_cs);
  if( rc==DBSQL_OK ) rc = dbsql_collation_needed(p_new->dbv,0,__use_dummy_cs);

#if !defined(DBSQL_OMIT_SCHEMA_PRAGMAS) \
  && !defined(DBSQL_OMIT_INTROSPECTION_PRAGMAS)
  /* Register UDFs from database [db] with [dbm] and [dbv]. */
  if( rc==DBSQL_OK ){
    rc = __register_ud_fs(p_new->db, p_new->dbm);
  }
  if( rc==DBSQL_OK ){
    rc = __register_ud_fs(p_new->db, p_new->dbv);
  }
#endif

  /* Copy the entire schema of database [db] into [dbm]. */
  if( rc==DBSQL_OK ){
    dbsql_stmt_t *sql = 0;
    rc = __idx_printf_prepare_stmt(p_new->db, &sql, errmsg, 
        "SELECT sql, name, substr(sql,1,14)=='create virtual' COLLATE nocase"
        " FROM dbsql_schema WHERE substr(name,1,7)!='dbsql_' COLLATE nocase"
        " ORDER BY 3 DESC, rowid"
    );
    while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(sql) ){
      const char *z_sql = (const char*)dbsql_column_text(sql, 0);
      const char *name = (const char*)dbsql_column_text(sql, 1);
      int exists = 0;
      rc = __expert_db_contains_object(p_new->dbm, name, &exists);
      if( rc==DBSQL_OK && z_sql && exists==0 ){
        rc = __expert_schema_sql(p_new->dbm, z_sql, errmsg);
      }
    }
    __idx_finalize(&rc, sql);
  }

  /* Create the vtab schema */
  if( rc==DBSQL_OK ){
    rc = __idx_create_vtab_schema(p_new, errmsg);
  }

  /* Register the auth callback with dbv */
  if( rc==DBSQL_OK ){
    dbsql_set_authorizer(p_new->dbv, __idx_auth_callback, (void*)p_new);
  }

  /* If an error has occurred, free the new object and return NULL. Otherwise,
  ** return the new sqlite3expert handle.  */
  if( rc!=DBSQL_OK ){
    dbsql_expert_destroy(p_new);
    p_new = 0;
  }
  return p_new;
}

/*
** Configure an sqlite3expert object.
*/
int dbsql_expert_config(__dbsqlexpert *p, int op, ...){
  int rc = DBSQL_OK;
  va_list ap;
  va_start(ap, op);
  switch( op ){
    case EXPERT_CONFIG_SAMPLE: {
      int i_val = va_arg(ap, int);
      if( i_val<0 ) i_val = 0;
      if( i_val>100 ) i_val = 100;
      p->i_sample = i_val;
      break;
    }
    default:
      rc = DBSQL_NOTFOUND;
      break;
  }

  va_end(ap);
  return rc;
}

/*
** Add an SQL statement to the analysis.
*/
int dbsql_expert_sql(
  __dbsqlexpert *p,               /* From dbsql_expert_new() */
  const char *sql,               /* SQL statement to add */
  char **err_p                    /* OUT: Error message (if any) */
){
  idx_scan_t *scan_orig = p->scan;
  idx_statement_t *stmt_orig = p->statement;
  int rc = DBSQL_OK;
  const char *z_stmt = sql;

  if( p->run ) return DBSQL_MISUSE;

  while( rc==DBSQL_OK && z_stmt && z_stmt[0] ){
    dbsql_stmt_t *stmt = 0;
    /* Ensure that the provided statement compiles against user's DB. */
    rc = __idx_prepare_stmt(p->db, &stmt, err_p, z_stmt);
    if( rc!=DBSQL_OK ) break;
    dbsql_finalize(stmt);
    rc = dbsql_prepare_v2(p->dbv, z_stmt, -1, &stmt, &z_stmt);
    if( rc==DBSQL_OK ){
      if( stmt ){
        idx_statement_t *p_new;
        const char *z = dbsql_sql(stmt);
        int64_t n = STRLEN(z);
        p_new = (idx_statement_t*)__idx_malloc(&rc, sizeof(idx_statement_t) + n+1);
        if( rc==DBSQL_OK ){
          p_new->z_sql = (char*)&p_new[1];
          memcpy(p_new->z_sql, z, n+1);
          p_new->next = p->statement;
          if( p->statement ) p_new->i_id = p->statement->i_id+1;
          p->statement = p_new;
        }
        dbsql_finalize(stmt);
      }
    }else{
      __idx_database_error(p->dbv, err_p);
    }
  }

  if( rc!=DBSQL_OK ){
    __idx_scan_free(p->scan, scan_orig);
    __idx_statement_free(p->statement, stmt_orig);
    p->scan = scan_orig;
    p->statement = stmt_orig;
  }

  return rc;
}

int dbsql_expert_analyze(__dbsqlexpert *p, char **err_p){
  int rc;
  idx_hash_entry_t *entry;

  /* Do trigger processing to collect any extra idx_scan_t structures */
  rc = __idx_process_triggers(p, err_p);

  /* Create candidate indexes within the in-memory database file */
  if( rc==DBSQL_OK ){
    rc = __idx_create_candidates(p);
  }else if ( rc==DBSQL_BUSY_TIMEOUT ){
    if( err_p )
      *err_p = dbsql_mprintf("Cannot find a unique index name to propose.");
    return rc;
  }

  /* Generate the stat1 data */
  if( rc==DBSQL_OK ){
    rc = __idx_populate_stat1(p, err_p);
  }

  /* Formulate the EXPERT_REPORT_CANDIDATES text */
  for(entry=p->h_idx.first; entry; entry=entry->next){
    p->candidates = __idx_append_text(&rc, p->candidates, 
        "%s;%s%s\n", entry->val, 
        entry->val2 ? " -- stat1: " : "", entry->val2
    );
  }

  /* Figure out which of the candidate indexes are preferred by the query
  ** planner and report the results to the user.  */
  if( rc==DBSQL_OK ){
    rc = __idx_find_indexes(p, err_p);
  }

  if( rc==DBSQL_OK ){
    p->run = 1;
  }
  return rc;
}

/*
** Return the total number of statements that have been added to this
** sqlite3expert using dbsql_expert_sql().
*/
int dbsql_expert_count(__dbsqlexpert *p){
  int n_ret = 0;
  if( p->statement ) n_ret = p->statement->i_id+1;
  return n_ret;
}

/*
** Return a component of the report.
*/
const char *dbsql_expert_report(__dbsqlexpert *p, int i_stmt, int report){
  const char *ret = 0;
  idx_statement_t *stmt;

  if( p->run==0 ) return 0;
  for(stmt=p->statement; stmt && stmt->i_id!=i_stmt; stmt=stmt->next);
  switch( report ){
    case EXPERT_REPORT_SQL:
      if( stmt ) ret = stmt->z_sql;
      break;
    case EXPERT_REPORT_INDEXES:
      if( stmt ) ret = stmt->idx;
      break;
    case EXPERT_REPORT_PLAN:
      if( stmt ) ret = stmt->eqp;
      break;
    case EXPERT_REPORT_CANDIDATES:
      ret = p->candidates;
      break;
  }
  return ret;
}

/*
** Free an sqlite3expert object.
*/
void dbsql_expert_destroy(__dbsqlexpert *p){
  if( p ){
    dbsql_close(p->dbm);
    dbsql_close(p->dbv);
    __idx_scan_free(p->scan, 0);
    __idx_statement_free(p->statement, 0);
    __idx_table_free(p->table);
    __idx_write_free(p->p_write);
    __idx_hash_clear(&p->h_idx);
    dbsql_free(p->candidates);
    dbsql_free(p);
  }
}

#endif /* ifndef DBSQL_OMIT_VIRTUALTABLE */
