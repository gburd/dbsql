/*
** 2005 May 25
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the implementation of the dbsql_prepare()
** interface, and routines that contribute to loading the database schema
** from disk.
*/
#include "dbsql_int.h"

/*
** Fill the __init_data structure with an error message that indicates
** that the database is corrupt.
*/
static void __corrupt_schema(
  __init_data *data,     /* Initialization context */
  char **objs,        /* Type and name of object being parsed */
  const char *extra   /* Error information */
){
  DBSQL *db = data->db;
  if( db->malloc_failed ){
    data->rc = DBSQL_NOMEM_BKPT;
  }else if( data->errmsg_p[0]!=0 ){
    /* A error message has already been generated.  Do not overwrite it */
  }else if( data->m_init_flags & (INITFLAG_AlterMask) ){
    static const char *alter_types[] = {
       "rename",
       "drop column",
       "add column",
       "drop constraint"
    };
    *data->errmsg_p = __m_printf(db, 
        "error in %s %s after %s: %s", objs[0], objs[1], 
        alter_types[(data->m_init_flags&INITFLAG_AlterMask)-1], 
        extra
    );
    data->rc = DBSQL_ERROR;
  }else if( db->flags & DBSQL_WriteSchema ){
    data->rc = DBSQL_CORRUPT_BKPT;
  }else{
    char *z;
    const char *obj = objs[1] ? objs[1] : "?";
    z = __m_printf(db, "malformed database schema (%s)", obj);
    if( extra && extra[0] ) z = __m_printf(db, "%z - %s", z, extra);
    *data->errmsg_p = z;
    data->rc = DBSQL_CORRUPT_BKPT;
  }
}

/*
** Check to see if any sibling index (another index on the same table)
** of p_index has the same root page number, and if it does, return true.
** This would indicate a corrupt schema.
*/
int __index_has_duplicate_root_page(index_t *p_index){
  index_t *p;
  for(p=p_index->table->p_index; p; p=p->next){
    if( p->tnum==p_index->tnum && p!=p_index ) return 1;
  }
  return 0;
}

/* forward declaration */
static int __prepare(
  DBSQL *db,              /* Database handle. */
  const char *sql,         /* UTF-8 encoded SQL statement. */
  int n_bytes,               /* Length of sql in bytes. */
  u_int32_t prep_flags,            /* Zero or more DBSQL_PREPARE_* flags */
  vdbe_t *reprepare,         /* VM being reprepared */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const char **tail       /* OUT: End of parsed string */
);


/*
** This is the callback routine for the code that initializes the
** database.  See __init() below for additional information.
** This routine is also called from the OP_ParseSchema opcode of the VDBE.
**
** Each callback contains the following information:
**
**     argv[0] = type of object: "table", "index", "trigger", or "view".
**     argv[1] = name of thing being created
**     argv[2] = associated table if an index or trigger
**     argv[3] = root page number for table or index. 0 for trigger or view.
**     argv[4] = SQL text for the CREATE statement.
**
*/
int __init_callback(void *init, int argc, char **argv, char **__not_used){
  __init_data *data = (__init_data*)init;
  DBSQL *db = data->db;
  int i_db = data->i_db;

  assert( argc==5 );
  UNUSED_PARAMETER2(__not_used, argc);
  assert( dbsql_mutex_held(db->mutex) );
  db->m_db_flags |= DBFLAG_EncodingFixed;
  if( argv==0 ) return 0;   /* Might happen if EMPTY_RESULT_CALLBACKS are on */
  data->n_init_row++;
  if( db->malloc_failed ){
    __corrupt_schema(data, argv, 0);
    return 1;
  }

  assert( i_db>=0 && i_db<db->n_db );
  if( argv[3]==0 ){
    __corrupt_schema(data, argv, 0);
  }else if( argv[4]
         && 'c'==__upper_to_lower[(unsigned char)argv[4][0]]
         && 'r'==__upper_to_lower[(unsigned char)argv[4][1]] ){
    /* Call the parser to process a CREATE TABLE, INDEX or VIEW.
    ** But because db->init.busy is set to 1, no VDBE code is generated
    ** or executed.  All the parser does is build the internal data
    ** structures that describe the table, index, or view.
    **
    ** No other valid SQL statement, other than the variable CREATE statements,
    ** can begin with the letters "C" and "R".  Thus, it is not possible run
    ** any other kind of statement while parsing the schema, even a corrupt
    ** schema.
    */
    int rc;
    u_int8_t saved_i_db = db->init.i_db;
    dbsql_stmt_t *stmt;
    TESTONLY(int rcp);            /* Return code from dbsql_prepare() */

    assert( db->init.busy );
    db->init.i_db = i_db;
    if( __get_u_int32(argv[3], &db->init.new_tnum)==0
     || (db->init.new_tnum>data->mx_page && data->mx_page>0)
    ){
      if( __config.extra_schema_checks ){
        __corrupt_schema(data, argv, "invalid rootpage");
      }
    }
    db->init.orphan_trigger = 0;
    db->init.inits = (const char**)argv;
    stmt = 0;
    TESTONLY(rcp = ) __prepare(db, argv[4], -1, 0, 0, &stmt, 0);
    rc = db->errCode;
    assert( (rc&0xFF)==(rcp&0xFF) );
    db->init.i_db = saved_i_db;
    /* assert( saved_i_db==0 || (db->m_db_flags & DBFLAG_Vacuum)!=0 ); */
    if( DBSQL_OK!=rc ){
      if( db->init.orphan_trigger ){
        assert( i_db==1 );
      }else{
        if( rc > data->rc ) data->rc = rc;
        if( rc==DBSQL_NOMEM ){
          __oom_fault(db);
        }else if( rc!=DBSQL_INTERRUPT && (rc&0xFF)!=DBSQL_LOCKED ){
          __corrupt_schema(data, argv, dbsql_errmsg(db));
        }
      }
    }
    db->init.inits = __std_type; /* Any array of string ptrs will do */
    dbsql_finalize(stmt);
  }else if( argv[1]==0 || (argv[4]!=0 && argv[4][0]!=0) ){
    __corrupt_schema(data, argv, 0);
  }else{
    /* If the SQL column is blank it means this is an index that
    ** was created to be the PRIMARY KEY or to fulfill a UNIQUE
    ** constraint for a CREATE TABLE.  The index should have already
    ** been created when we processed the CREATE TABLE.  All we have
    ** to do here is record the root page number for that index.
    */
    index_t *p_index;
    p_index = __find_index(db, argv[1], db->dbs[i_db].db_s_name);
    if( p_index==0 ){
      __corrupt_schema(data, argv, "orphan index");
    }else
    if( __get_u_int32(argv[3],&p_index->tnum)==0
     || p_index->tnum<2
     || p_index->tnum>data->mx_page
     || __index_has_duplicate_root_page(p_index)
    ){
      if( __config.extra_schema_checks ){
        __corrupt_schema(data, argv, "invalid rootpage");
      }
    }
  }
  return 0;
}

