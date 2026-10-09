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
** to handle SELECT statements in dbsql.
*/
#include "dbsql_int.h"

/*
** An instance of the following object is used to record information about
** how to process the DISTINCT keyword, to simplify passing that information
** into the __select_inner_loop() routine.
*/
typedef struct distinct_ctx_t distinct_ctx_t;
struct distinct_ctx_t {
  u_int8_t is_tnct;      /* 0: Not distinct. 1: DISTINCT  2: DISTINCT and ORDER BY */
  u_int8_t tnct_type;   /* One of the WHERE_DISTINCT_* operators */
  int tab_tnct;    /* Ephemeral table used for DISTINCT processing */
  int addr_tnct;   /* Address of OP_OpenEphemeral opcode for tab_tnct */
};

/*
** An instance of the following object is used to record information about
** the ORDER BY (or GROUP BY) clause of query is being coded.
**
** The defers[] array is used by the sorter-references optimization. For
** example, assuming there is no index that can be used for the ORDER BY,
** for the query:
**
**     SELECT a, bigblob FROM t1 ORDER BY a LIMIT 10;
**
** it may be more efficient to add just the "a" values to the sorter, and
** retrieve the associated "bigblob" values directly from table t1 as the
** 10 smallest "a" values are extracted from the sorter.
**
** When the sorter-reference optimization is used, there is one entry in the
** defers[] array for each database table that may be read as values are
** extracted from the sorter.
*/
typedef struct sort_ctx_t sort_ctx_t;
struct sort_ctx_t {
  expr_list_t *order_by;   /* The ORDER BY (or GROUP BY clause) */
  int n_ob_sat;           /* Number of ORDER BY terms satisfied by indices */
  int i_e_cursor;         /* Cursor number for the sorter */
  int reg_return;        /* Register holding block-output return address */
  int label_bk_out;       /* Start label for the block-output subroutine */
  int addr_sort_index;    /* Address of the OP_SorterOpen or OP_OpenEphemeral */
  int label_done;        /* Jump here when done, ex: LIMIT reached */
  int label_ob_lopt;      /* Jump here when sorter is full */
  u_int8_t sort_flags;         /* Zero or more SORTFLAG_* bits */
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
  u_int8_t n_defer;            /* Number of valid entries in defers[] */
  struct deferred_csr_t {
    table_t *tab;        /* table_t definition */
    int i_csr;           /* Cursor number for table */
    int n_key;           /* Number of PK columns for table tab (>=1) */
  } defers[4];
#endif
  struct row_load_info_t *deferred_row_load;  /* Deferred row loading info or NULL */
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
  int addr_push;         /* First instruction to push data into sorter */
  int addr_push_end;      /* Last instruction that pushes data into sorter */
#endif
};
#define SORTFLAG_UseSorter  0x01   /* Use __sorter_open instead of __open_ephemeral */

/*
** Delete all the content of a select_t structure.  Deallocate the structure
** itself depending on the value of b_free
**
** If b_free==1, call __dbsql_db_free() on the p object.
** If b_free==0, Leave the first select_t object unfreed
*/
static void __clear_select(DBSQL *db, select_t *p, int b_free){
  assert( db!=0 );
  while( p ){
    select_t *prior = p->prior;
    __expr_list_delete(db, p->e_list);
    __src_list_delete(db, p->src);
    __expr_delete(db, p->where);
    __expr_list_delete(db, p->group_by);
    __expr_delete(db, p->having);
    __expr_list_delete(db, p->order_by);
    __expr_delete(db, p->limit);
    if( OK_IF_ALWAYS_TRUE(p->with) ) __with_delete(db, p->with);
#ifndef DBSQL_OMIT_WINDOWFUNC
    if( OK_IF_ALWAYS_TRUE(p->win_defn) ){
      __window_list_delete(db, p->win_defn);
    }
    while( p->win ){
      assert( p->win->pp_this==&p->win );
      __window_unlink_from_select(p->win);
    }
#endif
    if( b_free ) __db_nn_free_nn(db, p);
    p = prior;
    b_free = 1;
  }
}

/*
** Initialize a select_dest_t structure.
*/
void __select_dest_init(select_dest_t *p_dest, int dest, int i_parm){
  p_dest->dest = (u_int8_t)dest;
  p_dest->i_sd_parm = i_parm;
  p_dest->i_sd_parm2 = 0;
  p_dest->aff_sdst = 0;
  p_dest->i_sdst = 0;
  p_dest->n_sdst = 0;
}


/*
** Allocate a new select_t structure and return a pointer to that
** structure.
*/
select_t *__select_new(
  parser_t *parser,        /* Parsing context */
  expr_list_t *e_list,     /* which columns to include in the result */
  src_list_t *src,        /* the FROM clause -- which tables to scan */
  expr_t *where,         /* the WHERE clause */
  expr_list_t *group_by,   /* the GROUP BY clause */
  expr_t *having,        /* the HAVING clause */
  expr_list_t *order_by,   /* the ORDER BY clause */
  u_int32_t sel_flags,         /* Flag parameters, such as SF_Distinct */
  expr_t *limit          /* LIMIT value.  NULL means not used */
){
  select_t *p_new, *allocated;
  select_t standin;
  allocated = p_new = __db_malloc_raw_nn(parser->db, sizeof(*p_new) );
  if( p_new==0 ){
    assert( parser->db->malloc_failed );
    p_new = &standin;
  }
  if( e_list==0 ){
    e_list = __expr_list_append(parser, 0,
                                   __expr(parser->db,TK_ASTERISK,0));
  }
  p_new->e_list = e_list;
  p_new->op = TK_SELECT;
  p_new->sel_flags = sel_flags;
  p_new->i_limit = 0;
  p_new->i_offset = 0;
  p_new->sel_id = ++parser->n_select;
  p_new->n_select_row = 0;
  if( src==0 ) src = __db_malloc_zero(parser->db, SZ_SRCLIST_1);
  p_new->src = src;
  p_new->where = where;
  p_new->group_by = group_by;
  p_new->having = having;
  p_new->order_by = order_by;
  p_new->prior = 0;
  p_new->next = 0;
  p_new->limit = limit;
  p_new->with = 0;
#ifndef DBSQL_OMIT_WINDOWFUNC
  p_new->win = 0;
  p_new->win_defn = 0;
#endif
  if( parser->db->malloc_failed ) {
    __clear_select(parser->db, p_new, p_new!=&standin);
    allocated = 0;
  }else{
    assert( p_new->src!=0 || parser->n_err>0 );
  }
  return allocated;
}


/*
** Delete the given select_t structure and all of its substructures.
*/
void __select_delete(DBSQL *db, select_t *p){
  if( OK_IF_ALWAYS_TRUE(p) ) __clear_select(db, p, 1);
}
void __select_delete_generic(DBSQL *db, void *p){
  if( ALWAYS(p) ) __clear_select(db, (select_t*)p, 1);
}

/*
** Return a pointer to the right-most SELECT statement in a compound.
*/
static select_t *__find_rightmost(select_t *p){
  while( p->next ) p = p->next;
  return p;
}

/*
** Given 1 to 3 identifiers preceding the JOIN keyword, determine the
** type of join.  Return an integer constant that expresses that type
** in terms of the following bit values:
**
**     JT_INNER
**     JT_CROSS
**     JT_OUTER
**     JT_NATURAL
**     JT_LEFT
**     JT_RIGHT
**
** A full outer join is the combination of JT_LEFT and JT_RIGHT.
**
** If an illegal or unsupported join type is seen, then still return
** a join type, but put an error in the parser structure.
**
** These are the valid join types:
**
**
**      a       b       c               Return Value
**     -------  -----    -----             ------------
**     CROSS      -        -                 JT_CROSS
**     INNER      -        -                 JT_INNER
**     LEFT       -        -                 JT_LEFT|JT_OUTER
**     LEFT     OUTER      -                 JT_LEFT|JT_OUTER
**     RIGHT      -        -                 JT_RIGHT|JT_OUTER
**     RIGHT    OUTER      -                 JT_RIGHT|JT_OUTER
**     FULL       -        -                 JT_LEFT|JT_RIGHT|JT_OUTER
**     FULL     OUTER      -                 JT_LEFT|JT_RIGHT|JT_OUTER
**     NATURAL  INNER      -                 JT_NATURAL|JT_INNER
**     NATURAL  LEFT       -                 JT_NATURAL|JT_LEFT|JT_OUTER
**     NATURAL  LEFT     OUTER               JT_NATURAL|JT_LEFT|JT_OUTER
**     NATURAL  RIGHT      -                 JT_NATURAL|JT_RIGHT|JT_OUTER
**     NATURAL  RIGHT    OUTER               JT_NATURAL|JT_RIGHT|JT_OUTER
**     NATURAL  FULL       -                 JT_NATURAL|JT_LEFT|JT_RIGHT
**     NATURAL  FULL     OUTER               JT_NATRUAL|JT_LEFT|JT_RIGHT
**
** To preserve historical compatibly, dbsql also accepts a variety
** of other non-standard and in many cases nonsensical join types.
** This routine makes as much sense at it can from the nonsense join
** type and returns a result.  Examples of accepted nonsense join types
** include but are not limited to:
**
**          INNER CROSS JOIN        ->   same as JOIN
**          NATURAL CROSS JOIN      ->   same as NATURAL JOIN
**          OUTER LEFT JOIN         ->   same as LEFT JOIN
**          LEFT NATURAL JOIN       ->   same as NATURAL LEFT JOIN
**          LEFT RIGHT JOIN         ->   same as FULL JOIN
**          RIGHT OUTER FULL JOIN   ->   same as FULL JOIN
**          CROSS CROSS CROSS JOIN  ->   same as JOIN
**
** The only restrictions on the join type name are:
**
**    *   "INNER" cannot appear together with "OUTER", "LEFT", "RIGHT",
**        or "FULL".
**
**    *   "CROSS" cannot appear together with "OUTER", "LEFT", "RIGHT,
**        or "FULL".
**
**    *   If "OUTER" is present then there must also be one of
**        "LEFT", "RIGHT", or "FULL"
*/
int __join_type(parser_t *parser, Token *a, Token *b, Token *c){
  int jointype = 0;
  Token *alls[3];
  Token *p;
                             /*   0123456789 123456789 123456789 123 */
  static const char key_text[] = "naturaleftouterightfullinnercross";
  static const struct {
    u_int8_t i;        /* Beginning of keyword text in key_text[] */
    u_int8_t n_char;    /* Length of the keyword in characters */
    u_int8_t code;     /* Join type mask */
  } keywords[] = {
    /* (0) natural */ { 0,  7, JT_NATURAL                },
    /* (1) left    */ { 6,  4, JT_LEFT|JT_OUTER          },
    /* (2) outer   */ { 10, 5, JT_OUTER                  },
    /* (3) right   */ { 14, 5, JT_RIGHT|JT_OUTER         },
    /* (4) full    */ { 19, 4, JT_LEFT|JT_RIGHT|JT_OUTER },
    /* (5) inner   */ { 23, 5, JT_INNER                  },
    /* (6) cross   */ { 28, 5, JT_INNER|JT_CROSS         },
  };
  int i, j;
  alls[0] = a;
  alls[1] = b;
  alls[2] = c;
  for(i=0; i<3 && alls[i]; i++){
    p = alls[i];
    for(j=0; j<ARRAY_SIZE(keywords); j++){
      if( p->n==keywords[j].n_char
          && __str_nicmp((char*)p->z, &key_text[keywords[j].i], p->n)==0 ){
        jointype |= keywords[j].code;
        break;
      }
    }
    testcase( j==0 || j==1 || j==2 || j==3 || j==4 || j==5 || j==6 );
    if( j>=ARRAY_SIZE(keywords) ){
      jointype |= JT_ERROR;
      break;
    }
  }
  if(
     (jointype & (JT_INNER|JT_OUTER))==(JT_INNER|JT_OUTER) ||
     (jointype & JT_ERROR)!=0 ||
     (jointype & (JT_OUTER|JT_LEFT|JT_RIGHT))==JT_OUTER
  ){
    const char *sp1 = " ";
    const char *sp2 = " ";
    if( b==0 ){ sp1++; }
    if( c==0 ){ sp2++; }
    __error_msg(parser, "unknown join type: "
       "%T%s%T%s%T", a, sp1, b, sp2, c);
    jointype = JT_INNER;
  }
  return jointype;
}

/*
** Return the index of a column in a table.  Return -1 if the column
** is not contained in the table.
*/
int __column_index(table_t *tab, const char *col){
  int i;
  u_int8_t h;
  const column_t *cols;
  int n_col;

  h = __str_i_hash(col);
  cols = tab->cols;
  n_col = tab->n_col;

  /* See if the hxes gives us a lucky match */
  i = tab->hxes[h % sizeof(tab->hxes)];
  assert( i<n_col );
  if( cols[i].h_name==h
   && __str_icmp(cols[i].cn_name, col)==0
  ){
    return i;
  }

  /* No lucky match from the hash table.  Do a full search. */
  i = 0;
  while( 1 /*exit-by-break*/ ){
    if( cols[i].h_name==h
     && __str_icmp(cols[i].cn_name, col)==0
    ){
      return i;
    }
    i++;
    if( i>=n_col ) break;
  }
  return -1;
}

/*
** Mark a subquery result column as having been used.
*/
void __src_item_column_used(src_item_t *item, int i_col){
  assert( item!=0 );
  assert( (int)item->fg.is_nested_from == IS_NESTED_FROM(item) );
  if( item->fg.is_nested_from ){
    expr_list_t *results;
    assert( item->fg.is_subquery );
    assert( item->u4.subq!=0 );
    assert( item->u4.subq->p_select!=0 );
    results = item->u4.subq->p_select->e_list;
    assert( results!=0 );
    assert( i_col>=0 && i_col<results->n_expr );
    results->a[i_col].fg.used = 1;
  }
}

/*
** Search the tables i_start..i_end (inclusive) in src, looking for a
** table that has a column named col.  The search is left-to-right.
** The first match found is returned.
**
** When found, set *pi_tab and *pi_col to the table index and column index
** of the matching column and return TRUE.
**
** If not found, return FALSE.
*/
static int __table_and_column_index(
  src_list_t *src,       /* Array of tables to search */
  int i_start,          /* First member of src->a[] to check */
  int i_end,            /* Last member of src->a[] to check */
  const char *col,    /* Name of the column we are looking for */
  int *pi_tab,          /* Write index of src->a[] here */
  int *pi_col,          /* Write index of src->a[*pi_tab].s_tab->cols[] here */
  int ignore_hidden    /* Ignore hidden columns */
){
  int i;               /* For looping over tables in src */
  int i_col;            /* index_t of column matching col */

  assert( i_end<src->n_src );
  assert( i_start>=0 );
  assert( (pi_tab==0)==(pi_col==0) );  /* Both or neither are NULL */

  for(i=i_start; i<=i_end; i++){
    i_col = __column_index(src->a[i].s_tab, col);
    if( i_col>=0
     && (ignore_hidden==0 || IS_HIDDEN_COLUMN(&src->a[i].s_tab->cols[i_col])==0)
    ){
      if( pi_tab ){
        __src_item_column_used(&src->a[i], i_col);
        *pi_tab = i;
        *pi_col = i_col;
      }
      return 1;
    }
  }
  return 0;
}

/*
** Set the EP_OuterON property on all terms of the given expression.
** And set the expr_t.w.i_join to i_table for every term in the
** expression.
**
** The EP_OuterON property is used on terms of an expression to tell
** the OUTER JOIN processing logic that this term is part of the
** join restriction specified in the ON or USING clause and not a part
** of the more general WHERE clause.  These terms are moved over to the
** WHERE clause during join processing but we need to remember that they
** originated in the ON or USING clause.
**
** The expr_t.w.i_join tells the WHERE clause processing that the
** expression depends on table w.i_join even if that table is not
** explicitly mentioned in the expression.  That information is needed
** for cases like this:
**
**    SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.b AND t1.x=5
**
** The where clause needs to defer the handling of the t1.x=5
** term until after the t2 loop of the join.  In that way, a
** NULL t2 row will be inserted whenever t1.x!=5.  If we do not
** defer the handling of t1.x=5, it will be processed immediately
** after the t1 loop and rows with t1.x!=5 will never appear in
** the output, which is incorrect.
*/
void __set_join_expr(expr_t *p, int i_table, u_int32_t join_flag){
  assert( join_flag==EP_OuterON || join_flag==EP_InnerON );
  while( p ){
    EXPR_SET_PROPERTY(p, join_flag);
    assert( !EXPR_HAS_PROPERTY(p, EP_TokenOnly|EP_Reduced) );
    EXPR_SET_VVA_PROPERTY(p, EP_NoReduce);
    p->w.i_join = i_table;
    if( EXPR_USE_X_LIST(p) ){
      if( p->x.list ){
        int i;
        for(i=0; i<p->x.list->n_expr; i++){
          __set_join_expr(p->x.list->a[i].expr, i_table, join_flag);
        }
      }
    }
    __set_join_expr(p->left, i_table, join_flag);
    p = p->right;
  }
}

/* Undo the work of __set_join_expr().  This is used when a LEFT JOIN
** is simplified into an ordinary JOIN, and when an ON expression is
** "pushed down" into the WHERE clause of a subquery.
**
** Convert every term that is marked with EP_OuterON and w.i_join==i_table into
** an ordinary term that omits the EP_OuterON mark.  Or if i_table<0, then
** just clear every EP_OuterON and EP_InnerON mark from the expression tree.
**
** If nullable is true, that means that expr_t p might evaluate to NULL even
** if it is a reference to a NOT NULL column.  This can happen, for example,
** if the table that p references is on the left side of a RIGHT JOIN.
** If nullable is true, then take care to not remove the EP_CanBeNull bit.
** See forum thread https://sqlite.org/forum/forumpost/b40696f50145d21c
*/
static void __unset_join_expr(expr_t *p, int i_table, int nullable){
  while( p ){
    if( i_table<0 || (EXPR_HAS_PROPERTY(p, EP_OuterON) && p->w.i_join==i_table) ){
      EXPR_CLEAR_PROPERTY(p, EP_OuterON|EP_InnerON);
      if( i_table>=0 ) EXPR_SET_PROPERTY(p, EP_InnerON);
    }
    if( p->op==TK_COLUMN && p->i_table==i_table && !nullable ){
      EXPR_CLEAR_PROPERTY(p, EP_CanBeNull);
    }
    if( p->op==TK_FUNCTION ){
      assert( EXPR_USE_X_LIST(p) );
      assert( p->left==0 );
      if( p->x.list ){
        int i;
        for(i=0; i<p->x.list->n_expr; i++){
          __unset_join_expr(p->x.list->a[i].expr, i_table, nullable);
        }
      }
    }
    __unset_join_expr(p->left, i_table, nullable);
    p = p->right;
  }
}

/*
** This routine processes the join information for a SELECT statement.
**
**   *  A NATURAL join is converted into a USING join.  After that, we
**      do not need to be concerned with NATURAL joins and we only have
**      think about USING joins.
**
**   *  ON and USING clauses result in extra terms being added to the
**      WHERE clause to enforce the specified constraints.  The extra
**      WHERE clause terms will be tagged with EP_OuterON or
**      EP_InnerON so that we know that they originated in ON/USING.
**
** The terms of a FROM clause are contained in the select_t.src structure.
** The left most table is the first entry in select_t.src.  The right-most
** table is the last entry.  The join operator is held in the entry to
** the right.  Thus entry 1 contains the join operator for the join between
** entries 0 and 1.  Any ON or USING clauses associated with the join are
** also attached to the right entry.
**
** This routine returns the number of errors encountered.
*/
static int __process_join(parser_t *parser, select_t *p){
  src_list_t *src;                  /* All tables in the FROM clause */
  int i, j;                       /* Loop counters */
  src_item_t *left;                 /* Left table being joined */
  src_item_t *right;                /* Right table being joined */

  src = p->src;
  left = &src->a[0];
  right = &left[1];
  for(i=0; i<src->n_src-1; i++, right++, left++){
    table_t *right_tab = right->s_tab;
    u_int32_t join_type;

    if( NEVER(left->s_tab==0 || right_tab==0) ) continue;
    join_type = (right->fg.jointype & JT_OUTER)!=0 ? EP_OuterON : EP_InnerON;

    /* If this is a NATURAL join, synthesize an appropriate USING clause
    ** to specify which columns should be joined.
    */
    if( right->fg.jointype & JT_NATURAL ){
      id_list_t *p_using = 0;
      if( right->fg.is_using || right->u3.on ){
        __error_msg(parser, "a NATURAL join may not have "
           "an ON or USING clause", 0);
        return 1;
      }
      for(j=0; j<right_tab->n_col; j++){
        char *name;   /* Name of column in the right table */

        if( IS_HIDDEN_COLUMN(&right_tab->cols[j]) ) continue;
        name = right_tab->cols[j].cn_name;
        if( __table_and_column_index(src, 0, i, name, 0, 0, 1) ){
          p_using = __id_list_append(parser, p_using, 0);
          if( p_using ){
            assert( p_using->n_id>0 );
            assert( p_using->a[p_using->n_id-1].name==0 );
            p_using->a[p_using->n_id-1].name = __db_str_dup(parser->db, name);
          }
        }
      }
      if( p_using ){
        right->fg.is_using = 1;
        right->fg.is_synth_using = 1;
        right->u3.p_using = p_using;
      }
      if( parser->n_err ) return 1;
    }

    /* Create extra terms on the WHERE clause for each column named
    ** in the USING clause.  Example: If the two tables to be joined are
    ** A and B and the USING clause names X, Y, and Z, then add this
    ** to the WHERE clause:    A.X=B.X AND A.Y=B.Y AND A.Z=B.Z
    ** Report an error if any column mentioned in the USING clause is
    ** not contained in both tables to be joined.
    */
    if( right->fg.is_using ){
      id_list_t *list = right->u3.p_using;
      DBSQL *db = parser->db;
      assert( list!=0 );
      for(j=0; j<list->n_id; j++){
        char *name;     /* Name of the term in the USING clause */
        int i_left;       /* table_t on the left with matching column name */
        int i_left_col;    /* column_t number of matching column on the left */
        int i_right_col;   /* column_t number of matching column on the right */
        expr_t *e1;       /* Reference to the column on the LEFT of the join */
        expr_t *e2;       /* Reference to the column on the RIGHT of the join */
        expr_t *eq;       /* Equality constraint.  e1 == e2 */

        name = list->a[j].name;
        i_right_col = __column_index(right_tab, name);
        if( i_right_col<0
         || __table_and_column_index(src, 0, i, name, &i_left, &i_left_col,
                                right->fg.is_synth_using)==0
        ){
          __error_msg(parser, "cannot join using column %s - column "
            "not present in both tables", name);
          return 1;
        }
        e1 = __create_column_expr(db, src, i_left, i_left_col);
        __src_item_column_used(&src->a[i_left], i_left_col);
        if( (src->a[0].fg.jointype & JT_LTORJ)!=0 && parser->n_err==0 ){
          /* This branch runs if the query contains one or more RIGHT or FULL
          ** JOINs.  If only a single table on the left side of this join
          ** contains the name column, then this branch is a no-op.
          ** But if there are two or more tables on the left side
          ** of the join, construct a coalesce() function that gathers all
          ** such tables.  Raise an error if more than one of those references
          ** to name is not also within a prior USING clause.
          **
          ** We really ought to raise an error if there are two or more
          ** non-USING references to name on the left of an INNER or LEFT
          ** JOIN.  But older versions of dbsql do not do that, so we avoid
          ** adding a new error so as to not break legacy applications.
          */
          expr_list_t *func_args = 0;   /* Arguments to the coalesce() */
          static const Token tk_coalesce = { "coalesce", 8 };
          assert( e1!=0 );
          EXPR_SET_PROPERTY(e1, EP_CanBeNull);
          while( __table_and_column_index(src, i_left+1, i, name, &i_left, &i_left_col,
                                     right->fg.is_synth_using)!=0 ){
            if( src->a[i_left].fg.is_using==0
             || __id_list_index(src->a[i_left].u3.p_using, name)<0
            ){
              __error_msg(parser, "ambiguous reference to %s in USING()",
                              name);
              break;
            }
            func_args = __expr_list_append(parser, func_args, e1);
            e1 = __create_column_expr(db, src, i_left, i_left_col);
            __src_item_column_used(&src->a[i_left], i_left_col);
          }
          if( func_args ){
            func_args = __expr_list_append(parser, func_args, e1);
            e1 = __expr_function(parser, func_args, &tk_coalesce, 0);
            if( e1 ){
              e1->aff_expr = DBSQL_AFF_DEFER;
            }
          }
        }else if( (src->a[i+1].fg.jointype & JT_LEFT)!=0 && parser->n_err==0 ){
          assert( e1!=0 );
          EXPR_SET_PROPERTY(e1, EP_CanBeNull);
        }
        e2 = __create_column_expr(db, src, i+1, i_right_col);
        __src_item_column_used(right, i_right_col);
        eq = __p_expr(parser, TK_EQ, e1, e2);
        assert( e2!=0 || eq==0 );
        if( eq ){
          EXPR_SET_PROPERTY(eq, join_type);
          assert( !EXPR_HAS_PROPERTY(eq, EP_TokenOnly|EP_Reduced) );
          EXPR_SET_VVA_PROPERTY(eq, EP_NoReduce);
          eq->w.i_join = e2->i_table;
        }
        p->where = __expr_and(parser, p->where, eq);
      }
    }

    /* Add the ON clause to the end of the WHERE clause, connected by
    ** an AND operator.
    */
    else if( right->u3.on ){
      __set_join_expr(right->u3.on, right->i_cursor, join_type);
      p->where = __expr_and(parser, p->where, right->u3.on);
      right->u3.on = 0;
      right->fg.is_on = 1;
      p->sel_flags |= SF_OnToWhere;
    }

    if( IS_VIRTUAL(right_tab) && join_type==EP_OuterON && right->u1.func_arg ){
      p->sel_flags |= SF_OnToWhere;
    }
  }
  return 0;
}

/*
** An instance of this object holds information (beyond parser and p_select)
** needed to load the next result row that is to be added to the sorter.
*/
typedef struct row_load_info_t row_load_info_t;
struct row_load_info_t {
  int reg_result;               /* Store results in array of registers here */
  u_int8_t ecel_flags;                /* Flag argument to __expr_code_expr_list() */
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
  expr_list_t *extra;            /* Extra columns needed by sorter refs */
  int reg_extra_result;          /* Where to load the extra columns */
#endif
};

/*
** This routine does the work of loading query data into an array of
** registers so that it can be added to the sorter.
*/
static void __inner_loop_load_row(
  parser_t *parser,             /* Statement under construction */
  select_t *p_select,           /* The query being coded */
  row_load_info_t *info         /* Info needed to complete the row load */
){
  __expr_code_expr_list(parser, p_select->e_list, info->reg_result,
                          0, info->ecel_flags);
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
  if( info->extra ){
    __expr_code_expr_list(parser, info->extra, info->reg_extra_result, 0, 0);
    __expr_list_delete(parser->db, info->extra);
  }
#endif
}

/*
** Code the OP_MakeRecord instruction that generates the entry to be
** added into the sorter.
**
** Return the register in which the result is stored.
*/
static int __make_sorter_record(
  parser_t *parser,
  sort_ctx_t *sort,
  select_t *p_select,
  int reg_base,
  int n_base
){
  int n_ob_sat = sort->n_ob_sat;
  vdbe_t *v = parser->vdbe;
  int reg_out = ++parser->n_mem;
  if( sort->deferred_row_load ){
    __inner_loop_load_row(parser, p_select, sort->deferred_row_load);
  }
  __vdbe_add_op3(v, OP_MakeRecord, reg_base+n_ob_sat, n_base-n_ob_sat, reg_out);
  return reg_out;
}

/*
** Generate code that will push the record in registers reg_data
** through reg_data+n_data-1 onto the sorter.
*/
static void __push_onto_sorter(
  parser_t *parser,         /* Parser context */
  sort_ctx_t *sort,        /* Information about the ORDER BY clause */
  select_t *p_select,       /* The whole SELECT statement */
  int reg_data,           /* First register holding data to be sorted */
  int reg_orig_data,       /* First register holding data before packing */
  int n_data,             /* Number of elements in the reg_data data array */
  int n_prefix_reg         /* No. of reg prior to reg_data available for use */
){
  vdbe_t *v = parser->vdbe;                         /* Stmt under construction */
  int seq = ((sort->sort_flags & SORTFLAG_UseSorter)==0);
  int n_expr = sort->order_by->n_expr;              /* No. of ORDER BY terms */
  int n_base = n_expr + seq + n_data;                /* Fields in sorter record */
  int reg_base;                                     /* Regs for sorter record */
  int reg_record = 0;                               /* Assembled sorter record */
  int n_ob_sat = sort->n_ob_sat;                      /* ORDER BY terms to skip */
  int op;                            /* Opcode to add sorter record to sorter */
  int i_limit;                        /* LIMIT counter */
  int i_skip = 0;                     /* End of the sorter insert loop */

  assert( seq==0 || seq==1 );

  /* Three cases:
  **   (1) The data to be sorted has already been packed into a Record
  **       by a prior OP_MakeRecord.  In this case n_data==1 and reg_data
  **       will be completely unrelated to reg_orig_data.
  **   (2) All output columns are included in the sort record.  In that
  **       case reg_data==reg_orig_data.
  **   (3) Some output columns are omitted from the sort record due to
  **       the DBSQL_ENABLE_SORTER_REFERENCES optimization, or due to the
  **       DBSQL_ECEL_OMITREF optimization, or due to the
  **       sort_ctx_t.deferred_row_load optimization.  In any of these cases
  **       reg_orig_data is 0 to prevent this routine from trying to copy
  **       values that might not yet exist.
  */
  assert( n_data==1 || reg_data==reg_orig_data || reg_orig_data==0 );

#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
  sort->addr_push = __vdbe_current_addr(v);
#endif

  if( n_prefix_reg ){
    assert( n_prefix_reg==n_expr+seq );
    reg_base = reg_data - n_prefix_reg;
  }else{
    reg_base = parser->n_mem + 1;
    parser->n_mem += n_base;
  }
  assert( p_select->i_offset==0 || p_select->i_limit!=0 );
  i_limit = p_select->i_offset ? p_select->i_offset+1 : p_select->i_limit;
  sort->label_done = __vdbe_make_label(parser);
  __expr_code_expr_list(parser, sort->order_by, reg_base, reg_orig_data,
                          DBSQL_ECEL_DUP | (reg_orig_data? DBSQL_ECEL_REF : 0));
  if( seq ){
    __vdbe_add_op2(v, OP_Sequence, sort->i_e_cursor, reg_base+n_expr);
  }
  if( n_prefix_reg==0 && n_data>0 ){
    __expr_code_move(parser, reg_data, reg_base+n_expr+seq, n_data);
  }
  if( n_ob_sat>0 ){
    int reg_prev_key;   /* The first n_ob_sat columns of the previous row */
    int addr_first;    /* Address of the OP_IfNot opcode */
    int addr_jmp;      /* Address of the OP_Jump opcode */
    vdbe_op_t *p_op;      /* Opcode that opens the sorter */
    int n_key;         /* Number of sorting key columns, including OP_Sequence */
    key_info_t *ki;     /* Original key_info_t on the sorter table */

    reg_record = __make_sorter_record(parser, sort, p_select, reg_base, n_base);
    reg_prev_key = parser->n_mem+1;
    parser->n_mem += sort->n_ob_sat;
    n_key = n_expr - sort->n_ob_sat + seq;
    if( seq ){
      addr_first = __vdbe_add_op1(v, OP_IfNot, reg_base+n_expr);
    }else{
      addr_first = __vdbe_add_op1(v, OP_SequenceTest, sort->i_e_cursor);
    }
    VDBE_COVERAGE(v);
    __vdbe_add_op3(v, OP_Compare, reg_prev_key, reg_base, sort->n_ob_sat);
    p_op = __vdbe_get_op(v, sort->addr_sort_index);
    if( parser->db->malloc_failed ) return;
    p_op->p2 = n_key + n_data;
    ki = p_op->p4.key_info;
    memset(ki->sort_flags, 0, ki->n_key_field); /* Makes OP_Jump testable */
    __vdbe_change_p4(v, -1, (char*)ki, P4_KEYINFO);
    testcase( ki->n_all_field > ki->n_key_field+2 );
    p_op->p4.key_info = __key_info_from_expr_list(parser,sort->order_by,n_ob_sat,
                                           ki->n_all_field-ki->n_key_field-1);
    p_op = 0; /* Ensure op not used after __vdbe_add_op3() */
    addr_jmp = __vdbe_current_addr(v);
    __vdbe_add_op3(v, OP_Jump, addr_jmp+1, 0, addr_jmp+1); VDBE_COVERAGE(v);
    sort->label_bk_out = __vdbe_make_label(parser);
    sort->reg_return = ++parser->n_mem;
    __vdbe_add_op2(v, OP_Gosub, sort->reg_return, sort->label_bk_out);
    __vdbe_add_op1(v, OP_ResetSorter, sort->i_e_cursor);
    if( i_limit ){
      __vdbe_add_op2(v, OP_IfNot, i_limit, sort->label_done);
      VDBE_COVERAGE(v);
    }
    __vdbe_jump_here(v, addr_first);
    __expr_code_move(parser, reg_base, reg_prev_key, sort->n_ob_sat);
    __vdbe_jump_here(v, addr_jmp);
  }
  if( i_limit ){
    /* At this point the values for the new sorter entry are stored
    ** in an array of registers. They need to be composed into a record
    ** and inserted into the sorter if either (a) there are currently
    ** less than LIMIT+OFFSET items or (b) the new record is smaller than
    ** the largest record currently in the sorter. If (b) is true and there
    ** are already LIMIT+OFFSET items in the sorter, delete the largest
    ** entry before inserting the new one. This way there are never more
    ** than LIMIT+OFFSET items in the sorter.
    **
    ** If the new record does not need to be inserted into the sorter,
    ** jump to the next iteration of the loop. If the sort->label_ob_lopt
    ** value is not zero, then it is a label of where to jump.  Otherwise,
    ** just bypass the row insert logic.  See the header comment on the
    ** __where_order_by_limit_opt_label() function for additional info.
    */
    int i_csr = sort->i_e_cursor;
    __vdbe_add_op2(v, OP_IfNotZero, i_limit, __vdbe_current_addr(v)+4);
    VDBE_COVERAGE(v);
    __vdbe_add_op2(v, OP_Last, i_csr, 0);
    i_skip = __vdbe_add_op4_int(v, OP_IdxLE,
                                 i_csr, 0, reg_base+n_ob_sat, n_expr-n_ob_sat);
    VDBE_COVERAGE(v);
    __vdbe_add_op1(v, OP_Delete, i_csr);
  }
  if( reg_record==0 ){
    reg_record = __make_sorter_record(parser, sort, p_select, reg_base, n_base);
  }
  if( sort->sort_flags & SORTFLAG_UseSorter ){
    op = OP_SorterInsert;
  }else{
    op = OP_IdxInsert;
  }
  __vdbe_add_op4_int(v, op, sort->i_e_cursor, reg_record,
                       reg_base+n_ob_sat, n_base-n_ob_sat);
  if( i_skip ){
    __vdbe_change_p2(v, i_skip,
         sort->label_ob_lopt ? sort->label_ob_lopt : __vdbe_current_addr(v));
  }
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
  sort->addr_push_end = __vdbe_current_addr(v)-1;
#endif
}

/*
** Add code to implement the OFFSET
*/
static void __code_offset(
  vdbe_t *v,          /* Generate code into this VM */
  int i_offset,      /* Register holding the offset counter */
  int i_continue     /* Jump here to skip the current record */
){
  if( i_offset>0 ){
    __vdbe_add_op3(v, OP_IfPos, i_offset, i_continue, 1); VDBE_COVERAGE(v);
    VDBE_COMMENT((v, "OFFSET"));
  }
}

/*
** Add code that will check to make sure the array of registers starting at
** i_mem form a distinct entry. This is used by both "SELECT DISTINCT ..." and
** distinct aggregates ("SELECT count(DISTINCT <expr>) ..."). Three strategies
** are available. Which is used depends on the value of parameter tnct_type,
** as follows:
**
**   WHERE_DISTINCT_UNORDERED/WHERE_DISTINCT_NOOP:
**     Build an ephemeral table that contains all entries seen before and
**     skip entries which have been seen before.
**
**     Parameter i_tab is the cursor number of an ephemeral table that must
**     be opened before the VM code generated by this routine is executed.
**     The ephemeral cursor table is queried for a record identical to the
**     record formed by the current array of registers. If one is found,
**     jump to VM address addr_repeat. Otherwise, insert a new record into
**     the ephemeral cursor and proceed.
**
**     The returned value in this case is a copy of parameter i_tab.
**
**   WHERE_DISTINCT_ORDERED:
**     In this case rows are being delivered sorted order. The ephemeral
**     table is not required. Instead, the current set of values
**     is compared against previous row. If they match, the new row
**     is not distinct and control jumps to VM address addr_repeat. Otherwise,
**     the VM program proceeds with processing the new row.
**
**     The returned value in this case is the register number of the first
**     in an array of registers used to store the previous result row so that
**     it can be compared to the next. The caller must ensure that this
**     register is initialized to NULL.  (The __fix_distinct_open_eph() routine
**     will take care of this initialization.)
**
**   WHERE_DISTINCT_UNIQUE:
**     In this case it has already been determined that the rows are distinct.
**     No special action is required. The return value is zero.
**
** Parameter e_list is the list of expressions used to generated the
** contents of each row. It is used by this routine to determine (a)
** how many elements there are in the array of registers and (b) the
** collation sequences that should be used for the comparisons if
** tnct_type is WHERE_DISTINCT_ORDERED.
*/
static int __code_distinct(
  parser_t *parser,     /* Parsing and code generating context */
  int tnct_type,     /* WHERE_DISTINCT_* value */
  int i_tab,          /* A sorting index used to test for distinctness */
  int addr_repeat,    /* Jump to here if not distinct */
  expr_list_t *e_list,  /* Expression for each element */
  int reg_elem        /* First element */
){
  int i_ret = 0;
  int n_result_col = e_list->n_expr;
  vdbe_t *v = parser->vdbe;

  switch( tnct_type ){
    case WHERE_DISTINCT_ORDERED: {
      int i;
      int i_jump;              /* Jump destination */
      int reg_prev;            /* Previous row content */

      /* Allocate space for the previous row */
      i_ret = reg_prev = parser->n_mem+1;
      parser->n_mem += n_result_col;

      i_jump = __vdbe_current_addr(v) + n_result_col;
      for(i=0; i<n_result_col; i++){
        coll_seq_t *coll = __expr_coll_seq(parser, e_list->a[i].expr);
        if( i<n_result_col-1 ){
          __vdbe_add_op3(v, OP_Ne, reg_elem+i, i_jump, reg_prev+i);
          VDBE_COVERAGE(v);
        }else{
          __vdbe_add_op3(v, OP_Eq, reg_elem+i, addr_repeat, reg_prev+i);
          VDBE_COVERAGE(v);
         }
        __vdbe_change_p4(v, -1, (const char *)coll, P4_COLLSEQ);
        __vdbe_change_p5(v, DBSQL_NULLEQ);
      }
      assert( __vdbe_current_addr(v)==i_jump || parser->db->malloc_failed );
      __vdbe_add_op3(v, OP_Copy, reg_elem, reg_prev, n_result_col-1);
      break;
    }

    case WHERE_DISTINCT_UNIQUE: {
      /* nothing to do */
      break;
    }

    default: {
      int r1 = __get_temp_reg(parser);
      __vdbe_add_op4_int(v, OP_Found, i_tab, addr_repeat, reg_elem, n_result_col);
      VDBE_COVERAGE(v);
      __vdbe_add_op3(v, OP_MakeRecord, reg_elem, n_result_col, r1);
      __vdbe_add_op4_int(v, OP_IdxInsert, i_tab, r1, reg_elem, n_result_col);
      __vdbe_change_p5(v, OPFLAG_USESEEKRESULT);
      __release_temp_reg(parser, r1);
      i_ret = i_tab;
      break;
    }
  }

  return i_ret;
}

/*
** This routine runs after __code_distinct().  It makes necessary
** adjustments to the OP_OpenEphemeral opcode that the __code_distinct()
** routine made use of.  This processing must be done separately since
** sometimes __code_distinct is called before the OP_OpenEphemeral is actually
** laid down.
**
** WHERE_DISTINCT_NOOP:
** WHERE_DISTINCT_UNORDERED:
**
**     No adjustments necessary.  This function is a no-op.
**
** WHERE_DISTINCT_UNIQUE:
**
**     The ephemeral table is not needed.  So change the
**     OP_OpenEphemeral opcode into an OP_Noop.
**
** WHERE_DISTINCT_ORDERED:
**
**     The ephemeral table is not needed.  But we do need register
**     i_val to be initialized to NULL.  So change the OP_OpenEphemeral
**     into an OP_Null on the i_val register.
*/
static void __fix_distinct_open_eph(
  parser_t *parser,     /* Parsing and code generating context */
  int tnct_type,     /* WHERE_DISTINCT_* value */
  int i_val,          /* Value returned by __code_distinct() */
  int i_open_eph_addr   /* Address of OP_OpenEphemeral instruction for i_tab */
){
  if( parser->n_err==0
   && (tnct_type==WHERE_DISTINCT_UNIQUE || tnct_type==WHERE_DISTINCT_ORDERED)
  ){
    vdbe_t *v = parser->vdbe;
    __vdbe_change_to_noop(v, i_open_eph_addr);
    if( __vdbe_get_op(v, i_open_eph_addr+1)->opcode==OP_Explain ){
      __vdbe_change_to_noop(v, i_open_eph_addr+1);
    }
    if( tnct_type==WHERE_DISTINCT_ORDERED ){
      /* Change the OP_OpenEphemeral to an OP_Null that sets the MEM_Cleared
      ** bit on the first register of the previous value.  This will cause the
      ** OP_Ne added in __code_distinct() to always fail on the first iteration of
      ** the loop even if the first row is all NULLs.  */
      vdbe_op_t *op = __vdbe_get_op(v, i_open_eph_addr);
      op->opcode = OP_Null;
      op->p1 = 1;
      op->p2 = i_val;
    }
  }
}

#ifdef DBSQL_ENABLE_SORTER_REFERENCES
/*
** This function is called as part of inner-loop generation for a SELECT
** statement with an ORDER BY that is not optimized by an index. It
** determines the expressions, if any, that the sorter-reference
** optimization should be used for. The sorter-reference optimization
** is used for SELECT queries like:
**
**   SELECT a, bigblob FROM t1 ORDER BY a LIMIT 10
**
** If the optimization is used for expression "bigblob", then instead of
** storing values read from that column in the sorter records, the PK of
** the row from table t1 is stored instead. Then, as records are extracted from
** the sorter to return to the user, the required value of bigblob is
** retrieved directly from table t1. If the values are very large, this
** can be more efficient than storing them directly in the sorter records.
**
** The expr_list_item_t.fg.sorter_ref flag is set for each expression in e_list
** for which the sorter-reference optimization should be enabled.
** Additionally, the sort->defers[] array is populated with entries
** for all cursors required to evaluate all selected expressions. Finally.
** output variable (*extra) is set to an expression list containing
** expressions for all extra PK values that should be stored in the
** sorter records.
*/
static void __select_expr_defer(
  parser_t *parser,                  /* Leave any error here */
  sort_ctx_t *sort,                 /* Sorter context */
  expr_list_t *e_list,               /* Expressions destined for sorter */
  expr_list_t **pp_extra              /* Expressions to append to sorter record */
){
  int i;
  int n_defer = 0;
  expr_list_t *extra = 0;
  for(i=0; i<e_list->n_expr; i++){
    struct expr_list_item_t *item = &e_list->a[i];
    if( item->u.x.i_order_by_col==0 ){
      expr_t *expr = item->expr;
      table_t *tab;
      if( expr->op==TK_COLUMN
       && expr->i_column>=0
       && ALWAYS( EXPR_USE_Y_TAB(expr) )
       && (tab = expr->y.tab)!=0
       && IS_ORDINARY_TABLE(tab)
       && (tab->cols[expr->i_column].col_flags & COLFLAG_SORTERREF)!=0
      ){
        int j;
        for(j=0; j<n_defer; j++){
          if( sort->defers[j].i_csr==expr->i_table ) break;
        }
        if( j==n_defer ){
          if( n_defer==ARRAY_SIZE(sort->defers) ){
            continue;
          }else{
            int n_key = 1;
            int k;
            index_t *pk = 0;
            if( !HAS_ROWID(tab) ){
              pk = __primary_key_index(tab);
              n_key = pk->n_key_col;
            }
            for(k=0; k<n_key; k++){
              expr_t *p_new = __p_expr(parser, TK_COLUMN, 0, 0);
              if( p_new ){
                p_new->i_table = expr->i_table;
                assert( EXPR_USE_Y_TAB(p_new) );
                p_new->y.tab = expr->y.tab;
                p_new->i_column = pk ? pk->columns[k] : -1;
                extra = __expr_list_append(parser, extra, p_new);
              }
            }
            sort->defers[n_defer].tab = expr->y.tab;
            sort->defers[n_defer].i_csr = expr->i_table;
            sort->defers[n_defer].n_key = n_key;
            n_defer++;
          }
        }
        item->fg.sorter_ref = 1;
      }
    }
  }
  sort->n_defer = (u_int8_t)n_defer;
  *pp_extra = extra;
}
#endif

/*
** This routine generates the code for the inside of the inner loop
** of a SELECT.
**
** If src_tab is negative, then the p->e_list expressions
** are evaluated in order to get the data for this row.  If src_tab is
** zero or more, then data is pulled from src_tab and p->e_list is used only
** to get the number of columns and the collation sequence for each column.
*/
static void __select_inner_loop(
  parser_t *parser,          /* The parser context */
  select_t *p,              /* The complete select statement being coded */
  int src_tab,             /* Pull data from this table if non-negative */
  sort_ctx_t *sort,         /* If not NULL, info on how to process ORDER BY */
  distinct_ctx_t *distinct, /* If not NULL, info on how to process DISTINCT */
  select_dest_t *p_dest,      /* How to dispose of the results */
  int i_continue,          /* Jump here to continue with next row */
  int i_break              /* Jump here to break out of the inner loop */
){
  vdbe_t *v = parser->vdbe;
  int i;
  int has_distinct;            /* True if the DISTINCT keyword is present */
  int dest = p_dest->dest;   /* How to dispose of results */
  int i_parm = p_dest->i_sd_parm; /* First argument to disposal method */
  int n_result_col;             /* Number of result columns */
  int n_prefix_reg = 0;         /* Number of extra registers before reg_result */
  row_load_info_t s_row_load_info;   /* Info for deferred row loading */

  /* Usually, reg_result is the first cell in an array of memory cells
  ** containing the current result row. In this case reg_orig is set to the
  ** same value. However, if the results are being sent to the sorter, the
  ** values for any expressions that are also part of the sort-key are omitted
  ** from this array. In this case reg_orig is set to zero.  */
  int reg_result;              /* Start of memory holding current results */
  int reg_orig;                /* Start of memory holding full result (or 0) */

  assert( v );
  assert( p->e_list!=0 );
  has_distinct = distinct ? distinct->tnct_type : WHERE_DISTINCT_NOOP;
  if( sort && sort->order_by==0 ) sort = 0;
  if( sort==0 && !has_distinct ){
    assert( i_continue!=0 );
    __code_offset(v, p->i_offset, i_continue);
  }

  /* Pull the requested columns.
  */
  n_result_col = p->e_list->n_expr;

  if( p_dest->i_sdst==0 ){
    if( sort ){
      n_prefix_reg = sort->order_by->n_expr;
      if( !(sort->sort_flags & SORTFLAG_UseSorter) ) n_prefix_reg++;
      parser->n_mem += n_prefix_reg;
    }
    p_dest->i_sdst = parser->n_mem+1;
    parser->n_mem += n_result_col;
  }else if( p_dest->i_sdst+n_result_col > parser->n_mem ){
    /* This is an error condition that can result, for example, when a SELECT
    ** on the right-hand side of an INSERT contains more result columns than
    ** there are columns in the table on the left.  The error will be caught
    ** and reported later.  But we need to make sure enough memory is allocated
    ** to avoid other spurious errors in the meantime. */
    parser->n_mem += n_result_col;
  }
  p_dest->n_sdst = n_result_col;
  reg_orig = reg_result = p_dest->i_sdst;
  if( src_tab>=0 ){
    for(i=0; i<n_result_col; i++){
      __vdbe_add_op3(v, OP_Column, src_tab, i, reg_result+i);
      VDBE_COMMENT((v, "%s", p->e_list->a[i].e_name));
    }
  }else if( dest!=SRT_Exists ){
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
    expr_list_t *extra = 0;
#endif
    /* If the destination is an EXISTS(...) expression, the actual
    ** values returned by the SELECT are not required.
    */
    u_int8_t ecel_flags;    /* "ecel" is an abbreviation of "__expr_code_expr_list" */
    expr_list_t *e_list;
    if( dest==SRT_Mem || dest==SRT_Output || dest==SRT_Coroutine ){
      ecel_flags = DBSQL_ECEL_DUP;
    }else{
      ecel_flags = 0;
    }
    if( sort && has_distinct==0 && dest!=SRT_EphemTab && dest!=SRT_Table ){
      /* For each expression in p->e_list that is a copy of an expression in
      ** the ORDER BY clause (sort->order_by), set the associated
      ** i_order_by_col value to one more than the index of the ORDER BY
      ** expression within the sort-key that __push_onto_sorter() will generate.
      ** This allows the p->e_list field to be omitted from the sorted record,
      ** saving space and CPU cycles.  */
      ecel_flags |= (DBSQL_ECEL_OMITREF|DBSQL_ECEL_REF);

      for(i=sort->n_ob_sat; i<sort->order_by->n_expr; i++){
        int j;
        if( (j = sort->order_by->a[i].u.x.i_order_by_col)>0 ){
          p->e_list->a[j-1].u.x.i_order_by_col = i+1-sort->n_ob_sat;
        }
      }
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
      __select_expr_defer(parser, sort, p->e_list, &extra);
      if( extra && parser->db->malloc_failed==0 ){
        /* If there are any extra PK columns to add to the sorter records,
        ** allocate extra memory cells and adjust the __open_ephemeral
        ** instruction to account for the larger records. This is only
        ** required if there are one or more WITHOUT ROWID tables with
        ** composite primary keys in the sort_ctx_t.defers[] array.  */
        vdbe_op_t *op = __vdbe_get_op(v, sort->addr_sort_index);
        op->p2 += (extra->n_expr - sort->n_defer);
        op->p4.key_info->n_all_field += (extra->n_expr - sort->n_defer);
        parser->n_mem += extra->n_expr;
      }
#endif

      /* Adjust n_result_col to account for columns that are omitted
      ** from the sorter by the optimizations in this branch */
      e_list = p->e_list;
      for(i=0; i<e_list->n_expr; i++){
        if( e_list->a[i].u.x.i_order_by_col>0
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
         || e_list->a[i].fg.sorter_ref
#endif
        ){
          n_result_col--;
          reg_orig = 0;
        }
      }

      testcase( reg_orig );
      testcase( dest==SRT_Set );
      testcase( dest==SRT_Mem );
      testcase( dest==SRT_Coroutine );
      testcase( dest==SRT_Output );
      assert( dest==SRT_Set || dest==SRT_Mem
           || dest==SRT_Coroutine || dest==SRT_Output
           || dest==SRT_Upfrom );
    }
    s_row_load_info.reg_result = reg_result;
    s_row_load_info.ecel_flags = ecel_flags;
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
    s_row_load_info.extra = extra;
    s_row_load_info.reg_extra_result = reg_result + n_result_col;
    if( extra ) n_result_col += extra->n_expr;
#endif
    if( p->i_limit
     && (ecel_flags & DBSQL_ECEL_OMITREF)!=0
     && n_prefix_reg>0
    ){
      assert( sort!=0 );
      assert( has_distinct==0 );
      sort->deferred_row_load = &s_row_load_info;
      reg_orig = 0;
    }else{
      __inner_loop_load_row(parser, p, &s_row_load_info);
    }
  }

  /* If the DISTINCT keyword was present on the SELECT statement
  ** and this row has been seen before, then do not make this row
  ** part of the result.
  */
  if( has_distinct ){
    int type = distinct->tnct_type;
    int i_tab = distinct->tab_tnct;
    assert( n_result_col==p->e_list->n_expr );
    i_tab = __code_distinct(parser, type, i_tab, i_continue, p->e_list, reg_result);
    __fix_distinct_open_eph(parser, type, i_tab, distinct->addr_tnct);
    if( sort==0 ){
      __code_offset(v, p->i_offset, i_continue);
    }
  }

  switch( dest ){
    /* Store the result as data using a unique key.
    */
    case SRT_Fifo:
    case SRT_DistFifo:
    case SRT_Table:
    case SRT_EphemTab: {
      int r1 = __get_temp_range(parser, n_prefix_reg+1);
      testcase( dest==SRT_Table );
      testcase( dest==SRT_EphemTab );
      testcase( dest==SRT_Fifo );
      testcase( dest==SRT_DistFifo );
      __vdbe_add_op3(v, OP_MakeRecord, reg_result, n_result_col, r1+n_prefix_reg);
#if !defined(DBSQL_ENABLE_NULL_TRIM) && defined(DBSQL_DEBUG)
      /* A destination of SRT_Table and a non-zero i_sd_parm2 parameter means
      ** that this is an "UPDATE ... FROM" on a virtual table or view. In this
      ** case set the p5 parameter of the OP_MakeRecord to OPFLAG_NOCHNG_MAGIC.
      ** This does not affect operation in any way - it just allows __make_record
      ** to process OPFLAG_NOCHANGE values without an assert() failing. */
      if( dest==SRT_Table && p_dest->i_sd_parm2 ){
        __vdbe_change_p5(v, OPFLAG_NOCHNG_MAGIC);
      }
#endif
#ifndef DBSQL_OMIT_CTE
      if( dest==SRT_DistFifo ){
        /* If the destination is __dist_fifo, then cursor (i_parm+1) is open
        ** on an ephemeral index. If the current row is already present
        ** in the index, do not write it to the output. If not, add the
        ** current row to the index and proceed with writing it to the
        ** output table as well.  */
        int addr = __vdbe_current_addr(v) + 4;
        __vdbe_add_op4_int(v, OP_Found, i_parm+1, addr, r1, 0);
        VDBE_COVERAGE(v);
        __vdbe_add_op4_int(v, OP_IdxInsert, i_parm+1, r1,reg_result,n_result_col);
        assert( sort==0 );
      }
#endif
      if( sort ){
        assert( reg_result==reg_orig );
        __push_onto_sorter(parser, sort, p, r1+n_prefix_reg, reg_orig, 1, n_prefix_reg);
      }else{
        int r2 = __get_temp_reg(parser);
        __vdbe_add_op2(v, OP_NewRowid, i_parm, r2);
        __vdbe_add_op3(v, OP_Insert, i_parm, r1, r2);
        __vdbe_change_p5(v, OPFLAG_APPEND);
        __release_temp_reg(parser, r2);
      }
      __release_temp_range(parser, r1, n_prefix_reg+1);
      break;
    }

    case SRT_Upfrom: {
      if( sort ){
        __push_onto_sorter(
            parser, sort, p, reg_result, reg_orig, n_result_col, n_prefix_reg);
      }else{
        int i2 = p_dest->i_sd_parm2;
        int r1 = __get_temp_reg(parser);

        /* If the UPDATE FROM join is an aggregate that matches no rows, it
        ** might still be trying to return one row, because that is what
        ** aggregates do.  Don't record that empty row in the output table. */
        __vdbe_add_op2(v, OP_IsNull, reg_result, i_break); VDBE_COVERAGE(v);

        __vdbe_add_op3(v, OP_MakeRecord,
                          reg_result+(i2<0), n_result_col-(i2<0), r1);
        if( i2<0 ){
          __vdbe_add_op3(v, OP_Insert, i_parm, r1, reg_result);
        }else{
          __vdbe_add_op4_int(v, OP_IdxInsert, i_parm, r1, reg_result, i2);
        }
      }
      break;
    }

#ifndef DBSQL_OMIT_SUBQUERY
    /* If we are creating a set for an "expr IN (SELECT ...)" construct,
    ** then there should be a single item on the stack.  Write this
    ** item into the set table with bogus data.
    */
    case SRT_Set: {
      if( sort ){
        /* At first glance you would think we could optimize out the
        ** ORDER BY in this case since the order of entries in the set
        ** does not matter.  But there might be a LIMIT clause, in which
        ** case the order does matter */
        __push_onto_sorter(
            parser, sort, p, reg_result, reg_orig, n_result_col, n_prefix_reg);
        p_dest->i_sd_parm2 = 0; /* Signal that any Bloom filter is unpopulated */
      }else{
        int r1 = __get_temp_reg(parser);
        assert( __strlen30(p_dest->aff_sdst)==n_result_col );
        __vdbe_add_op4(v, OP_MakeRecord, reg_result, n_result_col,
            r1, p_dest->aff_sdst, n_result_col);
        __vdbe_add_op4_int(v, OP_IdxInsert, i_parm, r1, reg_result, n_result_col);
        if( p_dest->i_sd_parm2 ){
          __vdbe_add_op4_int(v, OP_FilterAdd, p_dest->i_sd_parm2, 0,
                               reg_result, n_result_col);
          EXPLAIN_QUERY_PLAN((parser, 0, "CREATE BLOOM FILTER"));
        }
        __release_temp_reg(parser, r1);
      }
      break;
    }


    /* If any row exist in the result set, record that fact and abort.
    */
    case SRT_Exists: {
      __vdbe_add_op2(v, OP_Integer, 1, i_parm);
      /* The LIMIT clause will terminate the loop for us */
      break;
    }

    /* If this is a scalar select that is part of an expression, then
    ** store the results in the appropriate memory cell or array of
    ** memory cells and break out of the scan loop.
    */
    case SRT_Mem: {
      if( sort ){
        assert( n_result_col<=p_dest->n_sdst );
        __push_onto_sorter(
            parser, sort, p, reg_result, reg_orig, n_result_col, n_prefix_reg);
        p_dest->i_sd_parm = reg_result;
      }else{
        assert( n_result_col==p_dest->n_sdst );
        if( reg_result!=i_parm ){
          /* This occurs in cases where the SELECT had both a DISTINCT and
          ** an OFFSET clause.  */
          __vdbe_add_op3(v, OP_Copy, reg_result, i_parm, n_result_col-1);
        }
        /* The LIMIT clause will jump out of the loop for us */
      }
      break;
    }
#endif /* #ifndef DBSQL_OMIT_SUBQUERY */

    case SRT_Coroutine:       /* Send data to a co-routine */
    case SRT_Output: {        /* Return the results */
      testcase( dest==SRT_Coroutine );
      testcase( dest==SRT_Output );
      if( sort ){
        __push_onto_sorter(parser, sort, p, reg_result, reg_orig, n_result_col,
                       n_prefix_reg);
      }else if( dest==SRT_Coroutine ){
        __vdbe_add_op1(v, OP_Yield, p_dest->i_sd_parm);
      }else{
        __vdbe_add_op2(v, OP_ResultRow, reg_result, n_result_col);
      }
      break;
    }

#ifndef DBSQL_OMIT_CTE
    /* Write the results into a priority queue that is order according to
    ** dest->order_by (in so).  dest->i_sd_parm (in i_parm) is the cursor for an
    ** index with so->n_expr+2 columns.  Build a key using so for the first
    ** so->n_expr columns, then make sure all keys are unique by adding a
    ** final OP_Sequence column.  The last column is the record as a blob.
    */
    case SRT_DistQueue:
    case SRT_Queue: {
      int n_key;
      int r1, r2, r3;
      int addr_test = 0;
      expr_list_t *so;
      so = p_dest->order_by;
      assert( so );
      n_key = so->n_expr;
      r1 = __get_temp_reg(parser);
      r2 = __get_temp_range(parser, n_key+2);
      r3 = r2+n_key+1;
      if( dest==SRT_DistQueue ){
        /* If the destination is __dist_queue, then cursor (i_parm+1) is open
        ** on a second ephemeral index that holds all values every previously
        ** added to the queue. */
        addr_test = __vdbe_add_op4_int(v, OP_Found, i_parm+1, 0,
                                        reg_result, n_result_col);
        VDBE_COVERAGE(v);
      }
      __vdbe_add_op3(v, OP_MakeRecord, reg_result, n_result_col, r3);
      if( dest==SRT_DistQueue ){
        __vdbe_add_op2(v, OP_IdxInsert, i_parm+1, r3);
        __vdbe_change_p5(v, OPFLAG_USESEEKRESULT);
      }
      for(i=0; i<n_key; i++){
        __vdbe_add_op2(v, OP_SCopy,
                          reg_result + so->a[i].u.x.i_order_by_col - 1,
                          r2+i);
      }
      __vdbe_add_op2(v, OP_Sequence, i_parm, r2+n_key);
      __vdbe_add_op3(v, OP_MakeRecord, r2, n_key+2, r1);
      __vdbe_add_op4_int(v, OP_IdxInsert, i_parm, r1, r2, n_key+2);
      if( addr_test ) __vdbe_jump_here(v, addr_test);
      __release_temp_reg(parser, r1);
      __release_temp_range(parser, r2, n_key+2);
      break;
    }
#endif /* DBSQL_OMIT_CTE */



#if !defined(DBSQL_OMIT_TRIGGER)
    /* Discard the results.  This is used for SELECT statements inside
    ** the body of a TRIGGER.  The purpose of such selects is to call
    ** user-defined functions that have side effects.  We do not care
    ** about the actual results of the select.
    */
    default: {
      assert( dest==SRT_Discard );
      break;
    }
#endif
  }

  /* Jump to the end of the loop if the LIMIT is reached.  Except, if
  ** there is a sorter, in which case the sorter has already limited
  ** the output for us.
  */
  if( sort==0 && p->i_limit ){
    __vdbe_add_op2(v, OP_DecrJumpZero, p->i_limit, i_break); VDBE_COVERAGE(v);
  }
}

/*
** Allocate a key_info_t object sufficient for an index of N key columns and
** X extra columns.
*/
key_info_t *__key_info_alloc(DBSQL *db, int N, int X){
  int n_extra = (N+X)*(sizeof(coll_seq_t*)+1);
  key_info_t *p;
  assert( X>=0 );
  if( NEVER(N+X>0xffff) ) return (key_info_t*)__oom_fault(db);
  p = __db_malloc_raw_nn(db, SZ_KEYINFO(0) + n_extra);
  if( p ){
    p->sort_flags = (u_int8_t*)&p->colls[N+X];
    p->n_key_field = (u_int16_t)N;
    p->n_all_field = (u_int16_t)(N+X);
    p->enc = ENC(db);
    p->db = db;
    p->n_ref = 1;
    memset(p->colls, 0, n_extra);
  }else{
    return (key_info_t*)__oom_fault(db);
  }
  return p;
}

/*
** Deallocate a key_info_t object
*/
void __key_info_unref(key_info_t *p){
  if( p ){
    assert( p->db!=0 );
    assert( p->n_ref>0 );
    p->n_ref--;
    if( p->n_ref==0 ) __db_nn_free_nn(p->db, p);
  }
}

/*
** Make a new pointer to a key_info_t object
*/
key_info_t *__key_info_ref(key_info_t *p){
  if( p ){
    assert( p->n_ref>0 );
    p->n_ref++;
  }
  return p;
}

#ifdef DBSQL_DEBUG
/*
** Return TRUE if a key_info_t object can be change.  The key_info_t object
** can only be changed if this is just a single reference to the object.
**
** This routine is used only inside of assert() statements.
*/
int __key_info_is_writeable(key_info_t *p){ return p->n_ref==1; }
#endif /* DBSQL_DEBUG */

/*
** Given an expression list, generate a key_info_t structure that records
** the collating sequence for each expression in that expression list.
**
** If the expr_list_t is an ORDER BY or GROUP BY clause then the resulting
** key_info_t structure is appropriate for initializing a virtual index to
** implement that clause.  If the expr_list_t is the result set of a SELECT
** then the key_info_t structure is appropriate for initializing a virtual
** index to implement a DISTINCT test.
**
** Space to hold the key_info_t structure is obtained from malloc.  The calling
** function is responsible for seeing that this structure is eventually
** freed.
*/
key_info_t *__key_info_from_expr_list(
  parser_t *parser,       /* Parsing context */
  expr_list_t *list,     /* Form the key_info_t object from this expr_list_t */
  int i_start,          /* Begin with this column of list */
  int n_extra           /* Add this many extra columns to the end */
){
  int n_expr;
  key_info_t *info;
  struct expr_list_item_t *item;
  DBSQL *db = parser->db;
  int i;

  n_expr = list->n_expr;
  info = __key_info_alloc(db, n_expr-i_start, n_extra+1);
  if( info ){
    assert( __key_info_is_writeable(info) );
    for(i=i_start, item=list->a+i_start; i<n_expr; i++, item++){
      info->colls[i-i_start] = __expr_nn_coll_seq(parser, item->expr);
      info->sort_flags[i-i_start] = item->fg.sort_flags;
    }
  }
  return info;
}

/*
** Name of the connection operator, used for error messages.
*/
const char *__select_op_name(int id){
  char *z;
  switch( id ){
    case TK_ALL:       z = "UNION ALL";   break;
    case TK_INTERSECT: z = "INTERSECT";   break;
    case TK_EXCEPT:    z = "EXCEPT";      break;
    default:           z = "UNION";       break;
  }
  return z;
}

#ifndef DBSQL_OMIT_EXPLAIN
/*
** Unless an "EXPLAIN QUERY PLAN" command is being processed, this function
** is a no-op. Otherwise, it adds a single row of output to the EQP result,
** where the caption is of the form:
**
**   "USE TEMP B-TREE FOR xxx"
**
** where xxx is one of "DISTINCT", "ORDER BY" or "GROUP BY". Exactly which
** is determined by the usage argument.
*/
static void EXPLAIN_TEMP_TABLE(parser_t *parser, const char *usage){
  EXPLAIN_QUERY_PLAN((parser, 0, "USE TEMP B-TREE FOR %s", usage));
}

/*
** Assign expression b to lvalue a. A second, no-op, version of this macro
** is provided when DBSQL_OMIT_EXPLAIN is defined. This allows the code
** in __select() to assign values to structure member variables that
** only exist if DBSQL_OMIT_EXPLAIN is not defined without polluting the
** code with #ifndef directives.
*/
# define EXPLAIN_SET_INTEGER(a, b) a = b

#else
/* No-op versions of the explain_xxx() functions and macros. */
# define EXPLAIN_TEMP_TABLE(y,z)
# define EXPLAIN_SET_INTEGER(y,z)
#endif


/*
** If the inner loop was generated using a non-null order_by argument,
** then the results were placed in a sorter.  After the loop is terminated
** we need to run the sorter and output the results.  The following
** routine generates the code needed to do that.
*/
static void __generate_sort_tail(
  parser_t *parser,    /* Parsing context */
  select_t *p,        /* The SELECT statement */
  sort_ctx_t *sort,   /* Information on the ORDER BY clause */
  int n_column,      /* Number of columns of data */
  select_dest_t *p_dest /* Write the sorted results here */
){
  vdbe_t *v = parser->vdbe;                     /* The prepared statement */
  int addr_break = sort->label_done;            /* Jump here to exit loop */
  int addr_continue = __vdbe_make_label(parser);/* Jump here for next cycle */
  int addr;                       /* Top of output loop. Jump for Next. */
  int addr_once = 0;
  int i_tab;
  expr_list_t *order_by = sort->order_by;
  int dest = p_dest->dest;
  int i_parm = p_dest->i_sd_parm;
  int reg_row;
  int reg_rowid;
  int i_col;
  int n_key;                       /* Number of key columns in sorter record */
  int i_sort_tab;                   /* Sorter cursor to read from */
  int i;
  int seq;                       /* True if sorter record includes seq. no. */
  int n_ref_key = 0;
  struct expr_list_item_t *out_exes = p->e_list->a;
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
  int addr_explain;                /* Address of OP_Explain instruction */
#endif

  n_key = order_by->n_expr - sort->n_ob_sat;
  if( sort->n_ob_sat==0 || n_key==1 ){
    EXPLAIN_QUERY_PLAN2(addr_explain, (parser, 0,
      "USE TEMP B-TREE FOR %sORDER BY", sort->n_ob_sat?"LAST TERM OF ":""
    ));
  }else{
    EXPLAIN_QUERY_PLAN2(addr_explain, (parser, 0,
      "USE TEMP B-TREE FOR LAST %d TERMS OF ORDER BY", n_key
    ));
  }
  __vdbe_scan_status_range(v, addr_explain,sort->addr_push,sort->addr_push_end);
  __vdbe_scan_status_counters(v, addr_explain, addr_explain, sort->addr_push);


  assert( addr_break<0 );
  if( sort->label_bk_out ){
    __vdbe_add_op2(v, OP_Gosub, sort->reg_return, sort->label_bk_out);
    __vdbe_goto(v, addr_break);
    __vdbe_resolve_label(v, sort->label_bk_out);
  }

#ifdef DBSQL_ENABLE_SORTER_REFERENCES
  /* Open any cursors needed for sorter-reference expressions */
  for(i=0; i<sort->n_defer; i++){
    table_t *tab = sort->defers[i].tab;
    int i_db = __schema_to_index(parser->db, tab->schema);
    __open_table(parser, sort->defers[i].i_csr, i_db, tab, OP_OpenRead);
    n_ref_key = MAX(n_ref_key, sort->defers[i].n_key);
  }
#endif

  i_tab = sort->i_e_cursor;
  if( dest==SRT_Output || dest==SRT_Coroutine || dest==SRT_Mem ){
    if( dest==SRT_Mem && p->i_offset ){
      __vdbe_add_op2(v, OP_Null, 0, p_dest->i_sdst);
    }
    reg_rowid = 0;
    reg_row = p_dest->i_sdst;
  }else{
    reg_rowid = __get_temp_reg(parser);
    if( dest==SRT_EphemTab || dest==SRT_Table ){
      reg_row = __get_temp_reg(parser);
      n_column = 0;
    }else{
      reg_row = __get_temp_range(parser, n_column);
    }
  }
  if( sort->sort_flags & SORTFLAG_UseSorter ){
    int reg_sort_out = ++parser->n_mem;
    i_sort_tab = parser->n_tab++;
    if( sort->label_bk_out ){
      addr_once = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
    }
    __vdbe_add_op3(v, OP_OpenPseudo, i_sort_tab, reg_sort_out,
        n_key+1+n_column+n_ref_key);
    if( addr_once ) __vdbe_jump_here(v, addr_once);
    addr = 1 + __vdbe_add_op2(v, OP_SorterSort, i_tab, addr_break);
    VDBE_COVERAGE(v);
    assert( p->i_limit==0 && p->i_offset==0 );
    __vdbe_add_op3(v, OP_SorterData, i_tab, reg_sort_out, i_sort_tab);
    seq = 0;
  }else{
    addr = 1 + __vdbe_add_op2(v, OP_Sort, i_tab, addr_break); VDBE_COVERAGE(v);
    __code_offset(v, p->i_offset, addr_continue);
    i_sort_tab = i_tab;
    seq = 1;
    if( p->i_offset>0 ){
      __vdbe_add_op2(v, OP_AddImm, p->i_limit, -1);
    }
  }
  for(i=0, i_col=n_key+seq-1; i<n_column; i++){
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
    if( out_exes[i].fg.sorter_ref ) continue;
#endif
    if( out_exes[i].u.x.i_order_by_col==0 ) i_col++;
  }
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
  if( sort->n_defer ){
    int i_key = i_col+1;
    int reg_key = __get_temp_range(parser, n_ref_key);

    for(i=0; i<sort->n_defer; i++){
      int i_csr = sort->defers[i].i_csr;
      table_t *tab = sort->defers[i].tab;
      int n_key = sort->defers[i].n_key;

      __vdbe_add_op1(v, OP_NullRow, i_csr);
      if( HAS_ROWID(tab) ){
        __vdbe_add_op3(v, OP_Column, i_sort_tab, i_key++, reg_key);
        __vdbe_add_op3(v, OP_SeekRowid, i_csr,
            __vdbe_current_addr(v)+1, reg_key);
      }else{
        int k;
        int i_jmp;
        assert( __primary_key_index(tab)->n_key_col==n_key );
        for(k=0; k<n_key; k++){
          __vdbe_add_op3(v, OP_Column, i_sort_tab, i_key++, reg_key+k);
        }
        i_jmp = __vdbe_current_addr(v);
        __vdbe_add_op4_int(v, OP_SeekGE, i_csr, i_jmp+2, reg_key, n_key);
        __vdbe_add_op4_int(v, OP_IdxLE, i_csr, i_jmp+3, reg_key, n_key);
        __vdbe_add_op1(v, OP_NullRow, i_csr);
      }
    }
    __release_temp_range(parser, reg_key, n_ref_key);
  }
#endif
  for(i=n_column-1; i>=0; i--){
#ifdef DBSQL_ENABLE_SORTER_REFERENCES
    if( out_exes[i].fg.sorter_ref ){
      __expr_code(parser, out_exes[i].expr, reg_row+i);
    }else
#endif
    {
      int i_read;
      if( out_exes[i].u.x.i_order_by_col ){
        i_read = out_exes[i].u.x.i_order_by_col-1;
      }else{
        i_read = i_col--;
      }
      __vdbe_add_op3(v, OP_Column, i_sort_tab, i_read, reg_row+i);
      VDBE_COMMENT((v, "%s", out_exes[i].e_name));
    }
  }
  __vdbe_scan_status_range(v, addr_explain, addr_explain, -1);
  switch( dest ){
    case SRT_Table:
    case SRT_EphemTab: {
      __vdbe_add_op3(v, OP_Column, i_sort_tab, n_key+seq, reg_row);
      __vdbe_add_op2(v, OP_NewRowid, i_parm, reg_rowid);
      __vdbe_add_op3(v, OP_Insert, i_parm, reg_row, reg_rowid);
      __vdbe_change_p5(v, OPFLAG_APPEND);
      break;
    }
#ifndef DBSQL_OMIT_SUBQUERY
    case SRT_Set: {
      assert( n_column==__strlen30(p_dest->aff_sdst) );
      __vdbe_add_op4(v, OP_MakeRecord, reg_row, n_column, reg_rowid,
                        p_dest->aff_sdst, n_column);
      __vdbe_add_op4_int(v, OP_IdxInsert, i_parm, reg_rowid, reg_row, n_column);
      break;
    }
    case SRT_Mem: {
      /* The LIMIT clause will terminate the loop for us */
      break;
    }
#endif
    case SRT_Upfrom: {
      int i2 = p_dest->i_sd_parm2;
      int r1 = __get_temp_reg(parser);
      __vdbe_add_op3(v, OP_MakeRecord,reg_row+(i2<0),n_column-(i2<0),r1);
      if( i2<0 ){
        __vdbe_add_op3(v, OP_Insert, i_parm, r1, reg_row);
      }else{
        __vdbe_add_op4_int(v, OP_IdxInsert, i_parm, r1, reg_row, i2);
      }
      break;
    }
    default: {
      assert( dest==SRT_Output || dest==SRT_Coroutine );
      testcase( dest==SRT_Output );
      testcase( dest==SRT_Coroutine );
      if( dest==SRT_Output ){
        __vdbe_add_op2(v, OP_ResultRow, p_dest->i_sdst, n_column);
      }else{
        __vdbe_add_op1(v, OP_Yield, p_dest->i_sd_parm);
      }
      break;
    }
  }
  if( reg_rowid ){
    if( dest==SRT_Set ){
      __release_temp_range(parser, reg_row, n_column);
    }else{
      __release_temp_reg(parser, reg_row);
    }
    __release_temp_reg(parser, reg_rowid);
  }
  /* The bottom of the loop
  */
  __vdbe_resolve_label(v, addr_continue);
  if( sort->sort_flags & SORTFLAG_UseSorter ){
    __vdbe_add_op2(v, OP_SorterNext, i_tab, addr); VDBE_COVERAGE(v);
  }else{
    __vdbe_add_op2(v, OP_Next, i_tab, addr); VDBE_COVERAGE(v);
  }
  __vdbe_scan_status_range(v, addr_explain, __vdbe_current_addr(v)-1, -1);
  if( sort->reg_return ) __vdbe_add_op1(v, OP_Return, sort->reg_return);
  __vdbe_resolve_label(v, addr_break);
}

/*
** Return a pointer to a string containing the 'declaration type' of the
** expression expr. The string may be treated as static by the caller.
**
** The declaration type is the exact datatype definition extracted from the
** original CREATE TABLE statement if the expression is a column. The
** declaration type for a ROWID field is INTEGER. Exactly when an expression
** is considered a column can be complex in the presence of subqueries. The
** result-set expression in all of the following SELECT statements is
** considered a column by this function.
**
**   SELECT col FROM tbl;
**   SELECT (SELECT col FROM tbl;
**   SELECT (SELECT col FROM tbl);
**   SELECT abc FROM (SELECT col AS abc FROM tbl);
**
** The declaration type for any expression other than a column is NULL.
**
** This routine has either 3 or 6 parameters depending on whether or not
** the DBSQL_ENABLE_COLUMN_METADATA compile-time option is used.
*/
#ifdef DBSQL_ENABLE_COLUMN_METADATA
# define COLUMN_TYPE(A,B,C,D,E) __column_type_impl(A,B,C,D,E)
#else /* if !defined(DBSQL_ENABLE_COLUMN_METADATA) */
# define COLUMN_TYPE(A,B,C,D,E) __column_type_impl(A,B)
#endif
static const char *__column_type_impl(
  name_context_t *nc,
#ifndef DBSQL_ENABLE_COLUMN_METADATA
  expr_t *expr
#else
  expr_t *expr,
  const char **pz_orig_db,
  const char **pz_orig_tab,
  const char **pz_orig_col
#endif
){
  char const *type = 0;
  int j;
#ifdef DBSQL_ENABLE_COLUMN_METADATA
  char const *orig_db = 0;
  char const *orig_tab = 0;
  char const *orig_col = 0;
#endif

  assert( expr!=0 );
  assert( nc->src_list!=0 );
  switch( expr->op ){
    case TK_COLUMN: {
      /* The expression is a column. Locate the table the column is being
      ** extracted from in name_context_t.src_list. This table may be real
      ** database table or a subquery.
      */
      table_t *tab = 0;            /* table_t structure column is extracted from */
      select_t *s = 0;             /* select_t the column is extracted from */
      int i_col = expr->i_column;  /* index_t of column in tab */
      while( nc && !tab ){
        src_list_t *tab_list = nc->src_list;
        for(j=0;j<tab_list->n_src && tab_list->a[j].i_cursor!=expr->i_table;j++);
        if( j<tab_list->n_src ){
          tab = tab_list->a[j].s_tab;
          if( tab_list->a[j].fg.is_subquery ){
            s = tab_list->a[j].u4.subq->p_select;
          }else{
            s = 0;
          }
        }else{
          nc = nc->next;
        }
      }

      if( tab==0 ){
        /* At one time, code such as "SELECT new.x" within a trigger would
        ** cause this condition to run.  Since then, we have restructured how
        ** trigger code is generated and so this condition is no longer
        ** possible. However, it can still be true for statements like
        ** the following:
        **
        **   CREATE TABLE t1(col INTEGER);
        **   SELECT (SELECT t1.col) FROM FROM t1;
        **
        ** when COLUMN_TYPE() is called on the expression "t1.col" in the
        ** sub-select. In this case, set the column type to NULL, even
        ** though it should really be "INTEGER".
        **
        ** This is not a problem, as the column type of "t1.col" is never
        ** used. When COLUMN_TYPE() is called on the expression
        ** "(SELECT t1.col)", the correct type is returned (see the TK_SELECT
        ** branch below.  */
        break;
      }

      assert( tab && EXPR_USE_Y_TAB(expr) && expr->y.tab==tab );
      if( s ){
        /* The "table" is actually a sub-select or a view in the FROM clause
        ** of the SELECT statement. Return the declaration type and origin
        ** data for the result-set column of the sub-select.
        */
        if( i_col<s->e_list->n_expr
         && (!VIEW_CAN_HAVE_ROWID || i_col>=0)
        ){
          /* If i_col is less than zero, then the expression requests the
          ** rowid of the sub-select or view. This expression is legal (see
          ** test case misc2.2.2) - it always evaluates to NULL.
          */
          name_context_t s_nc;
          expr_t *p = s->e_list->a[i_col].expr;
          s_nc.src_list = s->src;
          s_nc.next = nc;
          s_nc.parser = nc->parser;
          type = COLUMN_TYPE(&s_nc, p,&orig_db,&orig_tab,&orig_col);
        }
      }else{
        /* A real table or a CTE table */
        assert( !s );
#ifdef DBSQL_ENABLE_COLUMN_METADATA
        if( i_col<0 ) i_col = tab->i_p_key;
        assert( i_col==XN_ROWID || (i_col>=0 && i_col<tab->n_col) );
        if( i_col<0 ){
          type = "INTEGER";
          orig_col = "rowid";
        }else{
          orig_col = tab->cols[i_col].cn_name;
          type = __column_type(&tab->cols[i_col],0);
        }
        orig_tab = tab->name;
        if( nc->parser && tab->schema ){
          int i_db = __schema_to_index(nc->parser->db, tab->schema);
          orig_db = nc->parser->db->dbs[i_db].db_s_name;
        }
#else
        assert( i_col==XN_ROWID || (i_col>=0 && i_col<tab->n_col) );
        if( i_col<0 ){
          type = "INTEGER";
        }else{
          type = __column_type(&tab->cols[i_col],0);
        }
#endif
      }
      break;
    }
#ifndef DBSQL_OMIT_SUBQUERY
    case TK_SELECT: {
      /* The expression is a sub-select. Return the declaration type and
      ** origin info for the single column in the result set of the SELECT
      ** statement.
      */
      name_context_t s_nc;
      select_t *s;
      expr_t *p;
      assert( EXPR_USE_X_SELECT(expr) );
      s = expr->x.p_select;
      p = s->e_list->a[0].expr;
      s_nc.src_list = s->src;
      s_nc.next = nc;
      s_nc.parser = nc->parser;
      type = COLUMN_TYPE(&s_nc, p, &orig_db, &orig_tab, &orig_col);
      break;
    }
#endif
  }

#ifdef DBSQL_ENABLE_COLUMN_METADATA 
  if( pz_orig_db ){
    assert( pz_orig_tab && pz_orig_col );
    *pz_orig_db = orig_db;
    *pz_orig_tab = orig_tab;
    *pz_orig_col = orig_col;
  }
#endif
  return type;
}

/*
** Generate code that will tell the VDBE the declaration types of columns
** in the result set.
*/
static void __generate_column_types(
  parser_t *parser,      /* Parser context */
  src_list_t *tab_list,  /* List of tables */
  expr_list_t *e_list    /* Expressions defining the result set */
){
#ifndef DBSQL_OMIT_DECLTYPE
  vdbe_t *v = parser->vdbe;
  int i;
  name_context_t s_nc;
  s_nc.src_list = tab_list;
  s_nc.parser = parser;
  s_nc.next = 0;
  for(i=0; i<e_list->n_expr; i++){
    expr_t *p = e_list->a[i].expr;
    const char *type;
#ifdef DBSQL_ENABLE_COLUMN_METADATA
    const char *orig_db = 0;
    const char *orig_tab = 0;
    const char *orig_col = 0;
    type = COLUMN_TYPE(&s_nc, p, &orig_db, &orig_tab, &orig_col);

    /* The vdbe must make its own copy of the column-type and other
    ** column specific strings, in case the schema is reset before this
    ** virtual machine is deleted.
    */
    __vdbe_set_col_name(v, i, COLNAME_DATABASE, orig_db, DBSQL_TRANSIENT);
    __vdbe_set_col_name(v, i, COLNAME_TABLE, orig_tab, DBSQL_TRANSIENT);
    __vdbe_set_col_name(v, i, COLNAME_COLUMN, orig_col, DBSQL_TRANSIENT);
#else
    type = COLUMN_TYPE(&s_nc, p, 0, 0, 0);
#endif
    __vdbe_set_col_name(v, i, COLNAME_DECLTYPE, type, DBSQL_TRANSIENT);
  }
#else
  UNUSED_PARAMETER(parser);
  UNUSED_PARAMETER(tab_list);
  UNUSED_PARAMETER(e_list);
#endif /* !defined(DBSQL_OMIT_DECLTYPE) */
}


/*
** Compute the column names for a SELECT statement.
**
** The only guarantee that dbsql makes about column names is that if the
** column has an AS clause assigning it a name, that will be the name used.
** That is the only documented guarantee.  However, countless applications
** developed over the years have made baseless assumptions about column names
** and will break if those assumptions changes.  Hence, use extreme caution
** when modifying this routine to avoid breaking legacy.
**
** See Also: __columns_from_expr_list()
**
** The PRAGMA short_column_names and PRAGMA full_column_names settings are
** deprecated.  The default setting is short=ON, full=OFF.  99.9% of all
** applications should operate this way.  Nevertheless, we need to support the
** other modes for legacy:
**
**    short=OFF, full=OFF:      column_t name is the text of the expression has it
**                              originally appears in the SELECT statement.  In
**                              other words, the span of the result expression.
**
**    short=ON, full=OFF:       (This is the default setting).  If the result
**                              refers directly to a table column, then the
**                              result column name is just the table column
**                              name: COLUMN.  Otherwise use span.
**
**    full=ON, short=ANY:       If the result refers directly to a table column,
**                              then the result column name with the table name
**                              prefix, ex: TABLE.COLUMN.  Otherwise use span.
*/
void __generate_column_names(
  parser_t *parser,      /* Parser context */
  select_t *p_select     /* Generate column names for this SELECT statement */
){
  vdbe_t *v = parser->vdbe;
  int i;
  table_t *tab;
  src_list_t *tab_list;
  expr_list_t *e_list;
  DBSQL *db = parser->db;
  int fullName;    /* TABLE.COLUMN if no AS clause and is a direct table ref */
  int src_name;     /* COLUMN or TABLE.COLUMN if no AS clause and is direct */

  if( parser->col_names_set ) return;
  /* column_t names are determined by the left-most term of a compound select */
  while( p_select->prior ) p_select = p_select->prior;
  TREETRACE(0x80,parser,p_select,("generating column names\n"));
  tab_list = p_select->src;
  e_list = p_select->e_list;
  assert( v!=0 );
  assert( tab_list!=0 );
  parser->col_names_set = 1;
  fullName = (db->flags & DBSQL_FullColNames)!=0;
  src_name = (db->flags & DBSQL_ShortColNames)!=0 || fullName;
  __vdbe_set_num_cols(v, e_list->n_expr);
  for(i=0; i<e_list->n_expr; i++){
    expr_t *p = e_list->a[i].expr;

    assert( p!=0 );
    assert( p->op!=TK_AGG_COLUMN );  /* Agg processing has not run yet */
    assert( p->op!=TK_COLUMN
        || (EXPR_USE_Y_TAB(p) && p->y.tab!=0) ); /* Covering idx not yet coded */
    if( e_list->a[i].e_name && e_list->a[i].fg.e_name==ENAME_NAME ){
      /* An AS clause always takes first priority */
      char *name = e_list->a[i].e_name;
      __vdbe_set_col_name(v, i, COLNAME_NAME, name, DBSQL_TRANSIENT);
    }else if( src_name && p->op==TK_COLUMN ){
      char *col;
      int i_col = p->i_column;
      tab = p->y.tab;
      assert( tab!=0 );
      if( i_col<0 ) i_col = tab->i_p_key;
      assert( i_col==-1 || (i_col>=0 && i_col<tab->n_col) );
      if( i_col<0 ){
        col = "rowid";
      }else{
        col = tab->cols[i_col].cn_name;
      }
      if( fullName ){
        char *name = 0;
        name = __m_printf(db, "%s.%s", tab->name, col);
        __vdbe_set_col_name(v, i, COLNAME_NAME, name, DBSQL_DYNAMIC);
      }else{
        __vdbe_set_col_name(v, i, COLNAME_NAME, col, DBSQL_TRANSIENT);
      }
    }else{
      const char *z = e_list->a[i].e_name;
      z = z==0 ? __m_printf(db, "column%d", i+1) : __db_str_dup(db, z);
      __vdbe_set_col_name(v, i, COLNAME_NAME, z, DBSQL_DYNAMIC);
    }
  }
  __generate_column_types(parser, tab_list, e_list);
}

/*
** Given an expression list (which is really the list of expressions
** that form the result set of a SELECT statement) compute appropriate
** column names for a table that would hold the expression list.
**
** All column names will be unique.
**
** Only the column names are computed.  column_t.type, column_t.coll,
** and other fields of column_t are zeroed.
**
** Return DBSQL_OK on success.  If a memory allocation error occurs,
** store NULL in *pa_col and 0 in *pn_col and return DBSQL_NOMEM.
**
** The only guarantee that dbsql makes about column names is that if the
** column has an AS clause assigning it a name, that will be the name used.
** That is the only documented guarantee.  However, countless applications
** developed over the years have made baseless assumptions about column names
** and will break if those assumptions changes.  Hence, use extreme caution
** when modifying this routine to avoid breaking legacy.
**
** See Also: __generate_column_names()
*/
int __columns_from_expr_list(
  parser_t *parser,          /* Parsing context */
  expr_list_t *e_list,       /* expr_t list from which to derive column names */
  int16_t *pn_col,             /* Write the number of columns here */
  column_t **pa_col          /* Write the new column list here */
){
  DBSQL *db = parser->db;   /* Database connection */
  int i, j;                   /* Loop counters */
  u_int32_t cnt;                    /* index_t added to make the name unique */
  column_t *cols, *col;        /* For looping over result columns */
  int n_col;                   /* Number of columns in the result set */
  char *name;                /* column_t name */
  int n_name;                  /* Size of name in name[] */
  hash_t ht;                    /* hash_t table of column names */
  table_t *tab;

  __hash_init(&ht);
  if( e_list ){
    n_col = e_list->n_expr;
    cols = __db_malloc_zero(db, sizeof(cols[0])*n_col);
    testcase( cols==0 );
    if( NEVER(n_col>32767) ) n_col = 32767;
  }else{
    n_col = 0;
    cols = 0;
  }
  assert( n_col==(int16_t)n_col );
  *pn_col = n_col;
  *pa_col = cols;

  for(i=0, col=cols; i<n_col && !parser->n_err; i++, col++){
    struct expr_list_item_t *x = &e_list->a[i];
    struct expr_list_item_t *collide;
    /* Get an appropriate name for the column
    */
    if( (name = x->e_name)!=0 && x->fg.e_name==ENAME_NAME ){
      /* If the column contains an "AS <name>" phrase, use <name> as the name */
    }else{
      expr_t *col_expr = __expr_skip_collate_and_likely(x->expr);
      while( ALWAYS(col_expr!=0) && col_expr->op==TK_DOT ){
        col_expr = col_expr->right;
        assert( col_expr!=0 );
      }
      if( col_expr->op==TK_COLUMN
       && ALWAYS( EXPR_USE_Y_TAB(col_expr) )
       && ALWAYS( col_expr->y.tab!=0 )
      ){
        /* For columns use the column name name */
        int i_col = col_expr->i_column;
        tab = col_expr->y.tab;
        if( i_col<0 ) i_col = tab->i_p_key;
        name = i_col>=0 ? tab->cols[i_col].cn_name : "rowid";
      }else if( col_expr->op==TK_ID ){
        assert( !EXPR_HAS_PROPERTY(col_expr, EP_IntValue) );
        name = col_expr->u.token;
      }else{
        /* Use the original text of the column expression as its name */
        assert( name==x->e_name );  /* pointer comparison intended */
      }
    }
    if( name && !__is_true_or_false(name) ){
      name = __db_str_dup(db, name);
    }else{
      name = __m_printf(db,"column%d",i+1);
    }

    /* Make sure the column name is unique.  If the name is not unique,
    ** append an integer to the name so that it becomes unique.
    */
    cnt = 0;
    while( name && (collide = __hash_find(&ht, name))!=0 ){
      if( collide->fg.using_term ){
        col->col_flags |= COLFLAG_NOEXPAND;
      }
      n_name = __strlen30(name);
      if( n_name>0 ){
        for(j=n_name-1; j>0 && __isdigit(name[j]); j--){}
        if( name[j]==':' ) n_name = j;
      }
      name = __m_printf(db, "%.*z:%u", n_name, name, ++cnt);
      __progress_check(parser);
      if( cnt>3 ){
        dbsql_randomness(sizeof(cnt), &cnt);
      }
    }
    col->cn_name = name;
    col->h_name = __str_i_hash(name);
    if( x->fg.no_expand ){
      col->col_flags |= COLFLAG_NOEXPAND;
    }
    __column_properties_from_name(0, col);
    if( name && __hash_insert(&ht, name, x)==x ){
      __oom_fault(db);
    }
  }
  __hash_clear(&ht);
  if( parser->n_err ){
    for(j=0; j<i; j++){
      __dbsql_db_free(db, cols[j].cn_name);
    }
    __dbsql_db_free(db, cols);
    *pa_col = 0;
    *pn_col = 0;
    return parser->rc;
  }
  return DBSQL_OK;
}

/*
** tab is a transient table_t object that represents a subquery of some
** kind (maybe a parenthesized subquery in the FROM clause of a larger
** query, or a VIEW, or a CTE).  This routine computes type information
** for that table_t object based on the select_t object that implements the
** subquery.  For the purposes of this routine, "type information" means:
**
**    *   The datatype name, as it might appear in a CREATE TABLE statement
**    *   Which collating sequence to use for the column
**    *   The affinity of the column
*/
void __subquery_column_types(
  parser_t *parser,      /* Parsing contexts */
  table_t *tab,        /* Add column type information to this table */
  select_t *p_select,    /* SELECT used to determine types and collations */
  char aff            /* Default affinity. */
){
  DBSQL *db = parser->db;
  column_t *col;
  coll_seq_t *coll;
  int i,j;
  expr_t *p;
  struct expr_list_item_t *a;
  name_context_t s_nc;

  assert( p_select!=0 );
  assert( (p_select->sel_flags & SF_Resolved)!=0 );
  assert( tab->n_col==p_select->e_list->n_expr || parser->n_err>0 );
  assert( aff==DBSQL_AFF_NONE || aff==DBSQL_AFF_BLOB );
  if( db->malloc_failed || IN_RENAME_OBJECT ) return;
  while( p_select->prior ) p_select = p_select->prior;
  a = p_select->e_list->a;
  memset(&s_nc, 0, sizeof(s_nc));
  s_nc.src_list = p_select->src;
  for(i=0, col=tab->cols; i<tab->n_col; i++, col++){
    const char *type;
    int64_t n;
    int m = 0;
    select_t *s2 = p_select;
    tab->tab_flags |= (col->col_flags & COLFLAG_NOINSERT);
    p = a[i].expr;
    /* col->sz_est = ... // column_t size est for SELECT tables never used */
    col->affinity = __expr_affinity(p);
    while( col->affinity<=DBSQL_AFF_NONE && s2->next!=0 ){
      m |= __expr_data_type(s2->e_list->a[i].expr);
      s2 = s2->next;
      col->affinity = __expr_affinity(s2->e_list->a[i].expr);
    }
    if( col->affinity<=DBSQL_AFF_NONE ){
      col->affinity = aff;
    }
    if( col->affinity>=DBSQL_AFF_TEXT && (s2->next || s2!=p_select) ){
      for(s2=s2->next; s2; s2=s2->next){
        m |= __expr_data_type(s2->e_list->a[i].expr);
      }
      if( col->affinity==DBSQL_AFF_TEXT && (m&0x01)!=0 ){
        col->affinity = DBSQL_AFF_BLOB;
      }else
      if( col->affinity>=DBSQL_AFF_NUMERIC && (m&0x02)!=0 ){
        col->affinity = DBSQL_AFF_BLOB;
      }
      if( col->affinity>=DBSQL_AFF_NUMERIC && p->op==TK_CAST ){
        col->affinity = DBSQL_AFF_FLEXNUM;
      }
    }
    type = COLUMN_TYPE(&s_nc, p, 0, 0, 0);
    if( type==0 || col->affinity!=__affinity_type(type, 0) ){
      if( col->affinity==DBSQL_AFF_NUMERIC
       || col->affinity==DBSQL_AFF_FLEXNUM
      ){
        type = "NUM";
      }else{
        type = 0;
        for(j=1; j<DBSQL_N_STDTYPE; j++){
          if( __std_type_affinity[j]==col->affinity ){
            type = __std_type[j];
            break;
          }
        }
      }
    }
    if( type ){
      const int64_t k = strlen(type);
      n = strlen(col->cn_name);
      col->cn_name = __db_realloc_or_free(db, col->cn_name, n+k+2);
      col->col_flags &= ~(COLFLAG_HASTYPE|COLFLAG_HASCOLL);
      if( col->cn_name ){
        memcpy(&col->cn_name[n+1], type, k+1);
        col->col_flags |= COLFLAG_HASTYPE;
      }
    }
    coll = __expr_coll_seq(parser, p);
    if( coll ){
      assert( tab->p_index==0 );
      __column_set_coll(db, col, coll->name);
    }
  }
  tab->sz_tab_row = 1; /* Any non-zero value works */
}

/*
** Given a SELECT statement, generate a table_t structure that describes
** the result set of that SELECT.
*/
table_t *__result_set_of_select(parser_t *parser, select_t *p_select, char aff){
  table_t *tab;
  DBSQL *db = parser->db;
  u_int64_t saved_flags;

  parser->n_nest_sel++;
#if DBSQL_MAX_EXPR_DEPTH>0
  if( parser->n_nest_sel >= db->limits[DBSQL_LIMIT_EXPR_DEPTH] ){
    __error_msg(parser, "VIEWs and/or subqueries nested too deep");
    return 0;
  }
#endif
  saved_flags = db->flags;
  db->flags &= ~(u_int64_t)DBSQL_FullColNames;
  db->flags |= DBSQL_ShortColNames;
  __select_prep(parser, p_select, 0);
  db->flags = saved_flags;
  if( parser->n_err ) return 0;
  while( p_select->prior ) p_select = p_select->prior;
  tab = __db_malloc_zero(db, sizeof(table_t) );
  if( tab==0 ){
    return 0;
  }
  tab->n_tab_ref = 1;
  tab->name = 0;
  tab->n_row_log_est = 200; assert( 200==__log_est(1048576) );
  __columns_from_expr_list(parser, p_select->e_list, &tab->n_col, &tab->cols);
  __subquery_column_types(parser, tab, p_select, aff);
  tab->i_p_key = -1;
  if( db->malloc_failed ){
    __delete_table(db, tab);
    return 0;
  }
  parser->n_nest_sel--;
  assert( parser->n_nest_sel>=0 );
  return tab;
}

/*
** Get a VDBE for the given parser context.  Create a new one if necessary.
** If an error occurs, return NULL and leave a message in parser.
*/
vdbe_t *__get_vdbe(parser_t *parser){
  if( parser->vdbe ){
    return parser->vdbe;
  }
  if( parser->toplevel==0
   && OPTIMIZATION_ENABLED(parser->db,DBSQL_FactorOutConst)
  ){
    parser->ok_const_factor = 1;
  }
  return __vdbe_create(parser);
}


/*
** Compute the i_limit and i_offset fields of the SELECT based on the
** limit expressions.  limit->left and limit->right hold the expressions
** that appear in the original SQL statement after the LIMIT and OFFSET
** keywords.  Or NULL if those keywords are omitted. i_limit and i_offset
** are the integer memory register numbers for counters used to compute
** the limit and offset.  If there is no limit and/or offset, then
** i_limit and i_offset are negative.
**
** This routine changes the values of i_limit and i_offset only if
** a limit or offset is defined by limit->left and limit->right.  i_limit
** and i_offset should have been preset to appropriate default values (zero)
** prior to calling this routine.
**
** The i_offset register (if it exists) is initialized to the value
** of the OFFSET.  The i_limit register is initialized to LIMIT.  Register
** i_offset+1 is initialized to LIMIT+OFFSET.
**
** Only if limit->left!=0 do the limit registers get
** redefined.  The UNION ALL operator uses this property to force
** the reuse of the same limit and offset registers across multiple
** SELECT statements.
*/
static void __compute_limit_registers(parser_t *parser, select_t *p, int i_break){
  vdbe_t *v = 0;
  int i_limit = 0;
  int i_offset;
  int n;
  expr_t *limit = p->limit;

  if( p->i_limit ) return;

  /*
  ** "LIMIT -1" always shows all rows.  There is some
  ** controversy about what the correct behavior should be.
  ** The current implementation interprets "LIMIT 0" to mean
  ** no rows.
  */
  if( limit ){
    assert( limit->op==TK_LIMIT );
    assert( limit->left!=0 );
    p->i_limit = i_limit = ++parser->n_mem;
    v = __get_vdbe(parser);
    assert( v!=0 );
    if( __expr_is_integer(limit->left, &n, parser) ){
      __vdbe_add_op2(v, OP_Integer, n, i_limit);
      VDBE_COMMENT((v, "LIMIT counter"));
      if( n==0 ){
        __vdbe_goto(v, i_break);
      }else if( n>=0 && p->n_select_row>__log_est((u_int64_t)n) ){
        p->n_select_row = __log_est((u_int64_t)n);
        p->sel_flags |= SF_FixedLimit;
      }
    }else{
      __expr_code(parser, limit->left, i_limit);
      __vdbe_add_op1(v, OP_MustBeInt, i_limit); VDBE_COVERAGE(v);
      VDBE_COMMENT((v, "LIMIT counter"));
      __vdbe_add_op2(v, OP_IfNot, i_limit, i_break); VDBE_COVERAGE(v);
    }
    if( limit->right ){
      p->i_offset = i_offset = ++parser->n_mem;
      parser->n_mem++;   /* Allocate an extra register for limit+offset */
      __expr_code(parser, limit->right, i_offset);
      __vdbe_add_op1(v, OP_MustBeInt, i_offset); VDBE_COVERAGE(v);
      VDBE_COMMENT((v, "OFFSET counter"));
      __vdbe_add_op3(v, OP_OffsetLimit, i_limit, i_offset+1, i_offset);
      VDBE_COMMENT((v, "LIMIT+OFFSET"));
    }
  }
}

#ifndef DBSQL_OMIT_COMPOUND_SELECT
/*
** Return the appropriate collating sequence for the i_col-th column of
** the result set for the compound-select statement "p".  Return NULL if
** the column has no default collating sequence.
**
** The collating sequence for the compound select is taken from the
** left-most term of the select that has a collating sequence.
*/
static coll_seq_t *__multi_select_coll_seq(parser_t *parser, select_t *p, int i_col){
  coll_seq_t *ret;
  if( p->prior ){
    ret = __multi_select_coll_seq(parser, p->prior, i_col);
  }else{
    ret = 0;
  }
  assert( i_col>=0 );
  /* i_col must be less than p->e_list->n_expr.  Otherwise an error would
  ** have been thrown during name resolution and we would not have gotten
  ** this far */
  if( ret==0 && ALWAYS(i_col<p->e_list->n_expr) ){
    ret = __expr_coll_seq(parser, p->e_list->a[i_col].expr);
  }
  return ret;
}

/*
** The select statement passed as the second parameter is a compound SELECT
** with an ORDER BY clause. This function allocates and returns a key_info_t
** structure suitable for implementing the ORDER BY.
**
** Space to hold the key_info_t structure is obtained from malloc. The calling
** function is responsible for ensuring that this structure is eventually
** freed.
*/
static key_info_t *__multi_select_by_merge_key_info(parser_t *parser, select_t *p, int n_extra){
  expr_list_t *order_by = p->order_by;
  int n_order_by = (order_by!=0) ? order_by->n_expr : 0;
  DBSQL *db = parser->db;
  key_info_t *ret = __key_info_alloc(db, n_order_by+n_extra, 1);
  if( ret ){
    int i;
    for(i=0; i<n_order_by; i++){
      struct expr_list_item_t *item = &order_by->a[i];
      expr_t *term = item->expr;
      coll_seq_t *coll;

      if( term->flags & EP_Collate ){
        coll = __expr_coll_seq(parser, term);
      }else{
        coll = __multi_select_coll_seq(parser, p, item->u.x.i_order_by_col-1);
        if( coll==0 ) coll = db->dflt_coll;
        order_by->a[i].expr =
          __expr_add_collate_string(parser, term, coll->name);
      }
      assert( __key_info_is_writeable(ret) );
      ret->colls[i] = coll;
      ret->sort_flags[i] = order_by->a[i].fg.sort_flags;
    }
  }

  return ret;
}

#ifndef DBSQL_OMIT_CTE
/*
** This routine generates VDBE code to compute the content of a WITH RECURSIVE
** query of the form:
**
**   <recursive-table> AS (<setup-query> UNION [ALL] <recursive-query>)
**                         \___________/             \_______________/
**                           p->prior                      p
**
**
** There is exactly one reference to the recursive-table in the FROM clause
** of recursive-query, marked with the src_list_t->a[].fg.is_recursive flag.
**
** The setup-query runs once to generate an initial set of rows that go
** into a Queue table.  Rows are extracted from the Queue table one by
** one.  Each row extracted from Queue is output to dest.  Then the single
** extracted row (now in the i_current table) becomes the content of the
** recursive-table for a recursive-query run.  The output of the recursive-query
** is added back into the Queue table.  Then another row is extracted from Queue
** and the iteration continues until the Queue table is empty.
**
** If the compound query operator is UNION then no duplicate rows are ever
** inserted into the Queue table.  The i_distinct table keeps a copy of all rows
** that have ever been inserted into Queue and causes duplicates to be
** discarded.  If the operator is UNION ALL, then duplicates are allowed.
**
** If the query has an ORDER BY, then entries in the Queue table are kept in
** ORDER BY order and the first entry is extracted for each cycle.  Without
** an ORDER BY, the Queue table is just a FIFO.
**
** If a LIMIT clause is provided, then the iteration stops after LIMIT rows
** have been output to dest.  A LIMIT of zero means to output no rows and a
** negative LIMIT means to output all rows.  If there is also an OFFSET clause
** with a positive value, then the first OFFSET outputs are discarded rather
** than being sent to dest.  The LIMIT count does not begin until after OFFSET
** rows have been skipped.
*/
static void __generate_with_recursive_query(
  parser_t *parser,        /* Parsing context */
  select_t *p,            /* The recursive SELECT to be coded */
  select_dest_t *p_dest     /* What to do with query results */
){
  src_list_t *src = p->src;      /* The FROM clause of the recursive query */
  int n_col = p->e_list->n_expr;  /* Number of columns in the recursive table */
  vdbe_t *v = parser->vdbe;      /* The prepared statement under construction */
  select_t *setup;               /* The setup query */
  select_t *first_rec;            /* Left-most recursive term */
  int addr_top;                  /* Top of the loop */
  int addr_cont, addr_break;      /* CONTINUE and BREAK addresses */
  int i_current = 0;             /* The Current table */
  int reg_current;               /* Register holding Current table */
  int i_queue;                   /* The Queue table */
  int i_distinct = 0;            /* To ensure unique results if UNION */
  int dest = SRT_Fifo;         /* How to write to Queue */
  select_dest_t dest_queue;         /* select_dest_t targeting the Queue table */
  int i;                        /* Loop counter */
  int rc;                       /* Result code */
  expr_list_t *order_by;           /* The ORDER BY clause */
  expr_t *limit;                 /* Saved LIMIT and OFFSET */
  int reg_limit, reg_offset;      /* Registers used by LIMIT and OFFSET */

#ifndef DBSQL_OMIT_WINDOWFUNC
  if( p->win ){
    __error_msg(parser, "cannot use window functions in recursive queries");
    return;
  }
#endif

  /* Obtain authorization to do a recursive query */
  if( __auth_check(parser, DBSQL_RECURSIVE, 0, 0, 0) ) return;

  /* Process the LIMIT and OFFSET clauses, if they exist */
  addr_break = __vdbe_make_label(parser);
  p->n_select_row = 320;  /* 4 billion rows */
  __compute_limit_registers(parser, p, addr_break);
  limit = p->limit;
  reg_limit = p->i_limit;
  reg_offset = p->i_offset;
  p->limit = 0;
  p->i_limit = p->i_offset = 0;
  order_by = p->order_by;

  /* Locate the cursor number of the Current table */
  for(i=0; ALWAYS(i<src->n_src); i++){
    if( src->a[i].fg.is_recursive ){
      i_current = src->a[i].i_cursor;
      break;
    }
  }

  /* Allocate cursors numbers for Queue and Distinct.  The cursor number for
  ** the Distinct table must be exactly one greater than Queue in order
  ** for the SRT_DistFifo and SRT_DistQueue destinations to work. */
  i_queue = parser->n_tab++;
  if( p->op==TK_UNION ){
    dest = order_by ? SRT_DistQueue : SRT_DistFifo;
    i_distinct = parser->n_tab++;
  }else{
    dest = order_by ? SRT_Queue : SRT_Fifo;
  }
  __select_dest_init(&dest_queue, dest, i_queue);

  /* Allocate cursors for Current, Queue, and Distinct. */
  reg_current = ++parser->n_mem;
  __vdbe_add_op3(v, OP_OpenPseudo, i_current, reg_current, n_col);
  if( order_by ){
    key_info_t *key_info = __multi_select_by_merge_key_info(parser, p, 1);
    __vdbe_add_op4(v, OP_OpenEphemeral, i_queue, order_by->n_expr+2, 0,
                      (char*)key_info, P4_KEYINFO);
    dest_queue.order_by = order_by;
  }else{
    __vdbe_add_op2(v, OP_OpenEphemeral, i_queue, n_col);
  }
  VDBE_COMMENT((v, "Queue table"));
  if( i_distinct ){
    /* Generate an ephemeral table used to enforce distinctness on the
    ** output of the recursive part of the CTE.
    */
    key_info_t *key_info;            /* Collating sequence for the result set */
    coll_seq_t **colls;             /* For looping through key_info->colls[] */

    assert( p->next==0 );
    assert( p->e_list!=0 );
    n_col = p->e_list->n_expr;
    key_info = __key_info_alloc(parser->db, n_col, 1);
    if( key_info ){
      for(i=0, colls=key_info->colls; i<n_col; i++, colls++){
        *colls = __multi_select_coll_seq(parser, p, i);
        if( 0==*colls ){
          *colls = parser->db->dflt_coll;
        }
      }
      __vdbe_add_op4(v, OP_OpenEphemeral, i_distinct, n_col, 0,
                           (void*)key_info, P4_KEYINFO);
    }else{
      assert( parser->n_err>0 );
    }
  }

  /* Detach the ORDER BY clause from the compound SELECT */
  p->order_by = 0;

  /* Figure out how many elements of the compound SELECT are part of the
  ** recursive query.  Make sure no recursive elements use aggregate
  ** functions.  Mark the recursive elements as UNION ALL even if they
  ** are really UNION because the distinctness will be enforced by the
  ** i_distinct table.  first_rec is left pointing to the left-most
  ** recursive term of the CTE.
  */
  for(first_rec=p; ALWAYS(first_rec!=0); first_rec=first_rec->prior){
    if( first_rec->sel_flags & SF_Aggregate ){
      __error_msg(parser, "recursive aggregate queries not supported");
      goto end_of_recursive_query;
    }
    first_rec->op = TK_ALL;
    if( (first_rec->prior->sel_flags & SF_Recursive)==0 ) break;
  }

  /* Store the results of the setup-query in Queue. */
  setup = first_rec->prior;
  setup->next = 0;
  EXPLAIN_QUERY_PLAN((parser, 1, "SETUP"));
  rc = __select(parser, setup, &dest_queue);
  setup->next = p;
  if( rc ) goto end_of_recursive_query;

  /* Find the next row in the Queue and output that row */
  addr_top = __vdbe_add_op2(v, OP_Rewind, i_queue, addr_break); VDBE_COVERAGE(v);

  /* Transfer the next row in Queue over to Current */
  __vdbe_add_op1(v, OP_NullRow, i_current); /* To reset column cache */
  if( order_by ){
    __vdbe_add_op3(v, OP_Column, i_queue, order_by->n_expr+1, reg_current);
  }else{
    __vdbe_add_op2(v, OP_RowData, i_queue, reg_current);
  }
  __vdbe_add_op1(v, OP_Delete, i_queue);

  /* Output the single row in Current */
  addr_cont = __vdbe_make_label(parser);
  __code_offset(v, reg_offset, addr_cont);
  __select_inner_loop(parser, p, i_current,
      0, 0, p_dest, addr_cont, addr_break);
  if( reg_limit ){
    __vdbe_add_op2(v, OP_DecrJumpZero, reg_limit, addr_break);
    VDBE_COVERAGE(v);
  }
  __vdbe_resolve_label(v, addr_cont);

  /* Execute the recursive SELECT taking the single row in Current as
  ** the value for the recursive-table. Store the results in the Queue.
  */
  first_rec->prior = 0;
  EXPLAIN_QUERY_PLAN((parser, 1, "RECURSIVE STEP"));
  __select(parser, p, &dest_queue);
  assert( first_rec->prior==0 );
  first_rec->prior = setup;

  /* Keep running the loop until the Queue is empty */
  __vdbe_goto(v, addr_top);
  __vdbe_resolve_label(v, addr_break);

end_of_recursive_query:
  __expr_list_delete(parser->db, p->order_by);
  p->order_by = order_by;
  p->limit = limit;
  return;
}
#endif /* DBSQL_OMIT_CTE */

/* Forward references */
static int __multi_select_by_merge(
  parser_t *parser,        /* Parsing context */
  select_t *p,            /* The right-most of SELECTs to be coded */
  select_dest_t *dest     /* What to do with query results */
);

/*
** Handle the special case of a compound-select that originates from a
** VALUES clause.  By handling this as a special case, we avoid deep
** recursion, and thus do not need to enforce the DBSQL_LIMIT_COMPOUND_SELECT
** on a VALUES clause.
**
** Because the select_t object originates from a VALUES clause:
**   (1) There is no LIMIT or OFFSET or else there is a LIMIT of exactly 1
**   (2) All terms are UNION ALL
**   (3) There is no ORDER BY clause
**
** The "LIMIT of exactly 1" case of condition (1) comes about when a VALUES
** clause occurs within scalar expression (ex: "SELECT (VALUES(1),(2),(3))").
** The __code_subselect will have added the LIMIT 1 clause in tht case.
** Since the limit is exactly 1, we only need to evaluate the left-most VALUES.
*/
static int __multi_select_values(
  parser_t *parser,        /* Parsing context */
  select_t *p,            /* The right-most of SELECTs to be coded */
  select_dest_t *dest     /* What to do with query results */
){
  int n_row = 1;
  int rc = 0;
  int show_all = p->limit==0;
  assert( p->sel_flags & SF_MultiValue );
  do{
    assert( p->sel_flags & SF_Values );
    assert( p->op==TK_ALL || (p->op==TK_SELECT && p->prior==0) );
    assert( p->next==0 || p->e_list->n_expr==p->next->e_list->n_expr );
#ifndef DBSQL_OMIT_WINDOWFUNC
    if( p->win ) return -1;
#endif
    if( p->prior==0 ) break;
    assert( p->prior->next==p );
    p = p->prior;
    n_row += show_all;
  }while(1);
  EXPLAIN_QUERY_PLAN((parser, 0, "SCAN %d CONSTANT ROW%s", n_row,
                    n_row==1 ? "" : "S"));
  while( p ){
    __select_inner_loop(parser, p, -1, 0, 0, dest, 1, 1);
    if( !show_all ) break;
    p->n_select_row = n_row;
    p = p->next;
  }
  return rc;
}

/*
** Return true if the SELECT statement which is known to be the recursive
** part of a recursive CTE still has its anchor terms attached.  If the
** anchor terms have already been removed, then return false.
*/
static int __has_anchor(select_t *p){
  while( p && (p->sel_flags & SF_Recursive)!=0 ){ p = p->prior; }
  return p!=0;
}

/*
** This routine is called to process a compound query form from
** two or more separate queries using UNION, UNION ALL, EXCEPT, or
** INTERSECT
**
** "p" points to the right-most of the two queries.  the query on the
** left is p->prior.  The left query could also be a compound query
** in which case this routine will be called recursively.
**
** The results of the total query are to be written into a destination
** of type dest with parameter i_parm.
**
** Example 1:  Consider a three-way compound SQL statement.
**
**     SELECT a FROM t1 UNION SELECT b FROM t2 UNION SELECT c FROM t3
**
** This statement is parsed up as follows:
**
**     SELECT c FROM t3
**      |
**      `----->  SELECT b FROM t2
**                |
**                `------>  SELECT a FROM t1
**
** The arrows in the diagram above represent the select_t.prior pointer.
** So if this routine is called with p equal to the t3 query, then
** prior will be the t2 query.  p->op will be TK_UNION in this case.
**
** Notice that because of the way dbsql parses compound SELECTs, the
** individual selects always group from left to right.
*/
static int __multi_select(
  parser_t *parser,        /* Parsing context */
  select_t *p,            /* The right-most of SELECTs to be coded */
  select_dest_t *p_dest     /* What to do with query results */
){
  int rc = DBSQL_OK;   /* Success code from a subroutine */
  select_t *prior;       /* Another SELECT immediately to our left */
  vdbe_t *v;              /* Generate code to this VDBE */
  select_dest_t dest;      /* Alternative data destination */
  select_t *p_delete = 0;  /* Chain of simple selects to delete */
  DBSQL *db;          /* Database connection */

  /* Make sure there is no ORDER BY or LIMIT clause on prior SELECTs.  Only
  ** the last (right-most) SELECT in the series may have an ORDER BY or LIMIT.
  */
  assert( p && p->prior );  /* Calling function guarantees this much */
  assert( (p->sel_flags & SF_Recursive)==0 || p->op==TK_ALL || p->op==TK_UNION );
  assert( p->sel_flags & SF_Compound );
  db = parser->db;
  prior = p->prior;
  dest = *p_dest;
  assert( prior->order_by==0 );
  assert( prior->limit==0 );

  v = __get_vdbe(parser);
  assert( v!=0 );  /* The VDBE already created by calling function */

  /* Create the destination temporary table if necessary
  */
  if( dest.dest==SRT_EphemTab ){
    assert( p->e_list );
    __vdbe_add_op2(v, OP_OpenEphemeral, dest.i_sd_parm, p->e_list->n_expr);
    dest.dest = SRT_Table;
  }

  /* Special handling for a compound-select that originates as a VALUES clause.
  */
  if( p->sel_flags & SF_MultiValue ){
    rc = __multi_select_values(parser, p, &dest);
    if( rc>=0 ) goto multi_select_end;
    rc = DBSQL_OK;
  }

  /* Make sure all SELECTs in the statement have the same number of elements
  ** in their result sets.
  */
  assert( p->e_list && prior->e_list );
  assert( p->e_list->n_expr==prior->e_list->n_expr );

#ifndef DBSQL_OMIT_CTE
  if( (p->sel_flags & SF_Recursive)!=0 && __has_anchor(p) ){
    __generate_with_recursive_query(parser, p, &dest);
  }else
#endif
  if( p->order_by ){
    /* If the compound has an ORDER BY clause, then always use the merge
    ** algorithm. */
    return __multi_select_by_merge(parser, p, p_dest);
  }else if( p->op!=TK_ALL ){
    /* If the compound is EXCEPT, INTERSECT, or UNION (anything other than
    ** UNION ALL) then also always use the merge algorithm.  However, the
    ** __multi_select_by_merge() routine requires that the compound have an
    ** ORDER BY clause, and it doesn't right now.  So invent one first. */
    expr_t *one = __expr_int32(db, 1);
    p->order_by = __expr_list_append(parser, 0, one);
    if( parser->n_err ) goto multi_select_end;
    assert( p->order_by!=0 );
    p->order_by->a[0].u.x.i_order_by_col = 1;
    return __multi_select_by_merge(parser, p, p_dest);
  }else{
    /* For a UNION ALL compound without ORDER BY, simply run the left
    ** query, then run the right query */
    int addr = 0;
    int n_limit = 0;  /* Initialize to suppress harmless compiler warning */

#ifndef DBSQL_OMIT_EXPLAIN
    if( prior->prior==0 ){
      EXPLAIN_QUERY_PLAN((parser, 1, "COMPOUND QUERY"));
      EXPLAIN_QUERY_PLAN((parser, 1, "LEFT-MOST SUBQUERY"));
    }
#endif
    assert( !prior->limit );
    prior->i_limit = p->i_limit;
    prior->i_offset = p->i_offset;
    prior->limit = __expr_dup(db, p->limit, 0);
    TREETRACE(0x200, parser, p, ("multiSelect UNION ALL left...\n"));
    rc = __select(parser, prior, &dest);
    __expr_delete(db, prior->limit);
    prior->limit = 0;
    if( rc ){
      goto multi_select_end;
    }
    p->prior = 0;
    p->i_limit = prior->i_limit;
    p->i_offset = prior->i_offset;
    if( p->i_limit ){
      addr = __vdbe_add_op1(v, OP_IfNot, p->i_limit); VDBE_COVERAGE(v);
      VDBE_COMMENT((v, "Jump ahead if LIMIT reached"));
      if( p->i_offset ){
        __vdbe_add_op3(v, OP_OffsetLimit,
                          p->i_limit, p->i_offset+1, p->i_offset);
      }
    }
    EXPLAIN_QUERY_PLAN((parser, 1, "UNION ALL"));
    TREETRACE(0x200, parser, p, ("multiSelect UNION ALL right...\n"));
    rc = __select(parser, p, &dest);
    testcase( rc!=DBSQL_OK );
    p_delete = p->prior;
    p->prior = prior;
    p->n_select_row = __log_est_add(p->n_select_row, prior->n_select_row);
    if( p->limit
     && __expr_is_integer(p->limit->left, &n_limit, parser)
     && n_limit>0 && p->n_select_row > __log_est((u_int64_t)n_limit)
    ){
      p->n_select_row = __log_est((u_int64_t)n_limit);
    }
    if( addr ){
      __vdbe_jump_here(v, addr);
    }
#ifndef DBSQL_OMIT_EXPLAIN
    if( p->next==0 ){
      EXPLAIN_QUERY_PLAN_POP(parser);
    }
#endif
  }

multi_select_end:
  p_dest->i_sdst = dest.i_sdst;
  p_dest->n_sdst = dest.n_sdst;
  p_dest->i_sd_parm2 = dest.i_sd_parm2;
  if( p_delete ){
    __parser_add_cleanup(parser, __select_delete_generic, p_delete);
  }
  return rc;
}
#endif /* DBSQL_OMIT_COMPOUND_SELECT */

/*
** Error message for when two or more terms of a compound select have different
** size result sets.
*/
void __select_wrong_num_terms_error(parser_t *parser, select_t *p){
  if( p->sel_flags & SF_Values ){
    __error_msg(parser, "all VALUES must have the same number of terms");
  }else{
    __error_msg(parser, "SELECTs to the left and right of %s"
      " do not have the same number of result columns",
      __select_op_name(p->op));
  }
}

/*
** Code an output subroutine for a coroutine implementation of a
** SELECT statement.
**
** The data to be output is contained in an array of in->n_sdst registers
** starting at register in->i_sdst.  dest is where the output should
** be sent.
**
** reg_return is the number of the register holding the subroutine
** return address.
**
** If reg_prev>0 then it is the first register in a vector that
** records the previous output.  mem[reg_prev] is a flag that is false
** if there has been no previous output.  If reg_prev>0 then code is
** generated to suppress duplicates.  key_info is used for comparing
** keys.
**
** If the LIMIT found in p->i_limit is reached, jump immediately to
** i_break.
*/
static int __generate_output_subroutine(
  parser_t *parser,          /* Parsing context */
  select_t *p,              /* The SELECT statement */
  select_dest_t *in,        /* Coroutine supplying data */
  select_dest_t *dest,      /* Where to send the data */
  int reg_return,          /* The return address register */
  int reg_prev,            /* Previous result register.  No uniqueness if 0 */
  key_info_t *key_info,      /* For comparing with previous entry */
  int i_break              /* Jump here if we hit the LIMIT */
){
  vdbe_t *v = parser->vdbe;
  int i_continue;
  int addr;

  assert( in->dest==SRT_Coroutine );

  addr = __vdbe_current_addr(v);
  i_continue = __vdbe_make_label(parser);

  /* Suppress duplicates for UNION, EXCEPT, and INTERSECT
  */
  if( reg_prev ){
    int addr1, addr2;
    addr1 = __vdbe_add_op1(v, OP_IfNot, reg_prev); VDBE_COVERAGE(v);
    addr2 = __vdbe_add_op4(v, OP_Compare, in->i_sdst, reg_prev+1, in->n_sdst,
                              (char*)__key_info_ref(key_info), P4_KEYINFO);
    __vdbe_add_op3(v, OP_Jump, addr2+2, i_continue, addr2+2); VDBE_COVERAGE(v);
    __vdbe_jump_here(v, addr1);
    __vdbe_add_op3(v, OP_Copy, in->i_sdst, reg_prev+1, in->n_sdst-1);
    __vdbe_add_op2(v, OP_Integer, 1, reg_prev);
  }
  if( parser->db->malloc_failed ) return 0;

  /* Suppress the first OFFSET entries if there is an OFFSET clause
  */
  __code_offset(v, p->i_offset, i_continue);

  switch( dest->dest ){
    /* Store the result as data using a unique key.
    */
    case SRT_Fifo:
    case SRT_DistFifo:
    case SRT_Table:
    case SRT_EphemTab: {
      int r1 = __get_temp_reg(parser);
      int r2 = __get_temp_reg(parser);
      int i_parm = dest->i_sd_parm;
      testcase( dest->dest==SRT_Table );
      testcase( dest->dest==SRT_EphemTab );
      testcase( dest->dest==SRT_Fifo );
      testcase( dest->dest==SRT_DistFifo );
      __vdbe_add_op3(v, OP_MakeRecord, in->i_sdst, in->n_sdst, r1);
#if !defined(DBSQL_ENABLE_NULL_TRIM) && defined(DBSQL_DEBUG)
      /* A destination of SRT_Table and a non-zero i_sd_parm2 parameter means
      ** that this is an "UPDATE ... FROM" on a virtual table or view. In this
      ** case set the p5 parameter of the OP_MakeRecord to OPFLAG_NOCHNG_MAGIC.
      ** This does not affect operation in any way - it just allows __make_record
      ** to process OPFLAG_NOCHANGE values without an assert() failing. */
      if( dest->dest==SRT_Table && dest->i_sd_parm2 ){
        __vdbe_change_p5(v, OPFLAG_NOCHNG_MAGIC);
      }
#endif
#ifndef DBSQL_OMIT_CTE
      if( dest->dest==SRT_DistFifo ){
        /* If the destination is __dist_fifo, then cursor (i_parm+1) is open
        ** on an ephemeral index that is used to enforce uniqueness on the
        ** total result.  At this point, we are processing the setup portion
        ** of the recursive CTE using the merge algorithm, so the results are
        ** guaranteed to be unique anyhow.  But we still need to populate the
        ** (i_parm+1) cursor for use by the subsequent recursive phase.
        */
        __vdbe_add_op4_int(v, OP_IdxInsert, i_parm+1, r1,
                             in->i_sdst, in->n_sdst);
      }
#endif
      __vdbe_add_op2(v, OP_NewRowid, i_parm, r2);
      __vdbe_add_op3(v, OP_Insert, i_parm, r1, r2);
      __vdbe_change_p5(v, OPFLAG_APPEND);
      __release_temp_reg(parser, r2);
      __release_temp_reg(parser, r1);
      break;
    }

    /* If any row exist in the result set, record that fact and abort.
    */
    case SRT_Exists: {
      __vdbe_add_op2(v, OP_Integer, 1, dest->i_sd_parm);
      /* The LIMIT clause will terminate the loop for us */
      break;
    }

#ifndef DBSQL_OMIT_SUBQUERY
    /* If we are creating a set for an "expr IN (SELECT ...)".
    */
    case SRT_Set: {
      int r1;
      testcase( in->n_sdst>1 );
      r1 = __get_temp_reg(parser);
      __vdbe_add_op4(v, OP_MakeRecord, in->i_sdst, in->n_sdst,
          r1, dest->aff_sdst, in->n_sdst);
      __vdbe_add_op4_int(v, OP_IdxInsert, dest->i_sd_parm, r1,
                           in->i_sdst, in->n_sdst);
      if( dest->i_sd_parm2>0 ){
        __vdbe_add_op4_int(v, OP_FilterAdd, dest->i_sd_parm2, 0,
                             in->i_sdst, in->n_sdst);
        EXPLAIN_QUERY_PLAN((parser, 0, "CREATE BLOOM FILTER"));
      }
      __release_temp_reg(parser, r1);
      break;
    }

    /* If this is a scalar select that is part of an expression, then
    ** store the results in the appropriate memory cell and break out
    ** of the scan loop.  Note that the select might return multiple columns
    ** if it is the RHS of a row-value IN operator.
    */
    case SRT_Mem: {
      testcase( in->n_sdst>1 );
      __expr_code_move(parser, in->i_sdst, dest->i_sd_parm, in->n_sdst);
      /* The LIMIT clause will jump out of the loop for us */
      break;
    }
#endif /* #ifndef DBSQL_OMIT_SUBQUERY */

    /* The results are stored in a sequence of registers
    ** starting at dest->i_sdst.  Then the co-routine yields.
    */
    case SRT_Coroutine: {
      if( dest->i_sdst==0 ){
        dest->i_sdst = __get_temp_range(parser, in->n_sdst);
        dest->n_sdst = in->n_sdst;
      }
      __expr_code_move(parser, in->i_sdst, dest->i_sdst, in->n_sdst);
      __vdbe_add_op1(v, OP_Yield, dest->i_sd_parm);
      break;
    }

#ifndef DBSQL_OMIT_CTE
    /* Write the results into a priority queue that is order according to
    ** dest->order_by (in so).  dest->i_sd_parm (in i_parm) is the cursor for an
    ** index with so->n_expr+2 columns.  Build a key using so for the first
    ** so->n_expr columns, then make sure all keys are unique by adding a
    ** final OP_Sequence column.  The last column is the record as a blob.
    */
    case SRT_DistQueue:
    case SRT_Queue: {
      int n_key;
      int r1, r2, r3, ii;
      expr_list_t *so;
      int i_parm = dest->i_sd_parm;
      so = dest->order_by;
      assert( so );
      n_key = so->n_expr;
      r1 = __get_temp_reg(parser);
      r2 = __get_temp_range(parser, n_key+2);
      r3 = r2+n_key+1;

      __vdbe_add_op3(v, OP_MakeRecord, in->i_sdst, in->n_sdst, r3);
      if( dest->dest==SRT_DistQueue ){
        __vdbe_add_op2(v, OP_IdxInsert, i_parm+1, r3);
      }
      for(ii=0; ii<n_key; ii++){
        __vdbe_add_op2(v, OP_SCopy,
                          in->i_sdst + so->a[ii].u.x.i_order_by_col - 1,
                          r2+ii);
      }
      __vdbe_add_op2(v, OP_Sequence, i_parm, r2+n_key);
      __vdbe_add_op3(v, OP_MakeRecord, r2, n_key+2, r1);
      __vdbe_add_op4_int(v, OP_IdxInsert, i_parm, r1, r2, n_key+2);
      __release_temp_reg(parser, r1);
      __release_temp_range(parser, r2, n_key+2);
      break;
    }
#endif /* DBSQL_OMIT_CTE */

    /* Ignore the output */
    case SRT_Discard: {
      break;
    }

    /* If none of the above, then the result destination must be
    ** SRT_Output.
    **
    ** For SRT_Output, results are stored in a sequence of registers. 
    ** Then the OP_ResultRow opcode is used to cause dbsql_step() to
    ** return the next row of result.
    */
    default: {
      assert( dest->dest==SRT_Output );
      __vdbe_add_op2(v, OP_ResultRow, in->i_sdst, in->n_sdst);
      break;
    }
  }

  /* Jump to the end of the loop if the LIMIT is reached.
  */
  if( p->i_limit ){
    __vdbe_add_op2(v, OP_DecrJumpZero, p->i_limit, i_break); VDBE_COVERAGE(v);
  }

  /* Generate the subroutine return
  */
  __vdbe_resolve_label(v, i_continue);
  __vdbe_add_op1(v, OP_Return, reg_return);

  return addr;
}

/*
** Generate code for a compound SELECT statement using a merge
** algorithm.  The compound must have an ORDER BY clause for this
** to work.
**
** We assume a query of the following form:
**
**      <select_a>  <operator>  <select_b>  ORDER BY <orderbylist>
**
** <operator> is one of UNION ALL, UNION, EXCEPT, or INTERSECT.  The idea
** is to code both <select_a> and <select_b> with the ORDER BY clause as
** co-routines.  Then run the co-routines in parallel and merge the results
** into the output.  In addition to the two coroutines (called select_a and
** select_b) there are 7 subroutines:
**
**    out_a:    Move the output of the select_a coroutine into the output
**             of the compound query.
**
**    out_b:    Move the output of the select_b coroutine into the output
**             of the compound query.  (Only generated for UNION and
**             UNION ALL.  EXCEPT and INTERSECT never output a row that
**             appears only in B.)
**
**    __alt_b:    Called when there is data from both coroutines and A<B.
**
**    __aeq_b:    Called when there is data from both coroutines and A==B.
**
**    __agt_b:    Called when there is data from both coroutines and A>B.
**
**    __eof_a:    Called when data is exhausted from select_a.
**
**    __eof_b:    Called when data is exhausted from select_b.
**
** The implementation of the latter five subroutines depend on which
** <operator> is used:
**
**
**             UNION ALL         UNION            EXCEPT          INTERSECT
**          -------------  -----------------  --------------  -----------------
**   __alt_b:   out_a, next_a      out_a, next_a       out_a, next_a         next_a
**
**   __aeq_b:   out_a, next_a         next_a             next_a         out_a, next_a
**
**   __agt_b:   out_b, next_b      out_b, next_b          next_b            next_b
**
**   __eof_a:   out_b, next_b      out_b, next_b          halt             halt
**
**   __eof_b:   out_a, next_a      out_a, next_a       out_a, next_a         halt
**
** In the __alt_b, __aeq_b, and __agt_b subroutines, an EOF on A following next_a
** causes an immediate jump to __eof_a and an EOF on B following next_b causes
** an immediate jump to __eof_b.  Within __eof_a and __eof_b, and EOF on entry or
** following next_x causes a jump to the end of the select processing.
**
** Duplicate removal in the UNION, EXCEPT, and INTERSECT cases is handled
** within the output subroutine.  The reg_prev register set holds the previously
** output value.  A comparison is made against this value and the output
** is skipped if the next results would be the same as the previous.
**
** The implementation plan is to implement the two coroutines and seven
** subroutines first, then put the control logic at the bottom.  Like this:
**
**          goto Init
**     co_a: coroutine for left query (A)
**     co_b: coroutine for right query (B)
**    out_a: output one row of A
**    out_b: output one row of B (UNION and UNION ALL only)
**    __eof_a: ...
**    __eof_b: ...
**    __alt_b: ...
**    __aeq_b: ...
**    __agt_b: ...
**    Init: initialize coroutine registers
**          yield co_a, on eof goto __eof_a
**          yield co_b, on eof goto __eof_b
**    Cmpr: Compare A, B
**          Jump __alt_b, __aeq_b, __agt_b
**     End: ...
**
** We call __alt_b, __aeq_b, __agt_b, __eof_a, and __eof_b "subroutines" but they are not
** actually called using Gosub and they do not Return.  __eof_a and __eof_b loop
** until all data is exhausted then jump to the "end" label.  __alt_b, __aeq_b,
** and __agt_b jump to either Cmpr or to one of __eof_a or __eof_b.
*/
#ifndef DBSQL_OMIT_COMPOUND_SELECT
static int __multi_select_by_merge(
  parser_t *parser,        /* Parsing context */
  select_t *p,            /* The right-most of SELECTs to be coded */
  select_dest_t *dest     /* What to do with query results */
){
  int i, j;             /* Loop counters */
  select_t *prior;       /* Another SELECT immediately to our left */
  select_t *split;       /* Left-most SELECT in the right-hand group */
  int n_select;          /* Number of SELECT statements in the compound */
  vdbe_t *v;              /* Generate code to this VDBE */
  select_dest_t dest_a;     /* Destination for coroutine A */
  select_dest_t dest_b;     /* Destination for coroutine B */
  int reg_addr_a;         /* Address register for select-A coroutine */
  int reg_addr_b;         /* Address register for select-B coroutine */
  int addr_select_a;      /* Address of the select-A coroutine */
  int addr_select_b;      /* Address of the select-B coroutine */
  int reg_out_a;          /* Address register for the output-A subroutine */
  int reg_out_b;          /* Address register for the output-B subroutine */
  int addr_out_a;         /* Address of the output-A subroutine */
  int addr_out_b = 0;     /* Address of the output-B subroutine */
  int addr_eof_a;         /* Address of the select-A-exhausted subroutine */
  int addr_eof_a_no_b;     /* Alternate addr_eof_a if B is uninitialized */
  int addr_eof_b;         /* Address of the select-B-exhausted subroutine */
  int addr_alt_b;         /* Address of the A<B subroutine */
  int addr_aeq_b;         /* Address of the A==B subroutine */
  int addr_agt_b;         /* Address of the A>B subroutine */
  int reg_limit_a;        /* Limit register for select-A */
  int reg_limit_b;        /* Limit register for select-A */
  int reg_prev;          /* A range of registers to hold previous output */
  int saved_limit;       /* Saved value of p->i_limit */
  int saved_offset;      /* Saved value of p->i_offset */
  int label_cmpr;        /* Label for the start of the merge algorithm */
  int label_end;         /* Label for the end of the overall SELECT stmt */
  int addr1;            /* Jump instructions that get retargeted */
  int op;               /* One of TK_ALL, TK_UNION, TK_EXCEPT, TK_INTERSECT */
  key_info_t *key_dup = 0; /* Comparison information for duplicate removal */
  key_info_t *key_merge;   /* Comparison information for merging rows */
  DBSQL *db;          /* Database connection */
  expr_list_t *order_by;   /* The ORDER BY clause */
  int n_order_by;         /* Number of terms in the ORDER BY clause */
  u_int32_t *permutes;        /* Mapping from ORDER BY terms to result set columns */

  assert( p->order_by!=0 );
  assert( key_dup==0 ); /* "Managed" code needs this.  Ticket #3382. */
  db = parser->db;
  v = parser->vdbe;
  assert( v!=0 );       /* Already thrown the error if VDBE alloc failed */
  label_end = __vdbe_make_label(parser);
  label_cmpr = __vdbe_make_label(parser);


  /* Patch up the ORDER BY clause
  */
  op = p->op; 
  assert( p->prior->order_by==0 );
  order_by = p->order_by;
  assert( order_by );
  n_order_by = order_by->n_expr;

  /* For operators other than UNION ALL we have to make sure that
  ** the ORDER BY clause covers every term of the result set.  Add
  ** terms to the ORDER BY clause as necessary.
  */
  if( op!=TK_ALL ){
    for(i=1; db->malloc_failed==0 && i<=p->e_list->n_expr; i++){
      struct expr_list_item_t *item;
      for(j=0, item=order_by->a; j<n_order_by; j++, item++){
        assert( item!=0 );
        assert( item->u.x.i_order_by_col>0 );
        if( item->u.x.i_order_by_col==i ) break;
      }
      if( j==n_order_by ){
        expr_t *p_new = __expr_int32(db, i);
        if( p_new==0 ) return DBSQL_NOMEM_BKPT;
        p->order_by = order_by = __expr_list_append(parser, order_by, p_new);
        if( order_by ) order_by->a[n_order_by++].u.x.i_order_by_col = (u_int16_t)i;
      }
    }
  }

  /* Compute the comparison permutation and keyinfo that is used with
  ** the permutation to determine if the next row of results comes
  ** from select_a or select_b.  Also add literal collations to the
  ** ORDER BY clause terms so that when select_a and select_b are
  ** evaluated, they use the correct collation.
  */
  permutes = __db_malloc_raw_nn(db, sizeof(u_int32_t)*(n_order_by + 1));
  if( permutes ){
    struct expr_list_item_t *item;
    int keep = 0;
    permutes[0] = n_order_by;
    for(i=1, item=order_by->a; i<=n_order_by; i++, item++){
      assert( item!=0 );
      assert( item->u.x.i_order_by_col>0 );
      assert( item->u.x.i_order_by_col<=p->e_list->n_expr );
      permutes[i] = item->u.x.i_order_by_col - 1;
      if( permutes[i]!=(u_int32_t)i-1 ) keep = 1;
    }
     if( keep==0 ){
      __db_free_nn(db, permutes);
      permutes = 0;
    }
  }
  key_merge = __multi_select_by_merge_key_info(parser, p, 1);

  /* Allocate a range of temporary registers and the key_info_t needed
  ** for the logic that removes duplicate result rows when the
  ** operator is UNION, EXCEPT, or INTERSECT (but not UNION ALL).
  */
  if( op==TK_ALL ){
    reg_prev = 0;
  }else{
    int n_expr = p->e_list->n_expr;
    assert( n_order_by>=n_expr || db->malloc_failed );
    reg_prev = parser->n_mem+1;
    parser->n_mem += n_expr+1;
    __vdbe_add_op2(v, OP_Integer, 0, reg_prev);
    key_dup = __key_info_alloc(db, n_expr, 1);
    if( key_dup ){
      assert( __key_info_is_writeable(key_dup) );
      for(i=0; i<n_expr; i++){
        key_dup->colls[i] = __multi_select_coll_seq(parser, p, i);
        key_dup->sort_flags[i] = 0;
      }
    }
  }

  /* Separate the left and the right query from one another
  */
  n_select = 1;
  if( (op==TK_ALL || op==TK_UNION)
   && OPTIMIZATION_ENABLED(db, DBSQL_BalancedMerge)
  ){
    for(split=p; split->prior!=0 && split->op==op; split=split->prior){
      n_select++;
      assert( split->prior->next==split );
    }
  }
  if( n_select<=3 ){
    split = p;
  }else{
    split = p;
    for(i=2; i<n_select; i+=2){ split = split->prior; }
  }
  prior = split->prior;
  assert( prior!=0 );
  split->prior = 0;
  prior->next = 0;
  assert( p->order_by == order_by );
  assert( order_by!=0 || db->malloc_failed );
  prior->order_by = __expr_list_dup(parser->db, order_by, 0);
  __resolve_order_group_by(parser, p, p->order_by, "ORDER");
  __resolve_order_group_by(parser, prior, prior->order_by, "ORDER");

  /* Compute the limit registers */
  __compute_limit_registers(parser, p, label_end);
  if( p->i_limit && op==TK_ALL ){
    reg_limit_a = ++parser->n_mem;
    reg_limit_b = ++parser->n_mem;
    __vdbe_add_op2(v, OP_Copy, p->i_offset ? p->i_offset+1 : p->i_limit,
                                  reg_limit_a);
    __vdbe_add_op2(v, OP_Copy, reg_limit_a, reg_limit_b);
  }else{
    reg_limit_a = reg_limit_b = 0;
  }
  __expr_delete(db, p->limit);
  p->limit = 0;

  reg_addr_a = ++parser->n_mem;
  reg_addr_b = ++parser->n_mem;
  reg_out_a = ++parser->n_mem;
  reg_out_b = ++parser->n_mem;
  __select_dest_init(&dest_a, SRT_Coroutine, reg_addr_a);
  __select_dest_init(&dest_b, SRT_Coroutine, reg_addr_b);

  EXPLAIN_QUERY_PLAN((parser, 1, "MERGE (%s)", __select_op_name(p->op)));

  /* Generate a coroutine to evaluate the SELECT statement to the
  ** left of the compound operator - the "A" select.
  */
  addr_select_a = __vdbe_current_addr(v) + 1;
  addr1 = __vdbe_add_op3(v, OP_InitCoroutine, reg_addr_a, 0, addr_select_a);
  VDBE_COMMENT((v, "SUBR: next-A"));
  prior->i_limit = reg_limit_a;
  EXPLAIN_QUERY_PLAN((parser, 1, "LEFT"));
  __select(parser, prior, &dest_a);
  __vdbe_end_coroutine(v, reg_addr_a);
  __vdbe_jump_here(v, addr1);

  /* Generate a coroutine to evaluate the SELECT statement on
  ** the right - the "B" select
  */
  addr_select_b = __vdbe_current_addr(v) + 1;
  addr1 = __vdbe_add_op3(v, OP_InitCoroutine, reg_addr_b, 0, addr_select_b);
  VDBE_COMMENT((v, "SUBR: next-B"));
  saved_limit = p->i_limit;
  saved_offset = p->i_offset;
  p->i_limit = reg_limit_b;
  p->i_offset = 0; 
  EXPLAIN_QUERY_PLAN((parser, 1, "RIGHT"));
  __select(parser, p, &dest_b);
  p->i_limit = saved_limit;
  p->i_offset = saved_offset;
  __vdbe_end_coroutine(v, reg_addr_b);

  /* Generate a subroutine that outputs the current row of the A
  ** select as the next output row of the compound select.
  */
  VDBE_NOOP_COMMENT((v, "SUBR: out-A"));
  addr_out_a = __generate_output_subroutine(parser,
                 p, &dest_a, dest, reg_out_a,
                 reg_prev, key_dup, label_end);
 
  /* Generate a subroutine that outputs the current row of the B
  ** select as the next output row of the compound select.
  */
  if( op==TK_ALL || op==TK_UNION ){
    VDBE_NOOP_COMMENT((v, "SUBR: out-B"));
    addr_out_b = __generate_output_subroutine(parser,
                 p, &dest_b, dest, reg_out_b,
                 reg_prev, key_dup, label_end);
  }
  __key_info_unref(key_dup);

  /* Generate a subroutine to run when the results from select A
  ** are exhausted and only data in select B remains.
  */
  if( op==TK_EXCEPT || op==TK_INTERSECT ){
    addr_eof_a_no_b = addr_eof_a = label_end;
  }else{ 
    VDBE_NOOP_COMMENT((v, "SUBR: eof-A"));
    addr_eof_a = __vdbe_add_op2(v, OP_Gosub, reg_out_b, addr_out_b);
    VDBE_COMMENT((v, "out-B"));
    addr_eof_a_no_b = __vdbe_add_op2(v, OP_Yield, reg_addr_b, label_end);
                                     VDBE_COVERAGE(v);
    VDBE_COMMENT((v, "next-B"));
    __vdbe_goto(v, addr_eof_a);
    p->n_select_row = __log_est_add(p->n_select_row, prior->n_select_row);
  }

  /* Generate a subroutine to run when the results from select B
  ** are exhausted and only data in select A remains.
  */
  if( op==TK_INTERSECT ){
    addr_eof_b = addr_eof_a;
    if( p->n_select_row > prior->n_select_row ) p->n_select_row = prior->n_select_row;
  }else{ 
    VDBE_NOOP_COMMENT((v, "SUBR: eof-B"));
    addr_eof_b = __vdbe_add_op2(v, OP_Gosub, reg_out_a, addr_out_a);
    VDBE_COMMENT((v, "out-A"));
    __vdbe_add_op2(v, OP_Yield, reg_addr_a, label_end); VDBE_COVERAGE(v);
    VDBE_COMMENT((v, "next-A"));
    __vdbe_goto(v, addr_eof_b);
  }

  /* Generate code to handle the case of A<B
  */
  addr_alt_b = __vdbe_add_op2(v, OP_Gosub, reg_out_a, addr_out_a);
  VDBE_COMMENT((v, "out-A"));
  __vdbe_add_op2(v, OP_Yield, reg_addr_a, addr_eof_a); VDBE_COVERAGE(v);
  VDBE_COMMENT((v, "next-A"));
  __vdbe_goto(v, label_cmpr);

  /* Generate code to handle the case of A==B
  */
  if( op==TK_ALL ){
    addr_aeq_b = addr_alt_b;
  }else if( op==TK_INTERSECT ){
    addr_aeq_b = addr_alt_b;
    addr_alt_b++;
  }else{
    addr_aeq_b = addr_alt_b + 1;
  }

  /* Generate code to handle the case of A>B
  */
  addr_agt_b = __vdbe_current_addr(v);
  if( op==TK_ALL || op==TK_UNION ){
    __vdbe_add_op2(v, OP_Gosub, reg_out_b, addr_out_b);
    VDBE_COMMENT((v, "out-B"));
    __vdbe_add_op2(v, OP_Yield, reg_addr_b, addr_eof_b); VDBE_COVERAGE(v);
    VDBE_COMMENT((v, "next-B"));
    __vdbe_goto(v, label_cmpr);
  }else{
    addr_agt_b++; /* Just do next-B.  Might as well use the next-B call
                ** in the next code block */
  }

  /* This code runs once to initialize everything.
  */
  __vdbe_jump_here(v, addr1);
  __vdbe_add_op2(v, OP_Yield, reg_addr_a, addr_eof_a_no_b); VDBE_COVERAGE(v);
  VDBE_COMMENT((v, "next-A"));
  /* v---  Also the A>B case for EXCEPT and INTERSECT */
  __vdbe_add_op2(v, OP_Yield, reg_addr_b, addr_eof_b); VDBE_COVERAGE(v);
  VDBE_COMMENT((v, "next-B"));

  /* Implement the main merge loop
  */
  if( permutes!=0 ){
    __vdbe_add_op4(v, OP_Permutation, 0, 0, 0, (char*)permutes, P4_INTARRAY);
  }
  __vdbe_resolve_label(v, label_cmpr);
  __vdbe_add_op4(v, OP_Compare, dest_a.i_sdst, dest_b.i_sdst, n_order_by,
                         (char*)key_merge, P4_KEYINFO);
  if( permutes!=0 ){
    __vdbe_change_p5(v, OPFLAG_PERMUTE);
  }
  __vdbe_add_op3(v, OP_Jump, addr_alt_b, addr_aeq_b, addr_agt_b);
  VDBE_COVERAGE_IF(v, op==TK_ALL);
  VDBE_COVERAGE_IF(v, op==TK_UNION);
  VDBE_COVERAGE_IF(v, op==TK_EXCEPT);
  VDBE_COVERAGE_IF(v, op==TK_INTERSECT);

  /* Jump to the this point in order to terminate the query.
  */
  __vdbe_resolve_label(v, label_end);

  /* Make arrangements to free the 2nd and subsequent arms of the compound
  ** after the parse has finished */
  if( split->prior ){
    __parser_add_cleanup(parser, __select_delete_generic, split->prior);
  }
  split->prior = prior;
  prior->next = split;
  __expr_list_delete(db, prior->order_by);
  prior->order_by = 0;

  /*** TBD:  Insert subroutine calls to close cursors on incomplete
  **** subqueries ****/
  EXPLAIN_QUERY_PLAN_POP(parser);
  return parser->n_err!=0;
}
#endif

#if !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW)

/* An instance of the subst_context_t object describes an substitution edit
** to be performed on a parse tree.
**
** All references to columns in table i_table are to be replaced by corresponding
** expressions in e_list.
**
** ## About "is_outer_join":
**
** The is_outer_join column indicates that the replacement will occur into a
** position in the parent that is NULL-able due to an OUTER JOIN.  Either the
** target slot in the parent is the right operand of a LEFT JOIN, or one of
** the left operands of a RIGHT JOIN.  In either case, we need to potentially
** bypass the substituted expression with OP_IfNullRow.
**
** Suppose the original expression is an integer constant. Even though the table
** has the null_row flag set, because the expression is an integer constant,
** it will not be NULLed out.  So instead, we insert an OP_IfNullRow opcode
** that checks to see if the null_row flag is set on the table.  If the null_row
** flag is set, then the value in the register is set to NULL and the original
** expression is bypassed.  If the null_row flag is not set, then the original
** expression runs to populate the register.
**
** Example where this is needed:
**
**      CREATE TABLE t1(a INTEGER PRIMARY KEY, b INT);
**      CREATE TABLE t2(x INT UNIQUE);
**
**      SELECT a,b,m,x FROM t1 LEFT JOIN (SELECT 59 AS m,x FROM t2) ON b=x;
**
** When the subquery on the right side of the LEFT JOIN is flattened, we
** have to add OP_IfNullRow in front of the OP_Integer that implements the
** "m" value of the subquery so that a NULL will be loaded instead of 59
** when processing a non-matched row of the left.
*/
typedef struct subst_context_t {
  parser_t *parser;            /* The parsing context */
  int i_table;               /* Replace references to this table */
  int i_new_table;            /* New table number */
  int is_outer_join;          /* Add TK_IF_NULL_ROW opcodes on each replacement */
  int n_sel_depth;            /* Depth of sub-query recursion.  Top==1 */
  expr_list_t *e_list;         /* Replacement expressions */
  expr_list_t *c_list;         /* Collation sequences for replacement expr */
} subst_context_t;

/* Forward Declarations */
static void __subst_expr_list(subst_context_t*, expr_list_t*);
static void __subst_select(subst_context_t*, select_t*, int);

/*
** Scan through the expression expr.  Replace every reference to
** a column in table number i_table with a copy of the i_column-th
** entry in e_list.  (But leave references to the ROWID column
** unchanged.)
**
** This routine is part of the flattening procedure.  A subquery
** whose result set is defined by e_list appears as entry in the
** FROM clause of a SELECT such that the VDBE cursor assigned to that
** FORM clause entry is i_table.  This routine makes the necessary
** changes to expr so that it refers directly to the source table
** of the subquery rather the result set of the subquery.
*/
static expr_t *__subst_expr(
  subst_context_t *subst,  /* Description of the substitution */
  expr_t *expr            /* expr_t in which substitution occurs */
){
  if( expr==0 ) return 0;
  if( EXPR_HAS_PROPERTY(expr, EP_OuterON|EP_InnerON)
   && expr->w.i_join==subst->i_table
  ){
    testcase( EXPR_HAS_PROPERTY(expr, EP_InnerON) );
    expr->w.i_join = subst->i_new_table;
  }
  if( expr->op==TK_COLUMN
   && expr->i_table==subst->i_table
   && !EXPR_HAS_PROPERTY(expr, EP_FixedCol)
  ){
#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
    if( expr->i_column<0 ){
      expr->op = TK_NULL;
    }else
#endif
    {
      expr_t *p_new;
      int i_column;
      expr_t *copy;
      expr_t if_null_row;
      i_column = expr->i_column;
      assert( i_column>=0 );
      assert( subst->e_list!=0 && i_column<subst->e_list->n_expr );
      assert( expr->right==0 );
      copy = subst->e_list->a[i_column].expr;
      if( __expr_is_vector(copy) ){
        __vector_error_msg(subst->parser, copy);
      }else{
        DBSQL *db = subst->parser->db;
        if( subst->is_outer_join
         && (copy->op!=TK_COLUMN || copy->i_table!=subst->i_new_table)
        ){
          memset(&if_null_row, 0, sizeof(if_null_row));
          if_null_row.op = TK_IF_NULL_ROW;
          if_null_row.left = copy;
          if_null_row.i_table = subst->i_new_table;
          if_null_row.i_column = -99;
          if_null_row.flags = EP_IfNullRow;
          copy = &if_null_row;
        }
        testcase( EXPR_HAS_PROPERTY(copy, EP_Subquery) );
        p_new = __expr_dup(db, copy, 0);
        if( db->malloc_failed ){
          __expr_delete(db, p_new);
          return expr;
        }
        if( subst->is_outer_join ){
          EXPR_SET_PROPERTY(p_new, EP_CanBeNull);
        }
        if( p_new->op==TK_TRUEFALSE ){
          p_new->u.i_value = __expr_truth_value(p_new);
          p_new->op = TK_INTEGER;
          EXPR_SET_PROPERTY(p_new, EP_IntValue);
        }

        /* Ensure that the expression now has an implicit collation sequence,
        ** just as it did when it was a column of a view or sub-query. */
        {
          coll_seq_t *nat = __expr_coll_seq(subst->parser, p_new);
          coll_seq_t *coll = __expr_coll_seq(subst->parser,
                subst->c_list->a[i_column].expr
          );
          if( nat!=coll || (p_new->op!=TK_COLUMN && p_new->op!=TK_COLLATE) ){
            p_new = __expr_add_collate_string(subst->parser, p_new,
                (coll ? coll->name : "BINARY")
            );
          }
        }
        EXPR_CLEAR_PROPERTY(p_new, EP_Collate);
        if( EXPR_HAS_PROPERTY(expr,EP_OuterON|EP_InnerON) ){
          __set_join_expr(p_new, expr->w.i_join,
                             expr->flags & (EP_OuterON|EP_InnerON));
        }
        __expr_delete(db, expr);
        expr = p_new;
      }
    }
  }else{
    if( expr->op==TK_IF_NULL_ROW && expr->i_table==subst->i_table ){
      expr->i_table = subst->i_new_table;
    }
    if( expr->op==TK_AGG_FUNCTION && expr->op2>=subst->n_sel_depth ){
      expr->op2--;
    }
    expr->left = __subst_expr(subst, expr->left);
    expr->right = __subst_expr(subst, expr->right);
    if( EXPR_USE_X_SELECT(expr) ){
      __subst_select(subst, expr->x.p_select, 1);
    }else{
      __subst_expr_list(subst, expr->x.list);
    }
#ifndef DBSQL_OMIT_WINDOWFUNC
    if( EXPR_HAS_PROPERTY(expr, EP_WinFunc) ){
      window_t *win = expr->y.win;
      win->filter = __subst_expr(subst, win->filter);
      __subst_expr_list(subst, win->partition);
      __subst_expr_list(subst, win->order_by);
    }
#endif
  }
  return expr;
}
static void __subst_expr_list(
  subst_context_t *subst, /* Description of the substitution */
  expr_list_t *list       /* List to scan and in which to make substitutes */
){
  int i;
  if( list==0 ) return;
  for(i=0; i<list->n_expr; i++){
    list->a[i].expr = __subst_expr(subst, list->a[i].expr);
  }
}
static void __subst_select(
  subst_context_t *subst, /* Description of the substitution */
  select_t *p,            /* SELECT statement in which to make substitutions */
  int do_prior           /* Do substitutes on p->prior too */
){
  src_list_t *src;
  src_item_t *item;
  int i;
  if( !p ) return;
  subst->n_sel_depth++;
  do{
    __subst_expr_list(subst, p->e_list);
    __subst_expr_list(subst, p->group_by);
    __subst_expr_list(subst, p->order_by);
    p->having = __subst_expr(subst, p->having);
    p->where = __subst_expr(subst, p->where);
    src = p->src;
    assert( src!=0 );
    for(i=src->n_src, item=src->a; i>0; i--, item++){
      if( item->fg.is_subquery ){
        __subst_select(subst, item->u4.subq->p_select, 1);
      }
      if( item->fg.is_tab_func ){
        __subst_expr_list(subst, item->u1.func_arg);
      }
    }
  }while( do_prior && (p = p->prior)!=0 );
  subst->n_sel_depth--;
}
#endif /* !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW) */

#if !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW)
/*
** p_select is a SELECT statement and src_item is one item in the FROM
** clause of that SELECT.
**
** This routine scans the entire SELECT statement and recomputes the
** src_item->col_used mask.
*/
static int __recompute_columns_used_expr(walker_t *walker, expr_t *expr){
  src_item_t *item;
  if( expr->op!=TK_COLUMN ) return WRC_Continue;
  item = walker->u.src_item;
  if( item->i_cursor!=expr->i_table ) return WRC_Continue;
  if( expr->i_column<0 ) return WRC_Continue;
  item->col_used |= __expr_col_used(expr);
  return WRC_Continue;
}
static void __recompute_columns_used(
  select_t *p_select,                 /* The complete SELECT statement */
  src_item_t *src_item                /* Which FROM clause item to recompute */
){
  walker_t w;
  if( NEVER(src_item->s_tab==0) ) return;
  memset(&w, 0, sizeof(w));
  w.expr_callback = __recompute_columns_used_expr;
  w.select_callback = __select_walk_noop;
  w.u.src_item = src_item;
  src_item->col_used = 0;
  __walk_select(&w, p_select);
}
#endif /* !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW) */

#if !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW)
/*
** Assign new cursor numbers to each of the items in src. For each
** new cursor number assigned, set an entry in the csr_maps[] array
** to map the old cursor number to the new:
**
**     csr_maps[i_old+1] = i_new;
**
** The array is guaranteed by the caller to be large enough for all
** existing cursor numbers in src.  csr_maps[0] is the array size.
**
** If src contains any sub-selects, call this routine recursively
** on the FROM clause of each such sub-select, with i_except set to -1.
*/
static void __srclist_renumber_cursors(
  parser_t *parser,                  /* parser_t context */
  int *csr_maps,                   /* Array to store cursor mappings in */
  src_list_t *src,                  /* FROM clause to renumber */
  int i_except                     /* FROM clause item to skip */
){
  int i;
  src_item_t *item;
  for(i=0, item=src->a; i<src->n_src; i++, item++){
    if( i!=i_except ){
      select_t *p;
      assert( item->i_cursor < csr_maps[0] );
      if( !item->fg.is_recursive || csr_maps[item->i_cursor+1]==0 ){
        csr_maps[item->i_cursor+1] = parser->n_tab++;
      }
      item->i_cursor = csr_maps[item->i_cursor+1];
      if( item->fg.is_subquery ){
        for(p=item->u4.subq->p_select; p; p=p->prior){
          __srclist_renumber_cursors(parser, csr_maps, p->src, -1);
        }
      }
    }
  }
}

/*
** *pi_cursor is a cursor number.  Change it if it needs to be mapped.
*/
static void __renumber_cursor_do_mapping(walker_t *walker, int *pi_cursor){
  int *csr_maps = walker->u.cols;
  int i_csr = *pi_cursor;
  if( i_csr < csr_maps[0] && csr_maps[i_csr+1]>0 ){
    *pi_cursor = csr_maps[i_csr+1];
  }
}

/*
** Expression walker callback used by __renumber_cursors() to update
** expr_t objects to match newly assigned cursor numbers.
*/
static int __renumber_cursors_cb(walker_t *walker, expr_t *expr){
  int op = expr->op;
  if( op==TK_COLUMN || op==TK_IF_NULL_ROW ){
    __renumber_cursor_do_mapping(walker, &expr->i_table);
  }
  if( EXPR_HAS_PROPERTY(expr, EP_OuterON) ){
    __renumber_cursor_do_mapping(walker, &expr->w.i_join);
  }
  return WRC_Continue;
}

/*
** Assign a new cursor number to each cursor in the FROM clause (select_t.src)
** of the SELECT statement passed as the second argument, and to each
** cursor in the FROM clause of any FROM clause sub-selects, recursively.
** Except, do not assign a new cursor number to the i_except'th element in
** the FROM clause of (*p). Update all expressions and other references
** to refer to the new cursor numbers.
**
** Argument csr_maps is an array that may be used for temporary working
** space. Two guarantees are made by the caller:
**
**   * the array is larger than the largest cursor number used within the
**     select statement passed as an argument, and
**
**   * the array entries for all cursor numbers that do *not* appear in
**     FROM clauses of the select statement as described above are
**     initialized to zero.
*/
static void __renumber_cursors(
  parser_t *parser,                  /* parser_t context */
  select_t *p,                      /* select_t to renumber cursors within */
  int i_except,                    /* FROM clause item to skip */
  int *csr_maps                    /* Working space */
){
  walker_t w;
  __srclist_renumber_cursors(parser, csr_maps, p->src, i_except);
  memset(&w, 0, sizeof(w));
  w.u.cols = csr_maps;
  w.expr_callback = __renumber_cursors_cb;
  w.select_callback = __select_walk_noop;
  __walk_select(&w, p);
}
#endif /* !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW) */

/*
** If sel is not part of a compound SELECT, return a pointer to its
** expression list. Otherwise, return a pointer to the expression list
** of the leftmost SELECT in the compound.
*/
static expr_list_t *__find_leftmost_exprlist(select_t *sel){
  while( sel->prior ){
    sel = sel->prior;
  }
  return sel->e_list;
}

/*
** Return true if any of the result-set columns in the compound query
** have incompatible affinities on one or more arms of the compound.
*/
static int __compound_has_different_affinities(select_t *p){
  int ii;
  expr_list_t *list;
  assert( p!=0 );
  assert( p->e_list!=0 );
  assert( p->prior!=0 );
  list = p->e_list;
  for(ii=0; ii<list->n_expr; ii++){
    char aff;
    select_t *sub1;
    assert( list->a[ii].expr!=0 );
    aff = __expr_affinity(list->a[ii].expr);
    for(sub1=p->prior; sub1; sub1=sub1->prior){
      assert( sub1->e_list!=0 );
      assert( sub1->e_list->n_expr>ii );
      assert( sub1->e_list->a[ii].expr!=0 );
      if( __expr_affinity(sub1->e_list->a[ii].expr)!=aff ){
        return 1;
      }
    }
  }
  return 0;
}

#if !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW)
/*
** This routine attempts to flatten subqueries as a performance optimization.
** This routine returns 1 if it makes changes and 0 if no flattening occurs.
**
** To understand the concept of flattening, consider the following
** query:
**
**     SELECT a FROM (SELECT x+y AS a FROM t1 WHERE z<100) WHERE a>5
**
** The default way of implementing this query is to execute the
** subquery first and store the results in a temporary table, then
** run the outer query on that temporary table.  This requires two
** passes over the data.  Furthermore, because the temporary table
** has no indices, the WHERE clause on the outer query cannot be
** optimized.
**
** This routine attempts to rewrite queries such as the above into
** a single flat select, like this:
**
**     SELECT x+y AS a FROM t1 WHERE z<100 AND a>5
**
** The code generated for this simplification gives the same result
** but only has to scan the data once.  And because indices might
** exist on the table t1, a complete scan of the data might be
** avoided.
**
** Flattening is subject to the following constraints:
**
**  (**)  We no longer attempt to flatten aggregate subqueries. Was:
**        The subquery and the outer query cannot both be aggregates.
**
**  (**)  We no longer attempt to flatten aggregate subqueries. Was:
**        (2) If the subquery is an aggregate then
**        (2a) the outer query must not be a join and
**        (2b) the outer query must not use subqueries
**             other than the one FROM-clause subquery that is a candidate
**             for flattening.  (This is due to ticket [2f7170d73bf9abf80]
**             from 2015-02-09.)
**
**   (3)  If the subquery is the right operand of a LEFT JOIN then
**        (3a) the subquery may not be a join
**        (**) Was (3b): "the FROM clause of the subquery may not contain
**             a virtual table"
**        (**) Was: "The outer query may not have a GROUP BY." This case
**             is now managed correctly
**        (3d) the outer query may not be DISTINCT.
**        See also (26) for restrictions on RIGHT JOIN.
**
**   (4)  The subquery can not be DISTINCT.
**
**  (**)  At one point restrictions (4) and (5) defined a subset of DISTINCT
**        sub-queries that were excluded from this optimization. Restriction
**        (4) has since been expanded to exclude all DISTINCT subqueries.
**
**  (**)  We no longer attempt to flatten aggregate subqueries.  Was:
**        If the subquery is aggregate, the outer query may not be DISTINCT.
**
**   (7)  The subquery must have a FROM clause.  TODO:  For subqueries without
**        A FROM clause, consider adding a FROM clause with the special
**        table dbsql_once that consists of a single row containing a
**        single NULL.
**
**   (8)  If the subquery uses LIMIT then the outer query may not be a join.
**
**   (9)  If the subquery uses LIMIT then the outer query may not be aggregate.
**
**  (**)  Restriction (10) was removed from the code on 2005-02-05 but we
**        accidentally carried the comment forward until 2014-09-15.  Original
**        constraint: "If the subquery is aggregate then the outer query
**        may not use LIMIT."
**
**  (11)  The subquery and the outer query may not both have ORDER BY clauses.
**
**  (**)  Not implemented.  Subsumed into restriction (3).  Was previously
**        a separate restriction deriving from ticket #350.
**
**  (13)  The subquery and outer query may not both use LIMIT.
**
**  (14)  The subquery may not use OFFSET.
**
**  (15)  If the outer query is part of a compound select, then the
**        subquery may not use LIMIT.
**        (See ticket #2339 and ticket [02a8e81d44]).
**
**  (16)  If the outer query is aggregate, then the subquery may not
**        use ORDER BY.  (Ticket #2942)  This used to not matter
**        until we introduced the group_concat() function. 
**
**  (17)  If the subquery is a compound select, then
**        (17a) all compound operators must be a UNION ALL, and
**        (17b) no terms within the subquery compound may be aggregate
**              or DISTINCT, and
**        (17c) every term within the subquery compound must have a FROM clause
**        (17d) the outer query may not be
**              (17d1) aggregate, or
**              (17d2) DISTINCT
**        (17e) the subquery may not contain window functions, and
**        (17f) the subquery must not be the RHS of a LEFT JOIN.
**        (17g) either the subquery is the first element of the outer
**              query or there are no RIGHT or FULL JOINs in any arm
**              of the subquery.  (This is a duplicate of condition (27b).)
**        (17h) The corresponding result set expressions in all arms of the
**              compound must have the same affinity.
**
**        The parent and sub-query may contain WHERE clauses. Subject to
**        rules (11), (13) and (14), they may also contain ORDER BY,
**        LIMIT and OFFSET clauses.  The subquery cannot use any compound
**        operator other than UNION ALL because all the other compound
**        operators have an implied DISTINCT which is disallowed by
**        restriction (4).
**
**        Also, each component of the sub-query must return the same number
**        of result columns. This is actually a requirement for any compound
**        SELECT statement, but all the code here does is make sure that no
**        such (illegal) sub-query is flattened. The caller will detect the
**        syntax error and return a detailed message.
**
**  (18)  If the sub-query is a compound select, then all terms of the
**        ORDER BY clause of the parent must be copies of a term returned
**        by the parent query.
**
**  (19)  If the subquery uses LIMIT then the outer query may not
**        have a WHERE clause.
**
**  (20)  If the sub-query is a compound select, then it must not use
**        an ORDER BY clause.  Ticket #3773.  We could relax this constraint
**        somewhat by saying that the terms of the ORDER BY clause must
**        appear as unmodified result columns in the outer query.  But we
**        have other optimizations in mind to deal with that case.
**
**  (21)  If the subquery uses LIMIT then the outer query may not be
**        DISTINCT.  (See ticket [752e1646fc]).
**
**  (22)  The subquery may not be a recursive CTE.
**
**  (23)  If the outer query is a recursive CTE, then the sub-query may not be
**        a compound query.  This restriction is because transforming the
**        parent to a compound query confuses the code that handles
**        recursive queries in __multi_select().
**
**  (**)  We no longer attempt to flatten aggregate subqueries.  Was:
**        The subquery may not be an aggregate that uses the built-in min() or
**        or max() functions.  (Without this restriction, a query like:
**        "SELECT x FROM (SELECT max(y), x FROM t1)" would not necessarily
**        return the value X for which Y was maximal.)
**
**  (25)  If either the subquery or the parent query contains a window
**        function in the select list or ORDER BY clause, flattening
**        is not attempted.
**
**  (26)  The subquery may not be the right operand of a RIGHT JOIN.
**        See also (3) for restrictions on LEFT JOIN.
**
**  (27)  The subquery may not contain a FULL or RIGHT JOIN unless it
**        is the first element of the parent query.  Two subcases:
**        (27a) the subquery is not a compound query.
**        (27b) the subquery is a compound query and the RIGHT JOIN occurs
**              in any arm of the compound query.  (See also (17g).)
**
**  (28)  The subquery is not a MATERIALIZED CTE.  (This is handled
**        in the caller before ever reaching this routine.)
**
**
** In this routine, the "p" parameter is a pointer to the outer query.
** The subquery is p->src->a[i_from].  is_agg is true if the outer query
** uses aggregates.
**
** If flattening is not attempted, this routine is a no-op and returns 0.
** If flattening is attempted this routine returns 1.
**
** All of the expression analysis must occur on both the outer query and
** the subquery before this routine runs.
*/
static int __flatten_subquery(
  parser_t *parser,       /* Parsing context */
  select_t *p,           /* The parent or outer SELECT statement */
  int i_from,           /* index_t in p->src->a[] of the inner subquery */
  int is_agg            /* True if outer SELECT uses aggregate functions */
){
  const char *saved_auth_context = parser->auth_context;
  select_t *parent;    /* Current UNION ALL term of the other query */
  select_t *sub;       /* The inner query or "subquery" */
  select_t *sub1;      /* Pointer to the rightmost select in sub-query */
  src_list_t *src;      /* The FROM clause of the outer query */
  src_list_t *sub_src;   /* The FROM clause of the subquery */
  int i_parent;        /* VDBE cursor number of the sub result set temp table */
  int i_new_parent = -1;/* Replacement table for i_parent */
  int is_outer_join = 0; /* True if sub is the right side of a LEFT JOIN */   
  int i;              /* Loop counter */
  expr_t *where;                    /* The WHERE clause */
  src_item_t *subitem;               /* The subquery */
  DBSQL *db = parser->db;
  walker_t w;                        /* walker_t to persist agginfo data */
  int *csr_maps = 0;

  /* Check to see if flattening is permitted.  Return 0 if not.
  */
  assert( p!=0 );
  assert( p->prior==0 );
  if( OPTIMIZATION_DISABLED(db, DBSQL_QueryFlattener) ) return 0;
  src = p->src;
  assert( src && i_from>=0 && i_from<src->n_src );
  subitem = &src->a[i_from];
  i_parent = subitem->i_cursor;
  assert( subitem->fg.is_subquery );
  sub = subitem->u4.subq->p_select;
  assert( sub!=0 );

#ifndef DBSQL_OMIT_WINDOWFUNC
  if( p->win || sub->win ) return 0;                  /* Restriction (25) */
#endif

  sub_src = sub->src;
  assert( sub_src );
  /* Prior to version 3.1.2, when LIMIT and OFFSET had to be simple constants,
  ** not arbitrary expressions, we allowed some combining of LIMIT and OFFSET
  ** because they could be computed at compile-time.  But when LIMIT and OFFSET
  ** became arbitrary expressions, we were forced to add restrictions (13)
  ** and (14). */
  if( sub->limit && p->limit ) return 0;              /* Restriction (13) */
  if( sub->limit && sub->limit->right ) return 0;   /* Restriction (14) */
  if( (p->sel_flags & SF_Compound)!=0 && sub->limit ){
    return 0;                                            /* Restriction (15) */
  }
  if( sub_src->n_src==0 ) return 0;                       /* Restriction (7)  */
  if( sub->sel_flags & SF_Distinct ) return 0;           /* Restriction (4)  */
  if( sub->limit && (src->n_src>1 || is_agg) ){
     return 0;         /* Restrictions (8)(9) */
  }
  if( p->order_by && sub->order_by ){
     return 0;                                           /* Restriction (11) */
  }
  if( is_agg && sub->order_by ) return 0;                /* Restriction (16) */
  if( sub->limit && p->where ) return 0;              /* Restriction (19) */
  if( sub->limit && (p->sel_flags & SF_Distinct)!=0 ){
     return 0;         /* Restriction (21) */
  }
  if( sub->sel_flags & (SF_Recursive) ){
    return 0; /* Restrictions (22) */
  }

  /*
  ** If the subquery is the right operand of a LEFT JOIN, then the
  ** subquery may not be a join itself (3a). Example of why this is not
  ** allowed:
  **
  **         t1 LEFT OUTER JOIN (t2 JOIN t3)
  **
  ** If we flatten the above, we would get
  **
  **         (t1 LEFT OUTER JOIN t2) JOIN t3
  **
  ** which is not at all the same thing.
  **
  ** See also tickets #306, #350, and #3300.
  */
  if( (subitem->fg.jointype & (JT_OUTER|JT_LTORJ))!=0 ){
    if( sub_src->n_src>1                        /* (3a) */
     /**** || IS_VIRTUAL(sub_src->a[0].s_tab)      (3b)-omitted */
     || (p->sel_flags & SF_Distinct)!=0         /* (3d) */
     || (subitem->fg.jointype & JT_RIGHT)!=0  /* (26) */
    ){
      return 0;
    }
    is_outer_join = 1;
  }

  assert( sub_src->n_src>0 );  /* True by restriction (7) */
  if( i_from>0 && (sub_src->a[0].fg.jointype & JT_LTORJ)!=0 ){
    return 0;   /* Restriction (27a) */
  }

  /* Condition (28) is blocked by the caller */
  assert( !subitem->fg.is_cte || subitem->u2.cte_use->m10d!=M10D_YES );

  /* Restriction (17): If the sub-query is a compound SELECT, then it must
  ** use only the UNION ALL operator. And none of the simple select queries
  ** that make up the compound SELECT are allowed to be aggregate or distinct
  ** queries.
  */
  if( sub->prior ){
    int ii;
    if( sub->order_by ){
      return 0;  /* Restriction (20) */
    }
    if( is_agg || (p->sel_flags & SF_Distinct)!=0 || is_outer_join>0 ){
      return 0; /* (17d1), (17d2), or (17f) */
    }
    for(sub1=sub; sub1; sub1=sub1->prior){
      testcase( (sub1->sel_flags & (SF_Distinct|SF_Aggregate))==SF_Distinct );
      testcase( (sub1->sel_flags & (SF_Distinct|SF_Aggregate))==SF_Aggregate );
      assert( sub->src!=0 );
      assert( (sub->sel_flags & SF_Recursive)==0 );
      assert( sub->e_list->n_expr==sub1->e_list->n_expr );
      if( (sub1->sel_flags & (SF_Distinct|SF_Aggregate))!=0    /* (17b) */
       || (sub1->prior && sub1->op!=TK_ALL)                 /* (17a) */
       || sub1->src->n_src<1                                  /* (17c) */
#ifndef DBSQL_OMIT_WINDOWFUNC
       || sub1->win                                          /* (17e) */
#endif
      ){
        return 0;
      }
      if( i_from>0 && (sub1->src->a[0].fg.jointype & JT_LTORJ)!=0 ){
        /* Without this restriction, the JT_LTORJ flag would end up being
        ** omitted on left-hand tables of the right join that is being
        ** flattened. */
        return 0;   /* Restrictions (17g), (27b) */
      }
      testcase( sub1->src->n_src>1 );
    }

    /* Restriction (18). */
    if( p->order_by ){
      for(ii=0; ii<p->order_by->n_expr; ii++){
        if( p->order_by->a[ii].u.x.i_order_by_col==0 ) return 0;
      }
    }

    /* Restriction (23) */
    if( (p->sel_flags & SF_Recursive) ) return 0;

    /* Restriction (17h) */
    if( __compound_has_different_affinities(sub) ) return 0;

    if( src->n_src>1 ){
      if( parser->n_select>500 ) return 0;
      if( OPTIMIZATION_DISABLED(db, DBSQL_FlttnUnionAll) ) return 0;
      csr_maps = __db_malloc_zero(db, ((int64_t)parser->n_tab+1)*sizeof(int));
      if( csr_maps ) csr_maps[0] = parser->n_tab;
    }
  }

  /***** If we reach this point, flattening is permitted. *****/
  TREETRACE(0x4,parser,p,("flatten %u.%p from term %d\n",
                   sub->sel_id, sub, i_from));

  /* Authorize the subquery */
  parser->auth_context = subitem->name;
  TESTONLY(i =) __auth_check(parser, DBSQL_SELECT, 0, 0, 0);
  testcase( i==DBSQL_DENY );
  parser->auth_context = saved_auth_context;

  /* Delete the transient structures associated with the subquery */
  
  if( ALWAYS(subitem->fg.is_subquery) ){
    sub1 = __subquery_detach(db, subitem);
  }else{
    sub1 = 0;
  }
  assert( subitem->fg.is_subquery==0 );
  assert( subitem->fg.fixed_schema==0 );
  __dbsql_db_free(db, subitem->name);
  __dbsql_db_free(db, subitem->alias);
  subitem->name = 0;
  subitem->alias = 0;
  assert( subitem->fg.is_using!=0 || subitem->u3.on==0 );

  /* If the sub-query is a compound SELECT statement, then (by restrictions
  ** 17 and 18 above) it must be a UNION ALL and the parent query must
  ** be of the form:
  **
  **     SELECT <expr-list> FROM (<sub-query>) <where-clause>
  **
  ** followed by any ORDER BY, LIMIT and/or OFFSET clauses. This block
  ** creates N-1 copies of the parent query without any ORDER BY, LIMIT or
  ** OFFSET clauses and joins them to the left-hand-side of the original
  ** using UNION ALL operators. In this case N is the number of simple
  ** select statements in the compound sub-query.
  **
  ** Example:
  **
  **     SELECT a+1 FROM (
  **        SELECT x FROM tab
  **        UNION ALL
  **        SELECT y FROM tab
  **        UNION ALL
  **        SELECT abs(z*2) FROM tab2
  **     ) WHERE a!=5 ORDER BY 1
  **
  ** Transformed into:
  **
  **     SELECT x+1 FROM tab WHERE x+1!=5
  **     UNION ALL
  **     SELECT y+1 FROM tab WHERE y+1!=5
  **     UNION ALL
  **     SELECT abs(z*2)+1 FROM tab2 WHERE abs(z*2)+1!=5
  **     ORDER BY 1
  **
  ** We call this the "compound-subquery flattening".
  */
  for(sub=sub->prior; sub; sub=sub->prior){
    select_t *p_new;
    expr_list_t *order_by = p->order_by;
    expr_t *limit = p->limit;
    select_t *prior = p->prior;
    table_t *item_tab = subitem->s_tab;
    subitem->s_tab = 0;
    p->order_by = 0;
    p->prior = 0;
    p->limit = 0;
    p_new = __select_dup(db, p, 0);
    p->limit = limit;
    p->order_by = order_by;
    p->op = TK_ALL;
    subitem->s_tab = item_tab;
    if( p_new==0 ){
      p->prior = prior;
    }else{
      p_new->sel_id = ++parser->n_select;
      if( csr_maps && ALWAYS(db->malloc_failed==0) ){
        __renumber_cursors(parser, p_new, i_from, csr_maps);
      }
      p_new->prior = prior;
      if( prior ) prior->next = p_new;
      p_new->next = p;
      p->prior = p_new;
      TREETRACE(0x4,parser,p,("compound-subquery flattener"
                              " creates %u as peer\n",p_new->sel_id));
    }
    assert( subitem->fg.is_subquery==0 );
  }
  __dbsql_db_free(db, csr_maps);
  if( db->malloc_failed ){
    assert( subitem->fg.fixed_schema==0 );
    assert( subitem->fg.is_subquery==0 );
    assert( subitem->u4.database==0 );
    __src_item_attach_subquery(parser, subitem, sub1, 0);
    return 1;
  }

  /* Defer deleting the table_t object associated with the
  ** subquery until code generation is
  ** complete, since there may still exist expr_t.tab entries that
  ** refer to the subquery even after flattening.  Ticket #3346.
  **
  ** subitem->s_tab is always non-NULL by test restrictions and tests above.
  */
  if( ALWAYS(subitem->s_tab!=0) ){
    table_t *tab_to_del = subitem->s_tab;
    if( tab_to_del->n_tab_ref==1 ){
      parser_t *toplevel = __parse_toplevel(parser);
      __parser_add_cleanup(toplevel, __delete_table_generic, tab_to_del);
      testcase( toplevel->early_cleanup );
    }else{
      tab_to_del->n_tab_ref--;
    }
    subitem->s_tab = 0;
  }

  /* The following loop runs once for each term in a compound-subquery
  ** flattening (as described above).  If we are doing a different kind
  ** of flattening - a flattening other than a compound-subquery flattening -
  ** then this loop only runs once.
  **
  ** This loop moves all of the FROM elements of the subquery into the
  ** the FROM clause of the outer query.  Before doing this, remember
  ** the cursor number for the original outer query FROM element in
  ** i_parent.  The i_parent cursor will never be used.  Subsequent code
  ** will scan expressions looking for i_parent references and replace
  ** those references with expressions that resolve to the subquery FROM
  ** elements we are now copying in.
  */
  sub = sub1;
  for(parent=p; parent; parent=parent->prior, sub=sub->prior){
    int n_sub_src;
    u_int8_t jointype = subitem->fg.jointype;
    assert( sub!=0 );
    sub_src = sub->src;     /* FROM clause of subquery */
    n_sub_src = sub_src->n_src;  /* Number of terms in subquery FROM clause */
    src = parent->src;     /* FROM clause of the outer query */

    /* The subquery uses a single slot of the FROM clause of the outer
    ** query.  If the subquery has more than one element in its FROM clause,
    ** then expand the outer query to make space for it to hold all elements
    ** of the subquery.
    **
    ** Example:
    **
    **    SELECT * FROM tab_a, (SELECT * FROM sub1, sub2), tab_b;
    **
    ** The outer query has 3 slots in its FROM clause.  One slot of the
    ** outer query (the middle slot) is used by the subquery.  The next
    ** block of code will expand the outer query FROM clause to 4 slots.
    ** The middle slot is expanded to two slots in order to make space
    ** for the two elements in the FROM clause of the subquery.
    */
    if( n_sub_src>1 ){
      src = __src_list_enlarge(parser, src, n_sub_src-1,i_from+1);
      if( src==0 ) break;
      parent->src = src;
      subitem = &src->a[i_from];
    }

    /* Transfer the FROM clause terms from the subquery into the
    ** outer query.
    */
    i_new_parent = sub_src->a[0].i_cursor;
    for(i=0; i<n_sub_src; i++){
      src_item_t *item = &src->a[i+i_from];
      assert( item->fg.is_tab_func==0 );
      assert( item->fg.is_subquery
           || item->fg.fixed_schema
           || item->u4.database==0 );
      if( item->fg.is_using ) __id_list_delete(db, item->u3.p_using);
      *item = sub_src->a[i];
      item->fg.jointype |= (jointype & JT_LTORJ);
      memset(&sub_src->a[i], 0, sizeof(sub_src->a[i]));
    }
    subitem->fg.jointype |= jointype;
 
    /* Begin substituting subquery result set expressions for
    ** references to the i_parent in the outer query.
    **
    ** Example:
    **
    **   SELECT a+5, b*10 FROM (SELECT x*3 AS a, y+10 AS b FROM t1) WHERE a>b;
    **   \                     \_____________ subquery __________/          /
    **    \_____________________ outer query ______________________________/
    **
    ** We look at every expression in the outer query and every place we see
    ** "a" we substitute "x*3" and every place we see "b" we substitute "y+10".
    */
    if( sub->order_by ){
      /* At this point, any non-zero i_order_by_col values indicate that the
      ** ORDER BY column expression is identical to the i_order_by_col'th
      ** expression returned by SELECT statement sub. Since these values
      ** do not necessarily correspond to columns in SELECT statement parent,
      ** zero them before transferring the ORDER BY clause.
      **
      ** Not doing this may cause an error if a subsequent call to this
      ** function attempts to flatten a compound sub-query into parent.
      ** See ticket [d11a6e908f].
      */
      expr_list_t *order_by = sub->order_by;
      for(i=0; i<order_by->n_expr; i++){
        order_by->a[i].u.x.i_order_by_col = 0;
      }
      assert( parent->order_by==0 );
      parent->order_by = order_by;
      sub->order_by = 0;
    }
    where = sub->where;
    sub->where = 0;
    if( is_outer_join>0 ){
      assert( sub_src->n_src==1 );
      __set_join_expr(where, i_new_parent, EP_OuterON);
    }
    if( where ){
      if( parent->where ){
        parent->where = __p_expr(parser, TK_AND, where, parent->where);
      }else{
        parent->where = where;
      }
    }
    if( db->malloc_failed==0 ){
      subst_context_t x;
      x.parser = parser;
      x.i_table = i_parent;
      x.i_new_table = i_new_parent;
      x.is_outer_join = is_outer_join;
      x.n_sel_depth = 0;
      x.e_list = sub->e_list;
      x.c_list = __find_leftmost_exprlist(sub);
      __subst_select(&x, parent, 0);
    }
 
    /* The flattened query is a compound if either the inner or the
    ** outer query is a compound. */
    parent->sel_flags |= sub->sel_flags & SF_Compound;
    assert( (sub->sel_flags & SF_Distinct)==0 ); /* restriction (17b) */
 
    /*
    ** SELECT ... FROM (SELECT ... LIMIT a OFFSET b) LIMIT x OFFSET y;
    **
    ** One is tempted to try to add a and b to combine the limits.  But this
    ** does not work if either limit is negative.
    */
    if( sub->limit ){
      parent->limit = sub->limit;
      sub->limit = 0;
    }

    /* Recompute the src_item_t.col_used masks for the flattened
    ** tables. */
    for(i=0; i<n_sub_src; i++){
      __recompute_columns_used(parent, &src->a[i+i_from]);
    }
  }

  /* Finally, delete what is left of the subquery and return success.
  */
  __agg_info_persist_walker_init(&w, parser);
  __walk_select(&w,sub1);
  __select_delete(db, sub1);

#if TREETRACE_ENABLED
  if( __tree_trace & 0x4 ){
    TREETRACE(0x4,parser,p,("After flattening:\n"));
    __tree_view_select(0, p, 0);
  }
#endif

  return 1;
}
#endif /* !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW) */

/*
** A structure to keep track of all of the column values that are fixed to
** a known value due to WHERE clause constraints of the form COLUMN=VALUE.
*/
typedef struct where_const_t where_const_t;
struct where_const_t {
  parser_t *parser;   /* Parsing context */
  u_int8_t *oom_fault;   /* Pointer to parser->db->malloc_failed */
  int n_const;      /* Number for COLUMN=CONSTANT terms */
  int n_chng;       /* Number of times a constant is propagated */
  int has_aff_blob; /* At least one column in exprs[] as affinity BLOB */
  u_int32_t m_exclude_on;  /* Which ON expressions to exclude from considertion.
                   ** Either EP_OuterON or EP_InnerON|EP_OuterON */
  expr_t **exprs;   /* [i*2] is COLUMN and [i*2+1] is VALUE */
};

/*
** Add a new entry to the p_const object.  Except, do not add duplicate
** column entries.  Also, do not add if doing so would not be appropriate.
**
** The caller guarantees the column is a column and value is a constant.
** This routine has to do some additional checks before completing the
** insert.
*/
static void __const_insert(
  where_const_t *p_const,  /* The where_const_t into which we are inserting */
  expr_t *column,       /* The COLUMN part of the constraint */
  expr_t *value,        /* The VALUE part of the constraint */
  expr_t *expr          /* Overall expression: COLUMN=VALUE or VALUE=COLUMN */
){
  int i;
  assert( column->op==TK_COLUMN );
  assert( __expr_is_constant(p_const->parser, value) );

  if( EXPR_HAS_PROPERTY(column, EP_FixedCol) ) return;
  if( __expr_affinity(value)!=0 ) return;
  if( !__is_binary(__expr_compare_coll_seq(p_const->parser,expr)) ){
    return;
  }

  /* 2018-10-25 ticket [cf5ed20f]
  ** Make sure the same column is not inserted more than once */
  for(i=0; i<p_const->n_const; i++){
    const expr_t *e2 = p_const->exprs[i*2];
    assert( e2->op==TK_COLUMN );
    if( e2->i_table==column->i_table
     && e2->i_column==column->i_column
    ){
      return;  /* Already present.  Return without doing anything. */
    }
  }
  assert( DBSQL_AFF_NONE<DBSQL_AFF_BLOB );
  if( __expr_affinity(column)<=DBSQL_AFF_BLOB ){
    p_const->has_aff_blob = 1;
  }

  p_const->n_const++;
  p_const->exprs = __db_realloc_or_free(p_const->parser->db, p_const->exprs,
                         p_const->n_const*2*sizeof(expr_t*));
  if( p_const->exprs==0 ){
    p_const->n_const = 0;
  }else{
    p_const->exprs[p_const->n_const*2-2] = column;
    p_const->exprs[p_const->n_const*2-1] = value;
  }
}

/*
** Find all terms of COLUMN=VALUE or VALUE=COLUMN in expr where VALUE
** is a constant expression and where the term must be true because it
** is part of the AND-connected terms of the expression.  For each term
** found, add it to the p_const structure.
*/
static void __find_const_in_where(where_const_t *p_const, expr_t *expr){
  expr_t *right, *left;
  if( NEVER(expr==0) ) return;
  if( EXPR_HAS_PROPERTY(expr, p_const->m_exclude_on) ){
    testcase( EXPR_HAS_PROPERTY(expr, EP_OuterON) );
    testcase( EXPR_HAS_PROPERTY(expr, EP_InnerON) );
    return;
  }
  if( expr->op==TK_AND ){
    __find_const_in_where(p_const, expr->right);
    __find_const_in_where(p_const, expr->left);
    return;
  }
  if( expr->op!=TK_EQ ) return;
  right = expr->right;
  left = expr->left;
  assert( right!=0 );
  assert( left!=0 );
  if( right->op==TK_COLUMN && __expr_is_constant(p_const->parser, left) ){
    __const_insert(p_const,right,left,expr);
  }
  if( left->op==TK_COLUMN && __expr_is_constant(p_const->parser, right) ){
    __const_insert(p_const,left,right,expr);
  }
}

/*
** This is a helper function for walker_t callback __propagate_constant_expr_rewrite().
**
** Argument expr is a candidate expression to be replaced by a value. If
** expr is equivalent to one of the columns named in walker->u.p_const,
** then overwrite it with the corresponding value. Except, do not do so
** if argument ignore_aff_blob is non-zero and the affinity of expr
** is DBSQL_AFF_BLOB.
*/
static int __propagate_constant_expr_rewrite_one(
  where_const_t *p_const,
  expr_t *expr,
  int ignore_aff_blob
){
  int i;
  if( p_const->oom_fault[0] ) return WRC_Prune;
  if( expr->op!=TK_COLUMN ) return WRC_Continue;
  if( EXPR_HAS_PROPERTY(expr, EP_FixedCol|p_const->m_exclude_on) ){
    testcase( EXPR_HAS_PROPERTY(expr, EP_FixedCol) );
    testcase( EXPR_HAS_PROPERTY(expr, EP_OuterON) );
    testcase( EXPR_HAS_PROPERTY(expr, EP_InnerON) );
    return WRC_Continue;
  }
  for(i=0; i<p_const->n_const; i++){
    expr_t *column = p_const->exprs[i*2];
    if( column==expr ) continue;
    if( column->i_table!=expr->i_table ) continue;
    if( column->i_column!=expr->i_column ) continue;
    assert( DBSQL_AFF_NONE<DBSQL_AFF_BLOB );
    if( ignore_aff_blob && __expr_affinity(column)<=DBSQL_AFF_BLOB ){
      break;
    }
    /* A match is found.  Add the EP_FixedCol property */
    p_const->n_chng++;
    EXPR_CLEAR_PROPERTY(expr, EP_Leaf);
    EXPR_SET_PROPERTY(expr, EP_FixedCol);
    assert( expr->left==0 );
    expr->left = __expr_dup(p_const->parser->db, p_const->exprs[i*2+1], 0);
    if( p_const->parser->db->malloc_failed ) return WRC_Prune;
    break;
  }
  return WRC_Prune;
}

/*
** This is a walker_t expression callback. expr is a node from the WHERE
** clause of a SELECT statement. This function examines expr to see if
** any substitutions based on the contents of walker->u.p_const should
** be made to expr or its immediate children.
**
** A substitution is made if:
**
**   + expr is a column with an affinity other than BLOB that matches
**     one of the columns in walker->u.p_const, or
**
**   + expr is a binary comparison operator (=, <=, >=, <, >) that
**     uses an affinity other than TEXT and one of its immediate
**     children is a column that matches one of the columns in
**     walker->u.p_const.
*/
static int __propagate_constant_expr_rewrite(walker_t *walker, expr_t *expr){
  where_const_t *p_const = walker->u.p_const;
  assert( TK_GT==TK_EQ+1 );
  assert( TK_LE==TK_EQ+2 );
  assert( TK_LT==TK_EQ+3 );
  assert( TK_GE==TK_EQ+4 );
  if( p_const->has_aff_blob ){
    if( (expr->op>=TK_EQ && expr->op<=TK_GE)
     || expr->op==TK_IS
    ){
      __propagate_constant_expr_rewrite_one(p_const, expr->left, 0);
      if( p_const->oom_fault[0] ) return WRC_Prune;
      if( __expr_affinity(expr->left)!=DBSQL_AFF_TEXT ){
        __propagate_constant_expr_rewrite_one(p_const, expr->right, 0);
      }
    }
  }
  return __propagate_constant_expr_rewrite_one(p_const, expr, p_const->has_aff_blob);
}

/*
** The WHERE-clause constant propagation optimization.
**
** If the WHERE clause contains terms of the form COLUMN=CONSTANT or
** CONSTANT=COLUMN that are top-level AND-connected terms that are not
** part of a ON clause from a LEFT JOIN, then throughout the query
** replace all other occurrences of COLUMN with CONSTANT.
**
** For example, the query:
**
**      SELECT * FROM t1, t2, t3 WHERE t1.a=39 AND t2.b=t1.a AND t3.c=t2.b
**
** Is transformed into
**
**      SELECT * FROM t1, t2, t3 WHERE t1.a=39 AND t2.b=39 AND t3.c=39
**
** Return true if any transformations where made and false if not.
**
** Implementation note:  Constant propagation is tricky due to affinity
** and collating sequence interactions.  Consider this example:
**
**    CREATE TABLE t1(a INT,b TEXT);
**    INSERT INTO t1 VALUES(123,'0123');
**    SELECT * FROM t1 WHERE a=123 AND b=a;
**    SELECT * FROM t1 WHERE a=123 AND b=123;
**
** The two SELECT statements above should return different answers.  b=a
** is always true because the comparison uses numeric affinity, but b=123
** is false because it uses text affinity and '0123' is not the same as '123'.
** To work around this, the expression tree is not actually changed from
** "b=a" to "b=123" but rather the "a" in "b=a" is tagged with EP_FixedCol
** and the "123" value is hung off of the left pointer.  Code generator
** routines know to generate the constant "123" instead of looking up the
** column value.  Also, to avoid collation problems, this optimization is
** only attempted if the "a=123" term uses the default BINARY collation.
**
** 2021-05-25 forum post 6a06202608: Another troublesome case is...
**
**    CREATE TABLE t1(x);
**    INSERT INTO t1 VALUES(10.0);
**    SELECT 1 FROM t1 WHERE x=10 AND x LIKE 10;
**
** The query should return no rows, because the t1.x value is '10.0' not '10'
** and '10.0' is not LIKE '10'.  But if we are not careful, the first WHERE
** term "x=10" will cause the second WHERE term to become "10 LIKE 10",
** resulting in a false positive.  To avoid this, constant propagation for
** columns with BLOB affinity is only allowed if the constant is used with
** operators ==, <=, <, >=, >, or IS in a way that will cause the correct
** type conversions to occur.  See logic associated with the has_aff_blob flag
** for details.
*/
static int __propagate_constants(
  parser_t *parser,   /* The parsing context */
  select_t *p        /* The query in which to propagate constants */
){
  where_const_t x;
  walker_t w;
  int n_chng = 0;
  x.parser = parser;
  x.oom_fault = &parser->db->malloc_failed;
  do{
    x.n_const = 0;
    x.n_chng = 0;
    x.exprs = 0;
    x.has_aff_blob = 0;
    if( ALWAYS(p->src!=0)
     && p->src->n_src>0
     && (p->src->a[0].fg.jointype & JT_LTORJ)!=0
    ){
      /* Do not propagate constants on any ON clause if there is a
      ** RIGHT JOIN anywhere in the query */
      x.m_exclude_on = EP_InnerON | EP_OuterON;
    }else{
      /* Do not propagate constants through the ON clause of a LEFT JOIN */
      x.m_exclude_on = EP_OuterON;
    }
    __find_const_in_where(&x, p->where);
    if( x.n_const ){
      memset(&w, 0, sizeof(w));
      w.parser = parser;
      w.expr_callback = __propagate_constant_expr_rewrite;
      w.select_callback = __select_walk_noop;
      w.select_callback2 = 0;
      w.walker_depth = 0;
      w.u.p_const = &x;
      __walk_expr(&w, p->where);
      __dbsql_db_free(x.parser->db, x.exprs);
      n_chng += x.n_chng;
    }
  }while( x.n_chng ); 
  return n_chng;
}

#if !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW)
# if !defined(DBSQL_OMIT_WINDOWFUNC)
/*
** This function is called to determine whether or not it is safe to
** push WHERE clause expression expr down to FROM clause sub-query
** subq, which contains at least one window function. Return 1
** if it is safe and the expression should be pushed down, or 0
** otherwise.
**
** It is only safe to push the expression down if it consists only
** of constants and copies of expressions that appear in the PARTITION
** BY clause of all window function used by the sub-query. It is safe
** to filter out entire partitions, but not rows within partitions, as
** this may change the results of the window functions.
**
** At the time this function is called it is guaranteed that
**
**   * the sub-query uses only one distinct window frame, and
**   * that the window frame has a PARTITION BY clause.
*/
static int __push_down_window_check(parser_t *parser, select_t *subq, expr_t *expr){
  assert( subq->win->partition );
  assert( (subq->sel_flags & SF_MultiPart)==0 );
  assert( subq->prior==0 );
  return __expr_is_constant_or_group_by(parser, expr, subq->win->partition);
}
# endif /* DBSQL_OMIT_WINDOWFUNC */
#endif /* !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW) */

#if !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW)
/*
** Make copies of relevant WHERE clause terms of the outer query into
** the WHERE clause of subquery.  Example:
**
**    SELECT * FROM (SELECT a AS x, c-d AS y FROM t1) WHERE x=5 AND y=10;
**
** Transformed into:
**
**    SELECT * FROM (SELECT a AS x, c-d AS y FROM t1 WHERE a=5 AND c-d=10)
**     WHERE x=5 AND y=10;
**
** The hope is that the terms added to the inner query will make it more
** efficient.
**
** NAME AMBIGUITY
**
** This optimization is called the "WHERE-clause push-down optimization"
** or sometimes the "predicate push-down optimization".
**
** Do not confuse this optimization with another unrelated optimization
** with a similar name:  The "__my_sql push-down optimization" causes WHERE
** clause terms that can be evaluated using only the index and without
** reference to the table are run first, so that if they are false,
** unnecessary table seeks are avoided.
**
** RULES
**
** Do not attempt this optimization if:
**
**   (1) (** This restriction was removed on 2017-09-29.  We used to
**           disallow this optimization for aggregate subqueries, but now
**           it is allowed by putting the extra terms on the HAVING clause.
**           The added HAVING clause is pointless if the subquery lacks
**           a GROUP BY clause.  But such a HAVING clause is also harmless
**           so there does not appear to be any reason to add extra logic
**           to suppress it. **)
**
**   (2) The inner query is the recursive part of a common table expression.
**
**   (3) The inner query has a LIMIT clause (since the changes to the WHERE
**       clause would change the meaning of the LIMIT).
**
**   (4) The inner query is the right operand of a LEFT JOIN and the
**       expression to be pushed down does not come from the ON clause
**       on that LEFT JOIN.
**
**   (5) The WHERE clause expression originates in the ON or USING clause
**       of a LEFT JOIN where i_cursor is not the right-hand table of that
**       left join.  An example:
**
**           SELECT *
**           FROM (SELECT 1 AS a1 UNION ALL SELECT 2) AS aa
**           JOIN (SELECT 1 AS b2 UNION ALL SELECT 2) AS bb ON (a1=b2)
**           LEFT JOIN (SELECT 8 AS c3 UNION ALL SELECT 9) AS cc ON (b2=2);
**
**       The correct answer is three rows:  (1,1,NULL),(2,2,8),(2,2,9).
**       But if the (b2=2) term were to be pushed down into the bb subquery,
**       then the (1,1,NULL) row would be suppressed.
**
**   (6) window_t functions make things tricky as changes to the WHERE clause
**       of the inner query could change the window over which window
**       functions are calculated. Therefore, do not attempt the optimization
**       if:
**
**     (6a) The inner query uses multiple incompatible window partitions.
**
**     (6b) The inner query is a compound and uses window-functions.
**
**     (6c) The WHERE clause does not consist entirely of constants and
**          copies of expressions found in the PARTITION BY clause of
**          all window-functions used by the sub-query. It is safe to
**          filter out entire partitions, as this does not change the
**          window over which any window-function is calculated.
**
**   (7) The inner query is a Common table_t Expression (CTE) that should
**       be materialized.  (This restriction is implemented in the calling
**       routine.)
**
**   (8) If the subquery is a compound that uses UNION, INTERSECT,
**       or EXCEPT, then all of the result set columns for all arms of
**       the compound must use the BINARY collating sequence.
**
**   (9) All three of the following are true:
**
**       (9a) The WHERE clause expression originates in the ON or USING clause
**            of a join (either an INNER or an OUTER join), and
**
**       (9b) The subquery is to the right of the ON/USING clause
**
**       (9c) There is a RIGHT JOIN (or FULL JOIN) in between the ON/USING
**            clause and the subquery.
**
**       Without this restriction, the WHERE-clause push-down optimization
**       might move the ON/USING filter expression from the left side of a
**       RIGHT JOIN over to the right side, which leads to incorrect answers.
**       See also restriction (6) in __expr_is_single_table_constraint().
**
**  (10) The inner query is not the right-hand table of a RIGHT JOIN.
**
**  (11) The subquery is not a VALUES clause
**
**  (12) The WHERE clause is not "rowid ISNULL" or the equivalent.  This
**       case only comes up if dbsql is compiled using
**       DBSQL_ALLOW_ROWID_IN_VIEW.
**
** Return 0 if no changes are made and non-zero if one or more WHERE clause
** terms are duplicated into the subquery.
*/
static int __push_down_where_terms(
  parser_t *parser,        /* parser_t context (for malloc() and error reporting) */
  select_t *subq,        /* The subquery whose WHERE clause is to be augmented */
  expr_t *where,         /* The WHERE clause of the outer query */
  src_list_t *src_list,    /* The complete from clause of the outer query */
  int i_src              /* Which FROM clause term to try to push into  */
){
  expr_t *p_new;
  src_item_t *src;        /* The subquery FROM term into which WHERE is pushed */
  int n_chng = 0;
  src = &src_list->a[i_src];
  if( where==0 ) return 0;
  if( subq->sel_flags & (SF_Recursive|SF_MultiPart) ){
    return 0;           /* restrictions (2) and (11) */
  }
  if( src->fg.jointype & (JT_LTORJ|JT_RIGHT) ){
    return 0;           /* restrictions (10) */
  }

  if( subq->prior ){
    select_t *sel;
    int not_union_all = 0;
    for(sel=subq; sel; sel=sel->prior){
      u_int8_t op = sel->op;
      assert( op==TK_ALL || op==TK_SELECT
           || op==TK_UNION || op==TK_INTERSECT || op==TK_EXCEPT );
      if( op!=TK_ALL && op!=TK_SELECT ){
        not_union_all = 1;
      }
#ifndef DBSQL_OMIT_WINDOWFUNC
      if( sel->win ) return 0;    /* restriction (6b) */
#endif
    }
    if( not_union_all ){
      /* If any of the compound arms are connected using UNION, INTERSECT,
      ** or EXCEPT, then we must ensure that none of the columns use a
      ** non-BINARY collating sequence. */
      for(sel=subq; sel; sel=sel->prior){
        int ii;
        const expr_list_t *list = sel->e_list;
        assert( list!=0 );
        for(ii=0; ii<list->n_expr; ii++){
          coll_seq_t *coll = __expr_coll_seq(parser, list->a[ii].expr);
          if( !__is_binary(coll) ){
            return 0;  /* Restriction (8) */
          }
        }
      }
    }
  }else{
#ifndef DBSQL_OMIT_WINDOWFUNC
    if( subq->win && subq->win->partition==0 ) return 0;
#endif
  }

#ifdef DBSQL_DEBUG
  /* Only the first term of a compound can have a WITH clause.  But make
  ** sure no other terms are marked SF_Recursive in case something changes
  ** in the future.
  */
  {
    select_t *p_x; 
    for(p_x=subq; p_x; p_x=p_x->prior){
      assert( (p_x->sel_flags & (SF_Recursive))==0 );
    }
  }
#endif

  if( subq->limit!=0 ){
    return 0; /* restriction (3) */
  }
  while( where->op==TK_AND ){
    n_chng += __push_down_where_terms(parser, subq, where->right, src_list, i_src);
    where = where->left;
  }

#if 0 /* These checks now done by __expr_is_single_table_constraint() */
  if( EXPR_HAS_PROPERTY(where, EP_OuterON|EP_InnerON) /* (9a) */
   && (src_list->a[0].fg.jointype & JT_LTORJ)!=0     /* Fast pre-test of (9c) */
  ){
    int jj;
    for(jj=0; jj<i_src; jj++){
      if( where->w.i_join==src_list->a[jj].i_cursor ){
        /* If we reach this point, both (9a) and (9b) are satisfied.
        ** The following loop checks (9c):
        */
        for(jj++; jj<i_src; jj++){
          if( (src_list->a[jj].fg.jointype & JT_RIGHT)!=0 ){
            return 0;  /* restriction (9) */
          }
        }
      }
    }
  }
  if( is_left_join
   && (EXPR_HAS_PROPERTY(where,EP_OuterON)==0
         || where->w.i_join!=i_cursor)
  ){
    return 0; /* restriction (4) */
  }
  if( EXPR_HAS_PROPERTY(where,EP_OuterON)
   && where->w.i_join!=i_cursor
  ){
    return 0; /* restriction (5) */
  }
#endif

#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
  if( VIEW_CAN_HAVE_ROWID && (where->op==TK_ISNULL || where->op==TK_NOTNULL) ){
    expr_t *left = where->left;
    if( ALWAYS(left) 
     && left->op==TK_COLUMN
     && left->i_column < 0
    ){
      return 0;  /* Restriction (12) */
    }
  }
#endif

  if( __expr_is_single_table_constraint(where, src_list, i_src, 1) ){
    n_chng++;
    subq->sel_flags |= SF_PushDown;
    while( subq ){
      subst_context_t x;
      p_new = __expr_dup(parser->db, where, 0);
      __unset_join_expr(p_new, -1, 1);
      x.parser = parser;
      x.i_table = src->i_cursor;
      x.i_new_table = src->i_cursor;
      x.is_outer_join = 0;
      x.n_sel_depth = 0;
      x.e_list = subq->e_list;
      x.c_list = __find_leftmost_exprlist(subq);
      p_new = __subst_expr(&x, p_new);
      assert( p_new!=0 || parser->n_err!=0 );
      if( parser->n_err==0 && p_new->op==TK_IN && EXPR_USE_X_SELECT(p_new) ){
        assert( p_new->x.p_select!=0 );
        p_new->x.p_select->sel_flags |= SF_ClonedRhsIn;
        assert( where!=0 );
        assert( where->op==TK_IN );
        assert( EXPR_USE_X_SELECT(where) );
        assert( where->x.p_select!=0 );
        where->x.p_select->sel_flags |= SF_ClonedRhsIn;
      }
#ifndef DBSQL_OMIT_WINDOWFUNC
      if( subq->win && 0==__push_down_window_check(parser, subq, p_new) ){
        /* Restriction 6c has prevented push-down in this case */
        __expr_delete(parser->db, p_new);
        n_chng--;
        break;
      }
#endif
      if( subq->sel_flags & SF_Aggregate ){
        subq->having = __expr_and(parser, subq->having, p_new);
      }else{
        subq->where = __expr_and(parser, subq->where, p_new);
      }
      subq = subq->prior;
    }
  }
  return n_chng;
}
#endif /* !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW) */

/*
** Check to see if a subquery contains result-set columns that are
** never used.  If it does, change the value of those result-set columns
** to NULL so that they do not cause unnecessary work to compute.
**
** Return the number of column that were changed to NULL.
*/
static int __disable_unused_subquery_result_columns(src_item_t *item){
  int n_col;
  select_t *sub;      /* The subquery to be simplified */
  select_t *x;        /* For looping over compound elements of sub */
  table_t *tab;       /* The table that describes the subquery */
  int j;             /* column_t number */
  int n_chng = 0;     /* Number of columns converted to NULL */
  Bitmask col_used;   /* Columns that may not be NULLed out */

  assert( item!=0 );
  if( item->fg.is_correlated || item->fg.is_cte ){
    return 0;
  }
  assert( item->s_tab!=0 );
  tab = item->s_tab;
  assert( item->fg.is_subquery );
  sub = item->u4.subq->p_select;
  assert( sub->e_list->n_expr==tab->n_col );
  for(x=sub; x; x=x->prior){
    if( (x->sel_flags & (SF_Distinct|SF_Aggregate))!=0 ){
      testcase( x->sel_flags & SF_Distinct );
      testcase( x->sel_flags & SF_Aggregate );
      return 0;
    }
    if( x->prior && x->op!=TK_ALL ){
      /* This optimization does not work for compound subqueries that
      ** use UNION, INTERSECT, or EXCEPT.  Only UNION ALL is allowed. */
      return 0;
    }
#ifndef DBSQL_OMIT_WINDOWFUNC
    if( x->win ){
      /* This optimization does not work for subqueries that use window
      ** functions. */
      return 0;
    }
#endif
  }
  col_used = item->col_used;
  if( sub->order_by ){
    expr_list_t *list = sub->order_by;
    for(j=0; j<list->n_expr; j++){
      u_int16_t i_col = list->a[j].u.x.i_order_by_col;
      if( i_col>0 ){
        i_col--;
        col_used |= ((Bitmask)1)<<(i_col>=BMS ? BMS-1 : i_col);
      }
    }
  }
  n_col = tab->n_col;
  for(j=0; j<n_col; j++){
    Bitmask m = j<BMS-1 ? MASKBIT(j) : TOPBIT;
    if( (m & col_used)!=0 ) continue;
    for(x=sub; x; x=x->prior) {
      expr_t *y = x->e_list->a[j].expr;
      if( y->op==TK_NULL ) continue;
      y->op = TK_NULL;
      EXPR_CLEAR_PROPERTY(y, EP_Skip|EP_Unlikely);
      x->sel_flags |= SF_PushDown;
      n_chng++;
    }
  }
  return n_chng;
}


/*
** The func is the only aggregate function in the query.  Check to see
** if the query is a candidate for the min/max optimization.
**
** If the query is a candidate for the min/max optimization, then set
** *min_max to be an ORDER BY clause to be used for the optimization
** and return either WHERE_ORDERBY_MIN or WHERE_ORDERBY_MAX depending on
** whether func is a min() or max() function.
**
** If the query is not a candidate for the min/max optimization, return
** WHERE_ORDERBY_NORMAL (which must be zero).
**
** This routine must be called after aggregate functions have been
** located but before their arguments have been subjected to aggregate
** analysis.
*/
static u_int8_t __min_max_query(DBSQL *db, expr_t *func, expr_list_t **min_max){
  int ret = WHERE_ORDERBY_NORMAL;      /* Return value */
  expr_list_t *e_list;                     /* Arguments to agg function */
  const char *z_func;                    /* Name of aggregate function func */
  expr_list_t *order_by;
  u_int8_t sort_flags = 0;

  assert( *min_max==0 );
  assert( func->op==TK_AGG_FUNCTION );
  assert( !IS_WINDOW_FUNC(func) );
  assert( EXPR_USE_X_LIST(func) );
  e_list = func->x.list;
  if( e_list==0
   || e_list->n_expr!=1
   || EXPR_HAS_PROPERTY(func, EP_WinFunc)
   || OPTIMIZATION_DISABLED(db, DBSQL_MinMaxOpt)
  ){
    return ret;
  }
  assert( !EXPR_HAS_PROPERTY(func, EP_IntValue) );
  z_func = func->u.token;
  if( __str_icmp(z_func, "min")==0 ){
    ret = WHERE_ORDERBY_MIN;
    if( __expr_can_be_null(e_list->a[0].expr) ){
      sort_flags = KEYINFO_ORDER_BIGNULL;
    }
  }else if( __str_icmp(z_func, "max")==0 ){
    ret = WHERE_ORDERBY_MAX;
    sort_flags = KEYINFO_ORDER_DESC;
  }else{
    return ret;
  }
  *min_max = order_by = __expr_list_dup(db, e_list, 0);
  assert( order_by!=0 || db->malloc_failed );
  if( order_by ) order_by->a[0].fg.sort_flags = sort_flags;
  return ret;
}

/*
** The select statement passed as the first argument is an aggregate query.
** The second argument is the associated aggregate-info object. This
** function tests if the SELECT is of the form:
**
**   SELECT count(*) FROM <tbl>
**
** where table is a database table, not a sub-select or view. If the query
** does match this pattern, then a pointer to the table_t object representing
** <tbl> is returned. Otherwise, NULL is returned.
**
** This routine checks to see if it is safe to use the count optimization.
** A correct answer is still obtained (though perhaps more slowly) if
** this routine returns NULL when it could have returned a table pointer.
** But returning the pointer when NULL should have been returned can
** result in incorrect answers and/or crashes.  So, when in doubt, return NULL.
*/
static table_t *__is_simple_count(select_t *p, agg_info_t *agg_info){
  table_t *tab;
  expr_t *expr;

  assert( !p->group_by );

  if( p->where
   || p->e_list->n_expr!=1
   || p->src->n_src!=1
   || p->src->a[0].fg.is_subquery
   || agg_info->n_func!=1
   || p->having
  ){
    return 0;
  }
  tab = p->src->a[0].s_tab;
  assert( tab!=0 );
  assert( !IsView(tab) );
  if( !IS_ORDINARY_TABLE(tab) ) return 0;
  expr = p->e_list->a[0].expr;
  assert( expr!=0 );
  if( expr->op!=TK_AGG_FUNCTION ) return 0;
  if( expr->agg_info!=agg_info ) return 0;
  if( (agg_info->funcs[0].func->func_flags&DBSQL_FUNC_COUNT)==0 ) return 0;
  assert( agg_info->funcs[0].f_expr==expr );
  testcase( EXPR_HAS_PROPERTY(expr, EP_Distinct) );
  testcase( EXPR_HAS_PROPERTY(expr, EP_WinFunc) );
  if( EXPR_HAS_PROPERTY(expr, EP_Distinct|EP_WinFunc) ) return 0;

  return tab;
}

/*
** If the source-list item passed as an argument was augmented with an
** INDEXED BY clause, then try to locate the specified index. If there
** was such a clause and the named index cannot be found, return
** DBSQL_ERROR and leave an error in parser. Otherwise, populate
** from->p_index and return DBSQL_OK.
*/
int __indexed_by_lookup(parser_t *parser, src_item_t *from){
  table_t *tab = from->s_tab;
  char *indexed_by = from->u1.indexed_by;
  index_t *idx;
  assert( tab!=0 );
  assert( from->fg.is_indexed_by!=0 );

  for(idx=tab->p_index;
      idx && __str_icmp(idx->name, indexed_by);
      idx=idx->next
  );
  if( !idx ){
    __error_msg(parser, "no such index: %s", indexed_by, 0);
    parser->check_schema = 1;
    return DBSQL_ERROR;
  }
  assert( from->fg.is_cte==0 );
  from->u2.ib_index = idx;
  return DBSQL_OK;
}

/*
** Detect compound SELECT statements that use an ORDER BY clause with
** an alternative collating sequence.
**
**    SELECT ... FROM t1 EXCEPT SELECT ... FROM t2 ORDER BY .. COLLATE ...
**
** These are rewritten as a subquery:
**
**    SELECT * FROM (SELECT ... FROM t1 EXCEPT SELECT ... FROM t2)
**     ORDER BY ... COLLATE ...
**
** This transformation is necessary because the __multi_select_by_merge() routine
** above that generates the code for a compound SELECT with an ORDER BY clause
** uses a merge algorithm that requires the same collating sequence on the
** result columns as on the ORDER BY clause.  See ticket
** http://sqlite.org/src/info/6709574d2a
**
** This transformation is only needed for EXCEPT, INTERSECT, and UNION.
** The UNION ALL operator works fine with __multi_select_by_merge() even when
** there are COLLATE terms in the ORDER BY.
*/
static int __convert_compound_select_to_subquery(walker_t *walker, select_t *p){
  int i;
  select_t *p_new;
  select_t *x;
  DBSQL *db;
  struct expr_list_item_t *a;
  src_list_t *new_src;
  parser_t *parser;
  Token dummy;

  if( p->prior==0 ) return WRC_Continue;
  if( p->order_by==0 ) return WRC_Continue;
  for(x=p; x && (x->op==TK_ALL || x->op==TK_SELECT); x=x->prior){}
  if( x==0 ) return WRC_Continue;
  a = p->order_by->a;
#ifndef DBSQL_OMIT_WINDOWFUNC
  /* If i_order_by_col is already non-zero, then it has already been matched
  ** to a result column of the SELECT statement. This occurs when the
  ** SELECT is rewritten for window-functions processing and then passed
  ** to __select_prep() and similar a second time. The rewriting done
  ** by this function is not required in this case. */
  if( a[0].u.x.i_order_by_col ) return WRC_Continue;
#endif
  for(i=p->order_by->n_expr-1; i>=0; i--){
    if( a[i].expr->flags & EP_Collate ) break;
  }
  if( i<0 ) return WRC_Continue;

  /* If we reach this point, that means the transformation is required. */

  parser = walker->parser;
  db = parser->db;
  p_new = __db_malloc_zero(db, sizeof(*p_new) );
  if( p_new==0 ) return WRC_Abort;
  memset(&dummy, 0, sizeof(dummy));
  new_src = __src_list_append_from_term(parser,0,0,0,&dummy,p_new,0);
  assert( new_src!=0 || parser->n_err );
  if( parser->n_err ){
    __src_list_delete(db, new_src);
    return WRC_Abort;
  }
  *p_new = *p;
  p->src = new_src;
  p->e_list = __expr_list_append(parser, 0, __expr(db, TK_ASTERISK, 0));
  p->op = TK_SELECT;
  p->where = 0;
  p_new->group_by = 0;
  p_new->having = 0;
  p_new->order_by = 0;
  p->prior = 0;
  p->next = 0;
  p->with = 0;
#ifndef DBSQL_OMIT_WINDOWFUNC
  p->win_defn = 0;
#endif
  p->sel_flags &= ~(u_int32_t)SF_Compound;
  assert( (p->sel_flags & SF_Converted)==0 );
  p->sel_flags |= SF_Converted;
  assert( p_new->prior!=0 );
  p_new->prior->next = p_new;
  p_new->limit = 0;
  return WRC_Continue;
}

/*
** Check to see if the FROM clause term from has table-valued function
** arguments.  If it does, leave an error message in parser and return
** non-zero, since from is not allowed to be a table-valued function.
*/
static int __cannot_be_function(parser_t *parser, src_item_t *from){
  if( from->fg.is_tab_func ){
    __error_msg(parser, "'%s' is not a function", from->name);
    return 1;
  }
  return 0;
}

#ifndef DBSQL_OMIT_CTE
/*
** Argument with (which may be NULL) points to a linked list of nested
** WITH contexts, from inner to outermost. If the table identified by
** FROM clause element item is really a common-table-expression (CTE)
** then return a pointer to the CTE definition for that table. Otherwise
** return NULL.
**
** If a non-NULL value is returned, set *context to point to the with_t
** object that the returned CTE belongs to.
*/
static struct cte_t *__search_with(
  with_t *with,                    /* Current innermost WITH clause */
  src_item_t *item,                 /* FROM clause element to resolve */
  with_t **context                /* OUT: WITH clause return value belongs to */
){
  const char *name = item->name;
  with_t *p;
  assert( item->fg.fixed_schema || item->u4.database==0 );
  assert( name!=0 );
  for(p=with; p; p=p->outer){
    int i;
    for(i=0; i<p->n_cte; i++){
      if( __str_icmp(name, p->a[i].name)==0 ){
        *context = p;
        return &p->a[i];
      }
    }
    if( p->view ) break;
  }
  return 0;
}

/* The code generator maintains a stack of active WITH clauses
** with the inner-most WITH clause being at the top of the stack.
**
** This routine pushes the WITH clause passed as the second argument
** onto the top of the stack. If argument b_free is true, then this
** WITH clause will never be popped from the stack but should instead
** be freed along with the parser_t object. In other cases, when
** b_free==0, the with_t object will be freed along with the SELECT
** statement with which it is associated.
**
** This routine returns a copy of with.  Or, if b_free is true and
** the with object is destroyed immediately due to an OOM condition,
** then this routine return NULL.
**
** If b_free is true, do not continue to use the with pointer after
** calling this routine,  Instead, use only the return value.
*/
with_t *__with_push(parser_t *parser, with_t *with, u_int8_t b_free){
  if( with ){
    if( b_free ){
      with = (with_t*)__parser_add_cleanup(parser, __with_delete_generic,
                      with);
      if( with==0 ) return 0;
    }
    if( parser->n_err==0 ){
      assert( parser->with!=with );
      with->outer = parser->with;
      parser->with = with;
    }
  }
  return with;
}

/*
** This function checks if argument from refers to a CTE declared by
** a WITH clause on the stack currently maintained by the parser (on the
** parser->with linked list).  And if currently processing a CTE
** CTE expression, through routine checks to see if the reference is
** a recursive reference to the CTE.
**
** If from matches a CTE according to either of these two above, from->s_tab
** and other fields are populated accordingly.
**
** Return 0 if no match is found.
** Return 1 if a match is found.
** Return 2 if an error condition is detected.
*/
static int __resolve_from_term_to_cte(
  parser_t *parser,                  /* The parsing context */
  walker_t *walker,                /* Current tree walker */
  src_item_t *from                  /* The FROM clause term to check */
){
  cte_t *cte;               /* Matched CTE (or NULL if no match) */
  with_t *with;             /* The matching WITH */

  assert( from->s_tab==0 );
  if( parser->with==0 ){
    /* There are no WITH clauses in the stack.  No match is possible */
    return 0;
  }
  if( parser->n_err ){
    /* Prior errors might have left parser->with in a goofy state, so
    ** go no further. */
    return 0;
  }
  assert( from->fg.had_schema==0 || from->fg.not_cte!=0 );
  if( from->fg.fixed_schema==0 && from->u4.database!=0 ){
    /* The FROM term contains a schema qualifier (ex: main.t1) and so
    ** it cannot possibly be a CTE reference. */
    return 0;
  }
  if( from->fg.not_cte ){
    /* The FROM term is specifically excluded from matching a CTE.
    **   (1)  It is part of a trigger that used to have database but had
    **        database removed by __fix_trigger_step().
    **   (2)  This is the first term in the FROM clause of an UPDATE.
    */
    return 0;
  }
  cte = __search_with(parser->with, from, &with);
  if( cte ){
    DBSQL *db = parser->db;
    table_t *tab;
    expr_list_t *e_list;
    select_t *sel;
    select_t *left;                /* Left-most SELECT statement */
    select_t *rec_term;             /* Left-most recursive term */
    int may_recursive;            /* True if compound joined by UNION [ALL] */
    with_t *saved_with;             /* Initial value of parser->with */
    int i_rec_tab = -1;             /* Cursor for recursive table */
    cte_use_t *cte_use;

    /* If cte->cte_err is non-NULL at this point, then this is an illegal
    ** recursive reference to CTE cte. Leave an error in parser and return
    ** early. If cte->cte_err is NULL, then this is not a recursive reference.
    ** In this case, proceed.  */
    if( cte->cte_err ){
      __error_msg(parser, cte->cte_err, cte->name);
      return 2;
    }
    if( __cannot_be_function(parser, from) ) return 2;

    assert( from->s_tab==0 );
    tab = __db_malloc_zero(db, sizeof(table_t));
    if( tab==0 ) return 2;
    cte_use = cte->use;
    if( cte_use==0 ){
      cte->use = cte_use = __db_malloc_zero(db, sizeof(cte_use[0]));
      if( cte_use==0
       || __parser_add_cleanup(parser,__dbsql_db_free,cte_use)==0
      ){
        __dbsql_db_free(db, tab);
        return 2;
      }
      cte_use->m10d = cte->m10d;
    }
    from->s_tab = tab;
    tab->n_tab_ref = 1;
    tab->name = __db_str_dup(db, cte->name);
    tab->i_p_key = -1;
    tab->n_row_log_est = 200; assert( 200==__log_est(1048576) );
    tab->tab_flags |= TF_Ephemeral | TF_NoVisibleRowid;
    __src_item_attach_subquery(parser, from, cte->p_select, 1);
    if( db->malloc_failed ) return 2;
    assert( from->fg.is_subquery && from->u4.subq );
    sel = from->u4.subq->p_select;
    assert( sel!=0 );
    sel->sel_flags |= SF_CopyCte;
    if( from->fg.is_indexed_by ){
      __error_msg(parser, "no such index: \"%s\"", from->u1.indexed_by);
      return 2;
    }
    assert( !from->fg.is_indexed_by );
    from->fg.is_cte = 1;
    from->u2.cte_use = cte_use;
    cte_use->n_use++;

    /* Check if this is a recursive CTE. */
    rec_term = sel;
    may_recursive = ( sel->op==TK_ALL || sel->op==TK_UNION );
    while( may_recursive && rec_term->op==sel->op ){
      int i;
      src_list_t *src = rec_term->src;
      assert( rec_term->prior!=0 );
      for(i=0; i<src->n_src; i++){
        src_item_t *item = &src->a[i];
        if( item->name!=0
         && !item->fg.had_schema
         && ALWAYS( !item->fg.is_subquery )
         && (item->fg.fixed_schema || item->u4.database==0)
         && 0==__str_icmp(item->name, cte->name)
        ){
          item->s_tab = tab;
          tab->n_tab_ref++;
          item->fg.is_recursive = 1;
          if( rec_term->sel_flags & SF_Recursive ){
            __error_msg(parser,
               "multiple references to recursive table: %s", cte->name
            );
            return 2;
          }
          rec_term->sel_flags |= SF_Recursive;
          if( i_rec_tab<0 ) i_rec_tab = parser->n_tab++;
          item->i_cursor = i_rec_tab;
        }
      }
      if( (rec_term->sel_flags & SF_Recursive)==0 ) break;
      rec_term = rec_term->prior;
    }

    cte->cte_err = "circular reference: %s";
    saved_with = parser->with;
    parser->with = with;
    if( sel->sel_flags & SF_Recursive ){
      int rc;
      assert( rec_term!=0 );
      assert( (rec_term->sel_flags & SF_Recursive)==0 );
      assert( rec_term->next!=0 );
      assert( (rec_term->next->sel_flags & SF_Recursive)!=0 );
      assert( rec_term->with==0 );
      rec_term->with = sel->with;
      rc = __walk_select(walker, rec_term);
      rec_term->with = 0;
      if( rc ){
        parser->with = saved_with;
        return 2;
      }
    }else{
      if( __walk_select(walker, sel) ){
        parser->with = saved_with;
        return 2;
      }
    }
    parser->with = with;

    for(left=sel; left->prior; left=left->prior);
    e_list = left->e_list;
    if( cte->cols ){
      if( e_list && e_list->n_expr!=cte->cols->n_expr ){
        __error_msg(parser, "table %s has %d values for %d columns",
            cte->name, e_list->n_expr, cte->cols->n_expr
        );
        parser->with = saved_with;
        return 2;
      }
      e_list = cte->cols;
    }

    __columns_from_expr_list(parser, e_list, &tab->n_col, &tab->cols);
    if( may_recursive ){
      if( sel->sel_flags & SF_Recursive ){
        cte->cte_err = "multiple recursive references: %s";
      }else{
        cte->cte_err = "recursive reference in a subquery: %s";
      }
      __walk_select(walker, sel);
    }
    cte->cte_err = 0;
    parser->with = saved_with;
    return 1;  /* Success */
  }
  return 0;  /* No match */
}
#endif

#ifndef DBSQL_OMIT_CTE
/*
** If the SELECT passed as the second argument has an associated WITH
** clause, pop it from the stack stored as part of the parser_t object.
**
** This function is used as the select_callback2() callback by
** __select_expand() when walking a SELECT tree to resolve table
** names and other FROM clause elements.
*/
void __select_pop_with(walker_t *walker, select_t *p){
  parser_t *parser = walker->parser;
  if( OK_IF_ALWAYS_TRUE(parser->with) && p->prior==0 ){
    with_t *with = __find_rightmost(p)->with;
    if( with!=0 ){
      assert( parser->with==with || parser->n_err );
      parser->with = with->outer;
    }
  }
}
#endif

/*
** The src_item_t structure passed as the second argument represents a
** sub-query in the FROM clause of a SELECT statement. This function
** allocates and populates the src_item_t.tab object. If successful,
** DBSQL_OK is returned. Otherwise, if an OOM error is encountered,
** DBSQL_NOMEM.
*/
int __expand_subquery(parser_t *parser, src_item_t *from){
  select_t *sel;
  table_t *tab;

  assert( from->fg.is_subquery );
  assert( from->u4.subq!=0 );
  sel = from->u4.subq->p_select;
  assert( sel );
  from->s_tab = tab = __db_malloc_zero(parser->db, sizeof(table_t));
  if( tab==0 ) return DBSQL_NOMEM;
  tab->n_tab_ref = 1;
  if( from->alias ){
    tab->name = __db_str_dup(parser->db, from->alias);
  }else{
    tab->name = __m_printf(parser->db, "%!S", from);
  }
  while( sel->prior ){ sel = sel->prior; }
  __columns_from_expr_list(parser, sel->e_list,&tab->n_col,&tab->cols);
  tab->i_p_key = -1;
  tab->tab_type = TABTYP_VIEW;
  tab->n_row_log_est = 200; assert( 200==__log_est(1048576) );
#ifndef DBSQL_ALLOW_ROWID_IN_VIEW
  /* The usual case - do not allow ROWID on a subquery */
  tab->tab_flags |= TF_Ephemeral | TF_NoVisibleRowid;
#else
  /* Legacy compatibility mode */
  tab->tab_flags |= TF_Ephemeral | __config.m_no_visible_rowid;
#endif
  return parser->n_err ? DBSQL_ERROR : DBSQL_OK;
}


/*
** Check the N src_item_t objects to the right of base.  (N might be zero!)
** If any of those src_item_t objects have a USING clause containing name
** then return true.
**
** If N is zero, or none of the N src_item_t objects to the right of base
** contains a USING clause, or if none of the USING clauses contain name,
** then return false.
*/
static int __in_any_using_clause(
  const char *name, /* Name we are looking for */
  src_item_t *base,    /* The base src_item_t.  Looking at base[1] and following */
  int N              /* How many __src_items to check */
){
  while( N>0 ){
    N--;
    base++;
    if( base->fg.is_using==0 ) continue;
    if( NEVER(base->u3.p_using==0) ) continue;
    if( __id_list_index(base->u3.p_using, name)>=0 ) return 1;
  }
  return 0;
}


/*
** This routine is a walker_t callback for "expanding" a SELECT statement.
** "Expanding" means to do the following:
**
**    (1)  Make sure VDBE cursor numbers have been assigned to every
**         element of the FROM clause.
**
**    (2)  Fill in the tab_list->a[].tab fields in the src_list_t that
**         defines FROM clause.  When views appear in the FROM clause,
**         fill tab_list->a[].p_select with a copy of the SELECT statement
**         that implements the view.  A copy is made of the view's SELECT
**         statement so that we can freely modify or delete that statement
**         without worrying about messing up the persistent representation
**         of the view.
**
**    (3)  Add terms to the WHERE clause to accommodate the NATURAL keyword
**         on joins and the ON and USING clause of joins.
**
**    (4)  Scan the list of columns in the result set (e_list) looking
**         for instances of the "*" operator or the TABLE.* operator.
**         If found, expand each "*" to be every column in every table
**         and TABLE.* to be every column in TABLE.
**
*/
static int __select_expander(walker_t *walker, select_t *p){
  parser_t *parser = walker->parser;
  int i, j, k, rc;
  src_list_t *tab_list;
  expr_list_t *e_list;
  src_item_t *from;
  DBSQL *db = parser->db;
  expr_t *e, *right, *expr;
  u_int16_t sel_flags = p->sel_flags;
  u_int32_t elist_flags = 0;

  p->sel_flags |= SF_Expanded;
  if( db->malloc_failed  ){
    return WRC_Abort;
  }
  assert( p->src!=0 );
  if( (sel_flags & SF_Expanded)!=0 ){
    return WRC_Prune;
  }
  if( walker->code ){
    /* Renumber sel_id because it has been copied from a view */
    p->sel_id = ++parser->n_select;
  }
  tab_list = p->src;
  e_list = p->e_list;
  if( parser->with && (p->sel_flags & SF_View) ){
    if( p->with==0 ){
      p->with = (with_t*)__db_malloc_zero(db, SZ_WITH(1) );
      if( p->with==0 ){
        return WRC_Abort;
      }
    }
    p->with->view = 1;
  }
  __with_push(parser, p->with, 0);

  /* Make sure cursor numbers have been assigned to all entries in
  ** the FROM clause of the SELECT statement.
  */
  __src_list_assign_cursors(parser, tab_list);

  /* Look up every table named in the FROM clause of the select.  If
  ** an entry of the FROM clause is a subquery instead of a table or view,
  ** then create a transient table structure to describe the subquery.
  */
  for(i=0, from=tab_list->a; i<tab_list->n_src; i++, from++){
    table_t *tab;
    assert( from->fg.is_recursive==0 || from->s_tab!=0 );
    if( from->s_tab ) continue;
    assert( from->fg.is_recursive==0 );
    if( from->name==0 ){
#ifndef DBSQL_OMIT_SUBQUERY
      select_t *sel;
      assert( from->fg.is_subquery && from->u4.subq!=0 );
      sel = from->u4.subq->p_select;
      /* A sub-query in the FROM clause of a SELECT */
      assert( sel!=0 );
      assert( from->s_tab==0 );
      if( __walk_select(walker, sel) ) return WRC_Abort;
      if( __expand_subquery(parser, from) ) return WRC_Abort;
#endif
#ifndef DBSQL_OMIT_CTE
    }else if( (rc = __resolve_from_term_to_cte(parser, walker, from))!=0 ){
      if( rc>1 ) return WRC_Abort;
      tab = from->s_tab;
      assert( tab!=0 );
#endif
    }else{
      /* An ordinary table or view name in the FROM clause */
      assert( from->s_tab==0 );
      from->s_tab = tab = __locate_table_item(parser, 0, from);
      if( tab==0 ) return WRC_Abort;
      if( tab->n_tab_ref>=0xffff ){
        __error_msg(parser, "too many references to \"%s\": max 65535",
           tab->name);
        from->s_tab = 0;
        return WRC_Abort;
      }
      tab->n_tab_ref++;
      if( !IS_VIRTUAL(tab) && __cannot_be_function(parser, from) ){
        return WRC_Abort;
      }
#if !defined(DBSQL_OMIT_VIEW) || !defined(DBSQL_OMIT_VIRTUALTABLE)
      if( !IS_ORDINARY_TABLE(tab) ){
        int16_t n_col;
        u_int8_t code_orig = walker->code;
        if( __view_get_column_names(parser, tab) ) return WRC_Abort;
        assert( from->fg.is_subquery==0 );
        if( IsView(tab) ){
          if( (db->flags & DBSQL_EnableView)==0
           && tab->schema!=db->dbs[1].schema
          ){
            __error_msg(parser, "access to view \"%s\" prohibited",
              tab->name);
          }
          __src_item_attach_subquery(parser, from, tab->u.view.p_select, 1);
        }
#ifndef DBSQL_OMIT_VIRTUALTABLE
        else if( ALWAYS(IS_VIRTUAL(tab))
         && (from->fg.from_ddl || (parser->prep_flags & DBSQL_PREPARE_FROM_DDL))
         && ALWAYS(tab->u.vtab.p!=0)
         && tab->u.vtab.p->vtab_risk > ((db->flags & DBSQL_TrustedSchema)!=0)
        ){
          __error_msg(parser, "unsafe use of virtual table \"%s\"",
                                  tab->name);
        }
        assert( DBSQL_VTABRISK_Normal==1 && DBSQL_VTABRISK_High==2 );
#endif
        n_col = tab->n_col;
        tab->n_col = -1;
        walker->code = 1;  /* Turn on select_t.sel_id renumbering */
        if( from->fg.is_subquery ){
          __walk_select(walker, from->u4.subq->p_select);
        }
        walker->code = code_orig;
        tab->n_col = n_col;
      }
#endif
    }

    /* Locate the index named by the INDEXED BY clause, if any. */
    if( from->fg.is_indexed_by && __indexed_by_lookup(parser, from) ){
      return WRC_Abort;
    }
  }

  /* Process NATURAL keywords, and ON and USING clauses of joins.
  */
  assert( db->malloc_failed==0 || parser->n_err!=0 );
  if( parser->n_err || __process_join(parser, p) ){
    return WRC_Abort;
  }

  /* For every "*" that occurs in the column list, insert the names of
  ** all columns in all tables.  And for every TABLE.* insert the names
  ** of all columns in TABLE.  The parser inserted a special expression
  ** with the TK_ASTERISK operator for each "*" that it found in the column
  ** list.  The following code just has to locate the TK_ASTERISK
  ** expressions and expand each one to the list of all columns in
  ** all tables.
  **
  ** The first loop just checks to see if there are any "*" operators
  ** that need expanding.
  */
  for(k=0; k<e_list->n_expr; k++){
    e = e_list->a[k].expr;
    if( e->op==TK_ASTERISK ) break;
    assert( e->op!=TK_DOT || e->right!=0 );
    assert( e->op!=TK_DOT || (e->left!=0 && e->left->op==TK_ID) );
    if( e->op==TK_DOT && e->right->op==TK_ASTERISK ) break;
    elist_flags |= e->flags;
  }
  if( k<e_list->n_expr ){
    /*
    ** If we get here it means the result set contains one or more "*"
    ** operators that need to be expanded.  Loop through each expression
    ** in the result set and expand them one by one.
    */
    struct expr_list_item_t *a = e_list->a;
    expr_list_t *p_new = 0;
    int flags = parser->db->flags;
    int long_names = (flags & DBSQL_FullColNames)!=0
                      && (flags & DBSQL_ShortColNames)==0;

    for(k=0; k<e_list->n_expr; k++){
      e = a[k].expr;
      elist_flags |= e->flags;
      right = e->right;
      assert( e->op!=TK_DOT || right!=0 );
      if( e->op!=TK_ASTERISK
       && (e->op!=TK_DOT || right->op!=TK_ASTERISK)
      ){
        /* This particular expression does not need to be expanded.
        */
        p_new = __expr_list_append(parser, p_new, a[k].expr);
        if( p_new ){
          p_new->a[p_new->n_expr-1].e_name = a[k].e_name;
          p_new->a[p_new->n_expr-1].fg.e_name = a[k].fg.e_name;
          a[k].e_name = 0;
        }
        a[k].expr = 0;
      }else{
        /* This expression is a "*" or a "TABLE.*" and needs to be
        ** expanded. */
        int table_seen = 0;      /* Set to 1 when TABLE matches */
        char *t_name = 0;       /* text of name of TABLE */
        int i_err_ofst;
        if( e->op==TK_DOT ){
          assert( (sel_flags & SF_NestedFrom)==0 );
          assert( e->left!=0 );
          assert( !EXPR_HAS_PROPERTY(e->left, EP_IntValue) );
          t_name = e->left->u.token;
          assert( EXPR_USE_W_OFST(e->left) );
          i_err_ofst = e->right->w.i_ofst;
        }else{
          assert( EXPR_USE_W_OFST(e) );
          i_err_ofst = e->w.i_ofst;
        }
        for(i=0, from=tab_list->a; i<tab_list->n_src; i++, from++){
          int n_add;                    /* Number of cols including rowid */
          table_t *tab = from->s_tab;  /* table_t for this data source */
          expr_list_t *nested_from;       /* Result-set of a nested FROM clause */
          char *tab_name;              /* AS name for this data source */
          const char *schema_name = 0; /* schema_t name for this data source */
          int i_db;                     /* schema_t index for this data src */
          id_list_t *p_using;              /* USING clause for from[1] */

          if( (tab_name = from->alias)==0 ){
            tab_name = tab->name;
          }
          if( db->malloc_failed ) break;
          assert( (int)from->fg.is_nested_from == IS_NESTED_FROM(from) );
          if( from->fg.is_nested_from ){
            assert( from->fg.is_subquery && from->u4.subq );
            assert( from->u4.subq->p_select!=0 );
            nested_from = from->u4.subq->p_select->e_list;
            assert( nested_from!=0 );
            assert( nested_from->n_expr==tab->n_col );
            assert( VISIBLE_ROWID(tab)==0 || VIEW_CAN_HAVE_ROWID );
          }else{
            if( t_name && __str_icmp(t_name, tab_name)!=0 ){
              continue;
            }
            nested_from = 0;
            i_db = __schema_to_index(db, tab->schema);
            schema_name = i_db>=0 ? db->dbs[i_db].db_s_name : "*";
          }
          if( i+1<tab_list->n_src
           && from[1].fg.is_using
           && (sel_flags & SF_NestedFrom)!=0
          ){
            int ii;
            p_using = from[1].u3.p_using;
            for(ii=0; ii<p_using->n_id; ii++){
              const char *u_name = p_using->a[ii].name;
              right = __expr(db, TK_ID, u_name);
              __expr_set_error_offset(right, i_err_ofst);
              p_new = __expr_list_append(parser, p_new, right);
              if( p_new ){
                struct expr_list_item_t *x = &p_new->a[p_new->n_expr-1];
                assert( x->e_name==0 );
                x->e_name = __m_printf(db,"..%s", u_name);
                x->fg.e_name = ENAME_TAB;
                x->fg.using_term = 1;
              }
            }
          }else{
            p_using = 0;
          }

          n_add = tab->n_col;
          if( VISIBLE_ROWID(tab) && (sel_flags & SF_NestedFrom)!=0 ) n_add++;
          for(j=0; j<n_add; j++){
            const char *name; 
            struct expr_list_item_t *x; /* Newly added expr_list_t term */

            if( j==tab->n_col ){
              name = __rowid_alias(tab);
              if( name==0 ) continue;
            }else{
              name = tab->cols[j].cn_name;

              /* If tab is actually an SF_NestedFrom sub-select, do not
              ** expand any ENAME_ROWID columns.  */
              if( nested_from && nested_from->a[j].fg.e_name==ENAME_ROWID ){
                continue;
              }

              if( t_name
               && nested_from
               && __match_e_name(&nested_from->a[j], 0, t_name, 0, 0)==0
              ){
                continue;
              }

              /* If a column is marked as 'hidden', omit it from the expanded
              ** result-set list unless the SELECT has the SF_IncludeHidden
              ** bit set.
              */
              if( (p->sel_flags & SF_IncludeHidden)==0
                && IS_HIDDEN_COLUMN(&tab->cols[j])
              ){
                continue;
              }
              if( (tab->cols[j].col_flags & COLFLAG_NOEXPAND)!=0
               && t_name==0
               && (sel_flags & (SF_NestedFrom))==0
              ){
                continue;
              }
            }
            assert( name );
            table_seen = 1;

            if( i>0 && t_name==0 && (sel_flags & SF_NestedFrom)==0 ){
              if( from->fg.is_using
               && __id_list_index(from->u3.p_using, name)>=0
              ){
                /* In a join with a USING clause, omit columns in the
                ** using clause from the table on the right. */
                continue;
              }
            }
            right = __expr(db, TK_ID, name);
            if( (tab_list->n_src>1
                 && (  (from->fg.jointype & JT_LTORJ)==0
                     || (sel_flags & SF_NestedFrom)!=0
                     || !__in_any_using_clause(name,from,tab_list->n_src-i-1)
                    )
                )
             || IN_RENAME_OBJECT
            ){
              expr_t *left;
              left = __expr(db, TK_ID, tab_name);
              expr = __p_expr(parser, TK_DOT, left, right);
              if( IN_RENAME_OBJECT && e->left ){
                __rename_token_remap(parser, left, e->left);
              }
              if( schema_name ){
                left = __expr(db, TK_ID, schema_name);
                expr = __p_expr(parser, TK_DOT, left, expr);
              }
            }else{
              expr = right;
            }
            __expr_set_error_offset(expr, i_err_ofst);
            p_new = __expr_list_append(parser, p_new, expr);
            if( p_new==0 ){
              break;  /* OOM */
            }
            x = &p_new->a[p_new->n_expr-1];
            assert( x->e_name==0 );
            if( (sel_flags & SF_NestedFrom)!=0 && !IN_RENAME_OBJECT ){
              if( nested_from && (!VIEW_CAN_HAVE_ROWID || j<nested_from->n_expr) ){
                assert( j<nested_from->n_expr );
                x->e_name = __db_str_dup(db, nested_from->a[j].e_name);
                testcase( x->e_name==0 );
              }else{
                x->e_name = __m_printf(db, "%s.%s.%s",
                                           schema_name, tab_name, name);
                testcase( x->e_name==0 );
              }
              x->fg.e_name = (j==tab->n_col ? ENAME_ROWID : ENAME_TAB);
              if( (from->fg.is_using
                   && __id_list_index(from->u3.p_using, name)>=0)
               || (p_using && __id_list_index(p_using, name)>=0)
               || (j<tab->n_col && (tab->cols[j].col_flags & COLFLAG_NOEXPAND))
              ){
                x->fg.no_expand = 1;
              }
            }else if( long_names ){
              x->e_name = __m_printf(db, "%s.%s", tab_name, name);
              x->fg.e_name = ENAME_NAME;
            }else{
              x->e_name = __db_str_dup(db, name);
              x->fg.e_name = ENAME_NAME;
            }
          }
        }
        if( !table_seen ){
          if( t_name ){
            __error_msg(parser, "no such table: %s", t_name);
          }else{
            __error_msg(parser, "no tables specified");
          }
        }
      }
    }
    __expr_list_delete(db, e_list);
    p->e_list = p_new;
  }
  if( p->e_list ){
    if( p->e_list->n_expr>db->limits[DBSQL_LIMIT_COLUMN] ){
      __error_msg(parser, "too many columns in result set");
      return WRC_Abort;
    }
    if( (elist_flags & (EP_HasFunc|EP_Subquery))!=0 ){
      p->sel_flags |= SF_ComplexResult;
    }
  }
#if TREETRACE_ENABLED
  if( __tree_trace & 0x8 ){
    TREETRACE(0x8,parser,p,("After result-set wildcard expansion:\n"));
    __tree_view_select(0, p, 0);
  }
#endif
  return WRC_Continue;
}

#if DBSQL_DEBUG
/*
** Always assert.  This select_callback2 implementation proves that the
** select_callback2 is never invoked.
*/
void __select_walk_assert2(walker_t *__not_used, select_t *__not_used2){
  UNUSED_PARAMETER2(__not_used, __not_used2);
  assert( 0 );
}
#endif
/*
** This routine "expands" a SELECT statement and all of its subqueries.
** For additional information on what it means to "expand" a SELECT
** statement, see the comment on the select_expand worker callback above.
**
** Expanding a SELECT statement is the first step in processing a
** SELECT statement.  The SELECT statement must be expanded before
** name resolution is performed.
**
** If anything goes wrong, an error message is written into parser.
** The calling function can detect the problem by looking at parser->n_err
** and/or parser->db->malloc_failed.
*/
static void __select_expand(parser_t *parser, select_t *p_select){
  walker_t w;
  w.expr_callback = __expr_walk_noop;
  w.parser = parser;
  if( OK_IF_ALWAYS_TRUE(parser->has_compound) ){
    w.select_callback = __convert_compound_select_to_subquery;
    w.select_callback2 = 0;
    __walk_select(&w, p_select);
  }
  w.select_callback = __select_expander;
  w.select_callback2 = __select_pop_with;
  w.code = 0;
  __walk_select(&w, p_select);
}


#ifndef DBSQL_OMIT_SUBQUERY
/*
** This is a walker_t.select_callback callback for the __select_type_info()
** interface.
**
** For each FROM-clause subquery, add column_t.type, column_t.coll, and
** column_t.affinity information to the table_t structure that represents
** the result set of that subquery.
**
** The table_t structure that represents the result set was constructed
** by __select_expander() but the type and collation and affinity information
** was omitted at that point because identifiers had not yet been resolved.
** This routine is called after identifier resolution.
*/
static void __select_add_subquery_type_info(walker_t *walker, select_t *p){
  parser_t *parser;
  int i;
  src_list_t *tab_list;
  src_item_t *from;

  if( p->sel_flags & SF_HasTypeInfo ) return;
  p->sel_flags |= SF_HasTypeInfo;
  parser = walker->parser;
  assert( (p->sel_flags & SF_Resolved) );
  tab_list = p->src;
  for(i=0, from=tab_list->a; i<tab_list->n_src; i++, from++){
    table_t *tab = from->s_tab;
    assert( tab!=0 );
    if( (tab->tab_flags & TF_Ephemeral)!=0 && from->fg.is_subquery ){
      /* A sub-query in the FROM clause of a SELECT */
      select_t *sel = from->u4.subq->p_select;
      __subquery_column_types(parser, tab, sel, DBSQL_AFF_NONE);
    }
  }
}
#endif


/*
** This routine adds datatype and collating sequence information to
** the table_t structures of all FROM-clause subqueries in a
** SELECT statement.
**
** Use this routine after name resolution.
*/
static void __select_add_type_info(parser_t *parser, select_t *p_select){
#ifndef DBSQL_OMIT_SUBQUERY
  walker_t w;
  w.select_callback = __select_walk_noop;
  w.select_callback2 = __select_add_subquery_type_info;
  w.expr_callback = __expr_walk_noop;
  w.parser = parser;
  __walk_select(&w, p_select);
#endif
}


/*
** This routine sets up a SELECT statement for processing.  The
** following is accomplished:
**
**     *  VDBE Cursor numbers are assigned to all FROM-clause terms.
**     *  Ephemeral table_t objects are created for all FROM-clause subqueries.
**     *  ON and USING clauses are shifted into WHERE statements
**     *  Wildcards "*" and "TABLE.*" in result sets are expanded.
**     *  Identifiers in expression are matched to tables.
**
** This routine acts recursively on all subqueries within the SELECT.
*/
void __select_prep(
  parser_t *parser,         /* The parser context */
  select_t *p,             /* The SELECT statement being coded. */
  name_context_t *outer_nc  /* Name context for container */
){
  assert( p!=0 || parser->db->malloc_failed );
  assert( parser->db->parser==parser );
  if( parser->db->malloc_failed ) return;
  if( p->sel_flags & SF_HasTypeInfo ) return;
  __select_expand(parser, p);
  if( parser->n_err ) return;
  __resolve_select_names(parser, p, outer_nc);
  if( parser->n_err ) return;
  __select_add_type_info(parser, p);
}

#if TREETRACE_ENABLED
/*
** Display all information about an agg_info_t object
*/
static void __print_agg_info(agg_info_t *agg_info){
  int ii;
  __debug_printf("AggInfo %d/%p:\n",
     agg_info->sel_id, agg_info);
  for(ii=0; ii<agg_info->n_column; ii++){
    struct agg_info_col_t *col = &agg_info->cols[ii];
    __debug_printf(
       "agg-column[%d] pTab=%s iTable=%d iColumn=%d iMem=%d"
       " iSorterColumn=%d %s\n",
       ii, col->tab ? col->tab->name : "NULL",
       col->i_table, col->i_column, agg_info->i_first_reg+ii,
       col->i_sorter_column,
       ii>=agg_info->n_accumulator ? "" : " Accumulator");
    __tree_view_expr(0, agg_info->cols[ii].c_expr, 0);
  }
  for(ii=0; ii<agg_info->n_func; ii++){
    __debug_printf("agg-func[%d]: iMem=%d\n",
        ii, agg_info->i_first_reg+agg_info->n_column+ii);
    __tree_view_expr(0, agg_info->funcs[ii].f_expr, 0);
  }
}
#endif /* TREETRACE_ENABLED */

/*
** Analyze the arguments to aggregate functions.  Create new agg_info->cols[]
** entries for columns that are arguments to aggregate functions but which
** are not otherwise used.
**
** The cols[] entries in agg_info_t prior to n_accumulator are columns that
** are referenced outside of aggregate functions.  These might be columns
** that are part of the GROUP by clause, for example.  Other database engines
** would throw an error if there is a column reference that is not in the
** GROUP BY clause and that is not part of an aggregate function argument.
** But dbsql allows this.
**
** The cols[] entries beginning with the cols[n_accumulator] and following
** are column references that are used exclusively as arguments to
** aggregate functions.  This routine is responsible for computing
** (or recomputing) those cols[] entries.
*/
static void __analyze_agg_func_args(
  agg_info_t *agg_info,
  name_context_t *nc
){
  int i;
  assert( agg_info!=0 );
  assert( agg_info->i_first_reg==0 );
  nc->nc_flags |= NC_InAggFunc;
  for(i=0; i<agg_info->n_func; i++){
    expr_t *expr = agg_info->funcs[i].f_expr;
    assert( expr->op==TK_FUNCTION || expr->op==TK_AGG_FUNCTION );
    assert( EXPR_USE_X_LIST(expr) );
    __expr_analyze_agg_list(nc, expr->x.list);
    if( expr->left ){
      assert( expr->left->op==TK_ORDER );
      assert( EXPR_USE_X_LIST(expr->left) );
      __expr_analyze_agg_list(nc, expr->left->x.list);
    }
#ifndef DBSQL_OMIT_WINDOWFUNC
    assert( !IS_WINDOW_FUNC(expr) );
    if( EXPR_HAS_PROPERTY(expr, EP_WinFunc) ){
      __expr_analyze_aggregates(nc, expr->y.win->filter);
    }
#endif
  }
  nc->nc_flags &= ~NC_InAggFunc;
}

/*
** An index on expressions is being used in the inner loop of an
** aggregate query with a GROUP BY clause.  This routine attempts
** to adjust the agg_info_t object to take advantage of index and to
** perhaps use the index as a covering index.
**
*/
static void __optimize_aggregate_use_of_indexed_expr(
  parser_t *parser,          /* Parsing context */
  select_t *p_select,        /* The SELECT statement being processed */
  agg_info_t *agg_info,      /* The aggregate info */
  name_context_t *nc        /* Name context used to resolve agg-func args */
){
  assert( agg_info->i_first_reg==0 );
  assert( p_select!=0 );
  assert( p_select->group_by!=0 );
  agg_info->n_column = agg_info->n_accumulator;
  if( ALWAYS(agg_info->n_sorting_column>0) ){
    int mx = p_select->group_by->n_expr - 1;
    int j, k;
    for(j=0; j<agg_info->n_column; j++){
      k = agg_info->cols[j].i_sorter_column;
      if( k>mx ) mx = k;
    }
    agg_info->n_sorting_column = mx+1;
  }
  __analyze_agg_func_args(agg_info, nc);
#if TREETRACE_ENABLED
  if( __tree_trace & 0x20 ){
    indexed_expr_t *i_epr;
    TREETRACE(0x20, parser, p_select,
        ("AggInfo (possibly) adjusted for Indexed Exprs\n"));
    __tree_view_select(0, p_select, 0);
    for(i_epr=parser->idx_epr; i_epr; i_epr=i_epr->ie_next){
      printf("data-cursor=%d index={%d,%d}\n",
          i_epr->i_data_cur, i_epr->i_idx_cur, i_epr->i_idx_col);
      __tree_view_expr(0, i_epr->expr, 0);
    }
    __print_agg_info(agg_info);
  }
#else
  UNUSED_PARAMETER(p_select);
  UNUSED_PARAMETER(parser);
#endif
}

/*
** walker_t callback for __aggregate_convert_indexed_expr_ref_to_column().
*/
static int __aggregate_idx_epr_ref_to_col_callback(walker_t *walker, expr_t *expr){
  agg_info_t *agg_info;
  struct agg_info_col_t *col;
  UNUSED_PARAMETER(walker);
  if( expr->agg_info==0 ) return WRC_Continue;
  if( expr->op==TK_AGG_COLUMN ) return WRC_Continue;
  if( expr->op==TK_AGG_FUNCTION ) return WRC_Continue;
  if( expr->op==TK_IF_NULL_ROW ) return WRC_Continue;
  agg_info = expr->agg_info;
  if( NEVER(expr->i_agg>=agg_info->n_column) ) return WRC_Continue;
  assert( expr->i_agg>=0 );
  col = &agg_info->cols[expr->i_agg];
  expr->op = TK_AGG_COLUMN;
  expr->i_table = col->i_table;
  expr->i_column = col->i_column;
  EXPR_CLEAR_PROPERTY(expr, EP_Skip|EP_Collate|EP_Unlikely);
  return WRC_Prune;
}

/*
** Convert every agg_info->funcs[].expr such that any node within
** those expressions that has app_info set is changed into a TK_AGG_COLUMN
** opcode.
*/
static void __aggregate_convert_indexed_expr_ref_to_column(agg_info_t *agg_info){
  int i;
  walker_t w;
  memset(&w, 0, sizeof(w));
  w.expr_callback = __aggregate_idx_epr_ref_to_col_callback;
  for(i=0; i<agg_info->n_func; i++){
    __walk_expr(&w, agg_info->funcs[i].f_expr);
  }
}


/*
** Allocate a block of registers so that there is one register for each
** agg_info->cols[] and agg_info->funcs[] entry in agg_info.  The first
** register in this block is stored in agg_info->i_first_reg.
**
** This routine may only be called once for each agg_info_t object.  Prior
** to calling this routine:
**
**     *  The cols[] and funcs[] arrays may be modified
**     *  The AGG_INFO_COLUMN_REG() and AGG_INFO_FUNC_REG() macros may not be used
**
** After calling this routine:
**
**     *  The cols[] and funcs[] arrays are fixed
**     *  The AGG_INFO_COLUMN_REG() and AGG_INFO_FUNC_REG() macros may be used
**
*/
static void __assign_aggregate_registers(parser_t *parser, agg_info_t *agg_info){
  assert( agg_info!=0 );
  assert( agg_info->i_first_reg==0 );
  agg_info->i_first_reg = parser->n_mem + 1;
  parser->n_mem += agg_info->n_column + agg_info->n_func;
}

/*
** Reset the aggregate accumulator.
**
** The aggregate accumulator is a set of memory cells that hold
** intermediate results while calculating an aggregate.  This
** routine generates code that stores NULLs in all of those memory
** cells.
*/
static void __reset_accumulator(parser_t *parser, agg_info_t *agg_info){
  vdbe_t *v = parser->vdbe;
  int i;
  struct agg_info_func_t *func;
  int n_reg = agg_info->n_func + agg_info->n_column;
  assert( agg_info->i_first_reg>0 );
  assert( parser->db->parser==parser );
  assert( parser->db->malloc_failed==0 || parser->n_err!=0 );
  if( n_reg==0 ) return;
  if( parser->n_err ) return;
  __vdbe_add_op3(v, OP_Null, 0, agg_info->i_first_reg,
                    agg_info->i_first_reg+n_reg-1);
  for(func=agg_info->funcs, i=0; i<agg_info->n_func; i++, func++){
    if( func->i_distinct>=0 ){
      expr_t *e = func->f_expr;
      assert( EXPR_USE_X_LIST(e) );
      if( e->x.list==0 || e->x.list->n_expr!=1 ){
        __error_msg(parser, "DISTINCT aggregates must have exactly one "
           "argument");
        func->i_distinct = -1;
      }else{
        key_info_t *key_info = __key_info_from_expr_list(parser, e->x.list,0,0);
        func->i_dist_addr = __vdbe_add_op4(v, OP_OpenEphemeral,
            func->i_distinct, 0, 0, (char*)key_info, P4_KEYINFO);
        EXPLAIN_QUERY_PLAN((parser, 0, "USE TEMP B-TREE FOR %s(DISTINCT)",
                          func->func->name));
      }
    }
    if( func->i_ob_tab>=0 ){
      expr_list_t *ob_list;
      key_info_t *key_info;
      int n_extra = 0;
      assert( func->f_expr->left!=0 );
      assert( func->f_expr->left->op==TK_ORDER );
      assert( EXPR_USE_X_LIST(func->f_expr->left) );
      assert( func->func!=0 );
      ob_list = func->f_expr->left->x.list;
      if( !func->ob_unique ){
        n_extra++;  /* One extra column for the OP_Sequence */
      }
      if( func->ob_payload ){
        /* extra columns for the function arguments */
        assert( EXPR_USE_X_LIST(func->f_expr) );
        assert( func->f_expr->x.list!=0 );
        n_extra += func->f_expr->x.list->n_expr;
      }
      if( func->use_subtype ){
        n_extra += func->f_expr->x.list->n_expr;
      }
      key_info = __key_info_from_expr_list(parser, ob_list, 0, n_extra);
      if( !func->ob_unique && parser->n_err==0 ){
        key_info->n_key_field++;
      }
      __vdbe_add_op4(v, OP_OpenEphemeral,
            func->i_ob_tab, ob_list->n_expr+n_extra, 0,
            (char*)key_info, P4_KEYINFO);
      EXPLAIN_QUERY_PLAN((parser, 0, "USE TEMP B-TREE FOR %s(ORDER BY)",
                          func->func->name));
    }
  }
}

/*
** Invoke the __op_agg_finalize opcode for every aggregate function
** in the agg_info_t structure.
*/
static void __finalize_agg_functions(parser_t *parser, agg_info_t *agg_info){
  vdbe_t *v = parser->vdbe;
  int i;
  struct agg_info_func_t *f;
  for(i=0, f=agg_info->funcs; i<agg_info->n_func; i++, f++){
    expr_list_t *list;
    assert( EXPR_USE_X_LIST(f->f_expr) );
    if( parser->n_err ) return;
    list = f->f_expr->x.list;
    if( f->i_ob_tab>=0 ){
      /* For an ORDER BY aggregate, calls to OP_AggStep were deferred.  Inputs
      ** were stored in emphermal table f->i_ob_tab.  Here, we extract those
      ** inputs (in ORDER BY order) and make all calls to OP_AggStep
      ** before doing the OP_AggFinal call. */
      int i_top;        /* Start of loop for extracting columns */
      int n_arg;        /* Number of columns to extract */
      int n_key;        /* Key columns to be skipped */
      int reg_agg;      /* Extract into this array */
      int j;           /* Loop counter */
     
      assert( f->func!=0 );
      n_arg = list->n_expr;
      reg_agg = __get_temp_range(parser, n_arg);

      if( f->ob_payload==0 ){
        n_key = 0;
      }else{
        assert( f->f_expr->left!=0 );
        assert( EXPR_USE_X_LIST(f->f_expr->left) );
        assert( f->f_expr->left->x.list!=0 );
        n_key = f->f_expr->left->x.list->n_expr;
        if( ALWAYS(!f->ob_unique) ) n_key++;
      }
      i_top = __vdbe_add_op1(v, OP_Rewind, f->i_ob_tab); VDBE_COVERAGE(v);
      for(j=n_arg-1; j>=0; j--){
        __vdbe_add_op3(v, OP_Column, f->i_ob_tab, n_key+j, reg_agg+j);
      }
      if( f->use_subtype ){
        int reg_subtype = __get_temp_reg(parser);
        int i_base_col = n_key + n_arg + (f->ob_payload==0 && f->ob_unique==0);
        for(j=n_arg-1; j>=0; j--){
          __vdbe_add_op3(v, OP_Column, f->i_ob_tab, i_base_col+j, reg_subtype);
          __vdbe_add_op2(v, OP_SetSubtype, reg_subtype, reg_agg+j);
        }
        __release_temp_reg(parser, reg_subtype);
      }
      __vdbe_add_op3(v, OP_AggStep, 0, reg_agg, AGG_INFO_FUNC_REG(agg_info,i));
      __vdbe_append_p4(v, f->func, P4_FUNCDEF);
      __vdbe_change_p5(v, (u_int16_t)n_arg);
      __vdbe_add_op2(v, OP_Next, f->i_ob_tab, i_top+1); VDBE_COVERAGE(v);
      __vdbe_jump_here(v, i_top);
      __release_temp_range(parser, reg_agg, n_arg);
    }
    __vdbe_add_op2(v, OP_AggFinal, AGG_INFO_FUNC_REG(agg_info,i),
                      list ? list->n_expr : 0);
    __vdbe_append_p4(v, f->func, P4_FUNCDEF);
  }
}

/*
** Generate code that will update the accumulator memory cells for an
** aggregate based on the current cursor position.
**
** If reg_acc is non-zero and there are no min() or max() aggregates
** in agg_info, then only populate the agg_info->n_accumulator accumulator
** registers if register reg_acc contains 0. The caller will take care
** of setting and clearing reg_acc.
**
** For an ORDER BY aggregate, the actual accumulator memory cell update
** is deferred until after all input rows have been received, so that they
** can be run in the requested order.  In that case, instead of invoking
** OP_AggStep to update the accumulator, just add the arguments that would
** have been passed into OP_AggStep into the sorting ephemeral table
** (along with the appropriate sort key).
*/
static void __update_accumulator(
  parser_t *parser,
  int reg_acc,
  agg_info_t *agg_info,
  int distinct_type
){
  vdbe_t *v = parser->vdbe;
  int i;
  int reg_hit = 0;
  int addr_hit_test = 0;
  struct agg_info_func_t *f;
  struct agg_info_col_t *c;

  assert( agg_info->i_first_reg>0 );
  if( parser->n_err ) return;
  agg_info->direct_mode = 1;
  for(i=0, f=agg_info->funcs; i<agg_info->n_func; i++, f++){
    int n_arg;
    int addr_next = 0;
    int reg_agg;
    int reg_agg_sz = 0;
    int reg_distinct = 0;
    expr_list_t *list;
    assert( EXPR_USE_X_LIST(f->f_expr) );
    assert( !IS_WINDOW_FUNC(f->f_expr) );
    assert( f->func!=0 );
    list = f->f_expr->x.list;
    if( EXPR_HAS_PROPERTY(f->f_expr, EP_WinFunc) ){
      expr_t *filter = f->f_expr->y.win->filter;
      if( agg_info->n_accumulator
       && (f->func->func_flags & DBSQL_FUNC_NEEDCOLL)
       && reg_acc
      ){
        /* If reg_acc==0, there there exists some min() or max() function
        ** without a FILTER clause that will ensure the magnet registers
        ** are populated. */
        if( reg_hit==0 ) reg_hit = ++parser->n_mem;
        /* If this is the first row of the group (reg_acc contains 0), clear the
        ** "magnet" register reg_hit so that the accumulator registers
        ** are populated if the FILTER clause jumps over the the
        ** invocation of min() or max() altogether. Or, if this is not
        ** the first row (reg_acc contains 1), set the magnet register so that
        ** the accumulators are not populated unless the min()/max() is invoked
        ** and indicates that they should be.  */
        __vdbe_add_op2(v, OP_Copy, reg_acc, reg_hit);
      }
      addr_next = __vdbe_make_label(parser);
      __expr_if_false(parser, filter, addr_next, DBSQL_JUMPIFNULL);
    }
    if( f->i_ob_tab>=0 ){
      /* Instead of invoking __agg_step, we must push the arguments that would
      ** have been passed to __agg_step onto the sorting table. */
      int jj;                /* Registered used so far in building the record */
      expr_list_t *ob_list;     /* The ORDER BY clause */
      assert( list!=0 );
      n_arg = list->n_expr;
      assert( n_arg>0 );
      assert( f->f_expr->left!=0 );
      assert( f->f_expr->left->op==TK_ORDER );
      assert( EXPR_USE_X_LIST(f->f_expr->left) );
      ob_list = f->f_expr->left->x.list;
      assert( ob_list!=0 );
      assert( ob_list->n_expr>0 );
      reg_agg_sz = ob_list->n_expr;
      if( !f->ob_unique ){
        reg_agg_sz++;   /* One register for OP_Sequence */
      }
      if( f->ob_payload ){
        reg_agg_sz += n_arg;
      }
      if( f->use_subtype ){
        reg_agg_sz += n_arg;
      }
      reg_agg_sz++;  /* One extra register to hold result of __make_record */
      reg_agg = __get_temp_range(parser, reg_agg_sz);
      reg_distinct = reg_agg;
      __expr_code_expr_list(parser, ob_list, reg_agg, 0, DBSQL_ECEL_DUP);
      jj = ob_list->n_expr;
      if( !f->ob_unique ){
        __vdbe_add_op2(v, OP_Sequence, f->i_ob_tab, reg_agg+jj);
        jj++;
      }
      if( f->ob_payload ){
        reg_distinct = reg_agg+jj;
        __expr_code_expr_list(parser, list, reg_distinct, 0, DBSQL_ECEL_DUP);
        jj += n_arg;
      }
      if( f->use_subtype ){
        int kk;
        int reg_base = f->ob_payload ? reg_distinct : reg_agg;
        for(kk=0; kk<n_arg; kk++, jj++){
          __vdbe_add_op2(v, OP_GetSubtype, reg_base+kk, reg_agg+jj);
        }
      }
    }else if( list ){
      n_arg = list->n_expr;
      reg_agg = __get_temp_range(parser, n_arg);
      reg_distinct = reg_agg;
      __expr_code_expr_list(parser, list, reg_agg, 0, DBSQL_ECEL_DUP);
    }else{
      n_arg = 0;
      reg_agg = 0;
    }
    if( f->i_distinct>=0 && list ){
      if( addr_next==0 ){
        addr_next = __vdbe_make_label(parser);
      }
      f->i_distinct = __code_distinct(parser, distinct_type,
          f->i_distinct, addr_next, list, reg_distinct);
    }
    if( f->i_ob_tab>=0 ){
      /* Insert a new record into the ORDER BY table */
      __vdbe_add_op3(v, OP_MakeRecord, reg_agg, reg_agg_sz-1,
                        reg_agg+reg_agg_sz-1);
      __vdbe_add_op4_int(v, OP_IdxInsert, f->i_ob_tab, reg_agg+reg_agg_sz-1,
                           reg_agg, reg_agg_sz-1);
      __release_temp_range(parser, reg_agg, reg_agg_sz);
    }else{
      /* Invoke the __agg_step function */
      if( f->func->func_flags & DBSQL_FUNC_NEEDCOLL ){
        coll_seq_t *coll = 0;
        struct expr_list_item_t *item;
        int j;
        assert( list!=0 );  /* list!=0 if f->func has NEEDCOLL */
        for(j=0, item=list->a; !coll && j<n_arg; j++, item++){
          coll = __expr_coll_seq(parser, item->expr);
        }
        if( !coll ){
          coll = parser->db->dflt_coll;
        }
        if( reg_hit==0 && agg_info->n_accumulator ) reg_hit = ++parser->n_mem;
        __vdbe_add_op4(v, OP_CollSeq, reg_hit, 0, 0,
                         (char *)coll, P4_COLLSEQ);
      }
      __vdbe_add_op3(v, OP_AggStep, 0, reg_agg, AGG_INFO_FUNC_REG(agg_info,i));
      __vdbe_append_p4(v, f->func, P4_FUNCDEF);
      __vdbe_change_p5(v, (u_int16_t)n_arg);
      __release_temp_range(parser, reg_agg, n_arg);
    }
    if( addr_next ){
      __vdbe_resolve_label(v, addr_next);
    }
    if( parser->n_err ) return;
  }
  if( reg_hit==0 && agg_info->n_accumulator ){
    reg_hit = reg_acc;
  }
  if( reg_hit ){
    addr_hit_test = __vdbe_add_op1(v, OP_If, reg_hit); VDBE_COVERAGE(v);
  }
  for(i=0, c=agg_info->cols; i<agg_info->n_accumulator; i++, c++){
    __expr_code(parser, c->c_expr, AGG_INFO_COLUMN_REG(agg_info,i));
    if( parser->n_err ) return;
  }

  agg_info->direct_mode = 0;
  if( addr_hit_test ){
    __vdbe_jump_here_or_pop_inst(v, addr_hit_test);
  }
}

/*
** Add a single OP_Explain instruction to the VDBE to explain a simple
** count(*) query ("SELECT count(*) FROM tab").
*/
#ifndef DBSQL_OMIT_EXPLAIN
static void EXPLAIN_SIMPLE_COUNT(
  parser_t *parser,                  /* parser_t context */
  table_t *tab,                    /* table_t being queried */
  index_t *idx                     /* index_t used to optimize scan, or NULL */
){
  if( parser->explain==2 ){
    int cover = (idx!=0 && (HAS_ROWID(tab) || !IS_PRIMARY_KEY_INDEX(idx)));
    __vdbe_explain(parser, 0, "SCAN %s%s%s",
        tab->name,
        cover ? " USING COVERING INDEX " : "",
        cover ? idx->name : ""
    );
  }
}
#else
# define EXPLAIN_SIMPLE_COUNT(a,b,c)
#endif

/*
** __walk_expr() callback used by __having_to_where().
**
** If the node passed to the callback is a TK_AND node, return
** WRC_Continue to tell __walk_expr() to iterate through child nodes.
**
** Otherwise, return WRC_Prune. In this case, also check if the
** sub-expression matches the criteria for being moved to the WHERE
** clause. If so, add it to the WHERE clause and replace the sub-expression
** within the HAVING expression with a constant "1".
*/
static int __having_to_where_expr_cb(walker_t *walker, expr_t *expr){
  if( expr->op!=TK_AND ){
    select_t *s = walker->u.p_select;
    /* This routine is called before the HAVING clause of the current
    ** SELECT is analyzed for aggregates. So if expr->agg_info is set
    ** here, it indicates that the expression is a correlated reference to a
    ** column from an outer aggregate query, or an aggregate function that
    ** belongs to an outer query. Do not move the expression to the WHERE
    ** clause in this obscure case, as doing so may corrupt the outer select_t
    ** statements agg_info_t structure.  */
    if( __expr_is_constant_or_group_by(walker->parser, expr, s->group_by)
     && EXPR_ALWAYS_FALSE(expr)==0
     && expr->agg_info==0
    ){
      DBSQL *db = walker->parser->db;
      expr_t *p_new = __expr_int32(db, 1);
      if( p_new ){
        expr_t *where = s->where;
        SWAP(expr_t, *p_new, *expr);
        p_new = __expr_and(walker->parser, where, p_new);
        s->where = p_new;
        walker->code = 1;
      }
    }
    return WRC_Prune;
  }
  return WRC_Continue;
}

/*
** Transfer eligible terms from the HAVING clause of a query, which is
** processed after grouping, to the WHERE clause, which is processed before
** grouping. For example, the query:
**
**   SELECT * FROM <tables> WHERE a=? GROUP BY b HAVING b=? AND c=?
**
** can be rewritten as:
**
**   SELECT * FROM <tables> WHERE a=? AND b=? GROUP BY b HAVING c=?
**
** A term of the HAVING expression is eligible for transfer if it consists
** entirely of constants and expressions that are also GROUP BY terms that
** use the "BINARY" collation sequence.
*/
static void __having_to_where(parser_t *parser, select_t *p){
  walker_t s_walker;
  memset(&s_walker, 0, sizeof(s_walker));
  s_walker.parser = parser;
  s_walker.expr_callback = __having_to_where_expr_cb;
  s_walker.u.p_select = p;
  __walk_expr(&s_walker, p->having);
#if TREETRACE_ENABLED
  if( s_walker.code && (__tree_trace & 0x100)!=0 ){
    TREETRACE(0x100,parser,p,("Move HAVING terms into WHERE:\n"));
    __tree_view_select(0, p, 0);
  }
#endif
}

/*
** Check to see if the p_this entry of tab_list is a self-join of another view.
** Search FROM-clause entries in the range of i_first..i_end, including i_first
** but stopping before i_end.
**
** If p_this is a self-join, then return the src_item_t for the first other
** instance of that view found.  If p_this is not a self-join then return 0.
*/
static src_item_t *__is_self_join_view(
  src_list_t *tab_list,           /* Search for self-joins in this FROM clause */
  src_item_t *p_this,              /* Search for prior reference to this subquery */
  int i_first, int i_end        /* Range of FROM-clause entries to search. */
){
  src_item_t *item;
  select_t *sel;
  assert( p_this->fg.is_subquery );
  sel = p_this->u4.subq->p_select;
  assert( sel!=0 );
  if( sel->sel_flags & SF_PushDown ) return 0;
  while( i_first<i_end ){
    select_t *s1;
    item = &tab_list->a[i_first++];
    if( !item->fg.is_subquery ) continue;
    if( item->fg.via_coroutine ) continue;
    if( item->name==0 ) continue;
    assert( item->s_tab!=0 );
    assert( p_this->s_tab!=0 );
    if( item->s_tab->schema!=p_this->s_tab->schema ) continue;
    if( dbsql_stricmp(item->name, p_this->name)!=0 ) continue;
    s1 = item->u4.subq->p_select;
    if( item->s_tab->schema==0 && sel->sel_id!=s1->sel_id ){
      /* The query flattener left two different CTE tables with identical
      ** names in the same FROM clause. */
      continue;
    }
    if( s1->sel_flags & SF_PushDown ){
      /* The view was modified by some other optimization such as
      ** __push_down_where_terms() */
      continue;
    }
    return item;
  }
  return 0;
}

/*
** Deallocate a single agg_info_t object
*/
static void __agginfo_free(DBSQL *db, void *arg){
  agg_info_t *p = (agg_info_t*)arg;
  __dbsql_db_free(db, p->cols);
  __dbsql_db_free(db, p->funcs);
  __db_free_nn(db, p);
}

/*
** Attempt to transform a query of the form
**
**    SELECT count(*) FROM (SELECT x FROM t1 UNION ALL SELECT y FROM t2)
**
** Into this:
**
**    SELECT (SELECT count(*) FROM t1)+(SELECT count(*) FROM t2)
**
** The transformation only works if all of the following are true:
**
**   *  The subquery is a UNION ALL of two or more terms
**   *  The subquery does not have a LIMIT clause
**   *  There is no WHERE or GROUP BY or HAVING clauses on the subqueries
**   *  The outer query is a simple count(*) with no WHERE clause or other
**      extraneous syntax.
**   *  None of the subqueries are DISTINCT (forumpost/a860f5fb2e 2025-03-10)
**
** Return TRUE if the optimization is undertaken.
*/
static int __count_of_view_optimization(parser_t *parser, select_t *p){
  select_t *sub, *prior;
  expr_t *expr;
  expr_t *count;
  DBSQL *db;
  src_item_t *from;
  if( (p->sel_flags & SF_Aggregate)==0 ) return 0;   /* This is an aggregate */
  if( p->e_list->n_expr!=1 ) return 0;               /* Single result column */
  if( p->where ) return 0;
  if( p->having ) return 0;
  if( p->group_by ) return 0;
  if( p->order_by ) return 0;
  expr = p->e_list->a[0].expr;
  if( expr->op!=TK_AGG_FUNCTION ) return 0;        /* Result is an aggregate */
  assert( EXPR_USE_U_TOKEN(expr) );
  if( dbsql_stricmp(expr->u.token,"count") ) return 0;  /* Is count() */
  assert( EXPR_USE_X_LIST(expr) );
  if( expr->x.list!=0 ) return 0;                 /* Must be count(*) */
  if( p->src->n_src!=1 ) return 0;                  /* One table in FROM  */
  if( EXPR_HAS_PROPERTY(expr, EP_WinFunc) ) return 0;/* Not a window function */
  from = p->src->a;
  if( from->fg.is_subquery==0 ) return 0;    /* FROM is a subquery */
  sub = from->u4.subq->p_select;
  if( sub->prior==0 ) return 0;                   /* Must be a compound */
  if( sub->sel_flags & SF_CopyCte ) return 0;       /* Not a CTE */
  do{
    if( sub->op!=TK_ALL && sub->prior ) return 0;  /* Must be UNION ALL */
    if( sub->where ) return 0;                      /* No WHERE clause */
    if( sub->limit ) return 0;                      /* No LIMIT clause */
    if( sub->sel_flags & (SF_Aggregate|SF_Distinct) ){
       testcase( sub->sel_flags & SF_Aggregate );
       testcase( sub->sel_flags & SF_Distinct );
       return 0;     /* Not an aggregate nor DISTINCT */
    }
    assert( sub->having==0 );  /* Due to the previous */
    sub = sub->prior;                              /* Repeat over compound */
  }while( sub );

  /* If we reach this point then it is OK to perform the transformation */

  db = parser->db;
  count = expr;
  expr = 0;
  sub = __subquery_detach(db, from);
  __src_list_delete(db, p->src);
  p->src = __db_malloc_zero(parser->db, SZ_SRCLIST_1);
  while( sub ){
    expr_t *term;
    prior = sub->prior;
    sub->prior = 0;
    sub->next = 0;
    sub->sel_flags |= SF_Aggregate;
    sub->sel_flags &= ~(u_int32_t)SF_Compound;
    sub->n_select_row = 0;
    __parser_add_cleanup(parser, __expr_list_delete_generic, sub->e_list);
    term = prior ? __expr_dup(db, count, 0) : count;
    sub->e_list = __expr_list_append(parser, 0, term);
    term = __p_expr(parser, TK_SELECT, 0, 0);
    __p_expr_add_select(parser, term, sub);
    if( expr==0 ){
      expr = term;
    }else{
      expr = __p_expr(parser, TK_PLUS, term, expr);
    }
    sub = prior;
  }
  p->e_list->a[0].expr = expr;
  p->sel_flags &= ~(u_int32_t)SF_Aggregate;

#if TREETRACE_ENABLED
  if( __tree_trace & 0x200 ){
    TREETRACE(0x200,parser,p,("After count-of-view optimization:\n"));
    __tree_view_select(0, p, 0);
  }
#endif
  return 1;
}

/*
** If any term of src, or any SF_NestedFrom sub-query, is not the same
** as src_item but has the same alias as p0, then return true.
** Otherwise return false.
*/
static int __same_src_alias(src_item_t *p0, src_list_t *src){
  int i;
  for(i=0; i<src->n_src; i++){
    src_item_t *p1 = &src->a[i];
    if( p1==p0 ) continue;
    if( p0->s_tab==p1->s_tab && 0==dbsql_stricmp(p0->alias, p1->alias) ){
      return 1;
    }
    if( p1->fg.is_subquery
     && (p1->u4.subq->p_select->sel_flags & SF_NestedFrom)!=0
     && __same_src_alias(p0, p1->u4.subq->p_select->src)
    ){
      return 1;
    }
  }
  return 0;
}

/*
** Return TRUE (non-zero) if the i-th entry in the tab_list src_list_t can
** be implemented as a co-routine.  The i-th entry is guaranteed to be
** a subquery.
**
** The subquery is implemented as a co-routine if all of the following are
** true:
**
**    (1)  The subquery will likely be implemented in the outer loop of
**         the query.  This will be the case if any one of the following
**         conditions hold:
**         (a)  The subquery is the only term in the FROM clause
**         (b)  The subquery is the left-most term and a CROSS JOIN or similar
**              requires it to be the outer loop
**         (c)  All of the following are true:
**                (i) The subquery is the left-most subquery in the FROM clause
**               (ii) There is nothing that would prevent the subquery from
**                    being used as the outer loop if the __where_begin()
**                    routine nominates it to that position.
**              (iii) The query is not a UPDATE ... FROM
**    (2)  The subquery is not a CTE that should be materialized because
**         (a) the AS MATERIALIZED keyword is used, or
**         (b) the CTE is used multiple times and does not have the
**             NOT MATERIALIZED keyword
**    (3)  The subquery is not part of a left operand for a RIGHT JOIN
**    (4)  The DBSQL_Coroutine optimization disable flag is not set
**    (5)  The subquery is not self-joined
*/
static int __from_clause_term_can_be_coroutine(
  parser_t *parser,          /* Parsing context */
  src_list_t *tab_list,      /* FROM clause */
  int i,                  /* Which term of the FROM clause holds the subquery */
  int sel_flags            /* Flags on the SELECT statement */
){
  src_item_t *item = &tab_list->a[i];
  if( item->fg.is_cte ){
    const cte_use_t *cte_use = item->u2.cte_use;
    if( cte_use->m10d==M10D_YES ) return 0;                          /* (2a) */
    if( cte_use->n_use>=2 && cte_use->m10d!=M10D_NO ) return 0;       /* (2b) */
  }
  if( tab_list->a[0].fg.jointype & JT_LTORJ ) return 0;               /* (3)  */
  if( OPTIMIZATION_DISABLED(parser->db, DBSQL_Coroutines) ) return 0; /* (4)  */
  if( __is_self_join_view(tab_list, item, i+1, tab_list->n_src)!=0 ){
    return 0;                                                          /* (5) */
  }
  if( i==0 ){
    if( tab_list->n_src==1 ) return 1;                             /* (1a) */
    if( tab_list->a[1].fg.jointype & JT_CROSS ) return 1;         /* (1b) */
    if( sel_flags & SF_UpdateFrom )              return 0;         /* (1c-iii) */
    return 1;
  }
  if( sel_flags & SF_UpdateFrom ) return 0;                        /* (1c-iii) */
  while( 1 /*exit-by-break*/ ){
    if( item->fg.jointype & (JT_OUTER|JT_CROSS)  ) return 0;     /* (1c-ii) */
    if( i==0 ) break;
    i--;
    item--;
    if( item->fg.is_subquery ) return 0;                          /* (1c-i) */
  }
  return 1;
}

/*
** Argument where is the WHERE clause belonging to SELECT statement p. This
** function attempts to transform expressions of the form:
**
**     EXISTS (SELECT ...)
**
** into joins. For example, given
**
**    CREATE TABLE sailors(sid INTEGER PRIMARY KEY, name TEXT);
**    CREATE TABLE reserves(sid INT, day DATE, PRIMARY KEY(sid, day));
**
**    SELECT name FROM sailors AS S WHERE EXISTS (
**      SELECT * FROM reserves AS R WHERE S.sid = R.sid AND R.day = '2022-10-25'
**    );
**
** the SELECT statement may be transformed as follows:
**
**    SELECT name FROM sailors AS S, reserves AS R
**      WHERE S.sid = R.sid AND R.day = '2022-10-25';
**
** **Approximately**.  Really, we have to ensure that the FROM-clause term
** that was formerly inside the EXISTS is only executed once.  This is handled
** by setting the src_item_t.fg.from_exists flag, which then causes code in
** the where.c file to exit the corresponding loop after the first successful
** match (if any).
*/
static DBSQL_NOINLINE void __exists_to_join(
  parser_t *parser,  /* Parsing context */
  select_t *p,      /* The SELECT statement being optimized */
  expr_t *where    /* part of the WHERE clause currently being examined */
){
  if( parser->n_err==0
   && where!=0
   && !EXPR_HAS_PROPERTY(where, EP_OuterON|EP_InnerON) 
   && ALWAYS(p->src!=0)
   && p->src->n_src<BMS
   && (p->limit==0 || p->limit->right==0)
  ){
    if( where->op==TK_AND ){
      expr_t *right = where->right;
      __exists_to_join(parser, p, where->left);
      __exists_to_join(parser, p, right);
    }
    else if( where->op==TK_EXISTS ){
      select_t *sub = where->x.p_select;
      expr_t *sub_where = sub->where;
      if( sub->src->n_src==1
       && (sub->sel_flags & SF_Aggregate)==0
       && !sub->src->a[0].fg.is_subquery
       && sub->limit==0
       && sub->prior==0
      ){
        /* Before combining the sub-select with the parent, renumber the 
        ** cursor used by the subselect. This is because the EXISTS expression
        ** might be a copy of another EXISTS expression from somewhere
        ** else in the tree, and in this case it is important that it use
        ** a unique cursor number.  */
        DBSQL *db = parser->db;
        int *csr_maps = __db_malloc_zero(db, (parser->n_tab+2)*sizeof(int));
        if( csr_maps==0 ) return;
        csr_maps[0] = (parser->n_tab+1);
        __renumber_cursors(parser, sub, -1, csr_maps);
        __dbsql_db_free(db, csr_maps);

        memset(where, 0, sizeof(*where));
        where->op = TK_INTEGER;
        where->u.i_value = 1;
        EXPR_SET_PROPERTY(where, EP_IntValue);
        assert( p->where!=0 );
        sub->src->a[0].fg.from_exists = 1;
        p->src = __src_list_append_list(parser, p->src, sub->src);
        if( sub_where ){
          p->where = __p_expr(parser, TK_AND, p->where, sub_where);
          sub->where = 0;
        }
        sub->src = 0;
        __parser_add_cleanup(parser, __select_delete_generic, sub);
#if TREETRACE_ENABLED
        if( __tree_trace & 0x100000 ){
          TREETRACE(0x100000,parser,p,
                    ("After EXISTS-to-JOIN optimization:\n"));
          __tree_view_select(0, p, 0);
        }
#endif
      }
    }
  }
}

/*
** Type used for walker_t callbacks by select_check_on_clauses().
*/
typedef struct check_on_ctx_t check_on_ctx_t;
struct check_on_ctx_t {
  src_list_t *src;                  /* src_list_t for this context */
  int i_join;                      /* Cursor numbers must be =< than this */
  int func_arg;                   /* True for table-function arg */
  check_on_ctx_t *parent;            /* Parent context */
};

/*
** True if the src_list_t passed as the only argument contains at least
** one RIGHT or FULL JOIN. False otherwise.  
*/
#define HAS_RIGHT_JOIN(src) (((src)->a[0].fg.jointype & JT_LTORJ)!=0)

/*
** The expr callback for the search of invalid ON clause terms.
*/
static int __select_check_on_clauses_expr(walker_t *walker, expr_t *expr){
  check_on_ctx_t *ctx = walker->u.check_on_ctx;

  /* Check if expr is root or near-root of an ON clause constraint that needs
  ** to be checked to ensure that it does not refer to tables in its FROM
  ** clause to the right of itself. i.e. it is either:
  **
  **   + an ON clause on an OUTER join, or
  **   + an ON clause on an INNER join within a FROM that features at
  **     least one RIGHT or FULL join.
  */
  if( (EXPR_HAS_PROPERTY(expr, EP_OuterON))
   || (EXPR_HAS_PROPERTY(expr, EP_InnerON) && HAS_RIGHT_JOIN(ctx->src))
  ){
    /* If check_on_ctx_t.i_join is already set, then fall through and process
    ** this expression node as normal. Or, if check_on_ctx_t.i_join is still 0,
    ** set it to the cursor number of the RHS of the join to which this
    ** ON expression was attached and then iterate through the entire 
    ** expression.  */
    assert( ctx->i_join==0 || ctx->i_join==expr->w.i_join );
    if( ctx->i_join==0 ){
      ctx->i_join = expr->w.i_join;
      __walk_expr_nn(walker, expr);
      ctx->i_join = 0;
      return WRC_Prune;
    }
  }

  if( expr->op==TK_COLUMN ){
    /* A column expression. Find the src_list_t (if any) to which it refers.
    ** Then, if check_on_ctx_t.i_join indicates that this expression is part of an
    ** ON clause from that src_list_t (i.e. if i_join is non-zero), check that it
    ** does not refer to a table to the right of check_on_ctx_t.i_join. */
    do {
      src_list_t *src = ctx->src;
      int n_src = src->n_src;
      int i_tab = expr->i_table;
      int ii;
      for(ii=0; ii<n_src && src->a[ii].i_cursor!=i_tab; ii++){}
      if( ii<n_src ){
        if( ctx->i_join && i_tab>ctx->i_join ){
          __error_msg(walker->parser, 
              "%s references tables to its right",
              (ctx->func_arg ? "table-function argument" : "ON clause")
          );
          return WRC_Abort;
        }
        break;
      }
      ctx = ctx->parent;
    }while( ctx );
  }
  return WRC_Continue;
}

/*
** The x_select callback for the search of invalid ON clause terms.
*/
static int __select_check_on_clauses_select(walker_t *walker, select_t *p_select){
  check_on_ctx_t *ctx = walker->u.check_on_ctx;
  if( p_select->src==ctx->src || p_select->src->n_src==0 ){
    return WRC_Continue;
  }else{
    check_on_ctx_t s_ctx;
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.src = p_select->src;
    s_ctx.parent = ctx;
    walker->u.check_on_ctx = &s_ctx;
    __walk_select(walker, p_select);
    walker->u.check_on_ctx = ctx;
    p_select->sel_flags &= ~SF_OnToWhere;
    return WRC_Prune;
  }
}

/*
** Check all ON clauses in p_select to verify that they do not reference
** columns to the right.
*/
void __select_check_on_clauses(parser_t *parser, select_t *p_select){
  walker_t w;
  check_on_ctx_t s_ctx;
  int ii;
  assert( p_select->sel_flags & SF_OnToWhere );
  assert( p_select->src!=0 && p_select->src->n_src>=2 );
  memset(&w, 0, sizeof(w));
  w.parser = parser;
  w.expr_callback = __select_check_on_clauses_expr;
  w.select_callback = __select_check_on_clauses_select;
  w.u.check_on_ctx = &s_ctx;
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_ctx.src = p_select->src;
  __walk_expr(&w, p_select->where);
  p_select->sel_flags &= ~SF_OnToWhere;

  /* Check for any table-function args that are attached to virtual tables 
  ** on the RHS of an outer join. They are subject to the same constraints
  ** as ON clauses. */
  s_ctx.func_arg = 1;
  for(ii=0; ii<p_select->src->n_src; ii++){
    src_item_t *item = &p_select->src->a[ii];
    if( item->fg.is_tab_func
     && (item->fg.jointype & JT_OUTER)
    ){
      s_ctx.i_join = item->i_cursor;
      __walk_expr_list(&w, item->u1.func_arg);
    }
  }
}

/*
** If p2 exists and p1 and p2 have the same number of terms, then change
** every term of p1 to have the same sort order as p2 and return true.
**
** If p2 is NULL or p1 and p2 are different lengths, then make no changes
** and return false.
**
** p1 must be non-NULL.
*/
static int __copy_sort_order(expr_list_t *p1, expr_list_t *p2){
  assert( p1 );
  if( p2 && p1->n_expr==p2->n_expr ){
    int ii;
    for(ii=0; ii<p1->n_expr; ii++){
      u_int8_t sort_flags;
      sort_flags = p2->a[ii].fg.sort_flags & KEYINFO_ORDER_DESC;
      p1->a[ii].fg.sort_flags = sort_flags;
    }
    return 1;
  }else{
    return 0;
  }
}

/*
** Generate byte-code for the SELECT statement given in the p argument. 
**
** The results are returned according to the select_dest_t structure.
** See comments in dbsql_int.h for further information.
**
** This routine returns the number of errors.  If any errors are
** encountered, then an appropriate error message is left in
** parser->errmsg.
**
** This routine does NOT free the select_t structure passed in.  The
** calling function needs to do that.
**
** This is a long function.  The following is an outline of the processing
** steps, with tags referencing various milestones:
**
**  *  Resolve names and similar preparation                tag-select-0100
**  *  Scan of the FROM clause                              tag-select-0200
**      +  OUTER JOIN strength reduction                      tag-select-0220
**      +  Sub-query ORDER BY removal                         tag-select-0230
**      +  Query flattening                                   tag-select-0240
**  *  Separate subroutine for compound-SELECT              tag-select-0300
**  *  WHERE-clause constant propagation                    tag-select-0330
**  *  Count()-of-VIEW optimization                         tag-select-0350
**  *  Scan of the FROM clause again                        tag-select-0400
**      +  Authorize unreferenced tables                      tag-select-0410
**      +  Predicate push-down optimization                   tag-select-0420
**      +  Omit unused subquery columns optimization          tag-select-0440
**      +  Generate code to implement subqueries              tag-select-0480
**         -  Co-routines                                       tag-select-0482
**         -  Reuse previously computed CTE                     tag-select-0484
**         -  REuse previously computed VIEW                    tag-select-0486
**         -  Materialize a VIEW or CTE                         tag-select-0488
**  *  DISTINCT ORDER BY -> GROUP BY optimization           tag-select-0500
**  *  Set up for ORDER BY                                  tag-select-0600
**  *  Create output table                                  tag-select-0630
**  *  Prepare registers for LIMIT                          tag-select-0650
**  *  Setup for DISTINCT                                   tag-select-0680
**  *  Generate code for non-aggregate and non-GROUP BY     tag-select-0700
**  *  Generate code for aggregate and/or GROUP BY          tag-select-0800
**      +  GROUP BY queries                                   tag-select-0810
**      +  non-GROUP BY queries                               tag-select-0820
**         -  Special case of count() w/o GROUP BY              tag-select-0821
**         -  General case of non-GROUP BY aggregates           tag-select-0822
**  *  Sort results, as needed                              tag-select-0900
**  *  Internal self-checks                                 tag-select-1000
*/
int __select(
  parser_t *parser,         /* The parser context */
  select_t *p,             /* The SELECT statement being coded. */
  select_dest_t *p_dest      /* What to do with the query results */
){
  int i, j;              /* Loop counters */
  where_info_t *w_info;     /* Return from __where_begin() */
  vdbe_t *v;               /* The virtual machine under construction */
  int is_agg;             /* True for select lists like "count(*)" */
  expr_list_t *e_list = 0;  /* List of columns to extract. */
  src_list_t *tab_list;     /* List of tables to select from */
  expr_t *where;          /* The WHERE clause.  May be NULL */
  expr_list_t *group_by;    /* The GROUP BY clause.  May be NULL */
  expr_t *having;         /* The HAVING clause.  May be NULL */
  agg_info_t *agg_info = 0; /* Aggregate information */
  int rc = 1;            /* Value to return from this function */
  distinct_ctx_t s_distinct; /* Info on how to code the DISTINCT keyword */
  sort_ctx_t s_sort;         /* Info on how to code the ORDER BY clause */
  int i_end;              /* Address of the end of the query */
  DBSQL *db;           /* The database connection */
  expr_list_t *min_max_order_by = 0;  /* Added ORDER BY for min/max queries */
  u_int8_t min_max_flag;                 /* Flag for min/max queries */

  db = parser->db;
  assert( parser==db->parser );
  v = __get_vdbe(parser);
  if( p==0 || parser->n_err ){
    return 1;
  }
  assert( db->malloc_failed==0 );
  if( __auth_check(parser, DBSQL_SELECT, 0, 0, 0) ) return 1;
#if TREETRACE_ENABLED
  TREETRACE(0x1,parser,p, ("begin processing:\n", parser->addr_explain));
  if( __tree_trace & 0x10000 ){
    if( (__tree_trace & 0x10001)==0x10000 ){
      __tree_view_line(0, "In __select() at %s:%d",
                           __FILE__, __LINE__);
    }
    __show_select(p);
  }
#endif

  /* tag-select-0100 */
  assert( p->order_by==0 || p_dest->dest!=SRT_DistFifo );
  assert( p->order_by==0 || p_dest->dest!=SRT_Fifo );
  assert( p->order_by==0 || p_dest->dest!=SRT_DistQueue );
  assert( p->order_by==0 || p_dest->dest!=SRT_Queue );
  if( IGNORABLE_DISTINCT(p_dest) ){
    assert(p_dest->dest==SRT_Exists     || p_dest->dest==SRT_Discard ||
           p_dest->dest==SRT_DistQueue  || p_dest->dest==SRT_DistFifo );
    /* All of these destinations are also able to ignore the ORDER BY clause */
    if( p->order_by ){
#if TREETRACE_ENABLED
      TREETRACE(0x800,parser,p, ("dropping superfluous ORDER BY:\n"));
      if( __tree_trace & 0x800 ){
        __tree_view_expr_list(0, p->order_by, 0, "ORDERBY");
      }
#endif   
      __parser_add_cleanup(parser, __expr_list_delete_generic,
                              p->order_by);
      testcase( parser->early_cleanup );
      p->order_by = 0;
    }
    p->sel_flags &= ~(u_int32_t)SF_Distinct;
  }
  __select_prep(parser, p, 0);
  if( parser->n_err ){
    goto select_end;
  }
  assert( db->malloc_failed==0 );
  assert( p->e_list!=0 );
#if TREETRACE_ENABLED
  if( __tree_trace & 0x10 ){
    TREETRACE(0x10,parser,p, ("after name resolution:\n"));
    __tree_view_select(0, p, 0);
  }
#endif

  /* If the SF_UFSrcCheck flag is set, then this function is being called
  ** as part of populating the temp table for an UPDATE...FROM statement.
  ** In this case, it is an error if the target object (src->a[0]) name
  ** or alias is duplicated within FROM clause (src->a[1..n]). 
  **
  ** Postgres disallows this case too. The reason is that some other
  ** systems handle this case differently, and not all the same way,
  ** which is just confusing. To avoid this, we follow PG's lead and
  ** disallow it altogether.  */
  if( p->sel_flags & SF_UFSrcCheck ){
    src_item_t *p0 = &p->src->a[0];
    if( __same_src_alias(p0, p->src) ){
      __error_msg(parser,
          "target object/alias may not appear in FROM clause: %s",
          p0->alias ? p0->alias : p0->s_tab->name
      );
      goto select_end;
    }

    /* Clear the SF_UFSrcCheck flag. The check has already been performed,
    ** and leaving this flag set can cause errors if a compound sub-query
    ** in p->src is flattened into this query and this function called
    ** again as part of compound SELECT processing.  */
    p->sel_flags &= ~(u_int32_t)SF_UFSrcCheck;
  }

  if( p_dest->dest==SRT_Output ){
    __generate_column_names(parser, p);
  }

#ifndef DBSQL_OMIT_WINDOWFUNC
  if( __window_rewrite(parser, p) ){
    assert( parser->n_err );
    goto select_end;
  }
#if TREETRACE_ENABLED
  if( p->win && (__tree_trace & 0x40)!=0 ){
    TREETRACE(0x40,parser,p, ("after window rewrite:\n"));
    __tree_view_select(0, p, 0);
  }
#endif
#endif /* DBSQL_OMIT_WINDOWFUNC */
  tab_list = p->src;
  is_agg = (p->sel_flags & SF_Aggregate)!=0;
  memset(&s_sort, 0, sizeof(s_sort));
  s_sort.order_by = p->order_by;

  /* Try to do various optimizations (flattening subqueries, and strength
  ** reduction of join operators) in the FROM clause up into the main query
  ** tag-select-0200
  */
#if !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW)
  for(i=0; !p->prior && i<tab_list->n_src; i++){
    src_item_t *item = &tab_list->a[i];
    select_t *sub = item->fg.is_subquery ? item->u4.subq->p_select : 0;
    table_t *tab = item->s_tab;

    /* The expander should have already created transient table_t objects
    ** even for FROM clause elements such as subqueries that do not correspond
    ** to a real table */
    assert( tab!=0 );

    /* Try to simplify joins:
    **
    **      LEFT JOIN  ->  JOIN
    **     RIGHT JOIN  ->  JOIN
    **      FULL JOIN  ->  RIGHT JOIN
    **
    ** If terms of the i-th table are used in the WHERE clause in such a
    ** way that the i-th table cannot be the NULL row of a join, then
    ** perform the appropriate simplification. This is called
    ** "OUTER JOIN strength reduction" in the dbsql documentation.
    ** tag-select-0220
    */
    if( (item->fg.jointype & (JT_LEFT|JT_LTORJ))!=0
     && __expr_implies_non_null_row(p->where, item->i_cursor,
                                     item->fg.jointype & JT_LTORJ)
     && OPTIMIZATION_ENABLED(db, DBSQL_SimplifyJoin)
    ){
      if( item->fg.jointype & JT_LEFT ){
        if( item->fg.jointype & JT_RIGHT ){
          TREETRACE(0x1000,parser,p,
                    ("FULL-JOIN simplifies to RIGHT-JOIN on term %d\n",i));
          item->fg.jointype &= ~JT_LEFT;
        }else{
          TREETRACE(0x1000,parser,p,
                    ("LEFT-JOIN simplifies to JOIN on term %d\n",i));
          item->fg.jointype &= ~(JT_LEFT|JT_OUTER);
          __unset_join_expr(p->where, item->i_cursor, 0);
        }
      }
      if( item->fg.jointype & JT_LTORJ ){
        for(j=i+1; j<tab_list->n_src; j++){
          src_item_t *i2 = &tab_list->a[j];
          if( i2->fg.jointype & JT_RIGHT ){
            if( i2->fg.jointype & JT_LEFT ){
              TREETRACE(0x1000,parser,p,
                        ("FULL-JOIN simplifies to LEFT-JOIN on term %d\n",j));
              i2->fg.jointype &= ~JT_RIGHT;
            }else{
              TREETRACE(0x1000,parser,p,
                        ("RIGHT-JOIN simplifies to JOIN on term %d\n",j));
              i2->fg.jointype &= ~(JT_RIGHT|JT_OUTER);
              __unset_join_expr(p->where, i2->i_cursor, 1);
            }
          }
        }
        for(j=tab_list->n_src-1; j>=0; j--){
          tab_list->a[j].fg.jointype &= ~JT_LTORJ;
          if( tab_list->a[j].fg.jointype & JT_RIGHT ) break;
        }
      }
    }

    /* No further action if this term of the FROM clause is not a subquery */
    if( sub==0 ) continue;

    /* Catch mismatch in the declared columns of a view and the number of
    ** columns in the SELECT on the RHS */
    if( tab->n_col!=sub->e_list->n_expr ){
      __error_msg(parser, "expected %d columns for '%s' but got %d",
                      tab->n_col, tab->name, sub->e_list->n_expr);
      goto select_end;
    }

    /* Do not attempt the usual optimizations (flattening and ORDER BY
    ** elimination) on a MATERIALIZED common table expression because
    ** a MATERIALIZED common table expression is an optimization fence.
    */
    if( item->fg.is_cte && item->u2.cte_use->m10d==M10D_YES ){
      continue;
    }

    /* Do not try to flatten an aggregate subquery.
    **
    ** Flattening an aggregate subquery is only possible if the outer query
    ** is not a join.  But if the outer query is not a join, then the subquery
    ** will be implemented as a co-routine and there is no advantage to
    ** flattening in that case.
    */
    if( (sub->sel_flags & SF_Aggregate)!=0 ) continue;
    assert( sub->group_by==0 );

    /* tag-select-0230:
    ** If a FROM-clause subquery has an ORDER BY clause that is not
    ** really doing anything, then delete it now so that it does not
    ** interfere with query flattening.  See the discussion at
    ** https://sqlite.org/forum/forumpost/2d76f2bcf65d256a
    **
    ** Beware of these cases where the ORDER BY clause may not be safely
    ** omitted:
    **
    **    (1)   There is also a LIMIT clause
    **    (2)   The subquery was added to help with window-function
    **          processing
    **    (3)   The subquery is in the FROM clause of an UPDATE
    **    (4)   The outer query uses an aggregate function other than
    **          the built-in count(), min(), or max().
    **    (5)   The ORDER BY isn't going to accomplish anything because
    **          one of:
    **            (a)  The outer query has a different ORDER BY clause
    **            (b)  The subquery is part of a join
    **          See forum post 062d576715d277c8
    **    (6)   The subquery is not a recursive CTE.  ORDER BY has a different
    **          meaning for recursive CTEs and this optimization does not
    **          apply.
    **
    ** Also retain the ORDER BY if the __omit_order_by optimization is disabled.
    */
    if( sub->order_by!=0
     && (p->order_by!=0 || tab_list->n_src>1)      /* Condition (5) */
     && sub->limit==0                           /* Condition (1) */
     && (sub->sel_flags & (SF_OrderByReqd|SF_Recursive))==0  /* (2) and (6) */
     && (p->sel_flags & SF_OrderByReqd)==0         /* Condition (3) and (4) */
     && OPTIMIZATION_ENABLED(db, DBSQL_OmitOrderBy)
    ){
      TREETRACE(0x800,parser,p,
                ("omit superfluous ORDER BY on %r FROM-clause subquery\n",i+1));
      __parser_add_cleanup(parser, __expr_list_delete_generic,
                              sub->order_by);
      sub->order_by = 0;
    }

    /* If the outer query contains a "complex" result set (that is,
    ** if the result set of the outer query uses functions or subqueries)
    ** and if the subquery contains an ORDER BY clause and if
    ** it will be implemented as a co-routine, then do not flatten.  This
    ** restriction allows SQL constructs like this:
    **
    **  SELECT expensive_function(x)
    **    FROM (SELECT x FROM tab ORDER BY y LIMIT 10);
    **
    ** The expensive_function() is only computed on the 10 rows that
    ** are output, rather than every row of the table.
    **
    ** The requirement that the outer query have a complex result set
    ** means that flattening does occur on simpler SQL constraints without
    ** the expensive_function() like:
    **
    **  SELECT x FROM (SELECT x FROM tab ORDER BY y LIMIT 10);
    */
    if( sub->order_by!=0
     && i==0
     && (p->sel_flags & SF_ComplexResult)!=0
     && (tab_list->n_src==1
         || (tab_list->a[1].fg.jointype&(JT_OUTER|JT_CROSS))!=0)
    ){
      continue;
    }

    /* tag-select-0240 */
    if( __flatten_subquery(parser, p, i, is_agg) ){
      if( parser->n_err ) goto select_end;
      /* This subquery can be absorbed into its parent. */
      i = -1;
    }
    tab_list = p->src;
    if( db->malloc_failed ) goto select_end;
    if( !IGNORABLE_ORDERBY(p_dest) ){
      s_sort.order_by = p->order_by;
    }
  }
#endif

#ifndef DBSQL_OMIT_COMPOUND_SELECT
  /* Handle compound SELECT statements using the separate __multi_select()
  ** procedure.  tag-select-0300
  */
  if( p->prior ){
    rc = __multi_select(parser, p, p_dest);
#if TREETRACE_ENABLED
    TREETRACE(0x400,parser,p,("end compound-select processing\n"));
    if( (__tree_trace & 0x400)!=0 && EXPLAIN_QUERY_PLAN_PARENT(parser)==0 ){
      __tree_view_select(0, p, 0);
    }
#endif
    if( p->next==0 ) EXPLAIN_QUERY_PLAN_POP(parser);
    return rc;
  }
#endif

  /* If there may be an "EXISTS (SELECT ...)" in the WHERE clause, attempt
  ** to change it into a join.  */
  if( parser->has_exists && OPTIMIZATION_ENABLED(db,DBSQL_ExistsToJoin) ){
    __exists_to_join(parser, p, p->where);
    tab_list = p->src;
  }

  /* Do the WHERE-clause constant propagation optimization if this is
  ** a join.  No need to spend time on this operation for non-join queries
  ** as the equivalent optimization will be handled by query planner in
  ** __where_begin().  tag-select-0330
  */
  if( p->where!=0
   && p->where->op==TK_AND
   && OPTIMIZATION_ENABLED(db, DBSQL_PropagateConst)
   && __propagate_constants(parser, p)
  ){
#if TREETRACE_ENABLED
    if( __tree_trace & 0x2000 ){
      TREETRACE(0x2000,parser,p,("After constant propagation:\n"));
      __tree_view_select(0, p, 0);
    }
#endif
  }else{
    TREETRACE(0x2000,parser,p,("Constant propagation not helpful\n"));
  }

  /* tag-select-0350 */
  if( OPTIMIZATION_ENABLED(db, DBSQL_QueryFlattener|DBSQL_CountOfView)
   && __count_of_view_optimization(parser, p)
  ){
    if( db->malloc_failed ) goto select_end;
    tab_list = p->src;
  }

  /* Loop over all terms in the FROM clause and do two things for each term:
  **
  **   (1) Authorize unreferenced tables
  **   (2) Generate code for all sub-queries
  **
  ** tag-select-0400
  */
  for(i=0; i<tab_list->n_src; i++){
    src_item_t *item = &tab_list->a[i];
    src_item_t *prior;
    select_dest_t dest;
    subquery_t *subq;
    select_t *sub;
#if !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW)
    const char *saved_auth_context;
#endif

    /* Authorized unreferenced tables.  tag-select-0410
    **
    ** Issue DBSQL_READ authorizations with a fake column name for any
    ** tables that are referenced but from which no values are extracted.
    ** Examples of where these kinds of null DBSQL_READ authorizations
    ** would occur:
    **
    **     SELECT count(*) FROM t1;   -- DBSQL_READ t1.""
    **     SELECT t1.* FROM t1, t2;   -- DBSQL_READ t2.""
    **
    ** The fake column name is an empty string.  It is possible for a table to
    ** have a column named by the empty string, in which case there is no way to
    ** distinguish between an unreferenced table and an actual reference to the
    ** "" column. The original design was for the fake column name to be a NULL,
    ** which would be unambiguous.  But legacy authorization callbacks might
    ** assume the column name is non-NULL and segfault.  The use of an empty
    ** string for the fake column name seems safer.
    */
    if( item->col_used==0 && item->name!=0 ){
      const char *z_db;
      if( item->fg.fixed_schema ){
        int i_db = __schema_to_index(parser->db, item->u4.schema);
        z_db = db->dbs[i_db].db_s_name;
      }else if( item->fg.is_subquery ){
        z_db = 0;
      }else{
        z_db = item->u4.database;
      }
      __auth_check(parser, DBSQL_READ, item->name, "", z_db);
    }

#if !defined(DBSQL_OMIT_SUBQUERY) || !defined(DBSQL_OMIT_VIEW)
    /* Generate code for all sub-queries in the FROM clause
    */
    if( item->fg.is_subquery==0 ) continue;
    subq = item->u4.subq;
    assert( subq!=0 );
    sub = subq->p_select;

    /* The code for a subquery should only be generated once. */
    if( subq->addr_fill_sub!=0 ) continue;

    /* Increment parser_t.n_height by the height of the largest expression
    ** tree referred to by this, the parent select. The child select
    ** may contain expression trees of at most
    ** (DBSQL_MAX_EXPR_DEPTH-parser_t.n_height) height. This is a bit
    ** more conservative than necessary, but much easier than enforcing
    ** an exact limit.
    */
    parser->n_height += __select_expr_height(p);

    /* Make copies of constant WHERE-clause terms in the outer query down
    ** inside the subquery.  This can help the subquery to run more efficiently.
    ** This is the "predicate push-down optimization".  tag-select-0420
    */
    if( OPTIMIZATION_ENABLED(db, DBSQL_PushDown)
     && (item->fg.is_cte==0
         || (item->u2.cte_use->m10d!=M10D_YES && item->u2.cte_use->n_use<2))
     && __push_down_where_terms(parser, sub, p->where, tab_list, i)
    ){
#if TREETRACE_ENABLED
      if( __tree_trace & 0x4000 ){
        TREETRACE(0x4000,parser,p,
            ("After WHERE-clause push-down into subquery %d:\n", sub->sel_id));
        __tree_view_select(0, p, 0);
      }
#endif
      assert( subq->p_select && (sub->sel_flags & SF_PushDown)!=0 );
    }else{
      TREETRACE(0x4000,parser,p,("WHERE-clause push-down not possible\n"));
    }

    /* Convert unused result columns of the subquery into simple NULL
    ** expressions, to avoid unneeded searching and computation.
    ** tag-select-0440
    */
    if( OPTIMIZATION_ENABLED(db, DBSQL_NullUnusedCols)
     && __disable_unused_subquery_result_columns(item)
    ){
#if TREETRACE_ENABLED
      if( __tree_trace & 0x4000 ){
        TREETRACE(0x4000,parser,p,
            ("Change unused result columns to NULL for subquery %d:\n",
             sub->sel_id));
        __tree_view_select(0, p, 0);
      }
#endif
    }

    saved_auth_context = parser->auth_context;
    parser->auth_context = item->name;

    /* Generate byte-code to implement the subquery  tag-select-0480
    */
    if( __from_clause_term_can_be_coroutine(parser, tab_list, i, p->sel_flags) ){
      /* Implement a co-routine that will return a single row of the result
      ** set on each invocation.  tag-select-0482
      */
      int addr_top = __vdbe_current_addr(v)+1;
    
      subq->reg_return = ++parser->n_mem;
      __vdbe_add_op3(v, OP_InitCoroutine, subq->reg_return, 0, addr_top);
      VDBE_COMMENT((v, "%!S", item));
      subq->addr_fill_sub = addr_top;
      __select_dest_init(&dest, SRT_Coroutine, subq->reg_return);
      EXPLAIN_QUERY_PLAN((parser, 1, "CO-ROUTINE %!S", item));
      __select(parser, sub, &dest);
      item->s_tab->n_row_log_est = sub->n_select_row;
      item->fg.via_coroutine = 1;
      subq->reg_result = dest.i_sdst;
      __vdbe_end_coroutine(v, subq->reg_return);
      VDBE_COMMENT((v, "end %!S", item));
      __vdbe_jump_here(v, addr_top-1);
      __clear_temp_reg_cache(parser);
    }else if( item->fg.is_cte && item->u2.cte_use->addr_m9e>0 ){
      /* This is a CTE for which materialization code has already been
      ** generated.  Invoke the subroutine to compute the materialization,
      ** then make the item->i_cursor be a copy of the ephemeral table that
      ** holds the result of the materialization. tag-select-0484 */
      cte_use_t *cte_use = item->u2.cte_use;
      __vdbe_add_op2(v, OP_Gosub, cte_use->reg_rtn, cte_use->addr_m9e);
      if( item->i_cursor!=cte_use->i_cur ){
        __vdbe_add_op2(v, OP_OpenDup, item->i_cursor, cte_use->i_cur);
        VDBE_COMMENT((v, "%!S", item));
      }
      sub->n_select_row = cte_use->n_row_est;
    }else if( (prior = __is_self_join_view(tab_list, item, 0, i))!=0 ){
      /* This view has already been materialized by a prior entry in
      ** this same FROM clause.  Reuse it.  tag-select-0486 */
      subquery_t *prior_subq;
      assert( prior->fg.is_subquery );
      prior_subq = prior->u4.subq;
      assert( prior_subq!=0 );
      if( prior_subq->addr_fill_sub ){
        __vdbe_add_op2(v, OP_Gosub, prior_subq->reg_return,
                                       prior_subq->addr_fill_sub);
      }
      __vdbe_add_op2(v, OP_OpenDup, item->i_cursor, prior->i_cursor);
      sub->n_select_row = prior_subq->p_select->n_select_row;
    }else{
      /* Materialize the view.  If the view is not correlated, generate a
      ** subroutine to do the materialization so that subsequent uses of
      ** the same view can reuse the materialization.  tag-select-0488 */
      int top_addr;
      int once_addr = 0;
#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
      int addr_explain;
#endif

      subq->reg_return = ++parser->n_mem;
      top_addr = __vdbe_add_op0(v, OP_Goto);
      subq->addr_fill_sub = top_addr+1;
      item->fg.is_materialized = 1;
      if( item->fg.is_correlated==0 ){
        /* If the subquery is not correlated and if we are not inside of
        ** a trigger, then we only need to compute the value of the subquery
        ** once. */
        once_addr = __vdbe_add_op0(v, OP_Once); VDBE_COVERAGE(v);
        VDBE_COMMENT((v, "materialize %!S", item));
      }else{
        VDBE_NOOP_COMMENT((v, "materialize %!S", item));
      }
      __select_dest_init(&dest, SRT_EphemTab, item->i_cursor);

      EXPLAIN_QUERY_PLAN2(addr_explain, (parser, 1, "MATERIALIZE %!S", item));
      __select(parser, sub, &dest);
      item->s_tab->n_row_log_est = sub->n_select_row;
      if( once_addr ) __vdbe_jump_here(v, once_addr);
      __vdbe_add_op2(v, OP_Return, subq->reg_return, top_addr+1);
      VDBE_COMMENT((v, "end %!S", item));
      __vdbe_scan_status_range(v, addr_explain, addr_explain, -1);
      __vdbe_jump_here(v, top_addr);
      __clear_temp_reg_cache(parser);
      if( item->fg.is_cte && item->fg.is_correlated==0 ){
        cte_use_t *cte_use = item->u2.cte_use;
        cte_use->addr_m9e = subq->addr_fill_sub;
        cte_use->reg_rtn = subq->reg_return;
        cte_use->i_cur = item->i_cursor;
        cte_use->n_row_est = sub->n_select_row;
      }
    }
    if( db->malloc_failed ) goto select_end;
    parser->n_height -= __select_expr_height(p);
    parser->auth_context = saved_auth_context;
#endif
  }

  /* Various elements of the SELECT copied into local variables for
  ** convenience */
  e_list = p->e_list;
  where = p->where;
  group_by = p->group_by;
  having = p->having;
  s_distinct.is_tnct = (p->sel_flags & SF_Distinct)!=0;

#if TREETRACE_ENABLED
  if( __tree_trace & 0x8000 ){
    TREETRACE(0x8000,parser,p,("After all FROM-clause analysis:\n"));
    __tree_view_select(0, p, 0);
  }
#endif

  /* tag-select-0500
  **
  ** If the query is DISTINCT with an ORDER BY but is not an aggregate, and
  ** if the select-list is the same as the ORDER BY list, then this query
  ** can be rewritten as a GROUP BY. In other words, this:
  **
  **     SELECT DISTINCT xyz FROM ... ORDER BY xyz
  **
  ** is transformed to:
  **
  **     SELECT xyz FROM ... GROUP BY xyz ORDER BY xyz
  **
  ** The second form is preferred as a single index (or temp-table) may be
  ** used for both the ORDER BY and DISTINCT processing. As originally
  ** written the query must use a temp-table for at least one of the ORDER
  ** BY and DISTINCT, and an index or separate temp-table for the other.
  */
  if( (p->sel_flags & (SF_Distinct|SF_Aggregate))==SF_Distinct
   && __copy_sort_order(e_list, s_sort.order_by)
   && __expr_list_compare(e_list, s_sort.order_by, -1)==0
   && OPTIMIZATION_ENABLED(db, DBSQL_GroupByOrder)
#ifndef DBSQL_OMIT_WINDOWFUNC
   && p->win==0
#endif
  ){
    p->sel_flags &= ~(u_int32_t)SF_Distinct;
    group_by = p->group_by = __expr_list_dup(db, e_list, 0);
    if( group_by ){
      for(i=0; i<group_by->n_expr; i++){
        group_by->a[i].u.x.i_order_by_col = i+1;
      }
    }
    p->sel_flags |= SF_Aggregate;
    /* Notice that even thought SF_Distinct has been cleared from p->sel_flags,
    ** the s_distinct.is_tnct is still set.  Hence, is_tnct represents the
    ** original setting of the SF_Distinct flag, not the current setting */
    assert( s_distinct.is_tnct );
    s_distinct.is_tnct = 2;

#if TREETRACE_ENABLED
    if( __tree_trace & 0x20000 ){
      TREETRACE(0x20000,parser,p,("Transform DISTINCT into GROUP BY:\n"));
      __tree_view_select(0, p, 0);
    }
#endif
  }

  /* If there is an ORDER BY clause, then create an ephemeral index to
  ** do the sorting.  But this sorting ephemeral index might end up
  ** being unused if the data can be extracted in pre-sorted order.
  ** If that is the case, then the OP_OpenEphemeral instruction will be
  ** changed to an OP_Noop once we figure out that the sorting index is
  ** not needed.  The s_sort.addr_sort_index variable is used to facilitate
  ** that change.  tag-select-0600
  */
  if( s_sort.order_by ){
    key_info_t *key_info;
    key_info = __key_info_from_expr_list(
        parser, s_sort.order_by, 0, e_list->n_expr);
    s_sort.i_e_cursor = parser->n_tab++;
    s_sort.addr_sort_index =
      __vdbe_add_op4(v, OP_OpenEphemeral,
          s_sort.i_e_cursor, s_sort.order_by->n_expr+1+e_list->n_expr, 0,
          (char*)key_info, P4_KEYINFO
      );
  }else{
    s_sort.addr_sort_index = -1;
  }

  /* If the output is destined for a temporary table, open that table.
  ** tag-select-0630
  */
  if( p_dest->dest==SRT_EphemTab ){
    __vdbe_add_op2(v, OP_OpenEphemeral, p_dest->i_sd_parm, e_list->n_expr);
    if( p->sel_flags & SF_NestedFrom ){
      /* Delete or NULL-out result columns that will never be used */
      int ii;
      for(ii=e_list->n_expr-1; ii>0 && e_list->a[ii].fg.used==0; ii--){
        __expr_delete(db, e_list->a[ii].expr);
        __dbsql_db_free(db, e_list->a[ii].e_name);
        e_list->n_expr--;
      }
      for(ii=0; ii<e_list->n_expr; ii++){
        if( e_list->a[ii].fg.used==0 ) e_list->a[ii].expr->op = TK_NULL;
      }
    }
  }

  /* Set the limiter.  tag-select-0650
  */
  i_end = __vdbe_make_label(parser);
  if( (p->sel_flags & SF_FixedLimit)==0 ){
    p->n_select_row = 320;  /* 4 billion rows */
  }
  if( p->limit ) __compute_limit_registers(parser, p, i_end);
  if( p->i_limit==0 && s_sort.addr_sort_index>=0 ){
    __vdbe_change_opcode(v, s_sort.addr_sort_index, OP_SorterOpen);
    s_sort.sort_flags |= SORTFLAG_UseSorter;
  }

  /* Open an ephemeral index to use for the distinct set. tag-select-0680
  */
  if( p->sel_flags & SF_Distinct ){
    s_distinct.tab_tnct = parser->n_tab++;
    s_distinct.addr_tnct = __vdbe_add_op4(v, OP_OpenEphemeral,
                       s_distinct.tab_tnct, 0, 0,
                       (char*)__key_info_from_expr_list(parser, p->e_list,0,0),
                       P4_KEYINFO);
    __vdbe_change_p5(v, BTREE_UNORDERED);
    s_distinct.tnct_type = WHERE_DISTINCT_UNORDERED;
  }else{
    s_distinct.tnct_type = WHERE_DISTINCT_NOOP;
  }

  if( !is_agg && group_by==0 ){
    /* No aggregate functions and no GROUP BY clause.  tag-select-0700 */
    u_int16_t wctrl_flags = (s_distinct.is_tnct ? WHERE_WANT_DISTINCT : 0)
                   | (p->sel_flags & SF_FixedLimit);
#ifndef DBSQL_OMIT_WINDOWFUNC
    window_t *win = p->win;      /* Main window object (or NULL) */
    if( win ){
      __window_code_init(parser, p);
    }
#endif
    assert( WHERE_USE_LIMIT==SF_FixedLimit );


    /* Begin the database scan. */
    TREETRACE(0x2,parser,p,("WhereBegin\n"));
    w_info = __where_begin(parser, tab_list, where, s_sort.order_by,
                               p->e_list, p, wctrl_flags, p->n_select_row);
    if( w_info==0 ) goto select_end;
    if( __where_output_row_count(w_info) < p->n_select_row ){
      p->n_select_row = __where_output_row_count(w_info);
      if( p_dest->dest<=SRT_DistQueue && p_dest->dest>=SRT_DistFifo ){
        /* TUNING: For a UNION CTE, because UNION is implies DISTINCT,
        ** reduce the estimated output row count by 8 (log_est_t 30). 
        ** Search for tag-20250414a to see other cases */
        p->n_select_row -= 30;
      }
    }
    if( s_distinct.is_tnct && __where_is_distinct(w_info) ){
      s_distinct.tnct_type = __where_is_distinct(w_info);
    }
    if( s_sort.order_by ){
      s_sort.n_ob_sat = __where_is_ordered(w_info);
      s_sort.label_ob_lopt = __where_order_by_limit_opt_label(w_info);
      if( s_sort.n_ob_sat==s_sort.order_by->n_expr ){
        s_sort.order_by = 0;
      }
    }
    TREETRACE(0x2,parser,p,("WhereBegin returns\n"));

    /* If sorting index that was created by a prior OP_OpenEphemeral
    ** instruction ended up not being needed, then change the OP_OpenEphemeral
    ** into an OP_Noop.
    */
    if( s_sort.addr_sort_index>=0 && s_sort.order_by==0 ){
      __vdbe_change_to_noop(v, s_sort.addr_sort_index);
    }

    assert( p->e_list==e_list );
#ifndef DBSQL_OMIT_WINDOWFUNC
    if( win ){
      int addr_gosub = __vdbe_make_label(parser);
      int i_cont = __vdbe_make_label(parser);
      int i_break = __vdbe_make_label(parser);
      int reg_gosub = ++parser->n_mem;

      __window_code_step(parser, p, w_info, reg_gosub, addr_gosub);

      __vdbe_add_op2(v, OP_Goto, 0, i_break);
      __vdbe_resolve_label(v, addr_gosub);
      VDBE_NOOP_COMMENT((v, "inner-loop subroutine"));
      s_sort.label_ob_lopt = 0;
      __select_inner_loop(parser, p, -1, &s_sort, &s_distinct, p_dest, i_cont, i_break);
      __vdbe_resolve_label(v, i_cont);
      __vdbe_add_op1(v, OP_Return, reg_gosub);
      VDBE_COMMENT((v, "end inner-loop subroutine"));
      __vdbe_resolve_label(v, i_break);
    }else
#endif /* DBSQL_OMIT_WINDOWFUNC */
    {
      /* Use the standard inner loop. */
      __select_inner_loop(parser, p, -1, &s_sort, &s_distinct, p_dest,
          __where_continue_label(w_info),
          __where_break_label(w_info));

      /* End the database scan loop.
      */
      TREETRACE(0x2,parser,p,("WhereEnd\n"));
      __where_end(w_info);
    }
  }else{
    /* This case is for when there exist aggregate functions or a GROUP BY
    ** clause or both.  tag-select-0800 */
    name_context_t s_nc;    /* Name context for processing aggregate information */
    int i_a_mem;          /* First mem_t address for storing current GROUP BY */
    int i_b_mem;          /* First mem_t address for previous GROUP BY */
    int i_use_flag;       /* mem_t address holding flag indicating that at least
                        ** one row of the input to the aggregator has been
                        ** processed */
    int i_abort_flag;     /* mem_t address which causes query abort if positive */
    int group_by_sort;    /* Rows come from source in GROUP BY order */
    int addr_end;        /* End of processing for this SELECT */
    int sort_p_tab = 0;   /* Pseudotable used to decode sorting results */
    int sort_out = 0;    /* Output register from the sorter */
    int order_by_grp = 0; /* True if the GROUP BY and ORDER BY are the same */

    /* Remove any and all aliases between the result set and the
    ** GROUP BY clause.
    */
    if( group_by ){
      int k;                        /* Loop counter */
      struct expr_list_item_t *item;  /* For looping over expression in a list */

      for(k=p->e_list->n_expr, item=p->e_list->a; k>0; k--, item++){
        item->u.x.i_alias = 0;
      }
      for(k=group_by->n_expr, item=group_by->a; k>0; k--, item++){
        item->u.x.i_alias = 0;
      }
      assert( 66==__log_est(100) );
      if( p->n_select_row>66 ) p->n_select_row = 66;

      /* If there is both a GROUP BY and an ORDER BY clause and they are
      ** identical, then it may be possible to disable the ORDER BY clause
      ** on the grounds that the GROUP BY will cause elements to come out
      ** in the correct order. It also may not - the GROUP BY might use a
      ** database index that causes rows to be grouped together as required
      ** but not actually sorted. Either way, record the fact that the
      ** ORDER BY and GROUP BY clauses are the same by setting the order_by_grp
      ** variable.  */
      if( __copy_sort_order(group_by, s_sort.order_by)
       && __expr_list_compare(group_by, s_sort.order_by, -1)==0
      ){
        order_by_grp = 1;
      }
    }else{
      assert( 0==__log_est(1) );
      p->n_select_row = 0;
    }

    /* Create a label to jump to when we want to abort the query */
    addr_end = __vdbe_make_label(parser);

    /* Convert TK_COLUMN nodes into TK_AGG_COLUMN and make entries in
    ** s_agg_info for all TK_AGG_FUNCTION nodes in expressions of the
    ** SELECT statement.
    */
    agg_info = __db_malloc_zero(db, sizeof(*agg_info) );
    if( agg_info ){
      __parser_add_cleanup(parser, __agginfo_free, agg_info);
      testcase( parser->early_cleanup );
    }
    if( db->malloc_failed ){
      goto select_end;
    }
    agg_info->sel_id = p->sel_id;
#ifdef DBSQL_DEBUG
    agg_info->p_select = p;
#endif
    memset(&s_nc, 0, sizeof(s_nc));
    s_nc.parser = parser;
    s_nc.src_list = tab_list;
    s_nc.u_nc.agg_info = agg_info;
    VVA_ONLY( s_nc.nc_flags = NC_UAggInfo; )
    agg_info->n_sorting_column = group_by ? group_by->n_expr : 0;
    agg_info->group_by = group_by;
    __expr_analyze_agg_list(&s_nc, e_list);
    __expr_analyze_agg_list(&s_nc, s_sort.order_by);
    if( having ){
      if( group_by ){
        assert( where==p->where );
        assert( having==p->having );
        assert( group_by==p->group_by );
        __having_to_where(parser, p);
        where = p->where;
      }
      __expr_analyze_aggregates(&s_nc, having);
    }
    agg_info->n_accumulator = agg_info->n_column;
    if( p->group_by==0 && p->having==0 && agg_info->n_func==1 ){
      min_max_flag = __min_max_query(db, agg_info->funcs[0].f_expr, &min_max_order_by);
    }else{
      min_max_flag = WHERE_ORDERBY_NORMAL;
    }
    __analyze_agg_func_args(agg_info, &s_nc);
    if( db->malloc_failed ) goto select_end;
#if TREETRACE_ENABLED
    if( __tree_trace & 0x20 ){
      TREETRACE(0x20,parser,p,("After aggregate analysis %p:\n", agg_info));
      __tree_view_select(0, p, 0);
      if( min_max_flag ){
        __debug_printf("MIN/MAX Optimization (0x%02x) adds:\n", min_max_flag);
        __tree_view_expr_list(0, min_max_order_by, 0, "ORDERBY");
      }
      __print_agg_info(agg_info);
    }
#endif


    /* Processing for aggregates with GROUP BY is very different and
    ** much more complex than aggregates without a GROUP BY.  tag-select-0810
    */
    if( group_by ){
      key_info_t *key_info;  /* Keying information for the group by clause */
      int addr1;          /* A-vs-B comparison jump */
      int addr_output_row;  /* Start of subroutine that outputs a result row */
      int reg_output_row;   /* Return address register for output subroutine */
      int addr_set_abort;   /* Set the abort flag and return */
      int addr_top_of_loop;  /* Top of the input loop */
      int addr_sorting_idx; /* The OP_OpenEphemeral for the sorting index */
      int addr_reset;      /* Subroutine for resetting the accumulator */
      int reg_reset;       /* Return address register for reset subroutine */
      expr_list_t *distinct = 0;
      u_int16_t dist_flag = 0;
      int dist = WHERE_DISTINCT_NOOP;

      if( agg_info->n_func==1
       && agg_info->funcs[0].i_distinct>=0
       && ALWAYS(agg_info->funcs[0].f_expr!=0)
       && ALWAYS(EXPR_USE_X_LIST(agg_info->funcs[0].f_expr))
       && agg_info->funcs[0].f_expr->x.list!=0
      ){
        expr_t *expr = agg_info->funcs[0].f_expr->x.list->a[0].expr;
        expr = __expr_dup(db, expr, 0);
        distinct = __expr_list_dup(db, group_by, 0);
        distinct = __expr_list_append(parser, distinct, expr);
        dist_flag = distinct ? (WHERE_WANT_DISTINCT|WHERE_AGG_DISTINCT) : 0;
      }

      /* If there is a GROUP BY clause we might need a sorting index to
      ** implement it.  Allocate that sorting index now.  If it turns out
      ** that we do not need it after all, the OP_SorterOpen instruction
      ** will be converted into a Noop. 
      */
      agg_info->sorting_idx = parser->n_tab++;
      key_info = __key_info_from_expr_list(parser, group_by,
                                            0, agg_info->n_column);
      addr_sorting_idx = __vdbe_add_op4(v, OP_SorterOpen,
          agg_info->sorting_idx, agg_info->n_sorting_column,
          0, (char*)key_info, P4_KEYINFO);

      /* Initialize memory locations used by GROUP BY aggregate processing
      */
      i_use_flag = ++parser->n_mem;
      i_abort_flag = ++parser->n_mem;
      reg_output_row = ++parser->n_mem;
      addr_output_row = __vdbe_make_label(parser);
      reg_reset = ++parser->n_mem;
      addr_reset = __vdbe_make_label(parser);
      i_a_mem = parser->n_mem + 1;
      parser->n_mem += group_by->n_expr;
      i_b_mem = parser->n_mem + 1;
      parser->n_mem += group_by->n_expr;
      __vdbe_add_op2(v, OP_Integer, 0, i_abort_flag);
      VDBE_COMMENT((v, "clear abort flag"));
      __vdbe_add_op3(v, OP_Null, 0, i_a_mem, i_a_mem+group_by->n_expr-1);
      __expr_null_register_range(parser, i_a_mem, group_by->n_expr);

      /* Begin a loop that will extract all source rows in GROUP BY order.
      ** This might involve two separate loops with an OP_Sort in between, or
      ** it might be a single loop that uses an index to extract information
      ** in the right order to begin with.
      */
      __vdbe_add_op2(v, OP_Gosub, reg_reset, addr_reset);
      TREETRACE(0x2,parser,p,("WhereBegin\n"));
      w_info = __where_begin(parser, tab_list, where, group_by, distinct,
          p, (s_distinct.is_tnct==2 ? WHERE_DISTINCTBY : WHERE_GROUPBY)
          |  (order_by_grp ? WHERE_SORTBYGROUP : 0) | dist_flag, 0
      );
      if( w_info==0 ){
        __expr_list_delete(db, distinct);
        goto select_end;
      }
      if( parser->idx_epr ){
        __optimize_aggregate_use_of_indexed_expr(parser, p, agg_info, &s_nc);
      }
      __assign_aggregate_registers(parser, agg_info);
      dist = __where_is_distinct(w_info);
      TREETRACE(0x2,parser,p,("WhereBegin returns\n"));
      if( __where_is_ordered(w_info)==group_by->n_expr ){
        /* The optimizer is able to deliver rows in group by order so
        ** we do not have to sort.  The OP_OpenEphemeral table will be
        ** cancelled later because we still need to use the key_info
        */
        group_by_sort = 0;
      }else{
        /* Rows are coming out in undetermined order.  We have to push
        ** each row into a sorting index, terminate the first loop,
        ** then loop over the sorting index in order to get the output
        ** in sorted order
        */
        int reg_base;
        int reg_record;
        int n_col;
        int n_group_by;

#ifdef DBSQL_ENABLE_STMT_SCANSTATUS
        int addr_exp;              /* Address of OP_Explain instruction */
#endif
        EXPLAIN_QUERY_PLAN2(addr_exp, (parser, 0, "USE TEMP B-TREE FOR %s",
            (s_distinct.is_tnct && (p->sel_flags&SF_Distinct)==0) ?
                    "DISTINCT" : "GROUP BY"
        ));

        group_by_sort = 1;
        n_group_by = group_by->n_expr;
        n_col = n_group_by;
        j = n_group_by;
        for(i=0; i<agg_info->n_column; i++){
          if( agg_info->cols[i].i_sorter_column>=j ){
            n_col++;
            j++;
          }
        }
        reg_base = __get_temp_range(parser, n_col);
        __expr_code_expr_list(parser, group_by, reg_base, 0, 0);
        j = n_group_by;
        agg_info->direct_mode = 1;
        for(i=0; i<agg_info->n_column; i++){
          struct agg_info_col_t *col = &agg_info->cols[i];
          if( col->i_sorter_column>=j ){
            __expr_code(parser, col->c_expr, j + reg_base);
            j++;
          }
        }
        agg_info->direct_mode = 0;
        reg_record = __get_temp_reg(parser);
        __vdbe_scan_status_counters(v, addr_exp, 0, __vdbe_current_addr(v));
        __vdbe_add_op3(v, OP_MakeRecord, reg_base, n_col, reg_record);
        __vdbe_add_op2(v, OP_SorterInsert, agg_info->sorting_idx, reg_record);
        __vdbe_scan_status_range(v, addr_exp, __vdbe_current_addr(v)-2, -1);
        __release_temp_reg(parser, reg_record);
        __release_temp_range(parser, reg_base, n_col);
        TREETRACE(0x2,parser,p,("WhereEnd\n"));
        __where_end(w_info);
        agg_info->sorting_idx_p_tab = sort_p_tab = parser->n_tab++;
        sort_out = __get_temp_reg(parser);
        __vdbe_scan_status_counters(v, addr_exp, __vdbe_current_addr(v), 0);
        __vdbe_add_op3(v, OP_OpenPseudo, sort_p_tab, sort_out, n_col);
        __vdbe_add_op2(v, OP_SorterSort, agg_info->sorting_idx, addr_end);
        VDBE_COMMENT((v, "GROUP BY sort")); VDBE_COVERAGE(v);
        agg_info->use_sorting_idx = 1;
        __vdbe_scan_status_range(v, addr_exp, -1, sort_p_tab);
        __vdbe_scan_status_range(v, addr_exp, -1, agg_info->sorting_idx);
      }

      /* If there are entries in aggg_info->funcs[] that contain subexpressions
      ** that are indexed (and that were previously identified and tagged
      ** in __optimize_aggregate_use_of_indexed_expr()) then those subexpressions
      ** must now be converted into a TK_AGG_COLUMN node so that the value
      ** is correctly pulled from the index rather than being recomputed. */
      if( parser->idx_epr ){
        __aggregate_convert_indexed_expr_ref_to_column(agg_info);
#if TREETRACE_ENABLED
        if( __tree_trace & 0x20 ){
          TREETRACE(0x20, parser, p,
             ("AggInfo function expressions converted to reference index\n"));
          __tree_view_select(0, p, 0);
          __print_agg_info(agg_info);
        }
#endif
      }

      /* If the index or temporary table used by the GROUP BY sort
      ** will naturally deliver rows in the order required by the ORDER BY
      ** clause, cancel the ephemeral table open coded earlier.
      **
      ** This is an optimization - the correct answer should result regardless.
      ** Use the DBSQL_GroupByOrder flag with DBSQL_TESTCTRL_OPTIMIZER to
      ** disable this optimization for testing purposes.  */
      if( order_by_grp && OPTIMIZATION_ENABLED(db, DBSQL_GroupByOrder)
       && (group_by_sort || __where_is_sorted(w_info))
      ){
        s_sort.order_by = 0;
        __vdbe_change_to_noop(v, s_sort.addr_sort_index);
      }

      /* Evaluate the current GROUP BY terms and store in b0, b1, b2...
      ** (b0 is memory location i_b_mem+0, b1 is i_b_mem+1, and so forth)
      ** Then compare the current GROUP BY terms against the GROUP BY terms
      ** from the previous row currently stored in a0, a1, a2...
      */
      addr_top_of_loop = __vdbe_current_addr(v);
      if( group_by_sort ){
        __vdbe_add_op3(v, OP_SorterData, agg_info->sorting_idx,
                          sort_out, sort_p_tab);
      }
      for(j=0; j<group_by->n_expr; j++){
        int i_order_by_col = group_by->a[j].u.x.i_order_by_col;
        
        if( group_by_sort ){
          __vdbe_add_op3(v, OP_Column, sort_p_tab, j, i_b_mem+j);
        }else{
          agg_info->direct_mode = 1;
          __expr_code(parser, group_by->a[j].expr, i_b_mem+j);
        }

        if( i_order_by_col ){
          expr_t *x = p->e_list->a[i_order_by_col-1].expr;
          expr_t *base = __expr_skip_collate_and_likely(x);
          while( ALWAYS(base!=0) && base->op==TK_IF_NULL_ROW ){
            x = base->left;
            base = __expr_skip_collate_and_likely(x);
          }
          if( ALWAYS(base!=0)
           && base->op!=TK_AGG_COLUMN
           && base->op!=TK_REGISTER
          ){
            __expr_to_register(x, i_a_mem+j);
          }
        }
      }
      __vdbe_add_op4(v, OP_Compare, i_a_mem, i_b_mem, group_by->n_expr,
                          (char*)__key_info_ref(key_info), P4_KEYINFO);
      addr1 = __vdbe_current_addr(v);
      __vdbe_add_op3(v, OP_Jump, addr1+1, 0, addr1+1); VDBE_COVERAGE(v);

      /* Generate code that runs whenever the GROUP BY changes.
      ** Changes in the GROUP BY are detected by the previous code
      ** block.  If there were no changes, this block is skipped.
      **
      ** This code copies current group by terms in b0,b1,b2,...
      ** over to a0,a1,a2.  It then calls the output subroutine
      ** and resets the aggregate accumulator registers in preparation
      ** for the next GROUP BY batch.
      */
      __vdbe_add_op2(v, OP_Gosub, reg_output_row, addr_output_row);
      VDBE_COMMENT((v, "output one row of %d", p->sel_id));
      __expr_code_move(parser, i_b_mem, i_a_mem, group_by->n_expr);
      __vdbe_add_op2(v, OP_IfPos, i_abort_flag, addr_end); VDBE_COVERAGE(v);
      VDBE_COMMENT((v, "check abort flag"));
      __vdbe_add_op2(v, OP_Gosub, reg_reset, addr_reset);
      VDBE_COMMENT((v, "reset accumulator %d", p->sel_id));

      /* Update the aggregate accumulators based on the content of
      ** the current row
      */
      __vdbe_jump_here(v, addr1);
      __update_accumulator(parser, i_use_flag, agg_info, dist);
      __vdbe_add_op2(v, OP_Integer, 1, i_use_flag);
      VDBE_COMMENT((v, "indicate data in accumulator %d", p->sel_id));

      /* End of the loop
      */
      if( group_by_sort ){
        __vdbe_add_op2(v, OP_SorterNext, agg_info->sorting_idx,addr_top_of_loop);
        VDBE_COVERAGE(v);
      }else{
        TREETRACE(0x2,parser,p,("WhereEnd\n"));
        __where_end(w_info);
        __vdbe_change_to_noop(v, addr_sorting_idx);
      }
      __expr_list_delete(db, distinct);

      /* Output the final row of result
      */
      __vdbe_add_op2(v, OP_Gosub, reg_output_row, addr_output_row);
      VDBE_COMMENT((v, "output final row of %d", p->sel_id));

      /* Jump over the subroutines
      */
      __vdbe_goto(v, addr_end);

      /* Generate a subroutine that outputs a single row of the result
      ** set.  This subroutine first looks at the i_use_flag.  If i_use_flag
      ** is less than or equal to zero, the subroutine is a no-op.  If
      ** the processing calls for the query to abort, this subroutine
      ** increments the i_abort_flag memory location before returning in
      ** order to signal the caller to abort.
      */
      addr_set_abort = __vdbe_current_addr(v);
      __vdbe_add_op2(v, OP_Integer, 1, i_abort_flag);
      VDBE_COMMENT((v, "set abort flag"));
      __vdbe_add_op1(v, OP_Return, reg_output_row);
      __vdbe_resolve_label(v, addr_output_row);
      addr_output_row = __vdbe_current_addr(v);
      __vdbe_add_op2(v, OP_IfPos, i_use_flag, addr_output_row+2);
      VDBE_COVERAGE(v);
      VDBE_COMMENT((v, "Groupby result generator entry point %d", p->sel_id));
      __vdbe_add_op1(v, OP_Return, reg_output_row);
      __finalize_agg_functions(parser, agg_info);
      __expr_if_false(parser, having, addr_output_row+1, DBSQL_JUMPIFNULL);
      __select_inner_loop(parser, p, -1, &s_sort,
                      &s_distinct, p_dest,
                      addr_output_row+1, addr_set_abort);
      __vdbe_add_op1(v, OP_Return, reg_output_row);
      VDBE_COMMENT((v, "end groupby result generator %d", p->sel_id));

      /* Generate a subroutine that will reset the group-by accumulator
      */
      __vdbe_resolve_label(v, addr_reset);
      __reset_accumulator(parser, agg_info);
      __vdbe_add_op2(v, OP_Integer, 0, i_use_flag);
      VDBE_COMMENT((v, "indicate accumulator %d empty", p->sel_id));
      __vdbe_add_op1(v, OP_Return, reg_reset);

      if( dist_flag!=0 && dist!=WHERE_DISTINCT_NOOP ){
        struct agg_info_func_t *f = &agg_info->funcs[0];
        __fix_distinct_open_eph(parser, dist, f->i_distinct, f->i_dist_addr);
      }
    } /* endif group_by.  Begin aggregate queries without GROUP BY: */
    else {
      /* Aggregate functions without GROUP BY. tag-select-0820 */
      table_t *tab;
      if( (tab = __is_simple_count(p, agg_info))!=0 ){
        /* tag-select-0821
        **
        ** If __is_simple_count() returns a pointer to a table_t structure, then
        ** the SQL statement is of the form:
        **
        **   SELECT count(*) FROM <tbl>
        **
        ** where the table_t structure returned represents table <tbl>.
        **
        ** This statement is so common that it is optimized specially. The
        ** OP_Count instruction is executed either on the intkey table that
        ** contains the data for table <tbl> or on one of its indexes. It
        ** is better to execute the op on an index, as indexes are almost
        ** always spread across less pages than their corresponding tables.
        */
        const int i_db = __schema_to_index(parser->db, tab->schema);
        const int i_csr = parser->n_tab++;     /* Cursor to scan b-tree */
        index_t *idx;                         /* Iterator variable */
        key_info_t *key_info = 0;               /* Keyinfo for scanned index */
        index_t *best = 0;                    /* Best index found so far */
        pgno_t i_root = tab->tnum;             /* Root page of scanned b-tree */

        __code_verify_schema(parser, i_db);
        __table_lock(parser, i_db, tab->tnum, 0, tab->name);

        /* Search for the index that has the lowest scan cost.
        **
        ** (2011-04-15) Do not do a full scan of an unordered index.
        **
        ** (2013-10-03) Do not count the entries in a partial index.
        **
        ** In practice the key_info_t structure will not be used. It is only
        ** passed to keep OP_OpenRead happy.
        */
        if( !HAS_ROWID(tab) ) best = __primary_key_index(tab);
        if( !p->src->a[0].fg.not_indexed ){
          for(idx=tab->p_index; idx; idx=idx->next){
            if( idx->unordered==0
             && idx->sz_idx_row<tab->sz_tab_row
             && idx->part_idx_where==0
             && (!best || idx->sz_idx_row<best->sz_idx_row)
            ){
              best = idx;
            }
          }
        }
        if( best ){
          i_root = best->tnum;
          key_info = __key_info_of_index(parser, best);
        }

        /* Open a read-only cursor, execute the OP_Count, close the cursor. */
        __vdbe_add_op4_int(v, OP_OpenRead, i_csr, (int)i_root, i_db, 1);
        if( key_info ){
          __vdbe_change_p4(v, -1, (char *)key_info, P4_KEYINFO);
        }
        __assign_aggregate_registers(parser, agg_info);
        __vdbe_add_op2(v, OP_Count, i_csr, AGG_INFO_FUNC_REG(agg_info,0));
        __vdbe_add_op1(v, OP_Close, i_csr);
        EXPLAIN_SIMPLE_COUNT(parser, tab, best);
      }else{
        /* The general case of an aggregate query without GROUP BY
        ** tag-select-0822 */
        int reg_acc = 0;           /* "populate accumulators" flag */
        expr_list_t *distinct = 0;
        u_int16_t dist_flag = 0;
        int dist;

        /* If there are accumulator registers but no min() or max() functions
        ** without FILTER clauses, allocate register reg_acc. Register reg_acc
        ** will contain 0 the first time the inner loop runs, and 1 thereafter.
        ** The code generated by __update_accumulator() uses this to ensure
        ** that the accumulator registers are (a) updated only once if
        ** there are no min() or max functions or (b) always updated for the
        ** first row visited by the aggregate, so that they are updated at
        ** least once even if the FILTER clause means the min() or max()
        ** function visits zero rows.  */
        if( agg_info->n_accumulator ){
          for(i=0; i<agg_info->n_func; i++){
            if( EXPR_HAS_PROPERTY(agg_info->funcs[i].f_expr, EP_WinFunc) ){
              continue;
            }
            if( agg_info->funcs[i].func->func_flags&DBSQL_FUNC_NEEDCOLL ){
              break;
            }
          }
          if( i==agg_info->n_func ){
            reg_acc = ++parser->n_mem;
            __vdbe_add_op2(v, OP_Integer, 0, reg_acc);
          }
        }else if( agg_info->n_func==1 && agg_info->funcs[0].i_distinct>=0 ){
          assert( EXPR_USE_X_LIST(agg_info->funcs[0].f_expr) );
          distinct = agg_info->funcs[0].f_expr->x.list;
          dist_flag = distinct ? (WHERE_WANT_DISTINCT|WHERE_AGG_DISTINCT) : 0;
        }
        __assign_aggregate_registers(parser, agg_info);

        /* This case runs if the aggregate has no GROUP BY clause.  The
        ** processing is much simpler since there is only a single row
        ** of output.
        */
        assert( p->group_by==0 );
        __reset_accumulator(parser, agg_info);

        /* If this query is a candidate for the min/max optimization, then
        ** min_max_flag will have been previously set to either
        ** WHERE_ORDERBY_MIN or WHERE_ORDERBY_MAX and min_max_order_by will
        ** be an appropriate ORDER BY expression for the optimization.
        */
        assert( min_max_flag==WHERE_ORDERBY_NORMAL || min_max_order_by!=0 );
        assert( min_max_order_by==0 || min_max_order_by->n_expr==1 );

        TREETRACE(0x2,parser,p,("WhereBegin\n"));
        w_info = __where_begin(parser, tab_list, where, min_max_order_by,
                                   distinct, p, min_max_flag|dist_flag, 0);
        if( w_info==0 ){
          goto select_end;
        }
        TREETRACE(0x2,parser,p,("WhereBegin returns\n"));
        dist = __where_is_distinct(w_info);
        __update_accumulator(parser, reg_acc, agg_info, dist);
        if( dist!=WHERE_DISTINCT_NOOP ){
          struct agg_info_func_t *f = agg_info->funcs;
          if( f ){
            __fix_distinct_open_eph(parser, dist, f->i_distinct, f->i_dist_addr);
          }
        }

        if( reg_acc ) __vdbe_add_op2(v, OP_Integer, 1, reg_acc);
        if( min_max_flag ){
          __where_min_max_opt_early_out(v, w_info);
        }
        TREETRACE(0x2,parser,p,("WhereEnd\n"));
        __where_end(w_info);
        __finalize_agg_functions(parser, agg_info);
      }

      s_sort.order_by = 0;
      __expr_if_false(parser, having, addr_end, DBSQL_JUMPIFNULL);
      __select_inner_loop(parser, p, -1, 0, 0,
                      p_dest, addr_end, addr_end);
    }
    __vdbe_resolve_label(v, addr_end);

  } /* endif aggregate query */

  if( s_distinct.tnct_type==WHERE_DISTINCT_UNORDERED ){
    EXPLAIN_TEMP_TABLE(parser, "DISTINCT");
  }

  /* If there is an ORDER BY clause, then we need to sort the results
  ** and send them to the callback one by one.  tag-select-0900
  */
  if( s_sort.order_by ){
    assert( p->e_list==e_list );
    __generate_sort_tail(parser, p, &s_sort, e_list->n_expr, p_dest);
  }

  /* Jump here to skip this query
  */
  __vdbe_resolve_label(v, i_end);

  /* The SELECT has been coded. If there is an error in the parser_t structure,
  ** set the return code to 1. Otherwise 0. */
  rc = (parser->n_err>0);

  /* Control jumps to here if an error is encountered above, or upon
  ** successful coding of the SELECT.
  */
select_end:
  assert( db->malloc_failed==0 || db->malloc_failed==1 );
  assert( db->malloc_failed==0 || parser->n_err!=0 );
  __expr_list_delete(db, min_max_order_by);
#ifdef DBSQL_DEBUG
  /* Internal self-checks.  tag-select-1000 */
  if( agg_info && !db->malloc_failed ){
#if TREETRACE_ENABLED
    if( __tree_trace & 0x20 ){
      TREETRACE(0x20,parser,p,("Finished with AggInfo\n"));
      __print_agg_info(agg_info);
    }
#endif
    for(i=0; i<agg_info->n_column; i++){
      expr_t *expr = agg_info->cols[i].c_expr;
      if( expr==0 ) continue;
      assert( expr->agg_info==agg_info );
      assert( expr->i_agg==i );
    }
    for(i=0; i<agg_info->n_func; i++){
      expr_t *expr = agg_info->funcs[i].f_expr;
      assert( expr!=0 );
      assert( expr->agg_info==agg_info );
      assert( expr->i_agg==i );
    }
  }
#endif

#if TREETRACE_ENABLED
  TREETRACE(0x1,parser,p,("end processing\n"));
  if( (__tree_trace & 0x40000)!=0 && EXPLAIN_QUERY_PLAN_PARENT(parser)==0 ){
    __tree_view_select(0, p, 0);
  }
#endif
  EXPLAIN_QUERY_PLAN_POP(parser);
  return rc;
}
