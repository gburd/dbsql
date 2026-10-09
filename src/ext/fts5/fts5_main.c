/*
** 2014 Jun 09
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
** This is an dbsql module implementing full-text search.
*/


#include "fts5_int.h"

/*
** This variable is set to false when running tests for which the on disk
** structures should not be corrupt. Otherwise, true. If it is false, extra
** assert() conditions in the fts5 code are activated - conditions that are
** only true if it is guaranteed that the fts5 database is not corrupt.
*/
#ifdef DBSQL_DEBUG
int dbsql_fts5_may_be_corrupt = 1;
#endif


typedef struct fts5_auxdata_t fts5_auxdata_t;
typedef struct fts5_auxiliary_t fts5_auxiliary_t;
typedef struct fts5_cursor_t fts5_cursor_t;
typedef struct fts5_full_table_t fts5_full_table_t;
typedef struct fts5_sorter_t fts5_sorter_t;
typedef struct fts5_tokenizer_module_t fts5_tokenizer_module_t;

/*
** NOTES ON TRANSACTIONS: 
**
** dbsql invokes the following virtual table methods as transactions are 
** opened and closed by the user:
**
**     begin():    Start of a new transaction.
**     x_sync():     Initial part of two-phase commit.
**     commit():   Final part of two-phase commit.
**     rollback(): Rollback the transaction.
**
** Anything that is required as part of a commit that may fail is performed
** in the x_sync() callback. Current versions of dbsql ignore any errors 
** returned by commit().
**
** And as sub-transactions are opened/closed:
**
**     savepoint(int S):  Open savepoint S.
**     release(int S):    Commit and close savepoint S.
**     rollback_to(int S): Rollback to start of savepoint S.
**
** During a write-transaction the fts5_index.c module may cache some data 
** in-memory. It is flushed to disk whenever x_sync(), release() or
** savepoint() is called. And discarded whenever rollback() or rollback_to() 
** is called.
**
** Additionally, if DBSQL_DEBUG is defined, an instance of the following
** structure is used to record the current transaction state. This information
** is not required, but it is used in the assert() statements executed by
** function FTS5_CHECK_TRANSACTION_STATE() (see below).
*/
struct fts5_transaction_state_t {
  int state;                     /* 0==closed, 1==open, 2==synced */
  int i_savepoint;                 /* Number of open savepoints (0 -> none) */
};

/*
** A single object of this type is allocated when the FTS5 module is 
** registered with a database handle. It is used to store pointers to
** all registered FTS5 extensions - tokenizers and auxiliary functions.
*/
struct fts5_global_t {
  fts5_api api;                   /* User visible part of object (see fts5.h) */
  DBSQL *db;                    /* Associated database connection */ 
  int64_t i_next_id;                    /* Used to allocate unique cursor ids */
  fts5_auxiliary_t *aux;            /* First in list of all aux. functions */
  fts5_tokenizer_module_t *p_tok;      /* First in list of all tokenizer modules */
  fts5_tokenizer_module_t *dflt_tok;  /* Default tokenizer module */
  fts5_cursor_t *csr;               /* First in list of all open cursors */
  u_int32_t locale_hdrs[4];
};

/*
** Size of header on fts5_locale() values. And macro to access a buffer
** containing a copy of the header from an fts5_config_t pointer.
*/
#define FTS5_LOCALE_HDR_SIZE ((int)sizeof( ((fts5_global_t*)0)->locale_hdrs ))
#define FTS5_LOCALE_HDR(config) ((const u_int8_t*)(config->__p_global->locale_hdrs))

#define FTS5_INSTTOKEN_SUBTYPE 73

/*
** Each auxiliary function registered with the FTS5 module is represented
** by an object of the following type. All such objects are stored as part
** of the fts5_global_t.aux list.
*/
struct fts5_auxiliary_t {
  fts5_global_t *__p_global;            /* Global context for this function */
  char *z_func;                    /* Function name (nul-terminated) */
  void *user_data;                /* User-data pointer */
  fts5_extension_function func;  /* Callback function */
  void (*destroy)(void*);        /* Destructor function */
  fts5_auxiliary_t *next;           /* Next registered auxiliary function */
};

/*
** Each tokenizer module registered with the FTS5 module is represented
** by an object of the following type. All such objects are stored as part
** of the fts5_global_t.tok list.
**
** v2_native:
**  True if the tokenizer was registered using create_tokenizer_v2(), false
**  for create_tokenizer(). If this variable is true, then x2 is populated
**  with the routines as supplied by the caller and x1 contains synthesized
**  wrapper routines. In this case the user-data pointer passed to 
**  x1.create should be a pointer to the fts5_tokenizer_module_t structure,
**  not a copy of user_data.
**
**  Of course, if v2_native is false, then x1 contains the real routines and
**  x2 the synthesized ones. In this case a pointer to the fts5_tokenizer_module_t
**  object should be passed to x2.create.
**
**  The synthesized wrapper routines are necessary for find_tokenizer(_v2)
**  calls.
*/
struct fts5_tokenizer_module_t {
  char *name;                    /* Name of tokenizer */
  void *user_data;                /* User pointer passed to create() */
  int v2_native;                  /* True if v2 native tokenizer */
  fts5_tokenizer x1;              /* Tokenizer functions */
  fts5_tokenizer_v2 x2;           /* V2 tokenizer functions */
  void (*destroy)(void*);        /* Destructor function */
  fts5_tokenizer_module_t *next;     /* Next registered tokenizer module */
};

struct fts5_full_table_t {
  fts5_table_t p;                    /* Public class members from fts5Int.h */
  fts5_storage_t *storage;          /* Document store */
  fts5_global_t *__p_global;            /* Global (connection wide) data */
  fts5_cursor_t *sort_csr;           /* Sort data from this cursor */
  int i_savepoint;                 /* Successful savepoint()+1 */

#ifdef DBSQL_DEBUG
  struct fts5_transaction_state_t ts;
#endif
};

struct fts5_match_phrase_t {
  fts5_buffer_t *poslist;           /* Pointer to current poslist */
  int n_term;                      /* Size of phrase in terms */
};

/*
** stmt:
**   SELECT rowid, <fts> FROM <fts> ORDER BY +rank;
**
** idx[]:
**   There is one entry in the idx[] array for each phrase in the query,
**   the value of which is the offset within poslists[] following the last 
**   byte of the position list for the corresponding phrase.
*/
struct fts5_sorter_t {
  dbsql_stmt_t *stmt;
  int64_t i_rowid;                     /* Current rowid */
  const u_int8_t *poslists;             /* Position lists for current row */
  int n_idx;                       /* Number of entries in idx[] */
  int idx[FLEXARRAY];            /* Offsets into poslists for current row */
};

/* Size (int bytes) of an fts5_sorter_t object with N indexes */
#define SZ_FTS5SORTER(N) (offsetof(fts5_sorter_t,n_idx)+((N+2)/2)*sizeof(int64_t))

/*
** Virtual-table cursor object.
**
** i_special:
**   If this is a 'special' query (refer to function __fts5_special_match()), 
**   then this variable contains the result of the query. 
**
** i_first_rowid, i_last_rowid:
**   These variables are only used for FTS5_PLAN_MATCH cursors. Assuming the
**   cursor iterates in ascending order of rowids, i_first_rowid is the lower
**   limit of rowids to return, and i_last_rowid the upper. In other words, the
**   WHERE clause in the user's query might have been:
**
**       <tbl> MATCH <expr> AND rowid BETWEEN $i_first_rowid AND $i_last_rowid
**
**   If the cursor iterates in descending order of rowid, i_first_rowid
**   is the upper limit (i.e. the "first" rowid visited) and i_last_rowid
**   the lower.
*/
struct fts5_cursor_t {
  dbsql_vtab_cursor_t base;       /* Base class used by dbsql core */
  fts5_cursor_t *next;              /* Next cursor in fts5_cursor_t.csr list */
  int *column_sizes;               /* Values for column_size() */
  int64_t i_csr_id;                     /* Cursor id */

  /* Zero from this point onwards on cursor reset */
  int plan;                      /* FTS5_PLAN_XXX value */
  int desc;                      /* True for "ORDER BY rowid DESC" queries */
  int64_t i_first_rowid;                /* Return no rowids earlier than this */
  int64_t i_last_rowid;                 /* Return no rowids later than this */
  dbsql_stmt_t *stmt;            /* Statement used to read %_content */
  fts5_expr_t *expr;                /* Expression for MATCH queries */
  fts5_sorter_t *sorter;            /* Sorter for "ORDER BY rank" queries */
  int csrflags;                   /* Mask of cursor flags (see below) */
  int64_t i_special;                   /* Result of special query */

  /* "rank" function. Populated on demand from vtab.column(). */
  char *z_rank;                    /* Custom rank function */
  char *z_rank_args;                /* Custom rank function args */
  fts5_auxiliary_t *rank;           /* Rank callback (or NULL) */
  int n_rank_arg;                   /* Number of trailing arguments for rank() */
  dbsql_value_t **rank_args;      /* Array of trailing arguments */
  dbsql_stmt_t *rank_arg_stmt;     /* Origin of objects in rank_args[] */

  /* Auxiliary data storage */
  fts5_auxiliary_t *aux;            /* Currently executing extension function */
  fts5_auxdata_t *auxdata;          /* First in linked list of saved aux-data */

  /* Cache used by auxiliary API functions inst() and inst_count() */
  fts5_poslist_reader_t *inst_iters;   /* One for each phrase */
  int n_inst_alloc;                 /* Size of insts[] array (entries / 3) */
  int n_inst_count;                 /* Number of phrase instances */
  int *insts;                     /* 3 integers per phrase instance */
};

/*
** Bits that make up the "idx_num" parameter passed indirectly by 
** best_index() to filter().
*/
#define FTS5_BI_MATCH        0x0001         /* <tbl> MATCH ? */
#define FTS5_BI_RANK         0x0002         /* rank MATCH ? */
#define FTS5_BI_ROWID_EQ     0x0004         /* rowid == ? */
#define FTS5_BI_ROWID_LE     0x0008         /* rowid <= ? */
#define FTS5_BI_ROWID_GE     0x0010         /* rowid >= ? */

#define FTS5_BI_ORDER_RANK   0x0020
#define FTS5_BI_ORDER_ROWID  0x0040
#define FTS5_BI_ORDER_DESC   0x0080

/*
** Values for fts5_cursor_t.csrflags
*/
#define FTS5CSR_EOF               0x01
#define FTS5CSR_REQUIRE_CONTENT   0x02
#define FTS5CSR_REQUIRE_DOCSIZE   0x04
#define FTS5CSR_REQUIRE_INST      0x08
#define FTS5CSR_FREE_ZRANK        0x10
#define FTS5CSR_REQUIRE_RESEEK    0x20
#define FTS5CSR_REQUIRE_POSLIST   0x40

#define BIT_FLAG_ALL_TEST(x,y) (((x) & (y))==(y))
#define BIT_FLAG_TEST(x,y)    (((x) & (y))!=0)


/*
** Macros to Set(), Clear() and Test() cursor flags.
*/
#define CSR_FLAG_SET(csr, flag)   ((csr)->csrflags |= (flag))
#define CSR_FLAG_CLEAR(csr, flag) ((csr)->csrflags &= ~(flag))
#define CSR_FLAG_TEST(csr, flag)  ((csr)->csrflags & (flag))

struct fts5_auxdata_t {
  fts5_auxiliary_t *aux;            /* Extension to which this belongs */
  void *ptr;                     /* Pointer value */
  void(*x_delete)(void*);          /* Destructor */
  fts5_auxdata_t *next;             /* Next object in linked list */
};

#ifdef DBSQL_DEBUG
#define FTS5_BEGIN      1
#define FTS5_SYNC       2
#define FTS5_COMMIT     3
#define FTS5_ROLLBACK   4
#define FTS5_SAVEPOINT  5
#define FTS5_RELEASE    6
#define FTS5_ROLLBACKTO 7
static void FTS5_CHECK_TRANSACTION_STATE(fts5_full_table_t *p, int op, int i_savepoint){
  switch( op ){
    case FTS5_BEGIN:
      assert( p->ts.state==0 );
      p->ts.state = 1;
      p->ts.i_savepoint = -1;
      break;

    case FTS5_SYNC:
      assert( p->ts.state==1 || p->ts.state==2 );
      p->ts.state = 2;
      break;

    case FTS5_COMMIT:
      assert( p->ts.state==2 );
      p->ts.state = 0;
      break;

    case FTS5_ROLLBACK:
      assert( p->ts.state==1 || p->ts.state==2 || p->ts.state==0 );
      p->ts.state = 0;
      break;

    case FTS5_SAVEPOINT:
      assert( p->ts.state>=1 );
      assert( i_savepoint>=0 );
      assert( i_savepoint>=p->ts.i_savepoint );
      p->ts.i_savepoint = i_savepoint;
      break;
      
    case FTS5_RELEASE:
      assert( p->ts.state>=1 );
      assert( i_savepoint>=0 );
      assert( i_savepoint<=p->ts.i_savepoint );
      p->ts.i_savepoint = i_savepoint-1;
      break;

    case FTS5_ROLLBACKTO:
      assert( p->ts.state>=1 );
      assert( i_savepoint>=-1 );
      /* The following assert() can fail if another vtab strikes an error
      ** within an savepoint() call then dbsql calls rollback_to() - without
      ** having called savepoint() on this vtab.  */
      /* assert( i_savepoint<=p->ts.i_savepoint ); */
      p->ts.i_savepoint = i_savepoint;
      break;
  }
}
#else
# define FTS5_CHECK_TRANSACTION_STATE(x,y,z)
#endif

/*
** Return true if tab is a contentless table. If parameter include_unindexed
** is true, this includes contentless tables that store UNINDEXED columns
** only.
*/
static int __fts5_is_contentless(fts5_full_table_t *tab, int include_unindexed){
  int content = tab->p.config->content;
  return (
    content==FTS5_CONTENT_NONE 
    || (include_unindexed && content==FTS5_CONTENT_UNINDEXED)
  );
}

/*
** Delete a virtual table handle allocated by __fts5_init_vtab(). 
*/
static void __fts5_free_vtab(fts5_full_table_t *tab){
  if( tab ){
    __fts5_index_close(tab->p.p_index);
    __fts5_storage_close(tab->storage);
    __fts5_config_free(tab->p.config);
    dbsql_free(tab);
  }
}

/*
** The disconnect() virtual table method.
*/
static int __fts5_disconnect_method(dbsql_vtab_t *vtab){
  __fts5_free_vtab((fts5_full_table_t*)vtab);
  return DBSQL_OK;
}

/*
** The destroy() virtual table method.
*/
static int __fts5_destroy_method(dbsql_vtab_t *vtab){
  fts5_table_t *tab = (fts5_table_t*)vtab;
  int rc = __fts5_drop_all(tab->config);
  if( rc==DBSQL_OK ){
    __fts5_free_vtab((fts5_full_table_t*)vtab);
  }
  return rc;
}

/*
** This function is the implementation of both the connect and create
** methods of the FTS3 virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fts5")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> "column name" and other module argument fields.
*/
static int __fts5_init_vtab(
  int create,                    /* True for create, false for connect */
  DBSQL *db,                    /* The dbsql database connection */
  void *aux,                     /* hash_t table containing tokenizers */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* create/connect argument array */
  dbsql_vtab_t **v_tab,          /* Write the resulting vtab structure here */
  char **err_p                    /* Write any error message here */
){
  fts5_global_t *__p_global = (fts5_global_t*)aux;
  const char **configs = (const char**)argv;
  int rc = DBSQL_OK;             /* Return code */
  fts5_config_t *config = 0;        /* Results of parsing argc/argv */
  fts5_full_table_t *tab = 0;        /* New virtual table object */

  /* Allocate the new vtab object and parse the configuration */
  tab = (fts5_full_table_t*)__fts5_malloc_zero(&rc, sizeof(fts5_full_table_t));
  if( rc==DBSQL_OK ){
    rc = __fts5_config_parse(__p_global, db, argc, configs, &config, err_p);
    assert( (rc==DBSQL_OK && *err_p==0) || config==0 );
  }
  if( rc==DBSQL_OK ){
    config->errmsg = err_p;
    tab->p.config = config;
    tab->__p_global = __p_global;
    if( create || __fts5_tokenizer_preload(&config->t) ){
      rc = __fts5_load_tokenizer(config);
    }
  }

  /* Open the index sub-system */
  if( rc==DBSQL_OK ){
    rc = __fts5_index_open(config, create, &tab->p.p_index, err_p);
  }

  /* Open the storage sub-system */
  if( rc==DBSQL_OK ){
    rc = __fts5_storage_open(
        config, tab->p.p_index, create, &tab->storage, err_p
    );
  }

  /* Call dbsql_declare_vtab() */
  if( rc==DBSQL_OK ){
    rc = __fts5_config_declare_vtab(config);
  }

  /* Load the initial configuration */
  if( rc==DBSQL_OK ){
    rc = __fts5_config_load(tab->p.config, tab->p.config->i_cookie-1);
  }

  if( rc==DBSQL_OK && config->content==FTS5_CONTENT_NORMAL ){
    rc = dbsql_vtab_config(db, DBSQL_VTAB_CONSTRAINT_SUPPORT, (int)1);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_vtab_config(db, DBSQL_VTAB_INNOCUOUS);
  }

  if( config ) config->errmsg = 0;
  if( rc!=DBSQL_OK ){
    __fts5_free_vtab(tab);
    tab = 0;
  }else if( create ){
    FTS5_CHECK_TRANSACTION_STATE(tab, FTS5_BEGIN, 0);
  }
  *v_tab = (dbsql_vtab_t*)tab;
  return rc;
}

