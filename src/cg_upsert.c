/*
** 2018-04-12
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code to implement various aspects of UPSERT
** processing and handling of the upsert_t object.
*/
#include "dbsql_int.h"

#ifndef DBSQL_OMIT_UPSERT
/*
** Free a list of upsert_t objects
*/
static void DBSQL_NOINLINE __upsert_delete_local(DBSQL *db, upsert_t *p){
  do{
    upsert_t *next = p->next_upsert;
    __expr_list_delete(db, p->upsert_target);
    __expr_delete(db, p->upsert_target_where);
    __expr_list_delete(db, p->upsert_set);
    __expr_delete(db, p->upsert_where);
    __dbsql_db_free(db, p->to_free);
    __dbsql_db_free(db, p);
    p = next;
  }while( p );
}
void __upsert_delete(DBSQL *db, upsert_t *p){
  if( p ) __upsert_delete_local(db, p);
}


/*
** Duplicate an upsert_t object.
*/
upsert_t *__upsert_dup(DBSQL *db, upsert_t *p){
  if( p==0 ) return 0;
  return __upsert_new(db,
           __expr_list_dup(db, p->upsert_target, 0),
           __expr_dup(db, p->upsert_target_where, 0),
           __expr_list_dup(db, p->upsert_set, 0),
           __expr_dup(db, p->upsert_where, 0),
           __upsert_dup(db, p->next_upsert)
         );
}

/*
** Create a new upsert_t object.
*/
upsert_t *__upsert_new(
  DBSQL *db,           /* Determines which memory allocator to use */
  expr_list_t *target,     /* Target argument to ON CONFLICT, or NULL */
  expr_t *target_where,    /* Optional WHERE clause on the target */
  expr_list_t *set,        /* UPDATE columns, or NULL for a DO NOTHING */
  expr_t *where,          /* WHERE clause for the ON CONFLICT UPDATE */
  upsert_t *next          /* Next ON CONFLICT clause in the list */
){
  upsert_t *p_new;
  p_new = __db_malloc_zero(db, sizeof(upsert_t));
  if( p_new==0 ){
    __expr_list_delete(db, target);
    __expr_delete(db, target_where);
    __expr_list_delete(db, set);
    __expr_delete(db, where);
    __upsert_delete(db, next);
    return 0;
  }else{
    p_new->upsert_target = target;
    p_new->upsert_target_where = target_where;
    p_new->upsert_set = set;
    p_new->upsert_where = where;
    p_new->is_do_update = set!=0;
    p_new->next_upsert = next;
  }
  return p_new;
}

