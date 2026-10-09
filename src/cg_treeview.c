/*
** 2015-06-08
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
** This file contains C code to implement the tree_view_t debugging routines.
** These routines print a parse tree to standard output for debugging and
** analysis. 
**
** The interfaces in this file is only available when compiling
** with DBSQL_DEBUG.
*/
#include "dbsql_int.h"
#ifdef DBSQL_DEBUG

/*
** Add a new subitem to the tree.  The more_to_follow flag indicates that this
** is not the last item in the tree.
*/
static void __tree_view_push(tree_view_t **pp, u_int8_t more_to_follow){
  tree_view_t *p = *pp;
  if( p==0 ){
    *pp = p = dbsql_malloc64( sizeof(*p) );
    if( p==0 ) return;
    memset(p, 0, sizeof(*p));
  }else{
    p->i_level++;
  }
  assert( more_to_follow==0 || more_to_follow==1 );
  if( p->i_level<(int)sizeof(p->line) ) p->line[p->i_level] = more_to_follow;
}

/*
** Finished with one layer of the tree
*/
static void __tree_view_pop(tree_view_t **pp){
  tree_view_t *p = *pp;
  if( p==0 ) return;
  p->i_level--;
  if( p->i_level<0 ){
    dbsql_free(p);
    *pp = 0;
  }
}

/*
** Generate a single line of output for the tree, with a prefix that contains
** all the appropriate tree lines
*/
void __tree_view_line(tree_view_t *p, const char *format, ...){
  va_list ap;
  int i;
  str_accum_t acc;
  char buf[1000];
  __str_accum_init(&acc, 0, buf, sizeof(buf), 0);
  if( p ){
    for(i=0; i<p->i_level && i<(int)sizeof(p->line)-1; i++){
      dbsql_str_append(&acc, p->line[i] ? "|   " : "    ", 4);
    }
    dbsql_str_append(&acc, p->line[i] ? "|-- " : "'-- ", 4);
  }
  if( format!=0 ){
    va_start(ap, format);
    dbsql_str_vappendf(&acc, format, ap);
    va_end(ap);
    assert( acc.n_char>0 || acc.acc_error );
    dbsql_str_append(&acc, "\n", 1);
  }
  __str_accum_finish(&acc);
  fprintf(stdout,"%s", buf);
  fflush(stdout);
}

/*
** Shorthand for starting a new tree item that consists of a single label
*/
static void __tree_view_item(tree_view_t *p, const char *label,u_int8_t more_follows){
  __tree_view_push(&p, more_follows);
  __tree_view_line(p, "%s", label);
}

/*
** Show a list of column_t objects in tree format.
*/
void __tree_view_column_list(
  tree_view_t *view,
  const column_t *cols,
  int n_col,
  u_int8_t more_to_follow
){
  int i;
  __tree_view_push(&view, more_to_follow);
  __tree_view_line(view, "COLUMNS");
  for(i=0; i<n_col; i++){
    u_int16_t flg = cols[i].col_flags;
    int col_more_to_follow = i<(n_col - 1);
    __tree_view_push(&view, col_more_to_follow);
    __tree_view_line(view, 0);
    printf(" %s", cols[i].cn_name);
    switch( cols[i].c_type ){
      case COLTYPE_ANY:      printf(" ANY");        break;
      case COLTYPE_BLOB:     printf(" BLOB");       break;
      case COLTYPE_INT:      printf(" INT");        break;
      case COLTYPE_INTEGER:  printf(" INTEGER");    break;
      case COLTYPE_REAL:     printf(" REAL");       break;
      case COLTYPE_TEXT:     printf(" TEXT");       break;
      case COLTYPE_CUSTOM: {
        if( flg & COLFLAG_HASTYPE ){
          const char *z = cols[i].cn_name;
          z += strlen(z)+1;
          printf(" X-%s", z);
          break;
        }
      }
    }
    if( flg & COLFLAG_PRIMKEY ) printf(" PRIMARY KEY");
    if( flg & COLFLAG_HIDDEN ) printf(" HIDDEN");
#ifdef COLFLAG_NOEXPAND
    if( flg & COLFLAG_NOEXPAND ) printf(" NO-EXPAND");
#endif
    if( flg ) printf(" flags=%04x", flg);
    printf("\n");      
    fflush(stdout);
    __tree_view_pop(&view);
  }
  __tree_view_pop(&view);
}

/*
** Generate a human-readable description of a WITH clause.
*/
void __tree_view_with(tree_view_t *view, const with_t *with, u_int8_t more_to_follow){
  int i;
  if( with==0 ) return;
  if( with->n_cte==0 ) return;
  if( with->outer ){
    __tree_view_line(view, "WITH (0x%p, pOuter=0x%p)",with,with->outer);
  }else{
    __tree_view_line(view, "WITH (0x%p)", with);
  }
  if( with->n_cte>0 ){
    __tree_view_push(&view, more_to_follow);
    for(i=0; i<with->n_cte; i++){
      str_accum_t x;
      char line[1000];
      const struct cte_t *cte = &with->a[i];
      __str_accum_init(&x, 0, line, sizeof(line), 0);
      dbsql_str_appendf(&x, "%s", cte->name);
      if( cte->cols && cte->cols->n_expr>0 ){
        char c_sep = '(';
        int j;
        for(j=0; j<cte->cols->n_expr; j++){
          dbsql_str_appendf(&x, "%c%s", c_sep, cte->cols->a[j].e_name);
          c_sep = ',';
        }
        dbsql_str_appendf(&x, ")");
      }
      if( cte->m10d!=M10D_ANY ){
        dbsql_str_appendf(&x, " %sMATERIALIZED", 
           cte->m10d==M10D_NO ? "NOT " : "");
      }
      if( cte->use ){
        dbsql_str_appendf(&x, " (pUse=0x%p, nUse=%d)", cte->use,
                 cte->use->n_use);
      }
      __str_accum_finish(&x);
      __tree_view_item(view, line, i<with->n_cte-1);
      __tree_view_select(view, cte->p_select, 0);
      __tree_view_pop(&view);
    }
    __tree_view_pop(&view);
  }
}

