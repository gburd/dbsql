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
**
*/



#include "fts5_int.h"

/*
** saved_row:
**   SQL statement FTS5_STMT_LOOKUP2 is a copy of FTS5_STMT_LOOKUP, it 
**   does a by-rowid lookup to retrieve a single row from the %_content 
**   table or equivalent external-content table/view.
**
**   However, FTS5_STMT_LOOKUP2 is only used when retrieving the original
**   values for a row being UPDATEd. In that case, the SQL statement is
**   not reset and saved_row is set to point at it. This is so that the
**   insert operation that follows the delete may access the original 
**   row values for any new values for which dbsql_value_nochange() returns
**   true. i.e. if the user executes:
**
**        CREATE VIRTUAL TABLE ft USING fts5(a, b, c, locale=1);
**        ...
**        UPDATE fts SET a=?, b=? WHERE rowid=?;
**
**   then the value passed to the update() method of this table as the 
**   new.c value is an dbsql_value_nochange() value. So in this case it
**   must be read from the saved row stored in fts5_storage_t.saved_row.
**
**   This is necessary - using dbsql_value_nochange() instead of just having
**   dbsql pass the original value back via update() - so as not to discard
**   any locale information associated with such values.
**
*/
struct fts5_storage_t {
  fts5_config_t *config;
  fts5_index_t *p_index;
  int totals_valid;               /* True if n_total_row/total_sizes[] are valid */
  int64_t n_total_row;                  /* Total number of rows in FTS table */
  int64_t *total_sizes;                /* Total sizes of each column */ 
  dbsql_stmt_t *saved_row;
  dbsql_stmt_t *stmts[12];
};


#if FTS5_STMT_SCAN_ASC!=0 
# error "FTS5_STMT_SCAN_ASC mismatch" 
#endif
#if FTS5_STMT_SCAN_DESC!=1 
# error "FTS5_STMT_SCAN_DESC mismatch" 
#endif
#if FTS5_STMT_LOOKUP!=2
# error "FTS5_STMT_LOOKUP mismatch" 
#endif

#define FTS5_STMT_LOOKUP2         3
#define FTS5_STMT_INSERT_CONTENT  4
#define FTS5_STMT_REPLACE_CONTENT 5
#define FTS5_STMT_DELETE_CONTENT  6
#define FTS5_STMT_REPLACE_DOCSIZE 7
#define FTS5_STMT_DELETE_DOCSIZE  8
#define FTS5_STMT_LOOKUP_DOCSIZE  9
#define FTS5_STMT_REPLACE_CONFIG 10
#define FTS5_STMT_SCAN           11

/*
** Prepare the two insert statements - fts5_storage_t.insert_content and
** fts5_storage_t.insert_docsize - if they have not already been prepared.
** Return DBSQL_OK if successful, or an dbsql error code if an error
** occurs.
*/
static int __fts5_storage_get_stmt(
  fts5_storage_t *p,                 /* Storage handle */
  int stmt,                      /* FTS5_STMT_XXX constant */
  dbsql_stmt_t **pp_stmt,          /* OUT: Prepared statement handle */
  char **errmsg_p                 /* OUT: Error message (if any) */
){
  int rc = DBSQL_OK;

  /* If there is no %_docsize table, there should be no requests for 
  ** statements to operate on it.  */
  assert( p->config->columnsize || (
        stmt!=FTS5_STMT_REPLACE_DOCSIZE 
     && stmt!=FTS5_STMT_DELETE_DOCSIZE 
     && stmt!=FTS5_STMT_LOOKUP_DOCSIZE 
  ));

  assert( stmt>=0 && stmt<ARRAY_SIZE(p->stmts) );
  if( p->stmts[stmt]==0 ){
    const char *stmts[] = {
      "SELECT %s FROM %s T WHERE T.%Q >= ? AND T.%Q <= ? ORDER BY T.%Q ASC",
      "SELECT %s FROM %s T WHERE T.%Q <= ? AND T.%Q >= ? ORDER BY T.%Q DESC",
      "SELECT %s FROM %s T WHERE T.%Q=?",               /* LOOKUP  */
      "SELECT %s FROM %s T WHERE T.%Q=?",               /* LOOKUP2  */

      "INSERT INTO %Q.'%q_content' VALUES(%s)",         /* INSERT_CONTENT  */
      "REPLACE INTO %Q.'%q_content' VALUES(%s)",        /* REPLACE_CONTENT */
      "DELETE FROM %Q.'%q_content' WHERE id=?",         /* DELETE_CONTENT  */
      "REPLACE INTO %Q.'%q_docsize' VALUES(?,?%s)",     /* REPLACE_DOCSIZE  */
      "DELETE FROM %Q.'%q_docsize' WHERE id=?",         /* DELETE_DOCSIZE  */

      "SELECT sz%s FROM %Q.'%q_docsize' WHERE id=?",    /* LOOKUP_DOCSIZE  */

      "REPLACE INTO %Q.'%q_config' VALUES(?,?)",        /* REPLACE_CONFIG */
      "SELECT %s FROM %s AS T",                         /* SCAN */
    };
    fts5_config_t *c = p->config;
    char *sql = 0;

    assert( ARRAY_SIZE(stmts)==ARRAY_SIZE(p->stmts) );

    switch( stmt ){
      case FTS5_STMT_SCAN:
        sql = dbsql_mprintf(stmts[stmt], 
            c->content_exprlist, c->z_content
        );
        break;

      case FTS5_STMT_SCAN_ASC:
      case FTS5_STMT_SCAN_DESC:
        sql = dbsql_mprintf(stmts[stmt], c->content_exprlist, 
            c->z_content, c->content_rowid, c->content_rowid,
            c->content_rowid
        );
        break;

      case FTS5_STMT_LOOKUP:
      case FTS5_STMT_LOOKUP2:
        sql = dbsql_mprintf(stmts[stmt], 
            c->content_exprlist, c->z_content, c->content_rowid
        );
        break;

      case FTS5_STMT_INSERT_CONTENT: 
      case FTS5_STMT_REPLACE_CONTENT: {
        char *bind = 0;
        int i;

        assert( c->content==FTS5_CONTENT_NORMAL 
             || c->content==FTS5_CONTENT_UNINDEXED 
        );

        /* Add bindings for the "c*" columns - those that store the actual
        ** table content. If content==NORMAL, then there is one binding
        ** for each column. Or, if content==UNINDEXED, then there are only
        ** bindings for the UNINDEXED columns. */
        for(i=0; rc==DBSQL_OK && i<(c->n_col+1); i++){
          if( !i || c->content==FTS5_CONTENT_NORMAL || c->ab_unindexed[i-1] ){
            bind = __fts5_mprintf(&rc, "%z%s?%d", bind, bind?",":"",i+1);
          }
        }

        /* Add bindings for any "l*" columns. Only non-UNINDEXED columns
        ** require these.  */
        if( c->locale && c->content==FTS5_CONTENT_NORMAL ){
          for(i=0; rc==DBSQL_OK && i<c->n_col; i++){
            if( c->ab_unindexed[i]==0 ){
              bind = __fts5_mprintf(&rc, "%z,?%d", bind, c->n_col+i+2);
            }
          }
        }

        sql = __fts5_mprintf(&rc, stmts[stmt], c->z_db, c->name,bind);
        dbsql_free(bind);
        break;
      }

      case FTS5_STMT_REPLACE_DOCSIZE: 
        sql = dbsql_mprintf(stmts[stmt], c->z_db, c->name,
          (c->contentless_delete ? ",?" : "")
        );
        break;

      case FTS5_STMT_LOOKUP_DOCSIZE: 
        sql = dbsql_mprintf(stmts[stmt], 
            (c->contentless_delete ? ",origin" : ""),
            c->z_db, c->name
        );
        break;

      default:
        sql = dbsql_mprintf(stmts[stmt], c->z_db, c->name);
        break;
    }

    if( sql==0 ){
      rc = DBSQL_NOMEM;
    }else{
      int f = DBSQL_PREPARE_PERSISTENT;
      if( stmt>FTS5_STMT_LOOKUP2 ) f |= DBSQL_PREPARE_NO_VTAB;
      p->config->lock++;
      rc = dbsql_prepare_v3(c->db, sql, -1, f, &p->stmts[stmt], 0);
      p->config->lock--;
      dbsql_free(sql);
      if( rc!=DBSQL_OK && errmsg_p ){
        *errmsg_p = dbsql_mprintf("%s", dbsql_errmsg(c->db));
      }
      if( rc==DBSQL_ERROR && stmt>FTS5_STMT_LOOKUP2 && stmt<FTS5_STMT_SCAN ){
        /* One of the internal tables - not the %_content table - is missing.
        ** This counts as a corrupted table.  */
       rc = DBSQL_CORRUPT;
      }
    }
  }

  *pp_stmt = p->stmts[stmt];
  dbsql_reset(*pp_stmt);
  return rc;
}


