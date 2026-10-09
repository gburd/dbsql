/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains C code routines that are called by the parser
** in order to generate code for DELETE FROM statements.
*/
#include "dbsql_int.h"

/*
** While a src_list_t can in general represent multiple tables and subqueries
** (as in the FROM clause of a SELECT statement) in this case it contains
** the name of a single table, as one might find in an INSERT, DELETE,
** or UPDATE statement.  Look up that table in the symbol table and
** return a pointer.  Set an error message and return NULL if the table
** name is not found or if any other error occurs.
**
** The following fields are initialized appropriate in src:
**
**    src->a[0].sp_tab        Pointer to the table_t object
**    src->a[0].u2.ib_index  Pointer to the INDEXED BY index, if there is one
**
*/
table_t *__src_list_lookup(parser_t *parser, src_list_t *src){
  src_item_t *item = src->a;
  table_t *tab;
  assert( item && src->n_src>=1 );
  tab = __locate_table_item(parser, 0, item);
  if( item->s_tab ) __delete_table(parser->db, item->s_tab);
  item->s_tab = tab;
  item->fg.not_cte = 1;
  if( tab ){
    tab->n_tab_ref++;
    if( item->fg.is_indexed_by && __indexed_by_lookup(parser, item) ){
      tab = 0;
    }
  }
  return tab;
}

/* Generate byte-code that will report the number of rows modified
** by a DELETE, INSERT, or UPDATE statement.
*/
void __code_change_count(vdbe_t *v, int reg_counter, const char *col_name){
  __vdbe_add_op0(v, OP_FkCheck);
  __vdbe_add_op2(v, OP_ResultRow, reg_counter, 1);
  __vdbe_set_num_cols(v, 1);
  __vdbe_set_col_name(v, 0, COLNAME_NAME, col_name, DBSQL_STATIC);
}

/* Return true if table tab is read-only.
**
** A table is read-only if any of the following are true:
**
**   1) It is a virtual table and no implementation of the update method
**      has been provided
**
**   2) A trigger is currently being coded and the table is a virtual table
**      that is DBSQL_VTAB_DIRECTONLY or if PRAGMA trusted_schema=OFF and
**      the table is not DBSQL_VTAB_INNOCUOUS.
**
**   3) It is a system table (i.e. dbsql_schema), this call is not
**      part of a nested parse and writable_schema pragma has not
**      been specified
**
**   4) The table is a shadow table, the database connection is in
**      defensive mode, and the current dbsql_prepare()
**      is for a top-level SQL statement.
*/
static int __vtab_is_read_only(parser_t *parser, table_t *tab){
  assert( IS_VIRTUAL(tab) );
  if( __get_v_table(parser->db, tab)->mod->module->update==0 ){
    return 1;
  }

  /* Within triggers:
  **   *  Do not allow DELETE, INSERT, or UPDATE of DBSQL_VTAB_DIRECTONLY
  **      virtual tables
  **   *  Only allow DELETE, INSERT, or UPDATE of non-DBSQL_VTAB_INNOCUOUS
  **      virtual tables if PRAGMA trusted_schema=ON.
  */
  if( (parser->toplevel!=0 || (parser->prep_flags & DBSQL_PREPARE_FROM_DDL))
   && tab->u.vtab.p->vtab_risk >
           ((parser->db->flags & DBSQL_TrustedSchema)!=0)
  ){
    __error_msg(parser, "unsafe use of virtual table \"%s\"",
      tab->name);
  }
  return 0;
}
static int __tab_is_read_only(parser_t *parser, table_t *tab){
  DBSQL *db;
  if( IS_VIRTUAL(tab) ){
    return __vtab_is_read_only(parser, tab);
  }
  if( (tab->tab_flags & (TF_Readonly|TF_Shadow))==0 ) return 0;
  db = parser->db;
  if( (tab->tab_flags & TF_Readonly)!=0 ){
    return __writable_schema(db)==0 && parser->nested==0;
  }
  assert( tab->tab_flags & TF_Shadow );
  return __read_only_shadow_tables(db);
}

/*
** Check to make sure the given table is writable.
**
** If tab is not writable  ->  generate an error message and return 1.
** If tab is writable but other errors have occurred -> return 1.
** If tab is writable and no prior errors -> return 0;
*/
int __is_read_only(parser_t *parser, table_t *tab, trigger_t *P_TRIGGER){
  if( __tab_is_read_only(parser, tab) ){
    __error_msg(parser, "table %s may not be modified", tab->name);
    return 1;
  }
#ifndef DBSQL_OMIT_VIEW
  if( IsView(tab)
   && (P_TRIGGER==0 || (P_TRIGGER->returning && P_TRIGGER->next==0))
  ){
    __error_msg(parser,"cannot modify %s because it is a view",tab->name);
    return 1;
  }
#endif
  return 0;
}


