/*
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the implementation for TRIGGERs
*/
#include "dbsql_int.h"

#ifndef DBSQL_OMIT_TRIGGER
/*
** Delete a linked list of trigger_step_t structures.
*/
void __delete_trigger_step(DBSQL *db, trigger_step_t *trigger_step){
  while( trigger_step ){
    trigger_step_t * tmp = trigger_step;
    trigger_step = trigger_step->next;

    __expr_delete(db, tmp->where);
    __expr_list_delete(db, tmp->expr_list);
    __select_delete(db, tmp->p_select);
    __id_list_delete(db, tmp->id_list);
    __upsert_delete(db, tmp->upsert);
    __src_list_delete(db, tmp->src);
    __dbsql_db_free(db, tmp->span);

    __dbsql_db_free(db, tmp);
  }
}

/*
** Given table tab, return a list of all the triggers attached to 
** the table. The list is connected by trigger_t.next pointers.
**
** All of the triggers on tab that are in the same database as tab
** are already attached to tab->P_TRIGGER.  But there might be additional
** triggers on tab in the TEMP schema.  This routine prepends all
** TEMP triggers on tab to the beginning of the tab->P_TRIGGER list
** and returns the combined list.
**
** To state it another way:  This routine returns a list of all triggers
** that fire off of tab.  The list will include any TEMP triggers on
** tab as well as the triggers lised in tab->P_TRIGGER.
*/
trigger_t *__trigger_list(parser_t *parser, table_t *tab){
  schema_t *tmp_schema;       /* schema_t of the tab table */
  trigger_t *list;           /* List of triggers to return */
  hash_elem_t *p;              /* Loop variable for TEMP triggers */

  assert( parser->disable_triggers==0 );
  tmp_schema = parser->db->dbs[1].schema;
  p = dbsql_hash_first(&tmp_schema->trig_hash);
  list = tab->P_TRIGGER;
  while( p ){
    trigger_t *trig = (trigger_t *)dbsql_hash_data(p);
    if( trig->tab_schema==tab->schema
     && trig->table
     && 0==__str_icmp(trig->table, tab->name)
     && (trig->tab_schema!=tmp_schema || trig->returning)
    ){
      trig->next = list;
      list = trig;
    }else if( trig->op==TK_RETURNING ){
#ifndef DBSQL_OMIT_VIRTUALTABLE
      assert( parser->db->vtab_ctx==0 );
#endif
      assert( parser->returning );
      assert( !parser->is_create );
      assert( &(parser->u1.d.returning->ret_trig) == trig );
      trig->table = tab->name;
      trig->tab_schema = tab->schema;
      trig->next = list;
      list = trig;
    }        
    p = dbsql_hash_next(p);    
  }
#if 0
  if( list ){
    trigger_t *x;
    printf("Triggers for %s:", tab->name);
    for(x=list; x; x=x->next){
      printf(" %s", x->name);
    }
    printf("\n");
    fflush(stdout);
  }
#endif
  return list;  
}

/*
** This is called by the parser when it sees a CREATE TRIGGER statement
** up to the point of the BEGIN before the trigger actions.  A trigger_t
** structure is generated based on the information available and stored
** in parser->new_trigger.  After the trigger actions have been parsed, the
** __finish_trigger() function is called to complete the trigger
** construction process.
*/
void __begin_trigger(
  parser_t *parser,      /* The parse context of the CREATE TRIGGER statement */
  Token *name1,      /* The name of the trigger */
  Token *name2,      /* The name of the trigger */
  int tr_tm,          /* One of TK_BEFORE, TK_AFTER, TK_INSTEAD */
  int op,             /* One of TK_INSERT, TK_UPDATE, TK_DELETE */
  id_list_t *columns,   /* column list if this is an UPDATE OF trigger */
  src_list_t *table_name,/* The name of the table/view the trigger applies to */
  expr_t *when,        /* WHEN clause */
  int is_temp,         /* True if the TEMPORARY keyword is present */
  int no_err           /* Suppress errors if the trigger already exists */
){
  trigger_t *P_TRIGGER = 0;  /* The new trigger */
  table_t *tab;            /* table_t that the trigger fires off of */
  char *z_name = 0;        /* Name of the trigger */
  DBSQL *db = parser->db;  /* The database connection */
  int i_db;                /* The database to store the trigger in */
  Token *name;           /* The unqualified db name */
  db_fixer_t s_fix;           /* State vector for the DB fixer */

  assert( name1!=0 );   /* name1->z might be NULL, but not name1 itself */
  assert( name2!=0 );
  assert( op==TK_INSERT || op==TK_UPDATE || op==TK_DELETE );
  assert( op>0 && op<0xff );
  if( is_temp ){
    /* If TEMP was specified, then the trigger name may not be qualified. */
    if( name2->n>0 ){
      __error_msg(parser, "temporary trigger may not have qualified name");
      goto trigger_cleanup;
    }
    i_db = 1;
    name = name1;
  }else{
    /* Figure out the db that the trigger will be created in */
    i_db = __two_part_name(parser, name1, name2, &name);
    if( i_db<0 ){
      goto trigger_cleanup;
    }
  }
  if( !table_name || db->malloc_failed ){
    goto trigger_cleanup;
  }

  /* A long-standing parser bug is that this syntax was allowed:
  **
  **    CREATE TRIGGER attached.demo AFTER INSERT ON attached.tab ....
  **                                                 ^^^^^^^^
  **
  ** To maintain backwards compatibility, ignore the database
  ** name on table_name if we are reparsing out of the schema table
  */
  if( db->init.busy && i_db!=1 ){
    assert( table_name->a[0].fg.fixed_schema==0 );
    assert( table_name->a[0].fg.is_subquery==0 );
    __dbsql_db_free(db, table_name->a[0].u4.database);
    table_name->a[0].u4.database = 0;
  }

  /* If the trigger name was unqualified, and the table is a temp table,
  ** then set i_db to 1 to create the trigger in the temporary database.
  ** If __src_list_lookup() returns 0, indicating the table does not
  ** exist, the error is caught by the block below.
  */
  tab = __src_list_lookup(parser, table_name);
  if( db->init.busy==0 && name2->n==0 && tab
        && tab->schema==db->dbs[1].schema ){
    i_db = 1;
  }

  /* Ensure the table name matches database name and that the table exists */
  if( db->malloc_failed ) goto trigger_cleanup;
  assert( table_name->n_src==1 );
  __fix_init(&s_fix, parser, i_db, "trigger", name);
  if( __fix_src_list(&s_fix, table_name) ){
    goto trigger_cleanup;
  }
  tab = __src_list_lookup(parser, table_name);
  if( !tab ){
    /* The table does not exist. */
    goto trigger_orphan_error;
  }
  if( IS_VIRTUAL(tab) ){
    __error_msg(parser, "cannot create triggers on virtual tables");
    goto trigger_orphan_error;
  }
  if( (tab->tab_flags & TF_Shadow)!=0 && __read_only_shadow_tables(db) ){
    __error_msg(parser, "cannot create triggers on shadow tables");
    goto trigger_orphan_error;
  }

  /* Check that the trigger name is not reserved and that no trigger of the
  ** specified name exists */
  z_name = __name_from_token(db, name);
  if( z_name==0 ){
    assert( db->malloc_failed );
    goto trigger_cleanup;
  }
  if( __check_object_name(parser, z_name, "trigger", tab->name) ){
    goto trigger_cleanup;
  }
  assert( __schema_mutex_held(db, i_db, 0) );
  if( !IN_RENAME_OBJECT ){
    if( __hash_find(&(db->dbs[i_db].schema->trig_hash),z_name) ){
      if( !no_err ){
        __error_msg(parser, "trigger %T already exists", name);
      }else{
        assert( !db->init.busy );
        __code_verify_schema(parser, i_db);
        VVA_ONLY( parser->if_not_exists = 1; )
      }
      goto trigger_cleanup;
    }
  }

  /* NB: The DBSQL_ALLOW_TRIGGERS_ON_SYSTEM_TABLES compile-time option is
  ** experimental and unsupported. Do not use it unless understand the
  ** implications and you cannot get by without this capability. */
#if !defined(DBSQL_ALLOW_TRIGGERS_ON_SYSTEM_TABLES) /* Experimental */
  /* Do not create a trigger on a system table */
  if( __str_nicmp(tab->name, "dbsql_", 6)==0 ){
    __error_msg(parser, "cannot create trigger on system table");
    goto trigger_cleanup;
  }
#endif

  /* INSTEAD of triggers are only for views and views only support INSTEAD
  ** of triggers.
  */
  if( IsView(tab) && tr_tm!=TK_INSTEAD ){
    __error_msg(parser, "cannot create %s trigger on view: %S", 
        (tr_tm == TK_BEFORE)?"BEFORE":"AFTER", table_name->a);
    goto trigger_orphan_error;
  }
  if( !IsView(tab) && tr_tm==TK_INSTEAD ){
    __error_msg(parser, "cannot create INSTEAD OF"
        " trigger on table: %S", table_name->a);
    goto trigger_orphan_error;
  }

#ifndef DBSQL_OMIT_AUTHORIZATION
  if( !IN_RENAME_OBJECT ){
    int i_tab_db = __schema_to_index(db, tab->schema);
    int code = DBSQL_CREATE_TRIGGER;
    const char *z_db = db->dbs[i_tab_db].db_s_name;
    const char *db_trig = is_temp ? db->dbs[1].db_s_name : z_db;
    if( i_tab_db==1 || is_temp ) code = DBSQL_CREATE_TEMP_TRIGGER;
    if( __auth_check(parser, code, z_name, tab->name, db_trig) ){
      goto trigger_cleanup;
    }
    if( __auth_check(parser, DBSQL_INSERT, SCHEMA_TABLE(i_tab_db),0,z_db)){
      goto trigger_cleanup;
    }
  }
#endif

  /* INSTEAD OF triggers can only appear on views and BEFORE triggers
  ** cannot appear on views.  So we might as well translate every
  ** INSTEAD OF trigger into a BEFORE trigger.  It simplifies code
  ** elsewhere.
  */
  if (tr_tm == TK_INSTEAD){
    tr_tm = TK_BEFORE;
  }

  /* Build the trigger_t object */
  P_TRIGGER = (trigger_t*)__db_malloc_zero(db, sizeof(trigger_t));
  if( P_TRIGGER==0 ) goto trigger_cleanup;
  P_TRIGGER->name = z_name;
  z_name = 0;
  P_TRIGGER->table = __db_str_dup(db, table_name->a[0].name);
  P_TRIGGER->schema = db->dbs[i_db].schema;
  P_TRIGGER->tab_schema = tab->schema;
  P_TRIGGER->op = (u_int8_t)op;
  P_TRIGGER->tr_tm = tr_tm==TK_BEFORE ? TRIGGER_BEFORE : TRIGGER_AFTER;
  if( IN_RENAME_OBJECT ){
    __rename_token_remap(parser, P_TRIGGER->table, table_name->a[0].name);
    P_TRIGGER->when = when;
    when = 0;
  }else{
    P_TRIGGER->when = __expr_dup(db, when, EXPRDUP_REDUCE);
  }
  P_TRIGGER->columns = columns;
  columns = 0;
  assert( parser->new_trigger==0 );
  parser->new_trigger = P_TRIGGER;

trigger_cleanup:
  __dbsql_db_free(db, z_name);
  __src_list_delete(db, table_name);
  __id_list_delete(db, columns);
  __expr_delete(db, when);
  if( !parser->new_trigger ){
    __delete_trigger(db, P_TRIGGER);
  }else{
    assert( parser->new_trigger==P_TRIGGER );
  }
  return;

trigger_orphan_error:
  if( db->init.i_db==1 ){
    /* Ticket #3810.
    ** Normally, whenever a table is dropped, all associated triggers are
    ** dropped too.  But if a TEMP trigger is created on a non-TEMP table
    ** and the table is dropped by a different database connection, the
    ** trigger is not visible to the database connection that does the
    ** drop so the trigger cannot be dropped.  This results in an
    ** "orphaned trigger" - a trigger whose associated table is missing.
    **
    ** 2020-11-05 see also https://sqlite.org/forum/forumpost/157dc791df
    */
    db->init.orphan_trigger = 1;
  }
  goto trigger_cleanup;
}

