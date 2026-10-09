/*
** 2008 August 16
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains routines used for walking the parser tree for
** an SQL statement.
*/
#include "dbsql_int.h"
#include <stdlib.h>
#include <string.h>


#if !defined(DBSQL_OMIT_WINDOWFUNC)
/*
** Walk all expressions linked into the list of window_t objects passed
** as the second argument.
*/
static int __walk_window_list(walker_t *walker, window_t *list, int one_only){
  window_t *win;
  for(win=list; win; win=win->next_win){
    int rc;
    rc = __walk_expr_list(walker, win->order_by);
    if( rc ) return WRC_Abort;
    rc = __walk_expr_list(walker, win->partition);
    if( rc ) return WRC_Abort;
    rc = __walk_expr(walker, win->filter);
    if( rc ) return WRC_Abort;
    rc = __walk_expr(walker, win->p_start);
    if( rc ) return WRC_Abort;
    rc = __walk_expr(walker, win->p_end);
    if( rc ) return WRC_Abort;
    if( one_only ) break;
  }
  return WRC_Continue;
}
#endif

/*
** Walk an expression tree.  Invoke the callback once for each node
** of the expression, while descending.  (In other words, the callback
** is invoked before visiting children.)
**
** The return value from the callback should be one of the WRC_*
** constants to specify how to proceed with the walk.
**
**    WRC_Continue      Continue descending down the tree.
**
**    WRC_Prune         Do not descend into child nodes, but allow
**                      the walk to continue with sibling nodes.
**
**    WRC_Abort         Do no more callbacks.  Unwind the stack and
**                      return from the top-level walk call.
**
** The return value from this routine is WRC_Abort to abandon the tree walk
** and WRC_Continue to continue.
*/
DBSQL_NOINLINE int __walk_expr_nn(walker_t *walker, expr_t *expr){
  int rc;
  testcase( EXPR_HAS_PROPERTY(expr, EP_TokenOnly) );
  testcase( EXPR_HAS_PROPERTY(expr, EP_Reduced) );
  while(1){
    rc = walker->expr_callback(walker, expr);
    if( rc ) return rc & WRC_Abort;
    if( !EXPR_HAS_PROPERTY(expr,(EP_TokenOnly|EP_Leaf)) ){
      assert( expr->x.list==0 || expr->right==0 );
      if( expr->left && __walk_expr_nn(walker, expr->left) ){
        return WRC_Abort;
      }
      if( expr->right ){
        assert( !EXPR_HAS_PROPERTY(expr, EP_WinFunc) );
        expr = expr->right;
        continue;
      }else if( EXPR_USE_X_SELECT(expr) ){
        assert( !EXPR_HAS_PROPERTY(expr, EP_WinFunc) );
        if( __walk_select(walker, expr->x.p_select) ) return WRC_Abort;
      }else{
        if( expr->x.list ){
          if( __walk_expr_list(walker, expr->x.list) ) return WRC_Abort;
        }
#ifndef DBSQL_OMIT_WINDOWFUNC
        if( EXPR_HAS_PROPERTY(expr, EP_WinFunc) ){
          if( __walk_window_list(walker, expr->y.win, 1) ) return WRC_Abort;
        }
#endif
      }
    }
    break;
  }
  return WRC_Continue;
}
int __walk_expr(walker_t *walker, expr_t *expr){
  return expr ? __walk_expr_nn(walker,expr) : WRC_Continue;
}

/*
** Call __walk_expr() for every expression in list p or until
** an abort request is seen.
*/
int __walk_expr_list(walker_t *walker, expr_list_t *p){
  int i;
  struct expr_list_item_t *item;
  if( p ){
    for(i=p->n_expr, item=p->a; i>0; i--, item++){
      if( __walk_expr(walker, item->expr) ) return WRC_Abort;
    }
  }
  return WRC_Continue;
}

/*
** This is a no-op callback for walker_t->select_callback2.  If this
** callback is set, then the select_t->win_defn list is traversed.
*/
void __walk_win_defn_dummy_callback(walker_t *walker, select_t *p){
  UNUSED_PARAMETER(walker);
  UNUSED_PARAMETER(p);
  /* No-op */
}

