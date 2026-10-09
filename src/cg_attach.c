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
** This file contains code used to implement the ATTACH and DETACH commands.
*/
#include "dbsql_int.h"

#ifndef DBSQL_OMIT_ATTACH
/*
** Resolve an expression that was part of an ATTACH or DETACH statement. This
** is slightly different from resolving a normal SQL expression, because simple
** identifiers are treated as strings, not possible column names or aliases.
**
** i.e. if the parser sees:
**
**     ATTACH DATABASE abc AS def
**
** it treats the two expressions as literal strings 'abc' and 'def' instead of
** looking for columns of the same name.
**
** This only applies to the root node of expr, so the statement:
**
**     ATTACH DATABASE abc||def AS 'db2'
**
** will fail because neither abc or def can be resolved.
*/
static int __resolve_attach_expr(name_context_t *name, expr_t *expr)
{
  int rc = DBSQL_OK;
  if( expr ){
    if( expr->op!=TK_ID ){
      rc = __resolve_expr_names(name, expr);
    }else{
      expr->op = TK_STRING;
    }
  }
  return rc;
}

/*
** Return true if name points to a name that may be used to refer to
** database i_db attached to handle db.
*/
int __db_is_named(DBSQL *db, int i_db, const char *name){
  return (
      __str_icmp(db->dbs[i_db].db_s_name, name)==0
   || (i_db==0 && __str_icmp("main", name)==0)
  );
}

