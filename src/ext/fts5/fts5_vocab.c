/*
** 2015 May 08
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
** This is an dbsql virtual table module implementing direct access to an
** existing FTS5 index. The module may create several different types of 
** tables:
**
** col:
**     CREATE TABLE vocab(term, col, doc, cnt, PRIMARY KEY(term, col));
**
**   One row for each term/column combination. The value of $doc is set to
**   the number of fts5 rows that contain at least one instance of term
**   $term within column $col. Field $cnt is set to the total number of 
**   instances of term $term in column $col (in any row of the fts5 table). 
**
** row:
**     CREATE TABLE vocab(term, doc, cnt, PRIMARY KEY(term));
**
**   One row for each term in the database. The value of $doc is set to
**   the number of fts5 rows that contain at least one instance of term
**   $term. Field $cnt is set to the total number of instances of term 
**   $term in the database.
**
** instance:
**     CREATE TABLE vocab(term, doc, col, offset, PRIMARY KEY(<all-fields>));
**
**   One row for each term instance in the database. 
*/


#include "fts5_int.h"


typedef struct fts5_vocab_table_t fts5_vocab_table_t;
typedef struct fts5_vocab_cursor_t fts5_vocab_cursor_t;

struct fts5_vocab_table_t {
  dbsql_vtab_t base;
  char *fts5_tbl;                 /* Name of fts5 table */
  char *fts5_db;                  /* db_t containing fts5 table */
  DBSQL *db;                    /* Database handle */
  fts5_global_t *__p_global;            /* FTS5 global object for this database */
  int type;                      /* FTS5_VOCAB_COL, ROW or INSTANCE */
  unsigned busy;                 /* True if busy */
};

struct fts5_vocab_cursor_t {
  dbsql_vtab_cursor_t base;
  dbsql_stmt_t *stmt;            /* Statement holding lock on p_index */
  fts5_table_t *fts5;               /* Associated FTS5 table */

  int eof;                       /* True if this cursor is at EOF */
  fts5_index_iter_t *iter;           /* Term/rowid iterator object */
  void *p_struct;                  /* From __fts5_structure_ref() */

  int n_le_term;                    /* Size of le_term in bytes */
  char *le_term;                  /* (term <= $le_term) paramater, or NULL */
  int col_used;                    /* Copy of dbsql_index_info_t.col_used */

  /* These are used by 'col' tables only */
  int i_col;
  int64_t *cnts;
  int64_t *docs;

  /* Output values used by all tables. */
  int64_t rowid;                      /* This table's current rowid value */
  fts5_buffer_t term;                /* Current value of 'term' column */

  /* Output values Used by 'instance' tables only */
  int64_t i_inst_pos;
  int i_inst_off;
};

#define FTS5_VOCAB_COL      0
#define FTS5_VOCAB_ROW      1
#define FTS5_VOCAB_INSTANCE 2

#define FTS5_VOCAB_COL_SCHEMA  "term, col, doc, cnt"
#define FTS5_VOCAB_ROW_SCHEMA  "term, doc, cnt"
#define FTS5_VOCAB_INST_SCHEMA "term, doc, col, offset"

/*
** Bits for the mask used as the idx_num value by best_index/filter.
*/
#define FTS5_VOCAB_TERM_EQ 0x0100
#define FTS5_VOCAB_TERM_GE 0x0200
#define FTS5_VOCAB_TERM_LE 0x0400

#define FTS5_VOCAB_COLUSED_MASK 0xFF


/*
** Translate a string containing an fts5vocab table type to an 
** FTS5_VOCAB_XXX constant. If successful, set *pe_type to the output
** value and return DBSQL_OK. Otherwise, set *err_p to an error message
** and return DBSQL_ERROR.
*/
static int __fts5_vocab_table_type(const char *type, char **err_p, int *pe_type){
  int rc = DBSQL_OK;
  char *copy = __fts5_strndup(&rc, type, -1);
  if( rc==DBSQL_OK ){
    __fts5_dequote(copy);
    if( dbsql_stricmp(copy, "col")==0 ){
      *pe_type = FTS5_VOCAB_COL;
    }else

    if( dbsql_stricmp(copy, "row")==0 ){
      *pe_type = FTS5_VOCAB_ROW;
    }else
    if( dbsql_stricmp(copy, "instance")==0 ){
      *pe_type = FTS5_VOCAB_INSTANCE;
    }else
    {
      *err_p = dbsql_mprintf("fts5vocab: unknown table type: %Q", copy);
      rc = DBSQL_ERROR;
    }
    dbsql_free(copy);
  }

  return rc;
}


