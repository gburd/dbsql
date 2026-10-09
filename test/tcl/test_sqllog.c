/*
** 2012 November 26
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
** OVERVIEW
**
**   This file contains experimental code used to record data from live
**   dbsql applications that may be useful for offline analysis. 
**   Specifically, this module can be used to capture the following
**   information:
**
**     1) The initial contents of all database files opened by the 
**        application, and
**
**     2) All SQL statements executed by the application.
**
**   The captured information can then be used to run (for example)
**   performance analysis looking for slow queries or to look for
**   optimization opportunities in either the application or in dbsql
**   itself.
**
** USAGE
**
**   To use this module, dbsql must be compiled with the DBSQL_ENABLE_SQLLOG
**   pre-processor symbol defined and this file linked into the application.
**   One way to link this file into the application is to append the content
**   of this file onto the end of the "DBSQL.c" amalgamation and then 
**   recompile the application as normal except with the addition  of the
**   -DSQLITE_ENABLE_SQLLOG option.
**
**   At runtime, logging is enabled by setting environment variable
**   DBSQL_SQLLOG_DIR to the name of a directory in which to store logged 
**   data. The logging directory must already exist.
**
**   Usually, if the application opens the same database file more than once
**   (either by attaching it or by using more than one database handle), only
**   a single copy is made. This behavior may be overridden (so that a 
**   separate copy is taken each time the database file is opened or attached)
**   by setting the environment variable DBSQL_SQLLOG_REUSE_FILES to 0.
**
**   If the environment variable DBSQL_SQLLOG_CONDITIONAL is defined, then
**   logging is only done for database connections if a file named
**   "<database>-sqllog" exists in the same directly as the main database
**   file when it is first opened ("<database>" is replaced by the actual 
**   name of the main database file).
**
** OUTPUT:
**
**   The DBSQL_SQLLOG_DIR is populated with three types of files:
**
**      sqllog_N.db   - Copies of database files. N may be any integer.
**
**      sqllog_N.sql  - A list of SQL statements executed by a single
**                      connection. N may be any integer.
**
**      sqllog.idx    - An index mapping from integer N to a database
**                      file name - indicating the full path of the
**                      database from which sqllog_N.db was copied.
**
** ERROR HANDLING:
**
**   This module attempts to make a best effort to continue logging if an
**   IO or other error is encountered. For example, if a log file cannot 
**   be opened logs are not collected for that connection, but other
**   logging proceeds as expected. Errors are logged by calling dbsql_log().
*/

#ifndef _DBSQL_H_
#include "dbsql.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <sys/types.h>
#include <unistd.h>
static int __get_process_id(void){
#ifdef _WIN32
  return (int)_getpid();
#else
  return (int)getpid();
#endif
}

/* Names of environment variables to be used */
#define ENVIRONMENT_VARIABLE1_NAME "DBSQL_SQLLOG_DIR"
#define ENVIRONMENT_VARIABLE2_NAME "DBSQL_SQLLOG_REUSE_FILES"
#define ENVIRONMENT_VARIABLE3_NAME "DBSQL_SQLLOG_CONDITIONAL"

/* Assume that all database and database file names are shorted than this. */
#define SQLLOG_NAMESZ 512

/* Maximum number of simultaneous database connections the process may
** open (if any more are opened an error is logged using dbsql_log()
** and processing is halted).
*/
#define MAX_CONNECTIONS 256

/* There is one instance of this object for each dbsql database connection
** that is being logged.
*/
struct sl_conn_t {
  int is_err;                      /* True if an error has occurred */
  DBSQL *db;                    /* Connection handle */
  int i_log;                       /* First integer value used in file names */
  FILE *fd;                       /* File descriptor for log file */
};

/* This object is a singleton that keeps track of all data loggers.
*/
static struct sl_global_t {
  /* Protected by MUTEX_STATIC_MAIN */
  dbsql_mutex_t *mutex;           /* Recursive mutex */
  int n_conn;                      /* Size of conns[] array */

