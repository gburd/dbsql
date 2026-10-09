/*
** 2013 Apr 22
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
** This file contains code for the "fts5tokenize" virtual table module.
** An fts5tokenize virtual table is created as follows:
**
**   CREATE VIRTUAL TABLE <tbl> USING fts5tokenize(
**       <tokenizer-name>, <arg-1>, ...
**   );
**
** The table created has the following schema:
**
**   CREATE TABLE <tbl>(input HIDDEN, token, start, end, position)
**
** When queried, the query must include a WHERE clause of type:
**
**   input = <string>
**
** The virtual table module tokenizes this <string>, using the FTS3 
** tokenizer specified by the arguments to the CREATE VIRTUAL TABLE 
** statement and returns one row for each token in the result. with_t
** fields set as follows:
**
**   input:   Always set to a copy of <string>
**   token:   A token from the input.
**   start:   Byte offset of the token within the input <string>.
**   end:     Byte offset of the byte immediately following the end of the
**            token within the input string.
**   pos:     Token offset of token within input.
**
*/
#if defined(DBSQL_TEST) && defined(DBSQL_ENABLE_FTS5)

#include "fts5.h"
#include <string.h>
#include <assert.h>

typedef struct fts5tok_table_t fts5tok_table_t;
typedef struct fts5tok_cursor_t fts5tok_cursor_t;
typedef struct fts5tok_row_t fts5tok_row_t;

/*
** Virtual table structure.
*/
struct fts5tok_table_t {
  dbsql_vtab_t base;              /* Base class used by dbsql core */
  fts5_tokenizer tok;             /* Tokenizer functions */
  fts5_tokenizer_t *p_tok;            /* Tokenizer instance */
};

/*
** A container for a rows values.
*/
struct fts5tok_row_t {
  char *token;
  int i_start;
  int i_end;
  int i_pos;
};

/*
** Virtual table cursor structure.
*/
struct fts5tok_cursor_t {
  dbsql_vtab_cursor_t base;       /* Base class used by dbsql core */
  int i_rowid;                     /* Current 'rowid' value */
  char *input;                   /* Input string */
  int n_row;                       /* Number of entries in rows[] */
  fts5tok_row_t *rows;               /* Array of rows to return */
};

static void __fts5tok_dequote(char *z){
  char q = z[0];

  if( q=='[' || q=='\'' || q=='"' || q=='`' ){
    int i_in = 1;
    int i_out = 0;
    if( q=='[' ) q = ']';  

    while( z[i_in] ){
      if( z[i_in]==q ){
        if( z[i_in+1]!=q ){
          /* Character i_in was the close quote. */
          i_in++;
          break;
        }else{
          /* Character i_in and i_in+1 form an escaped quote character. Skip
          ** the input cursor past both and copy a single quote character 
          ** to the output buffer. */
          i_in += 2;
          z[i_out++] = q;
        }
      }else{
        z[i_out++] = z[i_in++];
      }
    }

    z[i_out] = '\0';
  }
}

/*
** The second argument, argv[], is an array of pointers to nul-terminated
** strings. This function makes a copy of the array and strings into a 
** single block of memory. It then dequotes any of the strings that appear
** to be quoted.
**
** If successful, output parameter *paz_dequote is set to point at the
** array of dequoted strings and DBSQL_OK is returned. The caller is
** responsible for eventually calling dbsql_free() to free the array
** in this case. Or, if an error occurs, an dbsql error code is returned.
** The final value of *paz_dequote is undefined in this case.
*/
static int __fts5tok_dequote_array(
  int argc,                       /* Number of elements in argv[] */
  const char * const *argv,       /* Input array */
  char ***paz_dequote              /* Output array */
){
  int rc = DBSQL_OK;             /* Return code */
  if( argc==0 ){
    *paz_dequote = 0;
  }else{
    int i;
    int n_byte = 0;
    char **dequotes;

    for(i=0; i<argc; i++){
      n_byte += (int)(strlen(argv[i]) + 1);
    }

    *paz_dequote = dequotes = dbsql_malloc64(sizeof(char *)*argc + n_byte);
    if( dequotes==0 ){
      rc = DBSQL_NOMEM;
    }else{
      char *space = (char *)&dequotes[argc];
      for(i=0; i<argc; i++){
        int n = (int)strlen(argv[i]);
        dequotes[i] = space;
        memcpy(space, argv[i], n+1);
        __fts5tok_dequote(space);
        space += (n+1);
      }
    }
  }

  return rc;
}

