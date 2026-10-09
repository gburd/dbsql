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
** This file contains code used to implement the PRAGMA command.
*/
#include "dbsql_int.h"

#if !defined(DBSQL_ENABLE_LOCKING_STYLE)
#  if defined(__APPLE__)
#    define DBSQL_ENABLE_LOCKING_STYLE 1
#  else
#    define DBSQL_ENABLE_LOCKING_STYLE 0
#  endif
#endif

/***************************************************************************
** The "pragma.h" include file is an automatically generated file that
** that includes the __prag_type_xxxx macro definitions and the __a_pragma_name[]
** object.  This ensures that the __a_pragma_name[] table is arranged in
** lexicographical order to facility a binary search of the pragma name.
** Do not edit pragma.h directly.  Edit and rerun the script in at
** ../tool/mkpragmatab.tcl. */
#include "pragma.h"

/*
** When the 0x10 bit of PRAGMA optimize is set, any ANALYZE commands
** will be run with an analysis_limit set to the lessor of the value of
** the following macro or to the actual analysis_limit if it is non-zero,
** in order to prevent PRAGMA optimize from running for too long.
**
** The value of 2000 is chosen empirically so that the worst-case run-time
** for PRAGMA optimize does not exceed 100 milliseconds against a variety
** of test databases on a __raspberry_pi-4 compiled using -Os and without
** -DSQLITE_DEBUG.  Of course, your mileage may vary.  For the purpose of
** this paragraph, "worst-case" means that ANALYZE ends up being
** run on every table in the database.  The worst case typically only
** happens if PRAGMA optimize is run on a database file for which ANALYZE
** has not been previously run and the 0x10000 flag is included so that
** all tables are analyzed.  The usual case for PRAGMA optimize is that
** no ANALYZE commands will be run at all, or if any ANALYZE happens it
** will be against a single table, so that expected timing for PRAGMA
** optimize on a PI-4 is more like 1 millisecond or less with the 0x10000
** flag or less than 100 microseconds without the 0x10000 flag.
**
** An analysis limit of 2000 is almost always sufficient for the query
** planner to fully characterize an index.  The additional accuracy from
** a larger analysis is not usually helpful.
*/
#ifndef DBSQL_DEFAULT_OPTIMIZE_LIMIT
# define DBSQL_DEFAULT_OPTIMIZE_LIMIT 2000
#endif

/*
** Interpret the given string as a safety level.  Return 0 for OFF,
** 1 for ON or NORMAL, 2 for FULL, and 3 for EXTRA.  Return 1 for an empty or
** unrecognized string argument.  The FULL and EXTRA option is disallowed
** if the omit_full parameter it 1.
**
** Note that the values returned are one less that the values that
** should be passed into __sm_set_safety_level().  The is done
** to support legacy SQL code.  The safety level used to be boolean
** and older scripts may have used numbers 0 for OFF and 1 for ON.
*/
static u_int8_t __get_safety_level(const char *z, int omit_full, u_int8_t dflt){
                             /* 123456789 123456789 123 */
  static const char text[] = "onoffalseyestruextrafull";
  static const u_int8_t i_offset[] = {0, 1, 2,  4,    9,  12,  15,   20};
  static const u_int8_t i_length[] = {2, 2, 3,  5,    3,   4,   5,    4};
  static const u_int8_t i_value[] =  {1, 0, 0,  0,    1,   1,   3,    2};
                            /* on no off false yes true extra full */
  int i, n;
  if( __isdigit(*z) ){
    return (u_int8_t)__atoi(z);
  }
  n = __strlen30(z);
  for(i=0; i<ARRAY_SIZE(i_length); i++){
    if( i_length[i]==n && __str_nicmp(&text[i_offset[i]],z,n)==0
     && (!omit_full || i_value[i]<=1)
    ){
      return i_value[i];
    }
  }
  return dflt;
}

/*
** Interpret the given string as a boolean value.
*/
u_int8_t __get_boolean(const char *z, u_int8_t dflt){
  return __get_safety_level(z,1,dflt)!=0;
}

/* The __get_boolean() function is used by other modules but the
** remainder of this file is specific to PRAGMA processing.  So omit
** the rest of the file if PRAGMAs are omitted from the build.
*/
#if !defined(DBSQL_OMIT_PRAGMA)

/*
** Interpret the given string as a locking mode value.
*/
static int __get_locking_mode(const char *z){
  if( z ){
    if( 0==__str_icmp(z, "exclusive") ) return PAGER_LOCKINGMODE_EXCLUSIVE;
    if( 0==__str_icmp(z, "normal") ) return PAGER_LOCKINGMODE_NORMAL;
  }
  return PAGER_LOCKINGMODE_QUERY;
}

#ifndef DBSQL_OMIT_AUTOVACUUM
/*
** Interpret the given string as an auto-vacuum mode value.
**
** The following strings, "none", "full" and "incremental" are
** acceptable, as are their numeric equivalents: 0, 1 and 2 respectively.
*/
static int __get_auto_vacuum(const char *z){
  int i;
  if( 0==__str_icmp(z, "none") ) return BTREE_AUTOVACUUM_NONE;
  if( 0==__str_icmp(z, "full") ) return BTREE_AUTOVACUUM_FULL;
  if( 0==__str_icmp(z, "incremental") ) return BTREE_AUTOVACUUM_INCR;
  i = __atoi(z);
  return (u_int8_t)((i>=0&&i<=2)?i:0);
}
#endif /* ifndef DBSQL_OMIT_AUTOVACUUM */

#ifndef DBSQL_OMIT_PAGER_PRAGMAS
/*
** Interpret the given string as a temp db location. Return 1 for file
** backed temporary databases, 2 for the Red-Black tree in memory database
** and 0 to use the compile-time default.
*/
static int __get_temp_store(const char *z){
  if( z[0]>='0' && z[0]<='2' ){
    return z[0] - '0';
  }else if( __str_icmp(z, "file")==0 ){
    return 1;
  }else if( __str_icmp(z, "memory")==0 ){
    return 2;
  }else{
    return 0;
  }
}
#endif /* DBSQL_PAGER_PRAGMAS */

#ifndef DBSQL_OMIT_PAGER_PRAGMAS
/*
** Invalidate temp storage, either when the temp storage is changed
** from default, or when 'file' and the temp_store_directory has changed
*/
static int __invalidate_temp_storage(parser_t *parser){
  DBSQL *db = parser->db;
  if( db->dbs[1].bt!=0 ){
    if( !db->auto_commit
     || __sm_txn_state(db->dbs[1].bt)!=DBSQL_TXN_NONE
    ){
      __error_msg(parser, "temporary storage cannot be changed "
        "from within a transaction");
      return DBSQL_ERROR;
    }
    __sm_close(db->dbs[1].bt);
    db->dbs[1].bt = 0;
    __reset_all_schemas_of_connection(db);
  }
  return DBSQL_OK;
}
#endif /* DBSQL_PAGER_PRAGMAS */

#ifndef DBSQL_OMIT_PAGER_PRAGMAS
/*
** If the TEMP database is open, close it and mark the database schema
** as needing reloading.  This must be done when using the DBSQL_TEMP_STORE
** or DEFAULT_TEMP_STORE pragmas.
*/
static int __change_temp_storage(parser_t *parser, const char *storage_type){
  int ts = __get_temp_store(storage_type);
  DBSQL *db = parser->db;
  if( db->temp_store==ts ) return DBSQL_OK;
  if( __invalidate_temp_storage( parser ) != DBSQL_OK ){
    return DBSQL_ERROR;
  }
  db->temp_store = (u_int8_t)ts;
  return DBSQL_OK;
}
#endif /* DBSQL_PAGER_PRAGMAS */

/*
** Set result column names for a pragma.
*/
static void __set_pragma_result_column_names(
  vdbe_t *v,                     /* The query under construction */
  const pragma_name_t *pragma    /* The pragma */
){
  u_int8_t n = pragma->n_prag_c_name;
  __vdbe_set_num_cols(v, n==0 ? 1 : n);
  if( n==0 ){
    __vdbe_set_col_name(v, 0, COLNAME_NAME, pragma->name, DBSQL_STATIC);
  }else{
    int i, j;
    for(i=0, j=pragma->i_prag_c_name; i<n; i++, j++){
      __vdbe_set_col_name(v, i, COLNAME_NAME, __prag_c_name[j], DBSQL_STATIC);
    }
  }
}

/*
** Generate code to return a single integer value.
*/
static void __return_single_int(vdbe_t *v, int64_t value){
  __vdbe_add_op4_dup8(v, OP_Int64, 0, 1, 0, (const u_int8_t*)&value, P4_INT64);
  __vdbe_add_op2(v, OP_ResultRow, 1, 1);
}

/*
** Generate code to return a single text value.
*/
static void __return_single_text(
  vdbe_t *v,                /* Prepared statement under construction */
  const char *value      /* Value to be returned */
){
  if( value ){
    __vdbe_load_string(v, 1, (const char*)value);
    __vdbe_add_op2(v, OP_ResultRow, 1, 1);
  }
}


/*
** Set the safety_level and pager flags for pager i_db.  Or if i_db<0
** set these values for all pagers.
*/
#ifndef DBSQL_OMIT_PAGER_PRAGMAS
static void SET_ALL_PAGER_FLAGS(DBSQL *db){
  if( db->auto_commit ){
    db_t *p_db = db->dbs;
    int n = db->n_db;
    assert( DBSQL_FullFSync==PAGER_FULLFSYNC );
    assert( DBSQL_CkptFullFSync==PAGER_CKPT_FULLFSYNC );
    assert( DBSQL_CacheSpill==PAGER_CACHESPILL );
    assert( (PAGER_FULLFSYNC | PAGER_CKPT_FULLFSYNC | PAGER_CACHESPILL)
             ==  PAGER_FLAGS_MASK );
    assert( (p_db->safety_level & PAGER_SYNCHRONOUS_MASK)==p_db->safety_level );
    while( (n--) > 0 ){
      if( p_db->bt ){
        __sm_set_pager_flags(p_db->bt,
                 p_db->safety_level | (db->flags & PAGER_FLAGS_MASK) );
      }
      p_db++;
    }
  }
}
#else
# define SET_ALL_PAGER_FLAGS(X)  /* no-op */
#endif


/*
** Return a human-readable name for a constraint resolution action.
*/
#ifndef DBSQL_OMIT_FOREIGN_KEY
static const char *__action_name(u_int8_t action){
  const char *name;
  switch( action ){
    case OE_SetNull:  name = "SET NULL";        break;
    case OE_SetDflt:  name = "SET DEFAULT";     break;
    case OE_Cascade:  name = "CASCADE";         break;
    case OE_Restrict: name = "RESTRICT";        break;
    default:          name = "NO ACTION"; 
                      assert( action==OE_None ); break;
  }
  return name;
}
#endif


/*
** Parameter mode must be one of the PAGER_JOURNALMODE_XXX constants
** defined in pager.h. This function returns the associated lowercase
** journal-mode name.
*/
const char *__journal_modename(int mode){
  static char * const mode_names[] = {
    "delete", "persist", "off", "truncate", "memory"
#ifndef DBSQL_OMIT_WAL
     , "wal"
#endif
  };
  assert( PAGER_JOURNALMODE_DELETE==0 );
  assert( PAGER_JOURNALMODE_PERSIST==1 );
  assert( PAGER_JOURNALMODE_OFF==2 );
  assert( PAGER_JOURNALMODE_TRUNCATE==3 );
  assert( PAGER_JOURNALMODE_MEMORY==4 );
  assert( PAGER_JOURNALMODE_WAL==5 );
  assert( mode>=0 && mode<=ARRAY_SIZE(mode_names) );

  if( mode==ARRAY_SIZE(mode_names) ) return 0;
  return mode_names[mode];
}

/*
** Locate a pragma in the __a_pragma_name[] array.
*/
static const pragma_name_t *__pragma_locate(const char *name){
  int upr, lwr, mid = 0, rc;
  lwr = 0;
  upr = ARRAY_SIZE(__a_pragma_name)-1;
  while( lwr<=upr ){
    mid = (lwr+upr)/2;
    rc = dbsql_stricmp(name, __a_pragma_name[mid].name);
    if( rc==0 ) break;
    if( rc<0 ){
      upr = mid - 1;
    }else{
      lwr = mid + 1;
    }
  }
  return lwr>upr ? 0 : &__a_pragma_name[mid];
}

/*
** Create zero or more entries in the output for the SQL functions
** defined by func_def_t p.
*/
static void __pragma_funclist_line(
  vdbe_t *v,               /* The prepared statement being created */
  func_def_t *p,            /* A particular function definition */
  int is_builtin,         /* True if this is a built-in function */
  int show_intern_funcs    /* True if showing internal functions */
){
  u_int32_t mask =
      DBSQL_DETERMINISTIC |
      DBSQL_DIRECTONLY |
      DBSQL_SUBTYPE |
      DBSQL_INNOCUOUS |
      DBSQL_FUNC_INTERNAL
  ;
  if( show_intern_funcs ) mask = 0xffffffff;
  for(; p; p=p->next){
    const char *type;
    static const char *encs[] = { 0, "utf8", "utf16le", "utf16be" };

    assert( DBSQL_FUNC_ENCMASK==0x3 );
    assert( strcmp(encs[DBSQL_UTF8],"utf8")==0 );
    assert( strcmp(encs[DBSQL_UTF16LE],"utf16le")==0 );
    assert( strcmp(encs[DBSQL_UTF16BE],"utf16be")==0 );

    if( p->s_func==0 ) continue;
    if( (p->func_flags & DBSQL_FUNC_INTERNAL)!=0
     && show_intern_funcs==0
    ){
      continue;
    }   
    if( p->value!=0 ){
      type = "w";
    }else if( p->finalize!=0 ){
      type = "a";
    }else{
      type = "s";
    }
    __vdbe_multi_load(v, 1, "sissii",
       p->name, is_builtin,
       type, encs[p->func_flags&DBSQL_FUNC_ENCMASK],
       p->n_arg,
       (p->func_flags & mask) ^ DBSQL_INNOCUOUS
    );
  }
}


/*
** Helper subroutine for PRAGMA integrity_check:
**
** Generate code to output a single-column result row with a value of the
** string held in register 3.  Decrement the result count in register 1
** and halt if the maximum number of result rows have been issued.
*/
static int __integrity_check_result_row(vdbe_t *v){
  int addr;
  __vdbe_add_op2(v, OP_ResultRow, 3, 1);
  addr = __vdbe_add_op3(v, OP_IfPos, 1, __vdbe_current_addr(v)+2, 1);
  VDBE_COVERAGE(v);
  __vdbe_add_op0(v, OP_Halt);
  return addr;
}

/*
** Should table tab be skipped when doing an integrity_check?
** Return true or false.
**
** If obj_tab is not null, the return true if tab matches obj_tab.
**
** If obj_tab is null, then return true only if tab is an imposter table.
*/
static int __table_skip_integrity_check(const table_t *tab, const table_t *obj_tab){
  if( obj_tab ){
    return tab!=obj_tab;
  }else{
    return (tab->tab_flags & TF_Imposter)!=0;
  }
}