/*
** An SQL user-function registered to do the work of an ATTACH statement. The
** three arguments to the function come directly from an attach statement:
**
**     ATTACH DATABASE x AS y KEY z
**
**     SELECT dbsql_attach(x, y, z)
**
** If the optional "KEY z" syntax is omitted, an SQL NULL is passed as the
** third argument.
**
** If the db->init.reopen_memdb flags is set, then instead of attaching a
** new database, close the database on db->init.i_db and reopen it as an
** empty __mem_db.
*/
static void __attach_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **argv
){
  int i;
  int rc = 0;
  DBSQL *db = dbsql_context_db_handle(context);
  const char *name;
  const char *file;
  char *path = 0;
  char *err = 0;
  unsigned int flags;
  db_t *news;                 /* New array of db_t pointers */
  db_t *p_new = 0;             /* db_t object for the newly attached database */
  char *err_dyn = 0;
  dbsql_vfs_t *vfs;

  UNUSED_PARAMETER(__not_used);
  file = (const char *)dbsql_value_text(argv[0]);
  name = (const char *)dbsql_value_text(argv[1]);
  if( file==0 ) file = "";
  if( name==0 ) name = "";

#ifndef DBSQL_OMIT_DESERIALIZE
# define REOPEN_AS_MEMDB(db)  (db->init.reopen_memdb)
#else
# define REOPEN_AS_MEMDB(db)  (0)
#endif

  if( REOPEN_AS_MEMDB(db) ){
    /* This is not a real ATTACH.  Instead, this routine is being called
    ** from dbsql_deserialize() to close database db->init.i_db and
    ** reopen it as a __mem_db */
    sm_t *new_bt = 0;

    p_new = &db->dbs[db->init.i_db];
    assert( p_new->bt!=0 );
    if( __sm_txn_state(p_new->bt)!=DBSQL_TXN_NONE
     || __sm_is_in_backup(p_new->bt)
    ){
      rc = DBSQL_BUSY;
      goto attach_error;
    }

    vfs = dbsql_vfs_find("memdb");
    if( vfs==0 ) return;
    rc = __sm_open(vfs, "x\0", db, &new_bt, 0, DBSQL_OPEN_MAIN_DB);
    if( rc==DBSQL_OK ){
      schema_t *new_schema = __schema_get(db, new_bt);
      if( new_schema ){
        /* Both the sm_t and the new schema_t were allocated successfully.
        ** Close the old db and update the dbs[] slot with the new memdb
        ** values.  */
        __sm_close(p_new->bt);
        p_new->bt = new_bt;
        p_new->schema = new_schema;
      }else{
        __sm_close(new_bt);
        rc = DBSQL_NOMEM;
      }
    }
    if( rc ) goto attach_error;
  }else{
    /* This is a real ATTACH
    **
    ** Check for the following errors:
    **
    **     * Too many attached databases,
    **     * Transaction currently open
    **     * Specified database name already being used.
    */
    if( db->n_db>=db->limits[DBSQL_LIMIT_ATTACHED]+2 ){
      err_dyn = __m_printf(db, "too many attached databases - max %d", 
        db->limits[DBSQL_LIMIT_ATTACHED]
      );
      goto attach_error;
    }
    for(i=0; i<db->n_db; i++){
      assert( name );
      if( __db_is_named(db, i, name) ){
        err_dyn = __m_printf(db, "database %s is already in use", name);
        goto attach_error;
      }
    }
  
    /* Allocate the new entry in the db->dbs[] array and initialize the schema
    ** hash tables.
    */
    if( db->dbs==db->db_statics ){
      news = __db_malloc_raw_nn(db, sizeof(db->dbs[0])*3 );
      if( news==0 ) return;
      memcpy(news, db->dbs, sizeof(db->dbs[0])*2);
    }else{
      news = __db_realloc(db, db->dbs, sizeof(db->dbs[0])*(1+(int64_t)db->n_db));
      if( news==0 ) return;
    }
    db->dbs = news;
    p_new = &db->dbs[db->n_db];
    memset(p_new, 0, sizeof(*p_new));
  
    /* Open the database file. If the btree is successfully opened, use
    ** it to obtain the database schema. At this point the schema may
    ** or may not be initialized.
    */
    flags = db->open_flags;
    rc = __parse_uri(db->vfs->name, file, &flags, &vfs, &path, &err);
    if( rc!=DBSQL_OK ){
      if( rc==DBSQL_NOMEM ) __oom_fault(db);
      dbsql_result_error(context, err, -1);
      dbsql_free(err);
      return;
    }
    if( (db->flags & DBSQL_AttachWrite)==0 ){
      flags &= ~(DBSQL_OPEN_CREATE|DBSQL_OPEN_READWRITE);
      flags |= DBSQL_OPEN_READONLY;
    }else if( (db->flags & DBSQL_AttachCreate)==0 ){
      flags &= ~DBSQL_OPEN_CREATE;
    }
    assert( vfs );
    flags |= DBSQL_OPEN_MAIN_DB;
    rc = __sm_open(vfs, path, db, &p_new->bt, 0, flags);
    db->n_db++;
    p_new->db_s_name = __db_str_dup(db, name);
  }
  db->no_shared_cache = 0;
  if( rc==DBSQL_CONSTRAINT ){
    rc = DBSQL_ERROR;
    err_dyn = __m_printf(db, "database is already attached");
  }else if( rc==DBSQL_OK ){
    pager_t *pager;
    p_new->schema = __schema_get(db, p_new->bt);
    if( !p_new->schema ){
      rc = DBSQL_NOMEM_BKPT;
    }else if( p_new->schema->file_format && p_new->schema->enc!=ENC(db) ){
      err_dyn = __m_printf(db, 
        "attached databases must use the same text encoding as main database");
      rc = DBSQL_ERROR;
    }
    __sm_enter(p_new->bt);
    pager = __sm_pager(p_new->bt);
    __sm_pager_locking_mode(pager, db->dflt_lock_mode);
    __sm_secure_delete(p_new->bt,
                             __sm_secure_delete(db->dbs[0].bt,-1) );
#ifndef DBSQL_OMIT_PAGER_PRAGMAS
    __sm_set_pager_flags(p_new->bt,
                      PAGER_SYNCHRONOUS_FULL | (db->flags & PAGER_FLAGS_MASK));
#endif
    __sm_leave(p_new->bt);
  }
  p_new->safety_level = DBSQL_DEFAULT_SYNCHRONOUS+1;
  if( rc==DBSQL_OK && p_new->db_s_name==0 ){
    rc = DBSQL_NOMEM_BKPT;
  }
  dbsql_free_filename( path );

  /* If the file was opened successfully, read the schema for the new database.
  ** If this fails, or if opening the file failed, then close the file and 
  ** remove the entry from the db->dbs[] array. i.e. put everything back the
  ** way we found it.
  */
  if( rc==DBSQL_OK ){
    __sm_enter_all(db);
    db->init.i_db = 0;
    db->m_db_flags &= ~(DBFLAG_SchemaKnownOk);
#ifdef DBSQL_ENABLE_SETLK_TIMEOUT
    if( db->setlk_flags & DBSQL_SETLK_BLOCK_ON_CONNECT ){
      int val = 1;
      dbsql_file_t *fd = __sm_pager_file(__sm_pager(p_new->bt));
      __dbsql_os_file_control_hint(fd, DBSQL_FCNTL_BLOCK_ON_CONNECT, &val);
    }
#endif
    if( !REOPEN_AS_MEMDB(db) ){
      rc = __init(db, &err_dyn);
    }
    __sm_leave_all(db);
    assert( err_dyn==0 || rc!=DBSQL_OK );
  }
  if( rc ){
    if( ALWAYS(!REOPEN_AS_MEMDB(db)) ){
      int i_db = db->n_db - 1;
      assert( i_db>=2 );
      if( db->dbs[i_db].bt ){
        __sm_close(db->dbs[i_db].bt);
        db->dbs[i_db].bt = 0;
        db->dbs[i_db].schema = 0;
      }
      __reset_all_schemas_of_connection(db);
      db->n_db = i_db;
      if( rc==DBSQL_NOMEM || rc==DBSQL_IOERR_NOMEM ){
        __oom_fault(db);
        __dbsql_db_free(db, err_dyn);
        err_dyn = __m_printf(db, "out of memory");
      }else if( err_dyn==0 ){
        err_dyn = __m_printf(db, "unable to open database: %s", file);
      }
    }
    goto attach_error;
  }
  
  return;

attach_error:
  /* Return an error if we get here */
  if( err_dyn ){
    dbsql_result_error(context, err_dyn, -1);
    __dbsql_db_free(db, err_dyn);
  }
  if( rc ) dbsql_result_error_code(context, rc);
}

