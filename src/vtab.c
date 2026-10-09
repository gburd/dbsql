/*
** 2006 June 10
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used to help implement virtual tables.
*/
#ifndef DBSQL_OMIT_VIRTUALTABLE
#include "dbsql_int.h"

/*
** Before a virtual table create() or connect() method is invoked, the
** DBSQL.vtab_ctx member variable is set to point to an instance of
** this struct allocated on the stack. It is used by the implementation of
** the dbsql_declare_vtab() and dbsql_vtab_config() APIs, both of which
** are invoked only from within create and connect methods.
*/
struct vtab_ctx_t {
  v_table_t *v_table;    /* The virtual table being constructed */
  table_t *tab;        /* The table_t object to which the virtual table belongs */
  vtab_ctx_t *prior;    /* Parent context (if any) */
  int declared;      /* True after dbsql_declare_vtab() is called */
};

/*
** Construct and install a module_def_t object for a virtual table.  When this
** routine is called, it is guaranteed that all appropriate locks are held
** and the module is not already part of the connection.
**
** If there already exists a module with name, replace it with the new one.
** If module==0, then delete the module name if it exists.
*/
module_def_t *__vtab_create_module(
  DBSQL *db,                    /* Database in which module is registered */
  const char *name,              /* Name assigned to this module */
  const dbsql_module_t *module,  /* The definition of the module */
  void *aux,                     /* Context pointer for create/connect */
  void (*destroy)(void *)        /* module_def_t destructor function */
){
  module_def_t *mod;
  module_def_t *del;
  char *copy;
  if( module==0 ){
    copy = (char*)name;
    mod = 0;
  }else{
    int n_name = __strlen30(name);
    mod = (module_def_t *)__dbsql_malloc(sizeof(module_def_t) + n_name + 1);
    if( mod==0 ){
      __oom_fault(db);
      return 0;
    }
    copy = (char *)(&mod[1]);
    memcpy(copy, name, n_name+1);
    mod->name = copy;
    mod->module = module;
    mod->aux = aux;
    mod->destroy = destroy;
    mod->epo_tab = 0;
    mod->n_ref_module = 1;
  }
  del = (module_def_t *)__hash_insert(&db->modules,copy,(void*)mod);
  if( del ){
    if( del==mod ){
      __oom_fault(db);
      __dbsql_db_free(db, del);
      mod = 0;
    }else{
      __vtab_eponymous_table_clear(db, del);
      __vtab_module_unref(db, del);
    }
  }
  return mod;
}

/*
** The actual function that does the work of creating a new module.
** This function implements the dbsql_create_module() and
** dbsql_create_module_v2() interfaces.
*/
static int __create_module(
  DBSQL *db,                    /* Database in which module is registered */
  const char *name,              /* Name assigned to this module */
  const dbsql_module_t *module,  /* The definition of the module */
  void *aux,                     /* Context pointer for create/connect */
  void (*destroy)(void *)        /* module_def_t destructor function */
){
  int rc = DBSQL_OK;

  dbsql_mutex_enter(db->mutex);
  (void)__vtab_create_module(db, name, module, aux, destroy);
  rc = __api_exit(db, rc);
  if( rc!=DBSQL_OK && destroy ) destroy(aux);
  dbsql_mutex_leave(db->mutex);
  return rc;
}


/*
** External API function used to create a new virtual-table module.
*/
int dbsql_create_module(
  DBSQL *db,                    /* Database in which module is registered */
  const char *name,              /* Name assigned to this module */
  const dbsql_module_t *module,  /* The definition of the module */
  void *aux                      /* Context pointer for create/connect */
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || name==0 ) return DBSQL_MISUSE_BKPT;
#endif
  return __create_module(db, name, module, aux, 0);
}

/*
** External API function used to create a new virtual-table module.
*/
int dbsql_create_module_v2(
  DBSQL *db,                    /* Database in which module is registered */
  const char *name,              /* Name assigned to this module */
  const dbsql_module_t *module,  /* The definition of the module */
  void *aux,                     /* Context pointer for create/connect */
  void (*destroy)(void *)        /* module_def_t destructor function */
){
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || name==0 ) return DBSQL_MISUSE_BKPT;
#endif
  return __create_module(db, name, module, aux, destroy);
}

/*
** External API to drop all virtual-table modules, except those named
** on the names list.
*/
int dbsql_drop_modules(DBSQL *db, const char** names){
  hash_elem_t *p_this, *next;
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  for(p_this=dbsql_hash_first(&db->modules); p_this; p_this=next){
    module_def_t *mod = (module_def_t*)dbsql_hash_data(p_this);
    next = dbsql_hash_next(p_this);
    if( names ){
      int ii;
      for(ii=0; names[ii]!=0 && strcmp(names[ii],mod->name)!=0; ii++){}
      if( names[ii]!=0 ) continue;
    }
    __create_module(db, mod->name, 0, 0, 0);
  }
  dbsql_mutex_leave(db->mutex);
  return DBSQL_OK;
}

/*
** Decrement the reference count on a module_def_t object.  Destroy the
** module when the reference count reaches zero.
*/
void __vtab_module_unref(DBSQL *db, module_def_t *mod){
  assert( mod->n_ref_module>0 );
  mod->n_ref_module--;
  if( mod->n_ref_module==0 ){
    if( mod->destroy ){
      mod->destroy(mod->aux);
    }
    assert( mod->epo_tab==0 );
    __dbsql_db_free(db, mod);
  }
}

/*
** Lock the virtual table so that it cannot be disconnected.
** Locks nest.  Every lock should have a corresponding unlock.
** If an unlock is omitted, resources leaks will occur.
**
** If a disconnect is attempted while a virtual table is locked,
** the disconnect is deferred until all locks have been removed.
*/
void __vtab_lock(v_table_t *v_tab){
  v_tab->n_ref++;
}


/*
** tab is a pointer to a table_t structure representing a virtual-table.
** Return a pointer to the v_table_t object used by connection db to access
** this virtual-table, if one has been created, or NULL otherwise.
*/
v_table_t *__get_v_table(DBSQL *db, table_t *tab){
  v_table_t *vtab;
  assert( IS_VIRTUAL(tab) );
  for(vtab=tab->u.vtab.p; vtab && vtab->db!=db; vtab=vtab->next);
  return vtab;
}