/*
** Analyze the ON CONFLICT clause described by upsert.  Resolve all
** symbols in the conflict-target.
**
** Return DBSQL_OK if everything works, or an error code is something
** is wrong.
*/
int __upsert_analyze_target(
  parser_t *parser,     /* The parsing context */
  src_list_t *tab_list, /* table_t into which we are inserting */
  upsert_t *upsert,   /* The ON CONFLICT clauses */
  upsert_t *all       /* Complete list of all ON CONFLICT clauses */
){
  table_t *tab;            /* That table into which we are inserting */
  int rc;                 /* Result code */
  int i_cursor;            /* Cursor used by tab */
  index_t *idx;            /* One of the indexes of tab */
  expr_list_t *target;      /* The conflict-target clause */
  expr_t *term;            /* One term of the conflict-target clause */
  name_context_t s_nc;        /* Context for resolving symbolic names */
  expr_t s_col[2];           /* index_t column converted into an expr_t */
  int n_clause = 0;        /* Counter of ON CONFLICT clauses */

  assert( tab_list->n_src==1 );
  assert( tab_list->a[0].s_tab!=0 );
  assert( upsert!=0 );
  assert( upsert->upsert_target!=0 );

  /* Resolve all symbolic names in the conflict-target clause, which
  ** includes both the list of columns and the optional partial-index
  ** WHERE clause.
  */
  memset(&s_nc, 0, sizeof(s_nc));
  s_nc.parser = parser;
  s_nc.src_list = tab_list;
  for(; upsert && upsert->upsert_target;
        upsert=upsert->next_upsert, n_clause++){
    rc = __resolve_expr_list_names(&s_nc, upsert->upsert_target);
    if( rc ) return rc;
    rc = __resolve_expr_names(&s_nc, upsert->upsert_target_where);
    if( rc ) return rc;
  
    /* Check to see if the conflict target matches the rowid. */  
    tab = tab_list->a[0].s_tab;
    target = upsert->upsert_target;
    i_cursor = tab_list->a[0].i_cursor;
    if( HAS_ROWID(tab) 
     && target->n_expr==1
     && (term = target->a[0].expr)->op==TK_COLUMN
     && term->i_column==XN_ROWID
    ){
      /* The conflict-target is the rowid of the primary table */
      assert( upsert->upsert_idx==0 );
      continue;
    }
  
    /* Initialize s_col[0..1] to be an expression parse tree for a
    ** single column of an index.  The s_col[0] node will be the TK_COLLATE
    ** operator and s_col[1] will be the TK_COLUMN operator.  Code below
    ** will populate the specific collation and column number values
    ** prior to comparing against the conflict-target expression.
    */
    memset(s_col, 0, sizeof(s_col));
    s_col[0].op = TK_COLLATE;
    s_col[0].left = &s_col[1];
    s_col[1].op = TK_COLUMN;
    s_col[1].i_table = tab_list->a[0].i_cursor;
  
    /* Check for matches against other indexes */
    for(idx=tab->p_index; idx; idx=idx->next){
      int ii, jj, nn;
      if( !IS_UNIQUE_INDEX(idx) ) continue;
      if( target->n_expr!=idx->n_key_col ) continue;
      if( idx->part_idx_where ){
        if( upsert->upsert_target_where==0 ) continue;
        if( __expr_compare(parser, upsert->upsert_target_where,
                               idx->part_idx_where, i_cursor)!=0 ){
          continue;
        }
      }
      nn = idx->n_key_col;
      for(ii=0; ii<nn; ii++){
        expr_t *expr;
        s_col[0].u.token = (char*)idx->colls[ii];
        if( idx->columns[ii]==XN_EXPR ){
          assert( idx->col_exprs!=0 );
          assert( idx->col_exprs->n_expr>ii );
          assert( idx->has_expr );
          expr = idx->col_exprs->a[ii].expr;
          if( expr->op!=TK_COLLATE ){
            s_col[0].left = expr;
            expr = &s_col[0];
          }
        }else{
          s_col[0].left = &s_col[1];
          s_col[1].i_column = idx->columns[ii];
          expr = &s_col[0];
        }
        for(jj=0; jj<nn; jj++){
          if( __expr_compare(0,target->a[jj].expr,expr,i_cursor)<2 ){
            break;  /* column_t ii of the index matches column jj of target */
          }
        }
        if( jj>=nn ){
          /* The target contains no match for column jj of the index */
          break;
        }
      }
      if( ii<nn ){
        /* column_t ii of the index did not match any term of the conflict target.
        ** Continue the search with the next index. */
        continue;
      }
      upsert->upsert_idx = idx;
      if( __upsert_of_index(all,idx)!=upsert ){
        /* Really this should be an error.  The is_dup ON CONFLICT clause will
        ** never fire.  But this problem was not discovered until three years
        ** after multi-CONFLICT upsert was added, and so we silently ignore
        ** the problem to prevent breaking applications that might actually
        ** have redundant ON CONFLICT clauses. */
        upsert->is_dup = 1;
      }
      break;
    }
    if( upsert->upsert_idx==0 ){
      char which[16];
      if( n_clause==0 && upsert->next_upsert==0 ){
        which[0] = 0;
      }else{
        dbsql_snprintf(sizeof(which),which,"%r ", n_clause+1);
      }
      __error_msg(parser, "%sON CONFLICT clause does not match any "
                              "PRIMARY KEY or UNIQUE constraint", which);
      return DBSQL_ERROR;
    }
  }
  return DBSQL_OK;
}

/*
** Return true if upsert is the last ON CONFLICT clause with a
** conflict target, or if upsert is followed by another ON CONFLICT
** clause that targets the INTEGER PRIMARY KEY.
*/
int __upsert_next_is_ipk(upsert_t *upsert){
  upsert_t *next;
  if( NEVER(upsert==0) ) return 0;
  next = upsert->next_upsert;
  while( 1 /*exit-by-return*/ ){
    if( next==0 ) return 1;
    if( next->upsert_target==0 ) return 1;
    if( next->upsert_idx==0 ) return 1;
    if( !next->is_dup ) return 0;
    next = next->next_upsert;
  }
  return 0;
}