/*
** The disconnect() virtual table method.
*/
static int __fts5_vocab_disconnect_method(dbsql_vtab_t *vtab){
  fts5_vocab_table_t *tab = (fts5_vocab_table_t*)vtab;
  dbsql_free(tab);
  return DBSQL_OK;
}

/*
** The destroy() virtual table method.
*/
static int __fts5_vocab_destroy_method(dbsql_vtab_t *vtab){
  fts5_vocab_table_t *tab = (fts5_vocab_table_t*)vtab;
  dbsql_free(tab);
  return DBSQL_OK;
}

/*
** This function is the implementation of both the connect and create
** methods of the FTS3 virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fts5vocab")
**   argv[1]   -> database name
**   argv[2]   -> table name
**
** then:
**
**   argv[3]   -> name of fts5 table
**   argv[4]   -> type of fts5vocab table
**
** or, for tables in the TEMP schema only.
**
**   argv[3]   -> name of fts5 tables database
**   argv[4]   -> name of fts5 table
**   argv[5]   -> type of fts5vocab table
*/
static int __fts5_vocab_init_vtab(
  DBSQL *db,                    /* The dbsql database connection */
  void *aux,                     /* Pointer to fts5_global_t object */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* create/connect argument array */
  dbsql_vtab_t **v_tab,          /* Write the resulting vtab structure here */
  char **err_p                    /* Write any error message here */
){
  const char *schemas[] = { 
    "CREATE TABlE vocab(" FTS5_VOCAB_COL_SCHEMA  ")", 
    "CREATE TABlE vocab(" FTS5_VOCAB_ROW_SCHEMA  ")",
    "CREATE TABlE vocab(" FTS5_VOCAB_INST_SCHEMA ")"
  };

  fts5_vocab_table_t *ret = 0;
  int rc = DBSQL_OK;             /* Return code */
  int b_db;

  b_db = (argc==6 && strlen(argv[1])==4 && memcmp("temp", argv[1], 4)==0);

  if( argc!=5 && b_db==0 ){
    *err_p = dbsql_mprintf("wrong number of vtable arguments");
    rc = DBSQL_ERROR;
  }else{
    int64_t n_byte;                      /* Bytes of space to allocate */
    const char *z_db = b_db ? argv[3] : argv[1];
    const char *tab = b_db ? argv[4] : argv[3];
    const char *z_type = b_db ? argv[5] : argv[4];
    int64_t n_db = strlen(z_db)+1; 
    int64_t n_tab = strlen(tab)+1;
    int type = 0;
    
    rc = __fts5_vocab_table_type(z_type, err_p, &type);
    if( rc==DBSQL_OK ){
      assert( type>=0 && type<ARRAY_SIZE(schemas) );
      rc = dbsql_declare_vtab(db, schemas[type]);
    }

    n_byte = sizeof(fts5_vocab_table_t) + n_db + n_tab;
    ret = __fts5_malloc_zero(&rc, n_byte);
    if( ret ){
      ret->__p_global = (fts5_global_t*)aux;
      ret->type = type;
      ret->db = db;
      ret->fts5_tbl = (char*)&ret[1];
      ret->fts5_db = &ret->fts5_tbl[n_tab];
      memcpy(ret->fts5_tbl, tab, n_tab);
      memcpy(ret->fts5_db, z_db, n_db);
      __fts5_dequote(ret->fts5_tbl);
      __fts5_dequote(ret->fts5_db);
    }
  }

  *v_tab = (dbsql_vtab_t*)ret;
  return rc;
}


/*
** The connect() and create() methods for the virtual table. All the
** work is done in function __fts5_vocab_init_vtab().
*/
static int __fts5_vocab_connect_method(
  DBSQL *db,                    /* Database connection */
  void *aux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* create/connect argument array */
  dbsql_vtab_t **vtab,          /* OUT: New dbsql_vtab_t object */
  char **err_p                    /* OUT: dbsql_malloc'd error message */
){
  return __fts5_vocab_init_vtab(db, aux, argc, argv, vtab, err_p);
}
static int __fts5_vocab_create_method(
  DBSQL *db,                    /* Database connection */
  void *aux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* create/connect argument array */
  dbsql_vtab_t **vtab,          /* OUT: New dbsql_vtab_t object */
  char **err_p                    /* OUT: dbsql_malloc'd error message */
){
  return __fts5_vocab_init_vtab(db, aux, argc, argv, vtab, err_p);
}