/*
** Attempt to read the database schema and initialize internal
** data structures for a single database file.  The index of the
** database file is given by i_db.  i_db==0 is used for the main
** database.  i_db==1 should never be used.  i_db>=2 is used for
** auxiliary databases.  Return one of the DBSQL_ error codes to
** indicate success or failure.
*/
int __init_one(DBSQL *db, int i_db, char **errmsg_p, u_int32_t m_flags){
  int rc;
  int i;
#ifndef DBSQL_OMIT_DEPRECATED
  int size;
#endif
  db_t *p_db;
  char const *args[6];
  int meta[5];
  __init_data initData;
  const char *schema_tab_name;
  int opened_transaction = 0;
  int mask = ((db->m_db_flags & DBFLAG_EncodingFixed) | ~DBFLAG_EncodingFixed);

  assert( (db->m_db_flags & DBFLAG_SchemaKnownOk)==0 );
  assert( i_db>=0 && i_db<db->n_db );
  assert( db->dbs[i_db].schema );
  assert( dbsql_mutex_held(db->mutex) );
  assert( i_db==1 || __sm_holds_mutex(db->dbs[i_db].bt) );

  db->init.busy = 1;

  /* Construct the in-memory representation schema tables (dbsql_schema or
  ** dbsql_temp_schema) by invoking the parser directly.  The appropriate
  ** table name will be inserted automatically by the parser so we can just
  ** use the abbreviation "x" here.  The parser will also automatically tag
  ** the schema table as read-only. */
  args[0] = "table";
  args[1] = schema_tab_name = SCHEMA_TABLE(i_db);
  args[2] = args[1];
  args[3] = "1";
  args[4] = "CREATE TABLE x(type text,name text,tbl_name text,"
                            "rootpage int,sql text)";
  args[5] = 0;
  initData.db = db;
  initData.i_db = i_db;
  initData.rc = DBSQL_OK;
  initData.errmsg_p = errmsg_p;
  initData.m_init_flags = m_flags;
  initData.n_init_row = 0;
  initData.mx_page = 0;
  __init_callback(&initData, 5, (char **)args, 0);
  db->m_db_flags &= mask;
  if( initData.rc ){
    rc = initData.rc;
    goto error_out;
  }

  /* Create a cursor to hold the database open
  */
  p_db = &db->dbs[i_db];
  if( p_db->bt==0 ){
    assert( i_db==1 );
    DB_SET_PROPERTY(db, 1, DB_SchemaLoaded);
    rc = DBSQL_OK;
    goto error_out;
  }

  /* If there is not already a read-only (or read-write) transaction opened
  ** on the b-tree database, open one now. If a transaction is opened, it 
  ** will be closed before this function returns.  */
  __sm_enter(p_db->bt);
  if( __sm_txn_state(p_db->bt)==DBSQL_TXN_NONE ){
    rc = __sm_begin_trans(p_db->bt, 0, 0);
    if( rc!=DBSQL_OK ){
      __set_string(errmsg_p, db, __err_str(rc));
      goto initone_error_out;
    }
    opened_transaction = 1;
  }

  /* Get the database meta information.
  **
  ** Meta values are as follows:
  **    meta[0]   schema_t cookie.  Changes with each schema change.
  **    meta[1]   File format of schema layer.
  **    meta[2]   Size of the page cache.
  **    meta[3]   Largest rootpage (auto/incr_vacuum mode)
  **    meta[4]   db_t text encoding. 1:UTF-8 2:UTF-16LE 3:UTF-16BE
  **    meta[5]   User version
  **    meta[6]   Incremental vacuum mode
  **    meta[7]   unused
  **    meta[8]   unused
  **    meta[9]   unused
  **
  ** Note: The #defined DBSQL_UTF* symbols in dbsql_int.h correspond to
  ** the possible values of meta[4].
  */
  for(i=0; i<ARRAY_SIZE(meta); i++){
    __sm_get_meta(p_db->bt, i+1, (u_int32_t *)&meta[i]);
  }
  if( (db->flags & DBSQL_ResetDatabase)!=0 ){
    memset(meta, 0, sizeof(meta));
  }
  p_db->schema->schema_cookie = meta[BTREE_SCHEMA_VERSION-1];

  /* If opening a non-empty database, check the text encoding. For the
  ** main database, set DBSQL.enc to the encoding of the main database.
  ** For an attached db, it is an error if the encoding is not the same
  ** as DBSQL.enc.
  */
  if( meta[BTREE_TEXT_ENCODING-1] ){  /* text encoding */
    if( i_db==0 && (db->m_db_flags & DBFLAG_EncodingFixed)==0 ){
      u_int8_t encoding;
#ifndef DBSQL_OMIT_UTF16
      /* If opening the main database, set ENC(db). */
      encoding = (u_int8_t)meta[BTREE_TEXT_ENCODING-1] & 3;
      if( encoding==0 ) encoding = DBSQL_UTF8;
#else
      encoding = DBSQL_UTF8;
#endif
      __set_text_encoding(db, encoding);
    }else{
      /* If opening an attached database, the encoding much match ENC(db) */
      if( (meta[BTREE_TEXT_ENCODING-1] & 3)!=ENC(db) ){
        __set_string(errmsg_p, db, "attached databases must use the same"
            " text encoding as main database");
        rc = DBSQL_ERROR;
        goto initone_error_out;
      }
    }
  }
  p_db->schema->enc = ENC(db);

  if( p_db->schema->cache_size==0 ){
#ifndef DBSQL_OMIT_DEPRECATED
    size = __abs_int32(meta[BTREE_DEFAULT_CACHE_SIZE-1]);
    if( size==0 ){ size = DBSQL_DEFAULT_CACHE_SIZE; }
    p_db->schema->cache_size = size;
#else
    p_db->schema->cache_size = DBSQL_DEFAULT_CACHE_SIZE;
#endif
    __sm_set_cache_size(p_db->bt, p_db->schema->cache_size);
  }

  /*
  ** file_format==1    Version 3.0.0.
  ** file_format==2    Version 3.1.3.  // ALTER TABLE ADD COLUMN
  ** file_format==3    Version 3.1.4.  // ditto but with non-NULL defaults
  ** file_format==4    Version 3.3.0.  // DESC indices.  Boolean constants
  */
  p_db->schema->file_format = (u_int8_t)meta[BTREE_FILE_FORMAT-1];
  if( p_db->schema->file_format==0 ){
    p_db->schema->file_format = 1;
  }
  if( p_db->schema->file_format>DBSQL_MAX_FILE_FORMAT ){
    __set_string(errmsg_p, db, "unsupported file format");
    rc = DBSQL_ERROR;
    goto initone_error_out;
  }

  /* Ticket #2804:  When we open a database in the newer file format,
  ** clear the legacy_file_format pragma flag so that a VACUUM will
  ** not downgrade the database and thus invalidate any descending
  ** indices that the user might have created.
  */
  if( i_db==0 && meta[BTREE_FILE_FORMAT-1]>=4 ){
    db->flags &= ~(u_int64_t)DBSQL_LegacyFileFmt;
  }

  /* Read the schema information out of the schema tables
  */
  assert( db->init.busy );
  initData.mx_page = __sm_last_page(p_db->bt);
  {
    char *sql;
    sql = __m_printf(db, 
        "SELECT*FROM\"%w\".%s ORDER BY rowid",
        db->dbs[i_db].db_s_name, schema_tab_name);
#ifndef DBSQL_OMIT_AUTHORIZATION
    {
      dbsql_xauth auth;
      auth = db->auth;
      db->auth = 0;
#endif
      rc = dbsql_exec(db, sql, __init_callback, &initData, 0);
#ifndef DBSQL_OMIT_AUTHORIZATION
      db->auth = auth;
    }
#endif
    if( rc==DBSQL_OK ) rc = initData.rc;
    __dbsql_db_free(db, sql);
#ifndef DBSQL_OMIT_ANALYZE
    if( rc==DBSQL_OK ){
      __analysis_load(db, i_db);
    }
#endif
  }
  assert( p_db == &(db->dbs[i_db]) );
  if( db->malloc_failed ){
    rc = DBSQL_NOMEM_BKPT;
    __reset_all_schemas_of_connection(db);
    p_db = &db->dbs[i_db];
  }else
  if( rc==DBSQL_OK || ((db->flags&DBSQL_NoSchemaError) && rc!=DBSQL_NOMEM)){
    /* Hack: If the DBSQL_NoSchemaError flag is set, then consider
    ** the schema loaded, even if errors (other than OOM) occurred. In
    ** this situation the current dbsql_prepare() operation will fail,
    ** but the following one will attempt to compile the supplied statement
    ** against whatever subset of the schema was loaded before the error
    ** occurred.
    **
    ** The primary purpose of this is to allow access to the dbsql_schema
    ** table even when its contents have been corrupted.
    */
    DB_SET_PROPERTY(db, i_db, DB_SchemaLoaded);
    rc = DBSQL_OK;
  }

  /* Jump here for an error that occurs after successfully allocating
  ** cur_main and calling __sm_enter(). For an error that occurs
  ** before that point, jump to error_out.
  */
initone_error_out:
  if( opened_transaction ){
    __sm_commit(p_db->bt);
  }
  __sm_leave(p_db->bt);

error_out:
  if( rc ){
    if( rc==DBSQL_NOMEM || rc==DBSQL_IOERR_NOMEM ){
      __oom_fault(db);
    }
    __reset_one_schema(db, i_db);
  }
  db->init.busy = 0;
  return rc;
}