static int __fts5_exec_printf(
  DBSQL *db,
  char **err_p,
  const char *format,
  ...
){
  int rc;
  va_list ap;                     /* ... printf arguments */
  char *sql;

  va_start(ap, format);
  sql = dbsql_vmprintf(format, ap);

  if( sql==0 ){
    rc = DBSQL_NOMEM;
  }else{
    rc = dbsql_exec(db, sql, 0, 0, err_p);
    dbsql_free(sql);
  }

  va_end(ap);
  return rc;
}

/*
** Drop all shadow tables. Return DBSQL_OK if successful or an dbsql error
** code otherwise.
*/
int __fts5_drop_all(fts5_config_t *config){
  int rc = __fts5_exec_printf(config->db, 0, 
      "DROP TABLE IF EXISTS %Q.'%q_data';"
      "DROP TABLE IF EXISTS %Q.'%q_idx';"
      "DROP TABLE IF EXISTS %Q.'%q_config';",
      config->z_db, config->name,
      config->z_db, config->name,
      config->z_db, config->name
  );
  if( rc==DBSQL_OK && config->columnsize ){
    rc = __fts5_exec_printf(config->db, 0, 
        "DROP TABLE IF EXISTS %Q.'%q_docsize';",
        config->z_db, config->name
    );
  }
  if( rc==DBSQL_OK && config->content==FTS5_CONTENT_NORMAL ){
    rc = __fts5_exec_printf(config->db, 0, 
        "DROP TABLE IF EXISTS %Q.'%q_content';",
        config->z_db, config->name
    );
  }
  return rc;
}

static void __fts5_storage_rename_one(
  fts5_config_t *config,            /* Current FTS5 configuration */
  int *rc,                       /* IN/OUT: Error code */
  const char *tail,              /* Tail of table name e.g. "data", "config" */
  const char *name               /* New name of FTS5 table */
){
  if( *rc==DBSQL_OK ){
    *rc = __fts5_exec_printf(config->db, 0, 
        "ALTER TABLE %Q.'%q_%s' RENAME TO '%q_%s';",
        config->z_db, config->name, tail, name, tail
    );
  }
}

int __fts5_storage_rename(fts5_storage_t *storage, const char *name){
  fts5_config_t *config = storage->config;
  int rc = __fts5_storage_sync(storage);

  __fts5_storage_rename_one(config, &rc, "data", name);
  __fts5_storage_rename_one(config, &rc, "idx", name);
  __fts5_storage_rename_one(config, &rc, "config", name);
  if( config->columnsize ){
    __fts5_storage_rename_one(config, &rc, "docsize", name);
  }
  if( config->content==FTS5_CONTENT_NORMAL ){
    __fts5_storage_rename_one(config, &rc, "content", name);
  }
  return rc;
}

/*
** Create the shadow table named post, with definition defn. Return
** DBSQL_OK if successful, or an dbsql error code otherwise.
*/
int __fts5_create_table(
  fts5_config_t *config,            /* FTS5 configuration */
  const char *post,              /* Shadow table to create (e.g. "content") */
  const char *defn,              /* Columns etc. for shadow table */
  int without,                   /* True for without rowid */
  char **err_p                    /* OUT: Error message */
){
  int rc;
  char *err = 0;

  rc = __fts5_exec_printf(config->db, &err, "CREATE TABLE %Q.'%q_%q'(%s)%s",
      config->z_db, config->name, post, defn, 
#ifndef DBSQL_FTS5_NO_WITHOUT_ROWID
      without?" WITHOUT ROWID":
#endif
      ""
  );
  if( err ){
    *err_p = dbsql_mprintf(
        "fts5: error creating shadow table %q_%s: %s", 
        config->name, post, err
    );
    dbsql_free(err);
  }

  return rc;
}