/* 
** Implementation of the best_index method.
**
** Only constraints of the form:
**
**     term <= ?
**     term == ?
**     term >= ?
**
** are interpreted. Less-than and less-than-or-equal are treated 
** identically, as are greater-than and greater-than-or-equal.
*/
static int __fts5_vocab_best_index_method(
  dbsql_vtab_t *unused,
  dbsql_index_info_t *info
){
  int i;
  int i_term_eq = -1;
  int i_term_ge = -1;
  int i_term_le = -1;
  int idx_num = (int)info->col_used;
  int n_arg = 0;

  UNUSED_PARAM(unused);

  assert( (info->col_used & FTS5_VOCAB_COLUSED_MASK)==info->col_used );

  for(i=0; i<info->n_constraint; i++){
    struct dbsql_index_constraint_t *p = &info->constraints[i];
    if( p->usable==0 ) continue;
    if( p->i_column==0 ){          /* term column */
      if( p->op==DBSQL_INDEX_CONSTRAINT_EQ ) i_term_eq = i;
      if( p->op==DBSQL_INDEX_CONSTRAINT_LE ) i_term_le = i;
      if( p->op==DBSQL_INDEX_CONSTRAINT_LT ) i_term_le = i;
      if( p->op==DBSQL_INDEX_CONSTRAINT_GE ) i_term_ge = i;
      if( p->op==DBSQL_INDEX_CONSTRAINT_GT ) i_term_ge = i;
    }
  }

  if( i_term_eq>=0 ){
    idx_num |= FTS5_VOCAB_TERM_EQ;
    info->constraint_usages[i_term_eq].argv_index = ++n_arg;
    info->estimated_cost = 100;
  }else{
    info->estimated_cost = 1000000;
    if( i_term_ge>=0 ){
      idx_num |= FTS5_VOCAB_TERM_GE;
      info->constraint_usages[i_term_ge].argv_index = ++n_arg;
      info->estimated_cost = info->estimated_cost / 2;
    }
    if( i_term_le>=0 ){
      idx_num |= FTS5_VOCAB_TERM_LE;
      info->constraint_usages[i_term_le].argv_index = ++n_arg;
      info->estimated_cost = info->estimated_cost / 2;
    }
  }

  /* This virtual table always delivers results in ascending order of
  ** the "term" column (column 0). So if the user has requested this
  ** specifically - "ORDER BY term" or "ORDER BY term ASC" - set the
  ** dbsql_index_info_t.order_by_consumed flag to tell the core the results
  ** are already in sorted order.  */
  if( info->n_order_by==1 
   && info->order_bies[0].i_column==0 
   && info->order_bies[0].desc==0
  ){
    info->order_by_consumed = 1;
  }

  info->idx_num = idx_num;
  return DBSQL_OK;
}

