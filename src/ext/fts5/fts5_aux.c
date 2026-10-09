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
#include <math.h>                 /* amalgamator: keep */

/*
** Object used to iterate through all "coalesced phrase instances" in 
** a single column of the current row. If the phrase instances in the
** column being considered do not overlap, this object simply iterates
** through them. Or, if they do overlap (share one or more tokens in
** common), each set of overlapping instances is treated as a single
** match. See documentation for the highlight() auxiliary function for
** details.
**
** Usage is:
**
**   for(rc = __fts5_c_inst_iter_next(api, fts, i_col, &iter);
**      (rc==DBSQL_OK && 0==fts5_c_inst_iter_eof(&iter);
**      rc = __fts5_c_inst_iter_next(&iter)
**   ){
**     printf("instance starts at %d, ends at %d\n", iter.i_start, iter.i_end);
**   }
**
*/
typedef struct c_inst_iter_t c_inst_iter_t;
struct c_inst_iter_t {
  const fts5_extension_api_t *api;   /* API offered by current FTS version */
  fts5_context_t *fts;              /* First arg to pass to api functions */
  int i_col;                       /* column_t to search */
  int i_inst;                      /* Next phrase instance index */
  int n_inst;                      /* Total number of phrase instances */

  /* Output variables */
  int i_start;                     /* First token in coalesced phrase instance */
  int i_end;                       /* Last token in coalesced phrase instance */
};

/*
** Advance the iterator to the next coalesced phrase instance. Return
** an dbsql error code if an error occurs, or DBSQL_OK otherwise.
*/
static int __fts5_c_inst_iter_next(c_inst_iter_t *iter){
  int rc = DBSQL_OK;
  iter->i_start = -1;
  iter->i_end = -1;

  while( rc==DBSQL_OK && iter->i_inst<iter->n_inst ){
    int ip; int ic; int io;
    rc = iter->api->inst(iter->fts, iter->i_inst, &ip, &ic, &io);
    if( rc==DBSQL_OK ){
      if( ic==iter->i_col ){
        int i_end = io - 1 + iter->api->phrase_size(iter->fts, ip);
        if( iter->i_start<0 ){
          iter->i_start = io;
          iter->i_end = i_end;
        }else if( io<=iter->i_end ){
          if( i_end>iter->i_end ) iter->i_end = i_end;
        }else{
          break;
        }
      }
      iter->i_inst++;
    }
  }

  return rc;
}

/*
** Initialize the iterator object indicated by the final parameter to 
** iterate through coalesced phrase instances in column i_col.
*/
static int __fts5_c_inst_iter_init(
  const fts5_extension_api_t *api,
  fts5_context_t *fts,
  int i_col,
  c_inst_iter_t *iter
){
  int rc;

  memset(iter, 0, sizeof(c_inst_iter_t));
  iter->api = api;
  iter->fts = fts;
  iter->i_col = i_col;
  rc = api->inst_count(fts, &iter->n_inst);

  if( rc==DBSQL_OK ){
    rc = __fts5_c_inst_iter_next(iter);
  }

  return rc;
}



/*************************************************************************
** Start of highlight() implementation.
*/
typedef struct highlight_context_t highlight_context_t;
struct highlight_context_t {
  /* Constant parameters to __fts5_highlight_cb() */
  int i_range_start;                /* First token to include */
  int i_range_end;                  /* If non-zero, last token to include */
  const char *z_open;              /* Opening highlight */
  const char *z_close;             /* Closing highlight */
  const char *z_in;                /* Input text */
  int n_in;                        /* Size of input text in bytes */

  /* Variables modified by __fts5_highlight_cb() */
  c_inst_iter_t iter;                 /* Coalesced Instance Iterator */
  int i_pos;                       /* Current token offset in in[] */
  int i_off;                       /* Have copied up to this offset in in[] */
  int b_open;                      /* True if highlight is open */
  char *out;                     /* Output value */
};