/*
** Open a new fts5_index_t handle. If the create argument is true, create
** and initialize the underlying tables 
**
** If successful, set *pp to point to the new object and return DBSQL_OK.
** Otherwise, set *pp to NULL and return an dbsql error code.
*/
int __fts5_storage_open(
  fts5_config_t *config, 
  fts5_index_t *p_index, 
  int create, 
  fts5_storage_t **pp,
  char **err_p                    /* OUT: Error message */
){
  int rc = DBSQL_OK;
  fts5_storage_t *p;                 /* New object */
  int64_t n_byte;            /* Bytes of space to allocate */

  n_byte = sizeof(fts5_storage_t)               /* fts5_storage_t object */
        + config->n_col * sizeof(int64_t);      /* fts5_storage_t.total_sizes[] */
  *pp = p = (fts5_storage_t*)dbsql_malloc64(n_byte);
  if( !p ) return DBSQL_NOMEM;

  memset(p, 0, (size_t)n_byte);
  p->total_sizes = (int64_t*)&p[1];
  p->config = config;
  p->p_index = p_index;

  if( create ){
    if( config->content==FTS5_CONTENT_NORMAL 
     || config->content==FTS5_CONTENT_UNINDEXED 
    ){
      int i = 0;
      char *z_defn = 0;
      dbsql_str_t *defn = dbsql_str_new(config->db);

      dbsql_str_appendf(defn, "id INTEGER PRIMARY KEY");
      for(i=0; i<config->n_col; i++){
        if( config->content==FTS5_CONTENT_NORMAL || config->ab_unindexed[i] ){
          dbsql_str_appendf(defn, ", c%d", i);
        }
      }
      if( config->locale ){
        for(i=0; i<config->n_col; i++){
          if( config->ab_unindexed[i]==0 ){
            dbsql_str_appendf(defn, ", l%d", i);
          }
        }
      }
      z_defn = dbsql_str_finish(defn);

      if( z_defn ){
        rc = __fts5_create_table(config, "content", z_defn, 0, err_p);
        dbsql_free(z_defn);
      }else{
        rc = DBSQL_NOMEM;
      }
    }

    if( rc==DBSQL_OK && config->columnsize ){
      const char *cols = "id INTEGER PRIMARY KEY, sz BLOB";
      if( config->contentless_delete ){
        cols = "id INTEGER PRIMARY KEY, sz BLOB, origin INTEGER";
      }
      rc = __fts5_create_table(config, "docsize", cols, 0, err_p);
    }
    if( rc==DBSQL_OK ){
      rc = __fts5_create_table(
          config, "config", "k PRIMARY KEY, v", 1, err_p
      );
    }
    if( rc==DBSQL_OK ){
      rc = __fts5_storage_config_value(p, "version", 0, FTS5_CURRENT_VERSION);
    }
  }

  if( rc ){
    __fts5_storage_close(p);
    *pp = 0;
  }
  return rc;
}

/*
** Close a handle opened by an earlier call to __fts5_storage_open().
*/
int __fts5_storage_close(fts5_storage_t *p){
  int rc = DBSQL_OK;
  if( p ){
    int i;

    /* Finalize all SQL statements */
    for(i=0; i<ARRAY_SIZE(p->stmts); i++){
      dbsql_finalize(p->stmts[i]);
    }

    dbsql_free(p);
  }
  return rc;
}

typedef struct fts5_insert_ctx_t fts5_insert_ctx_t;
struct fts5_insert_ctx_t {
  fts5_storage_t *storage;
  int i_col;
  int sz_col;                      /* Size of column value in tokens */
};

/*
** Tokenization callback used when inserting tokens into the FTS index.
*/
static int __fts5_storage_insert_callback(
  void *context,                 /* Pointer to fts5_insert_ctx_t object */
  int tflags,
  const char *token,             /* Buffer containing token */
  int n_token,                     /* Size of token in bytes */
  int i_unused1,                   /* Start offset of token */
  int i_unused2                    /* End offset of token */
){
  fts5_insert_ctx_t *ctx = (fts5_insert_ctx_t*)context;
  fts5_index_t *idx = ctx->storage->p_index;
  UNUSED_PARAM2(i_unused1, i_unused2);
  if( n_token>FTS5_MAX_TOKEN_SIZE ) n_token = FTS5_MAX_TOKEN_SIZE;
  if( (tflags & FTS5_TOKEN_COLOCATED)==0 || ctx->sz_col==0 ){
    ctx->sz_col++;
  }
  return __fts5_index_write(idx, ctx->i_col, ctx->sz_col-1, token, n_token);
}

/*
** This function is used as part of an UPDATE statement that modifies the
** rowid of a row. In that case, this function is called first to set
** fts5_storage_t.saved_row to point to a statement that may be used to 
** access the original values of the row being deleted - i_del.
**
** DBSQL_OK is returned if successful, or an dbsql error code otherwise.
** It is not considered an error if row i_del does not exist. In this case
** saved_row is not set and DBSQL_OK returned.
*/
int __fts5_storage_find_delete_row(fts5_storage_t *p, int64_t i_del){
  int rc = DBSQL_OK;
  dbsql_stmt_t *seek = 0;

  assert( p->saved_row==0 );
  rc = __fts5_storage_get_stmt(p, FTS5_STMT_LOOKUP+1, &seek, 0);
  if( rc==DBSQL_OK ){
    dbsql_bind_int64(seek, 1, i_del);
    if( dbsql_step(seek)!=DBSQL_ROW ){
      rc = dbsql_reset(seek);
    }else{
      p->saved_row = seek;
    }
  }

  return rc;
}

/*
** If a row with rowid i_del is present in the %_content table, add the
** delete-markers to the FTS index necessary to delete it. Do not actually
** remove the %_content row at this time though.
**
** If parameter save_row is true, then fts5_storage_t.saved_row is left
** pointing to a statement (FTS5_STMT_LOOKUP2) that may be used to access
** the original values of the row being deleted. This is used by UPDATE
** statements.
*/
static int __fts5_storage_delete_from_index(
  fts5_storage_t *p, 
  int64_t i_del, 
  dbsql_value_t **vals,
  int save_row                    /* True to set saved_row */
){
  fts5_config_t *config = p->config;
  dbsql_stmt_t *seek = 0;        /* SELECT to read row i_del from %_data */
  int rc = DBSQL_OK;             /* Return code */
  int rc2;                        /* dbsql_reset() return code */
  int i_col;
  fts5_insert_ctx_t ctx;

  assert( save_row==0 || vals==0 );
  assert( save_row==0 || save_row==1 );
  assert( FTS5_STMT_LOOKUP2==FTS5_STMT_LOOKUP+1 );

  if( vals==0 ){
    if( p->saved_row && save_row ){
      seek = p->saved_row;
      p->saved_row = 0;
    }else{
      rc = __fts5_storage_get_stmt(p, FTS5_STMT_LOOKUP+save_row, &seek, 0);
      if( rc!=DBSQL_OK ) return rc;
      dbsql_bind_int64(seek, 1, i_del);
      if( dbsql_step(seek)!=DBSQL_ROW ){
        return dbsql_reset(seek);
      }
    }
  }

  ctx.storage = p;
  ctx.i_col = -1;
  for(i_col=1; rc==DBSQL_OK && i_col<=config->n_col; i_col++){
    if( config->ab_unindexed[i_col-1]==0 ){
      dbsql_value_t *val = 0;
      dbsql_value_t *p_free = 0;
      const char *text = 0;
      int n_text = 0;
      const char *loc = 0;
      int n_loc = 0;

      assert( seek==0 || vals==0 );
      assert( seek!=0 || vals!=0 );
      if( seek ){
        val = dbsql_column_value(seek, i_col);
      }else{
        val = vals[i_col-1];
      }

      if( config->locale && __fts5_is_locale_value(config, val) ){
        rc = __fts5_decode_locale_value(val, &text, &n_text, &loc, &n_loc);
      }else{
        if( dbsql_value_type(val)!=DBSQL_TEXT ){
          /* Make a copy of the value to work with. This is because the call
          ** to dbsql_value_text() below forces the type of the value to
          ** DBSQL_TEXT, and we may need to use it again later. */
          p_free = val = dbsql_value_dup(val);
          if( val==0 ){
            rc = DBSQL_NOMEM;
          }
        }
        if( rc==DBSQL_OK ){
          text = (const char*)dbsql_value_text(val);
          n_text = dbsql_value_bytes(val);
          if( config->locale && seek ){
            loc = (const char*)dbsql_column_text(seek, i_col+config->n_col);
            n_loc = dbsql_column_bytes(seek, i_col + config->n_col);
          }
        }
      }

      if( rc==DBSQL_OK ){
        __fts5_set_locale(config, loc, n_loc);
        ctx.sz_col = 0;
        rc = __fts5_tokenize(config, FTS5_TOKENIZE_DOCUMENT, 
            text, n_text, (void*)&ctx, __fts5_storage_insert_callback
        );
        p->total_sizes[i_col-1] -= (int64_t)ctx.sz_col;
        if( rc==DBSQL_OK && p->total_sizes[i_col-1]<0 ){
          rc = FTS5_CORRUPT;
        }
        __fts5_clear_locale(config);
      }
      dbsql_value_free(p_free);
    }
  }
  if( rc==DBSQL_OK && p->n_total_row<1 ){
    rc = FTS5_CORRUPT;
  }else{
    p->n_total_row--;
  }

  if( rc==DBSQL_OK && save_row ){
    assert( p->saved_row==0 );
    p->saved_row = seek;
  }else{
    rc2 = dbsql_reset(seek);
    if( rc==DBSQL_OK ) rc = rc2;
  }
  return rc;
}

