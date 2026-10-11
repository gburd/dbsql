/*
** 2017 July 15
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
** This file contains the implementation of the "unionvtab" and "swarmvtab"
** virtual tables. These modules provide read-only access to multiple tables,
** possibly in multiple database files, via a single database object.
** The source tables must have the following characteristics:
**
**   * They must all be rowid tables (not VIRTUAL or WITHOUT ROWID
**     tables or views).
**
**   * Each table must have the same set of columns, declared in
**     the same order and with the same declared types.
**
**   * The tables must not feature a user-defined column named "_rowid_".
**
**   * Each table must contain a distinct range of rowid values.
**
** The difference between the two virtual table modules is that for 
** "unionvtab", all source tables must be located in the main database or
** in databases ATTACHed to the main database by the user. For "swarmvtab",
** the tables may be located in any database file on disk. The "swarmvtab"
** implementation takes care of opening and closing database files
** automatically.
**
** UNIONVTAB
**
**   A "unionvtab" virtual table is created as follows:
**
**     CREATE VIRTUAL TABLE <name> USING unionvtab(<sql-statement>);
**
**   The implementation evalutes <sql statement> whenever a unionvtab virtual
**   table is created or opened. It should return one row for each source
**   database table. The four columns required of each row are:
**
**     1. The name of the database containing the table ("main" or "temp" or
**        the name of an attached database). Or NULL to indicate that all
**        databases should be searched for the table in the usual fashion.
**
**     2. The name of the database table.
**
**     3. The smallest rowid in the range of rowids that may be stored in the
**        database table (an integer).
**
**     4. The largest rowid in the range of rowids that may be stored in the
**        database table (an integer).
**
** SWARMVTAB
**
**  LEGACY SYNTAX:
**
**   A "swarmvtab" virtual table is created similarly to a unionvtab table:
**
**     CREATE VIRTUAL TABLE <name>
**      USING swarmvtab(<sql-statement>, <callback>);
**
**   The difference is that for a swarmvtab table, the first column returned
**   by the <sql statement> must return a path or URI that can be used to open
**   the database file containing the source table.  The <callback> option
**   is optional.  If included, it is the name of an application-defined
**   SQL function that is invoked with the URI of the file, if the file
**   does not already exist on disk when required by swarmvtab.
**
**  NEW SYNTAX:
**
**   Using the new syntax, a swarmvtab table is created with:
**
**      CREATE VIRTUAL TABLE <name> USING swarmvtab(
**        <sql-statement> [, <options>]
**      );
**
**   where valid <options> are:
**
**      missing=<udf-function-name>
**      openclose=<udf-function-name>
**      maxopen=<integer>
**      <sql-parameter>=<text-value>
**
**   The <sql-statement> must return the same 4 columns as for a swarmvtab
**   table in legacy mode. However, it may also return a 5th column - the
**   "context" column. The text value returned in this column is not used
**   at all by the swarmvtab implementation, except that it is passed as
**   an additional argument to the two UDF functions that may be invoked
**   (see below).
**
**   The "missing" option, if present, specifies the name of an SQL UDF
**   function to be invoked if a database file is not already present on
**   disk when required by swarmvtab. If the <sql-statement> did not provide
**   a context column, it is invoked as:
**
**     SELECT <missing-udf>(<database filename/uri>);
**
**   Or, if there was a context column:
**
**     SELECT <missing-udf>(<database filename/uri>, <context>);
**
**   The "openclose" option may also specify a UDF function. This function
**   is invoked right before swarmvtab opens a database, and right after
**   it closes one. The first argument - or first two arguments, if
**   <sql-statement> supplied the context column - is the same as for
**   the "missing" UDF. Following this, the UDF is passed integer value
**   0 before a db is opened, and 1 right after it is closed. If both
**   a missing and openclose UDF is supplied, the application should expect
**   the following sequence of calls (for a single database):
**
**      SELECT <openclose-udf>(<db filename>, <context>, 0);
**      if( db not already on disk ){
**          SELECT <missing-udf>(<db filename>, <context>);
**      }
**      ... swarmvtab uses database ...
**      SELECT <openclose-udf>(<db filename>, <context>, 1);
**
**   The "maxopen" option is used to configure the maximum number of
**   database files swarmvtab will hold open simultaneously (default 9).
**
**   If an option name begins with a ":" character, then it is assumed
**   to be an SQL parameter. In this case, the specified text value is
**   bound to the same variable of the <sql-statement> before it is 
**   executed. It is an error of the named SQL parameter does not exist.
**   For example:
**
**     CREATE VIRTUAL TABLE swarm USING swarmvtab(
**       'SELECT :path || localfile, tbl, min, max FROM swarmdir',
**       :path='/home/user/databases/'
**       missing='missing_func'
**     );
*/

#include "dbsql_ext.h"
DBSQL_EXTENSION_INIT1
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** Largest and smallest possible 64-bit signed integers. These macros
** copied from dbsql_int.h.
*/
#ifndef LARGEST_INT64
# define LARGEST_INT64  (0xffffffff|(((int64_t)0x7fffffff)<<32))
#endif
#ifndef SMALLEST_INT64
# define SMALLEST_INT64 (((int64_t)-1) - LARGEST_INT64)
#endif

/*
** The following is also copied from dbsql_int.h. To facilitate coverage
** testing.
*/
#ifndef ALWAYS
# if defined(DBSQL_COVERAGE_TEST) || defined(DBSQL_MUTATION_TEST)
#  define ALWAYS(X)      (1)
#  define NEVER(X)       (0)
# elif !defined(NDEBUG)
#  define ALWAYS(X)      ((X)?1:(assert(0),0))
#  define NEVER(X)       ((X)?(assert(0),1):0)
# else
#  define ALWAYS(X)      (X)
#  define NEVER(X)       (X)
# endif
#endif

/*
** The swarmvtab module attempts to keep the number of open database files
** at or below this limit. This may not be possible if there are too many
** simultaneous queries.
*/
#define SWARMVTAB_MAX_OPEN 9