/*
** Append text to the highlight_context_t output string - p->out. Argument
** z points to a buffer containing n bytes of text to append. If n is 
** negative, everything up until the first '\0' is appended to the output.
**
** If *rc is set to any value other than DBSQL_OK when this function is 
** called, it is a no-op. If an error (i.e. an OOM condition) is encountered, 
** *rc is set to an error code before returning. 
*/
static void __fts5_highlight_append(
  int *rc, 
  highlight_context_t *p, 
  const char *z, int n
){
  if( *rc==DBSQL_OK && z ){
    if( n<0 ) n = (int)strlen(z);
    p->out = dbsql_mprintf("%z%.*s", p->out, n, z);
    if( p->out==0 ) *rc = DBSQL_NOMEM;
  }
}

/*
** Tokenizer callback used by implementation of highlight() function.
*/
static int __fts5_highlight_cb(
  void *context,                 /* Pointer to highlight_context_t object */
  int tflags,                     /* Mask of FTS5_TOKEN_* flags */
  const char *token,             /* Buffer containing token */
  int n_token,                     /* Size of token in bytes */
  int i_start_off,                  /* Start byte offset of token */
  int i_end_off                     /* End byte offset of token */
){
  highlight_context_t *p = (highlight_context_t*)context;
  int rc = DBSQL_OK;
  int i_pos;

  UNUSED_PARAM2(token, n_token);

  if( tflags & FTS5_TOKEN_COLOCATED ) return DBSQL_OK;
  i_pos = p->i_pos++;

  if( p->i_range_end>=0 ){
    if( i_pos<p->i_range_start || i_pos>p->i_range_end ) return DBSQL_OK;
    if( p->i_range_start && i_pos==p->i_range_start ) p->i_off = i_start_off;
  }

  /* If the parenthesis is open, and this token is not part of the current
  ** phrase, and the starting byte offset of this token is past the point
  ** that has currently been copied into the output buffer, close the
  ** parenthesis. */
  if( p->b_open 
   && (i_pos<=p->iter.i_start || p->iter.i_start<0)
   && i_start_off>p->i_off 
  ){
    __fts5_highlight_append(&rc, p, p->z_close, -1);
    p->b_open = 0;
  }

  /* If this is the start of a new phrase, and the highlight is not open:
  **
  **   * copy text from the input up to the start of the phrase, and
  **   * open the highlight.
  */
  if( i_pos==p->iter.i_start && p->b_open==0 ){
    __fts5_highlight_append(&rc, p, &p->z_in[p->i_off], i_start_off - p->i_off);
    __fts5_highlight_append(&rc, p, p->z_open, -1);
    p->i_off = i_start_off;
    p->b_open = 1;
  }

  if( i_pos==p->iter.i_end ){
    if( p->b_open==0 ){
      assert( p->i_range_end>=0 );
      __fts5_highlight_append(&rc, p, p->z_open, -1);
      p->b_open = 1;
    }
    __fts5_highlight_append(&rc, p, &p->z_in[p->i_off], i_end_off - p->i_off);
    p->i_off = i_end_off;

    if( rc==DBSQL_OK ){
      rc = __fts5_c_inst_iter_next(&p->iter);
    }
  }

  if( i_pos==p->i_range_end ){
    if( p->b_open ){
      if( p->iter.i_start>=0 && i_pos>=p->iter.i_start ){
        __fts5_highlight_append(&rc, p, &p->z_in[p->i_off], i_end_off - p->i_off);
        p->i_off = i_end_off;
      }
      __fts5_highlight_append(&rc, p, p->z_close, -1);
      p->b_open = 0;
    }
    __fts5_highlight_append(&rc, p, &p->z_in[p->i_off], i_end_off - p->i_off);
    p->i_off = i_end_off;
  }

  return rc;
}


