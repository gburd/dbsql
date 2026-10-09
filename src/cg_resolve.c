/*
** 2008 August 18
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
** This file contains routines used for walking the parser tree and
** resolve all identifiers by associating them with a particular
** table and column.
*/
#include "dbsql_int.h"

/*
** Magic table number to mean the EXCLUDED table in an UPSERT statement.
*/
#define EXCLUDED_TABLE_NUMBER  2

/*
** Walk the expression tree expr and increase the aggregate function
** depth (the expr_t.op2 field) by N on every TK_AGG_FUNCTION node.
** This needs to occur when copying a TK_AGG_FUNCTION node from an
** outer query into an inner subquery.
**
** __incr_agg_function_depth(expr,n) is the main routine.  __incr_agg_depth(..)
** is a helper function - a callback for the tree walker.
**
** See also the __window_extra_agg_func_depth() routine in window.c
*/
static int __incr_agg_depth(walker_t *walker, expr_t *expr){
  if( expr->op==TK_AGG_FUNCTION ) expr->op2 += walker->u.n;
  return WRC_Continue;
}
static void __incr_agg_function_depth(expr_t *expr, int N){
  if( N>0 ){
    walker_t w;
    memset(&w, 0, sizeof(w));
    w.expr_callback = __incr_agg_depth;
    w.u.n = N;
    __walk_expr(&w, expr);
  }
}

/*
** Turn the expr expression into an alias for the i_col-th column of the
** result set in e_list.
**
** If the reference is followed by a COLLATE operator, then make sure
** the COLLATE operator is preserved.  For example:
**
**     SELECT a+b, c+d FROM t1 ORDER BY 1 COLLATE nocase;
**
** Should be transformed into:
**
**     SELECT a+b, c+d FROM t1 ORDER BY (a+b) COLLATE nocase;
**
** The n_subquery parameter specifies how many levels of subquery the
** alias is removed from the original expression.  The usual value is
** zero but it might be more if the alias is contained within a subquery
** of the original expression.  The expr_t.op2 field of TK_AGG_FUNCTION
** structures must be increased by the n_subquery amount.
*/
static void __resolve_alias(
  parser_t *parser,         /* Parsing context */
  expr_list_t *e_list,      /* A result set */
  int i_col,              /* A column in the result set.  0..e_list->n_expr-1 */
  expr_t *expr,           /* Transform this into an alias to the result set */
  int n_subquery          /* Number of subqueries that the label is moving */
){
  expr_t *orig;           /* The i_col-th column of the result set */
  expr_t *dup;            /* Copy of orig */
  DBSQL *db;           /* The database connection */

  assert( i_col>=0 && i_col<e_list->n_expr );
  orig = e_list->a[i_col].expr;
  assert( orig!=0 );
  assert( !EXPR_HAS_PROPERTY(expr, EP_Reduced|EP_TokenOnly) );
  if( expr->agg_info ) return;
  db = parser->db;
  dup = __expr_dup(db, orig, 0);
  if( db->malloc_failed ){
    __expr_delete(db, dup);
    dup = 0;
  }else{
    expr_t temp;
    __incr_agg_function_depth(dup, n_subquery);
    if( expr->op==TK_COLLATE ){
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
      dup = __expr_add_collate_string(parser, dup, expr->u.token);
    }
    memcpy(&temp, dup, sizeof(expr_t));
    memcpy(dup, expr, sizeof(expr_t));
    memcpy(expr, &temp, sizeof(expr_t));
    if( EXPR_HAS_PROPERTY(expr, EP_WinFunc) ){
      if( ALWAYS(expr->y.win!=0) ){
        expr->y.win->owner = expr;
      }
    }
    __expr_deferred_delete(parser, dup);
  }
}

/*
** Subqueries store the original database, table and column names for their
** result sets in expr_list_t.a[].span, in the form "DATABASE.TABLE.COLUMN",
** and mark the expression-list item by setting expr_list_t.a[].fg.e_name
** to ENAME_TAB.
**
** Check to see if the span/e_name of the expression-list item passed to this
** routine matches the db, tab, and col.  If any of db, tab, and col are
** NULL then those fields will match anything. Return true if there is a match,
** or false otherwise.
**
** SF_NestedFrom subqueries also store an entry for the implicit rowid (or
** _rowid_, or oid) column by setting expr_list_t.a[].fg.e_name to ENAME_ROWID,
** and setting span to "DATABASE.TABLE.<rowid-alias>". This type of item
** argument matches if col is a rowid alias. If it is not NULL, (*pb_rowid)
** is set to 1 if there is this kind of match.
*/
int __match_e_name(
  const struct expr_list_item_t *item,
  const char *col,
  const char *tab,
  const char *db,
  int *pb_rowid
){
  int n;
  const char *span;
  int e_name = item->fg.e_name;
  if( e_name!=ENAME_TAB && (e_name!=ENAME_ROWID || NEVER(pb_rowid==0)) ){
    return 0;
  }
  assert( pb_rowid==0 || *pb_rowid==0 );
  span = item->e_name;
  for(n=0; ALWAYS(span[n]) && span[n]!='.'; n++){}
  if( db && (__str_nicmp(span, db, n)!=0 || db[n]!=0) ){
    return 0;
  }
  span += n+1;
  for(n=0; ALWAYS(span[n]) && span[n]!='.'; n++){}
  if( tab && (__str_nicmp(span, tab, n)!=0 || tab[n]!=0) ){
    return 0;
  }
  span += n+1;
  if( col ){
    if( e_name==ENAME_TAB && __str_icmp(span, col)!=0 ) return 0;
    if( e_name==ENAME_ROWID && __is_rowid(col)==0 ) return 0;
  }
  if( e_name==ENAME_ROWID ) *pb_rowid = 1;
  return 1;
}

/*
** Return TRUE if the double-quoted string  mis-feature should be supported.
*/
static int __are_double_quoted_strings_enabled(DBSQL *db, name_context_t *top_nc){
  if( db->init.busy ) return 1;  /* Always support for legacy schemas */
  if( top_nc->nc_flags & NC_IsDDL ){
    /* Currently parsing a DDL statement */
    if( __writable_schema(db) && (db->flags & DBSQL_DqsDML)!=0 ){
      return 1;
    }
    return (db->flags & DBSQL_DqsDDL)!=0;
  }else{
    /* Currently parsing a DML statement */
    return (db->flags & DBSQL_DqsDML)!=0;
  }
}

/*
** The argument is guaranteed to be a non-NULL expr_t node of type TK_COLUMN.
** return the appropriate col_used mask.
*/
Bitmask __expr_col_used(expr_t *expr){
  int n;
  table_t *ex_tab;

  n = expr->i_column;
  assert( EXPR_USE_Y_TAB(expr) );
  ex_tab = expr->y.tab;
  assert( ex_tab!=0 );
  assert( n < ex_tab->n_col );
  if( (ex_tab->tab_flags & TF_HasGenerated)!=0
   && (ex_tab->cols[n].col_flags & COLFLAG_GENERATED)!=0
  ){
    testcase( ex_tab->n_col==BMS-1 );
    testcase( ex_tab->n_col==BMS );
    return ex_tab->n_col>=BMS ? ALLBITS : MASKBIT(ex_tab->n_col)-1;
  }else{
    testcase( n==BMS-1 );
    testcase( n==BMS );
    if( n>=BMS ) n = BMS-1;
    return ((Bitmask)1)<<n;
  }
}

/*
** Create a new expression term for the column specified by match and
** i_column.  Append this new expression term to the FULL JOIN Match set
** in *list.  Create a new *list if this is the first term in the
** set.
*/
static void __extend_fj_match(
  parser_t *parser,          /* Parsing context */
  expr_list_t **list,      /* expr_list_t to extend */
  src_item_t *match,        /* Source table containing the column */
  int16_t i_column             /* The column number */
){
  expr_t *p_new = __expr_alloc(parser->db, TK_COLUMN, 0, 0);
  if( p_new ){
    p_new->i_table = match->i_cursor;
    p_new->i_column = i_column;
    p_new->y.tab = match->s_tab;
    assert( (match->fg.jointype & (JT_LEFT|JT_LTORJ))!=0 );
    EXPR_SET_PROPERTY(p_new, EP_CanBeNull);
    *list = __expr_list_append(parser, *list, p_new);
  }
}

/*
** Return TRUE (non-zero) if tab is a valid name for the schema table tab.
*/
static DBSQL_NOINLINE int __is_valid_schema_table_name(
  const char *z_tab,         /* Name as it appears in the SQL */
  table_t *tab,              /* The schema table we are trying to match */
  const char *db           /* non-NULL if a database qualifier is present */
){
  const char *legacy;
  assert( tab!=0 );
  assert( tab->tnum==1 );
  if( __str_nicmp(z_tab, "dbsql_", 6)!=0 ) return 0;
  legacy = tab->name;
  if( strcmp(legacy+6, &LEGACY_TEMP_SCHEMA_TABLE[6])==0 ){
    if( __str_icmp(z_tab+6, &PREFERRED_TEMP_SCHEMA_TABLE[6])==0 ){
      return 1;
    }
    if( db==0 ) return 0;
    if( __str_icmp(z_tab+6, &LEGACY_SCHEMA_TABLE[6])==0 ) return 1;
    if( __str_icmp(z_tab+6, &PREFERRED_SCHEMA_TABLE[6])==0 ) return 1;
  }else{
    if( __str_icmp(z_tab+6, &PREFERRED_SCHEMA_TABLE[6])==0 ) return 1;
  }
  return 0;
}