/*
** schema_t of the tokenizer table.
*/
#define FTS3_TOK_SCHEMA "CREATE TABLE x(input HIDDEN, token, start, end, position)"

/*
** This function does all the work for both the connect and create methods.
** These tables have no persistent representation of their own, so connect
** and create are identical operations.
**
**   argv[0]: module name
**   argv[1]: database name 
**   argv[2]: table name
**   argv[3]: first argument (tokenizer name)
*/
static int __fts5tok_connect_method(
  DBSQL *db,                    /* Database connection */
  void *ctx,                     /* Pointer to fts5_api object */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* create/connect argument array */
  dbsql_vtab_t **vtab,          /* OUT: New dbsql_vtab_t object */
  char **err_p                    /* OUT: dbsql_malloc'd error message */
){
  fts5_api *api = (fts5_api*)ctx;
  fts5tok_table_t *tab = 0;
  int rc;
  char **dequotes = 0;
  int n_dequote = 0;

  rc = dbsql_declare_vtab(db, 
       "CREATE TABLE x(input HIDDEN, token, start, end, position)"
  );

  if( rc==DBSQL_OK ){
    n_dequote = argc-3;
    rc = __fts5tok_dequote_array(n_dequote, &argv[3], &dequotes);
  }

  if( rc==DBSQL_OK ){
    tab = (fts5tok_table_t*)dbsql_malloc64(sizeof(fts5tok_table_t));
    if( tab==0 ){
      rc = DBSQL_NOMEM;
    }else{
      memset(tab, 0, sizeof(fts5tok_table_t));
    }
  }

  if( rc==DBSQL_OK ){
    void *tok_ctx = 0;
    const char *module = 0;
    if( n_dequote>0 ){
      module = dequotes[0];
    }

    rc = api->find_tokenizer(api, module, &tok_ctx, &tab->tok);
    if( rc==DBSQL_OK ){
      const char **args = (n_dequote>1 ? (const char **)&dequotes[1] : 0);
      int n_arg = n_dequote>0 ? n_dequote-1 : 0;
      rc = tab->tok.create(tok_ctx, args, n_arg, &tab->p_tok);
    }
  }

  if( rc!=DBSQL_OK ){
    dbsql_free(tab);
    tab = 0;
  }

  *vtab = (dbsql_vtab_t*)tab;
  dbsql_free(dequotes);
  return rc;
}

/*
** This function does the work for both the disconnect and destroy methods.
** These tables have no persistent representation of their own, so disconnect
** and destroy are identical operations.
*/
static int __fts5tok_disconnect_method(dbsql_vtab_t *vtab){
  fts5tok_table_t *tab = (fts5tok_table_t *)vtab;
  if( tab->p_tok ){
    tab->tok.x_delete(tab->p_tok);
  }
  dbsql_free(tab);
  return DBSQL_OK;
}

/*
** best_index - Analyze a WHERE and ORDER BY clause.
*/
static int __fts5tok_best_index_method(
  dbsql_vtab_t *v_tab, 
  dbsql_index_info_t *info
){
  int i;

  for(i=0; i<info->n_constraint; i++){
    if( info->constraints[i].usable 
     && info->constraints[i].i_column==0 
     && info->constraints[i].op==DBSQL_INDEX_CONSTRAINT_EQ 
    ){
      info->idx_num = 1;
      info->constraint_usages[i].argv_index = 1;
      info->constraint_usages[i].omit = 1;
      info->estimated_cost = 1;
      return DBSQL_OK;
    }
  }

  info->idx_num = 0;
  assert( info->estimated_cost>1000000.0 );

  return DBSQL_OK;
}