/*
** This routine is called after all of the trigger actions have been parsed
** in order to complete the process of building the trigger.
*/
void __finish_trigger(
  parser_t *parser,          /* Parser context */
  trigger_step_t *step_list, /* The triggered program */
  Token *all             /* Token that describes the complete CREATE TRIGGER */
){
  trigger_t *trig = parser->new_trigger;   /* trigger_t being finished */
  char *name;                            /* Name of trigger */
  DBSQL *db = parser->db;               /* The database */
  db_fixer_t s_fix;                           /* Fixer object */
  int i_db;                                /* Database containing the trigger */
  Token name_token;                        /* trigger_t name for error reporting */

  parser->new_trigger = 0;
  if( NEVER(parser->n_err) || !trig ) goto triggerfinish_cleanup;
  name = trig->name;
  i_db = __schema_to_index(parser->db, trig->schema);
  assert( i_db>=00 && i_db<db->n_db );
  trig->step_list = step_list;
  while( step_list ){
    step_list->trig = trig;
    step_list = step_list->next;
  }
  __token_init(&name_token, trig->name);
  __fix_init(&s_fix, parser, i_db, "trigger", &name_token);
  if( __fix_trigger_step(&s_fix, trig->step_list) 
   || __fix_expr(&s_fix, trig->when) 
  ){
    goto triggerfinish_cleanup;
  }

#ifndef DBSQL_OMIT_ALTERTABLE
  if( IN_RENAME_OBJECT ){
    assert( !db->init.busy );
    parser->new_trigger = trig;
    trig = 0;
  }else
#endif

  /* if we are not initializing,
  ** build the dbsql_schema entry
  */
  if( !db->init.busy ){
    vdbe_t *v;
    char *z;

    /* If this is a new CREATE TABLE statement, and if shadow tables
    ** are read-only, and the trigger makes a change to a shadow table,
    ** then raise an error - do not allow the trigger to be created. */
    if( __read_only_shadow_tables(db) ){
      trigger_step_t *step;
      for(step=trig->step_list; step; step=step->next){
        if( step->src!=0
         && __shadow_table_name(db, step->src->a[0].name)
        ){
          __error_msg(parser, 
            "trigger \"%s\" may not write to shadow table \"%s\"",
            trig->name, step->src->a[0].name);
          goto triggerfinish_cleanup;
        }
      }
    }

    /* Make an entry in the dbsql_schema table */
    v = __get_vdbe(parser);
    if( v==0 ) goto triggerfinish_cleanup;
    __begin_write_operation(parser, 0, i_db);
    z = __db_str_n_dup(db, (char*)all->z, all->n);
    testcase( z==0 );
    __nested_parse(parser,
       "INSERT INTO %Q." LEGACY_SCHEMA_TABLE
       " VALUES('trigger',%Q,%Q,0,'CREATE TRIGGER %q')",
       db->dbs[i_db].db_s_name, name,
       trig->table, z);
    __dbsql_db_free(db, z);
    __change_cookie(parser, i_db);
    __vdbe_add_parse_schema_op(v, i_db,
        __m_printf(db, "type='trigger' AND name='%q'", name), 0);
  }

  if( db->init.busy ){
    trigger_t *link = trig;
    hash_t *hash = &db->dbs[i_db].schema->trig_hash;
    assert( __schema_mutex_held(db, i_db, 0) );
    assert( link!=0 );
    trig = __hash_insert(hash, name, trig);
    if( trig ){
      __oom_fault(db);
    }else if( link->schema==link->tab_schema ){
      table_t *tab;
      tab = __hash_find(&link->tab_schema->tbl_hash, link->table);
      assert( tab!=0 );
      link->next = tab->P_TRIGGER;
      tab->P_TRIGGER = link;
    }
  }

triggerfinish_cleanup:
  __delete_trigger(db, trig);
  assert( IN_RENAME_OBJECT || !parser->new_trigger );
  __delete_trigger_step(db, step_list);
}