/*
** Decrement the ref-count on a virtual table object. When the ref-count
** reaches zero, call the disconnect() method to delete the object.
*/
void __vtab_unlock(v_table_t *v_tab){
  DBSQL *db = v_tab->db;

  assert( db );
  assert( v_tab->n_ref>0 );
  assert( db->open_state==DBSQL_STATE_OPEN
       || db->open_state==DBSQL_STATE_ZOMBIE );

  v_tab->n_ref--;
  if( v_tab->n_ref==0 ){
    dbsql_vtab_t *p = v_tab->vtab;
    if( p ){
      p->module->disconnect(p);
    }
    __vtab_module_unref(v_tab->db, v_tab->mod);
    __dbsql_db_free(db, v_tab);
  }
}

/*
** table_t p is a virtual table. This function moves all elements in the
** p->u.vtab.p list to the DBSQL.disconnect lists of their associated
** database connections to be disconnected at the next opportunity.
** Except, if argument db is not NULL, then the entry associated with
** connection db is left in the p->u.vtab.p list.
*/
static v_table_t *__vtab_disconnect_all(DBSQL *db, table_t *p){
  v_table_t *ret = 0;
  v_table_t *v_table;

  assert( IS_VIRTUAL(p) );
  v_table = p->u.vtab.p;
  p->u.vtab.p = 0;

  /* Assert that the mutex (if any) associated with the sm_shared_t database
  ** that contains table p is held by the caller. See header comments
  ** above function __vtab_unlock_list() for an explanation of why
  ** this makes it safe to access the DBSQL.disconnect list of any
  ** database connection that may have an entry in the p->u.vtab.p list.
  */
  assert( db==0 || __schema_mutex_held(db, 0, p->schema) );

  while( v_table ){
    DBSQL *db2 = v_table->db;
    v_table_t *next = v_table->next;
    assert( db2 );
    if( db2==db ){
      ret = v_table;
      p->u.vtab.p = ret;
      ret->next = 0;
    }else{
      v_table->next = db2->disconnect;
      db2->disconnect = v_table;
    }
    v_table = next;
  }

  assert( !db || ret );
  return ret;
}

/*
** table_t *p is a virtual table. This function removes the v_table_t object
** for table *p associated with database connection db from the linked
** list in p->v_tab. It also decrements the v_table_t ref count. This is
** used when closing database connection db to free all of its v_table_t
** objects without disturbing the rest of the schema_t object (which may
** be being used by other shared-cache connections).
*/
void __vtab_disconnect(DBSQL *db, table_t *p){
  v_table_t **pp_v_tab;

  assert( IS_VIRTUAL(p) );
  assert( __sm_holds_all_mutexes(db) );
  assert( dbsql_mutex_held(db->mutex) );

  for(pp_v_tab=&p->u.vtab.p; *pp_v_tab; pp_v_tab=&(*pp_v_tab)->next){
    if( (*pp_v_tab)->db==db  ){
      v_table_t *v_tab = *pp_v_tab;
      *pp_v_tab = v_tab->next;
      __vtab_unlock(v_tab);
      break;
    }
  }
}


/*
** Disconnect all the virtual table objects in the DBSQL.disconnect list.
**
** This function may only be called when the mutexes associated with all
** shared b-tree databases opened using connection db are held by the
** caller. This is done to protect the DBSQL.disconnect list. The
** DBSQL.disconnect list is accessed only as follows:
**
**   1) By this function. In this case, all sm_shared_t mutexes and the mutex
**      associated with the database handle itself must be held.
**
**   2) By function __vtab_disconnect_all(), when it adds a v_table_t entry to
**      the DBSQL.disconnect list. In this case either the sm_shared_t mutex
**      associated with the database the virtual table is stored in is held
**      or, if the virtual table is stored in a non-sharable database, then
**      the database handle mutex is held.
**
** As a result, a DBSQL.disconnect cannot be accessed simultaneously
** by multiple threads. It is thread-safe.
*/
void __vtab_unlock_list(DBSQL *db){
  v_table_t *p = db->disconnect;

  assert( __sm_holds_all_mutexes(db) );
  assert( dbsql_mutex_held(db->mutex) );

  if( p ){
    db->disconnect = 0;
    do {
      v_table_t *next = p->next;
      __vtab_unlock(p);
      p = next;
    }while( p );
  }
}

/*
** Clear any and all virtual-table information from the table_t record.
** This routine is called, for example, just before deleting the table_t
** record.
**
** Since it is a virtual-table, the table_t structure contains a pointer
** to the head of a linked list of v_table_t structures. Each v_table_t
** structure is associated with a single DBSQL* user of the schema.
** The reference count of the v_table_t structure associated with database
** connection db is decremented immediately (which may lead to the
** structure being disconnected and free). Any other v_table_t structures
** in the list are moved to the DBSQL.disconnect list of the associated
** database connection.
*/
void __vtab_clear(DBSQL *db, table_t *p){
  assert( IS_VIRTUAL(p) );
  assert( db!=0 );
  if( db->pn_bytes_freed==0 ) __vtab_disconnect_all(0, p);
  if( p->u.vtab.args ){
    int i;
    for(i=0; i<p->u.vtab.n_arg; i++){
      if( i!=1 ) __dbsql_db_free(db, p->u.vtab.args[i]);
    }
    __dbsql_db_free(db, p->u.vtab.args);
  }
}

/*
** Add a new module argument to table->u.vtab.args[].
** The string is not copied - the pointer is stored.  The
** string will be freed automatically when the table is
** deleted.
*/
static void __add_module_argument(parser_t *parser, table_t *table, char *arg){
  int64_t n_bytes;
  char **module_args;
  DBSQL *db = parser->db;

  assert( IS_VIRTUAL(table) );
  n_bytes = sizeof(char *)*(2+table->u.vtab.n_arg);
  if( table->u.vtab.n_arg+3>=db->limits[DBSQL_LIMIT_COLUMN] ){
    __error_msg(parser, "too many columns on %s", table->name);
  }
  module_args = __db_realloc(db, table->u.vtab.args, n_bytes);
  if( module_args==0 ){
    __dbsql_db_free(db, arg);
  }else{
    int i = table->u.vtab.n_arg++;
    module_args[i] = arg;
    module_args[i+1] = 0;
    table->u.vtab.args = module_args;
  }
}

