/*
** 2015-06-06
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This module contains C code that generates VDBE code used to process
** the WHERE clause of SQL statements.
**
** This file was split off from where.c on 2015-06-06 in order to reduce the
** size of where.c and make it easier to edit.  This file contains the routines
** that actually generate the bulk of the WHERE loop code.  The original where.c
** file retains the code that does query planning and analysis.
*/
#include "dbsql_int.h"
#include "where_int.h"

#ifndef DBSQL_OMIT_EXPLAIN

/*
** Return the name of the i-th column of the idx index.
*/
static const char *__explain_index_column_name(index_t *idx, int i){
  i = idx->columns[i];
  if( i==XN_EXPR ) return "<expr>";
  if( i==XN_ROWID ) return "rowid";
  return idx->table->cols[i].cn_name;
}

/*
** This routine is a helper for __explain_index_range() below
**
** str holds the text of an expression that we are building up one term
** at a time.  This routine adds a new term to the end of the expression.
** Terms are separated by AND so add the "AND" text for second and subsequent
** terms only.
*/
static void __explain_append_term(
  str_accum_t *str,             /* The text expression being built */
  index_t *idx,                /* index_t to read column names from */
  int n_term,                  /* Number of terms */
  int i_term,                  /* Zero-based index of first term. */
  int and,                   /* Non-zero to append " AND " */
  const char *op             /* Name of the operator */
){
  int i;

  assert( n_term>=1 );
  if( and ) dbsql_str_append(str, " AND ", 5);

  if( n_term>1 ) dbsql_str_append(str, "(", 1);
  for(i=0; i<n_term; i++){
    if( i ) dbsql_str_append(str, ",", 1);
    dbsql_str_appendall(str, __explain_index_column_name(idx, i_term+i));
  }
  if( n_term>1 ) dbsql_str_append(str, ")", 1);

  dbsql_str_append(str, op, 1);

  if( n_term>1 ) dbsql_str_append(str, "(", 1);
  for(i=0; i<n_term; i++){
    if( i ) dbsql_str_append(str, ",", 1);
    dbsql_str_append(str, "?", 1);
  }
  if( n_term>1 ) dbsql_str_append(str, ")", 1);
}

/*
** Argument level describes a strategy for scanning table tab. This
** function appends text to str that describes the subset of table
** rows scanned by the strategy in the form of an SQL expression.
**
** For example, if the query:
**
**   SELECT * FROM t1 WHERE a=1 AND b>2;
**
** is run and there is an index on (a, b), then this function returns a
** string similar to:
**
**   "a=? AND b>?"
*/
static void __explain_index_range(str_accum_t *str, where_loop_t *loop){
  index_t *p_index = loop->u.btree.p_index;
  u_int16_t n_eq = loop->u.btree.n_eq;
  u_int16_t n_skip = loop->n_skip;
  int i, j;

  if( n_eq==0 && (loop->ws_flags&(WHERE_BTM_LIMIT|WHERE_TOP_LIMIT))==0 ) return;
  dbsql_str_append(str, " (", 2);
  for(i=0; i<n_eq; i++){
    const char *z = __explain_index_column_name(p_index, i);
    if( i ) dbsql_str_append(str, " AND ", 5);
    dbsql_str_appendf(str, i>=n_skip ? "%s=?" : "ANY(%s)", z);
  }

  j = i;
  if( loop->ws_flags&WHERE_BTM_LIMIT ){
    __explain_append_term(str, p_index, loop->u.btree.n_btm, j, i, ">");
    i = 1;
  }
  if( loop->ws_flags&WHERE_TOP_LIMIT ){
    __explain_append_term(str, p_index, loop->u.btree.n_top, j, i, "<");
  }
  dbsql_str_append(str, ")", 1);
}

/*
** This function sets the P4 value of an existing OP_Explain opcode to
** text describing the loop in level. If the OP_Explain opcode already has
** a P4 value, it is freed before it is overwritten.
*/
void __where_add_explain_text(
  parser_t *parser,                  /* parser_t context */
  int addr,                       /* Address of OP_Explain opcode */
  src_list_t *tab_list,              /* table_t list this loop refers to */
  where_level_t *level,             /* Scan to write OP_Explain opcode for */
  u_int16_t wctrl_flags                  /* Flags passed to __where_begin() */
){
#if !defined(DBSQL_DEBUG)
  if( __parse_toplevel(parser)->explain==2 || IS_STMT_SCANSTATUS(parser->db) )
#endif
  {
    vdbe_op_t *op = __vdbe_get_op(parser->vdbe, addr);
    src_item_t *item = &tab_list->a[level->i_from];
    DBSQL *db = parser->db;     /* Database handle */
    int is_search;                 /* True for a SEARCH. False for SCAN. */
    where_loop_t *loop;             /* The controlling where_loop_t object */
    u_int32_t flags;                    /* Flags that describe this loop */
#if defined(DBSQL_DEBUG) && !defined(DBSQL_OMIT_EXPLAIN)
    char *msg;                   /* Text to add to EQP output */
#endif
    str_accum_t str;                 /* EQP output string */
    char buf[100];               /* Initial space for EQP output string */

    if( db->malloc_failed ) return;

    loop = level->w_loop;
    flags = loop->ws_flags;

    is_search = (flags&(WHERE_BTM_LIMIT|WHERE_TOP_LIMIT))!=0
            || ((flags&WHERE_VIRTUALTABLE)==0 && (loop->u.btree.n_eq>0))
            || (wctrl_flags&(WHERE_ORDERBY_MIN|WHERE_ORDERBY_MAX));

    __str_accum_init(&str, db, buf, sizeof(buf), DBSQL_MAX_LENGTH);
    str.printf_flags = DBSQL_PRINTF_INTERNAL;
    dbsql_str_appendf(&str, "%s %S%s",
       is_search ? "SEARCH" : "SCAN",
       item,
       item->fg.from_exists ? " EXISTS" : "");
    if( (flags & (WHERE_IPK|WHERE_VIRTUALTABLE))==0 ){
      const char *fmt = 0;
      index_t *idx;

      assert( loop->u.btree.p_index!=0 );
      idx = loop->u.btree.p_index;
      assert( !(flags&WHERE_AUTO_INDEX) || (flags&WHERE_IDX_ONLY) );
      if( !HAS_ROWID(item->s_tab) && IS_PRIMARY_KEY_INDEX(idx) ){
        if( is_search ){
          fmt = "PRIMARY KEY";
        }
      }else if( flags & WHERE_PARTIALIDX ){
        fmt = "AUTOMATIC PARTIAL COVERING INDEX";
      }else if( flags & WHERE_AUTO_INDEX ){
        fmt = "AUTOMATIC COVERING INDEX";
      }else if( flags & (WHERE_IDX_ONLY|WHERE_EXPRIDX) ){
        fmt = "COVERING INDEX %s";
      }else{
        fmt = "INDEX %s";
      }
      if( fmt ){
        dbsql_str_append(&str, " USING ", 7);
        dbsql_str_appendf(&str, fmt, idx->name);
        __explain_index_range(&str, loop);
      }
    }else if( (flags & WHERE_IPK)!=0 && (flags & WHERE_CONSTRAINT)!=0 ){
      char c_range_op;
#if 0  /* Better output, but breaks many tests */
      const table_t *tab = item->tab;
      const char *rowid = tab->i_p_key>=0 ? tab->cols[tab->i_p_key].cn_name:
                              "rowid";
#else
      const char *rowid = "rowid";
#endif
      dbsql_str_appendf(&str, " USING INTEGER PRIMARY KEY (%s", rowid);
      if( flags&(WHERE_COLUMN_EQ|WHERE_COLUMN_IN) ){
        c_range_op = '=';
      }else if( (flags&WHERE_BOTH_LIMIT)==WHERE_BOTH_LIMIT ){
        dbsql_str_appendf(&str, ">? AND %s", rowid);
        c_range_op = '<';
      }else if( flags&WHERE_BTM_LIMIT ){
        c_range_op = '>';
      }else{
        assert( flags&WHERE_TOP_LIMIT);
        c_range_op = '<';
      }
      dbsql_str_appendf(&str, "%c?)", c_range_op);
    }
#ifndef DBSQL_OMIT_VIRTUALTABLE
    else if( (flags & WHERE_VIRTUALTABLE)!=0 ){
      dbsql_str_appendall(&str, " VIRTUAL TABLE INDEX ");
      dbsql_str_appendf(&str,
                  loop->u.vtab.idx_num_hex ? "0x%x:%s" : "%d:%s",
                  loop->u.vtab.idx_num, loop->u.vtab.idx_str);
    }
#endif
    if( item->fg.jointype & JT_LEFT ){
      dbsql_str_appendf(&str, " LEFT-JOIN");
    }
#ifdef DBSQL_EXPLAIN_ESTIMATED_ROWS
    if( loop->n_out>=10 ){
      dbsql_str_appendf(&str, " (~%llu rows)",
             __log_est_to_int(loop->n_out));
    }else{
      dbsql_str_append(&str, " (~1 row)", 9);
    }
#endif
#if defined(DBSQL_DEBUG) && !defined(DBSQL_OMIT_EXPLAIN)
    msg = __str_accum_finish(&str);
    __explain_breakpoint("",msg);
#endif

    assert( op->opcode==OP_Explain );
    assert( op->p4type==P4_DYNAMIC || op->p4.z==0 );
    __dbsql_db_free(db, op->p4.z);
    op->p4type = P4_DYNAMIC;
    op->p4.z = __str_accum_finish(&str);
  }
}


/*
** This function is a no-op unless currently processing an EXPLAIN QUERY PLAN
** command, or if stmt_scanstatus_v2() stats are enabled, or if DBSQL_DEBUG
** was defined at compile-time. If it is not a no-op, a single OP_Explain
** opcode is added to the output to describe the table scan strategy in level.
**
** If an OP_Explain opcode is added to the VM, its address is returned.
** Otherwise, if no OP_Explain is coded, zero is returned.
*/
int __where_explain_one_scan(
  parser_t *parser,                  /* parser_t context */
  src_list_t *tab_list,              /* table_t list this loop refers to */
  where_level_t *level,             /* Scan to write OP_Explain opcode for */
  u_int16_t wctrl_flags                  /* Flags passed to __where_begin() */
){
  int ret = 0;
#if !defined(DBSQL_DEBUG)
  if( __parse_toplevel(parser)->explain==2 || IS_STMT_SCANSTATUS(parser->db) )
#endif
  {
    if( (level->w_loop->ws_flags & WHERE_MULTI_OR)==0
     && (wctrl_flags & WHERE_OR_SUBCLAUSE)==0
    ){
      vdbe_t *v = parser->vdbe;
      int addr = __vdbe_current_addr(v);
      ret = __vdbe_add_op3(
          v, OP_Explain, addr, parser->addr_explain, level->w_loop->r_run
      );
      __where_add_explain_text(parser, addr, tab_list, level, wctrl_flags);
    }
  }
  return ret;
}

/*
** Add a single OP_Explain opcode that describes a Bloom filter.
**
** Or if not processing EXPLAIN QUERY PLAN and not in a DBSQL_DEBUG and/or
** DBSQL_ENABLE_STMT_SCANSTATUS build, then OP_Explain opcodes are not
** required and this routine is a no-op.
**
** If an OP_Explain opcode is added to the VM, its address is returned.
** Otherwise, if no OP_Explain is coded, zero is returned.
*/
int __where_explain_bloom_filter(
  const parser_t *parser,               /* parser_t context */
  const where_info_t *w_info,           /* WHERE clause */
  const where_level_t *level           /* Bloom filter on this level */
){
  int ret = 0;
  src_item_t *item = &w_info->tab_list->a[level->i_from];
  vdbe_t *v = parser->vdbe;      /* VM being constructed */
  DBSQL *db = parser->db;     /* Database handle */
  char *msg;                   /* Text to add to EQP output */
  int i;                        /* Loop counter */
  where_loop_t *loop;             /* The where loop */
  str_accum_t str;                 /* EQP output string */
  char buf[100];               /* Initial space for EQP output string */

  __str_accum_init(&str, db, buf, sizeof(buf), DBSQL_MAX_LENGTH);
  str.printf_flags = DBSQL_PRINTF_INTERNAL;
  dbsql_str_appendf(&str, "BLOOM FILTER ON %S (", item);
  loop = level->w_loop;
  if( loop->ws_flags & WHERE_IPK ){
    const table_t *tab = item->s_tab;
    if( tab->i_p_key>=0 ){
      dbsql_str_appendf(&str, "%s=?", tab->cols[tab->i_p_key].cn_name);
    }else{
      dbsql_str_appendf(&str, "rowid=?");
    }
  }else{
    for(i=loop->n_skip; i<loop->u.btree.n_eq; i++){
      const char *z = __explain_index_column_name(loop->u.btree.p_index, i);
      if( i>loop->n_skip ) dbsql_str_append(&str, " AND ", 5);
      dbsql_str_appendf(&str, "%s=?", z);
    }
  }
  dbsql_str_append(&str, ")", 1);
  msg = __str_accum_finish(&str);
  ret = __vdbe_add_op4(v, OP_Explain, __vdbe_current_addr(v),
                          parser->addr_explain, 0, msg,P4_DYNAMIC);

  __vdbe_scan_status(v, __vdbe_current_addr(v)-1, 0, 0, 0, 0);
  return ret;
}
#endif /* DBSQL_OMIT_EXPLAIN */

#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
/*
** Configure the VM passed as the first argument with an
** dbsql_stmt_scanstatus() entry corresponding to the scan used to
** implement level lvl. Argument srclist is a pointer to the FROM
** clause that the scan reads data from.
**
** If argument addr_explain is not 0, it must be the address of an
** OP_Explain instruction that describes the same loop.
*/
void __where_add_scan_status(
  vdbe_t *v,                        /* vdbe_t to add scanstatus entry to */
  src_list_t *srclist,              /* FROM clause lvl reads data from */
  where_level_t *lvl,               /* Level to add scanstatus() entry for */
  int addr_explain                 /* Address of OP_Explain (or 0) */
){
  if( IS_STMT_SCANSTATUS( __vdbe_db(v) ) ){
    const char *obj = 0;
    where_loop_t *loop = lvl->w_loop;
    int ws_flags = loop->ws_flags;
    int via_coroutine = 0;

    if( (ws_flags & WHERE_VIRTUALTABLE)==0  &&  loop->u.btree.p_index!=0 ){
      obj = loop->u.btree.p_index->name;
    }else{
      obj = srclist->a[lvl->i_from].name;
      via_coroutine = srclist->a[lvl->i_from].fg.via_coroutine;
    }
    __vdbe_scan_status(
        v, addr_explain, lvl->addr_body, lvl->addr_visit, loop->n_out, obj
    );

    if( via_coroutine==0 ){
      if( (ws_flags & (WHERE_MULTI_OR|WHERE_AUTO_INDEX))==0 ){
        __vdbe_scan_status_range(v, addr_explain, -1, lvl->i_tab_cur);
      }
      if( ws_flags & WHERE_INDEXED ){
        __vdbe_scan_status_range(v, addr_explain, -1, lvl->i_idx_cur);
      }
    }else{
      int addr;
      vdbe_op_t *op;
      assert( srclist->a[lvl->i_from].fg.is_subquery );
      addr = srclist->a[lvl->i_from].u4.subq->addr_fill_sub;
      op = __vdbe_get_op(v, addr-1);
      assert( __vdbe_db(v)->malloc_failed || op->opcode==OP_InitCoroutine );
      assert( __vdbe_db(v)->malloc_failed || op->p2>addr );
      __vdbe_scan_status_range(v, addr_explain, addr, op->p2-1);
    }
  }
}
#endif