/*
** Reset any saved statement saved_row. Zero saved_row as well. This
** should be called by the update() method of the fts5 table before 
** returning from any operation that may have set fts5_storage_t.saved_row.
*/
void __fts5_storage_release_delete_row(fts5_storage_t *storage){
  assert( storage->saved_row==0 
       || storage->saved_row==storage->stmts[FTS5_STMT_LOOKUP2] 
  );
  dbsql_reset(storage->saved_row);
  storage->saved_row = 0;
}

/*
** This function is called to process a DELETE on a contentless_delete=1
** table. It adds the tombstone required to delete the entry with rowid 
** i_del. If successful, DBSQL_OK is returned. Or, if an error occurs,
** an dbsql error code.
*/
static int __fts5_storage_contentless_delete(fts5_storage_t *p, int64_t i_del){
  int64_t i_origin = 0;
  dbsql_stmt_t *lookup = 0;
  int rc = DBSQL_OK;

  assert( p->config->contentless_delete );
  assert( p->config->content==FTS5_CONTENT_NONE
       || p->config->content==FTS5_CONTENT_UNINDEXED
  );

  /* Look up the origin of the document in the %_docsize table. Store
  ** this in stack variable i_origin.  */
  rc = __fts5_storage_get_stmt(p, FTS5_STMT_LOOKUP_DOCSIZE, &lookup, 0);
  if( rc==DBSQL_OK ){
    dbsql_bind_int64(lookup, 1, i_del);
    if( DBSQL_ROW==dbsql_step(lookup) ){
      i_origin = dbsql_column_int64(lookup, 1);
    }
    rc = dbsql_reset(lookup);
  }

  if( rc==DBSQL_OK && i_origin!=0 ){
    rc = __fts5_index_contentless_delete(p->p_index, i_origin, i_del);
  }

  return rc;
}

/*
** Insert a record into the %_docsize table. Specifically, do:
**
**   INSERT OR REPLACE INTO %_docsize(id, sz) VALUES(i_rowid, buf);
**
** If there is no %_docsize table (as happens if the columnsize=0 option
** is specified when the FTS5 table is created), this function is a no-op.
*/
static int __fts5_storage_insert_docsize(
  fts5_storage_t *p,                 /* Storage module to write to */
  int64_t i_rowid,                     /* id value */
  fts5_buffer_t *buf                /* sz value */
){
  int rc = DBSQL_OK;
  if( p->config->columnsize ){
    dbsql_stmt_t *replace = 0;
    rc = __fts5_storage_get_stmt(p, FTS5_STMT_REPLACE_DOCSIZE, &replace, 0);
    if( rc==DBSQL_OK ){
      dbsql_bind_int64(replace, 1, i_rowid);
      if( p->config->contentless_delete ){
        int64_t i_origin = 0;
        rc = __fts5_index_get_origin(p->p_index, &i_origin);
        dbsql_bind_int64(replace, 3, i_origin);
      }
    }
    if( rc==DBSQL_OK ){
      dbsql_bind_blob(replace, 2, buf->p, buf->n, DBSQL_STATIC);
      dbsql_step(replace);
      rc = dbsql_reset(replace);
      dbsql_bind_null(replace, 2);
    }
  }
  return rc;
}

/*
** Load the contents of the "averages" record from disk into the 
** p->n_total_row and p->total_sizes[] variables. If successful, and if
** argument cache is true, set the p->totals_valid flag to indicate
** that the contents of total_sizes[] and n_total_row are valid until
** further notice.
**
** Return DBSQL_OK if successful, or an dbsql error code if an error
** occurs.
*/
static int __fts5_storage_load_totals(fts5_storage_t *p, int cache){
  int rc = DBSQL_OK;
  if( p->totals_valid==0 ){
    rc = __fts5_index_get_averages(p->p_index, &p->n_total_row, p->total_sizes);
    p->totals_valid = cache;
  }
  return rc;
}

/*
** Store the current contents of the p->n_total_row and p->total_sizes[] 
** variables in the "averages" record on disk.
**
** Return DBSQL_OK if successful, or an dbsql error code if an error
** occurs.
*/
static int __fts5_storage_save_totals(fts5_storage_t *p){
  int n_col = p->config->n_col;
  int i;
  fts5_buffer_t buf;
  int rc = DBSQL_OK;
  memset(&buf, 0, sizeof(buf));

  __fts5_buffer_append_varint(&rc, &buf, p->n_total_row);
  for(i=0; i<n_col; i++){
    __fts5_buffer_append_varint(&rc, &buf, p->total_sizes[i]);
  }
  if( rc==DBSQL_OK ){
    rc = __fts5_index_set_averages(p->p_index, buf.p, buf.n);
  }
  dbsql_free(buf.p);

  return rc;
}