/*
** An SQL user-function registered to do the work of an DETACH statement. The
** three arguments to the function come directly from a detach statement:
**
**     DETACH DATABASE x
**
**     SELECT dbsql_detach(x)
*/
static void __detach_func(
  dbsql_func_t *context,
  int __not_used,
  dbsql_value_t **argv
){
  const char *name = (const char *)dbsql_value_text(argv[0]);
  DBSQL *db = dbsql_context_db_handle(context);
  int i;
  db_t *p_db = 0;
  hash_elem_t *entry;
  char err[128];

  UNUSED_PARAMETER(__not_used);

  if( name==0 ) name = "";
  for(i=0; i<db->n_db; i++){
    p_db = &db->dbs[i];
    if( p_db->bt==0 ) continue;
    if( __db_is_named(db, i, name) ) break;
  }

  if( i>=db->n_db ){
    dbsql_snprintf(sizeof(err),err, "no such database: %s", name);
    goto detach_error;
  }
  if( i<2 ){
    dbsql_snprintf(sizeof(err),err, "cannot detach database %s", name);
    goto detach_error;
  }
  if( __sm_txn_state(p_db->bt)!=DBSQL_TXN_NONE
   || __sm_is_in_backup(p_db->bt)
  ){
    dbsql_snprintf(sizeof(err),err, "database %s is locked", name);
    goto detach_error;
  }

  /* If any TEMP triggers reference the schema being detached, move those
  ** triggers to reference the TEMP schema itself. */
  assert( db->dbs[1].schema );
  entry = dbsql_hash_first(&db->dbs[1].schema->trig_hash);
  while( entry ){
    trigger_t *trig = (trigger_t*)dbsql_hash_data(entry);
    if( trig->tab_schema==p_db->schema ){
      trig->tab_schema = trig->schema;
    }
    entry = dbsql_hash_next(entry);
  }

  __sm_close(p_db->bt);
  p_db->bt = 0;
  p_db->schema = 0;
  __collapse_database_array(db);
  return;

detach_error:
  dbsql_result_error(context, err, -1);
}