/*
** Initialize all database files - the main database file, the file
** used to store temporary tables, and any additional database files
** created using ATTACH statements.  Return a success code.  If an
** error occurs, write an error message into *errmsg_p.
**
** After a database is initialized, the DB_SchemaLoaded bit is set
** bit is set in the flags field of the db_t structure. 
*/
int __init(DBSQL *db, char **errmsg_p){
  int i, rc;
  int commit_internal = !(db->m_db_flags&DBFLAG_SchemaChange);
  
  assert( dbsql_mutex_held(db->mutex) );
  assert( __sm_holds_mutex(db->dbs[0].bt) );
  assert( db->init.busy==0 );
  ENC(db) = SCHEMA_ENC(db);
  assert( db->n_db>0 );
  /* Do the main schema first */
  if( !DB_HAS_PROPERTY(db, 0, DB_SchemaLoaded) ){
    rc = __init_one(db, 0, errmsg_p, 0);
    if( rc ) return rc;
  }
  /* All other schemas after the main schema. The "temp" schema must be last */
  for(i=db->n_db-1; i>0; i--){
    assert( i==1 || __sm_holds_mutex(db->dbs[i].bt) );
    if( !DB_HAS_PROPERTY(db, i, DB_SchemaLoaded) ){
      rc = __init_one(db, i, errmsg_p, 0);
      if( rc ) return rc;
    }
  }
  if( commit_internal ){
    __commit_internal_changes(db);
  }
  return DBSQL_OK;
}

