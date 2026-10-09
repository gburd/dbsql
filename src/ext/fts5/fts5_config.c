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

#define FTS5_DEFAULT_PAGE_SIZE   4050
#define FTS5_DEFAULT_AUTOMERGE      4
#define FTS5_DEFAULT_USERMERGE      4
#define FTS5_DEFAULT_CRISISMERGE   16
#define FTS5_DEFAULT_HASHSIZE    (1024*1024)

#define FTS5_DEFAULT_DELETE_AUTOMERGE 10      /* default 10% */

/* Maximum allowed page size */
#define FTS5_MAX_PAGE_SIZE (64*1024)

static int fts5_iswhitespace(char x){
  return (x==' ');
}

static int fts5_isopenquote(char x){
  return (x=='"' || x=='\'' || x=='[' || x=='`');
}

/*
** Argument in points to a character that is part of a nul-terminated 
** string. Return a pointer to the first character following *in in 
** the string that is not a white-space character.
*/
static const char *__fts5_config_skip_whitespace(const char *in){
  const char *p = in;
  if( p ){
    while( fts5_iswhitespace(*p) ){ p++; }
  }
  return p;
}

/*
** Argument in points to a character that is part of a nul-terminated 
** string. Return a pointer to the first character following *in in 
** the string that is not a "bareword" character.
*/
static const char *__fts5_config_skip_bareword(const char *in){
  const char *p = in;
  while ( __fts5_is_bareword(*p) ) p++;
  if( p==in ) p = 0;
  return p;
}

static int fts5_isdigit(char a){
  return (a>='0' && a<='9');
}



static const char *__fts5_config_skip_literal(const char *in){
  const char *p = in;
  switch( *p ){
    case 'n': case 'N':
      if( dbsql_strnicmp("null", p, 4)==0 ){
        p = &p[4];
      }else{
        p = 0;
      }
      break;

    case 'x': case 'X':
      p++;
      if( *p=='\'' ){
        p++;
        while( (*p>='a' && *p<='f') 
            || (*p>='A' && *p<='F') 
            || (*p>='0' && *p<='9') 
            ){
          p++;
        }
        if( *p=='\'' && 0==((p-in)%2) ){
          p++;
        }else{
          p = 0;
        }
      }else{
        p = 0;
      }
      break;

    case '\'':
      p++;
      while( p ){
        if( *p=='\'' ){
          p++;
          if( *p!='\'' ) break;
        }
        p++;
        if( *p==0 ) p = 0;
      }
      break;

    default:
      /* maybe a number */
      if( *p=='+' || *p=='-' ) p++;
      while( fts5_isdigit(*p) ) p++;

      /* At this point, if the literal was an integer, the parse is 
      ** finished. Or, if it is a floating point value, it may continue
      ** with either a decimal point or an 'E' character. */
      if( *p=='.' && fts5_isdigit(p[1]) ){
        p += 2;
        while( fts5_isdigit(*p) ) p++;
      }
      if( p==in ) p = 0;

      break;
  }

  return p;
}

/*
** The first character of the string pointed to by argument z is guaranteed
** to be an open-quote character (see function fts5_isopenquote()).
**
** This function searches for the corresponding close-quote character within
** the string and, if found, dequotes the string in place and adds a new
** nul-terminator byte.
**
** If the close-quote is found, the value returned is the byte offset of
** the character immediately following it. Or, if the close-quote is not 
** found, -1 is returned. If -1 is returned, the buffer is left in an 
** undefined state.
*/
static int fts5Dequote(char *z){
  char q;
  int i_in = 1;
  int i_out = 0;
  q = z[0];

  /* Set stack variable q to the close-quote character */
  assert( q=='[' || q=='\'' || q=='"' || q=='`' );
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
  return i_in;
}

/*
** Convert an SQL-style quoted string into a normal string by removing
** the quote characters.  The conversion is done in-place.  If the
** input does not begin with a quote character, then this routine
** is a no-op.
**
** Examples:
**
**     "abc"   becomes   abc
**     'xyz'   becomes   xyz
**     [pqr]   becomes   pqr
**     `mno`   becomes   mno
*/
void __fts5_dequote(char *z){
  char quote;                     /* Quote character (if any ) */

  assert( 0==fts5_iswhitespace(z[0]) );
  quote = z[0];
  if( quote=='[' || quote=='\'' || quote=='"' || quote=='`' ){
    fts5Dequote(z);
  }
}


struct fts5_enum_t {
  const char *name;
  int val;
};
typedef struct fts5_enum_t fts5_enum_t;