/*
** Implementation of highlight() function.
*/
static void __fts5_highlight_function(
  const fts5_extension_api_t *api,   /* API offered by current FTS version */
  fts5_context_t *fts,              /* First arg to pass to api functions */
  dbsql_func_t *p_ctx,          /* Context for returning result/error */
  int n_val,                       /* Number of values in vals[] array */
  dbsql_value_t **vals           /* Array of trailing arguments */
){
  highlight_context_t ctx;
  int rc;
  int i_col;

  if( n_val!=3 ){
    const char *err = "wrong number of arguments to function highlight()";
    dbsql_result_error(p_ctx, err, -1);
    return;
  }

  i_col = dbsql_value_int(vals[0]);
  memset(&ctx, 0, sizeof(highlight_context_t));
  ctx.z_open = (const char*)dbsql_value_text(vals[1]);
  ctx.z_close = (const char*)dbsql_value_text(vals[2]);
  ctx.i_range_end = -1;
  rc = api->column_text(fts, i_col, &ctx.z_in, &ctx.n_in);
  if( rc==DBSQL_RANGE ){
    dbsql_result_text(p_ctx, "", -1, DBSQL_STATIC);
    rc = DBSQL_OK;
  }else if( ctx.z_in ){
    const char *loc = 0;         /* Locale of column i_col */
    int n_loc = 0;                 /* Size of loc in bytes */
    if( rc==DBSQL_OK ){
      rc = __fts5_c_inst_iter_init(api, fts, i_col, &ctx.iter);
    }

    if( rc==DBSQL_OK ){
      rc = api->column_locale(fts, i_col, &loc, &n_loc);
    }
    if( rc==DBSQL_OK ){
      rc = api->tokenize_v2(
          fts, ctx.z_in, ctx.n_in, loc, n_loc, (void*)&ctx, __fts5_highlight_cb
      );
    }
    if( ctx.b_open ){
      __fts5_highlight_append(&rc, &ctx, ctx.z_close, -1);
    }
    __fts5_highlight_append(&rc, &ctx, &ctx.z_in[ctx.i_off], ctx.n_in - ctx.i_off);

    if( rc==DBSQL_OK ){
      dbsql_result_text(p_ctx, (const char*)ctx.out, -1, DBSQL_TRANSIENT);
    }
    dbsql_free(ctx.out);
  }
  if( rc!=DBSQL_OK ){
    dbsql_result_error_code(p_ctx, rc);
  }
}
/*
** End of highlight() implementation.
**************************************************************************/

/*
** Context object passed to the __fts5_sentence_finder_cb() function.
*/
typedef struct fts5_s_finder_t fts5_s_finder_t;
struct fts5_s_finder_t {
  int i_pos;                       /* Current token position */
  int n_first_alloc;                /* Allocated size of firsts[] */
  int n_first;                     /* Number of entries in firsts[] */
  int *firsts;                    /* Array of first token in each sentence */
  const char *doc;               /* Document being tokenized */
};

/*
** Add an entry to the fts5_s_finder_t.firsts[] array. Grow the array if
** necessary. Return DBSQL_OK if successful, or DBSQL_NOMEM if an
** error occurs.
*/
static int __fts5_sentence_finder_add(fts5_s_finder_t *p, int i_add){
  if( p->n_first_alloc==p->n_first ){
    int n_new = p->n_first_alloc ? p->n_first_alloc*2 : 64;
    int *news;

    news = (int*)dbsql_realloc64(p->firsts, n_new*sizeof(int));
    if( news==0 ) return DBSQL_NOMEM;
    p->firsts = news;
    p->n_first_alloc = n_new;
  }
  p->firsts[p->n_first++] = i_add;
  return DBSQL_OK;
}

