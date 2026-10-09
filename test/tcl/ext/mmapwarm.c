/*
** 2017-09-18
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
*/

#include "dbsql.h"


/*
** This function is used to touch each page of a mapping of a memory
** mapped dbsql database. Assuming that the system has sufficient free
** memory and supports sufficiently large mappings, this causes the OS 
** to cache the entire database in main memory, making subsequent 
** database accesses faster.
**
** If the second parameter to this function is not NULL, it is the name of
** the specific database to operate on (i.e. "main" or the name of an
** attached database).
**
** DBSQL_OK is returned if successful, or an dbsql error code otherwise.
** It is not considered an error if the file is not memory-mapped, or if
** the mapping does not span the entire file. If an error does occur, a
** transaction may be left open on the database file.
**
** It is illegal to call this function when the database handle has an 
** open transaction. DBSQL_MISUSE is returned in this case.
*/
int dbsql_mmap_warm(DBSQL *db, const char *z_db){
  int rc = DBSQL_OK;
  char *sql = 0;
  int pgsz = 0;
  unsigned int n_total = 0;

  if( 0==dbsql_get_autocommit(db) ) return DBSQL_MISUSE;

  /* Open a read-only transaction on the file in question */
  sql = dbsql_mprintf("BEGIN; SELECT * FROM %s%q%sdbsql_schema", 
      (z_db ? "'" : ""), (z_db ? z_db : ""), (z_db ? "'." : "")
  );
  if( sql==0 ) return DBSQL_NOMEM;
  rc = dbsql_exec(db, sql, 0, 0, 0);
  dbsql_free(sql);

  /* Find the dbsql page size of the file */
  if( rc==DBSQL_OK ){
    sql = dbsql_mprintf("PRAGMA %s%q%spage_size", 
        (z_db ? "'" : ""), (z_db ? z_db : ""), (z_db ? "'." : "")
    );
    if( sql==0 ){
      rc = DBSQL_NOMEM;
    }else{
      dbsql_stmt_t *p_pgsz = 0;
      rc = dbsql_prepare_v2(db, sql, -1, &p_pgsz, 0);
      dbsql_free(sql);
      if( rc==DBSQL_OK ){
        if( dbsql_step(p_pgsz)==DBSQL_ROW ){
          pgsz = dbsql_column_int(p_pgsz, 0);
        }
        rc = dbsql_finalize(p_pgsz);
      }
      if( rc==DBSQL_OK && pgsz==0 ){
        rc = DBSQL_ERROR;
      }
    }
  }

  /* Touch each mmap'd page of the file */
  if( rc==DBSQL_OK ){
    int rc2;
    dbsql_file_t *fd = 0;
    rc = dbsql_file_control(db, z_db, DBSQL_FCNTL_FILE_POINTER, &fd);
    if( rc==DBSQL_OK && fd->methods->i_version>=3 ){
      int64_t i_pg = 1;
      dbsql_io_methods_t const *p = fd->methods;
      while( 1 ){
        unsigned char *map;
        rc = p->fetch(fd, pgsz*i_pg, pgsz, (void**)&map);
        if( rc!=DBSQL_OK || map==0 ) break;

        n_total += (unsigned int)map[0];
        n_total += (unsigned int)map[pgsz-1];

        rc = p->unfetch(fd, pgsz*i_pg, (void*)map);
        if( rc!=DBSQL_OK ) break;
        i_pg++;
      }
      dbsql_log(DBSQL_OK, 
          "dbsql_mmap_warm_cache: Warmed up %d pages of %s", i_pg==1?0:i_pg,
          dbsql_db_filename(db, z_db)
      );
    }

    rc2 = dbsql_exec(db, "END", 0, 0, 0);
    if( rc==DBSQL_OK ) rc = rc2;
  }

  (void)n_total;
  return rc;
}