/*
** The connect() and create() methods for the virtual table. All the
** work is done in function __fts5_init_vtab().
*/
static int __fts5_connect_method(
  DBSQL *db,                    /* Database connection */
  void *aux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* create/connect argument array */
  dbsql_vtab_t **vtab,          /* OUT: New dbsql_vtab_t object */
  char **err_p                    /* OUT: dbsql_malloc'd error message */
){
  return __fts5_init_vtab(0, db, aux, argc, argv, vtab, err_p);
}
static int __fts5_create_method(
  DBSQL *db,                    /* Database connection */
  void *aux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* create/connect argument array */
  dbsql_vtab_t **vtab,          /* OUT: New dbsql_vtab_t object */
  char **err_p                    /* OUT: dbsql_malloc'd error message */
){
  return __fts5_init_vtab(1, db, aux, argc, argv, vtab, err_p);
}

/*
** The different query plans.
*/
#define FTS5_PLAN_MATCH          1       /* (<tbl> MATCH ?) */
#define FTS5_PLAN_SOURCE         2       /* A source cursor for SORTED_MATCH */
#define FTS5_PLAN_SPECIAL        3       /* An internal query */
#define FTS5_PLAN_SORTED_MATCH   4       /* (<tbl> MATCH ? ORDER BY rank) */
#define FTS5_PLAN_SCAN           5       /* No usable constraint */
#define FTS5_PLAN_ROWID          6       /* (rowid = ?) */

/*
** Set the DBSQL_INDEX_SCAN_UNIQUE flag in idx_info->flags. Unless this
** extension is currently being used by a version of dbsql too old to
** support index-info flags. In that case this function is a no-op.
*/
static void __fts5_set_unique_flag(dbsql_index_info_t *idx_info){
#if DBSQL_VERSION_NUMBER>=3008012
#ifndef DBSQL_CORE
  if( dbsql_libversion_number()>=3008012 )
#endif
  {
    idx_info->idx_flags |= DBSQL_INDEX_SCAN_UNIQUE;
  }
#endif
}

static void __fts5_set_estimated_rows(dbsql_index_info_t *idx_info, int64_t n_row){
#if DBSQL_VERSION_NUMBER>=3008002
#ifndef DBSQL_CORE
  if( dbsql_libversion_number()>=3008002 )
#endif
  {
    idx_info->estimated_rows = MAX(1, n_row);
  }
#endif
}

static int __fts5_use_pattern_match(
  fts5_config_t *config, 
  struct dbsql_index_constraint_t *p
){
  assert( FTS5_PATTERN_GLOB==DBSQL_INDEX_CONSTRAINT_GLOB );
  assert( FTS5_PATTERN_LIKE==DBSQL_INDEX_CONSTRAINT_LIKE );
  if( config->t.pattern==FTS5_PATTERN_GLOB && p->op==FTS5_PATTERN_GLOB ){
    return 1;
  }
  if( config->t.pattern==FTS5_PATTERN_LIKE 
   && (p->op==FTS5_PATTERN_LIKE || p->op==FTS5_PATTERN_GLOB)
  ){
    return 1;
  }
  return 0;
}

/*
** Implementation of the best_index method for FTS5 tables. Within the 
** WHERE constraint, it searches for the following:
**
**   1. A MATCH constraint against the table column.
**   2. A MATCH constraint against the "rank" column.
**   3. A MATCH constraint against some other column.
**   4. An == constraint against the rowid column.
**   5. A < or <= constraint against the rowid column.
**   6. A > or >= constraint against the rowid column.
**
** Within the ORDER BY, the following are supported:
**
**   5. ORDER BY rank [ASC|DESC]
**   6. ORDER BY rowid [ASC|DESC]
**
** Information for the filter call is passed via both the idx_num and 
** idx_str variables. Specifically, idx_num is a bitmask of the following
** flags used to encode the ORDER BY clause:
**
**     FTS5_BI_ORDER_RANK
**     FTS5_BI_ORDER_ROWID
**     FTS5_BI_ORDER_DESC
**
** idx_str is used to encode data from the WHERE clause. For each argument
** passed to the filter method, the following is appended to idx_str:
**
**   Match against table column:            "m"
**   Match against rank column:             "r"
**   Match against other column:            "M<column-number>"
**   LIKE  against other column:            "L<column-number>"
**   GLOB  against other column:            "G<column-number>"
**   Equality constraint against the rowid: "="
**   A < or <= against the rowid:           "<"
**   A > or >= against the rowid:           ">"
**
** This function ensures that there is at most one "r" or "=". And that if
** there exists an "=" then there is no "<" or ">".
**
** If an unusable MATCH operator is present in the WHERE clause, then
** DBSQL_CONSTRAINT is returned.
**
** Costs are assigned as follows:
**
**  a) If a MATCH operator is present, the cost depends on the other
**     constraints also present. As follows:
**
**       * No other constraints:         cost=50000.0
**       * One rowid range constraint:   cost=37500.0
**       * Both rowid range constraints: cost=30000.0
**       * An == rowid constraint:       cost=25000.0
**
**  b) Otherwise, if there is no MATCH:
**
**       * No other constraints:         cost=3000000.0
**       * One rowid range constraints:  cost=2250000.0
**       * Both rowid range constraint:  cost=750000.0
**       * An == rowid constraint:       cost=25.0
**
** Costs are not modified by the ORDER BY clause.
**
** The ratios used in case (a) are based on informal results obtained from 
** the tool/fts5cost.tcl script. The "MATCH and ==" combination has the
** cost set quite high because the query may be a prefix query. Unless
** there is a prefix index, prefix queries with rowid constraints are much 
** more expensive than non-prefix queries with rowid constraints.
**
** The estimated rows returned is set to the cost/40. For simple queries,
** experimental results show that cost/4 might be about right. But for
** more complex queries that use multiple terms the number of rows might
** be far fewer than this. So we compromise and use cost/40.
*/
static int __fts5_best_index_method(dbsql_vtab_t *v_tab, dbsql_index_info_t *info){
  fts5_table_t *tab = (fts5_table_t*)v_tab;
  fts5_config_t *config = tab->config;
  const int n_col = config->n_col;
  int idx_flags = 0;               /* Parameter passed through to filter() */
  int i;

  char *idx_str;
  int i_idx_str = 0;
  int i_cons = 0;

  int seen_eq = 0;
  int seen_gt = 0;
  int seen_lt = 0;
  int n_seen_match = 0;
  int seen_rank = 0;


  assert( DBSQL_INDEX_CONSTRAINT_EQ<DBSQL_INDEX_CONSTRAINT_MATCH );
  assert( DBSQL_INDEX_CONSTRAINT_GT<DBSQL_INDEX_CONSTRAINT_MATCH );
  assert( DBSQL_INDEX_CONSTRAINT_LE<DBSQL_INDEX_CONSTRAINT_MATCH );
  assert( DBSQL_INDEX_CONSTRAINT_GE<DBSQL_INDEX_CONSTRAINT_MATCH );
  assert( DBSQL_INDEX_CONSTRAINT_LE<DBSQL_INDEX_CONSTRAINT_MATCH );

  if( config->lock ){
    tab->base.errmsg = dbsql_mprintf(
        "recursively defined fts5 content table"
    );
    return DBSQL_ERROR;
  }

  idx_str = (char*)dbsql_malloc64((int64_t)info->n_constraint * 8 + 1);
  if( idx_str==0 ) return DBSQL_NOMEM;
  info->idx_str = idx_str;
  info->need_to_free_idx_str = 1;

  for(i=0; i<info->n_constraint; i++){
    struct dbsql_index_constraint_t *p = &info->constraints[i];
    int i_col = p->i_column;
    if( p->op==DBSQL_INDEX_CONSTRAINT_MATCH
     || (p->op==DBSQL_INDEX_CONSTRAINT_EQ && i_col>=n_col)
    ){
      /* A MATCH operator or equivalent */
      if( p->usable==0 || i_col<0 ){
        /* As there exists an unusable MATCH constraint this is an 
        ** unusable plan. Return DBSQL_CONSTRAINT. */
        idx_str[i_idx_str] = 0;
        return DBSQL_CONSTRAINT;
      }else{
        if( i_col==n_col+1 ){
          if( seen_rank ) continue;
          idx_str[i_idx_str++] = 'r';
          seen_rank = 1;
        }else{
          n_seen_match++;
          idx_str[i_idx_str++] = 'M';
          dbsql_snprintf(6, &idx_str[i_idx_str], "%d", i_col);
          i_idx_str += (int)strlen(&idx_str[i_idx_str]);
          assert( idx_str[i_idx_str]=='\0' );
        }
        info->constraint_usages[i].argv_index = ++i_cons;
        info->constraint_usages[i].omit = 1;
      }
    }else if( p->usable ){
      if( i_col>=0 && i_col<n_col && __fts5_use_pattern_match(config, p) ){
        assert( p->op==FTS5_PATTERN_LIKE || p->op==FTS5_PATTERN_GLOB );
        idx_str[i_idx_str++] = p->op==FTS5_PATTERN_LIKE ? 'L' : 'G';
        dbsql_snprintf(6, &idx_str[i_idx_str], "%d", i_col);
        idx_str += strlen(&idx_str[i_idx_str]);
        info->constraint_usages[i].argv_index = ++i_cons;
        assert( idx_str[i_idx_str]=='\0' );
        n_seen_match++;
      }else if( seen_eq==0 && p->op==DBSQL_INDEX_CONSTRAINT_EQ && i_col<0 ){
        idx_str[i_idx_str++] = '=';
        seen_eq = 1;
        info->constraint_usages[i].argv_index = ++i_cons;
        info->constraint_usages[i].omit = 1;
      }
    }
  }

  if( seen_eq==0 ){
    for(i=0; i<info->n_constraint; i++){
      struct dbsql_index_constraint_t *p = &info->constraints[i];
      if( p->i_column<0 && p->usable ){
        int op = p->op;
        if( op==DBSQL_INDEX_CONSTRAINT_LT || op==DBSQL_INDEX_CONSTRAINT_LE ){
          if( seen_lt ) continue;
          idx_str[i_idx_str++] = '<';
          info->constraint_usages[i].argv_index = ++i_cons;
          seen_lt = 1;
        }else
        if( op==DBSQL_INDEX_CONSTRAINT_GT || op==DBSQL_INDEX_CONSTRAINT_GE ){
          if( seen_gt ) continue;
          idx_str[i_idx_str++] = '>';
          info->constraint_usages[i].argv_index = ++i_cons;
          seen_gt = 1;
        }
      }
    }
  }
  idx_str[i_idx_str] = '\0';

  /* Set idx_flags flags for the ORDER BY clause
  **
  ** Note that tokendata=1 tables cannot currently handle "ORDER BY rowid DESC".
  */
  if( info->n_order_by==1 ){
    int i_sort = info->order_bies[0].i_column;
    if( i_sort==(config->n_col+1) && n_seen_match>0 ){
      idx_flags |= FTS5_BI_ORDER_RANK;
    }else if( i_sort==-1 && (!info->order_bies[0].desc || !config->tokendata) ){
      idx_flags |= FTS5_BI_ORDER_ROWID;
    }
    if( BIT_FLAG_TEST(idx_flags, FTS5_BI_ORDER_RANK|FTS5_BI_ORDER_ROWID) ){
      info->order_by_consumed = 1;
      if( info->order_bies[0].desc ){
        idx_flags |= FTS5_BI_ORDER_DESC;
      }
    }
  }

  /* Calculate the estimated cost based on the flags set in idx_flags. */
  if( seen_eq ){
    info->estimated_cost = n_seen_match ? 25000.0 : 25.0;
    __fts5_set_estimated_rows(info, 1);
    __fts5_set_unique_flag(info);
  }else{
    int64_t n_est_rows;
    if( n_seen_match ){
      if( seen_lt && seen_gt ){
        info->estimated_cost = 50000.0;
      }else if( seen_lt || seen_gt ){
        info->estimated_cost = 37500.0;
      }else{
        info->estimated_cost = 50000.0;
      }
      n_est_rows = (int64_t)(info->estimated_cost / 40.0);
      for(i=1; i<n_seen_match; i++){
        info->estimated_cost *= 2.5;
        n_est_rows = n_est_rows / 2;
      }
    }else{
      if( seen_lt && seen_gt ){
        info->estimated_cost = 750000.0;
      }else if( seen_lt || seen_gt ){
        info->estimated_cost = 2250000.0;
      }else{
        info->estimated_cost = 3000000.0;
      }
      n_est_rows = (int64_t)(info->estimated_cost / 4.0);
    }
    __fts5_set_estimated_rows(info, n_est_rows);
  }

  info->idx_num = idx_flags;
  return DBSQL_OK;
}

static int __fts5_new_transaction(fts5_full_table_t *tab){
  fts5_cursor_t *csr;
  for(csr=tab->__p_global->csr; csr; csr=csr->next){
    if( csr->base.vtab==(dbsql_vtab_t*)tab ) return DBSQL_OK;
  }
  return __fts5_storage_reset(tab->storage);
}

/*
** Implementation of x_open method.
*/
static int __fts5_open_method(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **pp_csr){
  fts5_full_table_t *tab = (fts5_full_table_t*)v_tab;
  fts5_config_t *config = tab->p.config;
  fts5_cursor_t *csr = 0;           /* New cursor object */
  int64_t n_byte;            /* Bytes of space to allocate */
  int rc;                         /* Return code */

  rc = __fts5_new_transaction(tab);
  if( rc==DBSQL_OK ){
    n_byte = sizeof(fts5_cursor_t) + config->n_col * sizeof(int);
    csr = (fts5_cursor_t*)dbsql_malloc64(n_byte);
    if( csr ){
      fts5_global_t *__p_global = tab->__p_global;
      memset(csr, 0, (size_t)n_byte);
      csr->column_sizes = (int*)&csr[1];
      csr->next = __p_global->csr;
      __p_global->csr = csr;
      csr->i_csr_id = ++__p_global->i_next_id;
    }else{
      rc = DBSQL_NOMEM;
    }
  }
  *pp_csr = (dbsql_vtab_cursor_t*)csr;
  return rc;
}

static int __fts5_stmt_type(fts5_cursor_t *csr){
  if( csr->plan==FTS5_PLAN_SCAN ){
    return (csr->desc) ? FTS5_STMT_SCAN_DESC : FTS5_STMT_SCAN_ASC;
  }
  return FTS5_STMT_LOOKUP;
}

/*
** This function is called after the cursor passed as the only argument
** is moved to point at a different row. It clears all cached data 
** specific to the previous row stored by the cursor object.
*/
static void __fts5_csr_newrow(fts5_cursor_t *csr){
  CSR_FLAG_SET(csr, 
      FTS5CSR_REQUIRE_CONTENT 
    | FTS5CSR_REQUIRE_DOCSIZE 
    | FTS5CSR_REQUIRE_INST 
    | FTS5CSR_REQUIRE_POSLIST 
  );
}

static void __fts5_free_cursor_components(fts5_cursor_t *csr){
  fts5_full_table_t *tab = (fts5_full_table_t*)(csr->base.vtab);
  fts5_auxdata_t *data;
  fts5_auxdata_t *next;

  dbsql_free(csr->inst_iters);
  dbsql_free(csr->insts);
  if( csr->stmt ){
    int stmt = __fts5_stmt_type(csr);
    __fts5_storage_stmt_release(tab->storage, stmt, csr->stmt);
  }
  if( csr->sorter ){
    fts5_sorter_t *sorter = csr->sorter;
    dbsql_finalize(sorter->stmt);
    dbsql_free(sorter);
  }

  if( csr->plan!=FTS5_PLAN_SOURCE ){
    __fts5_expr_free(csr->expr);
  }

  for(data=csr->auxdata; data; data=next){
    next = data->next;
    if( data->x_delete ) data->x_delete(data->ptr);
    dbsql_free(data);
  }

  dbsql_finalize(csr->rank_arg_stmt);
  dbsql_free(csr->rank_args);

  if( CSR_FLAG_TEST(csr, FTS5CSR_FREE_ZRANK) ){
    dbsql_free(csr->z_rank);
    dbsql_free(csr->z_rank_args);
  }

  __fts5_index_close_reader(tab->p.p_index);
  memset(&csr->plan, 0, sizeof(fts5_cursor_t) - ((u_int8_t*)&csr->plan - (u_int8_t*)csr));
}


/*
** Close the cursor.  For additional information see the documentation
** on the x_close method of the virtual table interface.
*/
static int __fts5_close_method(dbsql_vtab_cursor_t *cursor){
  if( cursor ){
    fts5_full_table_t *tab = (fts5_full_table_t*)(cursor->vtab);
    fts5_cursor_t *csr = (fts5_cursor_t*)cursor;
    fts5_cursor_t **pp;

    __fts5_free_cursor_components(csr);
    /* Remove the cursor from the fts5_global_t.csr list */
    for(pp=&tab->__p_global->csr; (*pp)!=csr; pp=&(*pp)->next);
    *pp = csr->next;

    dbsql_free(csr);
  }
  return DBSQL_OK;
}

static int __fts5_sorter_next(fts5_cursor_t *csr){
  fts5_sorter_t *sorter = csr->sorter;
  int rc;

  rc = dbsql_step(sorter->stmt);
  if( rc==DBSQL_DONE ){
    rc = DBSQL_OK;
    CSR_FLAG_SET(csr, FTS5CSR_EOF|FTS5CSR_REQUIRE_CONTENT);
  }else if( rc==DBSQL_ROW ){
    const u_int8_t *a;
    const u_int8_t *blobs;
    int n_blob;
    int i;
    int i_off = 0;
    rc = DBSQL_OK;

    sorter->i_rowid = dbsql_column_int64(sorter->stmt, 0);
    n_blob = dbsql_column_bytes(sorter->stmt, 1);
    blobs = a = dbsql_column_blob(sorter->stmt, 1);

    /* n_blob==0 in detail=none mode. */
    if( n_blob>0 ){
      for(i=0; i<(sorter->n_idx-1); i++){
        int i_val;
        a += FTS5_GET_VARINT32(a, i_val);
        i_off += i_val;
        sorter->idx[i] = i_off;
      }
      sorter->idx[i] = &blobs[n_blob] - a;
      sorter->poslists = a;
    }

    __fts5_csr_newrow(csr);
  }

  return rc;
}