#if !defined(DBSQL_OMIT_VIEW) && !defined(DBSQL_OMIT_TRIGGER)
/*
** Evaluate a view and store its result in an ephemeral table.  The
** where argument is an optional WHERE clause that restricts the
** set of rows in the view that are to be added to the ephemeral table.
*/
void __materialize_view(
  parser_t *parser,       /* Parsing context */
  table_t *view,        /* View definition */
  expr_t *where,        /* Optional WHERE clause to be added */
  expr_list_t *order_by,  /* Optional ORDER BY clause */
  expr_t *limit,        /* Optional LIMIT clause */
  int i_cur             /* Cursor number for ephemeral table */
){
  select_dest_t dest;
  select_t *sel;
  src_list_t *from;
  DBSQL *db = parser->db;
  int i_db = __schema_to_index(db, view->schema);
  where = __expr_dup(db, where, 0);
  from = __src_list_append(parser, 0, 0, 0);
  if( from ){
    assert( from->n_src==1 );
    from->a[0].name = __db_str_dup(db, view->name);
    assert( from->a[0].fg.fixed_schema==0 && from->a[0].fg.is_subquery==0 );
    from->a[0].u4.database = __db_str_dup(db, db->dbs[i_db].db_s_name);
    assert( from->a[0].fg.is_using==0 );
    assert( from->a[0].u3.on==0 );
  }
  sel = __select_new(parser, 0, from, where, 0, 0, order_by,
                          SF_IncludeHidden, limit);
  __select_dest_init(&dest, SRT_EphemTab, i_cur);
  __select(parser, sel, &dest);
  __select_delete(db, sel);
}
#endif /* !defined(DBSQL_OMIT_VIEW) && !defined(DBSQL_OMIT_TRIGGER) */

#if defined(DBSQL_ENABLE_UPDATE_DELETE_LIMIT) && !defined(DBSQL_OMIT_SUBQUERY)
/*
** Generate an expression tree to implement the WHERE, ORDER BY,
** and LIMIT/OFFSET portion of DELETE and UPDATE statements.
**
**     DELETE FROM table_wxyz WHERE a<5 ORDER BY a LIMIT 1;
**                            \__________________________/
**                               limit_where (in_clause)
*/
expr_t *__limit_where(
  parser_t *parser,               /* The parser context */
  src_list_t *src,               /* the FROM clause -- which tables to scan */
  expr_t *where,                /* The WHERE clause.  May be null */
  expr_list_t *order_by,          /* The ORDER BY clause.  May be null */
  expr_t *limit,                /* The LIMIT clause.  May be null */
  char *stmt_type              /* Either DELETE or UPDATE.  For err msgs. */
){
  DBSQL *db = parser->db;
  expr_t *lhs = NULL;           /* LHS of IN(SELECT...) operator */
  expr_t *in_clause = NULL;      /* WHERE rowid IN ( select ) */
  expr_list_t *e_list = NULL;     /* Expression list containing only select_rowid*/
  src_list_t *select_src = NULL;  /* SELECT rowid FROM x ... (dup of src) */
  select_t *p_select = NULL;      /* Complete SELECT tree */
  table_t *tab;

  /* Check that there isn't an ORDER BY without a LIMIT clause.
  */
  if( order_by && limit==0 ) {
    __error_msg(parser, "ORDER BY without LIMIT on %s", stmt_type);
    __expr_delete(parser->db, where);
    __expr_list_delete(parser->db, order_by);
    return 0;
  }

  /* We only need to generate a select expression if there
  ** is a limit/offset term to enforce.
  */
  if( limit == 0 ) {
    return where;
  }

  /* Generate a select expression tree to enforce the limit/offset
  ** term for the DELETE or UPDATE statement.  For example:
  **   DELETE FROM table_a WHERE col1=1 ORDER BY col2 LIMIT 1 OFFSET 1
  ** becomes:
  **   DELETE FROM table_a WHERE rowid IN (
  **     SELECT rowid FROM table_a WHERE col1=1 ORDER BY col2 LIMIT 1 OFFSET 1
  **   );
  */

  tab = src->a[0].s_tab;
  if( HAS_ROWID(tab) ){
    lhs = __p_expr(parser, TK_ROW, 0, 0);
    e_list = __expr_list_append(
        parser, 0, __p_expr(parser, TK_ROW, 0, 0)
    );
  }else{
    index_t *pk = __primary_key_index(tab);
    assert( pk!=0 );
    assert( pk->n_key_col>=1 );
    if( pk->n_key_col==1 ){
      const char *name;
      assert( pk->columns[0]>=0 && pk->columns[0]<tab->n_col );
      name = tab->cols[pk->columns[0]].cn_name;
      lhs = __expr(db, TK_ID, name);
      e_list = __expr_list_append(parser, 0, __expr(db, TK_ID, name));
    }else{
      int i;
      for(i=0; i<pk->n_key_col; i++){
        expr_t *p;
        assert( pk->columns[i]>=0 && pk->columns[i]<tab->n_col );
        p = __expr(db, TK_ID, tab->cols[pk->columns[i]].cn_name);
        e_list = __expr_list_append(parser, e_list, p);
      }
      lhs = __p_expr(parser, TK_VECTOR, 0, 0);
      if( lhs ){
        lhs->x.list = __expr_list_dup(db, e_list, 0);
      }
    }
  }

  /* duplicate the FROM clause as it is needed by both the DELETE/UPDATE tree
  ** and the SELECT subtree. */
  src->a[0].s_tab = 0;
  select_src = __src_list_dup(db, src, 0);
  src->a[0].s_tab = tab;
  if( src->a[0].fg.is_indexed_by ){
    assert( src->a[0].fg.is_cte==0 );
    src->a[0].u2.ib_index = 0;
    src->a[0].fg.is_indexed_by = 0;
    __dbsql_db_free(db, src->a[0].u1.indexed_by);
  }else if( src->a[0].fg.is_cte ){
    src->a[0].u2.cte_use->n_use++;
  }

  /* generate the SELECT expression tree. */
  p_select = __select_new(parser, e_list, select_src, where, 0 ,0,
      order_by,0,limit
  );

  /* now generate the new WHERE rowid IN clause for the DELETE/UPDATE */
  in_clause = __p_expr(parser, TK_IN, lhs, 0);
  __p_expr_add_select(parser, in_clause, p_select);
  return in_clause;
}
#endif /* defined(DBSQL_ENABLE_UPDATE_DELETE_LIMIT) */
       /*      && !defined(DBSQL_OMIT_SUBQUERY) */

