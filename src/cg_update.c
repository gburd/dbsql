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
** to handle UPDATE statements.
*/
#include "dbsql_int.h"

#ifndef DBSQL_OMIT_VIRTUALTABLE
/* Forward declaration */
static void __update_virtual_table(
  parser_t *parser,       /* The parsing context */
  src_list_t *src,       /* The virtual table to be modified */
  table_t *tab,         /* The virtual table */
  expr_list_t *changes,  /* The columns to change in the UPDATE statement */
  expr_t *rowid_expr,    /* Expression used to recompute the rowid */
  int *x_refs,          /* Mapping from columns of tab to entries in changes */
  expr_t *where,        /* WHERE clause of the UPDATE statement */
  int on_error          /* ON CONFLICT strategy */
);
#endif /* DBSQL_OMIT_VIRTUALTABLE */

/*
** The most recently coded instruction was an OP_Column to retrieve the
** i-th column of table tab. This routine sets the P4 parameter of the
** OP_Column to the default value, if any.
**
** The default value of a column is specified by a DEFAULT clause in the
** column definition. This was either supplied by the user when the table
** was created, or added later to the table definition by an ALTER TABLE
** command. If the latter, then the row-records in the table btree on disk
** may not contain a value for the column and the default value, taken
** from the P4 parameter of the OP_Column instruction, is returned instead.
** If the former, then all row-records are guaranteed to include a value
** for the column and the P4 value is not required.
**
** column_t definitions created by an ALTER TABLE command may only have
** literal default values specified: a number, null or a string. (If a more
** complicated default expression value was provided, it is evaluated
** when the ALTER TABLE is executed and one of the literal values written
** into the dbsql_schema table.)
**
** Therefore, the P4 parameter is only required if the default value for
** the column is a literal number, string or null. The __value_from_expr()
** function is capable of transforming these types of expressions into
** dbsql_value_t objects.
**
** If column as REAL affinity and the table is an ordinary b-tree table
** (not a virtual table) then the value might have been stored as an
** integer.  In that case, add an OP_RealAffinity opcode to make sure
** it has been converted into REAL.
*/
void __column_default(vdbe_t *v, table_t *tab, int i, int i_reg){
  column_t *col;
  assert( tab!=0 );
  assert( tab->n_col>i );
  col = &tab->cols[i];
  if( col->i_dflt ){
    dbsql_value_t *value = 0;
    u_int8_t enc = ENC(__vdbe_db(v));
    assert( !IsView(tab) );
    VDBE_COMMENT((v, "%s.%s", tab->name, col->cn_name));
    assert( i<tab->n_col );
    __value_from_expr(__vdbe_db(v),
                         __column_expr(tab,col), enc,
                         col->affinity, &value);
    if( value ){
      __vdbe_append_p4(v, value, P4_MEM);
    }
  }
#ifndef DBSQL_OMIT_FLOATING_POINT
  if( col->affinity==DBSQL_AFF_REAL && !IS_VIRTUAL(tab) ){
    __vdbe_add_op1(v, OP_RealAffinity, i_reg);
  }
#endif
}

/*
** Check to see if column i_col of index idx references any of the
** columns defined by x_refs and chng_rowid.  Return true if it does
** and false if not.  This is an optimization.  False-positives are a
** performance degradation, but false-negatives can result in a corrupt
** index and incorrect answers.
**
** x_refs[j] will be non-negative if column j of the original table is
** being updated.  chng_rowid will be true if the rowid of the table is
** being updated.
*/
static int __index_column_is_being_updated(
  index_t *idx,      /* The index to check */
  int i_col,         /* Which column of the index to check */
  int *x_refs,       /* x_refs[j]>=0 if column j is being updated */
  int chng_rowid     /* true if the rowid is being updated */
){
  int16_t i_idx_col = idx->columns[i_col];
  assert( i_idx_col!=XN_ROWID ); /* Cannot index rowid */
  if( i_idx_col>=0 ){
    return x_refs[i_idx_col]>=0;
  }
  assert( i_idx_col==XN_EXPR );
  assert( idx->col_exprs!=0 );
  assert( idx->col_exprs->a[i_col].expr!=0 );
  return __expr_references_updated_column(idx->col_exprs->a[i_col].expr,
                                            x_refs,chng_rowid);
}

/*
** Check to see if index idx is a partial index whose conditional
** expression might change values due to an UPDATE.  Return true if
** the index is subject to change and false if the index is guaranteed
** to be unchanged.  This is an optimization.  False-positives are a
** performance degradation, but false-negatives can result in a corrupt
** index and incorrect answers.
**
** x_refs[j] will be non-negative if column j of the original table is
** being updated.  chng_rowid will be true if the rowid of the table is
** being updated.
*/
static int __index_where_clause_might_change(
  index_t *idx,      /* The index to check */
  int *x_refs,       /* x_refs[j]>=0 if column j is being updated */
  int chng_rowid     /* true if the rowid is being updated */
){
  if( idx->part_idx_where==0 ) return 0;
  return __expr_references_updated_column(idx->part_idx_where,
                                            x_refs, chng_rowid);
}

/*
** Allocate and return a pointer to an expression of type TK_ROW with
** expr_t.i_column set to value (i_col+1). The resolver will modify the
** expression to be a TK_COLUMN reading column i_col of the first
** table in the source-list (src->a[0]).
*/
static expr_t *__expr_row_column(parser_t *parser, int i_col){
  expr_t *ret = __p_expr(parser, TK_ROW, 0, 0);
  if( ret ) ret->i_column = i_col+1;
  return ret;
}