/*
** Disable a term in the WHERE clause.  Except, do not disable the term
** if it controls a LEFT OUTER JOIN and it did not originate in the ON
** or USING clause of that join.
**
** Consider the term t2.z='ok' in the following queries:
**
**   (1)  SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.x WHERE t2.z='ok'
**   (2)  SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.x AND t2.z='ok'
**   (3)  SELECT * FROM t1, t2 WHERE t1.a=t2.x AND t2.z='ok'
**
** The t2.z='ok' is disabled in the in (2) because it originates
** in the ON clause.  The term is disabled in (3) because it is not part
** of a LEFT OUTER JOIN.  In (1), the term is not disabled.
**
** Disabling a term causes that term to not be tested in the inner loop
** of the join.  Disabling is an optimization.  When terms are satisfied
** by indices, we disable them to prevent redundant tests in the inner
** loop.  We would get the correct results if nothing were ever disabled,
** but joins might run a little slower.  The trick is to disable as much
** as we can without disabling too much.  If we disabled in (1), we'd get
** the wrong answer.  See ticket #813.
**
** If all the children of a term are disabled, then that term is also
** automatically disabled.  In this way, terms get disabled if derived
** virtual terms are tested first.  For example:
**
**      x GLOB 'abc*' AND x>='abc' AND x<'acd'
**      \___________/     \______/     \_____/
**         parent          child1       child2
**
** Only the parent term was in the original WHERE clause.  The child1
** and child2 terms were added by the LIKE optimization.  If both of
** the virtual child terms are valid, then testing of the parent can be
** skipped.
**
** Usually the parent term is marked as TERM_CODED.  But if the parent
** term was originally TERM_LIKE, then the parent gets TERM_LIKECOND instead.
** The TERM_LIKECOND marking indicates that the term should be coded inside
** a conditional such that is only evaluated on the second pass of a
** LIKE-optimization loop, when scanning BLOBs instead of strings.
*/
static void __disable_term(where_level_t *level, where_term_t *term){
  int n_loop = 0;
  assert( term!=0 );
  while( (term->wt_flags & TERM_CODED)==0
      && (level->i_left_join==0 || EXPR_HAS_PROPERTY(term->expr, EP_OuterON))
      && (level->not_ready & term->prereq_all)==0
  ){
    if( n_loop && (term->wt_flags & TERM_LIKE)!=0 ){
      term->wt_flags |= TERM_LIKECOND;
    }else{
      term->wt_flags |= TERM_CODED;
    }
#ifdef WHERETRACE_ENABLED
    if( (__where_trace & 0x4001)==0x4001 ){
      __debug_printf("DISABLE-");
      __where_term_print(term, (int)(term - (term->wc->a)));
    }
#endif
    if( term->i_parent<0 ) break;
    term = &term->wc->a[term->i_parent];
    assert( term!=0 );
    term->n_child--;
    if( term->n_child!=0 ) break;
    n_loop++;
  }
}

/*
** Code an OP_Affinity opcode to apply the column affinity string aff
** to the n registers starting at base.
**
** As an optimization, DBSQL_AFF_BLOB and DBSQL_AFF_NONE entries (which
** are no-ops) at the beginning and end of aff are ignored.  If all entries
** in aff are DBSQL_AFF_BLOB or DBSQL_AFF_NONE, then no code gets generated.
**
** This routine makes its own copy of aff so that the caller is free
** to modify aff after this routine returns.
*/
static void __code_apply_affinity(parser_t *parser, int base, int n, char *aff){
  vdbe_t *v = parser->vdbe;
  if( aff==0 ){
    assert( parser->db->malloc_failed );
    return;
  }
  assert( v!=0 );

  /* Adjust base and n to skip over DBSQL_AFF_BLOB and DBSQL_AFF_NONE
  ** entries at the beginning and end of the affinity string.
  */
  assert( DBSQL_AFF_NONE<DBSQL_AFF_BLOB );
  while( n>0 && aff[0]<=DBSQL_AFF_BLOB ){
    n--;
    base++;
    aff++;
  }
  while( n>1 && aff[n-1]<=DBSQL_AFF_BLOB ){
    n--;
  }

  /* Code the OP_Affinity opcode if there is anything left to do. */
  if( n>0 ){
    __vdbe_add_op4(v, OP_Affinity, base, n, 0, aff, n);
  }
}

/*
** Expression right, which is the RHS of a comparison operation, is
** either a vector of n elements or, if n==1, a scalar expression.
** Before the comparison operation, affinity aff is to be applied
** to the right values. This function modifies characters within the
** affinity string to DBSQL_AFF_BLOB if either:
**
**   * the comparison will be performed with no affinity, or
**   * the affinity change in aff is guaranteed not to change the value.
*/
static void __update_range_affinity_str(
  expr_t *right,                   /* RHS of comparison */
  int n,                          /* Number of vector elements in comparison */
  char *aff                      /* Affinity string to modify */
){
  int i;
  for(i=0; i<n; i++){
    expr_t *p = __vector_field_subexpr(right, i);
    if( __compare_affinity(p, aff[i])==DBSQL_AFF_BLOB
     || __expr_needs_no_affinity_change(p, aff[i])
    ){
      aff[i] = DBSQL_AFF_BLOB;
    }
  }
}

/*
** The order_by->a[].u.x.i_order_by_col values might be incorrect because
** columns might have been rearranged in the result set.  This routine
** fixes them up.
**
** e_list is the new result set.  The e_list->a[].u.x.i_order_by_col values
** contain the *old* locations of each expression.  This is a temporary
** use of u.x.i_order_by_col, not its intended use.  The caller must reset
** u.x.i_order_by_col back to zero for all entries in e_list before the
** caller returns.
**
** This routine changes order_by->a[].u.x.i_order_by_col values from
** e_list->a[N].u.x.i_order_by_col into N+1.  (The "+1" is because of the 1-based
** indexing used by i_order_by_col.)  Or if no match, i_order_by_col is set to zero.
*/
static void __adjust_order_by_col(expr_list_t *order_by, expr_list_t *e_list){
  int i, j;
  if( order_by==0 ) return;
  for(i=0; i<order_by->n_expr; i++){
    int t = order_by->a[i].u.x.i_order_by_col;
    if( t==0 ) continue;
    for(j=0; j<e_list->n_expr; j++){
      if( e_list->a[j].u.x.i_order_by_col==t ){
        order_by->a[i].u.x.i_order_by_col = j+1;
        break;
      }
    }
    if( j>=e_list->n_expr ){
      order_by->a[i].u.x.i_order_by_col = 0;
    }
  }
}


/*
** x is an expression of the form:  (vector) IN (SELECT ...)
** In other words, it is a vector IN operator with a SELECT clause on the
** RHS.  But not all terms in the vector are indexable and the terms might
** not be in the correct order for indexing.
**
** This routine makes a copy of the input x expression and then adjusts
** the vector on the LHS with corresponding changes to the SELECT so that
** the vector contains only index terms and those terms are in the correct
** order.  The modified IN expression is returned.  The caller is responsible
** for deleting the returned expression.
**
** Example:
**
**    CREATE TABLE t1(a,b,c,d,e,f);
**    CREATE INDEX t1x1 ON t1(e,c);
**    SELECT * FROM t1 WHERE (a,b,c,d,e) IN (SELECT v,w,x,y,z FROM t2)
**                           \_______________________________________/
**                                     The x expression
**
** Since only columns e and c can be used with the index, in that order,
** the modified IN expression that is returned will be:
**
**        (e,c) IN (SELECT z,x FROM t2)
**
** The reduced x is different from the original (obviously) and thus is
** only used for indexing, to improve performance.  The original unaltered
** IN expression must also be run on each output row for correctness.
*/
static expr_t *__remove_unindexable_in_clause_terms(
  parser_t *parser,        /* The parsing context */
  int i_eq,              /* Look at loop terms starting here */
  where_loop_t *loop,     /* The current loop */
  expr_t *x              /* The IN expression to be reduced */
){
  DBSQL *db = parser->db;
  select_t *p_select;            /* Pointer to the SELECT on the RHS */
  expr_t *p_new;
  p_new = __expr_dup(db, x, 0);
  if( db->malloc_failed==0 ){
    for(p_select=p_new->x.p_select; p_select; p_select=p_select->prior){
      expr_list_t *orig_rhs;         /* Original unmodified RHS */
      expr_list_t *orig_lhs = 0;     /* Original unmodified LHS */
      expr_list_t *rhs = 0;         /* New RHS after modifications */
      expr_list_t *lhs = 0;         /* New LHS after mods */
      int i;                      /* Loop counter */

      assert( EXPR_USE_X_SELECT(p_new) );
      orig_rhs = p_select->e_list;
      assert( p_new->left!=0 );
      assert( EXPR_USE_X_LIST(p_new->left) );
      if( p_select==p_new->x.p_select ){
        orig_lhs = p_new->left->x.list;
      }
      for(i=i_eq; i<loop->n_l_term; i++){
        if( loop->l_terms[i]->expr==x ){
          int i_field;
          assert( (loop->l_terms[i]->e_operator & (WO_OR|WO_AND))==0 );
          i_field = loop->l_terms[i]->u.x.i_field - 1;
          if( NEVER(orig_rhs->a[i_field].expr==0) ){
            continue; /* Duplicate PK column */
          }
          rhs = __expr_list_append(parser, rhs, orig_rhs->a[i_field].expr);
          orig_rhs->a[i_field].expr = 0;
          if( rhs ) rhs->a[rhs->n_expr-1].u.x.i_order_by_col = i_field+1;
          if( orig_lhs ){
            assert( orig_lhs->a[i_field].expr!=0 );
            lhs = __expr_list_append(parser,lhs,orig_lhs->a[i_field].expr);
            orig_lhs->a[i_field].expr = 0;
          }
        }
      }
      __expr_list_delete(db, orig_rhs);
      if( orig_lhs ){
        __expr_list_delete(db, orig_lhs);
        p_new->left->x.list = lhs;
      }
      p_select->e_list = rhs;
      p_select->sel_id = ++parser->n_select; /* Req'd for subrtn_sig_t validity */
      if( lhs && lhs->n_expr==1 ){
        /* Take care here not to generate a TK_VECTOR containing only a
        ** single value. Since the parser never creates such a vector, some
        ** of the subroutines do not handle this case.  */
        expr_t *p = lhs->a[0].expr;
        lhs->a[0].expr = 0;
        __expr_delete(db, p_new->left);
        p_new->left = p;
      }

      /* If either the ORDER BY clause or the GROUP BY clause contains
      ** references to result-set columns, those references might now be
      ** obsolete.  So fix them up.
      */
      assert( rhs!=0 || db->malloc_failed );
      if( rhs ){
        __adjust_order_by_col(p_select->order_by, rhs);
        __adjust_order_by_col(p_select->group_by, rhs);
        for(i=0; i<rhs->n_expr; i++) rhs->a[i].u.x.i_order_by_col = 0;
      }

#if 0
      printf("For indexing, change the IN expr:\n");
      __tree_view_expr(0, x, 0);
      printf("Into:\n");
      __tree_view_expr(0, p_new, 0);
#endif
    }
  }
  return p_new;
}


#ifndef DBSQL_OMIT_SUBQUERY
/*
** Generate code for a single X IN (....) term of the WHERE clause.
**
** This is a special-case of __code_equality_term() that works for IN operators
** only.  It is broken out into a subroutine because this case is
** uncommon and by splitting it off into a subroutine, the common case
** runs faster.
**
** The current value for the constraint is left in  register i_target.
** This routine sets up a loop that will iterate over all values of X.
*/
static DBSQL_NOINLINE void __code_in_term(
  parser_t *parser,      /* The parsing context */
  where_term_t *term,   /* The term of the WHERE clause to be coded */
  where_level_t *level, /* The level of the FROM clause we are working on */
  int i_eq,            /* index_t of the equality term within this level */
  int rev,           /* True for reverse-order IN operations */
  int i_target         /* Attempt to leave results in this register */
){
  expr_t *x = term->expr;
  int type = IN_INDEX_NOOP;
  int i_tab;
  struct in_loop_t *in;
  where_loop_t *loop = level->w_loop;
  vdbe_t *v = parser->vdbe;
  int i;
  int n_eq = 0;
  int *maps = 0;

  if( (loop->ws_flags & WHERE_VIRTUALTABLE)==0
    && loop->u.btree.p_index!=0
    && loop->u.btree.p_index->sort_orders[i_eq]
  ){
    testcase( i_eq==0 );
    testcase( rev );
    rev = !rev;
  }
  assert( x->op==TK_IN );

  for(i=0; i<i_eq; i++){
    if( loop->l_terms[i] && loop->l_terms[i]->expr==x ){
      __disable_term(level, term);
      return;
    }
  }
  for(i=i_eq; i<loop->n_l_term; i++){
    assert( loop->l_terms[i]!=0 );
    if( loop->l_terms[i]->expr==x ) n_eq++;
  }

  i_tab = 0;
  if( !EXPR_USE_X_SELECT(x) || x->x.p_select->e_list->n_expr==1 ){
    type = __find_in_index(parser, x, IN_INDEX_LOOP, 0, 0, &i_tab);
  }else{
    DBSQL *db = parser->db;
    expr_t *x_mod = __remove_unindexable_in_clause_terms(parser, i_eq, loop, x);
    if( !db->malloc_failed ){
      maps = (int*)__db_malloc_zero(db, sizeof(int)*n_eq);
      type = __find_in_index(parser, x_mod, IN_INDEX_LOOP, 0, maps, &i_tab);
    }
    __expr_delete(db, x_mod);
  }

  if( type==IN_INDEX_INDEX_DESC ){
    testcase( rev );
    rev = !rev;
  }
  __vdbe_add_op2(v, rev ? OP_Last : OP_Rewind, i_tab, 0);
  VDBE_COVERAGE_IF(v, rev);
  VDBE_COVERAGE_IF(v, !rev);

  assert( (loop->ws_flags & WHERE_MULTI_OR)==0 );
  loop->ws_flags |= WHERE_IN_ABLE;
  if( level->u.in.n_in==0 ){
    level->addr_nxt = __vdbe_make_label(parser);
  }
  if( i_eq>0 && (loop->ws_flags & WHERE_IN_SEEKSCAN)==0 ){
    loop->ws_flags |= WHERE_IN_EARLYOUT;
  }

  i = level->u.in.n_in;
  level->u.in.n_in += n_eq;
  level->u.in.in_loops =
     __where_realloc(term->wc->w_info,
                         level->u.in.in_loops,
                         sizeof(level->u.in.in_loops[0])*level->u.in.n_in);
  in = level->u.in.in_loops;
  if( in ){
    int i_map = 0;               /* index_t in maps[] */
    in += i;
    for(i=i_eq; i<loop->n_l_term; i++){
      if( loop->l_terms[i]->expr==x ){
        int i_out = i_target + i - i_eq;
        if( type==IN_INDEX_ROWID ){
          in->addr_in_top = __vdbe_add_op2(v, OP_Rowid, i_tab, i_out);
        }else{
          int i_col = maps ? maps[i_map++] : 0;
          in->addr_in_top = __vdbe_add_op3(v,OP_Column,i_tab, i_col, i_out);
        }
        __vdbe_add_op1(v, OP_IsNull, i_out); VDBE_COVERAGE(v);
        if( i==i_eq ){
          in->i_cur = i_tab;
          in->end_loop_op = rev ? OP_Prev : OP_Next;
          if( i_eq>0 ){
            in->i_base = i_target - i;
            in->n_prefix = i;
          }else{
            in->n_prefix = 0;
          }
        }else{
          in->end_loop_op = OP_Noop;
        }
        in++;
      }
    }
    testcase( i_eq>0
              && (loop->ws_flags & WHERE_IN_SEEKSCAN)==0
              && (loop->ws_flags & WHERE_VIRTUALTABLE)!=0 );
    if( i_eq>0
     && (loop->ws_flags & (WHERE_IN_SEEKSCAN|WHERE_VIRTUALTABLE))==0
    ){
      __vdbe_add_op3(v, OP_SeekHit, level->i_idx_cur, 0, i_eq);
    }
  }else{
    level->u.in.n_in = 0;
  }
  __dbsql_db_free(parser->db, maps);
}
#endif