/*
** x_open - Open a cursor.
*/
static int __fts5tok_open_method(dbsql_vtab_t *v_tab, dbsql_vtab_cursor_t **pp_csr){
  fts5tok_cursor_t *csr;

  csr = (fts5tok_cursor_t *)dbsql_malloc64(sizeof(fts5tok_cursor_t));
  if( csr==0 ){
    return DBSQL_NOMEM;
  }
  memset(csr, 0, sizeof(fts5tok_cursor_t));

  *pp_csr = (dbsql_vtab_cursor_t *)csr;
  return DBSQL_OK;
}

/*
** Reset the tokenizer cursor passed as the only argument. As if it had
** just been returned by __fts5tok_open_method().
*/
static void __fts5tok_reset_cursor(fts5tok_cursor_t *csr){
  int i;
  for(i=0; i<csr->n_row; i++){
    dbsql_free(csr->rows[i].token);
  }
  dbsql_free(csr->input);
  dbsql_free(csr->rows);
  csr->input = 0;
  csr->rows = 0;
  csr->n_row = 0;
  csr->i_rowid = 0;
}

/*
** x_close - Close a cursor.
*/
static int __fts5tok_close_method(dbsql_vtab_cursor_t *cursor){
  fts5tok_cursor_t *csr = (fts5tok_cursor_t *)cursor;
  __fts5tok_reset_cursor(csr);
  dbsql_free(csr);
  return DBSQL_OK;
}

/*
** next - Advance the cursor to the next row, if any.
*/
static int __fts5tok_next_method(dbsql_vtab_cursor_t *cursor){
  fts5tok_cursor_t *csr = (fts5tok_cursor_t *)cursor;
  csr->i_rowid++;
  return DBSQL_OK;
}

static int __fts5tok_cb(
  void *ctx,         /* Pointer to fts5tok_cursor_t */
  int tflags,         /* Mask of FTS5_TOKEN_* flags */
  const char *token, /* Pointer to buffer containing token */
  int n_token,         /* Size of token in bytes */
  int i_start,         /* Byte offset of token within input text */
  int i_end            /* Byte offset of end of token within input text */
){
  fts5tok_cursor_t *csr = (fts5tok_cursor_t*)ctx;
  fts5tok_row_t *row;

  if( (csr->n_row & (csr->n_row-1))==0 ){
    int n_new = csr->n_row ? csr->n_row*2 : 32;
    fts5tok_row_t *news;
    news = (fts5tok_row_t*)dbsql_realloc64(csr->rows, n_new*sizeof(fts5tok_row_t));
    if( news==0 ) return DBSQL_NOMEM;
    memset(&news[csr->n_row], 0, sizeof(fts5tok_row_t)*(n_new-csr->n_row));
    csr->rows = news;
  }

  row = &csr->rows[csr->n_row];
  row->i_start = i_start;
  row->i_end = i_end;
  if( csr->n_row ){
    row->i_pos = row[-1].i_pos + ((tflags & FTS5_TOKEN_COLOCATED) ? 0 : 1);
  }
  row->token = dbsql_malloc64((int64_t)n_token+1);
  if( row->token==0 ) return DBSQL_NOMEM;
  memcpy(row->token, token, n_token);
  row->token[n_token] = 0;
  csr->n_row++;

  return DBSQL_OK;
}

/*
** filter - Initialize a cursor to point at the start of its data.
*/
static int __fts5tok_filter_method(
  dbsql_vtab_cursor_t *cursor,   /* The cursor used for this query */
  int idx_num,                     /* Strategy index */
  const char *idx_str,             /* Unused */
  int n_val,                       /* Number of elements in vals */
  dbsql_value_t **vals           /* Arguments for the indexing scheme */
){
  int rc = DBSQL_ERROR;
  fts5tok_cursor_t *csr = (fts5tok_cursor_t *)cursor;
  fts5tok_table_t *tab = (fts5tok_table_t *)(cursor->vtab);

  __fts5tok_reset_cursor(csr);
  if( idx_num==1 ){
    const char *byte = (const char *)dbsql_value_text(vals[0]);
    int64_t n_byte = dbsql_value_bytes(vals[0]);
    csr->input = dbsql_malloc64(n_byte+1);
    if( csr->input==0 ){
      rc = DBSQL_NOMEM;
    }else{
      if( n_byte>0 ) memcpy(csr->input, byte, n_byte);
      csr->input[n_byte] = 0;
      rc = tab->tok.tokenize(
          tab->p_tok, (void*)csr, 0, byte, n_byte, __fts5tok_cb
      );
    }
  }

  if( rc!=DBSQL_OK ) return rc;
  return __fts5tok_next_method(cursor);
}