static int __fts5_config_set_enum(
  const fts5_enum_t *enums, 
  const char *z_enum, 
  int *pe_val
){
  int n_enum = (int)strlen(z_enum);
  int i;
  int i_val = -1;

  for(i=0; enums[i].name; i++){
    if( dbsql_strnicmp(enums[i].name, z_enum, n_enum)==0 ){
      if( i_val>=0 ) return DBSQL_ERROR;
      i_val = enums[i].val;
    }
  }

  *pe_val = i_val;
  return i_val<0 ? DBSQL_ERROR : DBSQL_OK;
}

/*
** parser_t a "special" CREATE VIRTUAL TABLE directive and update
** configuration object config as appropriate.
**
** If successful, object config is updated and DBSQL_OK returned. If
** an error occurs, an dbsql error code is returned and an error message
** may be left in *err_p. It is the responsibility of the caller to
** eventually free any such error message using dbsql_free().
*/
static int __fts5_config_parse_special(
  fts5_config_t *config,            /* Configuration object to update */
  const char *cmd,               /* Special command to parse */
  const char *arg,               /* Argument to parse */
  char **err_p                    /* OUT: Error message */
){
  int rc = DBSQL_OK;
  int n_cmd = (int)strlen(cmd);

  if( dbsql_strnicmp("prefix", cmd, n_cmd)==0 ){
    const int n_byte = sizeof(int) * FTS5_MAX_PREFIX_INDEXES;
    const char *p;
    int first = 1;
    if( config->__a_prefix==0 ){
      config->__a_prefix = __fts5_malloc_zero(&rc, n_byte);
      if( rc ) return rc;
    }

    p = arg;
    while( 1 ){
      int n_pre = 0;

      while( p[0]==' ' ) p++;
      if( first==0 && p[0]==',' ){
        p++;
        while( p[0]==' ' ) p++;
      }else if( p[0]=='\0' ){
        break;
      }
      if( p[0]<'0' || p[0]>'9' ){
        *err_p = dbsql_mprintf("malformed prefix=... directive");
        rc = DBSQL_ERROR;
        break;
      }

      if( config->n_prefix==FTS5_MAX_PREFIX_INDEXES ){
        *err_p = dbsql_mprintf(
            "too many prefix indexes (max %d)", FTS5_MAX_PREFIX_INDEXES
        );
        rc = DBSQL_ERROR;
        break;
      }

      while( p[0]>='0' && p[0]<='9' && n_pre<1000 ){
        n_pre = n_pre*10 + (p[0] - '0');
        p++;
      }

      if( n_pre<=0 || n_pre>=1000 ){
        *err_p = dbsql_mprintf("prefix length out of range (max 999)");
        rc = DBSQL_ERROR;
        break;
      }

      config->__a_prefix[config->n_prefix] = n_pre;
      config->n_prefix++;
      first = 0;
    }
    assert( config->n_prefix<=FTS5_MAX_PREFIX_INDEXES );
    return rc;
  }

  if( dbsql_strnicmp("tokenize", cmd, n_cmd)==0 ){
    const char *p = (const char*)arg;
    int64_t n_arg = strlen(arg) + 1;
    char **args = __fts5_malloc_zero(&rc, (sizeof(char*) + 2) * n_arg);

    if( args ){
      char *space = (char*)&args[n_arg];
      if( config->t.args ){
        *err_p = dbsql_mprintf("multiple tokenize=... directives");
        rc = DBSQL_ERROR;
      }else{
        for(n_arg=0; p && *p; n_arg++){
          const char *p2 = __fts5_config_skip_whitespace(p);
          if( *p2=='\'' ){
            p = __fts5_config_skip_literal(p2);
          }else{
            p = __fts5_config_skip_bareword(p2);
          }
          if( p ){
            memcpy(space, p2, p-p2);
            args[n_arg] = space;
            __fts5_dequote(space);
            space += (p - p2) + 1;
            p = __fts5_config_skip_whitespace(p);
          }
        }
        if( p==0 ){
          *err_p = dbsql_mprintf("parse error in tokenize directive");
          rc = DBSQL_ERROR;
        }else{
          config->t.args = (const char**)args;
          config->t.n_arg = n_arg;
          args = 0;
        }
      }
    }
    dbsql_free(args);

    return rc;
  }

  if( dbsql_strnicmp("content", cmd, n_cmd)==0 ){
    if( config->content!=FTS5_CONTENT_NORMAL ){
      *err_p = dbsql_mprintf("multiple content=... directives");
      rc = DBSQL_ERROR;
    }else{
      if( arg[0] ){
        config->content = FTS5_CONTENT_EXTERNAL;
        config->z_content = __fts5_mprintf(&rc, "%Q.%Q", config->z_db,arg);
      }else{
        config->content = FTS5_CONTENT_NONE;
      }
    }
    return rc;
  }

  if( dbsql_strnicmp("contentless_delete", cmd, n_cmd)==0 ){
    if( (arg[0]!='0' && arg[0]!='1') || arg[1]!='\0' ){
      *err_p = dbsql_mprintf("malformed contentless_delete=... directive");
      rc = DBSQL_ERROR;
    }else{
      config->contentless_delete = (arg[0]=='1');
    }
    return rc;
  }

  if( dbsql_strnicmp("contentless_unindexed", cmd, n_cmd)==0 ){
    if( (arg[0]!='0' && arg[0]!='1') || arg[1]!='\0' ){
      *err_p = dbsql_mprintf("malformed contentless_delete=... directive");
      rc = DBSQL_ERROR;
    }else{
      config->contentless_unindexed = (arg[0]=='1');
    }
    return rc;
  }

  if( dbsql_strnicmp("content_rowid", cmd, n_cmd)==0 ){
    if( config->content_rowid ){
      *err_p = dbsql_mprintf("multiple content_rowid=... directives");
      rc = DBSQL_ERROR;
    }else{
      config->content_rowid = __fts5_strndup(&rc, arg, -1);
    }
    return rc;
  }

  if( dbsql_strnicmp("columnsize", cmd, n_cmd)==0 ){
    if( (arg[0]!='0' && arg[0]!='1') || arg[1]!='\0' ){
      *err_p = dbsql_mprintf("malformed columnsize=... directive");
      rc = DBSQL_ERROR;
    }else{
      config->columnsize = (arg[0]=='1');
    }
    return rc;
  }

  if( dbsql_strnicmp("locale", cmd, n_cmd)==0 ){
    if( (arg[0]!='0' && arg[0]!='1') || arg[1]!='\0' ){
      *err_p = dbsql_mprintf("malformed locale=... directive");
      rc = DBSQL_ERROR;
    }else{
      config->locale = (arg[0]=='1');
    }
    return rc;
  }

  if( dbsql_strnicmp("detail", cmd, n_cmd)==0 ){
    const fts5_enum_t details[] = {
      { "none", FTS5_DETAIL_NONE },
      { "full", FTS5_DETAIL_FULL },
      { "columns", FTS5_DETAIL_COLUMNS },
      { 0, 0 }
    };

    if( (rc = __fts5_config_set_enum(details, arg, &config->detail)) ){
      *err_p = dbsql_mprintf("malformed detail=... directive");
    }
    return rc;
  }

  if( dbsql_strnicmp("tokendata", cmd, n_cmd)==0 ){
    if( (arg[0]!='0' && arg[0]!='1') || arg[1]!='\0' ){
      *err_p = dbsql_mprintf("malformed tokendata=... directive");
      rc = DBSQL_ERROR;
    }else{
      config->tokendata = (arg[0]=='1');
    }
    return rc;
  }

  *err_p = dbsql_mprintf("unrecognized option: \"%.*s\"", n_cmd, cmd);
  return DBSQL_ERROR;
}