/*
** Generate a human-readable description of a src_list_t object.
*/
void __tree_view_src_list(tree_view_t *view, const src_list_t *src){
  int i;
  if( src==0 ) return;
  for(i=0; i<src->n_src; i++){
    const src_item_t *item = &src->a[i];
    str_accum_t x;
    int n = 0;
    char line[1000];
    __str_accum_init(&x, 0, line, sizeof(line), 0);
    x.printf_flags |= DBSQL_PRINTF_INTERNAL;
    dbsql_str_appendf(&x, "{%d:*} %!S", item->i_cursor, item);
    if( item->s_tab ){
      dbsql_str_appendf(&x, " tab=%Q nCol=%d ptr=%p used=%llx%s",
           item->s_tab->name, item->s_tab->n_col, item->s_tab, 
           item->col_used,
           item->fg.rowid_used ? "+rowid" : "");
    }
    if( (item->fg.jointype & (JT_LEFT|JT_RIGHT))==(JT_LEFT|JT_RIGHT) ){
      dbsql_str_appendf(&x, " FULL-OUTER-JOIN");
    }else if( item->fg.jointype & JT_LEFT ){
      dbsql_str_appendf(&x, " LEFT-JOIN");
    }else if( item->fg.jointype & JT_RIGHT ){
      dbsql_str_appendf(&x, " RIGHT-JOIN");
    }else if( item->fg.jointype & JT_CROSS ){
      dbsql_str_appendf(&x, " CROSS-JOIN");
    }
    if( item->fg.jointype & JT_LTORJ ){
      dbsql_str_appendf(&x, " LTORJ");
    }
    if( item->fg.from_ddl ){
      dbsql_str_appendf(&x, " DDL");
    }
    if( item->fg.is_cte ){
      static const char *mats[] = {",MAT", "", ",NO-MAT"};
      dbsql_str_appendf(&x, " CteUse=%d%s",
                          item->u2.cte_use->n_use,
                          mats[item->u2.cte_use->m10d]);
    }
    if( item->fg.is_on || (item->fg.is_using==0 && item->u3.on!=0) ){
      dbsql_str_appendf(&x, " isOn");
    }
    if( item->fg.is_tab_func )      dbsql_str_appendf(&x, " isTabFunc");
    if( item->fg.is_correlated )   dbsql_str_appendf(&x, " isCorrelated");
    if( item->fg.is_materialized ) dbsql_str_appendf(&x, " isMaterialized");
    if( item->fg.via_coroutine )   dbsql_str_appendf(&x, " viaCoroutine");
    if( item->fg.not_cte )         dbsql_str_appendf(&x, " notCte");
    if( item->fg.is_nested_from )   dbsql_str_appendf(&x, " isNestedFrom");
    if( item->fg.fixed_schema )    dbsql_str_appendf(&x, " fixedSchema");
    if( item->fg.had_schema )      dbsql_str_appendf(&x, " hadSchema");
    if( item->fg.is_subquery )     dbsql_str_appendf(&x, " isSubquery");

    __str_accum_finish(&x);
    __tree_view_item(view, line, i<src->n_src-1);
    n = 0;
    if( item->fg.is_subquery ) n++;
    if( item->fg.is_tab_func ) n++;
    if( item->fg.is_using || item->u3.on!=0 ) n++;
    if( item->fg.is_using ){
      __tree_view_id_list(view, item->u3.p_using, (--n)>0, "USING");
    }else if( item->u3.on!=0 ){
      __tree_view_item(view, "ON", (--n)>0);
      __tree_view_expr(view, item->u3.on, 0);
      __tree_view_pop(&view);
    }
    if( item->fg.is_subquery ){
      assert( n==1 );
      if( item->s_tab ){
        table_t *tab = item->s_tab;
        __tree_view_column_list(view, tab->cols, tab->n_col, 1);
      }
      assert( (int)item->fg.is_nested_from == IS_NESTED_FROM(item) );
      __tree_view_select(view, item->u4.subq->p_select, 0);
    }
    if( item->fg.is_tab_func ){
      __tree_view_expr_list(view, item->u1.func_arg, 0, "func-args:");
    }
    __tree_view_pop(&view);
  }
}