/*
** Generate code for a single equality term of the WHERE clause.  An equality
** term can be either X=expr or X IN (...).   term is the term to be
** coded.
**
** The current value for the constraint is left in a register, the index
** of which is returned.  An attempt is made store the result in i_target but
** this is only guaranteed for TK_ISNULL and TK_IN constraints.  If the
** constraint is a TK_EQ or TK_IS, then the current value might be left in
** some other register and it is the caller's responsibility to compensate.
**
** For a constraint of the form X=expr, the expression is evaluated in
** straight-line code.  For constraints of the form X IN (...)
** this routine sets up a loop that will iterate over all values of X.
*/
static int __code_equality_term(
  parser_t *parser,      /* The parsing context */
  where_term_t *term,   /* The term of the WHERE clause to be coded */
  where_level_t *level, /* The level of the FROM clause we are working on */
  int i_eq,            /* index_t of the equality term within this level */
  int rev,           /* True for reverse-order IN operations */
  int i_target         /* Attempt to leave results in this register */
){
  expr_t *x = term->expr;
  int i_reg;                  /* Register holding results */

  assert( level->w_loop->l_terms[i_eq]==term );
  assert( i_target>0 );
  if( x->op==TK_EQ || x->op==TK_IS ){
    i_reg = __expr_code_target(parser, x->right, i_target);
  }else if( x->op==TK_ISNULL ){
    i_reg = i_target;
    __vdbe_add_op2(parser->vdbe, OP_Null, 0, i_reg);
#ifndef DBSQL_OMIT_SUBQUERY
  }else{
    assert( x->op==TK_IN );
    i_reg = i_target;
    __code_in_term(parser, term, level, i_eq, rev, i_target);
#endif
  }

  /* As an optimization, try to disable the WHERE clause term that is
  ** driving the index as it will always be true.  The correct answer is
  ** obtained regardless, but we might get the answer with fewer CPU cycles
  ** by omitting the term.
  **
  ** But do not disable the term unless we are certain that the term is
  ** not a transitive constraint.  For an example of where that does not
  ** work, see https://sqlite.org/forum/forumpost/eb8613976a (2021-05-04)
  */
  if( (level->w_loop->ws_flags & WHERE_TRANSCONS)==0
   || (term->e_operator & WO_EQUIV)==0
  ){
    __disable_term(level, term);
  }

  return i_reg;
}

/*
** Generate code that will evaluate all == and IN constraints for an
** index scan.
**
** For example, consider table t1(a,b,c,d,e,f) with index i1(a,b,c).
** Suppose the WHERE clause is this:  a==5 AND b IN (1,2,3) AND c>5 AND c<10
** The index has as many as three equality constraints, but in this
** example, the third "c" value is an inequality.  So only two
** constraints are coded.  This routine will generate code to evaluate
** a==5 and b IN (1,2,3).  The current values for a and b will be stored
** in consecutive registers and the index of the first register is returned.
**
** In the example above n_eq==2.  But this subroutine works for any value
** of n_eq including 0.  If n_eq==0, this routine is nearly a no-op.
** The only thing it does is allocate the level->i_mem memory cell and
** compute the affinity string.
**
** The n_extra_reg parameter is 0 or 1.  It is 0 if all WHERE clause constraints
** are == or IN and are covered by the n_eq.  n_extra_reg is 1 if there is
** an inequality constraint (such as the "c>=5 AND c<10" in the example) that
** occurs after the n_eq quality constraints.
**
** This routine allocates a range of n_eq+n_extra_reg memory cells and returns
** the index of the first memory cell in that range. The code that
** calls this routine will use that memory range to store keys for
** start and termination conditions of the loop.
** key value of the loop.  If one or more IN operators appear, then
** this routine allocates an additional n_eq memory cells for internal
** use.
**
** Before returning, *aff is set to point to a buffer containing a
** copy of the column affinity string of the index allocated using
** __db_malloc(). Except, entries in the copy of the string associated
** with equality constraints that use BLOB or NONE affinity are set to
** DBSQL_AFF_BLOB. This is to deal with SQL such as the following:
**
**   CREATE TABLE t1(a TEXT PRIMARY KEY, b);
**   SELECT ... FROM t1 AS t2, t1 WHERE t1.a = t2.b;
**
** In the example above, the index on t1(a) has TEXT affinity. But since
** the right hand side of the equality constraint (t2.b) has BLOB/NONE affinity,
** no conversion should be attempted before using a t2.b value as part of
** a key to search the index. Hence the first byte in the returned affinity
** string in this example would be set to DBSQL_AFF_BLOB.
*/
static int __code_all_equality_terms(
  parser_t *parser,        /* Parsing context */
  where_level_t *level,   /* Which nested loop of the FROM we are coding */
  int rev,             /* Reverse the order of IN operators */
  int n_extra_reg,        /* Number of extra registers to allocate */
  char **pz_aff          /* OUT: Set to point to affinity string */
){
  u_int16_t n_eq;                      /* The number of == or IN constraints to code */
  u_int16_t n_skip;                    /* Number of left-most columns to skip */
  vdbe_t *v = parser->vdbe;      /* The vm under construction */
  index_t *idx;                  /* The index being used for this loop */
  where_term_t *term;             /* A single constraint term */
  where_loop_t *loop;             /* The where_loop_t object */
  int j;                        /* Loop counter */
  int reg_base;                  /* Base register */
  int n_reg;                     /* Number of registers to allocate */
  char *aff;                   /* Affinity string to return */

  /* This module is only called on query plans that use an index. */
  loop = level->w_loop;
  assert( (loop->ws_flags & WHERE_VIRTUALTABLE)==0 );
  n_eq = loop->u.btree.n_eq;
  n_skip = loop->n_skip;
  idx = loop->u.btree.p_index;
  assert( idx!=0 );

  /* Figure out how many memory cells we will need then allocate them.
  */
  reg_base = parser->n_mem + 1;
  n_reg = n_eq + n_extra_reg;
  parser->n_mem += n_reg;

  aff = __db_str_dup(parser->db,__index_affinity_str(parser->db,idx));
  assert( aff!=0 || parser->db->malloc_failed );

  if( n_skip ){
    int i_idx_cur = level->i_idx_cur;
    __vdbe_add_op3(v, OP_Null, 0, reg_base, reg_base+n_skip-1);
    __vdbe_add_op1(v, (rev?OP_Last:OP_Rewind), i_idx_cur);
    VDBE_COVERAGE_IF(v, rev==0);
    VDBE_COVERAGE_IF(v, rev!=0);
    VDBE_COMMENT((v, "begin skip-scan on %s", idx->name));
    j = __vdbe_add_op0(v, OP_Goto);
    assert( level->addr_skip==0 );
    level->addr_skip = __vdbe_add_op4_int(v, (rev?OP_SeekLT:OP_SeekGT),
                            i_idx_cur, 0, reg_base, n_skip);
    VDBE_COVERAGE_IF(v, rev==0);
    VDBE_COVERAGE_IF(v, rev!=0);
    __vdbe_jump_here(v, j);
    for(j=0; j<n_skip; j++){
      __vdbe_add_op3(v, OP_Column, i_idx_cur, j, reg_base+j);
      testcase( idx->columns[j]==XN_EXPR );
      VDBE_COMMENT((v, "%s", __explain_index_column_name(idx, j)));
    }
  }

  /* Evaluate the equality constraints
  */
  assert( aff==0 || (int)strlen(aff)>=n_eq );
  for(j=n_skip; j<n_eq; j++){
    int r1;
    term = loop->l_terms[j];
    assert( term!=0 );
    /* The following testcase is true for indices with redundant columns.
    ** Ex: CREATE INDEX i1 ON t1(a,b,a); SELECT * FROM t1 WHERE a=0 AND b=0; */
    testcase( (term->wt_flags & TERM_CODED)!=0 );
    testcase( term->wt_flags & TERM_VIRTUAL );
    r1 = __code_equality_term(parser, term, level, j, rev, reg_base+j);
    if( r1!=reg_base+j ){
      if( n_reg==1 ){
        __release_temp_reg(parser, reg_base);
        reg_base = r1;
      }else{
        __vdbe_add_op2(v, OP_Copy, r1, reg_base+j);
      }
    }
    if( term->e_operator & WO_IN ){
      if( term->expr->flags & EP_xIsSelect ){
        /* No affinity ever needs to be (or should be) applied to a value
        ** from the RHS of an "? IN (SELECT ...)" expression. The
        ** __find_in_index() routine has already ensured that the
        ** affinity of the comparison has been applied to the value.  */
        if( aff ) aff[j] = DBSQL_AFF_BLOB;
      }
    }else if( (term->e_operator & WO_ISNULL)==0 ){
      expr_t *right = term->expr->right;
      if( (term->wt_flags & TERM_IS)==0 && __expr_can_be_null(right) ){
        __vdbe_add_op2(v, OP_IsNull, reg_base+j, level->addr_brk);
        VDBE_COVERAGE(v);
      }
      if( parser->n_err==0 ){
        assert( parser->db->malloc_failed==0 );
        if( __compare_affinity(right, aff[j])==DBSQL_AFF_BLOB ){
          aff[j] = DBSQL_AFF_BLOB;
        }
        if( __expr_needs_no_affinity_change(right, aff[j]) ){
          aff[j] = DBSQL_AFF_BLOB;
        }
      }
    }
  }
  *pz_aff = aff;
  return reg_base;
}

#ifndef DBSQL_LIKE_DOESNT_MATCH_BLOBS
/*
** If the most recently coded instruction is a constant range constraint
** (a string literal) that originated from the LIKE optimization, then
** set P3 and P5 on the OP_String opcode so that the string will be cast
** to a BLOB at appropriate times.
**
** The LIKE optimization trys to evaluate "x LIKE 'abc%'" as a range
** expression: "x>='ABC' AND x<'abd'".  But this requires that the range
** scan loop run twice, once for strings and a second time for BLOBs.
** The OP_String opcodes on the second pass convert the upper and lower
** bound string constants to blobs.  This routine makes the necessary changes
** to the OP_String opcodes for that to happen.
**
** Except, of course, if DBSQL_LIKE_DOESNT_MATCH_BLOBS is defined, then
** only the one pass through the string space is required, so this routine
** becomes a no-op.
*/
static void WHERE_LIKE_OPTIMIZATION_STRING_FIXUP(
  vdbe_t *v,                /* prepared statement under construction */
  where_level_t *level,     /* The loop that contains the LIKE operator */
  where_term_t *term        /* The upper or lower bound just coded */
){
  if( term->wt_flags & TERM_LIKEOPT ){
    vdbe_op_t *op;
    assert( level->i_like_rep_cntr>0 );
    op = __vdbe_get_last_op(v);
    assert( op!=0 );
    assert( op->opcode==OP_String8
            || term->wc->w_info->parser->db->malloc_failed );
    op->p3 = (int)(level->i_like_rep_cntr>>1);  /* Register holding counter */
    op->p5 = (u_int8_t)(level->i_like_rep_cntr&1);    /* ASC or DESC */
  }
}
#else
# define WHERE_LIKE_OPTIMIZATION_STRING_FIXUP(A,B,C)
#endif

#ifdef DBSQL_ENABLE_CURSOR_HINTS
/*
** Information is passed from CODE_CURSOR_HINT() down to individual nodes of
** the expression tree (by __walk_expr()) using an instance of this
** structure.
*/
struct c_cur_hint_t {
  int i_tab_cur;    /* Cursor for the main table */
  int i_idx_cur;    /* Cursor for the index, if idx!=0.  Unused otherwise */
  index_t *idx;    /* The index used to access the table */
};

/*
** This function is called for every node of an expression that is a candidate
** for a cursor hint on an index cursor.  For TK_COLUMN nodes that reference
** the table c_cur_hint_t.i_tab_cur, verify that the same column can be
** accessed through the index.  If it cannot, then set walker->code to 1.
*/
static int __code_cursor_hint_check_expr(walker_t *walker, expr_t *expr){
  struct c_cur_hint_t *hint = walker->u.c_cur_hint;
  assert( hint->idx!=0 );
  if( expr->op==TK_COLUMN
   && expr->i_table==hint->i_tab_cur
   && __table_column_to_index(hint->idx, expr->i_column)<0
  ){
    walker->code = 1;
  }
  return WRC_Continue;
}

/*
** Test whether or not expression expr, which was part of a WHERE clause,
** should be included in the cursor-hint for a table that is on the rhs
** of a LEFT JOIN. Set walker_t.code to non-zero before returning if the
** expression is not suitable.
**
** An expression is unsuitable if it might evaluate to non NULL even if
** a TK_COLUMN node that does affect the value of the expression is set
** to NULL. For example:
**
**   col IS NULL
**   col IS NOT NULL
**   coalesce(col, 1)
**   CASE WHEN col THEN 0 ELSE 1 END
*/
static int __code_cursor_hint_is_or_function(walker_t *walker, expr_t *expr){
  if( expr->op==TK_IS
   || expr->op==TK_ISNULL || expr->op==TK_ISNOT
   || expr->op==TK_NOTNULL || expr->op==TK_CASE
  ){
    walker->code = 1;
  }else if( expr->op==TK_FUNCTION ){
    int d1;
    char d2[4];
    if( 0==__is_like_function(walker->parser->db, expr, &d1, d2) ){
      walker->code = 1;
    }
  }

  return WRC_Continue;
}


/*
** This function is called on every node of an expression tree used as an
** argument to the OP_CursorHint instruction. If the node is a TK_COLUMN
** that accesses any table other than the one identified by
** c_cur_hint_t.i_tab_cur, then do the following:
**
**   1) allocate a register and code an OP_Column instruction to read
**      the specified column into the new register, and
**
**   2) transform the expression node to a TK_REGISTER node that reads
**      from the newly populated register.
**
** Also, if the node is a TK_COLUMN that does access the table identified
** by c_cur_hint.i_tab_cur, and an index is being used (which we will
** know because c_cur_hint_t.idx!=0) then transform the TK_COLUMN into
** an access of the index rather than the original table.
*/
static int __code_cursor_hint_fix_expr(walker_t *walker, expr_t *expr){
  int rc = WRC_Continue;
  int reg;
  struct c_cur_hint_t *hint = walker->u.c_cur_hint;
  if( expr->op==TK_COLUMN ){
    if( expr->i_table!=hint->i_tab_cur ){
      reg = ++walker->parser->n_mem;   /* Register for column value */
      reg = __expr_code_target(walker->parser, expr, reg);
      expr->op = TK_REGISTER;
      expr->i_table = reg;
    }else if( hint->idx!=0 ){
      expr->i_table = hint->i_idx_cur;
      expr->i_column = __table_column_to_index(hint->idx, expr->i_column);
      assert( expr->i_column>=0 );
    }
  }else if( expr->agg_info ){
    rc = WRC_Prune;
    reg = ++walker->parser->n_mem;   /* Register for column value */
    reg = __expr_code_target(walker->parser, expr, reg);
    expr->op = TK_REGISTER;
    expr->i_table = reg;
  }else if( expr->op==TK_TRUEFALSE ){
    /* Do not walk disabled expressions.  tag-20230504-1 */
    return WRC_Prune;
  }
  return rc;
}