/*
** Process a pragma statement. 
**
** Pragmas are of this form:
**
**      PRAGMA [schema.]id [= value]
**
** The identifier might also be a string.  The value is a string, and
** identifier, or a number.  If minus_flag is true, then the value is
** a number that was preceded by a minus sign.
**
** If the left side is "database.id" then id1 is the database name
** and id2 is the id.  If the left side is just "id" then id1 is the
** id and id2 is any empty string.
*/
void __pragma(
  parser_t *parser,
  Token *id1,        /* First part of [schema.]id field */
  Token *id2,        /* Second part of [schema.]id field, or NULL */
  Token *value,      /* Token for <value>, or NULL */
  int minus_flag       /* True if a '-' sign preceded <value> */
){
  char *left = 0;       /* Nul-terminated UTF-8 string <id> */
  char *right = 0;      /* Nul-terminated UTF-8 string <value>, or NULL */
  const char *z_db = 0;   /* The database name */
  Token *id;            /* Pointer to <id> token */
  char *fcntls[4];       /* Argument to DBSQL_FCNTL_PRAGMA */
  int i_db;               /* Database index for <database> */
  int rc;                      /* return value form DBSQL_FCNTL_PRAGMA */
  DBSQL *db = parser->db;    /* The database connection */
  db_t *p_db;                     /* The specific database being pragmaed */
  vdbe_t *v = __get_vdbe(parser);  /* Prepared statement */
  const pragma_name_t *pragma;   /* The pragma */

  if( v==0 ) return;
  __vdbe_run_only_once(v);
  parser->n_mem = 2;

  /* Interpret the [schema.] part of the pragma statement. i_db is the
  ** index of the database this pragma is being applied to in db.dbs[]. */
  i_db = __two_part_name(parser, id1, id2, &id);
  if( i_db<0 ) return;
  p_db = &db->dbs[i_db];

  /* If the temp database has been explicitly named as part of the
  ** pragma, make sure it is open.
  */
  if( i_db==1 && __open_temp_database(parser) ){
    return;
  }

  left = __name_from_token(db, id);
  if( !left ) return;
  if( minus_flag ){
    right = __m_printf(db, "-%T", value);
  }else{
    right = __name_from_token(db, value);
  }

  assert( id2 );
  z_db = id2->n>0 ? p_db->db_s_name : 0;
  if( __auth_check(parser, DBSQL_PRAGMA, left, right, z_db) ){
    goto pragma_out;
  }

  /* Send an DBSQL_FCNTL_PRAGMA file-control to the underlying VFS
  ** connection.  If it returns DBSQL_OK, then assume that the VFS
  ** handled the pragma and generate a no-op prepared statement.
  **
  ** IMPLEMENTATION-OF: R-12238-55120 Whenever a PRAGMA statement is parsed,
  ** an DBSQL_FCNTL_PRAGMA file control is sent to the open dbsql_file_t
  ** object corresponding to the database file to which the pragma
  ** statement refers.
  **
  ** IMPLEMENTATION-OF: R-29875-31678 The argument to the DBSQL_FCNTL_PRAGMA
  ** file control is an array of pointers to strings (char**) in which the
  ** second element of the array is the name of the pragma and the third
  ** element is the argument to the pragma or NULL if the pragma has no
  ** argument.
  */
  fcntls[0] = 0;
  fcntls[1] = left;
  fcntls[2] = right;
  fcntls[3] = 0;
  db->busy_handler.n_busy = 0;
  rc = dbsql_file_control(db, z_db, DBSQL_FCNTL_PRAGMA, (void*)fcntls);
  if( rc==DBSQL_OK ){
    __vdbe_set_num_cols(v, 1);
    __vdbe_set_col_name(v, 0, COLNAME_NAME, fcntls[0], DBSQL_TRANSIENT);
    __return_single_text(v, fcntls[0]);
    dbsql_free(fcntls[0]);
    goto pragma_out;
  }
  if( rc!=DBSQL_NOTFOUND ){
    if( fcntls[0] ){
      __error_msg(parser, "%s", fcntls[0]);
      dbsql_free(fcntls[0]);
    }
    parser->n_err++;
    parser->rc = rc;
    goto pragma_out;
  }

  /* Locate the pragma in the lookup table */
  pragma = __pragma_locate(left);
  if( pragma==0 ){
    /* IMP: R-43042-22504 No error messages are generated if an
    ** unknown pragma is issued. */
    goto pragma_out;
  }

  /* Make sure the database schema is loaded if the pragma requires that */
  if( (pragma->m_prag_flg & PRAG_FLG_NEED_SCHEMA)!=0 ){
    if( __read_schema(parser) ) goto pragma_out;
  }

  /* Register the result column names for pragmas that return results */
  if( (pragma->m_prag_flg & PRAG_FLG_NO_COLUMNS)==0
   && ((pragma->m_prag_flg & PRAG_FLG_NO_COLUMNS1)==0 || right==0)
  ){
    __set_pragma_result_column_names(v, pragma);
  }

  /* Jump to the appropriate pragma handler */
  switch( pragma->prag_typ ){
 
#if !defined(DBSQL_OMIT_PAGER_PRAGMAS) && !defined(DBSQL_OMIT_DEPRECATED)
  /*
  **  PRAGMA [schema.]default_cache_size
  **  PRAGMA [schema.]default_cache_size=N
  **
  ** The first form reports the current persistent setting for the
  ** page cache size.  The value returned is the maximum number of
  ** pages in the page cache.  The second form sets both the current
  ** page cache size value and the persistent page cache size value
  ** stored in the database file.
  **
  ** Older versions of dbsql would set the default cache size to a
  ** negative number to indicate synchronous=OFF.  These days, synchronous
  ** is always on by default regardless of the sign of the default cache
  ** size.  But continue to take the absolute value of the default cache
  ** size of historical compatibility.
  */
  case PRAG_TYP_DEFAULT_CACHE_SIZE: {
    static const int i_ln = VDBE_OFFSET_LINENO(2);
    static const vdbe_op_list_t get_cache_size[] = {
      { OP_Transaction, 0, 0,        0},                         /* 0 */
      { OP_ReadCookie,  0, 1,        BTREE_DEFAULT_CACHE_SIZE},  /* 1 */
      { OP_IfPos,       1, 8,        0},
      { OP_Integer,     0, 2,        0},
      { OP_Subtract,    1, 2,        1},
      { OP_IfPos,       1, 8,        0},
      { OP_Integer,     0, 1,        0},                         /* 6 */
      { OP_Noop,        0, 0,        0},
      { OP_ResultRow,   1, 1,        0},
    };
    vdbe_op_t *ops;
    __vdbe_uses_btree(v, i_db);
    if( !right ){
      parser->n_mem += 2;
      __vdbe_verify_no_malloc_required(v, ARRAY_SIZE(get_cache_size));
      ops = __vdbe_add_op_list(v, ARRAY_SIZE(get_cache_size), get_cache_size, i_ln);
      if( ONLY_IF_REALLOC_STRESS(ops==0) ) break;
      ops[0].p1 = i_db;
      ops[1].p1 = i_db;
      ops[6].p1 = DBSQL_DEFAULT_CACHE_SIZE;
    }else{
      int size = __abs_int32(__atoi(right));
      __begin_write_operation(parser, 0, i_db);
      __vdbe_add_op3(v, OP_SetCookie, i_db, BTREE_DEFAULT_CACHE_SIZE, size);
      assert( __schema_mutex_held(db, i_db, 0) );
      p_db->schema->cache_size = size;
      __sm_set_cache_size(p_db->bt, p_db->schema->cache_size);
    }
    break;
  }
#endif /* !DBSQL_OMIT_PAGER_PRAGMAS && !DBSQL_OMIT_DEPRECATED */

#if !defined(DBSQL_OMIT_PAGER_PRAGMAS)
  /*
  **  PRAGMA [schema.]page_size
  **  PRAGMA [schema.]page_size=N
  **
  ** The first form reports the current setting for the
  ** database page size in bytes.  The second form sets the
  ** database page size value.  The value can only be set if
  ** the database has not yet been created.
  */
  case PRAG_TYP_PAGE_SIZE: {
    sm_t *bt = p_db->bt;
    assert( bt!=0 );
    if( !right ){
      int size = ALWAYS(bt) ? __sm_get_page_size(bt) : 0;
      __return_single_int(v, size);
    }else{
      /* Malloc may fail when setting the page-size, as there is an internal
      ** buffer that the pager module resizes using dbsql_realloc().
      */
      db->next_pagesize = __atoi(right);
      if( DBSQL_NOMEM==__sm_set_page_size(bt, db->next_pagesize,0,0) ){
        __oom_fault(db);
      }
    }
    break;
  }

  /*
  **  PRAGMA [schema.]secure_delete
  **  PRAGMA [schema.]secure_delete=ON/OFF/FAST
  **
  ** The first form reports the current setting for the
  ** secure_delete flag.  The second form changes the secure_delete
  ** flag setting and reports the new value.
  */
  case PRAG_TYP_SECURE_DELETE: {
    sm_t *bt = p_db->bt;
    int b = -1;
    assert( bt!=0 );
    if( right ){
      if( dbsql_stricmp(right, "fast")==0 ){
        b = 2;
      }else{
        b = __get_boolean(right, 0);
      }
    }
    if( id2->n==0 && b>=0 ){
      int ii;
      for(ii=0; ii<db->n_db; ii++){
        __sm_secure_delete(db->dbs[ii].bt, b);
      }
    }
    b = __sm_secure_delete(bt, b);
    __return_single_int(v, b);
    break;
  }

  /*
  **  PRAGMA [schema.]max_page_count
  **  PRAGMA [schema.]max_page_count=N
  **
  ** The first form reports the current setting for the
  ** maximum number of pages in the database file.  The
  ** second form attempts to change this setting.  Both
  ** forms return the current setting.
  **
  ** The absolute value of N is used.  This is undocumented and might
  ** change.  The only purpose is to provide an easy way to test
  ** the __abs_int32() function.
  **
  **  PRAGMA [schema.]page_count
  **
  ** Return the number of pages in the specified database.
  */
  case PRAG_TYP_PAGE_COUNT: {
    int i_reg;
    int64_t x = 0;
    __code_verify_schema(parser, i_db);
    i_reg = ++parser->n_mem;
    if( __dbsql_tolower(left[0])=='p' ){
      __vdbe_add_op2(v, OP_Pagecount, i_db, i_reg);
    }else{
      if( right && __dec_or_hex_to_i64(right,&x)==0 ){
        if( x<0 ) x = 0;
        else if( x>0xfffffffe ) x = 0xfffffffe;
      }else{
        x = 0;
      }
      __vdbe_add_op3(v, OP_MaxPgcnt, i_db, i_reg, (int)x);
    }
    __vdbe_add_op2(v, OP_ResultRow, i_reg, 1);
    break;
  }

  /*
  **  PRAGMA [schema.]locking_mode
  **  PRAGMA [schema.]locking_mode = (normal|exclusive)
  */
  case PRAG_TYP_LOCKING_MODE: {
    const char *ret = "normal";
    int mode = __get_locking_mode(right);

    if( id2->n==0 && mode==PAGER_LOCKINGMODE_QUERY ){
      /* Simple "PRAGMA locking_mode;" statement. This is a query for
      ** the current default locking mode (which may be different to
      ** the locking-mode of the main database).
      */
      mode = db->dflt_lock_mode;
    }else{
      pager_t *pager;
      if( id2->n==0 ){
        /* This indicates that no database name was specified as part
        ** of the PRAGMA command. In this case the locking-mode must be
        ** set on all attached databases, as well as the main db file.
        **
        ** Also, the DBSQL.dflt_lock_mode variable is set so that
        ** any subsequently attached databases also use the specified
        ** locking mode.
        */
        int ii;
        assert(p_db==&db->dbs[0]);
        for(ii=2; ii<db->n_db; ii++){
          pager = __sm_pager(db->dbs[ii].bt);
          __sm_pager_locking_mode(pager, mode);
        }
        db->dflt_lock_mode = (u_int8_t)mode;
      }
      pager = __sm_pager(p_db->bt);
      mode = __sm_pager_locking_mode(pager, mode);
    }

    assert( mode==PAGER_LOCKINGMODE_NORMAL
            || mode==PAGER_LOCKINGMODE_EXCLUSIVE );
    if( mode==PAGER_LOCKINGMODE_EXCLUSIVE ){
      ret = "exclusive";
    }
    __return_single_text(v, ret);
    break;
  }

  /*
  **  PRAGMA [schema.]journal_mode
  **  PRAGMA [schema.]journal_mode =
  **                      (delete|persist|off|truncate|memory|wal|off)
  */
  case PRAG_TYP_JOURNAL_MODE: {
    int mode;        /* One of the PAGER_JOURNALMODE_XXX symbols */
    int ii;           /* Loop counter */

    if( right==0 ){
      /* If there is no "=MODE" part of the pragma, do a query for the
      ** current mode */
      mode = PAGER_JOURNALMODE_QUERY;
    }else{
      const char *z_mode;
      int n = __strlen30(right);
      for(mode=0; (z_mode = __journal_modename(mode))!=0; mode++){
        if( __str_nicmp(right, z_mode, n)==0 ) break;
      }
      if( !z_mode ){
        /* If the "=MODE" part does not match any known journal mode,
        ** then do a query */
        mode = PAGER_JOURNALMODE_QUERY;
      }
      if( mode==PAGER_JOURNALMODE_OFF && (db->flags & DBSQL_Defensive)!=0 ){
        /* Do not allow journal-mode "OFF" in defensive since the database
        ** can become corrupted using ordinary SQL when the journal is off */
        mode = PAGER_JOURNALMODE_QUERY;
      }
    }
    if( mode==PAGER_JOURNALMODE_QUERY && id2->n==0 ){
      /* Convert "PRAGMA journal_mode" into "PRAGMA main.journal_mode" */
      i_db = 0;
      id2->n = 1;
    }
    for(ii=db->n_db-1; ii>=0; ii--){
      if( db->dbs[ii].bt && (ii==i_db || id2->n==0) ){
        __vdbe_uses_btree(v, ii);
        __vdbe_add_op3(v, OP_JournalMode, ii, 1, mode);
      }
    }
    __vdbe_add_op2(v, OP_ResultRow, 1, 1);
    break;
  }

  /*
  **  PRAGMA [schema.]journal_size_limit
  **  PRAGMA [schema.]journal_size_limit=N
  **
  ** Get or set the size limit on rollback journal files.
  */
  case PRAG_TYP_JOURNAL_SIZE_LIMIT: {
    pager_t *pager = __sm_pager(p_db->bt);
    int64_t i_limit = -2;
    if( right ){
      __dec_or_hex_to_i64(right, &i_limit);
      if( i_limit<-1 ) i_limit = -1;
    }
    i_limit = __sm_pager_journal_size_limit(pager, i_limit);
    __return_single_int(v, i_limit);
    break;
  }

#endif /* DBSQL_OMIT_PAGER_PRAGMAS */

  /*
  **  PRAGMA [schema.]auto_vacuum
  **  PRAGMA [schema.]auto_vacuum=N
  **
  ** Get or set the value of the database 'auto-vacuum' parameter.
  ** The value is one of:  0 NONE 1 FULL 2 INCREMENTAL
  */
#ifndef DBSQL_OMIT_AUTOVACUUM
  case PRAG_TYP_AUTO_VACUUM: {
    sm_t *bt = p_db->bt;
    assert( bt!=0 );
    if( !right ){
      __return_single_int(v, __sm_get_auto_vacuum(bt));
    }else{
      int e_auto = __get_auto_vacuum(right);
      assert( e_auto>=0 && e_auto<=2 );
      db->next_autovac = (u_int8_t)e_auto;
      /* Call __set_auto_vacuum() to set initialize the internal auto and
      ** incr-vacuum flags. This is required in case this connection
      ** creates the database file. It is important that it is created
      ** as an auto-vacuum capable db.
      */
      rc = __sm_set_auto_vacuum(bt, e_auto);
      if( rc==DBSQL_OK && (e_auto==1 || e_auto==2) ){
        /* When setting the auto_vacuum mode to either "full" or
        ** "incremental", write the value of meta[6] in the database
        ** file. Before writing to meta[6], check that meta[3] indicates
        ** that this really is an auto-vacuum capable database.
        */
        static const int i_ln = VDBE_OFFSET_LINENO(2);
        static const vdbe_op_list_t set_meta6[] = {
          { OP_Transaction,    0,         1,                 0},    /* 0 */
          { OP_ReadCookie,     0,         1,         BTREE_LARGEST_ROOT_PAGE},
          { OP_If,             1,         0,                 0},    /* 2 */
          { OP_Halt,           DBSQL_OK, OE_Abort,          0},    /* 3 */
          { OP_SetCookie,      0,         BTREE_INCR_VACUUM, 0},    /* 4 */
        };
        vdbe_op_t *ops;
        int i_addr = __vdbe_current_addr(v);
        __vdbe_verify_no_malloc_required(v, ARRAY_SIZE(set_meta6));
        ops = __vdbe_add_op_list(v, ARRAY_SIZE(set_meta6), set_meta6, i_ln);
        if( ONLY_IF_REALLOC_STRESS(ops==0) ) break;
        ops[0].p1 = i_db;
        ops[1].p1 = i_db;
        ops[2].p2 = i_addr+4;
        ops[4].p1 = i_db;
        ops[4].p3 = e_auto - 1;
        __vdbe_uses_btree(v, i_db);
      }
    }
    break;
  }
#endif

  /*
  **  PRAGMA [schema.]incremental_vacuum(N)
  **
  ** Do N steps of incremental vacuuming on a database.
  */
#ifndef DBSQL_OMIT_AUTOVACUUM
  case PRAG_TYP_INCREMENTAL_VACUUM: {
    int i_limit = 0, addr;
    if( right==0 || !__get_int32(right, &i_limit) || i_limit<=0 ){
      i_limit = 0x7fffffff;
    }
    __begin_write_operation(parser, 0, i_db);
    __vdbe_add_op2(v, OP_Integer, i_limit, 1);
    addr = __vdbe_add_op1(v, OP_IncrVacuum, i_db); VDBE_COVERAGE(v);
    __vdbe_add_op1(v, OP_ResultRow, 1);
    __vdbe_add_op2(v, OP_AddImm, 1, -1);
    __vdbe_add_op2(v, OP_IfPos, 1, addr); VDBE_COVERAGE(v);
    __vdbe_jump_here(v, addr);
    break;
  }
#endif

#ifndef DBSQL_OMIT_PAGER_PRAGMAS
  /*
  **  PRAGMA [schema.]cache_size
  **  PRAGMA [schema.]cache_size=N
  **
  ** The first form reports the current local setting for the
  ** page cache size. The second form sets the local
  ** page cache size value.  If N is positive then that is the
  ** number of pages in the cache.  If N is negative, then the
  ** number of pages is adjusted so that the cache uses -N kibibytes
  ** of memory.
  */
  case PRAG_TYP_CACHE_SIZE: {
    assert( __schema_mutex_held(db, i_db, 0) );
    if( !right ){
      __return_single_int(v, p_db->schema->cache_size);
    }else{
      int size = __atoi(right);
      p_db->schema->cache_size = size;
      __sm_set_cache_size(p_db->bt, p_db->schema->cache_size);
    }
    break;
  }

  /*
  **  PRAGMA [schema.]cache_spill
  **  PRAGMA cache_spill=BOOLEAN
  **  PRAGMA [schema.]cache_spill=N
  **
  ** The first form reports the current local setting for the
  ** page cache spill size. The second form turns cache spill on
  ** or off.  When turning cache spill on, the size is set to the
  ** current cache_size.  The third form sets a spill size that
  ** may be different form the cache size.
  ** If N is positive then that is the
  ** number of pages in the cache.  If N is negative, then the
  ** number of pages is adjusted so that the cache uses -N kibibytes
  ** of memory.
  **
  ** If the number of cache_spill pages is less then the number of
  ** cache_size pages, no spilling occurs until the page count exceeds
  ** the number of cache_size pages.
  **
  ** The cache_spill=BOOLEAN setting applies to all attached schemas,
  ** not just the schema specified.
  */
  case PRAG_TYP_CACHE_SPILL: {
    assert( __schema_mutex_held(db, i_db, 0) );
    if( !right ){
      __return_single_int(v,
         (db->flags & DBSQL_CacheSpill)==0 ? 0 :
            __sm_set_spill_size(p_db->bt,0));
    }else{
      int size = 1;
      if( __get_int32(right, &size) ){
        __sm_set_spill_size(p_db->bt, size);
      }
      if( __get_boolean(right, size!=0) ){
        db->flags |= DBSQL_CacheSpill;
      }else{
        db->flags &= ~(u_int64_t)DBSQL_CacheSpill;
      }
      SET_ALL_PAGER_FLAGS(db);
    }
    break;
  }

  /*
  **  PRAGMA [schema.]mmap_size(N)
  **
  ** Used to set mapping size limit. The mapping size limit is
  ** used to limit the aggregate size of all memory mapped regions of the
  ** database file. If this parameter is set to zero, then memory mapping
  ** is not used at all.  If N is negative, then the default memory map
  ** limit determined by dbsql_config(DBSQL_CONFIG_MMAP_SIZE) is set.
  ** The parameter N is measured in bytes.
  **
  ** This value is advisory.  The underlying VFS is free to memory map
  ** as little or as much as it wants.  Except, if N is set to 0 then the
  ** upper layers will never invoke the fetch interfaces to the VFS.
  */
  case PRAG_TYP_MMAP_SIZE: {
    int64_t sz;
#if DBSQL_MAX_MMAP_SIZE>0
    assert( __schema_mutex_held(db, i_db, 0) );
    if( right ){
      int ii;
      __dec_or_hex_to_i64(right, &sz);
      if( sz<0 ) sz = __global_config.sz_mmap;
      if( id2->n==0 ) db->sz_mmap = sz;
      for(ii=db->n_db-1; ii>=0; ii--){
        if( db->dbs[ii].bt && (ii==i_db || id2->n==0) ){
          __sm_set_mmap_limit(db->dbs[ii].bt, sz);
        }
      }
    }
    sz = -1;
    rc = dbsql_file_control(db, z_db, DBSQL_FCNTL_MMAP_SIZE, &sz);
#else
    sz = 0;
    rc = DBSQL_OK;
#endif
    if( rc==DBSQL_OK ){
      __return_single_int(v, sz);
    }else if( rc!=DBSQL_NOTFOUND ){
      parser->n_err++;
      parser->rc = rc;
    }
    break;
  }

  /*
  **   PRAGMA temp_store
  **   PRAGMA temp_store = "default"|"memory"|"file"
  **
  ** Return or set the local value of the temp_store flag.  Changing
  ** the local value does not make changes to the disk file and the default
  ** value will be restored the next time the database is opened.
  **
  ** Note that it is possible for the library compile-time options to
  ** override this setting
  */
  case PRAG_TYP_TEMP_STORE: {
    if( !right ){
      __return_single_int(v, db->temp_store);
    }else{
      __change_temp_storage(parser, right);
    }
    break;
  }

  /*
  **   PRAGMA temp_store_directory
  **   PRAGMA temp_store_directory = ""|"directory_name"
  **
  ** Return or set the local value of the temp_store_directory flag.  Changing
  ** the value sets a specific directory to be used for temporary files.
  ** Setting to a null string reverts to the default temporary directory search.
  ** If temporary directory is changed, then __invalidate_temp_storage.
  **
  */
  case PRAG_TYP_TEMP_STORE_DIRECTORY: {
    dbsql_mutex_enter(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_TEMPDIR));
    if( !right ){
      __return_single_text(v, dbsql_temp_directory);
    }else{
#ifndef DBSQL_OMIT_WSD
      if( right[0] ){
        int res;
        rc = __dbsql_os_access(db->vfs, right, DBSQL_ACCESS_READWRITE, &res);
        if( rc!=DBSQL_OK || res==0 ){
          __error_msg(parser, "not a writable directory");
          dbsql_mutex_leave(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_TEMPDIR));
          goto pragma_out;
        }
      }
      if( DBSQL_TEMP_STORE==0
       || (DBSQL_TEMP_STORE==1 && db->temp_store<=1)
       || (DBSQL_TEMP_STORE==2 && db->temp_store==1)
      ){
        __invalidate_temp_storage(parser);
      }
      dbsql_free(dbsql_temp_directory);
      if( right[0] ){
        dbsql_temp_directory = dbsql_mprintf("%s", right);
      }else{
        dbsql_temp_directory = 0;
      }
#endif /* DBSQL_OMIT_WSD */
    }
    dbsql_mutex_leave(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_TEMPDIR));
    break;
  }