  /* Protected by SLGlobal.mutex */
  int conditional;               /* Only trace if *-sqllog file is present */
  int reuse;                     /* True to avoid extra copies of db files */
  char prefix[SQLLOG_NAMESZ];    /* Prefix for all created files */
  char idx[SQLLOG_NAMESZ];       /* Full path to *.idx file */
  int i_next_log;                   /* Used to allocate file names */
  int i_next_db;                    /* Used to allocate database file names */
  int rec;                       /* True if __test_sqllog() is called rec. */
  int i_clock;                     /* Clock value */
  struct sl_conn_t conns[MAX_CONNECTIONS];
} sqllogglobal;

/*
** Return true if c is an ASCII whitespace character.
*/
static int sqllog_isspace(char c){
  return (c==' ' || c=='\t' || c=='\n' || c=='\v' || c=='\f' || c=='\r');
}

/*
** The first argument points to a nul-terminated string containing an SQL
** command. Before returning, this function sets *pz to point to the start
** of the first token in this command, and *pn to the number of bytes in 
** the token. This is used to check if the SQL command is an "ATTACH" or 
** not.
*/
static void __sqllog_tokenize(const char *z, const char **pz, int *pn){
  const char *p = z;
  int n;

  /* Skip past any whitespace */
  while( sqllog_isspace(*p) ){
    p++;
  }

  /* Figure out how long the first token is */
  *pz = p;
  n = 0;
  while( (p[n]>='a' && p[n]<='z') || (p[n]>='A' && p[n]<='Z') ) n++;
  *pn = n;
}

/*
** Check if the logs directory already contains a copy of database file 
** file. If so, return a pointer to the full path of the copy. Otherwise,
** return NULL.
**
** If a non-NULL value is returned, then the caller must arrange to 
** eventually free it using dbsql_free().
*/
static char *__sqllog_find_file(const char *file){
  char *ret = 0;
  FILE *fd = 0;

  /* Open the index file for reading */
  fd = fopen(sqllogglobal.idx, "r");
  if( fd==0 ){
    dbsql_log(DBSQL_IOERR, "sqllogFindFile(): error in fopen()");
    return 0;
  }

  /* Loop through each entry in the index file. If file is not NULL and the
  ** entry is a match, then set ret to point to the filename of the existing
  ** copy and break out of the loop.  */
  while( feof(fd)==0 ){
    char line[SQLLOG_NAMESZ*2+5];
    if( fgets(line, sizeof(line), fd) ){
      int n;
      char *z;

      line[sizeof(line)-1] = '\0';
      z = line;
      while( *z>='0' && *z<='9' ) z++;
      while( *z==' ' ) z++;

      n = strlen(z);
      while( n>0 && sqllog_isspace(z[n-1]) ) n--;

      if( n==strlen(file) && 0==memcmp(file, z, n) ){
        char buf[16];
        memset(buf, 0, sizeof(buf));
        z = line;
        while( *z>='0' && *z<='9' ){
          buf[z-line] = *z;
          z++;
        }
        ret = dbsql_mprintf("%s_%s.db", sqllogglobal.prefix, buf);
        break;
      }
    }
  }

  if( ferror(fd) ){
    dbsql_log(DBSQL_IOERR, "sqllogFindFile(): error reading index file");
  }

  fclose(fd);
  return ret;
}