/*
** eof - Return true if the cursor is at EOF, or false otherwise.
*/
static int __fts5tok_eof_method(dbsql_vtab_cursor_t *cursor){
  fts5tok_cursor_t *csr = (fts5tok_cursor_t *)cursor;
  return (csr->i_rowid>csr->n_row);
}

/*
** column - Return a column value.
*/
static int __fts5tok_column_method(
  dbsql_vtab_cursor_t *cursor,   /* Cursor to retrieve value from */
  dbsql_func_t *ctx,          /* Context for dbsql_result_xxx() calls */
  int i_col                        /* index_t of column to read value from */
){
  fts5tok_cursor_t *csr = (fts5tok_cursor_t *)cursor;
  fts5tok_row_t *row = &csr->rows[csr->i_rowid-1];

  /* CREATE TABLE x(input, token, start, end, position) */
  switch( i_col ){
    case 0:
      dbsql_result_text(ctx, csr->input, -1, DBSQL_TRANSIENT);
      break;
    case 1:
      dbsql_result_text(ctx, row->token, -1, DBSQL_TRANSIENT);
      break;
    case 2:
      dbsql_result_int(ctx, row->i_start);
      break;
    case 3:
      dbsql_result_int(ctx, row->i_end);
      break;
    default:
      assert( i_col==4 );
      dbsql_result_int(ctx, row->i_pos);
      break;
  }
  return DBSQL_OK;
}

/*
** rowid - Return the current rowid for the cursor.
*/
static int __fts5tok_rowid_method(
  dbsql_vtab_cursor_t *cursor,   /* Cursor to retrieve value from */
  int64_t *rowid            /* OUT: Rowid value */
){
  fts5tok_cursor_t *csr = (fts5tok_cursor_t *)cursor;
  *rowid = (int64_t)csr->i_rowid;
  return DBSQL_OK;
}

/*
** Register the fts5tok module with database connection db. Return DBSQL_OK
** if successful or an error code if dbsql_create_module() fails.
*/
int __fts5_test_register_tok(DBSQL *db, fts5_api *api){
  static const dbsql_module_t fts5tok_module = {
     0,                           /* i_version      */
     __fts5tok_connect_method,        /* create       */
     __fts5tok_connect_method,        /* connect      */
     __fts5tok_best_index_method,      /* best_index    */
     __fts5tok_disconnect_method,     /* disconnect   */
     __fts5tok_disconnect_method,     /* destroy      */
     __fts5tok_open_method,           /* x_open         */
     __fts5tok_close_method,          /* x_close        */
     __fts5tok_filter_method,         /* filter       */
     __fts5tok_next_method,           /* next         */
     __fts5tok_eof_method,            /* eof          */
     __fts5tok_column_method,         /* column       */
     __fts5tok_rowid_method,          /* rowid        */
     0,                           /* update       */
     0,                           /* begin        */
     0,                           /* x_sync         */
     0,                           /* commit       */
     0,                           /* rollback     */
     0,                           /* find_function */
     0,                           /* rename       */
     0,                           /* savepoint    */
     0,                           /* release      */
     0,                           /* rollback_to   */
     0,                           /* shadow_name   */
     0                            /* integrity    */
  };
  int rc;                         /* Return code */

  rc = dbsql_create_module(db, "fts5tokenize", &fts5tok_module, (void*)api);
  return rc;
}

#endif /* defined(DBSQL_TEST) && defined(DBSQL_ENABLE_FTS5) */
