/*
** 2013-02-28
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
** This file contains code to implement the next_char(A,T,F,W,C) SQL function.
**
** The next_char(A,T,F,W,C) function finds all valid "next" characters for
** string A given the vocabulary in T.F.  If the W value exists and is a
** non-empty string, then it is an SQL expression that limits the entries
** in T.F that will be considered.  If C exists and is a non-empty string,
** then it is the name of the collating sequence to use for comparison.  If
** 
** Only the first three arguments are required.  If the C parameter is 
** omitted or is NULL or is an empty string, then the default collating 
** sequence of T.F is used for comparision.  If the W parameter is omitted
** or is NULL or is an empty string, then no filtering of the output is
** done.
**
** The T.F column should be indexed using collation C or else this routine
** will be quite slow.
**
** For example, suppose an application has a dictionary like this:
**
**   CREATE TABLE dictionary(word TEXT UNIQUE);
**
** Further suppose that for user keypad entry, it is desired to disable
** (gray out) keys that are not valid as the next character.  If the
** the user has previously entered (say) 'cha' then to find all allowed
** next characters (and thereby determine when keys should not be grayed
** out) run the following query:
**
**   SELECT next_char('cha','dictionary','word');
**
** IMPLEMENTATION NOTES:
**
** The next_char function is implemented using recursive SQL that makes
** use of the table name and column name as part of a query.  If either
** the table name or column name are keywords or contain special characters,
** then they should be escaped.  For example:
**
**   SELECT next_char('cha','[dictionary]','[word]');
**
** This also means that the table name can be a subquery:
**
**   SELECT next_char('cha','(SELECT word AS w FROM dictionary)','w');
*/
#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <string.h>

/*
** A structure to hold context of the next_char() computation across
** nested function calls.
*/
typedef struct __next_char_context __next_char_context;
struct __next_char_context {
  DBSQL *db;                      /* Database connection */
  dbsql_stmt_t *stmt;              /* Prepared statement used to query */
  const unsigned char *prefix;     /* Prefix to scan */
  int n_prefix;                      /* Size of prefix in bytes */
  int n_alloc;                       /* Space allocated to results */
  int n_used;                        /* Space used in results */
  unsigned int *results;            /* Array of next characters */
  int malloc_failed;                 /* True if malloc fails */
  int other_error;                   /* True for any other failure */
};

/*
** Append a result character if the character is not already in the
** result.
*/
static void __next_char_append(__next_char_context *p, unsigned c){
  int i;
  for(i=0; i<p->n_used; i++){
    if( p->results[i]==c ) return;
  }
  if( p->n_used+1 > p->n_alloc ){
    unsigned int *news;
    int n = p->n_alloc*2 + 30;
    news = dbsql_realloc64(p->results, n*sizeof(unsigned int));
    if( news==0 ){
      p->malloc_failed = 1;
      return;
    }else{
      p->results = news;
      p->n_alloc = n;
    }
  }
  p->results[p->n_used++] = c;
}

/*
** Write a character into z[] as UTF8.  Return the number of bytes needed
** to hold the character
*/
static int __write_utf8(unsigned char *z, unsigned c){
  if( c<0x00080 ){
    z[0] = (unsigned char)(c&0xff);
    return 1;
  }
  if( c<0x00800 ){
    z[0] = 0xC0 + (unsigned char)((c>>6)&0x1F);
    z[1] = 0x80 + (unsigned char)(c & 0x3F);
    return 2;
  }
  if( c<0x10000 ){
    z[0] = 0xE0 + (unsigned char)((c>>12)&0x0F);
    z[1] = 0x80 + (unsigned char)((c>>6) & 0x3F);
    z[2] = 0x80 + (unsigned char)(c & 0x3F);
    return 3;
  }
  z[0] = 0xF0 + (unsigned char)((c>>18) & 0x07);
  z[1] = 0x80 + (unsigned char)((c>>12) & 0x3F);
  z[2] = 0x80 + (unsigned char)((c>>6) & 0x3F);
  z[3] = 0x80 + (unsigned char)(c & 0x3F);
  return 4;
}

/*
** Read a UTF8 character out of z[] and write it into *out.  Return
** the number of bytes in z[] that were used to construct the character.
*/
static int __read_utf8(const unsigned char *z, unsigned *out){
  static const unsigned char valid_bits[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x00, 0x01, 0x02, 0x03, 0x00, 0x01, 0x00, 0x00,
  };
  unsigned c = z[0];
  if( c<0xc0 ){
    *out = c;
    return 1;
  }else{
    int n = 1;
    c = valid_bits[c-0xc0];
    while( (z[n] & 0xc0)==0x80 ){
      c = (c<<6) + (0x3f & z[n++]);
    }
    if( c<0x80 || (c&0xFFFFF800)==0xD800 || (c&0xFFFFFFFE)==0xFFFE ){
      c = 0xFFFD;
    }
    *out = c;
    return n;
  }
}