#if DBSQL_OS_WIN
  /*
  **   PRAGMA data_store_directory
  **   PRAGMA data_store_directory = ""|"directory_name"
  **
  ** Return or set the local value of the data_store_directory flag.  Changing
  ** the value sets a specific directory to be used for database files that
  ** were specified with a relative pathname.  Setting to a null string reverts
  ** to the default database directory, which for database files specified with
  ** a relative path will probably be based on the current directory for the
  ** process.  Database file specified with an absolute path are not impacted
  ** by this setting, regardless of its value.
  **
  */
  case PRAG_TYP_DATA_STORE_DIRECTORY: {
    dbsql_mutex_enter(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_TEMPDIR));
    if( !right ){
      __return_single_text(v, dbsql_data_directory);
    }else{
#ifndef DBSQL_OMIT_WSD
      if( right[0] ){
        int res;
        rc = __dbsql_os_access(db->vfs, right, DBSQL_ACCESS_READWRITE, &res);
        if( rc!=DBSQL_OK || res==0 ){
          __error_msg(parser, "not a writable directory");
          dbsql_mutex_leave(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_TEMPDIR));
          goto pragma_out;
        }
      }
      dbsql_free(dbsql_data_directory);
      if( right[0] ){
        dbsql_data_directory = dbsql_mprintf("%s", right);
      }else{
        dbsql_data_directory = 0;
      }
#endif /* DBSQL_OMIT_WSD */
    }
    dbsql_mutex_leave(__dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_TEMPDIR));
    break;
  }
#endif

#if DBSQL_ENABLE_LOCKING_STYLE
  /*
  **   PRAGMA [schema.]lock_proxy_file
  **   PRAGMA [schema.]lock_proxy_file = ":auto:"|"lock_file_path"
  **
  ** Return or set the value of the lock_proxy_file flag.  Changing
  ** the value sets a specific file to be used for database access locks.
  **
  */
  case PRAG_TYP_LOCK_PROXY_FILE: {
    if( !right ){
      pager_t *pager = __sm_pager(p_db->bt);
      char *proxy_file_path = NULL;
      dbsql_file_t *file = __sm_pager_file(pager);
      __dbsql_os_file_control_hint(file, DBSQL_GET_LOCKPROXYFILE,
                           &proxy_file_path);
      __return_single_text(v, proxy_file_path);
    }else{
      pager_t *pager = __sm_pager(p_db->bt);
      dbsql_file_t *file = __sm_pager_file(pager);
      int res;
      if( right[0] ){
        res=__dbsql_os_file_control(file, DBSQL_SET_LOCKPROXYFILE,
                                     right);
      } else {
        res=__dbsql_os_file_control(file, DBSQL_SET_LOCKPROXYFILE,
                                     NULL);
      }
      if( res!=DBSQL_OK ){
        __error_msg(parser, "failed to set lock proxy file");
        goto pragma_out;
      }
    }
    break;
  }
#endif /* DBSQL_ENABLE_LOCKING_STYLE */     
   
  /*
  **   PRAGMA [schema.]synchronous
  **   PRAGMA [schema.]synchronous=OFF|ON|NORMAL|FULL|EXTRA
  **
  ** Return or set the local value of the synchronous flag.  Changing
  ** the local value does not make changes to the disk file and the
  ** default value will be restored the next time the database is
  ** opened.
  */
  case PRAG_TYP_SYNCHRONOUS: {
    if( !right ){
      __return_single_int(v, p_db->safety_level-1);
    }else{
      if( !db->auto_commit ){
        __error_msg(parser,
            "Safety level may not be changed inside a transaction");
      }else if( i_db!=1 ){
        int i_level = (__get_safety_level(right,0,1)+1) & PAGER_SYNCHRONOUS_MASK;
        if( i_level==0 ) i_level = 1;
        p_db->safety_level = i_level;
        p_db->sync_set = 1;
        SET_ALL_PAGER_FLAGS(db);
      }
    }
    break;
  }
#endif /* DBSQL_OMIT_PAGER_PRAGMAS */

#ifndef DBSQL_OMIT_FLAG_PRAGMAS
  case PRAG_TYP_FLAG: {
    if( right==0 ){
      __set_pragma_result_column_names(v, pragma);
      __return_single_int(v, (db->flags & pragma->i_arg)!=0 );
    }else{
      u_int64_t mask = pragma->i_arg;    /* Mask of bits to set or clear. */
      if( db->auto_commit==0 ){
        /* Foreign key support may not be enabled or disabled while not
        ** in auto-commit mode.  */
        mask &= ~(DBSQL_ForeignKeys);
      }

      if( __get_boolean(right, 0) ){
        if( (mask & DBSQL_WriteSchema)==0
         || (db->flags & DBSQL_Defensive)==0
        ){
          db->flags |= mask;
        }
      }else{
        db->flags &= ~mask;
        if( mask==DBSQL_DeferFKs ){
          db->n_deferred_imm_cons = 0;
          db->n_deferred_cons = 0;
        }
        if( (mask & DBSQL_WriteSchema)!=0
         && dbsql_stricmp(right, "reset")==0
        ){
          /* IMP: R-60817-01178 If the argument is "RESET" then schema
          ** writing is disabled (as with "PRAGMA writable_schema=OFF") and,
          ** in addition, the schema is reloaded. */
          __reset_all_schemas_of_connection(db);
        }
      }

      /* Many of the flag-pragmas modify the code generated by the SQL
      ** compiler (eg. count_changes). So add an opcode to expire all
      ** compiled SQL statements after modifying a pragma value.
      */
      __vdbe_add_op0(v, OP_Expire);
      SET_ALL_PAGER_FLAGS(db);
    }
    break;
  }
#endif /* DBSQL_OMIT_FLAG_PRAGMAS */