/*
** Assuming both the limit and order_by parameters are NULL, this function
** generates VM code to run the query:
**
**   SELECT <other-columns>, changes FROM tab_list WHERE where
**
** and write the results to the ephemeral table already opened as cursor
** i_eph. None of changes, tab_list or where are modified or consumed by
** this function, they must be deleted by the caller.
**
** Or, if limit and order_by are not NULL, and tab is not a view:
**
**   SELECT <other-columns>, changes FROM tab_list
**   WHERE where
**   GROUP BY <other-columns>
**   ORDER BY order_by LIMIT limit
**
** If tab is a view, the GROUP BY clause is omitted.
**
** Exactly how results are written to table i_eph, and exactly what
** the <other-columns> in the query above are is determined by the type
** of table tab_list->a[0].tab.
**
** If the table is a WITHOUT ROWID table, then argument pk must be its
** PRIMARY KEY. In this case <other-columns> are the primary key columns
** of the table, in order. The results of the query are written to ephemeral
** table i_eph as index keys, using OP_IdxInsert.
**
** If the table is actually a view, then <other-columns> are all columns of
** the view. The results are written to the ephemeral table i_eph as records
** with automatically assigned integer keys.
**
** If the table is a virtual or ordinary intkey table, then <other-columns>
** is its rowid. For a virtual table, the results are written to i_eph as
** records with automatically assigned integer keys For intkey tables, the
** rowid value in <other-columns> is used as the integer key, and the
** remaining fields make up the table record.
*/
static void __update_from_select(
  parser_t *parser,                  /* parser_t context */
  int i_eph,                       /* Cursor for open eph. table */
  index_t *pk,                     /* PK if table 0 is WITHOUT ROWID */
  expr_list_t *changes,             /* List of expressions to return */
  src_list_t *tab_list,              /* List of tables to select from */
  expr_t *where,                   /* WHERE clause for query */
  expr_list_t *order_by,             /* ORDER BY clause */
  expr_t *limit                    /* LIMIT clause */
){
  int i;
  select_dest_t dest;
  select_t *p_select = 0;
  expr_list_t *list = 0;
  expr_list_t *grp = 0;
  expr_t *limit2 = 0;
  expr_list_t *order_by2 = 0;
  DBSQL *db = parser->db;
  table_t *tab = tab_list->a[0].s_tab;
  src_list_t *src;
  expr_t *where2;
  int e_dest;

#ifdef DBSQL_ENABLE_UPDATE_DELETE_LIMIT
  if( order_by && limit==0 ) {
    __error_msg(parser, "ORDER BY without LIMIT on UPDATE");
    return;
  }
  order_by2 = __expr_list_dup(db, order_by, 0);
  limit2 = __expr_dup(db, limit, 0);
#else
  UNUSED_PARAMETER(order_by);
  UNUSED_PARAMETER(limit);
#endif

  src = __src_list_dup(db, tab_list, 0);
  where2 = __expr_dup(db, where, 0);

  assert( tab_list->n_src>1 );
  if( src ){
    assert( src->a[0].fg.not_cte );
    src->a[0].i_cursor = -1;
    src->a[0].s_tab->n_tab_ref--;
    src->a[0].s_tab = 0;
  }
  if( pk ){
    for(i=0; i<pk->n_key_col; i++){
      expr_t *p_new = __expr_row_column(parser, pk->columns[i]);
#ifdef DBSQL_ENABLE_UPDATE_DELETE_LIMIT
      if( limit ){
        grp = __expr_list_append(parser, grp, __expr_dup(db, p_new, 0));
      }
#endif
      list = __expr_list_append(parser, list, p_new);
    }
    e_dest = IS_VIRTUAL(tab) ? SRT_Table : SRT_Upfrom;
  }else if( IsView(tab) ){
    for(i=0; i<tab->n_col; i++){
      list = __expr_list_append(parser, list, __expr_row_column(parser, i));
    }
    e_dest = SRT_Table;
  }else{
    e_dest = IS_VIRTUAL(tab) ? SRT_Table : SRT_Upfrom;
    list = __expr_list_append(parser, 0, __p_expr(parser,TK_ROW,0,0));
#ifdef DBSQL_ENABLE_UPDATE_DELETE_LIMIT
    if( limit ){
      grp = __expr_list_append(parser, 0, __p_expr(parser,TK_ROW,0,0));
    }
#endif
  }
  assert( changes!=0 || parser->db->malloc_failed );
  if( changes ){
    for(i=0; i<changes->n_expr; i++){
      list = __expr_list_append(parser, list,
          __expr_dup(db, changes->a[i].expr, 0)
      );
    }
  }
  p_select = __select_new(parser, list,
      src, where2, grp, 0, order_by2,
      SF_UFSrcCheck|SF_IncludeHidden|SF_UpdateFrom, limit2
  );
  if( p_select ) p_select->sel_flags |= SF_OrderByReqd;
  __select_dest_init(&dest, e_dest, i_eph);
  dest.i_sd_parm2 = (pk ? pk->n_key_col : -1);
  __select(parser, p_select, &dest);
  __select_delete(db, p_select);
}