/*
** Duplicate a range of text from an SQL statement, then convert all
** whitespace characters into ordinary space characters.
*/
static char *__trigger_span_dup(DBSQL *db, const char *start, const char *end){
  char *z = __db_span_dup(db, start, end);
  int i;
  if( z ) for(i=0; z[i]; i++) if( __isspace(z[i]) ) z[i] = ' ';
  return z;
}    

/*
** Turn a SELECT statement (that the p_select parameter points to) into
** a trigger step.  Return a pointer to a trigger_step_t structure.
**
** The parser calls this routine when it finds a SELECT statement in
** body of a TRIGGER.  
*/
trigger_step_t *__trigger_select_step(
  DBSQL *db,                /* Database connection */
  select_t *p_select,            /* The SELECT statement */
  const char *start,         /* Start of SQL text */
  const char *end            /* End of SQL text */
){
  trigger_step_t *trigger_step = __db_malloc_zero(db, sizeof(trigger_step_t));
  if( trigger_step==0 ) {
    __select_delete(db, p_select);
    return 0;
  }
  trigger_step->op = TK_SELECT;
  trigger_step->p_select = p_select;
  trigger_step->orconf = OE_Default;
  trigger_step->span = __trigger_span_dup(db, start, end);
  return trigger_step;
}

/*
** Allocate space to hold a new trigger step.  The allocated space
** holds both the trigger_step_t object and the trigger_step_t.target.z string.
**
** If an OOM error occurs, NULL is returned and db->malloc_failed is set.
*/
static trigger_step_t *__trigger_step_allocate(
  parser_t *parser,              /* Parser context */
  u_int8_t op,                      /* trigger_t opcode */
  src_list_t *tab_list,          /* Target table */
  const char *start,         /* Start of SQL text */
  const char *end            /* End of SQL text */
){
  trigger_t *p_new = parser->new_trigger;
  DBSQL *db = parser->db;
  trigger_step_t *trigger_step = 0;

  if( parser->n_err==0 ){
    if( p_new 
     && p_new->schema!=db->dbs[1].schema 
     && tab_list->a[0].u4.database 
    ){
      __error_msg(parser, 
          "qualified table names are not allowed on INSERT, UPDATE, and DELETE "
          "statements within triggers");
    }else{
      trigger_step = __db_malloc_zero(db, sizeof(trigger_step_t));
      if( trigger_step ){
        trigger_step->src = __src_list_dup(db, tab_list, EXPRDUP_REDUCE);
        trigger_step->op = op;
        trigger_step->span = __trigger_span_dup(db, start, end);
        if( trigger_step->src && IN_RENAME_OBJECT ){
          __rename_token_remap(parser, 
              trigger_step->src->a[0].name, 
              tab_list->a[0].name
          );
        }
      }
    }
  }

  __src_list_delete(db, tab_list);
  return trigger_step;
}

/*
** Build a trigger step out of an INSERT statement.  Return a pointer
** to the new trigger step.
**
** The parser calls this routine when it sees an INSERT inside the
** body of a trigger.
*/
trigger_step_t *__trigger_insert_step(
  parser_t *parser,      /* Parser */
  src_list_t *tab_list,  /* table_t to INSERT into */
  id_list_t *column,    /* List of columns in table_name to insert into */
  select_t *p_select,    /* A SELECT statement that supplies values */
  u_int8_t orconf,          /* The conflict algorithm (OE_Abort, OE_Replace, etc.) */
  upsert_t *upsert,    /* ON CONFLICT clauses for upsert */
  const char *start, /* Start of SQL text */
  const char *end    /* End of SQL text */
){
  DBSQL *db = parser->db;
  trigger_step_t *trigger_step;

  assert(p_select != 0 || db->malloc_failed);

  trigger_step = __trigger_step_allocate(parser, TK_INSERT, tab_list, start, end);
  if( trigger_step ){
    if( IN_RENAME_OBJECT ){
      trigger_step->p_select = p_select;
      p_select = 0;
    }else{
      trigger_step->p_select = __select_dup(db, p_select, EXPRDUP_REDUCE);
    }
    trigger_step->id_list = column;
    trigger_step->upsert = upsert;
    trigger_step->orconf = orconf;
    if( upsert ){
      __has_explicit_nulls(parser, upsert->upsert_target);
    }
  }else{
    testcase( column );
    __id_list_delete(db, column);
    testcase( upsert );
    __upsert_delete(db, upsert);
  }
  __select_delete(db, p_select);

  return trigger_step;
}

/*
** Construct a trigger step that implements an UPDATE statement and return
** a pointer to that trigger step.  The parser calls this routine when it
** sees an UPDATE statement inside the body of a CREATE TRIGGER.
*/
trigger_step_t *__trigger_update_step(
  parser_t *parser,          /* Parser */
  src_list_t *tab_list,   /* Name of the table to be updated */
  src_list_t *from,      /* FROM clause for an UPDATE-FROM, or NULL */
  expr_list_t *e_list,    /* The SET clause: list of column and new values */
  expr_t *where,        /* The WHERE clause */
  u_int8_t orconf,           /* The conflict algorithm. (OE_Abort, OE_Ignore, etc) */
  const char *start,  /* Start of SQL text */
  const char *end     /* End of SQL text */
){
  DBSQL *db = parser->db;
  trigger_step_t *trigger_step;

  trigger_step = __trigger_step_allocate(parser, TK_UPDATE, tab_list, start, end);
  if( trigger_step ){
    src_list_t *from_dup = 0;
    if( IN_RENAME_OBJECT ){
      trigger_step->expr_list = e_list;
      trigger_step->where = where;
      from_dup = from;
      e_list = 0;
      where = 0;
      from = 0;
    }else{
      trigger_step->expr_list = __expr_list_dup(db, e_list, EXPRDUP_REDUCE);
      trigger_step->where = __expr_dup(db, where, EXPRDUP_REDUCE);
      from_dup = __src_list_dup(db, from, EXPRDUP_REDUCE);
    }
    trigger_step->orconf = orconf;

    if( from_dup && !IN_RENAME_OBJECT){
      select_t *sub;
      Token as = {0, 0};
      sub = __select_new(parser, 0, from_dup, 0,0,0,0, SF_NestedFrom, 0);
      from_dup = __src_list_append_from_term(parser, 0, 0, 0, &as, sub ,0);
    }
    if( from_dup && trigger_step->src ){
      trigger_step->src = __src_list_append_list(
          parser, trigger_step->src, from_dup
      );
    }else{
      __src_list_delete(db, from_dup);
    }
  }
  __expr_list_delete(db, e_list);
  __expr_delete(db, where);
  __src_list_delete(db, from);
  return trigger_step;
}