#ifndef DBSQL_OMIT_SCHEMA_PRAGMAS
  /*
  **   PRAGMA table_info(<table>)
  **
  ** Return a single row for each column of the named table. The columns of
  ** the returned data set are:
  **
  ** cid:        column_t id (numbered from left to right, starting at 0)
  ** name:       column_t name
  ** type:       column_t declaration type.
  ** notnull:    True if 'NOT NULL' is part of column declaration
  ** dflt_value: The default value for the column, if any.
  ** pk:         Non-zero for PK fields.
  */
  case PRAG_TYP_TABLE_INFO: if( right ){
    table_t *tab;
    __code_verify_named_schema(parser, z_db);
    tab = __locate_table(parser, LOCATE_NOERR, right, z_db);
    if( tab ){
      int i, k;
      int n_hidden = 0;
      column_t *col;
      index_t *pk = __primary_key_index(tab);
      parser->n_mem = 7;
      __view_get_column_names(parser, tab);
      for(i=0, col=tab->cols; i<tab->n_col; i++, col++){
        int is_hidden = 0;
        const expr_t *col_expr;
        if( col->col_flags & COLFLAG_NOINSERT ){
          if( pragma->i_arg==0 ){
            n_hidden++;
            continue;
          }
          if( col->col_flags & COLFLAG_VIRTUAL ){
            is_hidden = 2;  /* GENERATED ALWAYS AS ... VIRTUAL */
          }else if( col->col_flags & COLFLAG_STORED ){
            is_hidden = 3;  /* GENERATED ALWAYS AS ... STORED */
          }else{ assert( col->col_flags & COLFLAG_HIDDEN );
            is_hidden = 1;  /* HIDDEN */
          }
        }
        if( (col->col_flags & COLFLAG_PRIMKEY)==0 ){
          k = 0;
        }else if( pk==0 ){
          k = 1;
        }else{
          for(k=1; k<=tab->n_col && pk->columns[k-1]!=i; k++){}
        }
        col_expr = __column_expr(tab,col);
        assert( col_expr==0 || col_expr->op==TK_SPAN || is_hidden>=2 );
        assert( col_expr==0 || !EXPR_HAS_PROPERTY(col_expr, EP_IntValue)
                  || is_hidden>=2 );
        __vdbe_multi_load(v, 1, pragma->i_arg ? "issisii" : "issisi",
               i-n_hidden,
               col->cn_name,
               __column_type(col,""),
               col->not_null ? 1 : 0,
               (is_hidden>=2 || col_expr==0) ? 0 : col_expr->u.token,
               k,
               is_hidden);
      }
    }
  }
  break;

  /*
  **   PRAGMA table_list
  **
  ** Return a single row for each table, virtual table, or view in the
  ** entire schema.
  **
  ** schema:     Name of attached database hold this table
  ** name:       Name of the table itself
  ** type:       "table", "view", "virtual", "shadow"
  ** ncol:       Number of columns
  ** wr:         True for a WITHOUT ROWID table
  ** strict:     True for a STRICT table
  */
  case PRAG_TYP_TABLE_LIST: {
    int ii;
    parser->n_mem = 6;
    __code_verify_named_schema(parser, z_db);
    for(ii=0; ii<db->n_db; ii++){
      hash_elem_t *k;
      hash_t *hash;
      int init_n_col;
      if( z_db && dbsql_stricmp(z_db, db->dbs[ii].db_s_name)!=0 ) continue;

      /* Ensure that the table_t.n_col field is initialized for all views
      ** and virtual tables.  Each time we initialize a table_t.n_col value
      ** for a table, that can potentially disrupt the hash table, so restart
      ** the initialization scan.
      */
      hash = &db->dbs[ii].schema->tbl_hash;
      init_n_col = dbsql_hash_count(hash);
      while( init_n_col-- ){
        for(k=dbsql_hash_first(hash); 1; k=dbsql_hash_next(k) ){
          table_t *tab;
          if( k==0 ){ init_n_col = 0; break; }
          tab = dbsql_hash_data(k);
          if( tab->n_col==0 ){
            char *sql = __m_printf(db, "SELECT*FROM\"%w\"", tab->name);
            if( sql ){
              dbsql_stmt_t *dummy = 0;
              (void)dbsql_prepare_v3(db, sql, -1, DBSQL_PREPARE_DONT_LOG,
                                       &dummy, 0);
              (void)dbsql_finalize(dummy);
              __dbsql_db_free(db, sql);
            }
            if( db->malloc_failed ){
              __error_msg(db->parser, "out of memory");
              db->parser->rc = DBSQL_NOMEM_BKPT;
            }
            hash = &db->dbs[ii].schema->tbl_hash;
            break;
          }
        }
      }

      for(k=dbsql_hash_first(hash); k; k=dbsql_hash_next(k) ){
        table_t *tab = dbsql_hash_data(k);
        const char *type;
        if( right && dbsql_stricmp(right, tab->name)!=0 ) continue;
        if( IsView(tab) ){
          type = "view";
        }else if( IS_VIRTUAL(tab) ){
          type = "virtual";
        }else if( tab->tab_flags & TF_Shadow ){
          type = "shadow";
        }else{
          type = "table";
        }
        __vdbe_multi_load(v, 1, "sssiii",
           db->dbs[ii].db_s_name,
           __preferred_table_name(tab->name),
           type,
           tab->n_col,
           (tab->tab_flags & TF_WithoutRowid)!=0,
           (tab->tab_flags & TF_Strict)!=0
        );
      }
    }
  }
  break;

#ifdef DBSQL_DEBUG
  case PRAG_TYP_STATS: {
    index_t *idx;
    hash_elem_t *i;
    parser->n_mem = 5;
    __code_verify_schema(parser, i_db);
    for(i=dbsql_hash_first(&p_db->schema->tbl_hash); i; i=dbsql_hash_next(i)){
      table_t *tab = dbsql_hash_data(i);
      __vdbe_multi_load(v, 1, "ssiii",
           __preferred_table_name(tab->name),
           0,
           tab->sz_tab_row,
           tab->n_row_log_est,
           tab->tab_flags);
      for(idx=tab->p_index; idx; idx=idx->next){
        __vdbe_multi_load(v, 2, "siiiX",
           idx->name,
           idx->sz_idx_row,
           idx->row_log_ests[0],
           idx->has_stat1);
        __vdbe_add_op2(v, OP_ResultRow, 1, 5);
      }
    }
  }
  break;
#endif

  case PRAG_TYP_INDEX_INFO: if( right ){
    index_t *idx;
    table_t *tab;
    idx = __find_index(db, right, z_db);
    if( idx==0 ){
      /* If there is no index named right, check to see if there is a
      ** WITHOUT ROWID table named right, and if there is, show the
      ** structure of the PRIMARY KEY index for that table. */
      tab = __locate_table(parser, LOCATE_NOERR, right, z_db);
      if( tab && !HAS_ROWID(tab) ){
        idx = __primary_key_index(tab);
      }
    }
    if( idx ){
      int i_idx_db = __schema_to_index(db, idx->schema);
      int i;
      int mx;
      if( pragma->i_arg ){
        /* PRAGMA index_xinfo (newer version with more rows and columns) */
        mx = idx->n_column;
        parser->n_mem = 6;
      }else{
        /* PRAGMA index_info (legacy version) */
        mx = idx->n_key_col;
        parser->n_mem = 3;
      }
      tab = idx->table;
      __code_verify_schema(parser, i_idx_db);
      assert( parser->n_mem<=pragma->n_prag_c_name );
      for(i=0; i<mx; i++){
        int16_t cnum = idx->columns[i];
        __vdbe_multi_load(v, 1, "iisX", i, cnum,
                             cnum<0 ? 0 : tab->cols[cnum].cn_name);
        if( pragma->i_arg ){
          __vdbe_multi_load(v, 4, "isiX",
            idx->sort_orders[i],
            idx->colls[i],
            i<idx->n_key_col);
        }
        __vdbe_add_op2(v, OP_ResultRow, 1, parser->n_mem);
      }
    }
  }
  break;

  case PRAG_TYP_INDEX_LIST: if( right ){
    index_t *idx;
    table_t *tab;
    int i;
    tab = __find_table(db, right, z_db);
    if( tab ){
      int i_tab_db = __schema_to_index(db, tab->schema);
      parser->n_mem = 5;
      __code_verify_schema(parser, i_tab_db);
      for(idx=tab->p_index, i=0; idx; idx=idx->next, i++){
        const char *origins[] = { "c", "u", "pk" };
        __vdbe_multi_load(v, 1, "isisi",
           i,
           idx->name,
           IS_UNIQUE_INDEX(idx),
           origins[idx->idx_type],
           idx->part_idx_where!=0);
      }
    }
  }
  break;

  case PRAG_TYP_DATABASE_LIST: {
    int i;
    parser->n_mem = 3;
    for(i=0; i<db->n_db; i++){
      if( db->dbs[i].bt==0 ) continue;
      assert( db->dbs[i].db_s_name!=0 );
      __vdbe_multi_load(v, 1, "iss",
         i,
         db->dbs[i].db_s_name,
         __sm_get_filename(db->dbs[i].bt));
    }
  }
  break;

  case PRAG_TYP_COLLATION_LIST: {
    int i = 0;
    hash_elem_t *p;
    parser->n_mem = 2;
    for(p=dbsql_hash_first(&db->coll_seqs); p; p=dbsql_hash_next(p)){
      coll_seq_t *coll = (coll_seq_t *)dbsql_hash_data(p);
      __vdbe_multi_load(v, 1, "is", i++, coll->name);
    }
  }
  break;

#ifndef DBSQL_OMIT_INTROSPECTION_PRAGMAS
  case PRAG_TYP_FUNCTION_LIST: {
    int i;
    hash_elem_t *j;
    func_def_t *p;
    int show_intern_func = (db->m_db_flags & DBFLAG_InternalFunc)!=0;
    parser->n_mem = 6;
    for(i=0; i<DBSQL_FUNC_HASH_SZ; i++){
      for(p=__builtin_functions.a[i]; p; p=p->u.hash ){
        assert( p->func_flags & DBSQL_FUNC_BUILTIN );
        __pragma_funclist_line(v, p, 1, show_intern_func);
      }
    }
    for(j=dbsql_hash_first(&db->funcs); j; j=dbsql_hash_next(j)){
      p = (func_def_t*)dbsql_hash_data(j);
      assert( (p->func_flags & DBSQL_FUNC_BUILTIN)==0 );
      __pragma_funclist_line(v, p, 0, show_intern_func);
    }
  }
  break;

#ifndef DBSQL_OMIT_VIRTUALTABLE
  case PRAG_TYP_MODULE_LIST: {
    hash_elem_t *j;
    parser->n_mem = 1;
    for(j=dbsql_hash_first(&db->modules); j; j=dbsql_hash_next(j)){
      module_def_t *mod = (module_def_t*)dbsql_hash_data(j);
      __vdbe_multi_load(v, 1, "s", mod->name);
    }
  }
  break;
#endif /* DBSQL_OMIT_VIRTUALTABLE */

  case PRAG_TYP_PRAGMA_LIST: {
    int i;
    for(i=0; i<ARRAY_SIZE(__a_pragma_name); i++){
      __vdbe_multi_load(v, 1, "s", __a_pragma_name[i].name);
    }
  }
  break;
#endif /* DBSQL_INTROSPECTION_PRAGMAS */

#endif /* DBSQL_OMIT_SCHEMA_PRAGMAS */

#ifndef DBSQL_OMIT_FOREIGN_KEY
  case PRAG_TYP_FOREIGN_KEY_LIST: if( right ){
    f_key_t *fk;
    table_t *tab;
    tab = __find_table(db, right, z_db);
    if( tab && IS_ORDINARY_TABLE(tab) ){
      fk = tab->u.tab.f_key;
      if( fk ){
        int i_tab_db = __schema_to_index(db, tab->schema);
        int i = 0;
        parser->n_mem = 8;
        __code_verify_schema(parser, i_tab_db);
        while(fk){
          int j;
          for(j=0; j<fk->n_col; j++){
            __vdbe_multi_load(v, 1, "iissssss",
                   i,
                   j,
                   fk->to,
                   tab->cols[fk->cols[j].i_from].cn_name,
                   fk->cols[j].col,
                   __action_name(fk->actions[1]),  /* ON UPDATE */
                   __action_name(fk->actions[0]),  /* ON DELETE */
                   "NONE");
          }
          ++i;
          fk = fk->next_from;
        }
      }
    }
  }
  break;
#endif /* !defined(DBSQL_OMIT_FOREIGN_KEY) */

#ifndef DBSQL_OMIT_FOREIGN_KEY
#ifndef DBSQL_OMIT_TRIGGER
  case PRAG_TYP_FOREIGN_KEY_CHECK: {
    f_key_t *fk;             /* A foreign key constraint */
    table_t *tab;           /* Child table contain "REFERENCES" keyword */
    table_t *parent;        /* Parent table that child points to */
    index_t *idx;           /* index_t in the parent table */
    int i;                 /* Loop counter:  Foreign key number for tab */
    int j;                 /* Loop counter:  Field of the foreign key */
    hash_elem_t *k;           /* Loop counter:  Next table in schema */
    int x;                 /* result variable */
    int reg_result;         /* 3 registers to hold a result row */
    int reg_row;            /* Registers to hold a row from tab */
    int addr_top;           /* Top of a loop checking foreign keys */
    int addr_ok;            /* Jump here if the key is OK */
    int *cols;           /* child to parent column mapping */

    reg_result = parser->n_mem+1;
    parser->n_mem += 4;
    reg_row = ++parser->n_mem;
    k = dbsql_hash_first(&db->dbs[i_db].schema->tbl_hash);
    while( k ){
      if( right ){
        tab = __locate_table(parser, 0, right, z_db);
        k = 0;
      }else{
        tab = (table_t*)dbsql_hash_data(k);
        k = dbsql_hash_next(k);
      }
      if( tab==0 || !IS_ORDINARY_TABLE(tab) || tab->u.tab.f_key==0 ) continue;
      i_db = __schema_to_index(db, tab->schema);
      z_db = db->dbs[i_db].db_s_name;
      __code_verify_schema(parser, i_db);
      __table_lock(parser, i_db, tab->tnum, 0, tab->name);
      __touch_register(parser, tab->n_col+reg_row);
      __open_table(parser, 0, i_db, tab, OP_OpenRead);
      __vdbe_load_string(v, reg_result, tab->name);
      assert( IS_ORDINARY_TABLE(tab) );
      for(i=1, fk=tab->u.tab.f_key; fk; i++, fk=fk->next_from){
        parent = __find_table(db, fk->to, z_db);
        if( parent==0 ) continue;
        idx = 0;
        __table_lock(parser, i_db, parent->tnum, 0, parent->name);
        x = __fk_locate_index(parser, parent, fk, &idx, 0);
        if( x==0 ){
          if( idx==0 ){
            __open_table(parser, i, i_db, parent, OP_OpenRead);
          }else{
            __vdbe_add_op3(v, OP_OpenRead, i, idx->tnum, i_db);
            __vdbe_set_p4_key_info(parser, idx);
          }
        }else{
          k = 0;
          break;
        }
      }
      assert( parser->n_err>0 || fk==0 );
      if( fk ) break;
      if( parser->n_tab<i ) parser->n_tab = i;
      addr_top = __vdbe_add_op1(v, OP_Rewind, 0); VDBE_COVERAGE(v);
      assert( IS_ORDINARY_TABLE(tab) );
      for(i=1, fk=tab->u.tab.f_key; fk; i++, fk=fk->next_from){
        parent = __find_table(db, fk->to, z_db);
        idx = 0;
        cols = 0;
        if( parent ){
          x = __fk_locate_index(parser, parent, fk, &idx, &cols);
          assert( x==0 || db->malloc_failed );
        }
        addr_ok = __vdbe_make_label(parser);

        /* Generate code to read the child key values into registers
        ** reg_row..reg_row+n. If any of the child key values are NULL, this
        ** row cannot cause an FK violation. Jump directly to addr_ok in
        ** this case. */
        __touch_register(parser, reg_row + fk->n_col);
        for(j=0; j<fk->n_col; j++){
          int i_col = cols ? cols[j] : fk->cols[j].i_from;
          __expr_code_get_column_of_table(v, tab, 0, i_col, reg_row+j);
          __vdbe_add_op2(v, OP_IsNull, reg_row+j, addr_ok); VDBE_COVERAGE(v);
        }

        /* Generate code to query the parent index for a matching parent
        ** key. If a match is found, jump to addr_ok. */
        if( idx ){
          __vdbe_add_op4(v, OP_Affinity, reg_row, fk->n_col, 0,
              __index_affinity_str(db,idx), fk->n_col);
          __vdbe_add_op4_int(v, OP_Found, i, addr_ok, reg_row, fk->n_col);
          VDBE_COVERAGE(v);
        }else if( parent ){
          int jmp = __vdbe_current_addr(v)+2;
          __vdbe_add_op3(v, OP_SeekRowid, i, jmp, reg_row); VDBE_COVERAGE(v);
          __vdbe_goto(v, addr_ok);
          assert( fk->n_col==1 || db->malloc_failed );
        }

        /* Generate code to report an FK violation to the caller. */
        if( HAS_ROWID(tab) ){
          __vdbe_add_op2(v, OP_Rowid, 0, reg_result+1);
        }else{
          __vdbe_add_op2(v, OP_Null, 0, reg_result+1);
        }
        __vdbe_multi_load(v, reg_result+2, "siX", fk->to, i-1);
        __vdbe_add_op2(v, OP_ResultRow, reg_result, 4);
        __vdbe_resolve_label(v, addr_ok);
        __dbsql_db_free(db, cols);
      }
      __vdbe_add_op2(v, OP_Next, 0, addr_top+1); VDBE_COVERAGE(v);
      __vdbe_jump_here(v, addr_top);
    }
  }
  break;