/*
** Given the name of a column of the form X.Y.Z or Y.Z or just Z, look up
** that name in the set of source tables in src_list and make the expr
** expression node refer back to that source column.  The following changes
** are made to expr:
**
**    expr->i_db           Set the index in db->dbs[] of the database X
**                         (even if X is implied).
**    expr->i_table        Set to the cursor number for the table obtained
**                         from src_list.
**    expr->y.tab        Points to the table_t structure of X.Y (even if
**                         X and/or Y are implied.)
**    expr->i_column       Set to the column number within the table.
**    expr->op            Set to TK_COLUMN.
**    expr->left         Any expression this points to is deleted
**    expr->right        Any expression this points to is deleted.
**
** The db variable is the name of the database (the "X").  This value may be
** NULL meaning that name is of the form Y.Z or Z.  Any available database
** can be used.  The table variable is the name of the table (the "Y").  This
** value can be NULL if db is also NULL.  If table is NULL it
** means that the form of the name is Z and that columns from any table
** can be used.
**
** If the name cannot be resolved unambiguously, leave an error message
** in parser and return WRC_Abort.  Return WRC_Prune on success.
*/
static int __lookup_name(
  parser_t *parser,       /* The parsing context */
  const char *z_db,     /* Name of the database containing table, or NULL */
  const char *z_tab,    /* Name of table containing column, or NULL */
  const expr_t *right,  /* Name of the column. */
  name_context_t *nc,    /* The name context used to resolve the name */
  expr_t *expr          /* Make this EXPR node point to the selected column */
){
  int i, j;                         /* Loop counters */
  int cnt = 0;                      /* Number of matching column names */
  int cnt_tab = 0;                   /* Number of potential "rowid" matches */
  int n_subquery = 0;                /* How many levels of subquery */
  DBSQL *db = parser->db;         /* The database connection */
  src_item_t *item;                   /* Use for looping over src_list items */
  src_item_t *match = 0;              /* The matching src_list item */
  name_context_t *top_nc = nc;        /* First namecontext in the list */
  schema_t *schema = 0;              /* schema_t of the expression */
  int new_expr_op = TK_COLUMN;       /* New value for expr->op on success */
  table_t *tab = 0;                  /* table_t holding the row */
  expr_list_t *fj_match = 0;           /* Matches for FULL JOIN .. USING */
  const char *col = right->u.token;

  assert( nc );     /* the name context cannot be NULL. */
  assert( col );    /* The Z in X.Y.Z cannot be NULL */
  assert( z_db==0 || z_tab!=0 );
  assert( !EXPR_HAS_PROPERTY(expr, EP_TokenOnly|EP_Reduced) );

  /* Initialize the node to no-match */
  expr->i_table = -1;
  EXPR_SET_VVA_PROPERTY(expr, EP_NoReduce);

  /* Translate the schema name in db into a pointer to the corresponding
  ** schema.  If not found, schema will remain NULL and nothing will match
  ** resulting in an appropriate error message toward the end of this routine
  */
  if( z_db ){
    testcase( nc->nc_flags & NC_PartIdx );
    testcase( nc->nc_flags & NC_IsCheck );
    if( (nc->nc_flags & (NC_PartIdx|NC_IsCheck))!=0 ){
      /* Silently ignore database qualifiers inside CHECK constraints and
      ** partial indices.  Do not raise errors because that might break
      ** legacy and because it does not hurt anything to just ignore the
      ** database name. */
      z_db = 0;
    }else{
      for(i=0; i<db->n_db; i++){
        assert( db->dbs[i].db_s_name );
        if( __str_icmp(db->dbs[i].db_s_name,z_db)==0 ){
          schema = db->dbs[i].schema;
          break;
        }
      }
      if( i==db->n_db && __str_icmp("main", z_db)==0 ){
        /* This branch is taken when the main database has been renamed
        ** using DBSQL_DBCONFIG_MAINDBNAME. */
        schema = db->dbs[0].schema;
        z_db = db->dbs[0].db_s_name;
      }
    }
  }

  /* Start at the inner-most context and move outward until a match is found */
  assert( nc && cnt==0 );
  do{
    expr_list_t *e_list;
    src_list_t *src_list = nc->src_list;

    if( src_list ){
      for(i=0, item=src_list->a; i<src_list->n_src; i++, item++){
        tab = item->s_tab;
        assert( tab!=0 && tab->name!=0 );
        assert( tab->n_col>0 || parser->n_err );
        assert( (int)item->fg.is_nested_from == IS_NESTED_FROM(item));
        if( item->fg.is_nested_from ){
          /* In this case, item is a subquery that has been formed from a
          ** parenthesized subset of the FROM clause terms.  Example:
          **   .... FROM t1 LEFT JOIN (t2 RIGHT JOIN t3 USING(x)) USING(y) ...
          **                          \_________________________/
          **             This item -------------^
          */
          int hit = 0;
          select_t *sel;
          assert( item->fg.is_subquery );
          assert( item->u4.subq!=0 );
          sel = item->u4.subq->p_select;
          assert( sel!=0 );
          e_list = sel->e_list;
          assert( e_list!=0 );
          assert( e_list->n_expr==tab->n_col );
          for(j=0; j<e_list->n_expr; j++){
            int rowid = 0;       /* True if possible rowid match */
            if( !__match_e_name(&e_list->a[j], col, z_tab, z_db, &rowid) ){
              continue;
            }
            if( rowid==0 ){
              if( cnt>0 ){
                if( item->fg.is_using==0
                 || __id_list_index(item->u3.p_using, col)<0
                 || match==item
                ){
                  /* Two or more tables have the same column name which is
                  ** not joined by USING. Or, a single table has two columns
                  ** that match a USING term (if match==item). These are both
                  ** "ambiguous column name" errors. Signal as much by clearing
                  ** fj_match and letting cnt go above 1. */
                  __expr_list_delete(db, fj_match);
                  fj_match = 0;
                }else
                if( (item->fg.jointype & JT_RIGHT)==0 ){
                  /* An INNER or LEFT JOIN.  Use the left-most table */
                  continue;
                }else
                if( (item->fg.jointype & JT_LEFT)==0 ){
                  /* A RIGHT JOIN.  Use the right-most table */
                  cnt = 0;
                  __expr_list_delete(db, fj_match);
                  fj_match = 0;
                }else{
                  /* For a FULL JOIN, we must construct a coalesce() func */
                  __extend_fj_match(parser, &fj_match, match, expr->i_column);
                }
              }
              cnt++;
              hit = 1;
            }else if( cnt>0 ){
              /* This is a potential rowid match, but there has already been
              ** a real match found. So this can be ignored.  */
              continue;
            }
            cnt_tab++;
            match = item;
            expr->i_column = j;
            e_list->a[j].fg.used = 1;

            /* rowid cannot be part of a USING clause - assert() this. */
            assert( rowid==0 || e_list->a[j].fg.using_term==0 );
            if( e_list->a[j].fg.using_term ) break;
          }
          if( hit || z_tab==0 ) continue;
        }
        assert( z_db==0 || z_tab!=0 );
        if( z_tab ){
          if( z_db ){
            if( tab->schema!=schema ) continue;
            if( schema==0 && strcmp(z_db,"*")!=0 ) continue;
          }
          if( item->alias!=0 ){
            if( __str_icmp(z_tab, item->alias)!=0 ){
              continue;
            }
          }else if( __str_icmp(z_tab, tab->name)!=0 ){
            if( tab->tnum!=1 ) continue;
            if( !__is_valid_schema_table_name(z_tab, tab, z_db) ) continue;
          }
          assert( EXPR_USE_Y_TAB(expr) );
          if( IN_RENAME_OBJECT && item->alias ){
            __rename_token_remap(parser, 0, (void*)&expr->y.tab);
          }
        }
        j = __column_index(tab, col);
        if( j>=0 ){
          if( cnt>0 ){
            if( item->fg.is_using==0
             || __id_list_index(item->u3.p_using, col)<0
            ){
              /* Two or more tables have the same column name which is
              ** not joined by USING.  This is an error.  Signal as much
              ** by clearing fj_match and letting cnt go above 1. */
              __expr_list_delete(db, fj_match);
              fj_match = 0;
            }else
            if( (item->fg.jointype & JT_RIGHT)==0 ){
              /* An INNER or LEFT JOIN.  Use the left-most table */
              continue;
            }else
            if( (item->fg.jointype & JT_LEFT)==0 ){
              /* A RIGHT JOIN.  Use the right-most table */
              cnt = 0;
              __expr_list_delete(db, fj_match);
              fj_match = 0;
            }else{
              /* For a FULL JOIN, we must construct a coalesce() func */
              __extend_fj_match(parser, &fj_match, match, expr->i_column);
            }
          }
          cnt++;
          match = item;
          /* Substitute the rowid (column -1) for the INTEGER PRIMARY KEY */
          expr->i_column = j==tab->i_p_key ? -1 : (int16_t)j;
          if( item->fg.is_nested_from ){
            __src_item_column_used(item, j);
          }
        }
        if( 0==cnt && VISIBLE_ROWID(tab) ){
          /* tab is a potential ROWID match.  Keep track of it and match
          ** the ROWID later if that seems appropriate.  (Search for "cnt_tab"
          ** to find related code.)  Only allow a ROWID match if there is
          ** a single ROWID match candidate.
          */
#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
          /* In DBSQL_ALLOW_ROWID_IN_VIEW mode, allow a ROWID match
          ** if there is a single VIEW candidate or if there is a single
          ** non-VIEW candidate plus multiple VIEW candidates.  In other
          ** words non-VIEW candidate terms take precedence over VIEWs.
          */
          if( cnt_tab==0
           || (cnt_tab==1
               && match!=0
               && ALWAYS(match->s_tab!=0)
               && (match->s_tab->tab_flags & TF_Ephemeral)!=0
               && (tab->tab_flags & TF_Ephemeral)==0)
          ){
            cnt_tab = 1;
            match = item;
          }else{
            cnt_tab++;
          }
#else
          /* The (much more common) non-DBSQL_ALLOW_ROWID_IN_VIEW case is
          ** simpler since we require exactly one candidate, which will
          ** always be a non-VIEW
          */
          cnt_tab++;
          match = item;
#endif
        }
      }
      if( match ){
        expr->i_table = match->i_cursor;
        assert( EXPR_USE_Y_TAB(expr) );
        expr->y.tab = match->s_tab;
        if( (match->fg.jointype & (JT_LEFT|JT_LTORJ))!=0 ){
          EXPR_SET_PROPERTY(expr, EP_CanBeNull);
        }
        schema = expr->y.tab->schema;
      }
    } /* if( src_list ) */

#if !defined(DBSQL_OMIT_TRIGGER) || !defined(DBSQL_OMIT_UPSERT)
    /* If we have not already resolved the name, then maybe
    ** it is a new.* or old.* trigger argument reference.  Or
    ** maybe it is an excluded.* from an upsert.  Or maybe it is
    ** a reference in the RETURNING clause to a table being modified.
    */
    if( cnt==0 && z_db==0 ){
      tab = 0;
#ifndef DBSQL_OMIT_TRIGGER
      if( parser->trigger_tab!=0 ){
        int op = parser->trigger_op;
        assert( op==TK_DELETE || op==TK_UPDATE || op==TK_INSERT );
        if( parser->returning ){
          if( (nc->nc_flags & NC_UBaseReg)!=0
           && ALWAYS(z_tab==0
                     || __str_icmp(z_tab,parser->trigger_tab->name)==0
                     || __is_valid_schema_table_name(z_tab, parser->trigger_tab, 0))
          ){
            expr->i_table = op!=TK_DELETE;
            tab = parser->trigger_tab;
          }
        }else if( op!=TK_DELETE && z_tab && __str_icmp("new",z_tab) == 0 ){
          expr->i_table = 1;
          tab = parser->trigger_tab;
        }else if( op!=TK_INSERT && z_tab && __str_icmp("old",z_tab)==0 ){
          expr->i_table = 0;
          tab = parser->trigger_tab;
        }
      }
#endif /* DBSQL_OMIT_TRIGGER */
#ifndef DBSQL_OMIT_UPSERT
      if( (nc->nc_flags & NC_UUpsert)!=0 && z_tab!=0 ){
        upsert_t *upsert = nc->u_nc.upsert;
        if( upsert && __str_icmp("excluded",z_tab)==0 ){
          tab = upsert->upsert_src->a[0].s_tab;
          expr->i_table = EXCLUDED_TABLE_NUMBER;
        }
      }
#endif /* DBSQL_OMIT_UPSERT */

      if( tab ){
        int i_col;
        schema = tab->schema;
        cnt_tab++;
        i_col = __column_index(tab, col);
        if( i_col>=0 ){
          if( tab->i_p_key==i_col ) i_col = -1;
        }else{
          if( __is_rowid(col) && VISIBLE_ROWID(tab) ){
            i_col = -1;
          }else{
            i_col = tab->n_col;
          }
        }
        if( i_col<tab->n_col ){
          cnt++;
          match = 0;
#ifndef DBSQL_OMIT_UPSERT
          if( expr->i_table==EXCLUDED_TABLE_NUMBER ){
            testcase( i_col==(-1) );
            assert( EXPR_USE_Y_TAB(expr) );
            if( IN_RENAME_OBJECT ){
              expr->i_column = i_col;
              expr->y.tab = tab;
              new_expr_op = TK_COLUMN;
            }else{
              expr->i_table = nc->u_nc.upsert->reg_data +
                 __table_column_to_storage(tab, i_col);
              new_expr_op = TK_REGISTER;
            }
          }else
#endif /* DBSQL_OMIT_UPSERT */
          {
            assert( EXPR_USE_Y_TAB(expr) );
            expr->y.tab = tab;
            if( parser->returning ){
              new_expr_op = TK_REGISTER;
              expr->op2 = TK_COLUMN;
              expr->i_column = i_col;
              expr->i_table = nc->u_nc.i_base_reg + (tab->n_col+1)*expr->i_table +
                 __table_column_to_storage(tab, i_col) + 1;
            }else{
              expr->i_column = (int16_t)i_col;
              new_expr_op = TK_TRIGGER;
#ifndef DBSQL_OMIT_TRIGGER
              if( i_col<0 ){
                expr->aff_expr = DBSQL_AFF_INTEGER;
              }else if( expr->i_table==0 ){
                testcase( i_col==31 );
                testcase( i_col==32 );
                parser->oldmask |= (i_col>=32 ? 0xffffffff : (((u_int32_t)1)<<i_col));
              }else{
                testcase( i_col==31 );
                testcase( i_col==32 );
                parser->newmask |= (i_col>=32 ? 0xffffffff : (((u_int32_t)1)<<i_col));
              }
#endif /* DBSQL_OMIT_TRIGGER */
            }
          }
        }
      }
    }
#endif /* !defined(DBSQL_OMIT_TRIGGER) || !defined(DBSQL_OMIT_UPSERT) */

    /*
    ** Perhaps the name is a reference to the ROWID
    */
    if( cnt==0
     && cnt_tab>=1
     && match
     && (nc->nc_flags & (NC_IdxExpr|NC_GenCol))==0
     && __is_rowid(col)
     && ALWAYS(VISIBLE_ROWID(match->s_tab) || match->fg.is_nested_from)
    ){
      cnt = cnt_tab;
#if DBSQL_ALLOW_ROWID_IN_VIEW+0==2
      if( match->s_tab!=0 && IsView(match->s_tab) ){
        new_expr_op = TK_NULL;
      }
#endif
      if( match->fg.is_nested_from==0 ) expr->i_column = -1;
      expr->aff_expr = DBSQL_AFF_INTEGER;
    }

    /*
    ** If the input is of the form Z (not Y.Z or X.Y.Z) then the name Z
    ** might refer to an result-set alias.  This happens, for example, when
    ** we are resolving names in the WHERE clause of the following command:
    **
    **     SELECT a+b AS x FROM table WHERE x<10;
    **
    ** In cases like this, replace expr with a copy of the expression that
    ** forms the result set entry ("a+b" in the example) and return immediately.
    ** Note that the expression in the result set should have already been
    ** resolved by the time the WHERE clause is resolved.
    **
    ** The ability to use an output result-set column in the WHERE, GROUP BY,
    ** or HAVING clauses, or as part of a larger expression in the ORDER BY
    ** clause is not standard SQL.  This is a (goofy) dbsql extension, that
    ** is supported for backwards compatibility only. Hence, we issue a warning
    ** on dbsql_log() whenever the capability is used.
    */
    if( cnt==0
     && (nc->nc_flags & NC_UEList)!=0
     && z_tab==0
    ){
      e_list = nc->u_nc.e_list;
      assert( e_list!=0 );
      for(j=0; j<e_list->n_expr; j++){
        char *as = e_list->a[j].e_name;
        if( e_list->a[j].fg.e_name==ENAME_NAME
         && dbsql_stricmp(as, col)==0
        ){
          expr_t *orig;
          assert( expr->left==0 && expr->right==0 );
          assert( EXPR_USE_X_LIST(expr)==0 || expr->x.list==0 );
          assert( EXPR_USE_X_SELECT(expr)==0 || expr->x.p_select==0 );
          orig = e_list->a[j].expr;
          if( (nc->nc_flags&NC_AllowAgg)==0 && EXPR_HAS_PROPERTY(orig, EP_Agg) ){
            __error_msg(parser, "misuse of aliased aggregate %s", as);
            return WRC_Abort;
          }
          if( EXPR_HAS_PROPERTY(orig, EP_Win)
           && ((nc->nc_flags&NC_AllowWin)==0 || nc!=top_nc )
          ){
            __error_msg(parser, "misuse of aliased window function %s",as);
            return WRC_Abort;
          }
          if( __expr_vector_size(orig)!=1 ){
            __error_msg(parser, "row value misused");
            return WRC_Abort;
          }
          __resolve_alias(parser, e_list, j, expr, n_subquery);
          cnt = 1;
          match = 0;
          assert( z_tab==0 && z_db==0 );
          if( IN_RENAME_OBJECT ){
            __rename_token_remap(parser, 0, (void*)expr);
          }
          goto lookupname_end;
        }
      }
    }

    /* Advance to the next name context.  The loop will exit when either
    ** we have a match (cnt>0) or when we run out of name contexts.
    */
    if( cnt ) break;
    nc = nc->next;
    n_subquery++;
  }while( nc );


  /*
  ** If X and Y are NULL (in other words if only the column name Z is
  ** supplied) and the value of Z is enclosed in double-quotes, then
  ** Z is a string literal if it doesn't match any column names.  In that
  ** case, we need to return right away and not make any changes to
  ** expr.
  **
  ** Because no reference was made to outer contexts, the nc->n_ref
  ** fields are not changed in any context.
  */
  if( cnt==0 && z_tab==0 ){
    assert( expr->op==TK_ID );
    if( EXPR_HAS_PROPERTY(expr,EP_DblQuoted)
     && __are_double_quoted_strings_enabled(db, top_nc)
    ){
      /* If a double-quoted identifier does not match any known column name,
      ** then treat it as a string.
      **
      ** This hack was added in the early days of dbsql in a misguided attempt
      ** to be compatible with __my_sql 3.x, which used double-quotes for strings.
      ** I now sorely regret putting in this hack. The effect of this hack is
      ** that misspelled identifier names are silently converted into strings
      ** rather than causing an error, to the frustration of countless
      ** programmers. To all those frustrated programmers, my apologies.
      **
      ** Someday, I hope to get rid of this hack. Unfortunately there is
      ** a huge amount of legacy SQL that uses it. So for now, we just
      ** issue a warning.
      */
      dbsql_log(DBSQL_WARNING,
        "double-quoted string literal: \"%w\"", col);
#ifdef DBSQL_ENABLE_NORMALIZE
      __vdbe_add_dblquote_str(db, parser->vdbe, col);
#endif
      expr->op = TK_STRING;
      memset(&expr->y, 0, sizeof(expr->y));
      return WRC_Prune;
    }
    if( __expr_id_to_true_false(expr) ){
      return WRC_Prune;
    }
  }

  /*
  ** cnt==0 means there was not match.
  ** cnt>1 means there were two or more matches.
  **
  ** cnt==0 is always an error.  cnt>1 is often an error, but might
  ** be multiple matches for a NATURAL LEFT JOIN or a LEFT JOIN USING.
  */
  assert( fj_match==0 || cnt>0 );
  assert( !EXPR_HAS_PROPERTY(expr, EP_xIsSelect|EP_IntValue) );
  if( cnt!=1 ){
    const char *err;
    if( fj_match ){
      if( fj_match->n_expr==cnt-1 ){
        if( EXPR_HAS_PROPERTY(expr,EP_Leaf) ){
          EXPR_CLEAR_PROPERTY(expr,EP_Leaf);
        }else{
          __expr_delete(db, expr->left);
          expr->left = 0;
          __expr_delete(db, expr->right);
          expr->right = 0;
        }
        __extend_fj_match(parser, &fj_match, match, expr->i_column);
        expr->op = TK_FUNCTION;
        expr->u.token = "coalesce";
        expr->x.list = fj_match;
        expr->aff_expr = DBSQL_AFF_DEFER;
        cnt = 1;
        goto lookupname_end;
      }else{
        __expr_list_delete(db, fj_match);
        fj_match = 0;
      }
    }
    err = cnt==0 ? "no such column" : "ambiguous column name";
    if( z_db ){
      __error_msg(parser, "%s: %s.%s.%s", err, z_db, z_tab, col);
    }else if( z_tab ){
      __error_msg(parser, "%s: %s.%s", err, z_tab, col);
    }else if( cnt==0 && EXPR_HAS_PROPERTY(right,EP_DblQuoted) ){
      __error_msg(parser, "%s: \"%s\" - should this be a"
                              " string literal in single-quotes?",
                              err, col);
    }else{
      __error_msg(parser, "%s: %s", err, col);
    }
    __record_error_offset_of_expr(parser->db, expr);
    parser->check_schema = 1;
    top_nc->n_nc_err++;
    new_expr_op = TK_NULL;
  }
  assert( fj_match==0 );

  /* Remove all substructure from expr */
  if( !EXPR_HAS_PROPERTY(expr,(EP_TokenOnly|EP_Leaf)) ){
    __expr_delete(db, expr->left);
    expr->left = 0;
    __expr_delete(db, expr->right);
    expr->right = 0;
    EXPR_SET_PROPERTY(expr, EP_Leaf);
  }

  /* If a column from a table in src_list is referenced, then record
  ** this fact in the src_list.a[].col_used bitmask.  column_t 0 causes
  ** bit 0 to be set.  column_t 1 sets bit 1.  And so forth.  Bit 63 is
  ** set if the 63rd or any subsequent column is used.
  **
  ** The col_used mask is an optimization used to help determine if an
  ** index is a covering index.  The correct answer is still obtained
  ** if the mask contains extra set bits.  However, it is important to
  ** avoid setting bits beyond the maximum column number of the table.
  ** (See ticket [b92e5e8ec2cdbaa1]).
  **
  ** If a generated column is referenced, set bits for every column
  ** of the table.
  */
  if( match ){
    if( expr->i_column>=0 ){
      match->col_used |= __expr_col_used(expr);
    }else{
      match->fg.rowid_used = 1;
    }
  }

  expr->op = new_expr_op;
lookupname_end:
  if( cnt==1 ){
    assert( nc!=0 );
#ifndef DBSQL_OMIT_AUTHORIZATION
    if( parser->db->auth
     && (expr->op==TK_COLUMN || expr->op==TK_TRIGGER)
    ){
      __auth_read(parser, expr, schema, nc->src_list);
    }
#endif
    /* Increment the n_ref value on all name contexts from __top_nc up to
    ** the point where the name matched. */
    for(;;){
      assert( top_nc!=0 );
      top_nc->n_ref++;
      if( top_nc==nc ) break;
      top_nc = top_nc->next;
    }
    return WRC_Prune;
  } else {
    return WRC_Abort;
  }
}