/*
** Construct a trigger step that implements a DELETE statement and return
** a pointer to that trigger step.  The parser calls this routine when it
** sees a DELETE statement inside the body of a CREATE TRIGGER.
*/
trigger_step_t *__trigger_delete_step(
  parser_t *parser,          /* Parser */
  src_list_t *tab_list,      /* The table from which rows are deleted */
  expr_t *where,           /* The WHERE clause */
  const char *start,     /* Start of SQL text */
  const char *end        /* End of SQL text */
){
  DBSQL *db = parser->db;
  trigger_step_t *trigger_step;

  trigger_step = __trigger_step_allocate(parser, TK_DELETE, tab_list, start, end);
  if( trigger_step ){
    if( IN_RENAME_OBJECT ){
      trigger_step->where = where;
      where = 0;
    }else{
      trigger_step->where = __expr_dup(db, where, EXPRDUP_REDUCE);
    }
    trigger_step->orconf = OE_Default;
  }
  __expr_delete(db, where);
  return trigger_step;
}

/* 
** Recursively delete a trigger_t structure
*/
void __delete_trigger(DBSQL *db, trigger_t *P_TRIGGER){
  if( P_TRIGGER==0 || P_TRIGGER->returning ) return;
  __delete_trigger_step(db, P_TRIGGER->step_list);
  __dbsql_db_free(db, P_TRIGGER->name);
  __dbsql_db_free(db, P_TRIGGER->table);
  __expr_delete(db, P_TRIGGER->when);
  __id_list_delete(db, P_TRIGGER->columns);
  __dbsql_db_free(db, P_TRIGGER);
}

/*
** This function is called to drop a trigger from the database schema. 
**
** This may be called directly from the parser and therefore identifies
** the trigger by name.  The __drop_trigger_ptr() routine does the
** same job as this routine except it takes a pointer to the trigger
** instead of the trigger name.
**/
void __drop_trigger(parser_t *parser, src_list_t *name, int no_err){
  trigger_t *P_TRIGGER = 0;
  int i;
  const char *z_db;
  const char *z_name;
  DBSQL *db = parser->db;

  if( db->malloc_failed ) goto drop_trigger_cleanup;
  if( DBSQL_OK!=__read_schema(parser) ){
    goto drop_trigger_cleanup;
  }

  assert( name->n_src==1 );
  assert( name->a[0].fg.fixed_schema==0 && name->a[0].fg.is_subquery==0 );
  z_db = name->a[0].u4.database;
  z_name = name->a[0].name;
  assert( z_db!=0 || __sm_holds_all_mutexes(db) );
  for(i=OMIT_TEMPDB; i<db->n_db; i++){
    int j = (i<2) ? i^1 : i;  /* Search TEMP before MAIN */
    if( z_db && __db_is_named(db, j, z_db)==0 ) continue;
    assert( __schema_mutex_held(db, j, 0) );
    P_TRIGGER = __hash_find(&(db->dbs[j].schema->trig_hash), z_name);
    if( P_TRIGGER ) break;
  }
  if( !P_TRIGGER ){
    if( !no_err ){
      __error_msg(parser, "no such trigger: %S", name->a);
    }else{
      __code_verify_named_schema(parser, z_db);
    }
    parser->check_schema = 1;
    goto drop_trigger_cleanup;
  }
  __drop_trigger_ptr(parser, P_TRIGGER);

drop_trigger_cleanup:
  __src_list_delete(db, name);
}

/*
** Return a pointer to the table_t structure for the table that a trigger
** is set on.
*/
static table_t *__table_of_trigger(trigger_t *P_TRIGGER){
  return __hash_find(&P_TRIGGER->tab_schema->tbl_hash, P_TRIGGER->table);
}


/*
** Drop a trigger given a pointer to that trigger. 
*/
void __drop_trigger_ptr(parser_t *parser, trigger_t *P_TRIGGER){
  table_t   *table;
  vdbe_t *v;
  DBSQL *db = parser->db;
  int i_db;

  i_db = __schema_to_index(parser->db, P_TRIGGER->schema);
  assert( i_db>=0 && i_db<db->n_db );
  table = __table_of_trigger(P_TRIGGER);
  assert( (table && table->schema==P_TRIGGER->schema) || i_db==1 );
#ifndef DBSQL_OMIT_AUTHORIZATION
  if( table ){
    int code = DBSQL_DROP_TRIGGER;
    const char *z_db = db->dbs[i_db].db_s_name;
    const char *tab = SCHEMA_TABLE(i_db);
    if( i_db==1 ) code = DBSQL_DROP_TEMP_TRIGGER;
    if( __auth_check(parser, code, P_TRIGGER->name, table->name, z_db) ||
      __auth_check(parser, DBSQL_DELETE, tab, 0, z_db) ){
      return;
    }
  }
#endif

  /* Generate code to destroy the database record of the trigger.
  */
  if( (v = __get_vdbe(parser))!=0 ){
    __nested_parse(parser,
       "DELETE FROM %Q." LEGACY_SCHEMA_TABLE " WHERE name=%Q AND type='trigger'",
       db->dbs[i_db].db_s_name, P_TRIGGER->name
    );
    __change_cookie(parser, i_db);
    __vdbe_add_op4(v, OP_DropTrigger, i_db, 0, 0, P_TRIGGER->name, 0);
  }
}

/*
** Remove a trigger from the hash tables of the sqlite* pointer.
*/
void __unlink_and_delete_trigger(DBSQL *db, int i_db, const char *name){
  trigger_t *P_TRIGGER;
  hash_t *hash;

  assert( __schema_mutex_held(db, i_db, 0) );
  hash = &(db->dbs[i_db].schema->trig_hash);
  P_TRIGGER = __hash_insert(hash, name, 0);
  if( ALWAYS(P_TRIGGER) ){
    if( P_TRIGGER->schema==P_TRIGGER->tab_schema ){
      table_t *tab = __table_of_trigger(P_TRIGGER);
      if( tab ){
        trigger_t **pp;
        for(pp=&tab->P_TRIGGER; *pp; pp=&((*pp)->next)){
          if( *pp==P_TRIGGER ){
            *pp = (*pp)->next;
            break;
          }
        }
      }
    }
    __delete_trigger(db, P_TRIGGER);
    db->m_db_flags |= DBFLAG_SchemaChange;
  }
}

/*
** e_list is the SET clause of an UPDATE statement.  Each entry
** in e_list is of the format <id>=<expr>.  If any of the entries
** in e_list have an <id> which matches an identifier in id_list,
** then return TRUE.  If id_list==NULL, then it is considered a
** wildcard that matches anything.  Likewise if e_list==NULL then
** it matches anything so always return true.  Return false only
** if there is no match.
*/
static int __check_column_overlap(id_list_t *id_list, expr_list_t *e_list){
  int e;
  if( id_list==0 || NEVER(e_list==0) ) return 1;
  for(e=0; e<e_list->n_expr; e++){
    if( __id_list_index(id_list, e_list->a[e].e_name)>=0 ) return 1;
  }
  return 0; 
}

/*
** Return true if any TEMP triggers exist
*/
static int __temp_triggers_exist(DBSQL *db){
  if( NEVER(db->dbs[1].schema==0) ) return 0;
  if( dbsql_hash_first(&db->dbs[1].schema->trig_hash)==0 ) return 0;
  return 1;
}