/*
** Insert an OP_CursorHint instruction if it is appropriate to do so.
*/
static void CODE_CURSOR_HINT(
  src_item_t *tab_item,  /* FROM clause item */
  where_info_t *w_info,    /* The where clause */
  where_level_t *level,   /* Which loop to provide hints for */
  where_term_t *end_range  /* Hint this end-of-scan boundary term if not NULL */
){
  parser_t *parser = w_info->parser;
  DBSQL *db = parser->db;
  vdbe_t *v = parser->vdbe;
  expr_t *expr = 0;
  where_loop_t *loop = level->w_loop;
  int i_cur;
  where_clause_t *wc;
  where_term_t *term;
  int i, j;
  struct c_cur_hint_t s_hint;
  walker_t s_walker;

  if( OPTIMIZATION_DISABLED(db, DBSQL_CursorHints) ) return;
  i_cur = level->i_tab_cur;
  assert( i_cur==w_info->tab_list->a[level->i_from].i_cursor );
  s_hint.i_tab_cur = i_cur;
  s_hint.i_idx_cur = level->i_idx_cur;
  s_hint.idx = loop->u.btree.p_index;
  memset(&s_walker, 0, sizeof(s_walker));
  s_walker.parser = parser;
  s_walker.u.c_cur_hint = &s_hint;
  wc = &w_info->s_wc;
  for(i=0; i<wc->n_base; i++){
    term = &wc->a[i];
    if( term->wt_flags & (TERM_VIRTUAL|TERM_CODED) ) continue;
    if( term->prereq_all & level->not_ready ) continue;

    /* Any terms specified as part of the ON(...) clause for any LEFT
    ** JOIN for which the current table is not the rhs are omitted
    ** from the cursor-hint.
    **
    ** If this table is the rhs of a LEFT JOIN, "IS" or "IS NULL" terms
    ** that were specified as part of the WHERE clause must be excluded.
    ** This is to address the following:
    **
    **   SELECT ... t1 LEFT JOIN t2 ON (t1.a=t2.b) WHERE t2.c IS NULL;
    **
    ** Say there is a single row in t2 that matches (t1.a=t2.b), but its
    ** t2.c values is not NULL. If the (t2.c IS NULL) constraint is
    ** pushed down to the cursor, this row is filtered out, causing
    ** dbsql to synthesize a row of NULL values. Which does match the
    ** WHERE clause, and so the query returns a row. Which is incorrect.
    **
    ** For the same reason, WHERE terms such as:
    **
    **   WHERE 1 = (t2.c IS NULL)
    **
    ** are also excluded. See __code_cursor_hint_is_or_function() for details.
    */
    if( tab_item->fg.jointype & JT_LEFT ){
      expr_t *expr = term->expr;
      if( !EXPR_HAS_PROPERTY(expr, EP_OuterON)
       || expr->w.i_join!=tab_item->i_cursor
      ){
        s_walker.code = 0;
        s_walker.expr_callback = __code_cursor_hint_is_or_function;
        __walk_expr(&s_walker, term->expr);
        if( s_walker.code ) continue;
      }
    }else{
      if( EXPR_HAS_PROPERTY(term->expr, EP_OuterON) ) continue;
    }

    /* All terms in w_loop->l_terms[] except end_range are used to initialize
    ** the cursor.  These terms are not needed as hints for a pure range
    ** scan (that has no == terms) so omit them. */
    if( loop->u.btree.n_eq==0 && term!=end_range ){
      for(j=0; j<loop->n_l_term && loop->l_terms[j]!=term; j++){}
      if( j<loop->n_l_term ) continue;
    }

    /* No subqueries or non-deterministic functions allowed */
    if( __expr_contains_subquery(term->expr) ) continue;

    /* For an index scan, make sure referenced columns are actually in
    ** the index. */
    if( s_hint.idx!=0 ){
      s_walker.code = 0;
      s_walker.expr_callback = __code_cursor_hint_check_expr;
      __walk_expr(&s_walker, term->expr);
      if( s_walker.code ) continue;
    }

    /* If we survive all prior tests, that means this term is worth hinting */
    expr = __expr_and(parser, expr, __expr_dup(db, term->expr, 0));
  }
  if( expr!=0 ){
    s_walker.expr_callback = __code_cursor_hint_fix_expr;
    if( parser->n_err==0 ) __walk_expr(&s_walker, expr);
    __vdbe_add_op4(v, OP_CursorHint,
                      (s_hint.idx ? s_hint.i_idx_cur : s_hint.i_tab_cur), 0, 0,
                      (const char*)expr, P4_EXPR);
  }
}
#else
# define CODE_CURSOR_HINT(A,B,C,D)  /* No-op */
#endif /* DBSQL_ENABLE_CURSOR_HINTS */

/*
** Cursor i_cur is open on an intkey b-tree (a table). Register i_rowid contains
** a rowid value just read from cursor i_idx_cur, open on index idx. This
** function generates code to do a deferred seek of cursor i_cur to the
** rowid stored in register i_rowid.
**
** Normally, this is just:
**
**   OP_DeferredSeek $i_cur $i_rowid
**
** Which causes a seek on $i_cur to the row with rowid $i_rowid.
**
** However, if the scan currently being coded is a branch of an OR-loop and
** the statement currently being coded is a SELECT, then additional information
** is added that might allow OP_Column to omit the seek and instead do its
** lookup on the index, thus avoiding an expensive seek operation.  To
** enable this optimization, the P3 of OP_DeferredSeek is set to i_idx_cur
** and P4 is set to an array of integers containing one entry for each column
** in the table.  For each table column, if the column is the i'th
** column of the index, then the corresponding array entry is set to (i+1).
** If the column does not appear in the index at all, the array entry is set
** to 0.  The OP_Column opcode can check this array to see if the column it
** wants is in the index and if it is, it will substitute the index cursor
** and column number and continue with those new values, rather than seeking
** the table cursor.
*/
static void __code_deferred_seek(
  where_info_t *w_info,              /* Where clause context */
  index_t *idx,                    /* index_t scan is using */
  int i_cur,                       /* Cursor for IPK b-tree */
  int i_idx_cur                     /* index_t cursor */
){
  parser_t *parser = w_info->parser; /* parser_t context */
  vdbe_t *v = parser->vdbe;        /* vdbe_t to generate code within */

  assert( i_idx_cur>0 );
  assert( idx->columns[idx->n_column-1]==-1 );

  w_info->deferred_seek = 1;
  __vdbe_add_op3(v, OP_DeferredSeek, i_idx_cur, 0, i_cur);
  if( (w_info->wctrl_flags & (WHERE_OR_SUBCLAUSE|WHERE_RIGHT_JOIN))
   && DB_MASK_ALL_ZERO(__parse_toplevel(parser)->write_mask)
  ){
    int i;
    table_t *tab = idx->table;
    u_int32_t *ai = (u_int32_t*)__db_malloc_zero(parser->db, sizeof(u_int32_t)*(tab->n_col+1));
    if( ai ){
      ai[0] = tab->n_col;
      for(i=0; i<idx->n_column-1; i++){
        int x1, x2;
        assert( idx->columns[i]<tab->n_col );
        x1 = idx->columns[i];
        x2 = __table_column_to_storage(tab, x1);
        testcase( x1!=x2 );
        if( x1>=0 ) ai[x2+1] = i+1;
      }
      __vdbe_change_p4(v, -1, (char*)ai, P4_INTARRAY);
    }
  }
}

/*
** If the expression passed as the second argument is a vector, generate
** code to write the first n_reg elements of the vector into an array
** of registers starting with i_reg.
**
** If the expression is not a vector, then n_reg must be passed 1. In
** this case, generate code to evaluate the expression and leave the
** result in register i_reg.
*/
static void __code_expr_or_vector(parser_t *parser, expr_t *p, int i_reg, int n_reg){
  assert( n_reg>0 );
  if( p && __expr_is_vector(p) ){
#ifndef DBSQL_OMIT_SUBQUERY
    if( EXPR_USE_X_SELECT(p) ){
      vdbe_t *v = parser->vdbe;
      int i_select;
      assert( p->op==TK_SELECT );
      i_select = __code_subselect(parser, p);
      __vdbe_add_op3(v, OP_Copy, i_select, i_reg, n_reg-1);
    }else
#endif
    {
      int i;
      const expr_list_t *list;
      assert( EXPR_USE_X_LIST(p) );
      list = p->x.list;
      assert( n_reg<=list->n_expr );
      for(i=0; i<n_reg; i++){
        __expr_code(parser, list->a[i].expr, i_reg+i);
      }
    }
  }else{
    assert( n_reg==1 || parser->n_err );
    __expr_code(parser, p, i_reg);
  }
}

/*
** The truth expression is always true because it is the WHERE clause
** a partial index that is driving a query loop.  Look through all of the
** WHERE clause terms on the query, and if any of those terms must be
** true because truth is true, then mark those WHERE clause terms as
** coded.
*/
static void __where_apply_partial_index_constraints(
  expr_t *truth,
  int i_tab_cur,
  where_clause_t *wc
){
  int i;
  where_term_t *term;
  while( truth->op==TK_AND ){
    __where_apply_partial_index_constraints(truth->left, i_tab_cur, wc);
    truth = truth->right;
  }
  for(i=0, term=wc->a; i<wc->n_term; i++, term++){
    expr_t *expr;
    if( term->wt_flags & TERM_CODED ) continue;
    expr = term->expr;
    if( __expr_compare(0, expr, truth, i_tab_cur)==0 ){
      term->wt_flags |= TERM_CODED;
    }
  }
}

/*
** This routine is called right after An OP_Filter has been generated and
** before the corresponding index search has been performed.  This routine
** checks to see if there are additional Bloom filters in inner loops that
** can be checked prior to doing the index lookup.  If there are available
** inner-loop Bloom filters, then evaluate those filters now, before the
** index lookup.  The idea is that a Bloom filter check is way faster than
** an index lookup, and the Bloom filter might return false, meaning that
** the index lookup can be skipped.
**
** We know that an inner loop uses a Bloom filter because it has the
** where_level_t.reg_filter set.  If an inner-loop Bloom filter is checked,
** then clear the where_level_t.reg_filter value to prevent the Bloom filter
** from being checked a second time when the inner loop is evaluated.
*/
static DBSQL_NOINLINE void __filter_pull_down(
  parser_t *parser,       /* Parsing context */
  where_info_t *w_info,   /* Complete information about the WHERE clause */
  int i_level,          /* Which level of w_info->a[] should be coded */
  int addr_nxt,         /* Jump here to bypass inner loops */
  Bitmask not_ready     /* Loops that are not ready */
){
  int saved_addr_brk;
  while( ++i_level < w_info->n_level ){
    where_level_t *level = &w_info->a[i_level];
    where_loop_t *loop = level->w_loop;
    if( level->reg_filter==0 ) continue;
    if( level->w_loop->n_skip ) continue;
    /*         ,--- Because __construct_bloom_filter() has will not have set
    **  vvvvv--'    level->reg_filter if this were true. */
    if( NEVER(loop->prereq & not_ready) ) continue;
    saved_addr_brk = level->addr_brk;
    level->addr_brk = addr_nxt;
    if( loop->ws_flags & WHERE_IPK ){
      where_term_t *term = loop->l_terms[0];
      int reg_rowid;
      assert( term!=0 );
      assert( term->expr!=0 );
      testcase( term->wt_flags & TERM_VIRTUAL );
      reg_rowid = __get_temp_reg(parser);
      reg_rowid = __code_equality_term(parser, term, level, 0, 0, reg_rowid);
      __vdbe_add_op2(parser->vdbe, OP_MustBeInt, reg_rowid, addr_nxt);
      VDBE_COVERAGE(parser->vdbe);
      __vdbe_add_op4_int(parser->vdbe, OP_Filter, level->reg_filter,
                           addr_nxt, reg_rowid, 1);
      VDBE_COVERAGE(parser->vdbe);
    }else{
      u_int16_t n_eq = loop->u.btree.n_eq;
      int r1;
      char *start_aff;

      assert( loop->ws_flags & WHERE_INDEXED );
      assert( (loop->ws_flags & WHERE_COLUMN_IN)==0 );
      r1 = __code_all_equality_terms(parser,level,0,0,&start_aff);
      __code_apply_affinity(parser, r1, n_eq, start_aff);
      __dbsql_db_free(parser->db, start_aff);
      __vdbe_add_op4_int(parser->vdbe, OP_Filter, level->reg_filter,
                           addr_nxt, r1, n_eq);
      VDBE_COVERAGE(parser->vdbe);
    }
    level->reg_filter = 0;
    level->addr_brk = saved_addr_brk;
  }
}

/*
** Loop loop is a WHERE_INDEXED level that uses at least one IN(...)
** operator. Return true if level loop is guaranteed to visit only one
** row for each key generated for the index.
*/
static int __where_loop_is_one_row(where_loop_t *loop){
  if( loop->u.btree.p_index->on_error
   && loop->n_skip==0
   && loop->u.btree.n_eq==loop->u.btree.p_index->n_key_col
  ){
    int ii;
    for(ii=0; ii<loop->u.btree.n_eq; ii++){
      if( loop->l_terms[ii]->e_operator & (WO_IS|WO_ISNULL) ){
        return 0;
      }
    }
    return 1;
  }
  return 0;
}