/*
** The parser calls this routine when it first sees a CREATE VIRTUAL TABLE
** statement.  The module name has been parsed, but the optional list
** of parameters that follow the module name are still pending.
*/
void __vtab_begin_parse(
  parser_t *parser,        /* Parsing context */
  Token *name1,        /* Name of new table, or database name */
  Token *name2,        /* Name of new table or NULL */
  Token *module_name,   /* Name of the module for the virtual table */
  int if_not_exists       /* No error if the table already exists */
){
  table_t *table;        /* The new virtual table */
  DBSQL *db;          /* Database connection */

  __start_table(parser, name1, name2, 0, 0, 1, if_not_exists);
  table = parser->new_table;
  if( table==0 ) return;
  assert( 0==table->p_index );
  table->tab_type = TABTYP_VTAB;

  db = parser->db;

  assert( table->u.vtab.n_arg==0 );
  __add_module_argument(parser, table, __name_from_token(db, module_name));
  __add_module_argument(parser, table, 0);
  __add_module_argument(parser, table, __db_str_dup(db, table->name));
  assert( (parser->s_name_token.z==name2->z && name2->z!=0)
       || (parser->s_name_token.z==name1->z && name2->z==0)
  );
  parser->s_name_token.n = (int)(
      &module_name->z[module_name->n] - parser->s_name_token.z
  );

#ifndef DBSQL_OMIT_AUTHORIZATION
  /* Creating a virtual table invokes the authorization callback twice.
  ** The first invocation, to obtain permission to INSERT a row into the
  ** dbsql_schema table, has already been made by __start_table().
  ** The second call, to obtain permission to create the table, is made now.
  */
  if( table->u.vtab.args ){
    int i_db = __schema_to_index(db, table->schema);
    assert( i_db>=0 ); /* The database the table is being created in */
    __auth_check(parser, DBSQL_CREATE_VTABLE, table->name,
            table->u.vtab.args[0], parser->db->dbs[i_db].db_s_name);
  }
#endif
}

/*
** This routine takes the module argument that has been accumulating
** in parser->arg[] and appends it to the list of arguments on the
** virtual table currently under construction in parser->table.
*/
static void __add_argument_to_vtab(parser_t *parser){
  if( parser->s_arg.z && parser->new_table ){
    const char *z = (const char*)parser->s_arg.z;
    int n = parser->s_arg.n;
    DBSQL *db = parser->db;
    __add_module_argument(parser, parser->new_table, __db_str_n_dup(db, z, n));
  }
}

/*
** The parser calls this routine after the CREATE VIRTUAL TABLE statement
** has been completely parsed.
*/
void __vtab_finish_parse(parser_t *parser, Token *end){
  table_t *tab = parser->new_table;  /* The table being constructed */
  DBSQL *db = parser->db;         /* The database connection */

  if( tab==0 ) return;
  assert( IS_VIRTUAL(tab) );
  __add_argument_to_vtab(parser);
  parser->s_arg.z = 0;
  if( tab->u.vtab.n_arg<1 ) return;

  /* If the CREATE VIRTUAL TABLE statement is being entered for the
  ** first time (in other words if the virtual table is actually being
  ** created now instead of just being read out of dbsql_schema) then
  ** do additional initialization work and store the statement text
  ** in the dbsql_schema table.
  */
  if( !db->init.busy ){
    char *stmt;
    char *where;
    int i_db;
    int i_reg;
    vdbe_t *v;

    __may_abort(parser);

    /* Compute the complete text of the CREATE VIRTUAL TABLE statement */
    if( end ){
      parser->s_name_token.n = (int)(end->z - parser->s_name_token.z) + end->n;
    }
    stmt = __m_printf(db, "CREATE VIRTUAL TABLE %T", &parser->s_name_token);

    /* A slot for the record has already been allocated in the
    ** schema table.  We just need to update that slot with all
    ** the information we've collected.
    **
    ** The VM register number parser->u1.cr.reg_rowid holds the rowid of an
    ** entry in the dbsql_schema table that was created for this vtab
    ** by __start_table().
    */
    i_db = __schema_to_index(db, tab->schema);
    assert( parser->is_create );
    __nested_parse(parser,
      "UPDATE %Q." LEGACY_SCHEMA_TABLE " "
         "SET type='table', name=%Q, tbl_name=%Q, rootpage=0, sql=%Q "
       "WHERE rowid=#%d",
      db->dbs[i_db].db_s_name,
      tab->name,
      tab->name,
      stmt,
      parser->u1.cr.reg_rowid
    );
    v = __get_vdbe(parser);
    __change_cookie(parser, i_db);

    __vdbe_add_op0(v, OP_Expire);
    where = __m_printf(db, "name=%Q AND sql=%Q", tab->name, stmt);
    __vdbe_add_parse_schema_op(v, i_db, where, 0);
    __dbsql_db_free(db, stmt);

    i_reg = ++parser->n_mem;
    __vdbe_load_string(v, i_reg, tab->name);
    __vdbe_add_op2(v, OP_VCreate, i_db, i_reg);
  }else{
    /* If we are rereading the dbsql_schema table create the in-memory
    ** record of the table. */
    table_t *old;
    schema_t *schema = tab->schema;
    const char *name = tab->name;
    assert( name!=0 );
    __mark_all_shadow_tables_of(db, tab);
    old = __hash_insert(&schema->tbl_hash, name, tab);
    if( old ){
      __oom_fault(db);
      assert( tab==old );  /* Malloc must have failed inside __hash_insert() */
      return;
    }
    parser->new_table = 0;
  }
}