/*
** Implementation of x_open method.
*/
static int __fts5_vocab_open_method(
  dbsql_vtab_t *v_tab, 
  dbsql_vtab_cursor_t **pp_csr
){
  fts5_vocab_table_t *tab = (fts5_vocab_table_t*)v_tab;
  fts5_table_t *fts5 = 0;
  fts5_vocab_cursor_t *csr = 0;
  int rc = DBSQL_OK;
  dbsql_stmt_t *stmt = 0;
  char *sql = 0;

  if( tab->busy ){
    v_tab->errmsg = dbsql_mprintf(
       "recursive definition for %s.%s", tab->fts5_db, tab->fts5_tbl
    );
    return DBSQL_ERROR;
  }
  sql = __fts5_mprintf(&rc,
      "SELECT t.%Q FROM %Q.%Q AS t WHERE t.%Q MATCH '*id'",
      tab->fts5_tbl, tab->fts5_db, tab->fts5_tbl, tab->fts5_tbl
  );
  if( sql ){
    rc = dbsql_prepare_v2(tab->db, sql, -1, &stmt, 0);
  }
  dbsql_free(sql);
  assert( rc==DBSQL_OK || stmt==0 );
  if( rc==DBSQL_ERROR ) rc = DBSQL_OK;

  tab->busy = 1;
  if( stmt && dbsql_step(stmt)==DBSQL_ROW ){
    int64_t i_id = dbsql_column_int64(stmt, 0);
    fts5 = __fts5_table_from_csrid(tab->__p_global, i_id);
  }
  tab->busy = 0;

  if( rc==DBSQL_OK ){
    if( fts5==0 ){
      rc = dbsql_finalize(stmt);
      stmt = 0;
      if( rc==DBSQL_OK ){
        v_tab->errmsg = dbsql_mprintf(
            "no such fts5 table: %s.%s", tab->fts5_db, tab->fts5_tbl
        );
        rc = DBSQL_ERROR;
      }
    }else{
      rc = __fts5_flush_to_disk(fts5);
    }
  }

  if( rc==DBSQL_OK ){
    int64_t n_byte = fts5->config->n_col * sizeof(int64_t)*2 + sizeof(fts5_vocab_cursor_t);
    csr = (fts5_vocab_cursor_t*)__fts5_malloc_zero(&rc, n_byte);
  }

  if( csr ){
    csr->fts5 = fts5;
    csr->stmt = stmt;
    csr->cnts = (int64_t*)&csr[1];
    csr->docs = &csr->cnts[fts5->config->n_col];
  }else{
    dbsql_finalize(stmt);
  }

  *pp_csr = (dbsql_vtab_cursor_t*)csr;
  return rc;
}

/*
** Restore cursor csr to the state it was in immediately after being
** created by the x_open() method.
*/
static void __fts5_vocab_reset_cursor(fts5_vocab_cursor_t *csr){
  int n_col = csr->fts5->config->n_col;
  csr->rowid = 0;
  __fts5_iter_close(csr->iter);
  __fts5_structure_release(csr->p_struct);
  csr->p_struct = 0;
  csr->iter = 0;
  dbsql_free(csr->le_term);
  csr->n_le_term = -1;
  csr->le_term = 0;
  csr->eof = 0;
  csr->i_col = 0;
  csr->i_inst_pos = 0;
  csr->i_inst_off = 0;
  csr->col_used = 0;
  memset(csr->cnts, 0, sizeof(int64_t)*n_col);
  memset(csr->docs, 0, sizeof(int64_t)*n_col);
}

/*
** Close the cursor.  For additional information see the documentation
** on the x_close method of the virtual table interface.
*/
static int __fts5_vocab_close_method(dbsql_vtab_cursor_t *cursor){
  fts5_vocab_cursor_t *csr = (fts5_vocab_cursor_t*)cursor;
  __fts5_vocab_reset_cursor(csr);
  __fts5_buffer_free(&csr->term);
  dbsql_finalize(csr->stmt);
  dbsql_free(csr);
  return DBSQL_OK;
}

static int __fts5_vocab_instance_new_term(fts5_vocab_cursor_t *csr){
  int rc = DBSQL_OK;
  
  if( __fts5_iter_eof(csr->iter) ){
    csr->eof = 1;
  }else{
    const char *term;
    int n_term;
    term = __fts5_iter_term(csr->iter, &n_term);
    if( csr->n_le_term>=0 ){
      int n_cmp = MIN(n_term, csr->n_le_term);
      int cmp = memcmp(csr->le_term, term, n_cmp);
      if( cmp<0 || (cmp==0 && csr->n_le_term<n_term) ){
        csr->eof = 1;
      }
    }

    __fts5_buffer_set(&rc, &csr->term, n_term, (const u_int8_t*)term);
  }
  return rc;
}

static int __fts5_vocab_instance_next(fts5_vocab_cursor_t *csr){
  int detail = csr->fts5->config->detail;
  int rc = DBSQL_OK;
  fts5_index_iter_t *iter = csr->iter;
  int64_t *pp = &csr->i_inst_pos;
  int *po = &csr->i_inst_off;
  
  assert( __fts5_iter_eof(iter)==0 );
  assert( csr->eof==0 );
  while( detail==FTS5_DETAIL_NONE
      || __fts5_poslist_next64(iter->data, iter->n_data, po, pp) 
  ){
    csr->i_inst_pos = 0;
    csr->i_inst_off = 0;

    rc = __fts5_iter_next_scan(csr->iter);
    if( rc==DBSQL_OK ){
      rc = __fts5_vocab_instance_new_term(csr);
      if( csr->eof || detail==FTS5_DETAIL_NONE ) break;
    }
    if( rc ){
      csr->eof = 1;
      break;
    }
  }

  return rc;
}