/*
** Walk all expressions associated with SELECT statement p.  Do
** not invoke the SELECT callback on p, but do (of course) invoke
** any expr callbacks and SELECT callbacks that come from subqueries.
** Return WRC_Abort or WRC_Continue.
*/
int __walk_select_expr(walker_t *walker, select_t *p){
  if( __walk_expr_list(walker, p->e_list) ) return WRC_Abort;
  if( __walk_expr(walker, p->where) ) return WRC_Abort;
  if( __walk_expr_list(walker, p->group_by) ) return WRC_Abort;
  if( __walk_expr(walker, p->having) ) return WRC_Abort;
  if( __walk_expr_list(walker, p->order_by) ) return WRC_Abort;
  if( __walk_expr(walker, p->limit) ) return WRC_Abort;
#if !defined(DBSQL_OMIT_WINDOWFUNC)
  if( p->win_defn ){
    parser_t *parser;
    if( walker->select_callback2==__walk_win_defn_dummy_callback
     || ((parser = walker->parser)!=0 && IN_RENAME_OBJECT)
#ifndef DBSQL_OMIT_CTE
     || walker->select_callback2==__select_pop_with
#endif
    ){
      /* The following may return WRC_Abort if there are unresolvable
      ** symbols (e.g. a table that does not exist) in a window definition. */
      int rc = __walk_window_list(walker, p->win_defn, 0);
      return rc;
    }
  }
#endif
  return WRC_Continue;
}

/*
** Walk the parse trees associated with all subqueries in the
** FROM clause of SELECT statement p.  Do not invoke the select
** callback on p, but do invoke it on each FROM clause subquery
** and on any subqueries further down in the tree.  Return 
** WRC_Abort or WRC_Continue;
*/
int __walk_select_from(walker_t *walker, select_t *p){
  src_list_t *src;
  int i;
  src_item_t *item;

  src = p->src;
  if( ALWAYS(src) ){
    for(i=src->n_src, item=src->a; i>0; i--, item++){
      if( item->fg.is_subquery
       && __walk_select(walker, item->u4.subq->p_select)
      ){
        return WRC_Abort;
      }
      if( item->fg.is_tab_func
       && __walk_expr_list(walker, item->u1.func_arg)
      ){
        return WRC_Abort;
      }
    }
  }
  return WRC_Continue;
}

/*
** Call __walk_expr() for every expression in select_t statement p.
** Invoke __walk_select() for subqueries in the FROM clause and
** on the compound select chain, p->prior. 
**
** If it is not NULL, the select_callback() callback is invoked before
** the walk of the expressions and FROM clause. The select_callback2()
** method is invoked following the walk of the expressions and FROM clause,
** but only if both select_callback and select_callback2 are both non-NULL
** and if the expressions and FROM clause both return WRC_Continue;
**
** Return WRC_Continue under normal conditions.  Return WRC_Abort if
** there is an abort request.
**
** If the walker_t does not have an select_callback() then this routine
** is a no-op returning WRC_Continue.
*/
int __walk_select(walker_t *walker, select_t *p){
  int rc;
  if( p==0 ) return WRC_Continue;
  if( walker->select_callback==0 ) return WRC_Continue;
  do{
    rc = walker->select_callback(walker, p);
    if( rc ) return rc & WRC_Abort;
    if( __walk_select_expr(walker, p)
     || __walk_select_from(walker, p)
    ){
      return WRC_Abort;
    }
    if( walker->select_callback2 ){
      walker->select_callback2(walker, p);
    }
    p = p->prior;
  }while( p!=0 );
  return WRC_Continue;
}

/* Increase the walker_depth when entering a subquery, and
** decrease when leaving the subquery.
*/
int __walker_depth_increase(walker_t *walker, select_t *p_select){
  UNUSED_PARAMETER(p_select);
  walker->walker_depth++;
  return WRC_Continue;
}
void __walker_depth_decrease(walker_t *walker, select_t *p_select){
  UNUSED_PARAMETER(p_select);
  walker->walker_depth--;
}


/*
** No-op routine for the parse-tree walker.
**
** When this routine is the walker_t.expr_callback then expression trees
** are walked without any actions being taken at each node.  Presumably,
** when this routine is used for walker_t.expr_callback then 
** walker_t.select_callback is set to do something useful for every 
** subquery in the parser tree.
*/
int __expr_walk_noop(walker_t *__not_used, expr_t *__not_used2){
  UNUSED_PARAMETER2(__not_used, __not_used2);
  return WRC_Continue;
}

/*
** No-op routine for the parse-tree walker for SELECT statements.
** subquery in the parser tree.
*/
int __select_walk_noop(walker_t *__not_used, select_t *__not_used2){
  UNUSED_PARAMETER2(__not_used, __not_used2);
  return WRC_Continue;
}