/*
** Return a list of all triggers on table tab if there exists at least
** one trigger that must be fired when an operation of type 'op' is 
** performed on the table, and, if that operation is an UPDATE, if at
** least one of the columns in changes is being modified.
*/
static DBSQL_NOINLINE trigger_t *__triggers_really_exist(
  parser_t *parser,          /* parser_t context */
  table_t *tab,            /* The table the contains the triggers */
  int op,                 /* one of TK_DELETE, TK_INSERT, TK_UPDATE */
  expr_list_t *changes,     /* Columns that change in an UPDATE statement */
  int *p_mask              /* OUT: Mask of TRIGGER_BEFORE|TRIGGER_AFTER */
){
  int mask = 0;
  trigger_t *list = 0;
  trigger_t *p;

  list = __trigger_list(parser, tab);
  assert( list==0 || IS_VIRTUAL(tab)==0 
           || (list->returning && list->next==0) );
  if( list!=0 ){
    p = list;
    if( (parser->db->flags & DBSQL_EnableTrigger)==0
     && tab->P_TRIGGER!=0
     && __schema_to_index(parser->db, tab->P_TRIGGER->schema)!=1
    ){
      /* The DBSQL_DBCONFIG_ENABLE_TRIGGER setting is off.  That means that
      ** only TEMP triggers are allowed.  Truncate the list so that it
      ** includes only TEMP triggers */
      if( list==tab->P_TRIGGER ){
        list = 0;
        goto exit_triggers_exist;
      }
      while( ALWAYS(p->next) && p->next!=tab->P_TRIGGER ) p = p->next;
      p->next = 0;
      p = list;
    }
    do{
      if( p->op==op && __check_column_overlap(p->columns, changes) ){
        mask |= p->tr_tm;
      }else if( p->op==TK_RETURNING ){
        /* The first time a RETURNING trigger is seen, the "op" value tells
        ** us what time of trigger it should be. */
        assert( __is_toplevel(parser) );
        p->op = op;
        if( IS_VIRTUAL(tab) ){
          if( op!=TK_INSERT ){
            __error_msg(parser,
              "%s RETURNING is not available on virtual tables",
              op==TK_DELETE ? "DELETE" : "UPDATE");
          }
          p->tr_tm = TRIGGER_BEFORE;
        }else{
          p->tr_tm = TRIGGER_AFTER;
        }
        mask |= p->tr_tm;
      }else if( p->returning && p->op==TK_INSERT && op==TK_UPDATE
                && __is_toplevel(parser) ){
        /* Also fire a RETURNING trigger for an UPSERT */
        mask |= p->tr_tm;
      }
      p = p->next;
    }while( p );
  }
exit_triggers_exist:
  if( p_mask ){
    *p_mask = mask;
  }
  return (mask ? list : 0);
}
trigger_t *__triggers_exist(
  parser_t *parser,          /* parser_t context */
  table_t *tab,            /* The table the contains the triggers */
  int op,                 /* one of TK_DELETE, TK_INSERT, TK_UPDATE */
  expr_list_t *changes,     /* Columns that change in an UPDATE statement */
  int *mask              /* OUT: Mask of TRIGGER_BEFORE|TRIGGER_AFTER */
){
  assert( tab!=0 );
  if( (tab->P_TRIGGER==0 && !__temp_triggers_exist(parser->db))
   || parser->disable_triggers
  ){
    if( mask ) *mask = 0;
    return 0;
  }
  return __triggers_really_exist(parser,tab,op,changes,mask);
}

/*
** Return true if the expr term from the RETURNING clause argument
** list is of the form "*".  Raise an error if the terms if of the
** form "table.*".
*/
static int __is_asterisk_term(
  parser_t *parser,      /* Parsing context */
  expr_t *term         /* A term in the RETURNING clause */
){
  assert( term!=0 );
  if( term->op==TK_ASTERISK ) return 1;
  if( term->op!=TK_DOT ) return 0;
  assert( term->right!=0 );
  assert( term->left!=0 );
  if( term->right->op!=TK_ASTERISK ) return 0;
  __error_msg(parser, "RETURNING may not use \"TABLE.*\" wildcards");
  return 1;
}

/* The input list list is the list of result set terms from a RETURNING
** clause.  The table that we are returning from is tab.
**
** This routine makes a copy of the list, and at the same time expands
** any "*" wildcards to be the complete set of columns from tab.
*/
static expr_list_t *__expand_returning(
  parser_t *parser,        /* Parsing context */
  expr_list_t *list,      /* The arguments to RETURNING */
  table_t *tab           /* The table being updated */
){
  expr_list_t *p_new = 0;
  DBSQL *db = parser->db;
  int i;

  for(i=0; i<list->n_expr; i++){
    expr_t *old_expr = list->a[i].expr;
    if( NEVER(old_expr==0) ) continue;
    if( __is_asterisk_term(parser, old_expr) ){
      int jj;
      for(jj=0; jj<tab->n_col; jj++){
        expr_t *new_expr;
        if( IS_HIDDEN_COLUMN(tab->cols+jj) ) continue;
        new_expr = __expr(db, TK_ID, tab->cols[jj].cn_name);
        p_new = __expr_list_append(parser, p_new, new_expr);
        if( !db->malloc_failed ){
          struct expr_list_item_t *item = &p_new->a[p_new->n_expr-1];
          item->e_name = __db_str_dup(db, tab->cols[jj].cn_name);
          item->fg.e_name = ENAME_NAME;
        }
      }
    }else{
      expr_t *new_expr = __expr_dup(db, old_expr, 0);
      p_new = __expr_list_append(parser, p_new, new_expr);
      if( !db->malloc_failed && ALWAYS(list->a[i].e_name!=0) ){
        struct expr_list_item_t *item = &p_new->a[p_new->n_expr-1];
        item->e_name = __db_str_dup(db, list->a[i].e_name);
        item->fg.e_name = list->a[i].fg.e_name;
      }
    }
  }
  return p_new;
}

/* If the expr_t node is a subquery or an EXISTS operator or an IN operator that
** uses a subquery, and if the subquery is SF_Correlated, then mark the
** expression as EP_VarSelect.
*/
static int __returning_subquery_var_select(walker_t *__not_used, expr_t *expr){
  UNUSED_PARAMETER(__not_used);
  if( EXPR_USE_X_SELECT(expr)
   && (expr->x.p_select->sel_flags & SF_Correlated)!=0
  ){
    testcase( EXPR_HAS_PROPERTY(expr, EP_VarSelect) );
    EXPR_SET_PROPERTY(expr, EP_VarSelect);
  }
  return WRC_Continue;
}


/*
** If the SELECT references the table walker->u.tab, then do two things:
**
**    (1) Mark the SELECT as as SF_Correlated.
**    (2) Set walker->code to non-zero so that the caller will know
**        that (1) has happened.
*/
static int __returning_subquery_correlated(walker_t *walker, select_t *p_select){
  int i;
  src_list_t *src;
  assert( p_select!=0 );
  src = p_select->src;
  assert( src!=0 );
  for(i=0; i<src->n_src; i++){
    if( src->a[i].s_tab==walker->u.tab ){
      testcase( p_select->sel_flags & SF_Correlated );
      p_select->sel_flags |= SF_Correlated;
      walker->code = 1;
      break;
    }
  }
  return WRC_Continue;
}

/*
** Scan the expression list that is the argument to RETURNING looking
** for subqueries that depend on the table which is being modified in the
** statement that is hosting the RETURNING clause (tab).  Mark all such
** subqueries as SF_Correlated.  If the subqueries are part of an
** expression, mark the expression as EP_VarSelect.
**
** https://sqlite.org/forum/forumpost/2c83569ce8945d39
*/
static void __process_returning_subqueries(
  expr_list_t *e_list,
  table_t *tab
){
  walker_t w;
  memset(&w, 0, sizeof(w));
  w.expr_callback = __expr_walk_noop;
  w.select_callback = __returning_subquery_correlated;
  w.u.tab = tab;
  __walk_expr_list(&w, e_list);
  if( w.code ){
    w.expr_callback = __returning_subquery_var_select;
    w.select_callback = __select_walk_noop;
    __walk_expr_list(&w, e_list);
  }
}