/*
** This routine is a no-op if the database schema is already initialized.
** Otherwise, the schema is loaded. An error code is returned.
*/
int __read_schema(parser_t *parser){
  int rc = DBSQL_OK;
  DBSQL *db = parser->db;
  assert( dbsql_mutex_held(db->mutex) );
  if( !db->init.busy ){
    rc = __init(db, &parser->errmsg);
    if( rc!=DBSQL_OK ){
      parser->rc = rc;
      parser->n_err++;
    }else if( db->no_shared_cache ){
      db->m_db_flags |= DBFLAG_SchemaKnownOk;
    }
  }
  return rc;
}


/*
** Check schema cookies in all databases.  If any cookie is out
** of date set parser->rc to DBSQL_SCHEMA.  If all schema cookies
** make no changes to parser->rc.
*/
static void __schema_is_valid(parser_t *parser){
  DBSQL *db = parser->db;
  int i_db;
  int rc;
  int cookie;

  assert( parser->check_schema );
  assert( dbsql_mutex_held(db->mutex) );
  for(i_db=0; i_db<db->n_db; i_db++){
    int opened_transaction = 0;         /* True if a transaction is opened */
    sm_t *bt = db->dbs[i_db].bt;     /* sm_t database to read cookie from */
    if( bt==0 ) continue;

    /* If there is not already a read-only (or read-write) transaction opened
    ** on the b-tree database, open one now. If a transaction is opened, it 
    ** will be closed immediately after reading the meta-value. */
    if( __sm_txn_state(bt)==DBSQL_TXN_NONE ){
      rc = __sm_begin_trans(bt, 0, 0);
      if( rc==DBSQL_NOMEM || rc==DBSQL_IOERR_NOMEM ){
        __oom_fault(db);
        parser->rc = DBSQL_NOMEM;
      }
      if( rc!=DBSQL_OK ) return;
      opened_transaction = 1;
    }

    /* Read the schema cookie from the database. If it does not match the 
    ** value stored as part of the in-memory schema representation,
    ** set parser_t.rc to DBSQL_SCHEMA. */
    __sm_get_meta(bt, BTREE_SCHEMA_VERSION, (u_int32_t *)&cookie);
    assert( __schema_mutex_held(db, i_db, 0) );
    if( cookie!=db->dbs[i_db].schema->schema_cookie ){
      if( DB_HAS_PROPERTY(db, i_db, DB_SchemaLoaded) ) parser->rc = DBSQL_SCHEMA;
      __reset_one_schema(db, i_db);
    }

    /* Close the transaction, if one was opened. */
    if( opened_transaction ){
      __sm_commit(bt);
    }
  }
}

/*
** Convert a schema pointer into the i_db index that indicates
** which database file in db->dbs[] the schema refers to.
**
** If the same database is attached more than once, the first
** attached database is returned.
*/
int __schema_to_index(DBSQL *db, schema_t *schema){
  int i = -32768;

  /* If schema is NULL, then return -32768. This happens when code in 
  ** expr.c is trying to resolve a reference to a transient table (i.e. one
  ** created by a sub-select). In this case the return value of this 
  ** function should never be used.
  **
  ** We return -32768 instead of the more usual -1 simply because using
  ** -32768 as the incorrect index into db->dbs[] is much 
  ** more likely to cause a segfault than -1 (of course there are assert()
  ** statements too, but it never hurts to play the odds) and
  ** -32768 will still fit into a 16-bit signed integer.
  */
  assert( dbsql_mutex_held(db->mutex) );
  if( schema ){
    for(i=0; 1; i++){
      assert( i<db->n_db );
      if( db->dbs[i].schema==schema ){
        break;
      }
    }
    assert( i>=0 && i<db->n_db );
  }
  return i;
}