/*
** Allocate and return a pointer to an expression to load the column i_col
** from datasource i_src in src_list_t src.
*/
expr_t *__create_column_expr(DBSQL *db, src_list_t *src, int i_src, int i_col){
  expr_t *p = __expr_alloc(db, TK_COLUMN, 0, 0);
  if( p ){
    src_item_t *item = &src->a[i_src];
    table_t *tab;
    assert( EXPR_USE_Y_TAB(p) );
    tab = p->y.tab = item->s_tab;
    p->i_table = item->i_cursor;
    if( p->y.tab->i_p_key==i_col ){
      p->i_column = -1;
    }else{
      p->i_column = (yn_var_t)i_col;
      if( (tab->tab_flags & TF_HasGenerated)!=0
       && (tab->cols[i_col].col_flags & COLFLAG_GENERATED)!=0
      ){
        testcase( tab->n_col==63 );
        testcase( tab->n_col==64 );
        item->col_used = tab->n_col>=64 ? ALLBITS : MASKBIT(tab->n_col)-1;
      }else{
        testcase( i_col==BMS );
        testcase( i_col==BMS-1 );
        item->col_used |= ((Bitmask)1)<<(i_col>=BMS ? BMS-1 : i_col);
      }
    }
  }
  return p;
}

/*
** Report an error that an expression is not valid for some set of
** nc->nc_flags values determined by valid_mask.
**
** static void not_valid(
**   parser_t *parser,       // Leave error message here
**   name_context_t *nc,    // The name context
**   const char *msg,    // Type of error
**   int valid_mask,       // Set of contexts for which prohibited
**   expr_t *expr          // Invalidate this expression on error
** ){...}
**
** As an optimization, since the conditional is almost always false
** (because errors are rare), the conditional is moved outside of the
** function call using a macro.
*/
static void __not_valid_impl(
   parser_t *parser,       /* Leave error message here */
   name_context_t *nc,    /* The name context */
   const char *msg,    /* Type of error */
   expr_t *expr,         /* Invalidate this expression on error */
   expr_t *error         /* Associate error with this expression */
){
  const char *in = "partial index WHERE clauses";
  if( nc->nc_flags & NC_IdxExpr )      in = "index expressions";
#ifndef DBSQL_OMIT_CHECK
  else if( nc->nc_flags & NC_IsCheck ) in = "CHECK constraints";
#endif
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
  else if( nc->nc_flags & NC_GenCol ) in = "generated columns";
#endif
  __error_msg(parser, "%s prohibited in %s", msg, in);
  if( expr ) expr->op = TK_NULL;
  __record_error_offset_of_expr(parser->db, error);
}
#define __resolve_not_valid(P,N,M,X,E,R) \
  assert( ((X)&~(NC_IsCheck|NC_PartIdx|NC_IdxExpr|NC_GenCol))==0 ); \
  if( ((N)->nc_flags & (X))!=0 ) __not_valid_impl(P,N,M,E,R);