/*
** Generate code for the RETURNING trigger.  Unlike other triggers
** that invoke a subprogram in the bytecode, the code for RETURNING
** is generated in-line.
*/
static void __code_returning_trigger(
  parser_t *parser,       /* parser_t context */
  trigger_t *P_TRIGGER,   /* The trigger step that defines the RETURNING */
  table_t *tab,         /* The table to code triggers from */
  int reg_in            /* The first in an array of registers */
){
  vdbe_t *v = parser->vdbe;
  DBSQL *db = parser->db;
  expr_list_t *p_new;
  returning_t *returning;
  select_t s_select;
  src_list_t *from;
  union {
    src_list_t s_src;
    u_int8_t from_space[SZ_SRCLIST_1];
  } u_src;

  assert( v!=0 );
  if( !parser->returning ){
    /* This RETURNING trigger must be for a different statement as
    ** this statement lacks a RETURNING clause. */
    return;
  }
  assert( db->parser==parser );
  assert( !parser->is_create );
  returning = parser->u1.d.returning;
  if( P_TRIGGER != &(returning->ret_trig) ){
    /* This RETURNING trigger is for a different statement */
    return;
  }
  memset(&s_select, 0, sizeof(s_select));
  memset(&u_src, 0, sizeof(u_src));
  from = &u_src.s_src;
  s_select.e_list = __expr_list_dup(db, returning->return_el, 0);
  s_select.src = from;
  from->n_src = 1;
  from->a[0].s_tab = tab;
  from->a[0].name = tab->name; /* tag-20240424-1 */
  from->a[0].i_cursor = -1;
  __select_prep(parser, &s_select, 0);
  if( parser->n_err==0 ){
    assert( db->malloc_failed==0 );
    __generate_column_names(parser, &s_select);
  }
  __expr_list_delete(db, s_select.e_list);
  p_new = __expand_returning(parser, returning->return_el, tab);
  if( parser->n_err==0 ){
    name_context_t s_nc;
    memset(&s_nc, 0, sizeof(s_nc));
    if( returning->n_ret_col==0 ){
      returning->n_ret_col = p_new->n_expr;
      returning->i_ret_cur = parser->n_tab++;
    }
    s_nc.parser = parser;
    s_nc.u_nc.i_base_reg = reg_in;
    s_nc.nc_flags = NC_UBaseReg;
    parser->trigger_op = P_TRIGGER->op;
    parser->trigger_tab = tab;
    if( __resolve_expr_list_names(&s_nc, p_new)==DBSQL_OK
     && ALWAYS(!db->malloc_failed)
    ){
      int i;
      int n_col = p_new->n_expr;
      int reg = parser->n_mem+1;
      __process_returning_subqueries(p_new, tab);
      parser->n_mem += n_col+2;
      returning->i_ret_reg = reg;
      for(i=0; i<n_col; i++){
        expr_t *col = p_new->a[i].expr;
        assert( col!=0 ); /* Due to !db->malloc_failed ~9 lines above */
        __expr_code_factorable(parser, col, reg+i);
        if( __expr_affinity(col)==DBSQL_AFF_REAL ){
          __vdbe_add_op1(v, OP_RealAffinity, reg+i);
        }
      }
      __vdbe_add_op3(v, OP_MakeRecord, reg, i, reg+i);
      __vdbe_add_op2(v, OP_NewRowid, returning->i_ret_cur, reg+i+1);
      __vdbe_add_op3(v, OP_Insert, returning->i_ret_cur, reg+i, reg+i+1);
    }
  }
  __expr_list_delete(db, p_new);
  parser->trigger_op = 0;
  parser->trigger_tab = 0;
}



/*
** Generate VDBE code for the statements inside the body of a single 
** trigger.
*/
static int __code_trigger_program(
  parser_t *parser,            /* The parser context */
  trigger_step_t *step_list,   /* List of statements inside the trigger body */
  int orconf                /* Conflict algorithm. (OE_Abort, etc) */  
){
  trigger_step_t *step;
  vdbe_t *v = parser->vdbe;
  DBSQL *db = parser->db;

  assert( parser->trigger_tab && parser->toplevel );
  assert( step_list );
  assert( v!=0 );
  for(step=step_list; step; step=step->next){
    /* Figure out the ON CONFLICT policy that will be used for this step
    ** of the trigger program. If the statement that caused this trigger
    ** to fire had an explicit ON CONFLICT, then use it. Otherwise, use
    ** the ON CONFLICT policy that was specified as part of the trigger
    ** step statement. Example:
    **
    **   CREATE TRIGGER AFTER INSERT ON t1 BEGIN;
    **     INSERT OR REPLACE INTO t2 VALUES(new.a, new.b);
    **   END;
    **
    **   INSERT INTO t1 ... ;            -- insert into t2 uses REPLACE policy
    **   INSERT OR IGNORE INTO t1 ... ;  -- insert into t2 uses IGNORE policy
    */
    parser->orconf = (orconf==OE_Default)?step->orconf:(u_int8_t)orconf;
    assert( parser->ok_const_factor==0 );

#ifndef DBSQL_OMIT_TRACE
    if( step->span ){
      __vdbe_add_op4(v, OP_Trace, 0x7fffffff, 1, 0,
                        __m_printf(db, "-- %s", step->span),
                        P4_DYNAMIC);
    }
#endif

    switch( step->op ){
      case TK_UPDATE: {
        __update(parser, 
          __src_list_dup(db, step->src, 0),
          __expr_list_dup(db, step->expr_list, 0), 
          __expr_dup(db, step->where, 0), 
          parser->orconf, 0, 0, 0
        );
        __vdbe_add_op0(v, OP_ResetCount);
        break;
      }
      case TK_INSERT: {
        __insert(parser, 
          __src_list_dup(db, step->src, 0),
          __select_dup(db, step->p_select, 0), 
          __id_list_dup(db, step->id_list), 
          parser->orconf,
          __upsert_dup(db, step->upsert)
        );
        __vdbe_add_op0(v, OP_ResetCount);
        break;
      }
      case TK_DELETE: {
        __delete_from(parser, 
          __src_list_dup(db, step->src, 0),
          __expr_dup(db, step->where, 0), 0, 0
        );
        __vdbe_add_op0(v, OP_ResetCount);
        break;
      }
      default: assert( step->op==TK_SELECT ); {
        select_dest_t s_dest;
        select_t *p_select = __select_dup(db, step->p_select, 0);
        __select_dest_init(&s_dest, SRT_Discard, 0);
        __select(parser, p_select, &s_dest);
        __select_delete(db, p_select);
        break;
      }
    } 
  }

  return 0;
}

#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
/*
** This function is used to add VDBE_COMMENT() annotations to a VDBE
** program. It is not used in production code, only for debugging.
*/
static const char *__on_error_text(int on_error){
  switch( on_error ){
    case OE_Abort:    return "abort";
    case OE_Rollback: return "rollback";
    case OE_Fail:     return "fail";
    case OE_Replace:  return "replace";
    case OE_Ignore:   return "ignore";
    case OE_Default:  return "default";
  }
  return "n/a";
}
#endif

/*
** parser_t context structure from has just been used to create a sub-vdbe
** (trigger program). If an error has occurred, transfer error information
** from from to to.
*/
static void __transfer_parse_error(parser_t *to, parser_t *from){
  assert( from->errmsg==0 || from->n_err );
  assert( to->errmsg==0 || to->n_err );
  if( to->n_err==0 ){
    to->errmsg = from->errmsg;
    to->n_err = from->n_err;
    to->rc = from->rc;
  }else{
    __dbsql_db_free(from->db, from->errmsg);
  }
}