typedef struct union_csr_t union_csr_t;
typedef struct union_tab_t union_tab_t;
typedef struct union_src_t union_src_t;

/*
** Each source table (row returned by the initialization query) is 
** represented by an instance of the following structure stored in the
** union_tab_t.srcs[] array.
*/
struct union_src_t {
  char *z_db;                      /* Database containing source table */
  char *tab;                     /* Source table name */
  int64_t i_min;             /* Minimum rowid */
  int64_t i_max;             /* Maximum rowid */

  /* Fields used by swarmvtab only */
  char *z_file;                    /* Database file containing table tab */
  char *context;                 /* Context string, if any */
  int n_user;                      /* Current number of users */
  DBSQL *db;                    /* Database handle */
  union_src_t *next_closable;        /* Next in list of closable sources */
};

/*
** Virtual table  type for union vtab.
*/
struct union_tab_t {
  dbsql_vtab_t base;              /* Base class - must be first */
  DBSQL *db;                    /* Database handle */
  int swarm;                     /* 1 for "swarmvtab", 0 for "unionvtab" */
  int i_pk;                        /* INTEGER PRIMARY KEY column, or -1 */
  int n_src;                       /* Number of elements in the srcs[] array */
  union_src_t *srcs;                 /* Array of source tables, sorted by rowid */

  /* Used by swarmvtab only */
  int has_context;                /* Has context strings */
  char *source_str;               /* Expected __union_source_to_str() value */
  dbsql_stmt_t *not_found;        /* UDF to invoke if file not found on open */
  dbsql_stmt_t *open_close;       /* UDF to invoke on open and close */

  union_src_t *closable;            /* First in list of closable sources */
  int n_open;                      /* Current number of open sources */
  int n_max_open;                   /* Maximum number of open sources */
};

/*
** Virtual table cursor type for union vtab.
*/
struct union_csr_t {
  dbsql_vtab_cursor_t base;       /* Base class - must be first */
  dbsql_stmt_t *stmt;            /* SQL statement to run */

  /* Used by swarmvtab only */
  int64_t i_max_rowid;        /* Last rowid to visit */
  int i_tab;                       /* index_t of table read by stmt */
};

/*
** Given union_tab_t table tab and union_src_t object src, return the database
** handle that should be used to access the table identified by src. This
** is the main db handle for "unionvtab" tables, or the source-specific 
** handle for "swarmvtab".
*/
#define UNION_GET_DB(tab, src) ((tab)->swarm ? (src)->db : (tab)->db)

/*
** If *rc is other than DBSQL_OK when this function is called, it
** always returns NULL. Otherwise, it attempts to allocate and return
** a pointer to n_byte bytes of zeroed memory. If the memory allocation
** is attempted but fails, NULL is returned and *rc is set to 
** DBSQL_NOMEM.
*/
static void *__union_malloc(int *rc, int64_t n_byte){
  void *ret;
  assert( n_byte>0 );
  if( *rc==DBSQL_OK ){
    ret = dbsql_malloc64(n_byte);
    if( ret ){
      memset(ret, 0, (size_t)n_byte);
    }else{
      *rc = DBSQL_NOMEM;
    }
  }else{
    ret = 0;
  }
  return ret;
}

/*
** If *rc is other than DBSQL_OK when this function is called, it
** always returns NULL. Otherwise, it attempts to allocate and return
** a copy of the nul-terminated string passed as the second argument.
** If the allocation is attempted but fails, NULL is returned and *rc is 
** set to DBSQL_NOMEM.
*/
static char *__union_strdup(int *rc, const char *in){
  char *ret = 0;
  if( in ){
    int64_t n_byte = strlen(in) + 1;
    ret = __union_malloc(rc, n_byte);
    if( ret ){
      memcpy(ret, in, (size_t)n_byte);
    }
  }
  return ret;
}