/*
** Generate a human-readable description of a select_t object.
*/
void __tree_view_select(tree_view_t *view, const select_t *p, u_int8_t more_to_follow){
  int n = 0;
  int cnt = 0;
  if( p==0 ){
    __tree_view_line(view, "nil-SELECT");
    return;
  } 
  __tree_view_push(&view, more_to_follow);
  if( p->with ){
    __tree_view_with(view, p->with, 1);
    cnt = 1;
    __tree_view_push(&view, 1);
  }
  do{
    if( p->sel_flags & SF_WhereBegin ){
      __tree_view_line(view, "__where_begin()");
    }else{
      __tree_view_line(view,
        "SELECT%s%s (%u/%p) selFlags=0x%x nSelectRow=%d",
        ((p->sel_flags & SF_Distinct) ? " DISTINCT" : ""),
        ((p->sel_flags & SF_Aggregate) ? " agg_flag" : ""),
        p->sel_id, p, p->sel_flags,
        (int)p->n_select_row
      );
    }
    if( cnt++ ) __tree_view_pop(&view);
    if( p->prior ){
      n = 1000;
    }else{
      n = 0;
      if( p->src && p->src->n_src && p->src->n_alloc ) n++;
      if( p->where ) n++;
      if( p->group_by ) n++;
      if( p->having ) n++;
      if( p->order_by ) n++;
      if( p->limit ) n++;
#ifndef DBSQL_OMIT_WINDOWFUNC
      if( p->win ) n++;
      if( p->win_defn ) n++;
#endif
    }
    if( p->e_list ){
      __tree_view_expr_list(view, p->e_list, n>0, "result-set");
    }
    n--;
#ifndef DBSQL_OMIT_WINDOWFUNC
    if( p->win ){
      window_t *x;
      __tree_view_push(&view, (n--)>0);
      __tree_view_line(view, "window-functions");
      for(x=p->win; x; x=x->next_win){
        __tree_view_win_func(view, x, x->next_win!=0);
      }
      __tree_view_pop(&view);
    }
#endif
    if( p->src && p->src->n_src && p->src->n_alloc ){
      __tree_view_push(&view, (n--)>0);
      __tree_view_line(view, "FROM");
      __tree_view_src_list(view, p->src);
      __tree_view_pop(&view);
    }
    if( p->where ){
      __tree_view_item(view, "WHERE", (n--)>0);
      __tree_view_expr(view, p->where, 0);
      __tree_view_pop(&view);
    }
    if( p->group_by ){
      __tree_view_expr_list(view, p->group_by, (n--)>0, "GROUPBY");
    }
    if( p->having ){
      __tree_view_item(view, "HAVING", (n--)>0);
      __tree_view_expr(view, p->having, 0);
      __tree_view_pop(&view);
    }
#ifndef DBSQL_OMIT_WINDOWFUNC
    if( p->win_defn ){
      window_t *x;
      __tree_view_item(view, "WINDOW", (n--)>0);
      for(x=p->win_defn; x; x=x->next_win){
        __tree_view_window(view, x, x->next_win!=0);
      }
      __tree_view_pop(&view);
    }
#endif
    if( p->order_by ){
      __tree_view_expr_list(view, p->order_by, (n--)>0, "ORDERBY");
    }
    if( p->limit ){
      __tree_view_item(view, "LIMIT", (n--)>0);
      __tree_view_expr(view, p->limit->left, p->limit->right!=0);
      if( p->limit->right ){
        __tree_view_item(view, "OFFSET", 0);
        __tree_view_expr(view, p->limit->right, 0);
        __tree_view_pop(&view);
      }
      __tree_view_pop(&view);
    }
    if( p->prior ){
      const char *op = "UNION";
      switch( p->op ){
        case TK_ALL:         op = "UNION ALL";  break;
        case TK_INTERSECT:   op = "INTERSECT";  break;
        case TK_EXCEPT:      op = "EXCEPT";     break;
      }
      __tree_view_item(view, op, 1);
    }
    p = p->prior;
  }while( p!=0 );
  __tree_view_pop(&view);
}

#ifndef DBSQL_OMIT_WINDOWFUNC
/*
** Generate a description of starting or stopping bounds
*/
void __tree_view_bound(
  tree_view_t *view,        /* View context */
  u_int8_t bound,              /* UNBOUNDED, CURRENT, PRECEDING, FOLLOWING */
  expr_t *expr,            /* Value for PRECEDING or FOLLOWING */
  u_int8_t more_to_follow         /* True if more to follow */
){
  switch( bound ){
    case TK_UNBOUNDED: {
      __tree_view_item(view, "UNBOUNDED", more_to_follow);
      __tree_view_pop(&view);
      break;
    }
    case TK_CURRENT: {
      __tree_view_item(view, "CURRENT", more_to_follow);
      __tree_view_pop(&view);
      break;
    }
    case TK_PRECEDING: {
      __tree_view_item(view, "PRECEDING", more_to_follow);
      __tree_view_expr(view, expr, 0);
      __tree_view_pop(&view);
      break;
    }
    case TK_FOLLOWING: {
      __tree_view_item(view, "FOLLOWING", more_to_follow);
      __tree_view_expr(view, expr, 0);
      __tree_view_pop(&view);
      break;
    }
  }
}
#endif /* DBSQL_OMIT_WINDOWFUNC */

#ifndef DBSQL_OMIT_WINDOWFUNC
/*
** Generate a human-readable explanation for a window_t object
*/
void __tree_view_window(tree_view_t *view, const window_t *win, u_int8_t more){
  int n_element = 0;
  if( win==0 ) return;
  if( win->filter ){
    __tree_view_item(view, "FILTER", 1);
    __tree_view_expr(view, win->filter, 0);
    __tree_view_pop(&view);
    if( win->frm_type==TK_FILTER ) return;
  }
  __tree_view_push(&view, more);
  if( win->name ){
    __tree_view_line(view, "OVER %s (%p)", win->name, win);
  }else{
    __tree_view_line(view, "OVER (%p)", win);
  }
  if( win->z_base )    n_element++;
  if( win->order_by ) n_element++;
  if( win->frm_type!=0 && win->frm_type!=TK_FILTER ) n_element++;
  if( win->exclude ) n_element++;
  if( win->z_base ){
    __tree_view_push(&view, (--n_element)>0);
    __tree_view_line(view, "window: %s", win->z_base);
    __tree_view_pop(&view);
  }
  if( win->partition ){
    __tree_view_expr_list(view, win->partition, n_element>0,"PARTITION-BY");
  }
  if( win->order_by ){
    __tree_view_expr_list(view, win->order_by, (--n_element)>0, "ORDER-BY");
  }
  if( win->frm_type!=0 && win->frm_type!=TK_FILTER ){
    char buf[30];
    const char *frm_type = "ROWS";
    if( win->frm_type==TK_RANGE ) frm_type = "RANGE";
    if( win->frm_type==TK_GROUPS ) frm_type = "GROUPS";
    dbsql_snprintf(sizeof(buf),buf,"%s%s",frm_type,
        win->implicit_frame ? " (implied)" : "");
    __tree_view_item(view, buf, (--n_element)>0);
    __tree_view_bound(view, win->start, win->p_start, 1);
    __tree_view_bound(view, win->end, win->p_end, 0);
    __tree_view_pop(&view);
  }
  if( win->exclude ){
    char buf[30];
    const char *exclude;
    switch( win->exclude ){
      case TK_NO:      exclude = "NO OTHERS";   break;
      case TK_CURRENT: exclude = "CURRENT ROW"; break;
      case TK_GROUP:   exclude = "GROUP";       break;
      case TK_TIES:    exclude = "TIES";        break;
      default:
        dbsql_snprintf(sizeof(buf),buf,"invalid(%d)", win->exclude);
        exclude = buf;
        break;
    }
    __tree_view_push(&view, 0);
    __tree_view_line(view, "EXCLUDE %s", exclude);
    __tree_view_pop(&view);
  }
  __tree_view_pop(&view);
}
#endif /* DBSQL_OMIT_WINDOWFUNC */

