/*
** 2016 September 10
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains test code to delete an dbsql database and all
** of its associated files. Associated files include:
**
**   * The journal file.
**   * The wal file.
**   * The DBSQL_ENABLE_8_3_NAMES version of the db, journal or wal files.
**   * Files created by the test_multiplex.c module to extend any of the 
**     above.
*/

#ifndef DBSQL_OS_WIN
#  include <unistd.h>
#  include <errno.h>
#endif
#include <string.h>
#include <assert.h>
#include "dbsql.h"

/* The following #defines are copied from test_multiplex.c */
#ifndef MX_CHUNK_NUMBER 
# define MX_CHUNK_NUMBER 299
#endif
#ifndef DBSQL_MULTIPLEX_JOURNAL_8_3_OFFSET
# define DBSQL_MULTIPLEX_JOURNAL_8_3_OFFSET 400
#endif
#ifndef DBSQL_MULTIPLEX_WAL_8_3_OFFSET
# define DBSQL_MULTIPLEX_WAL_8_3_OFFSET 700
#endif

/*
** This routine is a copy of (most of) the code from dbsql function
** __file_suffix3(). It modifies the filename in buffer z in the
** same way as dbsql does when in 8.3 filenames mode.
*/
static void __delete83_name(char *z){
  int i, sz;
  sz = (int)strlen(z);
  for(i=sz-1; i>0 && z[i]!='/' && z[i]!='.'; i--){}
  if( z[i]=='.' && (sz>i+4) ) memmove(&z[i+1], &z[sz-3], 4);
}

/*
** file is a filename. Assuming no error occurs, if this file exists, 
** set *pb_exists to true and unlink it. Or, if the file does not exist,
** set *pb_exists to false before returning.
**
** If an error occurs, non-zero is returned. Or, if no error occurs, zero.
*/
static int __delete_unlink_if_exists(
  dbsql_vfs_t *vfs,
  const char *file, 
  int *pb_exists
){
  int rc = DBSQL_ERROR;
#ifdef _WIN32
  if( vfs ){
    if( pb_exists ) *pb_exists = 1;
    rc = vfs->x_delete(vfs, file, 0);
    if( rc==DBSQL_IOERR_DELETE_NOENT ){
      if( pb_exists ) *pb_exists = 0;
      rc = DBSQL_OK;
    }
  }
#else
  assert( vfs==0 );
  rc = access(file, F_OK);
  if( rc ){
    if( errno==ENOENT ){ 
      if( pb_exists ) *pb_exists = 0;
      rc = DBSQL_OK; 
    }
  }else{
    if( pb_exists ) *pb_exists = 1;
    rc = unlink(file);
  }
#endif
  return rc;
}

/*
** Delete the database file identified by the string argument passed to this
** function. The string must contain a filename, not an dbsql URI.
*/
DBSQL_API int dbsql_delete_database(
  const char *file               /* File to delete */
){
  char *buf;                     /* Buffer to sprintf() filenames to */
  int n_buf;                       /* Size of buffer in bytes */
  int rc = 0;                     /* System error code */
  int i;                          /* Iterate through fmts[] and m_files[] */

  const char *fmts[] = { "%s", "%s-journal", "%s-wal", "%s-shm" };

  struct m_file_t {
    const char *fmt;
    int i_offset;
    int b83;
  } m_files[] = {
    { "%s%03d",         0,   0 },
    { "%s-journal%03d", 0,   0 },
    { "%s-wal%03d",     0,   0 },
    { "%s%03d",         0,   1 },
    { "%s-journal%03d", DBSQL_MULTIPLEX_JOURNAL_8_3_OFFSET, 1 },
    { "%s-wal%03d",     DBSQL_MULTIPLEX_WAL_8_3_OFFSET, 1 },
  };

#ifdef _WIN32
  dbsql_vfs_t *vfs = dbsql_vfs_find("win32");
#else
  dbsql_vfs_t *vfs = 0;
#endif

  /* Allocate a buffer large enough for any of the files that need to be
  ** deleted.  */
  n_buf = (int)strlen(file) + 100;
  buf = (char*)dbsql_malloc(n_buf);
  if( buf==0 ) return DBSQL_NOMEM;

  /* Delete both the regular and 8.3 filenames versions of the database,
  ** journal, wal and shm files.  */
  for(i=0; rc==0 && i<sizeof(fmts)/sizeof(fmts[0]); i++){
    dbsql_snprintf(n_buf, buf, fmts[i], file);
    rc = __delete_unlink_if_exists(vfs, buf, 0);
    if( rc==0 && i!=0 ){
      __delete83_name(buf);
      rc = __delete_unlink_if_exists(vfs, buf, 0);
    }
  }

  /* Delete any multiplexor files */
  for(i=0; rc==0 && i<sizeof(m_files)/sizeof(m_files[0]); i++){
    struct m_file_t *p = &m_files[i];
    int i_chunk;
    for(i_chunk=1; i_chunk<=MX_CHUNK_NUMBER; i_chunk++){
      int exists;
      dbsql_snprintf(n_buf, buf, p->fmt, file, i_chunk+p->i_offset);
      if( p->b83 ) __delete83_name(buf);
      rc = __delete_unlink_if_exists(vfs, buf, &exists);
      if( exists==0 || rc!=0 ) break;
    }
  }

  dbsql_free(buf);
  return (rc ? DBSQL_ERROR : DBSQL_OK);
}