/*
** Remove a row from the FTS table.
*/
int __fts5_storage_delete(
  fts5_storage_t *p,                 /* Storage object */
  int64_t i_del,                       /* Rowid to delete from table */
  dbsql_value_t **vals,          /* Optional - values to remove from index */
  int save_row                    /* If true, set saved_row for deleted row */
){
  fts5_config_t *config = p->config;
  int rc;
  dbsql_stmt_t *del = 0;

  assert( config->content!=FTS5_CONTENT_NORMAL || vals==0 );
  rc = __fts5_storage_load_totals(p, 1);

  /* Delete the index records */
  if( rc==DBSQL_OK ){
    rc = __fts5_index_begin_write(p->p_index, 1, i_del);
  }

  if( rc==DBSQL_OK ){
    if( p->config->contentless_delete ){
      rc = __fts5_storage_contentless_delete(p, i_del);
      if( rc==DBSQL_OK 
       && save_row 
       && p->config->content==FTS5_CONTENT_UNINDEXED
      ){
        rc = __fts5_storage_find_delete_row(p, i_del);
      }
    }else{
      rc = __fts5_storage_delete_from_index(p, i_del, vals, save_row);
    }
  }

  /* Delete the %_docsize record */
  if( rc==DBSQL_OK && config->columnsize ){
    rc = __fts5_storage_get_stmt(p, FTS5_STMT_DELETE_DOCSIZE, &del, 0);
    if( rc==DBSQL_OK ){
      dbsql_bind_int64(del, 1, i_del);
      dbsql_step(del);
      rc = dbsql_reset(del);
    }
  }

  /* Delete the %_content record */
  if( config->content==FTS5_CONTENT_NORMAL 
   || config->content==FTS5_CONTENT_UNINDEXED 
  ){
    if( rc==DBSQL_OK ){
      rc = __fts5_storage_get_stmt(p, FTS5_STMT_DELETE_CONTENT, &del, 0);
    }
    if( rc==DBSQL_OK ){
      dbsql_bind_int64(del, 1, i_del);
      dbsql_step(del);
      rc = dbsql_reset(del);
    }
  }

  return rc;
}

/*
** Delete all entries in the FTS5 index.
*/
int __fts5_storage_delete_all(fts5_storage_t *p){
  fts5_config_t *config = p->config;
  int rc;

  p->totals_valid = 0;

  /* Delete the contents of the %_data and %_docsize tables. */
  rc = __fts5_exec_printf(config->db, 0,
      "DELETE FROM %Q.'%q_data';" 
      "DELETE FROM %Q.'%q_idx';",
      config->z_db, config->name,
      config->z_db, config->name
  );
  if( rc==DBSQL_OK && config->columnsize ){
    rc = __fts5_exec_printf(config->db, 0,
        "DELETE FROM %Q.'%q_docsize';", config->z_db, config->name
    );
  }

  if( rc==DBSQL_OK && config->content==FTS5_CONTENT_UNINDEXED ){
    rc = __fts5_exec_printf(config->db, 0,
        "DELETE FROM %Q.'%q_content';", config->z_db, config->name
    );
  }

  /* Reinitialize the %_data table. This call creates the initial structure
  ** and averages records.  */
  if( rc==DBSQL_OK ){
    rc = __fts5_index_reinit(p->p_index);
  }
  if( rc==DBSQL_OK ){
    rc = __fts5_storage_config_value(p, "version", 0, FTS5_CURRENT_VERSION);
  }
  return rc;
}

int __fts5_storage_rebuild(fts5_storage_t *p){
  fts5_buffer_t buf = {0,0,0};
  fts5_config_t *config = p->config;
  dbsql_stmt_t *scan = 0;
  fts5_insert_ctx_t ctx;
  int rc, rc2;

  memset(&ctx, 0, sizeof(fts5_insert_ctx_t));
  ctx.storage = p;
  rc = __fts5_storage_delete_all(p);
  if( rc==DBSQL_OK ){
    rc = __fts5_storage_load_totals(p, 1);
  }

  if( rc==DBSQL_OK ){
    rc = __fts5_storage_get_stmt(p, FTS5_STMT_SCAN, &scan, config->errmsg);
  }

  while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(scan) ){
    int64_t i_rowid = dbsql_column_int64(scan, 0);

    __fts5_buffer_zero(&buf);
    rc = __fts5_index_begin_write(p->p_index, 0, i_rowid);
    for(ctx.i_col=0; rc==DBSQL_OK && ctx.i_col<config->n_col; ctx.i_col++){
      ctx.sz_col = 0;
      if( config->ab_unindexed[ctx.i_col]==0 ){
        int n_text = 0;            /* Size of text in bytes */
        const char *text = 0;    /* Pointer to buffer containing text value */
        int n_loc = 0;             /* Size of loc in bytes */
        const char *loc = 0;     /* Pointer to buffer containing text value */

        dbsql_value_t *val = dbsql_column_value(scan, ctx.i_col+1);
        if( config->content==FTS5_CONTENT_EXTERNAL
         && __fts5_is_locale_value(config, val)
        ){
          rc = __fts5_decode_locale_value(val, &text, &n_text, &loc, &n_loc);
        }else{
          text = (const char*)dbsql_value_text(val);
          n_text = dbsql_value_bytes(val);
          if( config->locale ){
            int i_col = ctx.i_col + 1 + config->n_col;
            loc = (const char*)dbsql_column_text(scan, i_col);
            n_loc = dbsql_column_bytes(scan, i_col);
          }
        }

        if( rc==DBSQL_OK ){
          __fts5_set_locale(config, loc, n_loc);
          rc = __fts5_tokenize(config, 
              FTS5_TOKENIZE_DOCUMENT,
              text, n_text,
              (void*)&ctx,
              __fts5_storage_insert_callback
          );
          __fts5_clear_locale(config);
        }
      }
      __fts5_buffer_append_varint(&rc, &buf, ctx.sz_col);
      p->total_sizes[ctx.i_col] += (int64_t)ctx.sz_col;
    }
    p->n_total_row++;

    if( rc==DBSQL_OK ){
      rc = __fts5_storage_insert_docsize(p, i_rowid, &buf);
    }
  }
  dbsql_free(buf.p);
  rc2 = dbsql_reset(scan);
  if( rc==DBSQL_OK ) rc = rc2;

  /* Write the averages record */
  if( rc==DBSQL_OK ){
    rc = __fts5_storage_save_totals(p);
  }
  return rc;
}

int __fts5_storage_optimize(fts5_storage_t *p){
  return __fts5_index_optimize(p->p_index);
}

int __fts5_storage_merge(fts5_storage_t *p, int n_merge){
  return __fts5_index_merge(p->p_index, n_merge);
}

int __fts5_storage_reset(fts5_storage_t *p){
  return __fts5_index_reset(p->p_index);
}

/*
** Allocate a new rowid. This is used for "external content" tables when
** a NULL value is inserted into the rowid column. The new rowid is allocated
** by inserting a dummy row into the %_docsize table. The dummy will be
** overwritten later.
**
** If the %_docsize table does not exist, DBSQL_MISMATCH is returned. In
** this case the user is required to provide a rowid explicitly.
*/
static int __fts5_storage_new_rowid(fts5_storage_t *p, int64_t *pi_rowid){
  int rc = DBSQL_MISMATCH;
  if( p->config->columnsize ){
    dbsql_stmt_t *replace = 0;
    rc = __fts5_storage_get_stmt(p, FTS5_STMT_REPLACE_DOCSIZE, &replace, 0);
    if( rc==DBSQL_OK ){
      dbsql_bind_null(replace, 1);
      dbsql_bind_null(replace, 2);
      dbsql_step(replace);
      rc = dbsql_reset(replace);
    }
    if( rc==DBSQL_OK ){
      *pi_rowid = dbsql_last_insert_rowid(p->config->db);
    }
  }
  return rc;
}