/*
** Free all memory allocations in the parser object
*/
void __parse_object_reset(parser_t *parser){
  DBSQL *db = parser->db;
  assert( db!=0 );
  assert( db->parser==parser );
  assert( parser->nested==0 );
#ifndef DBSQL_OMIT_SHARED_CACHE
  if( parser->table_locks ) __db_nn_free_nn(db, parser->table_locks);
#endif
  while( parser->cleanup ){
    parse_cleanup_t *cleanup = parser->cleanup;
    parser->cleanup = cleanup->next;
    cleanup->cleanup(db, cleanup->ptr);
    __db_nn_free_nn(db, cleanup);
  }
  if( parser->labels ) __db_nn_free_nn(db, parser->labels);
  if( parser->const_expr ){
    __expr_list_delete(db, parser->const_expr);
  }
  assert( db->lookaside.disable >= parser->__disable_lookaside );
  db->lookaside.disable -= parser->__disable_lookaside;
  db->lookaside.sz = db->lookaside.disable ? 0 : db->lookaside.sz_true;
  assert( parser->db->parser==parser );
  db->parser = parser->outer_parse;
}

/*
** Add a new cleanup operation to a Parser.  The cleanup should happen when
** the parser object is destroyed.  But, beware: the cleanup might happen
** immediately.
**
** Use this mechanism for uncommon cleanups.  There is a higher setup
** cost for this mechanism (an extra malloc), so it should not be used
** for common cleanups that happen on most calls.  But for less
** common cleanups, we save a single NULL-pointer comparison in
** __parse_object_reset(), which reduces the total CPU cycle count.
**
** If a memory allocation error occurs, then the cleanup happens immediately.
** When either DBSQL_DEBUG or DBSQL_COVERAGE_TEST are defined, the
** parser->early_cleanup flag is set in that case.  Calling code show verify
** that test cases exist for which this happens, to guard against possible
** use-after-free errors following an OOM.  The preferred way to do this is
** to immediately follow the call to this routine with:
**
**       testcase( parser->early_cleanup );
**
** This routine returns a copy of its ptr input (the third parameter)
** except if an early cleanup occurs, in which case it returns NULL.  So
** another way to check for early cleanup is to check the return value.
** Or, stop using the ptr parameter with this call and use only its
** return value thereafter.  Something like this:
**
**       obj = __parser_add_cleanup(parser, destructor, obj);
*/
void *__parser_add_cleanup(
  parser_t *parser,                      /* Destroy when this Parser finishes */
  void (*x_cleanup)(DBSQL*,void*),   /* The cleanup routine */
  void *ptr                          /* Pointer to object to be cleaned up */
){
  parse_cleanup_t *cleanup;
  if( __fault_sim(300) ){
    cleanup = 0;
    __oom_fault(parser->db);
  }else{
    cleanup = __db_malloc_raw(parser->db, sizeof(*cleanup));
  }
  if( cleanup ){
    cleanup->next = parser->cleanup;
    parser->cleanup = cleanup;
    cleanup->ptr = ptr;
    cleanup->cleanup = x_cleanup;
  }else{
    x_cleanup(parser->db, ptr);
    ptr = 0;
#if defined(DBSQL_DEBUG) || defined(DBSQL_COVERAGE_TEST)
    parser->early_cleanup = 1;
#endif
  }
  return ptr;
}

/*
** Turn bulk memory into a valid parser_t object and link that parser_t object
** into database connection db.
**
** Call __parse_object_reset() to undo this operation.
**
** Caution:  Do not confuse this routine with __parse_object_init() which
** is generated by Lemon.
*/
void __parse_object_init(parser_t *parser, DBSQL *db){
  memset(PARSE_HDR(parser), 0, PARSE_HDR_SZ);
  memset(PARSE_TAIL(parser), 0, PARSE_TAIL_SZ);
  assert( db->parser!=parser );
  parser->outer_parse = db->parser;
  db->parser = parser;
  parser->db = db;
  if( db->malloc_failed ) __error_msg(parser, "out of memory");
}

/*
** Maximum number of times that we will try again to prepare a statement
** that returns DBSQL_ERROR_RETRY.
*/
#ifndef DBSQL_MAX_PREPARE_RETRY
# define DBSQL_MAX_PREPARE_RETRY 25
#endif