/*
** Expression p should encode a floating point value between 1.0 and 0.0.
** Return 134,217,728 (2^27) times this value.  Or return -1 if p is not
** a floating point value between 1.0 and 0.0.
*/
static int __expr_probability(expr_t *p){
  double r = -1.0;
  if( p->op!=TK_FLOAT ) return -1;
  assert( !EXPR_HAS_PROPERTY(p, EP_IntValue) );
  __ato_f(p->u.token, &r);
  assert( r>=0.0 );
  if( r>1.0 ) return -1;
  return (int)(r*134217728.0);
}

/*
** Set the EP_SubtArg property on every expression inside of
** list.  If any subexpression is actually a subquery, then
** also set the EP_SubtArg property on the first result-set
** column of that subquery.
*/
static DBSQL_NOINLINE void __resolve_set_expr_subtype_arg(expr_list_t *list){
  int nn, ii;
  nn = list ? list->n_expr : 0;
  for(ii=0; ii<nn; ii++){
    expr_t *expr = list->a[ii].expr;
    while( 1 /*exit-by-break*/ ){
      EXPR_SET_PROPERTY(expr, EP_SubtArg);
      if( expr->op==TK_SELECT ){
        assert( EXPR_USE_X_SELECT(expr) );
        assert( expr->x.p_select!=0 );
        __resolve_set_expr_subtype_arg(expr->x.p_select->e_list);
        break;
      }
      if( expr->op==TK_UPLUS ){
        expr = expr->left;
        assert( expr!=0 );
      }else{
        break;
      }
    }
  }
}