/*
** If the first character of the string passed as the only argument to this
** function is one of the 4 that may be used as an open quote character
** in SQL, this function assumes that the input is a well-formed quoted SQL 
** string. In this case the string is dequoted in place.
**
** If the first character of the input is not an open quote, then this
** function is a no-op.
*/
static void __union_dequote(char *z){
  if( z ){
    char q = z[0];

    /* Set stack variable q to the close-quote character */
    if( q=='[' || q=='\'' || q=='"' || q=='`' ){
      int i_in = 1;
      int i_out = 0;
      if( q=='[' ) q = ']';  
      while( ALWAYS(z[i_in]) ){
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
}

/*
** This function is a no-op if *rc is set to other than DBSQL_OK when it
** is called. NULL is returned in this case.
**
** Otherwise, the SQL statement passed as the third argument is prepared
** against the database handle passed as the second. If the statement is
** successfully prepared, a pointer to the new statement handle is 
** returned. It is the responsibility of the caller to eventually free the
** statement by calling dbsql_finalize(). Alternatively, if statement
** compilation fails, NULL is returned, *rc is set to an dbsql error
** code and *err_p may be set to an error message buffer allocated by
** dbsql_malloc().
*/
static dbsql_stmt_t *__union_prepare(
  int *p_rc,                       /* IN/OUT: Error code */
  DBSQL *db,                    /* Database handle */
  const char *sql,               /* SQL statement to prepare */
  char **err_p                    /* OUT: Error message */
){
  dbsql_stmt_t *ret = 0;
  assert( err_p );
  if( *p_rc==DBSQL_OK ){
    int rc = dbsql_prepare_v2(db, sql, -1, &ret, 0);
    if( rc!=DBSQL_OK ){
      *err_p = dbsql_mprintf("sql error: %s", dbsql_errmsg(db));
      *p_rc = rc;
    }
  }
  return ret;
}

/*
** Like __union_prepare(), except prepare the results of vprintf(fmt, ...)
** instead of a constant SQL string.
*/
static dbsql_stmt_t *__union_prepare_printf(
  int *rc,                       /* IN/OUT: Error code */
  char **err_p,                   /* OUT: Error message */
  DBSQL *db,                    /* Database handle */
  const char *fmt,               /* printf() format string */
  ...                             /* Trailing printf args */
){
  dbsql_stmt_t *ret = 0;
  char *sql;
  va_list ap;
  va_start(ap, fmt);

  sql = dbsql_vmprintf(fmt, ap);
  if( *rc==DBSQL_OK ){
    if( sql==0 ){
      *rc = DBSQL_NOMEM;
    }else{
      ret = __union_prepare(rc, db, sql, err_p);
    }
  }
  dbsql_free(sql);

  va_end(ap);
  return ret;
}


/*
** Call dbsql_reset() on SQL statement stmt. If *rc is set to 
** DBSQL_OK when this function is called, then it is set to the
** value returned by dbsql_reset() before this function exits.
** In this case, *err_p may be set to point to an error message
** buffer allocated by dbsql_malloc().
*/
#if 0
static void __union_reset(int *p_rc, dbsql_stmt_t *stmt, char **err_p){
  int rc = dbsql_reset(stmt);
  if( *p_rc==DBSQL_OK ){
    *p_rc = rc;
    if( rc ){
      *err_p = dbsql_mprintf("%s", dbsql_errmsg(dbsql_db_handle(stmt)));
    }
  }
}
#endif

/*
** Call dbsql_finalize() on SQL statement stmt. If *rc is set to 
** DBSQL_OK when this function is called, then it is set to the
** value returned by dbsql_finalize() before this function exits.
*/
static void __union_finalize(int *p_rc, dbsql_stmt_t *stmt, char **err_p){
  DBSQL *db = dbsql_db_handle(stmt);
  int rc = dbsql_finalize(stmt);
  if( *p_rc==DBSQL_OK ){
    *p_rc = rc;
    if( rc ){
      *err_p = dbsql_mprintf("%s", dbsql_errmsg(db));
    }
  }
}

/*
** If an "openclose" UDF was supplied when this virtual table was created,
** invoke it now. The first argument passed is the name of the database
** file for source src. The second is integer value b_close.
**
** If successful, return DBSQL_OK. Otherwise an dbsql error code. In this
** case if argument err_p is not NULL, also set (*err_p) to an English
** language error message. The caller is responsible for eventually freeing 
** any error message using dbsql_free().
*/
static int __union_invoke_open_close(
  union_tab_t *tab, 
  union_src_t *src, 
  int b_close,
  char **err_p
){
  int rc = DBSQL_OK;
  if( tab->open_close ){
    dbsql_bind_text(tab->open_close, 1, src->z_file, -1, DBSQL_STATIC);
    if( tab->has_context ){
      dbsql_bind_text(tab->open_close, 2, src->context, -1, DBSQL_STATIC);
    }
    dbsql_bind_int(tab->open_close, 2+tab->has_context, b_close);
    dbsql_step(tab->open_close);
    if( DBSQL_OK!=(rc = dbsql_reset(tab->open_close)) ){
      if( err_p ){
        *err_p = dbsql_mprintf("%s", dbsql_errmsg(tab->db));
      }
    }
  }
  return rc;
}

/*
** This function is a no-op for unionvtab. For swarmvtab, it attempts to
** close open database files until at most n_max are open. An dbsql error
** code is returned if an error occurs, or DBSQL_OK otherwise.
*/
static void __union_close_sources(union_tab_t *tab, int n_max){
  while( tab->closable && tab->n_open>n_max ){
    union_src_t *p;
    union_src_t **pp;
    for(pp=&tab->closable; (*pp)->next_closable; pp=&(*pp)->next_closable);
    p = *pp;
    assert( p->db );
    dbsql_close(p->db);
    p->db = 0;
    *pp = 0;
    tab->n_open--;
    __union_invoke_open_close(tab, p, 1, 0);
  }
}

/*
** disconnect method.
*/
static int __union_disconnect(dbsql_vtab_t *vtab){
  if( vtab ){
    union_tab_t *tab = (union_tab_t*)vtab;
    int i;
    for(i=0; i<tab->n_src; i++){
      union_src_t *src = &tab->srcs[i];
      int have_src_db = (src->db!=0);
      dbsql_close(src->db);
      if( have_src_db ){
        __union_invoke_open_close(tab, src, 1, 0);
      }
      dbsql_free(src->z_db);
      dbsql_free(src->tab);
      dbsql_free(src->z_file);
      dbsql_free(src->context);
    }
    dbsql_finalize(tab->not_found);
    dbsql_finalize(tab->open_close);
    dbsql_free(tab->source_str);
    dbsql_free(tab->srcs);
    dbsql_free(tab);
  }
  return DBSQL_OK;
}

/*
** Check that the table identified by src is a rowid table. If not,
** return DBSQL_ERROR and set (*err_p) to point to an English language
** error message. If the table is a rowid table and no error occurs,
** return DBSQL_OK and leave (*err_p) unmodified.
*/
static int __union_is_intkey_table(
  DBSQL *db,                    /* Database handle */
  union_src_t *src,                 /* Source table to test */
  char **err_p                    /* OUT: Error message */
){
  int pk = 0;
  const char *type = 0;
  int rc;

  dbsql_table_column_metadata(
      db, src->z_db, src->tab, "_rowid_", &type, 0, 0, &pk, 0
  );
  rc = dbsql_errcode(db);
  if( rc==DBSQL_ERROR 
   || (rc==DBSQL_OK && (!pk || dbsql_stricmp("integer", type)))
  ){
    rc = DBSQL_ERROR;
    *err_p = dbsql_mprintf("no such rowid table: %s%s%s",
        (src->z_db ? src->z_db : ""),
        (src->z_db ? "." : ""),
        src->tab
    );
  }
  return rc;
}

/*
** This function is a no-op if *rc is other than DBSQL_OK when it is
** called. In this case it returns NULL.
**
** Otherwise, this function checks that the source table passed as the
** second argument (a) exists, (b) is not a view and (c) has a column 
** named "_rowid_" of type "integer" that is the primary key.
** If this is not the case, *rc is set to DBSQL_ERROR and NULL is
** returned.
**
** Finally, if the source table passes the checks above, a nul-terminated
** string describing the column names and types belonging to the source
** table is returned. Tables with the same set of column names and types 
** cause this function to return identical strings. Is is the responsibility
** of the caller to free the returned string using dbsql_free() when
** it is no longer required.
*/
static char *__union_source_to_str(
  int *p_rc,                       /* IN/OUT: Error code */
  union_tab_t *tab,                 /* Virtual table object */
  union_src_t *src,                 /* Source table to test */
  char **err_p                    /* OUT: Error message */
){
  char *ret = 0;
  if( *p_rc==DBSQL_OK ){
    DBSQL *db = UNION_GET_DB(tab, src);
    int rc = __union_is_intkey_table(db, src, err_p);
    dbsql_stmt_t *stmt = __union_prepare(&rc, db, 
        "SELECT group_concat(quote(name) || '.' || quote(type)) "
        "FROM pragma_table_info(?, ?)", err_p
    );
    if( rc==DBSQL_OK ){
      dbsql_bind_text(stmt, 1, src->tab, -1, DBSQL_STATIC);
      dbsql_bind_text(stmt, 2, src->z_db, -1, DBSQL_STATIC);
      if( DBSQL_ROW==dbsql_step(stmt) ){
        const char *z = (const char*)dbsql_column_text(stmt, 0);
        ret = __union_strdup(&rc, z);
      }
      __union_finalize(&rc, stmt, err_p);
    }
    *p_rc = rc;
  }

  return ret;
}

/*
** Check that all configured source tables exist and have the same column
** names and datatypes. If this is not the case, or if some other error
** occurs, return an dbsql error code. In this case *err_p may be set
** to point to an error message buffer allocated by dbsql_mprintf().
** Or, if no problems regarding the source tables are detected and no
** other error occurs, DBSQL_OK is returned.
*/
static int __union_source_check(union_tab_t *tab, char **err_p){
  int rc = DBSQL_OK;
  char *z0 = 0;
  int i;

  assert( *err_p==0 );
  z0 = __union_source_to_str(&rc, tab, &tab->srcs[0], err_p);
  for(i=1; i<tab->n_src; i++){
    char *z = __union_source_to_str(&rc, tab, &tab->srcs[i], err_p);
    if( rc==DBSQL_OK && dbsql_stricmp(z, z0) ){
      *err_p = dbsql_mprintf("source table schema mismatch");
      rc = DBSQL_ERROR;
    }
    dbsql_free(z);
  }
  dbsql_free(z0);

  return rc;
}

/*
** Try to open the swarmvtab database.  If initially unable, invoke the
** not-found callback UDF and then try again.
*/
static int __union_open_database_inner(union_tab_t *tab, union_src_t *src, char **err_p){
  static const int open_flags = DBSQL_OPEN_READONLY | DBSQL_OPEN_URI;
  int rc;

  rc = __union_invoke_open_close(tab, src, 0, err_p);
  if( rc!=DBSQL_OK ) return rc;

  rc = dbsql_open_v2(src->z_file, &src->db, open_flags, 0);
  if( rc==DBSQL_OK ) return rc;
  if( tab->not_found ){
    dbsql_close(src->db);
    src->db = 0;
    dbsql_bind_text(tab->not_found, 1, src->z_file, -1, DBSQL_STATIC);
    if( tab->has_context ){
      dbsql_bind_text(tab->not_found, 2, src->context, -1, DBSQL_STATIC);
    }
    dbsql_step(tab->not_found);
    if( DBSQL_OK!=(rc = dbsql_reset(tab->not_found)) ){
      *err_p = dbsql_mprintf("%s", dbsql_errmsg(tab->db));
      return rc;
    }
    rc = dbsql_open_v2(src->z_file, &src->db, open_flags, 0);
  }
  if( rc!=DBSQL_OK ){
    *err_p = dbsql_mprintf("%s", dbsql_errmsg(src->db));
  }
  return rc;
}

/*
** This function may only be called for swarmvtab tables. The results of
** calling it on a unionvtab table are undefined.
**
** For a swarmvtab table, this function ensures that source database i_src
** is open. If the database is opened successfully and the schema is as
** expected, or if it is already open when this function is called, DBSQL_OK
** is returned.
**
** Alternatively If an error occurs while opening the databases, or if the
** database schema is unsuitable, an dbsql error code is returned and (*err_p)
** may be set to point to an English language error message. In this case it is
** the responsibility of the caller to eventually free the error message buffer
** using dbsql_free(). 
*/
static int __union_open_database(union_tab_t *tab, int i_src, char **err_p){
  int rc = DBSQL_OK;
  union_src_t *src = &tab->srcs[i_src];

  assert( tab->swarm && i_src<tab->n_src );
  if( src->db==0 ){
    __union_close_sources(tab, tab->n_max_open-1);
    rc = __union_open_database_inner(tab, src, err_p);
    if( rc==DBSQL_OK ){
      char *z = __union_source_to_str(&rc, tab, src, err_p);
      if( rc==DBSQL_OK ){
        if( tab->source_str==0 ){
          tab->source_str = z;
        }else{
          if( dbsql_stricmp(z, tab->source_str) ){
            *err_p = dbsql_mprintf("source table schema mismatch");
            rc = DBSQL_ERROR;
          }
          dbsql_free(z);
        }
      }
    }

    if( rc==DBSQL_OK ){
      src->next_closable = tab->closable;
      tab->closable = src;
      tab->n_open++;
    }else{
      dbsql_close(src->db);
      src->db = 0;
      __union_invoke_open_close(tab, src, 1, 0);
    }
  }

  return rc;
}


/*
** This function is a no-op for unionvtab tables. For swarmvtab, increment 
** the reference count for source table i_tab. If the reference count was
** zero before it was incremented, also remove the source from the closable
** list.
*/
static void __union_incr_refcount(union_tab_t *tab, int i_tab){
  if( tab->swarm ){
    union_src_t *src = &tab->srcs[i_tab];
    assert( src->n_user>=0 && src->db );
    if( src->n_user==0 ){
      union_src_t **pp;
      for(pp=&tab->closable; *pp!=src; pp=&(*pp)->next_closable);
      *pp = src->next_closable;
      src->next_closable = 0;
    }
    src->n_user++;
  }
}

/*
** Finalize the SQL statement csr->stmt and return the result.
**
** If this is a swarmvtab table (not unionvtab) and csr->stmt was not
** NULL when this function was called, also decrement the reference
** count on the associated source table. If this means the source tables
** refcount is now zero, add it to the closable list.
*/
static int __union_finalize_csr_stmt(union_csr_t *csr){
  int rc = DBSQL_OK;
  if( csr->stmt ){
    union_tab_t *tab = (union_tab_t*)csr->base.vtab;
    union_src_t *src = &tab->srcs[csr->i_tab];
    rc = dbsql_finalize(csr->stmt);
    csr->stmt = 0;
    if( tab->swarm ){
      src->n_user--;
      assert( src->n_user>=0 );
      if( src->n_user==0 ){
        src->next_closable = tab->closable;
        tab->closable = src;
      }
      __union_close_sources(tab, tab->n_max_open);
    }
  }
  return rc;
}

/* 
** Return true if the argument is a space, tab, CR or LF character.
*/
static int union_isspace(char c){
  return (c==' ' || c=='\n' || c=='\r' || c=='\t');
}

/* 
** Return true if the argument is an alphanumeric character in the 
** ASCII range.
*/
static int union_isidchar(char c){
  return ((c>='a' && c<='z') || (c>='A' && c<'Z') || (c>='0' && c<='9'));
}

/*
** This function is called to handle all arguments following the first 
** (the SQL statement) passed to a swarmvtab (not unionvtab) CREATE 
** VIRTUAL TABLE statement. It may bind parameters to the SQL statement 
** or configure members of the union_tab_t object passed as the second
** argument.
**
** Refer to header comments at the top of this file for a description
** of the arguments parsed.
**
** This function is a no-op if *rc is other than DBSQL_OK when it is
** called. Otherwise, if an error occurs, *rc is set to an dbsql error
** code. In this case *err_p may be set to point to a buffer containing
** an English language error message. It is the responsibility of the 
** caller to eventually free the buffer using dbsql_free().
*/
static void __union_configure_vtab(
  int *p_rc,                       /* IN/OUT: Error code */
  union_tab_t *tab,                 /* table_t to configure */
  dbsql_stmt_t *stmt,            /* SQL statement to find sources */
  int n_arg,                       /* Number of entries in args[] array */
  const char * const *args,      /* Array of arguments to consider */
  char **err_p                    /* OUT: Error message */
){
  int rc = *p_rc;
  int i;
  if( rc==DBSQL_OK ){
    tab->has_context = (dbsql_column_count(stmt)>4);
  }
  for(i=0; rc==DBSQL_OK && i<n_arg; i++){
    char *arg = __union_strdup(&rc, args[i]);
    if( arg ){
      int n_opt = 0;               /* Size of option name in bytes */
      char *opt;                 /* Pointer to option name */
      char *val;                 /* Pointer to value */

      __union_dequote(arg);
      opt = arg;
      while( union_isspace(*opt) ) opt++;
      val = opt;
      if( *val==':' ) val++;
      while( union_isidchar(*val) ) val++;
      n_opt = (int)(val-opt);

      while( union_isspace(*val) ) val++;
      if( *val=='=' ){
        opt[n_opt] = '\0';
        val++;
        while( union_isspace(*val) ) val++;
        val = __union_strdup(&rc, val);
        if( val ){
          __union_dequote(val);
          if( opt[0]==':' ){
            /* A value to bind to the SQL statement */
            int i_param = dbsql_bind_parameter_index(stmt, opt);
            if( i_param==0 ){
              *err_p = dbsql_mprintf(
                  "swarmvtab: no such SQL parameter: %s", opt
              );
              rc = DBSQL_ERROR;
            }else{
              rc = dbsql_bind_text(stmt, i_param, val, -1, DBSQL_TRANSIENT);
            }
          }else if( n_opt==7 && 0==dbsql_strnicmp(opt, "maxopen", 7) ){
            tab->n_max_open = atoi(val);
            if( tab->n_max_open<=0 ){
              *err_p = dbsql_mprintf("swarmvtab: illegal maxopen value");
              rc = DBSQL_ERROR;
            }
          }else if( n_opt==7 && 0==dbsql_strnicmp(opt, "missing", 7) ){
            if( tab->not_found ){
              *err_p = dbsql_mprintf(
                  "swarmvtab: duplicate \"missing\" option");
              rc = DBSQL_ERROR;
            }else{
              tab->not_found = __union_prepare_printf(&rc, err_p, tab->db,
                  "SELECT \"%w\"(?%s)", val, tab->has_context ? ",?" : ""
              );
            }
          }else if( n_opt==9 && 0==dbsql_strnicmp(opt, "openclose", 9) ){
            if( tab->open_close ){
              *err_p = dbsql_mprintf(
                  "swarmvtab: duplicate \"openclose\" option");
              rc = DBSQL_ERROR;
            }else{
              tab->open_close = __union_prepare_printf(&rc, err_p, tab->db,
                  "SELECT \"%w\"(?,?%s)", val, tab->has_context ? ",?" : ""
              );
            }
          }else{
            *err_p = dbsql_mprintf("swarmvtab: unrecognized option: %s",opt);
            rc = DBSQL_ERROR;
          }
          dbsql_free(val);
        }
      }else{
        if( i==0 && n_arg==1 ){
          tab->not_found = __union_prepare_printf(&rc, err_p, tab->db,
              "SELECT \"%w\"(?)", arg
          );
        }else{
          *err_p = dbsql_mprintf( "swarmvtab: parse error: %s", args[i]);
          rc = DBSQL_ERROR;
        }
      }
      dbsql_free(arg);
    }
  }
  *p_rc = rc;
}

/* 
** connect/create method.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("unionvtab" or "swarmvtab")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[3]   -> SQL statement
**   argv[4]   -> not-found callback UDF name
*/
static int __union_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **pp_vtab,
  char **err_p
){
  union_tab_t *tab = 0;
  int rc = DBSQL_OK;
  int swarm = (aux==0 ? 0 : 1);
  const char *vtab = (swarm ? "swarmvtab" : "unionvtab");

  if( dbsql_stricmp("temp", argv[1]) ){
    /* unionvtab tables may only be created in the temp schema */
    *err_p = dbsql_mprintf("%s tables must be created in TEMP schema", vtab);
    rc = DBSQL_ERROR;
  }else if( argc<4 || (argc>4 && swarm==0) ){
    *err_p = dbsql_mprintf("wrong number of arguments for %s", vtab);
    rc = DBSQL_ERROR;
  }else{
    int n_alloc = 0;               /* Allocated size of tab->srcs[] */
    dbsql_stmt_t *stmt = 0;      /* Argument statement */
    char *arg = __union_strdup(&rc, argv[3]);      /* Copy of argument to CVT */

    /* Prepare the SQL statement. Instead of executing it directly, sort
    ** the results by the "minimum rowid" field. This makes it easier to
    ** check that there are no rowid range overlaps between source tables 
    ** and that the union_tab_t.srcs[] array is always sorted by rowid.  */
    __union_dequote(arg);
    stmt = __union_prepare_printf(&rc, err_p, db, 
        "SELECT * FROM (%z) ORDER BY 3", arg
    );

    /* Allocate the union_tab_t structure */
    tab = __union_malloc(&rc, sizeof(union_tab_t));
    if( tab ){
      assert( rc==DBSQL_OK );
      tab->db = db;
      tab->swarm = swarm;
      tab->n_max_open = SWARMVTAB_MAX_OPEN;
    }

    /* parser_t other CVT arguments, if any */
    if( swarm ){
      __union_configure_vtab(&rc, tab, stmt, argc-4, &argv[4], err_p);
    }

    /* Iterate through the rows returned by the SQL statement specified
    ** as an argument to the CREATE VIRTUAL TABLE statement. */
    while( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(stmt) ){
      const char *z_db = (const char*)dbsql_column_text(stmt, 0);
      const char *z_tab = (const char*)dbsql_column_text(stmt, 1);
      int64_t i_min = dbsql_column_int64(stmt, 2);
      int64_t i_max = dbsql_column_int64(stmt, 3);
      union_src_t *src;

      /* Grow the tab->srcs[] array if required. */
      if( n_alloc<=tab->n_src ){
        int n_new = n_alloc ? n_alloc*2 : 8;
        union_src_t *news = (union_src_t*)dbsql_realloc64(
            tab->srcs, n_new*sizeof(union_src_t)
        );
        if( news==0 ){
          rc = DBSQL_NOMEM;
          break;
        }else{
          memset(&news[tab->n_src], 0, (n_new-tab->n_src)*sizeof(union_src_t));
          tab->srcs = news;
          n_alloc = n_new;
        }
      }

      /* Check for problems with the specified range of rowids */
      if( i_max<i_min || (tab->n_src>0 && i_min<=tab->srcs[tab->n_src-1].i_max) ){
        *err_p = dbsql_mprintf("rowid range mismatch error");
        rc = DBSQL_ERROR;
      }

      if( rc==DBSQL_OK ){
        src = &tab->srcs[tab->n_src++];
        src->tab = __union_strdup(&rc, (z_tab ? z_tab : ""));
        src->i_min = i_min;
        src->i_max = i_max;
        if( swarm ){
          src->z_file = __union_strdup(&rc, z_db);
        }else{
          src->z_db = __union_strdup(&rc, z_db);
        }
        if( tab->has_context ){
          const char *context = (const char*)dbsql_column_text(stmt, 4);
          src->context = __union_strdup(&rc, context);
        }
      }
    }
    __union_finalize(&rc, stmt, err_p);
    stmt = 0;

    /* It is an error if the SELECT statement returned zero rows. If only
    ** because there is no way to determine the schema of the virtual 
    ** table in this case.  */
    if( rc==DBSQL_OK && tab->n_src==0 ){
      *err_p = dbsql_mprintf("no source tables configured");
      rc = DBSQL_ERROR;
    }

    /* For unionvtab, verify that all source tables exist and have 
    ** compatible schemas. For swarmvtab, attach the first database and
    ** check that the first table is a rowid table only.  */
    if( rc==DBSQL_OK ){
      if( swarm ){
        rc = __union_open_database(tab, 0, err_p);
      }else{
        rc = __union_source_check(tab, err_p);
      }
    }

    /* Compose a CREATE TABLE statement and pass it to declare_vtab() */
    if( rc==DBSQL_OK ){
      union_src_t *src = &tab->srcs[0];
      DBSQL *tdb = UNION_GET_DB(tab, src);
      stmt = __union_prepare_printf(&rc, err_p, tdb, "SELECT "
          "'CREATE TABLE xyz('"
          "    || group_concat(quote(name) || ' ' || type, ', ')"
          "    || ')',"
          "max((cid+1) * (type='INTEGER' COLLATE nocase AND pk=1))-1 "
          "FROM pragma_table_info(%Q, ?)", 
          src->tab, src->z_db
      );
    }
    if( rc==DBSQL_OK && DBSQL_ROW==dbsql_step(stmt) ){
      const char *decl = (const char*)dbsql_column_text(stmt, 0);
      rc = dbsql_declare_vtab(db, decl);
      tab->i_pk = dbsql_column_int(stmt, 1);
    }

    __union_finalize(&rc, stmt, err_p);
  }

  if( rc!=DBSQL_OK ){
    __union_disconnect((dbsql_vtab_t*)tab);
    tab = 0;
  }

  *pp_vtab = (dbsql_vtab_t*)tab;
  return rc;
}

/*
** x_open
*/
static int __union_open(dbsql_vtab_t *p, dbsql_vtab_cursor_t **cursor){
  union_csr_t *csr;
  int rc = DBSQL_OK;
  (void)p;  /* Suppress harmless warning */
  csr = (union_csr_t*)__union_malloc(&rc, sizeof(union_csr_t));
  *cursor = csr ? &csr->base : 0;
  return rc;
}

/*
** x_close
*/
static int __union_close(dbsql_vtab_cursor_t *cur){
  union_csr_t *csr = (union_csr_t*)cur;
  __union_finalize_csr_stmt(csr);
  dbsql_free(csr);
  return DBSQL_OK;
}

/*
** This function does the work of the next() method. Except that, if it
** returns DBSQL_ROW, it should be called again within the same next()
** method call. See __union_next() for details.
*/
static int __do_union_next(union_csr_t *csr){
  int rc = DBSQL_OK;
  assert( csr->stmt );
  if( dbsql_step(csr->stmt)!=DBSQL_ROW ){
    union_tab_t *tab = (union_tab_t*)csr->base.vtab;
    rc = __union_finalize_csr_stmt(csr);
    if( rc==DBSQL_OK && tab->swarm ){
      csr->i_tab++;
      if( csr->i_tab<tab->n_src ){
        union_src_t *src = &tab->srcs[csr->i_tab];
        if( csr->i_max_rowid>=src->i_min ){
          /* It is necessary to scan the next table. */
          rc = __union_open_database(tab, csr->i_tab, &tab->base.errmsg);
          csr->stmt = __union_prepare_printf(&rc, &tab->base.errmsg, src->db,
              "SELECT rowid, * FROM %Q %s %lld",
              src->tab,
              (src->i_max>csr->i_max_rowid ? "WHERE _rowid_ <=" : "-- "),
              csr->i_max_rowid
          );
          if( rc==DBSQL_OK ){
            assert( csr->stmt );
            __union_incr_refcount(tab, csr->i_tab);
            rc = DBSQL_ROW;
          }
        }
      }
    }
  }

  return rc;
}

/*
** next
*/
static int __union_next(dbsql_vtab_cursor_t *cur){
  int rc;
  do {
    rc = __do_union_next((union_csr_t*)cur);
  }while( rc==DBSQL_ROW );
  return rc;
}

/*
** column
*/
static int __union_column(
  dbsql_vtab_cursor_t *cur,
  dbsql_func_t *ctx,
  int i
){
  union_csr_t *csr = (union_csr_t*)cur;
  dbsql_result_value(ctx, dbsql_column_value(csr->stmt, i+1));
  return DBSQL_OK;
}

/*
** rowid
*/
static int __union_rowid(dbsql_vtab_cursor_t *cur, int64_t *rowid){
  union_csr_t *csr = (union_csr_t*)cur;
  *rowid = dbsql_column_int64(csr->stmt, 0);
  return DBSQL_OK;
}

/*
** eof
*/
static int __union_eof(dbsql_vtab_cursor_t *cur){
  union_csr_t *csr = (union_csr_t*)cur;
  return csr->stmt==0;
}

/*
** filter
*/
static int __union_filter(
  dbsql_vtab_cursor_t *vtab_cursor, 
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  union_tab_t *tab = (union_tab_t*)(vtab_cursor->vtab);
  union_csr_t *csr = (union_csr_t*)vtab_cursor;
  int rc = DBSQL_OK;
  int i;
  char *sql = 0;
  int zero = 0;

  int64_t i_min = SMALLEST_INT64;
  int64_t i_max = LARGEST_INT64;

  assert( idx_num==0 
       || idx_num==DBSQL_INDEX_CONSTRAINT_EQ
       || idx_num==DBSQL_INDEX_CONSTRAINT_LE
       || idx_num==DBSQL_INDEX_CONSTRAINT_GE
       || idx_num==DBSQL_INDEX_CONSTRAINT_LT
       || idx_num==DBSQL_INDEX_CONSTRAINT_GT
       || idx_num==(DBSQL_INDEX_CONSTRAINT_GE|DBSQL_INDEX_CONSTRAINT_LE)
  );

  (void)idx_str;  /* Suppress harmless warning */
  
  if( idx_num==DBSQL_INDEX_CONSTRAINT_EQ ){
    assert( argc==1 );
    i_min = i_max = dbsql_value_int64(argv[0]);
  }else{

    if( idx_num & (DBSQL_INDEX_CONSTRAINT_LE|DBSQL_INDEX_CONSTRAINT_LT) ){
      assert( argc>=1 );
      i_max = dbsql_value_int64(argv[0]);
      if( idx_num & DBSQL_INDEX_CONSTRAINT_LT ){
        if( i_max==SMALLEST_INT64 ){
          zero = 1;
        }else{
          i_max--;
        }
      }
    }

    if( idx_num & (DBSQL_INDEX_CONSTRAINT_GE|DBSQL_INDEX_CONSTRAINT_GT) ){
      assert( argc>=1 );
      i_min = dbsql_value_int64(argv[argc-1]);
      if( idx_num & DBSQL_INDEX_CONSTRAINT_GT ){
        if( i_min==LARGEST_INT64 ){
          zero = 1;
        }else{
          i_min++;
        }
      }
    }
  }

  __union_finalize_csr_stmt(csr);
  if( zero ){
    return DBSQL_OK;
  }

  for(i=0; i<tab->n_src; i++){
    union_src_t *src = &tab->srcs[i];
    if( i_min>src->i_max || i_max<src->i_min ){
      continue;
    }

    sql = dbsql_mprintf("%z%sSELECT rowid, * FROM %s%q%s%Q"
        , sql
        , (sql ? " UNION ALL " : "")
        , (src->z_db ? "'" : "")
        , (src->z_db ? src->z_db : "")
        , (src->z_db ? "'." : "")
        , src->tab
    );
    if( sql==0 ){
      rc = DBSQL_NOMEM;
      break;
    }

    if( i_min==i_max ){
      sql = dbsql_mprintf("%z WHERE rowid=%lld", sql, i_min);
    }else{
      const char *where = "WHERE";
      if( i_min!=SMALLEST_INT64 && i_min>src->i_min ){
        sql = dbsql_mprintf("%z WHERE rowid>=%lld", sql, i_min);
        where = "AND";
      }
      if( i_max!=LARGEST_INT64 && i_max<src->i_max ){
        sql = dbsql_mprintf("%z %s rowid<=%lld", sql, where, i_max);
      }
    }

    if( tab->swarm ){
      csr->i_tab = i;
      csr->i_max_rowid = i_max;
      rc = __union_open_database(tab, i, &tab->base.errmsg);
      break;
    }
  }

  if( sql==0 ){
    return rc;
  }else{
    DBSQL *db = UNION_GET_DB(tab, &tab->srcs[csr->i_tab]);
    csr->stmt = __union_prepare(&rc, db, sql, &tab->base.errmsg);
    if( csr->stmt ){
      __union_incr_refcount(tab, csr->i_tab);
    }
    dbsql_free(sql);
  }
  if( rc!=DBSQL_OK ) return rc;
  return __union_next(vtab_cursor);
}

/*
** best_index.
**
** This implementation searches for constraints on the rowid field. EQ, 
** LE, LT, GE and GT are handled.
**
** If there is an EQ comparison, then idx_num is set to INDEX_CONSTRAINT_EQ.
** In this case the only argument passed to filter is the rhs of the ==
** operator.
**
** Otherwise, if an LE or LT constraint is found, then the INDEX_CONSTRAINT_LE
** or INDEX_CONSTRAINT_LT (but not both) bit is set in idx_num. The first
** argument to filter is the rhs of the <= or < operator.  Similarly, if 
** an GE or GT constraint is found, then the INDEX_CONSTRAINT_GE or
** INDEX_CONSTRAINT_GT bit is set in idx_num. The rhs of the >= or > operator
** is passed as either the first or second argument to filter, depending
** on whether or not there is also a LT|LE constraint.
*/
static int __union_best_index(
  dbsql_vtab_t *tab,
  dbsql_index_info_t *idx_info
){
  union_tab_t *p_tab = (union_tab_t*)tab;
  int i_eq = -1;
  int i_lt = -1;
  int i_gt = -1;
  int i;

  for(i=0; i<idx_info->n_constraint; i++){
    struct dbsql_index_constraint_t *p = &idx_info->constraints[i];
    if( p->usable && (p->i_column<0 || p->i_column==p_tab->i_pk) ){
      switch( p->op ){
        case DBSQL_INDEX_CONSTRAINT_EQ:
          i_eq = i;
          break;
        case DBSQL_INDEX_CONSTRAINT_LE:
        case DBSQL_INDEX_CONSTRAINT_LT:
          i_lt = i;
          break;
        case DBSQL_INDEX_CONSTRAINT_GE:
        case DBSQL_INDEX_CONSTRAINT_GT:
          i_gt = i;
          break;
      }
    }
  }

  if( i_eq>=0 ){
    idx_info->estimated_rows = 1;
    idx_info->idx_flags = DBSQL_INDEX_SCAN_UNIQUE;
    idx_info->estimated_cost = 3.0;
    idx_info->idx_num = DBSQL_INDEX_CONSTRAINT_EQ;
    idx_info->constraint_usages[i_eq].argv_index = 1;
    idx_info->constraint_usages[i_eq].omit = 1;
  }else{
    int i_cons = 1;
    int idx_num = 0;
    int64_t n_row = 1000000;
    if( i_lt>=0 ){
      n_row = n_row / 2;
      idx_info->constraint_usages[i_lt].argv_index = i_cons++;
      idx_info->constraint_usages[i_lt].omit = 1;
      idx_num |= idx_info->constraints[i_lt].op;
    }
    if( i_gt>=0 ){
      n_row = n_row / 2;
      idx_info->constraint_usages[i_gt].argv_index = i_cons++;
      idx_info->constraint_usages[i_gt].omit = 1;
      idx_num |= idx_info->constraints[i_gt].op;
    }
    idx_info->estimated_rows = n_row;
    idx_info->estimated_cost = 3.0 * (double)n_row;
    idx_info->idx_num = idx_num;
  }

  return DBSQL_OK;
}

/*
** Register the unionvtab virtual table module with database handle db.
*/
static int __create_union_vtab(DBSQL *db){
  static dbsql_module_t union_module = {
    0,                            /* i_version */
    __union_connect,
    __union_connect,
    __union_best_index,               /* best_index - query planner */
    __union_disconnect, 
    __union_disconnect,
    __union_open,                    /* x_open - open a cursor */
    __union_close,                   /* x_close - close a cursor */
    __union_filter,                  /* filter - configure scan constraints */
    __union_next,                    /* next - advance a cursor */
    __union_eof,                     /* eof - check for end of scan */
    __union_column,                  /* column - read data */
    __union_rowid,                   /* rowid - read data */
    0,                            /* update */
    0,                            /* begin */
    0,                            /* x_sync */
    0,                            /* commit */
    0,                            /* rollback */
    0,                            /* find_method */
    0,                            /* rename */
    0,                            /* savepoint */
    0,                            /* release */
    0,                            /* rollback_to */
    0,                            /* shadow_name */
    0                             /* integrity */
  };
  int rc;

  rc = dbsql_create_module(db, "unionvtab", &union_module, 0);
  if( rc==DBSQL_OK ){
    rc = dbsql_create_module(db, "swarmvtab", &union_module, (void*)db);
  }
  return rc;
}

#endif /* DBSQL_OMIT_VIRTUALTABLE */

#ifdef _WIN32
__declspec(dllexport)
#endif
int dbsql_unionvtab_init(
  DBSQL *db, 
  char **errmsg_p, 
  const dbsql_api_routines_t *api
){
  int rc = DBSQL_OK;
  DBSQL_EXTENSION_INIT2(api);
  (void)errmsg_p;  /* Suppress harmless warning */
#ifndef DBSQL_OMIT_VIRTUALTABLE
  rc = __create_union_vtab(db);
#endif
  return rc;
}