/*
** This procedure generates VDBE code for a single invocation of either the
** dbsql_detach() or dbsql_attach() SQL user functions.
*/
static void __code_attach(
  parser_t *parser,       /* The parser context */
  int type,            /* Either DBSQL_ATTACH or DBSQL_DETACH */
  func_def_t const *func,/* func_def_t wrapper for __detach_func() or __attach_func() */
  expr_t *auth_arg,      /* Expression to pass to authorization callback */
  expr_t *filename,     /* Name of database file */
  expr_t *dbname,       /* Name of the database to use internally */
  expr_t *key           /* Database key for encryption extension */
){
  int rc;
  name_context_t s_name;
  vdbe_t *v;
  DBSQL* db = parser->db;
  int reg_args;

  if( DBSQL_OK!=__read_schema(parser) ) goto attach_end;

  if( parser->n_err ) goto attach_end;
  memset(&s_name, 0, sizeof(name_context_t));
  s_name.parser = parser;

  if( 
      DBSQL_OK!=__resolve_attach_expr(&s_name, filename) ||
      DBSQL_OK!=__resolve_attach_expr(&s_name, dbname) ||
      DBSQL_OK!=__resolve_attach_expr(&s_name, key)
  ){
    goto attach_end;
  }

#ifndef DBSQL_OMIT_AUTHORIZATION
  if( ALWAYS(auth_arg) ){
    char *z_auth_arg;
    if( auth_arg->op==TK_STRING ){
      assert( !EXPR_HAS_PROPERTY(auth_arg, EP_IntValue) );
      z_auth_arg = auth_arg->u.token;
    }else{
      z_auth_arg = 0;
    }
    rc = __auth_check(parser, type, z_auth_arg, 0, 0);
    if(rc!=DBSQL_OK ){
      goto attach_end;
    }
  }
#endif /* DBSQL_OMIT_AUTHORIZATION */


  v = __get_vdbe(parser);
  reg_args = __get_temp_range(parser, 4);
  __expr_code(parser, filename, reg_args);
  __expr_code(parser, dbname, reg_args+1);
  __expr_code(parser, key, reg_args+2);

  assert( v || db->malloc_failed );
  if( v ){
    __vdbe_add_function_call(parser, 0, reg_args+3-func->n_arg, reg_args+3,
                               func->n_arg, func, 0);
    /* Code an OP_Expire. For an ATTACH statement, set P1 to true (expire this
    ** statement only). For DETACH, set it to false (expire all existing
    ** statements).
    */
    __vdbe_add_op1(v, OP_Expire, (type==DBSQL_ATTACH));
  }
  
attach_end:
  __expr_delete(db, filename);
  __expr_delete(db, dbname);
  __expr_delete(db, key);
}

/*
** Called by the parser to compile a DETACH statement.
**
**     DETACH dbname
*/
void __detach(parser_t *parser, expr_t *dbname){
  static const func_def_t detach_func = {
    1,                /* n_arg */
    DBSQL_UTF8,      /* func_flags */
    0,                /* user_data */
    0,                /* next */
    __detach_func,       /* s_func */
    0,                /* finalize */
    0, 0,             /* value, inverse */
    "dbsql_detach",  /* name */
    {0}
  };
  __code_attach(parser, DBSQL_DETACH, &detach_func, dbname, 0, 0, dbname);
}

/*
** Called by the parser to compile an ATTACH statement.
**
**     ATTACH p AS dbname KEY key
*/
void __attach(parser_t *parser, expr_t *p, expr_t *dbname, expr_t *key){
  static const func_def_t attach_func = {
    3,                /* n_arg */
    DBSQL_UTF8,      /* func_flags */
    0,                /* user_data */
    0,                /* next */
    __attach_func,       /* s_func */
    0,                /* finalize */
    0, 0,             /* value, inverse */
    "dbsql_attach",  /* name */
    {0}
  };
  __code_attach(parser, DBSQL_ATTACH, &attach_func, p, p, dbname, key);
}
#endif /* DBSQL_OMIT_ATTACH */

/*
** Expression callback used by __fix_aaaa() routines.
*/
static int __fix_expr_cb(walker_t *p, expr_t *expr){
  db_fixer_t *fix = p->u.fix;
  if( !fix->temp ) EXPR_SET_PROPERTY(expr, EP_FromDDL);
  if( expr->op==TK_VARIABLE ){
    if( fix->parser->db->init.busy ){
      expr->op = TK_NULL;
    }else{
      __error_msg(fix->parser, "%s cannot use variables", fix->type);
      return WRC_Abort;
    }
  }
  return WRC_Continue;
}