/*
** This routine is callback for __walk_expr().
**
** Resolve symbolic names into TK_COLUMN operators for the current
** node in the expression tree.  Return 0 to continue the search down
** the tree or 2 to abort the tree walk.
**
** This routine also does error checking and name resolution for
** function names.  The operator for aggregate functions is changed
** to TK_AGG_FUNCTION.
*/
static int __resolve_expr_step(walker_t *walker, expr_t *expr){
  name_context_t *nc;
  parser_t *parser;

  nc = walker->u.nc;
  assert( nc!=0 );
  parser = nc->parser;
  assert( parser==walker->parser );

#ifndef NDEBUG
  if( nc->src_list && nc->src_list->n_alloc>0 ){
    src_list_t *src_list = nc->src_list;
    int i;
    for(i=0; i<nc->src_list->n_src; i++){
      assert( src_list->a[i].i_cursor>=0 && src_list->a[i].i_cursor<parser->n_tab);
    }
  }
#endif
  switch( expr->op ){

    /* The special operator TK_ROW means use the rowid for the first
    ** column in the FROM clause.  This is used by the LIMIT and ORDER BY
    ** clause processing on UPDATE and DELETE statements, and by
    ** UPDATE ... FROM statement processing.
    */
    case TK_ROW: {
      src_list_t *src_list = nc->src_list;
      src_item_t *item;
      assert( src_list && src_list->n_src>=1 );
      item = src_list->a;
      expr->op = TK_COLUMN;
      assert( EXPR_USE_Y_TAB(expr) );
      expr->y.tab = item->s_tab;
      expr->i_table = item->i_cursor;
      expr->i_column--;
      expr->aff_expr = DBSQL_AFF_INTEGER;
      break;
    }

    /* An optimization:  Attempt to convert
    **
    **      "expr IS NOT NULL"  -->  "TRUE"
    **      "expr IS NULL"      -->  "FALSE"
    **
    ** if we can prove that "expr" is never NULL.  Call this the
    ** "NOT NULL strength reduction optimization".
    **
    ** If this optimization occurs, also restore the name_context_t ref-counts
    ** to the state they where in before the "column" LHS expression was
    ** resolved.  This prevents "column" from being counted as having been
    ** referenced, which might prevent a SELECT from being erroneously
    ** marked as correlated.
    **
    ** 2024-03-28: Beware of aggregates.  A bare column of aggregated table
    ** can still evaluate to NULL even though it is marked as NOT NULL.
    ** Example:
    **
    **       CREATE TABLE t1(a INT NOT NULL);
    **       SELECT a, a IS NULL, a IS NOT NULL, count(*) FROM t1;
    **
    ** The "a IS NULL" and "a IS NOT NULL" expressions cannot be optimized
    ** here because at the time this case is hit, we do not yet know whether
    ** or not t1 is being aggregated.  We have to assume the worst and omit
    ** the optimization.  The only time it is safe to apply this optimization
    ** is within the WHERE clause.
    */
    case TK_NOTNULL:
    case TK_ISNULL: {
      int an_ref[8];
      name_context_t *p;
      int i;
      for(i=0, p=nc; p && i<ARRAY_SIZE(an_ref); p=p->next, i++){
        an_ref[i] = p->n_ref;
      }
      __walk_expr(walker, expr->left);
      if( IN_RENAME_OBJECT ) return WRC_Prune;
      if( __expr_can_be_null(expr->left) ){
        /* The expression can be NULL.  So the optimization does not apply */
        return WRC_Prune;
      }

      for(i=0, p=nc; p; p=p->next, i++){
        if( (p->nc_flags & NC_Where)==0 ){
          return WRC_Prune;  /* Not in a WHERE clause.  Unsafe to optimize. */
        }
      }
      testcase( EXPR_HAS_PROPERTY(expr, EP_OuterON) );
      assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
#if TREETRACE_ENABLED
      if( __tree_trace & 0x80000 ){
        __debug_printf(
           "NOT NULL strength reduction converts the following to %d:\n",
           expr->op==TK_NOTNULL
        );
        __show_expr(expr);
      }
#endif /* TREETRACE_ENABLED */
      expr->u.i_value = (expr->op==TK_NOTNULL);
      expr->flags |= EP_IntValue;
      expr->op = TK_INTEGER;
      for(i=0, p=nc; p && i<ARRAY_SIZE(an_ref); p=p->next, i++){
        p->n_ref = an_ref[i];
      }
      __expr_delete(parser->db, expr->left);
      expr->left = 0;
      return WRC_Prune;
    }

    /* A column name:                    ID
    ** Or table name and column name:    ID.ID
    ** Or a database, table and column:  ID.ID.ID
    **
    ** The TK_ID and TK_OUT cases are combined so that there will only
    ** be one call to __lookup_name().  Then the compiler will in-line
    ** __lookup_name() for a size reduction and performance increase.
    */
    case TK_ID:
    case TK_DOT: {
      const char *table;
      const char *db;
      expr_t *right;

      if( expr->op==TK_ID ){
        db = 0;
        table = 0;
        assert( !EXPR_HAS_PROPERTY(expr, EP_IntValue) );
        right = expr;
      }else{
        expr_t *left = expr->left;
        testcase( nc->nc_flags & NC_IdxExpr );
        testcase( nc->nc_flags & NC_GenCol );
        __resolve_not_valid(parser, nc, "the \".\" operator",
                               NC_IdxExpr|NC_GenCol, 0, expr);
        right = expr->right;
        if( right->op==TK_ID ){
          db = 0;
        }else{
          assert( right->op==TK_DOT );
          assert( !EXPR_HAS_PROPERTY(right, EP_IntValue) );
          db = left->u.token;
          left = right->left;
          right = right->right;
        }
        assert( EXPR_USE_U_TOKEN(left) && EXPR_USE_U_TOKEN(right) );
        table = left->u.token;
        assert( EXPR_USE_Y_TAB(expr) );
        if( IN_RENAME_OBJECT ){
          __rename_token_remap(parser, (void*)expr, (void*)right);
          __rename_token_remap(parser, (void*)&expr->y.tab, (void*)left);
        }
      }
      return __lookup_name(parser, db, table, right, nc, expr);
    }

    /* Resolve function names
    */
    case TK_FUNCTION: {
      expr_list_t *list;            /* The argument list */
      int n;                      /* Number of arguments */
      int no_such_func = 0;       /* True if no such function exists */
      int wrong_num_args = 0;     /* True if wrong number of arguments */
      int is_agg = 0;             /* True if is an aggregate function */
      const char *id;            /* The function name. */
      func_def_t *def;              /* Information about the function */
      u_int8_t enc = ENC(parser->db);   /* The database encoding */
      int saved_allow_flags = (nc->nc_flags & (NC_AllowAgg | NC_AllowWin));
#ifndef DBSQL_OMIT_WINDOWFUNC
      window_t *win = (IS_WINDOW_FUNC(expr) ? expr->y.win : 0);
#endif
      assert( !EXPR_HAS_PROPERTY(expr, EP_xIsSelect|EP_IntValue) );
      assert( expr->left==0 || expr->left->op==TK_ORDER );
      list = expr->x.list;
      n = list ? list->n_expr : 0;
      id = expr->u.token;
      def = __find_function(parser->db, id, n, enc, 0);
      if( def==0 ){
        def = __find_function(parser->db, id, -2, enc, 0);
        if( def==0 ){
          no_such_func = 1;
        }else{
          wrong_num_args = 1;
        }
      }else{
        is_agg = def->finalize!=0;
        if( def->func_flags & DBSQL_FUNC_UNLIKELY ){
          EXPR_SET_PROPERTY(expr, EP_Unlikely);
          if( n==2 ){
            expr->i_table = __expr_probability(list->a[1].expr);
            if( expr->i_table<0 ){
              __error_msg(parser,
                "second argument to %#T() must be a "
                "constant between 0.0 and 1.0", expr);
              nc->n_nc_err++;
            }
          }else{
            /* EVIDENCE-OF: R-61304-29449 The unlikely(X) function is
            ** equivalent to likelihood(X, 0.0625).
            ** EVIDENCE-OF: R-01283-11636 The unlikely(X) function is
            ** short-hand for likelihood(X,0.0625).
            ** EVIDENCE-OF: R-36850-34127 The likely(X) function is short-hand
            ** for likelihood(X,0.9375).
            ** EVIDENCE-OF: R-53436-40973 The likely(X) function is equivalent
            ** to likelihood(X,0.9375). */
            /* TUNING: unlikely() probability is 0.0625.  likely() is 0.9375 */
            expr->i_table = def->name[0]=='u' ? 8388608 : 125829120;
          }            
        }
#ifndef DBSQL_OMIT_AUTHORIZATION
        {
          int auth = __auth_check(parser, DBSQL_FUNCTION, 0,def->name,0);
          if( auth!=DBSQL_OK ){
            if( auth==DBSQL_DENY ){
              __error_msg(parser, "not authorized to use function: %#T",
                                      expr);
              nc->n_nc_err++;
            }
            expr->op = TK_NULL;
            return WRC_Prune;
          }
        }
#endif

        /* If the function may call dbsql_value_subtype(), then set the 
        ** EP_SubtArg flag on all of its argument expressions. This prevents
        ** where.c from replacing the expression with a value read from an
        ** index on the same expression, which will not have the correct 
        ** subtype. Also set the flag if the function expression itself is
        ** an EP_SubtArg expression. In this case subtypes are required as 
        ** the function may return a value with a subtype back to its 
        ** caller using dbsql_result_value().  */
        if( (def->func_flags & DBSQL_SUBTYPE) 
         || EXPR_HAS_PROPERTY(expr, EP_SubtArg) 
        ){
          __resolve_set_expr_subtype_arg(list);
        }

        if( def->func_flags & (DBSQL_FUNC_CONSTANT|DBSQL_FUNC_SLOCHNG) ){
          /* For the purposes of the EP_ConstFunc flag, date and time
          ** functions and other functions that change slowly are considered
          ** constant because they are constant for the duration of one query.
          ** This allows them to be factored out of inner loops. */
          EXPR_SET_PROPERTY(expr,EP_ConstFunc);
        }
        if( (def->func_flags & DBSQL_FUNC_CONSTANT)==0 ){
          /* Clearly non-deterministic functions like random(), but also
          ** date/time functions that use 'now', and other functions like
          ** dbsql_version() that might change over time cannot be used
          ** in an index or generated column.  Curiously, they can be used
          ** in a CHECK constraint.  SQLServer, __my_sql, and __postgre_sql all
          ** allow this. */
          __resolve_not_valid(parser, nc, "non-deterministic functions",
                                 NC_IdxExpr|NC_PartIdx|NC_GenCol, 0, expr);
        }else{
          assert( (NC_SelfRef & 0xff)==NC_SelfRef ); /* Must fit in 8 bits */
          expr->op2 = nc->nc_flags & NC_SelfRef;
        }
        if( (def->func_flags & DBSQL_FUNC_INTERNAL)!=0
         && parser->nested==0
         && (parser->db->m_db_flags & DBFLAG_InternalFunc)==0
        ){
          /* Internal-use-only functions are disallowed unless the
          ** SQL is being compiled using __nested_parse() or
          ** the DBSQL_TESTCTRL_INTERNAL_FUNCTIONS test-control has be
          ** used to activate internal functions for testing purposes */
          no_such_func = 1;
          def = 0;
        }else
        if( (def->func_flags & (DBSQL_FUNC_DIRECT|DBSQL_FUNC_UNSAFE))!=0
         && !IN_RENAME_OBJECT
        ){
          if( nc->nc_flags & NC_FromDDL ) EXPR_SET_PROPERTY(expr, EP_FromDDL);
          __expr_function_usable(parser, expr, def);
        }
      }

      if( 0==IN_RENAME_OBJECT ){
#ifndef DBSQL_OMIT_WINDOWFUNC
        assert( is_agg==0 || (def->func_flags & DBSQL_FUNC_MINMAX)
          || (def->value==0 && def->inverse==0)
          || (def->value && def->inverse && def->s_func && def->finalize)
        );
        if( def && def->value==0 && win ){
          __error_msg(parser,
              "%#T() may not be used as a window function", expr
          );
          nc->n_nc_err++;
        }else if(
              (is_agg && (nc->nc_flags & NC_AllowAgg)==0)
           || (is_agg && (def->func_flags&DBSQL_FUNC_WINDOW) && !win)
           || (is_agg && win && (nc->nc_flags & NC_AllowWin)==0)
        ){
          const char *type;
          if( (def->func_flags & DBSQL_FUNC_WINDOW) || win ){
            type = "window";
          }else{
            type = "aggregate";
          }
          __error_msg(parser, "misuse of %s function %#T()",type,expr);
          nc->n_nc_err++;
          is_agg = 0;
        }
#else
        if( (is_agg && (nc->nc_flags & NC_AllowAgg)==0) ){
          __error_msg(parser,"misuse of aggregate function %#T()",expr);
          nc->n_nc_err++;
          is_agg = 0;
        }
#endif
        else if( no_such_func && parser->db->init.busy==0
#ifdef DBSQL_ENABLE_UNKNOWN_SQL_FUNCTION
                  && parser->explain==0
#endif
        ){
          __error_msg(parser, "no such function: %#T", expr);
          nc->n_nc_err++;
        }else if( wrong_num_args ){
          __error_msg(parser,"wrong number of arguments to function %#T()",
               expr);
          nc->n_nc_err++;
        }
#ifndef DBSQL_OMIT_WINDOWFUNC
        else if( is_agg==0 && EXPR_HAS_PROPERTY(expr, EP_WinFunc) ){
          __error_msg(parser,
              "FILTER may not be used with non-aggregate %#T()",
              expr
          );
          nc->n_nc_err++;
        }
#endif
        else if( is_agg==0 && expr->left ){
          __expr_order_by_aggregate_error(parser, expr);
          nc->n_nc_err++;
        }
        if( is_agg ){
          /* window_t functions may not be arguments of aggregate functions.
          ** Or arguments of other window functions. But aggregate functions
          ** may be arguments for window functions.  */
#ifndef DBSQL_OMIT_WINDOWFUNC
          nc->nc_flags &= ~(NC_AllowWin | (!win ? NC_AllowAgg : 0));
#else
          nc->nc_flags &= ~NC_AllowAgg;
#endif
        }
      }
      else if( EXPR_HAS_PROPERTY(expr, EP_WinFunc) || expr->left ){
        is_agg = 1;
      }
      __walk_expr_list(walker, list);
      if( is_agg ){
        if( expr->left ){
          assert( expr->left->op==TK_ORDER );
          assert( EXPR_USE_X_LIST(expr->left) );
          __walk_expr_list(walker, expr->left->x.list);
        }
#ifndef DBSQL_OMIT_WINDOWFUNC
        if( win && parser->n_err==0 ){
          select_t *sel = nc->win_select;
          assert( EXPR_USE_Y_WIN(expr) && win==expr->y.win );
          if( IN_RENAME_OBJECT==0 ){
            __window_update(parser, sel ? sel->win_defn : 0, win, def);
            if( parser->db->malloc_failed ) break;
          }
          __walk_expr_list(walker, win->partition);
          __walk_expr_list(walker, win->order_by);
          __walk_expr(walker, win->filter);
          __window_link(sel, win);
          nc->nc_flags |= NC_HasWin;
        }else
#endif /* DBSQL_OMIT_WINDOWFUNC */
        {
          name_context_t *nc2;          /* For looping up thru outer contexts */
          expr->op = TK_AGG_FUNCTION;
          expr->op2 = 0;
#ifndef DBSQL_OMIT_WINDOWFUNC
          if( EXPR_HAS_PROPERTY(expr, EP_WinFunc) ){
            __walk_expr(walker, expr->y.win->filter);
          }
#endif
          nc2 = nc;
          while( nc2
              && __references_src_list(parser, expr, nc2->src_list)==0
          ){
            expr->op2 += (1 + nc2->n_nested_select);
            nc2 = nc2->next;
          }
          assert( def!=0 || IN_RENAME_OBJECT );
          if( nc2 && def ){
            expr->op2 += nc2->n_nested_select;
            assert( DBSQL_FUNC_MINMAX==NC_MinMaxAgg );
            assert( DBSQL_FUNC_ANYORDER==NC_OrderAgg );
            testcase( (def->func_flags & DBSQL_FUNC_MINMAX)!=0 );
            testcase( (def->func_flags & DBSQL_FUNC_ANYORDER)!=0 );
            nc2->nc_flags |= NC_HasAgg
              | ((def->func_flags^DBSQL_FUNC_ANYORDER)
                  & (DBSQL_FUNC_MINMAX|DBSQL_FUNC_ANYORDER));
          }
        }
        nc->nc_flags |= saved_allow_flags;
      }
      /* FIX ME:  Compute expr->affinity based on the expected return
      ** type of the function
      */
      return WRC_Prune;
    }
#ifndef DBSQL_OMIT_SUBQUERY
    case TK_EXISTS:
    case TK_SELECT:
#endif
    case TK_IN: {
      testcase( expr->op==TK_IN );
      testcase( expr->op==TK_EXISTS );
      testcase( expr->op==TK_SELECT );
      if( EXPR_USE_X_SELECT(expr) ){
        int n_ref = nc->n_ref;
        testcase( nc->nc_flags & NC_IsCheck );
        testcase( nc->nc_flags & NC_PartIdx );
        testcase( nc->nc_flags & NC_IdxExpr );
        testcase( nc->nc_flags & NC_GenCol );
        assert( expr->x.p_select );
        if( expr->op==TK_EXISTS )  parser->has_exists = 1;
        if( nc->nc_flags & NC_SelfRef ){
          __not_valid_impl(parser, nc, "subqueries", expr, expr);
        }else{
          __walk_select(walker, expr->x.p_select);
        }
        assert( nc->n_ref>=n_ref );
        if( n_ref!=nc->n_ref ){
          EXPR_SET_PROPERTY(expr, EP_VarSelect);
          expr->x.p_select->sel_flags |= SF_Correlated;
        }
        nc->nc_flags |= NC_Subquery;
      }
      break;
    }
    case TK_VARIABLE: {
      testcase( nc->nc_flags & NC_IsCheck );
      testcase( nc->nc_flags & NC_PartIdx );
      testcase( nc->nc_flags & NC_IdxExpr );
      testcase( nc->nc_flags & NC_GenCol );
      __resolve_not_valid(parser, nc, "parameters",
               NC_IsCheck|NC_PartIdx|NC_IdxExpr|NC_GenCol, expr, expr);
      break;
    }
    case TK_IS:
    case TK_ISNOT: {
      expr_t *right = __expr_skip_collate_and_likely(expr->right);
      assert( !EXPR_HAS_PROPERTY(expr, EP_Reduced) );
      /* Handle special cases of "x IS TRUE", "x IS FALSE", "x IS NOT TRUE",
      ** and "x IS NOT FALSE". */
      if( ALWAYS(right) && (right->op==TK_ID || right->op==TK_TRUEFALSE) ){
        int rc = __resolve_expr_step(walker, right);
        if( rc==WRC_Abort ) return WRC_Abort;
        if( right->op==TK_TRUEFALSE ){
          expr->op2 = expr->op;
          expr->op = TK_TRUTH;
          return WRC_Continue;
        }
      }
      /* no break */ deliberate_fall_through
    }
    case TK_BETWEEN:
    case TK_EQ:
    case TK_NE:
    case TK_LT:
    case TK_LE:
    case TK_GT:
    case TK_GE: {
      int n_left, n_right;
      if( parser->db->malloc_failed ) break;
      assert( expr->left!=0 );
      n_left = __expr_vector_size(expr->left);
      if( expr->op==TK_BETWEEN ){
        assert( EXPR_USE_X_LIST(expr) );
        n_right = __expr_vector_size(expr->x.list->a[0].expr);
        if( n_right==n_left ){
          n_right = __expr_vector_size(expr->x.list->a[1].expr);
        }
      }else{
        assert( expr->right!=0 );
        n_right = __expr_vector_size(expr->right);
      }
      if( n_left!=n_right ){
        testcase( expr->op==TK_EQ );
        testcase( expr->op==TK_NE );
        testcase( expr->op==TK_LT );
        testcase( expr->op==TK_LE );
        testcase( expr->op==TK_GT );
        testcase( expr->op==TK_GE );
        testcase( expr->op==TK_IS );
        testcase( expr->op==TK_ISNOT );
        testcase( expr->op==TK_BETWEEN );
        __error_msg(parser, "row value misused");
        __record_error_offset_of_expr(parser->db, expr);
      }
      break;
    }
  }
  assert( parser->db->malloc_failed==0 || parser->n_err!=0 );
  return parser->n_err ? WRC_Abort : WRC_Continue;
}