static int __sqllog_find_attached(
  DBSQL *db,                    /* Database connection */
  const char *search,            /* Name to search for (or NULL) */
  char *name,                    /* OUT: Name of attached database */
  char *file                     /* OUT: Name of attached file */
){
  dbsql_stmt_t *stmt;
  int rc;

  /* The "PRAGMA database_list" command returns a list of databases in the
  ** order that they were attached. So a newly attached database is 
  ** described by the last row returned.  */
  assert( sqllogglobal.rec==0 );
  sqllogglobal.rec = 1;
  rc = dbsql_prepare_v2(db, "PRAGMA database_list", -1, &stmt, 0);
  if( rc==DBSQL_OK ){
    while( DBSQL_ROW==dbsql_step(stmt) ){
      const char *val1; int n_val1;
      const char *val2; int n_val2;

      val1 = (const char*)dbsql_column_text(stmt, 1);
      n_val1 = dbsql_column_bytes(stmt, 1);
      if( name ){
        memcpy(name, val1, n_val1+1);
      }

      val2 = (const char*)dbsql_column_text(stmt, 2);
      n_val2 = dbsql_column_bytes(stmt, 2);
      memcpy(file, val2, n_val2+1);

      if( search && strlen(search)==n_val1 
       && 0==dbsql_strnicmp(search, val1, n_val1)
      ){
        break;
      }
    }
    rc = dbsql_finalize(stmt);
  }
  sqllogglobal.rec = 0;

  if( rc!=DBSQL_OK ){
    dbsql_log(rc, "sqllogFindAttached(): error in \"PRAGMA database_list\"");
  }
  return rc;
}


/*
** Parameter search is the name of a database attached to the database 
** connection associated with the first argument. This function creates
** a backup of this database in the logs directory.
**
** The name used for the backup file is automatically generated. Call
** it file.
**
** If the b_log parameter is true, then a statement of the following form
** is written to the log file associated with *p:
**
**    ATTACH 'file' AS 'name';
**
** Otherwise, if b_log is false, a comment is added to the log file:
**
**    -- Main database file is 'file'
**
** The SLGlobal.mutex mutex is always held when this function is called.
*/
static void __sqllog_copydb(struct sl_conn_t *p, const char *search, int b_log){
  char name[SQLLOG_NAMESZ];      /* Attached database name */
  char file[SQLLOG_NAMESZ];      /* Database file name */
  char *z_free;
  char *init = 0;
  int rc;

  rc = __sqllog_find_attached(p->db, search, name, file);
  if( rc!=DBSQL_OK ) return;

  if( file[0]=='\0' ){
    init = dbsql_mprintf("");
  }else{
    if( sqllogglobal.reuse ){
      init = __sqllog_find_file(file);
    }else{
      init = 0;
    }
    if( init==0 ){
      int rc;
      DBSQL *copy = 0;
      int i_db;

      /* Generate a file-name to use for the copy of this database */
      i_db = sqllogglobal.i_next_db++;
      init = dbsql_mprintf("%s_%02d.db", sqllogglobal.prefix, i_db);

      /* Create the backup */
      assert( sqllogglobal.rec==0 );
      sqllogglobal.rec = 1;
      rc = dbsql_open(init, &copy);
      if( rc==DBSQL_OK ){
        dbsql_backup_t *bak;
        dbsql_exec(copy, "PRAGMA synchronous = 0", 0, 0, 0);
        bak = dbsql_backup_init(copy, "main", p->db, name);
        if( bak ){
          dbsql_backup_step(bak, -1);
          rc = dbsql_backup_finish(bak);
        }else{
          rc = dbsql_errcode(copy);
        }
        dbsql_close(copy);
      }
      sqllogglobal.rec = 0;

      if( rc==DBSQL_OK ){
        /* Write an entry into the database index file */
        FILE *fd = fopen(sqllogglobal.idx, "a");
        if( fd ){
          fprintf(fd, "%d %s\n", i_db, file);
          fclose(fd);
        }
      }else{
        dbsql_log(rc, "sqllogCopydb(): error backing up database");
      }
    }
  }

  if( b_log ){
    z_free = dbsql_mprintf("ATTACH '%q' AS '%q'; -- clock=%d\n", 
        init, name, sqllogglobal.i_clock++
    );
  }else{
    z_free = dbsql_mprintf("-- Main database is '%q'\n", init);
  }
  fprintf(p->fd, "%s", z_free);
  dbsql_free(z_free);

  dbsql_free(init);
}