/*
** Generate code for a DELETE FROM statement.
**
**     DELETE FROM table_wxyz WHERE a<5 AND b NOT NULL;
**                 \________/       \________________/
**                  tab_list              where
*/
void __delete_from(
  parser_t *parser,         /* The parser context */
  src_list_t *tab_list,     /* The table from which we should delete things */
  expr_t *where,          /* The WHERE clause.  May be null */
  expr_list_t *order_by,    /* ORDER BY clause. May be null */
  expr_t *limit           /* LIMIT clause. May be null */
){
  vdbe_t *v;               /* The virtual database engine */
  table_t *tab;           /* The table from which records will be deleted */
  int i;                 /* Loop counter */
  where_info_t *w_info;     /* Information about the WHERE clause */
  index_t *idx;           /* For looping over indices of the table */
  int i_tab_cur;           /* Cursor number for the table */
  int i_data_cur = 0;      /* VDBE cursor for the canonical data source */
  int i_idx_cur = 0;       /* Cursor number of the first index */
  int n_idx;              /* Number of indices */
  DBSQL *db;           /* Main database structure */
  auth_context_t s_context;  /* Authorization context */
  name_context_t s_nc;       /* Name context to resolve expressions in */
  int i_db;               /* Database number */
  int mem_cnt = 0;        /* Memory cell used for change counting */
  int rcauth;            /* Value returned by authorization callback */
  int one_pass;          /* ONEPASS_OFF or _SINGLE or _MULTI */
  int cur_one_passes[2];   /* The write cursors opened by WHERE_ONEPASS */
  u_int8_t *to_opens = 0;       /* Open cursor i_tab_cur+j if to_opens[j] is true */
  index_t *pk;            /* The PRIMARY KEY index on the table */
  int i_pk = 0;           /* First of n_pk registers holding PRIMARY KEY value */
  int16_t n_pk = 1;           /* Number of columns in the PRIMARY KEY */
  int i_key;              /* Memory cell holding key of row to be deleted */
  int16_t n_key;              /* Number of memory cells in the row key */
  int i_eph_cur = 0;       /* Ephemeral table holding all primary key values */
  int i_row_set = 0;       /* Register for rowset of rows to delete */
  int addr_bypass = 0;    /* Address of jump over the delete logic */
  int addr_loop = 0;      /* Top of the delete loop */
  int addr_eph_open = 0;   /* Instruction to open the Ephemeral table */
  int complex;          /* True if there are triggers or FKs or
                         ** subqueries in the WHERE clause */

#ifndef DBSQL_OMIT_TRIGGER
  int isView;                  /* True if attempting to delete from a view */
  trigger_t *P_TRIGGER;           /* List of table triggers, if required */
#endif

  memset(&s_context, 0, sizeof(s_context));
  db = parser->db;
  assert( db->parser==parser );
  if( parser->n_err ){
    goto delete_from_cleanup;
  }
  assert( db->malloc_failed==0 );
  assert( tab_list->n_src==1 );

  /* Locate the table which we want to delete.  This table has to be
  ** put in an src_list_t structure because some of the subroutines we
  ** will be calling are designed to work with multiple tables and expect
  ** an src_list_t* parameter instead of just a table_t* parameter.
  */
  tab = __src_list_lookup(parser, tab_list);
  if( tab==0 )  goto delete_from_cleanup;

  /* Figure out if we have any triggers and if the table being
  ** deleted from is a view
  */
#ifndef DBSQL_OMIT_TRIGGER
  P_TRIGGER = __triggers_exist(parser, tab, TK_DELETE, 0, 0);
  isView = IsView(tab);
#else
# define P_TRIGGER 0
# define isView 0
#endif
  complex = P_TRIGGER || __fk_required(parser, tab, 0, 0);
#ifdef DBSQL_OMIT_VIEW
# undef isView
# define isView 0
#endif

#if TREETRACE_ENABLED
  if( __tree_trace & 0x10000 ){
    __tree_view_line(0, "In __delete() at %s:%d", __FILE__, __LINE__);
    __tree_view_delete(parser->with, tab_list, where,
                          order_by, limit, P_TRIGGER);
  }
#endif

#ifdef DBSQL_ENABLE_UPDATE_DELETE_LIMIT
  if( !isView ){
    where = __limit_where(
        parser, tab_list, where, order_by, limit, "DELETE"
    );
    order_by = 0;
    limit = 0;
  }
#endif

  /* If tab is really a view, make sure it has been initialized.
  */
  if( __view_get_column_names(parser, tab) ){
    goto delete_from_cleanup;
  }

  if( __is_read_only(parser, tab, P_TRIGGER) ){
    goto delete_from_cleanup;
  }
  i_db = __schema_to_index(db, tab->schema);
  assert( i_db<db->n_db );
  rcauth = __auth_check(parser, DBSQL_DELETE, tab->name, 0,
                            db->dbs[i_db].db_s_name);
  assert( rcauth==DBSQL_OK || rcauth==DBSQL_DENY || rcauth==DBSQL_IGNORE );
  if( rcauth==DBSQL_DENY ){
    goto delete_from_cleanup;
  }
  assert(!isView || P_TRIGGER);

  /* Assign cursor numbers to the table and all its indices.
  */
  assert( tab_list->n_src==1 );
  i_tab_cur = tab_list->a[0].i_cursor = parser->n_tab++;
  for(n_idx=0, idx=tab->p_index; idx; idx=idx->next, n_idx++){
    parser->n_tab++;
  }

  /* Start the view context
  */
  if( isView ){
    __auth_context_push(parser, &s_context, tab->name);
  }

  /* Begin generating code.
  */
  v = __get_vdbe(parser);
  if( v==0 ){
    goto delete_from_cleanup;
  }
  if( parser->nested==0 ) __vdbe_count_changes(v);
  __begin_write_operation(parser, complex, i_db);

  /* If we are trying to delete from a view, realize that view into
  ** an ephemeral table.
  */
#if !defined(DBSQL_OMIT_VIEW) && !defined(DBSQL_OMIT_TRIGGER)
  if( isView ){
    __materialize_view(parser, tab,
        where, order_by, limit, i_tab_cur
    );
    i_data_cur = i_idx_cur = i_tab_cur;
    order_by = 0;
    limit = 0;
  }
#endif

  /* Resolve the column names in the WHERE clause.
  */
  memset(&s_nc, 0, sizeof(s_nc));
  s_nc.parser = parser;
  s_nc.src_list = tab_list;
  if( __resolve_expr_names(&s_nc, where) ){
    goto delete_from_cleanup;
  }

  /* Initialize the counter of the number of rows deleted, if
  ** we are counting rows.
  */
  if( (db->flags & DBSQL_CountRows)!=0
   && !parser->nested
   && !parser->trigger_tab
   && !parser->returning
  ){
    mem_cnt = ++parser->n_mem;
    __vdbe_add_op2(v, OP_Integer, 0, mem_cnt);
  }

#ifndef DBSQL_OMIT_TRUNCATE_OPTIMIZATION
  /* Special case: A DELETE without a WHERE clause deletes everything.
  ** It is easier just to erase the whole table. Prior to version 3.6.5,
  ** this optimization caused the row change count (the value returned by
  ** API function dbsql_count_changes) to be set incorrectly.
  **
  ** The "rcauth==DBSQL_OK" terms is the
  ** IMPLEMENTATION-OF: R-17228-37124 If the action code is DBSQL_DELETE and
  ** the callback returns DBSQL_IGNORE then the DELETE operation proceeds but
  ** the truncate optimization is disabled and all rows are deleted
  ** individually.
  */
  if( rcauth==DBSQL_OK
   && where==0
   && !complex
   && !IS_VIRTUAL(tab)
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
   && db->pre_update_callback==0
#endif
  ){
    assert( !isView );
    __table_lock(parser, i_db, tab->tnum, 1, tab->name);
    if( HAS_ROWID(tab) ){
      __vdbe_add_op4(v, OP_Clear, tab->tnum, i_db, mem_cnt ? mem_cnt : -1,
                        tab->name, P4_STATIC);
    }
    for(idx=tab->p_index; idx; idx=idx->next){
      assert( idx->schema==tab->schema );
      if( IS_PRIMARY_KEY_INDEX(idx) && !HAS_ROWID(tab) ){
        __vdbe_add_op3(v, OP_Clear, idx->tnum, i_db, mem_cnt ? mem_cnt : -1);
      }else{
        __vdbe_add_op2(v, OP_Clear, idx->tnum, i_db);
      }
    }
  }else
#endif /* DBSQL_OMIT_TRUNCATE_OPTIMIZATION */
  {
    u_int16_t wcf = WHERE_ONEPASS_DESIRED|WHERE_DUPLICATES_OK;
    if( s_nc.nc_flags & NC_Subquery ) complex = 1;
    wcf |= (complex ? 0 : WHERE_ONEPASS_MULTIROW);
    if( HAS_ROWID(tab) ){
      /* For a rowid table, initialize the row_set_t to an empty set */
      pk = 0;
      assert( n_pk==1 );
      i_row_set = ++parser->n_mem;
      __vdbe_add_op2(v, OP_Null, 0, i_row_set);
    }else{
      /* For a WITHOUT ROWID table, create an ephemeral table used to
      ** hold all primary keys for rows to be deleted. */
      pk = __primary_key_index(tab);
      assert( pk!=0 );
      n_pk = pk->n_key_col;
      i_pk = parser->n_mem+1;
      parser->n_mem += n_pk;
      i_eph_cur = parser->n_tab++;
      addr_eph_open = __vdbe_add_op2(v, OP_OpenEphemeral, i_eph_cur, n_pk);
      __vdbe_set_p4_key_info(parser, pk);
    }
 
    /* Construct a query to find the rowid or primary key for every row
    ** to be deleted, based on the WHERE clause. Set variable one_pass
    ** to indicate the strategy used to implement this delete:
    **
    **  ONEPASS_OFF:    Two-pass approach - use a FIFO for rowids/PK values.
    **  ONEPASS_SINGLE: One-pass approach - at most one row deleted.
    **  ONEPASS_MULTI:  One-pass approach - any number of rows may be deleted.
    */
    w_info = __where_begin(parser, tab_list, where, 0, 0,0,wcf,i_tab_cur+1);
    if( w_info==0 ) goto delete_from_cleanup;
    one_pass = __where_ok_one_pass(w_info, cur_one_passes);
    assert( IS_VIRTUAL(tab)==0 || one_pass!=ONEPASS_MULTI );
    assert( IS_VIRTUAL(tab) || complex || one_pass!=ONEPASS_OFF
            || OPTIMIZATION_DISABLED(db, DBSQL_OnePass) );
    if( one_pass!=ONEPASS_SINGLE ) __multi_write(parser);
    if( __where_uses_deferred_seek(w_info) ){
      __vdbe_add_op1(v, OP_FinishSeek, i_tab_cur);
    }
 
    /* Keep track of the number of rows to be deleted */
    if( mem_cnt ){
      __vdbe_add_op2(v, OP_AddImm, mem_cnt, 1);
    }
 
    /* Extract the rowid or primary key for the current row */
    if( pk ){
      for(i=0; i<n_pk; i++){
        assert( pk->columns[i]>=0 );
        __expr_code_get_column_of_table(v, tab, i_tab_cur,
                                        pk->columns[i], i_pk+i);
      }
      i_key = i_pk;
    }else{
      i_key = ++parser->n_mem;
      __expr_code_get_column_of_table(v, tab, i_tab_cur, -1, i_key);
    }
 
    if( one_pass!=ONEPASS_OFF ){
      /* For ONEPASS, no need to store the rowid/primary-key. There is only
      ** one, so just keep it in its register(s) and fall through to the
      ** delete code.  */
      n_key = n_pk; /* OP_Found will use an unpacked key */
      to_opens = __db_malloc_raw_nn(db, n_idx+2);
      if( to_opens==0 ){
        __where_end(w_info);
        goto delete_from_cleanup;
      }
      memset(to_opens, 1, n_idx+1);
      to_opens[n_idx+1] = 0;
      if( cur_one_passes[0]>=0 ) to_opens[cur_one_passes[0]-i_tab_cur] = 0;
      if( cur_one_passes[1]>=0 ) to_opens[cur_one_passes[1]-i_tab_cur] = 0;
      if( addr_eph_open ) __vdbe_change_to_noop(v, addr_eph_open);
      addr_bypass = __vdbe_make_label(parser);
    }else{
      if( pk ){
        /* Add the PK key for this row to the temporary table */
        i_key = ++parser->n_mem;
        n_key = 0;   /* Zero tells OP_Found to use a composite key */
        __vdbe_add_op4(v, OP_MakeRecord, i_pk, n_pk, i_key,
            __index_affinity_str(parser->db, pk), n_pk);
        __vdbe_add_op4_int(v, OP_IdxInsert, i_eph_cur, i_key, i_pk, n_pk);
      }else{
        /* Add the rowid of the row to be deleted to the row_set_t */
        n_key = 1;  /* OP_DeferredSeek always uses a single rowid */
        __vdbe_add_op2(v, OP_RowSetAdd, i_row_set, i_key);
      }
      __where_end(w_info);
    }
 
    /* Unless this is a view, open cursors for the table we are
    ** deleting from and all its indices. If this is a view, then the
    ** only effect this statement has is to fire the INSTEAD OF
    ** triggers.
    */
    if( !isView ){
      int i_addr_once = 0;
      if( one_pass==ONEPASS_MULTI ){
        i_addr_once = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
      }
      testcase( IS_VIRTUAL(tab) );
      __open_table_and_indices(parser, tab, OP_OpenWrite, OPFLAG_FORDELETE,
                                 i_tab_cur, to_opens, &i_data_cur, &i_idx_cur);
      assert( pk || IS_VIRTUAL(tab) || i_data_cur==i_tab_cur );
      assert( pk || IS_VIRTUAL(tab) || i_idx_cur==i_data_cur+1 );
      if( one_pass==ONEPASS_MULTI ){
        __vdbe_jump_here_or_pop_inst(v, i_addr_once);
      }
    }
 
    /* Set up a loop over the rowids/primary-keys that were found in the
    ** where-clause loop above.
    */
    if( one_pass!=ONEPASS_OFF ){
      assert( n_key==n_pk );  /* OP_Found will use an unpacked key */
      if( !IS_VIRTUAL(tab) && to_opens[i_data_cur-i_tab_cur] ){
        assert( pk!=0 || IsView(tab) );
        __vdbe_add_op4_int(v, OP_NotFound, i_data_cur, addr_bypass, i_key, n_key);
        VDBE_COVERAGE(v);
      }
    }else if( pk ){
      addr_loop = __vdbe_add_op1(v, OP_Rewind, i_eph_cur); VDBE_COVERAGE(v);
      if( IS_VIRTUAL(tab) ){
        __vdbe_add_op3(v, OP_Column, i_eph_cur, 0, i_key);
      }else{
        __vdbe_add_op2(v, OP_RowData, i_eph_cur, i_key);
      }
      assert( n_key==0 );  /* OP_Found will use a composite key */
    }else{
      addr_loop = __vdbe_add_op3(v, OP_RowSetRead, i_row_set, 0, i_key);
      VDBE_COVERAGE(v);
      assert( n_key==1 );
    } 
 
    /* Delete the row */
#ifndef DBSQL_OMIT_VIRTUALTABLE
    if( IS_VIRTUAL(tab) ){
      const char *v_tab = (const char *)__get_v_table(db, tab);
      __vtab_make_writable(parser, tab);
      assert( one_pass==ONEPASS_OFF || one_pass==ONEPASS_SINGLE );
      __may_abort(parser);
      if( one_pass==ONEPASS_SINGLE ){
        __vdbe_add_op1(v, OP_Close, i_tab_cur);
        if( __is_toplevel(parser) ){
          parser->is_multi_write = 0;
        }
      }
      __vdbe_add_op4(v, OP_VUpdate, 0, 1, i_key, v_tab, P4_VTAB);
      __vdbe_change_p5(v, OE_Abort);
    }else
#endif
    {
      int count = (parser->nested==0);    /* True to count changes */
      __generate_row_delete(parser, tab, P_TRIGGER, i_data_cur, i_idx_cur,
          i_key, n_key, count, OE_Default, one_pass, cur_one_passes[1]);
    }
 
    /* End of the loop over all rowids/primary-keys. */
    if( one_pass!=ONEPASS_OFF ){
      __vdbe_resolve_label(v, addr_bypass);
      __where_end(w_info);
    }else if( pk ){
      __vdbe_add_op2(v, OP_Next, i_eph_cur, addr_loop+1); VDBE_COVERAGE(v);
      __vdbe_jump_here(v, addr_loop);
    }else{
      __vdbe_goto(v, addr_loop);
      __vdbe_jump_here(v, addr_loop);
    }    
  } /* End non-truncate path */

  /* Update the dbsql_sequence table by storing the content of the
  ** maximum rowid counter values recorded while inserting into
  ** autoincrement tables.
  */
  if( parser->nested==0 && parser->trigger_tab==0 ){
    __autoincrement_end(parser);
  }

  /* Return the number of rows that were deleted. If this routine is
  ** generating code because of a call to __nested_parse(), do not
  ** invoke the callback function.
  */
  if( mem_cnt ){
    __code_change_count(v, mem_cnt, "rows deleted");
  }

delete_from_cleanup:
  __auth_context_pop(&s_context);
  __src_list_delete(db, tab_list);
  __expr_delete(db, where);
#if defined(DBSQL_ENABLE_UPDATE_DELETE_LIMIT)
  __expr_list_delete(db, order_by);
  __expr_delete(db, limit);
#endif
  if( to_opens ) __db_nn_free_nn(db, to_opens);
  return;
}
/* Make sure "isView" and other macros defined above are undefined. Otherwise
** they may interfere with compilation of other functions in this file
** (or in another file, if this file becomes part of the amalgamation).  */
#ifdef isView
 #undef isView