/*
** Create and populate a new trigger_prg_t object with a sub-program 
** implementing trigger P_TRIGGER with ON CONFLICT policy orconf.
*/
static trigger_prg_t *__code_row_trigger_local(
  parser_t *parser,       /* Current parse context */
  trigger_t *P_TRIGGER,   /* trigger_t to code */
  table_t *tab,         /* The table P_TRIGGER is attached to */
  int orconf           /* ON CONFLICT policy to code trigger program with */
){
  parser_t *top;                /* Top level parser_t object */
  DBSQL *db = parser->db;   /* Database handle */
  trigger_prg_t *prg;           /* Value to return */
  expr_t *when = 0;            /* Duplicate of trigger WHEN expression */
  vdbe_t *v;                    /* Temporary VM */
  name_context_t s_nc;            /* Name context for sub-vdbe */
  sub_program_t *program = 0;   /* Sub-vdbe for trigger program */
  int i_end_trigger = 0;        /* Label to jump to if WHEN is false */
  parser_t s_sub_parse;            /* parser_t context for sub-vdbe */
  int n_depth;                 /* trigger_t depth */

  /* Ensure that triggers are not chained too deep.  This test is linear
  ** in the chaining depth, but sensible code ought not be chaining
  ** triggers excessively, so that shouldn't be a problem.
  */  
  top = parser;
  for(n_depth=0; top->outer_parse; top = top->outer_parse, n_depth++){}
  if( n_depth>=db->limits[DBSQL_LIMIT_TRIGGER_DEPTH] ){
    __error_msg(parser, "triggers nested too deep");
    return 0;
  }

  top = __parse_toplevel(parser);
  assert( P_TRIGGER->name==0 || tab==__table_of_trigger(P_TRIGGER) );
  assert( top->vdbe );


  /* Allocate the trigger_prg_t and sub_program_t objects. To ensure that they
  ** are freed if an error occurs, link them into the parser_t.trigger_prg 
  ** list of the top-level parser_t object sooner rather than later.  */
  prg = __db_malloc_zero(db, sizeof(trigger_prg_t));
  if( !prg ) return 0;
  prg->next = top->trigger_prg;
  top->trigger_prg = prg;
  prg->program = program = __db_malloc_zero(db, sizeof(sub_program_t));
  if( !program ) return 0;
  __vdbe_link_sub_program(top->vdbe, program);
  prg->P_TRIGGER = P_TRIGGER;
  prg->orconf = orconf;
  prg->colmasks[0] = 0xffffffff;
  prg->colmasks[1] = 0xffffffff;

  /* Allocate and populate a new parser_t context to use for coding the 
  ** trigger sub-program.  */
  __parse_object_init(&s_sub_parse, db);
  memset(&s_nc, 0, sizeof(s_nc));
  s_nc.parser = &s_sub_parse;
  s_sub_parse.trigger_tab = tab;
  s_sub_parse.toplevel = top;
  s_sub_parse.auth_context = P_TRIGGER->name;
  s_sub_parse.trigger_op = P_TRIGGER->op;
  s_sub_parse.n_query_loop = parser->n_query_loop;
  s_sub_parse.prep_flags = parser->prep_flags;
  s_sub_parse.oldmask = 0;
  s_sub_parse.newmask = 0;

  v = __get_vdbe(&s_sub_parse);
  if( v ){
    VDBE_COMMENT((v, "Start: %s.%s (%s %s%s%s ON %s)", 
      P_TRIGGER->name, __on_error_text(orconf),
      (P_TRIGGER->tr_tm==TRIGGER_BEFORE ? "BEFORE" : "AFTER"),
        (P_TRIGGER->op==TK_UPDATE ? "UPDATE" : ""),
        (P_TRIGGER->op==TK_INSERT ? "INSERT" : ""),
        (P_TRIGGER->op==TK_DELETE ? "DELETE" : ""),
      tab->name
    ));
#ifndef DBSQL_OMIT_TRACE
    if( P_TRIGGER->name ){
      __vdbe_change_p4(v, -1, 
        __m_printf(db, "-- TRIGGER %s", P_TRIGGER->name), P4_DYNAMIC
      );
    }
#endif

    /* If one was specified, code the WHEN clause. If it evaluates to false
    ** (or NULL) the sub-vdbe is immediately halted by jumping to the 
    ** OP_Halt inserted at the end of the program.  */
    if( P_TRIGGER->when ){
      when = __expr_dup(db, P_TRIGGER->when, 0);
      if( db->malloc_failed==0
       && DBSQL_OK==__resolve_expr_names(&s_nc, when) 
      ){
        i_end_trigger = __vdbe_make_label(&s_sub_parse);
        __expr_if_false(&s_sub_parse, when, i_end_trigger, DBSQL_JUMPIFNULL);
      }
      __expr_delete(db, when);
    }

    /* Code the trigger program into the sub-vdbe. */
    __code_trigger_program(&s_sub_parse, P_TRIGGER->step_list, orconf);

    /* Insert an OP_Halt at the end of the sub-program. */
    if( i_end_trigger ){
      __vdbe_resolve_label(v, i_end_trigger);
    }
    __vdbe_add_op0(v, OP_Halt);
    VDBE_COMMENT((v, "End: %s.%s", P_TRIGGER->name, __on_error_text(orconf)));
    __transfer_parse_error(parser, &s_sub_parse);

    if( parser->n_err==0 ){
      assert( db->malloc_failed==0 );
      program->ops = __vdbe_take_op_array(v, &program->n_op, &top->n_max_arg);
    }
    program->n_mem = s_sub_parse.n_mem;
    program->n_csr = s_sub_parse.n_tab;
    program->token = (void *)P_TRIGGER;
    prg->colmasks[0] = s_sub_parse.oldmask;
    prg->colmasks[1] = s_sub_parse.newmask;
    __vdbe_delete(v);
  }else{
    __transfer_parse_error(parser, &s_sub_parse);
  }

  assert( !s_sub_parse.trigger_prg && !s_sub_parse.n_max_arg );
  __parse_object_reset(&s_sub_parse);
  return prg;
}
    
/*
** Return a pointer to a trigger_prg_t object containing the sub-program for
** trigger P_TRIGGER with default ON CONFLICT algorithm orconf. If no such
** trigger_prg_t object exists, a new object is allocated and populated before
** being returned.
*/
static trigger_prg_t *__get_row_trigger(
  parser_t *parser,       /* Current parse context */
  trigger_t *P_TRIGGER,   /* trigger_t to code */
  table_t *tab,         /* The table trigger P_TRIGGER is attached to */
  int orconf           /* ON CONFLICT algorithm. */
){
  parser_t *root = __parse_toplevel(parser);
  trigger_prg_t *prg;

  assert( P_TRIGGER->name==0 || tab==__table_of_trigger(P_TRIGGER) );

  /* It may be that this trigger has already been coded (or is in the
  ** process of being coded). If this is the case, then an entry with
  ** a matching trigger_prg_t.P_TRIGGER field will be present somewhere
  ** in the parser_t.trigger_prg list. Search for such an entry.  */
  for(prg=root->trigger_prg; 
      prg && (prg->P_TRIGGER!=P_TRIGGER || prg->orconf!=orconf); 
      prg=prg->next
  );

  /* If an existing trigger_prg_t could not be located, create a new one. */
  if( !prg ){
    prg = __code_row_trigger_local(parser, P_TRIGGER, tab, orconf);
    parser->db->err_byte_offset = -1;
  }

  return prg;
}