#ifndef DBSQL_OMIT_WINDOWFUNC
/*
** Generate a human-readable explanation for a window_t Function object
*/
void __tree_view_win_func(tree_view_t *view, const window_t *win, u_int8_t more){
  if( win==0 ) return;
  __tree_view_push(&view, more);
  __tree_view_line(view, "WINFUNC %s(%d)",
                       win->w_func->name, win->w_func->n_arg);
  __tree_view_window(view, win, 0);
  __tree_view_pop(&view);
}
#endif /* DBSQL_OMIT_WINDOWFUNC */

/*
** Generate a human-readable explanation of an expression tree.
*/
void __tree_view_expr(tree_view_t *view, const expr_t *expr, u_int8_t more_to_follow){
  const char *bin_op = 0;   /* Binary operator */
  const char *uni_op = 0;   /* Unary operator */
  char flgs[200];
  __tree_view_push(&view, more_to_follow);
  if( expr==0 ){
    __tree_view_line(view, "nil");
    __tree_view_pop(&view);
    return;
  }
  if( expr->flags || expr->aff_expr || expr->vva_flags || expr->agg_info ){
    str_accum_t x;
    __str_accum_init(&x, 0, flgs, sizeof(flgs), 0);
    dbsql_str_appendf(&x, " fg.af=%x.%c",
      expr->flags, expr->aff_expr ? expr->aff_expr : 'n');
    if( EXPR_HAS_PROPERTY(expr, EP_OuterON) ){
      dbsql_str_appendf(&x, " outer.iJoin=%d", expr->w.i_join);
    }
    if( EXPR_HAS_PROPERTY(expr, EP_InnerON) ){
      dbsql_str_appendf(&x, " inner.iJoin=%d", expr->w.i_join);
    }
    if( EXPR_HAS_PROPERTY(expr, EP_FromDDL) ){
      dbsql_str_appendf(&x, " DDL");
    }
    if( EXPR_HAS_VVA_PROPERTY(expr, EP_Immutable) ){
      dbsql_str_appendf(&x, " IMMUTABLE");
    }
    if( expr->agg_info!=0 ){
      dbsql_str_appendf(&x, " agg-column[%d]", expr->i_agg);
    }
    __str_accum_finish(&x);
  }else{
    flgs[0] = 0;
  }
  switch( expr->op ){
    case TK_AGG_COLUMN: {
      __tree_view_line(view, "AGG{%d:%d}%s",
            expr->i_table, expr->i_column, flgs);
      break;
    }
    case TK_COLUMN: {
      if( expr->i_table<0 ){
        /* This only happens when coding check constraints */
        char op2[16];
        if( expr->op2 ){
          dbsql_snprintf(sizeof(op2),op2," op2=0x%02x",expr->op2);
        }else{
          op2[0] = 0;
        }
        __tree_view_line(view, "COLUMN(%d)%s%s",
                                    expr->i_column, flgs, op2);
      }else{
        assert( EXPR_USE_Y_TAB(expr) );
        __tree_view_line(view, "{%d:%d} pTab=%p%s",
                        expr->i_table, expr->i_column,
                        expr->y.tab, flgs);
      }
      if( EXPR_HAS_PROPERTY(expr, EP_FixedCol) ){
        __tree_view_expr(view, expr->left, 0);
      }
      break;
    }
    case TK_INTEGER: {
      if( expr->flags & EP_IntValue ){
        __tree_view_line(view, "%d", expr->u.i_value);
      }else{
        __tree_view_line(view, "%s", expr->u.token);
      }
      break;
    }
#ifndef DBSQL_OMIT_FLOATING_POINT
    case TK_FLOAT: {
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __tree_view_line(view,"%s", expr->u.token);
      break;
    }
#endif
    case TK_STRING: {
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __tree_view_line(view,"%Q", expr->u.token);
      break;
    }
    case TK_NULL: {
      __tree_view_line(view,"NULL");
      break;
    }
    case TK_TRUEFALSE: {
      __tree_view_line(view,"%s%s",
         __expr_truth_value(expr) ? "TRUE" : "FALSE", flgs);
      break;
    }
#ifndef DBSQL_OMIT_BLOB_LITERAL
    case TK_BLOB: {
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __tree_view_line(view,"%s", expr->u.token);
      break;
    }
#endif
    case TK_VARIABLE: {
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __tree_view_line(view,"VARIABLE(%s,%d)",
                          expr->u.token, expr->i_column);
      break;
    }
    case TK_REGISTER: {
      __tree_view_line(view,"REGISTER(%d)", expr->i_table);
      break;
    }
    case TK_ID: {
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __tree_view_line(view,"ID \"%w\"", expr->u.token);
      break;
    }
#ifndef DBSQL_OMIT_CAST
    case TK_CAST: {
      /* Expressions of the form:   CAST(left AS token) */
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __tree_view_line(view,"CAST %Q", expr->u.token);
      __tree_view_expr(view, expr->left, 0);
      break;
    }
#endif /* DBSQL_OMIT_CAST */
    case TK_LT:      bin_op = "LT";     break;
    case TK_LE:      bin_op = "LE";     break;
    case TK_GT:      bin_op = "GT";     break;
    case TK_GE:      bin_op = "GE";     break;
    case TK_NE:      bin_op = "NE";     break;
    case TK_EQ:      bin_op = "EQ";     break;
    case TK_IS:      bin_op = "IS";     break;
    case TK_ISNOT:   bin_op = "ISNOT";  break;
    case TK_AND:     bin_op = "AND";    break;
    case TK_OR:      bin_op = "OR";     break;
    case TK_PLUS:    bin_op = "ADD";    break;
    case TK_STAR:    bin_op = "MUL";    break;
    case TK_MINUS:   bin_op = "SUB";    break;
    case TK_REM:     bin_op = "REM";    break;
    case TK_BITAND:  bin_op = "BITAND"; break;
    case TK_BITOR:   bin_op = "BITOR";  break;
    case TK_SLASH:   bin_op = "DIV";    break;
    case TK_LSHIFT:  bin_op = "LSHIFT"; break;
    case TK_RSHIFT:  bin_op = "RSHIFT"; break;
    case TK_CONCAT:  bin_op = "CONCAT"; break;
    case TK_DOT:     bin_op = "DOT";    break;
    case TK_LIMIT:   bin_op = "LIMIT";  break;

    case TK_UMINUS:  uni_op = "UMINUS"; break;
    case TK_UPLUS:   uni_op = "UPLUS";  break;
    case TK_BITNOT:  uni_op = "BITNOT"; break;
    case TK_NOT:     uni_op = "NOT";    break;
    case TK_ISNULL:  uni_op = "ISNULL"; break;
    case TK_NOTNULL: uni_op = "NOTNULL"; break;

    case TK_TRUTH: {
      int x;
      const char *ops[] = {
         "IS-FALSE", "IS-TRUE", "IS-NOT-FALSE", "IS-NOT-TRUE"
      };
      assert( expr->op2==TK_IS || expr->op2==TK_ISNOT );
      assert( expr->right );
      assert( __expr_skip_collate_and_likely(expr->right)->op
                  == TK_TRUEFALSE );
      x = (expr->op2==TK_ISNOT)*2 + __expr_truth_value(expr->right);
      uni_op = ops[x];
      break;
    }

    case TK_SPAN: {
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __tree_view_line(view, "SPAN %Q", expr->u.token);
      __tree_view_expr(view, expr->left, 0);
      break;
    }

    case TK_COLLATE: {
      /* COLLATE operators without the EP_Collate flag are intended to
      ** emulate collation associated with a table column.  These show
      ** up in the treeview output as "SOFT-COLLATE".  Explicit COLLATE
      ** operators that appear in the original SQL always have the
      ** EP_Collate bit set and appear in treeview output as just "COLLATE" */
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __tree_view_line(view, "%sCOLLATE %Q%s",
        !EXPR_HAS_PROPERTY(expr, EP_Collate) ? "SOFT-" : "",
        expr->u.token, flgs);
      __tree_view_expr(view, expr->left, 0);
      break;
    }

    case TK_AGG_FUNCTION:
    case TK_FUNCTION: {
      expr_list_t *farg;       /* List of function arguments */
      window_t *win;
      if( EXPR_HAS_PROPERTY(expr, EP_TokenOnly) ){
        farg = 0;
        win = 0;
      }else{
        assert( EXPR_USE_X_LIST(expr) );
        farg = expr->x.list;
#ifndef DBSQL_OMIT_WINDOWFUNC
        win = IS_WINDOW_FUNC(expr) ? expr->y.win : 0;
#else
        win = 0;
#endif 
      }
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      if( expr->op==TK_AGG_FUNCTION ){
        __tree_view_line(view, "AGG_FUNCTION%d %Q%s agg=%d[%d]/%p",
                             expr->op2, expr->u.token, flgs,
                             expr->agg_info ? expr->agg_info->sel_id : 0,
                             expr->i_agg, expr->agg_info);
      }else if( expr->op2!=0 ){
        const char *op2;
        char buf[8];
        dbsql_snprintf(sizeof(buf),buf,"0x%02x",expr->op2);
        op2 = buf;
        if( expr->op2==NC_IsCheck ) op2 = "NC_IsCheck";
        if( expr->op2==NC_IdxExpr ) op2 = "NC_IdxExpr";
        if( expr->op2==NC_PartIdx ) op2 = "NC_PartIdx";
        if( expr->op2==NC_GenCol ) op2 = "NC_GenCol";
        __tree_view_line(view, "FUNCTION %Q%s op2=%s",
                            expr->u.token, flgs, op2);
      }else{
        __tree_view_line(view, "FUNCTION %Q%s", expr->u.token, flgs);
      }
      if( farg ){
        __tree_view_expr_list(view, farg, win!=0 || expr->left, 0);
        if( expr->left ){
          expr_t *ob = expr->left;
          assert( ob->op==TK_ORDER );
          assert( EXPR_USE_X_LIST(ob) );
          __tree_view_expr_list(view, ob->x.list, win!=0, "ORDERBY");
        }
      }
#ifndef DBSQL_OMIT_WINDOWFUNC
      if( win ){
        __tree_view_window(view, win, 0);
      }
#endif
      break;
    }
    case TK_ORDER: {
      __tree_view_expr_list(view, expr->x.list, 0, "ORDERBY");
      break;
    }
#ifndef DBSQL_OMIT_SUBQUERY
    case TK_EXISTS: {
      assert( EXPR_USE_X_SELECT(expr) );
      __tree_view_line(view, "EXISTS-expr flags=0x%x", expr->flags);
      __tree_view_select(view, expr->x.p_select, 0);
      break;
    }
    case TK_SELECT: {
      assert( EXPR_USE_X_SELECT(expr) );
      __tree_view_line(view, "subquery-expr flags=0x%x", expr->flags);
      __tree_view_select(view, expr->x.p_select, 0);
      break;
    }
    case TK_IN: {
      dbsql_str_t *str = dbsql_str_new(0);
      char *z;
      dbsql_str_appendf(str, "IN flags=0x%x", expr->flags);
      if( expr->i_table ) dbsql_str_appendf(str, " iTable=%d",expr->i_table);
      if( EXPR_HAS_PROPERTY(expr, EP_Subrtn) ){
        dbsql_str_appendf(str, " subrtn(%d,%d)",
            expr->y.sub.reg_return, expr->y.sub.i_addr);
      }
      z = dbsql_str_finish(str);
      __tree_view_line(view, z);
      dbsql_free(z);
      __tree_view_expr(view, expr->left, 1);
      if( EXPR_USE_X_SELECT(expr) ){
        __tree_view_select(view, expr->x.p_select, 0);
      }else{
        __tree_view_expr_list(view, expr->x.list, 0, 0);
      }
      break;
    }
#endif /* DBSQL_OMIT_SUBQUERY */

    /*
    **    x BETWEEN y AND z
    **
    ** This is equivalent to
    **
    **    x>=y AND x<=z
    **
    ** X is stored in expr->left.
    ** Y is stored in expr->list->a[0].expr.
    ** Z is stored in expr->list->a[1].expr.
    */
    case TK_BETWEEN: {
      const expr_t *p_x, *y, *p_z;
      p_x = expr->left;
      assert( EXPR_USE_X_LIST(expr) );
      assert( expr->x.list->n_expr==2 );
      y = expr->x.list->a[0].expr;
      p_z = expr->x.list->a[1].expr;
      __tree_view_line(view, "BETWEEN%s", flgs);
      __tree_view_expr(view, p_x, 1);
      __tree_view_expr(view, y, 1);
      __tree_view_expr(view, p_z, 0);
      break;
    }
    case TK_TRIGGER: {
      /* If the opcode is TK_TRIGGER, then the expression is a reference
      ** to a column in the new.* or old.* pseudo-tables available to
      ** trigger programs. In this case expr_t.i_table is set to 1 for the
      ** new.* pseudo-table, or 0 for the old.* pseudo-table. expr_t.i_column
      ** is set to the column of the pseudo-table to read, or to -1 to
      ** read the rowid field.
      */
      __tree_view_line(view, "%s(%d)", 
          expr->i_table ? "NEW" : "OLD", expr->i_column);
      break;
    }
    case TK_CASE: {
      __tree_view_line(view, "CASE");
      __tree_view_expr(view, expr->left, 1);
      assert( EXPR_USE_X_LIST(expr) );
      __tree_view_expr_list(view, expr->x.list, 0, 0);
      break;
    }
#ifndef DBSQL_OMIT_TRIGGER
    case TK_RAISE: {
      const char *type = "unk";
      switch( expr->aff_expr ){
        case OE_Rollback:   type = "rollback";  break;
        case OE_Abort:      type = "abort";     break;
        case OE_Fail:       type = "fail";      break;
        case OE_Ignore:     type = "ignore";    break;
      }
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      __tree_view_line(view, "RAISE %s", type);
      __tree_view_expr(view, expr->left, 0);
      break;
    }
#endif
    case TK_MATCH: {
      __tree_view_line(view, "MATCH {%d:%d}%s",
                          expr->i_table, expr->i_column, flgs);
      __tree_view_expr(view, expr->right, 0);
      break;
    }
    case TK_VECTOR: {
      char *z = dbsql_mprintf("VECTOR%s",flgs);
      assert( EXPR_USE_X_LIST(expr) );
      __tree_view_bare_expr_list(view, expr->x.list, z);
      dbsql_free(z);
      break;
    }
    case TK_SELECT_COLUMN: {
      __tree_view_line(view, "SELECT-COLUMN %d of [0..%d]%s",
              expr->i_column, expr->i_table-1,
              expr->right==expr->left ? " (SELECT-owner)" : "");
      assert( EXPR_USE_X_SELECT(expr->left) );
      __tree_view_select(view, expr->left->x.p_select, 0);
      break;
    }
    case TK_IF_NULL_ROW: {
      __tree_view_line(view, "IF-NULL-ROW %d", expr->i_table);
      __tree_view_expr(view, expr->left, 0);
      break;
    }
    case TK_ERROR: {
      expr_t tmp;
      __tree_view_line(view, "ERROR");
      tmp = *expr;
      tmp.op = expr->op2;
      __tree_view_expr(view, &tmp, 0);
      break;
    }
    case TK_ROW: {
      if( expr->i_column<=0 ){
        __tree_view_line(view, "First FROM table rowid");
      }else{
        __tree_view_line(view, "First FROM table column %d",
            expr->i_column-1);
      }
      break;
    }
    default: {
      __tree_view_line(view, "op=%d", expr->op);
      break;
    }
  }
  if( bin_op ){
    __tree_view_line(view, "%s%s", bin_op, flgs);
    __tree_view_expr(view, expr->left, 1);
    __tree_view_expr(view, expr->right, 0);
  }else if( uni_op ){
    __tree_view_line(view, "%s%s", uni_op, flgs);
   __tree_view_expr(view, expr->left, 0);
  }
  __tree_view_pop(&view);
}