/*
** The parser calls this routine when it sees the first token
** of an argument to the module name in a CREATE VIRTUAL TABLE statement.
*/
void __vtab_arg_init(parser_t *parser){
  __add_argument_to_vtab(parser);
  parser->s_arg.z = 0;
  parser->s_arg.n = 0;
}

/*
** The parser calls this routine for each token after the first token
** in an argument to the module name in a CREATE VIRTUAL TABLE statement.
*/
void __vtab_arg_extend(parser_t *parser, Token *p){
  Token *arg = &parser->s_arg;
  if( arg->z==0 ){
    arg->z = p->z;
    arg->n = p->n;
  }else{
    assert(arg->z <= p->z);
    arg->n = (int)(&p->z[p->n] - arg->z);
  }
}

/*
** Invoke a virtual table constructor (either create or connect). The
** pointer to the function to invoke is passed as the fourth parameter
** to this procedure.
*/
static int __vtab_call_constructor(
  DBSQL *db,
  table_t *tab,
  module_def_t *mod,
  int (*construct)(DBSQL*,void*,int,const char*const*,dbsql_vtab_t**,char**),
  char **err_p
){
  vtab_ctx_t s_ctx;
  v_table_t *v_table;
  int rc;
  const char *const*args;
  int n_arg = tab->u.vtab.n_arg;
  char *err = 0;
  char *module_name;
  int i_db;
  vtab_ctx_t *ctx;

  assert( IS_VIRTUAL(tab) );
  args = (const char *const*)tab->u.vtab.args;

  /* Check that the virtual-table is not already being initialized */
  for(ctx=db->vtab_ctx; ctx; ctx=ctx->prior){
    if( ctx->tab==tab ){
      *err_p = __m_printf(db,
          "vtable constructor called recursively: %s", tab->name
      );
      return DBSQL_LOCKED;
    }
  }

  module_name = __db_str_dup(db, tab->name);
  if( !module_name ){
    return DBSQL_NOMEM_BKPT;
  }

  v_table = __malloc_zero(sizeof(v_table_t));
  if( !v_table ){
    __oom_fault(db);
    __dbsql_db_free(db, module_name);
    return DBSQL_NOMEM_BKPT;
  }
  v_table->db = db;
  v_table->mod = mod;
  v_table->vtab_risk = DBSQL_VTABRISK_Normal;

  i_db = __schema_to_index(db, tab->schema);
  tab->u.vtab.args[1] = db->dbs[i_db].db_s_name;

  /* Invoke the virtual table constructor */
  assert( &db->vtab_ctx );
  assert( construct );
  s_ctx.tab = tab;
  s_ctx.v_table = v_table;
  s_ctx.prior = db->vtab_ctx;
  s_ctx.declared = 0;
  db->vtab_ctx = &s_ctx;
  tab->n_tab_ref++;
  rc = construct(db, mod->aux, n_arg, args, &v_table->vtab, &err);
  assert( tab!=0 );
  assert( tab->n_tab_ref>1 || rc!=DBSQL_OK );
  __delete_table(db, tab);
  db->vtab_ctx = s_ctx.prior;
  if( rc==DBSQL_NOMEM ) __oom_fault(db);
  assert( s_ctx.tab==tab );

  if( DBSQL_OK!=rc ){
    if( err==0 ){
      *err_p = __m_printf(db, "vtable constructor failed: %s", module_name);
    }else {
      *err_p = __m_printf(db, "%s", err);
      dbsql_free(err);
    }
    __dbsql_db_free(db, v_table);
  }else if( ALWAYS(v_table->vtab) ){
    /* Justification of ALWAYS():  A correct vtab constructor must allocate
    ** the dbsql_vtab_t object if successful.  */
    memset(v_table->vtab, 0, sizeof(v_table->vtab[0]));
    v_table->vtab->module = mod->module;
    mod->n_ref_module++;
    v_table->n_ref = 1;
    if( s_ctx.declared==0 ){
      const char *format = "vtable constructor did not declare schema: %s";
      *err_p = __m_printf(db, format, module_name);
      __vtab_unlock(v_table);
      rc = DBSQL_ERROR;
    }else{
      int i_col;
      u_int16_t ooo_hidden = 0;
      /* If everything went according to plan, link the new v_table_t structure
      ** into the linked list headed by tab->u.vtab.p. Then loop through the
      ** columns of the table to see if any of them contain the token "hidden".
      ** If so, set the column_t COLFLAG_HIDDEN flag and remove the token from
      ** the type string.  */
      v_table->next = tab->u.vtab.p;
      tab->u.vtab.p = v_table;

      for(i_col=0; i_col<tab->n_col; i_col++){
        char *type = __column_type(&tab->cols[i_col], "");
        int n_type;
        int i = 0;
        n_type = __strlen30(type);
        for(i=0; i<n_type; i++){
          if( 0==__str_nicmp("hidden", &type[i], 6)
           && (i==0 || type[i-1]==' ')
           && (type[i+6]=='\0' || type[i+6]==' ')
          ){
            break;
          }
        }
        if( i<n_type ){
          int j;
          int n_del = 6 + (type[i+6] ? 1 : 0);
          for(j=i; (j+n_del)<=n_type; j++){
            type[j] = type[j+n_del];
          }
          if( type[i]=='\0' && i>0 ){
            assert(type[i-1]==' ');
            type[i-1] = '\0';
          }
          tab->cols[i_col].col_flags |= COLFLAG_HIDDEN;
          tab->tab_flags |= TF_HasHidden;
          ooo_hidden = TF_OOOHidden;
        }else{
          tab->tab_flags |= ooo_hidden;
        }
      }
    }
  }

  __dbsql_db_free(db, module_name);
  return rc;
}