#endif /* !defined(DBSQL_OMIT_TRIGGER) */
#endif /* !defined(DBSQL_OMIT_FOREIGN_KEY) */

#ifndef DBSQL_OMIT_CASE_SENSITIVE_LIKE_PRAGMA
  /* Reinstall the LIKE and GLOB functions.  The variant of LIKE
  ** used will be case sensitive or not depending on the RHS.
  */
  case PRAG_TYP_CASE_SENSITIVE_LIKE: {
    if( right ){
      __register_like_functions(db, __get_boolean(right, 0));
    }
  }
  break;
#endif /* DBSQL_OMIT_CASE_SENSITIVE_LIKE_PRAGMA */

#ifndef DBSQL_INTEGRITY_CHECK_ERROR_MAX
# define DBSQL_INTEGRITY_CHECK_ERROR_MAX 100
#endif

#ifndef DBSQL_OMIT_INTEGRITY_CHECK
  /*    PRAGMA integrity_check
  **    PRAGMA integrity_check(N)
  **    PRAGMA quick_check
  **    PRAGMA quick_check(N)
  **
  ** Verify the integrity of the database.
  **
  ** The "quick_check" is reduced version of
  ** integrity_check designed to detect most database corruption
  ** without the overhead of cross-checking indexes.  Quick_check
  ** is linear time whereas integrity_check is O(__nlog_n).
  **
  ** The maximum number of errors is 100 by default.  A different default
  ** can be specified using a numeric parameter N.
  **
  ** Or, the parameter N can be the name of a table.  In that case, only
  ** the one table named is verified.  The freelist is only verified if
  ** the named table is "dbsql_schema" (or one of its aliases).
  **
  ** All schemas are checked by default.  To check just a single
  ** schema, use the form:
  **
  **      PRAGMA schema.integrity_check;
  */
  case PRAG_TYP_INTEGRITY_CHECK: {
    int i, j, addr, mx_err;
    table_t *obj_tab = 0;     /* Check only this one table, if not NULL */

    int is_quick = (__dbsql_tolower(left[0])=='q');

    /* If the PRAGMA command was of the form "PRAGMA <db>.integrity_check",
    ** then i_db is set to the index of the database identified by <db>.
    ** In this case, the integrity of database i_db only is verified by
    ** the VDBE created below.
    **
    ** Otherwise, if the command was simply "PRAGMA integrity_check" (or
    ** "PRAGMA quick_check"), then i_db is set to 0. In this case, set i_db
    ** to -1 here, to indicate that the VDBE should verify the integrity
    ** of all attached databases.  */
    assert( i_db>=0 );
    assert( i_db==0 || id2->z );
    if( id2->z==0 ) i_db = -1;

    /* Initialize the VDBE program */
    parser->n_mem = 6;

    /* Set the maximum error count */
    mx_err = DBSQL_INTEGRITY_CHECK_ERROR_MAX;
    if( right ){
      if( __get_int32(value->z, &mx_err) ){
        if( mx_err<=0 ){
          mx_err = DBSQL_INTEGRITY_CHECK_ERROR_MAX;
        }
      }else{
        obj_tab = __locate_table(parser, 0, right,
                      i_db>=0 ? db->dbs[i_db].db_s_name : 0);
      }
    }
    __vdbe_add_op2(v, OP_Integer, mx_err-1, 1); /* reg[1] holds errors left */

    /* Do an integrity check on each database file */
    for(i=0; i<db->n_db; i++){
      hash_elem_t *x;     /* For looping over tables in the schema */
      hash_t *tbls;     /* Set of all tables in the schema */
      int *roots;      /* Array of root page numbers of all btrees */
      int cnt = 0;     /* Number of entries in roots[] */

      if( OMIT_TEMPDB && i==1 ) continue;
      if( i_db>=0 && i!=i_db ) continue;

      __code_verify_schema(parser, i);
      parser->ok_const_factor = 0;  /* tag-20230327-1 */

      /* Do an integrity check of the B-Tree
      **
      ** Begin by finding the root pages numbers
      ** for all tables and indices in the database.
      */
      assert( __schema_mutex_held(db, i, 0) );
      tbls = &db->dbs[i].schema->tbl_hash;
      for(cnt=0, x=dbsql_hash_first(tbls); x; x=dbsql_hash_next(x)){
        table_t *tab = dbsql_hash_data(x);  /* Current table */
        index_t *idx;                      /* An index on tab */
        int n_idx;                         /* Number of indexes on tab */
        if( __table_skip_integrity_check(tab,obj_tab) ) continue;
        if( HAS_ROWID(tab) ) cnt++;
        for(n_idx=0, idx=tab->p_index; idx; idx=idx->next, n_idx++){ cnt++; }
      }
      if( cnt==0 ) continue;
      if( obj_tab ) cnt++;
      roots = __db_malloc_raw_nn(db, sizeof(int)*(cnt+1));
      if( roots==0 ) break;
      cnt = 0;
      if( obj_tab ) roots[++cnt] = 0;
      for(x=dbsql_hash_first(tbls); x; x=dbsql_hash_next(x)){
        table_t *tab = dbsql_hash_data(x);
        index_t *idx;
        if( __table_skip_integrity_check(tab,obj_tab) ) continue;
        if( HAS_ROWID(tab) ) roots[++cnt] = tab->tnum;
        for(idx=tab->p_index; idx; idx=idx->next){
          roots[++cnt] = idx->tnum;
        }
      }
      roots[0] = cnt;

      /* Make sure sufficient number of registers have been allocated */
      __touch_register(parser, 8+cnt);
      __vdbe_add_op3(v, OP_Null, 0, 8, 8+cnt);
      __clear_temp_reg_cache(parser);

      /* Do the b-tree integrity checks */
      __vdbe_add_op4(v, OP_IntegrityCk, 1, cnt, 8, (char*)roots,P4_INTARRAY);
      __vdbe_change_p5(v, (u_int16_t)i);
      addr = __vdbe_add_op1(v, OP_IsNull, 2); VDBE_COVERAGE(v);
      __vdbe_add_op4(v, OP_String8, 0, 3, 0,
         __m_printf(db, "*** in database %s ***\n", db->dbs[i].db_s_name),
         P4_DYNAMIC);
      __vdbe_add_op3(v, OP_Concat, 2, 3, 3);
      __integrity_check_result_row(v);
      __vdbe_jump_here(v, addr);

      /* Check that the indexes all have the right number of rows */
      cnt = obj_tab ? 1 : 0;
      __vdbe_load_string(v, 2, "wrong # of entries in index ");
      for(x=dbsql_hash_first(tbls); x; x=dbsql_hash_next(x)){
        int i_tab = 0;
        table_t *tab = dbsql_hash_data(x);
        index_t *idx;
        if( __table_skip_integrity_check(tab,obj_tab) ) continue;
        if( HAS_ROWID(tab) ){
          i_tab = cnt++;
        }else{
          i_tab = cnt;
          for(idx=tab->p_index; ALWAYS(idx); idx=idx->next){
            if( IS_PRIMARY_KEY_INDEX(idx) ) break;
            i_tab++;
          }
        }
        for(idx=tab->p_index; idx; idx=idx->next){
          if( idx->part_idx_where==0 ){
            addr = __vdbe_add_op3(v, OP_Eq, 8+cnt, 0, 8+i_tab);
            VDBE_COVERAGE_NEVER_NULL(v);
            __vdbe_load_string(v, 4, idx->name);
            __vdbe_add_op3(v, OP_Concat, 4, 2, 3);
            __integrity_check_result_row(v);
            __vdbe_jump_here(v, addr);
          }
          cnt++;
        }
      }

      /* Make sure all the indices are constructed correctly.
      */
      for(x=dbsql_hash_first(tbls); x; x=dbsql_hash_next(x)){
        table_t *tab = dbsql_hash_data(x);
        index_t *idx, *pk;
        index_t *prior = 0;      /* Previous index */
        int loop_top;
        int i_data_cur, i_idx_cur;
        int r1 = -1;
        int strict;            /* True for a STRICT table */
        int r2;                 /* Previous key for WITHOUT ROWID tables */
        int mx_col;              /* Maximum non-virtual column number */

        if( __table_skip_integrity_check(tab,obj_tab) ) continue;
        if( !IS_ORDINARY_TABLE(tab) ) continue;
        if( is_quick || HAS_ROWID(tab) ){
          pk = 0;
          r2 = 0;
        }else{
          pk = __primary_key_index(tab);
          r2 = __get_temp_range(parser, pk->n_key_col);
          __vdbe_add_op3(v, OP_Null, 1, r2, r2+pk->n_key_col-1);
        }
        __open_table_and_indices(parser, tab, OP_OpenRead, 0,
                                   1, 0, &i_data_cur, &i_idx_cur);
        /* reg[7] counts the number of entries in the table.
        ** reg[8+i] counts the number of entries in the i-th index
        */
        __vdbe_add_op2(v, OP_Integer, 0, 7);
        for(j=0, idx=tab->p_index; idx; idx=idx->next, j++){
          __vdbe_add_op2(v, OP_Integer, 0, 8+j); /* index entries counter */
        }
        assert( parser->n_mem>=8+j );
        assert( __no_temps_in_range(parser,1,7+j) );
        __vdbe_add_op2(v, OP_Rewind, i_data_cur, 0); VDBE_COVERAGE(v);
        loop_top = __vdbe_add_op2(v, OP_AddImm, 7, 1);

        /* Fetch the right-most column from the table.  This will cause
        ** the entire record header to be parsed and sanity checked.  It
        ** will also prepopulate the cursor column cache that is used
        ** by the OP_IsType code, so it is a required step.
        */
        assert( !IS_VIRTUAL(tab) );
        if( HAS_ROWID(tab) ){
          mx_col = -1;
          for(j=0; j<tab->n_col; j++){
            if( (tab->cols[j].col_flags & COLFLAG_VIRTUAL)==0 ) mx_col++;
          }
          if( mx_col==tab->i_p_key ) mx_col--;
        }else{
          /* COLFLAG_VIRTUAL columns are not included in the WITHOUT ROWID
          ** PK index column-count, so there is no need to account for them
          ** in this case. */
          mx_col = __primary_key_index(tab)->n_column-1;
        }
        if( mx_col>=0 ){
          __vdbe_add_op3(v, OP_Column, i_data_cur, mx_col, 3);
          __vdbe_typeof_column(v, 3);
        }

        if( !is_quick ){
          if( pk ){
            /* Verify WITHOUT ROWID keys are in ascending order */
            int a1;
            char *err;
            a1 = __vdbe_add_op4_int(v, OP_IdxGT, i_data_cur, 0,r2,pk->n_key_col);
            VDBE_COVERAGE(v);
            __vdbe_add_op1(v, OP_IsNull, r2); VDBE_COVERAGE(v);
            err = __m_printf(db,
                   "row not in PRIMARY KEY order for %s",
                    tab->name);
            __vdbe_add_op4(v, OP_String8, 0, 3, 0, err, P4_DYNAMIC);
            __integrity_check_result_row(v);
            __vdbe_jump_here(v, a1);
            __vdbe_jump_here(v, a1+1);
            for(j=0; j<pk->n_key_col; j++){
              __expr_code_load_index_column(parser, pk, i_data_cur, j, r2+j);
            }
          }
        }
        /* Verify datatypes for all columns:
        **
        **   (1) NOT NULL columns may not contain a NULL
        **   (2) Datatype must be exact for non-ANY columns in STRICT tables
        **   (3) Datatype for TEXT columns in non-STRICT tables must be
        **       NULL, TEXT, or BLOB.
        **   (4) Datatype for numeric columns in non-STRICT tables must not
        **       be a TEXT value that can be losslessly converted to numeric.
        */
        strict = (tab->tab_flags & TF_Strict)!=0;
        for(j=0; j<tab->n_col; j++){
          char *err;
          column_t *col = tab->cols + j;  /* The column to be checked */
          int label_error;               /* Jump here to report an error */
          int label_ok;                  /* Jump here if all looks ok */
          int p1, p3, p4;               /* Operands to the OP_IsType opcode */
          int do_type_check;              /* Check datatypes (besides NOT NULL) */

          if( j==tab->i_p_key ) continue;
          if( strict ){
            do_type_check = col->c_type>COLTYPE_ANY;
          }else{
            do_type_check = col->affinity>DBSQL_AFF_BLOB;
          }
          if( col->not_null==0 && !do_type_check ) continue;

          /* Compute the operands that will be needed for OP_IsType */
          p4 = DBSQL_NULL;
          if( col->col_flags & COLFLAG_VIRTUAL ){
            __expr_code_get_column_of_table(v, tab, i_data_cur, j, 3);
            p1 = -1;
            p3 = 3;
          }else{
            if( col->i_dflt ){
              dbsql_value_t *dflt_value = 0;
              __value_from_expr(db, __column_expr(tab,col), ENC(db),
                                   col->affinity, &dflt_value);
              if( dflt_value ){
                p4 = dbsql_value_type(dflt_value);
                __value_free(dflt_value);
              }
            }
            p1 = i_data_cur;
            if( !HAS_ROWID(tab) ){
              testcase( j!=__table_column_to_storage(tab, j) );
              p3 = __table_column_to_index(__primary_key_index(tab), j);
            }else{
              p3 = __table_column_to_storage(tab,j);
              testcase( p3!=j);
            }
          }

          label_error = __vdbe_make_label(parser);
          label_ok = __vdbe_make_label(parser);
          if( col->not_null ){
            /* (1) NOT NULL columns may not contain a NULL */
            int jmp3;
            int jmp2 = __vdbe_add_op4_int(v, OP_IsType, p1, label_ok, p3, p4);
            VDBE_COVERAGE(v);
            if( p1<0 ){
              __vdbe_change_p5(v, 0x0f); /* INT, REAL, TEXT, or BLOB */
              jmp3 = jmp2;
            }else{
              __vdbe_change_p5(v, 0x0d); /* INT, TEXT, or BLOB */
              /* OP_IsType does not detect __na_n values in the database file
              ** which should be treated as a NULL.  So if the header type
              ** is REAL, we have to load the actual data using OP_Column
              ** to reliably determine if the value is a NULL. */
              __vdbe_add_op3(v, OP_Column, p1, p3, 3);
              __column_default(v, tab, j, 3);
              jmp3 = __vdbe_add_op2(v, OP_NotNull, 3, label_ok);
              VDBE_COVERAGE(v);
            }           
            err = __m_printf(db, "NULL value in %s.%s", tab->name,
                                col->cn_name);
            __vdbe_add_op4(v, OP_String8, 0, 3, 0, err, P4_DYNAMIC);
            if( do_type_check ){
              __vdbe_goto(v, label_error);
              __vdbe_jump_here(v, jmp2);
              __vdbe_jump_here(v, jmp3);
            }else{
              /* VDBE byte code will fall thru */
            }
          }
          if( strict && do_type_check ){
            /* (2) Datatype must be exact for non-ANY columns in STRICT tables*/
            static unsigned char std_type_masks[] = {
               0x1f,    /* ANY */
               0x18,    /* BLOB */
               0x11,    /* INT */
               0x11,    /* INTEGER */
               0x13,    /* REAL */
               0x14     /* TEXT */
            };
            __vdbe_add_op4_int(v, OP_IsType, p1, label_ok, p3, p4);
            assert( col->c_type>=1 && col->c_type<=sizeof(std_type_masks) );
            __vdbe_change_p5(v, std_type_masks[col->c_type-1]);
            VDBE_COVERAGE(v);
            err = __m_printf(db, "non-%s value in %s.%s",
                                  __std_type[col->c_type-1],
                                  tab->name, tab->cols[j].cn_name);
            __vdbe_add_op4(v, OP_String8, 0, 3, 0, err, P4_DYNAMIC);
          }else if( !strict && col->affinity==DBSQL_AFF_TEXT ){
            /* (3) Datatype for TEXT columns in non-STRICT tables must be
            **     NULL, TEXT, or BLOB. */
            __vdbe_add_op4_int(v, OP_IsType, p1, label_ok, p3, p4);
            __vdbe_change_p5(v, 0x1c); /* NULL, TEXT, or BLOB */
            VDBE_COVERAGE(v);
            err = __m_printf(db, "NUMERIC value in %s.%s",
                                  tab->name, tab->cols[j].cn_name);
            __vdbe_add_op4(v, OP_String8, 0, 3, 0, err, P4_DYNAMIC);
          }else if( !strict && col->affinity>=DBSQL_AFF_NUMERIC ){
            /* (4) Datatype for numeric columns in non-STRICT tables must not
            **     be a TEXT value that can be converted to numeric. */
            __vdbe_add_op4_int(v, OP_IsType, p1, label_ok, p3, p4);
            __vdbe_change_p5(v, 0x1b); /* NULL, INT, FLOAT, or BLOB */
            VDBE_COVERAGE(v);
            if( p1>=0 ){
              __expr_code_get_column_of_table(v, tab, i_data_cur, j, 3);
            }
            __vdbe_add_op4(v, OP_Affinity, 3, 1, 0, "C", P4_STATIC);
            __vdbe_add_op4_int(v, OP_IsType, -1, label_ok, 3, p4);
            __vdbe_change_p5(v, 0x1c); /* NULL, TEXT, or BLOB */
            VDBE_COVERAGE(v);
            err = __m_printf(db, "TEXT value in %s.%s",
                                  tab->name, tab->cols[j].cn_name);
            __vdbe_add_op4(v, OP_String8, 0, 3, 0, err, P4_DYNAMIC);
          }
          __vdbe_resolve_label(v, label_error);
          __integrity_check_result_row(v);
          __vdbe_resolve_label(v, label_ok);
        }
        /* Verify CHECK constraints */
        if( tab->check && (db->flags & DBSQL_IgnoreChecks)==0 ){
          expr_list_t *check = __expr_list_dup(db, tab->check, 0);
          if( db->malloc_failed==0 ){
            int addr_ck_fault = __vdbe_make_label(parser);
            int addr_ck_ok = __vdbe_make_label(parser);
            char *err;
            int k;
            parser->i_self_tab = i_data_cur + 1;
            for(k=check->n_expr-1; k>0; k--){
              __expr_if_false(parser, check->a[k].expr, addr_ck_fault, 0);
            }
            __expr_if_true(parser, check->a[0].expr, addr_ck_ok,
                DBSQL_JUMPIFNULL);
            __vdbe_resolve_label(v, addr_ck_fault);
            parser->i_self_tab = 0;
            err = __m_printf(db, "CHECK constraint failed in %s",
                tab->name);
            __vdbe_add_op4(v, OP_String8, 0, 3, 0, err, P4_DYNAMIC);
            __integrity_check_result_row(v);
            __vdbe_resolve_label(v, addr_ck_ok);
          }
          __expr_list_delete(db, check);
        }
        if( !is_quick ){ /* Omit the remaining tests for quick_check */
          /* Validate index entries for the current row */
          for(j=0, idx=tab->p_index; idx; idx=idx->next, j++){
            int jmp2, jmp3, jmp4, jmp5, label6;
            int kk;
            int ck_uniq = __vdbe_make_label(parser);
            if( pk==idx ) continue;
            r1 = __generate_index_key(parser, idx, i_data_cur, 0, 0, &jmp3,
                                         prior, r1);
            prior = idx;
            __vdbe_add_op2(v, OP_AddImm, 8+j, 1);/* increment entry count */
            /* Verify that an index entry exists for the current table row */
            __vdbe_add_op4_int(v, OP_Found, i_idx_cur+j, ck_uniq, r1,
                                        idx->n_column); VDBE_COVERAGE(v);
            jmp2 = __vdbe_add_op3(v, OP_IFindKey, i_idx_cur+j, ck_uniq, r1); 
            VDBE_COVERAGE(v);
            __vdbe_change_p4(v, -1, (const char*)idx, P4_INDEX);
            __vdbe_add_op4(v, OP_String8, 0, 3, 0,
              __m_printf(db, "index %s stores an imprecise floating-point "
                                 "value for row ", idx->name),
              P4_DYNAMIC);
            __vdbe_add_op3(v, OP_Concat, 7, 3, 3);
            __integrity_check_result_row(v);
            __vdbe_add_op2(v, OP_Goto, 0, ck_uniq);

            __vdbe_jump_here(v, jmp2);
            __vdbe_load_string(v, 3, "row ");
            __vdbe_add_op3(v, OP_Concat, 7, 3, 3);
            __vdbe_load_string(v, 4, " missing from index ");
            __vdbe_add_op3(v, OP_Concat, 4, 3, 3);
            jmp5 = __vdbe_load_string(v, 4, idx->name);
            __vdbe_add_op3(v, OP_Concat, 4, 3, 3);
            jmp4 = __integrity_check_result_row(v);
            __vdbe_resolve_label(v, ck_uniq);

            /* The OP_IdxRowid opcode is an optimized version of OP_Column
            ** that extracts the rowid off the end of the index record.
            ** But it only works correctly if index record does not have
            ** any extra bytes at the end.  Verify that this is the case. */
            if( HAS_ROWID(tab) ){
              int jmp7;
              __vdbe_add_op2(v, OP_IdxRowid, i_idx_cur+j, 3);
              jmp7 = __vdbe_add_op3(v, OP_Eq, 3, 0, r1+idx->n_column-1);
              VDBE_COVERAGE_NEVER_NULL(v);
              __vdbe_load_string(v, 3,
                 "rowid not at end-of-record for row ");
              __vdbe_add_op3(v, OP_Concat, 7, 3, 3);
              __vdbe_load_string(v, 4, " of index ");
              __vdbe_goto(v, jmp5-1);
              __vdbe_jump_here(v, jmp7);
            }

            /* Any indexed columns with non-BINARY collations must still hold
            ** the exact same text value as the table. */
            label6 = 0;
            for(kk=0; kk<idx->n_key_col; kk++){
              if( idx->colls[kk]==__str_binary ) continue;
              if( label6==0 ) label6 = __vdbe_make_label(parser);
              __vdbe_add_op3(v, OP_Column, i_idx_cur+j, kk, 3);
              __vdbe_add_op3(v, OP_Ne, 3, label6, r1+kk); VDBE_COVERAGE(v);
            }
            if( label6 ){
              int jmp6 = __vdbe_add_op0(v, OP_Goto);
              __vdbe_resolve_label(v, label6);
              __vdbe_load_string(v, 3, "row ");
              __vdbe_add_op3(v, OP_Concat, 7, 3, 3);
              __vdbe_load_string(v, 4, " values differ from index ");
              __vdbe_goto(v, jmp5-1);
              __vdbe_jump_here(v, jmp6);
            }
             
            /* For UNIQUE indexes, verify that only one entry exists with the
            ** current key.  The entry is unique if (1) any column is NULL
            ** or (2) the next entry has a different key */
            if( IS_UNIQUE_INDEX(idx) ){
              int uniq_ok = __vdbe_make_label(parser);
              int jmp6;
              for(kk=0; kk<idx->n_key_col; kk++){
                int i_col = idx->columns[kk];
                assert( i_col!=XN_ROWID && i_col<tab->n_col );
                if( i_col>=0 && tab->cols[i_col].not_null ) continue;
                __vdbe_add_op2(v, OP_IsNull, r1+kk, uniq_ok);
                VDBE_COVERAGE(v);
              }
              jmp6 = __vdbe_add_op1(v, OP_Next, i_idx_cur+j); VDBE_COVERAGE(v);
              __vdbe_goto(v, uniq_ok);
              __vdbe_jump_here(v, jmp6);
              __vdbe_add_op4_int(v, OP_IdxGT, i_idx_cur+j, uniq_ok, r1,
                                   idx->n_key_col); VDBE_COVERAGE(v);
              __vdbe_load_string(v, 3, "non-unique entry in index ");
              __vdbe_goto(v, jmp5);
              __vdbe_resolve_label(v, uniq_ok);
            }
            __vdbe_jump_here(v, jmp4);
            __resolve_part_idx_label(parser, jmp3);
          }
        }
        __vdbe_add_op2(v, OP_Next, i_data_cur, loop_top); VDBE_COVERAGE(v);
        __vdbe_jump_here(v, loop_top-1);
        if( pk ){
          assert( !is_quick );
          __release_temp_range(parser, r2, pk->n_key_col);
        }
      }

#ifndef DBSQL_OMIT_VIRTUALTABLE
      /* Second pass to invoke the integrity method on all virtual
      ** tables.
      */
      for(x=dbsql_hash_first(tbls); x; x=dbsql_hash_next(x)){
        table_t *tab = dbsql_hash_data(x);
        dbsql_vtab_t *v_tab;
        int a1;
        if( __table_skip_integrity_check(tab,obj_tab) ) continue;
        if( IS_ORDINARY_TABLE(tab) ) continue;
        if( !IS_VIRTUAL(tab) ) continue;
        if( tab->n_col<=0 ){
          const char *z_mod = tab->u.vtab.args[0];
          if( __hash_find(&db->modules, z_mod)==0 ) continue;
        }
        __view_get_column_names(parser, tab);
        if( tab->u.vtab.p==0 ) continue;
        v_tab = tab->u.vtab.p->vtab;
        if( NEVER(v_tab==0) ) continue;
        if( NEVER(v_tab->module==0) ) continue;
        if( v_tab->module->i_version<4 ) continue;
        if( v_tab->module->integrity==0 ) continue;
        __vdbe_add_op3(v, OP_VCheck, i, 3, is_quick);
        tab->n_tab_ref++;
        __vdbe_append_p4(v, tab, P4_TABLEREF);
        a1 = __vdbe_add_op1(v, OP_IsNull, 3); VDBE_COVERAGE(v);
        __integrity_check_result_row(v);
        __vdbe_jump_here(v, a1);
        continue;
      }
#endif
    }
    {
      static const int i_ln = VDBE_OFFSET_LINENO(2);
      static const vdbe_op_list_t end_code[] = {
        { OP_AddImm,      1, 0,        0},    /* 0 */
        { OP_IfNotZero,   1, 4,        0},    /* 1 */
        { OP_String8,     0, 3,        0},    /* 2 */
        { OP_ResultRow,   3, 1,        0},    /* 3 */
        { OP_Halt,        0, 0,        0},    /* 4 */
        { OP_String8,     0, 3,        0},    /* 5 */
        { OP_Goto,        0, 3,        0},    /* 6 */
      };
      vdbe_op_t *ops;

      ops = __vdbe_add_op_list(v, ARRAY_SIZE(end_code), end_code, i_ln);
      if( ops ){
        ops[0].p2 = 1-mx_err;
        ops[2].p4type = P4_STATIC;
        ops[2].p4.z = "ok";
        ops[5].p4type = P4_STATIC;
        ops[5].p4.z = (char*)__err_str(DBSQL_CORRUPT);
      }
      __vdbe_change_p3(v, 0, __vdbe_current_addr(v)-2);
    }
  }
  break;