/*
** e_list is a list of expressions which are really the result set of the
** a SELECT statement.  e is a term in an ORDER BY or GROUP BY clause.
** This routine checks to see if e is a simple identifier which corresponds
** to the AS-name of one of the terms of the expression list.  If it is,
** this routine return an integer between 1 and N where N is the number of
** elements in e_list, corresponding to the matching entry.  If there is
** no match, or if e is not a simple identifier, then this routine
** return 0.
**
** e_list has been resolved.  e has not.
*/
static int __resolve_as_name(
  parser_t *parser,     /* Parsing context for error messages */
  expr_list_t *e_list,  /* List of expressions to scan */
  expr_t *e           /* Expression we are trying to match */
){
  int i;             /* Loop counter */

  UNUSED_PARAMETER(parser);

  if( e->op==TK_ID ){
    const char *col;
    assert( !EXPR_HAS_PROPERTY(e, EP_IntValue) );
    col = e->u.token;
    for(i=0; i<e_list->n_expr; i++){
      if( e_list->a[i].fg.e_name==ENAME_NAME
       && dbsql_stricmp(e_list->a[i].e_name, col)==0
      ){
        return i+1;
      }
    }
  }
  return 0;
}

/*
** e is a pointer to an expression which is a single term in the
** ORDER BY of a compound SELECT.  The expression has not been
** name resolved.
**
** At the point this routine is called, we already know that the
** ORDER BY term is not an integer index into the result set.  That
** case is handled by the calling routine.
**
** Attempt to match e against result set columns in the left-most
** SELECT statement.  Return the index i of the matching column,
** as an indication to the caller that it should sort by the i-th column.
** The left-most column is 1.  In other words, the value returned is the
** same integer value that would be used in the SQL statement to indicate
** the column.
**
** If there is no match, return 0.  Return -1 if an error occurs.
*/
static int __resolve_order_by_term_to_expr_list(
  parser_t *parser,     /* Parsing context for error messages */
  select_t *p_select,   /* The SELECT statement with the ORDER BY clause */
  expr_t *e           /* The specific ORDER BY term */
){
  int i;             /* Loop counter */
  expr_list_t *e_list;  /* The columns of the result set */
  name_context_t nc;    /* Name context for resolving e */
  DBSQL *db;       /* Database connection */
  int rc;            /* Return code from subprocedures */
  u_int8_t saved_supp_err;   /* Saved value of db->suppress_err */

  assert( __expr_is_integer(e, &i, 0)==0 );
  e_list = p_select->e_list;

  /* Resolve all names in the ORDER BY term expression
  */
  memset(&nc, 0, sizeof(nc));
  nc.parser = parser;
  nc.src_list = p_select->src;
  nc.u_nc.e_list = e_list;
  nc.nc_flags = NC_AllowAgg|NC_UEList|NC_NoSelect;
  nc.n_nc_err = 0;
  db = parser->db;
  saved_supp_err = db->suppress_err;
  db->suppress_err = 1;
  rc = __resolve_expr_names(&nc, e);
  db->suppress_err = saved_supp_err;
  if( rc ) return 0;

  /* Try to match the ORDER BY expression against an expression
  ** in the result set.  Return an 1-based index of the matching
  ** result-set entry.
  */
  for(i=0; i<e_list->n_expr; i++){
    if( __expr_compare(0, e_list->a[i].expr, e, -1)<2 ){
      return i+1;
    }
  }

  /* If no match, return 0. */
  return 0;
}

/*
** Generate an ORDER BY or GROUP BY term out-of-range error.
*/
static void __resolve_out_of_range_error(
  parser_t *parser,         /* The error context into which to write the error */
  const char *type,     /* "ORDER" or "GROUP" */
  int i,                 /* The index (1-based) of the term out of range */
  int mx,                /* Largest permissible value of i */
  expr_t *error           /* Associate the error with the expression */
){
  __error_msg(parser,
    "%r %s BY term out of range - should be "
    "between 1 and %d", i, type, mx);
  __record_error_offset_of_expr(parser->db, error);
}

/*
** Analyze the ORDER BY clause in a compound SELECT statement.   Modify
** each term of the ORDER BY clause is a constant integer between 1
** and N where N is the number of columns in the compound SELECT.
**
** ORDER BY terms that are already an integer between 1 and N are
** unmodified.  ORDER BY terms that are integers outside the range of
** 1 through N generate an error.  ORDER BY terms that are expressions
** are matched against result set expressions of compound SELECT
** beginning with the left-most SELECT and working toward the right.
** At the first match, the ORDER BY expression is transformed into
** the integer column number.
**
** Return the number of errors seen.
*/
static int __resolve_compound_order_by(
  parser_t *parser,        /* Parsing context.  Leave error messages here */
  select_t *p_select       /* The SELECT statement containing the ORDER BY */
){
  int i;
  expr_list_t *order_by;
  expr_list_t *e_list;
  DBSQL *db;
  int more_to_do = 1;

  order_by = p_select->order_by;
  if( order_by==0 ) return 0;
  db = parser->db;
  if( order_by->n_expr>db->limits[DBSQL_LIMIT_COLUMN] ){
    __error_msg(parser, "too many terms in ORDER BY clause");
    return 1;
  }
  for(i=0; i<order_by->n_expr; i++){
    order_by->a[i].fg.done = 0;
  }
  p_select->next = 0;
  while( p_select->prior ){
    p_select->prior->next = p_select;
    p_select = p_select->prior;
  }
  while( p_select && more_to_do ){
    struct expr_list_item_t *item;
    more_to_do = 0;
    e_list = p_select->e_list;
    assert( e_list!=0 );
    for(i=0, item=order_by->a; i<order_by->n_expr; i++, item++){
      int i_col = -1;
      expr_t *e, *dup;
      if( item->fg.done ) continue;
      e = __expr_skip_collate_and_likely(item->expr);
      if( NEVER(e==0) ) continue;
      if( __expr_is_integer(e, &i_col, 0) ){
        if( i_col<=0 || i_col>e_list->n_expr ){
          __resolve_out_of_range_error(parser, "ORDER", i+1, e_list->n_expr, e);
          return 1;
        }
      }else{
        i_col = __resolve_as_name(parser, e_list, e);
        if( i_col==0 ){
          /* Now test if expression e matches one of the values returned
          ** by p_select. In the usual case this is done by duplicating the
          ** expression, resolving any symbols in it, and then comparing
          ** it against each expression returned by the SELECT statement.
          ** Once the comparisons are finished, the duplicate expression
          ** is deleted.
          **
          ** If this is running as part of an ALTER TABLE operation and
          ** the symbols resolve successfully, also resolve the symbols in the
          ** actual expression. This allows the code in alter.c to modify
          ** column references within the ORDER BY expression as required.  */
          dup = __expr_dup(db, e, 0);
          if( !db->malloc_failed ){
            assert(dup);
            i_col = __resolve_order_by_term_to_expr_list(parser, p_select, dup);
            if( IN_RENAME_OBJECT && i_col>0 ){
              __resolve_order_by_term_to_expr_list(parser, p_select, e);
            }
          }
          __expr_delete(db, dup);
        }
      }
      if( i_col>0 ){
        /* Convert the ORDER BY term into an integer column number i_col,
        ** taking care to preserve the COLLATE clause if it exists. */
        if( !IN_RENAME_OBJECT ){
          expr_t *p_new = __expr_int32(db, i_col);
          if( p_new==0 ) return 1;
          if( item->expr==e ){
            item->expr = p_new;
          }else{
            expr_t *parent = item->expr;
            assert( parent->op==TK_COLLATE );
            while( parent->left->op==TK_COLLATE ) parent = parent->left;
            assert( parent->left==e );
            parent->left = p_new;
          }
          __expr_delete(db, e);
          item->u.x.i_order_by_col = (u_int16_t)i_col;
        }
        item->fg.done = 1;
      }else{
        more_to_do = 1;
      }
    }
    p_select = p_select->next;
  }
  for(i=0; i<order_by->n_expr; i++){
    if( order_by->a[i].fg.done==0 ){
      __error_msg(parser, "%r ORDER BY term does not match any "
            "column in the result set", i+1);
      return 1;
    }
  }
  return 0;
}

/*
** Check every term in the ORDER BY or GROUP BY clause order_by of
** the SELECT statement p_select.  If any term is reference to a
** result set expression (as determined by the expr_list_t.a.u.x.i_order_by_col
** field) then convert that term into a copy of the corresponding result set
** column.
**
** If any errors are detected, add an error message to parser and
** return non-zero.  Return zero if no errors are seen.
*/
int __resolve_order_group_by(
  parser_t *parser,        /* Parsing context.  Leave error messages here */
  select_t *p_select,      /* The SELECT statement containing the clause */
  expr_list_t *order_by,   /* The ORDER BY or GROUP BY clause to be processed */
  const char *type     /* "ORDER" or "GROUP" */
){
  int i;
  DBSQL *db = parser->db;
  expr_list_t *e_list;
  struct expr_list_item_t *item;

  if( order_by==0 || parser->db->malloc_failed || IN_RENAME_OBJECT ) return 0;
  if( order_by->n_expr>db->limits[DBSQL_LIMIT_COLUMN] ){
    __error_msg(parser, "too many terms in %s BY clause", type);
    return 1;
  }
  e_list = p_select->e_list;
  assert( e_list!=0 );  /* __select_new() guarantees this */
  for(i=0, item=order_by->a; i<order_by->n_expr; i++, item++){
    if( item->u.x.i_order_by_col ){
      if( item->u.x.i_order_by_col>e_list->n_expr ){
        __resolve_out_of_range_error(parser, type, i+1, e_list->n_expr, 0);
        return 1;
      }
      __resolve_alias(parser, e_list, item->u.x.i_order_by_col-1, item->expr,0);
    }
  }
  return 0;
}

#ifndef DBSQL_OMIT_WINDOWFUNC
/*
** walker_t callback for WINDOW_REMOVE_EXPR_FROM_SELECT().
*/
static int __resolve_remove_windows_cb(walker_t *walker, expr_t *expr){
  UNUSED_PARAMETER(walker);
  if( EXPR_HAS_PROPERTY(expr, EP_WinFunc) ){
    window_t *win = expr->y.win;
    __window_unlink_from_select(win);
  }
  return WRC_Continue;
}

/*
** Remove any window_t objects owned by the expression expr from the
** select_t.win list of select_t object p_select.
*/
static void WINDOW_REMOVE_EXPR_FROM_SELECT(select_t *p_select, expr_t *expr){
  if( p_select->win ){
    walker_t s_walker;
    memset(&s_walker, 0, sizeof(walker_t));
    s_walker.expr_callback = __resolve_remove_windows_cb;
    s_walker.u.p_select = p_select;
    __walk_expr(&s_walker, expr);
  }
}
#else
# define WINDOW_REMOVE_EXPR_FROM_SELECT(a, b)
#endif /* DBSQL_OMIT_WINDOWFUNC */