/*
** Gobble up the first bareword or quoted word from the input buffer in.
** Return a pointer to the character immediately following the last in
** the gobbled word if successful, or a NULL pointer otherwise (failed
** to find close-quote character).
**
** Before returning, set out to point to a new buffer containing a
** nul-terminated, dequoted copy of the gobbled word. If the word was
** quoted, *pb_quoted is also set to 1 before returning.
**
** If *rc is other than DBSQL_OK when this function is called, it is
** a no-op (NULL is returned). Otherwise, if an OOM occurs within this
** function, *rc is set to DBSQL_NOMEM before returning. *rc is *not*
** set if a parse error (failed to find close quote) occurs.
*/
static const char *__fts5_config_gobble_word(
  int *rc,                       /* IN/OUT: Error code */
  const char *in,                /* Buffer to gobble string/bareword from */
  char **pz_out,                   /* OUT: malloc'd buffer containing str/bw */
  int *pb_quoted                   /* OUT: Set to true if dequoting required */
){
  const char *ret = 0;

  int64_t n_in = strlen(in);
  char *out = dbsql_malloc64(n_in+1);

  assert( *rc==DBSQL_OK );
  *pb_quoted = 0;
  *pz_out = 0;

  if( out==0 ){
    *rc = DBSQL_NOMEM;
  }else{
    memcpy(out, in, (size_t)(n_in+1));
    if( fts5_isopenquote(out[0]) ){
      int ii = fts5Dequote(out);
      ret = &in[ii];
      *pb_quoted = 1;
    }else{
      ret = __fts5_config_skip_bareword(in);
      if( ret ){
        out[ret-in] = '\0';
      }
    }
  }

  if( ret==0 ){
    dbsql_free(out);
  }else{
    *pz_out = out;
  }

  return ret;
}