/*
** Set the FTS5CSR_REQUIRE_RESEEK flag on all FTS5_PLAN_MATCH cursors 
** open on table tab.
*/
static void __fts5_trip_cursors(fts5_full_table_t *tab){
  fts5_cursor_t *csr;
  for(csr=tab->__p_global->csr; csr; csr=csr->next){
    if( csr->plan==FTS5_PLAN_MATCH
     && csr->base.vtab==(dbsql_vtab_t*)tab 
    ){
      CSR_FLAG_SET(csr, FTS5CSR_REQUIRE_RESEEK);
    }
  }
}

/*
** If the REQUIRE_RESEEK flag is set on the cursor passed as the first
** argument, close and reopen all fts5_index_iter_t iterators that the cursor 
** is using. Then attempt to move the cursor to a rowid equal to or laster
** (in the cursors sort order - ASC or DESC) than the current rowid. 
**
** If the new rowid is not equal to the old, set output parameter *pb_skip
** to 1 before returning. Otherwise, leave it unchanged.
**
** Return DBSQL_OK if successful or if no reseek was required, or an 
** error code if an error occurred.
*/
static int __fts5_cursor_reseek(fts5_cursor_t *csr, int *pb_skip){
  int rc = DBSQL_OK;
  assert( *pb_skip==0 );
  if( CSR_FLAG_TEST(csr, FTS5CSR_REQUIRE_RESEEK) ){
    fts5_full_table_t *tab = (fts5_full_table_t*)(csr->base.vtab);
    int desc = csr->desc;
    int64_t i_rowid = __fts5_expr_rowid(csr->expr);

    rc = __fts5_expr_first(
        csr->expr, tab->p.p_index, i_rowid, csr->i_last_rowid, desc
    );
    if( rc==DBSQL_OK &&  i_rowid!=__fts5_expr_rowid(csr->expr) ){
      *pb_skip = 1;
    }

    CSR_FLAG_CLEAR(csr, FTS5CSR_REQUIRE_RESEEK);
    __fts5_csr_newrow(csr);
    if( __fts5_expr_eof(csr->expr) ){
      CSR_FLAG_SET(csr, FTS5CSR_EOF);
      *pb_skip = 1;
    }
  }
  return rc;
}


/*
** Advance the cursor to the next row in the table that matches the 
** search criteria.
**
** Return DBSQL_OK if nothing goes wrong.  DBSQL_OK is returned
** even if we reach end-of-file.  The __fts5_eof_method() will be called
** subsequently to determine whether or not an EOF was hit.
*/
static int __fts5_next_method(dbsql_vtab_cursor_t *cursor){
  fts5_cursor_t *csr = (fts5_cursor_t*)cursor;
  int rc;

  assert( (csr->plan<3)==
          (csr->plan==FTS5_PLAN_MATCH || csr->plan==FTS5_PLAN_SOURCE) 
  );
  assert( !CSR_FLAG_TEST(csr, FTS5CSR_EOF) );

  /* If this cursor uses FTS5_PLAN_MATCH and this is a tokendata=1 table,
  ** clear any token mappings accumulated at the fts5_index.c level. In
  ** other cases, specifically FTS5_PLAN_SOURCE and FTS5_PLAN_SORTED_MATCH,
  ** we need to retain the mappings for the entire query.  */
  if( csr->plan==FTS5_PLAN_MATCH 
   && ((fts5_table_t*)cursor->vtab)->config->tokendata 
  ){
    __fts5_expr_clear_tokens(csr->expr);
  }

  if( csr->plan<3 ){
    int skip = 0;
    if( (rc = __fts5_cursor_reseek(csr, &skip)) || skip ) return rc;
    rc = __fts5_expr_next(csr->expr, csr->i_last_rowid);
    CSR_FLAG_SET(csr, __fts5_expr_eof(csr->expr));
    __fts5_csr_newrow(csr);
  }else{
    switch( csr->plan ){
      case FTS5_PLAN_SPECIAL: {
        CSR_FLAG_SET(csr, FTS5CSR_EOF);
        rc = DBSQL_OK;
        break;
      }
  
      case FTS5_PLAN_SORTED_MATCH: {
        rc = __fts5_sorter_next(csr);
        break;
      }
  
      default: {
        fts5_config_t *config = ((fts5_table_t*)cursor->vtab)->config;
        config->lock++;
        rc = dbsql_step(csr->stmt);
        config->lock--;
        if( rc!=DBSQL_ROW ){
          CSR_FLAG_SET(csr, FTS5CSR_EOF);
          rc = dbsql_reset(csr->stmt);
          if( rc!=DBSQL_OK ){
            cursor->vtab->errmsg = dbsql_mprintf(
                "%s", dbsql_errmsg(config->db)
            );
          }
        }else{
          rc = DBSQL_OK;
          CSR_FLAG_SET(csr, FTS5CSR_REQUIRE_DOCSIZE);
        }
        break;
      }
    }
  }
  
  return rc;
}


static int __fts5_prepare_statement(
  dbsql_stmt_t **stmt,
  fts5_config_t *config, 
  const char *fmt,
  ...
){
  dbsql_stmt_t *ret = 0;
  int rc;
  char *sql;
  va_list ap;

  va_start(ap, fmt);
  sql = dbsql_vmprintf(fmt, ap);
  if( sql==0 ){
    rc = DBSQL_NOMEM; 
  }else{
    rc = dbsql_prepare_v3(config->db, sql, -1, 
                            DBSQL_PREPARE_PERSISTENT, &ret, 0);
    if( rc!=DBSQL_OK ){
      __fts5_config_errmsg(config, "%s", dbsql_errmsg(config->db));
    }
    dbsql_free(sql);
  }

  va_end(ap);
  *stmt = ret;
  return rc;
} 

static int __fts5_cursor_first_sorted(
  fts5_full_table_t *tab, 
  fts5_cursor_t *csr, 
  int desc
){
  fts5_config_t *config = tab->p.config;
  fts5_sorter_t *sorter;
  int n_phrase;
  int64_t n_byte;
  int rc;
  const char *rank = csr->z_rank;
  const char *rank_args = csr->z_rank_args;
  
  n_phrase = __fts5_expr_phrase_count(csr->expr);
  n_byte = SZ_FTS5SORTER(n_phrase);
  sorter = (fts5_sorter_t*)dbsql_malloc64(n_byte);
  if( sorter==0 ) return DBSQL_NOMEM;
  memset(sorter, 0, (size_t)n_byte);
  sorter->n_idx = n_phrase;

  /* TODO: It would be better to have some system for reusing statement
  ** handles here, rather than preparing a new one for each query. But that
  ** is not possible as dbsql reference counts the virtual table objects.
  ** And since the statement required here reads from this very virtual 
  ** table, saving it creates a circular reference.
  **
  ** If dbsql a built-in statement cache, this wouldn't be a problem. */
  rc = __fts5_prepare_statement(&sorter->stmt, config,
      "SELECT rowid, rank FROM %Q.%Q ORDER BY %s(\"%w\"%s%s) %s",
      config->z_db, config->name, rank, config->name,
      (rank_args ? ", " : ""),
      (rank_args ? rank_args : ""),
      desc ? "DESC" : "ASC"
  );

  csr->sorter = sorter;
  if( rc==DBSQL_OK ){
    assert( tab->sort_csr==0 );
    tab->sort_csr = csr;
    rc = __fts5_sorter_next(csr);
    tab->sort_csr = 0;
  }

  if( rc!=DBSQL_OK ){
    dbsql_finalize(sorter->stmt);
    dbsql_free(sorter);
    csr->sorter = 0;
  }

  return rc;
}

static int __fts5_cursor_first(fts5_full_table_t *tab, fts5_cursor_t *csr, int desc){
  int rc;
  fts5_expr_t *expr = csr->expr;
  rc = __fts5_expr_first(
      expr, tab->p.p_index, csr->i_first_rowid, csr->i_last_rowid, desc
  );
  if( __fts5_expr_eof(expr) ){
    CSR_FLAG_SET(csr, FTS5CSR_EOF);
  }
  __fts5_csr_newrow(csr);
  return rc;
}

/*
** Process a "special" query. A special query is identified as one with a
** MATCH expression that begins with a '*' character. The remainder of
** the text passed to the MATCH operator are used as  the special query
** parameters.
*/
static int __fts5_special_match(
  fts5_full_table_t *tab, 
  fts5_cursor_t *csr, 
  const char *query
){
  int rc = DBSQL_OK;             /* Return code */
  const char *z = query;         /* Special query text */
  int n;                          /* Number of bytes in text at z */

  while( z[0]==' ' ) z++;
  for(n=0; z[n] && z[n]!=' '; n++);

  assert( tab->p.base.errmsg==0 );
  csr->plan = FTS5_PLAN_SPECIAL;

  if( n==5 && 0==dbsql_strnicmp("reads", z, n) ){
    csr->i_special = __fts5_index_reads(tab->p.p_index);
  }
  else if( n==2 && 0==dbsql_strnicmp("id", z, n) ){
    csr->i_special = csr->i_csr_id;
  }
  else{
    /* An unrecognized directive. Return an error message. */
    tab->p.base.errmsg = dbsql_mprintf("unknown special query: %.*s", n, z);
    rc = DBSQL_ERROR;
  }

  return rc;
}

/*
** Search for an auxiliary function named name that can be used with table
** tab. If one is found, return a pointer to the corresponding fts5_auxiliary_t
** structure. Otherwise, if no such function exists, return NULL.
*/
static fts5_auxiliary_t *__fts5_find_auxiliary(fts5_full_table_t *tab, const char *name){
  fts5_auxiliary_t *aux;

  for(aux=tab->__p_global->aux; aux; aux=aux->next){
    if( dbsql_stricmp(name, aux->z_func)==0 ) return aux;
  }

  /* No function of the specified name was found. Return 0. */
  return 0;
}


static int __fts5_find_rank_function(fts5_cursor_t *csr){
  fts5_full_table_t *tab = (fts5_full_table_t*)(csr->base.vtab);
  fts5_config_t *config = tab->p.config;
  int rc = DBSQL_OK;
  fts5_auxiliary_t *aux = 0;
  const char *rank = csr->z_rank;
  const char *rank_args = csr->z_rank_args;

  if( rank_args ){
    char *sql = __fts5_mprintf(&rc, "SELECT %s", rank_args);
    if( sql ){
      dbsql_stmt_t *stmt = 0;
      rc = dbsql_prepare_v3(config->db, sql, -1,
                              DBSQL_PREPARE_PERSISTENT, &stmt, 0);
      dbsql_free(sql);
      assert( rc==DBSQL_OK || csr->rank_arg_stmt==0 );
      if( rc==DBSQL_OK ){
        if( DBSQL_ROW==dbsql_step(stmt) ){
          int64_t n_byte;
          csr->n_rank_arg = dbsql_column_count(stmt);
          n_byte = sizeof(dbsql_value_t*)*csr->n_rank_arg;
          csr->rank_args = (dbsql_value_t**)__fts5_malloc_zero(&rc, n_byte);
          if( rc==DBSQL_OK ){
            int i;
            for(i=0; i<csr->n_rank_arg; i++){
              csr->rank_args[i] = dbsql_column_value(stmt, i);
            }
          }
          csr->rank_arg_stmt = stmt;
        }else{
          rc = dbsql_finalize(stmt);
          assert( rc!=DBSQL_OK );
        }
      }
    }
  }

  if( rc==DBSQL_OK ){
    aux = __fts5_find_auxiliary(tab, rank);
    if( aux==0 ){
      assert( tab->p.base.errmsg==0 );
      tab->p.base.errmsg = dbsql_mprintf("no such function: %s", rank);
      rc = DBSQL_ERROR;
    }
  }

  csr->rank = aux;
  return rc;
}


static int __fts5_cursor_parse_rank(
  fts5_config_t *config,
  fts5_cursor_t *csr, 
  dbsql_value_t *rank
){
  int rc = DBSQL_OK;
  if( rank ){
    const char *z = (const char*)dbsql_value_text(rank);
    char *z_rank = 0;
    char *rank_args = 0;

    if( z==0 ){
      if( dbsql_value_type(rank)==DBSQL_NULL ) rc = DBSQL_ERROR;
    }else{
      rc = __fts5_config_parse_rank(z, &z_rank, &rank_args);
    }
    if( rc==DBSQL_OK ){
      csr->z_rank = z_rank;
      csr->z_rank_args = rank_args;
      CSR_FLAG_SET(csr, FTS5CSR_FREE_ZRANK);
    }else if( rc==DBSQL_ERROR ){
      csr->base.vtab->errmsg = dbsql_mprintf(
          "parse error in rank function: %s", z
      );
    }
  }else{
    if( config->z_rank ){
      csr->z_rank = (char*)config->z_rank;
      csr->z_rank_args = (char*)config->z_rank_args;
    }else{
      csr->z_rank = (char*)FTS5_DEFAULT_RANK;
      csr->z_rank_args = 0;
    }
  }
  return rc;
}

static int64_t __fts5_get_rowid_limit(dbsql_value_t *val, int64_t i_default){
  if( val ){
    int type = dbsql_value_numeric_type(val);
    if( type==DBSQL_INTEGER ){
      return dbsql_value_int64(val);
    }
  }
  return i_default;
}

/*
** Set the error message on the virtual table passed as the first argument.
*/
static void __fts5_set_vtab_error(fts5_full_table_t *p, const char *format, ...){
  va_list ap;                     /* ... printf arguments */
  va_start(ap, format);
  dbsql_free(p->p.base.errmsg);
  p->p.base.errmsg = dbsql_vmprintf(format, ap);
  va_end(ap);
}

/*
** Arrange for subsequent calls to __fts5_tokenize() to use the locale
** specified by locale/n_locale. The buffer indicated by locale must remain
** valid until after the final call to __fts5_tokenize() that will use
** the locale.
*/
static void __fts5_set_locale(
  fts5_config_t *config, 
  const char *locale, 
  int n_locale
){
  fts5_tokenizer_config_t *t = &config->t;
  t->locale = locale;
  t->n_locale = n_locale;
}

/*
** Clear any locale configured by an earlier call to __fts5_set_locale().
*/
void __fts5_clear_locale(fts5_config_t *config){
  __fts5_set_locale(config, 0, 0);
}

/*
** Return true if the value passed as the only argument is an 
** fts5_locale() value.  
*/
int __fts5_is_locale_value(fts5_config_t *config, dbsql_value_t *val){
  int ret = 0;
  if( dbsql_value_type(val)==DBSQL_BLOB ){
    /* Call dbsql_value_bytes() after dbsql_value_blob() in this case.
    ** If the blob was created using zeroblob(), then dbsql_value_blob()
    ** may call malloc(). If this malloc() fails, then the values returned
    ** by both value_blob() and value_bytes() will be 0. If value_bytes() were 
    ** called first, then the NULL pointer returned by value_blob() might
    ** be dereferenced.  */
    const u_int8_t *blob = dbsql_value_blob(val);
    int n_blob = dbsql_value_bytes(val); 
    if( n_blob>FTS5_LOCALE_HDR_SIZE
     && 0==memcmp(blob, FTS5_LOCALE_HDR(config), FTS5_LOCALE_HDR_SIZE)
    ){
      ret = 1;
    }
  }
  return ret;
}

/*
** Value val is guaranteed to be an fts5_locale() value, according to
** __fts5_is_locale_value(). This function extracts the text and locale
** from the value and returns them separately.
**
** If successful, DBSQL_OK is returned and (*text) and (*loc) set
** to point to buffers containing the text and locale, as utf-8,
** respectively. In this case output parameters (*pn_text) and (*pn_loc) are
** set to the sizes in bytes of these two buffers.
**
** Or, if an error occurs, then an dbsql error code is returned. The final
** value of the four output parameters is undefined in this case.
*/
int __fts5_decode_locale_value(
  dbsql_value_t *val, 
  const char **text,
  int *pn_text, 
  const char **loc, 
  int *pn_loc
){
  const char *p = dbsql_value_blob(val);
  int n = dbsql_value_bytes(val);
  int n_loc = 0;

  assert( dbsql_value_type(val)==DBSQL_BLOB );
  assert( n>FTS5_LOCALE_HDR_SIZE );

  for(n_loc=FTS5_LOCALE_HDR_SIZE; p[n_loc]; n_loc++){
    if( n_loc==(n-1) ){
      return DBSQL_MISMATCH;
    }
  }
  *loc = &p[FTS5_LOCALE_HDR_SIZE];
  *pn_loc = n_loc - FTS5_LOCALE_HDR_SIZE;

  *text = &p[n_loc+1];
  *pn_text = n - n_loc - 1;
  return DBSQL_OK;
}

/*
** Argument val is the text of a full-text search expression. It may or
** may not have been wrapped by fts5_locale(). This function extracts
** the text of the expression, and sets output variable (*text) to 
** point to a nul-terminated buffer containing the expression.
**
** If val was an fts5_locale() value, then __fts5_set_locale() is called 
** to set the tokenizer to use the specified locale.
**
** If output variable (*pb_free_and_reset) is set to true, then the caller
** is required to (a) call __fts5_clear_locale() to reset the tokenizer 
** locale, and (b) call dbsql_free() to free (*text).
*/
static int __fts5_extract_expr_text(
  fts5_config_t *config,            /* Fts5 configuration */
  dbsql_value_t *val,            /* Value to extract expression text from */
  char **pz_text,                  /* OUT: nul-terminated buffer of text */
  int *pb_free_and_reset             /* OUT: Free (*text) and clear locale */
){
  int rc = DBSQL_OK;

  if( __fts5_is_locale_value(config, val) ){
    const char *text = 0;
    int n_text = 0;
    const char *loc = 0;
    int n_loc = 0;
    rc = __fts5_decode_locale_value(val, &text, &n_text, &loc, &n_loc);
    *pz_text = __fts5_mprintf(&rc, "%.*s", n_text, text);
    if( rc==DBSQL_OK ){
      __fts5_set_locale(config, loc, n_loc);
    }
    *pb_free_and_reset = 1;
  }else{
    *pz_text = (char*)dbsql_value_text(val);
    *pb_free_and_reset = 0;
  }

  return rc;
}