/*
** This function is an tokenize() callback used by the auxiliary snippet()
** function. Its job is to identify tokens that are the first in a sentence.
** For each such token, an entry is added to the SFinder.firsts[] array.
*/
static int __fts5_sentence_finder_cb(
  void *context,                 /* Pointer to highlight_context_t object */
  int tflags,                     /* Mask of FTS5_TOKEN_* flags */
  const char *token,             /* Buffer containing token */
  int n_token,                     /* Size of token in bytes */
  int i_start_off,                  /* Start offset of token */
  int i_end_off                     /* End offset of token */
){
  int rc = DBSQL_OK;

  UNUSED_PARAM2(token, n_token);
  UNUSED_PARAM(i_end_off);

  if( (tflags & FTS5_TOKEN_COLOCATED)==0 ){
    fts5_s_finder_t *p = (fts5_s_finder_t*)context;
    if( p->i_pos>0 ){
      int i;
      char c = 0;
      for(i=i_start_off-1; i>=0; i--){
        c = p->doc[i];
        if( c!=' ' && c!='\t' && c!='\n' && c!='\r' ) break;
      }
      if( i!=i_start_off-1 && (c=='.' || c==':') ){
        rc = __fts5_sentence_finder_add(p, p->i_pos);
      }
    }else{
      rc = __fts5_sentence_finder_add(p, 0);
    }
    p->i_pos++;
  }
  return rc;
}

static int __fts5_snippet_score(
  const fts5_extension_api_t *api,   /* API offered by current FTS version */
  fts5_context_t *fts,              /* First arg to pass to api functions */
  int n_docsize,                   /* Size of column in tokens */
  unsigned char *seens,           /* Array with one element per query phrase */
  int i_col,                       /* column_t to score */
  int i_pos,                       /* Starting offset to score */
  int n_token,                     /* Max tokens per snippet */
  int *pn_score,                   /* OUT: Score */
  int *pi_pos                      /* OUT: Adjusted offset */
){
  int rc;
  int i;
  int ip = 0;
  int ic = 0;
  int i_off = 0;
  int i_first = -1;
  int n_inst;
  int n_score = 0;
  int i_last = 0;
  int64_t i_end = (int64_t)i_pos + n_token;

  rc = api->inst_count(fts, &n_inst);
  for(i=0; i<n_inst && rc==DBSQL_OK; i++){
    rc = api->inst(fts, i, &ip, &ic, &i_off);
    if( rc==DBSQL_OK && ic==i_col && i_off>=i_pos && i_off<i_end ){
      n_score += (seens[ip] ? 1 : 1000);
      seens[ip] = 1;
      if( i_first<0 ) i_first = i_off;
      i_last = i_off + api->phrase_size(fts, ip);
    }
  }

  *pn_score = n_score;
  if( pi_pos ){
    int64_t i_adj = i_first - (n_token - (i_last-i_first)) / 2;
    if( (i_adj+n_token)>n_docsize ) i_adj = n_docsize - n_token;
    if( i_adj<0 ) i_adj = 0;
    *pi_pos = (int)i_adj;
  }

  return rc;
}

/*
** Return the value in val interpreted as utf-8 text. Except, if val 
** contains a NULL value, return a pointer to a static string zero
** bytes in length instead of a NULL pointer.
*/
static const char *__fts5_value_to_text(dbsql_value_t *val){
  const char *ret = (const char*)dbsql_value_text(val);
  return ret ? ret : "";
}

