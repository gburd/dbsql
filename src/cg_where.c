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
** This module contains C code that generates VDBE code used to process
** the WHERE clause of SQL statements.  This module is responsible for
** generating the code that loops through a table looking for applicable
** rows.  Indices are selected and used to speed the search when doing
** so is applicable.  Because this module is responsible for selecting
** indices, you might also think of this module as the "query optimizer".
*/
#include "dbsql_int.h"
#include "where_int.h"

/*
** Extra information appended to the end of dbsql_index_info_t but not
** visible to the best_index function, at least not directly.  The
** dbsql_vtab_collation() interface knows how to reach it, however.
**
** This object is not an API and can be changed from one release to the
** next.  As long as __allocate_index_info() and dbsql_vtab_collation()
** agree on the structure, all will be well.
*/
typedef struct hidden_index_info_t hidden_index_info_t;
struct hidden_index_info_t {
  where_clause_t *wc;        /* The Where clause being analyzed */
  parser_t *parser;           /* The parsing context */
  int distinct;           /* Value to return from dbsql_vtab_distinct() */
  u_int32_t m_in;                 /* Mask of terms that are <col> IN (...) */
  u_int32_t m_handle_in;           /* Terms that vtab will handle as <col> IN (...) */
  dbsql_value_t *rhses[FLEXARRAY];  /* RHS values for constraints. MUST BE LAST
                                   ** Extra space is allocated to hold up
                                   ** to n_term such values */
};

/* Size (in bytes) of a __hidden_inde_info object sufficient to hold as
** many as N constraints */
#define SZ_HIDDENINDEXINFO(N) \
                  (offsetof(hidden_index_info_t,rhses) + (N)*sizeof(dbsql_value_t*))

/* Forward declaration of methods */
static int __where_loop_resize(DBSQL*, where_loop_t*, int);

/*
** Return the estimated number of output rows from a WHERE clause
*/
log_est_t __where_output_row_count(where_info_t *w_info){
  return w_info->n_row_out;
}

/*
** Return one of the WHERE_DISTINCT_xxxxx values to indicate how this
** WHERE clause returns outputs for DISTINCT processing.
*/
int __where_is_distinct(where_info_t *w_info){
  return w_info->distinct;
}

/*
** Return the number of ORDER BY terms that are satisfied by the
** WHERE clause.  A return of 0 means that the output must be
** completely sorted.  A return equal to the number of ORDER BY
** terms means that no sorting is needed at all.  A return that
** is positive but less than the number of ORDER BY terms means that
** block sorting is required.
*/
int __where_is_ordered(where_info_t *w_info){
  return w_info->n_ob_sat<0 ? 0 : w_info->n_ob_sat;
}

/*
** In the ORDER BY LIMIT optimization, if the inner-most loop is known
** to emit rows in increasing order, and if the last row emitted by the
** inner-most loop did not fit within the sorter, then we can skip all
** subsequent rows for the current iteration of the inner loop (because they
** will not fit in the sorter either) and continue with the second inner
** loop - the loop immediately outside the inner-most.
**
** When a row does not fit in the sorter (because the sorter already
** holds LIMIT+OFFSET rows that are smaller), then a jump is made to the
** label returned by this function.
**
** If the ORDER BY LIMIT optimization applies, the jump destination should
** be the continuation for the second-inner-most loop.  If the ORDER BY
** LIMIT optimization does not apply, then the jump destination should
** be the continuation for the inner-most loop.
**
** It is always safe for this routine to return the continuation of the
** inner-most loop, in the sense that a correct answer will result. 
** returning_t the continuation the second inner loop is an optimization
** that might make the code run a little faster, but should not change
** the final answer.
*/
int __where_order_by_limit_opt_label(where_info_t *w_info){
  where_level_t *inner;
  if( !w_info->ordered_inner_loop ){
    /* The ORDER BY LIMIT optimization does not apply.  Jump to the
    ** continuation of the inner-most loop. */
    return w_info->i_continue;
  }
  inner = &w_info->a[w_info->n_level-1];
  assert( inner->addr_nxt!=0 );
  return inner->rj ? w_info->i_continue : inner->addr_nxt;
}

/*
** While generating code for the min/max optimization, after handling
** the aggregate-step call to min() or max(), check to see if any
** additional looping is required.  If the output order is such that
** we are certain that the correct answer has already been found, then
** code an OP_Goto to by pass subsequent processing.
**
** Any extra OP_Goto that is coded here is an optimization.  The
** correct answer should be obtained regardless.  This OP_Goto just
** makes the answer appear faster.
*/
void __where_min_max_opt_early_out(vdbe_t *v, where_info_t *w_info){
  where_level_t *inner;
  int i;
  if( !w_info->ordered_inner_loop ) return;
  if( w_info->n_ob_sat==0 ) return;
  for(i=w_info->n_level-1; i>=0; i--){
    inner = &w_info->a[i];
    if( (inner->w_loop->ws_flags & WHERE_COLUMN_IN)!=0 ){
      __vdbe_goto(v, inner->addr_nxt);
      return;
    }
  }
  __vdbe_goto(v, w_info->i_break);
}

/*
** Return the VDBE address or label to jump to in order to continue
** immediately with the next row of a WHERE clause.
*/
int __where_continue_label(where_info_t *w_info){
  assert( w_info->i_continue!=0 );
  return w_info->i_continue;
}

/*
** Return the VDBE address or label to jump to in order to break
** out of a WHERE loop.
*/
int __where_break_label(where_info_t *w_info){
  return w_info->i_break;
}

/*
** Return ONEPASS_OFF (0) if an UPDATE or DELETE statement is unable to
** operate directly on the rowids returned by a WHERE clause.  Return
** ONEPASS_SINGLE (1) if the statement can operation directly because only
** a single row is to be changed.  Return ONEPASS_MULTI (2) if the one-pass
** optimization can be used on multiple
**
** If the ONEPASS optimization is used (if this routine returns true)
** then also write the indices of open cursors used by ONEPASS
** into curs[0] and curs[1].  ia_cur[0] gets the cursor of the data
** table and ia_cur[1] gets the cursor used by an auxiliary index.
** Either value may be -1, indicating that cursor is not used.
** Any cursors returned will have been opened for writing.
**
** curs[0] and curs[1] both get -1 if the where-clause logic is
** unable to use the ONEPASS optimization.
*/
int __where_ok_one_pass(where_info_t *w_info, int *curs){
  memcpy(curs, w_info->cur_one_passes, sizeof(int)*2);
#ifdef WHERETRACE_ENABLED
  if( __where_trace && w_info->one_pass!=ONEPASS_OFF ){
    __debug_printf("%s cursors: %d %d\n",
         w_info->one_pass==ONEPASS_SINGLE ? "ONEPASS_SINGLE" : "ONEPASS_MULTI",
         curs[0], curs[1]);
  }
#endif
  return w_info->one_pass;
}

/*
** Return TRUE if the WHERE loop uses the OP_DeferredSeek opcode to move
** the data cursor to the row selected by the index cursor.
*/
int __where_uses_deferred_seek(where_info_t *w_info){
  return w_info->deferred_seek;
}

/*
** Move the content of src into dest
*/
static void __where_or_move(where_or_set_t *dest, where_or_set_t *src){
  dest->n = src->n;
  memcpy(dest->a, src->a, dest->n*sizeof(dest->a[0]));
}

/*
** Try to insert a new prerequisite/cost entry into the where_or_set_t set.
**
** The new entry might overwrite an existing entry, or it might be
** appended, or it might be discarded.  Do whatever is the right thing
** so that set keeps the N_OR_COST best entries seen so far.
*/
static int __where_or_insert(
  where_or_set_t *set,      /* The where_or_set_t to be updated */
  Bitmask prereq,        /* Prerequisites of the new entry */
  log_est_t r_run,           /* Run-cost of the new entry */
  log_est_t n_out            /* Number of outputs for the new entry */
){
  u_int16_t i;
  where_or_cost_t *p;
  for(i=set->n, p=set->a; i>0; i--, p++){
    if( r_run<=p->r_run && (prereq & p->prereq)==prereq ){
      goto where_or_insert_done;
    }
    if( p->r_run<=r_run && (p->prereq & prereq)==p->prereq ){
      return 0;
    }
  }
  if( set->n<N_OR_COST ){
    p = &set->a[set->n++];
    p->n_out = n_out;
  }else{
    p = set->a;
    for(i=1; i<set->n; i++){
      if( p->r_run>set->a[i].r_run ) p = set->a + i;
    }
    if( p->r_run<=r_run ) return 0;
  }
where_or_insert_done:
  p->prereq = prereq;
  p->r_run = r_run;
  if( p->n_out>n_out ) p->n_out = n_out;
  return 1;
}

/*
** Return the bitmask for the given cursor number.  Return 0 if
** i_cursor is not in the set.
*/
Bitmask __where_get_mask(where_mask_set_t *mask_set, int i_cursor){
  int i;
  assert( mask_set->n<=(int)sizeof(Bitmask)*8 );
  assert( mask_set->n>0 || mask_set->ix[0]<0 );
  assert( i_cursor>=-1 );
  if( mask_set->ix[0]==i_cursor ){
    return 1;
  }
  for(i=1; i<mask_set->n; i++){
    if( mask_set->ix[i]==i_cursor ){
      return MASKBIT(i);
    }
  }
  return 0;
}

/* Allocate memory that is automatically freed when w_info is freed.
*/
void *__where_malloc(where_info_t *w_info, u_int64_t n_byte){
  where_mem_block_t *block;
  block = __db_malloc_raw_nn(w_info->parser->db, n_byte+sizeof(*block));
  if( block ){
    block->next = w_info->mem_to_free;
    block->sz = n_byte;
    w_info->mem_to_free = block;
    block++;
  }
  return (void*)block;
}
void *__where_realloc(where_info_t *w_info, void *old, u_int64_t n_byte){
  void *p_new = __where_malloc(w_info, n_byte);
  if( p_new && old ){
    where_mem_block_t *old_blk = (where_mem_block_t*)old;
    old_blk--;
    assert( old_blk->sz<n_byte );
    memcpy(p_new, old, old_blk->sz);
  }
  return p_new;
}

/*
** Create a new mask for cursor i_cursor.
**
** There is one cursor per table in the FROM clause.  The number of
** tables in the FROM clause is limited by a test early in the
** __where_begin() routine.  So we know that the mask_set->ix[]
** array will never overflow.
*/
static void __create_mask(where_mask_set_t *mask_set, int i_cursor){
  assert( mask_set->n < ARRAY_SIZE(mask_set->ix) );
  mask_set->ix[mask_set->n++] = i_cursor;
}

/*
** If the right-hand branch of the expression is a TK_COLUMN, then return
** a pointer to the right-hand branch.  Otherwise, return NULL.
*/
static expr_t *__where_right_subexpr_is_column(expr_t *p){
  p = __expr_skip_collate_and_likely(p->right);
  if( ALWAYS(p!=0) && p->op==TK_COLUMN && !EXPR_HAS_PROPERTY(p, EP_FixedCol) ){
    return p;
  }
  return 0;
}

/*
** Term term is guaranteed to be a WO_IN term. It may be a component term
** of a vector IN expression of the form "(x, y, ...) IN (SELECT ...)".
** This function checks to see if the term is compatible with an index
** column with affinity idxaff (one of the DBSQL_AFF_XYZ values). If so,
** it returns a pointer to the name of the collation sequence (e.g. "BINARY"
** or "NOCASE") used by the comparison in term. If it is not compatible
** with affinity idxaff, NULL is returned.
*/
static DBSQL_NOINLINE const char *__index_in_affinity_ok(
  parser_t *parser, 
  where_term_t *term, 
  u_int8_t idxaff
){
  expr_t *x = term->expr;
  expr_t inexpr;

  assert( term->e_operator & WO_IN );

  if( __expr_is_vector(x->left) ){
    int i_field = term->u.x.i_field - 1;
    inexpr.flags = 0;
    inexpr.op = TK_EQ;
    inexpr.left = x->left->x.list->a[i_field].expr;
    assert( EXPR_USE_X_SELECT(x) );
    inexpr.right = x->x.p_select->e_list->a[i_field].expr;
    x = &inexpr;
  }

  if( __index_affinity_ok(x, idxaff) ){
    coll_seq_t *ret = __expr_compare_coll_seq(parser, x);
    return ret ? ret->name : __str_binary;
  }
  return 0;
}

/*
** Advance to the next where_term_t that matches according to the criteria
** established when the scan object was initialized by __where_scan_init().
** Return NULL if there are no more matching __where_terms.
*/
static where_term_t *__where_scan_next(where_scan_t *scan){
  int i_cur;            /* The cursor on the LHS of the term */
  int16_t i_column;         /* The column on the LHS of the term.  -1 for IPK */
  expr_t *x;            /* An expression being tested */
  where_clause_t *wc;    /* Shorthand for scan->wc */
  where_term_t *term;    /* The term being tested */
  int k = scan->k;    /* Where to start scanning */

  assert( scan->i_equiv<=scan->n_equiv );
  wc = scan->wc;
  while(1){
    i_column = scan->columns[scan->i_equiv-1];
    i_cur = scan->curs[scan->i_equiv-1];
    assert( wc!=0 );
    assert( i_cur>=0 );
    do{
      for(term=wc->a+k; k<wc->n_term; k++, term++){
        assert( (term->e_operator & (WO_OR|WO_AND))==0 || term->left_cursor<0 );
        if( term->left_cursor==i_cur
         && term->u.x.left_column==i_column
         && (i_column!=XN_EXPR
             || __expr_compare_skip(term->expr->left,
                                       scan->idx_expr,i_cur)==0)
         && (scan->i_equiv<=1 || !EXPR_HAS_PROPERTY(term->expr, EP_OuterON))
        ){
          if( (term->e_operator & WO_EQUIV)!=0
           && scan->n_equiv<ARRAY_SIZE(scan->curs)
           && (x = __where_right_subexpr_is_column(term->expr))!=0
          ){
            int j;
            for(j=0; j<scan->n_equiv; j++){
              if( scan->curs[j]==x->i_table
               && scan->columns[j]==x->i_column ){
                  break;
              }
            }
            if( j==scan->n_equiv ){
              scan->curs[j] = x->i_table;
              scan->columns[j] = x->i_column;
              scan->n_equiv++;
            }
          }
          if( (term->e_operator & scan->op_mask)!=0 ){
            /* Verify the affinity and collating sequence match */
            if( scan->coll_name && (term->e_operator & WO_ISNULL)==0 ){
              const char *coll_name;
              parser_t *parser = wc->w_info->parser;
              x = term->expr;

              if( (term->e_operator & WO_IN) ){
                coll_name = __index_in_affinity_ok(parser, term, scan->idxaff);
                if( !coll_name ) continue;
              }else{
                coll_seq_t *coll;
                if( !__index_affinity_ok(x, scan->idxaff) ){
                  continue;
                }
                assert(x->left);
                coll = __expr_compare_coll_seq(parser, x);
                coll_name = coll ? coll->name : __str_binary;
              }

              if( __str_icmp(coll_name, scan->coll_name) ){
                continue;
              }
            }
            if( (term->e_operator & (WO_EQ|WO_IS))!=0
             && (x = term->expr->right, ALWAYS(x!=0))
             && x->op==TK_COLUMN
             && x->i_table==scan->curs[0]
             && x->i_column==scan->columns[0]
            ){
              testcase( term->e_operator & WO_IS );
              continue;
            }
            scan->wc = wc;
            scan->k = k+1;
#ifdef WHERETRACE_ENABLED
            if( (__where_trace & 0x20000)!=0 && scan->n_equiv>1 ){
              int ii;
              __debug_printf("EQUIVALENT TO {%d:%d} (due to TERM-%d):",
                 scan->curs[0], scan->columns[0], term->i_term);
              for(ii=1; ii<scan->n_equiv; ii++){
                __debug_printf(" {%d:%d}",
                   scan->curs[ii], scan->columns[ii]);
              }
              __debug_printf("\n");
            }
#endif
            return term;
          }
        }
      }
      wc = wc->outer;
      k = 0;
    }while( wc!=0 );
    if( scan->i_equiv>=scan->n_equiv ) break;
    wc = scan->orig_wc;
    k = 0;
    scan->i_equiv++;
  }
  return 0;
}

/*
** This is __where_scan_init() for the case of an index on an expression.
** It is factored out into a separate tail-recursion subroutine so that
** the normal __where_scan_init() routine, which is a high-runner, does not
** need to push registers onto the stack as part of its prologue.
*/
static DBSQL_NOINLINE where_term_t *__where_scan_init_index_expr(where_scan_t *scan){
  scan->idxaff = __expr_affinity(scan->idx_expr);
  return __where_scan_next(scan);
}

/*
** Initialize a WHERE clause scanner object.  Return a pointer to the
** first match.  Return NULL if there are no matches.
**
** The scanner will be searching the WHERE clause wc.  It will look
** for terms of the form "X <op> <expr>" where X is column i_column of table
** i_cur.   Or if idx!=0 then X is column i_column of index idx.  idx
** must be one of the indexes of table i_cur.
**
** The <op> must be one of the operators described by op_mask.
**
** If the search is for X and the WHERE clause contains terms of the
** form X=Y then this routine might also return terms of the form
** "Y <op> <expr>".  The number of levels of transitivity is limited,
** but is enough to handle most commonly occurring SQL statements.
**
** If X is not the INTEGER PRIMARY KEY then X must be compatible with
** index idx.
*/
static where_term_t *__where_scan_init(
  where_scan_t *scan,       /* The where_scan_t object being initialized */
  where_clause_t *wc,       /* The WHERE clause to be scanned */
  int i_cur,               /* Cursor to scan for */
  int i_column,            /* column_t to scan for */
  u_int32_t op_mask,             /* Operator(s) to scan for */
  index_t *idx             /* Must be compatible with this index */
){
  scan->orig_wc = wc;
  scan->wc = wc;
  scan->idx_expr = 0;
  scan->idxaff = 0;
  scan->coll_name = 0;
  scan->op_mask = op_mask;
  scan->k = 0;
  scan->curs[0] = i_cur;
  scan->n_equiv = 1;
  scan->i_equiv = 1;
  if( idx ){
    int j = i_column;
    i_column = idx->columns[j];
    if( i_column==idx->table->i_p_key ){
      i_column = XN_ROWID;
    }else if( i_column>=0 ){
      scan->idxaff = idx->table->cols[i_column].affinity;
      scan->coll_name = idx->colls[j];
    }else if( i_column==XN_EXPR ){
      scan->idx_expr = idx->col_exprs->a[j].expr;
      scan->coll_name = idx->colls[j];
      scan->columns[0] = XN_EXPR;
      return __where_scan_init_index_expr(scan);
    }
  }else if( i_column==XN_EXPR ){
    return 0;
  }
  scan->columns[0] = i_column;
  return __where_scan_next(scan);
}

/*
** Search for a term in the WHERE clause that is of the form "X <op> <expr>"
** where X is a reference to the i_column of table i_cur or of index idx
** if idx!=0 and <op> is one of the WO_xx operator codes specified by
** the op parameter.  Return a pointer to the term.  Return 0 if not found.
**
** If idx!=0 then it must be one of the indexes of table i_cur. 
** Search for terms matching the i_column-th column of idx
** rather than the i_column-th column of table i_cur.
**
** The term returned might by Y=<expr> if there is another constraint in
** the WHERE clause that specifies that X=Y.  Any such constraints will be
** identified by the WO_EQUIV bit in the term->e_operator field.  The
** curs[]/ia_column[] arrays hold X and all its equivalents. There are 11
** slots in curs[]/columns[] so that means we can look for X plus up to 10
** other equivalent values.  Hence a search for X will return <expr> if X=A1
** and A1=A2 and A2=A3 and ... and A9=A10 and A10=<expr>.
**
** If there are multiple terms in the WHERE clause of the form "X <op> <expr>"
** then try for the one with no dependencies on <expr> - in other words where
** <expr> is a constant expression of some kind.  Only return entries of
** the form "X <op> Y" where Y is a column in another table if no terms of
** the form "X <op> <const-expr>" exist.   If no terms with a constant RHS
** exist, try to return a term that does not use WO_EQUIV.
*/
where_term_t *__where_find_term(
  where_clause_t *wc,     /* The WHERE clause to be searched */
  int i_cur,             /* Cursor number of LHS */
  int i_column,          /* column_t number of LHS */
  Bitmask not_ready,     /* RHS must not overlap with this mask */
  u_int32_t op,               /* Mask of WO_xx values describing operator */
  index_t *idx           /* Must be compatible with this index, if not NULL */
){
  where_term_t *result = 0;
  where_term_t *p;
  where_scan_t scan;

  p = __where_scan_init(&scan, wc, i_cur, i_column, op, idx);
  op &= WO_EQ|WO_IS;
  while( p ){
    if( (p->prereq_right & not_ready)==0 ){
      if( p->prereq_right==0 && (p->e_operator&op)!=0 ){
        testcase( p->e_operator & WO_IS );
        return p;
      }
      if( result==0 ) result = p;
    }
    p = __where_scan_next(&scan);
  }
  return result;
}

/*
** This function searches list for an entry that matches the i_col-th column
** of index idx.
**
** If such an expression is found, its index in list->a[] is returned. If
** no expression is found, -1 is returned.
*/
static int __find_index_col(
  parser_t *parser,                  /* parser_t context */
  expr_list_t *list,                /* Expression list to search */
  int i_base,                      /* Cursor for table associated with idx */
  index_t *idx,                    /* index_t to match column of */
  int i_col                        /* column_t of index to match */
){
  int i;
  const char *z_coll = idx->colls[i_col];

  for(i=0; i<list->n_expr; i++){
    expr_t *p = __expr_skip_collate_and_likely(list->a[i].expr);
    if( ALWAYS(p!=0)
     && (p->op==TK_COLUMN || p->op==TK_AGG_COLUMN)
     && p->i_column==idx->columns[i_col]
     && p->i_table==i_base
    ){
      coll_seq_t *coll = __expr_nn_coll_seq(parser, list->a[i].expr);
      if( 0==__str_icmp(coll->name, z_coll) ){
        return i;
      }
    }
  }

  return -1;
}

/*
** Return TRUE if the i_col-th column of index idx is NOT NULL
*/
static int __index_column_not_null(index_t *idx, int i_col){
  int j;
  assert( idx!=0 );
  assert( i_col>=0 && i_col<idx->n_column );
  j = idx->columns[i_col];
  if( j>=0 ){
    return idx->table->cols[j].not_null;
  }else if( j==(-1) ){
    return 1;
  }else{
    assert( j==(-2) );
    return 0;  /* Assume an indexed expression can always yield a NULL */

  }
}

/*
** Return true if the DISTINCT expression-list passed as the third argument
** is redundant.
**
** A DISTINCT list is redundant if any subset of the columns in the
** DISTINCT list are collectively unique and individually non-null.
*/
static int __is_distinct_redundant(
  parser_t *parser,            /* Parsing context */
  src_list_t *tab_list,        /* The FROM clause */
  where_clause_t *wc,         /* The WHERE clause */
  expr_list_t *distinct       /* The result set that needs to be DISTINCT */
){
  table_t *tab;
  index_t *idx;
  int i;                         
  int i_base;

  /* If there is more than one table or sub-select in the FROM clause of
  ** this query, then it will not be possible to show that the DISTINCT
  ** clause is redundant. */
  if( tab_list->n_src!=1 ) return 0;
  i_base = tab_list->a[0].i_cursor;
  tab = tab_list->a[0].s_tab;

  /* If any of the expressions is an IPK column on table i_base, then return
  ** true. Note: The (p->i_table==i_base) part of this test may be false if the
  ** current SELECT is a correlated sub-query.
  */
  for(i=0; i<distinct->n_expr; i++){
    expr_t *p = __expr_skip_collate_and_likely(distinct->a[i].expr);
    if( NEVER(p==0) ) continue;
    if( p->op!=TK_COLUMN && p->op!=TK_AGG_COLUMN ) continue;
    if( p->i_table==i_base && p->i_column<0 ) return 1;
  }

  /* Loop through all indices on the table, checking each to see if it makes
  ** the DISTINCT qualifier redundant. It does so if:
  **
  **   1. The index is itself UNIQUE, and
  **
  **   2. All of the columns in the index are either part of the distinct
  **      list, or else the WHERE clause contains a term of the form "col=X",
  **      where X is a constant value. The collation sequences of the
  **      comparison and select-list expressions must match those of the index.
  **
  **   3. All of those index columns for which the WHERE clause does not
  **      contain a "col=X" term are subject to a NOT NULL constraint.
  */
  for(idx=tab->p_index; idx; idx=idx->next){
    if( !IS_UNIQUE_INDEX(idx) ) continue;
    if( idx->part_idx_where ) continue;
    for(i=0; i<idx->n_key_col; i++){
      if( 0==__where_find_term(wc, i_base, i, ~(Bitmask)0, WO_EQ, idx) ){
        if( __find_index_col(parser, distinct, i_base, idx, i)<0 ) break;
        if( __index_column_not_null(idx, i)==0 ) break;
      }
    }
    if( i==idx->n_key_col ){
      /* This index implies that the DISTINCT qualifier is redundant. */
      return 1;
    }
  }

  return 0;
}


/*
** Estimate the logarithm of the input value to base 2.
*/
static log_est_t __est_log(log_est_t N){
  return N<=10 ? 0 : __log_est(N) - 33;
}

/*
** Convert OP_Column opcodes to OP_Copy in previously generated code.
**
** This routine runs over generated VDBE code and translates OP_Column
** opcodes into OP_Copy when the table is being accessed via co-routine
** instead of via table lookup.
**
** If the i_autoidx_cur is not zero, then any OP_Rowid instructions on
** cursor i_tab_cur are transformed into OP_Sequence opcode for the
** i_autoidx_cur cursor, in order to generate unique rowids for the
** automatic index being generated.
*/
static void __translate_column_to_copy(
  parser_t *parser,      /* Parsing context */
  int i_start,         /* Translate from this opcode to the end */
  int i_tab_cur,        /* OP_Column/OP_Rowid references to this table */
  int i_register,      /* The first column is in this register */
  int i_autoidx_cur     /* If non-zero, cursor of autoindex being generated */
){
  vdbe_t *v = parser->vdbe;
  vdbe_op_t *op = __vdbe_get_op(v, i_start);
  int i_end = __vdbe_current_addr(v);
  if( parser->db->malloc_failed ) return;
#ifdef DBSQL_DEBUG
  if( parser->db->flags & DBSQL_VdbeAddopTrace ){
    printf("CHECKING for column-to-copy on cursor %d for %d..%d\n",
            i_tab_cur, i_start, i_end);
  }
#endif
  for(; i_start<i_end; i_start++, op++){
    if( op->p1!=i_tab_cur ) continue;
    if( op->opcode==OP_Column ){
#ifdef DBSQL_DEBUG
      if( parser->db->flags & DBSQL_VdbeAddopTrace ){
        printf("TRANSLATE OP_Column to OP_Copy at %d\n", i_start);
      }
#endif
      op->opcode = OP_Copy;
      op->p1 = op->p2 + i_register;
      op->p2 = op->p3;
      op->p3 = 0;
      op->p5 = 2;  /* Cause the MEM_Subtype flag to be cleared */
    }else if( op->opcode==OP_Rowid ){
#ifdef DBSQL_DEBUG
      if( parser->db->flags & DBSQL_VdbeAddopTrace ){
        printf("TRANSLATE OP_Rowid to OP_Sequence at %d\n", i_start);
      }
#endif
      op->opcode = OP_Sequence;
      op->p1 = i_autoidx_cur;
#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
      if( i_autoidx_cur==0 ){
        op->opcode = OP_Null;
        op->p3 = 0;
      }
#endif
    }
  }
}

/*
** Two routines for printing the content of an dbsql_index_info_t
** structure.  Used for testing and debugging only.  If neither
** DBSQL_TEST or DBSQL_DEBUG are defined, then these routines
** are no-ops.
*/
#if !defined(DBSQL_OMIT_VIRTUALTABLE) && defined(WHERETRACE_ENABLED)
static void WHERE_TRACE_INDEX_INFO_INPUTS(
  dbsql_index_info_t *p,   /* The __index_info object */
  table_t *tab              /* The TABLE that is the virtual table */
){
  int i;
  if( (__where_trace & 0x10)==0 ) return;
  __debug_printf("dbsql_index_info inputs for %s:\n", tab->name);
  for(i=0; i<p->n_constraint; i++){
    __debug_printf(
       "  constraint[%d]: col=%d termid=%d op=%d usabled=%d collseq=%s\n",
       i,
       p->constraints[i].i_column,
       p->constraints[i].i_term_offset,
       p->constraints[i].op,
       p->constraints[i].usable,
       dbsql_vtab_collation(p,i));
  }
  for(i=0; i<p->n_order_by; i++){
    __debug_printf("  orderby[%d]: col=%d desc=%d\n",
       i,
       p->order_bies[i].i_column,
       p->order_bies[i].desc);
  }
}
static void WHERE_TRACE_INDEX_INFO_OUTPUTS(
  dbsql_index_info_t *p,   /* The __index_info object */
  table_t *tab              /* The TABLE that is the virtual table */
){
  int i;
  if( (__where_trace & 0x10)==0 ) return;
  __debug_printf("dbsql_index_info outputs for %s:\n", tab->name);
  for(i=0; i<p->n_constraint; i++){
    __debug_printf("  usage[%d]: argvIdx=%d omit=%d\n",
       i,
       p->constraint_usages[i].argv_index,
       p->constraint_usages[i].omit);
  }
  __debug_printf("  idxNum=%d\n", p->idx_num);
  __debug_printf("  idxStr=%s\n", p->idx_str);
  __debug_printf("  orderByConsumed=%d\n", p->order_by_consumed);
  __debug_printf("  estimatedCost=%g\n", p->estimated_cost);
  __debug_printf("  estimatedRows=%lld\n", p->estimated_rows);
}
#else
#define WHERE_TRACE_INDEX_INFO_INPUTS(A,B)
#define WHERE_TRACE_INDEX_INFO_OUTPUTS(A,B)
#endif

/*
** We know that src is an operand of an outer join.  Return true if
** term is a constraint that is compatible with that join.
**
** term must be EP_OuterON if src is the right operand of an
** outer join.  term can be either EP_OuterON or EP_InnerON if src
** is the left operand of a RIGHT join.
**
** See https://sqlite.org/forum/forumpost/206d99a16dd9212f
** for an example of a WHERE clause constraints that may not be used on
** the right table of a RIGHT JOIN because the constraint implies a
** not-NULL condition on the left table of the RIGHT JOIN.
*/
static int __constraint_compatible_with_outer_join(
  const where_term_t *term,       /* WHERE clause term to check */
  const src_item_t *src           /* table_t we are trying to access */
){
  assert( (src->fg.jointype&(JT_LEFT|JT_LTORJ|JT_RIGHT))!=0 ); /* By caller */
  testcase( (src->fg.jointype & (JT_LEFT|JT_LTORJ|JT_RIGHT))==JT_LEFT );
  testcase( (src->fg.jointype & (JT_LEFT|JT_LTORJ|JT_RIGHT))==JT_LTORJ );
  testcase( EXPR_HAS_PROPERTY(term->expr, EP_OuterON) )
  testcase( EXPR_HAS_PROPERTY(term->expr, EP_InnerON) );
  if( !EXPR_HAS_PROPERTY(term->expr, EP_OuterON|EP_InnerON)
   || term->expr->w.i_join != src->i_cursor
  ){
    return 0;
  }
  if( (src->fg.jointype & (JT_LEFT|JT_RIGHT))!=0
   && NEVER(EXPR_HAS_PROPERTY(term->expr, EP_InnerON))
  ){
    return 0;
  }
  return 1;
}

#ifndef DBSQL_OMIT_AUTOMATIC_INDEX
/*
** Return true if column i_col of table tab seem like it might be a
** good column to use as part of a query-time index.
**
** Current algorithm (subject to improvement!):
**
**   1.   If i_col is already the left-most column of some other index,
**        then return false.
**
**   2.   If i_col is part of an existing index that has an row_log_ests of
**        more than 20, then return false.
**
**   3.   If no disqualifying conditions above are found, return true.
**
** 2025-01-03: I experimented with a new rule that returns false if the
** the datatype of the column is "BOOLEAN". This did not improve
** performance on any queries at hand, but it did burn CPU cycles, so the
** idea was not committed.
*/
static DBSQL_NOINLINE int __column_is_good_index_candidate(
  const table_t *tab,
  int i_col
){
  const index_t *idx;
  for(idx = tab->p_index; idx!=0; idx=idx->next){
    int j;
    for(j=0; j<idx->n_key_col; j++){
       if( idx->columns[j]==i_col ){
         if( j==0 ) return 0;
         if( idx->has_stat1 && idx->row_log_ests[j+1]>20 ) return 0;
         break;
       }
    }
  }
  return 1;
}
#endif /* DBSQL_OMIT_AUTOMATIC_INDEX */



#ifndef DBSQL_OMIT_AUTOMATIC_INDEX
/*
** Return TRUE if the WHERE clause term term is of a form where it
** could be used with an index to access src, assuming an appropriate
** index existed.
*/
static int __term_can_drive_index(
  const where_term_t *term,        /* WHERE clause term to check */
  const src_item_t *src,           /* table_t we are trying to access */
  const Bitmask not_ready         /* Tables in outer loops of the join */
){
  char aff;
  int left_col;
  
  if( term->left_cursor!=src->i_cursor ) return 0;
  if( (term->e_operator & (WO_EQ|WO_IS))==0 ) return 0;
  assert( (src->fg.jointype & JT_RIGHT)==0 );
  if( (src->fg.jointype & (JT_LEFT|JT_LTORJ|JT_RIGHT))!=0
   && !__constraint_compatible_with_outer_join(term,src)
  ){
    return 0;  /* See https://sqlite.org/forum/forumpost/51e6959f61 */
  }
  if( (term->prereq_right & not_ready)!=0 ) return 0;
  assert( (term->e_operator & (WO_OR|WO_AND))==0 );
  left_col = term->u.x.left_column;
  if( left_col<0 ) return 0;
  aff = src->s_tab->cols[left_col].affinity;
  if( !__index_affinity_ok(term->expr, aff) ) return 0;
  testcase( term->expr->op==TK_IS );
  return __column_is_good_index_candidate(src->s_tab, left_col);
}
#endif


#ifndef DBSQL_OMIT_AUTOMATIC_INDEX

#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
/*
** Argument idx represents an automatic index that the current statement
** will create and populate. Add an OP_Explain with text of the form:
**
**     CREATE AUTOMATIC INDEX ON <table>(<cols>) [WHERE <expr>]
**
** This is only required if dbsql_stmt_scanstatus() is enabled, to
** associate an DBSQL_SCANSTAT_NCYCLE and DBSQL_SCANSTAT_NLOOP
** values with. In order to avoid breaking legacy code and test cases,
** the OP_Explain is not added if this is an EXPLAIN QUERY PLAN command.
*/
static void EXPLAIN_AUTOMATIC_INDEX(
  parser_t *parser,
  index_t *idx,                    /* Automatic index to explain */
  int partial,                   /* True if idx is a partial index */
  int *addr_explain               /* OUT: Address of OP_Explain */
){
  if( IS_STMT_SCANSTATUS(parser->db) && parser->explain!=2 ){
    table_t *tab = idx->table;
    const char *sep = "";
    char *text = 0;
    int ii = 0;
    dbsql_str_t *str = dbsql_str_new(parser->db);
    dbsql_str_appendf(str,"CREATE AUTOMATIC INDEX ON %s(", tab->name);
    assert( idx->n_column>1 );
    assert( idx->columns[idx->n_column-1]==XN_ROWID || !HAS_ROWID(tab) );
    for(ii=0; ii<(idx->n_column-1); ii++){
      const char *name = 0;
      int i_col = idx->columns[ii];

      name = tab->cols[i_col].cn_name;
      dbsql_str_appendf(str, "%s%s", sep, name);
      sep = ", ";
    }
    text = dbsql_str_finish(str);
    if( text==0 ){
      __oom_fault(parser->db);
    }else{
      *addr_explain = __vdbe_explain(
          parser, 0, "%s)%s", text, (partial ? " WHERE <expr>" : "")
      );
      dbsql_free(text);
    }
  }
}
#else
# define EXPLAIN_AUTOMATIC_INDEX(a,b,c,d)
#endif

/*
** Generate code to construct the index_t object for an automatic index
** and to set up the where_level_t object level so that the code generator
** makes use of the automatic index.
*/
static DBSQL_NOINLINE void __construct_automatic_index(
  parser_t *parser,              /* The parsing context */
  where_clause_t *wc,           /* The WHERE clause */
  const Bitmask not_ready,     /* Mask of cursors that are not available */
  where_level_t *level          /* Write new index here */
){
  int n_key_col;                /* Number of columns in the constructed index */
  where_term_t *term;           /* A single term of the WHERE clause */
  where_term_t *wc_end;          /* End of wc->a[] */
  index_t *idx;                /* Object describing the transient index */
  vdbe_t *v;                    /* Prepared statement under construction */
  int addr_init;               /* Address of the initialization bypass jump */
  table_t *table;              /* The table being indexed */
  int addr_top;                /* Top of the index fill loop */
  int reg_record;              /* Register holding an index record */
  int n;                      /* column_t counter */
  int i;                      /* Loop counter */
  int mx_bit_col;               /* Maximum column in src->col_used */
  coll_seq_t *coll;             /* Collating sequence to on a column */
  where_loop_t *loop;           /* The Loop object */
  char *not_used;             /* Extra space on the end of idx */
  Bitmask idx_cols;            /* Bitmap of columns used for indexing */
  Bitmask extra_cols;          /* Bitmap of additional columns */
  u_int8_t sent_warning = 0;         /* True if a warning has been issued */
  u_int8_t use_bloom_filter = 0;      /* True to also add a Bloom filter */
  expr_t *partial = 0;         /* Partial index_t Expression */
  int i_continue = 0;          /* Jump here to skip excluded rows */
  src_list_t *tab_list;          /* The complete FROM clause */
  src_item_t *src;              /* The FROM clause term to get the next index */
  int addr_counter = 0;        /* Address where integer counter is initialized */
  int reg_base;                /* Array of registers where record is assembled */
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
  int addr_exp = 0;            /* Address of OP_Explain */
#endif

  /* Generate code to skip over the creation and initialization of the
  ** transient index on 2nd and subsequent iterations of the loop. */
  v = parser->vdbe;
  assert( v!=0 );
  addr_init = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);

  /* Count the number of columns that will be added to the index
  ** and used to match WHERE clause constraints */
  n_key_col = 0;
  tab_list = wc->w_info->tab_list;
  src = &tab_list->a[level->i_from];
  table = src->s_tab;
  wc_end = &wc->a[wc->n_term];
  loop = level->w_loop;
  idx_cols = 0;
  for(term=wc->a; term<wc_end; term++){
    expr_t *expr = term->expr;
    /* Make the automatic index a partial index if there are terms in the
    ** WHERE clause (or the ON clause of a LEFT join) that constrain which
    ** rows of the target table (src) that can be used. */
    if( (term->wt_flags & TERM_VIRTUAL)==0
     && __expr_is_single_table_constraint(expr, tab_list, level->i_from, 0)
    ){
      partial = __expr_and(parser, partial,
                                __expr_dup(parser->db, expr, 0));
    }
    if( __term_can_drive_index(term, src, not_ready) ){
      int i_col;
      Bitmask c_mask;
      assert( (term->e_operator & (WO_OR|WO_AND))==0 );
      i_col = term->u.x.left_column;
      c_mask = i_col>=BMS ? MASKBIT(BMS-1) : MASKBIT(i_col);
      testcase( i_col==BMS );
      testcase( i_col==BMS-1 );
      if( !sent_warning ){
        dbsql_log(DBSQL_WARNING_AUTOINDEX,
            "automatic index on %s(%s)", table->name,
            table->cols[i_col].cn_name);
        sent_warning = 1;
      }
      if( (idx_cols & c_mask)==0 ){
        if( __where_loop_resize(parser->db, loop, n_key_col+1) ){
          goto end_auto_index_create;
        }
        loop->l_terms[n_key_col++] = term;
        idx_cols |= c_mask;
      }
    }
  }
  assert( n_key_col>0 || parser->db->malloc_failed );
  loop->u.btree.n_eq = loop->n_l_term = n_key_col;
  loop->ws_flags = WHERE_COLUMN_EQ | WHERE_IDX_ONLY | WHERE_INDEXED
                     | WHERE_AUTO_INDEX;

  /* Count the number of additional columns needed to create a
  ** covering index.  A "covering index" is an index that contains all
  ** columns that are needed by the query.  with_t a covering index, the
  ** original table never needs to be accessed.  Automatic indices must
  ** be a covering index because the index will not be updated if the
  ** original table changes and the index and table cannot both be used
  ** if they go out of sync.
  */
  if( IsView(table) ){
    extra_cols = ALLBITS & ~idx_cols;
  }else{
    extra_cols = src->col_used & (~idx_cols | MASKBIT(BMS-1));
  }
  if( !HAS_ROWID(table) ){
    /* For WITHOUT ROWID tables, ensure that all PRIMARY KEY columns are
    ** either in the idx_cols mask or in the extra_cols mask */
    for(i=0; i<table->n_col; i++){
      if( (table->cols[i].col_flags & COLFLAG_PRIMKEY)==0 ) continue;
      if( i>=BMS-1 ){
        extra_cols |= MASKBIT(BMS-1);
        break;
      }
      if( idx_cols & MASKBIT(i) ) continue;
      extra_cols |= MASKBIT(i);
    }
  }
  mx_bit_col = MIN(BMS-1,table->n_col);
  testcase( table->n_col==BMS-1 );
  testcase( table->n_col==BMS-2 );
  for(i=0; i<mx_bit_col; i++){
    if( extra_cols & MASKBIT(i) ) n_key_col++;
  }
  if( src->col_used & MASKBIT(BMS-1) ){
    n_key_col += table->n_col - BMS + 1;
  }

  /* Construct the index_t object to describe this index */
  assert( n_key_col <= table->n_col + MAX(0, table->n_col - BMS + 1) );
  /* ^-- This guarantees that the number of index columns will fit in the u16 */
  idx = __allocate_index_object(parser->db, n_key_col+HAS_ROWID(table),
                                    0, &not_used);
  if( idx==0 ) goto end_auto_index_create;
  loop->u.btree.p_index = idx;
  idx->name = "auto-index";
  idx->table = table;
  n = 0;
  idx_cols = 0;
  for(term=wc->a; term<wc_end; term++){
    if( __term_can_drive_index(term, src, not_ready) ){
      int i_col;
      Bitmask c_mask;
      assert( (term->e_operator & (WO_OR|WO_AND))==0 );
      i_col = term->u.x.left_column;
      c_mask = i_col>=BMS ? MASKBIT(BMS-1) : MASKBIT(i_col);
      testcase( i_col==BMS-1 );
      testcase( i_col==BMS );
      if( (idx_cols & c_mask)==0 ){
        expr_t *x = term->expr;
        idx_cols |= c_mask;
        idx->columns[n] = term->u.x.left_column;
        coll = __expr_compare_coll_seq(parser, x);
        assert( coll!=0 || parser->n_err>0 ); /* TH3 collate01.800 */
        idx->colls[n] = coll ? coll->name : __str_binary;
        n++;
        if( ALWAYS(x->left!=0)
         && __expr_affinity(x->left)!=DBSQL_AFF_TEXT
        ){
          /* TUNING: only use a Bloom filter on an automatic index
          ** if one or more key columns has the ability to hold numeric
          ** values, since strings all have the same hash in the Bloom
          ** filter implementation and hence a Bloom filter on a text column
          ** is not usually helpful. */
          use_bloom_filter = 1;
        }
      }
    }
  }
  assert( (u_int32_t)n==loop->u.btree.n_eq );

  /* Add additional columns needed to make the automatic index into
  ** a covering index */
  for(i=0; i<mx_bit_col; i++){
    if( extra_cols & MASKBIT(i) ){
      idx->columns[n] = i;
      idx->colls[n] = __str_binary;
      n++;
    }
  }
  if( src->col_used & MASKBIT(BMS-1) ){
    for(i=BMS-1; i<table->n_col; i++){
      idx->columns[n] = i;
      idx->colls[n] = __str_binary;
      n++;
    }
  }
  assert( n==n_key_col );
  if( HAS_ROWID(table) ){
    idx->columns[n] = XN_ROWID;
    idx->colls[n] = __str_binary;
  }

  /* Create the automatic index */
  EXPLAIN_AUTOMATIC_INDEX(parser, idx, partial!=0, &addr_exp);
  assert( level->i_idx_cur>=0 );
  level->i_idx_cur = parser->n_tab++;
  __vdbe_add_op2(v, OP_OpenAutoindex, level->i_idx_cur, n_key_col+1);
  __vdbe_set_p4_key_info(parser, idx);
  VDBE_COMMENT((v, "for %s", table->name));
  if( OPTIMIZATION_ENABLED(parser->db, DBSQL_BloomFilter) && use_bloom_filter ){
    __where_explain_bloom_filter(parser, wc->w_info, level);
    level->reg_filter = ++parser->n_mem;
    __vdbe_add_op2(v, OP_Blob, 10000, level->reg_filter);
  }

  /* Fill the automatic index with content */
  assert( src == &wc->w_info->tab_list->a[level->i_from] );
  if( src->fg.via_coroutine ){
    int reg_yield;
    subquery_t *subq;
    assert( src->fg.is_subquery );
    subq = src->u4.subq;
    assert( subq!=0 );
    reg_yield = subq->reg_return;
    addr_counter = __vdbe_add_op2(v, OP_Integer, 0, 0);
    __vdbe_add_op3(v, OP_InitCoroutine, reg_yield, 0, subq->addr_fill_sub);
    addr_top =  __vdbe_add_op1(v, OP_Yield, reg_yield);
    VDBE_COVERAGE(v);
    VDBE_COMMENT((v, "next row of %s", src->s_tab->name));
  }else{
    assert( level->addr_halt );
    addr_top = __vdbe_add_op2(v, OP_Rewind,level->i_tab_cur,level->addr_halt);
    VDBE_COVERAGE(v);
  }
  if( partial ){
    i_continue = __vdbe_make_label(parser);
    __expr_if_false(parser, partial, i_continue, DBSQL_JUMPIFNULL);
    loop->ws_flags |= WHERE_PARTIALIDX;
  }
  reg_record = __get_temp_reg(parser);
  reg_base = __generate_index_key(
      parser, idx, level->i_tab_cur, reg_record, 0, 0, 0, 0
  );
  if( level->reg_filter ){
    __vdbe_add_op4_int(v, OP_FilterAdd, level->reg_filter, 0,
                         reg_base, loop->u.btree.n_eq);
  }
  __vdbe_scan_status_counters(v, addr_exp, addr_exp, __vdbe_current_addr(v));
  __vdbe_add_op2(v, OP_IdxInsert, level->i_idx_cur, reg_record);
  __vdbe_change_p5(v, OPFLAG_USESEEKRESULT);
  if( partial ) __vdbe_resolve_label(v, i_continue);
  if( src->fg.via_coroutine ){
    assert( src->fg.is_subquery && src->u4.subq!=0 );
    __vdbe_change_p2(v, addr_counter, reg_base+n);
    testcase( parser->db->malloc_failed );
    assert( level->i_idx_cur>0 );
    __translate_column_to_copy(parser, addr_top, level->i_tab_cur,
                          src->u4.subq->reg_result, level->i_idx_cur);
    __vdbe_goto(v, addr_top);
    src->fg.via_coroutine = 0;
    __vdbe_jump_here(v, addr_top);
  }else{
    __vdbe_add_op2(v, OP_Next, level->i_tab_cur, addr_top+1); VDBE_COVERAGE(v);
    __vdbe_change_p5(v, DBSQL_STMTSTATUS_AUTOINDEX);
    if( (src->fg.jointype & JT_LEFT)!=0 ){
      __vdbe_jump_here(v, addr_top);
    }
  }
  __release_temp_reg(parser, reg_record);
 
  /* Jump here when skipping the initialization */
  __vdbe_jump_here(v, addr_init);
  __vdbe_scan_status_range(v, addr_exp, addr_exp, -1);

end_auto_index_create:
  __expr_delete(parser->db, partial);
}
#endif /* DBSQL_OMIT_AUTOMATIC_INDEX */

/*
** Generate bytecode that will initialize a Bloom filter that is appropriate
** for level.
**
** If there are inner loops within level that have the WHERE_BLOOMFILTER
** flag set, initialize a Bloomfilter for them as well.  Except don't do
** this recursive initialization if the DBSQL_BloomPulldown optimization has
** been turned off.
**
** When the Bloom filter is initialized, the WHERE_BLOOMFILTER flag is cleared
** from the loop, but the reg_filter value is set to a register that implements
** the Bloom filter.  When reg_filter is positive, the
** __where_code_one_loop_start() will generate code to test the Bloom filter
** and skip the subsequence B-Tree seek if the Bloom filter indicates that
** no matching rows exist.
**
** This routine may only be called if it has previously been determined that
** the loop would benefit from a Bloom filter, and the WHERE_BLOOMFILTER bit
** is set.
*/
static DBSQL_NOINLINE void __construct_bloom_filter(
  where_info_t *w_info,    /* The WHERE clause */
  int i_level,           /* index_t in w_info->a[] that is level */
  where_level_t *level,   /* Make a Bloom filter for this FROM term */
  Bitmask not_ready      /* Loops that are not ready */
){
  int addr_once;                        /* Address of opening OP_Once */
  int addr_top;                         /* Address of OP_Rewind */
  int addr_cont;                        /* Jump here to skip a row */
  const where_term_t *term;              /* For looping over WHERE clause terms */
  const where_term_t *wc_end;             /* Last WHERE clause term */
  parser_t *parser = w_info->parser;      /* Parsing context */
  vdbe_t *v = parser->vdbe;             /* VDBE under construction */
  where_loop_t *loop = level->w_loop;   /* The loop being coded */
  int i_cur;                            /* Cursor for table getting the filter */
  indexed_expr_t *saved_p_idx_epr;          /* saved copy of parser_t.idx_epr */
  indexed_expr_t *saved_p_idx_part_expr;     /* saved copy of parser_t.idx_part_expr */

  saved_p_idx_epr = parser->idx_epr;
  saved_p_idx_part_expr = parser->idx_part_expr;
  parser->idx_epr = 0;
  parser->idx_part_expr = 0;

  assert( loop!=0 );
  assert( v!=0 );
  assert( loop->ws_flags & WHERE_BLOOMFILTER );
  assert( (loop->ws_flags & WHERE_IDX_ONLY)==0 );

  addr_once = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
  do{
    const src_list_t *tab_list;
    const src_item_t *item;
    const table_t *tab;
    u_int64_t sz;
    int i_src;
    __where_explain_bloom_filter(parser, w_info, level);
    addr_cont = __vdbe_make_label(parser);
    i_cur = level->i_tab_cur;
    level->reg_filter = ++parser->n_mem;

    /* The Bloom filter is a Blob held in a register.  Initialize it
    ** to zero-filled blob of at least 80K bits, but maybe more if the
    ** estimated size of the table is larger.  We could actually
    ** measure the size of the table at run-time using OP_Count with
    ** P3==1 and use that value to initialize the blob.  But that makes
    ** testing complicated.  By basing the blob size on the value in the
    ** dbsql_stat1 table, testing is much easier.
    */
    tab_list = w_info->tab_list;
    i_src = level->i_from;
    item = &tab_list->a[i_src];
    assert( item!=0 );
    tab = item->s_tab;
    assert( tab!=0 );
    sz = __log_est_to_int(tab->n_row_log_est);
    if( sz<10000 ){
      sz = 10000;
    }else if( sz>10000000 ){
      sz = 10000000;
    }
    __vdbe_add_op2(v, OP_Blob, (int)sz, level->reg_filter);

    addr_top = __vdbe_add_op1(v, OP_Rewind, i_cur); VDBE_COVERAGE(v);
    wc_end = &w_info->s_wc.a[w_info->s_wc.n_term];
    for(term=w_info->s_wc.a; term<wc_end; term++){
      expr_t *expr = term->expr;
      if( (term->wt_flags & TERM_VIRTUAL)==0
       && __expr_is_single_table_constraint(expr, tab_list, i_src, 0)
      ){
        __expr_if_false(parser, term->expr, addr_cont, DBSQL_JUMPIFNULL);
      }
    }
    if( loop->ws_flags & WHERE_IPK ){
      int r1 = __get_temp_reg(parser);
      __vdbe_add_op2(v, OP_Rowid, i_cur, r1);
      __vdbe_add_op4_int(v, OP_FilterAdd, level->reg_filter, 0, r1, 1);
      __release_temp_reg(parser, r1);
    }else{
      index_t *idx = loop->u.btree.p_index;
      int n = loop->u.btree.n_eq;
      int r1 = __get_temp_range(parser, n);
      int jj;
      for(jj=0; jj<n; jj++){
        assert( idx->table==item->s_tab );
        __expr_code_load_index_column(parser, idx, i_cur, jj, r1+jj);
      }
      __vdbe_add_op4_int(v, OP_FilterAdd, level->reg_filter, 0, r1, n);
      __release_temp_range(parser, r1, n);
    }
    __vdbe_resolve_label(v, addr_cont);
    __vdbe_add_op2(v, OP_Next, level->i_tab_cur, addr_top+1);
    VDBE_COVERAGE(v);
    __vdbe_jump_here(v, addr_top);
    loop->ws_flags &= ~WHERE_BLOOMFILTER;
    if( OPTIMIZATION_DISABLED(parser->db, DBSQL_BloomPulldown) ) break;
    while( ++i_level < w_info->n_level ){
      const src_item_t *tab_item;
      level = &w_info->a[i_level];
      tab_item = &w_info->tab_list->a[level->i_from];
      if( tab_item->fg.jointype & (JT_LEFT|JT_LTORJ) ) continue;
      loop = level->w_loop;
      if( NEVER(loop==0) ) continue;
      if( loop->prereq & not_ready ) continue;
      if( (loop->ws_flags & (WHERE_BLOOMFILTER|WHERE_COLUMN_IN))
                 ==WHERE_BLOOMFILTER
      ){
        /* This is a candidate for bloom-filter pull-down (early evaluation).
        ** The test that WHERE_COLUMN_IN is omitted is important, as we are
        ** not able to do early evaluation of bloom filters that make use of
        ** the IN operator */
        break;
      }
    }
  }while( i_level < w_info->n_level );
  __vdbe_jump_here(v, addr_once);
  parser->idx_epr = saved_p_idx_epr;
  parser->idx_part_expr = saved_p_idx_part_expr;
}


#ifndef DBSQL_OMIT_VIRTUALTABLE
/*
** Return term i_term of the where_clause_t passed as the first argument. Terms
** are numbered from 0 upwards, starting with the terms in wc->a[], then
** those in wc->outer->a[] (if any), and so on.
*/
static where_term_t *__term_from_where_clause(where_clause_t *wc, int i_term){
  where_clause_t *p;
  for(p=wc; p; p=p->outer){
    if( i_term<p->n_term ) return &p->a[i_term];
    i_term -= p->n_term;
  }
  return 0;
}

/*
** Allocate and populate an dbsql_index_info_t structure. It is the
** responsibility of the caller to eventually release the structure
** by passing the pointer returned by this function to __free_index_info().
*/
static dbsql_index_info_t *__allocate_index_info(
  where_info_t *w_info,              /* The WHERE clause */
  where_clause_t *wc,               /* The WHERE clause being analyzed */
  Bitmask m_unusable,              /* Ignore terms with these prereqs */
  src_item_t *src,                  /* The FROM clause term that is the vtab */
  u_int16_t *pm_no_omit                   /* Mask of terms not to omit */
){
  int i, j;
  int n_term;
  parser_t *parser = w_info->parser;
  struct dbsql_index_constraint_t *idx_cons;
  struct dbsql_index_orderby_t *idx_order_by;
  struct dbsql_index_constraint_usage_t *usage;
  struct hidden_index_info_t *hidden;
  where_term_t *term;
  int n_order_by;
  dbsql_index_info_t *idx_info;
  u_int16_t m_no_omit = 0;
  const table_t *tab;
  int distinct = 0;
  expr_list_t *order_by = w_info->order_by;
  where_clause_t *p;

  assert( src!=0 );
  tab = src->s_tab;
  assert( tab!=0 );
  assert( IS_VIRTUAL(tab) );

  /* Find all WHERE clause constraints referring to this virtual table.
  ** Mark each term with the TERM_OK flag.  Set n_term to the number of
  ** terms found.
  */
  for(p=wc, n_term=0; p; p=p->outer){
    for(i=0, term=p->a; i<p->n_term; i++, term++){
      term->wt_flags &= ~TERM_OK;
      if( term->left_cursor != src->i_cursor ) continue;
      if( term->prereq_right & m_unusable ) continue;
      assert( IS_POWER_OF_TWO(term->e_operator & ~WO_EQUIV) );
      testcase( term->e_operator & WO_IN );
      testcase( term->e_operator & WO_ISNULL );
      testcase( term->e_operator & WO_IS );
      testcase( term->e_operator & WO_ALL );
      if( (term->e_operator & ~(WO_EQUIV))==0 ) continue;
      if( term->wt_flags & TERM_VNULL ) continue;

      assert( (term->e_operator & (WO_OR|WO_AND))==0 );
      assert( term->u.x.left_column>=XN_ROWID );
      assert( term->u.x.left_column<tab->n_col );
      if( (src->fg.jointype & (JT_LEFT|JT_LTORJ|JT_RIGHT))!=0
          && !__constraint_compatible_with_outer_join(term,src)
        ){
        continue;
      }
      n_term++;
      term->wt_flags |= TERM_OK;
    }
  }

  /* If the ORDER BY clause contains only columns in the current
  ** virtual table then allocate space for the order_bies part of
  ** the dbsql_index_info_t structure.
  */
  n_order_by = 0;
  if( order_by ){
    int n = order_by->n_expr;
    for(i=0; i<n; i++){
      expr_t *expr = order_by->a[i].expr;
      expr_t *e2;

      /* Skip over constant terms in the ORDER BY clause */
      if( __expr_is_constant(0, expr) ){
        continue;
      }

      /* Virtual tables are unable to deal with NULLS FIRST */
      if( order_by->a[i].fg.sort_flags & KEYINFO_ORDER_BIGNULL ) break;

      /* First case - a direct column references without a COLLATE operator */
      if( expr->op==TK_COLUMN && expr->i_table==src->i_cursor ){
        assert( expr->i_column>=XN_ROWID && expr->i_column<tab->n_col );
        continue;
      }

      /* 2nd case - a column reference with a COLLATE operator.  Only match
      ** of the COLLATE operator matches the collation of the column. */
      if( expr->op==TK_COLLATE
       && (e2 = expr->left)->op==TK_COLUMN
       && e2->i_table==src->i_cursor
      ){
        const char *coll;  /* The collating sequence name */
        assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
        assert( expr->u.token!=0 );
        assert( e2->i_column>=XN_ROWID && e2->i_column<tab->n_col );
        expr->i_column = e2->i_column;
        if( e2->i_column<0 ) continue;  /* Collseq does not matter for rowid */
        coll = __column_coll(&tab->cols[e2->i_column]);
        if( coll==0 ) coll = __str_binary;
        if( dbsql_stricmp(expr->u.token, coll)==0 ) continue;
      }

      /* No matches cause a break out of the loop */
      break;
    }
    if( i==n ){
      int sort_by_group = (w_info->wctrl_flags & WHERE_SORTBYGROUP)!=0;
      n_order_by = n;
      if( (w_info->wctrl_flags & WHERE_DISTINCTBY) && !src->fg.rowid_used ){
        distinct = 2 + sort_by_group;
      }else if( w_info->wctrl_flags & WHERE_GROUPBY ){
        distinct = 1 - sort_by_group;
      }else if( w_info->wctrl_flags & WHERE_WANT_DISTINCT ){
        distinct = 3;
      }
    }
  }

  /* Allocate the dbsql_index_info_t structure
  */
  idx_info = __db_malloc_zero(parser->db, sizeof(*idx_info)
                           + (sizeof(*idx_cons) + sizeof(*usage))*n_term
                           + sizeof(*idx_order_by)*n_order_by
                           + SZ_HIDDENINDEXINFO(n_term) );
  if( idx_info==0 ){
    __error_msg(parser, "out of memory");
    return 0;
  }
  hidden = (struct hidden_index_info_t*)&idx_info[1];
  idx_cons = (struct dbsql_index_constraint_t*)&hidden->rhses[n_term];
  idx_order_by = (struct dbsql_index_orderby_t*)&idx_cons[n_term];
  usage = (struct dbsql_index_constraint_usage_t*)&idx_order_by[n_order_by];
  idx_info->constraints = idx_cons;
  idx_info->order_bies = idx_order_by;
  idx_info->constraint_usages = usage;
  idx_info->col_used = (int64_t)src->col_used;
  if( HAS_ROWID(tab)==0 ){
    /* Ensure that all bits associated with PK columns are set. This is to
    ** ensure they are available for cases like RIGHT joins or OR loops. */
    index_t *pk = __primary_key_index((table_t*)tab);
    assert( pk!=0 );
    for(i=0; i<pk->n_key_col; i++){
      int i_col = pk->columns[i];
      assert( i_col>=0 );
      if( i_col>=BMS-1 ) i_col = BMS-1;
      idx_info->col_used |= MASKBIT(i_col);
    }
  }
  hidden->wc = wc;
  hidden->parser = parser;
  hidden->distinct = distinct;
  hidden->m_in = 0;
  for(p=wc, i=j=0; p; p=p->outer){
    int n_last = i+p->n_term;;
    for(term=p->a; i<n_last; i++, term++){
      u_int16_t op;
      if( (term->wt_flags & TERM_OK)==0 ) continue;
      idx_cons[j].i_column = term->u.x.left_column;
      idx_cons[j].i_term_offset = i;
      op = term->e_operator & WO_ALL;
      if( op==WO_IN ){
        if( (term->wt_flags & TERM_SLICE)==0 ){
          hidden->m_in |= SMASKBIT32(j);
        }
        op = WO_EQ;
      }
      if( op==WO_AUX ){
        idx_cons[j].op = term->match_op;
      }else if( op & (WO_ISNULL|WO_IS) ){
        if( op==WO_ISNULL ){
          idx_cons[j].op = DBSQL_INDEX_CONSTRAINT_ISNULL;
        }else{
          idx_cons[j].op = DBSQL_INDEX_CONSTRAINT_IS;
        }
      }else{
        idx_cons[j].op = (u_int8_t)op;
        /* The direct assignment in the previous line is possible only because
        ** the WO_ and DBSQL_INDEX_CONSTRAINT_ codes are identical.  The
        ** following asserts verify this fact. */
        assert( WO_EQ==DBSQL_INDEX_CONSTRAINT_EQ );
        assert( WO_LT==DBSQL_INDEX_CONSTRAINT_LT );
        assert( WO_LE==DBSQL_INDEX_CONSTRAINT_LE );
        assert( WO_GT==DBSQL_INDEX_CONSTRAINT_GT );
        assert( WO_GE==DBSQL_INDEX_CONSTRAINT_GE );
        assert( term->e_operator&(WO_IN|WO_EQ|WO_LT|WO_LE|WO_GT|WO_GE|WO_AUX) );

        if( op & (WO_LT|WO_LE|WO_GT|WO_GE)
            && __expr_is_vector(term->expr->right)
          ){
          testcase( j!=i );
          if( j<16 ) m_no_omit |= (1 << j);
          if( op==WO_LT ) idx_cons[j].op = WO_LE;
          if( op==WO_GT ) idx_cons[j].op = WO_GE;
        }
      }

      j++;
    }
  }
  assert( j==n_term );
  idx_info->n_constraint = j;
  for(i=j=0; i<n_order_by; i++){
    expr_t *expr = order_by->a[i].expr;
    if( __expr_is_constant(0, expr) ) continue;
    assert( expr->op==TK_COLUMN
         || (expr->op==TK_COLLATE && expr->left->op==TK_COLUMN
              && expr->i_column==expr->left->i_column) );
    idx_order_by[j].i_column = expr->i_column;
    idx_order_by[j].desc = order_by->a[i].fg.sort_flags & KEYINFO_ORDER_DESC;
    j++;
  }
  idx_info->n_order_by = j;

  *pm_no_omit = m_no_omit;
  return idx_info;
}

/*
** Free and zero the dbsql_index_info_t.idx_str value if needed.
*/
static void __free_idx_str(dbsql_index_info_t *idx_info){
  if( idx_info->need_to_free_idx_str ){
    dbsql_free(idx_info->idx_str);
    idx_info->idx_str = 0;
    idx_info->need_to_free_idx_str = 0;
  }
}  

/*
** Free an dbsql_index_info_t structure allocated by __allocate_index_info()
** and possibly modified by best_index methods.
*/
static void __free_index_info(DBSQL *db, dbsql_index_info_t *idx_info){
  hidden_index_info_t *hidden;
  int i;
  assert( idx_info!=0 );
  hidden = (hidden_index_info_t*)&idx_info[1];
  assert( hidden->parser!=0 );
  assert( hidden->parser->db==db );
  for(i=0; i<idx_info->n_constraint; i++){
    __value_free(hidden->rhses[i]); /* IMP: R-14553-25174 */
    hidden->rhses[i] = 0;
  }
  __free_idx_str(idx_info);
  __dbsql_db_free(db, idx_info);
}

/*
** The table object reference passed as the second argument to this function
** must represent a virtual table. This function invokes the best_index()
** method of the virtual table with the dbsql_index_info_t object that
** comes in as the 3rd argument to this function.
**
** If an error occurs, parser is populated with an error message and an
** appropriate error code is returned.  A return of DBSQL_CONSTRAINT from
** best_index is not considered an error.  DBSQL_CONSTRAINT indicates that
** the current configuration of "unusable" flags in dbsql_index_info_t can
** not result in a valid plan.
**
** Whether or not an error is returned, it is the responsibility of the
** caller to eventually free p->idx_str if p->need_to_free_idx_str indicates
** that this is required.
*/
static int __vtab_best_index(parser_t *parser, table_t *tab, dbsql_index_info_t *p){
  int rc;
  dbsql_vtab_t *vtab;

  assert( IS_VIRTUAL(tab) );
  vtab = __get_v_table(parser->db, tab)->vtab;
  WHERE_TRACE_INDEX_INFO_INPUTS(p, tab);
  parser->db->n_schema_lock++;
  rc = vtab->module->best_index(vtab, p);
  parser->db->n_schema_lock--;
  WHERE_TRACE_INDEX_INFO_OUTPUTS(p, tab);

  if( rc!=DBSQL_OK && rc!=DBSQL_CONSTRAINT ){
    if( rc==DBSQL_NOMEM ){
      __oom_fault(parser->db);
    }else if( !vtab->errmsg ){
      __error_msg(parser, "%s", __err_str(rc));
    }else{
      __error_msg(parser, "%s", vtab->errmsg);
    }
  }
  if( tab->u.vtab.p->all_schemas ){
    __vtab_uses_all_schemas(parser);
  }
  dbsql_free(vtab->errmsg);
  vtab->errmsg = 0;
  return rc;
}
#endif /* !defined(DBSQL_OMIT_VIRTUALTABLE) */

#ifdef DBSQL_ENABLE_STAT4
/*
** Estimate the location of a particular key among all keys in an
** index.  Store the results in stats as follows:
**
**    stats[0]      Est. number of rows less than rec
**    stats[1]      Est. number of rows equal to rec
**
** Return the index of the sample that is the smallest sample that
** is greater than or equal to rec. Note that this index is not an index
** into the samples[] array - it is an index into a virtual set of samples
** based on the contents of samples[] and the number of fields in record
** rec.
*/
static int __where_key_stats(
  parser_t *parser,              /* Database connection */
  index_t *idx,                /* index_t to consider domain of */
  unpacked_record_t *rec,       /* Vector of values to consider */
  int round_up,                /* Round up if true.  Round down if false */
  rowcnt_t *stats              /* OUT: stats written here */
){
  index_sample_t *samples = idx->samples;
  int i_col;                   /* index_t of required stats in an_eq[] etc. */
  int i;                      /* index_t of first sample >= rec */
  int i_sample;                /* Smallest sample larger than or equal to rec */
  int i_min = 0;               /* Smallest sample not yet tested */
  int i_test;                  /* Next sample to test */
  int res;                    /* Result of comparison operation */
  int n_field;                 /* Number of fields in rec */
  rowcnt_t i_lower = 0;         /* an_lt[] + an_eq[] of largest sample rec is > */

#ifndef DBSQL_DEBUG
  UNUSED_PARAMETER( parser );
#endif
  assert( rec!=0 );
  assert( idx->n_sample>0 );
  assert( rec->n_field>0 );


  /* Do a binary search to find the first sample greater than or equal
  ** to rec. If rec contains a single field, the set of samples to search
  ** is simply the samples[] array. If the samples in samples[] contain more
  ** than one fields, all fields following the first are ignored.
  **
  ** If rec contains N fields, where N is more than one, then as well as the
  ** samples in samples[] (truncated to N fields), the search also has to
  ** consider prefixes of those samples. For example, if the set of samples
  ** in samples is:
  **
  **     samples[0] = (a, 5)
  **     samples[1] = (a, 10)
  **     samples[2] = (b, 5)
  **     samples[3] = (c, 100)
  **     samples[4] = (c, 105)
  **
  ** Then the search space should ideally be the samples above and the
  ** unique prefixes [a], [b] and [c]. But since that is hard to organize,
  ** the code actually searches this set:
  **
  **     0: (a)
  **     1: (a, 5)
  **     2: (a, 10)
  **     3: (a, 10)
  **     4: (b)
  **     5: (b, 5)
  **     6: (c)
  **     7: (c, 100)
  **     8: (c, 105)
  **     9: (c, 105)
  **
  ** For each sample in the samples[] array, N samples are present in the
  ** effective sample array. In the above, samples 0 and 1 are based on
  ** sample samples[0]. Samples 2 and 3 on samples[1] etc.
  **
  ** Often, sample i of each block of N effective samples has (i+1) fields.
  ** Except, each sample may be extended to ensure that it is greater than or
  ** equal to the previous sample in the array. For example, in the above,
  ** sample 2 is the first sample of a block of N samples, so at first it
  ** appears that it should be 1 field in size. However, that would make it
  ** smaller than sample 1, so the binary search would not work. As a result,
  ** it is extended to two fields. The duplicates that this creates do not
  ** cause any problems.
  */
  if( !HAS_ROWID(idx->table) && IS_PRIMARY_KEY_INDEX(idx) ){
    n_field = idx->n_key_col;
  }else{
    n_field = idx->n_column;
  }
  n_field = MIN(rec->n_field, n_field);
  i_col = 0;
  i_sample = idx->n_sample * n_field;
  do{
    int i_samp;                    /* index_t in samples[] of test sample */
    int n;                        /* Number of fields in test sample */

    i_test = (i_min+i_sample)/2;
    i_samp = i_test / n_field;
    if( i_samp>0 ){
      /* The proposed effective sample is a prefix of sample samples[i_samp].
      ** Specifically, the shortest prefix of at least (1 + i_test%n_field)
      ** fields that is greater than the previous effective sample.  */
      for(n=(i_test % n_field) + 1; n<n_field; n++){
        if( samples[i_samp-1].an_lt[n-1]!=samples[i_samp].an_lt[n-1] ) break;
      }
    }else{
      n = i_test + 1;
    }

    rec->n_field = n;
    res = __vdbe_record_compare(samples[i_samp].n, samples[i_samp].p, rec);
    if( res<0 ){
      i_lower = samples[i_samp].an_lt[n-1] + samples[i_samp].an_eq[n-1];
      i_min = i_test+1;
    }else if( res==0 && n<n_field ){
      i_lower = samples[i_samp].an_lt[n-1];
      i_min = i_test+1;
      res = -1;
    }else{
      i_sample = i_test;
      i_col = n-1;
    }
  }while( res && i_min<i_sample );
  i = i_sample / n_field;

#ifdef DBSQL_DEBUG
  /* The following assert statements check that the binary search code
  ** above found the right answer. This block serves no purpose other
  ** than to invoke the asserts.  */
  if( parser->db->malloc_failed==0 ){
    if( res==0 ){
      /* If (res==0) is true, then rec must be equal to sample i. */
      assert( i<idx->n_sample );
      assert( i_col==n_field-1 );
      rec->n_field = n_field;
      assert( 0==__vdbe_record_compare(samples[i].n, samples[i].p, rec)
           || parser->db->malloc_failed
      );
    }else{
      /* Unless i==idx->n_sample, indicating that rec is larger than
      ** all samples in the samples[] array, rec must be smaller than the
      ** (i_col+1) field prefix of sample i.  */
      assert( i<=idx->n_sample && i>=0 );
      rec->n_field = i_col+1;
      assert( i==idx->n_sample
           || __vdbe_record_compare(samples[i].n, samples[i].p, rec)>0
           || parser->db->malloc_failed );

      /* if i==0 and i_col==0, then record rec is smaller than all samples
      ** in the samples[] array. Otherwise, if (i_col>0) then rec must
      ** be greater than or equal to the (i_col) field prefix of sample i.
      ** If (i>0), then rec must also be greater than sample (i-1).  */
      if( i_col>0 ){
        rec->n_field = i_col;
        assert( __vdbe_record_compare(samples[i].n, samples[i].p, rec)<=0
             || parser->db->malloc_failed || CORRUPT_DB );
      }
      if( i>0 ){
        rec->n_field = n_field;
        assert( __vdbe_record_compare(samples[i-1].n, samples[i-1].p, rec)<0
             || parser->db->malloc_failed || CORRUPT_DB );
      }
    }
  }
#endif /* ifdef DBSQL_DEBUG */

  if( res==0 ){
    /* Record rec is equal to sample i */
    assert( i_col==n_field-1 );
    stats[0] = samples[i].an_lt[i_col];
    stats[1] = samples[i].an_eq[i_col];
  }else{
    /* At this point, the (i_col+1) field prefix of samples[i] is the first
    ** sample that is greater than rec. Or, if i==idx->n_sample then rec
    ** is larger than all samples in the array. */
    rowcnt_t i_upper, i_gap;
    if( i>=idx->n_sample ){
      i_upper = idx->n_row_est0;
    }else{
      i_upper = samples[i].an_lt[i_col];
    }

    if( i_lower>=i_upper ){
      i_gap = 0;
    }else{
      i_gap = i_upper - i_lower;
    }
    if( round_up ){
      i_gap = (i_gap*2)/3;
    }else{
      i_gap = i_gap/3;
    }
    stats[0] = i_lower + i_gap;
    stats[1] = idx->avg_eqs[n_field-1];
  }

  /* Restore the rec->n_field value before returning.  */
  rec->n_field = n_field;
  return i;
}
#endif /* DBSQL_ENABLE_STAT4 */

/*
** If it is not NULL, term is a term that provides an upper or lower
** bound on a range scan. Without considering term, it is estimated
** that the scan will visit n_new rows. This function returns the number
** estimated to be visited after taking term into account.
**
** If the user explicitly specified a likelihood() value for this term,
** then the return value is the likelihood multiplied by the number of
** input rows. Otherwise, this function assumes that an "IS NOT NULL" term
** has a likelihood of 0.50, and any other term a likelihood of 0.25.
*/
static log_est_t __where_range_adjust(where_term_t *term, log_est_t n_new){
  log_est_t n_ret = n_new;
  if( term ){
    if( term->truth_prob<=0 ){
      n_ret += term->truth_prob;
    }else if( (term->wt_flags & TERM_VNULL)==0 ){
      n_ret -= 20;        assert( 20==__log_est(4) );
    }
  }
  return n_ret;
}


#ifdef DBSQL_ENABLE_STAT4
/*
** Return the affinity for a single column of an index.
*/
char __index_column_affinity(DBSQL *db, index_t *idx, int i_col){
  assert( i_col>=0 && i_col<idx->n_column );
  if( !idx->col_aff ){
    if( __index_affinity_str(db, idx)==0 ) return DBSQL_AFF_BLOB;
  }
  assert( idx->col_aff[i_col]!=0 );
  return idx->col_aff[i_col];
}
#endif


#ifdef DBSQL_ENABLE_STAT4
/*
** This function is called to estimate the number of rows visited by a
** range-scan on a skip-scan index. For example:
**
**   CREATE INDEX i1 ON t1(a, b, c);
**   SELECT * FROM t1 WHERE a=? AND c BETWEEN ? AND ?;
**
** Value loop->n_out is currently set to the estimated number of rows
** visited for scanning (a=? AND b=?). This function reduces that estimate
** by some factor to account for the (c BETWEEN ? AND ?) expression based
** on the stat4 data for the index. this scan will be performed multiple
** times (once for each (a,b) combination that matches a=?) is dealt with
** by the caller.
**
** It does this by scanning through all stat4 samples, comparing values
** extracted from lower and upper with the corresponding column in each
** sample. If L and U are the number of samples found to be less than or
** equal to the values extracted from lower and upper respectively, and
** N is the total number of samples, the loop->n_out value is adjusted
** as follows:
**
**   n_out = n_out * ( min(U - L, 1) / N )
**
** If lower is NULL, or a value cannot be extracted from the term, L is
** set to zero. If upper is NULL, or a value cannot be extracted from it,
** U is set to N.
**
** Normally, this function sets *pb_done to 1 before returning. However,
** if no value can be extracted from either lower or upper (and so the
** estimate of the number of rows delivered remains unchanged), *pb_done
** is left as is.
**
** If an error occurs, an dbsql error code is returned. Otherwise,
** DBSQL_OK.
*/
static int __where_range_skip_scan_est(
  parser_t *parser,       /* Parsing & code generating context */
  where_term_t *lower,   /* Lower bound on the range. ex: "x>123" Might be NULL */
  where_term_t *upper,   /* Upper bound on the range. ex: "x<455" Might be NULL */
  where_loop_t *loop,    /* Update the .n_out value of this loop */
  int *pb_done          /* Set to true if at least one expr. value extracted */
){
  index_t *p = loop->u.btree.p_index;
  int n_eq = loop->u.btree.n_eq;
  DBSQL *db = parser->db;
  int n_lower = -1;
  int n_upper = p->n_sample+1;
  int rc = DBSQL_OK;
  u_int8_t aff = __index_column_affinity(db, p, n_eq);
  coll_seq_t *coll;
 
  dbsql_value_t *p1 = 0;          /* Value extracted from lower */
  dbsql_value_t *p2 = 0;          /* Value extracted from upper */
  dbsql_value_t *val = 0;        /* Value extracted from record */

  coll = __locate_coll_seq(parser, p->colls[n_eq]);
  if( lower ){
    rc = __stat4_value_from_expr(parser, lower->expr->right, aff, &p1);
    n_lower = 0;
  }
  if( upper && rc==DBSQL_OK ){
    rc = __stat4_value_from_expr(parser, upper->expr->right, aff, &p2);
    n_upper = p2 ? 0 : p->n_sample;
  }

  if( p1 || p2 ){
    int i;
    int n_diff;
    for(i=0; rc==DBSQL_OK && i<p->n_sample; i++){
      rc = __stat4_column(db, p->samples[i].p, p->samples[i].n, n_eq, &val);
      if( rc==DBSQL_OK && p1 ){
        int res = __mem_compare(p1, val, coll);
        if( res>=0 ) n_lower++;
      }
      if( rc==DBSQL_OK && p2 ){
        int res = __mem_compare(p2, val, coll);
        if( res>=0 ) n_upper++;
      }
    }
    n_diff = (n_upper - n_lower);
    if( n_diff<=0 ) n_diff = 1;

    /* If there is both an upper and lower bound specified, and the
    ** comparisons indicate that they are close together, use the fallback
    ** method (assume that the scan visits 1/64 of the rows) for estimating
    ** the number of rows visited. Otherwise, estimate the number of rows
    ** using the method described in the header comment for this function. */
    if( n_diff!=1 || upper==0 || lower==0 ){
      int n_adjust = (__log_est(p->n_sample) - __log_est(n_diff));
      loop->n_out -= n_adjust;
      *pb_done = 1;
      WHERETRACE(0x20, ("range skip-scan regions: %u..%u  adjust=%d est=%d\n",
                           n_lower, n_upper, n_adjust*-1, loop->n_out));
    }

  }else{
    assert( *pb_done==0 );
  }

  __value_free(p1);
  __value_free(p2);
  __value_free(val);

  return rc;
}
#endif /* DBSQL_ENABLE_STAT4 */

/*
** This function is used to estimate the number of rows that will be visited
** by scanning an index for a range of values. The range may have an upper
** bound, a lower bound, or both. The WHERE clause terms that set the upper
** and lower bounds are represented by lower and upper respectively. For
** example, assuming that index p is on t1(a):
**
**   ... FROM t1 WHERE a > ? AND a < ? ...
**                    |_____|   |_____|
**                       |         |
**                     lower    upper
**
** If either of the upper or lower bound is not present, then NULL is passed in
** place of the corresponding where_term_t.
**
** The value in (builder->p_new->u.btree.n_eq) is the number of the index
** column subject to the range constraint. Or, equivalently, the number of
** equality constraints optimized by the proposed index scan. For example,
** assuming index p is on t1(a, b), and the SQL query is:
**
**   ... FROM t1 WHERE a = ? AND b > ? AND b < ? ...
**
** then n_eq is set to 1 (as the range restricted column, b, is the second
** left-most column of the index). Or, if the query is:
**
**   ... FROM t1 WHERE a > ? AND a < ? ...
**
** then n_eq is set to 0.
**
** When this function is called, *pn_out is set to the __log_est() of the
** number of rows that the index scan is expected to visit without
** considering the range constraints. If n_eq is 0, then *pn_out is the number of
** rows in the index. Assuming no error occurs, *pn_out is adjusted (reduced)
** to account for the range constraints lower and upper.
**
** In the absence of dbsql_stat4 ANALYZE data, or if such data cannot be
** used, a single range inequality reduces the search space by a factor of 4.
** and a pair of constraints (x>? AND x<?) reduces the expected number of
** rows visited by a factor of 64.
*/
static int __where_range_scan_est(
  parser_t *parser,       /* Parsing & code generating context */
  where_loop_builder_t *builder,
  where_term_t *lower,   /* Lower bound on the range. ex: "x>123" Might be NULL */
  where_term_t *upper,   /* Upper bound on the range. ex: "x<455" Might be NULL */
  where_loop_t *loop     /* Modify the .n_out and maybe .r_run fields */
){
  int rc = DBSQL_OK;
  int n_out = loop->n_out;
  log_est_t n_new;

#ifdef DBSQL_ENABLE_STAT4
  index_t *p = loop->u.btree.p_index;
  int n_eq = loop->u.btree.n_eq;

  if( p->n_sample>0 && ALWAYS(n_eq<p->n_sample_col)
   && OPTIMIZATION_ENABLED(parser->db, DBSQL_Stat4)
  ){
    if( n_eq==builder->n_rec_valid ){
      unpacked_record_t *rec = builder->rec;
      rowcnt_t a[2];
      int n_btm = loop->u.btree.n_btm;
      int n_top = loop->u.btree.n_top;

      /* Variable i_lower will be set to the estimate of the number of rows in
      ** the index that are less than the lower bound of the range query. The
      ** lower bound being the concatenation of $P and $L, where $P is the
      ** key-prefix formed by the n_eq values matched against the n_eq left-most
      ** columns of the index, and $L is the value in lower.
      **
      ** Or, if lower is NULL or $L cannot be extracted from it (because it
      ** is not a simple variable or literal value), the lower bound of the
      ** range is $P. Due to a quirk in the way __where_key_stats() works, even
      ** if $L is available, __where_key_stats() is called for both ($P) and
      ** ($P:$L) and the larger of the two returned values is used.
      **
      ** Similarly, i_upper is to be set to the estimate of the number of rows
      ** less than the upper bound of the range query. Where the upper bound
      ** is either ($P) or ($P:$U). Again, even if $U is available, both values
      ** of i_upper are requested of __where_key_stats() and the smaller used.
      **
      ** The number of rows between the two bounds is then just i_upper-i_lower.
      */
      rowcnt_t i_lower;     /* Rows less than the lower bound */
      rowcnt_t i_upper;     /* Rows less than the upper bound */
      int i_lwr_idx = -2;   /* samples[] for the lower bound */
      int i_upr_idx = -1;   /* samples[] for the upper bound */

      if( rec ){
        testcase( rec->n_field!=builder->n_rec_valid );
        rec->n_field = builder->n_rec_valid;
      }
      /* Determine i_lower and i_upper using ($P) only. */
      if( n_eq==0 ){
        i_lower = 0;
        i_upper = p->n_row_est0;
      }else{
        /* Note: this call could be optimized away - since the same values must
        ** have been requested when testing key $P in __where_equal_scan_est().  */
        __where_key_stats(parser, p, rec, 0, a);
        i_lower = a[0];
        i_upper = a[0] + a[1];
      }

      assert( lower==0 || (lower->e_operator & (WO_GT|WO_GE))!=0 );
      assert( upper==0 || (upper->e_operator & (WO_LT|WO_LE))!=0 );
      assert( p->sort_orders!=0 );
      if( p->sort_orders[n_eq] ){
        /* The roles of lower and upper are swapped for a DESC index */
        SWAP(where_term_t*, lower, upper);
        SWAP(int, n_btm, n_top);
      }

      /* If possible, improve on the i_lower estimate using ($P:$L). */
      if( lower ){
        int n;                    /* Values extracted from expr */
        expr_t *expr = lower->expr->right;
        rc = __stat4_probe_set_value(parser, p, &rec, expr, n_btm, n_eq, &n);
        if( rc==DBSQL_OK && n ){
          rowcnt_t i_new;
          u_int16_t mask = WO_GT|WO_LE;
          if( __expr_vector_size(expr)>n ) mask = (WO_LE|WO_LT);
          i_lwr_idx = __where_key_stats(parser, p, rec, 0, a);
          i_new = a[0] + ((lower->e_operator & mask) ? a[1] : 0);
          if( i_new>i_lower ) i_lower = i_new;
          n_out--;
          lower = 0;
        }
      }

      /* If possible, improve on the i_upper estimate using ($P:$U). */
      if( upper ){
        int n;                    /* Values extracted from expr */
        expr_t *expr = upper->expr->right;
        rc = __stat4_probe_set_value(parser, p, &rec, expr, n_top, n_eq, &n);
        if( rc==DBSQL_OK && n ){
          rowcnt_t i_new;
          u_int16_t mask = WO_GT|WO_LE;
          if( __expr_vector_size(expr)>n ) mask = (WO_LE|WO_LT);
          i_upr_idx = __where_key_stats(parser, p, rec, 1, a);
          i_new = a[0] + ((upper->e_operator & mask) ? a[1] : 0);
          if( i_new<i_upper ) i_upper = i_new;
          n_out--;
          upper = 0;
        }
      }

      builder->rec = rec;
      if( rc==DBSQL_OK ){
        if( i_upper>i_lower ){
          n_new = __log_est(i_upper - i_lower);
          /* TUNING:  If both i_upper and i_lower are derived from the same
          ** sample, then assume they are 4x more selective.  This brings
          ** the estimated selectivity more in line with what it would be
          ** if estimated without the use of STAT4 tables. */
          if( i_lwr_idx==i_upr_idx ){ n_new -= 20; }
          assert( 20==__log_est(4) );
        }else{
          n_new = 10;        assert( 10==__log_est(2) );
        }
        if( n_new<n_out ){
          n_out = n_new;
        }
        WHERETRACE(0x20, ("STAT4 range scan: %u..%u  est=%d\n",
                           (u_int32_t)i_lower, (u_int32_t)i_upper, n_out));
      }
    }else{
      int done = 0;
      rc = __where_range_skip_scan_est(parser, lower, upper, loop, &done);
      if( done ) return rc;
    }
  }
#else
  UNUSED_PARAMETER(parser);
  UNUSED_PARAMETER(builder);
  assert( lower || upper );
#endif
  assert( upper==0 || (upper->wt_flags & TERM_VNULL)==0 || parser->n_err>0 );
  n_new = __where_range_adjust(lower, n_out);
  n_new = __where_range_adjust(upper, n_new);

  /* TUNING: If there is both an upper and lower limit and neither limit
  ** has an application-defined likelihood(), assume the range is
  ** reduced by an additional 75%. This means that, by default, an open-ended
  ** range query (e.g. col > ?) is assumed to match 1/4 of the rows in the
  ** index. While a closed range (e.g. col BETWEEN ? AND ?) is estimated to
  ** match 1/64 of the index. */
  if( lower && lower->truth_prob>0 && upper && upper->truth_prob>0 ){
    n_new -= 20;
  }

  n_out -= (lower!=0) + (upper!=0);
  if( n_new<10 ) n_new = 10;
  if( n_new<n_out ) n_out = n_new;
#if defined(WHERETRACE_ENABLED)
  if( loop->n_out>n_out ){
    WHERETRACE(0x20,("Range scan lowers nOut from %d to %d\n",
                    loop->n_out, n_out));
  }
#endif
  loop->n_out = (log_est_t)n_out;
  return rc;
}

#ifdef DBSQL_ENABLE_STAT4
/*
** Estimate the number of rows that will be returned based on
** an equality constraint x=VALUE and where that VALUE occurs in
** the histogram data.  This only works when x is the left-most
** column of an index and dbsql_stat4 histogram data is available
** for that index.  When expr==NULL that means the constraint is
** "x IS NULL" instead of "x=VALUE".
**
** Write the estimated row count into *pn_row and return DBSQL_OK.
** If unable to make an estimate, leave *pn_row unchanged and return
** non-zero.
**
** This routine can fail if it is unable to load a collating sequence
** required for string comparison, or if unable to allocate memory
** for a UTF conversion required for comparison.  The error is stored
** in the parser structure.
*/
static int __where_equal_scan_est(
  parser_t *parser,       /* Parsing & code generating context */
  where_loop_builder_t *builder,
  expr_t *expr,         /* Expression for VALUE in the x=VALUE constraint */
  rowcnt_t *pn_row       /* Write the revised row estimate here */
){
  index_t *p = builder->p_new->u.btree.p_index;
  int n_eq = builder->p_new->u.btree.n_eq;
  unpacked_record_t *rec = builder->rec;
  int rc;                   /* Subfunction return code */
  rowcnt_t a[2];             /* Statistics */
  int ok;

  assert( n_eq>=1 );
  assert( n_eq<=p->n_column );
  assert( p->samples!=0 );
  assert( p->n_sample>0 );
  assert( builder->n_rec_valid<n_eq );

  /* If values are not available for all fields of the index to the left
  ** of this one, no estimate can be made. Return DBSQL_NOTFOUND. */
  if( builder->n_rec_valid<(n_eq-1) ){
    return DBSQL_NOTFOUND;
  }

  /* This is an optimization only. The call to __stat4_probe_set_value()
  ** below would return the same value.  */
  if( n_eq>=p->n_column ){
    *pn_row = 1;
    return DBSQL_OK;
  }

  rc = __stat4_probe_set_value(parser, p, &rec, expr, 1, n_eq-1, &ok);
  builder->rec = rec;
  if( rc!=DBSQL_OK ) return rc;
  if( ok==0 ) return DBSQL_NOTFOUND;
  builder->n_rec_valid = n_eq;

  __where_key_stats(parser, p, rec, 0, a);
  WHERETRACE(0x20,("equality scan regions %s(%d): %d\n",
                   p->name, n_eq-1, (int)a[1]));
  *pn_row = a[1];
 
  return rc;
}
#endif /* DBSQL_ENABLE_STAT4 */

#ifdef DBSQL_ENABLE_STAT4
/*
** Estimate the number of rows that will be returned based on
** an IN constraint where the right-hand side of the IN operator
** is a list of values.  Example:
**
**        WHERE x IN (1,2,3,4)
**
** Write the estimated row count into *pn_row and return DBSQL_OK.
** If unable to make an estimate, leave *pn_row unchanged and return
** non-zero.
**
** This routine can fail if it is unable to load a collating sequence
** required for string comparison, or if unable to allocate memory
** for a UTF conversion required for comparison.  The error is stored
** in the parser structure.
*/
static int __where_in_scan_est(
  parser_t *parser,       /* Parsing & code generating context */
  where_loop_builder_t *builder,
  expr_list_t *list,     /* The value list on the RHS of "x IN (v1,v2,v3,...)" */
  rowcnt_t *pn_row       /* Write the revised row estimate here */
){
  index_t *p = builder->p_new->u.btree.p_index;
  int64_t n_row0 = __log_est_to_int(p->row_log_ests[0]);
  int n_rec_valid = builder->n_rec_valid;
  int rc = DBSQL_OK;     /* Subfunction return code */
  rowcnt_t n_est;           /* Number of rows for a single term */
  rowcnt_t n_row_est = 0;    /* New estimate of the number of rows */
  int i;                  /* Loop counter */

  assert( p->samples!=0 );
  for(i=0; rc==DBSQL_OK && i<list->n_expr; i++){
    n_est = n_row0;
    rc = __where_equal_scan_est(parser, builder, list->a[i].expr, &n_est);
    n_row_est += n_est;
    builder->n_rec_valid = n_rec_valid;
  }

  if( rc==DBSQL_OK ){
    if( n_row_est > (rowcnt_t)n_row0 ) n_row_est = n_row0;
    *pn_row = n_row_est;
    WHERETRACE(0x20,("IN row estimate: est=%d\n", n_row_est));
  }
  assert( builder->n_rec_valid==n_rec_valid );
  return rc;
}
#endif /* DBSQL_ENABLE_STAT4 */


#if defined(WHERETRACE_ENABLED) || defined(DBSQL_DEBUG)
/*
** Print the content of a where_term_t object
*/
void __where_term_print(where_term_t *term, int i_term){
  if( term==0 ){
    __debug_printf("TERM-%-3d NULL\n", i_term);
  }else{
    char type[8];
    char left[50];
    memcpy(type, "....", 5);
    if( term->wt_flags & TERM_VIRTUAL ) type[0] = 'V';
    if( term->e_operator & WO_EQUIV  ) type[1] = 'E';
    if( EXPR_HAS_PROPERTY(term->expr, EP_OuterON) ) type[2] = 'L';
    if( term->wt_flags & TERM_CODED  ) type[3] = 'C';
    if( term->e_operator & WO_SINGLE ){
      assert( (term->e_operator & (WO_OR|WO_AND))==0 );
      dbsql_snprintf(sizeof(left),left,"left={%d:%d}",
                       term->left_cursor, term->u.x.left_column);
    }else if( (term->e_operator & WO_OR)!=0 && term->u.or_info!=0 ){
      dbsql_snprintf(sizeof(left),left,"indexable=0x%llx",
                       term->u.or_info->indexable);
    }else{
      dbsql_snprintf(sizeof(left),left,"left=%d", term->left_cursor);
    }
    i_term = term->i_term = MAX(i_term,term->i_term);
    __debug_printf(
       "TERM-%-3d %p %s %-12s op=%03x wtFlags=%04x",
       i_term, term, type, left, term->e_operator, term->wt_flags);
    /* The 0x10000 .wheretrace flag causes extra information to be
    ** shown about each Term */
    if( __where_trace & 0x10000 ){
      __debug_printf(" prob=%-3d prereq=%llx,%llx",
        term->truth_prob, (u_int64_t)term->prereq_all, (u_int64_t)term->prereq_right);
    }
    if( (term->e_operator & (WO_OR|WO_AND))==0 && term->u.x.i_field ){
      __debug_printf(" iField=%d", term->u.x.i_field);
    }
    if( term->i_parent>=0 ){
      __debug_printf(" iParent=%d", term->i_parent);
    }
    __debug_printf("\n");
    __tree_view_expr(0, term->expr, 0);
  }
}
void __show_where_term(where_term_t *term){
  __where_term_print(term, 0);
}
#endif

#ifdef WHERETRACE_ENABLED
/*
** Show the complete content of a where_clause_t
*/
void __where_clause_print(where_clause_t *wc){
  int i;
  for(i=0; i<wc->n_term; i++){
    __where_term_print(&wc->a[i], i);
  }
}
#endif

#ifdef WHERETRACE_ENABLED
/*
** Print a where_loop_t object for debugging purposes
**
** Format example:
**
**     .--- Position in WHERE clause           r_setup, r_run, n_out ---.
**     |                                                             |
**     |  .--- self_mask                       n_term ------.          |
**     |  |                                               |          |
**     |  |   .-- prereq    Idx          ws_flags----.     |          |
**     |  |   |             Name                    |     |          |
**     |  |   |           __|__        n_eq ---.  ___|__   |        __|__
**     | / \ / \         /     \              | /      \ / \      /     \
**     1.002.001         t2.t2xy              2 f 010241 N 2 cost 0,56,31
*/
void __where_loop_print(const where_loop_t *p, const where_clause_t *wc){
  where_info_t *w_info;
  if( wc ){
    int nb;
    src_item_t *item;
    table_t *tab;
    Bitmask m_all;

    w_info = wc->w_info;
    nb = 1+(w_info->tab_list->n_src+3)/4;
    item = w_info->tab_list->a + p->i_tab;
    tab = item->s_tab;
    m_all = (((Bitmask)1)<<(nb*4)) - 1;
    __debug_printf("%c%2d.%0*llx.%0*llx", p->c_id,
                       p->i_tab, nb, p->mask_self, nb, p->prereq & m_all);
    __debug_printf(" %12s",
                       item->alias ? item->alias : tab->name);
  }else{
    w_info = 0;
    __debug_printf("%c%2d.%03llx.%03llx %c%d",
         p->c_id, p->i_tab, p->mask_self, p->prereq & 0xfff, p->c_id, p->i_tab);
  }
  if( (p->ws_flags & WHERE_VIRTUALTABLE)==0 ){
    const char *name;
    if( p->u.btree.p_index && (name = p->u.btree.p_index->name)!=0 ){
      if( strncmp(name, "dbsql_autoindex_", 16)==0 ){
        int i = __strlen30(name) - 1;
        while( name[i]!='_' ) i--;
        name += i;
      }
      __debug_printf(".%-16s %2d", name, p->u.btree.n_eq);
    }else{
      __debug_printf("%20s","");
    }
  }else{
    char *z;
    if( p->u.vtab.idx_str ){
      z = dbsql_mprintf("(%d,\"%s\",%#x)",
                p->u.vtab.idx_num, p->u.vtab.idx_str, p->u.vtab.omit_mask);
    }else{
      z = dbsql_mprintf("(%d,%x)", p->u.vtab.idx_num, p->u.vtab.omit_mask);
    }
    __debug_printf(" %-19s", z);
    dbsql_free(z);
  }
  if( p->ws_flags & WHERE_SKIPSCAN ){
    __debug_printf(" f %06x %d-%d", p->ws_flags, p->n_l_term,p->n_skip);
  }else{
    __debug_printf(" f %06x N %d", p->ws_flags, p->n_l_term);
  }
  if( w_info && w_info->star_used && p->r_star_delta!=0 ){
    __debug_printf(" cost %d,%d,%d delta=%d\n",
                       p->r_setup, p->r_run, p->n_out, p->r_star_delta);
  }else{
    __debug_printf(" cost %d,%d,%d\n", p->r_setup, p->r_run, p->n_out);
  }
  if( p->n_l_term && (__where_trace & 0x4000)!=0 ){
    int i;
    for(i=0; i<p->n_l_term; i++){
      __where_term_print(p->l_terms[i], i);
    }
  }
}
void __show_where_loop(const where_loop_t *p){
  if( p ) __where_loop_print(p, 0);
}
void __show_where_loop_list(const where_loop_t *p){
  while( p ){
    __show_where_loop(p);
    p = p->next_loop;
  }
}
#endif

/*
** Convert bulk memory into a valid where_loop_t that can be passed
** to __where_loop_clear harmlessly.
*/
static void __where_loop_init(where_loop_t *p){
  p->l_terms = p->l_term_spaces;
  p->n_l_term = 0;
  p->n_l_slot = ARRAY_SIZE(p->l_term_spaces);
  p->ws_flags = 0;
}

/*
** Clear the where_loop_t.u union.  Leave where_loop_t.l_term intact.
*/
static void __where_loop_clear_union(DBSQL *db, where_loop_t *p){
  if( p->ws_flags & (WHERE_VIRTUALTABLE|WHERE_AUTO_INDEX) ){
    if( (p->ws_flags & WHERE_VIRTUALTABLE)!=0 && p->u.vtab.need_free ){
      dbsql_free(p->u.vtab.idx_str);
      p->u.vtab.need_free = 0;
      p->u.vtab.idx_str = 0;
    }else if( (p->ws_flags & WHERE_AUTO_INDEX)!=0 && p->u.btree.p_index!=0 ){
      __dbsql_db_free(db, p->u.btree.p_index->col_aff);
      __db_free_nn(db, p->u.btree.p_index);
      p->u.btree.p_index = 0;
    }
  }
}

/*
** Deallocate internal memory used by a where_loop_t object.  Leave the
** object in an initialized state, as if it had been newly allocated.
*/
static void __where_loop_clear(DBSQL *db, where_loop_t *p){
  if( p->l_terms!=p->l_term_spaces ){
    __db_free_nn(db, p->l_terms);
    p->l_terms = p->l_term_spaces;
    p->n_l_slot = ARRAY_SIZE(p->l_term_spaces);
  }
  __where_loop_clear_union(db, p);
  p->n_l_term = 0;
  p->ws_flags = 0;
}

/*
** Increase the memory allocation for loop->l_terms[] to be at least n.
*/
static int __where_loop_resize(DBSQL *db, where_loop_t *p, int n){
  where_term_t **pa_new;
  if( p->n_l_slot>=n ) return DBSQL_OK;
  n = (n+7)&~7;
  pa_new = __db_malloc_raw_nn(db, sizeof(p->l_terms[0])*n);
  if( pa_new==0 ) return DBSQL_NOMEM_BKPT;
  memcpy(pa_new, p->l_terms, sizeof(p->l_terms[0])*p->n_l_slot);
  if( p->l_terms!=p->l_term_spaces ) __db_free_nn(db, p->l_terms);
  p->l_terms = pa_new;
  p->n_l_slot = n;
  return DBSQL_OK;
}

/*
** Transfer content from the second loop into the first.
*/
static int __where_loop_xfer(DBSQL *db, where_loop_t *to, where_loop_t *from){
  __where_loop_clear_union(db, to);
  if( from->n_l_term > to->n_l_slot
   && __where_loop_resize(db, to, from->n_l_term)
  ){
    memset(to, 0, WHERE_LOOP_XFER_SZ);
    return DBSQL_NOMEM_BKPT;
  }
  memcpy(to, from, WHERE_LOOP_XFER_SZ);
  memcpy(to->l_terms, from->l_terms, to->n_l_term*sizeof(to->l_terms[0]));
  if( from->ws_flags & WHERE_VIRTUALTABLE ){
    from->u.vtab.need_free = 0;
  }else if( (from->ws_flags & WHERE_AUTO_INDEX)!=0 ){
    from->u.btree.p_index = 0;
  }
  return DBSQL_OK;
}

/*
** Delete a where_loop_t object
*/
static void __where_loop_delete(DBSQL *db, where_loop_t *p){
  assert( db!=0 );
  __where_loop_clear(db, p);
  __db_nn_free_nn(db, p);
}

/*
** Free a where_info_t structure
*/
static void __where_info_free(DBSQL *db, where_info_t *w_info){
  assert( w_info!=0 );
  assert( db!=0 );
  __where_clause_clear(&w_info->s_wc);
  while( w_info->loops ){
    where_loop_t *p = w_info->loops;
    w_info->loops = p->next_loop;
    __where_loop_delete(db, p);
  }
  while( w_info->mem_to_free ){
    where_mem_block_t *next = w_info->mem_to_free->next;
    __db_nn_free_nn(db, w_info->mem_to_free);
    w_info->mem_to_free = next;
  }
  __db_nn_free_nn(db, w_info);
}

/*
** Return TRUE if X is a proper subset of Y but is of equal or less cost.
** In other words, return true if all constraints of X are also part of Y
** and Y has additional constraints that might speed the search that X lacks
** but the cost of running X is not more than the cost of running Y.
**
** In other words, return true if the cost relationship between X and Y
** is inverted and needs to be adjusted.
**
** Case 1:
**
**   (1a)  X and Y use the same index.
**   (1b)  X has fewer == terms than Y
**   (1c)  Neither X nor Y use skip-scan
**   (1d)  X does not have a a greater cost than Y
**
** Case 2:
**
**   (2a)  X has the same or lower cost, or returns the same or fewer rows,
**         than Y.
**   (2b)  X uses fewer WHERE clause terms than Y
**   (2c)  Every WHERE clause term used by X is also used by Y
**   (2d)  X skips at least as many columns as Y
**   (2e)  If X is a covering index, than Y is too
*/
static int __where_loop_cheaper_proper_subset(
  const where_loop_t *x,       /* First where_loop_t to compare */
  const where_loop_t *y        /* Compare against this where_loop_t */
){
  int i, j;
  if( x->r_run>y->r_run && x->n_out>y->n_out ) return 0; /* (1d) and (2a) */
  assert( (x->ws_flags & WHERE_VIRTUALTABLE)==0 );
  assert( (y->ws_flags & WHERE_VIRTUALTABLE)==0 );
  if( x->u.btree.n_eq < y->u.btree.n_eq                  /* (1b) */
   && x->u.btree.p_index==y->u.btree.p_index             /* (1a) */
   && x->n_skip==0 && y->n_skip==0                       /* (1c) */
  ){
    return 1;  /* Case 1 is true */
  }
  if( x->n_l_term-x->n_skip >= y->n_l_term-y->n_skip ){
    return 0;                                            /* (2b) */
  }
  if( y->n_skip > x->n_skip ) return 0;                  /* (2d) */
  for(i=x->n_l_term-1; i>=0; i--){
    if( x->l_terms[i]==0 ) continue;
    for(j=y->n_l_term-1; j>=0; j--){
      if( y->l_terms[j]==x->l_terms[i] ) break;
    }
    if( j<0 ) return 0;                                  /* (2c) */
  }
  if( (x->ws_flags&WHERE_IDX_ONLY)!=0
   && (y->ws_flags&WHERE_IDX_ONLY)==0 ){
    return 0;                                            /* (2e) */
  }
  return 1;  /* Case 2 is true */
}

/*
** Try to adjust the cost and number of output rows of where_loop_t p_template
** upwards or downwards so that:
**
**   (1) p_template costs less than any other __where_loops that are a proper
**       subset of p_template
**
**   (2) p_template costs more than any other __where_loops for which p_template
**       is a proper subset.
**
** To say "where_loop_t X is a proper subset of Y" means that X uses fewer
** WHERE clause terms than Y and that every WHERE clause term used by X is
** also used by Y.
*/
static void __where_loop_adjust_cost(const where_loop_t *p, where_loop_t *p_template){
  if( (p_template->ws_flags & WHERE_INDEXED)==0 ) return;
  for(; p; p=p->next_loop){
    if( p->i_tab!=p_template->i_tab ) continue;
    if( (p->ws_flags & WHERE_INDEXED)==0 ) continue;
    if( __where_loop_cheaper_proper_subset(p, p_template) ){
      /* Adjust p_template cost downward so that it is cheaper than its
      ** subset p. */
      WHERETRACE(0x80,("subset cost adjustment %d,%d to %d,%d\n",
                       p_template->r_run, p_template->n_out,
                       MIN(p->r_run, p_template->r_run),
                       MIN(p->n_out - 1, p_template->n_out)));
      p_template->r_run = MIN(p->r_run, p_template->r_run);
      p_template->n_out = MIN(p->n_out - 1, p_template->n_out);
    }else if( __where_loop_cheaper_proper_subset(p_template, p) ){
      /* Adjust p_template cost upward so that it is costlier than p since
      ** p_template is a proper subset of p */
      WHERETRACE(0x80,("subset cost adjustment %d,%d to %d,%d\n",
                       p_template->r_run, p_template->n_out,
                       MAX(p->r_run, p_template->r_run),
                       MAX(p->n_out + 1, p_template->n_out)));
      p_template->r_run = MAX(p->r_run, p_template->r_run);
      p_template->n_out = MAX(p->n_out + 1, p_template->n_out);
    }
  }
}

/*
** Search the list of __where_loops in *prev looking for one that can be
** replaced by p_template.
**
** Return NULL if p_template does not belong on the where_loop_t list.
** In other words if p_template ought to be dropped from further consideration.
**
** If x is a where_loop_t that p_template can replace, then return the
** link that points to x.
**
** If p_template cannot replace any existing element of the list but needs
** to be added to the list as a new entry, then return a pointer to the
** tail of the list.
*/
static where_loop_t **__where_loop_find_lesser(
  where_loop_t **prev,
  const where_loop_t *p_template
){
  where_loop_t *p;
  for(p=(*prev); p; prev=&p->next_loop, p=*prev){
    if( p->i_tab!=p_template->i_tab || p->i_sort_idx!=p_template->i_sort_idx ){
      /* If either the i_tab or i_sort_idx values for two where_loop_t are different
      ** then those __where_loops need to be considered separately.  Neither is
      ** a candidate to replace the other. */
      continue;
    }
    /* In the current implementation, the r_setup value is either zero
    ** or the cost of building an automatic index (__nlog_n) and the __nlog_n
    ** is the same for compatible __where_loops. */
    assert( p->r_setup==0 || p_template->r_setup==0
                 || p->r_setup==p_template->r_setup );

    /* __where_loop_add_btree() always generates and inserts the automatic index
    ** case first.  Hence compatible candidate __where_loops never have a larger
    ** r_setup. Call this SETUP-INVARIANT */
    assert( p->r_setup>=p_template->r_setup );

    /* Any loop using an application-defined index (or PRIMARY KEY or
    ** UNIQUE constraint) with one or more == constraints is better
    ** than an automatic index. Unless it is a skip-scan. */
    if( (p->ws_flags & WHERE_AUTO_INDEX)!=0
     && (p_template->n_skip)==0
     && (p_template->ws_flags & WHERE_INDEXED)!=0
     && (p_template->ws_flags & WHERE_COLUMN_EQ)!=0
     && (p->prereq & p_template->prereq)==p_template->prereq
    ){
      break;
    }

    /* If existing where_loop_t p is better than p_template, p_template can be
    ** discarded.  where_loop_t p is better if:
    **   (1)  p has no more dependencies than p_template, and
    **   (2)  p has an equal or lower cost than p_template
    */
    if( (p->prereq & p_template->prereq)==p->prereq    /* (1)  */
     && p->r_setup<=p_template->r_setup                  /* (2a) */
     && p->r_run<=p_template->r_run                      /* (2b) */
     && p->n_out<=p_template->n_out                      /* (2c) */
    ){
      return 0;  /* Discard p_template */
    }

    /* If p_template is always better than p, then cause p to be overwritten
    ** with p_template.  p_template is better than p if:
    **   (1)  p_template has no more dependencies than p, and
    **   (2)  p_template has an equal or lower cost than p.
    */
    if( (p->prereq & p_template->prereq)==p_template->prereq   /* (1)  */
     && p->r_run>=p_template->r_run                             /* (2a) */
     && p->n_out>=p_template->n_out                             /* (2b) */
    ){
      assert( p->r_setup>=p_template->r_setup ); /* SETUP-INVARIANT above */
      break;   /* Cause p to be overwritten by p_template */
    }
  }
  return prev;
}

/*
** Insert or replace a where_loop_t entry using the template supplied.
**
** An existing where_loop_t entry might be overwritten if the new template
** is better and has fewer dependencies.  Or the template will be ignored
** and no insert will occur if an existing where_loop_t is faster and has
** fewer dependencies than the template.  Otherwise a new where_loop_t is
** added based on the template.
**
** If builder->or_set is not NULL then we care about only the
** prerequisites and r_run and n_out costs of the N best loops.  That
** information is gathered in the builder->or_set object.  This special
** processing mode is used only for OR clause processing.
**
** When accumulating multiple loops (when builder->or_set is NULL) we
** still might overwrite similar loops with the new template if the
** new template is better.  Loops may be overwritten if the following
** conditions are met:
**
**    (1)  They have the same i_tab.
**    (2)  They have the same i_sort_idx.
**    (3)  The template has same or fewer dependencies than the current loop
**    (4)  The template has the same or lower cost than the current loop
*/
static int __where_loop_insert(where_loop_builder_t *builder, where_loop_t *p_template){
  where_loop_t **prev, *p;
  where_info_t *w_info = builder->w_info;
  DBSQL *db = w_info->parser->db;
  int rc;

  /* Stop the search once we hit the query planner search limit */
  if( builder->i_plan_limit==0 ){
    WHERETRACE(0xffffffff,("=== query planner search limit reached ===\n"));
    if( builder->or_set ) builder->or_set->n = 0;
    return DBSQL_DONE;
  }
  builder->i_plan_limit--;

  __where_loop_adjust_cost(w_info->loops, p_template);

  /* If builder->or_set is defined, then only keep track of the costs
  ** and prereqs.
  */
  if( builder->or_set!=0 ){
    if( p_template->n_l_term ){
#if WHERETRACE_ENABLED
      u_int16_t n = builder->or_set->n;
      int x =
#endif
      __where_or_insert(builder->or_set, p_template->prereq, p_template->r_run,
                                    p_template->n_out);
#if WHERETRACE_ENABLED /* 0x8 */
      if( __where_trace & 0x8 ){
        __debug_printf(x?"   or-%d:  ":"   or-X:  ", n);
        __where_loop_print(p_template, builder->wc);
      }
#endif
    }
    return DBSQL_OK;
  }

  /* Look for an existing where_loop_t to replace with p_template
  */
  prev = __where_loop_find_lesser(&w_info->loops, p_template);

  if( prev==0 ){
    /* There already exists a where_loop_t on the list that is better
    ** than p_template, so just ignore p_template */
#if WHERETRACE_ENABLED /* 0x8 */
    if( __where_trace & 0x8 ){
      __debug_printf("   skip: ");
      __where_loop_print(p_template, builder->wc);
    }
#endif
    return DBSQL_OK; 
  }else{
    p = *prev;
  }

  /* If we reach this point it means that either p[] should be overwritten
  ** with p_template[] if p[] exists, or if p==NULL then allocate a new
  ** where_loop_t and insert it.
  */
#if WHERETRACE_ENABLED /* 0x8 */
  if( __where_trace & 0x8 ){
    if( p!=0 ){
      __debug_printf("replace: ");
      __where_loop_print(p, builder->wc);
      __debug_printf("   with: ");
    }else{
      __debug_printf("    add: ");
    }
    __where_loop_print(p_template, builder->wc);
  }
#endif
  if( p==0 ){
    /* Allocate a new where_loop_t to add to the end of the list */
    *prev = p = __db_malloc_raw_nn(db, sizeof(where_loop_t));
    if( p==0 ) return DBSQL_NOMEM_BKPT;
    __where_loop_init(p);
    p->next_loop = 0;
  }else{
    /* We will be overwriting where_loop_t p[].  But before we do, first
    ** go through the rest of the list and delete any other entries besides
    ** p[] that are also supplanted by p_template */
    where_loop_t **tail = &p->next_loop;
    where_loop_t *to_del;
    while( *tail ){
      tail = __where_loop_find_lesser(tail, p_template);
      if( tail==0 ) break;
      to_del = *tail;
      if( to_del==0 ) break;
      *tail = to_del->next_loop;
#if WHERETRACE_ENABLED /* 0x8 */
      if( __where_trace & 0x8 ){
        __debug_printf(" delete: ");
        __where_loop_print(to_del, builder->wc);
      }
#endif
      __where_loop_delete(db, to_del);
    }
  }
  rc = __where_loop_xfer(db, p, p_template);
  if( (p->ws_flags & WHERE_VIRTUALTABLE)==0 ){
    index_t *p_index = p->u.btree.p_index;
    if( p_index && p_index->idx_type==DBSQL_IDXTYPE_IPK ){
      p->u.btree.p_index = 0;
    }
  }
  return rc;
}

/*
** Callback for __est_like_pattern_length().
**
** If this node is a string literal that is longer walker->sz, then set
** walker->sz to the byte length of that string literal.
**
** walker->code indicates how to count characters:
**
**    code==0     Count as a GLOB pattern
**    code==1     Count as a LIKE pattern
*/
static int __expr_node_pattern_length_est(walker_t *walker, expr_t *expr){
  if( expr->op==TK_STRING ){
    int sz = 0;                    /* Pattern size in bytes */
    u_int8_t *z = (u_int8_t*)expr->u.token;  /* The pattern */
    u_int8_t c;                          /* Next character of the pattern */
    u_int8_t c1, c2, c3;                 /* Wildcards */
    if( walker->code ){
      c1 = '%';
      c2 = '_';
      c3 = 0;
    }else{
      c1 = '*';
      c2 = '?';
      c3 = '[';
    }
    while( (c = *(z++))!=0 ){
      if( c==c3 ){
        if( *z ) z++;
        while( *z && *z!=']' ) z++;
      }else if( c!=c1 && c!=c2 ){
        sz++;
      }
    }
    if( sz>walker->u.sz ) walker->u.sz = sz;
  }
  return WRC_Continue;
}

/*
** Return the length of the longest string literal in the given
** expression.
**
** code indicates how to count characters:
**
**    code==0     Count as a GLOB pattern
**    code==1     Count as a LIKE pattern
*/
static int __est_like_pattern_length(expr_t *p, u_int16_t code){
  walker_t w;
  w.u.sz = 0;
  w.code = code;
  w.expr_callback = __expr_node_pattern_length_est;
  w.select_callback = __select_walk_fail;
#ifdef DBSQL_DEBUG
  w.select_callback2 = __select_walk_assert2;
#endif
  __walk_expr(&w, p);
  return w.u.sz;
}

/*
** Adjust the where_loop_t.n_out value downward to account for terms of the
** WHERE clause that reference the loop but which are not used by an
** index.
*
** For every WHERE clause term that is not used by the index
** and which has a truth probability assigned by one of the likelihood(),
** likely(), or unlikely() SQL functions, reduce the estimated number
** of output rows by the probability specified.
**
** TUNING:  For every WHERE clause term that is not used by the index
** and which does not have an assigned truth probability, heuristics
** described below are used to try to estimate the truth probability.
** TODO --> Perhaps this is something that could be improved by better
** table statistics.
**
** Heuristic 1:  Estimate the truth probability as 93.75%.  The 93.75%
** value corresponds to -1 in log_est_t notation, so this means decrement
** the where_loop_t.n_out field for every such WHERE clause term.
**
** Heuristic 2:  If there exists one or more WHERE clause terms of the
** form "x==EXPR" and EXPR is not a constant 0 or 1, then make sure the
** final output row estimate is no greater than 1/4 of the total number
** of rows in the table.  In other words, assume that x==EXPR will filter
** out at least 3 out of 4 rows.  If EXPR is -1 or 0 or 1, then maybe the
** "x" column is boolean or else -1 or 0 or 1 is a common default value
** on the "x" column and so in that case only cap the output row estimate
** at 1/2 instead of 1/4.
**
** Heuristic 3:  If there is a LIKE or GLOB (or REGEXP or MATCH) operator
** with a large constant pattern, then reduce the size of the search
** space according to the length of the pattern, under the theory that
** longer patterns are less likely to match.  This heuristic was added
** to give better output-row count estimates when preparing queries for
** the Join-Order Benchmarks.  See forum thread 2026-01-30T09:57:54z
*/
static void __where_loop_output_adjust(
  where_clause_t *wc,      /* The WHERE clause */
  where_loop_t *loop,      /* The loop to adjust downward */
  log_est_t n_row            /* Number of rows in the entire table */
){
  where_term_t *term, *x;
  Bitmask not_allowed = ~(loop->prereq|loop->mask_self);
  int i, j;
  log_est_t i_reduce = 0;    /* loop->n_out should not exceed n_row-i_reduce */

  assert( (loop->ws_flags & WHERE_AUTO_INDEX)==0 );
  for(i=wc->n_base, term=wc->a; i>0; i--, term++){
    assert( term!=0 );
    if( (term->prereq_all & not_allowed)!=0 ) continue;
    if( (term->prereq_all & loop->mask_self)==0 ) continue;
    if( (term->wt_flags & TERM_VIRTUAL)!=0 ) continue;
    for(j=loop->n_l_term-1; j>=0; j--){
      x = loop->l_terms[j];
      if( x==0 ) continue;
      if( x==term ) break;
      if( x->i_parent>=0 && (&wc->a[x->i_parent])==term ) break;
    }
    if( j<0 ){
      __progress_check(wc->w_info->parser);
      if( loop->mask_self==term->prereq_all ){
        /* If there are extra terms in the WHERE clause not used by an index
        ** that depend only on the table being scanned, and that will tend to
        ** cause many rows to be omitted, then mark that table as
        ** "self-culling".
        **
        ** 2022-03-24:  Self-culling only applies if either the extra terms
        ** are straight comparison operators that are non-true with NULL
        ** operand, or if the loop is not an OUTER JOIN.
        */
        if( (term->e_operator & 0x3f)!=0
         || (wc->w_info->tab_list->a[loop->i_tab].fg.jointype
                  & (JT_LEFT|JT_LTORJ))==0
        ){
          loop->ws_flags |= WHERE_SELFCULL;
        }
      }
      if( term->truth_prob<=0 ){
        /* If a truth probability is specified using the likelihood() hints,
        ** then use the probability provided by the application. */
        loop->n_out += term->truth_prob;
      }else{
        /* In the absence of explicit truth probabilities, use heuristics to
        ** guess a reasonable truth probability. */
        expr_t *op_expr = term->expr;
        loop->n_out--;
        if( (term->e_operator&(WO_EQ|WO_IS))!=0
         && (term->wt_flags & TERM_HIGHTRUTH)==0  /* tag-20200224-1 */
        ){
          expr_t *right = op_expr->right;
          int k = 0;
          testcase( op_expr->op==TK_IS );
          if( __expr_is_integer(right, &k, 0) && k>=(-1) && k<=1 ){
            k = 10;
          }else{
            k = 20;
          }
          if( i_reduce<k ){
            term->wt_flags |= TERM_HEURTRUTH;
            i_reduce = k;
          }
        }else
        if( EXPR_HAS_PROPERTY(op_expr, EP_InfixFunc) 
         && op_expr->op==TK_FUNCTION
        ){
          int op;
          assert( EXPR_USE_X_LIST(op_expr) );
          assert( op_expr->x.list->n_expr>=2 );
          op = __expr_is_like_operator(op_expr);
          if( ALWAYS(op>0) ){
            int sz_pattern;
            expr_t *rhs = op_expr->x.list->a[0].expr;
            op = op==DBSQL_INDEX_CONSTRAINT_LIKE;
            sz_pattern = __est_like_pattern_length(rhs, op);
            if( sz_pattern>0 ){
              loop->n_out -= sz_pattern*2;
            }
          }
        }
      }
    }
  }
  if( loop->n_out > n_row-i_reduce ){
    loop->n_out = n_row - i_reduce;
  }
}

/*
** Term term is a vector range comparison operation. The first comparison
** in the vector can be optimized using column n_eq of the index. This
** function returns the total number of vector elements that can be used
** as part of the range comparison.
**
** For example, if the query is:
**
**   WHERE a = ? AND (b, c, d) > (?, ?, ?)
**
** and the index:
**
**   CREATE INDEX ... ON (a, b, c, d, e)
**
** then this function would be invoked with n_eq=1. The value returned in
** this case is 3.
*/
static int __where_range_vector_len(
  parser_t *parser,       /* Parsing context */
  int i_cur,            /* Cursor open on idx */
  index_t *idx,         /* The index to be used for a inequality constraint */
  int n_eq,             /* Number of prior equality constraints on same index */
  where_term_t *term     /* The vector inequality constraint */
){
  int n_cmp = __expr_vector_size(term->expr->left);
  int i;

  n_cmp = MIN(n_cmp, (idx->n_column - n_eq));
  for(i=1; i<n_cmp; i++){
    /* Test if comparison i of term is compatible with column (i+n_eq)
    ** of the index. If not, exit the loop.  */
    char aff;                     /* Comparison affinity */
    char idxaff = 0;              /* Indexed columns affinity */
    coll_seq_t *coll;               /* Comparison collation sequence */
    expr_t *lhs, *rhs;

    assert( EXPR_USE_X_LIST(term->expr->left) );
    lhs = term->expr->left->x.list->a[i].expr;
    rhs = term->expr->right;
    if( EXPR_USE_X_SELECT(rhs) ){
      rhs = rhs->x.p_select->e_list->a[i].expr;
    }else{
      rhs = rhs->x.list->a[i].expr;
    }

    /* Check that the LHS of the comparison is a column reference to
    ** the right column of the right source table. And that the sort
    ** order of the index column is the same as the sort order of the
    ** leftmost index column.  */
    if( lhs->op!=TK_COLUMN
     || lhs->i_table!=i_cur
     || lhs->i_column!=idx->columns[i+n_eq]
     || idx->sort_orders[i+n_eq]!=idx->sort_orders[n_eq]
    ){
      break;
    }

    testcase( lhs->i_column==XN_ROWID );
    aff = __compare_affinity(rhs, __expr_affinity(lhs));
    idxaff = __table_column_affinity(idx->table, lhs->i_column);
    if( aff!=idxaff ) break;

    if( EXPR_HAS_PROPERTY(term->expr, EP_Commuted) ) SWAP(expr_t*, rhs, lhs);
    coll = __binary_compare_coll_seq(parser, lhs, rhs);
    if( coll==0 ) break;
    if( __str_icmp(coll->name, idx->colls[i+n_eq]) ) break;
  }
  return i;
}

/*
** Adjust the cost C by the cost_mult factor T.  This only occurs if
** compiled with -DSQLITE_ENABLE_COSTMULT
*/
#ifdef DBSQL_ENABLE_COSTMULT
# define APPLY_COST_MULTIPLIER(C,T)  C += T
#else
# define APPLY_COST_MULTIPLIER(C,T)
#endif

/*
** We have so far matched builder->p_new->u.btree.n_eq terms of the
** index p_index. Try to match one more.
**
** When this function is called, builder->p_new->n_out contains the
** number of rows expected to be visited by filtering using the n_eq
** terms only. If it is modified, this value is restored before this
** function returns.
**
** If probe->idx_type==DBSQL_IDXTYPE_IPK, that means p_index is
** a fake index used for the INTEGER PRIMARY KEY.
*/
static int __where_loop_add_btree_index(
  where_loop_builder_t *builder,     /* The where_loop_t factory */
  src_item_t *src,                  /* FROM clause term being analyzed */
  index_t *probe,                  /* An index on src */
  log_est_t n_in_mul                   /* log(Number of iterations due to IN) */
){
  where_info_t *w_info = builder->w_info;  /* WHERE analyze context */
  parser_t *parser = w_info->parser;        /* Parsing context */
  DBSQL *db = parser->db;       /* Database connection malloc context */
  where_loop_t *p_new;                /* Template where_loop_t under construction */
  where_term_t *term;               /* A where_term_t under consideration */
  int op_mask;                     /* Valid operators for constraints */
  where_scan_t scan;                 /* Iterator for WHERE terms */
  Bitmask saved_prereq;           /* Original value of p_new->prereq */
  u_int16_t saved_n_l_term;               /* Original value of p_new->n_l_term */
  u_int16_t saved_n_eq;                  /* Original value of p_new->u.btree.n_eq */
  u_int16_t saved_n_btm;                 /* Original value of p_new->u.btree.n_btm */
  u_int16_t saved_n_top;                 /* Original value of p_new->u.btree.n_top */
  u_int16_t saved_n_skip;                /* Original value of p_new->n_skip */
  u_int32_t saved_ws_flags;              /* Original value of p_new->ws_flags */
  log_est_t saved_n_out;              /* Original value of p_new->n_out */
  int rc = DBSQL_OK;             /* Return code */
  log_est_t r_size;                   /* Number of rows in the table */
  log_est_t r_log_size;                /* Logarithm of table size */
  where_term_t *top = 0, *btm = 0; /* Top and bottom range constraints */

  p_new = builder->p_new;
  assert( db->malloc_failed==0 || parser->n_err>0 );
  if( parser->n_err ){
    return parser->rc;
  }
  WHERETRACE(0x800, ("BEGIN %s.addBtreeIdx(%s), nEq=%d, nSkip=%d, rRun=%d\n",
                     probe->table->name,probe->name,
                     p_new->u.btree.n_eq, p_new->n_skip, p_new->r_run));

  assert( (p_new->ws_flags & WHERE_VIRTUALTABLE)==0 );
  assert( (p_new->ws_flags & WHERE_TOP_LIMIT)==0 );
  if( p_new->ws_flags & WHERE_BTM_LIMIT ){
    op_mask = WO_LT|WO_LE;
  }else{
    assert( p_new->u.btree.n_btm==0 );
    op_mask = WO_EQ|WO_IN|WO_GT|WO_GE|WO_LT|WO_LE|WO_ISNULL|WO_IS;
  }
  if( probe->unordered ){
    op_mask &= ~(WO_GT|WO_GE|WO_LT|WO_LE);
  }

  assert( p_new->u.btree.n_eq<probe->n_column );
  assert( p_new->u.btree.n_eq<probe->n_key_col
       || probe->idx_type!=DBSQL_IDXTYPE_PRIMARYKEY );

  saved_n_eq = p_new->u.btree.n_eq;
  saved_n_btm = p_new->u.btree.n_btm;
  saved_n_top = p_new->u.btree.n_top;
  saved_n_skip = p_new->n_skip;
  saved_n_l_term = p_new->n_l_term;
  saved_ws_flags = p_new->ws_flags;
  saved_prereq = p_new->prereq;
  saved_n_out = p_new->n_out;
  term = __where_scan_init(&scan, builder->wc, src->i_cursor, saved_n_eq,
                        op_mask, probe);
  p_new->r_setup = 0;
  r_size = probe->row_log_ests[0];
  r_log_size = __est_log(r_size);
  for(; rc==DBSQL_OK && term!=0; term = __where_scan_next(&scan)){
    u_int16_t op = term->e_operator;   /* Shorthand for term->e_operator */
    log_est_t r_cost_idx;
    log_est_t n_out_unadjusted;        /* n_out before IN() and WHERE adjustments */
    int n_in = 0;
#ifdef DBSQL_ENABLE_STAT4
    int n_rec_valid = builder->n_rec_valid;
#endif
    if( (op==WO_ISNULL || (term->wt_flags&TERM_VNULL)!=0)
     && __index_column_not_null(probe, saved_n_eq)
    ){
      continue; /* ignore IS [NOT] NULL constraints on NOT NULL columns */
    }
    if( term->prereq_right & p_new->mask_self ) continue;

    /* Do not allow the upper bound of a LIKE optimization range constraint
    ** to mix with a lower range bound from some other source */
    if( term->wt_flags & TERM_LIKEOPT && term->e_operator==WO_LT ) continue;

    if( (src->fg.jointype & (JT_LEFT|JT_LTORJ|JT_RIGHT))!=0
     && !__constraint_compatible_with_outer_join(term,src)
    ){
      continue;
    }
    if( IS_UNIQUE_INDEX(probe) && saved_n_eq==probe->n_key_col-1 ){
      builder->bld_flags1 |= DBSQL_BLDF1_UNIQUE;
    }else{
      builder->bld_flags1 |= DBSQL_BLDF1_INDEXED;
    }
    p_new->ws_flags = saved_ws_flags;
    p_new->u.btree.n_eq = saved_n_eq;
    p_new->u.btree.n_btm = saved_n_btm;
    p_new->u.btree.n_top = saved_n_top;
    p_new->n_l_term = saved_n_l_term;
    if( p_new->n_l_term>=p_new->n_l_slot
     && __where_loop_resize(db, p_new, p_new->n_l_term+1)
    ){
       break; /* OOM while trying to enlarge the p_new->l_terms array */
    }
    p_new->l_terms[p_new->n_l_term++] = term;
    p_new->prereq = (saved_prereq | term->prereq_right) & ~p_new->mask_self;

    assert( n_in_mul==0
        || (p_new->ws_flags & WHERE_COLUMN_NULL)!=0
        || (p_new->ws_flags & WHERE_COLUMN_IN)!=0
        || (p_new->ws_flags & WHERE_SKIPSCAN)!=0
    );

    if( op & WO_IN ){
      expr_t *expr = term->expr;
      if( EXPR_USE_X_SELECT(expr) ){
        /* "x IN (SELECT ...)":  TUNING: the SELECT returns 25 rows */
        int i;
        int redundant = 0;
        n_in = 46;  assert( 46==__log_est(25) );

        /* The expression may actually be of the form (x, y) IN (SELECT...).
        ** In this case there is a separate term for each of (x) and (y).
        ** However, the n_in multiplier should only be applied once, not once
        ** for each such term. The following loop checks that term is the
        ** first such term in use, and sets n_in back to 0 if it is not. */
        for(i=0; i<p_new->n_l_term-1; i++){
          if( p_new->l_terms[i] && p_new->l_terms[i]->expr==expr ){
            n_in = 0;
            if( p_new->l_terms[i]->u.x.i_field == term->u.x.i_field ){
              /* Detect when two or more columns of an index match the same 
              ** column of a vector IN operater, and avoid adding the column
              ** to the where_loop_t more than once.  See tag-20250707-01
              ** in test/rowvalue.test */
              redundant = 1;
            }
          }
        }
        if( redundant ){
          p_new->n_l_term--;
          continue;
        }
      }else if( ALWAYS(expr->x.list && expr->x.list->n_expr) ){
        /* "x IN (value, value, ...)" */
        n_in = __log_est(expr->x.list->n_expr);
      }
      if( probe->has_stat1 && r_log_size>=10 ){
        log_est_t M, log_k, x;
        /* Let:
        **   N = the total number of rows in the table
        **   K = the number of entries on the RHS of the IN operator
        **   M = the number of rows in the table that match terms to the
        **       to the left in the same index.  If the IN operator is on
        **       the left-most index column, M==N.
        **
        ** Given the definitions above, it is better to omit the IN operator
        ** from the index lookup and instead do a scan of the M elements,
        ** testing each scanned row against the IN operator separately, if:
        **
        **        M*log(K) < K*log(N)
        **
        ** Our estimates for M, K, and N might be inaccurate, so we build in
        ** a safety margin of 2 (log_est_t: 10) that favors using the IN operator
        ** with the index, as using an index has better worst-case behavior.
        ** If we do not have real dbsql_stat1 data, always prefer to use
        ** the index.  Do not bother with this optimization on very small
        ** tables (less than 2 rows) as it is pointless in that case.
        */
        M = probe->row_log_ests[saved_n_eq];
        log_k = __est_log(n_in);
        /* TUNING      v-----  10 to bias toward indexed IN */
        x = M + log_k + 10 - (n_in + r_log_size);
        if( x>=0 ){
          WHERETRACE(0x40,
            ("IN operator (N=%d M=%d logK=%d nIn=%d rLogSize=%d x=%d) "
             "prefers indexed lookup\n",
             saved_n_eq, M, log_k, n_in, r_log_size, x));
        }else if( n_in_mul<2 && OPTIMIZATION_ENABLED(db, DBSQL_SeekScan) ){
          WHERETRACE(0x40,
            ("IN operator (N=%d M=%d logK=%d nIn=%d rLogSize=%d x=%d"
             " nInMul=%d) prefers skip-scan\n",
             saved_n_eq, M, log_k, n_in, r_log_size, x, n_in_mul));
          p_new->ws_flags |= WHERE_IN_SEEKSCAN;
        }else{
          WHERETRACE(0x40,
            ("IN operator (N=%d M=%d logK=%d nIn=%d rLogSize=%d x=%d"
             " nInMul=%d) prefers normal scan\n",
             saved_n_eq, M, log_k, n_in, r_log_size, x, n_in_mul));
          continue;
        }
      }
      p_new->ws_flags |= WHERE_COLUMN_IN;
    }else if( op & (WO_EQ|WO_IS) ){
      int i_col = probe->columns[saved_n_eq];
      p_new->ws_flags |= WHERE_COLUMN_EQ;
      assert( saved_n_eq==p_new->u.btree.n_eq );
      if( i_col==XN_ROWID
       || (i_col>=0 && n_in_mul==0 && saved_n_eq==probe->n_key_col-1)
      ){
        if( i_col==XN_ROWID || probe->uniq_not_null
         || (probe->n_key_col==1 && probe->on_error && (op & WO_EQ))
        ){
          p_new->ws_flags |= WHERE_ONEROW;
        }else{
          p_new->ws_flags |= WHERE_UNQ_WANTED;
        }
      }
      if( scan.i_equiv>1 ) p_new->ws_flags |= WHERE_TRANSCONS;
    }else if( op & WO_ISNULL ){
      p_new->ws_flags |= WHERE_COLUMN_NULL;
    }else{
      int n_vec_len = __where_range_vector_len(
          parser, src->i_cursor, probe, saved_n_eq, term
      );
      if( op & (WO_GT|WO_GE) ){
        testcase( op & WO_GT );
        testcase( op & WO_GE );
        p_new->ws_flags |= WHERE_COLUMN_RANGE|WHERE_BTM_LIMIT;
        p_new->u.btree.n_btm = n_vec_len;
        btm = term;
        top = 0;
        if( term->wt_flags & TERM_LIKEOPT ){
          /* Range constraints that come from the LIKE optimization are
          ** always used in pairs. */
          top = &term[1];
          assert( (top-(term->wc->a))<term->wc->n_term );
          assert( top->wt_flags & TERM_LIKEOPT );
          assert( top->e_operator==WO_LT );
          if( __where_loop_resize(db, p_new, p_new->n_l_term+1) ) break; /* OOM */
          p_new->l_terms[p_new->n_l_term++] = top;
          p_new->ws_flags |= WHERE_TOP_LIMIT;
          p_new->u.btree.n_top = 1;
        }
      }else{
        assert( op & (WO_LT|WO_LE) );
        testcase( op & WO_LT );
        testcase( op & WO_LE );
        p_new->ws_flags |= WHERE_COLUMN_RANGE|WHERE_TOP_LIMIT;
        p_new->u.btree.n_top = n_vec_len;
        top = term;
        btm = (p_new->ws_flags & WHERE_BTM_LIMIT)!=0 ?
                       p_new->l_terms[p_new->n_l_term-2] : 0;
      }
    }

    /* At this point p_new->n_out is set to the number of rows expected to
    ** be visited by the index scan before considering term term, or the
    ** values of n_in and n_in_mul. In other words, assuming that all
    ** "x IN(...)" terms are replaced with "x = ?". This block updates
    ** the value of p_new->n_out to account for term (but not n_in/n_in_mul).  */
    assert( p_new->n_out==saved_n_out );
    if( p_new->ws_flags & WHERE_COLUMN_RANGE ){
      /* Adjust n_out using stat4 data. Or, if there is no stat4
      ** data, using some other estimate.  */
      __where_range_scan_est(parser, builder, btm, top, p_new);
    }else{
      int n_eq = ++p_new->u.btree.n_eq;
      assert( op & (WO_ISNULL|WO_EQ|WO_IN|WO_IS) );

      assert( p_new->n_out==saved_n_out );
      if( term->truth_prob<=0 && probe->columns[saved_n_eq]>=0 ){
        assert( (op & WO_IN) || n_in==0 );
        testcase( op & WO_IN );
        p_new->n_out += term->truth_prob;
        p_new->n_out -= n_in;
      }else{
#ifdef DBSQL_ENABLE_STAT4
        rowcnt_t n_out = 0;
        if( n_in_mul==0
         && probe->n_sample
         && ALWAYS(p_new->u.btree.n_eq<=probe->n_sample_col)
         && ((op & WO_IN)==0 || EXPR_USE_X_LIST(term->expr))
         && OPTIMIZATION_ENABLED(db, DBSQL_Stat4)
        ){
          expr_t *expr = term->expr;
          if( (op & (WO_EQ|WO_ISNULL|WO_IS))!=0 ){
            testcase( op & WO_EQ );
            testcase( op & WO_IS );
            testcase( op & WO_ISNULL );
            rc = __where_equal_scan_est(parser, builder, expr->right, &n_out);
          }else{
            rc = __where_in_scan_est(parser, builder, expr->x.list, &n_out);
          }
          if( rc==DBSQL_NOTFOUND ) rc = DBSQL_OK;
          if( rc!=DBSQL_OK ) break;          /* Jump out of the term loop */
          if( n_out ){
            p_new->n_out = __log_est(n_out);
            if( n_eq==1
             /* TUNING: Mark terms as "low selectivity" if they seem likely
             ** to be true for half or more of the rows in the table.
             ** See tag-202002240-1 */
             && p_new->n_out+10 > probe->row_log_ests[0]
            ){
#if WHERETRACE_ENABLED /* 0x01 */
              if( __where_trace & 0x20 ){
                __debug_printf(
                   "STAT4 determines term has low selectivity:\n");
                __where_term_print(term, 999);
              }
#endif
              term->wt_flags |= TERM_HIGHTRUTH;
              if( term->wt_flags & TERM_HEURTRUTH ){
                /* If the term has previously been used with an assumption of
                ** higher selectivity, then set the flag to rerun the
                ** loop computations. */
                builder->bld_flags2 |= DBSQL_BLDF2_2NDPASS;
              }
            }
            if( p_new->n_out>saved_n_out ) p_new->n_out = saved_n_out;
            p_new->n_out -= n_in;
          }
        }
        if( n_out==0 )
#endif
        {
          p_new->n_out += (probe->row_log_ests[n_eq] - probe->row_log_ests[n_eq-1]);
          if( op & WO_ISNULL ){
            /* TUNING: If there is no likelihood() value, assume that a
            ** "col IS NULL" expression matches twice as many rows
            ** as (col=?). */
            p_new->n_out += 10;
          }
        }
      }
    }

    /* Set r_cost_idx to the estimated cost of visiting selected rows in the
    ** index.  The estimate is the sum of two values:
    **   1.  The cost of doing one search-by-key to find the first matching
    **       entry
    **   2.  Stepping forward in the index p_new->n_out times to find all
    **       additional matching entries.
    */
    assert( src->s_tab->sz_tab_row>0 );
    if( probe->idx_type==DBSQL_IDXTYPE_IPK ){
      /* The probe->sz_idx_row is low for an IPK table since the interior
      ** pages are small.  Thus sz_idx_row gives a good estimate of seek cost.
      ** But the leaf pages are full-size, so probe->sz_idx_row would badly
      ** under-estimate the scanning cost. */
      r_cost_idx = p_new->n_out + 16;
    }else{
      r_cost_idx = p_new->n_out + 1 + (15*probe->sz_idx_row)/src->s_tab->sz_tab_row;
    }
    r_cost_idx = __log_est_add(r_log_size, r_cost_idx);

    /* Estimate the cost of running the loop.  If all data is coming
    ** from the index, then this is just the cost of doing the index
    ** lookup and scan.  But if some data is coming out of the main table,
    ** we also have to add in the cost of doing p_new->n_out searches to
    ** locate the row in the main table that corresponds to the index entry.
    */
    p_new->r_run = r_cost_idx;
    if( (p_new->ws_flags & (WHERE_IDX_ONLY|WHERE_IPK|WHERE_EXPRIDX))==0 ){
      p_new->r_run = __log_est_add(p_new->r_run, p_new->n_out + 16);
    }
    APPLY_COST_MULTIPLIER(p_new->r_run, probe->table->cost_mult);

    n_out_unadjusted = p_new->n_out;
    p_new->r_run += n_in_mul + n_in;
    p_new->n_out += n_in_mul + n_in;
    __where_loop_output_adjust(builder->wc, p_new, r_size);
    if( src->fg.from_exists ) p_new->n_out = 0;
    rc = __where_loop_insert(builder, p_new);

    if( p_new->ws_flags & WHERE_COLUMN_RANGE ){
      p_new->n_out = saved_n_out;
    }else{
      p_new->n_out = n_out_unadjusted;
    }

    if( (p_new->ws_flags & WHERE_TOP_LIMIT)==0
     && p_new->u.btree.n_eq<probe->n_column
     && (p_new->u.btree.n_eq<probe->n_key_col ||
          probe->idx_type!=DBSQL_IDXTYPE_PRIMARYKEY)
    ){
      if( p_new->u.btree.n_eq>3 ){
        __progress_check(parser);
      }
      __where_loop_add_btree_index(builder, src, probe, n_in_mul+n_in);
    }
    p_new->n_out = saved_n_out;
#ifdef DBSQL_ENABLE_STAT4
    builder->n_rec_valid = n_rec_valid;
#endif
  }
  p_new->prereq = saved_prereq;
  p_new->u.btree.n_eq = saved_n_eq;
  p_new->u.btree.n_btm = saved_n_btm;
  p_new->u.btree.n_top = saved_n_top;
  p_new->n_skip = saved_n_skip;
  p_new->ws_flags = saved_ws_flags;
  p_new->n_out = saved_n_out;
  p_new->n_l_term = saved_n_l_term;

  /* Consider using a skip-scan if there are no WHERE clause constraints
  ** available for the left-most terms of the index, and if the average
  ** number of repeats in the left-most terms is at least 18.
  **
  ** The magic number 18 is selected on the basis that scanning 17 rows
  ** is almost always quicker than an index seek (even though if the index
  ** contains fewer than 2^17 rows we assume otherwise in other parts of
  ** the code). And, even if it is not, it should not be too much slower.
  ** On the other hand, the extra seeks could end up being significantly
  ** more expensive.  */
  assert( 42==__log_est(18) );
  if( saved_n_eq==saved_n_skip
   && saved_n_eq+1<probe->n_key_col
   && saved_n_eq==p_new->n_l_term
   && probe->no_skip_scan==0
   && probe->has_stat1!=0
   && OPTIMIZATION_ENABLED(db, DBSQL_SkipScan)
   && probe->row_log_ests[saved_n_eq+1]>=42  /* TUNING: Minimum for skip-scan */
   && src->fg.from_exists==0
   && (rc = __where_loop_resize(db, p_new, p_new->n_l_term+1))==DBSQL_OK
  ){
    log_est_t n_iter;
    p_new->u.btree.n_eq++;
    p_new->n_skip++;
    p_new->l_terms[p_new->n_l_term++] = 0;
    p_new->ws_flags |= WHERE_SKIPSCAN;
    n_iter = probe->row_log_ests[saved_n_eq] - probe->row_log_ests[saved_n_eq+1];
    p_new->n_out -= n_iter;
    /* TUNING:  Because uncertainties in the estimates for skip-scan queries,
    ** add a 1.375 fudge factor to make skip-scan slightly less likely. */
    n_iter += 5;
    __where_loop_add_btree_index(builder, src, probe, n_iter + n_in_mul);
    p_new->n_out = saved_n_out;
    p_new->u.btree.n_eq = saved_n_eq;
    p_new->n_skip = saved_n_skip;
    p_new->ws_flags = saved_ws_flags;
  }

  WHERETRACE(0x800, ("END %s.addBtreeIdx(%s), nEq=%d, rc=%d\n",
                      probe->table->name, probe->name, saved_n_eq, rc));
  return rc;
}

/*
** Return True if it is possible that p_index might be useful in
** implementing the ORDER BY clause in builder.
**
** Return False if builder does not contain an ORDER BY clause or
** if there is no way for p_index to be useful in implementing that
** ORDER BY clause.
*/
static int __index_might_help_with_order_by(
  where_loop_builder_t *builder,
  index_t *p_index,
  int i_cursor
){
  expr_list_t *ob;
  expr_list_t *col_exprs;
  int ii, jj;

  if( p_index->unordered ) return 0;
  if( (ob = builder->w_info->order_by)==0 ) return 0;
  for(ii=0; ii<ob->n_expr; ii++){
    expr_t *expr = __expr_skip_collate_and_likely(ob->a[ii].expr);
    if( NEVER(expr==0) ) continue;
    if( (expr->op==TK_COLUMN || expr->op==TK_AGG_COLUMN) 
     && expr->i_table==i_cursor 
    ){
      if( expr->i_column<0 ) return 1;
      for(jj=0; jj<p_index->n_key_col; jj++){
        if( expr->i_column==p_index->columns[jj] ) return 1;
      }
    }else if( (col_exprs = p_index->col_exprs)!=0 ){
      for(jj=0; jj<p_index->n_key_col; jj++){
        if( p_index->columns[jj]!=XN_EXPR ) continue;
        if( __expr_compare_skip(expr,col_exprs->a[jj].expr,i_cursor)==0 ){
          return 1;
        }
      }
    }
  }
  return 0;
}

/* Check to see if a partial index with part_index_where can be used
** in the current query.  Return true if it can be and false if not.
*/
static int __where_usable_partial_index(
  int i_tab,             /* The table for which we want an index */
  u_int8_t jointype,          /* The JT_* flags on the join */
  where_clause_t *wc,     /* The WHERE clause of the query */
  expr_t *where          /* The WHERE clause from the partial index */
){
  int i;
  where_term_t *term;
  parser_t *parser;

  if( jointype & JT_LTORJ ) return 0;
  parser = wc->w_info->parser;
  while( where->op==TK_AND ){
    if( !__where_usable_partial_index(i_tab,jointype,wc,where->left) ) return 0;
    where = where->right;
  }
  for(i=0, term=wc->a; i<wc->n_term; i++, term++){
    expr_t *expr;
    expr = term->expr;
    if( (!EXPR_HAS_PROPERTY(expr, EP_OuterON) || expr->w.i_join==i_tab)
     && ((jointype & JT_OUTER)==0 || EXPR_HAS_PROPERTY(expr, EP_OuterON))
     && __expr_implies_expr(parser, expr, where, i_tab)
     && !__expr_implies_expr(parser, expr, where, -1)
     && (term->wt_flags & TERM_VNULL)==0
    ){
      return 1;
    }
  }
  return 0;
}

/*
** idx is an index containing expressions.  Check it see if any of the
** expressions in the index match the expr expression.
*/
static int __expr_is_covered_by_index(
  const expr_t *expr,
  const index_t *idx,
  int i_tab_cur
){
  int i;
  for(i=0; i<idx->n_column; i++){
    if( idx->columns[i]==XN_EXPR
     && __expr_compare(0, expr, idx->col_exprs->a[i].expr, i_tab_cur)==0
    ){
      return 1;
    }
  }
  return 0;
}

/*
** Structure passed to the __where_is_covering_index walker_t callback.
*/
typedef struct covering_index_check_t covering_index_check_t;
struct covering_index_check_t {
  index_t *idx;       /* The index */
  int i_tab_cur;       /* Cursor number for the corresponding table */
  u_int8_t expr;          /* Uses an indexed expression */
  u_int8_t unidx;         /* Uses an unindexed column not within an indexed expr */
};

/*
** Information passed in is walk->u.cov_idx_ck.  Call it ck.
**
** If the expr_t node references the table with cursor ck->i_tab_cur, then
** make sure that column is covered by the index ck->idx.  We know that
** all columns less than 63 (really BMS-1) are covered, so we don't need
** to check them.  But we do need to check any column at 63 or greater.
**
** If the index does not cover the column, then set walk->code to
** non-zero and return WRC_Abort to stop the search.
**
** If this node does not disprove that the index can be a covering index,
** then just return WRC_Continue, to continue the search.
**
** If ck->idx contains indexed expressions and one of those expressions
** matches expr, then prune the search.
*/
static int __where_is_covering_index_walk_callback(walker_t *walk, expr_t *expr){
  int i;                    /* Loop counter */
  const index_t *idx;        /* The index of interest */
  const int16_t *columns;      /* Columns contained in the index */
  u_int16_t n_column;              /* Number of columns in the index */
  covering_index_check_t *ck;  /* Info about this search */

  ck = walk->u.cov_idx_ck;
  idx = ck->idx;
  if( (expr->op==TK_COLUMN || expr->op==TK_AGG_COLUMN) ){
    /* if( expr->i_column<(BMS-1) && idx->has_expr==0 ) return WRC_Continue;*/
    if( expr->i_table!=ck->i_tab_cur ) return WRC_Continue;
    idx = walk->u.cov_idx_ck->idx;
    columns = idx->columns;
    n_column = idx->n_column;
    for(i=0; i<n_column; i++){
      if( columns[i]==expr->i_column ) return WRC_Continue;
    }
    ck->unidx = 1;
    return WRC_Abort;
  }else if( idx->has_expr
         && __expr_is_covered_by_index(expr, idx, walk->u.cov_idx_ck->i_tab_cur) ){
    ck->expr = 1;
    return WRC_Prune;
  }
  return WRC_Continue;
}


/*
** idx is an index that covers all of the low-number columns used by
** w_info->p_select (columns from 0 through 62) or an index that has
** expressions terms.  Hence, we cannot determine whether or not it is
** a covering index by using the col_used bitmasks.  We have to do a search
** to see if the index is covering.  This routine does that search.
**
** The return value is one of these:
**
**      0                The index is definitely not a covering index
**
**      WHERE_IDX_ONLY   The index is definitely a covering index
**
**      WHERE_EXPRIDX    The index is likely a covering index, but it is
**                       difficult to determine precisely because of the
**                       expressions that are indexed.  Score it as a
**                       covering index, but still keep the main table open
**                       just in case we need it.
**
** This routine is an optimization.  It is always safe to return zero.
** But returning one of the other two values when zero should have been
** returned can lead to incorrect bytecode and assertion faults.
*/
static DBSQL_NOINLINE u_int32_t __where_is_covering_index(
  where_info_t *w_info,     /* The WHERE clause context */
  index_t *idx,           /* index_t that is being tested */
  int i_tab_cur            /* Cursor for the table being indexed */
){
  int i, rc;
  struct covering_index_check_t ck;
  walker_t w;
  if( w_info->p_select==0 ){
    /* We don't have access to the full query, so we cannot check to see
    ** if idx is covering.  Assume it is not. */
    return 0;
  }
  if( idx->has_expr==0 ){
    for(i=0; i<idx->n_column; i++){
      if( idx->columns[i]>=BMS-1 ) break;
    }
    if( i>=idx->n_column ){
      /* idx does not index any columns greater than 62, but we know from
      ** col_mask that columns greater than 62 are used, so this is not a
      ** covering index */
      return 0;
    }
  }
  ck.idx = idx;
  ck.i_tab_cur = i_tab_cur;
  ck.expr = 0;
  ck.unidx = 0;
  memset(&w, 0, sizeof(w));
  w.expr_callback = __where_is_covering_index_walk_callback;
  w.select_callback = __select_walk_noop;
  w.u.cov_idx_ck = &ck;
  __walk_select(&w, w_info->p_select);
  if( ck.unidx ){
    rc = 0;
  }else if( ck.expr ){
    rc = WHERE_EXPRIDX;
  }else{
    rc = WHERE_IDX_ONLY;
  }
  return rc;
}

/*
** This is an __parser_add_cleanup() callback that is invoked to
** free the parser_t->idx_epr list when the parser_t object is destroyed.
*/
static void __where_indexed_expr_cleanup(DBSQL *db, void *object){
  indexed_expr_t **pp = (indexed_expr_t**)object;
  while( *pp!=0 ){
    indexed_expr_t *p = *pp;
    *pp = p->ie_next;
    __expr_delete(db, p->expr);
    __db_free_nn(db, p);
  }
}

/*
** This function is called for a partial index - one with a WHERE clause - in 
** two scenarios. In both cases, it determines whether or not the WHERE 
** clause on the index implies that a column of the table may be safely
** replaced by a constant expression. For example, in the following 
** SELECT:
**
**   CREATE INDEX i1 ON t1(b, c) WHERE a=<expr>;
**   SELECT a, b, c FROM t1 WHERE a=<expr> AND b=?;
**
** The "a" in the select-list may be replaced by <expr>, iff:
**
**    (a) <expr> is a constant expression, and
**    (b) The (a=<expr>) comparison uses the BINARY collation sequence, and
**    (c) column_t "a" has an affinity other than NONE or BLOB.
**
** If argument item is NULL, then mask must not be NULL. In this case this 
** function is being called as part of determining whether or not idx
** is a covering index. This function clears any bits in (*mask) 
** corresponding to columns that may be replaced by constants as described
** above.
**
** Otherwise, if item is not NULL, then this function is being called
** as part of coding a loop that uses index idx. In this case, add entries
** to the parser_t.idx_part_expr list for each column that can be replaced
** by a constant.
*/
static void __where_part_idx_expr(
  parser_t *parser,                  /* parser_t context */
  index_t *idx,                    /* Partial index being processed */
  expr_t *part,                    /* WHERE clause being processed */
  Bitmask *mask,                 /* Mask to clear bits in */
  int i_idx_cur,                    /* Cursor number for index */
  src_item_t *item                  /* The FROM clause entry for the table */
){
  assert( item==0 || (item->fg.jointype & JT_RIGHT)==0 );
  assert( (item==0 || mask==0) && (mask!=0 || item!=0) );

  if( part->op==TK_AND ){
    __where_part_idx_expr(parser, idx, part->right, mask, i_idx_cur, item);
    part = part->left;
  }

  if( (part->op==TK_EQ || part->op==TK_IS) ){
    expr_t *left = part->left;
    expr_t *right = part->right;
    u_int8_t aff;

    if( left->op!=TK_COLUMN ) return;
    if( !__expr_is_constant(0, right) ) return;
    if( !__is_binary(__expr_compare_coll_seq(parser, part)) ) return;
    if( left->i_column<0 ) return;
    aff = idx->table->cols[left->i_column].affinity;
    if( aff>=DBSQL_AFF_TEXT ){
      if( item ){
        DBSQL *db = parser->db;
        indexed_expr_t *p = (indexed_expr_t*)__db_malloc_raw(db, sizeof(*p));
        if( p ){
          int null_row = (item->fg.jointype&(JT_LEFT|JT_LTORJ))!=0;
          p->expr = __expr_dup(db, right, 0);
          p->i_data_cur = item->i_cursor;
          p->i_idx_cur = i_idx_cur;
          p->i_idx_col = left->i_column;
          p->maybe_null_row = null_row;
          p->ie_next = parser->idx_part_expr;
          p->aff = aff;
          parser->idx_part_expr = p;
          if( p->ie_next==0 ){
            void *arg = (void*)&parser->idx_part_expr;
            __parser_add_cleanup(parser, __where_indexed_expr_cleanup, arg);
          }
        }
      }else if( left->i_column<(BMS-1) ){
        *mask &= ~((Bitmask)1 << left->i_column);
      }
    }
  }
}


/*
** Add all where_loop_t objects for a single table of the join where the table
** is identified by builder->p_new->i_tab.  That table is guaranteed to be
** a b-tree table, not a virtual table.
**
** The costs (where_loop_t.r_run) of the b-tree loops added by this function
** are calculated as follows:
**
** For a full scan, assuming the table (or index) contains n_row rows:
**
**     cost = n_row * 3.0                    // full-table scan
**     cost = n_row * K                      // scan of covering index
**     cost = n_row * (K+3.0)                // scan of non-covering index
**
** where K is a value between 1.1 and 3.0 set based on the relative
** estimated average size of the index and table records.
**
** For an index scan, where n_visit is the number of index rows visited
** by the scan, and n_seek is the number of seek operations required on
** the index b-tree:
**
**     cost = n_seek * (log(n_row) + K * n_visit)          // covering index
**     cost = n_seek * (log(n_row) + (K+3.0) * n_visit)    // non-covering index
**
** Normally, n_seek is 1. n_seek values greater than 1 come about if the
** WHERE clause includes "x IN (....)" terms used in place of "x=?". Or when
** implicit "x IN (SELECT x FROM tbl)" terms are added for skip-scans.
**
** The estimated values (n_row, n_visit, n_seek) often contain a large amount
** of uncertainty.  For this reason, scoring is designed to pick plans that
** "do the least harm" if the estimates are inaccurate.  For example, a
** log(n_row) factor is omitted from a non-covering index scan in order to
** bias the scoring in favor of using an index, since the worst-case
** performance of using an index is far better than the worst-case performance
** of a full table scan.
*/
static int __where_loop_add_btree(
  where_loop_builder_t *builder, /* WHERE clause information */
  Bitmask m_prereq             /* Extra prerequisites for using this table */
){
  where_info_t *w_info;          /* WHERE analysis context */
  index_t *probe;              /* An index we are evaluating */
  index_t s_pk;                  /* A fake index object for the primary key */
  log_est_t row_est_pks[2];       /* The row_log_ests[] value for the s_pk index */
  int16_t column_pks = -1;        /* The columns[] value for the s_pk index */
  src_list_t *tab_list;          /* The FROM clause */
  src_item_t *src;              /* The FROM clause btree term to add */
  where_loop_t *p_new;            /* Template where_loop_t object */
  int rc = DBSQL_OK;         /* Return code */
  int i_sort_idx = 1;           /* index_t number */
  int b;                      /* A boolean value */
  log_est_t r_size;               /* number of rows in the table */
  where_clause_t *wc;           /* The parsed WHERE clause */
  table_t *tab;                /* table_t being queried */
 
  p_new = builder->p_new;
  w_info = builder->w_info;
  tab_list = w_info->tab_list;
  src = tab_list->a + p_new->i_tab;
  tab = src->s_tab;
  wc = builder->wc;
  assert( !IS_VIRTUAL(src->s_tab) );

  if( src->fg.is_indexed_by ){
    assert( src->fg.is_cte==0 );
    /* An INDEXED BY clause specifies a particular index to use */
    probe = src->u2.ib_index;
  }else if( !HAS_ROWID(tab) ){
    probe = tab->p_index;
  }else{
    /* There is no INDEXED BY clause.  Create a fake index_t object in local
    ** variable s_pk to represent the rowid primary key index.  Make this
    ** fake index the first in a chain of index_t objects with all of the real
    ** indices to follow */
    index_t *first;                  /* First of real indices on the table */
    memset(&s_pk, 0, sizeof(index_t));
    s_pk.n_key_col = 1;
    s_pk.n_column = 1;
    s_pk.columns = &column_pks;
    s_pk.row_log_ests = row_est_pks;
    s_pk.on_error = OE_Replace;
    s_pk.table = tab;
    s_pk.sz_idx_row = 3;  /* TUNING: Interior rows of IPK table are very small */
    s_pk.idx_type = DBSQL_IDXTYPE_IPK;
    row_est_pks[0] = tab->n_row_log_est;
    row_est_pks[1] = 0;
    first = src->s_tab->p_index;
    if( src->fg.not_indexed==0 ){
      /* The real indices of the table are only considered if the
      ** NOT INDEXED qualifier is omitted from the FROM clause */
      s_pk.next = first;
    }
    probe = &s_pk;
  }
  r_size = tab->n_row_log_est;

#ifndef DBSQL_OMIT_AUTOMATIC_INDEX
  /* Automatic indexes */
  if( !builder->or_set      /* Not part of an OR optimization */
   && (w_info->wctrl_flags & (WHERE_RIGHT_JOIN|WHERE_OR_SUBCLAUSE))==0
   && (w_info->parser->db->flags & DBSQL_AutoIndex)!=0
   && !src->fg.is_indexed_by  /* Has no INDEXED BY clause */
   && !src->fg.not_indexed   /* Has no NOT INDEXED clause */
   && !src->fg.is_correlated /* Not a correlated subquery */
   && !src->fg.is_recursive  /* Not a recursive common table expression. */
   && (src->fg.jointype & JT_RIGHT)==0 /* Not the right tab of a RIGHT JOIN */
  ){
    /* Generate auto-index __where_loops */
    log_est_t r_log_size;         /* Logarithm of the number of rows in the table */
    where_term_t *term;
    where_term_t *wc_end = wc->a + wc->n_term;
    r_log_size = __est_log(r_size);
    for(term=wc->a; rc==DBSQL_OK && term<wc_end; term++){
      if( term->prereq_right & p_new->mask_self ) continue;
      if( __term_can_drive_index(term, src, 0) ){
        p_new->u.btree.n_eq = 1;
        p_new->n_skip = 0;
        p_new->u.btree.p_index = 0;
        p_new->n_l_term = 1;
        p_new->l_terms[0] = term;
        /* TUNING: One-time cost for computing the automatic index is
        ** estimated to be X*N*log2(N) where N is the number of rows in
        ** the table being indexed and where X is 7 (log_est_t=28) for normal
        ** tables or 0.5 (log_est_t=-10) for views and subqueries.  The value
        ** of X is smaller for views and subqueries so that the query planner
        ** will be more aggressive about generating automatic indexes for
        ** those objects, since there is no opportunity to add schema
        ** indexes on subqueries and views. */
        p_new->r_setup = r_log_size + r_size;
        if( !IsView(tab) && (tab->tab_flags & TF_Ephemeral)==0 ){
          p_new->r_setup += 28;
        }else{
          p_new->r_setup -= 25;  /* Greatly reduced setup cost for auto indexes
                               ** on ephemeral materializations of views */
        }
        APPLY_COST_MULTIPLIER(p_new->r_setup, tab->cost_mult);
        if( p_new->r_setup<0 ) p_new->r_setup = 0;
        /* TUNING: Each index lookup yields 20 rows in the table.  This
        ** is more than the usual guess of 10 rows, since we have no way
        ** of knowing how selective the index will ultimately be.  It would
        ** not be unreasonable to make this value much larger. */
        p_new->n_out = 43;  assert( 43==__log_est(20) );
        p_new->r_run = __log_est_add(r_log_size,p_new->n_out);
        p_new->ws_flags = WHERE_AUTO_INDEX;
        p_new->prereq = m_prereq | term->prereq_right;
        rc = __where_loop_insert(builder, p_new);
      }
    }
  }
#endif /* DBSQL_OMIT_AUTOMATIC_INDEX */

  /* Loop over all indices. If there was an INDEXED BY clause, then only
  ** consider index probe.  */
  for(; rc==DBSQL_OK && probe;
      probe=(src->fg.is_indexed_by ? 0 : probe->next), i_sort_idx++
  ){
    if( probe->part_idx_where!=0
     && !__where_usable_partial_index(src->i_cursor, src->fg.jointype, wc,
                                 probe->part_idx_where)
    ){
      testcase( p_new->i_tab!=src->i_cursor );  /* See ticket [98d973b8f5] */
      continue;  /* Partial index inappropriate for this query */
    }
    if( probe->no_query ) continue;
    r_size = probe->row_log_ests[0];
    p_new->u.btree.n_eq = 0;
    p_new->u.btree.n_btm = 0;
    p_new->u.btree.n_top = 0;
    p_new->u.btree.n_distinct_col = 0;
    p_new->n_skip = 0;
    p_new->n_l_term = 0;
    p_new->i_sort_idx = 0;
    p_new->r_setup = 0;
    p_new->prereq = m_prereq;
    p_new->n_out = r_size;
    p_new->u.btree.p_index = probe;
    p_new->u.btree.order_by = 0;
    b = __index_might_help_with_order_by(builder, probe, src->i_cursor);

    /* The ONEPASS_DESIRED flags never occurs together with ORDER BY */
    assert( (w_info->wctrl_flags & WHERE_ONEPASS_DESIRED)==0 || b==0 );
    if( probe->idx_type==DBSQL_IDXTYPE_IPK ){
      /* Integer primary key index */
      p_new->ws_flags = WHERE_IPK;

      /* Full table scan */
      p_new->i_sort_idx = b ? i_sort_idx : 0;
      /* TUNING: Cost of full table scan is 3.0*N.  The 3.0 factor is an
      ** extra cost designed to discourage the use of full table scans,
      ** since index lookups have better worst-case performance if our
      ** stat guesses are wrong.  Reduce the 3.0 penalty slightly
      ** (to 2.75) if we have valid STAT4 information for the table.
      ** At 2.75, a full table scan is preferred over using an index on
      ** a column with just two distinct values where each value has about
      ** an equal number of appearances.  Without STAT4 data, we still want
      ** to use an index in that case, since the constraint might be for
      ** the scarcer of the two values, and in that case an index lookup is
      ** better.
      */
#ifdef DBSQL_ENABLE_STAT4
      p_new->r_run = r_size + 16 - 2*((tab->tab_flags & TF_HasStat4)!=0);
#else
      p_new->r_run = r_size + 16;
#endif
      APPLY_COST_MULTIPLIER(p_new->r_run, tab->cost_mult);
      __where_loop_output_adjust(wc, p_new, r_size);
      if( src->fg.is_subquery ){
        if( src->fg.via_coroutine ) p_new->ws_flags |= WHERE_COROUTINE;
        /* Do not set btree.order_by for a recursive CTE. In this case
        ** the ORDER BY clause does not determine the overall order that
        ** rows are emitted from the CTE in.  */
        if( (src->u4.subq->p_select->sel_flags & SF_Recursive)==0 ){
          p_new->u.btree.order_by = src->u4.subq->p_select->order_by;
        }
      }else if( src->fg.from_exists ){
        p_new->n_out = 0;
      }
      rc = __where_loop_insert(builder, p_new);
      p_new->n_out = r_size;
      if( rc ) break;
    }else{
      Bitmask m;
      if( probe->is_covering ){
        m = 0;
        p_new->ws_flags = WHERE_IDX_ONLY | WHERE_INDEXED;
      }else{
        m = src->col_used & probe->col_not_idxed;
        if( probe->part_idx_where ){
          __where_part_idx_expr(
              w_info->parser, probe, probe->part_idx_where, &m, 0, 0
          );
        }
        p_new->ws_flags = WHERE_INDEXED;
        if( m==TOPBIT || (probe->has_expr && !probe->has_v_col && m!=0) ){
          u_int32_t is_cov = __where_is_covering_index(w_info, probe, src->i_cursor);
          if( is_cov==0 ){
            WHERETRACE(0x200,
               ("-> %s is not a covering index"
                " according to whereIsCoveringIndex()\n", probe->name));
            assert( m!=0 );
          }else{
            m = 0;
            p_new->ws_flags |= is_cov;
            if( is_cov & WHERE_IDX_ONLY ){
              WHERETRACE(0x200,
                 ("-> %s is a covering expression index"
                  " according to whereIsCoveringIndex()\n", probe->name));
            }else{
              assert( is_cov==WHERE_EXPRIDX );
              WHERETRACE(0x200,
                 ("-> %s might be a covering expression index"
                  " according to whereIsCoveringIndex()\n", probe->name));
            }
          }
        }else if( m==0 
           && (HAS_ROWID(tab) || w_info->p_select!=0 || __fault_sim(700))
        ){
          WHERETRACE(0x200,
             ("-> %s is a covering index according to bitmasks\n",
             probe->name, m==0 ? "is" : "is not"));
          p_new->ws_flags = WHERE_IDX_ONLY | WHERE_INDEXED;
        }
      }

      /* Full scan via index */
      if( b
       || !HAS_ROWID(tab)
       || probe->part_idx_where!=0
       || src->fg.is_indexed_by
       || ( m==0
         && probe->unordered==0
         && (probe->sz_idx_row<tab->sz_tab_row)
         && (w_info->wctrl_flags & WHERE_ONEPASS_DESIRED)==0
         && __global_config.use_cis
         && OPTIMIZATION_ENABLED(w_info->parser->db, DBSQL_CoverIdxScan)
          )
      ){
        p_new->i_sort_idx = b ? i_sort_idx : 0;

        /* The cost of visiting the index rows is N*K, where K is
        ** between 1.1 and 3.0, depending on the relative sizes of the
        ** index and table rows. */
        p_new->r_run = r_size + 1 + (15*probe->sz_idx_row)/tab->sz_tab_row;
        if( m!=0 ){
          /* If this is a non-covering index scan, add in the cost of
          ** doing table lookups.  The cost will be 3x the number of
          ** lookups.  Take into account WHERE clause terms that can be
          ** satisfied using just the index, and that do not require a
          ** table lookup. */
          log_est_t n_lookup = r_size + 16;  /* Base cost:  N*3 */
          int ii;
          int i_cur = src->i_cursor;
          where_clause_t *wc2 = &w_info->s_wc;
          for(ii=0; ii<wc2->n_term; ii++){
            where_term_t *term = &wc2->a[ii];
            if( !__expr_covered_by_index(term->expr, i_cur, probe) ){
              break;
            }
            /* term can be evaluated using just the index.  So reduce
            ** the expected number of table lookups accordingly */
            if( term->truth_prob<=0 ){
              n_lookup += term->truth_prob;
            }else{
              n_lookup--;
              if( term->e_operator & (WO_EQ|WO_IS) ) n_lookup -= 19;
            }
          }
         
          p_new->r_run = __log_est_add(p_new->r_run, n_lookup);
        }
        APPLY_COST_MULTIPLIER(p_new->r_run, tab->cost_mult);
        __where_loop_output_adjust(wc, p_new, r_size);
        if( (src->fg.jointype & JT_RIGHT)!=0 && probe->col_exprs ){
          /* Do not do an SCAN of a index-on-expression in a RIGHT JOIN
          ** because the cursor used to access the index might not be
          ** positioned to the correct row during the right-join no-match
          ** loop. */
        }else{
          if( src->fg.from_exists ) p_new->n_out = 0;
          rc = __where_loop_insert(builder, p_new);
        }
        p_new->n_out = r_size;
        if( rc ) break;
      }
    }

    builder->bld_flags1 = 0;
    rc = __where_loop_add_btree_index(builder, src, probe, 0);
    if( builder->bld_flags1==DBSQL_BLDF1_INDEXED ){
      /* If a non-unique index is used, or if a prefix of the key for
      ** unique index is used (making the index functionally non-unique)
      ** then the dbsql_stat1 data becomes important for scoring the
      ** plan */
      tab->tab_flags |= TF_MaybeReanalyze;
    }
#ifdef DBSQL_ENABLE_STAT4
    __stat4_probe_free(builder->rec);
    builder->n_rec_valid = 0;
    builder->rec = 0;
#endif
  }
  return rc;
}

#ifndef DBSQL_OMIT_VIRTUALTABLE

/*
** Return true if term is a virtual table LIMIT or OFFSET term.
*/
static int __is_limit_term(where_term_t *term){
  assert( term->e_operator==WO_AUX || term->match_op==0 );
  return term->match_op>=DBSQL_INDEX_CONSTRAINT_LIMIT
      && term->match_op<=DBSQL_INDEX_CONSTRAINT_OFFSET;
}

/*
** Return true if the first n_cons constraints in the usage array are
** marked as in-use (have argv_index>0). False otherwise.
*/
static int __all_constraints_used(
  struct dbsql_index_constraint_usage_t *__a_usage, 
  int n_cons
){
  int ii;
  for(ii=0; ii<n_cons; ii++){
    if( __a_usage[ii].argv_index<=0 ) return 0;
  }
  return 1;
}

/*
** Argument idx_info is already populated with all constraints that may
** be used by the virtual table identified by builder->p_new->i_tab. This
** function marks a subset of those constraints usable, invokes the
** best_index method and adds the returned plan to builder.
**
** A constraint is marked usable if:
**
**   * Argument m_usable indicates that its prerequisites are available, and
**
**   * It is not one of the operators specified in the m_exclude mask passed
**     as the fourth argument (which in practice is either WO_IN or 0).
**
** Argument m_prereq is a mask of tables that must be scanned before the
** virtual table in question. These are added to the plans prerequisites
** before it is added to builder.
**
** Output parameter *pb_in is set to true if the plan added to builder
** uses one or more WO_IN terms, or false otherwise.
*/
static int __where_loop_add_virtual_one(
  where_loop_builder_t *builder,
  Bitmask m_prereq,                /* Mask of tables that must be used. */
  Bitmask m_usable,                /* Mask of usable tables */
  u_int16_t m_exclude,                   /* Exclude terms using these operators */
  dbsql_index_info_t *idx_info,   /* Populated object for best_index */
  u_int16_t m_no_omit,                    /* Do not omit these constraints */
  int *pb_in,                      /* OUT: True if plan uses an IN(...) op */
  int *pb_retry_limit               /* OUT: Retry without LIMIT/OFFSET */
){
  where_clause_t *wc = builder->wc;
  hidden_index_info_t *hidden = (hidden_index_info_t*)&idx_info[1];
  struct dbsql_index_constraint_t *idx_cons;
  struct dbsql_index_constraint_usage_t *usage = idx_info->constraint_usages;
  int i;
  int mx_term;
  int rc = DBSQL_OK;
  where_loop_t *p_new = builder->p_new;
  parser_t *parser = builder->w_info->parser;
  src_item_t *src = &builder->w_info->tab_list->a[p_new->i_tab];
  int n_constraint = idx_info->n_constraint;

  assert( (m_usable & m_prereq)==m_prereq );
  *pb_in = 0;
  p_new->prereq = m_prereq;

  /* Set the usable flag on the subset of constraints identified by
  ** arguments m_usable and m_exclude. */
  idx_cons = *(struct dbsql_index_constraint_t**)&idx_info->constraints;
  for(i=0; i<n_constraint; i++, idx_cons++){
    where_term_t *term = __term_from_where_clause(wc, idx_cons->i_term_offset);
    idx_cons->usable = 0;
    if( (term->prereq_right & m_usable)==term->prereq_right
     && (term->e_operator & m_exclude)==0
     && (pb_retry_limit || !__is_limit_term(term))
    ){
      idx_cons->usable = 1;
    }
  }

  /* Initialize the output fields of the dbsql_index_info_t structure */
  memset(usage, 0, sizeof(usage[0])*n_constraint);
  assert( idx_info->need_to_free_idx_str==0 );
  idx_info->idx_str = 0;
  idx_info->idx_num = 0;
  idx_info->order_by_consumed = 0;
  idx_info->estimated_cost = DBSQL_BIG_DBL / (double)2;
  idx_info->estimated_rows = 25;
  idx_info->idx_flags = 0;
  hidden->m_handle_in = 0;

  /* Invoke the virtual table best_index() method */
  rc = __vtab_best_index(parser, src->s_tab, idx_info);
  if( rc ){
    if( rc==DBSQL_CONSTRAINT ){
      /* If the best_index method returns DBSQL_CONSTRAINT, that means
      ** that the particular combination of parameters provided is unusable.
      ** Make no entries in the loop table.
      */
      WHERETRACE(0xffffffff, ("  ^^^^--- non-viable plan rejected!\n"));
      __free_idx_str(idx_info);
      return DBSQL_OK;
    }
    return rc;
  }

  mx_term = -1;
  assert( p_new->n_l_slot>=n_constraint );
  memset(p_new->l_terms, 0, sizeof(p_new->l_terms[0])*n_constraint );
  memset(&p_new->u.vtab, 0, sizeof(p_new->u.vtab));
  idx_cons = *(struct dbsql_index_constraint_t**)&idx_info->constraints;
  for(i=0; i<n_constraint; i++, idx_cons++){
    int i_term;
    if( (i_term = usage[i].argv_index - 1)>=0 ){
      where_term_t *term;
      int j = idx_cons->i_term_offset;
      if( i_term>=n_constraint
       || j<0
       || (term = __term_from_where_clause(wc, j))==0
       || p_new->l_terms[i_term]!=0
       || idx_cons->usable==0
      ){
        __error_msg(parser,"%s.xBestIndex malfunction",src->s_tab->name);
        __free_idx_str(idx_info);
        return DBSQL_ERROR;
      }
      testcase( i_term==n_constraint-1 );
      testcase( j==0 );
      testcase( j==wc->n_term-1 );
      p_new->prereq |= term->prereq_right;
      assert( i_term<p_new->n_l_slot );
      p_new->l_terms[i_term] = term;
      if( i_term>mx_term ) mx_term = i_term;
      testcase( i_term==15 );
      testcase( i_term==16 );
      if( usage[i].omit ){
        if( i<16 && ((1<<i)&m_no_omit)==0 ){
          testcase( i!=i_term );
          p_new->u.vtab.omit_mask |= 1<<i_term;
        }else{
          testcase( i!=i_term );
        }
        if( term->match_op==DBSQL_INDEX_CONSTRAINT_OFFSET ){
          p_new->u.vtab.omit_offset = 1;
        }
      }
      if( SMASKBIT32(i) & hidden->m_handle_in ){
        p_new->u.vtab.m_handle_in |= MASKBIT32(i_term);
      }else if( (term->e_operator & WO_IN)!=0 ){
        /* A virtual table that is constrained by an IN clause may not
        ** consume the ORDER BY clause because (1) the order of IN terms
        ** is not necessarily related to the order of output terms and
        ** (2) Multiple outputs from a single IN value will not merge
        ** together.  */
        idx_info->order_by_consumed = 0;
        idx_info->idx_flags &= ~DBSQL_INDEX_SCAN_UNIQUE;
        *pb_in = 1; assert( (m_exclude & WO_IN)==0 );
      }

      /* Unless pb_retry_limit is non-NULL, there should be no LIMIT/OFFSET
      ** terms. And if there are any, they should follow all other terms. */
      assert( pb_retry_limit || !__is_limit_term(term) );
      assert( !__is_limit_term(term) || i>=n_constraint-2 );
      assert( !__is_limit_term(term) || i==n_constraint-1 || __is_limit_term(term+1) );

      if( __is_limit_term(term) && (*pb_in || !__all_constraints_used(usage, i)) ){
        /* If there is an IN(...) term handled as an == (separate call to
        ** filter for each value on the RHS of the IN) and a LIMIT or
        ** OFFSET term handled as well, the plan is unusable. Similarly,
        ** if there is a LIMIT/OFFSET and there are other unused terms,
        ** the plan cannot be used. In these cases set variable *pb_retry_limit
        ** to true to tell the caller to retry with LIMIT and OFFSET 
        ** disabled. */
        __free_idx_str(idx_info);
        *pb_retry_limit = 1;
        return DBSQL_OK;
      }
    }
  }

  p_new->n_l_term = mx_term+1;
  for(i=0; i<=mx_term; i++){
    if( p_new->l_terms[i]==0 ){
      /* The non-zero argv_idx values must be contiguous.  Raise an
      ** error if they are not */
      __error_msg(parser,"%s.xBestIndex malfunction",src->s_tab->name);
      __free_idx_str(idx_info);
      return DBSQL_ERROR;
    }
  }
  assert( p_new->n_l_term<=p_new->n_l_slot );
  p_new->u.vtab.idx_num = idx_info->idx_num;
  p_new->u.vtab.need_free = idx_info->need_to_free_idx_str;
  idx_info->need_to_free_idx_str = 0;
  p_new->u.vtab.idx_str = idx_info->idx_str;
  p_new->u.vtab.is_ordered = (int8_t)(idx_info->order_by_consumed ?
      idx_info->n_order_by : 0);
  p_new->u.vtab.idx_num_hex = (idx_info->idx_flags&DBSQL_INDEX_SCAN_HEX)!=0;
  p_new->r_setup = 0;
  p_new->r_run = __log_est_from_double(idx_info->estimated_cost);
  p_new->n_out = __log_est(idx_info->estimated_rows);

  /* Set the WHERE_ONEROW flag if the best_index() method indicated
  ** that the scan will visit at most one row. Clear it otherwise. */
  if( idx_info->idx_flags & DBSQL_INDEX_SCAN_UNIQUE ){
    p_new->ws_flags |= WHERE_ONEROW;
  }else{
    p_new->ws_flags &= ~WHERE_ONEROW;
  }
  rc = __where_loop_insert(builder, p_new);
  if( p_new->u.vtab.need_free ){
    dbsql_free(p_new->u.vtab.idx_str);
    p_new->u.vtab.need_free = 0;
  }
  WHERETRACE(0xffffffff, ("  bIn=%d prereqIn=%04llx prereqOut=%04llx\n",
                      *pb_in, (u_int64_t)m_prereq,
                      (u_int64_t)(p_new->prereq & ~m_prereq)));

  return rc;
}

/*
** Return the collating sequence for a constraint passed into best_index.
**
** idx_info must be an dbsql_index_info_t structure passed into best_index.
** This routine depends on there being a hidden_index_info_t structure immediately
** following the dbsql_index_info_t structure.
**
** Return a pointer to the collation name:
**
**    1. If there is an explicit COLLATE operator on the constraint, return it.
**
**    2. Else, if the column has an alternative collation, return that.
**
**    3. Otherwise, return "BINARY".
*/
const char *dbsql_vtab_collation(dbsql_index_info_t *idx_info, int i_cons){
  hidden_index_info_t *hidden = (hidden_index_info_t*)&idx_info[1];
  const char *ret = 0;
  if( i_cons>=0 && i_cons<idx_info->n_constraint ){
    coll_seq_t *c = 0;
    int i_term = idx_info->constraints[i_cons].i_term_offset;
    expr_t *x = __term_from_where_clause(hidden->wc, i_term)->expr;
    if( x->left ){
      c = __expr_compare_coll_seq(hidden->parser, x);
    }
    ret = (c ? c->name : __str_binary);
  }
  return ret;
}

/*
** Return true if constraint i_cons is really an IN(...) constraint, or
** false otherwise. If i_cons is an IN(...) constraint, set (if handle!=0)
** or clear (if handle==0) the flag to handle it using an iterator.
*/
int dbsql_vtab_in(dbsql_index_info_t *idx_info, int i_cons, int handle){
  hidden_index_info_t *hidden = (hidden_index_info_t*)&idx_info[1];
  u_int32_t m = SMASKBIT32(i_cons);
  if( m & hidden->m_in ){
    if( handle==0 ){
      hidden->m_handle_in &= ~m;
    }else if( handle>0 ){
      hidden->m_handle_in |= m;
    }
    return 1;
  }
  return 0;
}

/*
** This interface is callable from within the best_index callback only.
**
** If possible, set (*val) to point to an object containing the value
** on the right-hand-side of constraint i_cons.
*/
int dbsql_vtab_rhs_value(
  dbsql_index_info_t *idx_info,   /* Copy of first argument to best_index */
  int i_cons,                      /* Constraint for which RHS is wanted */
  dbsql_value_t **pp_val           /* Write value extracted here */
){
  hidden_index_info_t *h = (hidden_index_info_t*)&idx_info[1];
  dbsql_value_t *val = 0;
  int rc = DBSQL_OK;
  if( i_cons<0 || i_cons>=idx_info->n_constraint ){
    rc = DBSQL_MISUSE_BKPT; /* EV: R-30545-25046 */
  }else{
    if( h->rhses[i_cons]==0 ){
      where_term_t *term = __term_from_where_clause(
          h->wc, idx_info->constraints[i_cons].i_term_offset
      );
      rc = __value_from_expr(
          h->parser->db, term->expr->right, ENC(h->parser->db),
          DBSQL_AFF_BLOB, &h->rhses[i_cons]
      );
      testcase( rc!=DBSQL_OK );
    }
    val = h->rhses[i_cons];
  }
  *pp_val = val;

  if( rc==DBSQL_OK && val==0 ){  /* IMP: R-19933-32160 */
    rc = DBSQL_NOTFOUND;          /* IMP: R-36424-56542 */
  }

  return rc;
}

/*
** Return true if ORDER BY clause may be handled as DISTINCT.
*/
int dbsql_vtab_distinct(dbsql_index_info_t *idx_info){
  hidden_index_info_t *hidden = (hidden_index_info_t*)&idx_info[1];
  assert( hidden->distinct>=0 && hidden->distinct<=3 );
  return hidden->distinct;
}

/*
** Cause the prepared statement that is associated with a call to
** best_index to potentially use all schemas.  If the statement being
** prepared is read-only, then just start read transactions on all
** schemas.  But if this is a write operation, start writes on all
** schemas.
**
** This is used by the (built-in) dbsql_dbpage virtual table.
*/
void __vtab_uses_all_schemas(parser_t *parser){
  int n_db = parser->db->n_db;
  int i;
  for(i=0; i<n_db; i++){
    __code_verify_schema(parser, i);
  }
  if( DB_MASK_NON_ZERO(parser->write_mask) ){
    for(i=0; i<n_db; i++){
      __begin_write_operation(parser, 0, i);
    }
  }
}

/*
** Add all where_loop_t objects for a table of the join identified by
** builder->p_new->i_tab.  That table is guaranteed to be a virtual table.
**
** If there are no LEFT or CROSS JOIN joins in the query, both m_prereq and
** m_unusable are set to 0. Otherwise, m_prereq is a mask of all FROM clause
** entries that occur before the virtual table in the FROM clause and are
** separated from it by at least one LEFT or CROSS JOIN. Similarly, the
** m_unusable mask contains all FROM clause entries that occur after the
** virtual table and are separated from it by at least one LEFT or
** CROSS JOIN.
**
** For example, if the query were:
**
**   ... FROM t1, t2 LEFT JOIN t3, t4, vt CROSS JOIN t5, t6;
**
** then m_prereq corresponds to (t1, t2) and m_unusable to (t5, t6).
**
** All the tables in m_prereq must be scanned before the current virtual
** table. So any terms for which all prerequisites are satisfied by
** m_prereq may be specified as "usable" in all calls to best_index.
** Conversely, all tables in m_unusable must be scanned after the current
** virtual table, so any terms for which the prerequisites overlap with
** m_unusable should always be configured as "not-usable" for best_index.
*/
static int __where_loop_add_virtual(
  where_loop_builder_t *builder,  /* WHERE clause information */
  Bitmask m_prereq,             /* Tables that must be scanned before this one */
  Bitmask m_unusable            /* Tables that must be scanned after this one */
){
  int rc = DBSQL_OK;          /* Return code */
  where_info_t *w_info;           /* WHERE analysis context */
  parser_t *parser;               /* The parsing context */
  where_clause_t *wc;            /* The WHERE clause */
  src_item_t *src;               /* The FROM clause term to search */
  dbsql_index_info_t *p;       /* Object to pass to best_index() */
  int n_constraint;             /* Number of constraints in p */
  int in;                     /* True if plan uses IN(...) operator */
  where_loop_t *p_new;
  Bitmask m_best;               /* Tables used by best possible plan */
  u_int16_t m_no_omit;
  int retry = 0;              /* True to retry with LIMIT/OFFSET disabled */

  assert( (m_prereq & m_unusable)==0 );
  w_info = builder->w_info;
  parser = w_info->parser;
  wc = builder->wc;
  p_new = builder->p_new;
  src = &w_info->tab_list->a[p_new->i_tab];
  assert( IS_VIRTUAL(src->s_tab) );
  p = __allocate_index_info(w_info, wc, m_unusable, src, &m_no_omit);
  if( p==0 ) return DBSQL_NOMEM_BKPT;
  p_new->r_setup = 0;
  p_new->ws_flags = WHERE_VIRTUALTABLE;
  p_new->n_l_term = 0;
  p_new->u.vtab.need_free = 0;
  n_constraint = p->n_constraint;
  if( __where_loop_resize(parser->db, p_new, n_constraint) ){
    __free_index_info(parser->db, p);
    return DBSQL_NOMEM_BKPT;
  }

  /* First call best_index() with all constraints usable. */
  WHERETRACE(0x800, ("BEGIN %s.addVirtual()\n", src->s_tab->name));
  WHERETRACE(0x800, ("  VirtualOne: all usable\n"));
  rc = __where_loop_add_virtual_one(
      builder, m_prereq, ALLBITS, 0, p, m_no_omit, &in, &retry
  );
  if( retry ){
    assert( rc==DBSQL_OK );
    rc = __where_loop_add_virtual_one(
        builder, m_prereq, ALLBITS, 0, p, m_no_omit, &in, 0
    );
  }

  /* If the call to best_index() with all terms enabled produced a plan
  ** that does not require any source tables (IOW: a plan with m_best==0)
  ** and does not use an IN(...) operator, then there is no point in making
  ** any further calls to best_index() since they will all return the same
  ** result (if the best_index() implementation is sane). */
  if( rc==DBSQL_OK && ((m_best = (p_new->prereq & ~m_prereq))!=0 || in) ){
    int seen_zero = 0;             /* True if a plan with no prereqs seen */
    int seen_zero_no_in = 0;         /* Plan with no prereqs and no IN(...) seen */
    Bitmask m_prev = 0;
    Bitmask m_best_no_in = 0;

    /* If the plan produced by the earlier call uses an IN(...) term, call
    ** best_index again, this time with IN(...) terms disabled. */
    if( in ){
      WHERETRACE(0x800, ("  VirtualOne: all usable w/o IN\n"));
      rc = __where_loop_add_virtual_one(
          builder, m_prereq, ALLBITS, WO_IN, p, m_no_omit, &in, 0);
      assert( in==0 );
      m_best_no_in = p_new->prereq & ~m_prereq;
      if( m_best_no_in==0 ){
        seen_zero = 1;
        seen_zero_no_in = 1;
      }
    }

    /* Call best_index once for each distinct value of (prereq_right & ~m_prereq)
    ** in the set of terms that apply to the current virtual table.  */
    while( rc==DBSQL_OK ){
      int i;
      Bitmask m_next = ALLBITS;
      assert( m_next>0 );
      for(i=0; i<n_constraint; i++){
        int i_term = p->constraints[i].i_term_offset;
        Bitmask m_this = __term_from_where_clause(wc, i_term)->prereq_right & ~m_prereq;
        if( m_this>m_prev && m_this<m_next ) m_next = m_this;
      }
      m_prev = m_next;
      if( m_next==ALLBITS ) break;
      if( m_next==m_best || m_next==m_best_no_in ) continue;
      WHERETRACE(0x800, ("  VirtualOne: mPrev=%04llx mNext=%04llx\n",
                       (u_int64_t)m_prev, (u_int64_t)m_next));
      rc = __where_loop_add_virtual_one(
          builder, m_prereq, m_next|m_prereq, 0, p, m_no_omit, &in, 0);
      if( p_new->prereq==m_prereq ){
        seen_zero = 1;
        if( in==0 ) seen_zero_no_in = 1;
      }
    }

    /* If the calls to best_index() in the above loop did not find a plan
    ** that requires no source tables at all (i.e. one guaranteed to be
    ** usable), make a call here with all source tables disabled */
    if( rc==DBSQL_OK && seen_zero==0 ){
      WHERETRACE(0x800, ("  VirtualOne: all disabled\n"));
      rc = __where_loop_add_virtual_one(
          builder, m_prereq, m_prereq, 0, p, m_no_omit, &in, 0);
      if( in==0 ) seen_zero_no_in = 1;
    }

    /* If the calls to best_index() have so far failed to find a plan
    ** that requires no source tables at all and does not use an IN(...)
    ** operator, make a final call to obtain one here.  */
    if( rc==DBSQL_OK && seen_zero_no_in==0 ){
      WHERETRACE(0x800, ("  VirtualOne: all disabled and w/o IN\n"));
      rc = __where_loop_add_virtual_one(
          builder, m_prereq, m_prereq, WO_IN, p, m_no_omit, &in, 0);
    }
  }

  __free_index_info(parser->db, p);
  WHERETRACE(0x800, ("END %s.addVirtual(), rc=%d\n", src->s_tab->name, rc));
  return rc;
}
#endif /* DBSQL_OMIT_VIRTUALTABLE */

/*
** Add where_loop_t entries to handle OR terms.  This works for either
** btrees or virtual tables.
*/
static int __where_loop_add_or(
  where_loop_builder_t *builder,
  Bitmask m_prereq,
  Bitmask m_unusable
){
  where_info_t *w_info = builder->w_info;
  where_clause_t *wc;
  where_loop_t *p_new;
  where_term_t *term, *wc_end;
  int rc = DBSQL_OK;
  int i_cur;
  where_clause_t temp_wc;
  where_loop_builder_t s_sub_build;
  where_or_set_t s_sum, s_cur;
  src_item_t *item;
 
  wc = builder->wc;
  wc_end = wc->a + wc->n_term;
  p_new = builder->p_new;
  memset(&s_sum, 0, sizeof(s_sum));
  item = w_info->tab_list->a + p_new->i_tab;
  i_cur = item->i_cursor;

  /* The multi-index OR optimization does not work for RIGHT and FULL JOIN */
  if( item->fg.jointype & JT_RIGHT ) return DBSQL_OK;

  for(term=wc->a; term<wc_end && rc==DBSQL_OK; term++){
    if( (term->e_operator & WO_OR)!=0
     && (term->u.or_info->indexable & p_new->mask_self)!=0
    ){
      where_clause_t * const or_wc = &term->u.or_info->wc;
      where_term_t * const or_wc_end = &or_wc->a[or_wc->n_term];
      where_term_t *or_term;
      int once = 1;
      int i, j;
   
      s_sub_build = *builder;
      s_sub_build.or_set = &s_cur;

      WHERETRACE(0x400, ("Begin processing OR-clause %p\n", term));
      for(or_term=or_wc->a; or_term<or_wc_end; or_term++){
        if( (or_term->e_operator & WO_AND)!=0 ){
          s_sub_build.wc = &or_term->u.and_info->wc;
        }else if( or_term->left_cursor==i_cur ){
          temp_wc.w_info = wc->w_info;
          temp_wc.outer = wc;
          temp_wc.op = TK_AND;
          temp_wc.n_term = 1;
          temp_wc.n_base = 1;
          temp_wc.a = or_term;
          s_sub_build.wc = &temp_wc;
        }else{
          continue;
        }
        s_cur.n = 0;
#ifdef WHERETRACE_ENABLED
        WHERETRACE(0x400, ("OR-term %d of %p has %d subterms:\n",
                   (int)(or_term-or_wc->a), term, s_sub_build.wc->n_term));
        if( __where_trace & 0x20000 ){
          __where_clause_print(s_sub_build.wc);
        }
#endif
#ifndef DBSQL_OMIT_VIRTUALTABLE
        if( IS_VIRTUAL(item->s_tab) ){
          rc = __where_loop_add_virtual(&s_sub_build, m_prereq, m_unusable);
        }else
#endif
        {
          rc = __where_loop_add_btree(&s_sub_build, m_prereq);
        }
        if( rc==DBSQL_OK ){
          rc = __where_loop_add_or(&s_sub_build, m_prereq, m_unusable);
        }
        testcase( rc==DBSQL_NOMEM && s_cur.n>0 );
        testcase( rc==DBSQL_DONE );
        if( s_cur.n==0 ){
          s_sum.n = 0;
          break;
        }else if( once ){
          __where_or_move(&s_sum, &s_cur);
          once = 0;
        }else{
          where_or_set_t s_prev;
          __where_or_move(&s_prev, &s_sum);
          s_sum.n = 0;
          for(i=0; i<s_prev.n; i++){
            for(j=0; j<s_cur.n; j++){
              __where_or_insert(&s_sum, s_prev.a[i].prereq | s_cur.a[j].prereq,
                            __log_est_add(s_prev.a[i].r_run, s_cur.a[j].r_run),
                            __log_est_add(s_prev.a[i].n_out, s_cur.a[j].n_out));
            }
          }
        }
      }
      p_new->n_l_term = 1;
      p_new->l_terms[0] = term;
      p_new->ws_flags = WHERE_MULTI_OR;
      p_new->r_setup = 0;
      p_new->i_sort_idx = 0;
      memset(&p_new->u, 0, sizeof(p_new->u));
      for(i=0; rc==DBSQL_OK && i<s_sum.n; i++){
        /* TUNING: Currently s_sum.a[i].r_run is set to the sum of the costs
        ** of all sub-scans required by the OR-scan. However, due to rounding
        ** errors, it may be that the cost of the OR-scan is equal to its
        ** most expensive sub-scan. Add the smallest possible penalty
        ** (equivalent to multiplying the cost by 1.07) to ensure that
        ** this does not happen. Otherwise, for WHERE clauses such as the
        ** following where there is an index on "y":
        **
        **     WHERE likelihood(x=?, 0.99) OR y=?
        **
        ** the planner may elect to "OR" together a full-table scan and an
        ** index lookup. And other similarly odd results.  */
        p_new->r_run = s_sum.a[i].r_run + 1;
        p_new->n_out = s_sum.a[i].n_out;
        p_new->prereq = s_sum.a[i].prereq;
        rc = __where_loop_insert(builder, p_new);
      }
      WHERETRACE(0x400, ("End processing OR-clause %p\n", term));
    }
  }
  return rc;
}

/*
** Add all where_loop_t objects for all tables
*/
static int __where_loop_add_all(where_loop_builder_t *builder){
  where_info_t *w_info = builder->w_info;
  Bitmask m_prereq = 0;
  Bitmask m_prior = 0;
  int i_tab;
  src_list_t *tab_list = w_info->tab_list;
  src_item_t *item;
  src_item_t *end = &tab_list->a[w_info->n_level];
  DBSQL *db = w_info->parser->db;
  int rc = DBSQL_OK;
  int first_past_rj = 0;
  int has_right_cross_join = 0;
  where_loop_t *p_new;


  /* Loop over the tables in the join, from left to right */
  p_new = builder->p_new;

  /* Verify that p_new has already been initialized */
  assert( p_new->n_l_term==0 );
  assert( p_new->ws_flags==0 );
  assert( p_new->n_l_slot>=ARRAY_SIZE(p_new->l_term_spaces) );
  assert( p_new->l_terms!=0 );

  builder->i_plan_limit = DBSQL_QUERY_PLANNER_LIMIT;
  for(i_tab=0, item=tab_list->a; item<end; i_tab++, item++){
    Bitmask m_unusable = 0;
    p_new->i_tab = i_tab;
    builder->i_plan_limit += DBSQL_QUERY_PLANNER_LIMIT_INCR;
    p_new->mask_self = __where_get_mask(&w_info->s_mask_set, item->i_cursor);
    if( first_past_rj
     || (item->fg.jointype & (JT_OUTER|JT_CROSS|JT_LTORJ))!=0
    ){
      /* Add prerequisites to prevent reordering of FROM clause terms
      ** across CROSS joins and outer joins.  The first_past_rj boolean
      ** prevents the right operand of a RIGHT JOIN from being swapped with
      ** other elements even further to the right.
      **
      ** The has_right_cross_join flag prevent FROM-clause terms from moving
      ** from the right side of a LEFT JOIN or CROSS JOIN over to the
      ** left side of that same join.  This is a required restriction in
      ** the case of LEFT JOIN - an incorrect answer may results if it is
      ** not enforced.  This restriction is not required for CROSS JOIN.
      ** It is provided merely as a means of controlling join order, under
      ** the theory that no real-world queries that care about performance
      ** actually use the CROSS JOIN syntax.
      */
      if( item->fg.jointype & (JT_LTORJ|JT_CROSS) ){
        testcase( item->fg.jointype & JT_LTORJ );
        testcase( item->fg.jointype & JT_CROSS );
        has_right_cross_join = 1;
      }
      m_prereq |= m_prior;
      first_past_rj = (item->fg.jointype & JT_RIGHT)!=0;
    }else if( item->fg.from_exists ){
      /* joins that result from the EXISTS-to-JOIN optimization should not
      ** be moved to the left of any of their dependencies */
      where_clause_t *wc = &w_info->s_wc;
      where_term_t *term;
      int i;
      for(i=wc->n_base, term=wc->a; i>0; i--, term++){
        if( (p_new->mask_self & term->prereq_all)!=0 ){
          m_prereq |= (term->prereq_all & (p_new->mask_self-1));
        }
      }
    }else if( !has_right_cross_join ){
      m_prereq = 0;
    }
#ifndef DBSQL_OMIT_VIRTUALTABLE
    if( IS_VIRTUAL(item->s_tab) ){
      src_item_t *p;
      for(p=&item[1]; p<end; p++){
        if( m_unusable || (p->fg.jointype & (JT_OUTER|JT_CROSS)) ){
          m_unusable |= __where_get_mask(&w_info->s_mask_set, p->i_cursor);
        }
      }
      rc = __where_loop_add_virtual(builder, m_prereq, m_unusable);
    }else
#endif /* DBSQL_OMIT_VIRTUALTABLE */
    {
      rc = __where_loop_add_btree(builder, m_prereq);
    }
    if( rc==DBSQL_OK && builder->wc->has_or ){
      rc = __where_loop_add_or(builder, m_prereq, m_unusable);
    }
    m_prior |= p_new->mask_self;
    if( rc || db->malloc_failed ){
      if( rc==DBSQL_DONE ){
        /* We hit the query planner search limit set by i_plan_limit */
        dbsql_log(DBSQL_WARNING, "abbreviated query algorithm search");
        rc = DBSQL_OK;
      }else{
        break;
      }
    }
  }

  __where_loop_clear(db, p_new);
  return rc;
}

/* Implementation of the order-by-subquery optimization:
**
** where_loop_t loop, which the i_loop-th term of the nested loop, is really
** a subquery or CTE that has an ORDER BY clause.  See if any of the terms
** in the subquery ORDER BY clause will satisfy order_by from the outer
** query.  Mark off all satisfied terms (by setting bits in *ob_sat) and
** return TRUE if they do.  If not, return false.
**
** Example:
**
**    CREATE TABLE t1(a,b,c, PRIMARY KEY(a,b));
**    CREATE TABLE t2(x,y);
**    WITH t3(p,q) AS MATERIALIZED (SELECT x+y, x-y FROM t2 ORDER BY x+y)
**       SELECT * FROM t3 JOIN t1 ON a=q ORDER BY p, b;
**
** The CTE named "t3" comes out in the natural order of "p", so the first
** first them of "ORDER BY p,b" is satisfied by a sequential scan of "t3"
** and sorting only needs to occur on the second term "b".
**
** Limitations:
**
** (1)  The optimization is not applied if the outer ORDER BY contains
**      a COLLATE clause.  The optimization might be applied if the
**      outer ORDER BY uses NULLS FIRST, NULLS LAST, ASC, and/or DESC as
**      long as the subquery ORDER BY does the same.  But if the
**      outer ORDER BY uses COLLATE, even a redundant COLLATE, the
**      optimization is bypassed.
**
** (2)  The subquery ORDER BY terms must exactly match subquery result
**      columns, including any COLLATE annotations.  This routine relies
**      on i_order_by_col to do matching between order by terms and result
**      columns, and i_order_by_col will not be set if the result column
**      and ORDER BY collations differ.
**
** (3)  The subquery and outer ORDER BY can be in opposite directions as
**      long as  the subquery is materialized.  If the subquery is
**      implemented as a co-routine, the sort orders must be in the same
**      direction because there is no way to run a co-routine backwards.
*/
static DBSQL_NOINLINE int __where_path_match_subquery_ob(
  where_info_t *w_info,      /* The WHERE clause */
  where_loop_t *loop,       /* The nested loop term that is a subquery */
  int i_loop,              /* Which level of the nested loop.  0==outermost */
  int i_cur,               /* Cursor used by the this loop */
  expr_list_t *order_by,     /* The ORDER BY clause on the whole query */
  Bitmask *rev_mask,      /* When loops need to go in reverse order */
  Bitmask *ob_sat         /* Which terms of order_by are satisfied so far */
){
  int i_ob;                /* index_t into order_by->a[] */
  int j_sub;               /* index_t into sub_ob->a[] */
  u_int8_t rev = 0;             /* True if i_ob and j_sub sort in opposite directions */
  u_int8_t rev_idx = 0;          /* Sort direction for j_sub */
  expr_t *ob_expr;          /* Current term of outer ORDER BY */
  expr_list_t *sub_ob;       /* Complete ORDER BY on the subquery */

  sub_ob = loop->u.btree.order_by;
  assert( sub_ob!=0 );
  for(i_ob=0; (MASKBIT(i_ob) & *ob_sat)!=0; i_ob++){}
  for(j_sub=0; j_sub<sub_ob->n_expr && i_ob<order_by->n_expr; j_sub++, i_ob++){
    if( sub_ob->a[j_sub].u.x.i_order_by_col==0 ) break;
    ob_expr = order_by->a[i_ob].expr;
    if( ob_expr->op!=TK_COLUMN && ob_expr->op!=TK_AGG_COLUMN ) break;
    if( ob_expr->i_table!=i_cur ) break;
    if( ob_expr->i_column!=sub_ob->a[j_sub].u.x.i_order_by_col-1 ) break;
    if( (w_info->wctrl_flags & WHERE_GROUPBY)==0 ){
      u_int8_t sf_ob = order_by->a[i_ob].fg.sort_flags;   /* sort_flags for i_ob */
      u_int8_t sf_sub = sub_ob->a[j_sub].fg.sort_flags;   /* sort_flags for j_sub */
      if( (sf_sub & KEYINFO_ORDER_BIGNULL) != (sf_ob & KEYINFO_ORDER_BIGNULL) ){
        break;
      }
      rev_idx = sf_sub & KEYINFO_ORDER_DESC;
      if( j_sub>0 ){
        if( (rev^rev_idx)!=(sf_ob & KEYINFO_ORDER_DESC) ){
          break;
        }
      }else{
        rev = rev_idx ^ (sf_ob & KEYINFO_ORDER_DESC);
        if( rev ){
          if( (loop->ws_flags & WHERE_COROUTINE)!=0 ){
            /* Cannot run a co-routine in reverse order */
            break;
          }
          *rev_mask |= MASKBIT(i_loop);
        }
      }
    }
    *ob_sat |= MASKBIT(i_ob);
  }
  return j_sub>0;
}

/*
** Examine a where_path_t (with the addition of the extra where_loop_t of the 6th
** parameters) to see if it outputs rows in the requested ORDER BY
** (or GROUP BY) without requiring a separate sort operation.  Return N:
**
**   N>0:   N terms of the ORDER BY clause are satisfied
**   N==0:  No terms of the ORDER BY clause are satisfied
**   N<0:   Unknown yet how many terms of ORDER BY might be satisfied.  
**
** Note that processing for WHERE_GROUPBY and WHERE_DISTINCTBY is not as
** strict.  with_t GROUP BY and DISTINCT the only requirement is that
** equivalent rows appear immediately adjacent to one another.  GROUP BY
** and DISTINCT do not require rows to appear in any particular order as long
** as equivalent rows are grouped together.  Thus for GROUP BY and DISTINCT
** the order_by terms can be matched in any order.  with_t ORDER BY, the
** order_by terms must be matched in strict left-to-right order.
*/
static int8_t __where_path_satisfies_order_by(
  where_info_t *w_info,    /* The WHERE clause */
  expr_list_t *order_by,   /* ORDER BY or GROUP BY or DISTINCT clause to check */
  where_path_t *path,     /* The where_path_t to check */
  u_int16_t wctrl_flags,       /* WHERE_GROUPBY or _DISTINCTBY or _ORDERBY_LIMIT */
  u_int16_t n_loop,            /* Number of entries in path->loops[] */
  where_loop_t *last,     /* Add this where_loop_t to the end of path->loops[] */
  Bitmask *rev_mask     /* OUT: Mask of __where_loops to run in reverse order */
){
  u_int8_t rev_set;            /* True if rev is known */
  u_int8_t rev;               /* Composite sort order */
  u_int8_t rev_idx;            /* index_t sort order */
  u_int8_t is_order_distinct;   /* All prior __where_loops are order-distinct */
  u_int8_t distinct_columns;   /* True if the loop has UNIQUE NOT NULL columns */
  u_int8_t isMatch;           /* i_column matches a term of the ORDER BY clause */
  u_int16_t eq_op_mask;         /* Allowed equality operators */
  u_int16_t n_key_col;          /* Number of key columns in p_index */
  u_int16_t n_column;          /* Total number of ordered columns in the index */
  u_int16_t n_order_by;         /* Number terms in the ORDER BY clause */
  int i_loop;            /* index_t of where_loop_t in path being processed */
  int i, j;             /* Loop counters */
  int i_cur;             /* Cursor number for current where_loop_t */
  int i_column;          /* A column number within table i_cur */
  where_loop_t *loop = 0; /* Current where_loop_t being processed. */
  where_term_t *term;     /* A single term of the WHERE clause */
  expr_t *ob_expr;        /* An expression from the ORDER BY clause */
  coll_seq_t *coll;       /* COLLATE function from an ORDER BY clause term */
  index_t *p_index;        /* The index associated with loop */
  DBSQL *db = w_info->parser->db;  /* Database connection */
  Bitmask ob_sat = 0;    /* Mask of ORDER BY terms satisfied so far */
  Bitmask ob_done;       /* Mask of all ORDER BY terms */
  Bitmask order_distinct_mask;  /* Mask of all well-ordered loops */
  Bitmask ready;              /* Mask of inner loops */

  /*
  ** We say the where_loop_t is "one-row" if it generates no more than one
  ** row of output.  A where_loop_t is one-row if all of the following are true:
  **  (a) All index columns match with WHERE_COLUMN_EQ.
  **  (b) The index is unique
  ** Any where_loop_t with an WHERE_COLUMN_EQ constraint on the rowid is one-row.
  ** Every one-row where_loop_t will have the WHERE_ONEROW bit set in ws_flags.
  **
  ** We say the where_loop_t is "order-distinct" if the set of columns from
  ** that where_loop_t that are in the ORDER BY clause are different for every
  ** row of the where_loop_t.  Every one-row where_loop_t is automatically
  ** order-distinct.   A where_loop_t that has no columns in the ORDER BY clause
  ** is not order-distinct. To be order-distinct is not quite the same as being
  ** UNIQUE since a UNIQUE column or index can have multiple rows that
  ** are NULL and NULL values are equivalent for the purpose of order-distinct.
  ** To be order-distinct, the columns must be UNIQUE and NOT NULL.
  **
  ** The rowid for a table is always UNIQUE and NOT NULL so whenever the
  ** rowid appears in the ORDER BY clause, the corresponding where_loop_t is
  ** automatically order-distinct.
  */

  assert( order_by!=0 );
  if( n_loop && OPTIMIZATION_DISABLED(db, DBSQL_OrderByIdxJoin) ) return 0;

  n_order_by = order_by->n_expr;
  testcase( n_order_by==BMS-1 );
  if( n_order_by>BMS-1 ) return 0;  /* Cannot optimize overly large ORDER BYs */
  is_order_distinct = 1;
  ob_done = MASKBIT(n_order_by)-1;
  order_distinct_mask = 0;
  ready = 0;
  eq_op_mask = WO_EQ | WO_IS | WO_ISNULL;
  if( wctrl_flags & (WHERE_ORDERBY_LIMIT|WHERE_ORDERBY_MAX|WHERE_ORDERBY_MIN) ){
    eq_op_mask |= WO_IN;
  }
  for(i_loop=0; is_order_distinct && ob_sat<ob_done && i_loop<=n_loop; i_loop++){
    if( i_loop>0 ) ready |= loop->mask_self;
    if( i_loop<n_loop ){
      loop = path->loops[i_loop];
      if( wctrl_flags & WHERE_ORDERBY_LIMIT ) continue;
    }else{
      loop = last;
    }
    if( loop->ws_flags & WHERE_VIRTUALTABLE ){
      if( loop->u.vtab.is_ordered && w_info->order_by==order_by ){
        ob_sat = ob_done;
      }else{
        /* No further ORDER BY terms may be matched. So this call should
        ** return >=0, not -1. Clear is_order_distinct to ensure it does so. */
        is_order_distinct = 0;
      }
      break;
    }
    i_cur = w_info->tab_list->a[loop->i_tab].i_cursor;

    /* Mark off any ORDER BY term X that is a column in the table of
    ** the current loop for which there is term in the WHERE
    ** clause of the form X IS NULL or X=? that reference only outer
    ** loops.
    */
    for(i=0; i<n_order_by; i++){
      if( MASKBIT(i) & ob_sat ) continue;
      ob_expr = __expr_skip_collate_and_likely(order_by->a[i].expr);
      if( NEVER(ob_expr==0) ) continue;
      if( ob_expr->op!=TK_COLUMN && ob_expr->op!=TK_AGG_COLUMN ) continue;
      if( ob_expr->i_table!=i_cur ) continue;
      term = __where_find_term(&w_info->s_wc, i_cur, ob_expr->i_column,
                       ~ready, eq_op_mask, 0);
      if( term==0 ) continue;
      if( term->e_operator==WO_IN ){
        /* IN terms are only valid for sorting in the ORDER BY LIMIT
        ** optimization, and then only if they are actually used
        ** by the query plan */
        assert( wctrl_flags &
               (WHERE_ORDERBY_LIMIT|WHERE_ORDERBY_MIN|WHERE_ORDERBY_MAX) );
        for(j=0; j<loop->n_l_term && term!=loop->l_terms[j]; j++){}
        if( j>=loop->n_l_term ) continue;
      }
      if( (term->e_operator&(WO_EQ|WO_IS))!=0 && ob_expr->i_column>=0 ){
        parser_t *parser = w_info->parser;
        coll_seq_t *coll1 = __expr_nn_coll_seq(parser, order_by->a[i].expr);
        coll_seq_t *coll2 = __expr_compare_coll_seq(parser, term->expr);
        assert( coll1 );
        if( coll2==0 || __str_icmp(coll1->name, coll2->name) ){
          continue;
        }
        testcase( term->expr->op==TK_IS );
      }
      ob_sat |= MASKBIT(i);
    }

    if( (loop->ws_flags & WHERE_ONEROW)==0 ){
      if( loop->ws_flags & WHERE_IPK ){
        if( loop->u.btree.order_by
         && OPTIMIZATION_ENABLED(db, DBSQL_OrderBySubq)
         &&  __where_path_match_subquery_ob(w_info,loop,i_loop,i_cur,
                                     order_by,rev_mask, &ob_sat)
        ){
          n_column = 0;
          is_order_distinct = 0;
        }else{
          n_column = 1;
        }
        p_index = 0;
        n_key_col = 0;
      }else if( (p_index = loop->u.btree.p_index)==0 || p_index->unordered ){
        return 0;
      }else{
        n_key_col = p_index->n_key_col;
        n_column = p_index->n_column;
        assert( n_column==n_key_col+1 || !HAS_ROWID(p_index->table) );
        assert( p_index->columns[n_column-1]==XN_ROWID
                          || !HAS_ROWID(p_index->table));
        /* All relevant terms of the index must also be non-NULL in order
        ** for is_order_distinct to be true.  So the is_order_distinct value
        ** computed here might be a false positive.  Corrections will be
        ** made at tag-20210426-1 below */
        is_order_distinct = IS_UNIQUE_INDEX(p_index)
                          && (loop->ws_flags & WHERE_SKIPSCAN)==0;
      }

      /* Loop through all columns of the index and deal with the ones
      ** that are not constrained by == or IN.
      */
      rev = rev_set = 0;
      distinct_columns = 0;
      for(j=0; j<n_column; j++){
        u_int8_t once = 1; /* True to run the ORDER BY search loop */

        assert( j>=loop->u.btree.n_eq
            || (loop->l_terms[j]==0)==(j<loop->n_skip)
        );
        if( j<loop->u.btree.n_eq && j>=loop->n_skip ){
          u_int16_t op = loop->l_terms[j]->e_operator;

          /* Skip over == and IS and ISNULL terms.  (Also skip IN terms when
          ** doing WHERE_ORDERBY_LIMIT processing).  Except, IS and ISNULL
          ** terms imply that the index is not UNIQUE NOT NULL in which case
          ** the loop need to be marked as not order-distinct because it can
          ** have repeated NULL rows.
          **
          ** If the current term is a column of an ((?,?) IN (SELECT...))
          ** expression for which the SELECT returns more than one column,
          ** check that it is the only column used by this loop. Otherwise,
          ** if it is one of two or more, none of the columns can be
          ** considered to match an ORDER BY term.
          */
          if( (op & eq_op_mask)!=0 ){
            if( op & (WO_ISNULL|WO_IS) ){
              testcase( op & WO_ISNULL );
              testcase( op & WO_IS );
              testcase( is_order_distinct );
              is_order_distinct = 0;
            }
            continue; 
          }else if( ALWAYS(op & WO_IN) ){
            /* ALWAYS() justification: op is an equality operator due to the
            ** j<loop->u.btree.n_eq constraint above.  Any equality other
            ** than WO_IN is captured by the previous "if".  So this one
            ** always has to be WO_IN. */
            expr_t *x = loop->l_terms[j]->expr;
            for(i=j+1; i<loop->u.btree.n_eq; i++){
              if( loop->l_terms[i]->expr==x ){
                assert( (loop->l_terms[i]->e_operator & WO_IN) );
                once = 0;
                break;
              }
            }
          }
        }

        /* Get the column number in the table (i_column) and sort order
        ** (rev_idx) for the j-th column of the index.
        */
        if( p_index ){
          i_column = p_index->columns[j];
          rev_idx = p_index->sort_orders[j] & KEYINFO_ORDER_DESC;
          if( i_column==p_index->table->i_p_key ) i_column = XN_ROWID;
        }else{
          i_column = XN_ROWID;
          rev_idx = 0;
        }

        /* An unconstrained column that might be NULL means that this
        ** where_loop_t is not well-ordered.  tag-20210426-1
        */
        if( is_order_distinct ){
          if( i_column>=0
           && j>=loop->u.btree.n_eq
           && p_index->table->cols[i_column].not_null==0
          ){
            is_order_distinct = 0;
          }
          if( i_column==XN_EXPR ){
            is_order_distinct = 0;
          }
        }

        /* Find the ORDER BY term that corresponds to the j-th column
        ** of the index and mark that ORDER BY term having been satisfied.
        */
        isMatch = 0;
        for(i=0; once && i<n_order_by; i++){
          if( MASKBIT(i) & ob_sat ) continue;
          ob_expr = __expr_skip_collate_and_likely(order_by->a[i].expr);
          testcase( wctrl_flags & WHERE_GROUPBY );
          testcase( wctrl_flags & WHERE_DISTINCTBY );
          if( NEVER(ob_expr==0) ) continue;
          if( (wctrl_flags & (WHERE_GROUPBY|WHERE_DISTINCTBY))==0 ) once = 0;
          if( i_column>=XN_ROWID ){
            if( ob_expr->op!=TK_COLUMN && ob_expr->op!=TK_AGG_COLUMN ) continue;
            if( ob_expr->i_table!=i_cur ) continue;
            if( ob_expr->i_column!=i_column ) continue;
          }else{
            expr_t *ix_expr = p_index->col_exprs->a[j].expr;
            if( __expr_compare_skip(ob_expr, ix_expr, i_cur) ){
              continue;
            }
          }
          if( i_column!=XN_ROWID ){
            coll = __expr_nn_coll_seq(w_info->parser, order_by->a[i].expr);
            if( __str_icmp(coll->name, p_index->colls[j])!=0 ) continue;
          }
          if( wctrl_flags & WHERE_DISTINCTBY ){
            loop->u.btree.n_distinct_col = j+1;
          }
          isMatch = 1;
          break;
        }
        if( isMatch && (wctrl_flags & WHERE_GROUPBY)==0 ){
          /* Make sure the sort order is compatible in an ORDER BY clause.
          ** Sort order is irrelevant for a GROUP BY clause. */
          if( rev_set ){
            if( (rev ^ rev_idx)
                           != (order_by->a[i].fg.sort_flags&KEYINFO_ORDER_DESC)
            ){
              isMatch = 0;
            }
          }else{
            rev = rev_idx ^ (order_by->a[i].fg.sort_flags & KEYINFO_ORDER_DESC);
            if( rev ) *rev_mask |= MASKBIT(i_loop);
            rev_set = 1;
          }
        }
        if( isMatch && (order_by->a[i].fg.sort_flags & KEYINFO_ORDER_BIGNULL) ){
          if( j==loop->u.btree.n_eq ){
            loop->ws_flags |= WHERE_BIGNULL_SORT;
          }else{
            isMatch = 0;
          }
        }
        if( isMatch ){
          if( i_column==XN_ROWID ){
            testcase( distinct_columns==0 );
            distinct_columns = 1;
          }
          ob_sat |= MASKBIT(i);
        }else{
          /* No match found */
          if( j==0 || j<n_key_col ){
            testcase( is_order_distinct!=0 );
            is_order_distinct = 0;
          }
          break;
        }
      } /* end Loop over all index columns */
      if( distinct_columns ){
        testcase( is_order_distinct==0 );
        is_order_distinct = 1;
      }
    } /* end-if not one-row */

    /* Mark off any other ORDER BY terms that reference loop */
    if( is_order_distinct ){
      order_distinct_mask |= loop->mask_self;
      for(i=0; i<n_order_by; i++){
        expr_t *p;
        Bitmask m_term;
        if( MASKBIT(i) & ob_sat ) continue;
        p = order_by->a[i].expr;
        m_term = __where_expr_usage(&w_info->s_mask_set,p);
        if( m_term==0 && !__expr_is_constant(0,p) ) continue;
        if( (m_term&~order_distinct_mask)==0 ){
          ob_sat |= MASKBIT(i);
        }
      }
    }
  } /* End the loop over all __where_loops from outer-most down to inner-most */
  if( ob_sat==ob_done ) return (int8_t)n_order_by;
  if( !is_order_distinct ){
    for(i=n_order_by-1; i>0; i--){
      Bitmask m = ALWAYS(i<BMS) ? MASKBIT(i) - 1 : 0;
      if( (ob_sat&m)==m ) return i;
    }
    return 0;
  }
  return -1;
}


/*
** If the WHERE_GROUPBY flag is set in the mask passed to __where_begin(),
** the planner assumes that the specified order_by list is actually a GROUP
** BY clause - and so any order that groups rows as required satisfies the
** request.
**
** Normally, in this case it is not possible for the caller to determine
** whether or not the rows are really being delivered in sorted order, or
** just in some other order that provides the required grouping. However,
** if the WHERE_SORTBYGROUP flag is also passed to __where_begin(), then
** this function may be called on the returned where_info_t object. It returns
** true if the rows really will be sorted in the specified order, or false
** otherwise.
**
** For example, assuming:
**
**   CREATE INDEX i1 ON t1(x, Y);
**
** then
**
**   SELECT * FROM t1 GROUP BY x,y ORDER BY x,y;   -- __is_sorted()==1
**   SELECT * FROM t1 GROUP BY y,x ORDER BY y,x;   -- __is_sorted()==0
*/
int __where_is_sorted(where_info_t *w_info){
  assert( w_info->wctrl_flags & (WHERE_GROUPBY|WHERE_DISTINCTBY) );
  assert( w_info->wctrl_flags & WHERE_SORTBYGROUP );
  return w_info->sorted;
}

#ifdef WHERETRACE_ENABLED
/* For debugging use only: */
static const char *__where_path_name(where_path_t *path, int n_loop, where_loop_t *last){
  static char name[65];
  int i;
  for(i=0; i<n_loop; i++){ name[i] = path->loops[i]->c_id; }
  if( last ) name[i++] = last->c_id;
  name[i] = 0;
  return name;
}
#endif

/*
** Return the cost of sorting n_row rows, assuming that the keys have
** n_orderby columns and that the first n_sorted columns are already in
** order.
*/
static log_est_t __where_sorting_cost(
  where_info_t *w_info, /* Query planning context */
  log_est_t n_row,       /* Estimated number of rows to sort */
  int n_order_by,      /* Number of ORDER BY clause terms */
  int n_sorted        /* Number of initial ORDER BY terms naturally in order */
){
  /* Estimated cost of a full external sort, where N is
  ** the number of rows to sort is:
  **
  **   cost = (K * N * log(N)).
  **
  ** Or, if the order-by clause has X terms but only the last Y
  ** terms are out of order, then block-sorting will reduce the
  ** sorting cost to:
  **
  **   cost = (K * N * log(N)) * (Y/X)
  **
  ** The constant K is at least 2.0 but will be larger if there are a
  ** large number of columns to be sorted, as the sorting time is
  ** proportional to the amount of content to be sorted.  The algorithm
  ** does not currently distinguish between fat columns (BLOBs and TEXTs)
  ** and skinny columns (INTs).  It just uses the number of columns as
  ** an approximation for the row width.
  **
  ** And extra factor of 2.0 or 3.0 is added to the sorting cost if the sort
  ** is built using OP_IdxInsert and OP_Sort rather than with OP_SorterInsert.
  */
  log_est_t r_sort_cost, n_col;
  assert( w_info->p_select!=0 );
  assert( w_info->p_select->e_list!=0 );
  /* TUNING: sorting cost proportional to the number of output columns: */
  n_col = __log_est((w_info->p_select->e_list->n_expr+59)/30);
  r_sort_cost = n_row + n_col;
  if( n_sorted>0 ){
    /* Scale the result by (Y/X) */
    r_sort_cost += __log_est((n_order_by-n_sorted)*100/n_order_by) - 66;
  }

  /* Multiple by log(M) where M is the number of output rows.
  ** Use the LIMIT for M if it is smaller.  Or if this sort is for
  ** a DISTINCT operator, M will be the number of distinct output
  ** rows, so fudge it downwards a bit.
  */
  if( (w_info->wctrl_flags & WHERE_USE_LIMIT)!=0 ){
    r_sort_cost += 10;       /* TUNING: Extra 2.0x if using LIMIT */
    if( n_sorted!=0 ){
      r_sort_cost += 6;      /* TUNING: Extra 1.5x if also using partial sort */
    }
    if( w_info->i_limit<n_row ){
      n_row = w_info->i_limit;
    }
  }else if( (w_info->wctrl_flags & WHERE_WANT_DISTINCT) ){
    /* TUNING: In the sort for a DISTINCT operator, assume that the DISTINCT
    ** reduces the number of output rows by a factor of 2 */
    if( n_row>10 ){ n_row -= 10;  assert( 10==__log_est(2) ); }
  }
  r_sort_cost += __est_log(n_row);
  return r_sort_cost;
}

/*
** Compute the maximum number of paths in the solver algorithm, for
** queries that have three or more terms in the FROM clause.  Queries with
** two or fewer FROM clause terms are handled by the caller.
**
** Query planning is NP-hard.  We must limit the number of paths at
** each step of the solver search algorithm to avoid exponential behavior.
**
** The value returned is a tuning parameter.  Currently the value is:
**
**     18    for star queries
**     12    otherwise
**
** For the purposes of this heuristic, a star-query is defined as a query
** with a central "fact" table that is joined against multiple
** "dimension" tables, subject to the following constraints:
**
**   (aa)  Only a five-way or larger join is considered for this
**         optimization.  If there are fewer than four terms in the FROM
**         clause, this heuristic does not apply.
**
**   (bb)  The join between the fact table and the dimension tables must
**         be an INNER join.  CROSS and OUTER JOINs do not qualify.
**
**   (cc)  A table must have 3 or more dimension tables in order to be
**         considered a fact table. (Was 4 prior to 2026-02-10.)
**
**   (dd)  A table that is a self-join cannot be a dimension table.
**         Dimension tables are joined against fact tables.
**
** SIDE EFFECT:  (and really the whole point of this subroutine)
**
** If w_info describes a star-query, then the cost for SCANs of dimension
** __where_loops is increased to be slightly larger than the cost of a SCAN
** in the fact table.  Only SCAN costs are increased.  SEARCH costs are
** unchanged. This heuristic helps keep fact tables in outer loops. Without
** this heuristic, paths with fact tables in outer loops tend to get pruned
** by the mx_choice limit on the number of paths, resulting in poor query
** plans.  See the starschema1.test test module for examples of queries
** that need this heuristic to find good query plans.
**
** This heuristic can be completely disabled, so that no query is
** considered a star-query, using DBSQL_TESTCTRL_OPTIMIZATION to
** disable the DBSQL_StarQuery optimization.  In the CLI, the command
** to do that is:  ".testctrl opt -starquery".
**
** HISTORICAL NOTES:
**
** This optimization was first added on 2024-05-09 by check-in 38db9b5c83d.
** The original optimization reduced the cost and output size estimate for
** fact tables to help them move to outer loops.  But months later (as people
** started upgrading) performance regression reports started caming in,
** including:
**
**    forum post b18ef983e68d06d1 (2024-12-21)
**    forum post 0025389d0860af82 (2025-01-14)
**    forum post d87570a145599033 (2025-01-17)
**
** To address these, the criteria for a star-query was tightened to exclude
** cases where the fact and dimensions are separated by an outer join, and
** the affect of star-schema detection was changed to increase the r_run cost
** on just full table scans of dimension tables, rather than reducing costs
** in the all access methods of the fact table.
*/
static int __compute_mx_choice(where_info_t *w_info){
  int n_loop = w_info->n_level;    /* Number of terms in the join */
  where_loop_t *w_loop;             /* For looping over __where_loops */

#ifdef DBSQL_DEBUG
  /* The star-query detection code below makes use of the following
  ** properties of the where_loop_t list, so verify them before
  ** continuing:
  **    (1)  .mask_self is the bitmask corresponding to .i_tab
  **    (2)  The where_loop_t list is in ascending .i_tab order
  */
  for(w_loop=w_info->loops; w_loop; w_loop=w_loop->next_loop){
    assert( w_loop->mask_self==MASKBIT(w_loop->i_tab) );
    assert( w_loop->next_loop==0 || w_loop->i_tab<=w_loop->next_loop->i_tab );
  }
#endif /* DBSQL_DEBUG */

  if( n_loop>=4  /* Constraint (aa) */
   && !w_info->star_done
   && OPTIMIZATION_ENABLED(w_info->parser->db, DBSQL_StarQuery)
  ){
    src_item_t *from_tabs;    /* All terms of the FROM clause */
    int i_from_idx;          /* Term of FROM clause is the candidate fact-table */
    Bitmask m;             /* Bitmask for candidate fact-table */
    Bitmask m_self_join = 0; /* Tables that cannot be dimension tables */       
    where_loop_t *start;     /* Where to start searching for dimension-tables */

    w_info->star_done = 1; /* Only do this computation once */

    /* Look for fact tables with three or more dimensions where the
    ** dimension tables are not separately from the fact tables by an outer
    ** or cross join.  Adjust cost weights if found.
    */
    assert( !w_info->star_used );
    from_tabs = w_info->tab_list->a;
    start = w_info->loops;
    for(i_from_idx=0, m=1; i_from_idx<n_loop; i_from_idx++, m<<=1){
      int n_dep = 0;             /* Number of dimension tables */
      log_est_t mx_run;             /* Maximum SCAN cost of a fact table */
      Bitmask m_seen = 0;        /* Mask of dimension tables */
      src_item_t *fact_tab;        /* The candidate fact table */

      fact_tab = from_tabs + i_from_idx;
      if( (fact_tab->fg.jointype & (JT_OUTER|JT_CROSS))!=0 ){
        /* If the candidate fact-table is the right table of an outer join
        ** restrict the search for dimension-tables to be tables to the right
        ** of the fact-table.  Constraint (bb) */
        if( i_from_idx+3 > n_loop ){
          break;  /* ^-- Impossible to reach n_dep>=2 - Constraint (cc) */
        }
        while( start && start->i_tab<=i_from_idx ){
          start = start->next_loop;
        }
      }
      for(w_loop=start; w_loop; w_loop=w_loop->next_loop){
        if( (from_tabs[w_loop->i_tab].fg.jointype & (JT_OUTER|JT_CROSS))!=0 ){
          break; /* Constraint (bb) */
        }
        if( (w_loop->prereq & m)!=0        /* w_info depends on i_from_idx */
         && (w_loop->mask_self & m_seen)==0  /* w_info not already a dependency */
         && (w_loop->mask_self & m_self_join)==0 /* Not a self-join */
        ){
          if( from_tabs[w_loop->i_tab].s_tab==fact_tab->s_tab ){
            m_self_join |= m;
          }else{
            n_dep++;
            m_seen |= w_loop->mask_self;
          }
        }
      }
      if( n_dep<=2 ){
        continue; /* Constraint (cc) */
      }

      /* If we reach this point, it means that fact_tab is a fact table
      ** with four or more dimensions connected by inner joins.  Proceed
      ** to make cost adjustments. */
     
#ifdef WHERETRACE_ENABLED
      /* Make sure r_star_delta values are initialized */
      if( !w_info->star_used ){
        for(w_loop=w_info->loops; w_loop; w_loop=w_loop->next_loop){
          w_loop->r_star_delta = 0;
        }
      }
#endif
#ifdef WHERETRACE_ENABLED /* 0x80000 */
      if( __where_trace & 0x80000 ){
        Bitmask m_show = m_seen;
        __debug_printf("Fact table %s(%d), dimensions:",
            fact_tab->alias ? fact_tab->alias : fact_tab->s_tab->name,
            i_from_idx);
        for(w_loop=start; w_loop; w_loop=w_loop->next_loop){
          if( m_show & w_loop->mask_self ){
            src_item_t *dim = from_tabs + w_loop->i_tab;
            m_show &= ~w_loop->mask_self;
            __debug_printf(" %s(%d)",
              dim->alias ? dim->alias: dim->s_tab->name, w_loop->i_tab);
          }
        }
        __debug_printf("\n");
      }
#endif
      w_info->star_used = 1;

      /* Compute the maximum cost of any where_loop_t for the
      ** fact table plus one epsilon */
      mx_run = LOGEST_MIN;
      for(w_loop=start; w_loop; w_loop=w_loop->next_loop){
        if( w_loop->i_tab<i_from_idx ) continue;
        if( w_loop->i_tab>i_from_idx ) break;
        if( w_loop->r_run>mx_run ) mx_run = w_loop->r_run;
      }
      if( ALWAYS(mx_run<LOGEST_MAX) ) mx_run++;

      /* Increase the cost of table scans for dimension tables to be
      ** slightly more than the maximum cost of the fact table */
      for(w_loop=start; w_loop; w_loop=w_loop->next_loop){
        if( (w_loop->mask_self & m_seen)==0 ) continue;
        if( w_loop->n_l_term ) continue;
        if( w_loop->r_run<mx_run ){
#ifdef WHERETRACE_ENABLED /* 0x80000 */
          if( __where_trace & 0x80000 ){
            src_item_t *dim = from_tabs + w_loop->i_tab;
            __debug_printf(
              "Increase SCAN cost of %s to %d\n",
              dim->alias ? dim->alias: dim->s_tab->name, mx_run
            );
          }
          w_loop->r_star_delta = mx_run - w_loop->r_run;
#endif /* WHERETRACE_ENABLED */
          w_loop->r_run = mx_run;
        }
      }
    }
#ifdef WHERETRACE_ENABLED /* 0x80000 */
    if( (__where_trace & 0x80000)!=0 && w_info->star_used ){
      __debug_printf("WhereLoops changed by star-query heuristic:\n");
      for(w_loop=w_info->loops; w_loop; w_loop=w_loop->next_loop){
        if( w_loop->r_star_delta ){
          __where_loop_print(w_loop, &w_info->s_wc);
        }
      }
    }
#endif
  }
  return w_info->star_used ? 18 : 12;
}

/*
** Two where_loop_t objects, candidate and baseline, are known to have the
** same cost.  Look deep into each to see if candidate is even slightly
** better than baseline.  Return false if it is, if candidate is is preferred.
** Return true if baseline is preferred or if we cannot tell the difference.
**
**    Result       Meaning
**    --------     ----------------------------------------------------------
**    true         We cannot tell the difference in candidate and baseline
**    false        candidate seems like a better choice than baseline
*/
static DBSQL_NOINLINE int __where_loop_is_no_better(
  const where_loop_t *candidate,
  const where_loop_t *baseline
){
  if( (candidate->ws_flags & WHERE_INDEXED)==0 ) return 1;
  if( (baseline->ws_flags & WHERE_INDEXED)==0 ) return 1;
  if( candidate->u.btree.p_index->sz_idx_row <
        baseline->u.btree.p_index->sz_idx_row ) return 0;
  return 1;
}

/*
** Given the list of where_loop_t objects at w_info->loops, this routine
** attempts to find the lowest cost path that visits each where_loop_t
** once.  This path is then loaded into the w_info->a[].w_loop fields.
**
** Assume that the total number of output rows that will need to be sorted
** will be n_row_est (in the 10*log2 representation).  Or, ignore sorting
** costs if n_row_est==0.
**
** Return DBSQL_OK on success or DBSQL_NOMEM of a memory allocation
** error occurs.
*/
static int __where_path_solver(where_info_t *w_info, log_est_t n_row_est){
  int mx_choice;             /* Maximum number of simultaneous paths tracked */
  int n_loop;                /* Number of terms in the join */
  parser_t *parser;            /* Parsing context */
  int i_loop;                /* Loop counter over the terms of the join */
  int ii, jj;               /* Loop counters */
  int mx_i = 0;              /* index_t of next entry to replace */
  int n_order_by;             /* Number of ORDER BY clause terms */
  log_est_t mx_cost = 0;        /* Maximum cost of a set of paths */
  log_est_t mx_unsort = 0;      /* Maximum unsorted cost of a set of path */
  int n_to, n_from;           /* Number of valid entries in tos[] and froms[] */
  where_path_t *froms;         /* All n_from paths at the previous level */
  where_path_t *tos;           /* The n_to best paths at the current level */
  where_path_t *from;         /* An element of froms[] that we are working on */
  where_path_t *to;           /* An element of tos[] that we are working on */
  where_loop_t *w_loop;        /* One of the where_loop_t objects */
  where_loop_t **x;           /* Used to divy up the space memory */
  log_est_t *sort_costs = 0;    /* Sorting and partial sorting costs */
  char *space;             /* Temporary memory used by this routine */
  int n_space;               /* Bytes of space allocated at space */

  parser = w_info->parser;
  n_loop = w_info->n_level;
  WHERETRACE(0x002, ("---- begin solver.  (nRowEst=%d, nQueryLoop=%d)\n",
                     n_row_est, parser->n_query_loop));
  /* TUNING: mx_choice is the maximum number of possible paths to preserve
  ** at each step.  Based on the number of loops in the FROM clause:
  **
  **     n_loop      mx_choice
  **     -----      --------
  **       1            1            // the most common case
  **       2            5
  **       3+        12 or 18        // see __compute_mx_choice()
  */
  if( n_loop<=1 ){
    mx_choice = 1;
  }else if( n_loop==2 ){
    mx_choice = 5;
  }else if( parser->n_err ){
    mx_choice = 1;
  }else{
    mx_choice = __compute_mx_choice(w_info);
  }
  assert( n_loop<=w_info->tab_list->n_src );

  /* If n_row_est is zero and there is an ORDER BY clause, ignore it. In this
  ** case the purpose of this call is to estimate the number of rows returned
  ** by the overall query. Once this estimate has been obtained, the caller
  ** will invoke this function a second time, passing the estimate as the
  ** n_row_est parameter.  */
  if( w_info->order_by==0 || n_row_est==0 ){
    n_order_by = 0;
  }else{
    n_order_by = w_info->order_by->n_expr;
  }

  /* Allocate and initialize space for tos, froms and sort_costs[] */
  n_space = (sizeof(where_path_t)+sizeof(where_loop_t*)*n_loop)*mx_choice*2;
  n_space += sizeof(log_est_t) * n_order_by;
  space = __stack_alloc_raw_nn(parser->db, n_space);
  if( space==0 ) return DBSQL_NOMEM_BKPT;
  tos = (where_path_t*)space;
  froms = tos+mx_choice;
  memset(froms, 0, sizeof(froms[0]));
  x = (where_loop_t**)(froms+mx_choice);
  for(ii=mx_choice*2, from=tos; ii>0; ii--, from++, x += n_loop){
    from->loops = x;
  }
  if( n_order_by ){
    /* If there is an ORDER BY clause and it is not being ignored, set up
    ** space for the sort_costs[] array. Each element of the sort_costs array
    ** is either zero - meaning it has not yet been initialized - or the
    ** cost of sorting n_row_est rows of data where the first X terms of
    ** the ORDER BY clause are already in order, where X is the array
    ** index.  */
    sort_costs = (log_est_t*)x;
    memset(sort_costs, 0, sizeof(log_est_t) * n_order_by);
  }
  assert( sort_costs==0 || &space[n_space]==(char*)&sort_costs[n_order_by] );
  assert( sort_costs!=0 || &space[n_space]==(char*)x );

  /* Seed the search with a single where_path_t containing zero __where_loops.
  **
  ** TUNING: Do not let the number of iterations go above 28.  If the cost
  ** of computing an automatic index is not paid back within the first 28
  ** rows, then do not use the automatic index. */
  froms[0].n_row = MIN(parser->n_query_loop, 48);  assert( 48==__log_est(28) );
  n_from = 1;
  assert( froms[0].is_ordered==0 );
  if( n_order_by ){
    /* If n_loop is zero, then there are no FROM terms in the query. Since
    ** in this case the query may return a maximum of one row, the results
    ** are already in the requested order. Set is_ordered to n_order_by to
    ** indicate this. Or, if n_loop is greater than zero, set is_ordered to
    ** -1, indicating that the result set may or may not be ordered,
    ** depending on the loops added to the current plan.  */
    froms[0].is_ordered = n_loop>0 ? -1 : n_order_by;
  }

  /* Compute successively longer __where_paths using the previous generation
  ** of __where_paths as the basis for the next.  Keep track of the mx_choice
  ** best paths at each generation */
  for(i_loop=0; i_loop<n_loop; i_loop++){
    n_to = 0;
    for(ii=0, from=froms; ii<n_from; ii++, from++){
      for(w_loop=w_info->loops; w_loop; w_loop=w_loop->next_loop){
        log_est_t n_out;                      /* Rows visited by (from+w_loop) */
        log_est_t r_cost;                     /* Cost of path (from+w_loop) */
        log_est_t r_unsort;                   /* Unsorted cost of (from+w_loop) */
        int8_t is_ordered;                     /* is_ordered for (from+w_loop) */
        Bitmask mask_new;                  /* Mask of src visited by (..) */
        Bitmask rev_mask;                  /* Mask of rev-order loops for (..) */

        if( (w_loop->prereq & ~from->mask_loop)!=0 ) continue;
        if( (w_loop->mask_self & from->mask_loop)!=0 ) continue;
        if( (w_loop->ws_flags & WHERE_AUTO_INDEX)!=0 && from->n_row<3 ){
          /* Do not use an automatic index if the this loop is expected
          ** to run less than 1.25 times.  It is tempting to also exclude
          ** automatic index usage on an outer loop, but sometimes an automatic
          ** index is useful in the outer loop of a correlated subquery. */
          assert( 10==__log_est(2) );
          continue;
        }

        /* At this point, w_loop is a candidate to be the next loop.
        ** Compute its cost */
        r_unsort = w_loop->r_run + from->n_row;
        if( w_loop->r_setup ){
          r_unsort = __log_est_add(w_loop->r_setup, r_unsort);
        }
        r_unsort = __log_est_add(r_unsort, from->r_unsort);
        n_out = from->n_row + w_loop->n_out;
        mask_new = from->mask_loop | w_loop->mask_self;
        is_ordered = from->is_ordered;
        if( is_ordered<0 ){
          rev_mask = 0;
          is_ordered = __where_path_satisfies_order_by(w_info,
                       w_info->order_by, from, w_info->wctrl_flags,
                       i_loop, w_loop, &rev_mask);
        }else{
          rev_mask = from->rev_loop;
        }
        if( is_ordered>=0 && is_ordered<n_order_by ){
          if( sort_costs[is_ordered]==0 ){
            sort_costs[is_ordered] = __where_sorting_cost(
                w_info, n_row_est, n_order_by, is_ordered
            );
          }
          /* TUNING:  Add a small extra penalty (3) to sorting as an
          ** extra encouragement to the query planner to select a plan
          ** where the rows emerge in the correct order without any sorting
          ** required. */
          r_cost = __log_est_add(r_unsort, sort_costs[is_ordered]) + 3;

          WHERETRACE(0x002,
              ("---- sort cost=%-3d (%d/%d) increases cost %3d to %-3d\n",
               sort_costs[is_ordered], (n_order_by-is_ordered), n_order_by,
               r_unsort, r_cost));
        }else{
          r_cost = r_unsort;
          r_unsort -= 2;  /* TUNING:  Slight bias in favor of no-sort plans */
        }

        /* Check to see if w_loop should be added to the set of
        ** mx_choice best-so-far paths.
        **
        ** First look for an existing path among best-so-far paths
        ** that:
        **     (1) covers the same set of loops, and
        **     (2) has a compatible is_ordered value.
        **
        ** "Compatible is_ordered value" means either
        **     (A) both have is_ordered==-1, or
        **     (B) both have is_order>=0, or
        **     (C) ordering does not matter because this is the last round
        **         of the solver.
        **
        ** The term "((to->is_ordered^is_ordered)&0x80)==0" is equivalent
        ** to (to->is_ordered==(-1))==(is_ordered==(-1))" for the range
        ** of legal values for is_ordered, -1..64.
        */
        testcase( n_to==0 );
        for(jj=0, to=tos; jj<n_to; jj++, to++){
          if( to->mask_loop==mask_new
           && ( ((to->is_ordered^is_ordered)&0x80)==0 || i_loop==n_loop-1 )
          ){
            testcase( jj==n_to-1 );
            break;
          }
        }
        if( jj>=n_to ){
          /* None of the existing best-so-far paths match the candidate. */
          if( n_to>=mx_choice
           && (r_cost>mx_cost || (r_cost==mx_cost && r_unsort>=mx_unsort))
          ){
            /* The current candidate is no better than any of the mx_choice
            ** paths currently in the best-so-far buffer.  So discard
            ** this candidate as not viable. */
#ifdef WHERETRACE_ENABLED /* 0x4 */
            if( __where_trace&0x4 ){
              __debug_printf("Skip   %s cost=%-3d,%3d,%3d order=%c\n",
                  __where_path_name(from, i_loop, w_loop), r_cost, n_out, r_unsort,
                  is_ordered>=0 ? is_ordered+'0' : '?');
            }
#endif
            continue;
          }
          /* If we reach this points it means that the new candidate path
          ** needs to be added to the set of best-so-far paths. */
          if( n_to<mx_choice ){
            /* Increase the size of the tos set by one */
            jj = n_to++;
          }else{
            /* New path replaces the prior worst to keep count below mx_choice */
            jj = mx_i;
          }
          to = &tos[jj];
#ifdef WHERETRACE_ENABLED /* 0x4 */
          if( __where_trace&0x4 ){
            __debug_printf("New    %s cost=%-3d,%3d,%3d order=%c\n",
                __where_path_name(from, i_loop, w_loop), r_cost, n_out, r_unsort,
                is_ordered>=0 ? is_ordered+'0' : '?');
          }
#endif
        }else{
          /* Control reaches here if best-so-far path to=tos[jj] covers the
          ** same set of loops and has the same is_ordered setting as the
          ** candidate path.  Check to see if the candidate should replace
          ** to or if the candidate should be skipped.
          **
          ** The conditional is an expanded vector comparison equivalent to:
          **   (to->r_cost,to->n_row,to->r_unsort) <= (r_cost,n_out,r_unsort)
          */
          if( (to->r_cost<r_cost)
           || (to->r_cost==r_cost && to->n_row<n_out)
           || (to->r_cost==r_cost && to->n_row==n_out && to->r_unsort<r_unsort)
           || (to->r_cost==r_cost && to->n_row==n_out && to->r_unsort==r_unsort
                  && __where_loop_is_no_better(w_loop, to->loops[i_loop]) )
          ){
#ifdef WHERETRACE_ENABLED /* 0x4 */
            if( __where_trace&0x4 ){
              __debug_printf(
                  "Skip   %s cost=%-3d,%3d,%3d order=%c",
                  __where_path_name(from, i_loop, w_loop), r_cost, n_out, r_unsort,
                  is_ordered>=0 ? is_ordered+'0' : '?');
              __debug_printf("   vs %s cost=%-3d,%3d,%3d order=%c\n",
                  __where_path_name(to, i_loop+1, 0), to->r_cost, to->n_row,
                  to->r_unsort, to->is_ordered>=0 ? to->is_ordered+'0' : '?');
            }
#endif
            /* Discard the candidate path from further consideration */
            testcase( to->r_cost==r_cost );
            continue;
          }
          testcase( to->r_cost==r_cost+1 );
          /* Control reaches here if the candidate path is better than the
          ** to path.  Replace to with the candidate. */
#ifdef WHERETRACE_ENABLED /* 0x4 */
          if( __where_trace&0x4 ){
            __debug_printf(
                "Update %s cost=%-3d,%3d,%3d order=%c",
                __where_path_name(from, i_loop, w_loop), r_cost, n_out, r_unsort,
                is_ordered>=0 ? is_ordered+'0' : '?');
            __debug_printf("  was %s cost=%-3d,%3d,%3d order=%c\n",
                __where_path_name(to, i_loop+1, 0), to->r_cost, to->n_row,
                to->r_unsort, to->is_ordered>=0 ? to->is_ordered+'0' : '?');
          }
#endif
        }
        /* w_loop is a winner.  Add it to the set of best so far */
        to->mask_loop = from->mask_loop | w_loop->mask_self;
        to->rev_loop = rev_mask;
        to->n_row = n_out;
        to->r_cost = r_cost;
        to->r_unsort = r_unsort;
        to->is_ordered = is_ordered;
        memcpy(to->loops, from->loops, sizeof(where_loop_t*)*i_loop);
        to->loops[i_loop] = w_loop;
        if( n_to>=mx_choice ){
          mx_i = 0;
          mx_cost = tos[0].r_cost;
          mx_unsort = tos[0].n_row;
          for(jj=1, to=&tos[1]; jj<mx_choice; jj++, to++){
            if( to->r_cost>mx_cost
             || (to->r_cost==mx_cost && to->r_unsort>mx_unsort)
            ){
              mx_cost = to->r_cost;
              mx_unsort = to->r_unsort;
              mx_i = jj;
            }
          }
        }
      }
    }

#ifdef WHERETRACE_ENABLED  /* >=2 */
    if( __where_trace & 0x02 ){
      log_est_t r_min, r_floor = 0;
      int n_done = 0;
      int n_progress;
      __debug_printf("---- after round %d ----\n", i_loop);
      do{
        n_progress = 0;
        r_min = 0x7fff;
        for(ii=0, to=tos; ii<n_to; ii++, to++){
          if( to->r_cost>r_floor && to->r_cost<r_min ) r_min = to->r_cost;
        }
        for(ii=0, to=tos; ii<n_to; ii++, to++){
          if( to->r_cost==r_min ){
            __debug_printf(" %s cost=%-3d nrow=%-3d order=%c",
               __where_path_name(to, i_loop+1, 0), to->r_cost, to->n_row,
               to->is_ordered>=0 ? (to->is_ordered+'0') : '?');
            if( to->is_ordered>0 ){
              __debug_printf(" rev=0x%llx\n", to->rev_loop);
            }else{
              __debug_printf("\n");
            }
            n_done++;
            n_progress++;
          }
        }
        r_floor = r_min;
      }while( n_done<n_to && n_progress>0 );
    }
#endif

    /* Swap the roles of froms and tos for the next generation */
    from = tos;
    tos = froms;
    froms = from;
    n_from = n_to;
  }

  if( n_from==0 ){
    __error_msg(parser, "no query solution");
    __stack_free_nn(parser->db, space);
    return DBSQL_ERROR;
  }
 
  /* Only one path is available, which is the best path */
  assert( n_from==1 );
  from = froms;

  assert( w_info->n_level==n_loop );
  /* Load the lowest cost path into w_info */
  for(i_loop=0; i_loop<n_loop; i_loop++){
    where_level_t *level = w_info->a + i_loop;
    level->w_loop = w_loop = from->loops[i_loop];
    level->i_from = w_loop->i_tab;
    level->i_tab_cur = w_info->tab_list->a[level->i_from].i_cursor;
  }
  if( (w_info->wctrl_flags & WHERE_WANT_DISTINCT)!=0
   && (w_info->wctrl_flags & WHERE_DISTINCTBY)==0
   && w_info->distinct==WHERE_DISTINCT_NOOP
   && n_row_est
  ){
    Bitmask not_used;
    int rc = __where_path_satisfies_order_by(w_info, w_info->result_set, from,
                 WHERE_DISTINCTBY, n_loop-1, from->loops[n_loop-1], &not_used);
    if( rc==w_info->result_set->n_expr ){
      w_info->distinct = WHERE_DISTINCT_ORDERED;
    }
  }
  w_info->ordered_inner_loop = 0;
  if( w_info->order_by ){
    w_info->n_ob_sat = from->is_ordered;
    if( w_info->wctrl_flags & WHERE_DISTINCTBY ){
      if( from->is_ordered==w_info->order_by->n_expr ){
        w_info->distinct = WHERE_DISTINCT_ORDERED;
      }
      /* vvv--- See check-in [12ad822d9b827777] on 2023-03-16 ---vvv */
      assert( w_info->p_select->order_by==0
           || w_info->n_ob_sat <= w_info->p_select->order_by->n_expr );
    }else{
      w_info->rev_mask = from->rev_loop;
      if( w_info->n_ob_sat<=0 ){
        w_info->n_ob_sat = 0;
        if( n_loop>0 ){
          u_int32_t ws_flags = from->loops[n_loop-1]->ws_flags;
          if( (ws_flags & WHERE_ONEROW)==0
           && (ws_flags&(WHERE_IPK|WHERE_COLUMN_IN))!=(WHERE_IPK|WHERE_COLUMN_IN)
          ){
            Bitmask m = 0;
            int rc = __where_path_satisfies_order_by(w_info, w_info->order_by, from,
                      WHERE_ORDERBY_LIMIT, n_loop-1, from->loops[n_loop-1], &m);
            testcase( ws_flags & WHERE_IPK );
            testcase( ws_flags & WHERE_COLUMN_IN );
            if( rc==w_info->order_by->n_expr ){
              w_info->ordered_inner_loop = 1;
              w_info->rev_mask = m;
            }
          }
        }
      }else if( n_loop
            && w_info->n_ob_sat==1
            && (w_info->wctrl_flags & (WHERE_ORDERBY_MIN|WHERE_ORDERBY_MAX))!=0
            ){
        w_info->ordered_inner_loop = 1;
      }
    }
    if( (w_info->wctrl_flags & WHERE_SORTBYGROUP)
        && w_info->n_ob_sat==w_info->order_by->n_expr && n_loop>0
    ){
      Bitmask rev_mask = 0;
      int n_order = __where_path_satisfies_order_by(w_info, w_info->order_by,
          from, 0, n_loop-1, from->loops[n_loop-1], &rev_mask
      );
      assert( w_info->sorted==0 );
      if( n_order==w_info->order_by->n_expr ){
        w_info->sorted = 1;
        w_info->rev_mask = rev_mask;
      }
    }
  }

  w_info->n_row_out = from->n_row;
#ifdef WHERETRACE_ENABLED
  w_info->r_total_cost = from->r_cost;
#endif

  /* Free temporary memory and return success */
  __stack_free_nn(parser->db, space);
  return DBSQL_OK;
}

/*
** This routine implements a heuristic designed to improve query planning.
** This routine is called in between the first and second call to
** __where_path_solver().  Hence the name "Interstage" "Heuristic".
**
** The first call to __where_path_solver() (hereafter just "solver()") computes
** the best path without regard to the order of the outputs.  The second call
** to the solver() builds upon the first call to try to find an alternative
** path that satisfies the ORDER BY clause.
**
** This routine looks at the results of the first solver() run, and for
** every FROM clause term in the resulting query plan that uses an equality
** constraint against an index, disable other __where_loops for that same
** FROM clause term that would try to do a full-table scan.  This prevents
** an index search from being converted into a full-table scan in order to
** satisfy an ORDER BY clause, since even though we might get slightly better
** performance using the full-scan without sorting if the output size
** estimates are very precise, we might also get severe performance
** degradation using the full-scan if the output size estimate is too large.
** It is better to err on the side of caution.
**
** Except, if the first solver() call generated a full-table scan in an outer
** loop then stop this analysis at the first full-scan, since the second
** solver() run might try to swap that full-scan for another in order to
** get the output into the correct order.  In other words, we allow a
** rewrite like this:
**
**     First Solver()                      Second Solver()
**       |-- SCAN t1                         |-- SCAN t2
**       |-- SEARCH t2                       `-- SEARCH t1
**       `-- SORT USING B-TREE
**
** The purpose of this routine is to disallow rewrites such as:
**
**     First Solver()                      Second Solver()
**       |-- SEARCH t1                       |-- SCAN t2     <--- bad!
**       |-- SEARCH t2                       `-- SEARCH t1
**       `-- SORT USING B-TREE
**
** See test cases in test/where_n.test for the real-world query that
** originally provoked this heuristic.
*/
static DBSQL_NOINLINE void __where_interstage_heuristic(where_info_t *w_info){
  int i;
#ifdef WHERETRACE_ENABLED
  int once = 0;
#endif
  for(i=0; i<w_info->n_level; i++){
    where_loop_t *p = w_info->a[i].w_loop;
    if( p==0 ) break;
    if( (p->ws_flags & WHERE_VIRTUALTABLE)!=0 ){
      /* Treat a vtab scan as similar to a full-table scan */
      break;
    }
    if( (p->ws_flags & (WHERE_COLUMN_EQ|WHERE_COLUMN_NULL|WHERE_COLUMN_IN))!=0 ){
      u_int8_t i_tab = p->i_tab;
      where_loop_t *loop;
      for(loop=w_info->loops; loop; loop=loop->next_loop){
        if( loop->i_tab!=i_tab ) continue;
        if( (loop->ws_flags & (WHERE_CONSTRAINT|WHERE_AUTO_INDEX))!=0 ){
          /* Auto-index and index-constrained loops allowed to remain */
          continue;
        }
#ifdef WHERETRACE_ENABLED
        if( __where_trace & 0x80 ){
          if( once==0 ){
            __debug_printf("Loops disabled by interstage heuristic:\n");
            once = 1;
          }
          __where_loop_print(loop, &w_info->s_wc);
        }
#endif /* WHERETRACE_ENABLED */
        loop->prereq = ALLBITS;  /* Prevent 2nd solver() from using this one */
      }
    }else{
      break;
    }
  }
}

/*
** Most queries use only a single table (they are not joins) and have
** simple == constraints against indexed fields.  This routine attempts
** to plan those simple cases using much less ceremony than the
** general-purpose query planner, and thereby yield faster dbsql_prepare()
** times for the common case.
**
** Return non-zero on success, if this query can be handled by this
** no-frills query planner.  Return zero if this query needs the
** general-purpose query planner.
*/
static int __where_short_cut(where_loop_builder_t *builder){
  where_info_t *w_info;
  src_item_t *item;
  where_clause_t *wc;
  where_term_t *term;
  where_loop_t *loop;
  int i_cur;
  int j;
  table_t *tab;
  index_t *idx;
  where_scan_t scan;

  w_info = builder->w_info;
  if( w_info->wctrl_flags & WHERE_OR_SUBCLAUSE ) return 0;
  assert( w_info->tab_list->n_src>=1 );
  item = w_info->tab_list->a;
  tab = item->s_tab;
  if( IS_VIRTUAL(tab) ) return 0;
  if( item->fg.is_indexed_by || item->fg.not_indexed ){
    testcase( item->fg.is_indexed_by );
    testcase( item->fg.not_indexed );
    return 0;
  }
  i_cur = item->i_cursor;
  wc = &w_info->s_wc;
  loop = builder->p_new;
  loop->ws_flags = 0;
  loop->n_skip = 0;
  term = __where_scan_init(&scan, wc, i_cur, -1, WO_EQ|WO_IS, 0);
  while( term && term->prereq_right ) term = __where_scan_next(&scan);
  if( term ){
    testcase( term->e_operator & WO_IS );
    loop->ws_flags = WHERE_COLUMN_EQ|WHERE_IPK|WHERE_ONEROW;
    loop->l_terms[0] = term;
    loop->n_l_term = 1;
    loop->u.btree.n_eq = 1;
    /* TUNING: Cost of a rowid lookup is 10 */
    loop->r_run = 33;  /* 33==__log_est(10) */
  }else{
    for(idx=tab->p_index; idx; idx=idx->next){
      int op_mask;
      assert( loop->l_term_spaces==loop->l_terms );
      if( !IS_UNIQUE_INDEX(idx)
       || idx->part_idx_where!=0
       || idx->n_key_col>ARRAY_SIZE(loop->l_term_spaces)
      ) continue;
      op_mask = idx->uniq_not_null ? (WO_EQ|WO_IS) : WO_EQ;
      for(j=0; j<idx->n_key_col; j++){
        term = __where_scan_init(&scan, wc, i_cur, j, op_mask, idx);
        while( term && term->prereq_right ) term = __where_scan_next(&scan);
        if( term==0 ) break;
        testcase( term->e_operator & WO_IS );
        loop->l_terms[j] = term;
      }
      if( j!=idx->n_key_col ) continue;
      loop->ws_flags = WHERE_COLUMN_EQ|WHERE_ONEROW|WHERE_INDEXED;
      if( idx->is_covering || (item->col_used & idx->col_not_idxed)==0 ){
        loop->ws_flags |= WHERE_IDX_ONLY;
      }
      loop->n_l_term = j;
      loop->u.btree.n_eq = j;
      loop->u.btree.p_index = idx;
      /* TUNING: Cost of a unique index lookup is 15 */
      loop->r_run = 39;  /* 39==__log_est(15) */
      break;
    }
  }
  if( loop->ws_flags ){
    loop->n_out = (log_est_t)1;
    w_info->a[0].w_loop = loop;
    assert( w_info->s_mask_set.n==1 && i_cur==w_info->s_mask_set.ix[0] );
    loop->mask_self = 1; /* __where_get_mask(&w_info->s_mask_set, i_cur); */
    w_info->a[0].i_tab_cur = i_cur;
    w_info->n_row_out = 1;
    if( w_info->order_by ) w_info->n_ob_sat =  w_info->order_by->n_expr;
    if( w_info->wctrl_flags & WHERE_WANT_DISTINCT ){
      w_info->distinct = WHERE_DISTINCT_UNIQUE;
    }
    if( scan.i_equiv>1 ) loop->ws_flags |= WHERE_TRANSCONS;
#ifdef DBSQL_DEBUG
    loop->c_id = '0';
#endif
#ifdef WHERETRACE_ENABLED
    if( __where_trace & 0x02 ){
      __debug_printf("whereShortCut() used to compute solution\n");
    }
#endif
    return 1;
  }
  return 0;
}

/*
** Helper function for __expr_is_deterministic().
*/
static int __expr_node_is_deterministic(walker_t *walker, expr_t *expr){
  if( expr->op==TK_FUNCTION && EXPR_HAS_PROPERTY(expr, EP_ConstFunc)==0 ){
    walker->code = 0;
    return WRC_Abort;
  }
  return WRC_Continue;
}

/*
** Return true if the expression contains no non-deterministic SQL
** functions. Do not consider non-deterministic SQL functions that are
** part of sub-select statements.
*/
static int __expr_is_deterministic(expr_t *p){
  walker_t w;
  memset(&w, 0, sizeof(w));
  w.code = 1;
  w.expr_callback = __expr_node_is_deterministic;
  w.select_callback = __select_walk_fail;
  __walk_expr(&w, p);
  return w.code;
}

 
#ifdef WHERETRACE_ENABLED
/*
** Display all __where_loops in w_info
*/
static void __show_all_where_loops(where_info_t *w_info, where_clause_t *wc){
  if( __where_trace ){    /* Display all of the where_loop_t objects */
    where_loop_t *p;
    int i;
    static const char label[] = "0123456789abcdefghijklmnopqrstuvwyxz"
                                           "ABCDEFGHIJKLMNOPQRSTUVWYXZ";
    for(p=w_info->loops, i=0; p; p=p->next_loop, i++){
      p->c_id = label[i%(sizeof(label)-1)];
      __where_loop_print(p, wc);
    }
  }
}
# define WHERETRACE_ALL_LOOPS(W,C) __show_all_where_loops(W,C)
#else
# define WHERETRACE_ALL_LOOPS(W,C)
#endif

/* Attempt to omit tables from a join that do not affect the result.
** For a table to not affect the result, the following must be true:
**
**   1) The query must not be an aggregate.
**   2) The table must be the RHS of a LEFT JOIN.
**   3) Either the query must be DISTINCT, or else the ON or USING clause
**      must contain a constraint that limits the scan of the table to
**      at most a single row.
**   4) The table must not be referenced by any part of the query apart
**      from its own USING or ON clause.
**   5) The table must not have an inner-join ON or USING clause if there is
**      a RIGHT JOIN anywhere in the query.  Otherwise the ON/USING clause
**      might move from the right side to the left side of the RIGHT JOIN.
**      Note: Due to (2), this condition can only arise if the table is
**      the right-most table of a subquery that was flattened into the
**      main query and that subquery was the right-hand operand of an
**      inner join that held an ON or USING clause.
**   6) The ORDER BY clause has 63 or fewer terms
**   7) The omit-noop-join optimization is enabled.
**
** Items (1), (6), and (7) are checked by the caller.
**
** For example, given:
**
**     CREATE TABLE t1(ipk INTEGER PRIMARY KEY, v1);
**     CREATE TABLE t2(ipk INTEGER PRIMARY KEY, v2);
**     CREATE TABLE t3(ipk INTEGER PRIMARY KEY, v3);
**
** then table t2 can be omitted from the following:
**
**     SELECT v1, v3 FROM t1
**       LEFT JOIN t2 ON (t1.ipk=t2.ipk)
**       LEFT JOIN t3 ON (t1.ipk=t3.ipk)
**
** or from:
**
**     SELECT DISTINCT v1, v3 FROM t1
**       LEFT JOIN t2
**       LEFT JOIN t3 ON (t1.ipk=t3.ipk)
*/
static DBSQL_NOINLINE Bitmask __where_omit_noop_join(
  where_info_t *w_info,
  Bitmask not_ready
){
  int i;
  Bitmask tab_used;
  int HAS_RIGHT_JOIN;

  /* Preconditions checked by the caller */
  assert( w_info->n_level>=2 );
  assert( OPTIMIZATION_ENABLED(w_info->parser->db, DBSQL_OmitNoopJoin) );

  /* These two preconditions checked by the caller combine to guarantee
  ** condition (1) of the header comment */
  assert( w_info->result_set!=0 );
  assert( 0==(w_info->wctrl_flags & WHERE_AGG_DISTINCT) );

  tab_used = __where_expr_list_usage(&w_info->s_mask_set, w_info->result_set);
  if( w_info->order_by ){
    tab_used |= __where_expr_list_usage(&w_info->s_mask_set, w_info->order_by);
  }
  HAS_RIGHT_JOIN = (w_info->tab_list->a[0].fg.jointype & JT_LTORJ)!=0;
  for(i=w_info->n_level-1; i>=1; i--){
    where_term_t *term, *end;
    src_item_t *item;
    where_loop_t *loop;
    Bitmask m1;
    loop = w_info->a[i].w_loop;
    item = &w_info->tab_list->a[loop->i_tab];
    if( (item->fg.jointype & (JT_LEFT|JT_RIGHT))!=JT_LEFT ) continue;
    if( (w_info->wctrl_flags & WHERE_WANT_DISTINCT)==0
     && (loop->ws_flags & WHERE_ONEROW)==0
    ){
      continue;
    }
    if( (tab_used & loop->mask_self)!=0 ) continue;
    end = w_info->s_wc.a + w_info->s_wc.n_term;
    for(term=w_info->s_wc.a; term<end; term++){
      if( (term->prereq_all & loop->mask_self)!=0 ){
        if( !EXPR_HAS_PROPERTY(term->expr, EP_OuterON)
         || term->expr->w.i_join!=item->i_cursor
        ){
          break;
        }
      }
      if( HAS_RIGHT_JOIN
       && EXPR_HAS_PROPERTY(term->expr, EP_InnerON)
       && NEVER(term->expr->w.i_join==item->i_cursor)
      ){
        break;  /* restriction (5) */
      }
    }
    if( term<end ) continue;
    WHERETRACE(0xffffffff,("-> omit unused FROM-clause term %c\n",loop->c_id));
    m1 = MASKBIT(i)-1;
    testcase( ((w_info->rev_mask>>1) & ~m1)!=0 );
    w_info->rev_mask = (m1 & w_info->rev_mask) | ((w_info->rev_mask>>1) & ~m1);
    not_ready &= ~loop->mask_self;
    for(term=w_info->s_wc.a; term<end; term++){
      if( (term->prereq_all & loop->mask_self)!=0 ){
        term->wt_flags |= TERM_CODED;
        term->prereq_all = 0;
      }
    }
    if( i!=w_info->n_level-1 ){
      int n_byte = (w_info->n_level-1-i) * sizeof(where_level_t);
      memmove(&w_info->a[i], &w_info->a[i+1], n_byte);
    }
    w_info->n_level--;
    assert( w_info->n_level>0 );
  }
  return not_ready;
}

/*
** Check to see if there are any SEARCH loops that might benefit from
** using a Bloom filter.  Consider a Bloom filter if:
**
**   (1)  The SEARCH happens more than N times where N is the number
**        of rows in the table that is being considered for the Bloom
**        filter.
**   (2)  Some searches are expected to find zero rows.  (This is determined
**        by the WHERE_SELFCULL flag on the term.)
**   (3)  Bloom-filter processing is not disabled.  (Checked by the
**        caller.)
**   (4)  The size of the table being searched is known by ANALYZE.
**
** This block of code merely checks to see if a Bloom filter would be
** appropriate, and if so sets the WHERE_BLOOMFILTER flag on the
** where_loop_t.  The implementation of the Bloom filter comes further
** down where the code for each where_loop_t is generated.
*/
static DBSQL_NOINLINE void __where_check_if_bloom_filter_is_useful(
  const where_info_t *w_info
){
  int i;
  log_est_t n_search = 0;

  assert( w_info->n_level>=2 );
  assert( OPTIMIZATION_ENABLED(w_info->parser->db, DBSQL_BloomFilter) );
  for(i=0; i<w_info->n_level; i++){
    where_loop_t *loop = w_info->a[i].w_loop;
    const unsigned int req_flags = (WHERE_SELFCULL|WHERE_COLUMN_EQ);
    src_item_t *item = &w_info->tab_list->a[loop->i_tab];
    table_t *tab = item->s_tab;
    if( (tab->tab_flags & TF_HasStat1)==0 ) break;
    tab->tab_flags |= TF_MaybeReanalyze;
    if( i>=1
     && (loop->ws_flags & req_flags)==req_flags
     /* vvvvvv--- Always the case if WHERE_COLUMN_EQ is defined */
     && ALWAYS((loop->ws_flags & (WHERE_IPK|WHERE_INDEXED))!=0)
    ){
      if( n_search > tab->n_row_log_est ){
        testcase( item->fg.jointype & JT_LEFT );
        loop->ws_flags |= WHERE_BLOOMFILTER;
        loop->ws_flags &= ~WHERE_IDX_ONLY;
        WHERETRACE(0xffffffff, (
           "-> use Bloom-filter on loop %c because there are ~%.1e "
           "lookups into %s which has only ~%.1e rows\n",
           loop->c_id, (double)__log_est_to_int(n_search), tab->name,
           (double)__log_est_to_int(tab->n_row_log_est)));
      }
    }
    n_search += loop->n_out;
  }
}

/*
** The index idx is used by a query and contains one or more expressions.
** In other words idx is an index on an expression.  i_idx_cur is the cursor
** number for the index and i_data_cur is the cursor number for the corresponding
** table.
**
** This routine adds indexed_expr_t entries to the parser_t->idx_epr field for
** each of the expressions in the index so that the expression code generator
** will know to replace occurrences of the indexed expression with
** references to the corresponding column of the index.
*/
static DBSQL_NOINLINE void __where_add_indexed_expr(
  parser_t *parser,     /* Add indexed_expr_t entries to parser->idx_epr */
  index_t *idx,       /* The index-on-expression that contains the expressions */
  int i_idx_cur,       /* Cursor number for idx */
  src_item_t *tab_item  /* The FROM clause entry for the table */
){
  int i;
  indexed_expr_t *p;
  table_t *tab;
  assert( idx->has_expr );
  tab = idx->table;
  for(i=0; i<idx->n_column; i++){
    expr_t *expr;
    int j = idx->columns[i];
    if( j==XN_EXPR ){
      expr = idx->col_exprs->a[i].expr;
    }else if( j>=0 && (tab->cols[j].col_flags & COLFLAG_VIRTUAL)!=0 ){
      expr = __column_expr(tab, &tab->cols[j]);
    }else{
      continue;
    }
    if( __expr_is_constant(0,expr) ) continue;
    p = __db_malloc_raw(parser->db,  sizeof(indexed_expr_t));
    if( p==0 ) break;
    p->ie_next = parser->idx_epr;
#ifdef WHERETRACE_ENABLED
    if( __where_trace & 0x200 ){
      __debug_printf("New pParse->pIdxEpr term {%d,%d}\n", i_idx_cur, i);
      if( __where_trace & 0x5000 ) __show_expr(expr);
    }
#endif
    p->expr = __expr_dup(parser->db, expr, 0);
    p->i_data_cur = tab_item->i_cursor;
    p->i_idx_cur = i_idx_cur;
    p->i_idx_col = i;
    p->maybe_null_row = (tab_item->fg.jointype & (JT_LEFT|JT_LTORJ|JT_RIGHT))!=0;
    if( __index_affinity_str(parser->db, idx) ){
      p->aff = idx->col_aff[i];
    }
#ifdef DBSQL_ENABLE_EXPLAIN_COMMENTS
    p->idx_name = idx->name;
#endif
    parser->idx_epr = p;
    if( p->ie_next==0 ){
      void *arg = (void*)&parser->idx_epr;
      __parser_add_cleanup(parser, __where_indexed_expr_cleanup, arg);
    }
  }
}

/*
** Set the reverse-scan order mask to one for all tables in the query
** with the exception of MATERIALIZED common table expressions that have
** their own internal ORDER BY clauses.
**
** This implements the PRAGMA reverse_unordered_selects=ON setting.
** (Also DBSQL_DBCONFIG_REVERSE_SCANORDER).
*/
static DBSQL_NOINLINE void __where_reverse_scan_order(where_info_t *w_info){
  int ii;
  for(ii=0; ii<w_info->tab_list->n_src; ii++){
    src_item_t *item = &w_info->tab_list->a[ii];
    if( !item->fg.is_cte
     || item->u2.cte_use->m10d!=M10D_YES
     || NEVER(item->fg.is_subquery==0)
     || item->u4.subq->p_select->order_by==0
    ){
      w_info->rev_mask |= MASKBIT(ii);
    }
  }
}

/*
** Generate the beginning of the loop used for WHERE clause processing.
** The return value is a pointer to an opaque structure that contains
** information needed to terminate the loop.  Later, the calling routine
** should invoke __where_end() with the return value of this function
** in order to complete the WHERE clause processing.
**
** If an error occurs, this routine returns NULL.
**
** The basic idea is to do a nested loop, one loop for each table in
** the FROM clause of a select.  (INSERT and UPDATE statements are the
** same as a SELECT with only a single table in the FROM clause.)  For
** example, if the SQL is this:
**
**       SELECT * FROM t1, t2, t3 WHERE ...;
**
** Then the code generated is conceptually like the following:
**
**      foreach row1 in t1 do       \    Code generated
**        foreach row2 in t2 do      |-- by __where_begin()
**          foreach row3 in t3 do   /
**            ...
**          end                     \    Code generated
**        end                        |-- by __where_end()
**      end                         /
**
** Note that the loops might not be nested in the order in which they
** appear in the FROM clause if a different order is better able to make
** use of indices.  Note also that when the IN operator appears in
** the WHERE clause, it might result in additional nested loops for
** scanning through all values on the right-hand side of the IN.
**
** There are sm_t cursors associated with each table.  t1 uses cursor
** number tab_list->a[0].i_cursor.  t2 uses the cursor tab_list->a[1].i_cursor.
** And so forth.  This routine generates code to open those VDBE cursors
** and __where_end() generates the code to close them.
**
** The code that __where_begin() generates leaves the cursors named
** in tab_list pointing at their appropriate entries.  The [...] code
** can use OP_Column and OP_Rowid opcodes on these cursors to extract
** data from the various tables of the loop.
**
** If the WHERE clause is empty, the foreach loops must each scan their
** entire tables.  Thus a three-way join is an O(N^3) operation.  But if
** the tables have indices and there are terms in the WHERE clause that
** refer to those indices, a complete table scan can be avoided and the
** code will run much faster.  Most of the work of this routine is checking
** to see if there are indices that can be used to speed up the loop.
**
** Terms of the WHERE clause are also used to limit which rows actually
** make it to the "..." in the middle of the loop.  After each "foreach",
** terms of the WHERE clause that use only terms in that loop and outer
** loops are evaluated and if false a jump is made around all subsequent
** inner loops (or around the "..." if the test occurs within the inner-
** most loop)
**
** OUTER JOINS
**
** An outer join of tables t1 and t2 is conceptually coded as follows:
**
**    foreach row1 in t1 do
**      flag = 0
**      foreach row2 in t2 do
**        start:
**          ...
**          flag = 1
**      end
**      if flag==0 then
**        move the row2 cursor to a null row
**        goto start
**      fi
**    end
**
** ORDER BY CLAUSE PROCESSING
**
** order_by is a pointer to the ORDER BY clause (or the GROUP BY clause
** if the WHERE_GROUPBY flag is set in wctrl_flags) of a SELECT statement
** if there is one.  If there is no ORDER BY clause or if this routine
** is called from an UPDATE or DELETE statement, then order_by is NULL.
**
** The i_idx_cur parameter is the cursor number of an index.  If
** WHERE_OR_SUBCLAUSE is set, i_idx_cur is the cursor number of an index
** to use for OR clause processing.  The WHERE clause should use this
** specific cursor.  If WHERE_ONEPASS_DESIRED is set, then i_idx_cur is
** the first cursor in an array of cursors for all indices.  i_idx_cur should
** be used to compute the appropriate cursor depending on which index is
** used.
*/
where_info_t *__where_begin(
  parser_t *parser,          /* The parser context */
  src_list_t *tab_list,      /* FROM clause: A list of all tables to be scanned */
  expr_t *where,           /* The WHERE clause */
  expr_list_t *order_by,     /* An ORDER BY (or GROUP BY) clause, or NULL */
  expr_list_t *result_set,   /* Query result set.  Req'd for DISTINCT */
  select_t *p_select,        /* The entire SELECT statement */
  u_int16_t wctrl_flags,         /* The WHERE_* flags defined in dbsql_int.h */
  int i_aux_arg             /* If WHERE_OR_SUBCLAUSE is set, index cursor number
                          ** If WHERE_USE_LIMIT, then the limit amount */
){
  int n_byte_w_info;            /* Num. bytes allocated for where_info_t struct */
  int n_tab_list;              /* Number of elements in tab_list */
  where_info_t *w_info;         /* Will become the return value of this function */
  vdbe_t *v = parser->vdbe;   /* The virtual database engine */
  Bitmask not_ready;          /* Cursors that are not yet positioned */
  where_loop_builder_t s_wlb;     /* The where_loop_t builder */
  where_mask_set_t *mask_set;    /* The expression mask set */
  where_level_t *level;        /* A single level in w_info->a[] */
  where_loop_t *loop;          /* Pointer to a single where_loop_t object */
  int ii;                    /* Loop counter */
  DBSQL *db;               /* Database connection */
  int rc;                    /* Return code */
  u_int8_t fordelete = 0;         /* OPFLAG_FORDELETE or zero, as appropriate */

  assert( (wctrl_flags & WHERE_ONEPASS_MULTIROW)==0 || (
        (wctrl_flags & WHERE_ONEPASS_DESIRED)!=0
     && (wctrl_flags & WHERE_OR_SUBCLAUSE)==0
  ));

  /* Only one of WHERE_OR_SUBCLAUSE or WHERE_USE_LIMIT */
  assert( (wctrl_flags & WHERE_OR_SUBCLAUSE)==0
            || (wctrl_flags & WHERE_USE_LIMIT)==0 );

  /* Variable initialization */
  db = parser->db;
  memset(&s_wlb, 0, sizeof(s_wlb));

  /* An ORDER/GROUP BY clause of more than 63 terms cannot be optimized */
  testcase( order_by && order_by->n_expr==BMS-1 );
  if( order_by && order_by->n_expr>=BMS ){
    order_by = 0;
    wctrl_flags &= ~WHERE_WANT_DISTINCT;
    wctrl_flags |= WHERE_KEEP_ALL_JOINS; /* Disable omit-noop-join opt */
  }

  /* The number of tables in the FROM clause is limited by the number of
  ** bits in a Bitmask
  */
  testcase( tab_list->n_src==BMS );
  if( tab_list->n_src>BMS ){
    __error_msg(parser, "at most %d tables in a join", BMS);
    return 0;
  }

  /* This function normally generates a nested loop for all tables in
  ** tab_list.  But if the WHERE_OR_SUBCLAUSE flag is set, then we should
  ** only generate code for the first table in tab_list and assume that
  ** any cursors associated with subsequent tables are uninitialized.
  */
  n_tab_list = (wctrl_flags & WHERE_OR_SUBCLAUSE) ? 1 : tab_list->n_src;

  /* Allocate and initialize the where_info_t structure that will become the
  ** return value. A single allocation is used to store the where_info_t
  ** struct, the contents of where_info_t.a[], the where_clause_t structure
  ** and the where_mask_set_t structure. Since where_clause_t contains an 8-byte
  ** field (type Bitmask) it must be aligned on an 8-byte boundary on
  ** some architectures. Hence the ROUND8() below.
  */
  n_byte_w_info = SZ_WHEREINFO(n_tab_list);
  w_info = __db_malloc_raw_nn(db, n_byte_w_info + sizeof(where_loop_t));
  if( db->malloc_failed ){
    __dbsql_db_free(db, w_info);
    w_info = 0;
    goto where_begin_error;
  }
  w_info->parser = parser;
  w_info->tab_list = tab_list;
  w_info->order_by = order_by;
#if WHERETRACE_ENABLED
  w_info->where = where;
#endif
  w_info->result_set = result_set;
  w_info->cur_one_passes[0] = w_info->cur_one_passes[1] = -1;
  w_info->n_level = n_tab_list;
  w_info->i_break = w_info->i_continue = __vdbe_make_label(parser);
  w_info->wctrl_flags = wctrl_flags;
  w_info->i_limit = i_aux_arg;
  w_info->saved_n_query_loop = parser->n_query_loop;
  w_info->p_select = p_select;
  memset(&w_info->n_ob_sat, 0,
         offsetof(where_info_t,s_wc) - offsetof(where_info_t,n_ob_sat));
  memset(&w_info->a[0], 0, sizeof(where_loop_t)+n_tab_list*sizeof(where_level_t));
  assert( w_info->one_pass==ONEPASS_OFF );  /* ONEPASS defaults to OFF */
  mask_set = &w_info->s_mask_set;
  mask_set->n = 0;
  mask_set->ix[0] = -99; /* Initialize ix[0] to a value that can never be
                         ** a valid cursor number, to avoid an initial
                         ** test for mask_set->n==0 in __where_get_mask() */
  s_wlb.w_info = w_info;
  s_wlb.wc = &w_info->s_wc;
  s_wlb.p_new = (where_loop_t*)(((char*)w_info)+n_byte_w_info);
  assert( EIGHT_BYTE_ALIGNMENT(s_wlb.p_new) );
  __where_loop_init(s_wlb.p_new);
#ifdef DBSQL_DEBUG
  s_wlb.p_new->c_id = '*';
#endif

  /* Split the WHERE clause into separate subexpressions where each
  ** subexpression is separated by an AND operator.
  */
  __where_clause_init(&w_info->s_wc, w_info);
  __where_split(&w_info->s_wc, where, TK_AND);
   
  /* Special case: No FROM clause
  */
  if( n_tab_list==0 ){
    if( order_by ) w_info->n_ob_sat = order_by->n_expr;
    if( (wctrl_flags & WHERE_WANT_DISTINCT)!=0
     && OPTIMIZATION_ENABLED(db, DBSQL_DistinctOpt)
    ){
      w_info->distinct = WHERE_DISTINCT_UNIQUE;
    }
    if( ALWAYS(w_info->p_select)
     && (w_info->p_select->sel_flags & SF_MultiValue)==0
    ){
      EXPLAIN_QUERY_PLAN((parser, 0, "SCAN CONSTANT ROW"));
    }
  }else{
    /* Assign a bit from the bitmask to every term in the FROM clause.
    **
    ** The N-th term of the FROM clause is assigned a bitmask of 1<<N.
    **
    ** The rule of the previous sentence ensures that if X is the bitmask for
    ** a table T, then X-1 is the bitmask for all other tables to the left of T.
    ** Knowing the bitmask for all tables to the left of a left join is
    ** important.  Ticket #3015.
    **
    ** Note that bitmasks are created for all tab_list->n_src tables in
    ** tab_list, not just the first n_tab_list tables.  n_tab_list is normally
    ** equal to tab_list->n_src but might be shortened to 1 if the
    ** WHERE_OR_SUBCLAUSE flag is set.
    */
    ii = 0;
    do{
      __create_mask(mask_set, tab_list->a[ii].i_cursor);
      __where_tab_func_args(parser, &tab_list->a[ii], &w_info->s_wc);
    }while( (++ii)<tab_list->n_src );
  #ifdef DBSQL_DEBUG
    {
      Bitmask mx = 0;
      for(ii=0; ii<tab_list->n_src; ii++){
        Bitmask m = __where_get_mask(mask_set, tab_list->a[ii].i_cursor);
        assert( m>=mx );
        mx = m;
      }
    }
  #endif
  }
 
  /* Analyze all of the subexpressions. */
  __where_expr_analyze(tab_list, &w_info->s_wc);
  if( p_select && p_select->limit ){
    __where_add_limit(&w_info->s_wc, p_select);
  }
  if( parser->n_err ) goto where_begin_error;

  /* The False-WHERE-Term-Bypass optimization:
  **
  ** If there are WHERE terms that are false, then no rows will be output,
  ** so skip over all of the code generated here.
  **
  ** Conditions:
  **
  **   (1)  The WHERE term must not refer to any tables in the join.
  **   (2)  The term must not come from an ON clause on the
  **        right-hand side of a LEFT or FULL JOIN.
  **   (3)  The term must not come from an ON clause, or there must be
  **        no RIGHT or FULL OUTER joins in tab_list.
  **   (4)  If the expression contains non-deterministic functions
  **        that are not within a sub-select. This is not required
  **        for correctness but rather to preserves dbsql's legacy
  **        behaviour in the following two cases:
  **
  **          WHERE random()>0;           -- eval random() once per row
  **          WHERE (SELECT random())>0;  -- eval random() just once overall
  **
  ** Note that the Where term need not be a constant in order for this
  ** optimization to apply, though it does need to be constant relative to
  ** the current subquery (condition 1).  The term might include variables
  ** from outer queries so that the value of the term changes from one
  ** invocation of the current subquery to the next.
  */
  for(ii=0; ii<s_wlb.wc->n_base; ii++){
    where_term_t *t = &s_wlb.wc->a[ii];  /* A term of the WHERE clause */
    expr_t *x;                          /* The expression of t */
    if( t->wt_flags & TERM_VIRTUAL ) continue;
    x = t->expr;
    assert( x!=0 );
    assert( t->prereq_all!=0 || !EXPR_HAS_PROPERTY(x, EP_OuterON) );
    if( t->prereq_all==0                           /* Conditions (1) and (2) */
     && (n_tab_list==0 || __expr_is_deterministic(x))   /* Condition (4) */
     && !(EXPR_HAS_PROPERTY(x, EP_InnerON)          /* Condition (3) */
          && (tab_list->a[0].fg.jointype & JT_LTORJ)!=0 )
    ){
      __expr_if_false(parser, x, w_info->i_break, DBSQL_JUMPIFNULL);
      t->wt_flags |= TERM_CODED;
    }
  }

  if( wctrl_flags & WHERE_WANT_DISTINCT ){
    if( OPTIMIZATION_DISABLED(db, DBSQL_DistinctOpt) ){
      /* Disable the DISTINCT optimization if DBSQL_DistinctOpt is set via
      ** dbsql_test_ctrl(DBSQL_TESTCTRL_OPTIMIZATIONS,...) */
      wctrl_flags &= ~WHERE_WANT_DISTINCT;
      w_info->wctrl_flags &= ~WHERE_WANT_DISTINCT;
    }else if( __is_distinct_redundant(parser, tab_list, &w_info->s_wc, result_set) ){
      /* The DISTINCT marking is pointless.  Ignore it. */
      w_info->distinct = WHERE_DISTINCT_UNIQUE;
    }else if( order_by==0 ){
      /* Try to ORDER BY the result set to make distinct processing easier */
      w_info->wctrl_flags |= WHERE_DISTINCTBY;
      w_info->order_by = result_set;
    }
  }

  /* Construct the where_loop_t objects */
#if defined(WHERETRACE_ENABLED)
  if( __where_trace & 0xffffffff ){
    __debug_printf("*** Optimizer Start *** (wctrlFlags: 0x%x",wctrl_flags);
    if( wctrl_flags & WHERE_USE_LIMIT ){
      __debug_printf(", limit: %d", i_aux_arg);
    }
    __debug_printf(")\n");
    if( __where_trace & 0x8000 ){
      select_t s_select;
      memset(&s_select, 0, sizeof(s_select));
      s_select.sel_flags = SF_WhereBegin;
      s_select.src = tab_list;
      s_select.where = where;
      s_select.order_by = order_by;
      s_select.e_list = result_set;
      __tree_view_select(0, &s_select, 0);
    }
    if( __where_trace & 0x4000 ){ /* Display all WHERE clause terms */
      __debug_printf("---- WHERE clause at start of analysis:\n");
      __where_clause_print(s_wlb.wc);
    }
  }
#endif

  if( n_tab_list!=1 || __where_short_cut(&s_wlb)==0 ){
    rc = __where_loop_add_all(&s_wlb);
    if( rc ) goto where_begin_error;

#ifdef DBSQL_ENABLE_STAT4
    /* If one or more where_term_t.truth_prob values were used in estimating
    ** loop parameters, but then those truth_prob values were subsequently
    ** changed based on STAT4 information while computing subsequent loops,
    ** then we need to rerun the whole loop building process so that all
    ** loops will be built using the revised truth_prob values. */
    if( s_wlb.bld_flags2 & DBSQL_BLDF2_2NDPASS ){
      WHERETRACE_ALL_LOOPS(w_info, s_wlb.wc);
      WHERETRACE(0xffffffff,
           ("**** Redo all loop computations due to"
            " TERM_HIGHTRUTH changes ****\n"));
      while( w_info->loops ){
        where_loop_t *p = w_info->loops;
        w_info->loops = p->next_loop;
        __where_loop_delete(db, p);
      }
      rc = __where_loop_add_all(&s_wlb);
      if( rc ) goto where_begin_error;
    }
#endif
    WHERETRACE_ALL_LOOPS(w_info, s_wlb.wc);
 
    __where_path_solver(w_info, 0);
    if( db->malloc_failed ) goto where_begin_error;
    if( w_info->order_by ){
       __where_interstage_heuristic(w_info);
       __where_path_solver(w_info, w_info->n_row_out<0 ? 1 : w_info->n_row_out+1);
       if( db->malloc_failed ) goto where_begin_error;
    }

    /* TUNING:  Assume that a DISTINCT clause on a subquery reduces
    ** the output size by a factor of 8 (log_est_t -30).  Search for
    ** tag-20250414a to see other cases.
    */
    if( (w_info->wctrl_flags & WHERE_WANT_DISTINCT)!=0 ){
      WHERETRACE(0x0080,("nRowOut reduced from %d to %d due to DISTINCT\n",
                         w_info->n_row_out, w_info->n_row_out-30));
      w_info->n_row_out -= 30;
    }

  }
  assert( w_info->tab_list!=0 );
  if( w_info->order_by==0 && (db->flags & DBSQL_ReverseOrder)!=0 ){
    __where_reverse_scan_order(w_info);
  }
  if( parser->n_err ){
    goto where_begin_error;
  }
  assert( db->malloc_failed==0 );
#ifdef WHERETRACE_ENABLED
  if( __where_trace ){
    __debug_printf("---- Solution cost=%d, nRow=%d",
                       w_info->r_total_cost, w_info->n_row_out);
    if( w_info->n_ob_sat>0 ){
      __debug_printf(" ORDERBY=%d,0x%llx", w_info->n_ob_sat, w_info->rev_mask);
    }
    switch( w_info->distinct ){
      case WHERE_DISTINCT_UNIQUE: {
        __debug_printf("  DISTINCT=unique");
        break;
      }
      case WHERE_DISTINCT_ORDERED: {
        __debug_printf("  DISTINCT=ordered");
        break;
      }
      case WHERE_DISTINCT_UNORDERED: {
        __debug_printf("  DISTINCT=unordered");
        break;
      }
    }
    __debug_printf("\n");
    for(ii=0; ii<w_info->n_level; ii++){
      __where_loop_print(w_info->a[ii].w_loop, s_wlb.wc);
    }
  }
#endif

  /* Attempt to omit tables from a join that do not affect the result.
  ** See the comment on __where_omit_noop_join() for further information.
  **
  ** This query optimization is factored out into a separate "no-inline"
  ** procedure to keep the __where_begin() procedure from becoming
  ** too large.  If __where_begin() becomes too large, that prevents
  ** some C-compiler optimizers from in-lining the
  ** __where_code_one_loop_start() procedure, and it is important to
  ** in-line __where_code_one_loop_start() for performance reasons.
  */
  not_ready = ~(Bitmask)0;
  if( w_info->n_level>=2       /* Must be a join, or this opt8n is pointless */
   && result_set!=0           /* Condition (1) */
   && 0==(wctrl_flags & (WHERE_AGG_DISTINCT|WHERE_KEEP_ALL_JOINS)) /* (1),(6) */
   && OPTIMIZATION_ENABLED(db, DBSQL_OmitNoopJoin)                /* (7) */
  ){
    not_ready = __where_omit_noop_join(w_info, not_ready);
    n_tab_list = w_info->n_level;
    assert( n_tab_list>0 );
  }

  /* Check to see if there are any SEARCH loops that might benefit from
  ** using a Bloom filter.
  */
  if( w_info->n_level>=2
   && OPTIMIZATION_ENABLED(db, DBSQL_BloomFilter)
  ){
    __where_check_if_bloom_filter_is_useful(w_info);
  }

#if defined(WHERETRACE_ENABLED)
  if( __where_trace & 0x4000 ){ /* Display all terms of the WHERE clause */
    __debug_printf("---- WHERE clause at end of analysis:\n");
    __where_clause_print(s_wlb.wc);
  }
  WHERETRACE(0xffffffff,("*** Optimizer Finished ***\n"));
#endif
  w_info->parser->n_query_loop += w_info->n_row_out;

  /* If the caller is an UPDATE or DELETE statement that is requesting
  ** to use a one-pass algorithm, determine if this is appropriate.
  **
  ** A one-pass approach can be used if the caller has requested one
  ** and either (a) the scan visits at most one row or (b) each
  ** of the following are true:
  **
  **   * the caller has indicated that a one-pass approach can be used
  **     with multiple rows (by setting WHERE_ONEPASS_MULTIROW), and
  **   * the table is not a virtual table, and
  **   * either the scan does not use the OR optimization or the caller
  **     is a DELETE operation (WHERE_DUPLICATES_OK is only specified
  **     for DELETE).
  **
  ** The last qualification is because an UPDATE statement uses
  ** where_info_t.cur_one_passes[1] to determine whether or not it really can
  ** use a one-pass approach, and this is not set accurately for scans
  ** that use the OR optimization.
  */
  assert( (wctrl_flags & WHERE_ONEPASS_DESIRED)==0 || w_info->n_level==1 );
  if( (wctrl_flags & WHERE_ONEPASS_DESIRED)!=0 ){
    int ws_flags = w_info->a[0].w_loop->ws_flags;
    int onerow = (ws_flags & WHERE_ONEROW)!=0;
    assert( !(ws_flags&WHERE_VIRTUALTABLE) || IS_VIRTUAL(tab_list->a[0].s_tab) );
    if( onerow || (
        0!=(wctrl_flags & WHERE_ONEPASS_MULTIROW)
     && !IS_VIRTUAL(tab_list->a[0].s_tab)
     && (0==(ws_flags & WHERE_MULTI_OR) || (wctrl_flags & WHERE_DUPLICATES_OK))
     && OPTIMIZATION_ENABLED(db, DBSQL_OnePass)
    )){
      w_info->one_pass = onerow ? ONEPASS_SINGLE : ONEPASS_MULTI;
      if( HAS_ROWID(tab_list->a[0].s_tab) && (ws_flags & WHERE_IDX_ONLY) ){
        if( wctrl_flags & WHERE_ONEPASS_MULTIROW ){
          fordelete = OPFLAG_FORDELETE;
        }
        w_info->a[0].w_loop->ws_flags = (ws_flags & ~WHERE_IDX_ONLY);
      }
    }
  }

  /* Open all tables in the tab_list and any indices selected for
  ** searching those tables.
  */
  for(ii=0, level=w_info->a; ii<n_tab_list; ii++, level++){
    table_t *tab;     /* table_t to open */
    int i_db;         /* index_t of database containing table/index */
    src_item_t *tab_item;

    tab_item = &tab_list->a[level->i_from];
    tab = tab_item->s_tab;
    i_db = __schema_to_index(db, tab->schema);
    loop = level->w_loop;
    level->addr_brk = __vdbe_make_label(parser);
    if( ii==0 || (tab_item[0].fg.jointype & JT_LEFT)!=0 ){
      level->addr_halt = level->addr_brk;
    }else if( w_info->a[ii-1].rj ){
      level->addr_halt = w_info->a[ii-1].addr_brk;
    }else{
      level->addr_halt = w_info->a[ii-1].addr_halt;
    }
    if( (tab->tab_flags & TF_Ephemeral)!=0 || IsView(tab) ){
      /* Do nothing */
    }else
#ifndef DBSQL_OMIT_VIRTUALTABLE
    if( (loop->ws_flags & WHERE_VIRTUALTABLE)!=0 ){
      const char *v_tab = (const char *)__get_v_table(db, tab);
      int i_cur = tab_item->i_cursor;
      __vdbe_add_op4(v, OP_VOpen, i_cur, 0, 0, v_tab, P4_VTAB);
    }else if( IS_VIRTUAL(tab) ){
      /* noop */
    }else
#endif
    if( ((loop->ws_flags & WHERE_IDX_ONLY)==0
         && (wctrl_flags & WHERE_OR_SUBCLAUSE)==0)
     || (tab_item->fg.jointype & (JT_LTORJ|JT_RIGHT))!=0
    ){
      int op = OP_OpenRead;
      if( w_info->one_pass!=ONEPASS_OFF ){
        op = OP_OpenWrite;
        w_info->cur_one_passes[0] = tab_item->i_cursor;
      };
      __open_table(parser, tab_item->i_cursor, i_db, tab, op);
      assert( tab_item->i_cursor==level->i_tab_cur );
      testcase( w_info->one_pass==ONEPASS_OFF && tab->n_col==BMS-1 );
      testcase( w_info->one_pass==ONEPASS_OFF && tab->n_col==BMS );
      if( w_info->one_pass==ONEPASS_OFF
       && tab->n_col<BMS
       && (tab->tab_flags & (TF_HasGenerated|TF_WithoutRowid))==0
       && (loop->ws_flags & (WHERE_AUTO_INDEX|WHERE_BLOOMFILTER))==0
      ){
        /* If we know that only a prefix of the record will be used,
        ** it is advantageous to reduce the "column count" field in
        ** the P4 operand of the OP_OpenRead/Write opcode. */
        Bitmask b = tab_item->col_used;
        int n = 0;
        for(; b; b=b>>1, n++){}
        __vdbe_change_p4(v, -1, DBSQL_INT_TO_PTR(n), P4_INT32);
        assert( n<=tab->n_col );
      }
#ifdef DBSQL_ENABLE_CURSOR_HINTS
      if( loop->u.btree.p_index!=0 && (tab->tab_flags & TF_WithoutRowid)==0 ){
        __vdbe_change_p5(v, OPFLAG_SEEKEQ|fordelete);
      }else
#endif
      {
        __vdbe_change_p5(v, fordelete);
      }
#ifdef DBSQL_ENABLE_COLUMN_USED_MASK
      __vdbe_add_op4_dup8(v, OP_ColumnsUsed, tab_item->i_cursor, 0, 0,
                            (const u_int8_t*)&tab_item->col_used, P4_INT64);
#endif
      if( ii>=2
       && (tab_item[0].fg.jointype & (JT_LTORJ|JT_LEFT))==0 
       && level->addr_halt==w_info->a[0].addr_halt
      ){
        __vdbe_add_op2(v, OP_IfEmpty, tab_item->i_cursor, w_info->i_break);
        VDBE_COVERAGE(v);
      }
    }else{
      __table_lock(parser, i_db, tab->tnum, 0, tab->name);
    }
    if( loop->ws_flags & WHERE_INDEXED ){
      index_t *ix = loop->u.btree.p_index;
      int i_index_cur;
      int op = OP_OpenRead;
      /* i_aux_arg is always set to a positive value if ONEPASS is possible */
      assert( i_aux_arg!=0 || (w_info->wctrl_flags & WHERE_ONEPASS_DESIRED)==0 );
      if( !HAS_ROWID(tab) && IS_PRIMARY_KEY_INDEX(ix)
       && (wctrl_flags & WHERE_OR_SUBCLAUSE)!=0
      ){
        /* This is one term of an OR-optimization using the PRIMARY KEY of a
        ** WITHOUT ROWID table.  No need for a separate index */
        i_index_cur = level->i_tab_cur;
        op = 0;
      }else if( w_info->one_pass!=ONEPASS_OFF ){
        index_t *j = tab_item->s_tab->p_index;
        i_index_cur = i_aux_arg;
        assert( wctrl_flags & WHERE_ONEPASS_DESIRED );
        while( ALWAYS(j) && j!=ix ){
          i_index_cur++;
          j = j->next;
        }
        op = OP_OpenWrite;
        w_info->cur_one_passes[1] = i_index_cur;
      }else if( i_aux_arg && (wctrl_flags & WHERE_OR_SUBCLAUSE)!=0 ){
        i_index_cur = i_aux_arg;
        op = OP_ReopenIdx;
      }else{
        i_index_cur = parser->n_tab++;
        if( ix->has_expr && OPTIMIZATION_ENABLED(db, DBSQL_IndexedExpr) ){
          __where_add_indexed_expr(parser, ix, i_index_cur, tab_item);
        }
        if( ix->part_idx_where && (tab_item->fg.jointype & JT_RIGHT)==0 ){
          __where_part_idx_expr(
              parser, ix, ix->part_idx_where, 0, i_index_cur, tab_item
          );
        }
      }
      level->i_idx_cur = i_index_cur;
      assert( ix!=0 );
      assert( ix->schema==tab->schema );
      assert( i_index_cur>=0 );
      if( op ){
        __vdbe_add_op3(v, op, i_index_cur, ix->tnum, i_db);
        __vdbe_set_p4_key_info(parser, ix);
        if( (loop->ws_flags & WHERE_CONSTRAINT)!=0
         && (loop->ws_flags & (WHERE_COLUMN_RANGE|WHERE_SKIPSCAN))==0
         && (loop->ws_flags & WHERE_BIGNULL_SORT)==0
         && (loop->ws_flags & WHERE_IN_SEEKSCAN)==0
         && (w_info->wctrl_flags&WHERE_ORDERBY_MIN)==0
         && w_info->distinct!=WHERE_DISTINCT_ORDERED
        ){
          __vdbe_change_p5(v, OPFLAG_SEEKEQ);
        }
        VDBE_COMMENT((v, "%s", ix->name));
#ifdef DBSQL_ENABLE_COLUMN_USED_MASK
        {
          u_int64_t col_used = 0;
          int ii, jj;
          for(ii=0; ii<ix->n_column; ii++){
            jj = ix->columns[ii];
            if( jj<0 ) continue;
            if( jj>63 ) jj = 63;
            if( (tab_item->col_used & MASKBIT(jj))==0 ) continue;
            col_used |= ((u_int64_t)1)<<(ii<63 ? ii : 63);
          }
          __vdbe_add_op4_dup8(v, OP_ColumnsUsed, i_index_cur, 0, 0,
                                (u_int8_t*)&col_used, P4_INT64);
        }
#endif /* DBSQL_ENABLE_COLUMN_USED_MASK */
      }
    }
    if( i_db>=0 ) __code_verify_schema(parser, i_db);
    if( (tab_item->fg.jointype & JT_RIGHT)!=0
     && (level->rj = __where_malloc(w_info, sizeof(where_right_join_t)))!=0
    ){
      where_right_join_t *rj = level->rj;
      rj->i_match = parser->n_tab++;
      rj->reg_bloom = ++parser->n_mem;
      __vdbe_add_op2(v, OP_Blob, 65536, rj->reg_bloom);
      rj->reg_return = ++parser->n_mem;
      __vdbe_add_op2(v, OP_Null, 0, rj->reg_return);
      assert( tab==tab_item->s_tab );
      if( HAS_ROWID(tab) ){
        key_info_t *info;
        __vdbe_add_op2(v, OP_OpenEphemeral, rj->i_match, 1);
        info = __key_info_alloc(parser->db, 1, 0);
        if( info ){
          info->colls[0] = 0;
          info->sort_flags[0] = 0;
          __vdbe_append_p4(v, info, P4_KEYINFO);
        }
      }else{
        index_t *pk = __primary_key_index(tab);
        __vdbe_add_op2(v, OP_OpenEphemeral, rj->i_match, pk->n_key_col);
        __vdbe_set_p4_key_info(parser, pk);
      }
      loop->ws_flags &= ~WHERE_IDX_ONLY;
      /* The nature of RIGHT JOIN processing is such that it messes up
      ** the output order.  So omit any ORDER BY/GROUP BY elimination
      ** optimizations.  We need to do an actual sort for RIGHT JOIN. */
      w_info->n_ob_sat = 0;
      w_info->distinct = WHERE_DISTINCT_UNORDERED;
    }
  }
  w_info->i_top = __vdbe_current_addr(v);
  if( db->malloc_failed ) goto where_begin_error;

  /* Generate the code to do the search.  Each iteration of the for
  ** loop below generates code for a single nested loop of the VM
  ** program.
  */
  for(ii=0; ii<n_tab_list; ii++){
    int addr_explain;
    int ws_flags;
    src_item_t *src;
    if( parser->n_err ) goto where_begin_error;
    level = &w_info->a[ii];
    ws_flags = level->w_loop->ws_flags;
    src = &tab_list->a[level->i_from];
    if( src->fg.is_materialized ){
      subquery_t *subq;
      int i_once = 0;
      assert( src->fg.is_subquery );
      subq = src->u4.subq;
      if( src->fg.is_correlated==0 ){
        i_once = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
      }else{
        i_once = 0;
      }
      __vdbe_add_op2(v, OP_Gosub, subq->reg_return, subq->addr_fill_sub);
      VDBE_COMMENT((v, "materialize %!S", src));
      if( i_once )  __vdbe_jump_here(v, i_once);
    }
    assert( tab_list == w_info->tab_list );
    if( (ws_flags & (WHERE_AUTO_INDEX|WHERE_BLOOMFILTER))!=0 ){
      if( (ws_flags & WHERE_AUTO_INDEX)!=0 ){
#ifndef DBSQL_OMIT_AUTOMATIC_INDEX
        __construct_automatic_index(parser, &w_info->s_wc, not_ready, level);
#endif
      }else{
        __construct_bloom_filter(w_info, ii, level, not_ready);
      }
      if( db->malloc_failed ) goto where_begin_error;
    }
    addr_explain = __where_explain_one_scan(
        parser, tab_list, level, wctrl_flags
    );
    level->addr_body = __vdbe_current_addr(v);
    not_ready = __where_code_one_loop_start(parser,v,w_info,ii,level,not_ready);
    w_info->i_continue = level->addr_cont;
    if( (ws_flags&WHERE_MULTI_OR)==0 && (wctrl_flags&WHERE_OR_SUBCLAUSE)==0 ){
      __where_add_scan_status(v, tab_list, level, addr_explain);
    }
  }

  /* Done. */
  VDBE_MODULE_COMMENT((v, "Begin WHERE-core"));
  w_info->i_end_where = __vdbe_current_addr(v);
  return w_info;

  /* Jump here if malloc fails */
where_begin_error:
  if( w_info ){
    parser->n_query_loop = w_info->saved_n_query_loop;
    __where_info_free(db, w_info);
  }
#ifdef WHERETRACE_ENABLED
  /* Prevent harmless compiler warnings about debugging routines
  ** being declared but never used */
  __show_where_loop_list(0);
#endif /* WHERETRACE_ENABLED */
  return 0;
}

/*
** Part of __where_end() will rewrite opcodes to reference the
** index rather than the main table.  In DBSQL_DEBUG mode, we want
** to trace those changes if PRAGMA vdbe_addoptrace=on.  This routine
** does that.
*/
#ifndef DBSQL_DEBUG
# define OPCODE_REWRITE_TRACE(D,K,P) /* no-op */
#else
# define OPCODE_REWRITE_TRACE(D,K,P) __where_opcode_rewrite_trace(D,K,P)
  static void __where_opcode_rewrite_trace(
    DBSQL *db,
    int pc,
    vdbe_op_t *op
  ){
    if( (db->flags & DBSQL_VdbeAddopTrace)==0 ) return;
    __vdbe_print_op(0, pc, op);
    __show_where_term(0); /* So compiler won't complain about unused func */
  }
#endif

/*
** Generate the end of the WHERE loop.  See comments on
** __where_begin() for additional information.
*/
void __where_end(where_info_t *w_info){
  parser_t *parser = w_info->parser;
  vdbe_t *v = parser->vdbe;
  int i;
  where_level_t *level;
  where_loop_t *loop;
  src_list_t *tab_list = w_info->tab_list;
  DBSQL *db = parser->db;
  int i_end = __vdbe_current_addr(v);
  int n_rj = 0;
#ifndef DBSQL_DISABLE_SKIPAHEAD_DISTINCT
  int addr_seek = 0;
#endif

  /* Generate loop termination code.
  */
  VDBE_MODULE_COMMENT((v, "End WHERE-core"));
  for(i=w_info->n_level-1; i>=0; i--){
    int addr;
    level = &w_info->a[i];
    if( level->rj ){
      /* Terminate the subroutine that forms the interior of the loop of
      ** the RIGHT JOIN table */
      where_right_join_t *rj = level->rj;
      __vdbe_resolve_label(v, level->addr_cont);
      /* Replace addr_cont with a new label that will never be used, just so
      ** the subsequent call to resolve level->addr_cont will have something
      ** to resolve. */
      level->addr_cont = __vdbe_make_label(parser);
      rj->end_subrtn = __vdbe_current_addr(v);
      __vdbe_add_op3(v, OP_Return, rj->reg_return, rj->addr_subrtn, 1);
      VDBE_COVERAGE(v);
      n_rj++;
    }
    loop = level->w_loop;
    if( level->op!=OP_Noop ){
#ifndef DBSQL_DISABLE_SKIPAHEAD_DISTINCT
      index_t *idx;
      int n;
      if( w_info->distinct==WHERE_DISTINCT_ORDERED
       && i==w_info->n_level-1  /* Ticket [ef9318757b152e3] 2017-10-21 */
       && (loop->ws_flags & WHERE_INDEXED)!=0
       && (idx = loop->u.btree.p_index)->has_stat1
       && (n = loop->u.btree.n_distinct_col)>0
       && idx->row_log_ests[n]>=36
      ){
        int r1 = parser->n_mem+1;
        int j, op;
        int addr_if_null = 0; /* Init to avoid false-positive compiler warning */
        if( level->i_left_join ){
          addr_if_null = __vdbe_add_op2(v, OP_IfNullRow, level->i_idx_cur, r1);
        }
        for(j=0; j<n; j++){
          __vdbe_add_op3(v, OP_Column, level->i_idx_cur, j, r1+j);
        }
        parser->n_mem += n+1;
        op = level->op==OP_Prev ? OP_SeekLT : OP_SeekGT;
        addr_seek = __vdbe_add_op4_int(v, op, level->i_idx_cur, 0, r1, n);
        VDBE_COVERAGE_IF(v, op==OP_SeekLT);
        VDBE_COVERAGE_IF(v, op==OP_SeekGT);
        __vdbe_add_op2(v, OP_Goto, 1, level->p2);
        if( level->i_left_join ){
          __vdbe_jump_here(v, addr_if_null);
        }
      }
#endif /* DBSQL_DISABLE_SKIPAHEAD_DISTINCT */
    }
    if( tab_list->a[level->i_from].fg.from_exists ){
      /* This is an EXISTS-to-JOIN optimization loop. If this loop sees a 
      ** successful row, it should break out of itself. */
      __vdbe_add_op2(v, OP_Goto, 0, level->addr_brk);
      VDBE_COMMENT((v, "EXISTS break %d", i));
    }
    __vdbe_resolve_label(v, level->addr_cont);
    if( level->op!=OP_Noop ){
      __vdbe_add_op3(v, level->op, level->p1, level->p2, level->p3);
      __vdbe_change_p5(v, level->p5);
      VDBE_COVERAGE(v);
      VDBE_COVERAGE_IF(v, level->op==OP_Next);
      VDBE_COVERAGE_IF(v, level->op==OP_Prev);
      VDBE_COVERAGE_IF(v, level->op==OP_VNext);
      if( level->reg_bignull ){
        __vdbe_resolve_label(v, level->addr_bignull);
        __vdbe_add_op2(v, OP_DecrJumpZero, level->reg_bignull, level->p2-1);
        VDBE_COVERAGE(v);
      }
#ifndef DBSQL_DISABLE_SKIPAHEAD_DISTINCT
      if( addr_seek ){
        __vdbe_jump_here(v, addr_seek);
        addr_seek = 0;
      }
#endif
    }
    if( (loop->ws_flags & WHERE_IN_ABLE)!=0 && level->u.in.n_in>0 ){
      struct in_loop_t *in;
      int j;
      __vdbe_resolve_label(v, level->addr_nxt);
      for(j=level->u.in.n_in, in=&level->u.in.in_loops[j-1]; j>0; j--, in--){
        assert( __vdbe_get_op(v, in->addr_in_top+1)->opcode==OP_IsNull
                 || parser->db->malloc_failed );
        __vdbe_jump_here(v, in->addr_in_top+1);
        if( in->end_loop_op!=OP_Noop ){
          if( in->n_prefix ){
            int early_out =
                (loop->ws_flags & WHERE_VIRTUALTABLE)==0
                 && (loop->ws_flags & WHERE_IN_EARLYOUT)!=0;
            if( level->i_left_join ){
              /* For LEFT JOIN queries, cursor in->i_cur may not have been
              ** opened yet. This occurs for WHERE clauses such as
              ** "a = ? AND b IN (...)", where the index is on (a, b). If
              ** the RHS of the (a=?) is NULL, then the "b IN (...)" may
              ** never have been coded, but the body of the loop run to
              ** return the null-row. So, if the cursor is not open yet,
              ** jump over the OP_Next or OP_Prev instruction about to
              ** be coded.  */
              __vdbe_add_op2(v, OP_IfNotOpen, in->i_cur,
                  __vdbe_current_addr(v) + 2 + early_out);
              VDBE_COVERAGE(v);
            }
            if( early_out ){
              __vdbe_add_op4_int(v, OP_IfNoHope, level->i_idx_cur,
                  __vdbe_current_addr(v)+2,
                  in->i_base, in->n_prefix);
              VDBE_COVERAGE(v);
              /* Retarget the OP_IsNull against the left operand of IN so
              ** it jumps past the OP_IfNoHope.  This is because the
              ** OP_IsNull also bypasses the OP_Affinity opcode that is
              ** required by OP_IfNoHope. */
              __vdbe_jump_here(v, in->addr_in_top+1);
            }
          }
          __vdbe_add_op2(v, in->end_loop_op, in->i_cur, in->addr_in_top);
          VDBE_COVERAGE(v);
          VDBE_COVERAGE_IF(v, in->end_loop_op==OP_Prev);
          VDBE_COVERAGE_IF(v, in->end_loop_op==OP_Next);
        }
        __vdbe_jump_here(v, in->addr_in_top-1);
      }
    }
    __vdbe_resolve_label(v, level->addr_brk);
    if( level->rj ){
      __vdbe_add_op3(v, OP_Return, level->rj->reg_return, 0, 1);
      VDBE_COVERAGE(v);
    }
    if( level->addr_skip ){
      __vdbe_goto(v, level->addr_skip);
      VDBE_COMMENT((v, "next skip-scan on %s", loop->u.btree.p_index->name));
      __vdbe_jump_here(v, level->addr_skip);
      __vdbe_jump_here(v, level->addr_skip-2);
    }
#ifndef DBSQL_LIKE_DOESNT_MATCH_BLOBS
    if( level->addr_like_rep ){
      __vdbe_add_op2(v, OP_DecrJumpZero, (int)(level->i_like_rep_cntr>>1),
                        level->addr_like_rep);
      VDBE_COVERAGE(v);
    }
#endif
    if( level->i_left_join ){
      int ws = loop->ws_flags;
      addr = __vdbe_add_op1(v, OP_IfPos, level->i_left_join); VDBE_COVERAGE(v);
      assert( (ws & WHERE_IDX_ONLY)==0 || (ws & WHERE_INDEXED)!=0 );
      if( (ws & WHERE_IDX_ONLY)==0 ){
        src_item_t *src = &tab_list->a[level->i_from];
        assert( level->i_tab_cur==src->i_cursor );
        if( src->fg.via_coroutine ){
          int m, n;
          assert( src->fg.is_subquery );
          n = src->u4.subq->reg_result;
          assert( src->s_tab!=0 );
          m = src->s_tab->n_col;
          __vdbe_add_op3(v, OP_Null, 0, n, n+m-1);
        }
        __vdbe_add_op1(v, OP_NullRow, level->i_tab_cur);
      }
      if( (ws & WHERE_INDEXED)
       || ((ws & WHERE_MULTI_OR) && level->u.covering_idx)
      ){
        if( ws & WHERE_MULTI_OR ){
          index_t *ix = level->u.covering_idx;
          int i_db = __schema_to_index(db, ix->schema);
          __vdbe_add_op3(v, OP_ReopenIdx, level->i_idx_cur, ix->tnum, i_db);
          __vdbe_set_p4_key_info(parser, ix);
        }
        __vdbe_add_op1(v, OP_NullRow, level->i_idx_cur);
      }
      if( level->op==OP_Return ){
        __vdbe_add_op2(v, OP_Gosub, level->p1, level->addr_first);
      }else{
        __vdbe_goto(v, level->addr_first);
      }
      __vdbe_jump_here(v, addr);
    }
    VDBE_MODULE_COMMENT((v, "End WHERE-loop%d: %s", i,
                     w_info->tab_list->a[level->i_from].s_tab->name));
  }

  assert( w_info->n_level<=tab_list->n_src );
  for(i=0, level=w_info->a; i<w_info->n_level; i++, level++){
    int k, last;
    vdbe_op_t *p_op, *last_op;
    index_t *idx = 0;
    src_item_t *tab_item = &tab_list->a[level->i_from];
    table_t *tab = tab_item->s_tab;
    assert( tab!=0 );
    loop = level->w_loop;

    /* Do RIGHT JOIN processing.  Generate code that will output the
    ** unmatched rows of the right operand of the RIGHT JOIN with
    ** all of the columns of the left operand set to NULL.
    */
    if( level->rj ){
      __where_right_join_loop(w_info, i, level);
      continue;
    }

    /* For a co-routine, change all OP_Column references to the table of
    ** the co-routine into OP_Copy of result contained in a register.
    ** OP_Rowid becomes OP_Null.
    */
    if( tab_item->fg.via_coroutine ){
      testcase( parser->db->malloc_failed );
      assert( tab_item->fg.is_subquery );
      assert( tab_item->u4.subq->reg_result>=0 );
      __translate_column_to_copy(parser, level->addr_body, level->i_tab_cur,
                            tab_item->u4.subq->reg_result, 0);
      continue;
    }

    /* If this scan uses an index, make VDBE code substitutions to read data
    ** from the index instead of from the table where possible.  In some cases
    ** this optimization prevents the table from ever being read, which can
    ** yield a significant performance boost.
    **
    ** Calls to the code generator in between __where_begin and
    ** __where_end will have created code that references the table
    ** directly.  This loop scans all that code looking for opcodes
    ** that reference the table and converts them into opcodes that
    ** reference the index.
    */
    if( loop->ws_flags & (WHERE_INDEXED|WHERE_IDX_ONLY) ){
      idx = loop->u.btree.p_index;
    }else if( loop->ws_flags & WHERE_MULTI_OR ){
      idx = level->u.covering_idx;
    }
    if( idx
     && !db->malloc_failed
    ){
      if( w_info->one_pass==ONEPASS_OFF || !HAS_ROWID(idx->table) ){
        last = i_end;
      }else{
        last = w_info->i_end_where;
      }
      if( idx->has_expr ){
        indexed_expr_t *p = parser->idx_epr;
        while( p ){
          if( p->i_idx_cur==level->i_idx_cur ){
#ifdef WHERETRACE_ENABLED
            if( __where_trace & 0x200 ){
              __debug_printf("Disable pParse->pIdxEpr term {%d,%d}\n",
                                  p->i_idx_cur, p->i_idx_col);
              if( __where_trace & 0x5000 ) __show_expr(p->expr);
            }
#endif
            p->i_data_cur = -1;
            p->i_idx_cur = -1;
          }
          p = p->ie_next;
        }
      }
      k = level->addr_body + 1;
#ifdef DBSQL_DEBUG
      if( db->flags & DBSQL_VdbeAddopTrace ){
        printf("TRANSLATE cursor %d->%d in opcode range %d..%d\n",
                level->i_tab_cur, level->i_idx_cur, k, last-1);
      }
      /* Proof that the "+1" on the k value above is safe */
      p_op = __vdbe_get_op(v, k - 1);
      assert( p_op->opcode!=OP_Column || p_op->p1!=level->i_tab_cur );
      assert( p_op->opcode!=OP_Rowid  || p_op->p1!=level->i_tab_cur );
      assert( p_op->opcode!=OP_IfNullRow || p_op->p1!=level->i_tab_cur );
#endif
      p_op = __vdbe_get_op(v, k);
      last_op = p_op + (last - k);
      assert( p_op<=last_op );
      do{
        if( p_op->p1!=level->i_tab_cur ){
          /* no-op */
        }else if( p_op->opcode==OP_Column
#ifdef DBSQL_ENABLE_OFFSET_SQL_FUNC
         || p_op->opcode==OP_Offset
#endif
        ){
          int x = p_op->p2;
          assert( idx->table==tab );
#ifdef DBSQL_ENABLE_OFFSET_SQL_FUNC
          if( p_op->opcode==OP_Offset ){
            /* Do not need to translate the column number */
          }else
#endif
          if( !HAS_ROWID(tab) ){
            index_t *pk = __primary_key_index(tab);
            x = pk->columns[x];
            assert( x>=0 );
          }else{
            testcase( x!=__storage_column_to_table(tab,x) );
            x = __storage_column_to_table(tab,x);
          }
          x = __table_column_to_index(idx, x);
          if( x>=0 ){
            p_op->p2 = x;
            p_op->p1 = level->i_idx_cur;
            OPCODE_REWRITE_TRACE(db, k, p_op);
          }else if( loop->ws_flags & (WHERE_IDX_ONLY|WHERE_EXPRIDX) ){
            if( loop->ws_flags & WHERE_IDX_ONLY ){
              /* An error. loop is supposed to be a covering index loop,
              ** and yet the VM code refers to a column of the table that 
              ** is not part of the index.  */
              __error_msg(parser, "internal query planner error");
              parser->rc = DBSQL_INTERNAL;
            }else{
              /* The WHERE_EXPRIDX flag is set by the planner when it is likely
              ** that loop is a covering index loop, but it is not possible
              ** to be 100% sure. In this case, any OP_Explain opcode
              ** corresponding to this loop describes the index as a "COVERING
              ** INDEX". But, op proves that loop is not actually a covering 
              ** index loop. So clear the WHERE_EXPRIDX flag and rewrite the
              ** text that accompanies the OP_Explain opcode, if any.  */
              loop->ws_flags &= ~WHERE_EXPRIDX;
              __where_add_explain_text(parser,
                  level->addr_body-1,
                  tab_list,
                  level,
                  w_info->wctrl_flags
              );
            }
          }
        }else if( p_op->opcode==OP_Rowid ){
          p_op->p1 = level->i_idx_cur;
          p_op->opcode = OP_IdxRowid;
          OPCODE_REWRITE_TRACE(db, k, p_op);
        }else if( p_op->opcode==OP_IfNullRow ){
          p_op->p1 = level->i_idx_cur;
          OPCODE_REWRITE_TRACE(db, k, p_op);
        }
#ifdef DBSQL_DEBUG
        k++;
#endif
      }while( (++p_op)<last_op );
#ifdef DBSQL_DEBUG
      if( db->flags & DBSQL_VdbeAddopTrace ) printf("TRANSLATE complete\n");
#endif
    }
  }

  /* The "break" point is here, just past the end of the outer loop.
  ** Set it.
  */
  __vdbe_resolve_label(v, w_info->i_break);

  /* Final cleanup
  */
  parser->n_query_loop = w_info->saved_n_query_loop;
  __where_info_free(db, w_info);
  parser->within_rj_subrtn -= n_rj;
  return;
}