/*
** This function is invoked by the parser to call the connect() method
** of the virtual table tab. If an error occurs, an error code is returned
** and an error left in parser.
**
** This call is a no-op if table tab is not a virtual table.
*/
int __vtab_call_connect(parser_t *parser, table_t *tab){
  DBSQL *db = parser->db;
  const char *z_mod;
  module_def_t *mod;
  int rc;

  assert( tab );
  assert( IS_VIRTUAL(tab) );
  if( __get_v_table(db, tab) ){
    return DBSQL_OK;
  }

  /* Locate the required virtual table module */
  z_mod = tab->u.vtab.args[0];
  mod = (module_def_t*)__hash_find(&db->modules, z_mod);

  if( !mod ){
    const char *module = tab->u.vtab.args[0];
    __error_msg(parser, "no such module: %s", module);
    rc = DBSQL_ERROR;
  }else{
    char *err = 0;
    rc = __vtab_call_constructor(db, tab, mod, mod->module->connect, &err);
    if( rc!=DBSQL_OK ){
      __error_msg(parser, "%s", err);
      parser->rc = rc;
    }
    __dbsql_db_free(db, err);
  }

  return rc;
}
/*
** Grow the db->v_trans[] array so that there is room for at least one
** more v-table. Return DBSQL_NOMEM if a malloc fails, or DBSQL_OK otherwise.
*/
static int __grow_v_trans(DBSQL *db){
  const int ARRAY_INCR = 5;

  /* Grow the DBSQL.v_trans array if required */
  if( (db->n_v_trans%ARRAY_INCR)==0 ){
    v_table_t **v_trans;
    int64_t n_bytes = sizeof(dbsql_vtab_t*)*
                                 ((int64_t)db->n_v_trans + ARRAY_INCR);
    v_trans = __db_realloc(db, (void *)db->v_trans, n_bytes);
    if( !v_trans ){
      return DBSQL_NOMEM_BKPT;
    }
    memset(&v_trans[db->n_v_trans], 0, sizeof(dbsql_vtab_t *)*ARRAY_INCR);
    db->v_trans = v_trans;
  }

  return DBSQL_OK;
}

/*
** Add the virtual table v_tab to the array DBSQL.v_trans[]. Space should
** have already been reserved using __grow_v_trans().
*/
static void __add_to_v_trans(DBSQL *db, v_table_t *v_tab){
  /* Add vtab to the end of DBSQL.v_trans */
  db->v_trans[db->n_v_trans++] = v_tab;
  __vtab_lock(v_tab);
}

/*
** This function is invoked by the vdbe to call the create method
** of the virtual table named tab in database i_db.
**
** If an error occurs, *err_p is set to point to an English language
** description of the error and an DBSQL_XXX error code is returned.
** In this case the caller must call __dbsql_db_free(db, ) on *err_p.
*/
int __vtab_call_create(DBSQL *db, int i_db, const char *z_tab, char **err_p){
  int rc = DBSQL_OK;
  table_t *tab;
  module_def_t *mod;
  const char *z_mod;

  tab = __find_table(db, z_tab, db->dbs[i_db].db_s_name);
  assert( tab && IS_VIRTUAL(tab) && !tab->u.vtab.p );

  /* Locate the required virtual table module */
  z_mod = tab->u.vtab.args[0];
  mod = (module_def_t*)__hash_find(&db->modules, z_mod);

  /* If the module has been registered and includes a Create method,
  ** invoke it now. If the module has not been registered, return an
  ** error. Otherwise, do nothing.
  */
  if( mod==0 || mod->module->create==0 || mod->module->destroy==0 ){
    *err_p = __m_printf(db, "no such module: %s", z_mod);
    rc = DBSQL_ERROR;
  }else{
    rc = __vtab_call_constructor(db, tab, mod, mod->module->create, err_p);
  }

  /* Justification of ALWAYS():  The constructor method is required to
  ** create a valid dbsql_vtab_t if it returns DBSQL_OK. */
  if( rc==DBSQL_OK && ALWAYS(__get_v_table(db, tab)) ){
    rc = __grow_v_trans(db);
    if( rc==DBSQL_OK ){
      __add_to_v_trans(db, __get_v_table(db, tab));
    }
  }

  return rc;
}

/*
** This function is used to set the schema of a virtual table.  It is only
** valid to call this function from within the create() or connect() of a
** virtual table module.
*/
int dbsql_declare_vtab(DBSQL *db, const char *create_table){
  vtab_ctx_t *ctx;
  int rc = DBSQL_OK;
  table_t *tab;
  parser_t s_parse;
  int init_busy;
  int i;
  const unsigned char *z;
  static const u_int8_t keywords[] = { TK_CREATE, TK_TABLE, 0 };

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) || create_table==0 ){
    return DBSQL_MISUSE_BKPT;
  }
