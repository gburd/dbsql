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
** This module contains C code that generates VDBE code used to process
** the WHERE clause of SQL statements.
**
** This file was originally part of where.c but was split out to improve
** readability and editability.  This file contains utility routines for
** analyzing expr_t objects in the WHERE clause.
*/
#include "dbsql_int.h"
#include "where_int.h"

/* Forward declarations */
static void __expr_analyze(src_list_t*, where_clause_t*, int);

/*
** Deallocate all memory associated with a where_or_info_t object.
*/
static void __where_or_info_delete(DBSQL *db, where_or_info_t *p){
  __where_clause_clear(&p->wc);
  __dbsql_db_free(db, p);
}

/*
** Deallocate all memory associated with a where_and_info_t object.
*/
static void __where_and_info_delete(DBSQL *db, where_and_info_t *p){
  __where_clause_clear(&p->wc);
  __dbsql_db_free(db, p);
}

/*
** Add a single new where_term_t entry to the where_clause_t object wc.
** The new where_term_t object is constructed from expr_t p and with wt_flags.
** The index in wc->a[] of the new where_term_t is returned on success.
** 0 is returned if the new where_term_t could not be added due to a memory
** allocation error.  The memory allocation failure will be recorded in
** the db->malloc_failed flag so that higher-level functions can detect it.
**
** This routine will increase the size of the wc->a[] array as necessary.
**
** If the wt_flags argument includes TERM_DYNAMIC, then responsibility
** for freeing the expression p is assumed by the where_clause_t object wc.
** This is true even if this routine fails to allocate a new where_term_t.
**
** WARNING:  This routine might reallocate the space used to store
** __where_terms.  All pointers to __where_terms should be invalidated after
** calling this routine.  Such pointers may be reinitialized by referencing
** the wc->a[] array.
*/
static int __where_clause_insert(where_clause_t *wc, expr_t *p, u_int16_t wt_flags){
  where_term_t *term;
  int idx;
  testcase( wt_flags & TERM_VIRTUAL );
  if( wc->n_term>=wc->n_slot ){
    where_term_t *old = wc->a;
    DBSQL *db = wc->w_info->parser->db;
    wc->a = __where_malloc(wc->w_info, sizeof(wc->a[0])*wc->n_slot*2 );
    if( wc->a==0 ){
      if( wt_flags & TERM_DYNAMIC ){
        __expr_delete(db, p);
      }
      wc->a = old;
      return 0;
    }
    memcpy(wc->a, old, sizeof(wc->a[0])*wc->n_term);
    wc->n_slot = wc->n_slot*2;
  }
  term = &wc->a[idx = wc->n_term++];
  if( (wt_flags & TERM_VIRTUAL)==0 ) wc->n_base = wc->n_term;
  if( p && EXPR_HAS_PROPERTY(p, EP_Unlikely) ){
    term->truth_prob = __log_est(p->i_table) - 270;
  }else{
    term->truth_prob = 1;
  }
  term->expr = __expr_skip_collate_and_likely(p);
  term->wt_flags = wt_flags;
  term->wc = wc;
  term->i_parent = -1;
  memset(&term->e_operator, 0,
         sizeof(where_term_t) - offsetof(where_term_t,e_operator));
  return idx;
}

/*
** Return TRUE if the given operator is one of the operators that is
** allowed for an indexable WHERE clause term.  The allowed operators are
** "=", "<", ">", "<=", ">=", "IN", "IS", and "IS NULL"
*/
static int __allowed_op(int op){
  assert( TK_GT>TK_EQ && TK_GT<TK_GE );
  assert( TK_LT>TK_EQ && TK_LT<TK_GE );
  assert( TK_LE>TK_EQ && TK_LE<TK_GE );
  assert( TK_GE==TK_EQ+4 );
  assert( TK_IN<TK_EQ );
  assert( TK_IS<TK_EQ );
  assert( TK_ISNULL<TK_EQ );
  if( op>TK_GE ) return 0;
  if( op>=TK_EQ ) return 1;
  return op==TK_IN || op==TK_ISNULL || op==TK_IS;
}

/*
** Commute a comparison operator.  Expressions of the form "X op Y"
** are converted into "Y op X".
*/
static u_int16_t __expr_commute(parser_t *parser, expr_t *expr){
  if( expr->left->op==TK_VECTOR
   || expr->right->op==TK_VECTOR
   || __binary_compare_coll_seq(parser, expr->left, expr->right) !=
      __binary_compare_coll_seq(parser, expr->right, expr->left)
  ){
    expr->flags ^= EP_Commuted;
  }
  SWAP(expr_t*,expr->right,expr->left);
  if( expr->op>=TK_GT ){
    assert( TK_LT==TK_GT+2 );
    assert( TK_GE==TK_LE+2 );
    assert( TK_GT>TK_EQ );
    assert( TK_GT<TK_LE );
    assert( expr->op>=TK_GT && expr->op<=TK_GE );
    expr->op = ((expr->op-TK_GT)^2)+TK_GT;
  }
  return 0;
}

/*
** Translate from TK_xx operator to WO_xx bitmask.
*/
static u_int16_t __operator_mask(int op){
  u_int16_t c;
  assert( __allowed_op(op) );
  if( op>=TK_EQ ){
    assert( (WO_EQ<<(op-TK_EQ)) < 0x7fff );
    c = (u_int16_t)(WO_EQ<<(op-TK_EQ));
  }else if( op==TK_IN ){
    c = WO_IN;
  }else if( op==TK_ISNULL ){
    c = WO_ISNULL;
  }else{
    assert( op==TK_IS );
    c = WO_IS;
  }
  assert( op!=TK_ISNULL || c==WO_ISNULL );
  assert( op!=TK_IN || c==WO_IN );
  assert( op!=TK_EQ || c==WO_EQ );
  assert( op!=TK_LT || c==WO_LT );
  assert( op!=TK_LE || c==WO_LE );
  assert( op!=TK_GT || c==WO_GT );
  assert( op!=TK_GE || c==WO_GE );
  assert( op!=TK_IS || c==WO_IS );
  return c;
}


#ifndef DBSQL_OMIT_LIKE_OPTIMIZATION
/*
** Check to see if the given expression is a LIKE or GLOB operator that
** can be optimized using inequality constraints.  Return TRUE if it is
** so and false if not.
**
** In order for the operator to be optimizible, the RHS must be a string
** literal that does not begin with a wildcard.  The LHS must be a column
** that may only be NULL, a string, or a BLOB, never a number. (This means
** that virtual tables cannot participate in the LIKE optimization.)  The
** collating sequence for the column on the LHS must be appropriate for
** the operator.
*/
static int __is_like_or_glob(
  parser_t *parser,    /* Parsing and code generating context */
  expr_t *expr,      /* Test this expression */
  expr_t **pp_prefix,  /* Pointer to TK_STRING expression with pattern prefix */
  int *pis_complete, /* True if the only wildcard is % in the last character */
  int *pno_case      /* True if uppercase is equivalent to lowercase */
){
  const u_int8_t *z = 0;           /* String on RHS of LIKE operator */
  expr_t *right, *left;      /* Right and left size of LIKE operator */
  expr_list_t *list;           /* List of operands to the LIKE operator */
  u_int8_t c;                      /* One character in z[] */
  int cnt;                   /* Number of non-wildcard prefix characters */
  u_int8_t wc[4];                  /* Wildcard characters */
  DBSQL *db = parser->db;  /* Database connection */
  dbsql_value_t *val = 0;
  int op;                    /* Opcode of right */
  int rc;                    /* Result code to return */

  if( !__is_like_function(db, expr, pno_case, (char*)wc) ){
    return 0;
  }
#ifdef DBSQL_EBCDIC
  if( *pno_case ) return 0;
#endif
  assert( EXPR_USE_X_LIST(expr) );
  list = expr->x.list;
  left = list->a[1].expr;

  right = __expr_skip_collate(list->a[0].expr);
  op = right->op;
  if( op==TK_VARIABLE && (db->flags & DBSQL_EnableQPSG)==0 ){
    vdbe_t *reprepare = parser->reprepare;
    int i_col = right->i_column;
    val = __vdbe_get_bound_value(reprepare, i_col, DBSQL_AFF_BLOB);
    if( val && dbsql_value_type(val)==DBSQL_TEXT ){
      z = dbsql_value_text(val);
    }
    __vdbe_set_varmask(parser->vdbe, i_col);
    assert( right->op==TK_VARIABLE || right->op==TK_REGISTER );
  }else if( op==TK_STRING ){
    assert( !EXPR_HAS_PROPERTY(right, EP_IntValue) );
     z = (u_int8_t*)right->u.token;
  }
  if( z ){
    /* Count the number of prefix bytes prior to the first wildcard,
    ** U+fffd character, or malformed utf-8. If the underlying database
    ** has a UTF16LE encoding, then only consider ASCII characters.  Note that
    ** the encoding of z[] is UTF8 - we are dealing with only UTF8 here in this
    ** code, but the database engine itself might be processing content using a
    ** different encoding. */
    cnt = 0;
    while( (c=z[cnt])!=0 && c!=wc[0] && c!=wc[1] && c!=wc[2] ){
      cnt++;
      if( c==wc[3] && z[cnt]>0 && z[cnt]<0x80 ){
        cnt++;
      }else if( c>=0x80 ){
        const u_int8_t *z2 = z+cnt-1;
        if( c==0xff || __utf8_read(&z2)==0xfffd  /* bad utf-8 */
         || ENC(db)==DBSQL_UTF16LE 
        ){
          cnt--;
          break;
        }else{
          cnt = (int)(z2-z);
        }
      }
    }

    /* The optimization is possible only if (1) the pattern does not begin
    ** with a wildcard and if (2) the non-wildcard prefix does not end with
    ** an (illegal 0xff) character, or (3) the pattern does not consist of
    ** a single escape character. The second condition is necessary so
    ** that we can increment the prefix key to find an upper bound for the
    ** range search. The third is because the caller assumes that the pattern
    ** consists of at least one character after all escapes have been
    ** removed.  */
    if( (cnt>1 || (cnt>0 && z[0]!=wc[3])) && ALWAYS(255!=(u_int8_t)z[cnt-1]) ){
      expr_t *prefix;

      /* A "complete" match if the pattern ends with "*" or "%" */
      *pis_complete = c==wc[0] && z[cnt+1]==0 && ENC(db)!=DBSQL_UTF16LE;

      /* Get the pattern prefix.  Remove all escapes from the prefix. */
      prefix = __expr(db, TK_STRING, (char*)z);
      if( prefix ){
        int i_from, i_to;
        char *z_new;
        assert( !EXPR_HAS_PROPERTY(prefix, EP_IntValue) );
        z_new = prefix->u.token;
        z_new[cnt] = 0;
        for(i_from=i_to=0; i_from<cnt; i_from++){
          if( z_new[i_from]==wc[3] ) i_from++;
          z_new[i_to++] = z_new[i_from];
        }
        z_new[i_to] = 0;
        assert( i_to>0 );

        /* If the LHS is not an ordinary column with TEXT affinity, then the
        ** pattern prefix boundaries (both the start and end boundaries) must
        ** not look like a number.  Otherwise the pattern might be treated as
        ** a number, which will invalidate the LIKE optimization.
        **
        ** Getting this right has been a persistent source of bugs in the
        ** LIKE optimization.  See, for example:
        **    2018-09-10 https://sqlite.org/src/info/c94369cae9b561b1
        **    2019-05-02 https://sqlite.org/src/info/b043a54c3de54b28
        **    2019-06-10 https://sqlite.org/src/info/fd76310a5e843e07
        **    2019-06-14 https://sqlite.org/src/info/ce8717f0885af975
        **    2019-09-03 https://sqlite.org/src/info/0f0428096f17252a
        */
        if( left->op!=TK_COLUMN
         || __expr_affinity(left)!=DBSQL_AFF_TEXT
         || (ALWAYS( EXPR_USE_Y_TAB(left) )
             && ALWAYS(left->y.tab)
             && IS_VIRTUAL(left->y.tab))  /* Might be numeric */
        ){
          int is_num;
          double r_dummy;
          assert( z_new[i_to]==0 );
          is_num = __ato_f(z_new, &r_dummy);
          if( is_num<=0 ){
            if( i_to==1 && z_new[0]=='-' ){
              is_num = +1;
            }else{
              z_new[i_to-1]++;
              is_num = __ato_f(z_new, &r_dummy);
              z_new[i_to-1]--;
            }
          }
          if( is_num>0 ){
            __expr_delete(db, prefix);
            __value_free(val);
            return 0;
          }
        }
      }
      *pp_prefix = prefix;

      /* If the RHS pattern is a bound parameter, make arrangements to
      ** reprepare the statement when that parameter is rebound */
      if( op==TK_VARIABLE ){
        vdbe_t *v = parser->vdbe;
        __vdbe_set_varmask(v, right->i_column);
        assert( !EXPR_HAS_PROPERTY(right, EP_IntValue) );
        if( *pis_complete && right->u.token[1] ){
          /* If the rhs of the LIKE expression is a variable, and the current
          ** value of the variable means there is no need to invoke the LIKE
          ** function, then no OP_Variable will be added to the program.
          ** This causes problems for the dbsql_bind_parameter_name()
          ** API. To work around them, add a dummy OP_Variable here.
          */
          int r1 = __get_temp_reg(parser);
          __expr_code_target(parser, right, r1);
          __vdbe_change_p3(v, __vdbe_current_addr(v)-1, 0);
          __release_temp_reg(parser, r1);
        }
      }
    }else{
      z = 0;
    }
  }

  rc = (z!=0);
  __value_free(val);
  return rc;
}
#endif /* DBSQL_OMIT_LIKE_OPTIMIZATION */