static int __fts5_config_parse_column(
  fts5_config_t *p, 
  char *col, 
  char *arg, 
  char **err_p,
  int *pb_unindexed
){
  int rc = DBSQL_OK;
  if( 0==dbsql_stricmp(col, FTS5_RANK_NAME) 
   || 0==dbsql_stricmp(col, FTS5_ROWID_NAME) 
  ){
    *err_p = dbsql_mprintf("reserved fts5 column name: %s", col);
    rc = DBSQL_ERROR;
  }else if( arg ){
    if( 0==dbsql_stricmp(arg, "unindexed") ){
      p->ab_unindexed[p->n_col] = 1;
      *pb_unindexed = 1;
    }else{
      *err_p = dbsql_mprintf("unrecognized column option: %s", arg);
      rc = DBSQL_ERROR;
    }
  }

  p->cols[p->n_col++] = col;
  return rc;
}

/*
** Populate the fts5_config_t.content_exprlist string.
*/
static int __fts5_config_make_exprlist(fts5_config_t *p){
  int i;
  int rc = DBSQL_OK;
  fts5_buffer_t buf = {0, 0, 0};

  __fts5_buffer_append_printf(&rc, &buf, "T.%Q", p->content_rowid);
  if( p->content!=FTS5_CONTENT_NONE ){
    assert( p->content==FTS5_CONTENT_EXTERNAL
         || p->content==FTS5_CONTENT_NORMAL
         || p->content==FTS5_CONTENT_UNINDEXED
    );
    for(i=0; i<p->n_col; i++){
      if( p->content==FTS5_CONTENT_EXTERNAL ){
        __fts5_buffer_append_printf(&rc, &buf, ", T.%Q", p->cols[i]);
      }else if( p->content==FTS5_CONTENT_NORMAL || p->ab_unindexed[i] ){
        __fts5_buffer_append_printf(&rc, &buf, ", T.c%d", i);
      }else{
        __fts5_buffer_append_printf(&rc, &buf, ", NULL");
      }
    }
  }
  if( p->content==FTS5_CONTENT_NORMAL && p->locale ){
    for(i=0; i<p->n_col; i++){
      if( p->ab_unindexed[i]==0 ){
        __fts5_buffer_append_printf(&rc, &buf, ", T.l%d", i);
      }else{
        __fts5_buffer_append_printf(&rc, &buf, ", NULL");
      }
    }
  }

  assert( p->content_exprlist==0 );
  p->content_exprlist = (char*)buf.p;
  return rc;
}