/*
** select_t callback used by __fix_aaaa() routines.
*/
static int __fix_select_cb(walker_t *p, select_t *p_select){
  db_fixer_t *fix = p->u.fix;
  int i;
  src_item_t *item;
  DBSQL *db = fix->parser->db;
  int i_db = __find_db_name(db, fix->z_db);
  src_list_t *list = p_select->src;

  if( NEVER(list==0) ) return WRC_Continue;
  for(i=0, item=list->a; i<list->n_src; i++, item++){
    if( fix->temp==0 && item->fg.is_subquery==0 ){
      if( item->fg.fixed_schema==0 && item->u4.database!=0 ){
        if( i_db!=__find_db_name(db, item->u4.database) ){
          __error_msg(fix->parser,
              "%s %T cannot reference objects in database %s",
              fix->type, fix->name, item->u4.database);
          return WRC_Abort;
        }
        __dbsql_db_free(db, item->u4.database);
        item->fg.not_cte = 1;
        item->fg.had_schema = 1;
      }
      item->u4.schema = fix->schema;
      item->fg.from_ddl = 1;
      item->fg.fixed_schema = 1;
    }
#if !defined(DBSQL_OMIT_VIEW) || !defined(DBSQL_OMIT_TRIGGER)
    if( list->a[i].fg.is_using==0
     && __walk_expr(&fix->w, list->a[i].u3.on)
    ){
      return WRC_Abort;
    }
#endif
  }
  if( p_select->with ){
    for(i=0; i<p_select->with->n_cte; i++){
      if( __walk_select(p, p_select->with->a[i].p_select) ){
        return WRC_Abort;
      }
    }
  }
  return WRC_Continue;
}

/*
** Initialize a db_fixer_t structure.  This routine must be called prior
** to passing the structure to one of the dbsql_fix_aaaa() routines below.
*/
void __fix_init(
  db_fixer_t *fix,      /* The fixer to be initialized */
  parser_t *parser,      /* Error messages will be written here */
  int i_db,            /* This is the database that must be used */
  const char *type,  /* "view", "trigger", or "index" */
  const Token *name  /* Name of the view, trigger, or index */
){
  DBSQL *db = parser->db;
  assert( db->n_db>i_db );
  fix->parser = parser;
  fix->z_db = db->dbs[i_db].db_s_name;
  fix->schema = db->dbs[i_db].schema;
  fix->type = type;
  fix->name = name;
  fix->temp = (i_db==1);
  fix->w.parser = parser;
  fix->w.expr_callback = __fix_expr_cb;
  fix->w.select_callback = __fix_select_cb;
  fix->w.select_callback2 = __walk_win_defn_dummy_callback;
  fix->w.walker_depth = 0;
  fix->w.code = 0;
  fix->w.u.fix = fix;
}

/*
** The following set of routines walk through the parse tree and assign
** a specific database to all table references where the database name
** was left unspecified in the original SQL statement.  The fix structure
** must have been initialized by a prior call to __fix_init().
**
** These routines are used to make sure that an index, trigger, or
** view in one database does not refer to objects in a different database.
** (Exception: indices, triggers, and views in the TEMP database are
** allowed to refer to anything.)  If a reference is explicitly made
** to an object in a different database, an error message is added to
** parser->errmsg and these routines return non-zero.  If everything
** checks out, these routines return 0.
*/
int __fix_src_list(
  db_fixer_t *fix,       /* Context of the fixation */
  src_list_t *list       /* The Source list to check and modify */
){
  int res = 0;
  if( list ){
    select_t s; 
    memset(&s, 0, sizeof(s));
    s.src = list;
    res = __walk_select(&fix->w, &s);
  }
  return res;
}
#if !defined(DBSQL_OMIT_VIEW) || !defined(DBSQL_OMIT_TRIGGER)
int __fix_select(
  db_fixer_t *fix,       /* Context of the fixation */
  select_t *p_select      /* The SELECT statement to be fixed to one database */
){
  return __walk_select(&fix->w, p_select);
}
int __fix_expr(
  db_fixer_t *fix,     /* Context of the fixation */
  expr_t *expr        /* The expression to be fixed to one database */
){
  return __walk_expr(&fix->w, expr);
}
#endif

#ifndef DBSQL_OMIT_TRIGGER
int __fix_trigger_step(
  db_fixer_t *fix,     /* Context of the fixation */
  trigger_step_t *step /* The trigger step be fixed to one database */
){
  while( step ){
    if( __walk_select(&fix->w, step->p_select)
     || __walk_expr(&fix->w, step->where) 
     || __walk_expr_list(&fix->w, step->expr_list)
     || __fix_src_list(fix, step->src)
    ){
      return 1;
    }
#ifndef DBSQL_OMIT_UPSERT
    {
      upsert_t *up;
      for(up=step->upsert; up; up=up->next_upsert){
        if( __walk_expr_list(&fix->w, up->upsert_target)
         || __walk_expr(&fix->w, up->upsert_target_where)
         || __walk_expr_list(&fix->w, up->upsert_set)
         || __walk_expr(&fix->w, up->upsert_where)
        ){
          return 1;
        }
      }
    }
#endif
    step = step->next;
  }

  return 0;
}
#endif