/*
** If it is not already open, open the log file for connection *p. 
**
** The SLGlobal.mutex mutex is always held when this function is called.
*/
static void __sqllog_openlog(struct sl_conn_t *p){
  /* If the log file has not yet been opened, open it now. */
  if( p->fd==0 ){
    char *z_log;

    /* If it is still NULL, have global.prefix point to a copy of 
    ** environment variable $ENVIRONMENT_VARIABLE1_NAME.  */
    if( sqllogglobal.prefix[0]==0 ){
      FILE *fd;
      char *var = getenv(ENVIRONMENT_VARIABLE1_NAME);
      if( var==0 || strlen(var)+10>=(sizeof(sqllogglobal.prefix)) ) return;
      dbsql_snprintf(sizeof(sqllogglobal.prefix), sqllogglobal.prefix,
                        "%s/sqllog_%05d", var, __get_process_id());
      dbsql_snprintf(sizeof(sqllogglobal.idx), sqllogglobal.idx,
                        "%s.idx", sqllogglobal.prefix);
      if( getenv(ENVIRONMENT_VARIABLE2_NAME) ){
        sqllogglobal.reuse = atoi(getenv(ENVIRONMENT_VARIABLE2_NAME));
      }
      fd = fopen(sqllogglobal.idx, "w");
      if( fd ) fclose(fd);
    }

    /* Open the log file */
    z_log = dbsql_mprintf("%s_%05d.sql", sqllogglobal.prefix, p->i_log);
    p->fd = fopen(z_log, "w");
    dbsql_free(z_log);
    if( p->fd==0 ){
      dbsql_log(DBSQL_IOERR, "sqllogOpenlog(): Failed to open log file");
    }
  }
}

/*
** This function is called if the SQLLOG callback is invoked to report
** execution of an SQL statement. Parameter p is the connection the statement
** was executed by and parameter sql is the text of the statement itself.
*/
static void __test_sqllog_stmt(struct sl_conn_t *p, const char *sql){
  const char *first;             /* Pointer to first token in sql */
  int n_first;                     /* Size of token first in bytes */

  __sqllog_tokenize(sql, &first, &n_first);
  if( n_first!=6 || 0!=dbsql_strnicmp("ATTACH", first, 6) ){
    /* Not an ATTACH statement. Write this directly to the log. */
    fprintf(p->fd, "%s; -- clock=%d\n", sql, sqllogglobal.i_clock++);
  }else{
    /* This is an ATTACH statement. Copy the database. */
    __sqllog_copydb(p, 0, 1);
  }
}

/*
** The database handle passed as the only argument has just been opened.
** Return true if this module should log initial databases and SQL 
** statements for this connection, or false otherwise.
**
** If an error occurs, dbsql_log() is invoked to report it to the user
** and zero returned.
*/
static int __sqllog_trace_db(DBSQL *db){
  int ret = 1;
  if( sqllogglobal.conditional ){
    char file[SQLLOG_NAMESZ];      /* Attached database name */
    int rc = __sqllog_find_attached(db, "main", 0, file);
    if( rc==DBSQL_OK ){
      int n_file = strlen(file);
      if( (SQLLOG_NAMESZ-n_file)<8 ){
        dbsql_log(DBSQL_IOERR, 
            "sqllogTraceDb(): database name too long (%d bytes)", n_file
        );
        ret = 0;
      }else{
        memcpy(&file[n_file], "-sqllog", 8);
        ret = !access(file, F_OK);
      }
    }
  }
  return ret;
}