#endif

  /* Verify that the first two keywords in the CREATE TABLE statement
  ** really are "CREATE" and "TABLE".  If this is not the case, then
  ** dbsql_declare_vtab() is being misused.
  */
  z = (const unsigned char*)create_table;
  for(i=0; keywords[i]; i++){
    int token_type = 0;
    do{
      z += __get_token(z, &token_type);
    }while( token_type==TK_SPACE || token_type==TK_COMMENT );
    if( token_type!=keywords[i] ){
      __error_with_msg(db, DBSQL_ERROR, "syntax error");
      return DBSQL_ERROR;
    }
  }
 
  dbsql_mutex_enter(db->mutex);
  ctx = db->vtab_ctx;
  if( !ctx || ctx->declared ){
    __dbsql_error(db, DBSQL_MISUSE_BKPT);
    dbsql_mutex_leave(db->mutex);
    return DBSQL_MISUSE_BKPT;
  }

  tab = ctx->tab;
  assert( IS_VIRTUAL(tab) );

  __parse_object_init(&s_parse, db);
  s_parse.parse_mode = PARSE_MODE_DECLARE_VTAB;
  s_parse.disable_triggers = 1;
  /* We should never be able to reach this point while loading the
  ** schema.  Nevertheless, defend against that (turn off db->init.busy)
  ** in case a bug arises. */
  assert( db->init.busy==0 );
  init_busy = db->init.busy;
  db->init.busy = 0;
  s_parse.n_query_loop = 1;
  if( DBSQL_OK==__run_parser(&s_parse, create_table) ){
    assert( s_parse.new_table!=0 );
    assert( !db->malloc_failed );
    assert( IS_ORDINARY_TABLE(s_parse.new_table) );
    assert( s_parse.errmsg==0 );
    if( !tab->cols ){
      table_t *p_new = s_parse.new_table;
      index_t *idx;
      tab->cols = p_new->cols;
      assert( IS_ORDINARY_TABLE(p_new) );
      __expr_list_delete(db, p_new->u.tab.dflt_list);
      tab->n_nv_col = tab->n_col = p_new->n_col;
      tab->tab_flags |= p_new->tab_flags & (TF_WithoutRowid|TF_NoVisibleRowid);
      p_new->n_col = 0;
      p_new->cols = 0;
      assert( tab->p_index==0 );
      assert( HAS_ROWID(p_new) || __primary_key_index(p_new)!=0 );
      if( !HAS_ROWID(p_new)
       && ctx->v_table->mod->module->update!=0
       && __primary_key_index(p_new)->n_key_col!=1
      ){
        /* WITHOUT ROWID virtual tables must either be read-only (update==0)
        ** or else must have a single-column PRIMARY KEY */
        rc = DBSQL_ERROR;
      }
      idx = p_new->p_index;
      if( idx ){
        assert( idx->next==0 );
        tab->p_index = idx;
        p_new->p_index = 0;
        idx->table = tab;
      }
    }
    ctx->declared = 1;
  }else{
    __error_with_msg(db, DBSQL_ERROR,
          (s_parse.errmsg ? "%s" : 0), s_parse.errmsg);
    __dbsql_db_free(db, s_parse.errmsg);
    rc = DBSQL_ERROR;
  }
  s_parse.parse_mode = PARSE_MODE_NORMAL;

  if( s_parse.vdbe ){
    __vdbe_finalize(s_parse.vdbe);
  }
  __delete_table(db, s_parse.new_table);
  __parse_object_reset(&s_parse);
  db->init.busy = init_busy;

  assert( (rc&0xff)==rc );
  rc = __api_exit(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

/*
** This function is invoked by the vdbe to call the destroy method
** of the virtual table named tab in database i_db. This occurs
** when a DROP TABLE is mentioned.
**
** This call is a no-op if tab is not a virtual table.
*/
int __vtab_call_destroy(DBSQL *db, int i_db, const char *z_tab){
  int rc = DBSQL_OK;
  table_t *tab;

  tab = __find_table(db, z_tab, db->dbs[i_db].db_s_name);
  if( ALWAYS(tab!=0)
   && ALWAYS(IS_VIRTUAL(tab))
   && ALWAYS(tab->u.vtab.p!=0)
  ){
    v_table_t *p;
    int (*destroy)(dbsql_vtab_t *);
    for(p=tab->u.vtab.p; p; p=p->next){
      assert( p->vtab );
      if( p->vtab->n_ref>0 ){
        return DBSQL_LOCKED;
      }
    }
    p = __vtab_disconnect_all(db, tab);
    destroy = p->mod->module->destroy;
    if( destroy==0 ) destroy = p->mod->module->disconnect;
    assert( destroy!=0 );
    tab->n_tab_ref++;
    rc = destroy(p->vtab);
    /* Remove the dbsql_vtab_t* from the v_trans[] array, if applicable */
    if( rc==DBSQL_OK ){
      assert( tab->u.vtab.p==p && p->next==0 );
      p->vtab = 0;
      tab->u.vtab.p = 0;
      __vtab_unlock(p);
    }
    __delete_table(db, tab);
  }

  return rc;
}

/*
** This function invokes either the rollback or commit method
** of each of the virtual tables in the DBSQL.v_trans array. The method
** called is identified by the second argument, "offset", which is
** the offset of the method to call in the dbsql_module_t structure.
**
** The array is cleared after invoking the callbacks.
*/
static void __call_finaliser(DBSQL *db, int offset){
  int i;
  if( db->v_trans ){
    v_table_t **v_trans = db->v_trans;
    db->v_trans = 0;
    for(i=0; i<db->n_v_trans; i++){
      v_table_t *v_tab = v_trans[i];
      dbsql_vtab_t *p = v_tab->vtab;
      if( p ){
        int (*x)(dbsql_vtab_t *);
        x = *(int (**)(dbsql_vtab_t *))((char *)p->module + offset);
        if( x ) x(p);
      }
      v_tab->i_savepoint = 0;
      __vtab_unlock(v_tab);
    }
    __dbsql_db_free(db, v_trans);
    db->n_v_trans = 0;
  }
}

/*
** Invoke the x_sync method of all virtual tables in the DBSQL.v_trans
** array. Return the error code for the first error that occurs, or
** DBSQL_OK if all x_sync operations are successful.
**
** If an error message is available, leave it in p->errmsg.
*/
int __vtab_sync(DBSQL *db, vdbe_t *p){
  int i;
  int rc = DBSQL_OK;
  v_table_t **v_trans = db->v_trans;

  db->v_trans = 0;
  for(i=0; rc==DBSQL_OK && i<db->n_v_trans; i++){
    int (*x)(dbsql_vtab_t *);
    dbsql_vtab_t *vtab = v_trans[i]->vtab;
    if( vtab && (x = vtab->module->x_sync)!=0 ){
      rc = x(vtab);
      __vtab_import_errmsg(p, vtab);
    }
  }
  db->v_trans = v_trans;
  return rc;
}

/*
** Invoke the rollback method of all virtual tables in the
** DBSQL.v_trans array. Then clear the array itself.
*/
int __vtab_rollback(DBSQL *db){
  __call_finaliser(db, offsetof(dbsql_module_t,rollback));
  return DBSQL_OK;
}

/*
** Invoke the commit method of all virtual tables in the
** DBSQL.v_trans array. Then clear the array itself.
*/
int __vtab_commit(DBSQL *db){
  __call_finaliser(db, offsetof(dbsql_module_t,commit));
  return DBSQL_OK;
}

/*
** If the virtual table vtab supports the transaction interface
** (begin/rollback/commit and optionally x_sync) and a transaction is
** not currently open, invoke the begin method now.
**
** If the begin call is successful, place the dbsql_vtab_t pointer
** in the DBSQL.v_trans array.
*/
int __vtab_begin(DBSQL *db, v_table_t *v_tab){
  int rc = DBSQL_OK;
  const dbsql_module_t *module;

  /* Special case: If db->v_trans is NULL and db->n_v_trans is greater
  ** than zero, then this function is being called from within a
  ** virtual module x_sync() callback. It is illegal to write to
  ** virtual module tables in this case, so return DBSQL_LOCKED.
  */
  if( __vtab_in_sync(db) ){
    return DBSQL_LOCKED;
  }
  if( !v_tab ){
    return DBSQL_OK;
  }
  module = v_tab->vtab->module;

  if( module->begin ){
    int i;

    /* If vtab is already in the v_trans array, return early */
    for(i=0; i<db->n_v_trans; i++){
      if( db->v_trans[i]==v_tab ){
        return DBSQL_OK;
      }
    }

    /* Invoke the begin method. If successful, add the vtab to the
    ** DBSQL.v_trans[] array. */
    rc = __grow_v_trans(db);
    if( rc==DBSQL_OK ){
      rc = module->begin(v_tab->vtab);
      if( rc==DBSQL_OK ){
        int i_svpt = db->n_statement + db->n_savepoint;
        __add_to_v_trans(db, v_tab);
        if( i_svpt && module->savepoint ){
          v_tab->i_savepoint = i_svpt;
          rc = module->savepoint(v_tab->vtab, i_svpt-1);
        }
      }
    }
  }
  return rc;
}

/*
** Invoke either the savepoint, rollback_to or release method of all
** virtual tables that currently have an open transaction. Pass i_savepoint
** as the second argument to the virtual table method invoked.
**
** If op is SAVEPOINT_BEGIN, the savepoint method is invoked. If it is
** SAVEPOINT_ROLLBACK, the rollback_to method. Otherwise, if op is
** SAVEPOINT_RELEASE, then the release method of each virtual table with
** an open transaction is invoked.
**
** If any virtual table method returns an error code other than DBSQL_OK,
** processing is abandoned and the error returned to the caller of this
** function immediately. If all calls to virtual table methods are successful,
** DBSQL_OK is returned.
*/
int __vtab_savepoint(DBSQL *db, int op, int i_savepoint){
  int rc = DBSQL_OK;

  assert( op==SAVEPOINT_RELEASE||op==SAVEPOINT_ROLLBACK||op==SAVEPOINT_BEGIN );
  assert( i_savepoint>=-1 );
  if( db->v_trans ){
    int i;
    for(i=0; rc==DBSQL_OK && i<db->n_v_trans; i++){
      v_table_t *v_tab = db->v_trans[i];
      const dbsql_module_t *mod = v_tab->mod->module;
      if( v_tab->vtab && mod->i_version>=2 ){
        int (*method)(dbsql_vtab_t *, int);
        __vtab_lock(v_tab);
        switch( op ){
          case SAVEPOINT_BEGIN:
            method = mod->savepoint;
            v_tab->i_savepoint = i_savepoint+1;
            break;
          case SAVEPOINT_ROLLBACK:
            method = mod->rollback_to;
            break;
          default:
            method = mod->release;
            break;
        }
        if( method && v_tab->i_savepoint>i_savepoint ){
          u_int64_t saved_flags = (db->flags & DBSQL_Defensive);
          db->flags &= ~(u_int64_t)DBSQL_Defensive;
          rc = method(v_tab->vtab, i_savepoint);
          db->flags |= saved_flags;
        }
        __vtab_unlock(v_tab);
      }
    }
  }
  return rc;
}

/*
** The first parameter (def) is a function implementation.  The
** second parameter (expr) is the first argument to this function.
** If expr is a column in a virtual table, then let the virtual
** table implementation have an opportunity to overload the function.
**
** This routine is used to allow virtual table implementations to
** overload MATCH, LIKE, GLOB, and REGEXP operators.
**
** Return either the def argument (indicating no change) or a
** new func_def_t structure that is marked as ephemeral using the
** DBSQL_FUNC_EPHEM flag.
*/
func_def_t *__vtab_overload_function(
  DBSQL *db,    /* Database connection for reporting malloc problems */
  func_def_t *def,  /* Function to possibly overload */
  int n_arg,       /* Number of arguments to the function */
  expr_t *expr     /* First argument to the function */
){
  table_t *tab;
  dbsql_vtab_t *vtab;
  dbsql_module_t *mod;
  void (*s_func)(dbsql_func_t*,int,dbsql_value_t**) = 0;
  void *arg = 0;
  func_def_t *p_new;
  int rc = 0;

  /* Check to see the left operand is a column in a virtual table */
  if( NEVER(expr==0) ) return def;
  if( expr->op!=TK_COLUMN ) return def;
  assert( EXPR_USE_Y_TAB(expr) );
  tab = expr->y.tab;
  if( NEVER(tab==0) ) return def;
  if( !IS_VIRTUAL(tab) ) return def;
  vtab = __get_v_table(db, tab)->vtab;
  assert( vtab!=0 );
  assert( vtab->module!=0 );
  mod = (dbsql_module_t *)vtab->module;
  if( mod->find_function==0 ) return def;

  /* Call the find_function method on the virtual table implementation
  ** to see if the implementation wants to overload this function.
  **
  ** Though undocumented, we have historically always invoked find_function
  ** with an all lower-case function name.  Continue in this tradition to
  ** avoid any chance of an incompatibility.
  */
#ifdef DBSQL_DEBUG
  {
    int i;
    for(i=0; def->name[i]; i++){
      unsigned char x = (unsigned char)def->name[i];
      assert( x==__upper_to_lower[x] );
    }
  }
#endif
  rc = mod->find_function(vtab, n_arg, def->name, &s_func, &arg);
  if( rc==0 ){
    return def;
  }

  /* Create a new ephemeral function definition for the overloaded
  ** function */
  p_new = __db_malloc_zero(db, sizeof(*p_new)
                             + __strlen30(def->name) + 1);
  if( p_new==0 ){
    return def;
  }
  *p_new = *def;
  p_new->name = (const char*)&p_new[1];
  memcpy((char*)&p_new[1], def->name, __strlen30(def->name)+1);
  p_new->s_func = s_func;
  p_new->user_data = arg;
  p_new->func_flags |= DBSQL_FUNC_EPHEM;
  return p_new;
}

/*
** Make sure virtual table tab is contained in the parser->virtual_locks[]
** array so that an OP_VBegin will get generated for it.  Add tab to the
** array if it is missing.  If tab is already in the array, this routine
** is a no-op.
*/
void __vtab_make_writable(parser_t *parser, table_t *tab){
  parser_t *toplevel = __parse_toplevel(parser);
  int i, n;
  table_t **vtab_locks;

  assert( IS_VIRTUAL(tab) );
  for(i=0; i<toplevel->n_vtab_lock; i++){
    if( tab==toplevel->vtab_locks[i] ) return;
  }
  n = (toplevel->n_vtab_lock+1)*sizeof(toplevel->vtab_locks[0]);
  vtab_locks = __dbsql_realloc(toplevel->vtab_locks, n);
  if( vtab_locks ){
    toplevel->vtab_locks = vtab_locks;
    toplevel->vtab_locks[toplevel->n_vtab_lock++] = tab;
  }else{
    __oom_fault(toplevel->db);
  }
}

/*
** Check to see if virtual table module mod can be have an eponymous
** virtual table instance.  If it can, create one if one does not already
** exist. Return non-zero if either the eponymous virtual table instance
** exists when this routine returns or if an attempt to create it failed
** and an error message was left in parser.
**
** An eponymous virtual table instance is one that is named after its
** module, and more importantly, does not require a CREATE VIRTUAL TABLE
** statement in order to come into existence.  Eponymous virtual table
** instances always exist.  They cannot be DROP-ed.
**
** Any virtual table module for which connect and create are the same
** method can have an eponymous virtual table instance.
*/
int __vtab_eponymous_table_init(parser_t *parser, module_def_t *mod){
  const dbsql_module_t *module = mod->module;
  table_t *tab;
  char *err = 0;
  int rc;
  DBSQL *db = parser->db;
  if( mod->epo_tab ) return 1;
  if( module->create!=0 && module->create!=module->connect ) return 0;
  tab = __db_malloc_zero(db, sizeof(table_t));
  if( tab==0 ) return 0;
  tab->name = __db_str_dup(db, mod->name);
  if( tab->name==0 ){
    __dbsql_db_free(db, tab);
    return 0;
  }
  mod->epo_tab = tab;
  tab->n_tab_ref = 1;
  tab->tab_type = TABTYP_VTAB;
  tab->schema = db->dbs[0].schema;
  assert( tab->u.vtab.n_arg==0 );
  tab->i_p_key = -1;
  tab->tab_flags |= TF_Eponymous;
  __add_module_argument(parser, tab, __db_str_dup(db, tab->name));
  __add_module_argument(parser, tab, 0);
  __add_module_argument(parser, tab, __db_str_dup(db, tab->name));
  db->n_schema_lock++;
  rc = __vtab_call_constructor(db, tab, mod, module->connect, &err);
  db->n_schema_lock--;
  if( rc ){
    __error_msg(parser, "%s", err);
    parser->rc = rc;
    __dbsql_db_free(db, err);
    __vtab_eponymous_table_clear(db, mod);
  }
  return 1;
}

/*
** Erase the eponymous virtual table instance associated with
** virtual table module mod, if it exists.
*/
void __vtab_eponymous_table_clear(DBSQL *db, module_def_t *mod){
  table_t *tab = mod->epo_tab;
  if( tab!=0 ){
    /* Mark the table as Ephemeral prior to deleting it, so that the
    ** __delete_table() routine will know that it is not stored in
    ** the schema. */
    tab->tab_flags |= TF_Ephemeral;
    __delete_table(db, tab);
    mod->epo_tab = 0;
  }
}

/*
** Return the ON CONFLICT resolution mode in effect for the virtual
** table update operation currently in progress.
**
** The results of this routine are undefined unless it is called from
** within an update method.
*/
int dbsql_vtab_on_conflict(DBSQL *db){
  static const unsigned char maps[] = {
    DBSQL_ROLLBACK, DBSQL_ABORT, DBSQL_FAIL, DBSQL_IGNORE, DBSQL_REPLACE
  };
#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  assert( OE_Rollback==1 && OE_Abort==2 && OE_Fail==3 );
  assert( OE_Ignore==4 && OE_Replace==5 );
  assert( db->vtab_on_conflict>=1 && db->vtab_on_conflict<=5 );
  return (int)maps[db->vtab_on_conflict-1];
}

/*
** Call from within the create() or connect() methods to provide
** the dbsql core with additional information about the behavior
** of the virtual table being implemented.
*/
int dbsql_vtab_config(DBSQL *db, int op, ...){
  va_list ap;
  int rc = DBSQL_OK;
  vtab_ctx_t *p;

#ifdef DBSQL_ENABLE_API_ARMOR
  if( !__safety_check_ok(db) ) return DBSQL_MISUSE_BKPT;
#endif
  dbsql_mutex_enter(db->mutex);
  p = db->vtab_ctx;
  if( !p ){
    rc = DBSQL_MISUSE_BKPT;
  }else{
    assert( p->tab==0 || IS_VIRTUAL(p->tab) );
    va_start(ap, op);
    switch( op ){
      case DBSQL_VTAB_CONSTRAINT_SUPPORT: {
        p->v_table->constraint = (u_int8_t)va_arg(ap, int);
        break;
      }
      case DBSQL_VTAB_INNOCUOUS: {
        p->v_table->vtab_risk = DBSQL_VTABRISK_Low;
        break;
      }
      case DBSQL_VTAB_DIRECTONLY: {
        p->v_table->vtab_risk = DBSQL_VTABRISK_High;
        break;
      }
      case DBSQL_VTAB_USES_ALL_SCHEMAS: {
        p->v_table->all_schemas = 1;
        break;
      }
      default: {
        rc = DBSQL_MISUSE_BKPT;
        break;
      }
    }
    va_end(ap);
  }

  if( rc!=DBSQL_OK ) __dbsql_error(db, rc);
  dbsql_mutex_leave(db->mutex);
  return rc;
}

#endif /* DBSQL_OMIT_VIRTUALTABLE */