/*
** This is the filter interface for the virtual table.  See
** the virtual table filter method documentation for additional
** information.
** 
** There are three possible query strategies:
**
**   1. Full-text search using a MATCH operator.
**   2. A by-rowid lookup.
**   3. A full-table scan.
*/
static int __fts5_filter_method(
  dbsql_vtab_cursor_t *cursor,   /* The cursor used for this query */
  int idx_num,                     /* Strategy index */
  const char *idx_str,             /* Unused */
  int n_val,                       /* Number of elements in vals */
  dbsql_value_t **vals           /* Arguments for the indexing scheme */
){
  fts5_full_table_t *tab = (fts5_full_table_t*)(cursor->vtab);
  fts5_config_t *config = tab->p.config;
  fts5_cursor_t *csr = (fts5_cursor_t*)cursor;
  int rc = DBSQL_OK;             /* Error code */
  int desc;                      /* True if ORDER BY [rank|rowid] DESC */
  int order_by_rank;               /* True if ORDER BY rank */
  dbsql_value_t *rank = 0;       /* rank MATCH ? expression (or NULL) */
  dbsql_value_t *rowid_eq = 0;    /* rowid = ? expression (or NULL) */
  dbsql_value_t *rowid_le = 0;    /* rowid <= ? expression (or NULL) */
  dbsql_value_t *rowid_ge = 0;    /* rowid >= ? expression (or NULL) */
  int i_col;                       /* column_t on LHS of MATCH operator */
  char **errmsg = config->errmsg;
  int prefix_insttoken = config->prefix_insttoken;
  int i;
  int i_idx_str = 0;
  fts5_expr_t *expr = 0;

  assert( config->lock==0 );
  if( csr->plan ){
    __fts5_free_cursor_components(csr);
    memset(&csr->plan, 0, sizeof(fts5_cursor_t) - ((u_int8_t*)&csr->plan-(u_int8_t*)csr));
  }

  assert( csr->stmt==0 );
  assert( csr->expr==0 );
  assert( csr->csrflags==0 );
  assert( csr->rank==0 );
  assert( csr->z_rank==0 );
  assert( csr->z_rank_args==0 );
  assert( tab->sort_csr==0 || n_val==0 );

  assert( errmsg==0 || errmsg==&tab->p.base.errmsg );
  config->errmsg = &tab->p.base.errmsg;

  /* Decode the arguments passed through to this function. */
  for(i=0; i<n_val; i++){
    switch( idx_str[i_idx_str++] ){
      case 'r':
        rank = vals[i];
        break;
      case 'M': {
        char *text = 0;
        int free_and_reset = 0;
        int internal = 0;

        rc = __fts5_extract_expr_text(config, vals[i], &text, &free_and_reset);
        if( rc!=DBSQL_OK ) goto filter_out;
        if( text==0 ) text = "";
        if( dbsql_value_subtype(vals[i])==FTS5_INSTTOKEN_SUBTYPE ){
          config->prefix_insttoken = 1;
        }

        i_col = 0;
        do{
          i_col = i_col*10 + (idx_str[i_idx_str]-'0');
          i_idx_str++;
        }while( idx_str[i_idx_str]>='0' && idx_str[i_idx_str]<='9' );

        if( text[0]=='*' ){
          /* The user has issued a query of the form "MATCH '*...'". This
          ** indicates that the MATCH expression is not a full text query,
          ** but a request for an internal parameter.  */
          rc = __fts5_special_match(tab, csr, &text[1]);
          internal = 1;
        }else{
          char **err_p = &tab->p.base.errmsg;
          rc = __fts5_expr_new(config, 0, i_col, text, &expr, err_p);
          if( rc==DBSQL_OK ){
            rc = __fts5_expr_and(&csr->expr, expr);
            expr = 0;
          }
        }

        if( free_and_reset ){
          dbsql_free(text);
          __fts5_clear_locale(config);
        }

        if( internal || rc!=DBSQL_OK ) goto filter_out;

        break;
      }
      case 'L':
      case 'G': {
        int glob = (idx_str[i_idx_str-1]=='G');
        const char *text = (const char*)dbsql_value_text(vals[i]);
        i_col = 0;
        do{
          i_col = i_col*10 + (idx_str[i_idx_str]-'0');
          i_idx_str++;
        }while( idx_str[i_idx_str]>='0' && idx_str[i_idx_str]<='9' );
        if( text ){
          rc = __fts5_expr_pattern(config, glob, i_col, text, &expr);
        }
        if( rc==DBSQL_OK ){
          rc = __fts5_expr_and(&csr->expr, expr);
          expr = 0;
        }
        if( rc!=DBSQL_OK ) goto filter_out;
        break;
      }
      case '=':
        rowid_eq = vals[i];
        break;
      case '<':
        rowid_le = vals[i];
        break;
      default: assert( idx_str[i_idx_str-1]=='>' );
        rowid_ge = vals[i];
        break;
    }
  }
  order_by_rank = ((idx_num & FTS5_BI_ORDER_RANK) ? 1 : 0);
  csr->desc = desc = ((idx_num & FTS5_BI_ORDER_DESC) ? 1 : 0);

  /* Set the cursor upper and lower rowid limits. Only some strategies 
  ** actually use them. This is ok, as the best_index() method leaves the
  ** dbsql_index_constraint_t.omit flag clear for range constraints
  ** on the rowid field.  */
  if( rowid_eq ){
    rowid_le = rowid_ge = rowid_eq;
  }
  if( desc ){
    csr->i_first_rowid = __fts5_get_rowid_limit(rowid_le, LARGEST_INT64);
    csr->i_last_rowid = __fts5_get_rowid_limit(rowid_ge, SMALLEST_INT64);
  }else{
    csr->i_last_rowid = __fts5_get_rowid_limit(rowid_le, LARGEST_INT64);
    csr->i_first_rowid = __fts5_get_rowid_limit(rowid_ge, SMALLEST_INT64);
  }

  rc = __fts5_index_load_config(tab->p.p_index);
  if( rc!=DBSQL_OK ) goto filter_out;

  if( tab->sort_csr ){
    /* If sort_csr is non-NULL, then this call is being made as part of 
    ** processing for a "... MATCH <expr> ORDER BY rank" query (plan is
    ** set to FTS5_PLAN_SORTED_MATCH). sort_csr is the cursor that will
    ** return results to the user for this query. The current cursor 
    ** (cursor) is used to execute the query issued by function 
    ** __fts5_cursor_first_sorted() above.  */
    assert( rowid_eq==0 && rowid_le==0 && rowid_ge==0 && rank==0 );
    assert( n_val==0 && order_by_rank==0 && desc==0 );
    assert( csr->i_last_rowid==LARGEST_INT64 );
    assert( csr->i_first_rowid==SMALLEST_INT64 );
    if( tab->sort_csr->desc ){
      csr->i_last_rowid = tab->sort_csr->i_first_rowid;
      csr->i_first_rowid = tab->sort_csr->i_last_rowid;
    }else{
      csr->i_last_rowid = tab->sort_csr->i_last_rowid;
      csr->i_first_rowid = tab->sort_csr->i_first_rowid;
    }
    csr->plan = FTS5_PLAN_SOURCE;
    csr->expr = tab->sort_csr->expr;
    rc = __fts5_cursor_first(tab, csr, desc);
  }else if( csr->expr ){
    assert( rc==DBSQL_OK );
    rc = __fts5_cursor_parse_rank(config, csr, rank);
    if( rc==DBSQL_OK ){
      if( order_by_rank ){
        csr->plan = FTS5_PLAN_SORTED_MATCH;
        rc = __fts5_cursor_first_sorted(tab, csr, desc);
      }else{
        csr->plan = FTS5_PLAN_MATCH;
        rc = __fts5_cursor_first(tab, csr, desc);
      }
    }
  }else if( config->z_content==0 ){
    __fts5_set_vtab_error(tab,"%s: table does not support scanning",config->name);
    rc = DBSQL_ERROR;
  }else{
    /* This is either a full-table scan (plan==FTS5_PLAN_SCAN) or a lookup
    ** by rowid (plan==FTS5_PLAN_ROWID).  */
    csr->plan = (rowid_eq ? FTS5_PLAN_ROWID : FTS5_PLAN_SCAN);
    rc = __fts5_storage_stmt(
        tab->storage, __fts5_stmt_type(csr), &csr->stmt, &tab->p.base.errmsg
    );
    if( rc==DBSQL_OK ){
      if( rowid_eq!=0 ){
        assert( csr->plan==FTS5_PLAN_ROWID );
        dbsql_bind_value(csr->stmt, 1, rowid_eq);
      }else{
        dbsql_bind_int64(csr->stmt, 1, csr->i_first_rowid);
        dbsql_bind_int64(csr->stmt, 2, csr->i_last_rowid);
      }
      rc = __fts5_next_method(cursor);
    }
  }

 filter_out:
  __fts5_expr_free(expr);
  config->errmsg = errmsg;
  config->prefix_insttoken = prefix_insttoken;
  return rc;
}

/* 
** This is the eof method of the virtual table. dbsql calls this 
** routine to find out if it has reached the end of a result set.
*/
static int __fts5_eof_method(dbsql_vtab_cursor_t *cursor){
  fts5_cursor_t *csr = (fts5_cursor_t*)cursor;
  return (CSR_FLAG_TEST(csr, FTS5CSR_EOF) ? 1 : 0);
}

/*
** Return the rowid that the cursor currently points to.
*/
static int64_t __fts5_cursor_rowid(fts5_cursor_t *csr){
  assert( csr->plan==FTS5_PLAN_MATCH 
       || csr->plan==FTS5_PLAN_SORTED_MATCH 
       || csr->plan==FTS5_PLAN_SOURCE 
       || csr->plan==FTS5_PLAN_SCAN 
       || csr->plan==FTS5_PLAN_ROWID 
  );
  if( csr->sorter ){
    return csr->sorter->i_rowid;
  }else if( csr->plan>=FTS5_PLAN_SCAN ){
    return dbsql_column_int64(csr->stmt, 0);
  }else{
    return __fts5_expr_rowid(csr->expr);
  }
}

/* 
** This is the rowid method. The dbsql core calls this routine to
** retrieve the rowid for the current row of the result set. fts5
** exposes %_content.rowid as the rowid for the virtual table. The
** rowid should be written to *rowid.
*/
static int __fts5_rowid_method(dbsql_vtab_cursor_t *cursor, int64_t *rowid){
  fts5_cursor_t *csr = (fts5_cursor_t*)cursor;
  int plan = csr->plan;
  
  assert( CSR_FLAG_TEST(csr, FTS5CSR_EOF)==0 );
  if( plan==FTS5_PLAN_SPECIAL ){
    *rowid = 0;
  }else{
    *rowid = __fts5_cursor_rowid(csr);
  }

  return DBSQL_OK;
}


/*
** If the cursor requires seeking (seek_required flag is set), seek it.
** Return DBSQL_OK if no error occurs, or an dbsql error code otherwise.
**
** If argument errormsg is true and an error occurs, an error message may
** be left in dbsql_vtab_t.errmsg.
*/
static int __fts5_seek_cursor(fts5_cursor_t *csr, int errormsg){
  int rc = DBSQL_OK;

  /* If the cursor does not yet have a statement handle, obtain one now. */ 
  if( csr->stmt==0 ){
    fts5_full_table_t *tab = (fts5_full_table_t*)(csr->base.vtab);
    int stmt = __fts5_stmt_type(csr);
    rc = __fts5_storage_stmt(
        tab->storage, stmt, &csr->stmt, (errormsg?&tab->p.base.errmsg:0)
    );
    assert( rc!=DBSQL_OK || tab->p.base.errmsg==0 );
    assert( CSR_FLAG_TEST(csr, FTS5CSR_REQUIRE_CONTENT) );
  }

  if( rc==DBSQL_OK && CSR_FLAG_TEST(csr, FTS5CSR_REQUIRE_CONTENT) ){
    fts5_table_t *tab = (fts5_table_t*)(csr->base.vtab);
    assert( csr->expr );
    dbsql_reset(csr->stmt);
    dbsql_bind_int64(csr->stmt, 1, __fts5_cursor_rowid(csr));
    tab->config->lock++;
    rc = dbsql_step(csr->stmt);
    tab->config->lock--;
    if( rc==DBSQL_ROW ){
      rc = DBSQL_OK;
      CSR_FLAG_CLEAR(csr, FTS5CSR_REQUIRE_CONTENT);
    }else{
      rc = dbsql_reset(csr->stmt);
      if( rc==DBSQL_OK ){
        rc = FTS5_CORRUPT;
        __fts5_set_vtab_error((fts5_full_table_t*)tab, 
            "fts5: missing row %lld from content table %s",
            __fts5_cursor_rowid(csr), 
            tab->config->z_content
        );
      }else if( tab->config->errmsg ){
        __fts5_set_vtab_error((fts5_full_table_t*)tab, 
            "%s", dbsql_errmsg(tab->config->db)
        );
      }
    }
  }
  return rc;
}

/*
** This function is called to handle an FTS INSERT command. In other words,
** an INSERT statement of the form:
**
**     INSERT INTO fts(fts) VALUES($cmd)
**     INSERT INTO fts(fts, rank) VALUES($cmd, $val)
**
** Argument val is the value assigned to column "fts" by the INSERT 
** statement. This function returns DBSQL_OK if successful, or an dbsql
** error code if an error occurs.
**
** The commands implemented by this function are documented in the "Special
** INSERT Directives" section of the documentation. It should be updated if
** more commands are added to this function.
*/
static int __fts5_special_insert(
  fts5_full_table_t *tab,            /* Fts5 table object */
  const char *cmd,               /* Text inserted into table-name column */
  dbsql_value_t *val             /* Value inserted into rank column */
){
  fts5_config_t *config = tab->p.config;
  int rc = DBSQL_OK;
  int error = 0;
  int load_config = 0;

  if( 0==dbsql_stricmp("delete-all", cmd) ){
    if( config->content==FTS5_CONTENT_NORMAL ){
      __fts5_set_vtab_error(tab, 
          "'delete-all' may only be used with a "
          "contentless or external content fts5 table"
      );
      rc = DBSQL_ERROR;
    }else{
      rc = __fts5_storage_delete_all(tab->storage);
    }
    load_config = 1;
  }else if( 0==dbsql_stricmp("rebuild", cmd) ){
    if( __fts5_is_contentless(tab, 1) ){
      __fts5_set_vtab_error(tab, 
          "'rebuild' may not be used with a contentless fts5 table"
      );
      rc = DBSQL_ERROR;
    }else{
      rc = __fts5_storage_rebuild(tab->storage);
    }
    load_config = 1;
  }else if( 0==dbsql_stricmp("optimize", cmd) ){
    rc = __fts5_storage_optimize(tab->storage);
  }else if( 0==dbsql_stricmp("merge", cmd) ){
    int n_merge = dbsql_value_int(val);
    rc = __fts5_storage_merge(tab->storage, n_merge);
  }else if( 0==dbsql_stricmp("integrity-check", cmd) ){
    int i_arg = dbsql_value_int(val);
    rc = __fts5_storage_integrity(tab->storage, i_arg);
#ifdef DBSQL_DEBUG
  }else if( 0==dbsql_stricmp("prefix-index", cmd) ){
    config->prefix_index = dbsql_value_int(val);
#endif
  }else if( 0==dbsql_stricmp("flush", cmd) ){
    rc = __fts5_flush_to_disk(&tab->p);
  }else{
    rc = __fts5_flush_to_disk(&tab->p);
    if( rc==DBSQL_OK ){
      rc = __fts5_index_load_config(tab->p.p_index);
    }
    if( rc==DBSQL_OK ){
      rc = __fts5_config_set_value(tab->p.config, cmd, val, &error);
    }
    if( rc==DBSQL_OK ){
      if( error ){
        rc = DBSQL_ERROR;
      }else{
        rc = __fts5_storage_config_value(tab->storage, cmd, val, 0);
      }
    }
  }

  if( rc==DBSQL_OK && load_config ){
    tab->p.config->i_cookie--;
    rc = __fts5_index_load_config(tab->p.p_index);
  }

  return rc;
}

static int __fts5_special_delete(
  fts5_full_table_t *tab, 
  dbsql_value_t **vals
){
  int rc = DBSQL_OK;
  int type1 = dbsql_value_type(vals[1]);
  if( type1==DBSQL_INTEGER ){
    int64_t i_del = dbsql_value_int64(vals[1]);
    rc = __fts5_storage_delete(tab->storage, i_del, &vals[2], 0);
  }
  return rc;
}

static void __fts5_storage_insert(
  int *p_rc, 
  fts5_full_table_t *tab, 
  dbsql_value_t **vals, 
  int64_t *pi_rowid
){
  int rc = *p_rc;
  if( rc==DBSQL_OK ){
    rc = __fts5_storage_content_insert(tab->storage, 0, vals, pi_rowid);
  }
  if( rc==DBSQL_OK ){
    rc = __fts5_storage_index_insert(tab->storage, vals, *pi_rowid);
  }
  *p_rc = rc;
}