/*
** Generate a human-readable explanation of an expression list.
*/
void __tree_view_bare_expr_list(
  tree_view_t *view,
  const expr_list_t *list,
  const char *label
){
  if( label==0 || label[0]==0 ) label = "LIST";
  if( list==0 ){
    __tree_view_line(view, "%s (empty)", label);
  }else{
    int i;
    __tree_view_line(view, "%s", label);
    for(i=0; i<list->n_expr; i++){
      int j = list->a[i].u.x.i_order_by_col;
      u_int8_t sort_flags = list->a[i].fg.sort_flags;
      char *name = list->a[i].e_name;
      int more_to_follow = i<list->n_expr - 1;
      if( j || name || sort_flags ){
        __tree_view_push(&view, more_to_follow);
        more_to_follow = 0;
        __tree_view_line(view, 0);
        if( name ){
          switch( list->a[i].fg.e_name ){
            default:
              fprintf(stdout, "AS %s ", name);
              break;
            case ENAME_TAB:
              fprintf(stdout, "TABLE-ALIAS-NAME(\"%s\") ", name);
              if( list->a[i].fg.used ) fprintf(stdout, "(used) ");
              if( list->a[i].fg.using_term ) fprintf(stdout, "(USING-term) ");
              if( list->a[i].fg.no_expand ) fprintf(stdout, "(NoExpand) ");
              break;
            case ENAME_SPAN:
              fprintf(stdout, "SPAN(\"%s\") ", name);
              break;
          }
        }
        if( j ){
          fprintf(stdout, "iOrderByCol=%d ", j);
        }
        if( sort_flags & KEYINFO_ORDER_DESC ){
          fprintf(stdout, "DESC ");
        }else if( sort_flags & KEYINFO_ORDER_BIGNULL ){
          fprintf(stdout, "NULLS-LAST");
        }
        fprintf(stdout, "\n");
        fflush(stdout);
      }
      __tree_view_expr(view, list->a[i].expr, more_to_follow);
      if( j || name || sort_flags ){
        __tree_view_pop(&view);
      }
    }
  }
}
void __tree_view_expr_list(
  tree_view_t *view,
  const expr_list_t *list,
  u_int8_t more_to_follow,
  const char *label
){
  __tree_view_push(&view, more_to_follow);
  __tree_view_bare_expr_list(view, list, label);
  __tree_view_pop(&view);
}