#endif /* DBSQL_OMIT_INTEGRITY_CHECK */

#ifndef DBSQL_OMIT_UTF16
  /*
  **   PRAGMA encoding
  **   PRAGMA encoding = "utf-8"|"utf-16"|"utf-16le"|"utf-16be"
  **
  ** In its first form, this pragma returns the encoding of the main
  ** database. If the database is not initialized, it is initialized now.
  **
  ** The second form of this pragma is a no-op if the main database file
  ** has not already been initialized. In this case it sets the default
  ** encoding that will be used for the main database file if a new file
  ** is created. If an existing main database file is opened, then the
  ** default text encoding for the existing database is used.
  **
  ** In all cases new databases created using the ATTACH command are
  ** created to use the same default text encoding as the main database. If
  ** the main database has not been initialized and/or created when ATTACH
  ** is executed, this is done before the ATTACH operation.
  **
  ** In the second form this pragma sets the text encoding to be used in
  ** new database files created using this database handle. It is only
  ** useful if invoked immediately after the main database i
  */
  case PRAG_TYP_ENCODING: {
    static const struct enc_name_t {
      char *name;
      u_int8_t enc;
    } encnames[] = {
      { "UTF8",     DBSQL_UTF8        },
      { "UTF-8",    DBSQL_UTF8        },  /* Must be element [1] */
      { "UTF-16le", DBSQL_UTF16LE     },  /* Must be element [2] */
      { "UTF-16be", DBSQL_UTF16BE     },  /* Must be element [3] */
      { "UTF16le",  DBSQL_UTF16LE     },
      { "UTF16be",  DBSQL_UTF16BE     },
      { "UTF-16",   0                  }, /* DBSQL_UTF16NATIVE */
      { "UTF16",    0                  }, /* DBSQL_UTF16NATIVE */
      { 0, 0 }
    };
    const struct enc_name_t *p_enc;
    if( !right ){    /* "PRAGMA encoding" */
      if( __read_schema(parser) ) goto pragma_out;
      assert( encnames[DBSQL_UTF8].enc==DBSQL_UTF8 );
      assert( encnames[DBSQL_UTF16LE].enc==DBSQL_UTF16LE );
      assert( encnames[DBSQL_UTF16BE].enc==DBSQL_UTF16BE );
      __return_single_text(v, encnames[ENC(parser->db)].name);
    }else{                        /* "PRAGMA encoding = XXX" */
      /* Only change the value of sqlite.enc if the database handle is not
      ** initialized. If the main database exists, the new sqlite.enc value
      ** will be overwritten when the schema is next loaded. If it does not
      ** already exists, it will be created to use the new encoding value.
      */
      if( (db->m_db_flags & DBFLAG_EncodingFixed)==0 ){
        for(p_enc=&encnames[0]; p_enc->name; p_enc++){
          if( 0==__str_icmp(right, p_enc->name) ){
            u_int8_t enc = p_enc->enc ? p_enc->enc : DBSQL_UTF16NATIVE;
            SCHEMA_ENC(db) = enc;
            __set_text_encoding(db, enc);
            break;
          }
        }
        if( !p_enc->name ){
          __error_msg(parser, "unsupported encoding: %s", right);
        }
      }
    }
  }
  break;
#endif /* DBSQL_OMIT_UTF16 */