/*
** The DBSQL_CONFIG_SQLLOG callback registered by dbsql_init_sqllog().
**
** The type parameter has the following values:
**
**    0:  Opening a new database connection.  sql is the name of the
**        file being opened.  db is a pointer to the newly created database
**        connection.
**
**    1:  An SQL statement has run to completion.  sql is the text of the
**        SQL statement with all parameters expanded to their actual values.
**
**    2:  Closing a database connection.  sql is NULL.  The db pointer to
**        the database connection being closed has already been shut down
**        and cannot be used for any further SQL.
**
** The ctx parameter is a copy of the pointer that was originally passed
** into the dbsql_config(DBSQL_CONFIG_SQLLOG) statement.  In this
** particular implementation, ctx is always a pointer to the 
** sqllogglobal global variable define above.
*/
static void __test_sqllog(void *ctx, DBSQL *db, const char *sql, int type){
  struct sl_conn_t *p = 0;
  dbsql_mutex_t *mainmtx = dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_MAIN);

  assert( type==0 || type==1 || type==2 );
  assert( (type==2)==(sql==0) );

  /* This is a database open command. */
  if( type==0 ){
    dbsql_mutex_enter(mainmtx);
    if( sqllogglobal.mutex==0 ){
      sqllogglobal.mutex = dbsql_mutex_alloc(DBSQL_MUTEX_RECURSIVE);
    }
    dbsql_mutex_leave(mainmtx);

    dbsql_mutex_enter(sqllogglobal.mutex);
    if( sqllogglobal.rec==0 && __sqllog_trace_db(db) ){

      dbsql_mutex_enter(mainmtx);
      p = &sqllogglobal.conns[sqllogglobal.n_conn++];
      p->fd = 0;
      p->db = db;
      p->i_log = sqllogglobal.i_next_log++;
      dbsql_mutex_leave(mainmtx);

      /* Open the log and take a copy of the main database file */
      __sqllog_openlog(p);
      if( p->fd ) __sqllog_copydb(p, "main", 0);
    }
    dbsql_mutex_leave(sqllogglobal.mutex);
  }

  else{

    int i;
    for(i=0; i<sqllogglobal.n_conn; i++){
      p = &sqllogglobal.conns[i];
      if( p->db==db ) break;
    }

    /* A database handle close command */
    if( type==2 ){
      dbsql_mutex_enter(mainmtx);
      if( i<sqllogglobal.n_conn ){
        if( p->fd ) fclose(p->fd);
        p->db = 0;
        p->fd = 0;
        sqllogglobal.n_conn--;
      }

      if( sqllogglobal.n_conn==0 ){
        dbsql_mutex_free(sqllogglobal.mutex);
        sqllogglobal.mutex = 0;
      }else if( i<sqllogglobal.n_conn ){
        int n_shift = &sqllogglobal.conns[sqllogglobal.n_conn] - p;
        if( n_shift>0 ){
          memmove(p, &p[1], n_shift*sizeof(struct sl_conn_t));
        }
      }
      dbsql_mutex_leave(mainmtx);

    /* An ordinary SQL command. */
    }else if( i<sqllogglobal.n_conn && p->fd ){
      dbsql_mutex_enter(sqllogglobal.mutex);
      if( sqllogglobal.rec==0 ){
        __test_sqllog_stmt(p, sql);
      }
      dbsql_mutex_leave(sqllogglobal.mutex);
    }
  }
}

/*
** This function is called either before dbsql_initialized() or by it.
** It checks if the DBSQL_SQLLOG_DIR variable is defined, and if so 
** registers an DBSQL_CONFIG_SQLLOG callback to record the applications
** database activity.
*/
void dbsql_init_sqllog(void){
  if( getenv(ENVIRONMENT_VARIABLE1_NAME) ){
    if( DBSQL_OK==dbsql_config(DBSQL_CONFIG_SQLLOG, __test_sqllog, 0) ){
      memset(&sqllogglobal, 0, sizeof(sqllogglobal));
      sqllogglobal.reuse = 1;
      if( getenv(ENVIRONMENT_VARIABLE3_NAME) ){
        sqllogglobal.conditional = 1;
      }
    }
  }
}