/*
**
** This function is called when the user attempts an UPDATE on a contentless
** table. Parameter rowid_modified is true if the UPDATE statement modifies
** the rowid value. Parameter vals[] contains the new values for each user
** defined column of the fts5 table. config is the configuration object of the
** table being updated (guaranteed to be contentless). The contentless_delete=1
** and contentless_unindexed=1 options may or may not be set. 
**
** This function returns DBSQL_OK if the UPDATE can go ahead, or an dbsql
** error code if it cannot. In this case an error message is also loaded into
** config. Output parameter (*pb_content) is set to true if the caller should
** update the %_content table only - not the FTS index or any other shadow
** table. This occurs when an UPDATE modifies only UNINDEXED columns of the
** table.
**
** An UPDATE may proceed if:
**
**   * The only columns modified are UNINDEXED columns, or
**
**   * The contentless_delete=1 option was specified and all of the indexed
**     columns (not a subset) have been modified.
*/
static int __fts5_contentless_update(
  fts5_config_t *config,
  dbsql_value_t **vals,
  int rowid_modified,
  int *pb_content
){
  int ii;
  int seen_index = 0;             /* Have seen modified indexed column */
  int seen_index_nc = 0;           /* Have seen unmodified indexed column */
  int rc = DBSQL_OK;

  for(ii=0; ii<config->n_col; ii++){
    if( config->ab_unindexed[ii]==0 ){
      if( dbsql_value_nochange(vals[ii]) ){
        seen_index_nc++;
      }else{
        seen_index++;
      }
    }
  }

  if( seen_index==0 && rowid_modified==0 ){
    *pb_content = 1;
  }else{
    if( seen_index_nc || config->contentless_delete==0 ){
      rc = DBSQL_ERROR;
      __fts5_config_errmsg(config, 
          (config->contentless_delete ?
          "%s a subset of columns on fts5 contentless-delete table: %s" :
          "%s contentless fts5 table: %s")
          , "cannot UPDATE", config->name
      );
    }
  }

  return rc;
}

/* 
** This function is the implementation of the update callback used by 
** FTS3 virtual tables. It is invoked by dbsql each time a row is to be
** inserted, updated or deleted.
**
** A delete specifies a single argument - the rowid of the row to remove.
** 
** Update and insert operations pass:
**
**   1. The "old" rowid, or NULL.
**   2. The "new" rowid.
**   3. Values for each of the n_col matchable columns.
**   4. Values for the two hidden columns (<tablename> and "rank").
*/
static int __fts5_update_method(
  dbsql_vtab_t *vtab,            /* Virtual table handle */
  int n_arg,                       /* Size of argument array */
  dbsql_value_t **vals,          /* Array of arguments */
  int64_t *rowid            /* OUT: The affected (or effected) rowid */
){
  fts5_full_table_t *tab = (fts5_full_table_t*)vtab;
  fts5_config_t *config = tab->p.config;
  int type0;                     /* value_type() of vals[0] */
  int rc = DBSQL_OK;             /* Return code */

  /* A transaction must be open when this is called. */
  assert( tab->ts.state==1 || tab->ts.state==2 );

  assert( vtab->errmsg==0 );
  assert( n_arg==1 || n_arg==(2+config->n_col+2) );
  assert( dbsql_value_type(vals[0])==DBSQL_INTEGER 
       || dbsql_value_type(vals[0])==DBSQL_NULL 
  );
  assert( tab->p.config->errmsg==0 );
  if( config->pgsz==0 ){
    rc = __fts5_config_load(tab->p.config, tab->p.config->i_cookie);
    if( rc!=DBSQL_OK ) return rc;
  }

  tab->p.config->errmsg = &tab->p.base.errmsg;

  /* Put any active cursors into REQUIRE_SEEK state. */
  __fts5_trip_cursors(tab);

  type0 = dbsql_value_type(vals[0]);
  if( type0==DBSQL_NULL 
   && dbsql_value_type(vals[2+config->n_col])!=DBSQL_NULL 
  ){
    /* A "special" INSERT op. These are handled separately. */
    const char *z = (const char*)dbsql_value_text(vals[2+config->n_col]);
    if( config->content!=FTS5_CONTENT_NORMAL 
      && 0==dbsql_stricmp("delete", z) 
    ){
      if( config->contentless_delete ){
        __fts5_set_vtab_error(tab, 
            "'delete' may not be used with a contentless_delete=1 table"
        );
        rc = DBSQL_ERROR;
      }else{
        rc = __fts5_special_delete(tab, vals);
      }
    }else{
      rc = __fts5_special_insert(tab, z, vals[2 + config->n_col + 1]);
    }
  }else{
    /* A regular INSERT, UPDATE or DELETE statement. The trick here is that
    ** any conflict on the rowid value must be detected before any 
    ** modifications are made to the database file. There are 4 cases:
    **
    **   1) DELETE
    **   2) UPDATE (rowid not modified)
    **   3) UPDATE (rowid modified)
    **   4) INSERT
    **
    ** Cases 3 and 4 may violate the rowid constraint.
    */
    int conflict = DBSQL_ABORT;
    if( config->content==FTS5_CONTENT_NORMAL || config->contentless_delete ){
      conflict = dbsql_vtab_on_conflict(config->db);
    }

    assert( type0==DBSQL_INTEGER || type0==DBSQL_NULL );
    assert( n_arg!=1 || type0==DBSQL_INTEGER );

    /* DELETE */
    if( n_arg==1 ){
      /* It is only possible to DELETE from a contentless table if the
      ** contentless_delete=1 flag is set. */
      if( __fts5_is_contentless(tab, 1) && config->contentless_delete==0 ){
        __fts5_set_vtab_error(tab, 
            "cannot DELETE from contentless fts5 table: %s", config->name
        );
        rc = DBSQL_ERROR;
      }else{
        int64_t i_del = dbsql_value_int64(vals[0]);  /* Rowid to delete */
        rc = __fts5_storage_delete(tab->storage, i_del, 0, 0);
      }
    }

    /* INSERT or UPDATE */
    else{
      int type1 = dbsql_value_numeric_type(vals[1]);

      /* It is an error to write an fts5_locale() value to a table without
      ** the locale=1 option. */
      if( config->locale==0 ){
        int ii;
        for(ii=0; ii<config->n_col; ii++){
          dbsql_value_t *val = vals[ii+2];
          if( __fts5_is_locale_value(config, val) ){
            __fts5_set_vtab_error(tab, "fts5_locale() requires locale=1");
            rc = DBSQL_MISMATCH;
            goto update_out;
          }
        }
      }

      if( type0!=DBSQL_INTEGER ){
        /* An INSERT statement. If the conflict-mode is REPLACE, first remove
        ** the current entry (if any). */
        if( conflict==DBSQL_REPLACE && type1==DBSQL_INTEGER ){
          int64_t i_new = dbsql_value_int64(vals[1]);  /* Rowid to delete */
          rc = __fts5_storage_delete(tab->storage, i_new, 0, 0);
        }
        __fts5_storage_insert(&rc, tab, vals, rowid);
      }

      /* UPDATE */
      else{
        fts5_storage_t *storage = tab->storage;
        int64_t i_old = dbsql_value_int64(vals[0]);  /* Old rowid */
        int64_t i_new = dbsql_value_int64(vals[1]);  /* New rowid */
        int content = 0;         /* Content only update */

        /* If this is a contentless table (including contentless_unindexed=1
        ** tables), check if the UPDATE may proceed.  */
        if( __fts5_is_contentless(tab, 1) ){
          rc = __fts5_contentless_update(config, &vals[2], i_old!=i_new, &content);
          if( rc!=DBSQL_OK ) goto update_out;
        }

        if( type1!=DBSQL_INTEGER ){
          rc = DBSQL_MISMATCH;
        }else if( i_old!=i_new ){
          assert( content==0 );
          if( conflict==DBSQL_REPLACE ){
            rc = __fts5_storage_delete(storage, i_old, 0, 1);
            if( rc==DBSQL_OK ){
              rc = __fts5_storage_delete(storage, i_new, 0, 0);
            }
            __fts5_storage_insert(&rc, tab, vals, rowid);
          }else{
            rc = __fts5_storage_find_delete_row(storage, i_old);
            if( rc==DBSQL_OK ){
              rc = __fts5_storage_content_insert(storage, 0, vals, rowid);
            }
            if( rc==DBSQL_OK ){
              rc = __fts5_storage_delete(storage, i_old, 0, 0);
            }
            if( rc==DBSQL_OK ){
              rc = __fts5_storage_index_insert(storage, vals, *rowid);
            }
          }
        }else if( content ){
          /* This occurs when an UPDATE on a contentless table affects *only*
          ** UNINDEXED columns. This is a no-op for contentless_unindexed=0
          ** tables, or a write to the %_content table only for =1 tables.  */
          assert( __fts5_is_contentless(tab, 1) );
          rc = __fts5_storage_find_delete_row(storage, i_old);
          if( rc==DBSQL_OK ){
            rc = __fts5_storage_content_insert(storage, 1, vals, rowid);
          }
        }else{
          rc = __fts5_storage_delete(storage, i_old, 0, 1);
          __fts5_storage_insert(&rc, tab, vals, rowid);
        }
        __fts5_storage_release_delete_row(storage);
      }
    }
  }

 update_out:
  __fts5_index_close_reader(tab->p.p_index);
  tab->p.config->errmsg = 0;
  return rc;
}

/*
** Implementation of x_sync() method. 
*/
static int __fts5_sync_method(dbsql_vtab_t *vtab){
  int rc;
  fts5_full_table_t *tab = (fts5_full_table_t*)vtab;
  FTS5_CHECK_TRANSACTION_STATE(tab, FTS5_SYNC, 0);
  tab->p.config->errmsg = &tab->p.base.errmsg;
  rc = __fts5_flush_to_disk(&tab->p);
  tab->p.config->errmsg = 0;
  return rc;
}

/*
** Implementation of begin() method. 
*/
static int __fts5_begin_method(dbsql_vtab_t *vtab){
  int rc = __fts5_new_transaction((fts5_full_table_t*)vtab);
  if( rc==DBSQL_OK ){
    FTS5_CHECK_TRANSACTION_STATE((fts5_full_table_t*)vtab, FTS5_BEGIN, 0);
  }
  return rc;
}

/*
** Implementation of commit() method. This is a no-op. The contents of
** the pending-terms hash-table have already been flushed into the database
** by __fts5_sync_method().
*/
static int __fts5_commit_method(dbsql_vtab_t *vtab){
  UNUSED_PARAM(vtab);  /* Call below is a no-op for NDEBUG builds */
  FTS5_CHECK_TRANSACTION_STATE((fts5_full_table_t*)vtab, FTS5_COMMIT, 0);
  return DBSQL_OK;
}

/*
** Implementation of rollback(). Discard the contents of the pending-terms
** hash-table. Any changes made to the database are reverted by dbsql.
*/
static int __fts5_rollback_method(dbsql_vtab_t *vtab){
  int rc;
  fts5_full_table_t *tab = (fts5_full_table_t*)vtab;
  FTS5_CHECK_TRANSACTION_STATE(tab, FTS5_ROLLBACK, 0);
  rc = __fts5_storage_rollback(tab->storage);
  tab->p.config->pgsz = 0;
  return rc;
}

static int __fts5_csr_poslist(fts5_cursor_t*, int, const u_int8_t**, int*);

static void *__fts5_api_user_data(fts5_context_t *ctx){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  return csr->aux->user_data;
}

static int __fts5_api_column_count(fts5_context_t *ctx){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  return ((fts5_table_t*)(csr->base.vtab))->config->n_col;
}

static int __fts5_api_column_total_size(
  fts5_context_t *ctx, 
  int i_col, 
  int64_t *pn_token
){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_full_table_t *tab = (fts5_full_table_t*)(csr->base.vtab);
  return __fts5_storage_size(tab->storage, i_col, pn_token);
}

static int __fts5_api_row_count(fts5_context_t *ctx, int64_t *pn_row){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_full_table_t *tab = (fts5_full_table_t*)(csr->base.vtab);
  return __fts5_storage_row_count(tab->storage, pn_row);
}

/*
** Implementation of tokenize_v2() API.
*/
static int __fts5_api_tokenize_v2(
  fts5_context_t *ctx, 
  const char *text, int n_text, 
  const char *loc, int n_loc, 
  void *user_data,
  int (*token)(void*, int, const char*, int, int, int)
){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_table_t *tab = (fts5_table_t*)(csr->base.vtab);
  int rc = DBSQL_OK;

  __fts5_set_locale(tab->config, loc, n_loc);
  rc = __fts5_tokenize(tab->config, 
      FTS5_TOKENIZE_AUX, text, n_text, user_data, token
  );
  __fts5_set_locale(tab->config, 0, 0);

  return rc;
}

/*
** Implementation of tokenize() API. This is just tokenize_v2() with NULL/0
** passed as the locale.
*/
static int __fts5_api_tokenize(
  fts5_context_t *ctx, 
  const char *text, int n_text, 
  void *user_data,
  int (*token)(void*, int, const char*, int, int, int)
){
  return __fts5_api_tokenize_v2(ctx, text, n_text, 0, 0, user_data, token);
}

static int __fts5_api_phrase_count(fts5_context_t *ctx){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  return __fts5_expr_phrase_count(csr->expr);
}

static int __fts5_api_phrase_size(fts5_context_t *ctx, int i_phrase){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  return __fts5_expr_phrase_size(csr->expr, i_phrase);
}

/*
** Argument stmt is an SQL statement of the type used by fts5_cursor_t. This
** function extracts the text value of column i_col of the current row.
** Additionally, if there is an associated locale, it invokes
** __fts5_set_locale() to configure the tokenizer. In all cases the caller
** should invoke __fts5_clear_locale() to clear the locale at some point
** after this function returns.
**
** If successful, (*text) is set to point to a buffer containing the text
** value as utf-8 and DBSQL_OK returned. (*pn_text) is set to the size of that
** buffer in bytes. It is not guaranteed to be nul-terminated. If an error
** occurs, an dbsql error code is returned. The final values of the two
** output parameters are undefined in this case.
*/
static int __fts5_text_from_stmt(
  fts5_config_t *config,
  dbsql_stmt_t *stmt,
  int i_col,
  const char **text,
  int *pn_text
){
  dbsql_value_t *val = dbsql_column_value(stmt, i_col+1);
  const char *loc = 0;
  int n_loc = 0;
  int rc = DBSQL_OK;

  if( config->locale 
   && config->content==FTS5_CONTENT_EXTERNAL
   && __fts5_is_locale_value(config, val) 
  ){
    rc = __fts5_decode_locale_value(val, text, pn_text, &loc, &n_loc);
  }else{
    *text = (const char*)dbsql_value_text(val);
    *pn_text = dbsql_value_bytes(val);
    if( config->locale && config->content==FTS5_CONTENT_NORMAL ){
      loc = (const char*)dbsql_column_text(stmt, i_col+1+config->n_col);
      n_loc = dbsql_column_bytes(stmt, i_col+1+config->n_col);
    }
  }
  __fts5_set_locale(config, loc, n_loc);
  return rc;
}

static int __fts5_api_column_text(
  fts5_context_t *ctx, 
  int i_col, 
  const char **pz, 
  int *pn
){
  int rc = DBSQL_OK;
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_table_t *tab = (fts5_table_t*)(csr->base.vtab);

  assert( csr->plan!=FTS5_PLAN_SPECIAL );
  if( i_col<0 || i_col>=tab->config->n_col ){
    rc = DBSQL_RANGE;
  }else if( __fts5_is_contentless((fts5_full_table_t*)(csr->base.vtab), 0) ){
    *pz = 0;
    *pn = 0;
  }else{
    rc = __fts5_seek_cursor(csr, 0);
    if( rc==DBSQL_OK ){
      rc = __fts5_text_from_stmt(tab->config, csr->stmt, i_col, pz, pn);
      __fts5_clear_locale(tab->config);
    }
  }
  return rc;
}

/*
** This is called by various API functions - inst, phrase_first, 
** phrase_first_column etc. - to obtain the position list for phrase i_phrase
** of the current row. This function works for both detail=full tables (in
** which case the position-list was read from the fts index) or for other
** detail= modes if the row content is available.
*/
static int __fts5_csr_poslist(
  fts5_cursor_t *csr,               /* Fts5 cursor object */
  int i_phrase,                    /* Phrase to find position list for */
  const u_int8_t **pa,                  /* OUT: Pointer to position list buffer */
  int *pn                         /* OUT: Size of (*pa) in bytes */
){
  fts5_config_t *config = ((fts5_table_t*)(csr->base.vtab))->config;
  int rc = DBSQL_OK;
  int live = (csr->sorter==0);

  if( i_phrase<0 || i_phrase>=__fts5_expr_phrase_count(csr->expr) ){
    rc = DBSQL_RANGE;
  }else if( config->detail!=FTS5_DETAIL_FULL 
         && __fts5_is_contentless((fts5_full_table_t*)csr->base.vtab, 1)
  ){
    *pa = 0;
    *pn = 0;
    return DBSQL_OK;
  }else if( CSR_FLAG_TEST(csr, FTS5CSR_REQUIRE_POSLIST) ){
    if( config->detail!=FTS5_DETAIL_FULL ){
      fts5_poslist_populator_t *populators;
      int i;

      populators = __fts5_expr_clear_poslists(csr->expr, live);
      if( populators==0 ) rc = DBSQL_NOMEM;
      if( rc==DBSQL_OK ){
        rc = __fts5_seek_cursor(csr, 0);
      }
      for(i=0; i<config->n_col && rc==DBSQL_OK; i++){
        const char *z = 0;
        int n = 0; 
        rc = __fts5_text_from_stmt(config, csr->stmt, i, &z, &n);
        if( rc==DBSQL_OK ){
          rc = __fts5_expr_populate_poslists(
              config, csr->expr, populators, i, z, n
          );
        }
        __fts5_clear_locale(config);
      }
      dbsql_free(populators);

      if( csr->sorter ){
        __fts5_expr_check_poslists(csr->expr, csr->sorter->i_rowid);
      }
    }
    CSR_FLAG_CLEAR(csr, FTS5CSR_REQUIRE_POSLIST);
  }

  if( rc==DBSQL_OK ){
    if( csr->sorter && config->detail==FTS5_DETAIL_FULL ){
      fts5_sorter_t *sorter = csr->sorter;
      int i1 = (i_phrase==0 ? 0 : sorter->idx[i_phrase-1]);
      *pn = sorter->idx[i_phrase] - i1;
      *pa = &sorter->poslists[i1];
    }else{
      *pn = __fts5_expr_poslist(csr->expr, i_phrase, pa);
    }
  }else{
    *pa = 0;
    *pn = 0;
  }

  return rc;
}