/*
** Insert a new row into the FTS content table.
*/
int __fts5_storage_content_insert(
  fts5_storage_t *p, 
  int replace,                   /* True to use REPLACE instead of INSERT */
  dbsql_value_t **vals, 
  int64_t *pi_rowid
){
  fts5_config_t *config = p->config;
  int rc = DBSQL_OK;

  /* Insert the new row into the %_content table. */
  if( config->content!=FTS5_CONTENT_NORMAL 
   && config->content!=FTS5_CONTENT_UNINDEXED
  ){
    if( dbsql_value_type(vals[1])==DBSQL_INTEGER ){
      *pi_rowid = dbsql_value_int64(vals[1]);
    }else{
      rc = __fts5_storage_new_rowid(p, pi_rowid);
    }
  }else{
    dbsql_stmt_t *insert = 0;    /* Statement to write %_content table */
    int i;                        /* Counter variable */

    assert( FTS5_STMT_INSERT_CONTENT+1==FTS5_STMT_REPLACE_CONTENT );
    assert( replace==0 || replace==1 );
    rc = __fts5_storage_get_stmt(p, FTS5_STMT_INSERT_CONTENT+replace, &insert, 0);
    if( insert ) dbsql_clear_bindings(insert);

    /* Bind the rowid value */
    dbsql_bind_value(insert, 1, vals[1]);

    /* Loop through values for user-defined columns. i=2 is the leftmost
    ** user-defined column. As is column 1 of saved_row.  */
    for(i=2; rc==DBSQL_OK && i<=config->n_col+1; i++){
      int unindexed = config->ab_unindexed[i-2];
      if( config->content==FTS5_CONTENT_NORMAL || unindexed ){
        dbsql_value_t *val = vals[i];

        if( dbsql_value_nochange(val) && p->saved_row ){
          /* This is an UPDATE statement, and user-defined column (i-2) was not
          ** modified.  Retrieve the value from fts5_storage_t.saved_row.  */
          val = dbsql_column_value(p->saved_row, i-1);
          if( config->locale && unindexed==0 ){
            dbsql_bind_value(insert, config->n_col + i,
                dbsql_column_value(p->saved_row, config->n_col + i - 1)
            );
          }
        }else if( __fts5_is_locale_value(config, val) ){
          const char *text = 0;
          const char *loc = 0;
          int n_text = 0;
          int n_loc = 0;
          assert( config->locale );

          rc = __fts5_decode_locale_value(val, &text, &n_text, &loc, &n_loc);
          if( rc==DBSQL_OK ){
            dbsql_bind_text(insert, i, text, n_text, DBSQL_TRANSIENT);
            if( unindexed==0 ){
              int i_loc = config->n_col + i;
              dbsql_bind_text(insert, i_loc, loc, n_loc, DBSQL_TRANSIENT);
            }
          }

          continue;
        }

        rc = dbsql_bind_value(insert, i, val);
      }
    }
    if( rc==DBSQL_OK ){
      dbsql_step(insert);
      rc = dbsql_reset(insert);
    }
    *pi_rowid = dbsql_last_insert_rowid(config->db);
  }

  return rc;
}

/*
** Insert new entries into the FTS index and %_docsize table.
*/
int __fts5_storage_index_insert(
  fts5_storage_t *p, 
  dbsql_value_t **vals, 
  int64_t i_rowid
){
  fts5_config_t *config = p->config;
  int rc = DBSQL_OK;             /* Return code */
  fts5_insert_ctx_t ctx;              /* Tokenization callback context object */
  fts5_buffer_t buf;                 /* Buffer used to build up %_docsize blob */

  memset(&buf, 0, sizeof(fts5_buffer_t));
  ctx.storage = p;
  rc = __fts5_storage_load_totals(p, 1);

  if( rc==DBSQL_OK ){
    rc = __fts5_index_begin_write(p->p_index, 0, i_rowid);
  }
  for(ctx.i_col=0; rc==DBSQL_OK && ctx.i_col<config->n_col; ctx.i_col++){
    ctx.sz_col = 0;
    if( config->ab_unindexed[ctx.i_col]==0 ){
      int n_text = 0;              /* Size of text in bytes */
      const char *text = 0;      /* Pointer to buffer containing text value */
      int n_loc = 0;               /* Size of text in bytes */
      const char *loc = 0;       /* Pointer to buffer containing text value */

      dbsql_value_t *val = vals[ctx.i_col+2];
      if( p->saved_row && dbsql_value_nochange(val) ){
        val = dbsql_column_value(p->saved_row, ctx.i_col+1);
        if( config->content==FTS5_CONTENT_NORMAL && config->locale ){
          int i_col = ctx.i_col + 1 + config->n_col;
          loc = (const char*)dbsql_column_text(p->saved_row, i_col);
          n_loc = dbsql_column_bytes(p->saved_row, i_col);
        }
      }else{
        val = vals[ctx.i_col+2];
      }

      if( config->locale && __fts5_is_locale_value(config, val) ){
        rc = __fts5_decode_locale_value(val, &text, &n_text, &loc, &n_loc);
      }else{
        text = (const char*)dbsql_value_text(val);
        n_text = dbsql_value_bytes(val);
      }

      if( rc==DBSQL_OK ){
        __fts5_set_locale(config, loc, n_loc);
        rc = __fts5_tokenize(config, 
            FTS5_TOKENIZE_DOCUMENT, text, n_text, (void*)&ctx,
            __fts5_storage_insert_callback
        );
        __fts5_clear_locale(config);
      }
    }
    __fts5_buffer_append_varint(&rc, &buf, ctx.sz_col);
    p->total_sizes[ctx.i_col] += (int64_t)ctx.sz_col;
  }
  p->n_total_row++;

  /* Write the %_docsize record */
  if( rc==DBSQL_OK ){
    rc = __fts5_storage_insert_docsize(p, i_rowid, &buf);
  }
  dbsql_free(buf.p);

  return rc;
}

static int __fts5_storage_count(fts5_storage_t *p, const char *suffix, int64_t *pn_row){
  fts5_config_t *config = p->config;
  char *sql;
  int rc;

  sql = dbsql_mprintf("SELECT count(*) FROM %Q.'%q_%s'", 
      config->z_db, config->name, suffix
  );
  if( sql==0 ){
    rc = DBSQL_NOMEM;
  }else{
    dbsql_stmt_t *cnt = 0;
    rc = dbsql_prepare_v2(config->db, sql, -1, &cnt, 0);
    if( rc==DBSQL_OK ){
      if( DBSQL_ROW==dbsql_step(cnt) ){
        *pn_row = dbsql_column_int64(cnt, 0);
      }
      rc = dbsql_finalize(cnt);
    }
  }

  dbsql_free(sql);
  return rc;
}

/*
** Context object used by __fts5_storage_integrity().
*/
typedef struct fts5_integrity_ctx_t fts5_integrity_ctx_t;
struct fts5_integrity_ctx_t {
  int64_t i_rowid;
  int i_col;
  int sz_col;
  u_int64_t cksum;
  fts5_termset_t *termset;
  fts5_config_t *config;
};