/*
** order_by is an ORDER BY or GROUP BY clause in SELECT statement p_select.
** The Name context of the SELECT statement is nc.  type is either
** "ORDER" or "GROUP" depending on which type of clause order_by is.
**
** This routine resolves each term of the clause into an expression.
** If the order-by term is an integer I between 1 and N (where N is the
** number of columns in the result set of the SELECT) then the expression
** in the resolution is a copy of the I-th result-set expression.  If
** the order-by term is an identifier that corresponds to the AS-name of
** a result-set expression, then the term resolves to a copy of the
** result-set expression.  Otherwise, the expression is resolved in
** the usual way - using __resolve_expr_names().
**
** This routine returns the number of errors.  If errors occur, then
** an appropriate error message might be left in parser.  (OOM errors
** excepted.)
*/
static int __resolve_order_group_by_local(
  name_context_t *nc,     /* The name context of the SELECT statement */
  select_t *p_select,      /* The SELECT statement holding order_by */
  expr_list_t *order_by,   /* An ORDER BY or GROUP BY clause to resolve */
  const char *type     /* Either "ORDER" or "GROUP", as appropriate */
){
  int i, j;                      /* Loop counters */
  int i_col;                      /* column_t number */
  struct expr_list_item_t *item;   /* A term of the ORDER BY clause */
  parser_t *parser;                 /* Parsing context */
  int n_result;                   /* Number of terms in the result set */

  assert( order_by!=0 );
  n_result = p_select->e_list->n_expr;
  parser = nc->parser;
  for(i=0, item=order_by->a; i<order_by->n_expr; i++, item++){
    expr_t *e = item->expr;
    expr_t *e2 = __expr_skip_collate_and_likely(e);
    if( NEVER(e2==0) ) continue;
    if( type[0]!='G' ){
      i_col = __resolve_as_name(parser, p_select->e_list, e2);
      if( i_col>0 ){
        /* If an AS-name match is found, mark this ORDER BY column as being
        ** a copy of the i_col-th result-set column.  The subsequent call to
        ** __resolve_order_group_by() will convert the expression to a
        ** copy of the i_col-th result-set expression. */
        item->u.x.i_order_by_col = (u_int16_t)i_col;
        continue;
      }
    }
    if( __expr_is_integer(e2, &i_col, 0) ){
      /* The ORDER BY term is an integer constant.  Again, set the column
      ** number so that __resolve_order_group_by() will convert the
      ** order-by term to a copy of the result-set expression */
      if( i_col<1 || i_col>0xffff ){
        __resolve_out_of_range_error(parser, type, i+1, n_result, e2);
        return 1;
      }
      item->u.x.i_order_by_col = (u_int16_t)i_col;
      continue;
    }

    /* Otherwise, treat the ORDER BY term as an ordinary expression */
    item->u.x.i_order_by_col = 0;
    if( __resolve_expr_names(nc, e) ){
      return 1;
    }
    for(j=0; j<p_select->e_list->n_expr; j++){
      if( __expr_compare(0, e, p_select->e_list->a[j].expr, -1)==0 ){
        /* Since this expression is being changed into a reference
        ** to an identical expression in the result set, remove all window_t
        ** objects belonging to the expression from the select_t.win list. */
        WINDOW_REMOVE_EXPR_FROM_SELECT(p_select, e);
        item->u.x.i_order_by_col = j+1;
      }
    }
  }
  return __resolve_order_group_by(parser, p_select, order_by, type);
}

/*
** Resolve names in the SELECT statement p and all of its descendants.
*/
static int __resolve_select_step(walker_t *walker, select_t *p){
  name_context_t *outer_nc;  /* Context that contains this SELECT */
  name_context_t s_nc;        /* Name context of this SELECT */
  int is_compound;         /* True if p is a compound select */
  int n_compound;          /* Number of compound terms processed so far */
  parser_t *parser;          /* Parsing context */
  int i;                  /* Loop counter */
  expr_list_t *group_by;     /* The GROUP BY clause */
  select_t *leftmost;      /* Left-most of SELECT of a compound */
  DBSQL *db;            /* Database connection */
 

  assert( p!=0 );
  if( p->sel_flags & SF_Resolved ){
    return WRC_Prune;
  }
  outer_nc = walker->u.nc;
  parser = walker->parser;
  db = parser->db;

  /* Normally __select_expand() will be called first and will have
  ** already expanded this SELECT.  However, if this is a subquery within
  ** an expression, __resolve_expr_names() will be called without a
  ** prior call to __select_expand().  When that happens, let
  ** __select_prep() do all of the processing for this SELECT.
  ** __select_prep() will invoke both __select_expand() and
  ** this routine in the correct order.
  */
  if( (p->sel_flags & SF_Expanded)==0 ){
    __select_prep(parser, p, outer_nc);
    return parser->n_err ? WRC_Abort : WRC_Prune;
  }

  is_compound = p->prior!=0;
  n_compound = 0;
  leftmost = p;
  while( p ){
    assert( (p->sel_flags & SF_Expanded)!=0 );
    assert( (p->sel_flags & SF_Resolved)==0 );
    p->sel_flags |= SF_Resolved;

    /* Resolve the expressions in the LIMIT and OFFSET clauses. These
    ** are not allowed to refer to any names, so pass an empty name_context_t.
    */
    memset(&s_nc, 0, sizeof(s_nc));
    s_nc.parser = parser;
    s_nc.win_select = p;
    if( __resolve_expr_names(&s_nc, p->limit) ){
      return WRC_Abort;
    }

    /* If the SF_Converted flags is set, then this select_t object was
    ** was created by the __convert_compound_select_to_subquery() function.
    ** In this case the ORDER BY clause (p->order_by) should be resolved
    ** as if it were part of the sub-query, not the parent. This block
    ** moves the order_by down to the sub-query. It will be moved back
    ** after the names have been resolved.  */
    if( p->sel_flags & SF_Converted ){
      select_t *sub;
      assert( p->src->a[0].fg.is_subquery );
      assert( p->src->a[0].u4.subq!=0 );
      sub = p->src->a[0].u4.subq->p_select;
      assert( sub!=0 );
      assert( p->src->n_src==1 && p->order_by );
      assert( sub->prior && sub->order_by==0 );
      sub->order_by = p->order_by;
      p->order_by = 0;
    }
 
    /* Recursively resolve names in all subqueries in the FROM clause
    */
    if( outer_nc ) outer_nc->n_nested_select++;
    for(i=0; i<p->src->n_src; i++){
      src_item_t *item = &p->src->a[i];
      assert( item->name!=0
              || item->fg.is_subquery );  /* Test of tag-20240424-1*/
      if( item->fg.is_subquery
       && (item->u4.subq->p_select->sel_flags & SF_Resolved)==0
      ){
        int n_ref = outer_nc ? outer_nc->n_ref : 0;
        const char *saved_context = parser->auth_context;

        if( item->name ) parser->auth_context = item->name;
        __resolve_select_names(parser, item->u4.subq->p_select, outer_nc);
        parser->auth_context = saved_context;
        if( parser->n_err ) return WRC_Abort;
        assert( db->malloc_failed==0 );

        /* If the number of references to the outer context changed when
        ** expressions in the sub-select were resolved, the sub-select
        ** is correlated. It is not required to check the refcount on any
        ** but the innermost outer context object, as __lookup_name() increments
        ** the refcount on all contexts between the current one and the
        ** context containing the column when it resolves a name. */
        if( outer_nc ){
          assert( item->fg.is_correlated==0 && outer_nc->n_ref>=n_ref );
          item->fg.is_correlated = (outer_nc->n_ref>n_ref);
        }
      }
    }
    if( outer_nc && ALWAYS(outer_nc->n_nested_select>0) ){
      outer_nc->n_nested_select--;
    }
 
    /* Set up the local name-context to pass to __resolve_expr_names() to
    ** resolve the result-set expression list.
    */
    s_nc.nc_flags = NC_AllowAgg|NC_AllowWin;
    s_nc.src_list = p->src;
    s_nc.next = outer_nc;
 
    /* Resolve names in the result set. */
    if( __resolve_expr_list_names(&s_nc, p->e_list) ) return WRC_Abort;
    s_nc.nc_flags &= ~NC_AllowWin;
 
    /* If there are no aggregate functions in the result-set, and no GROUP BY
    ** expression, do not allow aggregates in any of the other expressions.
    */
    assert( (p->sel_flags & SF_Aggregate)==0 );
    group_by = p->group_by;
    if( group_by || (s_nc.nc_flags & NC_HasAgg)!=0 ){
      assert( NC_MinMaxAgg==SF_MinMaxAgg );
      assert( NC_OrderAgg==SF_OrderByReqd );
      p->sel_flags |= SF_Aggregate | (s_nc.nc_flags&(NC_MinMaxAgg|NC_OrderAgg));
    }else{
      s_nc.nc_flags &= ~NC_AllowAgg;
    }
 
    /* Add the output column list to the name-context before parsing the
    ** other expressions in the SELECT statement. This is so that
    ** expressions in the WHERE clause (etc.) can refer to expressions by
    ** aliases in the result set.
    **
    ** Minor point: If this is the case, then the expression will be
    ** re-evaluated for each reference to it.
    */
    assert( (s_nc.nc_flags & (NC_UAggInfo|NC_UUpsert|NC_UBaseReg))==0 );
    s_nc.u_nc.e_list = p->e_list;
    s_nc.nc_flags |= NC_UEList;
    if( p->having ){
      if( (p->sel_flags & SF_Aggregate)==0 ){
        __error_msg(parser, "HAVING clause on a non-aggregate query");
        return WRC_Abort;
      }
      if( __resolve_expr_names(&s_nc, p->having) ) return WRC_Abort;
    }
    s_nc.nc_flags |= NC_Where;
    if( __resolve_expr_names(&s_nc, p->where) ) return WRC_Abort;
    s_nc.nc_flags &= ~NC_Where;

    /* Resolve names in table-valued-function arguments */
    for(i=0; i<p->src->n_src; i++){
      src_item_t *item = &p->src->a[i];
      if( item->fg.is_tab_func
       && __resolve_expr_list_names(&s_nc, item->u1.func_arg)
      ){
        return WRC_Abort;
      }
    }

#ifndef DBSQL_OMIT_WINDOWFUNC
    if( IN_RENAME_OBJECT ){
      window_t *win;
      for(win=p->win_defn; win; win=win->next_win){
        if( __resolve_expr_list_names(&s_nc, win->order_by)
         || __resolve_expr_list_names(&s_nc, win->partition)
        ){
          return WRC_Abort;
        }
      }
    }
#endif

    s_nc.nc_flags |= NC_AllowAgg|NC_AllowWin;

    /* If this is a converted compound query, move the ORDER BY clause from
    ** the sub-query back to the parent query. At this point each term
    ** within the ORDER BY clause has been transformed to an integer value.
    ** These integers will be replaced by copies of the corresponding result
    ** set expressions by the call to __resolve_order_group_by_local() below.  */
    if( p->sel_flags & SF_Converted ){
      select_t *sub;
      assert( p->src->a[0].fg.is_subquery );
      sub = p->src->a[0].u4.subq->p_select;
      assert( sub!=0 );
      p->order_by = sub->order_by;
      sub->order_by = 0;
    }

    /* Process the ORDER BY clause for singleton SELECT statements.
    ** The ORDER BY clause for compounds SELECT statements is handled
    ** below, after all of the result-sets for all of the elements of
    ** the compound have been resolved.
    **
    ** If there is an ORDER BY clause on a term of a compound-select other
    ** than the right-most term, then that is a syntax error.  But the error
    ** is not detected until much later, and so we need to go ahead and
    ** resolve those symbols on the incorrect ORDER BY for consistency.
    */
    if( p->order_by!=0
     && is_compound<=n_compound  /* Defer right-most ORDER BY of a compound */
     && __resolve_order_group_by_local(&s_nc, p, p->order_by, "ORDER")
    ){
      return WRC_Abort;
    }
    if( db->malloc_failed ){
      return WRC_Abort;
    }
    s_nc.nc_flags &= ~NC_AllowWin;
 
    /* Resolve the GROUP BY clause.  At the same time, make sure
    ** the GROUP BY clause does not contain aggregate functions.
    */
    if( group_by ){
      struct expr_list_item_t *item;
   
      if( __resolve_order_group_by_local(&s_nc, p, group_by, "GROUP") || db->malloc_failed ){
        return WRC_Abort;
      }
      for(i=0, item=group_by->a; i<group_by->n_expr; i++, item++){
        if( EXPR_HAS_PROPERTY(item->expr, EP_Agg) ){
          __error_msg(parser, "aggregate functions are not allowed in "
              "the GROUP BY clause");
          return WRC_Abort;
        }
      }
    }

    /* If this is part of a compound SELECT, check that it has the right
    ** number of expressions in the select list. */
    if( p->next && p->e_list->n_expr!=p->next->e_list->n_expr ){
      __select_wrong_num_terms_error(parser, p->next);
      return WRC_Abort;
    }

    /* If the SELECT statement contains ON clauses that were moved into
    ** the WHERE clause, go through and verify that none of the terms
    ** in the ON clauses reference tables to the right of the ON clause. */
    if( (p->sel_flags & SF_OnToWhere) ){
      __select_check_on_clauses(parser, p);
      if( parser->n_err ) return WRC_Abort;
    }

    /* Advance to the next term of the compound
    */
    p = p->prior;
    n_compound++;
  }

  /* Resolve the ORDER BY on a compound SELECT after all terms of
  ** the compound have been resolved.
  */
  if( is_compound && __resolve_compound_order_by(parser, leftmost) ){
    return WRC_Abort;
  }

  return WRC_Prune;
}