/*
** Given the list of ON CONFLICT clauses described by upsert, and
** a particular index idx, return a pointer to the particular ON CONFLICT
** clause that applies to the index.  Or, if the index is not subject to
** any ON CONFLICT clause, return NULL.
*/
upsert_t *__upsert_of_index(upsert_t *upsert, index_t *idx){
  while(
      upsert
   && upsert->upsert_target!=0
   && upsert->upsert_idx!=idx
  ){
     upsert = upsert->next_upsert;
  }
  return upsert;
}

/*
** Generate bytecode that does an UPDATE as part of an upsert.
**
** If idx is NULL, then the UNIQUE constraint that failed was the IPK.
** In this case parameter i_cur is a cursor open on the table b-tree that
** currently points to the conflicting table row. Otherwise, if idx
** is not NULL, then idx is the constraint that failed and i_cur is a
** cursor points to the conflicting row.
*/
void __upsert_do_update(
  parser_t *parser,        /* The parsing and code-generating context */
  upsert_t *upsert,      /* The ON CONFLICT clause for the upsert */
  table_t *tab,          /* The table being updated */
  index_t *idx,          /* The UNIQUE constraint that failed */
  int i_cur              /* Cursor for idx (or tab if idx==NULL) */
){
  vdbe_t *v = parser->vdbe;
  DBSQL *db = parser->db;
  src_list_t *src;            /* FROM clause for the UPDATE */
  int i_data_cur;
  int i;
  upsert_t *top = upsert;

  assert( v!=0 );
  assert( upsert!=0 );
  i_data_cur = upsert->i_data_cur;
  upsert = __upsert_of_index(top, idx);
  VDBE_NOOP_COMMENT((v, "Begin DO UPDATE of UPSERT"));
  if( idx && i_cur!=i_data_cur ){
    if( HAS_ROWID(tab) ){
      int reg_rowid = __get_temp_reg(parser);
      __vdbe_add_op2(v, OP_IdxRowid, i_cur, reg_rowid);
      __vdbe_add_op3(v, OP_SeekRowid, i_data_cur, 0, reg_rowid);
      VDBE_COVERAGE(v);
      __release_temp_reg(parser, reg_rowid);
    }else{
      index_t *pk = __primary_key_index(tab);
      int n_pk = pk->n_key_col;
      int i_pk = parser->n_mem+1;
      parser->n_mem += n_pk;
      for(i=0; i<n_pk; i++){
        int k;
        assert( pk->columns[i]>=0 );
        k = __table_column_to_index(idx, pk->columns[i]);
        __vdbe_add_op3(v, OP_Column, i_cur, k, i_pk+i);
        VDBE_COMMENT((v, "%s.%s", idx->name,
                    tab->cols[pk->columns[i]].cn_name));
      }
      __vdbe_verify_abortable(v, OE_Abort);
      i = __vdbe_add_op4_int(v, OP_Found, i_data_cur, 0, i_pk, n_pk);
      VDBE_COVERAGE(v);
      __vdbe_add_op4(v, OP_Halt, DBSQL_CORRUPT, OE_Abort, 0, 
            "corrupt database", P4_STATIC);
      __may_abort(parser);
      __vdbe_jump_here(v, i);
    }
  }
  /* upsert does not own top->upsert_src - the outer INSERT statement does.
  ** So we have to make a copy before passing it down into __update() */
  src = __src_list_dup(db, top->upsert_src, 0);
  /* excluded.* columns of type REAL need to be converted to a hard real */
  for(i=0; i<tab->n_col; i++){
    if( tab->cols[i].affinity==DBSQL_AFF_REAL ){
      int i_storage = top->reg_data + __table_column_to_storage(tab, i);
      __vdbe_add_op1(v, OP_RealAffinity, i_storage);
    }
  }
  __update(parser, src, __expr_list_dup(db,upsert->upsert_set,0),
      __expr_dup(db,upsert->upsert_where,0), OE_Abort, 0, 0, upsert);
  VDBE_NOOP_COMMENT((v, "End DO UPDATE of UPSERT"));
}

#endif /* DBSQL_OMIT_UPSERT */