/*
** Generate code for the trigger program associated with trigger p on 
** table tab. The reg, orconf and ignore_jump parameters passed to this
** function are the same as those described in the header function for
** __code_row_trigger()
*/
void __code_row_trigger_direct(
  parser_t *parser,       /* parser_t context */
  trigger_t *p,          /* trigger_t to code */
  table_t *tab,         /* The table to code triggers from */
  int reg,             /* Reg array containing OLD.* and NEW.* values */
  int orconf,          /* ON CONFLICT policy */
  int ignore_jump       /* Instruction to jump to for RAISE(IGNORE) */
){
  vdbe_t *v = __get_vdbe(parser); /* Main VM */
  trigger_prg_t *prg;
  prg = __get_row_trigger(parser, p, tab, orconf);
  assert( prg || parser->n_err );

  /* Code the OP_Program opcode in the parent VDBE. P4 of the OP_Program 
  ** is a pointer to the sub-vdbe containing the trigger program.  */
  if( prg ){
    int recursive = (p->name && 0==(parser->db->flags&DBSQL_RecTriggers));

    __vdbe_add_op4(v, OP_Program, reg, ignore_jump, ++parser->n_mem,
                      (const char *)prg->program, P4_SUBPROGRAM);
    VDBE_COMMENT(
        (v, "Call: %s.%s", (p->name?p->name:"fkey"), __on_error_text(orconf)));

    /* Set the P5 operand of the OP_Program instruction to non-zero if
    ** recursive invocation of this trigger program is disallowed. Recursive
    ** invocation is disallowed if (a) the sub-program is really a trigger,
    ** not a foreign key action, and (b) the flag to enable recursive triggers
    ** is clear.  */
    __vdbe_change_p5(v, (u_int16_t)recursive);
  }
}

/*
** This is called to code the required FOR EACH ROW triggers for an operation
** on table tab. The operation to code triggers for (INSERT, UPDATE or DELETE)
** is given by the op parameter. The tr_tm parameter determines whether the
** BEFORE or AFTER triggers are coded. If the operation is an UPDATE, then
** parameter changes is passed the list of columns being modified.
**
** If there are no triggers that fire at the specified time for the specified
** operation on tab, this function is a no-op.
**
** The reg argument is the address of the first in an array of registers 
** that contain the values substituted for the new.* and old.* references
** in the trigger program. If N is the number of columns in table tab
** (a copy of tab->n_col), then registers are populated as follows:
**
**   Register       Contains
**   ------------------------------------------------------
**   reg+0          OLD.rowid
**   reg+1          OLD.* value of left-most column of tab
**   ...            ...
**   reg+N          OLD.* value of right-most column of tab
**   reg+N+1        NEW.rowid
**   reg+N+2        NEW.* value of left-most column of tab
**   ...            ...
**   reg+N+N+1      NEW.* value of right-most column of tab
**
** For ON DELETE triggers, the registers containing the NEW.* values will
** never be accessed by the trigger program, so they are not allocated or 
** populated by the caller (there is no data to populate them with anyway). 
** Similarly, for ON INSERT triggers the values stored in the OLD.* registers
** are never accessed, and so are not allocated by the caller. So, for an
** ON INSERT trigger, the value passed to this function as parameter reg
** is not a readable register, although registers (reg+N) through 
** (reg+N+N+1) are.
**
** Parameter orconf is the default conflict resolution algorithm for the
** trigger program to use (REPLACE, IGNORE etc.). Parameter ignore_jump
** is the instruction that control should jump to if a trigger program
** raises an IGNORE exception.
*/
void __code_row_trigger(
  parser_t *parser,       /* parser_t context */
  trigger_t *P_TRIGGER,   /* List of triggers on table tab */
  int op,              /* One of TK_UPDATE, TK_INSERT, TK_DELETE */
  expr_list_t *changes,  /* Changes list for any UPDATE OF triggers */
  int tr_tm,           /* One of TRIGGER_BEFORE, TRIGGER_AFTER */
  table_t *tab,         /* The table to code triggers from */
  int reg,             /* The first in an array of registers (see above) */
  int orconf,          /* ON CONFLICT policy */
  int ignore_jump       /* Instruction to jump to for RAISE(IGNORE) */
){
  trigger_t *p;          /* Used to iterate through P_TRIGGER list */

  assert( op==TK_UPDATE || op==TK_INSERT || op==TK_DELETE );
  assert( tr_tm==TRIGGER_BEFORE || tr_tm==TRIGGER_AFTER );
  assert( (op==TK_UPDATE)==(changes!=0) );

  for(p=P_TRIGGER; p; p=p->next){

    /* Sanity checking:  The schema for the trigger and for the table are
    ** always defined.  The trigger must be in the same schema as the table
    ** or else it must be a TEMP trigger. */
    assert( p->schema!=0 );
    assert( p->tab_schema!=0 );
    assert( p->schema==p->tab_schema 
         || p->schema==parser->db->dbs[1].schema );

    /* Determine whether we should code this trigger.  One of two choices:
    **   1. The trigger is an exact match to the current DML statement
    **   2. This is a RETURNING trigger for INSERT but we are currently
    **      doing the UPDATE part of an UPSERT.
    */
    if( (p->op==op || (p->returning && p->op==TK_INSERT && op==TK_UPDATE))
     && p->tr_tm==tr_tm 
     && __check_column_overlap(p->columns, changes)
    ){
      if( !p->returning ){
        __code_row_trigger_direct(parser, p, tab, reg, orconf, ignore_jump);
      }else if( __is_toplevel(parser) ){
        __code_returning_trigger(parser, p, tab, reg);
      }
    }
  }
}

/*
** Triggers may access values stored in the old.* or new.* pseudo-table. 
** This function returns a 32-bit bitmask indicating which columns of the 
** old.* or new.* tables actually are used by triggers. This information 
** may be used by the caller, for example, to avoid having to load the entire
** old.* record into memory when executing an UPDATE or DELETE command.
**
** Bit 0 of the returned mask is set if the left-most column of the
** table may be accessed using an [old|new].<col> reference. Bit 1 is set if
** the second leftmost column value is required, and so on. If there
** are more than 32 columns in the table, and at least one of the columns
** with an index greater than 32 may be accessed, 0xffffffff is returned.
**
** It is not possible to determine if the old.rowid or new.rowid column is 
** accessed by triggers. The caller must always assume that it is.
**
** Parameter is_new must be either 1 or 0. If it is 0, then the mask returned
** applies to the old.* table. If 1, the new.* table.
**
** Parameter tr_tm must be a mask with one or both of the TRIGGER_BEFORE
** and TRIGGER_AFTER bits set. Values accessed by BEFORE triggers are only
** included in the returned mask if the TRIGGER_BEFORE bit is set in the
** tr_tm parameter. Similarly, values accessed by AFTER triggers are only
** included in the returned mask if the TRIGGER_AFTER bit is set in tr_tm.
*/
u_int32_t __trigger_colmask(
  parser_t *parser,       /* parser_t context */
  trigger_t *P_TRIGGER,   /* List of triggers on table tab */
  expr_list_t *changes,  /* Changes list for any UPDATE OF triggers */
  int is_new,           /* 1 for new.* ref mask, 0 for old.* ref mask */
  int tr_tm,           /* Mask of TRIGGER_BEFORE|TRIGGER_AFTER */
  table_t *tab,         /* The table to code triggers from */
  int orconf           /* Default ON CONFLICT policy for trigger steps */
){
  const int op = changes ? TK_UPDATE : TK_DELETE;
  u_int32_t mask = 0;
  trigger_t *p;

  assert( is_new==1 || is_new==0 );
  if( IsView(tab) ){
    return 0xffffffff;
  }
  for(p=P_TRIGGER; p; p=p->next){
    if( p->op==op
     && (tr_tm&p->tr_tm)
     && __check_column_overlap(p->columns,changes)
    ){
      if( p->returning ){
        mask = 0xffffffff;
      }else{
        trigger_prg_t *prg;
        prg = __get_row_trigger(parser, p, tab, orconf);
        if( prg ){
          mask |= prg->colmasks[is_new];
        }
      }
    }
  }

  return mask;
}

#endif /* !defined(DBSQL_OMIT_TRIGGER) */