/*
** Implementation of snippet() function.
*/
static void __fts5_snippet_function(
  const fts5_extension_api_t *api,   /* API offered by current FTS version */
  fts5_context_t *fts,              /* First arg to pass to api functions */
  dbsql_func_t *p_ctx,          /* Context for returning result/error */
  int n_val,                       /* Number of values in vals[] array */
  dbsql_value_t **vals           /* Array of trailing arguments */
){
  highlight_context_t ctx;
  int rc = DBSQL_OK;             /* Return code */
  int i_col;                       /* 1st argument to snippet() */
  const char *ellips;            /* 4th argument to snippet() */
  int64_t n_token;                     /* 5th argument to snippet() */
  int n_inst = 0;                  /* Number of instance matches this row */
  int i;                          /* Used to iterate through instances */
  int n_phrase;                    /* Number of phrases in query */
  unsigned char *seens;           /* Array of "seen instance" flags */
  int i_best_col;                   /* column_t containing best snippet */
  int i_best_start = 0;             /* First token of best snippet */
  int n_best_score = 0;             /* Score of best snippet */
  int n_col_size = 0;               /* Total size of i_best_col in tokens */
  fts5_s_finder_t s_finder;            /* Used to find the beginnings of sentences */
  int n_col;

  if( n_val!=5 ){
    const char *err = "wrong number of arguments to function snippet()";
    dbsql_result_error(p_ctx, err, -1);
    return;
  }

  n_col = api->column_count(fts);
  memset(&ctx, 0, sizeof(highlight_context_t));
  i_col = dbsql_value_int(vals[0]);
  ctx.z_open = __fts5_value_to_text(vals[1]);
  ctx.z_close = __fts5_value_to_text(vals[2]);
  ctx.i_range_end = -1;
  ellips = __fts5_value_to_text(vals[3]);
  n_token = (int)(MIN( MAX(dbsql_value_int64(vals[4]), 0), 64));

  i_best_col = (i_col>=0 ? i_col : 0);
  n_phrase = api->phrase_count(fts);
  seens = dbsql_malloc64(n_phrase);
  if( seens==0 ){
    rc = DBSQL_NOMEM;
  }
  if( rc==DBSQL_OK ){
    rc = api->inst_count(fts, &n_inst);
  }

  memset(&s_finder, 0, sizeof(fts5_s_finder_t));
  for(i=0; i<n_col; i++){
    if( i_col<0 || i_col==i ){
      const char *loc = 0;       /* Locale of column i_col */
      int n_loc = 0;               /* Size of loc in bytes */
      int n_doc;
      int n_docsize;
      int ii;
      s_finder.i_pos = 0;
      s_finder.n_first = 0;
      rc = api->column_text(fts, i, &s_finder.doc, &n_doc);
      if( rc!=DBSQL_OK ) break;
      rc = api->column_locale(fts, i, &loc, &n_loc);
      if( rc!=DBSQL_OK ) break;
      rc = api->tokenize_v2(fts, 
          s_finder.doc, n_doc, loc, n_loc, (void*)&s_finder, __fts5_sentence_finder_cb
      );
      if( rc!=DBSQL_OK ) break;
      rc = api->column_size(fts, i, &n_docsize);
      if( rc!=DBSQL_OK ) break;

      for(ii=0; rc==DBSQL_OK && ii<n_inst; ii++){
        int ip, ic, io;
        int i_adj;
        int n_score;
        int jj;

        rc = api->inst(fts, ii, &ip, &ic, &io);
        if( ic!=i ) continue;
        if( io>n_docsize ) rc = FTS5_CORRUPT;
        if( rc!=DBSQL_OK ) continue;
        memset(seens, 0, n_phrase);
        rc = __fts5_snippet_score(api, fts, n_docsize, seens, i,
            io, n_token, &n_score, &i_adj
        );
        if( rc==DBSQL_OK && n_score>n_best_score ){
          n_best_score = n_score;
          i_best_col = i;
          i_best_start = i_adj;
          n_col_size = n_docsize;
        }

        if( rc==DBSQL_OK && s_finder.n_first && n_docsize>n_token ){
          for(jj=0; jj<(s_finder.n_first-1); jj++){
            if( s_finder.firsts[jj+1]>io ) break;
          }

          if( s_finder.firsts[jj]<io ){
            memset(seens, 0, n_phrase);
            rc = __fts5_snippet_score(api, fts, n_docsize, seens, i, 
              s_finder.firsts[jj], n_token, &n_score, 0
            );

            n_score += (s_finder.firsts[jj]==0 ? 120 : 100);
            if( rc==DBSQL_OK && n_score>n_best_score ){
              n_best_score = n_score;
              i_best_col = i;
              i_best_start = s_finder.firsts[jj];
              n_col_size = n_docsize;
            }
          }
        }
      }
    }
  }

  if( rc==DBSQL_OK ){
    rc = api->column_text(fts, i_best_col, &ctx.z_in, &ctx.n_in);
  }
  if( rc==DBSQL_OK && n_col_size==0 ){
    rc = api->column_size(fts, i_best_col, &n_col_size);
  }
  if( ctx.z_in ){
    const char *loc = 0;         /* Locale of column i_best_col */
    int n_loc = 0;                 /* Bytes in loc */

    if( rc==DBSQL_OK ){
      rc = __fts5_c_inst_iter_init(api, fts, i_best_col, &ctx.iter);
    }

    ctx.i_range_start = i_best_start;
    ctx.i_range_end = i_best_start + n_token - 1;

    if( i_best_start>0 ){
      __fts5_highlight_append(&rc, &ctx, ellips, -1);
    }

    /* Advance iterator ctx.iter so that it points to the first coalesced
    ** phrase instance at or following position i_best_start. */
    while( ctx.iter.i_start>=0 && ctx.iter.i_start<i_best_start && rc==DBSQL_OK ){
      rc = __fts5_c_inst_iter_next(&ctx.iter);
    }

    if( rc==DBSQL_OK ){
      rc = api->column_locale(fts, i_best_col, &loc, &n_loc);
    }
    if( rc==DBSQL_OK ){
      rc = api->tokenize_v2(
          fts, ctx.z_in, ctx.n_in, loc, n_loc, (void*)&ctx,__fts5_highlight_cb
      );
    }
    if( ctx.b_open ){
      __fts5_highlight_append(&rc, &ctx, ctx.z_close, -1);
    }
    if( ctx.i_range_end>=(n_col_size-1) ){
      __fts5_highlight_append(&rc, &ctx, &ctx.z_in[ctx.i_off], ctx.n_in - ctx.i_off);
    }else{
      __fts5_highlight_append(&rc, &ctx, ellips, -1);
    }
  }
  if( rc==DBSQL_OK ){
    dbsql_result_text(p_ctx, (const char*)ctx.out, -1, DBSQL_TRANSIENT);
  }else{
    dbsql_result_error_code(p_ctx, rc);
  }
  dbsql_free(ctx.out);
  dbsql_free(seens);
  dbsql_free(s_finder.firsts);
}