/*
** Generate a human-readable explanation of an id-list.
*/
void __tree_view_bare_id_list(
  tree_view_t *view,
  const id_list_t *list,
  const char *label
){
  if( label==0 || label[0]==0 ) label = "LIST";
  if( list==0 ){
    __tree_view_line(view, "%s (empty)", label);
  }else{
    int i;
    __tree_view_line(view, "%s", label);
    for(i=0; i<list->n_id; i++){
      char *name = list->a[i].name;
      int more_to_follow = i<list->n_id - 1;
      if( name==0 ) name = "(null)";
      __tree_view_push(&view, more_to_follow);
      __tree_view_line(view, 0);
      fprintf(stdout, "%s\n", name);
      __tree_view_pop(&view);
    }
  }
}
void __tree_view_id_list(
  tree_view_t *view,
  const id_list_t *list,
  u_int8_t more_to_follow,
  const char *label
){
  __tree_view_push(&view, more_to_follow);
  __tree_view_bare_id_list(view, list, label);
  __tree_view_pop(&view);
}

/*
** Generate a human-readable explanation of a list of upsert_t objects
*/
void __tree_view_upsert(
  tree_view_t *view,
  const upsert_t *upsert,
  u_int8_t more_to_follow
){
  if( upsert==0 ) return;
  __tree_view_push(&view, more_to_follow);
  while( upsert ){
    int n;
    __tree_view_push(&view, upsert->next_upsert!=0 || more_to_follow);
    __tree_view_line(view, "ON CONFLICT DO %s", 
         upsert->is_do_update ? "UPDATE" : "NOTHING");
    n = (upsert->upsert_set!=0) + (upsert->upsert_where!=0);
    __tree_view_expr_list(view, upsert->upsert_target, (n--)>0, "TARGET");
    __tree_view_expr_list(view, upsert->upsert_set, (n--)>0, "SET");
    if( upsert->upsert_where ){
      __tree_view_item(view, "WHERE", (n--)>0);
      __tree_view_expr(view, upsert->upsert_where, 0);
      __tree_view_pop(&view);
    }
    __tree_view_pop(&view);
    upsert = upsert->next_upsert;
  }
  __tree_view_pop(&view);
}