/*
** Generate code for the start of the i_level-th loop in the WHERE clause
** implementation described by w_info.
*/
Bitmask __where_code_one_loop_start(
  parser_t *parser,       /* Parsing context */
  vdbe_t *v,             /* Prepared statement under construction */
  where_info_t *w_info,   /* Complete information about the WHERE clause */
  int i_level,          /* Which level of w_info->a[] should be coded */
  where_level_t *level,  /* The current level pointer */
  Bitmask not_ready     /* Which tables are currently available */
){
  int j, k;            /* Loop counters */
  int i_cur;            /* The VDBE cursor for the table */
  int addr_nxt;         /* Where to jump to continue with the next IN case */
  int rev;            /* True if we need to scan in reverse order */
  where_loop_t *loop;    /* The where_loop_t object being coded */
  where_clause_t *wc;    /* Decomposition of the entire WHERE clause */
  where_term_t *term;               /* A WHERE clause term */
  DBSQL *db;                    /* Database connection */
  src_item_t *tab_item;              /* FROM clause term being coded */
  int addr_brk;                    /* Jump here to break out of the loop */
  int addr_cont;                   /* Jump here to continue with next cycle */
  int i_rowid_reg = 0;        /* Rowid is stored in this register, if not zero */
  int i_release_reg = 0;      /* Temp register to free before returning */
  index_t *idx = 0;          /* index_t used by loop (if any) */
  int i_loop;                /* Iteration of constraint generator loop */

  wc = &w_info->s_wc;
  db = parser->db;
  loop = level->w_loop;
  tab_item = &w_info->tab_list->a[level->i_from];
  i_cur = tab_item->i_cursor;
  level->not_ready = not_ready & ~__where_get_mask(&w_info->s_mask_set, i_cur);
  rev = (w_info->rev_mask>>i_level)&1;
  VDBE_MODULE_COMMENT((v, "Begin WHERE-loop%d: %s",
                     i_level, tab_item->s_tab->name));
#if WHERETRACE_ENABLED /* 0x4001 */
  if( __where_trace & 0x1 ){
    __debug_printf("Coding level %d of %d:  notReady=%llx  iFrom=%d\n",
       i_level, w_info->n_level, (u_int64_t)not_ready, level->i_from);
    if( __where_trace & 0x1000 ){
      __where_loop_print(loop, wc);
    }
  }
  if( (__where_trace & 0x4001)==0x4001 ){
    if( i_level==0 ){
      __debug_printf("WHERE clause being coded:\n");
      __tree_view_expr(0, w_info->where, 0);
    }
    __debug_printf("All WHERE-clause terms before coding:\n");
    __where_clause_print(wc);
  }
#endif

  /* Create labels for the "break" and "continue" instructions
  ** for the current loop.  Jump to addr_brk to break out of a loop.
  ** Jump to cont to go immediately to the next iteration of the
  ** loop.
  **
  ** When there is an IN operator, we also have a "addr_nxt" label that
  ** means to continue with the next IN value combination.  When
  ** there are no IN operators in the constraints, the "addr_nxt" label
  ** is the same as "addr_brk".
  */
  addr_brk = level->addr_nxt = level->addr_brk;
  addr_cont = level->addr_cont = __vdbe_make_label(parser);

  /* If this is the right table of a LEFT OUTER JOIN, allocate and
  ** initialize a memory cell that records if this table matches any
  ** row of the left table of the join.
  */
  assert( (w_info->wctrl_flags & (WHERE_OR_SUBCLAUSE|WHERE_RIGHT_JOIN))
       || level->i_from>0 || (tab_item[0].fg.jointype & JT_LEFT)==0
  );
  if( level->i_from>0 && (tab_item[0].fg.jointype & JT_LEFT)!=0 ){
    level->i_left_join = ++parser->n_mem;
    __vdbe_add_op2(v, OP_Integer, 0, level->i_left_join);
    VDBE_COMMENT((v, "init LEFT JOIN match flag"));
  }

  /* Special case of a FROM clause subquery implemented as a co-routine */
  if( tab_item->fg.via_coroutine ){
    int reg_yield;
    subquery_t *subq;
    assert( tab_item->fg.is_subquery && tab_item->u4.subq!=0 );
    subq = tab_item->u4.subq;
    reg_yield = subq->reg_return;
    __vdbe_add_op3(v, OP_InitCoroutine, reg_yield, 0, subq->addr_fill_sub);
    level->p2 =  __vdbe_add_op2(v, OP_Yield, reg_yield, addr_brk);
    VDBE_COVERAGE(v);
    VDBE_COMMENT((v, "next row of %s", tab_item->s_tab->name));
    level->op = OP_Goto;
  }else

#ifndef DBSQL_OMIT_VIRTUALTABLE
  if(  (loop->ws_flags & WHERE_VIRTUALTABLE)!=0 ){
    /* Case 1:  The table is a virtual-table.  Use the VFilter and VNext
    **          to access the data.
    */
    int i_reg;   /* P3 Value for OP_VFilter */
    int addr_not_found;
    int n_constraint = loop->n_l_term;

    i_reg = __get_temp_range(parser, n_constraint+2);
    addr_not_found = level->addr_brk;
    for(j=0; j<n_constraint; j++){
      int i_target = i_reg+j+2;
      term = loop->l_terms[j];
      if( NEVER(term==0) ) continue;
      if( term->e_operator & WO_IN ){
        if( SMASKBIT32(j) & loop->u.vtab.m_handle_in ){
          int i_tab = parser->n_tab++;
          int i_cache = ++parser->n_mem;
          __code_rhs_of_in(parser, term->expr, i_tab, 0);
          __vdbe_add_op3(v, OP_VInitIn, i_tab, i_target, i_cache);
        }else{
          __code_equality_term(parser, term, level, j, rev, i_target);
          addr_not_found = level->addr_nxt;
        }
      }else{
        expr_t *right = term->expr->right;
        __code_expr_or_vector(parser, right, i_target, 1);
        if( term->match_op==DBSQL_INDEX_CONSTRAINT_OFFSET
         && loop->u.vtab.omit_offset
        ){
          assert( term->e_operator==WO_AUX );
          assert( w_info->p_select!=0 );
          assert( w_info->p_select->i_offset>0 );
          __vdbe_add_op2(v, OP_Integer, 0, w_info->p_select->i_offset);
          VDBE_COMMENT((v,"Zero OFFSET counter"));
        }
      }
    }
    __vdbe_add_op2(v, OP_Integer, loop->u.vtab.idx_num, i_reg);
    __vdbe_add_op2(v, OP_Integer, n_constraint, i_reg+1);
    /* The instruction immediately prior to OP_VFilter must be an OP_Integer
    ** that sets the "argc" value for v_filter.  This is necessary for
    ** resolve_p2() to work correctly.  See tag-20250207a. */
    __vdbe_add_op4(v, OP_VFilter, i_cur, addr_not_found, i_reg,
                      loop->u.vtab.idx_str,
                      loop->u.vtab.need_free ? P4_DYNAMIC : P4_STATIC);
    VDBE_COVERAGE(v);
    loop->u.vtab.need_free = 0;
    /* An OOM inside of __add_op4(OP_VFilter) instruction above might have freed
    ** the u.vtab.idx_str.  NULL it out to prevent a use-after-free */
    if( db->malloc_failed ) loop->u.vtab.idx_str = 0;
    level->p1 = i_cur;
    level->op = w_info->one_pass ? OP_Noop : OP_VNext;
    level->p2 = __vdbe_current_addr(v);
    assert( (loop->ws_flags & WHERE_MULTI_OR)==0 );

    for(j=0; j<n_constraint; j++){
      term = loop->l_terms[j];
      if( j<16 && (loop->u.vtab.omit_mask>>j)&1 ){
        __disable_term(level, term);
        continue;
      }
      if( (term->e_operator & WO_IN)!=0
       && (SMASKBIT32(j) & loop->u.vtab.m_handle_in)==0
       && !db->malloc_failed
      ){
        expr_t *compare;  /* The comparison operator */
        expr_t *right;    /* RHS of the comparison */
        vdbe_op_t *p_op;     /* Opcode to access the value of the IN constraint */
        int i_in;         /* IN loop corresponding to the j-th constraint */

        /* Reload the constraint value into reg[i_reg+j+2].  The same value
        ** was loaded into the same register prior to the OP_VFilter, but
        ** the filter implementation might have changed the datatype or
        ** encoding of the value in the register, so it *must* be reloaded.
        */
        for(i_in=0; ALWAYS(i_in<level->u.in.n_in); i_in++){
          p_op = __vdbe_get_op(v, level->u.in.in_loops[i_in].addr_in_top);
          if( (p_op->opcode==OP_Column && p_op->p3==i_reg+j+2)
           || (p_op->opcode==OP_Rowid && p_op->p2==i_reg+j+2)
          ){
            testcase( p_op->opcode==OP_Rowid );
            __vdbe_add_op3(v, p_op->opcode, p_op->p1, p_op->p2, p_op->p3);
            break;
          }
        }

        /* Generate code that will continue to the next row if
        ** the IN constraint is not satisfied
        */
        compare = __p_expr(parser, TK_EQ, 0, 0);
        if( !db->malloc_failed ){
          int i_fld = term->u.x.i_field;
          expr_t *left = term->expr->left;
          assert( left!=0 );
          if( i_fld>0 ){
            assert( left->op==TK_VECTOR );
            assert( EXPR_USE_X_LIST(left) );
            assert( i_fld<=left->x.list->n_expr );
            compare->left = left->x.list->a[i_fld-1].expr;
          }else{
            compare->left = left;
          }
          compare->right = right = __expr(db, TK_REGISTER, 0);
          if( right ){
            right->i_table = i_reg+j+2;
            __expr_if_false(
                parser, compare, level->addr_cont, DBSQL_JUMPIFNULL
            );
          }
          compare->left = 0;
        }
        __expr_delete(db, compare);
      }
    }

    /* These registers need to be preserved in case there is an IN operator
    ** loop.  So we could deallocate the registers here (and potentially
    ** reuse them later) if (loop->ws_flags & WHERE_IN_ABLE)==0.  But it seems
    ** simpler and safer to simply not reuse the registers.
    **
    **    __release_temp_range(parser, i_reg, n_constraint+2);
    */
  }else
#endif /* DBSQL_OMIT_VIRTUALTABLE */

  if( (loop->ws_flags & WHERE_IPK)!=0
   && (loop->ws_flags & (WHERE_COLUMN_IN|WHERE_COLUMN_EQ))!=0
  ){
    /* Case 2:  We can directly reference a single row using an
    **          equality comparison against the ROWID field.  Or
    **          we reference multiple rows using a "rowid IN (...)"
    **          construct.
    */
    assert( loop->u.btree.n_eq==1 );
    term = loop->l_terms[0];
    assert( term!=0 );
    assert( term->expr!=0 );
    testcase( term->wt_flags & TERM_VIRTUAL );
    i_release_reg = ++parser->n_mem;
    i_rowid_reg = __code_equality_term(parser, term, level, 0, rev, i_release_reg);
    if( i_rowid_reg!=i_release_reg ) __release_temp_reg(parser, i_release_reg);
    addr_nxt = level->addr_nxt;
    if( level->reg_filter ){
      __vdbe_add_op2(v, OP_MustBeInt, i_rowid_reg, addr_nxt);
      VDBE_COVERAGE(v);
      __vdbe_add_op4_int(v, OP_Filter, level->reg_filter, addr_nxt,
                           i_rowid_reg, 1);
      VDBE_COVERAGE(v);
      __filter_pull_down(parser, w_info, i_level, addr_nxt, not_ready);
    }
    __vdbe_add_op3(v, OP_SeekRowid, i_cur, addr_nxt, i_rowid_reg);
    VDBE_COVERAGE(v);
    level->op = OP_Noop;
  }else if( (loop->ws_flags & WHERE_IPK)!=0
         && (loop->ws_flags & WHERE_COLUMN_RANGE)!=0
  ){
    /* Case 3:  We have an inequality comparison against the ROWID field.
    */
    int test_op = OP_Noop;
    int start;
    int mem_end_value = 0;
    where_term_t *p_start, *end;

    j = 0;
    p_start = end = 0;
    if( loop->ws_flags & WHERE_BTM_LIMIT ) p_start = loop->l_terms[j++];
    if( loop->ws_flags & WHERE_TOP_LIMIT ) end = loop->l_terms[j++];
    assert( p_start!=0 || end!=0 );
    if( rev ){
      term = p_start;
      p_start = end;
      end = term;
    }
    CODE_CURSOR_HINT(tab_item, w_info, level, end);
    if( p_start ){
      expr_t *p_x;             /* The expression that defines the start bound */
      int r1, r_temp;        /* Registers for holding the start boundary */
      int op;               /* Cursor seek operation */

      /* The following constant maps TK_xx codes into corresponding
      ** seek opcodes.  It depends on a particular ordering of TK_xx
      */
      const u_int8_t move_ops[] = {
           /* TK_GT */  OP_SeekGT,
           /* TK_LE */  OP_SeekLE,
           /* TK_LT */  OP_SeekLT,
           /* TK_GE */  OP_SeekGE
      };
      assert( TK_LE==TK_GT+1 );      /* Make sure the ordering.. */
      assert( TK_LT==TK_GT+2 );      /*  ... of the TK_xx values... */
      assert( TK_GE==TK_GT+3 );      /*  ... is correct. */

      assert( (p_start->wt_flags & TERM_VNULL)==0 );
      testcase( p_start->wt_flags & TERM_VIRTUAL );
      p_x = p_start->expr;
      assert( p_x!=0 );
      testcase( p_start->left_cursor!=i_cur ); /* transitive constraints */
      if( __expr_is_vector(p_x->right) ){
        r1 = r_temp = __get_temp_reg(parser);
        __code_expr_or_vector(parser, p_x->right, r1, 1);
        testcase( p_x->op==TK_GT );
        testcase( p_x->op==TK_GE );
        testcase( p_x->op==TK_LT );
        testcase( p_x->op==TK_LE );
        op = move_ops[((p_x->op - TK_GT - 1) & 0x3) | 0x1];
        assert( p_x->op!=TK_GT || op==OP_SeekGE );
        assert( p_x->op!=TK_GE || op==OP_SeekGE );
        assert( p_x->op!=TK_LT || op==OP_SeekLE );
        assert( p_x->op!=TK_LE || op==OP_SeekLE );
      }else{
        r1 = __expr_code_temp(parser, p_x->right, &r_temp);
        __disable_term(level, p_start);
        op = move_ops[(p_x->op - TK_GT)];
      }
      __vdbe_add_op3(v, op, i_cur, addr_brk, r1);
      VDBE_COMMENT((v, "pk"));
      VDBE_COVERAGE_IF(v, p_x->op==TK_GT);
      VDBE_COVERAGE_IF(v, p_x->op==TK_LE);
      VDBE_COVERAGE_IF(v, p_x->op==TK_LT);
      VDBE_COVERAGE_IF(v, p_x->op==TK_GE);
      __release_temp_reg(parser, r_temp);
    }else{
      __vdbe_add_op2(v, rev ? OP_Last : OP_Rewind, i_cur, level->addr_halt);
      VDBE_COVERAGE_IF(v, rev==0);
      VDBE_COVERAGE_IF(v, rev!=0);
    }
    if( end ){
      expr_t *p_x;
      p_x = end->expr;
      assert( p_x!=0 );
      assert( (end->wt_flags & TERM_VNULL)==0 );
      testcase( end->left_cursor!=i_cur ); /* Transitive constraints */
      testcase( end->wt_flags & TERM_VIRTUAL );
      mem_end_value = ++parser->n_mem;
      __code_expr_or_vector(parser, p_x->right, mem_end_value, 1);
      if( 0==__expr_is_vector(p_x->right)
       && (p_x->op==TK_LT || p_x->op==TK_GT)
      ){
        test_op = rev ? OP_Le : OP_Ge;
      }else{
        test_op = rev ? OP_Lt : OP_Gt;
      }
      if( 0==__expr_is_vector(p_x->right) ){
        __disable_term(level, end);
      }
    }
    start = __vdbe_current_addr(v);
    level->op = rev ? OP_Prev : OP_Next;
    level->p1 = i_cur;
    level->p2 = start;
    assert( level->p5==0 );
    if( test_op!=OP_Noop ){
      i_rowid_reg = ++parser->n_mem;
      __vdbe_add_op2(v, OP_Rowid, i_cur, i_rowid_reg);
      __vdbe_add_op3(v, test_op, mem_end_value, addr_brk, i_rowid_reg);
      VDBE_COVERAGE_IF(v, test_op==OP_Le);
      VDBE_COVERAGE_IF(v, test_op==OP_Lt);
      VDBE_COVERAGE_IF(v, test_op==OP_Ge);
      VDBE_COVERAGE_IF(v, test_op==OP_Gt);
      __vdbe_change_p5(v, DBSQL_AFF_NUMERIC | DBSQL_JUMPIFNULL);
    }
  }else if( loop->ws_flags & WHERE_INDEXED ){
    /* Case 4: Search using an index.
    **
    ** The WHERE clause may contain zero or more equality
    ** terms ("==" or "IN" or "IS" operators) that refer to the N
    ** left-most columns of the index. It may also contain
    ** inequality constraints (>, <, >= or <=) on the indexed
    ** column that immediately follows the N equalities. Only
    ** the right-most column can be an inequality - the rest must
    ** use the "==", "IN", or "IS" operators. For example, if the
    ** index is on (x,y,z), then the following clauses are all
    ** optimized:
    **
    **    x=5
    **    x=5 AND y=10
    **    x=5 AND y<10
    **    x=5 AND y>5 AND y<10
    **    x=5 AND y=5 AND z<=10
    **
    ** The z<10 term of the following cannot be used, only
    ** the x=5 term:
    **
    **    x=5 AND z<10
    **
    ** N may be zero if there are inequality constraints.
    ** If there are no inequality constraints, then N is at
    ** least one.
    **
    ** This case is also used when there are no WHERE clause
    ** constraints but an index is selected anyway, in order
    ** to force the output order to conform to an ORDER BY.
    */
    static const u_int8_t start_ops[] = {
      0,
      0,
      OP_Rewind,           /* 2: (!start_constraints && start_eq &&  !rev) */
      OP_Last,             /* 3: (!start_constraints && start_eq &&   rev) */
      OP_SeekGT,           /* 4: (start_constraints  && !start_eq && !rev) */
      OP_SeekLT,           /* 5: (start_constraints  && !start_eq &&  rev) */
      OP_SeekGE,           /* 6: (start_constraints  &&  start_eq && !rev) */
      OP_SeekLE            /* 7: (start_constraints  &&  start_eq &&  rev) */
    };
    static const u_int8_t end_ops[] = {
      OP_IdxGE,            /* 0: (end_constraints && !rev && !end_eq) */
      OP_IdxGT,            /* 1: (end_constraints && !rev &&  end_eq) */
      OP_IdxLE,            /* 2: (end_constraints &&  rev && !end_eq) */
      OP_IdxLT,            /* 3: (end_constraints &&  rev &&  end_eq) */
    };
    u_int16_t n_eq = loop->u.btree.n_eq;     /* Number of == or IN terms */
    u_int16_t n_btm = loop->u.btree.n_btm;   /* Length of BTM vector */
    u_int16_t n_top = loop->u.btree.n_top;   /* Length of TOP vector */
    int reg_base;                 /* Base register holding constraint values */
    where_term_t *range_start = 0;  /* Inequality constraint at range start */
    where_term_t *range_end = 0;    /* Inequality constraint at range end */
    int start_eq;                 /* True if range start uses ==, >= or <= */
    int end_eq;                   /* True if range end uses ==, >= or <= */
    int start_constraints;       /* Start of range is constrained */
    int n_constraint;             /* Number of constraint terms */
    int i_idx_cur;                 /* The VDBE cursor for the index */
    int n_extra_reg = 0;           /* Number of extra registers needed */
    int op;                      /* Instruction opcode */
    char *start_aff;             /* Affinity for start of range constraint */
    char *end_aff = 0;           /* Affinity for end of range constraint */
    u_int8_t seek_past_null = 0;        /* True to seek past initial nulls */
    u_int8_t stop_at_null = 0;          /* Add condition to terminate at NULLs */
    int omit_table;               /* True if we use the index only */
    int reg_bignull = 0;          /* big-null flag register */
    int addr_seek_scan = 0;        /* Opcode of the OP_SeekScan, if any */

    idx = loop->u.btree.p_index;
    i_idx_cur = level->i_idx_cur;
    assert( n_eq>=loop->n_skip );

    /* Find any inequality constraint terms for the start and end
    ** of the range.
    */
    j = n_eq;
    if( loop->ws_flags & WHERE_BTM_LIMIT ){
      range_start = loop->l_terms[j++];
      n_extra_reg = MAX(n_extra_reg, loop->u.btree.n_btm);
      /* Like optimization range constraints always occur in pairs */
      assert( (range_start->wt_flags & TERM_LIKEOPT)==0 ||
              (loop->ws_flags & WHERE_TOP_LIMIT)!=0 );
    }
    if( loop->ws_flags & WHERE_TOP_LIMIT ){
      range_end = loop->l_terms[j++];
      n_extra_reg = MAX(n_extra_reg, loop->u.btree.n_top);
#ifndef DBSQL_LIKE_DOESNT_MATCH_BLOBS
      if( (range_end->wt_flags & TERM_LIKEOPT)!=0 ){
        assert( range_start!=0 );                     /* LIKE opt constraints */
        assert( range_start->wt_flags & TERM_LIKEOPT );   /* occur in pairs */
        level->i_like_rep_cntr = (u_int32_t)++parser->n_mem;
        __vdbe_add_op2(v, OP_Integer, 1, (int)level->i_like_rep_cntr);
        VDBE_COMMENT((v, "LIKE loop counter"));
        level->addr_like_rep = __vdbe_current_addr(v);
        /* i_like_rep_cntr actually stores 2x the counter register number.  The
        ** bottom bit indicates whether the search order is ASC or DESC. */
        testcase( rev );
        testcase( idx->sort_orders[n_eq]==DBSQL_SO_DESC );
        assert( (rev & ~1)==0 );
        level->i_like_rep_cntr <<=1;
        level->i_like_rep_cntr |= rev ^ (idx->sort_orders[n_eq]==DBSQL_SO_DESC);
      }
#endif
      if( range_start==0 ){
        j = idx->columns[n_eq];
        if( (j>=0 && idx->table->cols[j].not_null==0) || j==XN_EXPR ){
          seek_past_null = 1;
        }
      }
    }
    assert( range_end==0 || (range_end->wt_flags & TERM_VNULL)==0 );

    /* If the WHERE_BIGNULL_SORT flag is set, then index column n_eq uses
    ** a non-default "big-null" sort (either ASC NULLS LAST or DESC NULLS
    ** FIRST). In both cases separate ordered scans are made of those
    ** index entries for which the column is null and for those for which
    ** it is not. For an ASC sort, the non-NULL entries are scanned first.
    ** For DESC, NULL entries are scanned first.
    */
    if( (loop->ws_flags & (WHERE_TOP_LIMIT|WHERE_BTM_LIMIT))==0
     && (loop->ws_flags & WHERE_BIGNULL_SORT)!=0
    ){
      assert( seek_past_null==0 && n_extra_reg==0 && n_btm==0 && n_top==0 );
      assert( range_end==0 && range_start==0 );
      testcase( loop->n_skip>0 );
      n_extra_reg = 1;
      seek_past_null = 1;
      level->reg_bignull = reg_bignull = ++parser->n_mem;
      if( level->i_left_join ){
        __vdbe_add_op2(v, OP_Integer, 0, reg_bignull);
      }
      level->addr_bignull = __vdbe_make_label(parser);
    }

    /* If we are doing a reverse order scan on an ascending index, or
    ** a forward order scan on a descending index, interchange the
    ** start and end terms (range_start and range_end).
    */
    if( (n_eq<idx->n_column && rev==(idx->sort_orders[n_eq]==DBSQL_SO_ASC)) ){
      SWAP(where_term_t *, range_end, range_start);
      SWAP(u_int8_t, seek_past_null, stop_at_null);
      SWAP(u_int8_t, n_btm, n_top);
    }

    if( i_level>0 && (loop->ws_flags & WHERE_IN_SEEKSCAN)!=0 ){
      /* In case OP_SeekScan is used, ensure that the index cursor does not
      ** point to a valid row for the first iteration of this loop. */
      __vdbe_add_op1(v, OP_NullRow, i_idx_cur);
    }

    /* Generate code to evaluate all constraint terms using == or IN
    ** and store the values of those terms in an array of registers
    ** starting at reg_base.
    */
    CODE_CURSOR_HINT(tab_item, w_info, level, range_end);
    reg_base = __code_all_equality_terms(parser,level,rev,n_extra_reg,&start_aff);
    assert( start_aff==0 || __strlen30(start_aff)>=n_eq );
    if( start_aff && n_top ){
      end_aff = __db_str_dup(db, &start_aff[n_eq]);
    }
    addr_nxt = (reg_bignull ? level->addr_bignull : level->addr_nxt);

    testcase( range_start && (range_start->e_operator & WO_LE)!=0 );
    testcase( range_start && (range_start->e_operator & WO_GE)!=0 );
    testcase( range_end && (range_end->e_operator & WO_LE)!=0 );
    testcase( range_end && (range_end->e_operator & WO_GE)!=0 );
    start_eq = !range_start || range_start->e_operator & (WO_LE|WO_GE);
    end_eq =   !range_end || range_end->e_operator & (WO_LE|WO_GE);
    start_constraints = range_start || n_eq>0;

    /* Seek the index cursor to the start of the range. */
    n_constraint = n_eq;
    if( range_start ){
      expr_t *right = range_start->expr->right;
      __code_expr_or_vector(parser, right, reg_base+n_eq, n_btm);
      WHERE_LIKE_OPTIMIZATION_STRING_FIXUP(v, level, range_start);
      if( (range_start->wt_flags & TERM_VNULL)==0
       && __expr_can_be_null(right)
      ){
        __vdbe_add_op2(v, OP_IsNull, reg_base+n_eq, addr_nxt);
        VDBE_COVERAGE(v);
      }
      if( start_aff ){
        __update_range_affinity_str(right, n_btm, &start_aff[n_eq]);
      }
      n_constraint += n_btm;
      testcase( range_start->wt_flags & TERM_VIRTUAL );
      if( __expr_is_vector(right)==0 ){
        __disable_term(level, range_start);
      }else{
        start_eq = 1;
      }
      seek_past_null = 0;
    }else if( seek_past_null ){
      start_eq = 0;
      __vdbe_add_op2(v, OP_Null, 0, reg_base+n_eq);
      start_constraints = 1;
      n_constraint++;
    }else if( reg_bignull ){
      __vdbe_add_op2(v, OP_Null, 0, reg_base+n_eq);
      start_constraints = 1;
      n_constraint++;
    }
    __code_apply_affinity(parser, reg_base, n_constraint - seek_past_null, start_aff);
    if( loop->n_skip>0 && n_constraint==loop->n_skip ){
      /* The skip-scan logic inside the call to code_all_equality_constraints()
      ** above has already left the cursor sitting on the correct row,
      ** so no further seeking is needed */
    }else{
      if( reg_bignull ){
        __vdbe_add_op2(v, OP_Integer, 1, reg_bignull);
        VDBE_COMMENT((v, "NULL-scan pass ctr"));
      }
      if( level->reg_filter ){
        __vdbe_add_op4_int(v, OP_Filter, level->reg_filter, addr_nxt,
                             reg_base, n_eq);
        VDBE_COVERAGE(v);
        __filter_pull_down(parser, w_info, i_level, addr_nxt, not_ready);
      }

      op = start_ops[(start_constraints<<2) + (start_eq<<1) + rev];
      assert( op!=0 );
      if( (loop->ws_flags & WHERE_IN_SEEKSCAN)!=0 && op==OP_SeekGE ){
        assert( reg_bignull==0 );
        /* TUNING:  The OP_SeekScan opcode seeks to reduce the number
        ** of expensive seek operations by replacing a single seek with
        ** 1 or more step operations.  The question is, how many steps
        ** should we try before giving up and going with a seek.  The cost
        ** of a seek is proportional to the logarithm of the of the number
        ** of entries in the tree, so basing the number of steps to try
        ** on the estimated number of rows in the btree seems like a good
        ** guess. */
        addr_seek_scan = __vdbe_add_op1(v, OP_SeekScan,
                                         (idx->row_log_ests[0]+9)/10);
        if( range_start || range_end ){
          __vdbe_change_p5(v, 1);
          __vdbe_change_p2(v, addr_seek_scan, __vdbe_current_addr(v)+1);
          addr_seek_scan = 0;
        }
        VDBE_COVERAGE(v);
      }
      __vdbe_add_op4_int(v, op, i_idx_cur, addr_nxt, reg_base, n_constraint);
      VDBE_COVERAGE(v);
      VDBE_COVERAGE_IF(v, op==OP_Rewind);  testcase( op==OP_Rewind );
      VDBE_COVERAGE_IF(v, op==OP_Last);    testcase( op==OP_Last );
      VDBE_COVERAGE_IF(v, op==OP_SeekGT);  testcase( op==OP_SeekGT );
      VDBE_COVERAGE_IF(v, op==OP_SeekGE);  testcase( op==OP_SeekGE );
      VDBE_COVERAGE_IF(v, op==OP_SeekLE);  testcase( op==OP_SeekLE );
      VDBE_COVERAGE_IF(v, op==OP_SeekLT);  testcase( op==OP_SeekLT );

      assert( seek_past_null==0 || stop_at_null==0 );
      if( reg_bignull ){
        assert( seek_past_null==1 || stop_at_null==1 );
        assert( seek_past_null==!stop_at_null );
        assert( stop_at_null==start_eq );
        __vdbe_add_op2(v, OP_Goto, 0, __vdbe_current_addr(v)+2);
        op = start_ops[(n_constraint>1)*4 + 2 + rev];
        __vdbe_add_op4_int(v, op, i_idx_cur, addr_nxt, reg_base,
                             n_constraint-start_eq);
        VDBE_COVERAGE(v);
        VDBE_COVERAGE_IF(v, op==OP_Rewind);  testcase( op==OP_Rewind );
        VDBE_COVERAGE_IF(v, op==OP_Last);    testcase( op==OP_Last );
        VDBE_COVERAGE_IF(v, op==OP_SeekGE);  testcase( op==OP_SeekGE );
        VDBE_COVERAGE_IF(v, op==OP_SeekLE);  testcase( op==OP_SeekLE );
        assert( op==OP_Rewind || op==OP_Last || op==OP_SeekGE || op==OP_SeekLE);
      }
    }

    /* Load the value for the inequality constraint at the end of the
    ** range (if any).
    */
    n_constraint = n_eq;
    assert( level->p2==0 );
    if( range_end ){
      expr_t *right = range_end->expr->right;
      assert( addr_seek_scan==0 );
      __code_expr_or_vector(parser, right, reg_base+n_eq, n_top);
      WHERE_LIKE_OPTIMIZATION_STRING_FIXUP(v, level, range_end);
      if( (range_end->wt_flags & TERM_VNULL)==0
       && __expr_can_be_null(right)
      ){
        __vdbe_add_op2(v, OP_IsNull, reg_base+n_eq, addr_nxt);
        VDBE_COVERAGE(v);
      }
      if( end_aff ){
        __update_range_affinity_str(right, n_top, end_aff);
        __code_apply_affinity(parser, reg_base+n_eq, n_top, end_aff);
      }else{
        assert( parser->db->malloc_failed );
      }
      n_constraint += n_top;
      testcase( range_end->wt_flags & TERM_VIRTUAL );

      if( __expr_is_vector(right)==0 ){
        __disable_term(level, range_end);
      }else{
        end_eq = 1;
      }
    }else if( stop_at_null ){
      if( reg_bignull==0 ){
        __vdbe_add_op2(v, OP_Null, 0, reg_base+n_eq);
        end_eq = 0;
      }
      n_constraint++;
    }
    if( start_aff ) __db_nn_free_nn(db, start_aff);
    if( end_aff ) __db_nn_free_nn(db, end_aff);

    /* Top of the loop body */
    level->p2 = __vdbe_current_addr(v);

    /* Check if the index cursor is past the end of the range. */
    if( n_constraint ){
      if( reg_bignull ){
        /* Except, skip the end-of-range check while doing the NULL-scan */
        __vdbe_add_op2(v, OP_IfNot, reg_bignull, __vdbe_current_addr(v)+3);
        VDBE_COMMENT((v, "If NULL-scan 2nd pass"));
        VDBE_COVERAGE(v);
      }
      op = end_ops[rev*2 + end_eq];
      __vdbe_add_op4_int(v, op, i_idx_cur, addr_nxt, reg_base, n_constraint);
      testcase( op==OP_IdxGT );  VDBE_COVERAGE_IF(v, op==OP_IdxGT );
      testcase( op==OP_IdxGE );  VDBE_COVERAGE_IF(v, op==OP_IdxGE );
      testcase( op==OP_IdxLT );  VDBE_COVERAGE_IF(v, op==OP_IdxLT );
      testcase( op==OP_IdxLE );  VDBE_COVERAGE_IF(v, op==OP_IdxLE );
      if( addr_seek_scan ) __vdbe_jump_here(v, addr_seek_scan);
    }
    if( reg_bignull ){
      /* During a NULL-scan, check to see if we have reached the end of
      ** the NULLs */
      assert( seek_past_null==!stop_at_null );
      assert( seek_past_null+stop_at_null==1 );
      assert( n_constraint+seek_past_null>0 );
      __vdbe_add_op2(v, OP_If, reg_bignull, __vdbe_current_addr(v)+2);
      VDBE_COMMENT((v, "If NULL-scan 1st pass"));
      VDBE_COVERAGE(v);
      op = end_ops[rev*2 + seek_past_null];
      __vdbe_add_op4_int(v, op, i_idx_cur, addr_nxt, reg_base,
                           n_constraint+seek_past_null);
      testcase( op==OP_IdxGT );  VDBE_COVERAGE_IF(v, op==OP_IdxGT );
      testcase( op==OP_IdxGE );  VDBE_COVERAGE_IF(v, op==OP_IdxGE );
      testcase( op==OP_IdxLT );  VDBE_COVERAGE_IF(v, op==OP_IdxLT );
      testcase( op==OP_IdxLE );  VDBE_COVERAGE_IF(v, op==OP_IdxLE );
    }

    if( (loop->ws_flags & WHERE_IN_EARLYOUT)!=0 ){
      __vdbe_add_op3(v, OP_SeekHit, i_idx_cur, n_eq, n_eq);
    }

    /* Seek the table cursor, if required */
    omit_table = (loop->ws_flags & WHERE_IDX_ONLY)!=0
           && (w_info->wctrl_flags & (WHERE_OR_SUBCLAUSE|WHERE_RIGHT_JOIN))==0;
    if( omit_table ){
      /* idx is a covering index.  No need to access the main table. */
    }else if( HAS_ROWID(idx->table) ){
      __code_deferred_seek(w_info, idx, i_cur, i_idx_cur);
    }else if( i_cur!=i_idx_cur ){
      index_t *pk = __primary_key_index(idx->table);
      i_rowid_reg = __get_temp_range(parser, pk->n_key_col);
      for(j=0; j<pk->n_key_col; j++){
        k = __table_column_to_index(idx, pk->columns[j]);
        __vdbe_add_op3(v, OP_Column, i_idx_cur, k, i_rowid_reg+j);
      }
      __vdbe_add_op4_int(v, OP_NotFound, i_cur, addr_cont,
                           i_rowid_reg, pk->n_key_col); VDBE_COVERAGE(v);
    }

    if( level->i_left_join==0 ){
      /* If a partial index is driving the loop, try to eliminate WHERE clause
      ** terms from the query that must be true due to the WHERE clause of
      ** the partial index.  This optimization does not work on an outer join,
      ** as shown by:
      **
      ** 2019-11-02 ticket 623eff57e76d45f6      (LEFT JOIN)
      ** 2025-05-29 forum post 7dee41d32506c4ae  (RIGHT JOIN)
      */
      if( idx->part_idx_where && level->rj==0 ){
        __where_apply_partial_index_constraints(idx->part_idx_where, i_cur, wc);
      }
    }else{
      testcase( idx->part_idx_where );
      /* The following assert() is not a requirement, merely an observation:
      ** The OR-optimization doesn't work for the right hand table of
      ** a LEFT JOIN: */
      assert( (w_info->wctrl_flags & (WHERE_OR_SUBCLAUSE|WHERE_RIGHT_JOIN))==0 );
    }

    /* Record the instruction used to terminate the loop. */
    if( (loop->ws_flags & WHERE_ONEROW)
     || (level->u.in.n_in && reg_bignull==0 && __where_loop_is_one_row(loop))
    ){
      level->op = OP_Noop;
    }else if( rev ){
      level->op = OP_Prev;
    }else{
      level->op = OP_Next;
    }
    level->p1 = i_idx_cur;
    level->p3 = (loop->ws_flags&WHERE_UNQ_WANTED)!=0 ? 1:0;
    if( (loop->ws_flags & WHERE_CONSTRAINT)==0 ){
      level->p5 = DBSQL_STMTSTATUS_FULLSCAN_STEP;
    }else{
      assert( level->p5==0 );
    }
    if( omit_table ) idx = 0;
  }else

#ifndef DBSQL_OMIT_OR_OPTIMIZATION
  if( loop->ws_flags & WHERE_MULTI_OR ){
    /* Case 5:  Two or more separately indexed terms connected by OR
    **
    ** Example:
    **
    **   CREATE TABLE t1(a,b,c,d);
    **   CREATE INDEX i1 ON t1(a);
    **   CREATE INDEX i2 ON t1(b);
    **   CREATE INDEX i3 ON t1(c);
    **
    **   SELECT * FROM t1 WHERE a=5 OR b=7 OR (c=11 AND d=13)
    **
    ** In the example, there are three indexed terms connected by OR.
    ** The top of the loop looks like this:
    **
    **          Null       1                # Zero the rowset in reg 1
    **
    ** Then, for each indexed term, the following. The arguments to
    ** __row_set_test are such that the rowid of the current row is inserted
    ** into the row_set_t. If it is already present, control skips the
    ** Gosub opcode and jumps straight to the code generated by __where_end().
    **
    **        __where_begin(<term>)
    **          __row_set_test                  # Insert rowid into rowset
    **          Gosub      2 A
    **        __where_end()
    **
    ** Following the above, code to terminate the loop. Label A, the target
    ** of the Gosub above, jumps to the instruction right after the Goto.
    **
    **          Null       1                # Zero the rowset in reg 1
    **          Goto       B                # The loop is finished.
    **
    **       A: <loop body>                 # Return data, whatever.
    **
    **          Return     2                # Jump back to the Gosub
    **
    **       B: <after the loop>
    **
    ** Added 2014-05-26: If the table is a WITHOUT ROWID table, then
    ** use an ephemeral index instead of a row_set_t to record the primary
    ** keys of the rows we have already seen.
    **
    */
    where_clause_t *or_wc;    /* The OR-clause broken out into subterms */
    src_list_t *or_tab;       /* Shortened table list or OR-clause generation */
    index_t *cov = 0;             /* Potential covering index (or NULL) */
    int i_cov_cur = parser->n_tab++;  /* Cursor used for index scans (if any) */

    int reg_return = ++parser->n_mem;           /* Register used with OP_Gosub */
    int reg_rowset = 0;                        /* Register for row_set_t object */
    int reg_rowid = 0;                         /* Register holding rowid */
    int i_loop_body = __vdbe_make_label(parser);/* Start of loop body */
    int i_ret_init;                             /* Address of reg_return init */
    int untested_terms = 0;             /* Some terms not completely tested */
    int ii;                            /* Loop counter */
    expr_t *and_expr = 0;                /* An ".. AND (...)" expression */
    table_t *tab = tab_item->s_tab;

    term = loop->l_terms[0];
    assert( term!=0 );
    assert( term->e_operator & WO_OR );
    assert( (term->wt_flags & TERM_ORINFO)!=0 );
    or_wc = &term->u.or_info->wc;
    level->op = OP_Return;
    level->p1 = reg_return;

    /* Set up a new src_list_t in or_tab containing the table being scanned
    ** by this loop in the a[0] slot and all not_ready tables in a[1..] slots.
    ** This becomes the src_list_t in the recursive call to __where_begin().
    */
    if( w_info->n_level>1 || tab_item->fg.from_exists ){
      int n_not_ready;                 /* The number of not_ready tables */
      src_item_t *orig_src;              /* Original list of tables */
      n_not_ready = w_info->n_level - i_level - 1;
      or_tab = __db_malloc_raw_nn(db, SZ_SRCLIST(n_not_ready+1));
      if( or_tab==0 ) return not_ready;
      or_tab->n_alloc = (u_int8_t)(n_not_ready + 1);
      or_tab->n_src = or_tab->n_alloc;
      memcpy(or_tab->a, tab_item, sizeof(*tab_item));
      orig_src = w_info->tab_list->a;
      for(k=1; k<=n_not_ready; k++){
        memcpy(&or_tab->a[k], &orig_src[level[k].i_from], sizeof(or_tab->a[k]));
      }

      /* Clear the from_exists flag on the OR-optimized table entry so that
      ** the calls to __where_end() do not code early-exits after the
      ** first row is visited. The early exit applies to this table's
      ** overall loop - including the multiple OR branches and any WHERE
      ** conditions not passed to the sub-loops - not to the sub-loops.  */
      or_tab->a[0].fg.from_exists = 0;
    }else{
      or_tab = w_info->tab_list;
    }

    /* Initialize the rowset register to contain NULL. An SQL NULL is
    ** equivalent to an empty rowset.  Or, create an ephemeral index
    ** capable of holding primary keys in the case of a WITHOUT ROWID.
    **
    ** Also initialize reg_return to contain the address of the instruction
    ** immediately following the OP_Return at the bottom of the loop. This
    ** is required in a few obscure LEFT JOIN cases where control jumps
    ** over the top of the loop into the body of it. In this case the
    ** correct response for the end-of-loop code (the OP_Return) is to
    ** fall through to the next instruction, just as an OP_Next does if
    ** called on an uninitialized cursor.
    */
    if( (w_info->wctrl_flags & WHERE_DUPLICATES_OK)==0 ){
      if( HAS_ROWID(tab) ){
        reg_rowset = ++parser->n_mem;
        __vdbe_add_op2(v, OP_Null, 0, reg_rowset);
      }else{
        index_t *pk = __primary_key_index(tab);
        reg_rowset = parser->n_tab++;
        __vdbe_add_op2(v, OP_OpenEphemeral, reg_rowset, pk->n_key_col);
        __vdbe_set_p4_key_info(parser, pk);
      }
      reg_rowid = ++parser->n_mem;
    }
    i_ret_init = __vdbe_add_op2(v, OP_Integer, 0, reg_return);

    /* If the original WHERE clause is z of the form:  (x1 OR x2 OR ...) AND y
    ** Then for every term n, evaluate as the subexpression: n AND y
    ** That way, terms in y that are factored into the disjunction will
    ** be picked up by the recursive calls to __where_begin() below.
    **
    ** Actually, each subexpression is converted to "n AND w" where w is
    ** the "interesting" terms of z - terms that did not originate in the
    ** ON or USING clause of a LEFT JOIN, and terms that are usable as
    ** indices.
    **
    ** This optimization also only applies if the (x1 OR x2 OR ...) term
    ** is not contained in the ON clause of a LEFT JOIN.
    ** See ticket http://sqlite.org/src/info/f2369304e4
    **
    ** 2022-02-04:  Do not push down slices of a row-value comparison.
    ** In other words, "w" or "y" may not be a slice of a vector.  Otherwise,
    ** the initialization of the right-hand operand of the vector comparison
    ** might not occur, or might occur only in an OR branch that is not
    ** taken.  dbsqlfuzz 80a9fade844b4fb43564efc972bcb2c68270f5d1.
    **
    ** 2022-03-03:  Do not push down expressions that involve subqueries.
    ** The subquery might get coded as a subroutine.  Any table-references
    ** in the subquery might be resolved to index-references for the index on
    ** the OR branch in which the subroutine is coded.  But if the subroutine
    ** is invoked from a different OR branch that uses a different index, such
    ** index-references will not work.  tag-20220303a
    ** https://sqlite.org/forum/forumpost/36937b197273d403
    */
    if( wc->n_term>1 ){
      int i_term;
      for(i_term=0; i_term<wc->n_term; i_term++){
        expr_t *expr = wc->a[i_term].expr;
        if( &wc->a[i_term] == term ) continue;
        testcase( wc->a[i_term].wt_flags & TERM_VIRTUAL );
        testcase( wc->a[i_term].wt_flags & TERM_CODED );
        testcase( wc->a[i_term].wt_flags & TERM_SLICE );
        if( (wc->a[i_term].wt_flags & (TERM_VIRTUAL|TERM_CODED|TERM_SLICE))!=0 ){
          continue;
        }
        if( (wc->a[i_term].e_operator & WO_ALL)==0 ) continue;
        if( EXPR_HAS_PROPERTY(expr, EP_Subquery) ) continue;  /* tag-20220303a */
        expr = __expr_dup(db, expr, 0);
        and_expr = __expr_and(parser, and_expr, expr);
      }
      if( and_expr ){
        /* The extra 0x10000 bit on the opcode is masked off and does not
        ** become part of the new expr_t.op.  However, it does make the
        ** op==TK_AND comparison inside of __p_expr() false, and this
        ** prevents __p_expr() from applying the AND short-circuit
        ** optimization, which we do not want here. */
        and_expr = __p_expr(parser, TK_AND|0x10000, 0, and_expr);
      }
    }

    /* Run a separate WHERE clause for each term of the OR clause.  After
    ** eliminating duplicates from other WHERE clauses, the action for each
    ** sub-WHERE clause is to to invoke the main loop body as a subroutine.
    */
    EXPLAIN_QUERY_PLAN((parser, 1, "MULTI-INDEX OR"));
    for(ii=0; ii<or_wc->n_term; ii++){
      where_term_t *or_term = &or_wc->a[ii];
      if( or_term->left_cursor==i_cur || (or_term->e_operator & WO_AND)!=0 ){
        where_info_t *sub_w_info;           /* Info for single OR-term scan */
        expr_t *or_expr = or_term->expr; /* Current OR clause term */
        expr_t *p_delete;                  /* Local copy of OR clause term */
        int jmp1 = 0;                   /* Address of jump operation */
        testcase( (tab_item[0].fg.jointype & JT_LEFT)!=0
               && !EXPR_HAS_PROPERTY(or_expr, EP_OuterON)
        ); /* See TH3 vtab25.400 and ticket 614b25314c766238 */
        p_delete = or_expr = __expr_dup(db, or_expr, 0);
        if( db->malloc_failed ){
          __expr_delete(db, p_delete);
          continue;
        }
        if( and_expr ){
          and_expr->left = or_expr;
          or_expr = and_expr;
        }
        /* Loop through table entries that match term or_term. */
        EXPLAIN_QUERY_PLAN((parser, 1, "INDEX %d", ii+1));
        WHERETRACE(0xffffffff, ("Subplan for OR-clause:\n"));
        sub_w_info = __where_begin(parser, or_tab, or_expr, 0, 0, 0,
                                      WHERE_OR_SUBCLAUSE, i_cov_cur);
        assert( sub_w_info || parser->n_err );
        if( sub_w_info ){
          where_loop_t *sub_loop;
          int addr_explain = __where_explain_one_scan(
              parser, or_tab, &sub_w_info->a[0], 0
          );
          __where_add_scan_status(v, or_tab, &sub_w_info->a[0], addr_explain);

          /* This is the sub-WHERE clause body.  First skip over
          ** duplicate rows from prior sub-WHERE clauses, and record the
          ** rowid (or PRIMARY KEY) for the current row so that the same
          ** row will be skipped in subsequent sub-WHERE clauses.
          */
          if( (w_info->wctrl_flags & WHERE_DUPLICATES_OK)==0 ){
            int i_set = ((ii==or_wc->n_term-1)?-1:ii);
            if( HAS_ROWID(tab) ){
              __expr_code_get_column_of_table(v, tab, i_cur, -1, reg_rowid);
              jmp1 = __vdbe_add_op4_int(v, OP_RowSetTest, reg_rowset, 0,
                                          reg_rowid, i_set);
              VDBE_COVERAGE(v);
            }else{
              index_t *pk = __primary_key_index(tab);
              int n_pk = pk->n_key_col;
              int i_pk;
              int r;

              /* Read the PK into an array of temp registers. */
              r = __get_temp_range(parser, n_pk);
              for(i_pk=0; i_pk<n_pk; i_pk++){
                int i_col = pk->columns[i_pk];
                __expr_code_get_column_of_table(v, tab, i_cur, i_col,r+i_pk);
              }

              /* Check if the temp table already contains this key. If so,
              ** the row has already been included in the result set and
              ** can be ignored (by jumping past the Gosub below). Otherwise,
              ** insert the key into the temp table and proceed with processing
              ** the row.
              **
              ** Use some of the same optimizations as OP_RowSetTest: If i_set
              ** is zero, assume that the key cannot already be present in
              ** the temp table. And if i_set is -1, assume that there is no
              ** need to insert the key into the temp table, as it will never
              ** be tested for.  */
              if( i_set ){
                jmp1 = __vdbe_add_op4_int(v, OP_Found, reg_rowset, 0, r, n_pk);
                VDBE_COVERAGE(v);
              }
              if( i_set>=0 ){
                __vdbe_add_op3(v, OP_MakeRecord, r, n_pk, reg_rowid);
                __vdbe_add_op4_int(v, OP_IdxInsert, reg_rowset, reg_rowid,
                                     r, n_pk);
                if( i_set ) __vdbe_change_p5(v, OPFLAG_USESEEKRESULT);
              }

              /* Release the array of temp registers */
              __release_temp_range(parser, r, n_pk);
            }
          }

          /* Invoke the main loop body as a subroutine */
          __vdbe_add_op2(v, OP_Gosub, reg_return, i_loop_body);

          /* Jump here (skipping the main loop body subroutine) if the
          ** current sub-WHERE row is a duplicate from prior sub-WHEREs. */
          if( jmp1 ) __vdbe_jump_here(v, jmp1);

          /* The sub_w_info->untested_terms flag means that this OR term
          ** contained one or more AND term from a not_ready table.  The
          ** terms from the not_ready table could not be tested and will
          ** need to be tested later.
          */
          if( sub_w_info->untested_terms ) untested_terms = 1;

          /* If all of the OR-connected terms are optimized using the same
          ** index, and the index is opened using the same cursor number
          ** by each call to __where_begin() made by this loop, it may
          ** be possible to use that index as a covering index.
          **
          ** If the call to __where_begin() above resulted in a scan that
          ** uses an index, and this is either the first OR-connected term
          ** processed or the index is the same as that used by all previous
          ** terms, set cov to the candidate covering index. Otherwise, set
          ** cov to NULL to indicate that no candidate covering index will
          ** be available.
          */
          sub_loop = sub_w_info->a[0].w_loop;
          assert( (sub_loop->ws_flags & WHERE_AUTO_INDEX)==0 );
          if( (sub_loop->ws_flags & WHERE_INDEXED)!=0
           && (ii==0 || sub_loop->u.btree.p_index==cov)
           && (HAS_ROWID(tab) || !IS_PRIMARY_KEY_INDEX(sub_loop->u.btree.p_index))
          ){
            assert( sub_w_info->a[0].i_idx_cur==i_cov_cur );
            cov = sub_loop->u.btree.p_index;
          }else{
            cov = 0;
          }
          if( __where_uses_deferred_seek(sub_w_info) ){
            w_info->deferred_seek = 1;
          }

          /* Finish the loop through table entries that match term or_term. */
          __where_end(sub_w_info);
          EXPLAIN_QUERY_PLAN_POP(parser);
        }
        __expr_delete(db, p_delete);
      }
    }
    EXPLAIN_QUERY_PLAN_POP(parser);
    assert( level->w_loop==loop );
    assert( (loop->ws_flags & WHERE_MULTI_OR)!=0 );
    assert( (loop->ws_flags & WHERE_IN_ABLE)==0 );
    level->u.covering_idx = cov;
    if( cov ) level->i_idx_cur = i_cov_cur;
    if( and_expr ){
      and_expr->left = 0;
      __expr_delete(db, and_expr);
    }
    __vdbe_change_p1(v, i_ret_init, __vdbe_current_addr(v));
    __vdbe_goto(v, level->addr_brk);
    __vdbe_resolve_label(v, i_loop_body);

    /* Set the P2 operand of the OP_Return opcode that will end the current
    ** loop to point to this spot, which is the top of the next containing
    ** loop.  The byte-code formatter will use that P2 value as a hint to
    ** indent everything in between the this point and the final OP_Return.
    ** See tag-20220407a in vdbe.c and shell.c */
    assert( level->op==OP_Return );
    level->p2 = __vdbe_current_addr(v);

    if( w_info->tab_list!=or_tab ){ __db_free_nn(db, or_tab); }
    if( !untested_terms ) __disable_term(level, term);
  }else
#endif /* DBSQL_OMIT_OR_OPTIMIZATION */

  {
    /* Case 6:  There is no usable index.  We must do a complete
    **          scan of the entire table.
    */
    static const u_int8_t steps[] = { OP_Next, OP_Prev };
    static const u_int8_t starts[] = { OP_Rewind, OP_Last };
    assert( rev==0 || rev==1 );
    if( tab_item->fg.is_recursive ){
      /* Tables marked is_recursive have only a single row that is stored in
      ** a pseudo-cursor.  No need to Rewind or Next such cursors. */
      level->op = OP_Noop;
    }else{
      CODE_CURSOR_HINT(tab_item, w_info, level, 0);
      level->op = steps[rev];
      level->p1 = i_cur;
      level->p2 = 1 + __vdbe_add_op2(v, starts[rev],i_cur,level->addr_halt);
      VDBE_COVERAGE_IF(v, rev==0);
      VDBE_COVERAGE_IF(v, rev!=0);
      level->p5 = DBSQL_STMTSTATUS_FULLSCAN_STEP;
    }
  }

#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
  level->addr_visit = __vdbe_current_addr(v);
#endif

  /* Insert code to test every subexpression that can be completely
  ** computed using the current set of tables.
  **
  ** This loop may run between one and three times, depending on the
  ** constraints to be generated. The value of stack variable i_loop
  ** determines the constraints coded by each iteration, as follows:
  **
  ** i_loop==1: Code only expressions that are entirely covered by idx.
  ** i_loop==2: Code remaining expressions that do not contain correlated
  **           sub-queries.
  ** i_loop==3: Code all remaining expressions.
  **
  ** An effort is made to skip unnecessary iterations of the loop.
  **
  ** This optimization of causing simple query restrictions to occur before
  ** more complex one is call the "push-down" optimization in __my_sql.  Here
  ** in dbsql, the name is "__my_sql push-down", since there is also another
  ** totally unrelated optimization called "WHERE-clause push-down".
  ** Sometimes the qualifier is omitted, resulting in an ambiguity, so beware.
  */
  i_loop = (idx ? 1 : 2);
  do{
    int i_next = 0;                /* Next value for i_loop */
    for(term=wc->a, j=wc->n_term; j>0; j--, term++){
      expr_t *e;
      int skip_like_addr = 0;
      testcase( term->wt_flags & TERM_VIRTUAL );
      testcase( term->wt_flags & TERM_CODED );
      if( term->wt_flags & (TERM_VIRTUAL|TERM_CODED) ) continue;
      if( (term->prereq_all & level->not_ready)!=0 ){
        testcase( w_info->untested_terms==0
            && (w_info->wctrl_flags & WHERE_OR_SUBCLAUSE)!=0 );
        w_info->untested_terms = 1;
        continue;
      }
      e = term->expr;
      assert( e!=0 );
      if( tab_item->fg.jointype & (JT_LEFT|JT_LTORJ|JT_RIGHT) ){
        if( !EXPR_HAS_PROPERTY(e,EP_OuterON|EP_InnerON) ){
          /* Defer processing WHERE clause constraints until after outer
          ** join processing.  tag-20220513a */
          continue;
        }else if( (tab_item->fg.jointype & JT_LEFT)==JT_LEFT
               && !EXPR_HAS_PROPERTY(e,EP_OuterON) ){
          continue;
        }else{
          Bitmask m = __where_get_mask(&w_info->s_mask_set, e->w.i_join);
          if( m & level->not_ready ){
            /* An ON clause that is not ripe */
            continue;
          }
        }
      }
      if( i_loop==1 && !__expr_covered_by_index(e, level->i_tab_cur, idx) ){
        i_next = 2;
        continue;
      }
      if( i_loop<3 && (term->wt_flags & TERM_VARSELECT) ){
        if( i_next==0 ) i_next = 3;
        continue;
      }

      if( (term->wt_flags & TERM_LIKECOND)!=0 ){
        /* If the TERM_LIKECOND flag is set, that means that the range search
        ** is sufficient to guarantee that the LIKE operator is true, so we
        ** can skip the call to the like(A,B) function.  But this only works
        ** for strings.  So do not skip the call to the function on the pass
        ** that compares BLOBs. */
#ifdef DBSQL_LIKE_DOESNT_MATCH_BLOBS
        continue;
#else
        u_int32_t x = level->i_like_rep_cntr;
        if( x>0 ){
          skip_like_addr = __vdbe_add_op1(v, (x&1)?OP_IfNot:OP_If,(int)(x>>1));
          VDBE_COVERAGE_IF(v, (x&1)==1);
          VDBE_COVERAGE_IF(v, (x&1)==0);
        }
#endif
      }
#ifdef WHERETRACE_ENABLED /* 0xffffffff */
      if( __where_trace ){
        VDBE_NOOP_COMMENT((v, "WhereTerm[%d] (%p) priority=%d",
                         wc->n_term-j, term, i_loop));
      }
      if( __where_trace & 0x4000 ){
        __debug_printf("Coding auxiliary constraint:\n");
        __where_term_print(term, wc->n_term-j);
      }
#endif
      __expr_if_false(parser, e, addr_cont, DBSQL_JUMPIFNULL);
      if( skip_like_addr ) __vdbe_jump_here(v, skip_like_addr);
      term->wt_flags |= TERM_CODED;
    }
    i_loop = i_next;
  }while( i_loop>0 );

  /* Insert code to test for implied constraints based on transitivity
  ** of the "==" operator.
  **
  ** Example: If the WHERE clause contains "t1.a=t2.b" and "t2.b=123"
  ** and we are coding the t1 loop and the t2 loop has not yet coded,
  ** then we cannot use the "t1.a=t2.b" constraint, but we can code
  ** the implied "t1.a=123" constraint.
  */
  for(term=wc->a, j=wc->n_base; j>0; j--, term++){
    expr_t *e, s_e_alt;
    where_term_t *alt;
    if( term->wt_flags & (TERM_VIRTUAL|TERM_CODED) ) continue;
    if( (term->e_operator & (WO_EQ|WO_IS))==0 ) continue;
    if( (term->e_operator & WO_EQUIV)==0 ) continue;
    if( term->left_cursor!=i_cur ) continue;
    if( tab_item->fg.jointype & (JT_LEFT|JT_LTORJ|JT_RIGHT) ) continue;
    e = term->expr;
#ifdef WHERETRACE_ENABLED /* 0x4001 */
    if( (__where_trace & 0x4001)==0x4001 ){
      __debug_printf("Coding transitive constraint:\n");
      __where_term_print(term, wc->n_term-j);
    }
#endif
    assert( !EXPR_HAS_PROPERTY(e, EP_OuterON) );
    assert( (term->prereq_right & level->not_ready)!=0 );
    assert( (term->e_operator & (WO_OR|WO_AND))==0 );
    alt = __where_find_term(wc, i_cur, term->u.x.left_column, not_ready,
                    WO_EQ|WO_IN|WO_IS, 0);
    if( alt==0 ) continue;
    if( alt->wt_flags & (TERM_CODED) ) continue;
    if( EXPR_HAS_PROPERTY(alt->expr, EP_Collate) ) continue;
    if( (alt->e_operator & WO_IN)
     && EXPR_USE_X_SELECT(alt->expr)
     && (alt->expr->x.p_select->e_list->n_expr>1)
    ){
      continue;
    }
    testcase( alt->e_operator & WO_EQ );
    testcase( alt->e_operator & WO_IS );
    testcase( alt->e_operator & WO_IN );
    VDBE_MODULE_COMMENT((v, "begin transitive constraint"));
    s_e_alt = *alt->expr;
    s_e_alt.left = e->left;
    __expr_if_false(parser, &s_e_alt, addr_cont, DBSQL_JUMPIFNULL);
    alt->wt_flags |= TERM_CODED;
  }

  /* For a RIGHT OUTER JOIN, record the fact that the current row has
  ** been matched at least once.
  */
  if( level->rj ){
    table_t *tab;
    int n_pk;
    int r;
    int jmp1 = 0;
    where_right_join_t *rj = level->rj;

    /* tab is the right-hand table of the RIGHT JOIN.  Generate code that
    ** will record that the current row of that table has been matched at
    ** least once.  This is accomplished by storing the PK for the row in
    ** both the i_match index and the reg_bloom Bloom filter.
    */
    tab = w_info->tab_list->a[level->i_from].s_tab;
    if( HAS_ROWID(tab) ){
      r = __get_temp_range(parser, 2);
      __expr_code_get_column_of_table(v, tab, level->i_tab_cur, -1, r+1);
      n_pk = 1;
    }else{
      int i_pk;
      index_t *pk = __primary_key_index(tab);
      n_pk = pk->n_key_col;
      r = __get_temp_range(parser, n_pk+1);
      for(i_pk=0; i_pk<n_pk; i_pk++){
        int i_col = pk->columns[i_pk];
        __expr_code_get_column_of_table(v, tab, i_cur, i_col,r+1+i_pk);
      }
    }
    jmp1 = __vdbe_add_op4_int(v, OP_Found, rj->i_match, 0, r+1, n_pk);
    VDBE_COVERAGE(v);
    VDBE_COMMENT((v, "match against %s", tab->name));
    __vdbe_add_op3(v, OP_MakeRecord, r+1, n_pk, r);
    __vdbe_add_op4_int(v, OP_IdxInsert, rj->i_match, r, r+1, n_pk);
    __vdbe_add_op4_int(v, OP_FilterAdd, rj->reg_bloom, 0, r+1, n_pk);
    __vdbe_change_p5(v, OPFLAG_USESEEKRESULT);
    __vdbe_jump_here(v, jmp1);
    __release_temp_range(parser, r, n_pk+1);
  }

  /* For a LEFT OUTER JOIN, generate code that will record the fact that
  ** at least one row of the right table has matched the left table.
  */
  if( level->i_left_join ){
    level->addr_first = __vdbe_current_addr(v);
    __vdbe_add_op2(v, OP_Integer, 1, level->i_left_join);
    VDBE_COMMENT((v, "record LEFT JOIN hit"));
    if( level->rj==0 ){
      goto code_outer_join_constraints; /* WHERE clause constraints */
    }
  }

  if( level->rj ){
    /* Create a subroutine used to process all interior loops and code
    ** of the RIGHT JOIN.  During normal operation, the subroutine will
    ** be in-line with the rest of the code.  But at the end, a separate
    ** loop will run that invokes this subroutine for unmatched rows
    ** of tab, with all tables to left begin set to NULL.
    */
    where_right_join_t *rj = level->rj;
    __vdbe_add_op2(v, OP_BeginSubrtn, 0, rj->reg_return);
    rj->addr_subrtn = __vdbe_current_addr(v);
    assert( parser->within_rj_subrtn < 255 );
    parser->within_rj_subrtn++;

    /* WHERE clause constraints must be deferred until after outer join
    ** row elimination has completed, since WHERE clause constraints apply
    ** to the results of the OUTER JOIN.  The following loop generates the
    ** appropriate WHERE clause constraint checks.  tag-20220513a.
    */
  code_outer_join_constraints:
    for(term=wc->a, j=0; j<wc->n_base; j++, term++){
      testcase( term->wt_flags & TERM_VIRTUAL );
      testcase( term->wt_flags & TERM_CODED );
      if( term->wt_flags & (TERM_VIRTUAL|TERM_CODED) ) continue;
      if( (term->prereq_all & level->not_ready)!=0 ){
        assert( w_info->untested_terms );
        continue;
      }
      if( tab_item->fg.jointype & JT_LTORJ ) continue;
      assert( term->expr );
      __expr_if_false(parser, term->expr, addr_cont, DBSQL_JUMPIFNULL);
      term->wt_flags |= TERM_CODED;
    }
  }

#if WHERETRACE_ENABLED /* 0x4001 */
  if( __where_trace & 0x4000 ){
    __debug_printf("All WHERE-clause terms after coding level %d:\n",
                       i_level);
    __where_clause_print(wc);
  }
  if( __where_trace & 0x1 ){
    __debug_printf("End Coding level %d:  notReady=%llx\n",
       i_level, (u_int64_t)level->not_ready);
  }
#endif
  return level->not_ready;
}