#ifndef DBSQL_OMIT_SCHEMA_VERSION_PRAGMAS
  /*
  **   PRAGMA [schema.]schema_version
  **   PRAGMA [schema.]schema_version = <integer>
  **
  **   PRAGMA [schema.]user_version
  **   PRAGMA [schema.]user_version = <integer>
  **
  **   PRAGMA [schema.]freelist_count
  **
  **   PRAGMA [schema.]data_version
  **
  **   PRAGMA [schema.]application_id
  **   PRAGMA [schema.]application_id = <integer>
  **
  ** The pragma's schema_version and user_version are used to set or get
  ** the value of the schema-version and user-version, respectively. Both
  ** the schema-version and the user-version are 32-bit signed integers
  ** stored in the database header.
  **
  ** The schema-cookie is usually only manipulated internally by dbsql. It
  ** is incremented by dbsql whenever the database schema is modified (by
  ** creating or dropping a table or index). The schema version is used by
  ** dbsql each time a query is executed to ensure that the internal cache
  ** of the schema used when compiling the SQL query matches the schema of
  ** the database against which the compiled query is actually executed.
  ** Subverting this mechanism by using "PRAGMA schema_version" to modify
  ** the schema-version is potentially dangerous and may lead to program
  ** crashes or database corruption. Use with caution!
  **
  ** The user-version is not used internally by dbsql. It may be used by
  ** applications for any purpose.
  */
  case PRAG_TYP_HEADER_VALUE: {
    int i_cookie = pragma->i_arg;  /* Which cookie to read or write */
    __vdbe_uses_btree(v, i_db);
    if( right && (pragma->m_prag_flg & PRAG_FLG_READ_ONLY)==0 ){
      /* Write the specified cookie value */
      static const vdbe_op_list_t set_cookie[] = {
        { OP_Transaction,    0,  1,  0},    /* 0 */
        { OP_SetCookie,      0,  0,  0},    /* 1 */
      };
      vdbe_op_t *ops;
      __vdbe_verify_no_malloc_required(v, ARRAY_SIZE(set_cookie));
      ops = __vdbe_add_op_list(v, ARRAY_SIZE(set_cookie), set_cookie, 0);
      if( ONLY_IF_REALLOC_STRESS(ops==0) ) break;
      ops[0].p1 = i_db;
      ops[1].p1 = i_db;
      ops[1].p2 = i_cookie;
      ops[1].p3 = __atoi(right);
      ops[1].p5 = 1;
      if( i_cookie==BTREE_SCHEMA_VERSION && (db->flags & DBSQL_Defensive)!=0 ){
        /* Do not allow the use of PRAGMA schema_version=VALUE in defensive
        ** mode.  Change the OP_SetCookie opcode into a no-op.  */
        ops[1].opcode = OP_Noop;
      }
    }else{
      /* Read the specified cookie value */
      static const vdbe_op_list_t read_cookie[] = {
        { OP_Transaction,     0,  0,  0},    /* 0 */
        { OP_ReadCookie,      0,  1,  0},    /* 1 */
        { OP_ResultRow,       1,  1,  0}
      };
      vdbe_op_t *ops;
      __vdbe_verify_no_malloc_required(v, ARRAY_SIZE(read_cookie));
      ops = __vdbe_add_op_list(v, ARRAY_SIZE(read_cookie),read_cookie,0);
      if( ONLY_IF_REALLOC_STRESS(ops==0) ) break;
      ops[0].p1 = i_db;
      ops[1].p1 = i_db;
      ops[1].p3 = i_cookie;
      __vdbe_reusable(v);
    }
  }
  break;
#endif /* DBSQL_OMIT_SCHEMA_VERSION_PRAGMAS */

#ifndef DBSQL_OMIT_COMPILEOPTION_DIAGS
  /*
  **   PRAGMA compile_options
  **
  ** Return the names of all compile-time options used in this build,
  ** one option per row.
  */
  case PRAG_TYP_COMPILE_OPTIONS: {
    int i = 0;
    const char *opt;
    parser->n_mem = 1;
    while( (opt = dbsql_compileoption_get(i++))!=0 ){
      __vdbe_load_string(v, 1, opt);
      __vdbe_add_op2(v, OP_ResultRow, 1, 1);
    }
    __vdbe_reusable(v);
  }
  break;
#endif /* DBSQL_OMIT_COMPILEOPTION_DIAGS */

#ifndef DBSQL_OMIT_WAL
  /*
  **   PRAGMA [schema.]wal_checkpoint = passive|full|restart|truncate
  **
  ** Checkpoint the database.
  */
  case PRAG_TYP_WAL_CHECKPOINT: {
    int i_bt = (id2->z?i_db:DBSQL_MAX_DB);
    int mode = DBSQL_CHECKPOINT_PASSIVE;
    if( right ){
      if( __str_icmp(right, "full")==0 ){
        mode = DBSQL_CHECKPOINT_FULL;
      }else if( __str_icmp(right, "restart")==0 ){
        mode = DBSQL_CHECKPOINT_RESTART;
      }else if( __str_icmp(right, "truncate")==0 ){
        mode = DBSQL_CHECKPOINT_TRUNCATE;
      }else if( __str_icmp(right, "noop")==0 ){
        mode = DBSQL_CHECKPOINT_NOOP;
      }
    }
    parser->n_mem = 3;
    __vdbe_add_op3(v, OP_Checkpoint, i_bt, mode, 1);
    __vdbe_add_op2(v, OP_ResultRow, 1, 3);
  }
  break;

  /*
  **   PRAGMA wal_autocheckpoint
  **   PRAGMA wal_autocheckpoint = N
  **
  ** Configure a database connection to automatically checkpoint a database
  ** after accumulating N frames in the log. Or query for the current value
  ** of N.
  */
  case PRAG_TYP_WAL_AUTOCHECKPOINT: {
    if( right ){
      dbsql_wal_autocheckpoint(db, __atoi(right));
    }
    __return_single_int(v,
       db->wal_callback==__wal_default_hook ?
           DBSQL_PTR_TO_INT(db->wal_arg) : 0);
  }
  break;
#endif

  /*
  **  PRAGMA shrink_memory
  **
  ** IMPLEMENTATION-OF: R-23445-46109 This pragma causes the database
  ** connection on which it is invoked to free up as much memory as it
  ** can, by calling dbsql_db_release_memory().
  */
  case PRAG_TYP_SHRINK_MEMORY: {
    dbsql_db_release_memory(db);
    break;
  }

  /*
  **  PRAGMA optimize
  **  PRAGMA optimize(MASK)
  **  PRAGMA schema.optimize
  **  PRAGMA schema.optimize(MASK)
  **
  ** Attempt to optimize the database.  All schemas are optimized in the first
  ** two forms, and only the specified schema is optimized in the latter two.
  **
  ** The details of optimizations performed by this pragma are expected
  ** to change and improve over time.  Applications should anticipate that
  ** this pragma will perform new optimizations in future releases.
  **
  ** The optional argument is a bitmask of optimizations to perform:
  **
  **    0x00001    Debugging mode.  Do not actually perform any optimizations
  **               but instead return one line of text for each optimization
  **               that would have been done.  Off by default.
  **
  **    0x00002    Run ANALYZE on tables that might benefit.  On by default.
  **               See below for additional information.
  **
  **    0x00010    Run all ANALYZE operations using an analysis_limit that
  **               is the lessor of the current analysis_limit and the
  **               DBSQL_DEFAULT_OPTIMIZE_LIMIT compile-time option.
  **               The default value of DBSQL_DEFAULT_OPTIMIZE_LIMIT is
  **               currently (2024-02-19) set to 2000, which is such that
  **               the worst case run-time for PRAGMA optimize on a 100MB
  **               database will usually be less than 100 milliseconds on
  **               a __raspberry_pi-4 class machine.  On by default.
  **
  **    0x10000    Look at tables to see if they need to be reanalyzed
  **               due to growth or shrinkage even if they have not been
  **               queried during the current connection.  Off by default.
  **
  ** The default MASK is and always shall be 0x0fffe.  In the current
  ** implementation, the default mask only covers the 0x00002 optimization,
  ** though additional optimizations that are covered by 0x0fffe might be
  ** added in the future.  Optimizations that are off by default and must
  ** be explicitly requested have masks of 0x10000 or greater.
  **
  ** DETERMINATION OF WHEN TO RUN ANALYZE
  **
  ** In the current implementation, a table is analyzed if only if all of
  ** the following are true:
  **
  ** (1) MASK bit 0x00002 is set.
  **
  ** (2) The table is an ordinary table, not a virtual table or view.
  **
  ** (3) The table name does not begin with "dbsql_".
  **
  ** (4) One or more of the following is true:
  **      (4a) The 0x10000 MASK bit is set.
  **      (4b) One or more indexes on the table lacks an entry
  **           in the dbsql_stat1 table.
  **      (4c) The query planner used dbsql_stat1-style statistics for one
  **           or more indexes of the table at some point during the lifetime
  **           of the current connection.
  **
  ** (5) One or more of the following is true:
  **      (5a) One or more indexes on the table lacks an entry
  **           in the dbsql_stat1 table.  (Same as 4a)
  **      (5b) The number of rows in the table has increased or decreased by
  **           10-fold.  In other words, the current size of the table is
  **           10 times larger than the size in dbsql_stat1 or else the
  **           current size is less than 1/10th the size in dbsql_stat1.
  **
  ** The rules for when tables are analyzed are likely to change in
  ** future releases.  Future versions of dbsql might accept a string
  ** literal argument to this pragma that contains a mnemonic description
  ** of the options rather than a bitmap.
  */
  case PRAG_TYP_OPTIMIZE: {
    int i_db_last;           /* Loop termination point for the schema loop */
    int i_tab_cur;           /* Cursor for a table whose size needs checking */
    hash_elem_t *k;           /* Loop over tables of a schema */
    schema_t *schema;       /* The current schema */
    table_t *tab;           /* A table in the schema */
    index_t *idx;           /* An index of the table */
    log_est_t sz_threshold;    /* Size threshold above which reanalysis needed */
    char *sub_sql;         /* SQL statement for the OP_SqlExec opcode */
    u_int32_t op_mask;            /* Mask of operations to perform */
    int n_limit;            /* Analysis limit to use */
    int n_check = 0;        /* Number of tables to be optimized */
    int n_btree = 0;        /* Number of btrees to scan */
    int n_index;            /* Number of indexes on the current table */

    if( right ){
      op_mask = (u_int32_t)__atoi(right);
      if( (op_mask & 0x02)==0 ) break;
    }else{
      op_mask = 0xfffe;
    }
    if( (op_mask & 0x10)==0 ){
      n_limit = 0;
    }else if( db->n_analysis_limit>0
           && db->n_analysis_limit<DBSQL_DEFAULT_OPTIMIZE_LIMIT ){
      n_limit = 0;
    }else{
      n_limit = DBSQL_DEFAULT_OPTIMIZE_LIMIT;
    }
    i_tab_cur = parser->n_tab++;
    for(i_db_last = z_db?i_db:db->n_db-1; i_db<=i_db_last; i_db++){
      if( i_db==1 ) continue;
      __code_verify_schema(parser, i_db);
      schema = db->dbs[i_db].schema;
      for(k=dbsql_hash_first(&schema->tbl_hash); k; k=dbsql_hash_next(k)){
        tab = (table_t*)dbsql_hash_data(k);

        /* This only works for ordinary tables */
        if( !IS_ORDINARY_TABLE(tab) ) continue;

        /* Do not scan system tables */
        if( 0==__str_nicmp(tab->name, "dbsql_", 6) ) continue;

        /* Find the size of the table as last recorded in dbsql_stat1.
        ** If any index is unanalyzed, then the threshold is -1 to
        ** indicate a new, unanalyzed index
        */
        sz_threshold = tab->n_row_log_est;
        n_index = 0;
        for(idx=tab->p_index; idx; idx=idx->next){
          n_index++;
          if( !idx->has_stat1 ){
            sz_threshold = -1; /* Always analyze if any index lacks statistics */
          }
        }

        /* If table tab has not been used in a way that would benefit from
        ** having analysis statistics during the current session, then skip it,
        ** unless the 0x10000 MASK bit is set. */
        if( (tab->tab_flags & TF_MaybeReanalyze)!=0 ){
          /* Check for size change if stat1 has been used for a query */
        }else if( op_mask & 0x10000 ){
          /* Check for size change if 0x10000 is set */
        }else if( tab->p_index!=0 && sz_threshold<0 ){
          /* Do analysis if unanalyzed indexes exists */
        }else{
          /* Otherwise, we can skip this table */
          continue;
        }

        n_check++;
        if( n_check==2 ){
          /* If ANALYZE might be invoked two or more times, hold a write
          ** transaction for efficiency */
          __begin_write_operation(parser, 0, i_db);
        }
        n_btree += n_index+1;

        /* Reanalyze if the table is 10 times larger or smaller than
        ** the last analysis.  Unconditional reanalysis if there are
        ** unanalyzed indexes. */
        __open_table(parser, i_tab_cur, i_db, tab, OP_OpenRead);
        if( sz_threshold>=0 ){
          const log_est_t i_range = 33;   /* 10x size change */
          __vdbe_add_op4_int(v, OP_IfSizeBetween, i_tab_cur,
                         __vdbe_current_addr(v)+2+(op_mask&1),
                         sz_threshold>=i_range ? sz_threshold-i_range : -1,
                         sz_threshold+i_range);
          VDBE_COVERAGE(v);
        }else{
          __vdbe_add_op2(v, OP_Rewind, i_tab_cur,
                         __vdbe_current_addr(v)+2+(op_mask&1));
          VDBE_COVERAGE(v);
        }
        sub_sql = __m_printf(db, "ANALYZE \"%w\".\"%w\"",
                                 db->dbs[i_db].db_s_name, tab->name);
        if( op_mask & 0x01 ){
          int r1 = __get_temp_reg(parser);
          __vdbe_add_op4(v, OP_String8, 0, r1, 0, sub_sql, P4_DYNAMIC);
          __vdbe_add_op2(v, OP_ResultRow, r1, 1);
        }else{
          __vdbe_add_op4(v, OP_SqlExec, n_limit ? 0x02 : 00, n_limit, 0,
                            sub_sql, P4_DYNAMIC);
        }
      }
    }
    __vdbe_add_op0(v, OP_Expire);

    /* In a schema with a large number of tables and indexes, scale back
    ** the analysis_limit to avoid excess run-time in the worst case.
    */
    if( !db->malloc_failed && n_limit>0 && n_btree>100 ){
      int i_addr, i_end;
      vdbe_op_t *ops;
      n_limit = 100*n_limit/n_btree;
      if( n_limit<100 ) n_limit = 100;
      ops = __vdbe_get_op(v, 0);
      i_end = __vdbe_current_addr(v);
      for(i_addr=0; i_addr<i_end; i_addr++){
        if( ops[i_addr].opcode==OP_SqlExec ) ops[i_addr].p2 = n_limit;
      }
    }
    break;
  }

  /*
  **   PRAGMA busy_timeout
  **   PRAGMA busy_timeout = N
  **
  ** Call dbsql_busy_timeout(db, N).  Return the current timeout value
  ** if one is set.  If no busy handler or a different busy handler is set
  ** then 0 is returned.  Setting the busy_timeout to 0 or negative
  ** disables the timeout.
  */
  /*case PRAG_TYP_BUSY_TIMEOUT*/ default: {
    assert( pragma->prag_typ==PRAG_TYP_BUSY_TIMEOUT );
    if( right ){
      dbsql_busy_timeout(db, __atoi(right));
    }
    __return_single_int(v, db->busy_timeout);
    break;
  }

  /*
  **   PRAGMA soft_heap_limit
  **   PRAGMA soft_heap_limit = N
  **
  ** IMPLEMENTATION-OF: R-26343-45930 This pragma invokes the
  ** dbsql_soft_heap_limit64() interface with the argument N, if N is
  ** specified and is a non-negative integer.
  ** IMPLEMENTATION-OF: R-64451-07163 The soft_heap_limit pragma always
  ** returns the same integer that would be returned by the
  ** dbsql_soft_heap_limit64(-1) C-language function.
  */
  case PRAG_TYP_SOFT_HEAP_LIMIT: {
    int64_t N;
    if( right && __dec_or_hex_to_i64(right, &N)==DBSQL_OK ){
      dbsql_soft_heap_limit64(N);
    }
    __return_single_int(v, dbsql_soft_heap_limit64(-1));
    break;
  }

  /*
  **   PRAGMA hard_heap_limit
  **   PRAGMA hard_heap_limit = N
  **
  ** Invoke dbsql_hard_heap_limit64() to query or set the hard heap
  ** limit.  The hard heap limit can be activated or lowered by this
  ** pragma, but not raised or deactivated.  Only the
  ** dbsql_hard_heap_limit64() C-language API can raise or deactivate
  ** the hard heap limit.  This allows an application to set a heap limit
  ** constraint that cannot be relaxed by an untrusted SQL script.
  */
  case PRAG_TYP_HARD_HEAP_LIMIT: {
    int64_t N;
    if( right && __dec_or_hex_to_i64(right, &N)==DBSQL_OK ){
      int64_t i_prior = dbsql_hard_heap_limit64(-1);
      if( N>0 && (i_prior==0 || i_prior>N) ) dbsql_hard_heap_limit64(N);
    }
    __return_single_int(v, dbsql_hard_heap_limit64(-1));
    break;
  }

  /*
  **   PRAGMA threads
  **   PRAGMA threads = N
  **
  ** Configure the maximum number of worker threads.  Return the new
  ** maximum, which might be less than requested.
  */
  case PRAG_TYP_THREADS: {
    int64_t N;
    if( right
     && __dec_or_hex_to_i64(right, &N)==DBSQL_OK
     && N>=0
    ){
      dbsql_limit(db, DBSQL_LIMIT_WORKER_THREADS, (int)(N&0x7fffffff));
    }
    __return_single_int(v, dbsql_limit(db, DBSQL_LIMIT_WORKER_THREADS, -1));
    break;
  }

  /*
  **   PRAGMA analysis_limit
  **   PRAGMA analysis_limit = N
  **
  ** Configure the maximum number of rows that ANALYZE will examine
  ** in each index that it looks at.  Return the new limit.
  */
  case PRAG_TYP_ANALYSIS_LIMIT: {
    int64_t N;
    if( right
     && __dec_or_hex_to_i64(right, &N)==DBSQL_OK /* IMP: R-40975-20399 */
     && N>=0
    ){
      db->n_analysis_limit = (int)(N&0x7fffffff);
    }
    __return_single_int(v, db->n_analysis_limit); /* IMP: R-57594-65522 */
    break;
  }