/************************************************************************/

/*
** The first time the bm25() function is called for a query, an instance
** of the following structure is allocated and populated.
*/
typedef struct fts5_bm25_data_t fts5_bm25_data_t;
struct fts5_bm25_data_t {
  int n_phrase;                    /* Number of phrases in query */
  double avgdl;                   /* Average number of tokens in each row */
  double *idfs;                   /* IDF for each phrase */
  double *freqs;                  /* Array used to calculate phrase freq. */
};

/*
** Callback used by __fts5_bm25_get_data() to count the number of rows in the
** table matched by each individual phrase within the query.
*/
static int __fts5_count_cb(
  const fts5_extension_api_t *api, 
  fts5_context_t *fts,
  void *user_data                 /* Pointer to int64_t variable */
){
  int64_t *pn = (int64_t*)user_data;
  UNUSED_PARAM2(api, fts);
  (*pn)++;
  return DBSQL_OK;
}

/*
** Set *data to point to the fts5_bm25_data_t object for the current query. 
** If the object has not already been allocated, allocate and populate it
** now.
*/
static int __fts5_bm25_get_data(
  const fts5_extension_api_t *api, 
  fts5_context_t *fts,
  fts5_bm25_data_t **data           /* OUT: bm25-data object for this query */
){
  int rc = DBSQL_OK;             /* Return code */
  fts5_bm25_data_t *p;                /* Object to return */

  p = (fts5_bm25_data_t*)api->get_auxdata(fts, 0);
  if( p==0 ){
    int n_phrase;                  /* Number of phrases in query */
    int64_t n_row = 0;       /* Number of rows in table */
    int64_t n_token = 0;     /* Number of tokens in table */
    int64_t n_byte;          /* Bytes of space to allocate */
    int i;

    /* Allocate the fts5_bm25_data_t object */
    n_phrase = api->phrase_count(fts);
    n_byte = sizeof(fts5_bm25_data_t) + n_phrase*2*sizeof(double);
    p = (fts5_bm25_data_t*)dbsql_malloc64(n_byte);
    if( p==0 ){
      rc = DBSQL_NOMEM;
    }else{
      memset(p, 0, (size_t)n_byte);
      p->n_phrase = n_phrase;
      p->idfs = (double*)&p[1];
      p->freqs = &p->idfs[n_phrase];
    }

    /* Calculate the average document length for this FTS5 table */
    if( rc==DBSQL_OK ) rc = api->row_count(fts, &n_row);
    assert( rc!=DBSQL_OK || n_row>0 );
    if( rc==DBSQL_OK ) rc = api->column_total_size(fts, -1, &n_token);
    if( rc==DBSQL_OK ) p->avgdl = (double)n_token  / (double)n_row;

    /* Calculate an IDF for each phrase in the query */
    for(i=0; rc==DBSQL_OK && i<n_phrase; i++){
      int64_t n_hit = 0;
      rc = api->query_phrase(fts, i, (void*)&n_hit, __fts5_count_cb);
      if( rc==DBSQL_OK ){
        /* Calculate the IDF (Inverse Document Frequency) for phrase i.
        ** This is done using the standard BM25 formula as found on wikipedia:
        **
        **   IDF = log( (N - n_hit + 0.5) / (n_hit + 0.5) )
        **
        ** where "N" is the total number of documents in the set and n_hit
        ** is the number that contain at least one instance of the phrase
        ** under consideration.
        **
        ** The problem with this is that if (N < 2*n_hit), the IDF is 
        ** negative. Which is undesirable. So the minimum allowable IDF is
        ** (1e-6) - roughly the same as a term that appears in just over
        ** half of set of 5,000,000 documents.  */
        double idf = log( (n_row - n_hit + 0.5) / (n_hit + 0.5) );
        if( idf<=0.0 ) idf = 1e-6;
        p->idfs[i] = idf;
      }
    }

    if( rc!=DBSQL_OK ){
      dbsql_free(p);
    }else{
      rc = api->set_auxdata(fts, p, dbsql_free);
    }
    if( rc!=DBSQL_OK ) p = 0;
  }
  *data = p;
  return rc;
}