/*
** Arguments n_arg/args contain the string arguments passed to the create
** or connect method of the virtual table. This function attempts to 
** allocate an instance of fts5_config_t containing the results of parsing
** those arguments.
**
** If successful, DBSQL_OK is returned and *out is set to point to the
** new fts5_config_t object. If an error occurs, an dbsql error code is 
** returned, *out is set to NULL and an error message may be left in
** *err_p. It is the responsibility of the caller to eventually free any 
** such error message using dbsql_free().
*/
int __fts5_config_parse(
  fts5_global_t *__p_global,
  DBSQL *db,
  int n_arg,                       /* Number of arguments */
  const char **args,             /* Array of n_arg CREATE VIRTUAL TABLE args */
  fts5_config_t **out,             /* OUT: Results of parse */
  char **err_p                    /* OUT: Error message */
){
  int rc = DBSQL_OK;             /* Return code */
  fts5_config_t *ret;               /* New object to return */
  int i;
  int64_t n_byte;
  int unindexed = 0;             /* True if there are one or more UNINDEXED */

  *out = ret = (fts5_config_t*)dbsql_malloc64(sizeof(fts5_config_t));
  if( ret==0 ) return DBSQL_NOMEM;
  memset(ret, 0, sizeof(fts5_config_t));
  ret->__p_global = __p_global;
  ret->db = db;
  ret->i_cookie = -1;

  n_byte = n_arg * (sizeof(char*) + sizeof(u_int8_t));
  ret->cols = (char**)__fts5_malloc_zero(&rc, n_byte);
  ret->ab_unindexed = ret->cols ? (u_int8_t*)&ret->cols[n_arg] : 0;
  ret->z_db = __fts5_strndup(&rc, args[1], -1);
  ret->name = __fts5_strndup(&rc, args[2], -1);
  ret->columnsize = 1;
  ret->detail = FTS5_DETAIL_FULL;
#ifdef DBSQL_DEBUG
  ret->prefix_index = 1;
#endif
  if( rc==DBSQL_OK && dbsql_stricmp(ret->name, FTS5_RANK_NAME)==0 ){
    *err_p = dbsql_mprintf("reserved fts5 table name: %s", ret->name);
    rc = DBSQL_ERROR;
  }

  assert( (ret->ab_unindexed && ret->cols) || rc!=DBSQL_OK );
  for(i=3; rc==DBSQL_OK && i<n_arg; i++){
    const char *orig = args[i];
    const char *z;
    char *one = 0;
    char *two = 0;
    int option = 0;
    int must_be_col = 0;

    z = __fts5_config_gobble_word(&rc, orig, &one, &must_be_col);
    z = __fts5_config_skip_whitespace(z);
    if( z && *z=='=' ){
      option = 1;
      assert( one!=0 );
      z++;
      if( must_be_col ) z = 0;
    }
    z = __fts5_config_skip_whitespace(z);
    if( z && z[0] ){
      int dummy;
      z = __fts5_config_gobble_word(&rc, z, &two, &dummy);
      if( z && z[0] ) z = 0;
    }

    if( rc==DBSQL_OK ){
      if( z==0 ){
        *err_p = dbsql_mprintf("parse error in \"%s\"", orig);
        rc = DBSQL_ERROR;
      }else{
        if( option ){
          rc = __fts5_config_parse_special(ret, 
            ALWAYS(one)?one:"",
            two?two:"",
            err_p
          );
        }else{
          rc = __fts5_config_parse_column(ret, one, two, err_p, &unindexed);
          one = 0;
        }
      }
    }

    dbsql_free(one);
    dbsql_free(two);
  }

  /* We only allow contentless_delete=1 if the table is indeed contentless. */
  if( rc==DBSQL_OK 
   && ret->contentless_delete 
   && ret->content!=FTS5_CONTENT_NONE 
  ){
    *err_p = dbsql_mprintf(
        "contentless_delete=1 requires a contentless table"
    );
    rc = DBSQL_ERROR;
  }

  /* We only allow contentless_delete=1 if columnsize=0 is not present. 
  **
  ** This restriction may be removed at some point. 
  */
  if( rc==DBSQL_OK && ret->contentless_delete && ret->columnsize==0 ){
    *err_p = dbsql_mprintf(
        "contentless_delete=1 is incompatible with columnsize=0"
    );
    rc = DBSQL_ERROR;
  }

  /* We only allow contentless_unindexed=1 if the table is actually a
  ** contentless one.
  */
  if( rc==DBSQL_OK 
   && ret->contentless_unindexed 
   && ret->content!=FTS5_CONTENT_NONE
  ){
    *err_p = dbsql_mprintf(
        "contentless_unindexed=1 requires a contentless table"
    );
    rc = DBSQL_ERROR;
  }

  /* If no content option was specified, fill in the default values. */
  if( rc==DBSQL_OK && ret->z_content==0 ){
    const char *tail = 0;
    assert( ret->content==FTS5_CONTENT_NORMAL
         || ret->content==FTS5_CONTENT_NONE
    );
    if( ret->content==FTS5_CONTENT_NORMAL ){
      tail = "content";
    }else if( unindexed && ret->contentless_unindexed ){
      ret->content = FTS5_CONTENT_UNINDEXED;
      tail = "content";
    }else if( ret->columnsize ){
      tail = "docsize";
    }

    if( tail ){
      ret->z_content = __fts5_mprintf(
          &rc, "%Q.'%q_%s'", ret->z_db, ret->name, tail
      );
    }
  }

  if( rc==DBSQL_OK && ret->content_rowid==0 ){
    ret->content_rowid = __fts5_strndup(&rc, "rowid", -1);
  }

  /* Formulate the content_exprlist text */
  if( rc==DBSQL_OK ){
    rc = __fts5_config_make_exprlist(ret);
  }

  if( rc!=DBSQL_OK ){
    __fts5_config_free(ret);
    *out = 0;
  }
  return rc;
}