/*
** The __next_char_context structure has been set up.  Add all "next" characters
** to the result set.
*/
static void __find_next_chars(__next_char_context *p){
  unsigned c_prev = 0;
  unsigned char prev[8];
  int n, rc;
  
  for(;;){
    dbsql_bind_text(p->stmt, 1, (char*)p->prefix, p->n_prefix,
                      DBSQL_STATIC);
    n = __write_utf8(prev, c_prev+1);
    dbsql_bind_text(p->stmt, 2, (char*)prev, n, DBSQL_STATIC);
    rc = dbsql_step(p->stmt);
    if( rc==DBSQL_DONE ){
      dbsql_reset(p->stmt);
      return;
    }else if( rc!=DBSQL_ROW ){
      p->other_error = rc;
      return;
    }else{
      const unsigned char *out = dbsql_column_text(p->stmt, 0);
      unsigned c_next;
      if( dbsql_column_bytes(p->stmt,0)<=p->n_prefix ){
        dbsql_reset(p->stmt);
        continue;
      }
      n = __read_utf8(out+p->n_prefix, &c_next);
      dbsql_reset(p->stmt);
      __next_char_append(p, c_next);
      c_prev = c_next;
      if( p->malloc_failed ) return;
    }
  }
}


/*
** next_character(A,T,F,W)
**
** Return a string composted of all next possible characters after
** A for elements of T.F.  If W is supplied, then it is an SQL expression
** that limits the elements in T.F that are considered.
*/
static void __next_char_func(
  dbsql_func_t *context,
  int argc,
  dbsql_value_t **argv
){
  __next_char_context c;
  const unsigned char *table = dbsql_value_text(argv[1]);
  const unsigned char *field = dbsql_value_text(argv[2]);
  const unsigned char *where;
  const unsigned char *coll_name;
  char *where_clause = 0;
  char *coll = 0;
  char *sql;
  int rc;

  memset(&c, 0, sizeof(c));
  c.db = dbsql_context_db_handle(context);
  c.prefix = dbsql_value_text(argv[0]);
  c.n_prefix = dbsql_value_bytes(argv[0]);
  if( table==0 || field==0 || c.prefix==0 ) return;
  if( argc>=4
   && (where = dbsql_value_text(argv[3]))!=0
   && where[0]!=0
  ){
    where_clause = dbsql_mprintf("AND (%s)", where);
    if( where_clause==0 ){
      dbsql_result_error_nomem(context);
      return;
    }
  }else{
    where_clause = "";
  }
  if( argc>=5
   && (coll_name = dbsql_value_text(argv[4]))!=0
   && coll_name[0]!=0 
  ){
    coll = dbsql_mprintf("collate \"%w\"", coll_name);
    if( coll==0 ){
      dbsql_result_error_nomem(context);
      if( where_clause[0] ) dbsql_free(where_clause);
      return;
    }
  }else{
    coll = "";
  }
  sql = dbsql_mprintf(
    "SELECT %s FROM %s"
    " WHERE %s>=(?1 || ?2) %s"
    "   AND %s<=(?1 || char(1114111)) %s" /* 1114111 == 0x10ffff */
    "   %s"
    " ORDER BY 1 %s ASC LIMIT 1",
    field, table, field, coll, field, coll, where_clause, coll
  );
  if( where_clause[0] ) dbsql_free(where_clause);
  if( coll[0] ) dbsql_free(coll);
  if( sql==0 ){
    dbsql_result_error_nomem(context);
    return;
  }

  rc = dbsql_prepare_v2(c.db, sql, -1, &c.stmt, 0);
  dbsql_free(sql);
  if( rc ){
    dbsql_result_error(context, dbsql_errmsg(c.db), -1);
    return;
  }
  __find_next_chars(&c);
  if( c.malloc_failed ){
    dbsql_result_error_nomem(context);
  }else{
    unsigned char *res;
    res = dbsql_malloc64( c.n_used*4 + 1 );
    if( res==0 ){
      dbsql_result_error_nomem(context);
    }else{
      int i;
      int n = 0;
      for(i=0; i<c.n_used; i++){
        n += __write_utf8(res+n, c.results[i]);
      }
      res[n] = 0;
      dbsql_result_text(context, (const char*)res, n, dbsql_free);
    }
  }
  dbsql_finalize(c.stmt);
  dbsql_free(c.results);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_nextchar_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Unused parameter */
  rc = dbsql_create_function(db, "next_char", 3,
                               DBSQL_UTF8|DBSQL_INNOCUOUS, 0,
                               __next_char_func, 0, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "next_char", 4,
                                 DBSQL_UTF8|DBSQL_INNOCUOUS, 0,
                                 __next_char_func, 0, 0);
  }
  if( rc==DBSQL_OK ){
    rc = dbsql_create_function(db, "next_char", 5,
                                 DBSQL_UTF8|DBSQL_INNOCUOUS, 0,
                                 __next_char_func, 0, 0);
  }
  return rc;
}