/*
** Compile the UTF-8 encoded SQL statement sql into a statement handle.
*/
static int __prepare(
  DBSQL *db,              /* Database handle. */
  const char *sql,         /* UTF-8 encoded SQL statement. */
  int n_bytes,               /* Length of sql in bytes. */
  u_int32_t prep_flags,            /* Zero or more DBSQL_PREPARE_* flags */
  vdbe_t *reprepare,         /* VM being reprepared */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const char **tail       /* OUT: End of parsed string */
){
  int rc = DBSQL_OK;       /* Result code */
  int i;                    /* Loop counter */
  parser_t s_parse;             /* Parsing context */

  /* __parse_object_init(&s_parse, db); // inlined for performance */
  memset(PARSE_HDR(&s_parse), 0, PARSE_HDR_SZ);
  memset(PARSE_TAIL(&s_parse), 0, PARSE_TAIL_SZ);
  s_parse.outer_parse = db->parser;
  db->parser = &s_parse;
  s_parse.db = db;
  if( reprepare ){
    s_parse.reprepare = reprepare;
    s_parse.explain = dbsql_stmt_isexplain((dbsql_stmt_t*)reprepare);
  }else{
    assert( s_parse.reprepare==0 );
  }
  assert( stmt && *stmt==0 );
  if( db->malloc_failed ){
    __error_msg(&s_parse, "out of memory");
    db->errCode = rc = DBSQL_NOMEM;
    goto end_prepare;
  }
  assert( dbsql_mutex_held(db->mutex) );

  /* For a long-term use prepared statement avoid the use of
  ** lookaside memory.
  */
  if( prep_flags & DBSQL_PREPARE_PERSISTENT ){
    s_parse.__disable_lookaside++;
    DISABLE_LOOKASIDE;
  }
  s_parse.prep_flags = prep_flags & 0xff;

  /* Check to verify that it is possible to get a read lock on all
  ** database schemas.  The inability to get a read lock indicates that
  ** some other database connection is holding a write-lock, which in
  ** turn means that the other connection has made uncommitted changes
  ** to the schema.
  **
  ** Were we to proceed and prepare the statement against the uncommitted
  ** schema changes and if those schema changes are subsequently rolled
  ** back and different changes are made in their place, then when this
  ** prepared statement goes to run the schema cookie would fail to detect
  ** the schema change.  Disaster would follow.
  **
  ** This thread is currently holding mutexes on all Btrees (because
  ** of the __sm_enter_all() in __lock_and_prepare()) so it
  ** is not possible for another thread to start a new schema change
  ** while this routine is running.  Hence, we do not need to hold 
  ** locks on the schema, we just need to make sure nobody else is 
  ** holding them.
  **
  ** Note that setting READ_UNCOMMITTED overrides most lock detection,
  ** but it does *not* override schema lock detection, so this all still
  ** works even if READ_UNCOMMITTED is set.
  */
  if( !db->no_shared_cache ){
    for(i=0; i<db->n_db; i++) {
      sm_t *bt = db->dbs[i].bt;
      if( bt ){
        assert( __sm_holds_mutex(bt) );
        rc = __sm_schema_locked(bt);
        if( rc ){
          const char *z_db = db->dbs[i].db_s_name;
          __error_with_msg(db, rc, "database schema is locked: %s", z_db);
          testcase( db->flags & DBSQL_ReadUncommit );
          goto end_prepare;
        }
      }
    }
  }

#ifndef DBSQL_OMIT_VIRTUALTABLE
  if( db->disconnect ) __vtab_unlock_list(db);
#endif

  if( n_bytes>=0 && (n_bytes==0 || sql[n_bytes-1]!=0) ){
    char *sql_copy;
    int mx_len = db->limits[DBSQL_LIMIT_SQL_LENGTH];
    testcase( n_bytes==mx_len );
    testcase( n_bytes==mx_len+1 );
    if( n_bytes>mx_len ){
      __error_with_msg(db, DBSQL_TOOBIG, "statement too long");
      rc = __api_exit(db, DBSQL_TOOBIG);
      goto end_prepare;
    }
    sql_copy = __db_str_n_dup(db, sql, n_bytes);
    if( sql_copy ){
      __run_parser(&s_parse, sql_copy);
      s_parse.tail = &sql[s_parse.tail-sql_copy];
      __dbsql_db_free(db, sql_copy);
    }else{
      s_parse.tail = &sql[n_bytes];
    }
  }else{
    __run_parser(&s_parse, sql);
  }
  assert( 0==s_parse.n_query_loop );

  if( tail ){
    *tail = s_parse.tail;
  }

  if( db->init.busy==0 ){
    __vdbe_set_sql(s_parse.vdbe, sql, (int)(s_parse.tail-sql), prep_flags);
  }
  if( db->malloc_failed ){
    s_parse.rc = DBSQL_NOMEM_BKPT;
    s_parse.check_schema = 0;
  }
  if( s_parse.rc!=DBSQL_OK && s_parse.rc!=DBSQL_DONE ){
    if( s_parse.check_schema && db->init.busy==0 ){
      __schema_is_valid(&s_parse);
    }
    if( s_parse.vdbe ){
      __vdbe_finalize(s_parse.vdbe);
    }
    assert( 0==(*stmt) );
    rc = s_parse.rc;
    if( s_parse.errmsg ){
      __error_with_msg(db, rc, "%s", s_parse.errmsg);
      __dbsql_db_free(db, s_parse.errmsg);
    }else{
      __dbsql_error(db, rc);
    }
  }else{
    assert( s_parse.errmsg==0 );
    *stmt = (dbsql_stmt_t*)s_parse.vdbe;
    rc = DBSQL_OK;
    __error_clear(db);
  }


  /* Delete any trigger_prg_t structures allocated while parsing this statement. */
  while( s_parse.trigger_prg ){
    trigger_prg_t *t = s_parse.trigger_prg;
    s_parse.trigger_prg = t->next;
    __dbsql_db_free(db, t);
  }

end_prepare:

  __parse_object_reset(&s_parse);
  return rc;
}
static int __lock_and_prepare(
  DBSQL *db,              /* Database handle. */
  const char *sql,         /* UTF-8 encoded SQL statement. */
  int n_bytes,               /* Length of sql in bytes. */
  u_int32_t prep_flags,            /* Zero or more DBSQL_PREPARE_* flags */
  vdbe_t *old,               /* VM being reprepared */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const char **tail       /* OUT: End of parsed string */
){
  int rc;
  int cnt = 0;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( stmt==0 ) return DBSQL_MISUSE_BKPT;
#endif
  *stmt = 0;
  if( !__safety_check_ok(db)||sql==0 ){
    return DBSQL_MISUSE_BKPT;
  }
  dbsql_mutex_enter(db->mutex);
  __sm_enter_all(db);
  do{
    /* Make multiple attempts to compile the SQL, until it either succeeds
    ** or encounters a permanent error.  A schema problem after one schema
    ** reset is considered a permanent error. */
    rc = __prepare(db, sql, n_bytes, prep_flags, old, stmt, tail);
    assert( rc==DBSQL_OK || *stmt==0 );
    if( rc==DBSQL_OK || db->malloc_failed ) break;
    cnt++;
  }while( (rc==DBSQL_ERROR_RETRY && ALWAYS(cnt<=DBSQL_MAX_PREPARE_RETRY))
       || (rc==DBSQL_SCHEMA && (__reset_one_schema(db,-1), cnt)==1) );
  __sm_leave_all(db);
  assert( rc!=DBSQL_ERROR_RETRY );
  rc = __api_exit(db, rc);
  assert( (rc&db->err_mask)==rc );
  db->busy_handler.n_busy = 0;
  dbsql_mutex_leave(db->mutex);
  assert( rc==DBSQL_OK || (*stmt)==0 );
  return rc;
}