/*
** Free the configuration object passed as the only argument.
*/
void __fts5_config_free(fts5_config_t *config){
  if( config ){
    int i;
    if( config->t.p_tok ){
      if( config->t.api1 ){
        config->t.api1->x_delete(config->t.p_tok);
      }else{
        config->t.api2->x_delete(config->t.p_tok);
      }
    }
    dbsql_free((char*)config->t.args);
    dbsql_free(config->z_db);
    dbsql_free(config->name);
    for(i=0; i<config->n_col; i++){
      dbsql_free(config->cols[i]);
    }
    dbsql_free(config->cols);
    dbsql_free(config->__a_prefix);
    dbsql_free(config->z_rank);
    dbsql_free(config->z_rank_args);
    dbsql_free(config->z_content);
    dbsql_free(config->content_rowid);
    dbsql_free(config->content_exprlist);
    dbsql_free(config);
  }
}

/*
** Call dbsql_declare_vtab() based on the contents of the configuration
** object passed as the only argument. Return DBSQL_OK if successful, or
** an dbsql error code if an error occurs.
*/
int __fts5_config_declare_vtab(fts5_config_t *config){
  int i;
  int rc = DBSQL_OK;
  char *sql;

  sql = __fts5_mprintf(&rc, "CREATE TABLE x(");
  for(i=0; sql && i<config->n_col; i++){
    const char *sep = (i==0?"":", ");
    sql = __fts5_mprintf(&rc, "%z%s%Q", sql, sep, config->cols[i]);
  }
  sql = __fts5_mprintf(&rc, "%z, %Q HIDDEN, %s HIDDEN)", 
      sql, config->name, FTS5_RANK_NAME
  );

  assert( sql || rc==DBSQL_NOMEM );
  if( sql ){
    rc = dbsql_declare_vtab(config->db, sql);
    dbsql_free(sql);
  }
 
  return rc;
}

/*
** Tokenize the text passed via the second and third arguments.
**
** The callback is invoked once for each token in the input text. The
** arguments passed to it are, in order:
**
**     void *ctx          // Copy of 4th argument to __fts5_tokenize()
**     const char *token  // Pointer to buffer containing token
**     int n_token          // Size of token in bytes
**     int i_start          // Byte offset of start of token within input text
**     int i_end            // Byte offset of end of token within input text
**     int i_pos            // Position of token in input (first token is 0)
**
** If the callback returns a non-zero value the tokenization is abandoned
** and no further callbacks are issued. 
**
** This function returns DBSQL_OK if successful or an dbsql error code
** if an error occurs. If the tokenization was abandoned early because
** the callback returned DBSQL_DONE, this is not an error and this function
** still returns DBSQL_OK. Or, if the tokenization was abandoned early
** because the callback returned another non-zero value, it is assumed
** to be an dbsql error code and returned to the caller.
*/
int __fts5_tokenize(
  fts5_config_t *config,            /* FTS5 Configuration object */
  int flags,                      /* FTS5_TOKENIZE_* flags */
  const char *text, int n_text,   /* Text to tokenize */
  void *ctx,                     /* Context passed to token() */
  int (*token)(void*, int, const char*, int, int, int)    /* Callback */
){
  int rc = DBSQL_OK;
  if( text ){
    if( config->t.p_tok==0 ){
      rc = __fts5_load_tokenizer(config);
    }
    if( rc==DBSQL_OK ){
      if( config->t.api1 ){
        rc = config->t.api1->tokenize(
            config->t.p_tok, ctx, flags, text, n_text, token
        );
      }else{
        rc = config->t.api2->tokenize(config->t.p_tok, ctx, flags, 
            text, n_text, config->t.locale, config->t.n_locale, token
        );
      }
    }
  }
  return rc;
}

/*
** Argument in points to the first character in what is expected to be
** a comma-separated list of SQL literals followed by a ')' character.
** If it actually is this, return a pointer to the ')'. Otherwise, return
** NULL to indicate a parse error.
*/
static const char *__fts5_config_skip_args(const char *in){
  const char *p = in;
  
  while( 1 ){
    p = __fts5_config_skip_whitespace(p);
    p = __fts5_config_skip_literal(p);
    p = __fts5_config_skip_whitespace(p);
    if( p==0 || *p==')' ) break;
    if( *p!=',' ){
      p = 0;
      break;
    }
    p++;
  }

  return p;
}