/*
** Generate the code for the loop that finds all non-matched terms
** for a RIGHT JOIN.
*/
DBSQL_NOINLINE void __where_right_join_loop(
  where_info_t *w_info,
  int i_level,
  where_level_t *level
){
  parser_t *parser = w_info->parser;
  vdbe_t *v = parser->vdbe;
  where_right_join_t *rj = level->rj;
  expr_t *sub_where = 0;
  where_clause_t *wc = &w_info->s_wc;
  where_info_t *sub_w_info;
  where_loop_t *loop = level->w_loop;
  src_item_t *tab_item = &w_info->tab_list->a[level->i_from];
  src_list_t *from;
  union {
    src_list_t s_src;
    u_int8_t from_space[SZ_SRCLIST_1];
  } u_src;
  Bitmask m_all = 0;
  int k;

  EXPLAIN_QUERY_PLAN((parser, 1, "RIGHT-JOIN %s", tab_item->s_tab->name));
  __vdbe_no_jumps_outside_subrtn(v, rj->addr_subrtn, rj->end_subrtn,
                                  rj->reg_return);
  for(k=0; k<i_level; k++){
    int i_idx_cur;
    src_item_t *right;
    assert( w_info->a[k].w_loop->i_tab == w_info->a[k].i_from );
    right = &w_info->tab_list->a[w_info->a[k].i_from];
    m_all |= w_info->a[k].w_loop->mask_self;
    if( right->fg.via_coroutine ){
      subquery_t *subq;
      assert( right->fg.is_subquery && right->u4.subq!=0 );
      subq = right->u4.subq;
      assert( subq->p_select!=0 && subq->p_select->e_list!=0 );
      __vdbe_add_op3(
          v, OP_Null, 0, subq->reg_result,
          subq->reg_result + subq->p_select->e_list->n_expr-1
      );
    }
    __vdbe_add_op1(v, OP_NullRow, w_info->a[k].i_tab_cur);
    i_idx_cur = w_info->a[k].i_idx_cur;
    if( i_idx_cur ){
      __vdbe_add_op1(v, OP_NullRow, i_idx_cur);
    }
  }
  if( (tab_item->fg.jointype & JT_LTORJ)==0 ){
    m_all |= loop->mask_self;
    for(k=0; k<wc->n_term; k++){
      where_term_t *term = &wc->a[k];
      if( (term->wt_flags & (TERM_VIRTUAL|TERM_SLICE))!=0
       && term->e_operator!=WO_ROWVAL
      ){
        break;
      }
      if( term->prereq_all & ~m_all ) continue;
      if( EXPR_HAS_PROPERTY(term->expr, EP_OuterON|EP_InnerON) ) continue;
      sub_where = __expr_and(parser, sub_where,
                                 __expr_dup(parser->db, term->expr, 0));
    }
  }
  if( level->i_idx_cur ){
    /* sub_where may contain expressions that read from an index on the
    ** table on the RHS of the right join. All such expressions first test
    ** if the index is pointing at a NULL row, and if so, read from the
    ** table cursor instead. So ensure that the index cursor really is 
    ** pointing at a NULL row here, so that no values are read from it during
    ** the scan of the RHS of the RIGHT join below.  */
    __vdbe_add_op1(v, OP_NullRow, level->i_idx_cur);
  }
  from = &u_src.s_src;
  from->n_src = 1;
  from->n_alloc = 1;
  memcpy(&from->a[0], tab_item, sizeof(src_item_t));
  from->a[0].fg.jointype = 0;
  assert( parser->within_rj_subrtn < 100 );
  parser->within_rj_subrtn++;
  sub_w_info = __where_begin(parser, from, sub_where, 0, 0, 0,
                                WHERE_RIGHT_JOIN, 0);
  if( sub_w_info ){
    int i_cur = level->i_tab_cur;
    int r = ++parser->n_mem;
    int n_pk;
    int jmp;
    int addr_cont = __where_continue_label(sub_w_info);
    table_t *tab = tab_item->s_tab;
    if( HAS_ROWID(tab) ){
      __expr_code_get_column_of_table(v, tab, i_cur, -1, r);
      n_pk = 1;
    }else{
      int i_pk;
      index_t *pk = __primary_key_index(tab);
      n_pk = pk->n_key_col;
      parser->n_mem += n_pk - 1;
      for(i_pk=0; i_pk<n_pk; i_pk++){
        int i_col = pk->columns[i_pk];
        __expr_code_get_column_of_table(v, tab, i_cur, i_col,r+i_pk);
      }
    }
    jmp = __vdbe_add_op4_int(v, OP_Filter, rj->reg_bloom, 0, r, n_pk);
    VDBE_COVERAGE(v);
    __vdbe_add_op4_int(v, OP_Found, rj->i_match, addr_cont, r, n_pk);
    VDBE_COVERAGE(v);
    __vdbe_jump_here(v, jmp);
    __vdbe_add_op2(v, OP_Gosub, rj->reg_return, rj->addr_subrtn);
    __where_end(sub_w_info);
  }
  __expr_delete(parser->db, sub_where);
  EXPLAIN_QUERY_PLAN_POP(parser);
  assert( parser->within_rj_subrtn>0 );
  parser->within_rj_subrtn--;
}