/*
** Ensure that the fts5_cursor_t.n_inst_count and insts[] variables are populated
** correctly for the current view. Return DBSQL_OK if successful, or an
** dbsql error code otherwise.
*/
static int __fts5_cache_inst_array(fts5_cursor_t *csr){
  int rc = DBSQL_OK;
  fts5_poslist_reader_t *iters;       /* One iterator for each phrase */
  int n_iter;                      /* Number of iterators/phrases */
  int n_col = ((fts5_table_t*)csr->base.vtab)->config->n_col;
  
  n_iter = __fts5_expr_phrase_count(csr->expr);
  if( csr->inst_iters==0 ){
    int64_t n_byte = sizeof(fts5_poslist_reader_t) * n_iter;
    csr->inst_iters = (fts5_poslist_reader_t*)__fts5_malloc_zero(&rc, n_byte);
  }
  iters = csr->inst_iters;

  if( iters ){
    int n_inst = 0;                /* Number instances seen so far */
    int i;

    /* Initialize all iterators */
    for(i=0; i<n_iter && rc==DBSQL_OK; i++){
      const u_int8_t *a;
      int n; 
      rc = __fts5_csr_poslist(csr, i, &a, &n);
      if( rc==DBSQL_OK ){
        __fts5_poslist_reader_init(a, n, &iters[i]);
      }
    }

    if( rc==DBSQL_OK ){
      while( 1 ){
        int *insts;
        int i_best = -1;
        for(i=0; i<n_iter; i++){
          if( (iters[i].eof==0) 
              && (i_best<0 || iters[i].i_pos<iters[i_best].i_pos) 
            ){
            i_best = i;
          }
        }
        if( i_best<0 ) break;

        n_inst++;
        if( n_inst>=csr->n_inst_alloc ){
          int n_new_size = csr->n_inst_alloc ? csr->n_inst_alloc*2 : 32;
          insts = (int*)dbsql_realloc64(
              csr->insts, n_new_size*sizeof(int)*3
              );
          if( insts ){
            csr->insts = insts;
            csr->n_inst_alloc = n_new_size;
          }else{
            n_inst--;
            rc = DBSQL_NOMEM;
            break;
          }
        }

        insts = &csr->insts[3 * (n_inst-1)];
        insts[0] = i_best;
        insts[1] = FTS5_POS2COLUMN(iters[i_best].i_pos);
        insts[2] = FTS5_POS2OFFSET(iters[i_best].i_pos);
        assert( insts[1]>=0 );
        if( insts[1]>=n_col ){
          rc = FTS5_CORRUPT;
          break;
        }
        __fts5_poslist_reader_next(&iters[i_best]);
      }
    }

    csr->n_inst_count = n_inst;
    CSR_FLAG_CLEAR(csr, FTS5CSR_REQUIRE_INST);
  }
  return rc;
}

static int __fts5_api_inst_count(fts5_context_t *ctx, int *pn_inst){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  int rc = DBSQL_OK;
  if( CSR_FLAG_TEST(csr, FTS5CSR_REQUIRE_INST)==0 
   || DBSQL_OK==(rc = __fts5_cache_inst_array(csr)) ){
    *pn_inst = csr->n_inst_count;
  }
  return rc;
}

static int __fts5_api_inst(
  fts5_context_t *ctx, 
  int i_idx, 
  int *pi_phrase, 
  int *pi_col, 
  int *pi_off
){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  int rc = DBSQL_OK;
  if( CSR_FLAG_TEST(csr, FTS5CSR_REQUIRE_INST)==0 
   || DBSQL_OK==(rc = __fts5_cache_inst_array(csr)) 
  ){
    if( i_idx<0 || i_idx>=csr->n_inst_count ){
      rc = DBSQL_RANGE;
    }else{
      *pi_phrase = csr->insts[i_idx*3];
      *pi_col = csr->insts[i_idx*3 + 1];
      *pi_off = csr->insts[i_idx*3 + 2];
    }
  }
  return rc;
}

static int64_t __fts5_api_rowid(fts5_context_t *ctx){
  return __fts5_cursor_rowid((fts5_cursor_t*)ctx);
}

static int __fts5_column_size_cb(
  void *context,                 /* Pointer to int */
  int tflags,
  const char *unused,            /* Buffer containing token */
  int n_unused,                    /* Size of token in bytes */
  int i_unused1,                   /* Start offset of token */
  int i_unused2                    /* End offset of token */
){
  int *cnt = (int*)context;
  UNUSED_PARAM2(unused, n_unused);
  UNUSED_PARAM2(i_unused1, i_unused2);
  if( (tflags & FTS5_TOKEN_COLOCATED)==0 ){
    (*cnt)++;
  }
  return DBSQL_OK;
}

static int __fts5_api_column_size(fts5_context_t *ctx, int i_col, int *pn_token){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_full_table_t *tab = (fts5_full_table_t*)(csr->base.vtab);
  fts5_config_t *config = tab->p.config;
  int rc = DBSQL_OK;

  if( CSR_FLAG_TEST(csr, FTS5CSR_REQUIRE_DOCSIZE) ){
    if( config->columnsize ){
      int64_t i_rowid = __fts5_cursor_rowid(csr);
      rc = __fts5_storage_docsize(tab->storage, i_rowid, csr->column_sizes);
    }else if( !config->z_content || config->content==FTS5_CONTENT_UNINDEXED ){
      int i;
      for(i=0; i<config->n_col; i++){
        if( config->ab_unindexed[i]==0 ){
          csr->column_sizes[i] = -1;
        }
      }
    }else{
      int i;
      rc = __fts5_seek_cursor(csr, 0);
      for(i=0; rc==DBSQL_OK && i<config->n_col; i++){
        if( config->ab_unindexed[i]==0 ){
          const char *z = 0; 
          int n = 0;
          csr->column_sizes[i] = 0;
          rc = __fts5_text_from_stmt(config, csr->stmt, i, &z, &n);
          if( rc==DBSQL_OK ){
            rc = __fts5_tokenize(config, FTS5_TOKENIZE_AUX, 
                z, n, (void*)&csr->column_sizes[i], __fts5_column_size_cb
            );
          }
          __fts5_clear_locale(config);
        }
      }
    }
    CSR_FLAG_CLEAR(csr, FTS5CSR_REQUIRE_DOCSIZE);
  }
  if( i_col<0 ){
    int i;
    *pn_token = 0;
    for(i=0; i<config->n_col; i++){
      *pn_token += csr->column_sizes[i];
    }
  }else if( i_col<config->n_col ){
    *pn_token = csr->column_sizes[i_col];
  }else{
    *pn_token = 0;
    rc = DBSQL_RANGE;
  }
  return rc;
}

/*
** Implementation of the set_auxdata() method.
*/
static int __fts5_api_set_auxdata(
  fts5_context_t *ctx,              /* Fts5 context */
  void *ptr,                     /* Pointer to save as auxdata */
  void(*x_delete)(void*)           /* Destructor for ptr (or NULL) */
){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_auxdata_t *data;

  /* Search through the cursors list of fts5_auxdata_t objects for one that
  ** corresponds to the currently executing auxiliary function.  */
  for(data=csr->auxdata; data; data=data->next){
    if( data->aux==csr->aux ) break;
  }

  if( data ){
    if( data->x_delete ){
      data->x_delete(data->ptr);
    }
  }else{
    int rc = DBSQL_OK;
    data = (fts5_auxdata_t*)__fts5_malloc_zero(&rc, sizeof(fts5_auxdata_t));
    if( data==0 ){
      if( x_delete ) x_delete(ptr);
      return rc;
    }
    data->aux = csr->aux;
    data->next = csr->auxdata;
    csr->auxdata = data;
  }

  data->x_delete = x_delete;
  data->ptr = ptr;
  return DBSQL_OK;
}

static void *__fts5_api_get_auxdata(fts5_context_t *ctx, int clear){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_auxdata_t *data;
  void *ret = 0;

  for(data=csr->auxdata; data; data=data->next){
    if( data->aux==csr->aux ) break;
  }

  if( data ){
    ret = data->ptr;
    if( clear ){
      data->ptr = 0;
      data->x_delete = 0;
    }
  }

  return ret;
}

static void __fts5_api_phrase_next(
  fts5_context_t *ctx, 
  fts5_phrase_iter_t *iter, 
  int *pi_col, int *pi_off
){
  if( iter->a>=iter->b ){
    *pi_col = -1;
    *pi_off = -1;
  }else{
    int i_val;
    iter->a += FTS5_GET_VARINT32(iter->a, i_val);
    if( i_val==1 ){
      /* Avoid returning a (*pi_col) value that is too large for the table,
      ** even if the position-list is corrupt. The caller might not be
      ** expecting it.  */
      int n_col = ((fts5_table_t*)(((fts5_cursor_t*)ctx)->base.vtab))->config->n_col;
      iter->a += FTS5_GET_VARINT32(iter->a, i_val);
      *pi_col = (i_val>=n_col ? n_col-1 : i_val);
      *pi_off = 0;
      iter->a += FTS5_GET_VARINT32(iter->a, i_val);
    }
    *pi_off += (i_val-2);
  }
}

static int __fts5_api_phrase_first(
  fts5_context_t *ctx, 
  int i_phrase, 
  fts5_phrase_iter_t *iter, 
  int *pi_col, int *pi_off
){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  int n;
  int rc = __fts5_csr_poslist(csr, i_phrase, &iter->a, &n);
  if( rc==DBSQL_OK ){
    assert( iter->a || n==0 );
    iter->b = (iter->a ? &iter->a[n] : 0);
    *pi_col = 0;
    *pi_off = 0;
    __fts5_api_phrase_next(ctx, iter, pi_col, pi_off);
  }
  return rc;
}

static void __fts5_api_phrase_next_column(
  fts5_context_t *ctx, 
  fts5_phrase_iter_t *iter, 
  int *pi_col
){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_config_t *config = ((fts5_table_t*)(csr->base.vtab))->config;

  if( config->detail==FTS5_DETAIL_COLUMNS ){
    if( iter->a>=iter->b ){
      *pi_col = -1;
    }else{
      int i_incr;
      iter->a += FTS5_GET_VARINT32(&iter->a[0], i_incr);
      *pi_col += (i_incr-2);
    }
  }else{
    while( 1 ){
      int dummy;
      if( iter->a>=iter->b ){
        *pi_col = -1;
        return;
      }
      if( iter->a[0]==0x01 ) break;
      iter->a += FTS5_GET_VARINT32(iter->a, dummy);
    }
    iter->a += 1 + FTS5_GET_VARINT32(&iter->a[1], *pi_col);
  }
}

static int __fts5_api_phrase_first_column(
  fts5_context_t *ctx, 
  int i_phrase, 
  fts5_phrase_iter_t *iter, 
  int *pi_col
){
  int rc = DBSQL_OK;
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_config_t *config = ((fts5_table_t*)(csr->base.vtab))->config;

  if( config->detail==FTS5_DETAIL_COLUMNS ){
    fts5_sorter_t *sorter = csr->sorter;
    if( i_phrase<0 || i_phrase>=__fts5_expr_phrase_count(csr->expr) ){
      rc = DBSQL_RANGE;
    }else{
      int n;
      if( sorter ){
        int i1 = (i_phrase==0 ? 0 : sorter->idx[i_phrase-1]);
        n = sorter->idx[i_phrase] - i1;
        iter->a = &sorter->poslists[i1];
      }else{
        rc = __fts5_expr_phrase_collist(csr->expr, i_phrase, &iter->a, &n);
      }
      if( rc==DBSQL_OK ){
        assert( iter->a || n==0 );
        iter->b = (iter->a ? &iter->a[n] : 0);
        *pi_col = 0;
        __fts5_api_phrase_next_column(ctx, iter, pi_col);
      }
    }
  }else{
    int n;
    rc = __fts5_csr_poslist(csr, i_phrase, &iter->a, &n);
    if( rc==DBSQL_OK ){
      assert( iter->a || n==0 );
      iter->b = (iter->a ? &iter->a[n] : 0);
      if( n<=0 ){
        *pi_col = -1;
      }else if( iter->a[0]==0x01 ){
        iter->a += 1 + FTS5_GET_VARINT32(&iter->a[1], *pi_col);
      }else{
        *pi_col = 0;
      }
    }
  }

  return rc;
}

/*
** query_token() API implemenetation.
*/
static int __fts5_api_query_token(
  fts5_context_t* ctx, 
  int i_phrase, 
  int i_token, 
  const char **out, 
  int *pn_out
){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  return __fts5_expr_query_token(csr->expr, i_phrase, i_token, out, pn_out);
}

/*
** inst_token() API implemenetation.
*/
static int __fts5_api_inst_token(
  fts5_context_t *ctx,
  int i_idx, 
  int i_token,
  const char **out, int *pn_out
){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  int rc = DBSQL_OK;
  if( CSR_FLAG_TEST(csr, FTS5CSR_REQUIRE_INST)==0 
   || DBSQL_OK==(rc = __fts5_cache_inst_array(csr)) 
  ){
    if( i_idx<0 || i_idx>=csr->n_inst_count ){
      rc = DBSQL_RANGE;
    }else{
      int i_phrase = csr->insts[i_idx*3];
      int i_col = csr->insts[i_idx*3 + 1];
      int i_off = csr->insts[i_idx*3 + 2];
      int64_t i_rowid = __fts5_cursor_rowid(csr);
      rc = __fts5_expr_inst_token(
          csr->expr, i_rowid, i_phrase, i_col, i_off, i_token, out, pn_out
      );
    }
  }
  return rc;
}


static int __fts5_api_query_phrase(fts5_context_t*, int, void*, 
    int(*)(const fts5_extension_api_t*, fts5_context_t*, void*)
);

/*
** The column_locale() API.
*/
static int __fts5_api_column_locale(
  fts5_context_t *ctx, 
  int i_col, 
  const char **locale, 
  int *pn_locale
){
  int rc = DBSQL_OK;
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_config_t *config = ((fts5_table_t*)(csr->base.vtab))->config;

  *locale = 0;
  *pn_locale = 0;

  assert( csr->plan!=FTS5_PLAN_SPECIAL );
  if( i_col<0 || i_col>=config->n_col ){
    rc = DBSQL_RANGE;
  }else if(
      config->ab_unindexed[i_col]==0
   && 0==__fts5_is_contentless((fts5_full_table_t*)csr->base.vtab, 1)
   && config->locale
  ){
    rc = __fts5_seek_cursor(csr, 0);
    if( rc==DBSQL_OK ){
      const char *dummy = 0;
      int n_dummy = 0;
      rc = __fts5_text_from_stmt(config, csr->stmt, i_col, &dummy, &n_dummy);
      if( rc==DBSQL_OK ){
        *locale = config->t.locale;
        *pn_locale = config->t.n_locale;
      }
      __fts5_clear_locale(config);
    }
  }

  return rc;
}

static const fts5_extension_api_t __s_fts5_api = {
  4,                            /* i_version */
  __fts5_api_user_data,
  __fts5_api_column_count,
  __fts5_api_row_count,
  __fts5_api_column_total_size,
  __fts5_api_tokenize,
  __fts5_api_phrase_count,
  __fts5_api_phrase_size,
  __fts5_api_inst_count,
  __fts5_api_inst,
  __fts5_api_rowid,
  __fts5_api_column_text,
  __fts5_api_column_size,
  __fts5_api_query_phrase,
  __fts5_api_set_auxdata,
  __fts5_api_get_auxdata,
  __fts5_api_phrase_first,
  __fts5_api_phrase_next,
  __fts5_api_phrase_first_column,
  __fts5_api_phrase_next_column,
  __fts5_api_query_token,
  __fts5_api_inst_token,
  __fts5_api_column_locale,
  __fts5_api_tokenize_v2
};

/*
** Implementation of API function query_phrase().
*/
static int __fts5_api_query_phrase(
  fts5_context_t *ctx, 
  int i_phrase, 
  void *user_data,
  int(*callback)(const fts5_extension_api_t*, fts5_context_t*, void*)
){
  fts5_cursor_t *csr = (fts5_cursor_t*)ctx;
  fts5_full_table_t *tab = (fts5_full_table_t*)(csr->base.vtab);
  int rc;
  fts5_cursor_t *p_new = 0;

  rc = __fts5_open_method(csr->base.vtab, (dbsql_vtab_cursor_t**)&p_new);
  if( rc==DBSQL_OK ){
    p_new->plan = FTS5_PLAN_MATCH;
    p_new->i_first_rowid = SMALLEST_INT64;
    p_new->i_last_rowid = LARGEST_INT64;
    p_new->base.vtab = (dbsql_vtab_t*)tab;
    rc = __fts5_expr_clone_phrase(csr->expr, i_phrase, &p_new->expr);
  }

  if( rc==DBSQL_OK ){
    for(rc = __fts5_cursor_first(tab, p_new, 0);
        rc==DBSQL_OK && CSR_FLAG_TEST(p_new, FTS5CSR_EOF)==0;
        rc = __fts5_next_method((dbsql_vtab_cursor_t*)p_new)
    ){
      rc = callback(&__s_fts5_api, (fts5_context_t*)p_new, user_data);
      if( rc!=DBSQL_OK ){
        if( rc==DBSQL_DONE ) rc = DBSQL_OK;
        break;
      }
    }
  }

  __fts5_close_method((dbsql_vtab_cursor_t*)p_new);
  return rc;
}

static void __fts5_api_invoke(
  fts5_auxiliary_t *aux,
  fts5_cursor_t *csr,
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  assert( csr->aux==0 );
  assert( csr->plan!=FTS5_PLAN_SPECIAL );
  csr->aux = aux;
  aux->func(&__s_fts5_api, (fts5_context_t*)csr, context, argc, argv);
  csr->aux = 0;
}

static fts5_cursor_t *__fts5_cursor_from_csrid(fts5_global_t *__p_global, int64_t i_csr_id){
  fts5_cursor_t *csr;
  for(csr=__p_global->csr; csr; csr=csr->next){
    if( csr->i_csr_id==i_csr_id ) break;
  }
  return csr;
}

/*
** Parameter fmt is a printf() style formatting string. This function
** formats it using the trailing arguments and returns the result as
** an error message to the context passed as the first argument.
*/
static void __fts5_result_error(dbsql_func_t *ctx, const char *fmt, ...){
  char *err = 0;
  va_list ap;
  va_start(ap, fmt);
  err = dbsql_vmprintf(fmt, ap);
  dbsql_result_error(ctx, err, -1);
  dbsql_free(err);
  va_end(ap);
}

static void __fts5_api_callback(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){

  fts5_auxiliary_t *aux;
  fts5_cursor_t *csr;
  int64_t i_csr_id;

  assert( argc>=1 );
  aux = (fts5_auxiliary_t*)dbsql_user_data(context);
  i_csr_id = dbsql_value_int64(argv[0]);

  csr = __fts5_cursor_from_csrid(aux->__p_global, i_csr_id);
  if( csr==0 || (csr->plan==0 || csr->plan==FTS5_PLAN_SPECIAL) ){
    __fts5_result_error(context, "no such cursor: %lld", i_csr_id);
  }else{
    dbsql_vtab_t *tab = csr->base.vtab;
    __fts5_api_invoke(aux, csr, context, argc-1, &argv[1]);
    dbsql_free(tab->errmsg);
    tab->errmsg = 0;
  }
}


/*
** Given cursor id i_id, return a pointer to the corresponding fts5_table_t 
** object. Or NULL If the cursor id does not exist.
*/
fts5_table_t *__fts5_table_from_csrid(
  fts5_global_t *__p_global,            /* FTS5 global context for db handle */
  int64_t i_csr_id                      /* Id of cursor to find */
){
  fts5_cursor_t *csr;
  csr = __fts5_cursor_from_csrid(__p_global, i_csr_id);
  if( csr ){
    return (fts5_table_t*)csr->base.vtab;
  }
  return 0;
}

/*
** Return a "position-list blob" corresponding to the current position of
** cursor csr via dbsql_result_blob(). A position-list blob contains
** the current position-list for each phrase in the query associated with
** cursor csr.
**
** A position-list blob begins with (n_phrase-1) varints, where n_phrase is
** the number of phrases in the query. Following the varints are the
** concatenated position lists for each phrase, in order.
**
** The first varint (if it exists) contains the size of the position list
** for phrase 0. The second (same disclaimer) contains the size of position
** list 1. And so on. There is no size field for the final position list,
** as it can be derived from the total size of the blob.
*/
static int __fts5_poslist_blob(dbsql_func_t *ctx, fts5_cursor_t *csr){
  int i;
  int rc = DBSQL_OK;
  int n_phrase = __fts5_expr_phrase_count(csr->expr);
  fts5_buffer_t val;

  memset(&val, 0, sizeof(fts5_buffer_t));
  switch( ((fts5_table_t*)(csr->base.vtab))->config->detail ){
    case FTS5_DETAIL_FULL:

      /* Append the varints */
      for(i=0; i<(n_phrase-1); i++){
        const u_int8_t *dummy;
        int n_byte = __fts5_expr_poslist(csr->expr, i, &dummy);
        __fts5_buffer_append_varint(&rc, &val, n_byte);
      }

      /* Append the position lists */
      for(i=0; i<n_phrase; i++){
        const u_int8_t *poslist;
        int n_poslist;
        n_poslist = __fts5_expr_poslist(csr->expr, i, &poslist);
        __fts5_buffer_append_blob(&rc, &val, n_poslist, poslist);
      }
      break;

    case FTS5_DETAIL_COLUMNS:

      /* Append the varints */
      for(i=0; rc==DBSQL_OK && i<(n_phrase-1); i++){
        const u_int8_t *dummy;
        int n_byte;
        rc = __fts5_expr_phrase_collist(csr->expr, i, &dummy, &n_byte);
        __fts5_buffer_append_varint(&rc, &val, n_byte);
      }

      /* Append the position lists */
      for(i=0; rc==DBSQL_OK && i<n_phrase; i++){
        const u_int8_t *poslist;
        int n_poslist;
        rc = __fts5_expr_phrase_collist(csr->expr, i, &poslist, &n_poslist);
        __fts5_buffer_append_blob(&rc, &val, n_poslist, poslist);
      }
      break;

    default:
      break;
  }

  dbsql_result_blob(ctx, val.p, val.n, dbsql_free);
  return rc;
}

/* 
** This is the column method, called by dbsql to request a value from
** the row that the supplied cursor currently points to.
*/
static int __fts5_column_method(
  dbsql_vtab_cursor_t *cursor,   /* Cursor to retrieve value from */
  dbsql_func_t *ctx,          /* Context for dbsql_result_xxx() calls */
  int i_col                        /* index_t of column to read value from */
){
  fts5_full_table_t *tab = (fts5_full_table_t*)(cursor->vtab);
  fts5_config_t *config = tab->p.config;
  fts5_cursor_t *csr = (fts5_cursor_t*)cursor;
  int rc = DBSQL_OK;
  
  assert( CSR_FLAG_TEST(csr, FTS5CSR_EOF)==0 );

  if( csr->plan==FTS5_PLAN_SPECIAL ){
    if( i_col==config->n_col ){
      dbsql_result_int64(ctx, csr->i_special);
    }
  }else

  if( i_col==config->n_col ){
    /* User is requesting the value of the special column with the same name
    ** as the table. Return the cursor integer id number. This value is only
    ** useful in that it may be passed as the first argument to an FTS5
    ** auxiliary function.  */
    dbsql_result_int64(ctx, csr->i_csr_id);
  }else if( i_col==config->n_col+1 ){
    /* The value of the "rank" column. */

    if( csr->plan==FTS5_PLAN_SOURCE ){
      __fts5_poslist_blob(ctx, csr);
    }else if( 
        csr->plan==FTS5_PLAN_MATCH
     || csr->plan==FTS5_PLAN_SORTED_MATCH
    ){
      if( csr->rank || DBSQL_OK==(rc = __fts5_find_rank_function(csr)) ){
        __fts5_api_invoke(csr->rank, csr, ctx, csr->n_rank_arg, csr->rank_args);
      }
    }
  }else{
    if( !dbsql_vtab_nochange(ctx) && config->content!=FTS5_CONTENT_NONE ){
      config->errmsg = &tab->p.base.errmsg;
      rc = __fts5_seek_cursor(csr, 1);
      if( rc==DBSQL_OK ){
        dbsql_value_t *val = dbsql_column_value(csr->stmt, i_col+1);
        if( config->locale 
         && config->content==FTS5_CONTENT_EXTERNAL 
         && __fts5_is_locale_value(config, val)
        ){
          const char *z = 0;
          int n = 0;
          rc = __fts5_text_from_stmt(config, csr->stmt, i_col, &z, &n);
          if( rc==DBSQL_OK ){
            dbsql_result_text(ctx, z, n, DBSQL_TRANSIENT);
          }
          __fts5_clear_locale(config);
        }else{
          dbsql_result_value(ctx, val);
        }
      }

      config->errmsg = 0;
    }
  }

  return rc;
}


/*
** This routine implements the find_function method for the FTS3
** virtual table.
*/
static int __fts5_find_function_method(
  dbsql_vtab_t *vtab,            /* Virtual table handle */
  int n_unused,                    /* Number of SQL function arguments */
  const char *name,              /* Name of SQL function */
  void (**px_func)(dbsql_func_t*,int,dbsql_value_t**), /* OUT: Result */
  void **arg                    /* OUT: User data for *px_func */
){
  fts5_full_table_t *tab = (fts5_full_table_t*)vtab;
  fts5_auxiliary_t *aux;

  UNUSED_PARAM(n_unused);
  aux = __fts5_find_auxiliary(tab, name);
  if( aux ){
    *px_func = __fts5_api_callback;
    *arg = (void*)aux;
    return 1;
  }

  /* No function of the specified name was found. Return 0. */
  return 0;
}

/*
** Implementation of FTS5 rename method. Rename an fts5 table.
*/
static int __fts5_rename_method(
  dbsql_vtab_t *vtab,            /* Virtual table handle */
  const char *name               /* New name of table */
){
  int rc;
  fts5_full_table_t *tab = (fts5_full_table_t*)vtab;
  rc = __fts5_storage_rename(tab->storage, name);
  return rc;
}

int __fts5_flush_to_disk(fts5_table_t *tab){
  __fts5_trip_cursors((fts5_full_table_t*)tab);
  return __fts5_storage_sync(((fts5_full_table_t*)tab)->storage);
}

/*
** The savepoint() method.
**
** Flush the contents of the pending-terms table to disk.
*/
static int __fts5_savepoint_method(dbsql_vtab_t *vtab, int i_savepoint){
  fts5_full_table_t *tab = (fts5_full_table_t*)vtab;
  int rc = DBSQL_OK;

  FTS5_CHECK_TRANSACTION_STATE(tab, FTS5_SAVEPOINT, i_savepoint);
  rc = __fts5_flush_to_disk((fts5_table_t*)vtab);
  if( rc==DBSQL_OK ){
    tab->i_savepoint = i_savepoint+1;
  }
  return rc;
}

/*
** The release() method.
**
** This is a no-op.
*/
static int __fts5_release_method(dbsql_vtab_t *vtab, int i_savepoint){
  fts5_full_table_t *tab = (fts5_full_table_t*)vtab;
  int rc = DBSQL_OK;
  FTS5_CHECK_TRANSACTION_STATE(tab, FTS5_RELEASE, i_savepoint);
  if( (i_savepoint+1)<tab->i_savepoint ){
    rc = __fts5_flush_to_disk(&tab->p);
    if( rc==DBSQL_OK ){
      tab->i_savepoint = i_savepoint;
    }
  }
  return rc;
}

/*
** The rollback_to() method.
**
** Discard the contents of the pending terms table.
*/
static int __fts5_rollback_to_method(dbsql_vtab_t *vtab, int i_savepoint){
  fts5_full_table_t *tab = (fts5_full_table_t*)vtab;
  int rc = DBSQL_OK;
  FTS5_CHECK_TRANSACTION_STATE(tab, FTS5_ROLLBACKTO, i_savepoint);
  __fts5_trip_cursors(tab);
  if( (i_savepoint+1)<=tab->i_savepoint ){
    tab->p.config->pgsz = 0;
    rc = __fts5_storage_rollback(tab->storage);
  }
  return rc;
}

/*
** Register a new auxiliary function with global context __p_global.
*/
static int __fts5_create_aux(
  fts5_api *api,                 /* Global context (one per db handle) */
  const char *name,              /* Name of new function */
  void *user_data,                /* User data for aux. function */
  fts5_extension_function func,  /* Aux. function implementation */
  void(*destroy)(void*)          /* Destructor for user_data */
){
  fts5_global_t *__p_global = (fts5_global_t*)api;
  int rc = dbsql_overload_function(__p_global->db, name, -1);
  if( rc==DBSQL_OK ){
    fts5_auxiliary_t *aux;
    int64_t n_name;            /* Size of name in bytes, including \0 */
    int64_t n_byte;            /* Bytes of space to allocate */

    n_name = strlen(name) + 1;
    n_byte = sizeof(fts5_auxiliary_t) + n_name;
    aux = (fts5_auxiliary_t*)dbsql_malloc64(n_byte);
    if( aux ){
      memset(aux, 0, (size_t)n_byte);
      aux->z_func = (char*)&aux[1];
      memcpy(aux->z_func, name, n_name);
      aux->__p_global = __p_global;
      aux->user_data = user_data;
      aux->func = func;
      aux->destroy = destroy;
      aux->next = __p_global->aux;
      __p_global->aux = aux;
    }else{
      rc = DBSQL_NOMEM;
    }
  }

  return rc;
}

/*
** This function is used by create_tokenizer_v2() and create_tokenizer().
** It allocates and partially populates a new fts5_tokenizer_module_t object.
** The new object is already linked into the fts5_global_t context before
** returning.
**
** If successful, DBSQL_OK is returned and a pointer to the new
** fts5_tokenizer_module_t object returned via output parameter (*pp_new). All
** that is required is for the caller to fill in the methods in
** fts5_tokenizer_module_t.x1 and x2, and to set fts5_tokenizer_module_t.v2_native
** as appropriate.
**
** If an error occurs, an dbsql error code is returned and the final value
** of (*pp_new) undefined.
*/
static int __fts5_new_tokenizer_module(
  fts5_global_t *__p_global,            /* Global context (one per db handle) */
  const char *name,              /* Name of new function */
  void *user_data,                /* User data for aux. function */
  void(*destroy)(void*),         /* Destructor for user_data */
  fts5_tokenizer_module_t **pp_new
){
  int rc = DBSQL_OK;
  fts5_tokenizer_module_t *p_new;
  int64_t n_name;          /* Size of name and its \0 terminator */
  int64_t n_byte;          /* Bytes of space to allocate */

  n_name = strlen(name) + 1;
  n_byte = sizeof(fts5_tokenizer_module_t) + n_name;
  *pp_new = p_new = (fts5_tokenizer_module_t*)__fts5_malloc_zero(&rc, n_byte);
  if( p_new ){
    p_new->name = (char*)&p_new[1];
    memcpy(p_new->name, name, n_name);
    p_new->user_data = user_data;
    p_new->destroy = destroy;
    p_new->next = __p_global->p_tok;
    __p_global->p_tok = p_new;
    if( p_new->next==0 ){
      __p_global->dflt_tok = p_new;
    }
  }

  return rc;
}

/*
** An instance of this type is used as the fts5_tokenizer_t object for
** wrapper tokenizers - those that provide access to a v1 tokenizer via
** the fts5_tokenizer_v2 API, and those that provide access to a v2 tokenizer
** via the fts5_tokenizer API.
*/
typedef struct fts5_vto_v_tokenizer_t fts5_vto_v_tokenizer_t;
struct fts5_vto_v_tokenizer_t {
  int v2_native;                  /* True if v2 native tokenizer */
  fts5_tokenizer x1;              /* Tokenizer functions */
  fts5_tokenizer_v2 x2;           /* V2 tokenizer functions */
  fts5_tokenizer_t *real;
};

/*
** Create a wrapper tokenizer. The context argument ctx points to the
** fts5_tokenizer_module_t object.
*/
static int __fts5_vto_v_create(
  void *ctx, 
  const char **args, 
  int n_arg, 
  fts5_tokenizer_t **out
){
  fts5_tokenizer_module_t *mod = (fts5_tokenizer_module_t*)ctx;
  fts5_vto_v_tokenizer_t *p_new = 0;
  int rc = DBSQL_OK;

  p_new = (fts5_vto_v_tokenizer_t*)__fts5_malloc_zero(&rc, sizeof(*p_new));
  if( rc==DBSQL_OK ){
    p_new->x1 = mod->x1;
    p_new->x2 = mod->x2;
    p_new->v2_native = mod->v2_native;
    if( mod->v2_native ){
      rc = mod->x2.create(mod->user_data, args, n_arg, &p_new->real);
    }else{
      rc = mod->x1.create(mod->user_data, args, n_arg, &p_new->real);
    }
    if( rc!=DBSQL_OK ){
      dbsql_free(p_new);
      p_new = 0;
    }
  }

  *out = (fts5_tokenizer_t*)p_new;
  return rc;
}

/*
** Delete an fts5_vto_v_tokenizer_t wrapper tokenizer. 
*/
static void __fts5_vto_v_delete(fts5_tokenizer_t *tok){
  fts5_vto_v_tokenizer_t *p = (fts5_vto_v_tokenizer_t*)tok;
  if( p ){
    if( p->v2_native ){
      p->x2.x_delete(p->real);
    }else{
      p->x1.x_delete(p->real);
    }
    dbsql_free(p);
  }
}


/*
** tokenizer method for a wrapper tokenizer that offers the v1 interface
** (no support for locales).
*/
static int __fts5_v1to_v2_tokenize(
  fts5_tokenizer_t *tok, 
  void *ctx, int flags,
  const char *text, int n_text, 
  int (*token)(void*, int, const char*, int, int, int)
){
  fts5_vto_v_tokenizer_t *p = (fts5_vto_v_tokenizer_t*)tok;
  assert( p->v2_native );
  return p->x2.tokenize(p->real, ctx, flags, text, n_text, 0, 0, token);
}

/*
** tokenizer method for a wrapper tokenizer that offers the v2 interface
** (with locale support).
*/
static int __fts5_v2to_v1_tokenize(
  fts5_tokenizer_t *tok, 
  void *ctx, int flags,
  const char *text, int n_text, 
  const char *locale, int n_locale, 
  int (*token)(void*, int, const char*, int, int, int)
){
  fts5_vto_v_tokenizer_t *p = (fts5_vto_v_tokenizer_t*)tok;
  assert( p->v2_native==0 );
  UNUSED_PARAM2(locale,n_locale);
  return p->x1.tokenize(p->real, ctx, flags, text, n_text, token);
}

/*
** Register a new tokenizer. This is the implementation of the 
** fts5_api.create_tokenizer_v2() method.
*/
static int __fts5_create_tokenizer_v2(
  fts5_api *api,                 /* Global context (one per db handle) */
  const char *name,              /* Name of new function */
  void *user_data,                /* User data for aux. function */
  fts5_tokenizer_v2 *tokenizer,  /* Tokenizer implementation */
  void(*destroy)(void*)          /* Destructor for user_data */
){
  fts5_global_t *__p_global = (fts5_global_t*)api;
  int rc = DBSQL_OK;

  if( tokenizer->i_version>2 ){
    rc = DBSQL_ERROR;
  }else{
    fts5_tokenizer_module_t *p_new = 0;
    rc = __fts5_new_tokenizer_module(__p_global, name, user_data, destroy, &p_new);
    if( p_new ){
      p_new->x2 = *tokenizer;
      p_new->v2_native = 1;
      p_new->x1.create = __fts5_vto_v_create;
      p_new->x1.tokenize = __fts5_v1to_v2_tokenize;
      p_new->x1.x_delete = __fts5_vto_v_delete;
    }
  }

  return rc;
}

/*
** The fts5_api.create_tokenizer() method.
*/
static int __fts5_create_tokenizer(
  fts5_api *api,                 /* Global context (one per db handle) */
  const char *name,              /* Name of new function */
  void *user_data,                /* User data for aux. function */
  fts5_tokenizer *tokenizer,     /* Tokenizer implementation */
  void(*destroy)(void*)          /* Destructor for user_data */
){
  fts5_tokenizer_module_t *p_new = 0;
  int rc = DBSQL_OK;

  rc = __fts5_new_tokenizer_module(
      (fts5_global_t*)api, name, user_data, destroy, &p_new
  );
  if( p_new ){
    p_new->x1 = *tokenizer;
    p_new->x2.create = __fts5_vto_v_create;
    p_new->x2.tokenize = __fts5_v2to_v1_tokenize;
    p_new->x2.x_delete = __fts5_vto_v_delete;
  }
  return rc;
}

/*
** Search the global context passed as the first argument for a tokenizer
** module named name. If found, return a pointer to the fts5_tokenizer_module_t
** object. Otherwise, return NULL.
*/
static fts5_tokenizer_module_t *__fts5_locate_tokenizer(
  fts5_global_t *__p_global,            /* Global (one per db handle) object */
  const char *name               /* Name of tokenizer module to find */
){
  fts5_tokenizer_module_t *mod = 0;

  if( name==0 ){
    mod = __p_global->dflt_tok;
  }else{
    for(mod=__p_global->p_tok; mod; mod=mod->next){
      if( dbsql_stricmp(name, mod->name)==0 ) break;
    }
  }

  return mod;
}

/*
** Find a tokenizer. This is the implementation of the 
** fts5_api.find_tokenizer_v2() method.
*/
static int __fts5_find_tokenizer_v2(
  fts5_api *api,                 /* Global context (one per db handle) */
  const char *name,              /* Name of tokenizer */
  void **user_data,
  fts5_tokenizer_v2 **tokenizer /* Populate this object */
){
  int rc = DBSQL_OK;
  fts5_tokenizer_module_t *mod;

  mod = __fts5_locate_tokenizer((fts5_global_t*)api, name);
  if( mod ){
    if( mod->v2_native ){
      *user_data = mod->user_data;
    }else{
      *user_data = (void*)mod;
    }
    *tokenizer = &mod->x2;
  }else{
    *tokenizer = 0;
    *user_data = 0;
    rc = DBSQL_ERROR;
  }

  return rc;
}

/*
** Find a tokenizer. This is the implementation of the 
** fts5_api.find_tokenizer() method.
*/
static int __fts5_find_tokenizer(
  fts5_api *api,                 /* Global context (one per db handle) */
  const char *name,              /* Name of new function */
  void **user_data,
  fts5_tokenizer *tokenizer      /* Populate this object */
){
  int rc = DBSQL_OK;
  fts5_tokenizer_module_t *mod;

  mod = __fts5_locate_tokenizer((fts5_global_t*)api, name);
  if( mod ){
    if( mod->v2_native==0 ){
      *user_data = mod->user_data;
    }else{
      *user_data = (void*)mod;
    }
    *tokenizer = mod->x1;
  }else{
    memset(tokenizer, 0, sizeof(*tokenizer));
    *user_data = 0;
    rc = DBSQL_ERROR;
  }

  return rc;
}

/*
** Attempt to instantiate the tokenizer.
*/
int __fts5_load_tokenizer(fts5_config_t *config){
  const char **args = config->t.args;
  const int n_arg = config->t.n_arg;
  fts5_tokenizer_module_t *mod = 0;
  int rc = DBSQL_OK;

  mod = __fts5_locate_tokenizer(config->__p_global, n_arg==0 ? 0 : args[0]);
  if( mod==0 ){
    assert( n_arg>0 );
    rc = DBSQL_ERROR;
    __fts5_config_errmsg(config, "no such tokenizer: %s", args[0]);
  }else{
    int (*create)(void*, const char**, int, fts5_tokenizer_t**) = 0;
    if( mod->v2_native ){
      create = mod->x2.create;
      config->t.api2 = &mod->x2;
    }else{
      config->t.api1 = &mod->x1;
      create = mod->x1.create;
    }
    
    rc = create(mod->user_data, 
        (args?&args[1]:0), (n_arg?n_arg-1:0), &config->t.p_tok
    );

    if( rc!=DBSQL_OK ){
      if( rc!=DBSQL_NOMEM ){
        __fts5_config_errmsg(config, "error in tokenizer constructor");
      }
    }else if( mod->v2_native==0 ){
      config->t.pattern = __fts5_tokenizer_pattern(
          mod->x1.create, config->t.p_tok
      );
    }
  }

  if( rc!=DBSQL_OK ){
    config->t.api1 = 0;
    config->t.api2 = 0;
    config->t.p_tok = 0;
  }

  return rc;
}


/*
** destroy callback passed to dbsql_create_module(). This is invoked
** when the db handle is being closed. Free memory associated with 
** tokenizers and aux functions registered with this db handle.
*/
static void __fts5_module_destroy(void *ctx){
  fts5_tokenizer_module_t *tok, *next_tok;
  fts5_auxiliary_t *aux, *next_aux;
  fts5_global_t *__p_global = (fts5_global_t*)ctx;

  for(aux=__p_global->aux; aux; aux=next_aux){
    next_aux = aux->next;
    if( aux->destroy ) aux->destroy(aux->user_data);
    dbsql_free(aux);
  }

  for(tok=__p_global->p_tok; tok; tok=next_tok){
    next_tok = tok->next;
    if( tok->destroy ) tok->destroy(tok->user_data);
    dbsql_free(tok);
  }

  dbsql_free(__p_global);
}

/*
** Implementation of the fts5() function used by clients to obtain the
** API pointer.
*/
static void fts5Fts5Func(
  dbsql_func_t *ctx,          /* Function call context */
  int n_arg,                       /* Number of args */
  dbsql_value_t **args           /* Function arguments */
){
  fts5_global_t *__p_global = (fts5_global_t*)dbsql_user_data(ctx);
  fts5_api **api;
  UNUSED_PARAM(n_arg);
  assert( n_arg==1 );
  api = (fts5_api**)dbsql_value_pointer(args[0], "fts5_api_ptr");
  if( api ) *api = &__p_global->api;
}

/*
** Implementation of fts5_source_id() function.
*/
static void __fts5_source_id_func(
  dbsql_func_t *ctx,          /* Function call context */
  int n_arg,                       /* Number of args */
  dbsql_value_t **unuseds        /* Function arguments */
){
  assert( n_arg==0 );
  UNUSED_PARAM2(n_arg, unuseds);
  dbsql_result_text(ctx, "--FTS5-SOURCE-ID--", -1, DBSQL_TRANSIENT);
}

/*
** Implementation of fts5_locale(LOCALE, TEXT) function.
**
** If parameter LOCALE is NULL, or a zero-length string, then a copy of
** TEXT is returned. Otherwise, both LOCALE and TEXT are interpreted as
** text, and the value returned is a blob consisting of:
**
**     * The 4 bytes 0x00, 0xE0, 0xB2, 0xEb (FTS5_LOCALE_HEADER).
**     * The LOCALE, as utf-8 text, followed by
**     * 0x00, followed by
**     * The TEXT, as utf-8 text.
**
** There is no final nul-terminator following the TEXT value.
*/
static void __fts5_locale_func(
  dbsql_func_t *ctx,          /* Function call context */
  int n_arg,                       /* Number of args */
  dbsql_value_t **args           /* Function arguments */
){
  const char *locale = 0;
  int64_t n_locale = 0;
  const char *text = 0;
  int64_t n_text = 0;

  assert( n_arg==2 );
  UNUSED_PARAM(n_arg);

  locale = (const char*)dbsql_value_text(args[0]);
  n_locale = dbsql_value_bytes(args[0]);

  text = (const char*)dbsql_value_text(args[1]);
  n_text = dbsql_value_bytes(args[1]);

  if( locale==0 || locale[0]=='\0' ){
    dbsql_result_text(ctx, text, n_text, DBSQL_TRANSIENT);
  }else{
    fts5_global_t *p = (fts5_global_t*)dbsql_user_data(ctx);
    u_int8_t *blob = 0;
    u_int8_t *csr = 0;
    int64_t n_blob = 0;

    n_blob = FTS5_LOCALE_HDR_SIZE + n_locale + 1 + n_text;
    blob = (u_int8_t*)dbsql_malloc64(n_blob);
    if( blob==0 ){
      dbsql_result_error_nomem(ctx);
      return;
    }

    csr = blob;
    memcpy(csr, (const u_int8_t*)p->locale_hdrs, FTS5_LOCALE_HDR_SIZE);
    csr += FTS5_LOCALE_HDR_SIZE;
    memcpy(csr, locale, n_locale);
    csr += n_locale;
    (*csr++) = 0x00;
    if( text ) memcpy(csr, text, n_text);
    assert( &csr[n_text]==&blob[n_blob] );

    dbsql_result_blob(ctx, blob, n_blob, dbsql_free);
  }
}

/*
** Implementation of fts5_insttoken() function.
*/
static void __fts5_insttoken_func(
  dbsql_func_t *ctx,          /* Function call context */
  int n_arg,                       /* Number of args */
  dbsql_value_t **args           /* Function arguments */
){
  assert( n_arg==1 );
  (void)n_arg;
  dbsql_result_value(ctx, args[0]);
  dbsql_result_subtype(ctx, FTS5_INSTTOKEN_SUBTYPE);
}

/*
** Return true if name is the extension on one of the shadow tables used
** by this module.
*/
static int __fts5_shadow_name(const char *name){
  static const char *names[] = {
    "config", "content", "data", "docsize", "idx"
  };
  unsigned int i;
  for(i=0; i<sizeof(names)/sizeof(names[0]); i++){
    if( dbsql_stricmp(name, names[i])==0 ) return 1;
  }
  return 0;
}

/*
** Run an integrity check on the FTS5 data structures.  Return a string
** if anything is found amiss.  Return a NULL pointer if everything is
** OK.
*/
static int __fts5_integrity_method(
  dbsql_vtab_t *vtab,    /* the FTS5 virtual table to check */
  const char *schema,    /* Name of schema in which this table lives */
  const char *tabname,   /* Name of the table itself */
  int is_quick,            /* True if this is a quick-check */
  char **err_p            /* Write error message here */
){
  fts5_full_table_t *tab = (fts5_full_table_t*)vtab;
  int rc;

  assert( err_p!=0 && *err_p==0 );
  UNUSED_PARAM(is_quick);
  assert( tab->p.config->errmsg==0 );
  tab->p.config->errmsg = err_p;
  rc = __fts5_storage_integrity(tab->storage, 0);
  if( *err_p==0 && rc!=DBSQL_OK ){
    if( (rc&0xff)==DBSQL_CORRUPT ){
      *err_p = dbsql_mprintf("malformed inverted index for FTS5 table %s.%s",
          schema, tabname);
      rc = (*err_p) ? DBSQL_OK : DBSQL_NOMEM;
    }else{
      *err_p = dbsql_mprintf("unable to validate the inverted index for"
          " FTS5 table %s.%s: %s",
          schema, tabname, dbsql_errstr(rc));
    }
  }else if( (rc&0xff)==DBSQL_CORRUPT ){
    rc = DBSQL_OK;
  }
  __fts5_index_close_reader(tab->p.p_index);
  tab->p.config->errmsg = 0;

  return rc;
}

static int fts5Init(DBSQL *db){
  static const dbsql_module_t fts5Mod = {
    /* i_version      */ 4,
    /* create       */ __fts5_create_method,
    /* connect      */ __fts5_connect_method,
    /* best_index    */ __fts5_best_index_method,
    /* disconnect   */ __fts5_disconnect_method,
    /* destroy      */ __fts5_destroy_method,
    /* x_open         */ __fts5_open_method,
    /* x_close        */ __fts5_close_method,
    /* filter       */ __fts5_filter_method,
    /* next         */ __fts5_next_method,
    /* eof          */ __fts5_eof_method,
    /* column       */ __fts5_column_method,
    /* rowid        */ __fts5_rowid_method,
    /* update       */ __fts5_update_method,
    /* begin        */ __fts5_begin_method,
    /* x_sync         */ __fts5_sync_method,
    /* commit       */ __fts5_commit_method,
    /* rollback     */ __fts5_rollback_method,
    /* find_function */ __fts5_find_function_method,
    /* rename       */ __fts5_rename_method,
    /* savepoint    */ __fts5_savepoint_method,
    /* release      */ __fts5_release_method,
    /* rollback_to   */ __fts5_rollback_to_method,
    /* shadow_name   */ __fts5_shadow_name,
    /* integrity    */ __fts5_integrity_method
  };

  int rc;
  fts5_global_t *__p_global = 0;

  __p_global = (fts5_global_t*)dbsql_malloc64(sizeof(fts5_global_t));
  if( __p_global==0 ){
    rc = DBSQL_NOMEM;
  }else{
    void *p = (void*)__p_global;
    memset(__p_global, 0, sizeof(fts5_global_t));
    __p_global->db = db;
    __p_global->api.i_version = 3;
    __p_global->api.create_function = __fts5_create_aux;
    __p_global->api.create_tokenizer = __fts5_create_tokenizer;
    __p_global->api.find_tokenizer = __fts5_find_tokenizer;
    __p_global->api.create_tokenizer_v2 = __fts5_create_tokenizer_v2;
    __p_global->api.find_tokenizer_v2 = __fts5_find_tokenizer_v2;

    /* Initialize __p_global->locale_hdrs[] to a 128-bit pseudo-random vector.
    ** The constants below were generated randomly.  */
    dbsql_randomness(sizeof(__p_global->locale_hdrs), __p_global->locale_hdrs);
    __p_global->locale_hdrs[0] ^= 0xF924976D;
    __p_global->locale_hdrs[1] ^= 0x16596E13;
    __p_global->locale_hdrs[2] ^= 0x7C80BEAA;
    __p_global->locale_hdrs[3] ^= 0x9B03A67F;
    assert( sizeof(__p_global->locale_hdrs)==16 );

    rc = dbsql_create_module_v2(db, "fts5", &fts5Mod, p, __fts5_module_destroy);
    if( rc==DBSQL_OK ) rc = __fts5_index_init(db);
    if( rc==DBSQL_OK ) rc = __fts5_expr_init(__p_global, db);
    if( rc==DBSQL_OK ) rc = __fts5_aux_init(&__p_global->api);
    if( rc==DBSQL_OK ) rc = __fts5_tokenizer_init(&__p_global->api);
    if( rc==DBSQL_OK ) rc = __fts5_vocab_init(__p_global, db);
    if( rc==DBSQL_OK ){
      rc = dbsql_create_function(
          db, "fts5", 1, DBSQL_UTF8, p, fts5Fts5Func, 0, 0
      );
    }
    if( rc==DBSQL_OK ){
      rc = dbsql_create_function(
          db, "fts5_source_id", 0, 
          DBSQL_UTF8|DBSQL_DETERMINISTIC|DBSQL_INNOCUOUS,
          p, __fts5_source_id_func, 0, 0
      );
    }
    if( rc==DBSQL_OK ){
      rc = dbsql_create_function(
          db, "fts5_locale", 2, 
          DBSQL_UTF8|DBSQL_INNOCUOUS|DBSQL_RESULT_SUBTYPE|DBSQL_SUBTYPE,
          p, __fts5_locale_func, 0, 0
      );
    }
    if( rc==DBSQL_OK ){
      rc = dbsql_create_function(
          db, "fts5_insttoken", 1, 
          DBSQL_UTF8|DBSQL_INNOCUOUS|DBSQL_RESULT_SUBTYPE,
          p, __fts5_insttoken_func, 0, 0
      );
    }
  }

  /* If DBSQL_FTS5_ENABLE_TEST_MI is defined, assume that the file
  ** fts5_test_mi.c is compiled and linked into the executable. And call
  ** its entry point to enable the matchinfo() demo.  */
#ifdef DBSQL_FTS5_ENABLE_TEST_MI
  if( rc==DBSQL_OK ){
    extern int __fts5_test_register_matchinfo_api(fts5_api*);
    rc = __fts5_test_register_matchinfo_api(&__p_global->api);
  }
#endif

  return rc;
}

/*
** The following functions are used to register the module with dbsql. If
** this module is being built as part of the dbsql core (DBSQL_CORE is
** defined), then dbsql_open() will call __fts5_init() directly.
**
** Or, if this module is being built as a loadable extension, 
** __fts5_init() is omitted and the two standard entry points
** dbsql_fts_init() and dbsql_fts5_init() defined instead.
*/
#ifndef DBSQL_CORE
#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_fts_init(
  DBSQL *db,
  char **errmsg_p,
  const dbsql_api_routines_t *api
){
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Unused parameter */
  return fts5Init(db);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_fts5_init(
  DBSQL *db,
  char **errmsg_p,
  const dbsql_api_routines_t *api
){
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Unused parameter */
  return fts5Init(db);
}
#else
int __fts5_init(DBSQL *db){
  return fts5Init(db);
}
#endif