#if TREETRACE_ENABLED
/*
** Generate a human-readable diagram of the data structure that go
** into generating an DELETE statement.
*/
void __tree_view_delete(
  const with_t *with,
  const src_list_t *tab_list,
  const expr_t *where,
  const expr_list_t *order_by,
  const expr_t *limit,
  const trigger_t *P_TRIGGER
){
  int n = 0;
  tree_view_t *view = 0;
  __tree_view_push(&view, 0);
  __tree_view_line(view, "DELETE");
  if( with ) n++;
  if( tab_list ) n++;
  if( where ) n++;
  if( order_by ) n++;
  if( limit ) n++;
  if( P_TRIGGER ) n++;
  if( with ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_with(view, with, 0);
    __tree_view_pop(&view);
  }
  if( tab_list ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_line(view, "FROM");
    __tree_view_src_list(view, tab_list);
    __tree_view_pop(&view);
  }
  if( where ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_line(view, "WHERE");
    __tree_view_expr(view, where, 0);
    __tree_view_pop(&view);
  }
  if( order_by ){
    __tree_view_expr_list(view, order_by, (--n)>0, "ORDER-BY");
  }
  if( limit ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_line(view, "LIMIT");
    __tree_view_expr(view, limit, 0);
    __tree_view_pop(&view);
  }
  if( P_TRIGGER ){
    __tree_view_trigger(view, P_TRIGGER, (--n)>0, 1);
  }
  __tree_view_pop(&view);
}
#endif /* TREETRACE_ENABLED */

#if TREETRACE_ENABLED
/*
** Generate a human-readable diagram of the data structure that go
** into generating an INSERT statement.
*/
void __tree_view_insert(
  const with_t *with,
  const src_list_t *tab_list,
  const id_list_t *column_list,
  const select_t *p_select,
  const expr_list_t *expr_list,
  int on_error,
  const upsert_t *upsert,
  const trigger_t *P_TRIGGER
){
  tree_view_t *view = 0;
  int n = 0;
  const char *label = "INSERT";
  switch( on_error ){
    case OE_Replace:  label = "REPLACE";             break;
    case OE_Ignore:   label = "INSERT OR IGNORE";    break;
    case OE_Rollback: label = "INSERT OR ROLLBACK";  break;
    case OE_Abort:    label = "INSERT OR ABORT";     break;
    case OE_Fail:     label = "INSERT OR FAIL";      break;
  }
  __tree_view_push(&view, 0);
  __tree_view_line(view, label);
  if( with ) n++;
  if( tab_list ) n++;
  if( column_list ) n++;
  if( p_select ) n++;
  if( expr_list ) n++;
  if( upsert ) n++;
  if( P_TRIGGER ) n++;
  if( with ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_with(view, with, 0);
    __tree_view_pop(&view);
  }
  if( tab_list ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_line(view, "INTO");
    __tree_view_src_list(view, tab_list);
    __tree_view_pop(&view);
  }
  if( column_list ){
    __tree_view_id_list(view, column_list, (--n)>0, "COLUMNS");
  }
  if( p_select ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_line(view, "DATA-SOURCE");
    __tree_view_select(view, p_select, 0);
    __tree_view_pop(&view);
  }
  if( expr_list ){
    __tree_view_expr_list(view, expr_list, (--n)>0, "VALUES");
  }
  if( upsert ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_line(view, "UPSERT");
    __tree_view_upsert(view, upsert, 0);
    __tree_view_pop(&view);
  }
  if( P_TRIGGER ){
    __tree_view_trigger(view, P_TRIGGER, (--n)>0, 1);
  }
  __tree_view_pop(&view);
}
#endif /* TREETRACE_ENABLED */