/*
** Tokenization callback used by integrity check.
*/
static int __fts5_storage_integrity_callback(
  void *context,                 /* Pointer to fts5_integrity_ctx_t object */
  int tflags,
  const char *token,             /* Buffer containing token */
  int n_token,                     /* Size of token in bytes */
  int i_unused1,                   /* Start offset of token */
  int i_unused2                    /* End offset of token */
){
  fts5_integrity_ctx_t *ctx = (fts5_integrity_ctx_t*)context;
  fts5_termset_t *termset = ctx->termset;
  int present;
  int ii;
  int rc = DBSQL_OK;
  int i_pos;
  int i_col;

  UNUSED_PARAM2(i_unused1, i_unused2);
  if( n_token>FTS5_MAX_TOKEN_SIZE ) n_token = FTS5_MAX_TOKEN_SIZE;

  if( (tflags & FTS5_TOKEN_COLOCATED)==0 || ctx->sz_col==0 ){
    ctx->sz_col++;
  }

  switch( ctx->config->detail ){
    case FTS5_DETAIL_FULL:
      i_pos = ctx->sz_col-1;
      i_col = ctx->i_col;
      break;

    case FTS5_DETAIL_COLUMNS:
      i_pos = ctx->i_col;
      i_col = 0;
      break;

    default:
      assert( ctx->config->detail==FTS5_DETAIL_NONE );
      i_pos = 0;
      i_col = 0;
      break;
  }

  rc = __fts5_termset_add(termset, 0, token, n_token, &present);
  if( rc==DBSQL_OK && present==0 ){
    ctx->cksum ^= __fts5_index_entry_cksum(
        ctx->i_rowid, i_col, i_pos, 0, token, n_token
    );
  }

  for(ii=0; rc==DBSQL_OK && ii<ctx->config->n_prefix; ii++){
    const int n_char = ctx->config->__a_prefix[ii];
    int n_byte = __fts5_index_charlen_to_bytelen(token, n_token, n_char);
    if( n_byte ){
      rc = __fts5_termset_add(termset, ii+1, token, n_byte, &present);
      if( present==0 ){
        ctx->cksum ^= __fts5_index_entry_cksum(
            ctx->i_rowid, i_col, i_pos, ii+1, token, n_byte
        );
      }
    }
  }

  return rc;
}

/*
** Check that the contents of the FTS index match that of the %_content
** table. Return DBSQL_OK if they do, or DBSQL_CORRUPT if not. Return
** some other dbsql error code if an error occurs while attempting to
** determine this.
*/
int __fts5_storage_integrity(fts5_storage_t *p, int i_arg){
  fts5_config_t *config = p->config;
  int rc = DBSQL_OK;             /* Return code */
  int *col_sizes;                  /* Array of size config->n_col */
  int64_t *total_sizes;                /* Array of size config->n_col */
  fts5_integrity_ctx_t ctx;
  dbsql_stmt_t *scan;
  int use_cksum;

  memset(&ctx, 0, sizeof(fts5_integrity_ctx_t));
  ctx.config = p->config;
  total_sizes = (int64_t*)dbsql_malloc64(config->n_col*(sizeof(int)+sizeof(int64_t)));
  if( !total_sizes ) return DBSQL_NOMEM;
  col_sizes = (int*)&total_sizes[config->n_col];
  memset(total_sizes, 0, sizeof(int64_t) * config->n_col);

  use_cksum = (config->content==FTS5_CONTENT_NORMAL
           || (config->content==FTS5_CONTENT_EXTERNAL && i_arg)
  );
  if( use_cksum ){
    /* Generate the expected index checksum based on the contents of the
    ** %_content table. This block stores the checksum in ctx.cksum. */
    rc = __fts5_storage_get_stmt(p, FTS5_STMT_SCAN, &scan, 0);
    if( rc==DBSQL_OK ){
      int rc2;
      while( DBSQL_ROW==dbsql_step(scan) ){
        int i;
        ctx.i_rowid = dbsql_column_int64(scan, 0);
        ctx.sz_col = 0;
        if( config->columnsize ){
          rc = __fts5_storage_docsize(p, ctx.i_rowid, col_sizes);
        }
        if( rc==DBSQL_OK && config->detail==FTS5_DETAIL_NONE ){
          rc = __fts5_termset_new(&ctx.termset);
        }
        for(i=0; rc==DBSQL_OK && i<config->n_col; i++){
          if( config->ab_unindexed[i]==0 ){
            const char *text = 0;
            int n_text = 0;
            const char *loc = 0;
            int n_loc = 0;
            dbsql_value_t *val = dbsql_column_value(scan, i+1);

            if( config->content==FTS5_CONTENT_EXTERNAL 
             && __fts5_is_locale_value(config, val)
            ){
              rc = __fts5_decode_locale_value(
                  val, &text, &n_text, &loc, &n_loc
              );
            }else{
              if( config->content==FTS5_CONTENT_NORMAL && config->locale ){
                int i_col = i + 1 + config->n_col;
                loc = (const char*)dbsql_column_text(scan, i_col);
                n_loc = dbsql_column_bytes(scan, i_col);
              }
              text = (const char*)dbsql_value_text(val);
              n_text = dbsql_value_bytes(val);
            }

            ctx.i_col = i;
            ctx.sz_col = 0;

            if( rc==DBSQL_OK && config->detail==FTS5_DETAIL_COLUMNS ){
              rc = __fts5_termset_new(&ctx.termset);
            }

            if( rc==DBSQL_OK ){
              __fts5_set_locale(config, loc, n_loc);
              rc = __fts5_tokenize(config, 
                  FTS5_TOKENIZE_DOCUMENT,
                  text, n_text,
                  (void*)&ctx,
                  __fts5_storage_integrity_callback
              );
              __fts5_clear_locale(config);
            }

            /* If this is not a columnsize=0 database, check that the number
            ** of tokens in the value matches the col_sizes[] value read from
            ** the %_docsize table.  */
            if( rc==DBSQL_OK 
             && config->columnsize 
             && ctx.sz_col!=col_sizes[i] 
            ){
              rc = FTS5_CORRUPT;
            }
            total_sizes[i] += ctx.sz_col;
            if( config->detail==FTS5_DETAIL_COLUMNS ){
              __fts5_termset_free(ctx.termset);
              ctx.termset = 0;
            }
          }
        }
        __fts5_termset_free(ctx.termset);
        ctx.termset = 0;
  
        if( rc!=DBSQL_OK ) break;
      }
      rc2 = dbsql_reset(scan);
      if( rc==DBSQL_OK ) rc = rc2;
    }

    /* Test that the "totals" (sometimes called "averages") record looks Ok */
    if( rc==DBSQL_OK ){
      int i;
      rc = __fts5_storage_load_totals(p, 0);
      for(i=0; rc==DBSQL_OK && i<config->n_col; i++){
        if( p->total_sizes[i]!=total_sizes[i] ) rc = FTS5_CORRUPT;
      }
    }

    /* Check that the %_docsize and %_content tables contain the expected
    ** number of rows.  */
    if( rc==DBSQL_OK && config->content==FTS5_CONTENT_NORMAL ){
      int64_t n_row = 0;
      rc = __fts5_storage_count(p, "content", &n_row);
      if( rc==DBSQL_OK && n_row!=p->n_total_row ) rc = FTS5_CORRUPT;
    }
    if( rc==DBSQL_OK && config->columnsize ){
      int64_t n_row = 0;
      rc = __fts5_storage_count(p, "docsize", &n_row);
      if( rc==DBSQL_OK && n_row!=p->n_total_row ) rc = FTS5_CORRUPT;
    }
  }

  /* Pass the expected checksum down to the FTS index module. It will
  ** verify, amongst other things, that it matches the checksum generated by
  ** inspecting the index itself.  */
  if( rc==DBSQL_OK ){
    rc = __fts5_index_integrity_check(p->p_index, ctx.cksum, use_cksum);
  }

  dbsql_free(total_sizes);
  return rc;
}