/*
** Advance the cursor to the next row in the table.
*/
static int __fts5_vocab_next_method(dbsql_vtab_cursor_t *cursor){
  fts5_vocab_cursor_t *csr = (fts5_vocab_cursor_t*)cursor;
  fts5_vocab_table_t *tab = (fts5_vocab_table_t*)cursor->vtab;
  int n_col = csr->fts5->config->n_col;
  int rc;

  rc = __fts5_structure_test(csr->fts5->p_index, csr->p_struct);
  if( rc!=DBSQL_OK ) return rc;
  csr->rowid++;

  if( tab->type==FTS5_VOCAB_INSTANCE ){
    return __fts5_vocab_instance_next(csr);
  }

  if( tab->type==FTS5_VOCAB_COL ){
    for(csr->i_col++; csr->i_col<n_col; csr->i_col++){
      if( csr->docs[csr->i_col] ) break;
    }
  }

  if( tab->type!=FTS5_VOCAB_COL || csr->i_col>=n_col ){
    if( __fts5_iter_eof(csr->iter) ){
      csr->eof = 1;
    }else{
      const char *term;
      int n_term;

      term = __fts5_iter_term(csr->iter, &n_term);
      assert( n_term>=0 );
      if( csr->n_le_term>=0 ){
        int n_cmp = MIN(n_term, csr->n_le_term);
        int cmp = memcmp(csr->le_term, term, n_cmp);
        if( cmp<0 || (cmp==0 && csr->n_le_term<n_term) ){
          csr->eof = 1;
          return DBSQL_OK;
        }
      }

      __fts5_buffer_set(&rc, &csr->term, n_term, (const u_int8_t*)term);
      memset(csr->cnts, 0, n_col * sizeof(int64_t));
      memset(csr->docs, 0, n_col * sizeof(int64_t));
      csr->i_col = 0;

      assert( tab->type==FTS5_VOCAB_COL || tab->type==FTS5_VOCAB_ROW );
      while( rc==DBSQL_OK ){
        int detail = csr->fts5->config->detail;
        const u_int8_t *pos; int n_pos;   /* Position list */
        int64_t i_pos = 0;               /* 64-bit position read from poslist */
        int i_off = 0;               /* Current offset within position list */

        pos = csr->iter->data;
        n_pos = csr->iter->n_data;

        switch( tab->type ){
          case FTS5_VOCAB_ROW:
            /* Do not bother counting the number of instances if the "cnt"
            ** column is not being read (according to col_used).  */
            if( detail==FTS5_DETAIL_FULL && (csr->col_used & 0x04) ){
              while( i_pos<n_pos ){
                u_int32_t ii;
                FTS5_FAST_GET_VARINT32(pos, i_pos, ii);
                if( ii==1 ){ 
                  /* New column in the position list */
                  FTS5_FAST_GET_VARINT32(pos, i_pos, ii);
                }else{
                  /* An instance - increment csr->cnts[] */
                  csr->cnts[0]++;
                }
              }
            }
            csr->docs[0]++;
            break;

          case FTS5_VOCAB_COL:
            if( detail==FTS5_DETAIL_FULL ){
              int i_col = -1;
              while( 0==__fts5_poslist_next64(pos, n_pos, &i_off, &i_pos) ){
                int ii = FTS5_POS2COLUMN(i_pos);
                if( i_col!=ii ){
                  if( ii>=n_col ){
                    rc = FTS5_CORRUPT;
                    break;
                  }
                  csr->docs[ii]++;
                  i_col = ii;
                }
                csr->cnts[ii]++;
              }
            }else if( detail==FTS5_DETAIL_COLUMNS ){
              while( 0==__fts5_poslist_next64(pos, n_pos, &i_off,&i_pos) ){
                assert_nc( i_pos>=0 && i_pos<n_col );
                if( i_pos>=n_col ){
                  rc = FTS5_CORRUPT;
                  break;
                }
                csr->docs[i_pos]++;
              }
            }else{
              assert( detail==FTS5_DETAIL_NONE );
              csr->docs[0]++;
            }
            break;

          default:
            assert( tab->type==FTS5_VOCAB_INSTANCE );
            break;
        }

        if( rc==DBSQL_OK ){
          rc = __fts5_iter_next_scan(csr->iter);
        }
        if( tab->type==FTS5_VOCAB_INSTANCE ) break;

        if( rc==DBSQL_OK ){
          term = __fts5_iter_term(csr->iter, &n_term);
          if( n_term!=csr->term.n 
          || (n_term>0 && memcmp(term, csr->term.p, n_term)) 
          ){
            break;
          }
          if( __fts5_iter_eof(csr->iter) ) break;
        }
      }
    }
  }

  if( rc==DBSQL_OK && csr->eof==0 && tab->type==FTS5_VOCAB_COL ){
    for(/* noop */; csr->i_col<n_col && csr->docs[csr->i_col]==0; csr->i_col++);
    if( csr->i_col==n_col ){
      rc = FTS5_CORRUPT;
    }
  }
  return rc;
}