/*
** Rerun the compilation of a statement after a schema change.
**
** If the statement is successfully recompiled, return DBSQL_OK. Otherwise,
** if the statement cannot be recompiled because another connection has
** locked the dbsql_schema table, return DBSQL_LOCKED. If any other error
** occurs, return DBSQL_SCHEMA.
*/
int __reprepare(vdbe_t *p){
  int rc;
  dbsql_stmt_t *p_new;
  const char *sql;
  DBSQL *db;
  u_int8_t prep_flags;

  assert( dbsql_mutex_held(__vdbe_db(p)->mutex) );
  sql = dbsql_sql((dbsql_stmt_t *)p);
  assert( sql!=0 );  /* Reprepare only called for prepare_v2() statements */
  db = __vdbe_db(p);
  assert( dbsql_mutex_held(db->mutex) );
  prep_flags = __vdbe_prepare_flags(p);
  rc = __lock_and_prepare(db, sql, -1, prep_flags, p, &p_new, 0);
  if( rc ){
    if( rc==DBSQL_NOMEM ){
      __oom_fault(db);
    }
    assert( p_new==0 );
    return rc;
  }else{
    assert( p_new!=0 );
  }
  __vdbe_swap((vdbe_t*)p_new, p);
  __transfer_bindings(p_new, (dbsql_stmt_t*)p);
  __vdbe_reset_step_result((vdbe_t*)p_new);
  __vdbe_finalize((vdbe_t*)p_new);
  return DBSQL_OK;
}


/*
** Two versions of the official API.  Legacy and new use.  In the legacy
** version, the original SQL text is not saved in the prepared statement
** and so if a schema change occurs, DBSQL_SCHEMA is returned by
** dbsql_step().  In the new version, the original SQL text is retained
** and the statement is automatically recompiled if an schema change
** occurs.
*/
int dbsql_prepare(
  DBSQL *db,              /* Database handle. */
  const char *sql,         /* UTF-8 encoded SQL statement. */
  int n_bytes,               /* Length of sql in bytes. */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const char **tail       /* OUT: End of parsed string */
){
  int rc;
  rc = __lock_and_prepare(db,sql,n_bytes,0,0,stmt,tail);
  assert( rc==DBSQL_OK || stmt==0 || *stmt==0 );  /* VERIFY: F13021 */
  return rc;
}
int dbsql_prepare_v2(
  DBSQL *db,              /* Database handle. */
  const char *sql,         /* UTF-8 encoded SQL statement. */
  int n_bytes,               /* Length of sql in bytes. */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const char **tail       /* OUT: End of parsed string */
){
  int rc;
  /* EVIDENCE-OF: R-37923-12173 The dbsql_prepare_v2() interface works
  ** exactly the same as dbsql_prepare_v3() with a zero prep_flags
  ** parameter.
  **
  ** Proof in that the 5th parameter to __lock_and_prepare is 0 */
  rc = __lock_and_prepare(db,sql,n_bytes,DBSQL_PREPARE_SAVESQL,0,
                             stmt,tail);
  assert( rc==DBSQL_OK || stmt==0 || *stmt==0 );
  return rc;
}
int dbsql_prepare_v3(
  DBSQL *db,              /* Database handle. */
  const char *sql,         /* UTF-8 encoded SQL statement. */
  int n_bytes,               /* Length of sql in bytes. */
  unsigned int prep_flags,   /* Zero or more DBSQL_PREPARE_* flags */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const char **tail       /* OUT: End of parsed string */
){
  int rc;
  /* EVIDENCE-OF: R-56861-42673 dbsql_prepare_v3() differs from
  ** dbsql_prepare_v2() only in having the extra prep_flags parameter,
  ** which is a bit array consisting of zero or more of the
  ** DBSQL_PREPARE_* flags.
  **
  ** Proof by comparison to the implementation of dbsql_prepare_v2()
  ** directly above. */
  rc = __lock_and_prepare(db,sql,n_bytes,
                 DBSQL_PREPARE_SAVESQL|(prep_flags&DBSQL_PREPARE_MASK),
                 0,stmt,tail);
  assert( rc==DBSQL_OK || stmt==0 || *stmt==0 );
  return rc;
}