#endif
#ifdef P_TRIGGER
 #undef P_TRIGGER
#endif

/*
** This routine generates VDBE code that causes a single row of a
** single table to be deleted.  Both the original table entry and
** all indices are removed.
**
** Preconditions:
**
**   1.  i_data_cur is an open cursor on the btree that is the canonical data
**       store for the table.  (This will be either the table itself,
**       in the case of a rowid table, or the PRIMARY KEY index in the case
**       of a WITHOUT ROWID table.)
**
**   2.  Read/write cursors for all indices of tab must be open as
**       cursor number i_idx_cur+i for the i-th index.
**
**   3.  The primary key for the row to be deleted must be stored in a
**       sequence of n_pk memory cells starting at i_pk.  If n_pk==0 that means
**       that a search record formed from OP_MakeRecord is contained in the
**       single memory location i_pk.
**
** mode:
**   Parameter mode may be passed either ONEPASS_OFF (0), ONEPASS_SINGLE, or
**   ONEPASS_MULTI.  If mode is not ONEPASS_OFF, then the cursor
**   i_data_cur already points to the row to delete. If mode is ONEPASS_OFF
**   then this function must seek i_data_cur to the entry identified by i_pk
**   and n_pk before reading from it.
**
**   If mode is ONEPASS_MULTI, then this call is being made as part
**   of a ONEPASS delete that affects multiple rows. In this case, if
**   i_idx_no_seek is a valid cursor number (>=0) and is not the same as
**   i_data_cur, then its position should be preserved following the delete
**   operation. Or, if i_idx_no_seek is not a valid cursor number, the
**   position of i_data_cur should be preserved instead.
**
** i_idx_no_seek:
**   If i_idx_no_seek is a valid cursor number (>=0) not equal to i_data_cur,
**   then it identifies an index cursor (from within array of cursors
**   starting at i_idx_cur) that already points to the index entry to be deleted.
**   Except, this optimization is disabled if there are BEFORE triggers since
**   the trigger body might have moved the cursor.
*/
void __generate_row_delete(
  parser_t *parser,     /* Parsing context */
  table_t *tab,       /* table_t containing the row to be deleted */
  trigger_t *P_TRIGGER, /* List of triggers to (potentially) fire */
  int i_data_cur,      /* Cursor from which column data is extracted */
  int i_idx_cur,       /* First index cursor */
  int i_pk,           /* First memory cell containing the PRIMARY KEY */
  int16_t n_pk,           /* Number of PRIMARY KEY memory cells */
  u_int8_t count,          /* If non-zero, increment the row change counter */
  u_int8_t onconf,         /* Default ON CONFLICT policy for triggers */
  u_int8_t mode,          /* ONEPASS_OFF, _SINGLE, or _MULTI.  See above */
  int i_idx_no_seek     /* Cursor number of cursor that does not need seeking */
){
  vdbe_t *v = parser->vdbe;        /* vdbe_t */
  int i_old = 0;                   /* First register in OLD.* array */
  int i_label;                     /* Label resolved to end of generated code */
  u_int8_t op_seek;                      /* Seek opcode */

  /* vdbe_t is guaranteed to have been allocated by this stage. */
  assert( v );
  VDBE_MODULE_COMMENT((v, "BEGIN: GenRowDel(%d,%d,%d,%d)",
                         i_data_cur, i_idx_cur, i_pk, (int)n_pk));

  /* Seek cursor i_cur to the row to delete. If this row no longer exists
  ** (this can happen if a trigger program has already deleted it), do
  ** not attempt to delete it or fire any DELETE triggers.  */
  i_label = __vdbe_make_label(parser);
  op_seek = HAS_ROWID(tab) ? OP_NotExists : OP_NotFound;
  if( mode==ONEPASS_OFF ){
    __vdbe_add_op4_int(v, op_seek, i_data_cur, i_label, i_pk, n_pk);
    VDBE_COVERAGE_IF(v, op_seek==OP_NotExists);
    VDBE_COVERAGE_IF(v, op_seek==OP_NotFound);
  }

  /* If there are any triggers to fire, allocate a range of registers to
  ** use for the old.* references in the triggers.  */
  if( __fk_required(parser, tab, 0, 0) || P_TRIGGER ){
    u_int32_t mask;                     /* Mask of OLD.* columns in use */
    int i_col;                     /* Iterator used while populating OLD.* */
    int addr_start;                /* Start of BEFORE trigger programs */

    /* TODO: Could use temporary registers here. Also could attempt to
    ** avoid copying the contents of the rowid register.  */
    mask = __trigger_colmask(
        parser, P_TRIGGER, 0, 0, TRIGGER_BEFORE|TRIGGER_AFTER, tab, onconf
    );
    mask |= __fk_oldmask(parser, tab);
    i_old = parser->n_mem+1;
    parser->n_mem += (1 + tab->n_col);

    /* Populate the OLD.* pseudo-table register array. These values will be
    ** used by any BEFORE and AFTER triggers that exist.  */
    __vdbe_add_op2(v, OP_Copy, i_pk, i_old);
    for(i_col=0; i_col<tab->n_col; i_col++){
      testcase( mask!=0xffffffff && i_col==31 );
      testcase( mask!=0xffffffff && i_col==32 );
      if( mask==0xffffffff || (i_col<=31 && (mask & MASKBIT32(i_col))!=0) ){
        int kk = __table_column_to_storage(tab, i_col);
        __expr_code_get_column_of_table(v, tab, i_data_cur, i_col, i_old+kk+1);
      }
    }

    /* Invoke BEFORE DELETE trigger programs. */
    addr_start = __vdbe_current_addr(v);
    __code_row_trigger(parser, P_TRIGGER,
        TK_DELETE, 0, TRIGGER_BEFORE, tab, i_old, onconf, i_label
    );

    /* If any BEFORE triggers were coded, then seek the cursor to the
    ** row to be deleted again. It may be that the BEFORE triggers moved
    ** the cursor or already deleted the row that the cursor was
    ** pointing to.
    **
    ** Also disable the i_idx_no_seek optimization since the BEFORE trigger
    ** may have moved that cursor.
    */
    if( addr_start<__vdbe_current_addr(v) ){
      __vdbe_add_op4_int(v, op_seek, i_data_cur, i_label, i_pk, n_pk);
      VDBE_COVERAGE_IF(v, op_seek==OP_NotExists);
      VDBE_COVERAGE_IF(v, op_seek==OP_NotFound);
      testcase( i_idx_no_seek>=0 );
      i_idx_no_seek = -1;
    }

    /* Do FK processing. This call checks that any FK constraints that
    ** refer to this table (i.e. constraints attached to other tables)
    ** are not violated by deleting this row.  */
    __fk_check(parser, tab, i_old, 0, 0, 0);
  }

  /* Delete the index and table entries. Skip this step if tab is really
  ** a view (in which case the only effect of the DELETE statement is to
  ** fire the INSTEAD OF triggers). 
  **
  ** If variable 'count' is non-zero, then this OP_Delete instruction should
  ** invoke the update-hook. The pre-update-hook, on the other hand should
  ** be invoked unless table tab is a system table. The difference is that
  ** the update-hook is not invoked for rows removed by REPLACE, but the
  ** pre-update-hook is.
  */
  if( !IsView(tab) ){
    u_int8_t p5 = 0;
    __generate_row_index_delete(parser, tab, i_data_cur, i_idx_cur,0,i_idx_no_seek);
    __vdbe_add_op2(v, OP_Delete, i_data_cur, (count?OPFLAG_NCHANGE:0));
    if( parser->nested==0 || 0==dbsql_stricmp(tab->name, "dbsql_stat1") ){
      __vdbe_append_p4(v, (char*)tab, P4_TABLE);
    }
    if( mode!=ONEPASS_OFF ){
      __vdbe_change_p5(v, OPFLAG_AUXDELETE);
    }
    if( i_idx_no_seek>=0 && i_idx_no_seek!=i_data_cur ){
      __vdbe_add_op1(v, OP_Delete, i_idx_no_seek);
    }
    if( mode==ONEPASS_MULTI ) p5 |= OPFLAG_SAVEPOSITION;
    __vdbe_change_p5(v, p5);
  }

  /* Do any ON CASCADE, SET NULL or SET DEFAULT operations required to
  ** handle rows (possibly in other tables) that refer via a foreign key
  ** to the row just deleted. */
  __fk_actions(parser, tab, 0, i_old, 0, 0);

  /* Invoke AFTER DELETE trigger programs. */
  if( P_TRIGGER ){
    __code_row_trigger(parser, P_TRIGGER,
        TK_DELETE, 0, TRIGGER_AFTER, tab, i_old, onconf, i_label
    );
  }

  /* Jump here if the row had already been deleted before any BEFORE
  ** trigger programs were invoked. Or if a trigger program throws a
  ** RAISE(IGNORE) exception.  */
  __vdbe_resolve_label(v, i_label);
  VDBE_MODULE_COMMENT((v, "END: GenRowDel()"));
}