/*
** Parameter in contains a rank() function specification. The format of 
** this is:
**
**   + Bareword (function name)
**   + Open parenthesis - "("
**   + Zero or more SQL literals in a comma separated list
**   + Close parenthesis - ")"
*/
int __fts5_config_parse_rank(
  const char *in,                /* Input string */
  char **pz_rank,                  /* OUT: Rank function name */
  char **pz_rank_args               /* OUT: Rank function arguments */
){
  const char *p = in;
  const char *rank;
  char *z_rank = 0;
  char *rank_args = 0;
  int rc = DBSQL_OK;

  *pz_rank = 0;
  *pz_rank_args = 0;

  if( p==0 ){
    rc = DBSQL_ERROR;
  }else{
    p = __fts5_config_skip_whitespace(p);
    rank = p;
    p = __fts5_config_skip_bareword(p);

    if( p ){
      z_rank = __fts5_malloc_zero(&rc, 1 + p - rank);
      if( z_rank ) memcpy(z_rank, rank, p-rank);
    }else{
      rc = DBSQL_ERROR;
    }

    if( rc==DBSQL_OK ){
      p = __fts5_config_skip_whitespace(p);
      if( *p!='(' ) rc = DBSQL_ERROR;
      p++;
    }
    if( rc==DBSQL_OK ){
      const char *args; 
      p = __fts5_config_skip_whitespace(p);
      args = p;
      if( *p!=')' ){
        p = __fts5_config_skip_args(p);
        if( p==0 ){
          rc = DBSQL_ERROR;
        }else{
          rank_args = __fts5_malloc_zero(&rc, 1 + p - args);
          if( rank_args ) memcpy(rank_args, args, p-args);
        }
      }
    }
  }

  if( rc!=DBSQL_OK ){
    dbsql_free(z_rank);
    assert( rank_args==0 );
  }else{
    *pz_rank = z_rank;
    *pz_rank_args = rank_args;
  }
  return rc;
}

int __fts5_config_set_value(
  fts5_config_t *config, 
  const char *key, 
  dbsql_value_t *p_val,
  int *pb_badkey
){
  int rc = DBSQL_OK;

  if( 0==dbsql_stricmp(key, "pgsz") ){
    int pgsz = 0;
    if( DBSQL_INTEGER==dbsql_value_numeric_type(p_val) ){
      pgsz = dbsql_value_int(p_val);
    }
    if( pgsz<32 || pgsz>FTS5_MAX_PAGE_SIZE ){
      *pb_badkey = 1;
    }else{
      config->pgsz = pgsz;
    }
  }

  else if( 0==dbsql_stricmp(key, "hashsize") ){
    int n_hash_size = -1;
    if( DBSQL_INTEGER==dbsql_value_numeric_type(p_val) ){
      n_hash_size = dbsql_value_int(p_val);
    }
    if( n_hash_size<=0 ){
      *pb_badkey = 1;
    }else{
      config->n_hash_size = n_hash_size;
    }
  }

  else if( 0==dbsql_stricmp(key, "automerge") ){
    int n_automerge = -1;
    if( DBSQL_INTEGER==dbsql_value_numeric_type(p_val) ){
      n_automerge = dbsql_value_int(p_val);
    }
    if( n_automerge<0 || n_automerge>64 ){
      *pb_badkey = 1;
    }else{
      if( n_automerge==1 ) n_automerge = FTS5_DEFAULT_AUTOMERGE;
      config->n_automerge = n_automerge;
    }
  }

  else if( 0==dbsql_stricmp(key, "usermerge") ){
    int n_usermerge = -1;
    if( DBSQL_INTEGER==dbsql_value_numeric_type(p_val) ){
      n_usermerge = dbsql_value_int(p_val);
    }
    if( n_usermerge<2 || n_usermerge>16 ){
      *pb_badkey = 1;
    }else{
      config->n_usermerge = n_usermerge;
    }
  }

  else if( 0==dbsql_stricmp(key, "crisismerge") ){
    int n_crisis_merge = -1;
    if( DBSQL_INTEGER==dbsql_value_numeric_type(p_val) ){
      n_crisis_merge = dbsql_value_int(p_val);
    }
    if( n_crisis_merge<0 ){
      *pb_badkey = 1;
    }else{
      if( n_crisis_merge<=1 ) n_crisis_merge = FTS5_DEFAULT_CRISISMERGE;
      if( n_crisis_merge>=FTS5_MAX_SEGMENT ) n_crisis_merge = FTS5_MAX_SEGMENT-1;
      config->n_crisis_merge = n_crisis_merge;
    }
  }

  else if( 0==dbsql_stricmp(key, "deletemerge") ){
    int n_val = -1;
    if( DBSQL_INTEGER==dbsql_value_numeric_type(p_val) ){
      n_val = dbsql_value_int(p_val);
    }else{
      *pb_badkey = 1;
    }
    if( n_val<0 ) n_val = FTS5_DEFAULT_DELETE_AUTOMERGE;
    if( n_val>100 ) n_val = 0;
    config->n_delete_merge = n_val;
  }

  else if( 0==dbsql_stricmp(key, "rank") ){
    const char *in = (const char*)dbsql_value_text(p_val);
    char *rank;
    char *rank_args;
    rc = __fts5_config_parse_rank(in, &rank, &rank_args);
    if( rc==DBSQL_OK ){
      dbsql_free(config->z_rank);
      dbsql_free(config->z_rank_args);
      config->z_rank = rank;
      config->z_rank_args = rank_args;
    }else if( rc==DBSQL_ERROR ){
      rc = DBSQL_OK;
      *pb_badkey = 1;
    }
  }

  else if( 0==dbsql_stricmp(key, "secure-delete") ){
    int val = -1;
    if( DBSQL_INTEGER==dbsql_value_numeric_type(p_val) ){
      val = dbsql_value_int(p_val);
    }
    if( val<0 ){
      *pb_badkey = 1;
    }else{
      config->secure_delete = (val ? 1 : 0);
    }
  }

  else if( 0==dbsql_stricmp(key, "insttoken") ){
    int val = -1;
    if( DBSQL_INTEGER==dbsql_value_numeric_type(p_val) ){
      val = dbsql_value_int(p_val);
    }
    if( val<0 ){
      *pb_badkey = 1;
    }else{
      config->prefix_insttoken = (val ? 1 : 0);
    }

  }else{
    *pb_badkey = 1;
  }
  return rc;
}