#ifndef DBSQL_OMIT_UTF16
/*
** Compile the UTF-16 encoded SQL statement sql into a statement handle.
*/
static int __prepare16(
  DBSQL *db,              /* Database handle. */ 
  const void *sql,         /* UTF-16 encoded SQL statement. */
  int n_bytes,               /* Length of sql in bytes. */
  u_int32_t prep_flags,            /* Zero or more DBSQL_PREPARE_* flags */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const void **tail       /* OUT: End of parsed string */
){
  /* This function currently works by first transforming the UTF-16
  ** encoded string to UTF-8, then invoking dbsql_prepare(). The
  ** tricky bit is figuring out the pointer to return in *tail.
  */
  char *sql8;
  const char *tail8 = 0;
  int rc = DBSQL_OK;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( stmt==0 ) return DBSQL_MISUSE_BKPT;
#endif
  *stmt = 0;
  if( !__safety_check_ok(db)||sql==0 ){
    return DBSQL_MISUSE_BKPT;
  }

  /* Make sure n_bytes is non-negative and correct.  It should be the
  ** number of bytes until the end of the input buffer or until the first
  ** U+0000 character.  If the input n_bytes is odd, convert it into
  ** an even number.  If the input n_bytes is negative, then the input
  ** must be terminated by at least one U+0000 character */
  if( n_bytes>=0 ){
    int sz;
    const char *z = (const char*)sql;
    for(sz=0; sz<n_bytes && (z[sz]!=0 || z[sz+1]!=0); sz += 2){}
    n_bytes = sz;
  }else{
    int sz;
    const char *z = (const char*)sql;
    for(sz=0; z[sz]!=0 || z[sz+1]!=0; sz += 2){}
    n_bytes = sz;
  }

  dbsql_mutex_enter(db->mutex);
  sql8 = __utf16to8(db, sql, n_bytes, DBSQL_UTF16NATIVE);
  if( sql8 ){
    rc = __lock_and_prepare(db, sql8, -1, prep_flags, 0, stmt, &tail8);
  }

  if( tail8 && tail ){
    /* If dbsql_prepare returns a tail pointer, we calculate the
    ** equivalent pointer into the UTF-16 string by counting the unicode
    ** characters between sql8 and tail8, and then returning a pointer
    ** the same number of characters into the UTF-16 string.
    */
    int chars_parsed = __utf8_char_len(sql8, (int)(tail8-sql8));
    *tail = (u_int8_t *)sql + __utf16_byte_len(sql, n_bytes, chars_parsed);
  }
  __dbsql_db_free(db, sql8); 
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** Two versions of the official API.  Legacy and new use.  In the legacy
** version, the original SQL text is not saved in the prepared statement
** and so if a schema change occurs, DBSQL_SCHEMA is returned by
** dbsql_step().  In the new version, the original SQL text is retained
** and the statement is automatically recompiled if an schema change
** occurs.
*/
int dbsql_prepare16(
  DBSQL *db,              /* Database handle. */ 
  const void *sql,         /* UTF-16 encoded SQL statement. */
  int n_bytes,               /* Length of sql in bytes. */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const void **tail       /* OUT: End of parsed string */
){
  int rc;
  rc = __prepare16(db,sql,n_bytes,0,stmt,tail);
  assert( rc==DBSQL_OK || stmt==0 || *stmt==0 );  /* VERIFY: F13021 */
  return rc;
}
int dbsql_prepare16_v2(
  DBSQL *db,              /* Database handle. */ 
  const void *sql,         /* UTF-16 encoded SQL statement. */
  int n_bytes,               /* Length of sql in bytes. */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const void **tail       /* OUT: End of parsed string */
){
  int rc;
  rc = __prepare16(db,sql,n_bytes,DBSQL_PREPARE_SAVESQL,stmt,tail);
  assert( rc==DBSQL_OK || stmt==0 || *stmt==0 );  /* VERIFY: F13021 */
  return rc;
}
int dbsql_prepare16_v3(
  DBSQL *db,              /* Database handle. */ 
  const void *sql,         /* UTF-16 encoded SQL statement. */
  int n_bytes,               /* Length of sql in bytes. */
  unsigned int prep_flags,   /* Zero or more DBSQL_PREPARE_* flags */
  dbsql_stmt_t **stmt,    /* OUT: A pointer to the prepared statement */
  const void **tail       /* OUT: End of parsed string */
){
  int rc;
  rc = __prepare16(db,sql,n_bytes,
         DBSQL_PREPARE_SAVESQL|(prep_flags&DBSQL_PREPARE_MASK),
         stmt,tail);
  assert( rc==DBSQL_OK || stmt==0 || *stmt==0 );  /* VERIFY: F13021 */
  return rc;
}

#endif /* DBSQL_OMIT_UTF16 */