#if defined(DBSQL_DEBUG) || defined(DBSQL_TEST)
  /*
  ** Report the current state of file logs for all databases
  */
  case PRAG_TYP_LOCK_STATUS: {
    static const char *const lock_names[] = {
      "unlocked", "shared", "reserved", "pending", "exclusive"
    };
    int i;
    parser->n_mem = 2;
    for(i=0; i<db->n_db; i++){
      sm_t *bt;
      const char *state = "unknown";
      int j;
      if( db->dbs[i].db_s_name==0 ) continue;
      bt = db->dbs[i].bt;
      if( bt==0 || __sm_pager(bt)==0 ){
        state = "closed";
      }else if( dbsql_file_control(db, i ? db->dbs[i].db_s_name : 0,
                                     DBSQL_FCNTL_LOCKSTATE, &j)==DBSQL_OK ){
         state = lock_names[j];
      }
      __vdbe_multi_load(v, 1, "ss", db->dbs[i].db_s_name, state);
    }
    break;
  }
#endif

#if defined(DBSQL_ENABLE_CEROD)
  case PRAG_TYP_ACTIVATE_EXTENSIONS: if( right ){
    if( __str_nicmp(right, "cerod-", 6)==0 ){
      dbsql_activate_cerod(&right[6]);
    }
  }
  break;
#endif

  } /* End of the PRAGMA switch */

  /* The following block is a no-op unless DBSQL_DEBUG is defined. Its only
  ** purpose is to execute assert() statements to verify that if the
  ** PRAG_FLG_NO_COLUMNS1 flag is set and the caller specified an argument
  ** to the PRAGMA, the implementation has not added any OP_ResultRow
  ** instructions to the VM.  */
  if( (pragma->m_prag_flg & PRAG_FLG_NO_COLUMNS1) && right ){
    __vdbe_verify_no_result_row(v);
  }

pragma_out:
  __dbsql_db_free(db, left);
  __dbsql_db_free(db, right);
}
#ifndef DBSQL_OMIT_VIRTUALTABLE
/*****************************************************************************
** Implementation of an eponymous virtual table that runs a pragma.
**
*/
typedef struct pragma_vtab_t pragma_vtab_t;
typedef struct pragma_vtab_cursor_t pragma_vtab_cursor_t;
struct pragma_vtab_t {
  dbsql_vtab_t base;        /* Base class.  Must be first */
  DBSQL *db;              /* The database connection to which it belongs */
  const pragma_name_t *name;  /* Name of the pragma */
  u_int8_t n_hidden;               /* Number of hidden columns */
  u_int8_t i_hidden;               /* index_t of the first hidden column */
};
struct pragma_vtab_cursor_t {
  dbsql_vtab_cursor_t base; /* Base class.  Must be first */
  dbsql_stmt_t *pragma;    /* The pragma statement to run */
  int64_t i_rowid;      /* Current rowid */
  char *args[2];           /* Value of the argument and schema */
};

/*
** Pragma virtual table module connect method.
*/
static int __pragma_vtab_connect(
  DBSQL *db,
  void *aux,
  int argc, const char *const*argv,
  dbsql_vtab_t **vtab,
  char **err_p
){
  const pragma_name_t *pragma = (const pragma_name_t*)aux;
  pragma_vtab_t *tab = 0;
  int rc;
  int i, j;
  char c_sep = '(';
  str_accum_t acc;
  char buf[200];

  UNUSED_PARAMETER(argc);
  UNUSED_PARAMETER(argv);
  __str_accum_init(&acc, 0, buf, sizeof(buf), 0);
  dbsql_str_appendall(&acc, "CREATE TABLE x");
  for(i=0, j=pragma->i_prag_c_name; i<pragma->n_prag_c_name; i++, j++){
    dbsql_str_appendf(&acc, "%c\"%s\"", c_sep, __prag_c_name[j]);
    c_sep = ',';
  }
  if( i==0 ){
    dbsql_str_appendf(&acc, "(\"%s\"", pragma->name);
    i++;
  }
  j = 0;
  if( pragma->m_prag_flg & PRAG_FLG_RESULT1 ){
    dbsql_str_appendall(&acc, ",arg HIDDEN");
    j++;
  }
  if( pragma->m_prag_flg & (PRAG_FLG_SCHEMA_OPT|PRAG_FLG_SCHEMA_REQ) ){
    dbsql_str_appendall(&acc, ",schema HIDDEN");
    j++;
  }
  dbsql_str_append(&acc, ")", 1);
  __str_accum_finish(&acc);
  assert( strlen(buf) < sizeof(buf)-1 );
  rc = dbsql_declare_vtab(db, buf);
  if( rc==DBSQL_OK ){
    tab = (pragma_vtab_t*)dbsql_malloc(sizeof(pragma_vtab_t));
    if( tab==0 ){
      rc = DBSQL_NOMEM;
    }else{
      memset(tab, 0, sizeof(pragma_vtab_t));
      tab->name = pragma;
      tab->db = db;
      tab->i_hidden = i;
      tab->n_hidden = j;
    }
  }else{
    *err_p = dbsql_mprintf("%s", dbsql_errmsg(db));
  }

  *vtab = (dbsql_vtab_t*)tab;
  return rc;
}

/*
** Pragma virtual table module disconnect method.
*/
static int __pragma_vtab_disconnect(dbsql_vtab_t *vtab){
  pragma_vtab_t *tab = (pragma_vtab_t*)vtab;
  dbsql_free(tab);
  return DBSQL_OK;
}

/* Figure out the best index to use to search a pragma virtual table.
**
** There are not really any index choices.  But we want to encourage the
** query planner to give == constraints on as many hidden parameters as
** possible, and especially on the first hidden parameter.  So return a
** high cost if hidden parameters are unconstrained.
*/
static int __pragma_vtab_best_index(dbsql_vtab_t *tab, dbsql_index_info_t *idx_info){
  pragma_vtab_t *p_tab = (pragma_vtab_t*)tab;
  const struct dbsql_index_constraint_t *constraint;
  int i, j;
  int seen[2];

  idx_info->estimated_cost = (double)1;
  if( p_tab->n_hidden==0 ){ return DBSQL_OK; }
  constraint = idx_info->constraints;
  seen[0] = 0;
  seen[1] = 0;
  for(i=0; i<idx_info->n_constraint; i++, constraint++){
    if( constraint->i_column < p_tab->i_hidden ) continue;
    if( constraint->op!=DBSQL_INDEX_CONSTRAINT_EQ ) continue;
    if( constraint->usable==0 ) return DBSQL_CONSTRAINT;
    j = constraint->i_column - p_tab->i_hidden;
    assert( j < 2 );
    seen[j] = i+1;
  }
  if( seen[0]==0 ){
    idx_info->estimated_cost = (double)2147483647;
    idx_info->estimated_rows = 2147483647;
    return DBSQL_OK;
  }
  j = seen[0]-1;
  idx_info->constraint_usages[j].argv_index = 1;
  idx_info->constraint_usages[j].omit = 1;
  idx_info->estimated_cost = (double)20;
  idx_info->estimated_rows = 20;
  if( seen[1] ){
    j = seen[1]-1;
    idx_info->constraint_usages[j].argv_index = 2;
    idx_info->constraint_usages[j].omit = 1;
  }
  return DBSQL_OK;
}

/* Create a new cursor for the pragma virtual table */
static int __pragma_vtab_open(dbsql_vtab_t *vtab, dbsql_vtab_cursor_t **cursor){
  pragma_vtab_cursor_t *csr;
  csr = (pragma_vtab_cursor_t*)dbsql_malloc(sizeof(*csr));
  if( csr==0 ) return DBSQL_NOMEM;
  memset(csr, 0, sizeof(pragma_vtab_cursor_t));
  csr->base.vtab = vtab;
  *cursor = &csr->base;
  return DBSQL_OK;
}

/* Clear all content from pragma virtual table cursor. */
static void __pragma_vtab_cursor_clear(pragma_vtab_cursor_t *csr){
  int i;
  dbsql_finalize(csr->pragma);
  csr->pragma = 0;
  csr->i_rowid = 0;
  for(i=0; i<ARRAY_SIZE(csr->args); i++){
    dbsql_free(csr->args[i]);
    csr->args[i] = 0;
  }
}

/* Close a pragma virtual table cursor */
static int __pragma_vtab_close(dbsql_vtab_cursor_t *cur){
  pragma_vtab_cursor_t *csr = (pragma_vtab_cursor_t*)cur;
  __pragma_vtab_cursor_clear(csr);
  dbsql_free(csr);
  return DBSQL_OK;
}

/* Advance the pragma virtual table cursor to the next row */
static int __pragma_vtab_next(dbsql_vtab_cursor_t *vtab_cursor){
  pragma_vtab_cursor_t *csr = (pragma_vtab_cursor_t*)vtab_cursor;
  int rc = DBSQL_OK;

  /* Increment the rowid value */
  csr->i_rowid++;
  assert( csr->pragma );
  if( DBSQL_ROW!=dbsql_step(csr->pragma) ){
    rc = dbsql_finalize(csr->pragma);
    csr->pragma = 0;
    __pragma_vtab_cursor_clear(csr);
  }
  return rc;
}

/*
** Pragma virtual table module filter method.
*/
static int __pragma_vtab_filter(
  dbsql_vtab_cursor_t *vtab_cursor,
  int idx_num, const char *idx_str,
  int argc, dbsql_value_t **argv
){
  pragma_vtab_cursor_t *csr = (pragma_vtab_cursor_t*)vtab_cursor;
  pragma_vtab_t *tab = (pragma_vtab_t*)(vtab_cursor->vtab);
  int rc;
  int i, j;
  str_accum_t acc;
  char *sql;

  UNUSED_PARAMETER(idx_num);
  UNUSED_PARAMETER(idx_str);
  __pragma_vtab_cursor_clear(csr);
  j = (tab->name->m_prag_flg & PRAG_FLG_RESULT1)!=0 ? 0 : 1;
  for(i=0; i<argc; i++, j++){
    const char *text = (const char*)dbsql_value_text(argv[i]);
    assert( j<ARRAY_SIZE(csr->args) );
    assert( csr->args[j]==0 );
    if( text ){
      csr->args[j] = dbsql_mprintf("%s", text);
      if( csr->args[j]==0 ){
        return DBSQL_NOMEM;
      }
    }
  }
  __str_accum_init(&acc, 0, 0, 0, tab->db->limits[DBSQL_LIMIT_SQL_LENGTH]);
  dbsql_str_appendall(&acc, "PRAGMA ");
  if( csr->args[1] ){
    dbsql_str_appendf(&acc, "%Q.", csr->args[1]);
  }
  dbsql_str_appendall(&acc, tab->name->name);
  if( csr->args[0] ){
    dbsql_str_appendf(&acc, "=%Q", csr->args[0]);
  }
  sql = __str_accum_finish(&acc);
  if( sql==0 ) return DBSQL_NOMEM;
  rc = dbsql_prepare_v2(tab->db, sql, -1, &csr->pragma, 0);
  dbsql_free(sql);
  if( rc!=DBSQL_OK ){
    tab->base.errmsg = dbsql_mprintf("%s", dbsql_errmsg(tab->db));
    return rc;
  }
  return __pragma_vtab_next(vtab_cursor);
}

/*
** Pragma virtual table module eof method.
*/
static int __pragma_vtab_eof(dbsql_vtab_cursor_t *vtab_cursor){
  pragma_vtab_cursor_t *csr = (pragma_vtab_cursor_t*)vtab_cursor;
  return (csr->pragma==0);
}

/* The column method simply returns the corresponding column from
** the PRAGMA. 
*/
static int __pragma_vtab_column(
  dbsql_vtab_cursor_t *vtab_cursor,
  dbsql_func_t *ctx,
  int i
){
  pragma_vtab_cursor_t *csr = (pragma_vtab_cursor_t*)vtab_cursor;
  pragma_vtab_t *tab = (pragma_vtab_t*)(vtab_cursor->vtab);
  if( i<tab->i_hidden ){
    dbsql_result_value(ctx, dbsql_column_value(csr->pragma, i));
  }else{
    dbsql_result_text(ctx, csr->args[i-tab->i_hidden],-1,DBSQL_TRANSIENT);
  }
  return DBSQL_OK;
}

/*
** Pragma virtual table module rowid method.
*/
static int __pragma_vtab_rowid(dbsql_vtab_cursor_t *vtab_cursor, int64_t *p){
  pragma_vtab_cursor_t *csr = (pragma_vtab_cursor_t*)vtab_cursor;
  *p = csr->i_rowid;
  return DBSQL_OK;
}

/* The pragma virtual table object */
static const dbsql_module_t __pragma_vtab_module = {
  0,                           /* i_version */
  0,                           /* create - create a table */
  __pragma_vtab_connect,           /* connect - connect to an existing table */
  __pragma_vtab_best_index,         /* best_index - Determine search strategy */
  __pragma_vtab_disconnect,        /* disconnect - Disconnect from a table */
  0,                           /* destroy - Drop a table */
  __pragma_vtab_open,              /* x_open - open a cursor */
  __pragma_vtab_close,             /* x_close - close a cursor */
  __pragma_vtab_filter,            /* filter - configure scan constraints */
  __pragma_vtab_next,              /* next - advance a cursor */
  __pragma_vtab_eof,               /* eof */
  __pragma_vtab_column,            /* column - read data */
  __pragma_vtab_rowid,             /* rowid - read data */
  0,                           /* update - write data */
  0,                           /* begin - begin transaction */
  0,                           /* x_sync - sync transaction */
  0,                           /* commit - commit transaction */
  0,                           /* rollback - rollback transaction */
  0,                           /* find_function - function overloading */
  0,                           /* rename - rename the table */
  0,                           /* savepoint */
  0,                           /* release */
  0,                           /* rollback_to */
  0,                           /* shadow_name */
  0                            /* integrity */
};

/*
** Check to see if tab_name is really the name of a pragma.  If it is,
** then register an eponymous virtual table for that pragma and return
** a pointer to the module_def_t object for the new virtual table.
*/
module_def_t *__pragma_vtab_register(DBSQL *db, const char *z_name){
  const pragma_name_t *name;
  assert( dbsql_strnicmp(z_name, "pragma_", 7)==0 );
  name = __pragma_locate(z_name+7);
  if( name==0 ) return 0;
  if( (name->m_prag_flg & (PRAG_FLG_RESULT0|PRAG_FLG_RESULT1))==0 ) return 0;
  assert( __hash_find(&db->modules, z_name)==0 );
  return __vtab_create_module(db, z_name, &__pragma_vtab_module, (void*)name, 0);
}

#endif /* DBSQL_OMIT_VIRTUALTABLE */

#endif /* DBSQL_OMIT_PRAGMA */