/*
** Implementation of bm25() function.
*/
static void fts5Bm25Function(
  const fts5_extension_api_t *api,   /* API offered by current FTS version */
  fts5_context_t *fts,              /* First arg to pass to api functions */
  dbsql_func_t *ctx,          /* Context for returning result/error */
  int n_val,                       /* Number of values in vals[] array */
  dbsql_value_t **vals           /* Array of trailing arguments */
){
  const double k1 = 1.2;          /* Constant "k1" from BM25 formula */
  const double b = 0.75;          /* Constant "b" from BM25 formula */
  int rc;                         /* Error code */
  double score = 0.0;             /* SQL function return value */
  fts5_bm25_data_t *data;            /* Values allocated/calculated once only */
  int i;                          /* Iterator variable */
  int n_inst = 0;                  /* Value returned by inst_count() */
  double D = 0.0;                 /* Total number of tokens in row */
  double *freqs = 0;              /* Array of phrase freq. for current row */

  /* Calculate the phrase frequency (symbol "f(qi,D)" in the documentation)
  ** for each phrase in the query for the current row. */
  rc = __fts5_bm25_get_data(api, fts, &data);
  if( rc==DBSQL_OK ){
    freqs = data->freqs;
    memset(freqs, 0, sizeof(double) * data->n_phrase);
    rc = api->inst_count(fts, &n_inst);
  }
  for(i=0; rc==DBSQL_OK && i<n_inst; i++){
    int ip; int ic; int io;
    rc = api->inst(fts, i, &ip, &ic, &io);
    if( rc==DBSQL_OK ){
      double w = (n_val > ic) ? dbsql_value_double(vals[ic]) : 1.0;
      freqs[ip] += w;
    }
  }

  /* Figure out the total size of the current row in tokens. */
  if( rc==DBSQL_OK ){
    int n_tok;
    rc = api->column_size(fts, -1, &n_tok);
    D = (double)n_tok;
  }

  /* Determine and return the BM25 score for the current row. Or, if an
  ** error has occurred, throw an exception. */
  if( rc==DBSQL_OK ){
    for(i=0; i<data->n_phrase; i++){
      score += data->idfs[i] * (
          ( freqs[i] * (k1 + 1.0) ) / 
          ( freqs[i] + k1 * (1 - b + b * D / data->avgdl) )
      );
    }
    dbsql_result_double(ctx, -1.0 * score);
  }else{
    dbsql_result_error_code(ctx, rc);
  }
}