#if TREETRACE_ENABLED
/*
** Generate a human-readable diagram of the data structure that go
** into generating an UPDATE statement.
*/
void __tree_view_update(
  const with_t *with,
  const src_list_t *tab_list,
  const expr_list_t *changes,
  const expr_t *where,
  int on_error,
  const expr_list_t *order_by,
  const expr_t *limit,
  const upsert_t *upsert,
  const trigger_t *P_TRIGGER
){
  int n = 0;
  tree_view_t *view = 0;
  const char *label = "UPDATE";
  switch( on_error ){
    case OE_Replace:  label = "UPDATE OR REPLACE";   break;
    case OE_Ignore:   label = "UPDATE OR IGNORE";    break;
    case OE_Rollback: label = "UPDATE OR ROLLBACK";  break;
    case OE_Abort:    label = "UPDATE OR ABORT";     break;
    case OE_Fail:     label = "UPDATE OR FAIL";      break;
  }
  __tree_view_push(&view, 0);
  __tree_view_line(view, label);
  if( with ) n++;
  if( tab_list ) n++;
  if( changes ) n++;
  if( where ) n++;
  if( order_by ) n++;
  if( limit ) n++;
  if( upsert ) n++;
  if( P_TRIGGER ) n++;
  if( with ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_with(view, with, 0);
    __tree_view_pop(&view);
  }
  if( tab_list ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_line(view, "FROM");
    __tree_view_src_list(view, tab_list);
    __tree_view_pop(&view);
  }
  if( changes ){
    __tree_view_expr_list(view, changes, (--n)>0, "SET");
  }
  if( where ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_line(view, "WHERE");
    __tree_view_expr(view, where, 0);
    __tree_view_pop(&view);
  }
  if( order_by ){
    __tree_view_expr_list(view, order_by, (--n)>0, "ORDER-BY");
  }
  if( limit ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_line(view, "LIMIT");
    __tree_view_expr(view, limit, 0);
    __tree_view_pop(&view);
  }
  if( upsert ){
    __tree_view_push(&view, (--n)>0);
    __tree_view_line(view, "UPSERT");
    __tree_view_upsert(view, upsert, 0);
    __tree_view_pop(&view);
  }
  if( P_TRIGGER ){
    __tree_view_trigger(view, P_TRIGGER, (--n)>0, 1);
  }
  __tree_view_pop(&view);
}
#endif /* TREETRACE_ENABLED */

#ifndef DBSQL_OMIT_TRIGGER
/*
** Show a human-readable graph of a trigger_step_t
*/
void __tree_view_trigger_step(
  tree_view_t *view,
  const trigger_step_t *step,
  u_int8_t more_to_follow,
  u_int8_t show_full_list
){
  int cnt = 0;
  if( step==0 ) return;
  __tree_view_push(&view, 
      more_to_follow || (show_full_list && step->next!=0));
  do{
    if( cnt++ && step->next==0 ){
      __tree_view_pop(&view);
      __tree_view_push(&view, 0);
    }
    __tree_view_line(view, "%s", step->span ? step->span : "RETURNING");
  }while( show_full_list && (step = step->next)!=0 );
  __tree_view_pop(&view);
}
  
/*
** Show a human-readable graph of a trigger_t
*/
void __tree_view_trigger(
  tree_view_t *view,
  const trigger_t *P_TRIGGER,
  u_int8_t more_to_follow,
  u_int8_t show_full_list
){
  int cnt = 0;
  if( P_TRIGGER==0 ) return;
  __tree_view_push(&view,
     more_to_follow || (show_full_list && P_TRIGGER->next!=0));
  do{
    if( cnt++ && P_TRIGGER->next==0 ){
      __tree_view_pop(&view);
      __tree_view_push(&view, 0);
    }
    __tree_view_line(view, "TRIGGER %s", P_TRIGGER->name);
    __tree_view_push(&view, 0);
    __tree_view_trigger_step(view, P_TRIGGER->step_list, 0, 1);
    __tree_view_pop(&view);
  }while( show_full_list && (P_TRIGGER = P_TRIGGER->next)!=0 );
  __tree_view_pop(&view);
}
#endif /* DBSQL_OMIT_TRIGGER */
  

/*
** These simplified versions of the tree-view routines omit unnecessary
** parameters.  These variants are intended to be used from a symbolic
** debugger, such as "gdb", during interactive debugging sessions.
**
** This routines are given external linkage so that they will always be
** accessible to the debugging, and to avoid warnings about unused
** functions.  But these routines only exist in debugging builds, so they
** do not contaminate the interface.
**
** See Also:
**
**     __show_where_term() in where.c
*/
void __show_expr(const expr_t *p){ __tree_view_expr(0,p,0); }
void __show_expr_list(const expr_list_t *p){ __tree_view_expr_list(0,p,0,0);}
void __show_id_list(const id_list_t *p){ __tree_view_id_list(0,p,0,0); }
void __show_src_list(const src_list_t *p){
  tree_view_t *view = 0;
  __tree_view_push(&view, 0);
  __tree_view_line(view, "SRCLIST");
  __tree_view_src_list(view,p);
  __tree_view_pop(&view);
}
void __show_select(const select_t *p){ __tree_view_select(0,p,0); }
void __show_with(const with_t *p){ __tree_view_with(0,p,0); }
void __show_upsert(const upsert_t *p){ __tree_view_upsert(0,p,0); }
#ifndef DBSQL_OMIT_TRIGGER
void __show_trigger_step(const trigger_step_t *p){
  __tree_view_trigger_step(0,p,0,0);
}
void __show_trigger_step_list(const trigger_step_t *p){
  __tree_view_trigger_step(0,p,0,1);
}
void __show_trigger(const trigger_t *p){ __tree_view_trigger(0,p,0,0); }
void __show_trigger_list(const trigger_t *p){ __tree_view_trigger(0,p,0,1);}
#endif
#ifndef DBSQL_OMIT_WINDOWFUNC
void __show_window(const window_t *p){ __tree_view_window(0,p,0); }
void __show_win_func(const window_t *p){ __tree_view_win_func(0,p,0); }
#endif

#endif /* DBSQL_DEBUG */