/*
** Obtain an dbsql statement handle that may be used to read data from the
** %_content table.
*/
int __fts5_storage_stmt(
  fts5_storage_t *p, 
  int stmt, 
  dbsql_stmt_t **pp, 
  char **errmsg_p
){
  int rc;
  assert( stmt==FTS5_STMT_SCAN_ASC 
       || stmt==FTS5_STMT_SCAN_DESC
       || stmt==FTS5_STMT_LOOKUP
  );
  rc = __fts5_storage_get_stmt(p, stmt, pp, errmsg_p);
  if( rc==DBSQL_OK ){
    assert( p->stmts[stmt]==*pp );
    p->stmts[stmt] = 0;
  }
  return rc;
}

/*
** Release an dbsql statement handle obtained via an earlier call to
** __fts5_storage_stmt(). The stmt parameter passed to this function
** must match that passed to the __fts5_storage_stmt() call.
*/
void __fts5_storage_stmt_release(
  fts5_storage_t *p, 
  int stmt, 
  dbsql_stmt_t *p_stmt
){
  assert( stmt==FTS5_STMT_SCAN_ASC
       || stmt==FTS5_STMT_SCAN_DESC
       || stmt==FTS5_STMT_LOOKUP
  );
  if( p->stmts[stmt]==0 ){
    dbsql_reset(p_stmt);
    p->stmts[stmt] = p_stmt;
  }else{
    dbsql_finalize(p_stmt);
  }
}

static int __fts5_storage_decode_size_array(
  int *cols, int n_col,            /* Array to populate */
  const u_int8_t *blobs, int n_blob      /* Record to read varints from */
){
  int i;
  int i_off = 0;
  for(i=0; i<n_col; i++){
    if( i_off>=n_blob ) return 1;
    i_off += FTS5_GET_VARINT32(&blobs[i_off], cols[i]);
  }
  return (i_off!=n_blob);
}

/*
** Argument cols points to an array of integers containing one entry for
** each table column. This function reads the %_docsize record for the
** specified rowid and populates cols[] with the results.
**
** An dbsql error code is returned if an error occurs, or DBSQL_OK
** otherwise.
*/
int __fts5_storage_docsize(fts5_storage_t *p, int64_t i_rowid, int *cols){
  int n_col = p->config->n_col;    /* Number of user columns in table */
  dbsql_stmt_t *lookup = 0;      /* Statement to query %_docsize */
  int rc;                         /* Return Code */

  assert( p->config->columnsize );
  rc = __fts5_storage_get_stmt(p, FTS5_STMT_LOOKUP_DOCSIZE, &lookup, 0);
  if( lookup ){
    int corrupt = 1;
    assert( rc==DBSQL_OK );
    dbsql_bind_int64(lookup, 1, i_rowid);
    if( DBSQL_ROW==dbsql_step(lookup) ){
      const u_int8_t *blobs = dbsql_column_blob(lookup, 0);
      int n_blob = dbsql_column_bytes(lookup, 0);
      if( 0==__fts5_storage_decode_size_array(cols, n_col, blobs, n_blob) ){
        corrupt = 0;
      }
    }
    rc = dbsql_reset(lookup);
    if( corrupt && rc==DBSQL_OK ){
      rc = FTS5_CORRUPT;
    }
  }else{
    assert( rc!=DBSQL_OK );
  }

  return rc;
}

int __fts5_storage_size(fts5_storage_t *p, int i_col, int64_t *pn_token){
  int rc = __fts5_storage_load_totals(p, 0);
  if( rc==DBSQL_OK ){
    *pn_token = 0;
    if( i_col<0 ){
      int i;
      for(i=0; i<p->config->n_col; i++){
        *pn_token += p->total_sizes[i];
      }
    }else if( i_col<p->config->n_col ){
      *pn_token = p->total_sizes[i_col];
    }else{
      rc = DBSQL_RANGE;
    }
  }
  return rc;
}

int __fts5_storage_row_count(fts5_storage_t *p, int64_t *pn_row){
  int rc = __fts5_storage_load_totals(p, 0);
  if( rc==DBSQL_OK ){
    /* n_total_row being zero does not necessarily indicate a corrupt 
    ** database - it might be that the FTS5 table really does contain zero
    ** rows. However this function is only called from the row_count() API,
    ** and there is no way for that API to be invoked if the table contains
    ** no rows. Hence the FTS5_CORRUPT return.  */
    *pn_row = p->n_total_row;
    if( p->n_total_row<=0 ) rc = FTS5_CORRUPT;
  }
  return rc;
}

/*
** Flush any data currently held in-memory to disk.
*/
int __fts5_storage_sync(fts5_storage_t *p){
  int rc = DBSQL_OK;
  int64_t i_last_rowid = dbsql_last_insert_rowid(p->config->db);
  if( p->totals_valid ){
    rc = __fts5_storage_save_totals(p);
    if( rc==DBSQL_OK ){
      p->totals_valid = 0;
    }
  }
  if( rc==DBSQL_OK ){
    rc = __fts5_index_sync(p->p_index);
  }
  dbsql_set_last_insert_rowid(p->config->db, i_last_rowid);
  return rc;
}

int __fts5_storage_rollback(fts5_storage_t *p){
  p->totals_valid = 0;
  return __fts5_index_rollback(p->p_index);
}

int __fts5_storage_config_value(
  fts5_storage_t *p, 
  const char *z,
  dbsql_value_t *val,
  int i_val
){
  dbsql_stmt_t *replace = 0;
  int rc = __fts5_storage_get_stmt(p, FTS5_STMT_REPLACE_CONFIG, &replace, 0);
  if( rc==DBSQL_OK ){
    dbsql_bind_text(replace, 1, z, -1, DBSQL_STATIC);
    if( val ){
      dbsql_bind_value(replace, 2, val);
    }else{
      dbsql_bind_int(replace, 2, i_val);
    }
    dbsql_step(replace);
    rc = dbsql_reset(replace);
    dbsql_bind_null(replace, 1);
  }
  if( rc==DBSQL_OK && val ){
    int i_new = p->config->i_cookie + 1;
    rc = __fts5_index_set_cookie(p->p_index, i_new);
    if( rc==DBSQL_OK ){
      p->config->i_cookie = i_new;
    }
  }
  return rc;
}