/*
** If expr is one of "like", "glob", "match", or "regexp", then
** return the corresponding DBSQL_INDEX_CONSTRAINT_xxxx value.
** If not, return 0.
**
** expr is guaranteed to be a TK_FUNCTION.
*/
int __expr_is_like_operator(const expr_t *expr){
  static const struct {
    const char *z_op;
    unsigned char op;
  } ops[] = {
    { "match",  DBSQL_INDEX_CONSTRAINT_MATCH },
    { "glob",   DBSQL_INDEX_CONSTRAINT_GLOB },
    { "like",   DBSQL_INDEX_CONSTRAINT_LIKE },
    { "regexp", DBSQL_INDEX_CONSTRAINT_REGEXP }
  };
  int i;
  assert( expr->op==TK_FUNCTION );
  assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
  for(i=0; i<ARRAY_SIZE(ops); i++){
    if( __str_icmp(expr->u.token, ops[i].z_op)==0 ){
      return ops[i].op;
    }
  }
  return 0;
}


#ifndef DBSQL_OMIT_VIRTUALTABLE
/*
** Check to see if the expr expression is a form that needs to be passed
** to the best_index method of virtual tables.  Forms of interest include:
**
**          Expression                   Virtual table_t Operator
**          -----------------------      ---------------------------------
**      1.  column MATCH expr            DBSQL_INDEX_CONSTRAINT_MATCH
**      2.  column GLOB expr             DBSQL_INDEX_CONSTRAINT_GLOB
**      3.  column LIKE expr             DBSQL_INDEX_CONSTRAINT_LIKE
**      4.  column REGEXP expr           DBSQL_INDEX_CONSTRAINT_REGEXP
**      5.  column != expr               DBSQL_INDEX_CONSTRAINT_NE
**      6.  expr != column               DBSQL_INDEX_CONSTRAINT_NE
**      7.  column IS NOT expr           DBSQL_INDEX_CONSTRAINT_ISNOT
**      8.  expr IS NOT column           DBSQL_INDEX_CONSTRAINT_ISNOT
**      9.  column IS NOT NULL           DBSQL_INDEX_CONSTRAINT_ISNOTNULL
**
** In every case, "column" must be a column of a virtual table.  If there
** is a match, set *left to the "column" expression, set *right to the
** "expr" expression (even though in forms (6) and (8) the column is on the
** right and the expression is on the left).  Also set *pe_op2 to the
** appropriate virtual table operator.  The return value is 1 or 2 if there
** is a match.  The usual return is 1, but if the RHS is also a column
** of virtual table in forms (5) or (7) then return 2.
**
** If the expression matches none of the patterns above, return 0.
*/
static int __is_auxiliary_vtab_operator(
  DBSQL *db,                    /* Parsing context */
  expr_t *expr,                    /* Test this expression */
  unsigned char *pe_op2,           /* OUT: 0 for MATCH, or else an op2 value */
  expr_t **pp_left,                  /* column_t expression to left of MATCH/op2 */
  expr_t **pp_right                  /* Expression to left of MATCH/op2 */
){
  if( expr->op==TK_FUNCTION ){
    expr_list_t *list;
    expr_t *col;                     /* column_t reference */
    int i;

    assert( EXPR_USE_X_LIST(expr) );
    list = expr->x.list;
    if( list==0 || list->n_expr!=2 ){
      return 0;
    }

    /* Built-in operators MATCH, GLOB, LIKE, and REGEXP attach to a
    ** virtual table on their second argument, which is the same as
    ** the left-hand side operand in their in-fix form.
    **
    **       vtab_column MATCH expression
    **       MATCH(expression,vtab_column)
    */
    col = list->a[1].expr;
    assert( col->op!=TK_COLUMN || (EXPR_USE_Y_TAB(col) && col->y.tab!=0) );
    if( EXPR_IS_VTAB(col) && (i = __expr_is_like_operator(expr))!=0 ){
      *pe_op2 = i;
      *pp_right = list->a[0].expr;
      *pp_left = col;
      return 1;
    }

    /* We can also match against the first column of overloaded
    ** functions where find_function returns a value of at least
    ** DBSQL_INDEX_CONSTRAINT_FUNCTION.
    **
    **      OVERLOADED(vtab_column,expression)
    **
    ** Historically, find_function expected to see lower-case function
    ** names.  But for this use case, find_function is expected to deal
    ** with function names in an arbitrary case.
    */
    col = list->a[0].expr;
    assert( col->op!=TK_COLUMN || EXPR_USE_Y_TAB(col) );
    assert( col->op!=TK_COLUMN || (EXPR_USE_Y_TAB(col) && col->y.tab!=0) );
    if( EXPR_IS_VTAB(col) ){
      dbsql_vtab_t *vtab;
      dbsql_module_t *mod;
      void (*x_not_used)(dbsql_func_t*,int,dbsql_value_t**);
      void *not_used;
      vtab = __get_v_table(db, col->y.tab)->vtab;
      assert( vtab!=0 );
      assert( vtab->module!=0 );
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      mod = (dbsql_module_t *)vtab->module;
      if( mod->find_function!=0 ){
        i = mod->find_function(vtab,2, expr->u.token, &x_not_used, &not_used);
        if( i>=DBSQL_INDEX_CONSTRAINT_FUNCTION ){
          *pe_op2 = i;
          *pp_right = list->a[1].expr;
          *pp_left = col;
          return 1;
        }
      }
    }
  }else if( expr->op>=TK_EQ ){
    /* Comparison operators are a common case.  Save a few comparisons for
    ** that common case by terminating early. */
    assert( TK_NE < TK_EQ );
    assert( TK_ISNOT < TK_EQ );
    assert( TK_NOTNULL < TK_EQ );
    return 0;
  }else if( expr->op==TK_NE || expr->op==TK_ISNOT || expr->op==TK_NOTNULL ){
    int res = 0;
    expr_t *left = expr->left;
    expr_t *right = expr->right;
    assert( left->op!=TK_COLUMN || (EXPR_USE_Y_TAB(left) && left->y.tab!=0) );
    if( EXPR_IS_VTAB(left) ){
      res++;
    }
    assert( right==0 || right->op!=TK_COLUMN
            || (EXPR_USE_Y_TAB(right) && right->y.tab!=0) );
    if( right && EXPR_IS_VTAB(right) ){
      res++;
      SWAP(expr_t*, left, right);
    }
    *pp_left = left;
    *pp_right = right;
    if( expr->op==TK_NE ) *pe_op2 = DBSQL_INDEX_CONSTRAINT_NE;
    if( expr->op==TK_ISNOT ) *pe_op2 = DBSQL_INDEX_CONSTRAINT_ISNOT;
    if( expr->op==TK_NOTNULL ) *pe_op2 = DBSQL_INDEX_CONSTRAINT_ISNOTNULL;
    return res;
  }
  return 0;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

/*
** If the base expression originated in the ON or USING clause of
** a join, then transfer the appropriate markings over to derived.
*/
static void __transfer_join_markings(expr_t *derived, expr_t *base){
  if( derived && EXPR_HAS_PROPERTY(base, EP_OuterON|EP_InnerON) ){
    derived->flags |= base->flags & (EP_OuterON|EP_InnerON);
    derived->w.i_join = base->w.i_join;
  }
}

/*
** Mark term i_child as being a child of term i_parent
*/
static void __mark_term_as_child(where_clause_t *wc, int i_child, int i_parent){
  wc->a[i_child].i_parent = i_parent;
  wc->a[i_child].truth_prob = wc->a[i_parent].truth_prob;
  assert(   wc->a[i_parent].n_child <  UMXV(wc->a[0].n_child) );
  wc->a[i_parent].n_child++;
  testcase( wc->a[i_parent].n_child == UMXV(wc->a[0].n_child) );

}

/*
** Return the N-th AND-connected subterm of term.  Or if term is not
** a conjunction, then return just term when N==0.  If N is exceeds
** the number of available subterms, return NULL.
*/
static where_term_t *__where_nth_subterm(where_term_t *term, int N){
  if( term->e_operator!=WO_AND ){
    return N==0 ? term : 0;
  }
  if( N<term->u.and_info->wc.n_term ){
    return &term->u.and_info->wc.a[N];
  }
  return 0;
}

/*
** Subterms one and two are contained within WHERE clause wc.  The
** two subterms are in disjunction - they are OR-ed together.
**
** If these two terms are both of the form:  "A op B" with the same
** A and B values but different operators and if the operators are
** compatible (if one is = and the other is <, for example) then
** add a new virtual AND term to wc that is the combination of the
** two.
**
** Some examples:
**
**    x<y OR x=y    -->     x<=y
**    x=y OR x=y    -->     x=y
**    x<=y OR x<y   -->     x<=y
**
** The following is NOT generated:
**
**    x<y OR x>y    -->     x!=y    
*/
static void __where_combine_disjuncts(
  src_list_t *src,         /* the FROM clause */
  where_clause_t *wc,      /* The complete WHERE clause */
  where_term_t *one,       /* First disjunct */
  where_term_t *two        /* Second disjunct */
){
  u_int16_t e_op = one->e_operator | two->e_operator;
  DBSQL *db;           /* Database connection (for malloc) */
  expr_t *p_new;            /* New virtual expression */
  int op;                /* Operator for the combined expression */
  int idx_new;            /* index_t in wc of the next virtual term */
  expr_t *a, *b;         /* Expressions associated with one and two */

  if( (one->wt_flags | two->wt_flags) & TERM_VNULL ) return;
  if( (one->e_operator & (WO_EQ|WO_LT|WO_LE|WO_GT|WO_GE))==0 ) return;
  if( (two->e_operator & (WO_EQ|WO_LT|WO_LE|WO_GT|WO_GE))==0 ) return;
  if( (e_op & (WO_EQ|WO_LT|WO_LE))!=e_op
   && (e_op & (WO_EQ|WO_GT|WO_GE))!=e_op ) return;
  a = one->expr;
  b = two->expr;
  assert( a->left!=0 && a->right!=0 );
  assert( b->left!=0 && b->right!=0 );
  if( __expr_compare(0,a->left, b->left, -1) )  return;
  if( __expr_compare(0,a->right, b->right,-1) ) return;
  if( EXPR_HAS_PROPERTY(a,EP_Commuted)!=EXPR_HAS_PROPERTY(b,EP_Commuted) ){
    return;
  }
  /* If we reach this point, it means the two subterms can be combined */
  if( (e_op & (e_op-1))!=0 ){
    if( e_op & (WO_LT|WO_LE) ){
      e_op = WO_LE;
    }else{
      assert( e_op & (WO_GT|WO_GE) );
      e_op = WO_GE;
    }
  }
  db = wc->w_info->parser->db;
  p_new = __expr_dup(db, a, 0);
  if( p_new==0 ) return;
  for(op=TK_EQ; e_op!=(WO_EQ<<(op-TK_EQ)); op++){ assert( op<TK_GE ); }
  p_new->op = op;
  idx_new = __where_clause_insert(wc, p_new, TERM_VIRTUAL|TERM_DYNAMIC);
  __expr_analyze(src, wc, idx_new);
}

#if !defined(DBSQL_OMIT_OR_OPTIMIZATION) && !defined(DBSQL_OMIT_SUBQUERY)
/*
** Analyze a term that consists of two or more OR-connected
** subterms.  So in:
**
**     ... WHERE  (a=5) AND (b=7 OR c=9 OR d=13) AND (d=13)
**                          ^^^^^^^^^^^^^^^^^^^^
**
** This routine analyzes terms such as the middle term in the above example.
** A __where_or_term object is computed and attached to the term under
** analysis, regardless of the outcome of the analysis.  Hence:
**
**     where_term_t.wt_flags   |=  TERM_ORINFO
**     where_term_t.u.or_info  =  a dynamically allocated __where_or_term object
**
** The term being analyzed must have two or more of OR-connected subterms.
** A single subterm might be a set of AND-connected sub-subterms.
** Examples of terms under analysis:
**
**     (A)     t1.x=t2.y OR t1.x=t2.z OR t1.y=15 OR t1.z=t3.a+5
**     (B)     x=expr1 OR expr2=x OR x=expr3
**     (C)     t1.x=t2.y OR (t1.x=t2.z AND t1.y=15)
**     (D)     x=expr1 OR (y>11 AND y<22 AND z LIKE '*hello*')
**     (E)     (p.a=1 AND q.b=2 AND r.c=3) OR (p.x=4 AND q.y=5 AND r.z=6)
**     (F)     x>A OR (x=A AND y>=B)
**
** CASE 1:
**
** If all subterms are of the form T.C=expr for some single column of C and
** a single table T (as shown in example B above) then create a new virtual
** term that is an equivalent IN expression.  In other words, if the term
** being analyzed is:
**
**      x = expr1  OR  expr2 = x  OR  x = expr3
**
** then create a new virtual term like this:
**
**      x IN (expr1,expr2,expr3)
**
** CASE 2:
**
** If there are exactly two disjuncts and one side has x>A and the other side
** has x=A (for the same x and A) then add a new virtual conjunct term to the
** WHERE clause of the form "x>=A".  Example:
**
**      x>A OR (x=A AND y>B)    adds:    x>=A
**
** The added conjunct can sometimes be helpful in query planning.
**
** CASE 3:
**
** If all subterms are indexable by a single table T, then set
**
**     where_term_t.e_operator              =  WO_OR
**     where_term_t.u.or_info->indexable  |=  the cursor number for table T
**
** A subterm is "indexable" if it is of the form
** "T.C <op> <expr>" where C is any column of table T and
** <op> is one of "=", "<", "<=", ">", ">=", "IS NULL", or "IN".
** A subterm is also indexable if it is an AND of two or more
** subsubterms at least one of which is indexable.  Indexable AND
** subterms have their e_operator set to WO_AND and they have
** u.and_info set to a dynamically allocated __where_and_term object.
**
** From another point of view, "indexable" means that the subterm could
** potentially be used with an index if an appropriate index exists.
** This analysis does not consider whether or not the index exists; that
** is decided elsewhere.  This analysis only looks at whether subterms
** appropriate for indexing exist.
**
** All examples A through E above satisfy case 3.  But if a term
** also satisfies case 1 (such as B) we know that the optimizer will
** always prefer case 1, so in that case we pretend that case 3 is not
** satisfied.
**
** It might be the case that multiple tables are indexable.  For example,
** (E) above is indexable on tables P, Q, and R.
**
** Terms that satisfy case 3 are candidates for lookup by using
** separate indices to find rowids for each subterm and composing
** the union of all rowids using a row_set_t object.  This is similar
** to "bitmap indices" in other database engines.
**
** OTHERWISE:
**
** If none of cases 1, 2, or 3 apply, then leave the e_operator set to
** zero.  This term is not useful for search.
*/
static void __expr_analyze_or_term(
  src_list_t *src,            /* the FROM clause */
  where_clause_t *wc,         /* the complete WHERE clause */
  int idx_term               /* index_t of the OR-term to be analyzed */
){
  where_info_t *w_info = wc->w_info;        /* WHERE clause processing context */
  parser_t *parser = w_info->parser;         /* Parser context */
  DBSQL *db = parser->db;               /* Database connection */
  where_term_t *term = &wc->a[idx_term];    /* The term to be analyzed */
  expr_t *expr = term->expr;             /* The expression of the term */
  int i;                                  /* Loop counters */
  where_clause_t *or_wc;       /* Breakup of term into subterms */
  where_term_t *or_term;       /* A Sub-term within the or_wc */
  where_or_info_t *or_info;     /* Additional information associated with term */
  Bitmask chng_to_in;         /* Tables that might satisfy case 1 */
  Bitmask indexable;        /* Tables that are indexable, satisfying case 2 */

  /*
  ** Break the OR clause into its separate subterms.  The subterms are
  ** stored in a where_clause_t structure containing within the where_or_info_t
  ** object that is attached to the original OR clause term.
  */
  assert( (term->wt_flags & (TERM_DYNAMIC|TERM_ORINFO|TERM_ANDINFO))==0 );
  assert( expr->op==TK_OR );
  term->u.or_info = or_info = __db_malloc_zero(db, sizeof(*or_info));
  if( or_info==0 ) return;
  term->wt_flags |= TERM_ORINFO;
  or_wc = &or_info->wc;
  memset(or_wc->statics, 0, sizeof(or_wc->statics));
  __where_clause_init(or_wc, w_info);
  __where_split(or_wc, expr, TK_OR);
  __where_expr_analyze(src, or_wc);
  if( db->malloc_failed ) return;
  assert( or_wc->n_term>=2 );

  /*
  ** Compute the set of tables that might satisfy cases 1 or 3.
  */
  indexable = ~(Bitmask)0;
  chng_to_in = ~(Bitmask)0;
  for(i=or_wc->n_term-1, or_term=or_wc->a; i>=0 && indexable; i--, or_term++){
    if( (or_term->e_operator & WO_SINGLE)==0 ){
      where_and_info_t *and_info;
      assert( (or_term->wt_flags & (TERM_ANDINFO|TERM_ORINFO))==0 );
      chng_to_in = 0;
      and_info = __db_malloc_raw_nn(db, sizeof(*and_info));
      if( and_info ){
        where_clause_t *and_wc;
        where_term_t *and_term;
        int j;
        Bitmask b = 0;
        or_term->u.and_info = and_info;
        or_term->wt_flags |= TERM_ANDINFO;
        or_term->e_operator = WO_AND;
        or_term->left_cursor = -1;
        and_wc = &and_info->wc;
        memset(and_wc->statics, 0, sizeof(and_wc->statics));
        __where_clause_init(and_wc, wc->w_info);
        __where_split(and_wc, or_term->expr, TK_AND);
        __where_expr_analyze(src, and_wc);
        and_wc->outer = wc;
        if( !db->malloc_failed ){
          for(j=0, and_term=and_wc->a; j<and_wc->n_term; j++, and_term++){
            assert( and_term->expr );
            if( __allowed_op(and_term->expr->op)
             || and_term->e_operator==WO_AUX
            ){
              b |= __where_get_mask(&w_info->s_mask_set, and_term->left_cursor);
            }
          }
        }
        indexable &= b;
      }
    }else if( or_term->wt_flags & TERM_COPIED ){
      /* Skip this term for now.  We revisit it when we process the
      ** corresponding TERM_VIRTUAL term */
    }else{
      Bitmask b;
      b = __where_get_mask(&w_info->s_mask_set, or_term->left_cursor);
      if( or_term->wt_flags & TERM_VIRTUAL ){
        where_term_t *other = &or_wc->a[or_term->i_parent];
        b |= __where_get_mask(&w_info->s_mask_set, other->left_cursor);
      }
      indexable &= b;
      if( (or_term->e_operator & WO_EQ)==0 ){
        chng_to_in = 0;
      }else{
        chng_to_in &= b;
      }
    }
  }

  /*
  ** Record the set of tables that satisfy case 3.  The set might be
  ** empty.
  */
  or_info->indexable = indexable;
  term->e_operator = WO_OR;
  term->left_cursor = -1;
  if( indexable ){
    wc->has_or = 1;
  }

  /* For a two-way OR, attempt to implementation case 2.
  */
  if( indexable && or_wc->n_term==2 ){
    int i_one = 0;
    where_term_t *one;
    while( (one = __where_nth_subterm(&or_wc->a[0],i_one++))!=0 ){
      int i_two = 0;
      where_term_t *two;
      while( (two = __where_nth_subterm(&or_wc->a[1],i_two++))!=0 ){
        __where_combine_disjuncts(src, wc, one, two);
      }
    }
  }

  /*
  ** chng_to_in holds a set of tables that *might* satisfy case 1.  But
  ** we have to do some additional checking to see if case 1 really
  ** is satisfied.
  **
  ** chng_to_in will hold either 0, 1, or 2 bits.  The 0-bit case means
  ** that there is no possibility of transforming the OR clause into an
  ** IN operator because one or more terms in the OR clause contain
  ** something other than == on a column in the single table.  The 1-bit
  ** case means that every term of the OR clause is of the form
  ** "table.column=expr" for some single table.  The one bit that is set
  ** will correspond to the common table.  We still need to check to make
  ** sure the same column is used on all terms.  The 2-bit case is when
  ** the all terms are of the form "table1.column=table2.column".  It
  ** might be possible to form an IN operator with either table1.column
  ** or table2.column as the LHS if either is common to every term of
  ** the OR clause.
  **
  ** Note that terms of the form "table.column1=table.column2" (the
  ** same table on both sizes of the ==) cannot be optimized.
  */
  if( chng_to_in ){
    int ok_to_chng_to_in = 0;     /* True if the conversion to IN is valid */
    int i_column = -1;         /* column_t index on lhs of IN operator */
    int i_cursor = -1;         /* table_t cursor common to all terms */
    int j = 0;                /* Loop counter */

    /* Search for a table and column that appears on one side or the
    ** other of the == operator in every subterm.  That table and column
    ** will be recorded in i_cursor and i_column.  There might not be any
    ** such table and column.  Set ok_to_chng_to_in if an appropriate table
    ** and column is found but leave ok_to_chng_to_in false if not found.
    */
    for(j=0; j<2 && !ok_to_chng_to_in; j++){
      expr_t *left = 0;
      or_term = or_wc->a;
      for(i=or_wc->n_term-1; i>=0; i--, or_term++){
        assert( or_term->e_operator & WO_EQ );
        or_term->wt_flags &= ~TERM_OK;
        if( or_term->left_cursor==i_cursor ){
          /* This is the 2-bit case and we are on the second iteration and
          ** current term is from the first iteration.  So skip this term. */
          assert( j==1 );
          continue;
        }
        if( (chng_to_in & __where_get_mask(&w_info->s_mask_set,
                                            or_term->left_cursor))==0 ){
          /* This term must be of the form t1.a==t2.b where t2 is in the
          ** chng_to_in set but t1 is not.  This term will be either preceded
          ** or followed by an inverted copy (t2.b==t1.a).  Skip this term
          ** and use its inversion. */
          testcase( or_term->wt_flags & TERM_COPIED );
          testcase( or_term->wt_flags & TERM_VIRTUAL );
          assert( or_term->wt_flags & (TERM_COPIED|TERM_VIRTUAL) );
          continue;
        }
        assert( (or_term->e_operator & (WO_OR|WO_AND))==0 );
        i_column = or_term->u.x.left_column;
        i_cursor = or_term->left_cursor;
        left = or_term->expr->left;
        break;
      }
      if( i<0 ){
        /* No candidate table+column was found.  This can only occur
        ** on the second iteration */
        assert( j==1 );
        assert( IS_POWER_OF_TWO(chng_to_in) );
        assert( chng_to_in==__where_get_mask(&w_info->s_mask_set, i_cursor) );
        break;
      }
      testcase( j==1 );

      /* We have found a candidate table and column.  Check to see if that
      ** table and column is common to every term in the OR clause */
      ok_to_chng_to_in = 1;
      for(; i>=0 && ok_to_chng_to_in; i--, or_term++){
        assert( or_term->e_operator & WO_EQ );
        assert( (or_term->e_operator & (WO_OR|WO_AND))==0 );
        if( or_term->left_cursor!=i_cursor ){
          or_term->wt_flags &= ~TERM_OK;
        }else if( or_term->u.x.left_column!=i_column || (i_column==XN_EXPR
               && __expr_compare(parser, or_term->expr->left, left, -1)
        )){
          ok_to_chng_to_in = 0;
        }else{
          int aff_left, aff_right;
          /* If the right-hand side is also a column, then the affinities
          ** of both right and left sides must be such that no type
          ** conversions are required on the right.  (Ticket #2249)
          */
          aff_right = __expr_affinity(or_term->expr->right);
          aff_left = __expr_affinity(or_term->expr->left);
          if( aff_right!=0 && aff_right!=aff_left ){
            ok_to_chng_to_in = 0;
          }else{
            or_term->wt_flags |= TERM_OK;
          }
        }
      }
    }

    /* At this point, ok_to_chng_to_in is true if original term satisfies
    ** case 1.  In that case, construct a new virtual term that is
    ** term converted into an IN operator.
    */
    if( ok_to_chng_to_in ){
      expr_t *dup;            /* A transient duplicate expression */
      expr_list_t *list = 0;   /* The RHS of the IN operator */
      expr_t *left = 0;       /* The LHS of the IN operator */
      expr_t *p_new;            /* The complete IN operator */

      for(i=or_wc->n_term-1, or_term=or_wc->a; i>=0; i--, or_term++){
        if( (or_term->wt_flags & TERM_OK)==0 ) continue;
        assert( or_term->e_operator & WO_EQ );
        assert( (or_term->e_operator & (WO_OR|WO_AND))==0 );
        assert( or_term->left_cursor==i_cursor );
        assert( or_term->u.x.left_column==i_column );
        dup = __expr_dup(db, or_term->expr->right, 0);
        list = __expr_list_append(w_info->parser, list, dup);
        left = or_term->expr->left;
      }
      assert( left!=0 );
      dup = __expr_dup(db, left, 0);
      p_new = __p_expr(parser, TK_IN, dup, 0);
      if( p_new ){
        int idx_new;
        __transfer_join_markings(p_new, expr);
        assert( EXPR_USE_X_LIST(p_new) );
        p_new->x.list = list;
        idx_new = __where_clause_insert(wc, p_new, TERM_VIRTUAL|TERM_DYNAMIC);
        testcase( idx_new==0 );
        __expr_analyze(src, wc, idx_new);
        /* term = &wc->a[idx_term]; // would be needed if term where reused */
        __mark_term_as_child(wc, idx_new, idx_term);
      }else{
        __expr_list_delete(db, list);
      }
    }
  }
}
#endif /* !DBSQL_OMIT_OR_OPTIMIZATION && !DBSQL_OMIT_SUBQUERY */

/*
** We already know that expr is a binary operator where both operands are
** column references.  This routine checks to see if expr is an equivalence
** relation:
**   1.  The DBSQL_Transitive optimization must be enabled
**   2.  Must be either an == or an IS operator
**   3.  Not originating in the ON clause of an OUTER JOIN
**   4.  The operator is not IS or else the query does not contain RIGHT JOIN
**   5.  The affinities of A and B must be compatible
**   6.  Both operands use the same collating sequence, and they must not
**       use explicit COLLATE clauses.
** If this routine returns TRUE, that means that the RHS can be substituted
** for the LHS anyplace else in the WHERE clause where the LHS column occurs.
** This is an optimization.  No harm comes from returning 0.  But if 1 is
** returned when it should not be, then incorrect answers might result.
*/
static int __term_is_equivalence(parser_t *parser, expr_t *expr, src_list_t *src){
  char aff1, aff2;
  if( !OPTIMIZATION_ENABLED(parser->db, DBSQL_Transitive) ) return 0;  /* (1) */
  if( expr->op!=TK_EQ && expr->op!=TK_IS ) return 0;                 /* (2) */
  if( EXPR_HAS_PROPERTY(expr, EP_OuterON|EP_Collate) ) return 0;        /* (3) */
  assert( src!=0 );
  if( expr->op==TK_IS
   && src->n_src>=2
   && (src->a[0].fg.jointype & JT_LTORJ)!=0
  ){
    return 0;                                                          /* (4) */
  }
  aff1 = __expr_affinity(expr->left);
  aff2 = __expr_affinity(expr->right);
  if( aff1!=aff2
   && (!__is_numeric_affinity(aff1) || !__is_numeric_affinity(aff2))
  ){
    return 0;                                                          /* (5) */
  }
  if( !__expr_coll_seq_match(parser, expr->left, expr->right) ){
    return 0;                                                          /* (6) */
  }
  return 1;
}

/*
** Recursively walk the expressions of a SELECT statement and generate
** a bitmask indicating which tables are used in that expression
** tree.
*/
static Bitmask __expr_select_usage(where_mask_set_t *mask_set, select_t *s){
  Bitmask mask = 0;
  while( s ){
    src_list_t *src = s->src;
    mask |= __where_expr_list_usage(mask_set, s->e_list);
    mask |= __where_expr_list_usage(mask_set, s->group_by);
    mask |= __where_expr_list_usage(mask_set, s->order_by);
    mask |= __where_expr_usage(mask_set, s->where);
    mask |= __where_expr_usage(mask_set, s->having);
    if( ALWAYS(src!=0) ){
      int i;
      for(i=0; i<src->n_src; i++){
        if( src->a[i].fg.is_subquery ){
          mask |= __expr_select_usage(mask_set, src->a[i].u4.subq->p_select);
        }
        if( src->a[i].fg.is_using==0 ){
          mask |= __where_expr_usage(mask_set, src->a[i].u3.on);
        }
        if( src->a[i].fg.is_tab_func ){
          mask |= __where_expr_list_usage(mask_set, src->a[i].u1.func_arg);
        }
      }
    }
    s = s->prior;
  }
  return mask;
}

/*
** Expression expr is one operand of a comparison operator that might
** be useful for indexing.  This routine checks to see if expr appears
** in any index.  Return TRUE (1) if expr is an indexed term and return
** FALSE (0) if not.  If TRUE is returned, also set cur_cols[0] to the cursor
** number of the table that is indexed and cur_cols[1] to the column number
** of the column that is indexed, or XN_EXPR (-2) if an expression is being
** indexed.
**
** If expr is a TK_COLUMN column reference, then this routine always returns
** true even if that particular column is not indexed, because the column
** might be added to an automatic index later.
*/
static DBSQL_NOINLINE int __expr_might_be_indexed2(
  src_list_t *from,        /* The FROM clause */
  int *cur_cols,         /* Write the referenced table cursor and column here */
  expr_t *expr,           /* An operand of a comparison operator */
  int j                  /* Start looking with the j-th from entry */
){
  index_t *idx;
  int i;
  int i_cur;
  do{
    i_cur = from->a[j].i_cursor;
    for(idx=from->a[j].s_tab->p_index; idx; idx=idx->next){
      if( idx->col_exprs==0 ) continue;
      for(i=0; i<idx->n_key_col; i++){
        if( idx->columns[i]!=XN_EXPR ) continue;
        assert( idx->has_expr );
        if( __expr_compare_skip(expr,idx->col_exprs->a[i].expr,i_cur)==0
         && !__expr_is_constant(0,idx->col_exprs->a[i].expr)
        ){
          cur_cols[0] = i_cur;
          cur_cols[1] = XN_EXPR;
          return 1;
        }
      }
    }
  }while( ++j < from->n_src );
  return 0;
}
static int __expr_might_be_indexed(
  src_list_t *from,        /* The FROM clause */
  int *cur_cols,         /* Write the referenced table cursor & column here */
  expr_t *expr,           /* An operand of a comparison operator */
  int op                 /* The specific comparison operator */
){
  int i;

  /* If this expression is a vector to the left or right of a
  ** inequality constraint (>, <, >= or <=), perform the processing
  ** on the first element of the vector.  */
  assert( TK_GT+1==TK_LE && TK_GT+2==TK_LT && TK_GT+3==TK_GE );
  assert( TK_IS<TK_GE && TK_ISNULL<TK_GE && TK_IN<TK_GE );
  assert( op<=TK_GE );
  if( expr->op==TK_VECTOR && (op>=TK_GT && ALWAYS(op<=TK_GE)) ){
    assert( EXPR_USE_X_LIST(expr) );
    expr = expr->x.list->a[0].expr;
  }

  if( expr->op==TK_COLUMN ){
    cur_cols[0] = expr->i_table;
    cur_cols[1] = expr->i_column;
    return 1;
  }

  for(i=0; i<from->n_src; i++){
    index_t *idx;
    for(idx=from->a[i].s_tab->p_index; idx; idx=idx->next){
      if( idx->col_exprs ){
        return __expr_might_be_indexed2(from,cur_cols,expr,i);
      }
    }
  }
  return 0;
}


/*
** The input to this routine is an where_term_t structure with only the
** "expr" field filled in.  The job of this routine is to analyze the
** subexpression and populate all the other fields of the where_term_t
** structure.
**
** If the expression is of the form "<expr> <op> X" it gets commuted
** to the standard form of "X <op> <expr>".
**
** If the expression is of the form "X <op> Y" where both X and Y are
** columns, then the original expression is unchanged and a new virtual
** term of the form "Y <op> X" is added to the WHERE clause and
** analyzed separately.  The original term is marked with TERM_COPIED
** and the new term is marked with TERM_DYNAMIC (because it's expr
** needs to be freed with the where_clause_t) and TERM_VIRTUAL (because it
** is a commuted copy of a prior term.)  The original term has n_child=1
** and the copy has idx_parent set to the index of the original term.
*/
static void __expr_analyze(
  src_list_t *src,            /* the FROM clause */
  where_clause_t *wc,         /* the WHERE clause */
  int idx_term               /* index_t of the term to be analyzed */
){
  where_info_t *w_info = wc->w_info; /* WHERE clause processing context */
  where_term_t *term;                /* The term to be analyzed */
  where_mask_set_t *mask_set;          /* Set of table index masks */
  expr_t *expr;                     /* The expression to be analyzed */
  Bitmask prereq_left;              /* Prerequisites of the expr->left */
  Bitmask prereq_all;               /* Prerequisites of expr */
  Bitmask extra_right = 0;          /* Extra dependencies on LEFT JOIN */
  expr_t *str1 = 0;                 /* RHS of LIKE/GLOB operator */
  int is_complete = 0;              /* RHS of LIKE/GLOB ends with wildcard */
  int no_case = 0;                  /* uppercase equivalent to lowercase */
  int op;                          /* Top-level operator.  expr->op */
  parser_t *parser = w_info->parser;  /* Parsing context */
  DBSQL *db = parser->db;        /* Database connection */
  unsigned char op2 = 0;          /* op2 value for LIKE/REGEXP/GLOB */
  int n_left;                       /* Number of elements on left side vector */

  if( db->malloc_failed ){
    return;
  }
  assert( wc->n_term > idx_term );
  term = &wc->a[idx_term];
#ifdef DBSQL_DEBUG
  term->i_term = idx_term;
#endif
  mask_set = &w_info->s_mask_set;
  expr = term->expr;
  assert( expr!=0 ); /* Because malloc() has not failed */
  assert( expr->op!=TK_AS && expr->op!=TK_COLLATE );
  mask_set->var_select = 0;
  prereq_left = __where_expr_usage(mask_set, expr->left);
  op = expr->op;
  if( op==TK_IN ){
    assert( expr->right==0 );
    if( __expr_check_in(parser, expr) ) return;
    if( EXPR_USE_X_SELECT(expr) ){
      term->prereq_right = __expr_select_usage(mask_set, expr->x.p_select);
    }else{
      term->prereq_right = __where_expr_list_usage(mask_set, expr->x.list);
    }
    prereq_all = prereq_left | term->prereq_right;
  }else{
    term->prereq_right = __where_expr_usage(mask_set, expr->right);
    if( expr->left==0
     || EXPR_HAS_PROPERTY(expr, EP_xIsSelect|EP_IfNullRow)
     || expr->x.list!=0
    ){
      prereq_all = __where_expr_usage_nn(mask_set, expr);
    }else{
      prereq_all = prereq_left | term->prereq_right;
    }
  }
  if( mask_set->var_select ) term->wt_flags |= TERM_VARSELECT;

#ifdef DBSQL_DEBUG
  if( prereq_all!=__where_expr_usage_nn(mask_set, expr) ){
    printf("\n*** Incorrect prereqAll computed for:\n");
    __tree_view_expr(0,expr,0);
    assert( 0 );
  }
#endif

  if( EXPR_HAS_PROPERTY(expr, EP_OuterON|EP_InnerON) ){
    Bitmask x = __where_get_mask(mask_set, expr->w.i_join);
    if( EXPR_HAS_PROPERTY(expr, EP_OuterON) ){
      prereq_all |= x;
      extra_right = x-1;  /* ON clause terms may not be used with an index
                         ** on left table of a LEFT JOIN.  Ticket #3015 */
    }else if( (prereq_all>>1)>=x ){
      EXPR_CLEAR_PROPERTY(expr, EP_InnerON);
    }
  }
  term->prereq_all = prereq_all;
  term->left_cursor = -1;
  term->i_parent = -1;
  term->e_operator = 0;
  if( __allowed_op(op) ){
    int cur_cols[2];
    expr_t *left = __expr_skip_collate(expr->left);
    expr_t *right = __expr_skip_collate(expr->right);
    u_int16_t op_mask = (term->prereq_right & prereq_left)==0 ? WO_ALL : WO_EQUIV;

    if( term->u.x.i_field>0 ){
      assert( op==TK_IN );
      assert( left->op==TK_VECTOR );
      assert( EXPR_USE_X_LIST(left) );
      left = left->x.list->a[term->u.x.i_field-1].expr;
    }

    if( __expr_might_be_indexed(src, cur_cols, left, op) ){
      term->left_cursor = cur_cols[0];
      assert( (term->e_operator & (WO_OR|WO_AND))==0 );
      term->u.x.left_column = cur_cols[1];
      term->e_operator = __operator_mask(op) & op_mask;
    }
    if( op==TK_IS ) term->wt_flags |= TERM_IS;
    if( right
     && __expr_might_be_indexed(src, cur_cols, right, op)
     && !EXPR_HAS_PROPERTY(right, EP_FixedCol)
    ){
      where_term_t *p_new;
      expr_t *dup;
      u_int16_t extra_op = 0;        /* Extra bits for p_new->e_operator */
      assert( term->u.x.i_field==0 );
      if( term->left_cursor>=0 ){
        int idx_new;
        dup = __expr_dup(db, expr, 0);
        if( db->malloc_failed ){
          __expr_delete(db, dup);
          return;
        }
        idx_new = __where_clause_insert(wc, dup, TERM_VIRTUAL|TERM_DYNAMIC);
        if( idx_new==0 ) return;
        p_new = &wc->a[idx_new];
        __mark_term_as_child(wc, idx_new, idx_term);
        if( op==TK_IS ) p_new->wt_flags |= TERM_IS;
        term = &wc->a[idx_term];
        term->wt_flags |= TERM_COPIED;
        assert( w_info->tab_list!=0 );
        if( __term_is_equivalence(parser, dup, w_info->tab_list) ){
          term->e_operator |= WO_EQUIV;
          extra_op = WO_EQUIV;
        }
      }else{
        dup = expr;
        p_new = term;
      }
      p_new->wt_flags |= __expr_commute(parser, dup);
      p_new->left_cursor = cur_cols[0];
      assert( (term->e_operator & (WO_OR|WO_AND))==0 );
      p_new->u.x.left_column = cur_cols[1];
      testcase( (prereq_left | extra_right) != prereq_left );
      p_new->prereq_right = prereq_left | extra_right;
      p_new->prereq_all = prereq_all;
      p_new->e_operator = (__operator_mask(dup->op) + extra_op) & op_mask;
    }else
    if( op==TK_ISNULL
     && !EXPR_HAS_PROPERTY(expr,EP_OuterON)
     && 0==__expr_can_be_null(left)
    ){
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      expr->op = TK_TRUEFALSE;  /* See tag-20230504-1 */
      expr->u.token = "false";
      EXPR_SET_PROPERTY(expr, EP_IsFalse);
      term->prereq_all = 0;
      term->e_operator = 0;
    }
  }

#ifndef DBSQL_OMIT_BETWEEN_OPTIMIZATION
  /* If a term is the BETWEEN operator, create two new virtual terms
  ** that define the range that the BETWEEN implements.  For example:
  **
  **      a BETWEEN b AND c
  **
  ** is converted into:
  **
  **      (a BETWEEN b AND c) AND (a>=b) AND (a<=c)
  **
  ** The two new terms are added onto the end of the where_clause_t object.
  ** The new terms are "dynamic" and are children of the original BETWEEN
  ** term.  That means that if the BETWEEN term is coded, the children are
  ** skipped.  Or, if the children are satisfied by an index, the original
  ** BETWEEN term is skipped.
  */
  else if( expr->op==TK_BETWEEN && wc->op==TK_AND ){
    expr_list_t *list;
    int i;
    static const u_int8_t ops[] = {TK_GE, TK_LE};
    assert( EXPR_USE_X_LIST(expr) );
    list = expr->x.list;
    assert( list!=0 );
    assert( list->n_expr==2 );
    assert( wc->a[idx_term].n_child==0 );
    for(i=0; i<2; i++){
      expr_t *new_expr;
      int idx_new;
      new_expr = __p_expr(parser, ops[i],
                             __expr_dup(db, expr->left, 0),
                             __expr_dup(db, list->a[i].expr, 0));
      __transfer_join_markings(new_expr, expr);
      idx_new = __where_clause_insert(wc, new_expr, TERM_VIRTUAL|TERM_DYNAMIC);
      testcase( idx_new==0 );
      __expr_analyze(src, wc, idx_new);
      term = &wc->a[idx_term];
      __mark_term_as_child(wc, idx_new, idx_term);
    }
  }
#endif /* DBSQL_OMIT_BETWEEN_OPTIMIZATION */

#if !defined(DBSQL_OMIT_OR_OPTIMIZATION) && !defined(DBSQL_OMIT_SUBQUERY)
  /* Analyze a term that is composed of two or more subterms connected by
  ** an OR operator.
  */
  else if( expr->op==TK_OR && !EXPR_HAS_PROPERTY(expr, EP_Collate) ){
    assert( wc->op==TK_AND );
    __expr_analyze_or_term(src, wc, idx_term);
    term = &wc->a[idx_term];
  }
#endif /* DBSQL_OMIT_OR_OPTIMIZATION */
  /* The form "x IS NOT NULL" can sometimes be evaluated more efficiently
  ** as "x>NULL" if x is not an INTEGER PRIMARY KEY.  So construct a
  ** virtual term of that form.
  **
  ** The virtual term must be tagged with TERM_VNULL.
  */
  else if( expr->op==TK_NOTNULL ){
    if( expr->left->op==TK_COLUMN
     && expr->left->i_column>=0
     && !EXPR_HAS_PROPERTY(expr, EP_OuterON)
    ){
      expr_t *new_expr;
      expr_t *left = expr->left;
      int idx_new;
      where_term_t *new_term;
 
      new_expr = __p_expr(parser, TK_GT,
                              __expr_dup(db, left, 0),
                              __expr_alloc(db, TK_NULL, 0, 0));
 
      idx_new = __where_clause_insert(wc, new_expr,
                                TERM_VIRTUAL|TERM_DYNAMIC|TERM_VNULL);
      if( idx_new ){
        new_term = &wc->a[idx_new];
        new_term->prereq_right = 0;
        new_term->left_cursor = left->i_table;
        new_term->u.x.left_column = left->i_column;
        new_term->e_operator = WO_GT;
        __mark_term_as_child(wc, idx_new, idx_term);
        term = &wc->a[idx_term];
        term->wt_flags |= TERM_COPIED;
        new_term->prereq_all = term->prereq_all;
      }
    }
  }


#ifndef DBSQL_OMIT_LIKE_OPTIMIZATION
  /* Add constraints to reduce the search space on a LIKE or GLOB
  ** operator.
  **
  ** A like pattern of the form "x LIKE 'bcs%'" is changed into constraints
  **
  **          x>='ABC' AND x<'abd' AND x LIKE 'bcs%'
  **
  ** The last character of the prefix "abc" is incremented to form the
  ** termination condition "abd".  If case is not significant (the default
  ** for LIKE) then the lower-bound is made all uppercase and the upper-
  ** bound is made all lowercase so that the bounds also work when comparing
  ** BLOBs.
  */
  else if( expr->op==TK_FUNCTION
   && wc->op==TK_AND
   && __is_like_or_glob(parser, expr, &str1, &is_complete, &no_case)
  ){
    expr_t *left;       /* LHS of LIKE/GLOB operator */
    expr_t *str2;       /* Copy of str1 - RHS of LIKE/GLOB operator */
    expr_t *new_expr1;
    expr_t *new_expr2;
    int idx_new1;
    int idx_new2;
    const char *coll_seq_name;     /* Name of collating sequence */
    const u_int16_t wt_flags = TERM_LIKEOPT | TERM_VIRTUAL | TERM_DYNAMIC;

    assert( EXPR_USE_X_LIST(expr) );
    left = expr->x.list->a[1].expr;
    str2 = __expr_dup(db, str1, 0);
    assert( str1==0 || !EXPR_HAS_PROPERTY(str1, EP_IntValue) );
    assert( str2==0 || !EXPR_HAS_PROPERTY(str2, EP_IntValue) );


    /* Convert the lower bound to upper-case and the upper bound to
    ** lower-case (upper-case is less than lower-case in ASCII) so that
    ** the range constraints also work for BLOBs
    */
    if( no_case && !parser->db->malloc_failed ){
      int i;
      char c;
      term->wt_flags |= TERM_LIKE;
      for(i=0; (c = str1->u.token[i])!=0; i++){
        str1->u.token[i] = __dbsql_toupper(c);
        str2->u.token[i] = __dbsql_tolower(c);
      }
    }

    if( !db->malloc_failed ){
      u_int8_t *p_c;       /* Last character before the first wildcard */
      p_c = (u_int8_t*)&str2->u.token[__strlen30(str2->u.token)-1];
      if( no_case ){
        /* The point is to increment the last character before the first
        ** wildcard.  But if we increment '@', that will push it into the
        ** alphabetic range where case conversions will mess up the
        ** inequality.  To avoid this, make sure to also run the full
        ** LIKE on all candidate expressions by clearing the is_complete flag
        */
        if( *p_c=='A'-1 ) is_complete = 0;
        *p_c = __upper_to_lower[*p_c];
      }

      /* Increment the value of the last utf8 character in the prefix. */
      while( *p_c==0xBF && p_c>(u_int8_t*)str2->u.token ){
        *p_c = 0x80;
        p_c--;
      }
      assert( *p_c!=0xFF );        /* __is_like_or_glob() guarantees this */
      (*p_c)++;
    }
    coll_seq_name = no_case ? "NOCASE" : __str_binary;
    new_expr1 = __expr_dup(db, left, 0);
    new_expr1 = __p_expr(parser, TK_GE,
           __expr_add_collate_string(parser,new_expr1,coll_seq_name),
           str1);
    __transfer_join_markings(new_expr1, expr);
    idx_new1 = __where_clause_insert(wc, new_expr1, wt_flags);
    testcase( idx_new1==0 );
    new_expr2 = __expr_dup(db, left, 0);
    new_expr2 = __p_expr(parser, TK_LT,
           __expr_add_collate_string(parser,new_expr2,coll_seq_name),
           str2);
    __transfer_join_markings(new_expr2, expr);
    idx_new2 = __where_clause_insert(wc, new_expr2, wt_flags);
    testcase( idx_new2==0 );
    __expr_analyze(src, wc, idx_new1);
    __expr_analyze(src, wc, idx_new2);
    term = &wc->a[idx_term];
    if( is_complete ){
      __mark_term_as_child(wc, idx_new1, idx_term);
      __mark_term_as_child(wc, idx_new2, idx_term);
    }
  }
#endif /* DBSQL_OMIT_LIKE_OPTIMIZATION */

  /* If there is a vector == or IS term - e.g. "(a, b) == (?, ?)" - create
  ** new terms for each component comparison - "a = ?" and "b = ?".  The
  ** new terms completely replace the original vector comparison, which is
  ** no longer used.
  **
  ** This is only required if at least one side of the comparison operation
  ** is not a sub-select.
  **
  ** tag-20220128a
  */
  if( (expr->op==TK_EQ || expr->op==TK_IS)
   && (n_left = __expr_vector_size(expr->left))>1
   && __expr_vector_size(expr->right)==n_left
   && ( (expr->left->flags & EP_xIsSelect)==0
     || (expr->right->flags & EP_xIsSelect)==0)
   && wc->op==TK_AND
  ){
    int i;
    for(i=0; i<n_left; i++){
      int idx_new;
      expr_t *p_new;
      expr_t *left = __expr_for_vector_field(parser, expr->left, i, n_left);
      expr_t *right = __expr_for_vector_field(parser, expr->right, i, n_left);

      p_new = __p_expr(parser, expr->op, left, right);
      __transfer_join_markings(p_new, expr);
      idx_new = __where_clause_insert(wc, p_new, TERM_DYNAMIC|TERM_SLICE);
      __expr_analyze(src, wc, idx_new);
    }
    term = &wc->a[idx_term];
    term->wt_flags |= TERM_CODED|TERM_VIRTUAL;  /* Disable the original */
    term->e_operator = WO_ROWVAL;
  }

  /* If there is a vector IN term - e.g. "(a, b) IN (SELECT ...)" - create
  ** a virtual term for each vector component. The expression object
  ** used by each such virtual term is expr (the full vector IN(...)
  ** expression). The where_term_t.u.x.i_field variable identifies the index within
  ** the vector on the LHS that the virtual term represents.
  **
  ** This only works if the RHS is a simple SELECT (not a compound) that does
  ** not use window functions.
  */
  else if( expr->op==TK_IN
   && term->u.x.i_field==0
   && expr->left->op==TK_VECTOR
   && ALWAYS( EXPR_USE_X_SELECT(expr) )
   && (expr->x.p_select->prior==0 || (expr->x.p_select->sel_flags & SF_Values))
#ifndef DBSQL_OMIT_WINDOWFUNC
   && expr->x.p_select->win==0
#endif
   && wc->op==TK_AND
   && expr->x.p_select->e_list->n_expr <= UMXV(term->n_child)
   /* ^-- See bug 2026-06-04T10:00:49Z */
  ){
    int i;
    assert( term->n_child==0 );
    for(i=0; i<__expr_vector_size(expr->left); i++){
      int idx_new;
      idx_new = __where_clause_insert(wc, expr, TERM_VIRTUAL|TERM_SLICE);
      wc->a[idx_new].u.x.i_field = i+1;
      __expr_analyze(src, wc, idx_new);
      __mark_term_as_child(wc, idx_new, idx_term);
    }
  }

#ifndef DBSQL_OMIT_VIRTUALTABLE
  /* Add a WO_AUX auxiliary term to the constraint set if the
  ** current expression is of the form "column OP expr" where OP
  ** is an operator that gets passed into virtual tables but which is
  ** not normally optimized for ordinary tables.  In other words, OP
  ** is one of MATCH, LIKE, GLOB, REGEXP, !=, IS, IS NOT, or NOT NULL.
  ** This information is used by the best_index methods of
  ** virtual tables.  The native query optimizer does not attempt
  ** to do anything with MATCH functions.
  */
  else if( wc->op==TK_AND ){
    expr_t *right = 0, *left = 0;
    int res = __is_auxiliary_vtab_operator(db, expr, &op2, &left, &right);
    while( res-- > 0 ){
      int idx_new;
      where_term_t *new_term;
      Bitmask prereq_column, prereq_expr;

      prereq_expr = __where_expr_usage(mask_set, right);
      prereq_column = __where_expr_usage(mask_set, left);
      if( (prereq_expr & prereq_column)==0 ){
        expr_t *new_expr;
        new_expr = __p_expr(parser, TK_MATCH,
            0, __expr_dup(db, right, 0));
        if( EXPR_HAS_PROPERTY(expr, EP_OuterON) && new_expr ){
          EXPR_SET_PROPERTY(new_expr, EP_OuterON);
          new_expr->w.i_join = expr->w.i_join;
        }
        idx_new = __where_clause_insert(wc, new_expr, TERM_VIRTUAL|TERM_DYNAMIC);
        testcase( idx_new==0 );
        new_term = &wc->a[idx_new];
        new_term->prereq_right = prereq_expr | extra_right;
        new_term->left_cursor = left->i_table;
        new_term->u.x.left_column = left->i_column;
        new_term->e_operator = WO_AUX;
        new_term->match_op = op2;
        __mark_term_as_child(wc, idx_new, idx_term);
        term = &wc->a[idx_term];
        term->wt_flags |= TERM_COPIED;
        new_term->prereq_all = term->prereq_all;
      }
      SWAP(expr_t*, left, right);
    }
  }
#endif /* DBSQL_OMIT_VIRTUALTABLE */

  /* Prevent ON clause terms of a LEFT JOIN from being used to drive
  ** an index for tables to the left of the join.
  */
  testcase( term!=&wc->a[idx_term] );
  term = &wc->a[idx_term];
  term->prereq_right |= extra_right;
}

/***************************************************************************
** Routines with file scope above.  Interface to the rest of the where.c
** subsystem follows.
***************************************************************************/

/*
** This routine identifies subexpressions in the WHERE clause where
** each subexpression is separated by the AND operator or some other
** operator specified in the op parameter.  The where_clause_t structure
** is filled with pointers to subexpressions.  For example:
**
**    WHERE  a=='hello' AND coalesce(b,11)<10 AND (c+12!=d OR c==22)
**           \________/     \_______________/     \________________/
**            slot[0]            slot[1]               slot[2]
**
** The original WHERE clause in expr is unaltered.  All this routine
** does is make slot[] entries point to substructure within expr.
**
** In the previous sentence and in the diagram, "slot[]" refers to
** the where_clause_t.a[] array.  The slot[] array grows as needed to contain
** all terms of the WHERE clause.
*/
void __where_split(where_clause_t *wc, expr_t *expr, u_int8_t op){
  expr_t *e2 = __expr_skip_collate_and_likely(expr);
  wc->op = op;
  assert( e2!=0 || expr==0 );
  if( e2==0 ) return;
  if( e2->op!=op ){
    __where_clause_insert(wc, expr, 0);
  }else{
    __where_split(wc, e2->left, op);
    __where_split(wc, e2->right, op);
  }
}

/*
** Add either a LIMIT (if match_op==DBSQL_INDEX_CONSTRAINT_LIMIT) or
** OFFSET (if match_op==DBSQL_INDEX_CONSTRAINT_OFFSET) term to the
** where-clause passed as the first argument. The value for the term
** is found in register i_reg.
**
** In the common case where the value is a simple integer
** (example: "LIMIT 5 OFFSET 10") then the expression codes as a
** TK_INTEGER so that it will be available to dbsql_vtab_rhs_value().
** If not, then it codes as a TK_REGISTER expression.
*/
static void __where_add_limit_expr(
  where_clause_t *wc,   /* Add the constraint to this WHERE clause */
  int i_reg,           /* Register that will hold value of the limit/offset */
  expr_t *expr,        /* Expression that defines the limit/offset */
  int i_csr,           /* Cursor to which the constraint applies */
  int match_op        /* DBSQL_INDEX_CONSTRAINT_LIMIT or _OFFSET */
){
  parser_t *parser = wc->w_info->parser;
  DBSQL *db = parser->db;
  expr_t *p_new;
  int i_val = 0;

  if( __expr_is_integer(expr, &i_val, parser) && i_val>=0 ){
    expr_t *val = __expr_int32(db, i_val);
    if( val==0 ) return;
    p_new = __p_expr(parser, TK_MATCH, 0, val);
  }else{
    expr_t *val = __expr_alloc(db, TK_REGISTER, 0, 0);
    if( val==0 ) return;
    val->i_table = i_reg;
    p_new = __p_expr(parser, TK_MATCH, 0, val);
  }
  if( p_new ){
    where_term_t *term;
    int idx;
    idx = __where_clause_insert(wc, p_new, TERM_DYNAMIC|TERM_VIRTUAL);
    term = &wc->a[idx];
    term->left_cursor = i_csr;
    term->e_operator = WO_AUX;
    term->match_op = match_op;
  }
}

/*
** Possibly add terms corresponding to the LIMIT and OFFSET clauses of the
** SELECT statement passed as the second argument. These terms are only
** added if:
**
**   1. The SELECT statement has a LIMIT clause, and
**   2. The SELECT statement is not an aggregate or DISTINCT query, and
**   3. The SELECT statement has exactly one object in its FROM clause, and
**      that object is a virtual table, and
**   4. There are no terms in the WHERE clause that will not be passed
**      to the virtual table best_index method.
**   5. The ORDER BY clause, if any, will be made available to the best_index
**      method.
**
** LIMIT and OFFSET terms are ignored by most of the planner code. They
** exist only so that they may be passed to the best_index method of the
** single virtual table in the FROM clause of the SELECT.
*/
void DBSQL_NOINLINE __where_add_limit(where_clause_t *wc, select_t *p){
  assert( p!=0 && p->limit!=0 );                 /* 1 -- checked by caller */
  if( p->group_by==0
   && (p->sel_flags & (SF_Distinct|SF_Aggregate))==0             /* 2 */
   && (p->src->n_src==1 && IS_VIRTUAL(p->src->a[0].s_tab))      /* 3 */
  ){
    expr_list_t *order_by = p->order_by;
    int i_csr = p->src->a[0].i_cursor;
    int ii;

    /* Check condition (4). Return early if it is not met. */
    for(ii=0; ii<wc->n_term; ii++){
      if( wc->a[ii].wt_flags & TERM_CODED ){
        /* This term is a vector operation that has been decomposed into
        ** other, subsequent terms.  It can be ignored. See tag-20220128a */
        assert( wc->a[ii].wt_flags & TERM_VIRTUAL );
        assert( wc->a[ii].e_operator==WO_ROWVAL );
        continue;
      }
      if( wc->a[ii].n_child ){
        /* If this term has child terms, then they are also part of the
        ** wc->a[] array. So this term can be ignored, as a LIMIT clause
        ** will only be added if each of the child terms passes the
        ** (left_cursor==i_csr) test below.  */
        continue;
      }
      if( wc->a[ii].left_cursor==i_csr && wc->a[ii].prereq_right==0 ) continue;

      /* If this term has a parent with exactly one child, and the parent will
      ** be passed through to best_index, then this term can be ignored.  */
      if( wc->a[ii].i_parent>=0 ){
        where_term_t *parent = &wc->a[ wc->a[ii].i_parent ];
        if( parent->left_cursor==i_csr
         && parent->prereq_right==0
         && parent->n_child==1
        ){
          continue;
        }
      }

      /* This term will not be passed through. Do not add a LIMIT clause. */
      return;
    }

    /* Check condition (5). Return early if it is not met. */
    if( order_by ){
      for(ii=0; ii<order_by->n_expr; ii++){
        expr_t *expr = order_by->a[ii].expr;
        if( expr->op!=TK_COLUMN ) return;
        if( expr->i_table!=i_csr ) return;
        if( order_by->a[ii].fg.sort_flags & KEYINFO_ORDER_BIGNULL ) return;
      }
    }

    /* All conditions are met. Add the terms to the where-clause object. */
    assert( p->limit->op==TK_LIMIT );
    if( p->i_offset!=0 && (p->sel_flags & SF_Compound)==0 ){
      __where_add_limit_expr(wc, p->i_offset, p->limit->right,
                        i_csr, DBSQL_INDEX_CONSTRAINT_OFFSET);
    }
    if( p->i_offset==0 || (p->sel_flags & SF_Compound)==0 ){
      __where_add_limit_expr(wc, p->i_limit, p->limit->left,
                        i_csr, DBSQL_INDEX_CONSTRAINT_LIMIT);
    }
  }
}

/*
** Initialize a preallocated where_clause_t structure.
*/
void __where_clause_init(
  where_clause_t *wc,        /* The where_clause_t to be initialized */
  where_info_t *w_info        /* The WHERE processing context */
){
  wc->w_info = w_info;
  wc->has_or = 0;
  wc->outer = 0;
  wc->n_term = 0;
  wc->n_base = 0;
  wc->n_slot = ARRAY_SIZE(wc->statics);
  wc->a = wc->statics;
}

/*
** Deallocate a where_clause_t structure.  The where_clause_t structure
** itself is not freed.  This routine is the inverse of
** __where_clause_init().
*/
void __where_clause_clear(where_clause_t *wc){
  DBSQL *db = wc->w_info->parser->db;
  assert( wc->n_term>=wc->n_base );
  if( wc->n_term>0 ){
    where_term_t *a = wc->a;
    where_term_t *lasts = &wc->a[wc->n_term-1];
#ifdef DBSQL_DEBUG
    int i;
    /* Verify that every term past wc->n_base is virtual */
    for(i=wc->n_base; i<wc->n_term; i++){
      assert( (wc->a[i].wt_flags & TERM_VIRTUAL)!=0 );
    }
#endif
    while(1){
      assert( a->match_op==0 || a->e_operator==WO_AUX );
      if( a->wt_flags & TERM_DYNAMIC ){
        __expr_delete(db, a->expr);
      }
      if( a->wt_flags & (TERM_ORINFO|TERM_ANDINFO) ){
        if( a->wt_flags & TERM_ORINFO ){
          assert( (a->wt_flags & TERM_ANDINFO)==0 );
          __where_or_info_delete(db, a->u.or_info);
        }else{
          assert( (a->wt_flags & TERM_ANDINFO)!=0 );
          __where_and_info_delete(db, a->u.and_info);
        }
      }
      if( a==lasts ) break;
      a++;
    }
  }
}


/*
** These routines walk (recursively) an expression tree and generate
** a bitmask indicating which tables are used in that expression
** tree.
**
** __where_expr_usage(__mask_set, expr_t) ->
**
**       Return a Bitmask of all tables referenced by expr_t.  expr_t can be
**       be NULL, in which case 0 is returned.
**
** __where_expr_usage_nn(__mask_set, expr_t) ->
**
**       Same as __where_expr_usage() except that expr_t must not be
**       NULL.  The "NN" suffix on the name stands for "Not Null".
**
** __where_expr_list_usage(__mask_set, expr_list_t) ->
**
**       Return a Bitmask of all tables referenced by every expression
**       in the expression list expr_list_t.  expr_list_t can be NULL, in which
**       case 0 is returned.
**
** __where_expr_usage_full(__mask_set, expr_list_t) ->
**
**       Internal use only.  Called only by __where_expr_usage_nn() for
**       complex expressions that require pushing register values onto
**       the stack.  Many calls to __where_expr_usage_nn() do not need
**       the more complex analysis done by this routine.  Hence, the
**       computations done by this routine are broken out into a separate
**       "no-inline" function to avoid the stack push overhead in the
**       common case where it is not needed.
*/
static DBSQL_NOINLINE Bitmask __where_expr_usage_full(
  where_mask_set_t *mask_set,
  expr_t *p
){
  Bitmask mask;
  mask = (p->op==TK_IF_NULL_ROW) ? __where_get_mask(mask_set, p->i_table) : 0;
  if( p->left ) mask |= __where_expr_usage_nn(mask_set, p->left);
  if( p->right ){
    mask |= __where_expr_usage_nn(mask_set, p->right);
    assert( p->x.list==0 );
  }else if( EXPR_USE_X_SELECT(p) ){
    if( EXPR_HAS_PROPERTY(p, EP_VarSelect) ) mask_set->var_select = 1;
    mask |= __expr_select_usage(mask_set, p->x.p_select);
  }else if( p->x.list ){
    mask |= __where_expr_list_usage(mask_set, p->x.list);
  }
#ifndef DBSQL_OMIT_WINDOWFUNC
  if( (p->op==TK_FUNCTION || p->op==TK_AGG_FUNCTION) && EXPR_USE_Y_WIN(p) ){
    assert( p->y.win!=0 );
    mask |= __where_expr_list_usage(mask_set, p->y.win->partition);
    mask |= __where_expr_list_usage(mask_set, p->y.win->order_by);
    mask |= __where_expr_usage(mask_set, p->y.win->filter);
  }
#endif
  return mask;
}
Bitmask __where_expr_usage_nn(where_mask_set_t *mask_set, expr_t *p){
  if( p->op==TK_COLUMN && !EXPR_HAS_PROPERTY(p, EP_FixedCol) ){
    return __where_get_mask(mask_set, p->i_table);
  }else if( EXPR_HAS_PROPERTY(p, EP_TokenOnly|EP_Leaf) ){
    assert( p->op!=TK_IF_NULL_ROW );
    return 0;
  }
  return __where_expr_usage_full(mask_set, p);
}
Bitmask __where_expr_usage(where_mask_set_t *mask_set, expr_t *p){
  return p ? __where_expr_usage_nn(mask_set,p) : 0;
}
Bitmask __where_expr_list_usage(where_mask_set_t *mask_set, expr_list_t *list){
  int i;
  Bitmask mask = 0;
  if( list ){
    for(i=0; i<list->n_expr; i++){
      mask |= __where_expr_usage(mask_set, list->a[i].expr);
    }
  }
  return mask;
}


/*
** Call __expr_analyze on all terms in a WHERE clause. 
**
** Note that __expr_analyze() might add new virtual terms onto the
** end of the WHERE clause.  We do not want to analyze these new
** virtual terms, so start analyzing at the end and work forward
** so that the added virtual terms are never processed.
*/
void __where_expr_analyze(
  src_list_t *tab_list,       /* the FROM clause */
  where_clause_t *wc         /* the WHERE clause to be analyzed */
){
  int i;
  for(i=wc->n_term-1; i>=0; i--){
    __expr_analyze(tab_list, wc, i);
  }
}

/*
** For table-valued-functions, transform the function arguments into
** new WHERE clause terms. 
**
** Each function argument translates into an equality constraint against
** a HIDDEN column in the table.
*/
void __where_tab_func_args(
  parser_t *parser,                    /* Parsing context */
  src_item_t *item,                   /* The FROM clause term to process */
  where_clause_t *wc                  /* Xfer function arguments to here */
){
  table_t *tab;
  int j, k;
  expr_list_t *args;
  expr_t *col_ref;
  expr_t *term;
  if( item->fg.is_tab_func==0 ) return;
  tab = item->s_tab;
  assert( tab!=0 );
  args = item->u1.func_arg;
  if( args==0 ) return;
  for(j=k=0; j<args->n_expr; j++){
    expr_t *rhs;
    u_int32_t join_type;
    while( k<tab->n_col && (tab->cols[k].col_flags & COLFLAG_HIDDEN)==0 ){k++;}
    if( k>=tab->n_col ){
      __error_msg(parser, "too many arguments on %s() - max %d",
                      tab->name, j);
      return;
    }
    col_ref = __expr_alloc(parser->db, TK_COLUMN, 0, 0);
    if( col_ref==0 ) return;
    col_ref->i_table = item->i_cursor;
    col_ref->i_column = k++;
    assert( EXPR_USE_Y_TAB(col_ref) );
    col_ref->y.tab = tab;
    item->col_used |= __expr_col_used(col_ref);
    rhs = __p_expr(parser, TK_UPLUS,
        __expr_dup(parser->db, args->a[j].expr, 0), 0);
    term = __p_expr(parser, TK_EQ, col_ref, rhs);
    if( item->fg.jointype & (JT_LEFT|JT_RIGHT) ){
      testcase( item->fg.jointype & JT_LEFT );  /* testtag-20230227a */
      testcase( item->fg.jointype & JT_RIGHT ); /* testtag-20230227b */
      join_type = EP_OuterON;
    }else{
      testcase( item->fg.jointype & JT_LTORJ ); /* testtag-20230227c */
      join_type = EP_InnerON;
    }
    __set_join_expr(term, item->i_cursor, join_type);
    __where_clause_insert(wc, term, TERM_DYNAMIC);
  }
}