/*
** This routine generates VDBE code that causes the deletion of all
** index entries associated with a single row of a single table, tab
**
** Preconditions:
**
**   1.  A read/write cursor "i_data_cur" must be open on the canonical storage
**       btree for the table tab.  (This will be either the table itself
**       for rowid tables or to the primary key index for WITHOUT ROWID
**       tables.)
**
**   2.  Read/write cursors for all indices of tab must be open as
**       cursor number i_idx_cur+i for the i-th index.  (The tab->p_index
**       index is the 0-th index.)
**
**   3.  The "i_data_cur" cursor must be already be positioned on the row
**       that is to be deleted.
*/
void __generate_row_index_delete(
  parser_t *parser,     /* Parsing and code generating context */
  table_t *tab,       /* table_t containing the row to be deleted */
  int i_data_cur,      /* Cursor of table holding data. */
  int i_idx_cur,       /* First index cursor */
  int *reg_idx,      /* Only delete if reg_idx!=0 && reg_idx[i]>0 */
  int i_idx_no_seek     /* Do not delete from this cursor */
){
  int i;             /* index_t loop counter */
  int r1 = -1;       /* Register holding an index key */
  int i_part_idx_label; /* Jump destination for skipping partial index entries */
  index_t *idx;       /* Current index */
  index_t *prior = 0; /* Prior index */
  vdbe_t *v;           /* The prepared statement under construction */
  index_t *pk;        /* PRIMARY KEY index, or NULL for rowid tables */

  v = parser->vdbe;
  pk = HAS_ROWID(tab) ? 0 : __primary_key_index(tab);
  for(i=0, idx=tab->p_index; idx; i++, idx=idx->next){
    assert( i_idx_cur+i!=i_data_cur || pk==idx );
    if( reg_idx!=0 && reg_idx[i]==0 ) continue;
    if( idx==pk ) continue;
    if( i_idx_cur+i==i_idx_no_seek ) continue;
    VDBE_MODULE_COMMENT((v, "GenRowIdxDel for %s", idx->name));
    r1 = __generate_index_key(parser, idx, i_data_cur, 0, 1,
        &i_part_idx_label, prior, r1);
    __vdbe_add_op3(v, OP_IdxDelete, i_idx_cur+i, r1,
        idx->uniq_not_null ? idx->n_key_col : idx->n_column
    );
    __vdbe_change_p4(v, -1, (const char*)idx, P4_INDEX);
    __resolve_part_idx_label(parser, i_part_idx_label);
    prior = idx;
  }
}