/*
** Load the contents of the %_config table into memory.
*/
int __fts5_config_load(fts5_config_t *config, int i_cookie){
  const char *z_select = "SELECT k, v FROM %Q.'%q_config'";
  char *sql;
  dbsql_stmt_t *p = 0;
  int rc = DBSQL_OK;
  int i_version = 0;

  /* Set default values */
  config->pgsz = FTS5_DEFAULT_PAGE_SIZE;
  config->n_automerge = FTS5_DEFAULT_AUTOMERGE;
  config->n_usermerge = FTS5_DEFAULT_USERMERGE;
  config->n_crisis_merge = FTS5_DEFAULT_CRISISMERGE;
  config->n_hash_size = FTS5_DEFAULT_HASHSIZE;
  config->n_delete_merge = FTS5_DEFAULT_DELETE_AUTOMERGE;

  sql = __fts5_mprintf(&rc, z_select, config->z_db, config->name);
  if( sql ){
    rc = dbsql_prepare_v2(config->db, sql, -1, &p, 0);
    dbsql_free(sql);
  }

  assert( rc==DBSQL_OK || p==0 );
  if( rc==DBSQL_OK ){
    while( DBSQL_ROW==dbsql_step(p) ){
      const char *k = (const char*)dbsql_column_text(p, 0);
      dbsql_value_t *val = dbsql_column_value(p, 1);
      if( 0==dbsql_stricmp(k, "version") ){
        i_version = dbsql_value_int(val);
      }else{
        int dummy = 0;
        __fts5_config_set_value(config, k, val, &dummy);
      }
    }
    rc = dbsql_finalize(p);
  }
  
  if( rc==DBSQL_OK 
   && i_version!=FTS5_CURRENT_VERSION
   && i_version!=FTS5_CURRENT_VERSION_SECUREDELETE
  ){
    rc = DBSQL_ERROR;
    __fts5_config_errmsg(config, "invalid fts5 file format "
        "(found %d, expected %d or %d) - run 'rebuild'",
        i_version, FTS5_CURRENT_VERSION, FTS5_CURRENT_VERSION_SECUREDELETE
    );
  }else{
    config->i_version = i_version;
  }

  if( rc==DBSQL_OK ){
    config->i_cookie = i_cookie;
  }
  return rc;
}

/*
** Set (*config->errmsg) to point to an dbsql_malloc()ed buffer 
** containing the error message created using printf() style formatting
** string fmt and its trailing arguments.
*/
void __fts5_config_errmsg(fts5_config_t *config, const char *fmt, ...){
  va_list ap;                     /* ... printf arguments */
  char *msg = 0;

  va_start(ap, fmt);
  msg = dbsql_vmprintf(fmt, ap);
  if( config->errmsg ){
    assert( *config->errmsg==0 );
    *config->errmsg = msg;
  }else{
    dbsql_free(msg);
  }

  va_end(ap);
}