/*
** Implementation of fts5_get_locale() function.
*/
static void __fts5_get_locale_function(
  const fts5_extension_api_t *api,   /* API offered by current FTS version */
  fts5_context_t *fts,              /* First arg to pass to api functions */
  dbsql_func_t *ctx,          /* Context for returning result/error */
  int n_val,                       /* Number of values in vals[] array */
  dbsql_value_t **vals           /* Array of trailing arguments */
){
  int i_col = 0;
  int type = 0;
  int rc = DBSQL_OK;
  const char *locale = 0;
  int n_locale = 0;

  /* column_locale() must be available */
  assert( api->i_version>=4 );

  if( n_val!=1 ){
    const char *z = "wrong number of arguments to function fts5_get_locale()";
    dbsql_result_error(ctx, z, -1);
    return;
  }

  type = dbsql_value_numeric_type(vals[0]);
  if( type!=DBSQL_INTEGER ){
    const char *z = "non-integer argument passed to function fts5_get_locale()";
    dbsql_result_error(ctx, z, -1);
    return;
  }

  i_col = dbsql_value_int(vals[0]);
  if( i_col<0 || i_col>=api->column_count(fts) ){
    dbsql_result_error_code(ctx, DBSQL_RANGE);
    return;
  }

  rc = api->column_locale(fts, i_col, &locale, &n_locale);
  if( rc!=DBSQL_OK ){
    dbsql_result_error_code(ctx, rc);
    return;
  }

  dbsql_result_text(ctx, locale, n_locale, DBSQL_TRANSIENT);
}

int __fts5_aux_init(fts5_api *api){
  struct builtin_t {
    const char *z_func;            /* Function name (nul-terminated) */
    void *user_data;              /* User-data pointer */
    fts5_extension_function func;/* Callback function */
    void (*destroy)(void*);      /* Destructor function */
  } builtins [] = {
    { "snippet",         0, __fts5_snippet_function,   0 },
    { "highlight",       0, __fts5_highlight_function, 0 },
    { "bm25",            0, fts5Bm25Function,      0 },
    { "fts5_get_locale", 0, __fts5_get_locale_function, 0 },
  };
  int rc = DBSQL_OK;             /* Return code */
  int i;                          /* To iterate through builtin functions */

  for(i=0; rc==DBSQL_OK && i<ARRAY_SIZE(builtins); i++){
    rc = api->create_function(api,
        builtins[i].z_func,
        builtins[i].user_data,
        builtins[i].func,
        builtins[i].destroy
    );
  }

  return rc;
}