/*
** Generate code that will assemble an index key and stores it in register
** reg_out.  The key with be for index idx which is an index on tab.
** i_cur is the index of a cursor open on the tab table and pointing to
** the entry that needs indexing.  If tab is a WITHOUT ROWID table, then
** i_cur must be the cursor of the PRIMARY KEY index.
**
** Return a register number which is the first in a block of
** registers that holds the elements of the index key.  The
** block of registers has already been deallocated by the time
** this routine returns.
**
** If *pi_part_idx_label is not NULL, fill it in with a label and jump
** to that label if idx is a partial index that should be skipped.
** The label should be resolved using __resolve_part_idx_label().
** A partial index should be skipped if its WHERE clause evaluates
** to false or null.  If idx is not a partial index, *pi_part_idx_label
** will be set to zero which is an empty label that is ignored by
** __resolve_part_idx_label().
**
** The prior and reg_prior parameters are used to implement a cache to
** avoid unnecessary register loads.  If prior is not NULL, then it is
** a pointer to a different index for which an index key has just been
** computed into register reg_prior.  If the current idx index is generating
** its key into the same sequence of registers and if prior and idx share
** a column in common, then the register corresponding to that column already
** holds the correct value and the loading of that register is skipped.
** This optimization is helpful when doing a DELETE or an INTEGRITY_CHECK
** on a table with multiple indices, and especially with the ROWID or
** PRIMARY KEY columns of the index.
*/
int __generate_index_key(
  parser_t *parser,       /* Parsing context */
  index_t *idx,         /* The index for which to generate a key */
  int i_data_cur,        /* Cursor number from which to take column data */
  int reg_out,          /* Put the new key into this register if not 0 */
  int prefix_only,      /* Compute only a unique prefix of the key */
  int *pi_part_idx_label, /* OUT: Jump to this label to skip partial index */
  index_t *prior,       /* Previously generated index key */
  int reg_prior         /* Register holding previous generated key */
){
  vdbe_t *v = parser->vdbe;
  int j;
  int reg_base;
  int n_col;

  if( pi_part_idx_label ){
    if( idx->part_idx_where ){
      *pi_part_idx_label = __vdbe_make_label(parser);
      parser->i_self_tab = i_data_cur + 1;
      __expr_if_false_dup(parser, idx->part_idx_where, *pi_part_idx_label,
                            DBSQL_JUMPIFNULL);
      parser->i_self_tab = 0;
      prior = 0; /* Ticket a9efb42811fa41ee 2019-11-02;
                  ** part_idx_where may have corrupted reg_prior registers */
    }else{
      *pi_part_idx_label = 0;
    }
  }
  n_col = (prefix_only && idx->uniq_not_null) ? idx->n_key_col : idx->n_column;
  reg_base = __get_temp_range(parser, n_col);
  if( prior && (reg_base!=reg_prior || prior->part_idx_where) ) prior = 0;
  for(j=0; j<n_col; j++){
    if( prior
     && prior->columns[j]==idx->columns[j]
     && prior->columns[j]!=XN_EXPR
    ){
      /* This column was already computed by the previous index */
      continue;
    }
    __expr_code_load_index_column(parser, idx, i_data_cur, j, reg_base+j);
    if( idx->columns[j]>=0 ){
      /* If the column affinity is REAL but the number is an integer, then it
      ** might be stored in the table as an integer (using a compact
      ** representation) then converted to REAL by an OP_RealAffinity opcode.
      ** But we are getting ready to store this value back into an index, where
      ** it should be converted by to INTEGER again.  So omit the
      ** OP_RealAffinity opcode if it is present */
      __vdbe_delete_prior_opcode(v, OP_RealAffinity);
    }
  }
  if( reg_out ){
    __vdbe_add_op3(v, OP_MakeRecord, reg_base, n_col, reg_out);
  }
  __release_temp_range(parser, reg_base, n_col);
  return reg_base;
}

/*
** If a prior call to __generate_index_key() generated a jump-over label
** because it was a partial index, then this routine should be called to
** resolve that label.
*/
void __resolve_part_idx_label(parser_t *parser, int i_label){
  if( i_label ){
    __vdbe_resolve_label(parser->vdbe, i_label);
  }
}