/*
** This is the filter implementation for the virtual table.
*/
static int __fts5_vocab_filter_method(
  dbsql_vtab_cursor_t *cursor,   /* The cursor used for this query */
  int idx_num,                     /* Strategy index */
  const char *unused,            /* Unused */
  int n_unused,                    /* Number of elements in vals */
  dbsql_value_t **vals           /* Arguments for the indexing scheme */
){
  fts5_vocab_table_t *tab = (fts5_vocab_table_t*)cursor->vtab;
  fts5_vocab_cursor_t *csr = (fts5_vocab_cursor_t*)cursor;
  int type = tab->type;
  int rc = DBSQL_OK;

  int i_val = 0;
  int f = FTS5INDEX_QUERY_SCAN;
  const char *term = 0;
  int n_term = 0;

  dbsql_value_t *eq = 0;
  dbsql_value_t *ge = 0;
  dbsql_value_t *le = 0;

  UNUSED_PARAM2(unused, n_unused);

  __fts5_vocab_reset_cursor(csr);
  if( idx_num & FTS5_VOCAB_TERM_EQ ) eq = vals[i_val++];
  if( idx_num & FTS5_VOCAB_TERM_GE ) ge = vals[i_val++];
  if( idx_num & FTS5_VOCAB_TERM_LE ) le = vals[i_val++];
  csr->col_used = (idx_num & FTS5_VOCAB_COLUSED_MASK);

  if( eq ){
    term = (const char *)dbsql_value_text(eq);
    n_term = dbsql_value_bytes(eq);
    f = FTS5INDEX_QUERY_NOTOKENDATA;
  }else{
    if( ge ){
      term = (const char *)dbsql_value_text(ge);
      n_term = dbsql_value_bytes(ge);
    }
    if( le ){
      const char *copy = (const char *)dbsql_value_text(le);
      if( copy==0 ) copy = "";
      csr->n_le_term = dbsql_value_bytes(le);
      csr->le_term = dbsql_malloc64((int64_t)csr->n_le_term+1);
      if( csr->le_term==0 ){
        rc = DBSQL_NOMEM;
      }else{
        memcpy(csr->le_term, copy, csr->n_le_term+1);
      }
    }
  }

  if( rc==DBSQL_OK ){
    fts5_index_t *p_index = csr->fts5->p_index;
    rc = __fts5_index_query(p_index, term, n_term, f, 0, &csr->iter);
    if( rc==DBSQL_OK ){
      csr->p_struct = __fts5_structure_ref(p_index);
    }
  }
  if( rc==DBSQL_OK && type==FTS5_VOCAB_INSTANCE ){
    rc = __fts5_vocab_instance_new_term(csr);
  }
  if( rc==DBSQL_OK && !csr->eof 
   && (type!=FTS5_VOCAB_INSTANCE 
    || csr->fts5->config->detail!=FTS5_DETAIL_NONE)
  ){
    rc = __fts5_vocab_next_method(cursor);
  }

  return rc;
}

/* 
** This is the eof method of the virtual table. dbsql calls this 
** routine to find out if it has reached the end of a result set.
*/
static int __fts5_vocab_eof_method(dbsql_vtab_cursor_t *cursor){
  fts5_vocab_cursor_t *csr = (fts5_vocab_cursor_t*)cursor;
  return csr->eof;
}