/*
** Process an UPDATE statement.
**
**   UPDATE OR IGNORE tbl SET a=b, c=d FROM tbl2... WHERE e<5 AND f NOT NULL;
**          \_______/ \_/     \______/      \_____/       \________________/
**           on_error   |      changes         |                where
**                     \_______________________/
**                               tab_list
*/
void __update(
  parser_t *parser,         /* The parser context */
  src_list_t *tab_list,     /* The table in which we should change things */
  expr_list_t *changes,    /* Things to be changed */
  expr_t *where,          /* The WHERE clause.  May be null */
  int on_error,           /* How to handle constraint errors */
  expr_list_t *order_by,    /* ORDER BY clause. May be null */
  expr_t *limit,          /* LIMIT clause. May be null */
  upsert_t *upsert        /* ON CONFLICT clause, or null */
){
  int i, j, k;           /* Loop counters */
  table_t *tab;           /* The table to be updated */
  int addr_top = 0;       /* VDBE instruction address of the start of the loop */
  where_info_t *w_info = 0; /* Information about the WHERE clause */
  vdbe_t *v;               /* The virtual database engine */
  index_t *idx;           /* For looping over indices */
  index_t *pk;            /* The PRIMARY KEY index for WITHOUT ROWID tables */
  int n_idx;              /* Number of indices that need updating */
  int n_all_idx;           /* Total number of indexes */
  int i_base_cur;          /* Base cursor number */
  int i_data_cur;          /* Cursor for the canonical data btree */
  int i_idx_cur;           /* Cursor for the first index */
  DBSQL *db;           /* The database structure */
  int *reg_idx = 0;      /* Registers for to each index and the main table */
  int *x_refs = 0;        /* x_refs[i] is the index in changes->a[] of the
                         ** an expression for the i-th column of the table.
                         ** x_refs[i]==-1 if the i-th column is not changed. */
  u_int8_t *to_opens;           /* 1 for tables and indices to be opened */
  u_int8_t chng_pk;             /* PRIMARY KEY changed in a WITHOUT ROWID table */
  u_int8_t chng_rowid;          /* Rowid changed in a normal table */
  u_int8_t chng_key;            /* Either chng_pk or chng_rowid */
  expr_t *rowid_expr = 0;  /* Expression defining the new record number */
  int i_rowid_expr = -1;   /* index_t of "rowid=" (or IPK) assignment in changes */
  auth_context_t s_context;  /* The authorization context */
  name_context_t s_nc;       /* The name-context to resolve expressions in */
  int i_db;               /* Database containing the table being updated */
  int one_pass;          /* ONEPASS_XXX value from where.c */
  int has_fk;             /* True if foreign key processing is required */
  int label_break;        /* Jump here to break out of UPDATE loop */
  int label_continue;     /* Jump here to continue next step of UPDATE loop */
  int flags;             /* Flags for __where_begin() */

#ifndef DBSQL_OMIT_TRIGGER
  int isView;            /* True when updating a view (INSTEAD OF trigger) */
  trigger_t *P_TRIGGER;     /* List of triggers on tab, if required */
  int tmask;             /* Mask of TRIGGER_BEFORE|TRIGGER_AFTER */
#endif
  int newmask;           /* Mask of NEW.* columns accessed by BEFORE triggers */
  int i_eph = 0;          /* Ephemeral table holding all primary key values */
  int n_key = 0;          /* Number of elements in reg_key for WITHOUT ROWID */
  int cur_one_passes[2];   /* The write cursors opened by WHERE_ONEPASS */
  int addr_open = 0;      /* Address of OP_OpenEphemeral */
  int i_pk = 0;           /* First of n_pk cells holding PRIMARY KEY value */
  int16_t n_pk = 0;           /* Number of components of the PRIMARY KEY */
  int replace = 0;      /* True if REPLACE conflict resolution might happen */
  int finish_seek = 1;   /* The OP_FinishSeek opcode is needed */
  int n_change_from = 0;   /* If there is a FROM, changes->n_expr, else 0 */

  /* Register Allocations */
  int reg_row_count = 0;   /* A count of rows changed */
  int reg_old_rowid = 0;   /* The old rowid */
  int reg_new_rowid = 0;   /* The new rowid */
  int reg_new = 0;        /* Content of the NEW.* table in triggers */
  int reg_old = 0;        /* Content of OLD.* table in triggers */
  int reg_row_set = 0;     /* Rowset of rows to be updated */
  int reg_key = 0;        /* composite PRIMARY KEY value */

  memset(&s_context, 0, sizeof(s_context));
  db = parser->db;
  assert( db->parser==parser );
  if( parser->n_err ){
    goto update_cleanup;
  }
  assert( db->malloc_failed==0 );

  /* Locate the table which we want to update.
  */
  tab = __src_list_lookup(parser, tab_list);
  if( tab==0 ) goto update_cleanup;
  i_db = __schema_to_index(parser->db, tab->schema);

  /* Figure out if we have any triggers and if the table being
  ** updated is a view.
  */
#ifndef DBSQL_OMIT_TRIGGER
  P_TRIGGER = __triggers_exist(parser, tab, TK_UPDATE, changes, &tmask);
  isView = IsView(tab);
  assert( P_TRIGGER || tmask==0 );
#else
# define P_TRIGGER 0
# define isView 0
# define tmask 0
#endif
#ifdef DBSQL_OMIT_VIEW
# undef isView
# define isView 0
#endif

#if TREETRACE_ENABLED
  if( __tree_trace & 0x10000 ){
    __tree_view_line(0, "In __update() at %s:%d", __FILE__, __LINE__);
    __tree_view_update(parser->with, tab_list, changes, where,
                          on_error, order_by, limit, upsert, P_TRIGGER);
  }
#endif

  /* If there was a FROM clause, set n_change_from to the number of expressions
  ** in the change-list. Otherwise, set it to 0. There cannot be a FROM
  ** clause if this function is being called to generate code for part of
  ** an UPSERT statement.  */
  n_change_from = (tab_list->n_src>1) ? changes->n_expr : 0;
  assert( n_change_from==0 || upsert==0 );

#ifdef DBSQL_ENABLE_UPDATE_DELETE_LIMIT
  if( !isView && n_change_from==0 ){
    where = __limit_where(
        parser, tab_list, where, order_by, limit, "UPDATE"
    );
    order_by = 0;
    limit = 0;
  }
#endif

  if( __view_get_column_names(parser, tab) ){
    goto update_cleanup;
  }
  if( __is_read_only(parser, tab, P_TRIGGER) ){
    goto update_cleanup;
  }

  /* Allocate a cursors for the main database table and for all indices.
  ** The index cursors might not be used, but if they are used they
  ** need to occur right after the database cursor.  So go ahead and
  ** allocate enough space, just in case.
  */
  i_base_cur = i_data_cur = parser->n_tab++;
  i_idx_cur = i_data_cur+1;
  pk = HAS_ROWID(tab) ? 0 : __primary_key_index(tab);
  testcase( pk!=0 && pk!=tab->p_index );
  for(n_idx=0, idx=tab->p_index; idx; idx=idx->next, n_idx++){
    if( pk==idx ){
      i_data_cur = parser->n_tab;
    }
    parser->n_tab++;
  }
  if( upsert ){
    /* On an UPSERT, reuse the same cursors already opened by INSERT */
    i_data_cur = upsert->i_data_cur;
    i_idx_cur = upsert->i_idx_cur;
    parser->n_tab = i_base_cur;
  }
  tab_list->a[0].i_cursor = i_data_cur;

  /* Allocate space for x_refs[], reg_idx[], and to_opens[].
  ** Initialize x_refs[] and to_opens[] to their default values.
  */
  x_refs = __db_malloc_raw_nn(db, sizeof(int) * (tab->n_col+n_idx+1) + n_idx+2 );
  if( x_refs==0 ) goto update_cleanup;
  reg_idx = x_refs+tab->n_col;
  to_opens = (u_int8_t*)(reg_idx+n_idx+1);
  memset(to_opens, 1, n_idx+1);
  to_opens[n_idx+1] = 0;
  for(i=0; i<tab->n_col; i++) x_refs[i] = -1;

  /* Initialize the name-context */
  memset(&s_nc, 0, sizeof(s_nc));
  s_nc.parser = parser;
  s_nc.src_list = tab_list;
  s_nc.u_nc.upsert = upsert;
  s_nc.nc_flags = NC_UUpsert;

  /* Begin generating code. */
  v = __get_vdbe(parser);
  if( v==0 ) goto update_cleanup;

  /* Resolve the column names in all the expressions of the
  ** of the UPDATE statement.  Also find the column index
  ** for each column to be updated in the changes array.  For each
  ** column to be updated, make sure we have authorization to change
  ** that column.
  */
  chng_rowid = chng_pk = 0;
  for(i=0; i<changes->n_expr; i++){
    /* If this is an UPDATE with a FROM clause, do not resolve expressions
    ** here. The call to __select() below will do that. */
    if( n_change_from==0 && __resolve_expr_names(&s_nc, changes->a[i].expr) ){
      goto update_cleanup;
    }
    j = __column_index(tab, changes->a[i].e_name);
    if( j>=0 ){
      if( j==tab->i_p_key ){
        chng_rowid = 1;
        rowid_expr = changes->a[i].expr;
        i_rowid_expr = i;
      }else if( pk && (tab->cols[j].col_flags & COLFLAG_PRIMKEY)!=0 ){
        chng_pk = 1;
      }
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
      else if( tab->cols[j].col_flags & COLFLAG_GENERATED ){
        testcase( tab->cols[j].col_flags & COLFLAG_VIRTUAL );
        testcase( tab->cols[j].col_flags & COLFLAG_STORED );
        __error_msg(parser,
           "cannot UPDATE generated column \"%s\"",
           tab->cols[j].cn_name);
        goto update_cleanup;
      }
#endif
      x_refs[j] = i;
    }else{
      if( pk==0 && __is_rowid(changes->a[i].e_name) ){
        j = -1;
        chng_rowid = 1;
        rowid_expr = changes->a[i].expr;
        i_rowid_expr = i;
      }else{
        __error_msg(parser, "no such column: %s", changes->a[i].e_name);
        parser->check_schema = 1;
        goto update_cleanup;
      }
    }
#ifndef DBSQL_OMIT_AUTHORIZATION
    {
      int rc;
      rc = __auth_check(parser, DBSQL_UPDATE, tab->name,
                            j<0 ? "ROWID" : tab->cols[j].cn_name,
                            db->dbs[i_db].db_s_name);
      if( rc==DBSQL_DENY ){
        goto update_cleanup;
      }else if( rc==DBSQL_IGNORE ){
        x_refs[j] = -1;
      }
    }
#endif
  }
  assert( (chng_rowid & chng_pk)==0 );
  assert( chng_rowid==0 || chng_rowid==1 );
  assert( chng_pk==0 || chng_pk==1 );
  chng_key = chng_rowid + chng_pk;

#ifndef DBSQL_OMIT_GENERATED_COLUMNS
  /* Mark generated columns as changing if their generator expressions
  ** reference any changing column.  The actual x_refs[] value for
  ** generated expressions is not used, other than to check to see that it
  ** is non-negative, so the value of x_refs[] for generated columns can be
  ** set to any non-negative number.  We use 99999 so that the value is
  ** obvious when looking at x_refs[] in a symbolic debugger.
  */
  if( tab->tab_flags & TF_HasGenerated ){
    int progress;
    testcase( tab->tab_flags & TF_HasVirtual );
    testcase( tab->tab_flags & TF_HasStored );
    do{
      progress = 0;
      for(i=0; i<tab->n_col; i++){
        if( x_refs[i]>=0 ) continue;
        if( (tab->cols[i].col_flags & COLFLAG_GENERATED)==0 ) continue;
        if( __expr_references_updated_column(
                __column_expr(tab, &tab->cols[i]),
                 x_refs, chng_rowid)
        ){
          x_refs[i] = 99999;
          progress = 1;
        }
      }
    }while( progress );
  }
#endif

  /* The SET expressions are not actually used inside the WHERE loop.
  ** So reset the col_used mask. Unless this is a virtual table. In that
  ** case, set all bits of the col_used mask (to ensure that the virtual
  ** table implementation makes all columns available).
  */
  tab_list->a[0].col_used = IS_VIRTUAL(tab) ? ALLBITS : 0;

  has_fk = __fk_required(parser, tab, x_refs, chng_key);

  /* There is one entry in the reg_idx[] array for each index on the table
  ** being updated.  Fill in reg_idx[] with a register number that will hold
  ** the key for accessing each index.
  */
  if( on_error==OE_Replace ) replace = 1;
  for(n_all_idx=0, idx=tab->p_index; idx; idx=idx->next, n_all_idx++){
    int reg;
    if( chng_key || has_fk>1 || idx==pk
     || __index_where_clause_might_change(idx,x_refs,chng_rowid)
    ){
      reg = ++parser->n_mem;
      parser->n_mem += idx->n_column;
    }else{
      reg = 0;
      for(i=0; i<idx->n_key_col; i++){
        if( __index_column_is_being_updated(idx, i, x_refs, chng_rowid) ){
          reg = ++parser->n_mem;
          parser->n_mem += idx->n_column;
          if( on_error==OE_Default && idx->on_error==OE_Replace ){
            replace = 1;
          }
          break;
        }
      }
    }
    if( reg==0 ) to_opens[n_all_idx+1] = 0;
    reg_idx[n_all_idx] = reg;
  }
  reg_idx[n_all_idx] = ++parser->n_mem;  /* Register storing the table record */
  if( replace ){
    /* If REPLACE conflict resolution might be invoked, open cursors on all
    ** indexes in case they are needed to delete records.  */
    memset(to_opens, 1, n_idx+1);
  }

  if( parser->nested==0 ) __vdbe_count_changes(v);
  __begin_write_operation(parser, P_TRIGGER || has_fk, i_db);

  /* Allocate required registers. */
  if( !IS_VIRTUAL(tab) ){
    /* For now, reg_row_set and reg_idx[n_all_idx] share the same register.
    ** If reg_row_set turns out to be needed, then reg_idx[n_all_idx] will be
    ** reallocated.  reg_idx[n_all_idx] is the register in which the main
    ** table record is written.  reg_row_set holds the row_set_t for the
    ** two-pass update algorithm. */
    assert( reg_idx[n_all_idx]==parser->n_mem );
    reg_row_set = reg_idx[n_all_idx];
    reg_old_rowid = reg_new_rowid = ++parser->n_mem;
    if( chng_pk || P_TRIGGER || has_fk ){
      reg_old = parser->n_mem + 1;
      parser->n_mem += tab->n_col;
    }
    if( chng_key || P_TRIGGER || has_fk ){
      reg_new_rowid = ++parser->n_mem;
    }
    reg_new = parser->n_mem + 1;
    parser->n_mem += tab->n_col;
  }

  /* Start the view context. */
  if( isView ){
    __auth_context_push(parser, &s_context, tab->name);
  }

  /* If we are trying to update a view, realize that view into
  ** an ephemeral table.
  */
#if !defined(DBSQL_OMIT_VIEW) && !defined(DBSQL_OMIT_TRIGGER)
  if( n_change_from==0 && isView ){
    __materialize_view(parser, tab,
        where, order_by, limit, i_data_cur
    );
    order_by = 0;
    limit = 0;
  }
#endif

  /* Resolve the column names in all the expressions in the
  ** WHERE clause.
  */
  if( n_change_from==0 && __resolve_expr_names(&s_nc, where) ){
    goto update_cleanup;
  }

#ifndef DBSQL_OMIT_VIRTUALTABLE
  /* Virtual tables must be handled separately */
  if( IS_VIRTUAL(tab) ){
    __update_virtual_table(parser, tab_list, tab, changes, rowid_expr, x_refs,
                       where, on_error);
    goto update_cleanup;
  }
#endif

  /* Jump to label_break to abandon further processing of this UPDATE */
  label_continue = label_break = __vdbe_make_label(parser);

  /* Not an UPSERT.  Normal processing.  Begin by
  ** initialize the count of updated rows */
  if( (db->flags&DBSQL_CountRows)!=0
   && !parser->trigger_tab
   && !parser->nested
   && !parser->returning
   && upsert==0
  ){
    reg_row_count = ++parser->n_mem;
    __vdbe_add_op2(v, OP_Integer, 0, reg_row_count);
  }

  if( n_change_from==0 && HAS_ROWID(tab) ){
    __vdbe_add_op3(v, OP_Null, 0, reg_row_set, reg_old_rowid);
    i_eph = parser->n_tab++;
    addr_open = __vdbe_add_op3(v, OP_OpenEphemeral, i_eph, 0, reg_row_set);
  }else{
    assert( pk!=0 || HAS_ROWID(tab) );
    n_pk = pk ? pk->n_key_col : 0;
    i_pk = parser->n_mem+1;
    parser->n_mem += n_pk;
    parser->n_mem += n_change_from;
    reg_key = ++parser->n_mem;
    if( upsert==0 ){
      int n_eph_col = n_pk + n_change_from + (isView ? tab->n_col : 0);
      i_eph = parser->n_tab++;
      if( pk ) __vdbe_add_op3(v, OP_Null, 0, i_pk, i_pk+n_pk-1);
      addr_open = __vdbe_add_op2(v, OP_OpenEphemeral, i_eph, n_eph_col);
      if( pk ){
        key_info_t *key_info = __key_info_of_index(parser, pk);
        if( key_info ){
          key_info->n_all_field = n_eph_col;
          __vdbe_append_p4(v, key_info, P4_KEYINFO);
        }
      }
      if( n_change_from ){
        __update_from_select(
            parser, i_eph, pk, changes, tab_list, where, order_by, limit
        );
#ifndef DBSQL_OMIT_SUBQUERY
        if( isView ) i_data_cur = i_eph;
#endif
      }
    }
  }

  if( n_change_from ){
    __multi_write(parser);
    one_pass = ONEPASS_OFF;
    n_key = n_pk;
    reg_key = i_pk;
  }else{
    if( upsert ){
      /* If this is an UPSERT, then all cursors have already been opened by
      ** the outer INSERT and the data cursor should be pointing at the row
      ** that is to be updated.  So bypass the code that searches for the
      ** row(s) to be updated.
      */
      w_info = 0;
      one_pass = ONEPASS_SINGLE;
      __expr_if_false(parser, where, label_break, DBSQL_JUMPIFNULL);
      finish_seek = 0;
    }else{
      /* Begin the database scan.
      **
      ** Do not consider a single-pass strategy for a multi-row update if
      ** there is anything that might disrupt the cursor being used to do
      ** the UPDATE:
      **   (1) This is a nested UPDATE
      **   (2) There are triggers
      **   (3) There are FOREIGN KEY constraints
      **   (4) There are REPLACE conflict handlers
      **   (5) There are subqueries in the WHERE clause
      */
      flags = WHERE_ONEPASS_DESIRED;
      if( !parser->nested
       && !P_TRIGGER
       && !has_fk
       && !chng_key
       && !replace
       && (where==0 || !EXPR_HAS_PROPERTY(where, EP_Subquery))
      ){
        flags |= WHERE_ONEPASS_MULTIROW;
      }
      w_info = __where_begin(parser, tab_list, where,0,0,0,flags,i_idx_cur);
      if( w_info==0 ) goto update_cleanup;

      /* A one-pass strategy that might update more than one row may not
      ** be used if any column of the index used for the scan is being
      ** updated. Otherwise, if there is an index on "b", statements like
      ** the following could create an infinite loop:
      **
      **   UPDATE t1 SET b=b+1 WHERE b>?
      **
      ** Fall back to ONEPASS_OFF if where.c has selected a ONEPASS_MULTI
      ** strategy that uses an index for which one or more columns are being
      ** updated.  */
      one_pass = __where_ok_one_pass(w_info, cur_one_passes);
      finish_seek = __where_uses_deferred_seek(w_info);
      if( one_pass!=ONEPASS_SINGLE ){
        __multi_write(parser);
        if( one_pass==ONEPASS_MULTI ){
          int i_cur = cur_one_passes[1];
          if( i_cur>=0 && i_cur!=i_data_cur && to_opens[i_cur-i_base_cur] ){
            one_pass = ONEPASS_OFF;
          }
          assert( i_cur!=i_data_cur || !HAS_ROWID(tab) );
        }
      }
    }

    if( HAS_ROWID(tab) ){
      /* Read the rowid of the current row of the WHERE scan. In ONEPASS_OFF
      ** mode, write the rowid into the FIFO. In either of the one-pass modes,
      ** leave it in register reg_old_rowid.  */
      __vdbe_add_op2(v, OP_Rowid, i_data_cur, reg_old_rowid);
      if( one_pass==ONEPASS_OFF ){
        reg_idx[n_all_idx] = ++parser->n_mem;
        __vdbe_add_op3(v, OP_Insert, i_eph, reg_row_set, reg_old_rowid);
      }else{
        if( ALWAYS(addr_open) ) __vdbe_change_to_noop(v, addr_open);
      }
    }else{
      /* Read the PK of the current row into an array of registers. In
      ** ONEPASS_OFF mode, serialize the array into a record and store it in
      ** the ephemeral table. Or, in ONEPASS_SINGLE or MULTI mode, change
      ** the OP_OpenEphemeral instruction to a Noop (the ephemeral table
      ** is not required) and leave the PK fields in the array of registers.  */
      for(i=0; i<n_pk; i++){
        assert( pk->columns[i]>=0 );
        __expr_code_get_column_of_table(v, tab, i_data_cur,
                                        pk->columns[i], i_pk+i);
      }
      if( one_pass ){
        if( addr_open ) __vdbe_change_to_noop(v, addr_open);
        n_key = n_pk;
        reg_key = i_pk;
      }else{
        __vdbe_add_op4(v, OP_MakeRecord, i_pk, n_pk, reg_key,
                          __index_affinity_str(db, pk), n_pk);
        __vdbe_add_op4_int(v, OP_IdxInsert, i_eph, reg_key, i_pk, n_pk);
      }
    }
  }

  if( upsert==0 ){
    if( n_change_from==0 && one_pass!=ONEPASS_MULTI ){
      __where_end(w_info);
    }

    if( !isView ){
      int addr_once = 0;
      int i_not_used1 = 0;
      int i_not_used2 = 0;

      /* Open every index that needs updating. */
      if( one_pass!=ONEPASS_OFF ){
        if( cur_one_passes[0]>=0 ) to_opens[cur_one_passes[0]-i_base_cur] = 0;
        if( cur_one_passes[1]>=0 ) to_opens[cur_one_passes[1]-i_base_cur] = 0;
      }

      if( one_pass==ONEPASS_MULTI && (n_idx-(cur_one_passes[1]>=0))>0 ){
        addr_once = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
      }
      __open_table_and_indices(parser, tab, OP_OpenWrite, 0, i_base_cur,
                                 to_opens, &i_not_used1, &i_not_used2);
      if( addr_once ){
        __vdbe_jump_here_or_pop_inst(v, addr_once);
      }
    }

    /* Top of the update loop */
    if( one_pass!=ONEPASS_OFF ){
      if( cur_one_passes[0]!=i_data_cur
       && cur_one_passes[1]!=i_data_cur
#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
       && !isView
#endif
      ){
        assert( pk );
        __vdbe_add_op4_int(v, OP_NotFound, i_data_cur, label_break, reg_key,n_key);
        VDBE_COVERAGE(v);
      }
      if( one_pass!=ONEPASS_SINGLE ){
        label_continue = __vdbe_make_label(parser);
      }
      __vdbe_add_op2(v, OP_IsNull, pk ? reg_key : reg_old_rowid, label_break);
      VDBE_COVERAGE_IF(v, pk==0);
      VDBE_COVERAGE_IF(v, pk!=0);
    }else if( pk || n_change_from ){
      label_continue = __vdbe_make_label(parser);
      __vdbe_add_op2(v, OP_Rewind, i_eph, label_break); VDBE_COVERAGE(v);
      addr_top = __vdbe_current_addr(v);
      if( n_change_from ){
        if( !isView ){
          if( pk ){
            for(i=0; i<n_pk; i++){
              __vdbe_add_op3(v, OP_Column, i_eph, i, i_pk+i);
            }
            __vdbe_add_op4_int(
                v, OP_NotFound, i_data_cur, label_continue, i_pk, n_pk
            ); VDBE_COVERAGE(v);
          }else{
            __vdbe_add_op2(v, OP_Rowid, i_eph, reg_old_rowid);
            __vdbe_add_op3(
                v, OP_NotExists, i_data_cur, label_continue, reg_old_rowid
            ); VDBE_COVERAGE(v);
          }
        }
      }else{
        __vdbe_add_op2(v, OP_RowData, i_eph, reg_key);
        __vdbe_add_op4_int(v, OP_NotFound, i_data_cur, label_continue, reg_key,0);
        VDBE_COVERAGE(v);
      }
    }else{
      __vdbe_add_op2(v, OP_Rewind, i_eph, label_break); VDBE_COVERAGE(v);
      label_continue = __vdbe_make_label(parser);
      addr_top = __vdbe_add_op2(v, OP_Rowid, i_eph, reg_old_rowid);
      VDBE_COVERAGE(v);
      __vdbe_add_op3(v, OP_NotExists, i_data_cur, label_continue, reg_old_rowid);
      VDBE_COVERAGE(v);
    }
  }

  /* If the rowid value will change, set register reg_new_rowid to
  ** contain the new value. If the rowid is not being modified,
  ** then reg_new_rowid is the same register as reg_old_rowid, which is
  ** already populated.  */
  assert( chng_key || P_TRIGGER || has_fk || reg_old_rowid==reg_new_rowid );
  if( chng_rowid ){
    assert( i_rowid_expr>=0 );
    if( n_change_from==0 ){
      __expr_code(parser, rowid_expr, reg_new_rowid);
    }else{
      __vdbe_add_op3(v, OP_Column, i_eph, i_rowid_expr, reg_new_rowid);
    }
    __vdbe_add_op1(v, OP_MustBeInt, reg_new_rowid); VDBE_COVERAGE(v);
  }

  /* Compute the old pre-UPDATE content of the row being changed, if that
  ** information is needed */
  if( chng_pk || has_fk || P_TRIGGER ){
    u_int32_t oldmask = (has_fk ? __fk_oldmask(parser, tab) : 0);
    oldmask |= __trigger_colmask(parser,
        P_TRIGGER, changes, 0, TRIGGER_BEFORE|TRIGGER_AFTER, tab, on_error
    );
    for(i=0; i<tab->n_col; i++){
      u_int32_t col_flags = tab->cols[i].col_flags;
      k = __table_column_to_storage(tab, i) + reg_old;
      if( oldmask==0xffffffff
       || (i<32 && (oldmask & MASKBIT32(i))!=0)
       || (col_flags & COLFLAG_PRIMKEY)!=0
      ){
        testcase(  oldmask!=0xffffffff && i==31 );
        __expr_code_get_column_of_table(v, tab, i_data_cur, i, k);
      }else{
        __vdbe_add_op2(v, OP_Null, 0, k);
      }
    }
    if( chng_rowid==0 && pk==0 ){
#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
      if( isView ) __vdbe_add_op2(v, OP_Null, 0, reg_old_rowid);
#endif
      __vdbe_add_op2(v, OP_Copy, reg_old_rowid, reg_new_rowid);
    }
  }

  /* Populate the array of registers beginning at reg_new with the new
  ** row data. This array is used to check constants, create the new
  ** table and index records, and as the values for any new.* references
  ** made by triggers.
  **
  ** If there are one or more BEFORE triggers, then do not populate the
  ** registers associated with columns that are (a) not modified by
  ** this UPDATE statement and (b) not accessed by new.* references. The
  ** values for registers not modified by the UPDATE must be reloaded from
  ** the database after the BEFORE triggers are fired anyway (as the trigger
  ** may have modified them). So not loading those that are not going to
  ** be used eliminates some redundant opcodes.
  */
  newmask = __trigger_colmask(
      parser, P_TRIGGER, changes, 1, TRIGGER_BEFORE, tab, on_error
  );
  for(i=0, k=reg_new; i<tab->n_col; i++, k++){
    if( i==tab->i_p_key ){
      __vdbe_add_op2(v, OP_Null, 0, k);
    }else if( (tab->cols[i].col_flags & COLFLAG_GENERATED)!=0 ){
      if( tab->cols[i].col_flags & COLFLAG_VIRTUAL ) k--;
    }else{
      j = x_refs[i];
      if( j>=0 ){
        if( n_change_from ){
          int n_off = (isView ? tab->n_col : n_pk);
          assert( one_pass==ONEPASS_OFF );
          __vdbe_add_op3(v, OP_Column, i_eph, n_off+j, k);
        }else{
          __expr_code(parser, changes->a[j].expr, k);
        }
      }else if( 0==(tmask&TRIGGER_BEFORE) || i>31 || (newmask & MASKBIT32(i)) ){
        /* This branch loads the value of a column that will not be changed
        ** into a register. This is done if there are no BEFORE triggers, or
        ** if there are one or more BEFORE triggers that use this value via
        ** a new.* reference in a trigger program.
        */
        testcase( i==31 );
        testcase( i==32 );
        __expr_code_get_column_of_table(v, tab, i_data_cur, i, k);
        finish_seek = 0;
      }else{
        __vdbe_add_op2(v, OP_Null, 0, k);
      }
    }
  }
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
  if( tab->tab_flags & TF_HasGenerated ){
    testcase( tab->tab_flags & TF_HasVirtual );
    testcase( tab->tab_flags & TF_HasStored );
    __compute_generated_columns(parser, reg_new, tab);
  }
#endif

  /* Fire any BEFORE UPDATE triggers. This happens before constraints are
  ** verified. One could argue that this is wrong.
  */
  if( tmask&TRIGGER_BEFORE ){
    __table_affinity(v, tab, reg_new);
    __code_row_trigger(parser, P_TRIGGER, TK_UPDATE, changes,
        TRIGGER_BEFORE, tab, reg_old_rowid, on_error, label_continue);

    if( !isView ){
      /* The row-trigger may have deleted the row being updated. In this
      ** case, jump to the next row. No updates or AFTER triggers are
      ** required. This behavior - what happens when the row being updated
      ** is deleted or renamed by a BEFORE trigger - is left undefined in the
      ** documentation.
      */
      if( pk ){
        __vdbe_add_op4_int(v, OP_NotFound,i_data_cur,label_continue,reg_key,n_key);
        VDBE_COVERAGE(v);
      }else{
        __vdbe_add_op3(v, OP_NotExists, i_data_cur, label_continue,reg_old_rowid);
        VDBE_COVERAGE(v);
      }

      /* After-BEFORE-trigger-reload-loop:
      ** If it did not delete it, the BEFORE trigger may still have modified
      ** some of the columns of the row being updated. Load the values for
      ** all columns not modified by the update statement into their registers
      ** in case this has happened. Only unmodified columns are reloaded.
      ** The values computed for modified columns use the values before the
      ** BEFORE trigger runs.  See test case trigger1-18.0 (added 2018-04-26)
      ** for an example.
      */
      for(i=0, k=reg_new; i<tab->n_col; i++, k++){
        if( tab->cols[i].col_flags & COLFLAG_GENERATED ){
          if( tab->cols[i].col_flags & COLFLAG_VIRTUAL ) k--;
        }else if( x_refs[i]<0 && i!=tab->i_p_key ){
          __expr_code_get_column_of_table(v, tab, i_data_cur, i, k);
        }
      }
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
      if( tab->tab_flags & TF_HasGenerated ){
        testcase( tab->tab_flags & TF_HasVirtual );
        testcase( tab->tab_flags & TF_HasStored );
        __compute_generated_columns(parser, reg_new, tab);
      }
#endif
    }
  }

  if( !isView ){
    /* Do constraint checks. */
    assert( reg_old_rowid>0 );
    __generate_constraint_checks(parser, tab, reg_idx, i_data_cur, i_idx_cur,
        reg_new_rowid, reg_old_rowid, chng_key, on_error, label_continue, &replace,
        x_refs, 0);

    /* If REPLACE conflict handling may have been used, or if the PK of the
    ** row is changing, then the __generate_constraint_checks() above may have
    ** moved cursor i_data_cur. Reseek it. */
    if( replace || chng_key ){
      if( pk ){
        __vdbe_add_op4_int(v, OP_NotFound,i_data_cur,label_continue,reg_key,n_key);
      }else{
        __vdbe_add_op3(v, OP_NotExists, i_data_cur, label_continue,reg_old_rowid);
      }
      VDBE_COVERAGE(v);
    }

    /* Do FK constraint checks. */
    if( has_fk ){
      __fk_check(parser, tab, reg_old_rowid, 0, x_refs, chng_key);
    }

    /* Delete the index entries associated with the current record.  */
    __generate_row_index_delete(parser, tab, i_data_cur, i_idx_cur, reg_idx, -1);

    /* We must run the OP_FinishSeek opcode to resolve a prior
    ** OP_DeferredSeek if there is any possibility that there have been
    ** no OP_Column opcodes since the OP_DeferredSeek was issued.  But
    ** we want to avoid the OP_FinishSeek if possible, as running it
    ** costs CPU cycles. */
    if( finish_seek ){
      __vdbe_add_op1(v, OP_FinishSeek, i_data_cur);
    }

    /* If changing the rowid value, or if there are foreign key constraints
    ** to process, delete the old record. Otherwise, add a noop OP_Delete
    ** to invoke the pre-update hook.
    **
    ** That (reg_new==regnew_rowid+1) is true is also important for the
    ** pre-update hook. If the caller invokes preupdate_new(), the returned
    ** value is copied from memory cell (reg_new_rowid+1+i_col), where i_col
    ** is the column index supplied by the user.
    */
    assert( reg_new==reg_new_rowid+1 );
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
    __vdbe_add_op3(v, OP_Delete, i_data_cur,
        OPFLAG_ISUPDATE | ((has_fk>1 || chng_key) ? 0 : OPFLAG_ISNOOP),
        reg_new_rowid
    );
    if( one_pass==ONEPASS_MULTI ){
      assert( has_fk==0 && chng_key==0 );
      __vdbe_change_p5(v, OPFLAG_SAVEPOSITION);
    }
    if( !parser->nested ){
      __vdbe_append_p4(v, tab, P4_TABLE);
    }
#else
    if( has_fk>1 || chng_key ){
      __vdbe_add_op2(v, OP_Delete, i_data_cur, 0);
    }
#endif

    if( has_fk ){
      __fk_check(parser, tab, 0, reg_new_rowid, x_refs, chng_key);
    }

    /* Insert the new index entries and the new record. */
    __complete_insertion(
        parser, tab, i_data_cur, i_idx_cur, reg_new_rowid, reg_idx,
        OPFLAG_ISUPDATE | (one_pass==ONEPASS_MULTI ? OPFLAG_SAVEPOSITION : 0),
        0, 0
    );

    /* Do any ON CASCADE, SET NULL or SET DEFAULT operations required to
    ** handle rows (possibly in other tables) that refer via a foreign key
    ** to the row just updated. */
    if( has_fk ){
      __fk_actions(parser, tab, changes, reg_old_rowid, x_refs, chng_key);
    }
  }

  /* Increment the row counter
  */
  if( reg_row_count ){
    __vdbe_add_op2(v, OP_AddImm, reg_row_count, 1);
  }

  if( P_TRIGGER ){
    __code_row_trigger(parser, P_TRIGGER, TK_UPDATE, changes,
        TRIGGER_AFTER, tab, reg_old_rowid, on_error, label_continue);
  }

  /* Repeat the above with the next record to be updated, until
  ** all record selected by the WHERE clause have been updated.
  */
  if( one_pass==ONEPASS_SINGLE ){
    /* Nothing to do at end-of-loop for a single-pass */
  }else if( one_pass==ONEPASS_MULTI ){
    __vdbe_resolve_label(v, label_continue);
    __where_end(w_info);
  }else{
    __vdbe_resolve_label(v, label_continue);
    __vdbe_add_op2(v, OP_Next, i_eph, addr_top); VDBE_COVERAGE(v);
  }
  __vdbe_resolve_label(v, label_break);

  /* Update the dbsql_sequence table by storing the content of the
  ** maximum rowid counter values recorded while inserting into
  ** autoincrement tables.
  */
  if( parser->nested==0 && parser->trigger_tab==0 && upsert==0 ){
    __autoincrement_end(parser);
  }

  /*
  ** Return the number of rows that were changed, if we are tracking
  ** that information.
  */
  if( reg_row_count ){
    __code_change_count(v, reg_row_count, "rows updated");
  }

update_cleanup:
  __auth_context_pop(&s_context);
  __dbsql_db_free(db, x_refs); /* Also frees reg_idx[] and to_opens[] */
  __src_list_delete(db, tab_list);
  __expr_list_delete(db, changes);
  __expr_delete(db, where);
#if defined(DBSQL_ENABLE_UPDATE_DELETE_LIMIT)
  __expr_list_delete(db, order_by);
  __expr_delete(db, limit);
#endif
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

#ifndef DBSQL_OMIT_VIRTUALTABLE
/*
** Generate code for an UPDATE of a virtual table.
**
** There are two possible strategies - the default and the special
** "onepass" strategy. Onepass is only used if the virtual table
** implementation indicates that where may match at most one row.
**
** The default strategy is to create an ephemeral table that contains
** for each row to be changed:
**
**   (A)  The original rowid of that row.
**   (B)  The revised rowid for the row.
**   (C)  The content of every column in the row.
**
** Then loop through the contents of this ephemeral table executing a
** VUpdate for each row. When finished, drop the ephemeral table.
**
** The "onepass" strategy does not use an ephemeral table. Instead, it
** stores the same values (A, B and C above) in a register array and
** makes a single invocation of VUpdate.
*/
static void __update_virtual_table(
  parser_t *parser,       /* The parsing context */
  src_list_t *src,       /* The virtual table to be modified */
  table_t *tab,         /* The virtual table */
  expr_list_t *changes,  /* The columns to change in the UPDATE statement */
  expr_t *rowid,        /* Expression used to recompute the rowid */
  int *x_refs,          /* Mapping from columns of tab to entries in changes */
  expr_t *where,        /* WHERE clause of the UPDATE statement */
  int on_error          /* ON CONFLICT strategy */
){
  vdbe_t *v = parser->vdbe;  /* Virtual machine under construction */
  int ephem_tab;             /* table_t holding the result of the SELECT */
  int i;                    /* Loop counter */
  DBSQL *db = parser->db; /* Database connection */
  const char *v_tab = (const char*)__get_v_table(db, tab);
  where_info_t *w_info = 0;
  int n_arg = 2 + tab->n_col;      /* Number of arguments to VUpdate */
  int reg_arg;                     /* First register in VUpdate arg array */
  int reg_rec;                     /* Register in which to assemble record */
  int reg_rowid;                   /* Register for ephemeral table rowid */
  int i_csr = src->a[0].i_cursor;  /* Cursor used for virtual table scan */
  int dummies[2];                  /* Unused arg for __where_ok_one_pass() */
  int one_pass;                   /* True to use onepass strategy */
  int addr;                       /* Address of OP_OpenEphemeral */

  /* Allocate n_arg registers in which to gather the arguments for VUpdate. Then
  ** create and open the ephemeral table in which the records created from
  ** these arguments will be temporarily stored. */
  assert( v );
  ephem_tab = parser->n_tab++;
  addr= __vdbe_add_op2(v, OP_OpenEphemeral, ephem_tab, n_arg);
  reg_arg = parser->n_mem + 1;
  parser->n_mem += n_arg;
  if( src->n_src>1 ){
    index_t *pk = 0;
    expr_t *row;
    expr_list_t *list;
    if( HAS_ROWID(tab) ){
      if( rowid ){
        row = __expr_dup(db, rowid, 0);
      }else{
        row = __p_expr(parser, TK_ROW, 0, 0);
      }
    }else{
      int16_t i_pk;      /* PRIMARY KEY column */
      pk = __primary_key_index(tab);
      assert( pk!=0 );
      assert( pk->n_key_col==1 );
      i_pk = pk->columns[0];
      if( x_refs[i_pk]>=0 ){
        row = __expr_dup(db, changes->a[x_refs[i_pk]].expr, 0);
      }else{
        row = __expr_row_column(parser, i_pk);
      }
    }
    list = __expr_list_append(parser, 0, row);

    for(i=0; i<tab->n_col; i++){
      if( x_refs[i]>=0 ){
        list = __expr_list_append(parser, list,
          __expr_dup(db, changes->a[x_refs[i]].expr, 0)
        );
      }else{
        expr_t *row_expr = __expr_row_column(parser, i);
        if( row_expr ) row_expr->op2 = OPFLAG_NOCHNG;
        list = __expr_list_append(parser, list, row_expr);
      }
    }

    __update_from_select(parser, ephem_tab, pk, list, src, where, 0, 0);
    __expr_list_delete(db, list);
    one_pass = ONEPASS_OFF;
  }else{
    reg_rec = ++parser->n_mem;
    reg_rowid = ++parser->n_mem;

    /* Start scanning the virtual table */
    w_info = __where_begin(
        parser, src, where, 0, 0, 0, WHERE_ONEPASS_DESIRED, 0
    );
    if( w_info==0 ) return;

    /* Populate the argument registers. */
    for(i=0; i<tab->n_col; i++){
      assert( (tab->cols[i].col_flags & COLFLAG_GENERATED)==0 );
      if( x_refs[i]>=0 ){
        __expr_code(parser, changes->a[x_refs[i]].expr, reg_arg+2+i);
      }else{
        __vdbe_add_op3(v, OP_VColumn, i_csr, i, reg_arg+2+i);
        __vdbe_change_p5(v, OPFLAG_NOCHNG);/* For dbsql_vtab_nochange() */
      }
    }
    if( HAS_ROWID(tab) ){
      __vdbe_add_op2(v, OP_Rowid, i_csr, reg_arg);
      if( rowid ){
        __expr_code(parser, rowid, reg_arg+1);
      }else{
        __vdbe_add_op2(v, OP_Rowid, i_csr, reg_arg+1);
      }
    }else{
      index_t *pk;   /* PRIMARY KEY index */
      int16_t i_pk;      /* PRIMARY KEY column */
      pk = __primary_key_index(tab);
      assert( pk!=0 );
      assert( pk->n_key_col==1 );
      i_pk = pk->columns[0];
      __vdbe_add_op3(v, OP_VColumn, i_csr, i_pk, reg_arg);
      __vdbe_add_op2(v, OP_SCopy, reg_arg+2+i_pk, reg_arg+1);
    }

    one_pass = __where_ok_one_pass(w_info, dummies);

    /* There is no ONEPASS_MULTI on virtual tables */
    assert( one_pass==ONEPASS_OFF || one_pass==ONEPASS_SINGLE );

    if( one_pass ){
      /* If using the onepass strategy, no-op out the OP_OpenEphemeral coded
      ** above. */
      __vdbe_change_to_noop(v, addr);
      __vdbe_add_op1(v, OP_Close, i_csr);
    }else{
      /* Create a record from the argument register contents and insert it into
      ** the ephemeral table. */
      __multi_write(parser);
      __vdbe_add_op3(v, OP_MakeRecord, reg_arg, n_arg, reg_rec);
#if defined(DBSQL_DEBUG) && !defined(DBSQL_ENABLE_NULL_TRIM)
      /* Signal an assert() within OP_MakeRecord that it is allowed to
      ** accept no-change records with serial_type 10 */
      __vdbe_change_p5(v, OPFLAG_NOCHNG_MAGIC);
#endif
      __vdbe_add_op2(v, OP_NewRowid, ephem_tab, reg_rowid);
      __vdbe_add_op3(v, OP_Insert, ephem_tab, reg_rec, reg_rowid);
    }
  }


  if( one_pass==ONEPASS_OFF ){
    /* End the virtual table scan */
    if( src->n_src==1 ){
      __where_end(w_info);
    }

    /* Begin scanning through the ephemeral table. */
    addr = __vdbe_add_op1(v, OP_Rewind, ephem_tab); VDBE_COVERAGE(v);

    /* Extract arguments from the current row of the ephemeral table and
    ** invoke the VUpdate method.  */
    for(i=0; i<n_arg; i++){
      __vdbe_add_op3(v, OP_Column, ephem_tab, i, reg_arg+i);
    }
  }
  __vtab_make_writable(parser, tab);
  __vdbe_add_op4(v, OP_VUpdate, 0, n_arg, reg_arg, v_tab, P4_VTAB);
  __vdbe_change_p5(v, on_error==OE_Default ? OE_Abort : on_error);
  __may_abort(parser);

  /* End of the ephemeral table scan. Or, if using the onepass strategy,
  ** jump to here if the scan visited zero rows. */
  if( one_pass==ONEPASS_OFF ){
    __vdbe_add_op2(v, OP_Next, ephem_tab, addr+1); VDBE_COVERAGE(v);
    __vdbe_jump_here(v, addr);
    __vdbe_add_op2(v, OP_Close, ephem_tab, 0);
  }else{
    __where_end(w_info);
  }
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */
