/*
** 2003 April 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to implement the VACUUM command.
**
** Most of the code in this file may be omitted by defining the
** DBSQL_OMIT_VACUUM macro.
*/
#include "dbsql_int.h"
#include "vdbe_int.h"

#if !defined(DBSQL_OMIT_VACUUM) && !defined(DBSQL_OMIT_ATTACH)

/*
** Execute sql on database db.
**
** If sql returns rows, then each row will have exactly one
** column.  (This will only happen if sql begins with "SELECT".)
** Take each row of result and call __exec_sql() again recursively.
**
** The __exec_sql_f() routine does the same thing, except it accepts
** a format string as its third argument
*/
static int __exec_sql(DBSQL *db, char **errmsg_p, const char *sql){
  dbsql_stmt_t *stmt;
  int rc;

  /* printf("SQL: [%s]\n", sql); fflush(stdout); */
  rc = dbsql_prepare_v2(db, sql, -1, &stmt, 0);
  if( rc!=DBSQL_OK ) return rc;
  while( DBSQL_ROW==(rc = dbsql_step(stmt)) ){
    const char *sub_sql = (const char*)dbsql_column_text(stmt,0);
    assert( dbsql_strnicmp(sql,"SELECT",6)==0 );
    /* The secondary SQL must be one of CREATE TABLE, CREATE INDEX,
    ** or INSERT.  Historically there have been attacks that first
    ** corrupt the dbsql_schema.sql field with other kinds of statements
    ** then run VACUUM to get those statements to execute at inappropriate
    ** times. */
    if( sub_sql
     && (strncmp(sub_sql,"CRE",3)==0 || strncmp(sub_sql,"INS",3)==0)
    ){
      rc = __exec_sql(db, errmsg_p, sub_sql);
      if( rc!=DBSQL_OK ) break;
    }
  }
  assert( rc!=DBSQL_ROW );
  if( rc==DBSQL_DONE ) rc = DBSQL_OK;
  if( rc ){
    __set_string(errmsg_p, db, dbsql_errmsg(db));
  }
  (void)dbsql_finalize(stmt);
  return rc;
}
static int __exec_sql_f(DBSQL *db, char **errmsg_p, const char *sql, ...){
  char *z;
  va_list ap;
  int rc;
  va_start(ap, sql);
  z = __vm_printf(db, sql, ap);
  va_end(ap);
  if( z==0 ) return DBSQL_NOMEM;
  rc = __exec_sql(db, errmsg_p, z);
  __dbsql_db_free(db, z);
  return rc;
}

/*
** The VACUUM command is used to clean up the database,
** collapse free space, etc.  It is modelled after the VACUUM command
** in __postgre_sql.  The VACUUM command works as follows:
**
**   (1)  Create a new transient database file
**   (2)  Copy all content from the database being vacuumed into
**        the new transient database file
**   (3)  Copy content from the transient database back into the
**        original database.
**
** The transient database requires temporary disk space approximately
** equal to the size of the original database.  The copy operation of
** step (3) requires additional temporary disk space approximately equal
** to the size of the original database for the rollback journal.
** Hence, temporary disk space that is approximately 2x the size of the
** original database is required.  Every page of the database is written
** approximately 3 times:  Once for step (2) and twice for step (3).
** Two writes per page are required in step (3) because the original
** database content must be written into the rollback journal prior to
** overwriting the database with the vacuumed content.
**
** Only 1x temporary space and only 1x writes would be required if
** the copy of step (3) were replaced by deleting the original database
** and renaming the transient database as the original.  But that will
** not work if other processes are attached to the original database.
** And a power loss in between deleting the original and renaming the
** transient would cause the database file to appear to be deleted
** following reboot.
*/
void __vacuum(parser_t *parser, Token *nm, expr_t *into){
  vdbe_t *v = __get_vdbe(parser);
  int i_db = 0;
  if( v==0 ) goto build_vacuum_end;
  if( parser->n_err ) goto build_vacuum_end;
  if( nm ){
#ifndef DBSQL_BUG_COMPATIBLE_20160819
    /* Default behavior:  Report an error if the argument to VACUUM is
    ** not recognized */
    i_db = __two_part_name(parser, nm, nm, &nm);
    if( i_db<0 ) goto build_vacuum_end;
#else
    /* When DBSQL_BUG_COMPATIBLE_20160819 is defined, unrecognized arguments
    ** to VACUUM are silently ignored.  This is a back-out of a bug fix that
    ** occurred on 2016-08-19 (https://sqlite.org/src/info/083f9e6270).
    ** The buggy behavior is required for binary compatibility with some
    ** legacy applications. */
    i_db = __find_db(parser->db, nm);
    if( i_db<0 ) i_db = 0;
#endif
  }
  if( i_db!=1 ){
    int i_into_reg = 0;
    if( into && __resolve_self_reference(parser,0,0,into,0)==0 ){
      i_into_reg = ++parser->n_mem;
      __expr_code(parser, into, i_into_reg);
    }
    __vdbe_add_op2(v, OP_Vacuum, i_db, i_into_reg);
    __vdbe_uses_btree(v, i_db);
  }
build_vacuum_end:
  __expr_delete(parser->db, into);
  return;
}

/*
** This routine implements the OP_Vacuum opcode of the VDBE.
*/
DBSQL_NOINLINE int __run_vacuum(
  char **errmsg_p,        /* Write error message here */
  DBSQL *db,            /* Database connection */
  int i_db,                /* Which attached DB to vacuum */
  dbsql_value_t *out     /* Write results here, if not NULL. VACUUM INTO */
){
  int rc = DBSQL_OK;     /* Return code from service routines */
  sm_t *p_main;           /* The database being vacuumed */
  sm_t *temp;           /* The temporary database we vacuum into */
  u_int32_t saved_m_db_flags;     /* Saved value of db->m_db_flags */
  u_int64_t saved_flags;        /* Saved value of db->flags */
  int64_t saved_n_change;      /* Saved value of db->n_change */
  int64_t saved_n_total_change; /* Saved value of db->n_total_change */
  u_int32_t saved_open_flags;    /* Saved value of db->open_flags */
  u_int8_t saved_m_trace;        /* Saved trace settings */
  db_t *p_db = 0;            /* Database to detach at end of vacuum */
  int is_mem_db;            /* True if vacuuming a :memory: database */
  int n_res;               /* Bytes of reserved space at the end of each page */
  int n_db;                /* Number of attached databases */
  const char *db_main;    /* schema_t name of database to vacuum */
  const char *z_out;       /* Name of output file */
  u_int32_t pgflags = PAGER_SYNCHRONOUS_OFF; /* sync flags for output db */
  u_int64_t i_random;            /* Random value used for db_vacuum[] */
  char db_vacuum[42];     /* Name of the ATTACH-ed database used for vacuum */


  if( !db->auto_commit ){
    __set_string(errmsg_p, db, "cannot VACUUM from within a transaction");
    return DBSQL_ERROR; /* IMP: R-12218-18073 */
  }
  if( db->n_vdbe_active>1 ){
    __set_string(errmsg_p, db,"cannot VACUUM - SQL statements in progress");
    return DBSQL_ERROR; /* IMP: R-15610-35227 */
  }
  saved_open_flags = db->open_flags;
  if( out ){
    if( dbsql_value_type(out)!=DBSQL_TEXT ){
      __set_string(errmsg_p, db, "non-text filename");
      return DBSQL_ERROR;
    }
    z_out = (const char*)dbsql_value_text(out);
    db->open_flags &= ~DBSQL_OPEN_READONLY;
    db->open_flags |= DBSQL_OPEN_CREATE|DBSQL_OPEN_READWRITE;
  }else{
    z_out = "";
  }

  /* Save the current value of the database flags so that it can be 
  ** restored before returning. Then set the writable-schema flag, and
  ** disable CHECK and foreign key constraints.  */
  saved_flags = db->flags;
  saved_m_db_flags = db->m_db_flags;
  saved_n_change = db->n_change;
  saved_n_total_change = db->n_total_change;
  saved_m_trace = db->m_trace;
  db->flags |= DBSQL_WriteSchema | DBSQL_IgnoreChecks | DBSQL_Comments
               | DBSQL_AttachCreate | DBSQL_AttachWrite;
  db->m_db_flags |= DBFLAG_PreferBuiltin | DBFLAG_Vacuum;
  db->flags &= ~(u_int64_t)(DBSQL_ForeignKeys | DBSQL_ReverseOrder
                   | DBSQL_Defensive | DBSQL_CountRows);
  db->m_trace = 0;

  db_main = db->dbs[i_db].db_s_name;
  p_main = db->dbs[i_db].bt;
  is_mem_db = __sm_pager_is_memdb(__sm_pager(p_main));

  /* Attach the temporary database as 'vacuum_XXXXXX'. The synchronous pragma
  ** can be set to 'off' for this file, as it is not recovered if a crash
  ** occurs anyway. The integrity of the database is maintained by a
  ** (possibly synchronous) transaction opened on the main database before
  ** __sm_copy_file() is called.
  **
  ** An optimization would be to use a non-journaled pager.
  ** (Later:) I tried setting "PRAGMA vacuum_XXXXXX.journal_mode=OFF" but
  ** that actually made the VACUUM run slower.  Very little journalling
  ** actually occurs when doing a vacuum since the vacuum_db is initially
  ** empty.  Only the journal header is written.  Apparently it takes more
  ** time to parse and run the PRAGMA to turn journalling off than it does
  ** to write the journal header file.
  */
  dbsql_randomness(sizeof(i_random),&i_random);
  dbsql_snprintf(sizeof(db_vacuum), db_vacuum, "vacuum_%016llx", i_random);
  n_db = db->n_db;
  rc = __exec_sql_f(db, errmsg_p, "ATTACH %Q AS %s", z_out, db_vacuum);
  db->open_flags = saved_open_flags;
  if( rc!=DBSQL_OK ) goto end_of_vacuum;
  assert( (db->n_db-1)==n_db );
  p_db = &db->dbs[n_db];
  assert( strcmp(p_db->db_s_name,db_vacuum)==0 );
  temp = p_db->bt;
  n_res = __sm_get_requested_reserve(p_main);
  if( out ){
    dbsql_file_t *id = __sm_pager_file(__sm_pager(temp));
    int64_t sz = 0;
    const char *filename;
    if( id->methods!=0 && (__dbsql_os_file_size(id, &sz)!=DBSQL_OK || sz>0) ){
      rc = DBSQL_ERROR;
      __set_string(errmsg_p, db, "output file already exists");
      goto end_of_vacuum;
    }
    db->m_db_flags |= DBFLAG_VacuumInto;

    /* For a VACUUM INTO, the pager-flags are set to the same values as
    ** they are for the database being vacuumed, except that PAGER_CACHESPILL
    ** is always set. */
    pgflags = db->dbs[i_db].safety_level | (db->flags & PAGER_FLAGS_MASK);

    /* If the VACUUM INTO target file is a URI filename and if the
    ** "reserve=N" query parameter is present, reset the reserve to the
    ** amount specified, if the amount is within range */
    filename = __sm_get_filename(temp);
    if( ALWAYS(filename) ){
      int n_new = (int)dbsql_uri_int64(filename, "reserve", n_res);
      if( n_new>=0 && n_new<=255 ) n_res = n_new;
    }
  }

  __sm_set_cache_size(temp, db->dbs[i_db].schema->cache_size);
  __sm_set_spill_size(temp, __sm_set_spill_size(p_main,0));
  __sm_set_pager_flags(temp, pgflags|PAGER_CACHESPILL);

  /* Begin a transaction and take an exclusive lock on the main database
  ** file. This is done before the __sm_get_page_size(p_main) call below,
  ** to ensure that we do not try to change the page-size on a WAL database.
  */
  rc = __exec_sql(db, errmsg_p, "BEGIN");
  if( rc!=DBSQL_OK ) goto end_of_vacuum;
  rc = __sm_begin_trans(p_main, out==0 ? 2 : 0, 0);
  if( rc!=DBSQL_OK ) goto end_of_vacuum;

  /* Do not attempt to change the page size for a WAL database */
  if( __sm_pager_get_journal_mode(__sm_pager(p_main))
                                               ==PAGER_JOURNALMODE_WAL
   && out==0
  ){
    db->next_pagesize = 0;
  }

  if( __sm_set_page_size(temp, __sm_get_page_size(p_main), n_res, 0)
   || (!is_mem_db && __sm_set_page_size(temp, db->next_pagesize, n_res, 0))
   || NEVER(db->malloc_failed)
  ){
    rc = DBSQL_NOMEM_BKPT;
    goto end_of_vacuum;
  }

#ifndef DBSQL_OMIT_AUTOVACUUM
  __sm_set_auto_vacuum(temp, db->next_autovac>=0 ? db->next_autovac :
                                           __sm_get_auto_vacuum(p_main));
#endif

  /* Query the schema of the main database. Create a mirror schema
  ** in the temporary database.
  */
  db->init.i_db = n_db; /* force new CREATE statements into vacuum_db */
  rc = __exec_sql_f(db, errmsg_p,
      "SELECT sql FROM \"%w\".dbsql_schema"
      " WHERE type='table'AND name<>'dbsql_sequence'"
      " AND coalesce(rootpage,1)>0",
      db_main
  );
  if( rc!=DBSQL_OK ) goto end_of_vacuum;
  rc = __exec_sql_f(db, errmsg_p,
      "SELECT sql FROM \"%w\".dbsql_schema"
      " WHERE type='index'",
      db_main
  );
  if( rc!=DBSQL_OK ) goto end_of_vacuum;
  db->init.i_db = 0;

  /* Loop through the tables in the main database. For each, do
  ** an "INSERT INTO vacuum_db.xxx SELECT * FROM main.xxx;" to copy
  ** the contents to the temporary database.
  */
  rc = __exec_sql_f(db, errmsg_p,
      "SELECT'INSERT INTO %s.'||quote(name)"
      "||' SELECT*FROM\"%w\".'||quote(name)"
      "FROM %s.dbsql_schema "
      "WHERE type='table'AND coalesce(rootpage,1)>0",
      db_vacuum, db_main, db_vacuum
  );
  assert( (db->m_db_flags & DBFLAG_Vacuum)!=0 );
  db->m_db_flags &= ~DBFLAG_Vacuum;
  if( rc!=DBSQL_OK ) goto end_of_vacuum;

  /* Copy the triggers, views, and virtual tables from the main database
  ** over to the temporary database.  None of these objects has any
  ** associated storage, so all we have to do is copy their entries
  ** from the schema table.
  */
  rc = __exec_sql_f(db, errmsg_p,
      "INSERT INTO %s.dbsql_schema"
      " SELECT*FROM \"%w\".dbsql_schema"
      " WHERE type IN('view','trigger')"
      " OR(type='table'AND rootpage=0)",
      db_vacuum, db_main
  );
  if( rc ) goto end_of_vacuum;

  /* At this point, there is a write transaction open on both the 
  ** vacuum database and the main database. Assuming no error occurs,
  ** both transactions are closed by this block - the main database
  ** transaction by __sm_copy_file() and the other by an explicit
  ** call to __sm_commit().
  */
  {
    u_int32_t meta;
    int i;

    /* This array determines which meta meta values are preserved in the
    ** vacuum.  Even entries are the meta value number and odd entries
    ** are an increment to apply to the meta value after the vacuum.
    ** The increment is used to increase the schema cookie so that other
    ** connections to the same database will know to reread the schema.
    */
    static const unsigned char copies[] = {
       BTREE_SCHEMA_VERSION,     1,  /* Add one to the old schema cookie */
       BTREE_DEFAULT_CACHE_SIZE, 0,  /* Preserve the default page cache size */
       BTREE_TEXT_ENCODING,      0,  /* Preserve the text encoding */
       BTREE_USER_VERSION,       0,  /* Preserve the user version */
       BTREE_APPLICATION_ID,     0,  /* Preserve the application id */
    };

    assert( DBSQL_TXN_WRITE==__sm_txn_state(temp) );
    assert( out!=0 || DBSQL_TXN_WRITE==__sm_txn_state(p_main) );

    /* Copy sm_t meta values */
    for(i=0; i<ARRAY_SIZE(copies); i+=2){
      /* __get_meta() and __update_meta() cannot fail in this context because
      ** we already have page 1 loaded into cache and marked dirty. */
      __sm_get_meta(p_main, copies[i], &meta);
      rc = __sm_update_meta(temp, copies[i], meta+copies[i+1]);
      if( NEVER(rc!=DBSQL_OK) ) goto end_of_vacuum;
    }

    if( out==0 ){
      rc = __sm_copy_file(p_main, temp);
    }
    if( rc!=DBSQL_OK ) goto end_of_vacuum;
    rc = __sm_commit(temp);
    if( rc!=DBSQL_OK ) goto end_of_vacuum;
#ifndef DBSQL_OMIT_AUTOVACUUM
    if( out==0 ){
      __sm_set_auto_vacuum(p_main, __sm_get_auto_vacuum(temp));
    }
#endif
  }

  assert( rc==DBSQL_OK );
  if( out==0 ){
    n_res = __sm_get_requested_reserve(temp);
    rc = __sm_set_page_size(p_main, __sm_get_page_size(temp), n_res,1);
  }

end_of_vacuum:
  /* Restore the original value of db->flags */
  db->init.i_db = 0;
  db->m_db_flags = saved_m_db_flags;
  db->flags = saved_flags;
  db->n_change = saved_n_change;
  db->n_total_change = saved_n_total_change;
  db->m_trace = saved_m_trace;
  __sm_set_page_size(p_main, -1, 0, 1);

  /* Currently there is an SQL level transaction open on the vacuum
  ** database. No locks are held on any other files (since the main file
  ** was committed at the btree level). So it safe to end the transaction
  ** by manually setting the auto_commit flag to true and detaching the
  ** vacuum database. The vacuum_db journal file is deleted when the pager
  ** is closed by the DETACH.
  */
  db->auto_commit = 1;

  if( p_db ){
    __sm_close(p_db->bt);
    p_db->bt = 0;
    p_db->schema = 0;
  }

  /* This both clears the schemas and reduces the size of the db->dbs[]
  ** array. */ 
  __reset_all_schemas_of_connection(db);

  return rc;
}

#endif  /* DBSQL_OMIT_VACUUM && DBSQL_OMIT_ATTACH */