static int __fts5_vocab_column_method(
  dbsql_vtab_cursor_t *cursor,   /* Cursor to retrieve value from */
  dbsql_func_t *ctx,          /* Context for dbsql_result_xxx() calls */
  int i_col                        /* index_t of column to read value from */
){
  fts5_vocab_cursor_t *csr = (fts5_vocab_cursor_t*)cursor;
  int detail = csr->fts5->config->detail;
  int type = ((fts5_vocab_table_t*)(cursor->vtab))->type;
  int64_t i_val = 0;

  if( i_col==0 ){
    dbsql_result_text(
        ctx, (const char*)csr->term.p, csr->term.n, DBSQL_TRANSIENT
    );
  }else if( type==FTS5_VOCAB_COL ){
    assert( i_col==1 || i_col==2 || i_col==3 );
    if( i_col==1 ){
      if( detail!=FTS5_DETAIL_NONE ){
        const char *z = csr->fts5->config->cols[csr->i_col];
        dbsql_result_text(ctx, z, -1, DBSQL_STATIC);
      }
    }else if( i_col==2 ){
      i_val = csr->docs[csr->i_col];
    }else{
      i_val = csr->cnts[csr->i_col];
    }
  }else if( type==FTS5_VOCAB_ROW ){
    assert( i_col==1 || i_col==2 );
    if( i_col==1 ){
      i_val = csr->docs[0];
    }else{
      i_val = csr->cnts[0];
    }
  }else{
    assert( type==FTS5_VOCAB_INSTANCE );
    switch( i_col ){
      case 1:
        dbsql_result_int64(ctx, csr->iter->i_rowid);
        break;
      case 2: {
        int ii = -1;
        if( detail==FTS5_DETAIL_FULL ){
          ii = FTS5_POS2COLUMN(csr->i_inst_pos);
        }else if( detail==FTS5_DETAIL_COLUMNS ){
          ii = (int)csr->i_inst_pos;
        }
        if( ii>=0 && ii<csr->fts5->config->n_col ){
          const char *z = csr->fts5->config->cols[ii];
          dbsql_result_text(ctx, z, -1, DBSQL_STATIC);
        }
        break;
      }
      default: {
        assert( i_col==3 );
        if( detail==FTS5_DETAIL_FULL ){
          int ii = FTS5_POS2OFFSET(csr->i_inst_pos);
          dbsql_result_int(ctx, ii);
        }
        break;
      }
    }
  }

  if( i_val>0 ) dbsql_result_int64(ctx, i_val);
  return DBSQL_OK;
}

/* 
** This is the rowid method. The dbsql core calls this routine to
** retrieve the rowid for the current row of the result set. The
** rowid should be written to *rowid.
*/
static int __fts5_vocab_rowid_method(
  dbsql_vtab_cursor_t *cursor, 
  int64_t *rowid
){
  fts5_vocab_cursor_t *csr = (fts5_vocab_cursor_t*)cursor;
  *rowid = csr->rowid;
  return DBSQL_OK;
}

int __fts5_vocab_init(fts5_global_t *__p_global, DBSQL *db){
  static const dbsql_module_t fts5Vocab = {
    /* i_version      */ 2,
    /* create       */ __fts5_vocab_create_method,
    /* connect      */ __fts5_vocab_connect_method,
    /* best_index    */ __fts5_vocab_best_index_method,
    /* disconnect   */ __fts5_vocab_disconnect_method,
    /* destroy      */ __fts5_vocab_destroy_method,
    /* x_open         */ __fts5_vocab_open_method,
    /* x_close        */ __fts5_vocab_close_method,
    /* filter       */ __fts5_vocab_filter_method,
    /* next         */ __fts5_vocab_next_method,
    /* eof          */ __fts5_vocab_eof_method,
    /* column       */ __fts5_vocab_column_method,
    /* rowid        */ __fts5_vocab_rowid_method,
    /* update       */ 0,
    /* begin        */ 0,
    /* x_sync         */ 0,
    /* commit       */ 0,
    /* rollback     */ 0,
    /* find_function */ 0,
    /* rename       */ 0,
    /* savepoint    */ 0,
    /* release      */ 0,
    /* rollback_to   */ 0,
    /* shadow_name   */ 0,
    /* integrity    */ 0
  };
  void *p = (void*)__p_global;

  return dbsql_create_module_v2(db, "fts5vocab", &fts5Vocab, p, 0);
}