/*
** This routine walks an expression tree and resolves references to
** table columns and result-set columns.  At the same time, do error
** checking on function usage and set a flag if any aggregate functions
** are seen.
**
** To resolve table columns references we look for nodes (or subtrees) of the
** form X.Y.Z or Y.Z or just Z where
**
**      X:   The name of a database.  Ex:  "main" or "temp" or
**           the symbolic name assigned to an ATTACH-ed database.
**
**      Y:   The name of a table in a FROM clause.  Or in a trigger
**           one of the special names "old" or "new".
**
**      Z:   The name of a column in table Y.
**
** The node at the root of the subtree is modified as follows:
**
**    expr_t.op        Changed to TK_COLUMN
**    expr_t.tab      Points to the table_t object for X.Y
**    expr_t.i_column   The column index in X.Y.  -1 for the rowid.
**    expr_t.i_table    The VDBE cursor number for X.Y
**
**
** To resolve result-set references, look for expression nodes of the
** form Z (with no X and Y prefix) where the Z matches the right-hand
** size of an AS clause in the result-set of a SELECT.  The Z expression
** is replaced by a copy of the left-hand side of the result-set expression.
** table_t-name and function resolution occurs on the substituted expression
** tree.  For example, in:
**
**      SELECT a+b AS x, c+d AS y FROM t1 ORDER BY x;
**
** The "x" term of the order by is replaced by "a+b" to render:
**
**      SELECT a+b AS x, c+d AS y FROM t1 ORDER BY a+b;
**
** Function calls are checked to make sure that the function is
** defined and that the correct number of arguments are specified.
** If the function is an aggregate function, then the NC_HasAgg flag is
** set and the opcode is changed from TK_FUNCTION to TK_AGG_FUNCTION.
** If an expression contains aggregate functions then the EP_Agg
** property on the expression is set.
**
** An error message is left in parser if anything is amiss.  The number
** if errors is returned.
*/
int __resolve_expr_names(
  name_context_t *nc,       /* Namespace to resolve expressions in. */
  expr_t *expr             /* The expression to be analyzed. */
){
  int saved_has_agg;
  walker_t w;

  if( expr==0 ) return DBSQL_OK;
  saved_has_agg = nc->nc_flags & (NC_HasAgg|NC_MinMaxAgg|NC_HasWin|NC_OrderAgg);
  nc->nc_flags &= ~(NC_HasAgg|NC_MinMaxAgg|NC_HasWin|NC_OrderAgg);
  w.parser = nc->parser;
  w.expr_callback = __resolve_expr_step;
  w.select_callback = (nc->nc_flags & NC_NoSelect) ? 0 : __resolve_select_step;
  w.select_callback2 = 0;
  w.u.nc = nc;
#if DBSQL_MAX_EXPR_DEPTH>0
  w.parser->n_height += expr->n_height;
  if( __expr_check_height(w.parser, w.parser->n_height) ){
    return DBSQL_ERROR;
  }
#endif
  assert( expr!=0 );
  __walk_expr_nn(&w, expr);
#if DBSQL_MAX_EXPR_DEPTH>0
  w.parser->n_height -= expr->n_height;
#endif
  assert( EP_Agg==NC_HasAgg );
  assert( EP_Win==NC_HasWin );
  testcase( nc->nc_flags & NC_HasAgg );
  testcase( nc->nc_flags & NC_HasWin );
  EXPR_SET_PROPERTY(expr, nc->nc_flags & (NC_HasAgg|NC_HasWin) );
  nc->nc_flags |= saved_has_agg;
  return nc->n_nc_err>0 || w.parser->n_err>0;
}

/*
** Resolve all names for all expression in an expression list.  This is
** just like __resolve_expr_names() except that it works for an expression
** list rather than a single expression.
**
** The return value is DBSQL_OK (0) for success or DBSQL_ERROR (1) for a
** failure.
*/
int __resolve_expr_list_names(
  name_context_t *nc,       /* Namespace to resolve expressions in. */
  expr_list_t *list         /* The expression list to be analyzed. */
){
  int i;
  int saved_has_agg = 0;
  walker_t w;
  if( list==0 ) return DBSQL_OK;
  w.parser = nc->parser;
  w.expr_callback = __resolve_expr_step;
  w.select_callback = __resolve_select_step;
  w.select_callback2 = 0;
  w.u.nc = nc;
  saved_has_agg = nc->nc_flags & (NC_HasAgg|NC_MinMaxAgg|NC_HasWin|NC_OrderAgg);
  nc->nc_flags &= ~(NC_HasAgg|NC_MinMaxAgg|NC_HasWin|NC_OrderAgg);
  for(i=0; i<list->n_expr; i++){
    expr_t *expr = list->a[i].expr;
    if( expr==0 ) continue;
#if DBSQL_MAX_EXPR_DEPTH>0
    w.parser->n_height += expr->n_height;
    if( __expr_check_height(w.parser, w.parser->n_height) ){
      return DBSQL_ERROR;
    }
#endif
    __walk_expr_nn(&w, expr);
#if DBSQL_MAX_EXPR_DEPTH>0
    w.parser->n_height -= expr->n_height;
#endif
    assert( EP_Agg==NC_HasAgg );
    assert( EP_Win==NC_HasWin );
    testcase( nc->nc_flags & NC_HasAgg );
    testcase( nc->nc_flags & NC_HasWin );
    if( nc->nc_flags & (NC_HasAgg|NC_MinMaxAgg|NC_HasWin|NC_OrderAgg) ){
      EXPR_SET_PROPERTY(expr, nc->nc_flags & (NC_HasAgg|NC_HasWin) );
      saved_has_agg |= nc->nc_flags &
                          (NC_HasAgg|NC_MinMaxAgg|NC_HasWin|NC_OrderAgg);
      nc->nc_flags &= ~(NC_HasAgg|NC_MinMaxAgg|NC_HasWin|NC_OrderAgg);
    }
    if( w.parser->n_err>0 ) return DBSQL_ERROR;
  }
  nc->nc_flags |= saved_has_agg;
  return DBSQL_OK;
}

/*
** Resolve all names in all expressions of a SELECT and in all
** descendants of the SELECT, including compounds off of p->prior,
** subqueries in expressions, and subqueries used as FROM clause
** terms.
**
** See __resolve_expr_names() for a description of the kinds of
** transformations that occur.
**
** All SELECT statements should have been expanded using
** __select_expand() prior to invoking this routine.
*/
void __resolve_select_names(
  parser_t *parser,         /* The parser context */
  select_t *p,             /* The SELECT statement being coded. */
  name_context_t *outer_nc  /* Name context for parent SELECT statement */
){
  walker_t w;

  assert( p!=0 );
  w.expr_callback = __resolve_expr_step;
  w.select_callback = __resolve_select_step;
  w.select_callback2 = 0;
  w.parser = parser;
  w.u.nc = outer_nc;
  __walk_select(&w, p);
}

/*
** Resolve names in expressions that can only reference a single table
** or which cannot reference any tables at all.  Examples:
**
**                                                    "type" flag
**                                                    ------------
**    (1)   CHECK constraints                         NC_IsCheck
**    (2)   WHERE clauses on partial indices          NC_PartIdx
**    (3)   Expressions in indexes on expressions     NC_IdxExpr
**    (4)   Expression arguments to VACUUM INTO.      0
**    (5)   GENERATED ALWAYS as expressions           NC_GenCol
**
** In all cases except (4), the expr_t.i_table value for expr_t.op==TK_COLUMN
** nodes of the expression is set to -1 and the expr_t.i_column value is
** set to the column number.  In case (4), TK_COLUMN nodes cause an error.
**
** Any errors cause an error message to be set in parser.
*/
int __resolve_self_reference(
  parser_t *parser,   /* Parsing context */
  table_t *tab,     /* The table being referenced, or NULL */
  int type,        /* NC_IsCheck, NC_PartIdx, NC_IdxExpr, NC_GenCol, or 0 */
  expr_t *expr,     /* Expression to resolve.  May be NULL. */
  expr_list_t *list  /* Expression list to resolve.  May be NULL. */
){
  src_list_t *src;                  /* Fake src_list_t for parser->new_table */
  name_context_t s_nc;                /* Name context for parser->new_table */
  int rc;
  union {
    src_list_t s_src;
    u_int8_t src_space[SZ_SRCLIST_1];     /* Memory space for the fake src_list_t */
  } u_src;

  assert( type==0 || tab!=0 );
  assert( type==NC_IsCheck || type==NC_PartIdx || type==NC_IdxExpr
          || type==NC_GenCol || tab==0 );
  memset(&s_nc, 0, sizeof(s_nc));
  memset(&u_src, 0, sizeof(u_src));
  src = &u_src.s_src;
  if( tab ){
    src->n_src = 1;
    src->a[0].name = tab->name;
    src->a[0].s_tab = tab;
    src->a[0].i_cursor = -1;
    if( tab->schema!=parser->db->dbs[1].schema ){
      /* Cause EP_FromDDL to be set on TK_FUNCTION nodes of non-TEMP
      ** schema elements */
      type |= NC_FromDDL;
    }
  }
  s_nc.parser = parser;
  s_nc.src_list = src;
  s_nc.nc_flags = type | NC_IsDDL;
  if( (rc = __resolve_expr_names(&s_nc, expr))!=DBSQL_OK ) return rc;
  if( list ) rc = __resolve_expr_list_names(&s_nc, list);
  return rc;
}
