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
** to handle INSERT statements in dbsql.
*/
#include "dbsql_int.h"

/*
** Generate code that will
**
**   (1) acquire a lock for table tab then
**   (2) open tab as cursor i_cur.
**
** If tab is a WITHOUT ROWID table, then it is the PRIMARY KEY index
** for that table that is actually opened.
*/
void __open_table(
  parser_t *parser,  /* Generate code into this VDBE */
  int i_cur,       /* The cursor number of the table */
  int i_db,        /* The database index in DBSQL.dbs[] */
  table_t *tab,    /* The table to be opened */
  int opcode      /* OP_OpenRead or OP_OpenWrite */
){
  vdbe_t *v;
  assert( !IS_VIRTUAL(tab) );
  assert( parser->vdbe!=0 );
  v = parser->vdbe;
  assert( opcode==OP_OpenWrite || opcode==OP_OpenRead );
  if( !parser->db->no_shared_cache ){
    __table_lock(parser, i_db, tab->tnum,
                     (opcode==OP_OpenWrite)?1:0, tab->name);
  }
  if( HAS_ROWID(tab) ){
    __vdbe_add_op4_int(v, opcode, i_cur, tab->tnum, i_db, tab->n_nv_col);
    VDBE_COMMENT((v, "%s", tab->name));
  }else{
    index_t *pk = __primary_key_index(tab);
    assert( pk!=0 );
    assert( pk->tnum==tab->tnum || CORRUPT_DB );
    __vdbe_add_op3(v, opcode, i_cur, pk->tnum, i_db);
    __vdbe_set_p4_key_info(parser, pk);
    VDBE_COMMENT((v, "%s", tab->name));
  }
}

/*
** Return a pointer to the column affinity string associated with index
** idx. A column affinity string has one character for each column in
** the table, according to the affinity of the column:
**
**  Character      column_t affinity
**  ------------------------------
**  'A'            BLOB
**  'B'            TEXT
**  'C'            NUMERIC
**  'D'            INTEGER
**  'F'            REAL
**
** An extra 'D' is appended to the end of the string to cover the
** rowid that appears as the last column in every index.
**
** Memory for the buffer containing the column index affinity string
** is managed along with the rest of the index_t structure. It will be
** released when __delete_index() is called.
*/
static DBSQL_NOINLINE const char *__compute_index_aff_str(DBSQL *db, index_t *idx){
  /* The first time a column affinity string for a particular index is
  ** required, it is allocated and populated here. It is then stored as
  ** a member of the index_t structure for subsequent use.
  **
  ** The column affinity string will eventually be deleted by
  ** dbsql_delete_index() when the index_t structure itself is cleaned
  ** up.
  */
  int n;
  table_t *tab = idx->table;
  idx->col_aff = (char *)__db_malloc_raw(0, idx->n_column+1);
  if( !idx->col_aff ){
    __oom_fault(db);
    return 0;
  }
  for(n=0; n<idx->n_column; n++){
    int16_t x = idx->columns[n];
    char aff;
    if( x>=0 ){
      aff = tab->cols[x].affinity;
    }else if( x==XN_ROWID ){
      aff = DBSQL_AFF_INTEGER;
    }else{
      assert( x==XN_EXPR );
      assert( idx->has_expr );
      assert( idx->col_exprs!=0 );
      aff = __expr_affinity(idx->col_exprs->a[n].expr);
    }
    if( aff<DBSQL_AFF_BLOB ) aff = DBSQL_AFF_BLOB;
    if( aff>DBSQL_AFF_NUMERIC) aff = DBSQL_AFF_NUMERIC;
    idx->col_aff[n] = aff;
  }
  idx->col_aff[n] = 0;
  return idx->col_aff;
}
const char *__index_affinity_str(DBSQL *db, index_t *idx){
  if( !idx->col_aff ) return __compute_index_aff_str(db, idx);
  return idx->col_aff;
}


/*
** Compute an affinity string for a table.   Space is obtained
** from __db_malloc().  The caller is responsible for freeing
** the space when done.
*/
char *__table_affinity_str(DBSQL *db, const table_t *tab){
  char *col_aff;
  col_aff = (char *)__db_malloc_raw(db, tab->n_col+1);
  if( col_aff ){
    int i, j;
    for(i=j=0; i<tab->n_col; i++){
      if( (tab->cols[i].col_flags & COLFLAG_VIRTUAL)==0 ){
        col_aff[j++] = tab->cols[i].affinity;
      }
    }
    do{
      col_aff[j--] = 0;
    }while( j>=0 && col_aff[j]<=DBSQL_AFF_BLOB );
  }
  return col_aff; 
}

/*
** Make changes to the evolving bytecode to do affinity transformations
** of values that are about to be gathered into a row for table tab.
**
** For ordinary (legacy, non-strict) tables:
** -----------------------------------------
**
** Compute the affinity string for table tab, if it has not already been
** computed.  As an optimization, omit trailing DBSQL_AFF_BLOB affinities.
**
** If the affinity string is empty (because it was all DBSQL_AFF_BLOB entries
** which were then optimized out) then this routine becomes a no-op.
**
** Otherwise if i_reg>0 then code an OP_Affinity opcode that will set the
** affinities for register i_reg and following.  Or if i_reg==0,
** then just set the P4 operand of the previous opcode (which should  be
** an OP_MakeRecord) to the affinity string.
**
** A column affinity string has one character per column:
**
**    Character      column_t affinity
**    ---------      ---------------
**    'A'            BLOB
**    'B'            TEXT
**    'C'            NUMERIC
**    'D'            INTEGER
**    'E'            REAL
**
** For STRICT tables:
** ------------------
**
** Generate an appropriate OP_TypeCheck opcode that will verify the
** datatypes against the column definitions in tab.  If i_reg==0, that
** means an OP_MakeRecord opcode has already been generated and should be
** the last opcode generated.  The new OP_TypeCheck needs to be inserted
** before the OP_MakeRecord.  The new OP_TypeCheck should use the same
** register set as the OP_MakeRecord.  If i_reg>0 then register i_reg is
** the first of a series of registers that will form the new record.
** Apply the type checking to that array of registers.
*/
void __table_affinity(vdbe_t *v, table_t *tab, int i_reg){
  int i;
  char *col_aff;
  if( tab->tab_flags & TF_Strict ){
    if( i_reg==0 ){
      /* Move the previous opcode (which should be OP_MakeRecord) forward
      ** by one slot and insert a new OP_TypeCheck where the current
      ** OP_MakeRecord is found */
      vdbe_op_t *prev;
      int p3;
      __vdbe_append_p4(v, tab, P4_TABLE);
      prev = __vdbe_get_last_op(v);
      assert( prev!=0 );
      assert( prev->opcode==OP_MakeRecord || __vdbe_db(v)->malloc_failed );
      prev->opcode = OP_TypeCheck;
      p3 = prev->p3;
      prev->p3 = 0;
      __vdbe_add_op3(v, OP_MakeRecord, prev->p1, prev->p2, p3);
    }else{
      /* Insert an isolated OP_Typecheck */
      __vdbe_add_op2(v, OP_TypeCheck, i_reg, tab->n_nv_col);
      __vdbe_append_p4(v, tab, P4_TABLE);
    }
    return;
  }
  col_aff = tab->col_aff;
  if( col_aff==0 ){
    col_aff = __table_affinity_str(0, tab);
    if( !col_aff ){
      __oom_fault(__vdbe_db(v));
      return;
    }
    tab->col_aff = col_aff;
  }
  assert( col_aff!=0 );
  i = __strlen30_nn(col_aff);
  if( i ){
    if( i_reg ){
      __vdbe_add_op4(v, OP_Affinity, i_reg, i, 0, col_aff, i);
    }else{
      assert( __vdbe_get_last_op(v)->opcode==OP_MakeRecord
              || __vdbe_db(v)->malloc_failed );
      __vdbe_change_p4(v, -1, col_aff, i);
    }
  }
}

/*
** Return non-zero if the table tab in database i_db or any of its indices
** have been opened at any point in the VDBE program. This is used to see if
** a statement of the form  "INSERT INTO <i_db, tab> SELECT ..." can
** run without using a temporary table for the results of the SELECT.
*/
static int __reads_table(parser_t *p, int i_db, table_t *tab){
  vdbe_t *v = __get_vdbe(p);
  int i;
  int i_end = __vdbe_current_addr(v);
#ifndef DBSQL_OMIT_VIRTUALTABLE
  v_table_t *v_tab = IS_VIRTUAL(tab) ? __get_v_table(p->db, tab) : 0;
#endif

  for(i=1; i<i_end; i++){
    vdbe_op_t *op = __vdbe_get_op(v, i);
    assert( op!=0 );
    if( op->opcode==OP_OpenRead && op->p3==i_db ){
      index_t *p_index;
      pgno_t tnum = op->p2;
      if( tnum==tab->tnum ){
        return 1;
      }
      for(p_index=tab->p_index; p_index; p_index=p_index->next){
        if( tnum==p_index->tnum ){
          return 1;
        }
      }
    }
#ifndef DBSQL_OMIT_VIRTUALTABLE
    if( op->opcode==OP_VOpen && op->p4.vtab==v_tab ){
      assert( op->p4.vtab!=0 );
      assert( op->p4type==P4_VTAB );
      return 1;
    }
#endif
  }
  return 0;
}

/* This walker callback will compute the union of col_flags flags for all
** referenced columns in a CHECK constraint or generated column expression.
*/
static int __expr_column_flag_union(walker_t *walker, expr_t *expr){
  if( expr->op==TK_COLUMN && expr->i_column>=0 ){
    assert( expr->i_column < walker->u.tab->n_col );
    walker->code |= walker->u.tab->cols[expr->i_column].col_flags;
  }
  return WRC_Continue;
}

#ifndef DBSQL_OMIT_GENERATED_COLUMNS
/*
** All regular columns for table tab have been puts into registers
** starting with i_reg_store.  The registers that correspond to STORED
** or VIRTUAL columns have not yet been initialized.  This routine goes
** back and computes the values for those columns based on the previously
** computed normal columns.
*/
void __compute_generated_columns(
  parser_t *parser,    /* Parsing context */
  int i_reg_store,    /* Register holding the first column */
  table_t *tab       /* The table */
){
  int i;
  walker_t w;
  column_t *redo;
  int progress;
  vdbe_op_t *op;

  assert( tab->tab_flags & TF_HasGenerated );
  testcase( tab->tab_flags & TF_HasVirtual );
  testcase( tab->tab_flags & TF_HasStored );

  /* Before computing generated columns, first go through and make sure
  ** that appropriate affinity has been applied to the regular columns
  */
  __table_affinity(parser->vdbe, tab, i_reg_store);
  if( (tab->tab_flags & TF_HasStored)!=0 ){
    op = __vdbe_get_last_op(parser->vdbe);
    if( op->opcode==OP_Affinity ){
      /* Change the OP_Affinity argument to '@' (NONE) for all stored
      ** columns.  '@' is the no-op affinity and those columns have not
      ** yet been computed. */
      int ii, jj;
      char *p4 = op->p4.z;
      assert( p4!=0 );
      assert( op->p4type==P4_DYNAMIC );
      for(ii=jj=0; p4[jj]; ii++){
        if( tab->cols[ii].col_flags & COLFLAG_VIRTUAL ){
          continue;
        }
        if( tab->cols[ii].col_flags & COLFLAG_STORED ){
          p4[jj] = DBSQL_AFF_NONE;
        }
        jj++;
      }
    }else if( op->opcode==OP_TypeCheck ){
      /* If an OP_TypeCheck was generated because the table is STRICT,
      ** then set the P3 operand to indicate that generated columns should
      ** not be checked */
      op->p3 = 1;
    }
  }

  /* Because there can be multiple generated columns that refer to one another,
  ** this is a two-pass algorithm.  On the first pass, mark all generated
  ** columns as "not available".
  */
  for(i=0; i<tab->n_col; i++){
    if( tab->cols[i].col_flags & COLFLAG_GENERATED ){
      testcase( tab->cols[i].col_flags & COLFLAG_VIRTUAL );
      testcase( tab->cols[i].col_flags & COLFLAG_STORED );
      tab->cols[i].col_flags |= COLFLAG_NOTAVAIL;
    }
  }

  w.u.tab = tab;
  w.expr_callback = __expr_column_flag_union;
  w.select_callback = 0;
  w.select_callback2 = 0;

  /* On the second pass, compute the value of each NOT-AVAILABLE column.
  ** Companion code in the TK_COLUMN case of __expr_code_target() will
  ** compute dependencies and mark remove the COLSPAN_NOTAVAIL mark, as
  ** they are needed.
  */
  parser->i_self_tab = -i_reg_store;
  do{
    progress = 0;
    redo = 0;
    for(i=0; i<tab->n_col; i++){
      column_t *col = tab->cols + i;
      if( (col->col_flags & COLFLAG_NOTAVAIL)!=0 ){
        int x;
        col->col_flags |= COLFLAG_BUSY;
        w.code = 0;
        __walk_expr(&w, __column_expr(tab, col));
        col->col_flags &= ~COLFLAG_BUSY;
        if( w.code & COLFLAG_NOTAVAIL ){
          redo = col;
          continue;
        }
        progress = 1;
        assert( col->col_flags & COLFLAG_GENERATED );
        x = __table_column_to_storage(tab, i) + i_reg_store;
        __expr_code_generated_column(parser, tab, col, x);
        col->col_flags &= ~COLFLAG_NOTAVAIL;
      }
    }
  }while( redo && progress );
  if( redo ){
    __error_msg(parser, "generated column loop on \"%s\"", redo->cn_name);
  }
  parser->i_self_tab = 0;
}
#endif /* DBSQL_OMIT_GENERATED_COLUMNS */


#ifndef DBSQL_OMIT_AUTOINCREMENT
/*
** Locate or create an autoinc_info_t structure associated with table tab
** which is in database i_db.  Return the register number for the register
** that holds the maximum rowid.  Return zero if tab is not an AUTOINCREMENT
** table.  (Also return zero when doing a VACUUM since we do not want to
** update the AUTOINCREMENT counters during a VACUUM.)
**
** There is at most one autoinc_info_t structure per table even if the
** same table is autoincremented multiple times due to inserts within
** triggers.  A new autoinc_info_t structure is created if this is the
** first use of table tab.  On 2nd and subsequent uses, the original
** autoinc_info_t structure is used.
**
** Four consecutive registers are allocated:
**
**   (1)  The name of the tab table.
**   (2)  The maximum ROWID of tab.
**   (3)  The rowid in dbsql_sequence of tab
**   (4)  The original value of the max ROWID in tab, or NULL if none
**
** The 2nd register is the one that is returned.  That is all the
** insert routine needs to know about.
*/
static int AUTO_INC_BEGIN(
  parser_t *parser,      /* Parsing context */
  int i_db,            /* index_t of the database holding tab */
  table_t *tab         /* The table we are writing to */
){
  int mem_id = 0;      /* Register holding maximum rowid */
  assert( parser->db->dbs[i_db].schema!=0 );
  if( (tab->tab_flags & TF_Autoincrement)!=0
   && (parser->db->m_db_flags & DBFLAG_Vacuum)==0
  ){
    parser_t *toplevel = __parse_toplevel(parser);
    autoinc_info_t *info;
    table_t *seq_tab = parser->db->dbs[i_db].schema->seq_tab;

    /* Verify that the dbsql_sequence table exists and is an ordinary
    ** rowid table with exactly two columns.
    ** Ticket d8dc2b3a58cd5dc2918a1d4acb 2018-05-23 */
    if( seq_tab==0
     || !HAS_ROWID(seq_tab)
     || NEVER(IS_VIRTUAL(seq_tab))
     || seq_tab->n_col!=2
    ){
      parser->n_err++;
      parser->rc = DBSQL_CORRUPT_SEQUENCE;
      return 0;
    }

    info = toplevel->ainc;
    while( info && info->tab!=tab ){ info = info->next; }
    if( info==0 ){
      info = __db_malloc_raw_nn(parser->db, sizeof(*info));
      __parser_add_cleanup(toplevel, __dbsql_db_free, info);
      testcase( parser->early_cleanup );
      if( parser->db->malloc_failed ) return 0;
      info->next = toplevel->ainc;
      toplevel->ainc = info;
      info->tab = tab;
      info->i_db = i_db;
      toplevel->n_mem++;                  /* Register to hold name of table */
      info->reg_ctr = ++toplevel->n_mem;  /* Max rowid register */
      toplevel->n_mem +=2;       /* Rowid in dbsql_sequence + orig max val */
    }
    mem_id = info->reg_ctr;
  }
  return mem_id;
}

/*
** This routine generates code that will initialize all of the
** register used by the autoincrement tracker. 
*/
void __autoincrement_begin(parser_t *parser){
  autoinc_info_t *p;            /* Information about an AUTOINCREMENT */
  DBSQL *db = parser->db;  /* The database connection */
  db_t *p_db;                   /* Database only autoinc table */
  int mem_id;                 /* Register holding max rowid */
  vdbe_t *v = parser->vdbe;   /* VDBE under construction */

  /* This routine is never called during trigger-generation.  It is
  ** only called from the top-level */
  assert( parser->trigger_tab==0 );
  assert( __is_toplevel(parser) );

  assert( v );   /* We failed long ago if this is not so */
  for(p = parser->ainc; p; p = p->next){
    static const int i_ln = VDBE_OFFSET_LINENO(2);
    static const vdbe_op_list_t auto_inc[] = {
      /* 0  */ {OP_Null,    0,  0, 0},
      /* 1  */ {OP_Rewind,  0, 10, 0},
      /* 2  */ {OP_Column,  0,  0, 0},
      /* 3  */ {OP_Ne,      0,  9, 0},
      /* 4  */ {OP_Rowid,   0,  0, 0},
      /* 5  */ {OP_Column,  0,  1, 0},
      /* 6  */ {OP_AddImm,  0,  0, 0},
      /* 7  */ {OP_Copy,    0,  0, 0},
      /* 8  */ {OP_Goto,    0, 11, 0},
      /* 9  */ {OP_Next,    0,  2, 0},
      /* 10 */ {OP_Integer, 0,  0, 0},
      /* 11 */ {OP_Close,   0,  0, 0}
    };
    vdbe_op_t *ops;
    p_db = &db->dbs[p->i_db];
    mem_id = p->reg_ctr;
    assert( __schema_mutex_held(db, 0, p_db->schema) );
    __open_table(parser, 0, p->i_db, p_db->schema->seq_tab, OP_OpenRead);
    __vdbe_load_string(v, mem_id-1, p->tab->name);
    ops = __vdbe_add_op_list(v, ARRAY_SIZE(auto_inc), auto_inc, i_ln);
    if( ops==0 ) break;
    ops[0].p2 = mem_id;
    ops[0].p3 = mem_id+2;
    ops[2].p3 = mem_id;
    ops[3].p1 = mem_id-1;
    ops[3].p3 = mem_id;
    ops[3].p5 = DBSQL_JUMPIFNULL;
    ops[4].p2 = mem_id+1;
    ops[5].p3 = mem_id;
    ops[6].p1 = mem_id;
    ops[7].p2 = mem_id+2;
    ops[7].p1 = mem_id;
    ops[10].p2 = mem_id;
    if( parser->n_tab==0 ) parser->n_tab = 1;
  }
}

/*
** Update the maximum rowid for an autoincrement calculation.
**
** This routine should be called when the reg_rowid register holds a
** new rowid that is about to be inserted.  If that new rowid is
** larger than the maximum rowid in the mem_id memory cell, then the
** memory cell is updated.
*/
static void AUTO_INC_STEP(parser_t *parser, int mem_id, int reg_rowid){
  if( mem_id>0 ){
    __vdbe_add_op2(parser->vdbe, OP_MemMax, mem_id, reg_rowid);
  }
}

/*
** This routine generates the code needed to write autoincrement
** maximum rowid values back into the dbsql_sequence register.
** Every statement that might do an INSERT into an autoincrement
** table (either directly or through triggers) needs to call this
** routine just before the "exit" code.
*/
static DBSQL_NOINLINE void __auto_increment_end(parser_t *parser){
  autoinc_info_t *p;
  vdbe_t *v = parser->vdbe;
  DBSQL *db = parser->db;

  assert( v );
  for(p = parser->ainc; p; p = p->next){
    static const int i_ln = VDBE_OFFSET_LINENO(2);
    static const vdbe_op_list_t auto_inc_end[] = {
      /* 0 */ {OP_NotNull,     0, 2, 0},
      /* 1 */ {OP_NewRowid,    0, 0, 0},
      /* 2 */ {OP_MakeRecord,  0, 2, 0},
      /* 3 */ {OP_Insert,      0, 0, 0},
      /* 4 */ {OP_Close,       0, 0, 0}
    };
    vdbe_op_t *ops;
    db_t *p_db = &db->dbs[p->i_db];
    int i_rec;
    int mem_id = p->reg_ctr;

    i_rec = __get_temp_reg(parser);
    assert( __schema_mutex_held(db, 0, p_db->schema) );
    __vdbe_add_op3(v, OP_Le, mem_id+2, __vdbe_current_addr(v)+7, mem_id);
    VDBE_COVERAGE(v);
    __open_table(parser, 0, p->i_db, p_db->schema->seq_tab, OP_OpenWrite);
    ops = __vdbe_add_op_list(v, ARRAY_SIZE(auto_inc_end), auto_inc_end, i_ln);
    if( ops==0 ) break;
    ops[0].p1 = mem_id+1;
    ops[1].p2 = mem_id+1;
    ops[2].p1 = mem_id-1;
    ops[2].p3 = i_rec;
    ops[3].p2 = i_rec;
    ops[3].p3 = mem_id+1;
    ops[3].p5 = OPFLAG_APPEND;
    __release_temp_reg(parser, i_rec);
  }
}
void __autoincrement_end(parser_t *parser){
  if( parser->ainc ) __auto_increment_end(parser);
}
#else
/*
** If DBSQL_OMIT_AUTOINCREMENT is defined, then the three routines
** above are all no-ops
*/
# define AUTO_INC_BEGIN(A,B,C) (0)
# define AUTO_INC_STEP(A,B,C)
#endif /* DBSQL_OMIT_AUTOINCREMENT */

/*
** If argument val is a select_t object returned by an __multi_values()
** that was able to use the co-routine optimization, finish coding the
** co-routine.
*/
void __multi_values_end(parser_t *parser, select_t *val){
  if( ALWAYS(val) && val->src->n_src>0 ){
    src_item_t *item = &val->src->a[0];
    assert( (item->fg.is_subquery && item->u4.subq!=0) || parser->n_err );
    if( item->fg.is_subquery ){
      __vdbe_end_coroutine(parser->vdbe, item->u4.subq->reg_return);
      __vdbe_jump_here(parser->vdbe, item->u4.subq->addr_fill_sub - 1);
    }
  }
}

/*
** Return true if all expressions in the expression-list passed as the
** only argument are constant.
*/
static int __expr_list_is_constant(parser_t *parser, expr_list_t *row){
  int ii;
  for(ii=0; ii<row->n_expr; ii++){
    if( 0==__expr_is_constant(parser, row->a[ii].expr) ) return 0;
  }
  return 1;
}

/*
** Return true if all expressions in the expression-list passed as the
** only argument are both constant and have no affinity.
*/
static int __expr_list_is_no_affinity(parser_t *parser, expr_list_t *row){
  int ii;
  if( __expr_list_is_constant(parser,row)==0 ) return 0;
  for(ii=0; ii<row->n_expr; ii++){
    expr_t *expr = row->a[ii].expr;
    assert( expr->op!=TK_RAISE );
    assert( expr->aff_expr==0 );
    if( 0!=__expr_affinity(expr) ) return 0;
  }
  return 1;

}

/*
** This function is called by the parser for the second and subsequent
** rows of a multi-row VALUES clause. Argument left is the part of
** the VALUES clause already parsed, argument row is the vector of values
** for the new row. The select_t object returned represents the complete
** VALUES clause, including the new row.
**
** There are two ways in which this may be achieved - by incremental 
** coding of a co-routine (the "co-routine" method) or by returning a
** select_t object equivalent to the following (the "UNION ALL" method):
**
**        "left UNION ALL SELECT row"
**
** If the VALUES clause contains a lot of rows, this compound select_t
** object may consume a lot of memory.
**
** When the co-routine method is used, each row that will be returned
** by the VALUES clause is coded into part of a co-routine as it is 
** passed to this function. The returned select_t object is equivalent to:
**
**     SELECT * FROM (
**       select_t object to read co-routine
**     )
**
** The co-routine method is used in most cases. Exceptions are:
**
**    a) If the current statement has a WITH clause. This is to avoid
**       statements like:
**
**            WITH cte AS ( VALUES('x'), ('y') ... )
**            SELECT * FROM cte AS a, cte AS b;
**
**       This will not work, as the co-routine uses a hard-coded register
**       for its OP_Yield instructions, and so it is not possible for two
**       cursors to iterate through it concurrently.
**
**    b) The schema is currently being parsed (i.e. the VALUES clause is part 
**       of a schema item like a VIEW or TRIGGER). In this case there is no VM
**       being generated when parsing is taking place, and so generating 
**       a co-routine is not possible.
**
**    c) There are non-constant expressions in the VALUES clause (e.g.
**       the VALUES clause is part of a correlated sub-query).
**
**    d) One or more of the values in the first row of the VALUES clause
**       has an affinity (i.e. is a CAST expression). This causes problems
**       because the complex rules dbsql uses (see function 
**       __subquery_column_types() in select.c) to determine the effective
**       affinity of such a column for all rows require access to all values in
**       the column simultaneously. 
*/
select_t *__multi_values(parser_t *parser, select_t *left, expr_list_t *row){

  if( parser->has_with                   /* condition (a) above */
   || parser->db->init.busy              /* condition (b) above */
   || __expr_list_is_constant(parser,row)==0 /* condition (c) above */
   || (left->src->n_src==0 &&
       __expr_list_is_no_affinity(parser,left->e_list)==0) /* condition (d) above */
   || IN_SPECIAL_PARSE
  ){
    /* The co-routine method cannot be used. Fall back to UNION ALL. */
    select_t *p_select = 0;
    int f = SF_Values | SF_MultiValue;
    if( left->src->n_src ){
      __multi_values_end(parser, left);
      f = SF_Values;
    }else if( left->prior ){
      /* In this case set the SF_MultiValue flag only if it was set on left */
      f = (f & left->sel_flags);
    }
    p_select = __select_new(parser, row, 0, 0, 0, 0, 0, f, 0);
    left->sel_flags &= ~(u_int32_t)SF_MultiValue;
    if( p_select ){
      p_select->op = TK_ALL;
      p_select->prior = left;
      left = p_select;
    }
  }else{
    src_item_t *p = 0;               /* src_item_t that reads from co-routine */

    if( left->src->n_src==0 ){
      /* Co-routine has not yet been started and the special select_t object
      ** that accesses the co-routine has not yet been created. This block 
      ** does both those things. */
      vdbe_t *v = __get_vdbe(parser);
      select_t *ret = __select_new(parser, 0, 0, 0, 0, 0, 0, 0, 0);

      /* Ensure the database schema has been read. This is to ensure we have
      ** the correct text encoding.  */
      if( (parser->db->m_db_flags & DBFLAG_SchemaKnownOk)==0 ){
        __read_schema(parser);
      }

      if( ret ){
        select_dest_t dest;
        subquery_t *subq;
        ret->src->n_src = 1;
        ret->prior = left->prior;
        ret->op = left->op;
        if( ret->prior ) ret->sel_flags |= SF_Values;
        left->prior = 0;
        left->op = TK_SELECT;
        assert( left->next==0 );
        assert( ret->next==0 );
        p = &ret->src->a[0];
        p->fg.via_coroutine = 1;
        p->i_cursor = -1;
        assert( !p->fg.is_indexed_by && !p->fg.is_tab_func );
        p->u1.n_row = 2;
        if( __src_item_attach_subquery(parser, p, left, 0) ){
          subq = p->u4.subq;
          subq->addr_fill_sub = __vdbe_current_addr(v) + 1;
          subq->reg_return = ++parser->n_mem;
          __vdbe_add_op3(v, OP_InitCoroutine,
                            subq->reg_return, 0, subq->addr_fill_sub);
          __select_dest_init(&dest, SRT_Coroutine, subq->reg_return);

          /* Allocate registers for the output of the co-routine. Do so so
          ** that there are two unused registers immediately before those
          ** used by the co-routine. This allows the code in __insert()
          ** to use these registers directly, instead of copying the output
          ** of the co-routine to a separate array for processing.  */
          dest.i_sdst = parser->n_mem + 3; 
          dest.n_sdst = left->e_list->n_expr;
          parser->n_mem += 2 + dest.n_sdst;

          left->sel_flags |= SF_MultiValue;
          __select(parser, left, &dest);
          subq->reg_result = dest.i_sdst;
          assert( parser->n_err || dest.i_sdst>0 );
        }
        left = ret;
      }
    }else{
      p = &left->src->a[0];
      assert( !p->fg.is_tab_func && !p->fg.is_indexed_by );
      p->u1.n_row++;
    }
  
    if( parser->n_err==0 ){
      subquery_t *subq;
      assert( p!=0 );
      assert( p->fg.is_subquery );
      subq = p->u4.subq;
      assert( subq!=0 );
      assert( subq->p_select!=0 );
      assert( subq->p_select->e_list!=0 );
      if( subq->p_select->e_list->n_expr!=row->n_expr ){
        __select_wrong_num_terms_error(parser, subq->p_select);
      }else{
        __expr_code_expr_list(parser, row, subq->reg_result, 0, 0);
        __vdbe_add_op1(parser->vdbe, OP_Yield, subq->reg_return);
      }
    }
    __expr_list_delete(parser->db, row);
  }

  return left;
}

/* Forward declaration */
static int __xfer_optimization(
  parser_t *parser,        /* Parser context */
  table_t *dest,         /* The table we are inserting into */
  select_t *p_select,      /* A SELECT statement to use as the data source */
  int on_error,          /* How to handle constraint errors */
  int i_db_dest           /* The database of dest */
);

/*
** This routine is called to handle SQL of the following forms:
**
**    insert into TABLE (IDLIST) values(EXPRLIST),(EXPRLIST),...
**    insert into TABLE (IDLIST) select
**    insert into TABLE (IDLIST) default values
**
** The IDLIST following the table name is always optional.  If omitted,
** then a list of all (non-hidden) columns for the table is substituted.
** The IDLIST appears in the column parameter.  column is NULL if IDLIST
** is omitted.
**
** For the p_select parameter holds the values to be inserted for the
** first two forms shown above.  A VALUES clause is really just short-hand
** for a SELECT statement that omits the FROM clause and everything else
** that follows.  If the p_select parameter is NULL, that means that the
** DEFAULT VALUES form of the INSERT statement is intended.
**
** The code generated follows one of four templates.  For a simple
** insert with data coming from a single-row VALUES clause, the code executes
** once straight down through.  Pseudo-code follows (we call this
** the "1st template"):
**
**         open write cursor to <table> and its indices
**         put VALUES clause expressions into registers
**         write the resulting record into <table>
**         cleanup
**
** The three remaining templates assume the statement is of the form
**
**   INSERT INTO <table> SELECT ...
**
** If the SELECT clause is of the restricted form "SELECT * FROM <table2>" -
** in other words if the SELECT pulls all columns from a single table
** and there is no WHERE or LIMIT or GROUP BY or ORDER BY clauses, and
** if <table2> and <table1> are distinct tables but have identical
** schemas, including all the same indices, then a special optimization
** is invoked that copies raw records from <table2> over to <table1>.
** See the __xfer_optimization() function for the implementation of this
** template.  This is the 2nd template.
**
**         open a write cursor to <table>
**         open read cursor on <table2>
**         transfer all records in <table2> over to <table>
**         close cursors
**         foreach index on <table>
**           open a write cursor on the <table> index
**           open a read cursor on the corresponding <table2> index
**           transfer all records from the read to the write cursors
**           close cursors
**         end foreach
**
** The 3rd template is for when the second template does not apply
** and the SELECT clause does not read from <table> at any time.
** The generated code follows this template:
**
**         X <- A
**         goto B
**      A: setup for the SELECT
**         loop over the rows in the SELECT
**           load values into registers R..R+n
**           yield X
**         end loop
**         cleanup after the SELECT
**         end-coroutine X
**      B: open write cursor to <table> and its indices
**      C: yield X, at EOF goto D
**         insert the select result into <table> from R..R+n
**         goto C
**      D: cleanup
**
** The 4th template is used if the insert statement takes its
** values from a SELECT but the data is being inserted into a table
** that is also read as part of the SELECT.  In the third form,
** we have to use an intermediate table to store the results of
** the select.  The template is like this:
**
**         X <- A
**         goto B
**      A: setup for the SELECT
**         loop over the tables in the SELECT
**           load value into register R..R+n
**           yield X
**         end loop
**         cleanup after the SELECT
**         end co-routine R
**      B: open temp table
**      L: yield X, at EOF goto M
**         insert row from R..R+n into temp table
**         goto L
**      M: open write cursor to <table> and its indices
**         rewind temp table
**      C: loop over rows of intermediate table
**           transfer values form intermediate table into <table>
**         end loop
**      D: cleanup
*/
void __insert(
  parser_t *parser,        /* Parser context */
  src_list_t *tab_list,    /* Name of table into which we are inserting */
  select_t *p_select,      /* A SELECT statement to use as the data source */
  id_list_t *column,      /* column_t names corresponding to IDLIST, or NULL. */
  int on_error,          /* How to handle constraint errors */
  upsert_t *upsert       /* ON CONFLICT clauses for upsert, or NULL */
){
  DBSQL *db;          /* The main database structure */
  table_t *tab;          /* The table to insert into.  aka TABLE */
  int i, j;             /* Loop counters */
  vdbe_t *v;              /* Generate code into this virtual machine */
  index_t *idx;          /* For looping over indices of the table */
  int n_column;          /* Number of columns in the data */
  int n_hidden = 0;      /* Number of hidden columns if TABLE is virtual */
  int i_data_cur = 0;     /* VDBE cursor that is the main data repository */
  int i_idx_cur = 0;      /* First index cursor */
  int ipk_column = -1;   /* column_t that is the INTEGER PRIMARY KEY */
  int end_of_loop;        /* Label for the end of the insertion loop */
  int src_tab = 0;       /* Data comes from this temporary cursor if >=0 */
  int addr_ins_top = 0;   /* Jump to label "D" */
  int addr_cont = 0;     /* Top of insert loop. Label "C" in templates 3 and 4 */
  select_dest_t dest;      /* Destination for SELECT on rhs of INSERT */
  int i_db;              /* index_t of database holding TABLE */
  u_int8_t use_temp_table = 0;  /* Store SELECT results in intermediate table */
  u_int8_t appendFlag = 0;    /* True if the insert is likely to be an append */
  u_int8_t without_rowid;      /* 0 for normal table.  1 for WITHOUT ROWID table */
  u_int8_t id_list_in_order;    /* True if IDLIST is in table order */
  expr_list_t *list = 0;  /* List of VALUES() to be inserted  */
  int i_reg_store;        /* Register in which to store next column */

  /* Register allocations */
  int reg_from_select = 0;/* Base register for data coming from SELECT */
  int reg_autoinc = 0;   /* Register holding the AUTOINCREMENT counter */
  int reg_row_count = 0;  /* Memory cell used for the row counter */
  int reg_ins;           /* Block of regs holding rowid+data being inserted */
  int reg_rowid;         /* registers holding insert rowid */
  int reg_data;          /* register holding first column to insert */
  int *reg_idx = 0;     /* One register allocated to each index */
  int *tab_col_maps = 0;  /* Mapping from tab columns to col entries */

#ifndef DBSQL_OMIT_TRIGGER
  int isView;                 /* True if attempting to insert into a view */
  trigger_t *P_TRIGGER;          /* List of triggers on tab, if required */
  int tmask;                  /* Mask of trigger times */
#endif

  db = parser->db;
  assert( db->parser==parser );
  if( parser->n_err ){
    goto insert_cleanup;
  }
  assert( db->malloc_failed==0 );
  dest.i_sd_parm = 0;  /* Suppress a harmless compiler warning */

  /* If the select_t object is really just a simple VALUES() list with a
  ** single row (the common case) then keep that one row of values
  ** and discard the other (unused) parts of the p_select object
  */
  if( p_select && (p_select->sel_flags & SF_Values)!=0 && p_select->prior==0 ){
    list = p_select->e_list;
    p_select->e_list = 0;
    __select_delete(db, p_select);
    p_select = 0;
  }

  /* Locate the table into which we will be inserting new information.
  */
  assert( tab_list->n_src==1 );
  tab = __src_list_lookup(parser, tab_list);
  if( tab==0 ){
    goto insert_cleanup;
  }
  i_db = __schema_to_index(db, tab->schema);
  assert( i_db<db->n_db );
  if( __auth_check(parser, DBSQL_INSERT, tab->name, 0,
                       db->dbs[i_db].db_s_name) ){
    goto insert_cleanup;
  }
  without_rowid = !HAS_ROWID(tab);

  /* Figure out if we have any triggers and if the table being
  ** inserted into is a view
  */
#ifndef DBSQL_OMIT_TRIGGER
  P_TRIGGER = __triggers_exist(parser, tab, TK_INSERT, 0, &tmask);
  isView = IsView(tab);
#else
# define P_TRIGGER 0
# define tmask 0
# define isView 0
#endif
#ifdef DBSQL_OMIT_VIEW
# undef isView
# define isView 0
#endif
  assert( (P_TRIGGER && tmask) || (P_TRIGGER==0 && tmask==0) );

#if TREETRACE_ENABLED
  if( __tree_trace & 0x10000 ){
    __tree_view_line(0, "In __insert() at %s:%d", __FILE__, __LINE__);
    __tree_view_insert(parser->with, tab_list, column, p_select, list,
                          on_error, upsert, P_TRIGGER);
  }
#endif

  /* If tab is really a view, make sure it has been initialized.
  ** __view_get_column_names() is a no-op if tab is not a view.
  */
  if( __view_get_column_names(parser, tab) ){
    goto insert_cleanup;
  }

  /* Cannot insert into a read-only table.
  */
  if( __is_read_only(parser, tab, P_TRIGGER) ){
    goto insert_cleanup;
  }

  /* Allocate a VDBE
  */
  v = __get_vdbe(parser);
  if( v==0 ) goto insert_cleanup;
  if( parser->nested==0 ) __vdbe_count_changes(v);
  __begin_write_operation(parser, p_select || P_TRIGGER, i_db);

#ifndef DBSQL_OMIT_XFER_OPT
  /* If the statement is of the form
  **
  **       INSERT INTO <table1> SELECT * FROM <table2>;
  **
  ** Then special optimizations can be applied that make the transfer
  ** very fast and which reduce fragmentation of indices.
  **
  ** This is the 2nd template.
  */
  if( column==0
   && p_select!=0
   && P_TRIGGER==0
   && __xfer_optimization(parser, tab, p_select, on_error, i_db)
  ){
    assert( !P_TRIGGER );
    assert( list==0 );
    goto insert_end;
  }
#endif /* DBSQL_OMIT_XFER_OPT */

  /* If this is an AUTOINCREMENT table, look up the sequence number in the
  ** dbsql_sequence table and store it in memory cell reg_autoinc.
  */
  reg_autoinc = AUTO_INC_BEGIN(parser, i_db, tab);

  /* Allocate a block registers to hold the rowid and the values
  ** for all columns of the new row.
  */
  reg_rowid = reg_ins = parser->n_mem+1;
  parser->n_mem += tab->n_col + 1;
  if( IS_VIRTUAL(tab) ){
    reg_rowid++;
    parser->n_mem++;
  }
  reg_data = reg_rowid+1;

  /* If the INSERT statement included an IDLIST term, then make sure
  ** all elements of the IDLIST really are columns of the table and
  ** remember the column indices.
  **
  ** If the table has an INTEGER PRIMARY KEY column and that column
  ** is named in the IDLIST, then record in the ipk_column variable
  ** the index into IDLIST of the primary key column.  ipk_column is
  ** the index of the primary key as it appears in IDLIST, not as
  ** is appears in the original table.  (The index of the INTEGER
  ** PRIMARY KEY in the original table is tab->i_p_key.)  After this
  ** loop, if ipk_column==(-1), that means that integer primary key
  ** is unspecified, and hence the table is either WITHOUT ROWID or
  ** it will automatically generated an integer primary key.
  **
  ** id_list_in_order is true if the columns in IDLIST are in storage
  ** order.  This enables an optimization that avoids shuffling the
  ** columns into storage order.  False negatives are harmless,
  ** but false positives will cause database corruption.
  */
  id_list_in_order = (tab->tab_flags & (TF_OOOHidden|TF_HasStored))==0;
  if( column ){
    tab_col_maps = __db_malloc_zero(db, tab->n_col*sizeof(int));
    if( tab_col_maps==0 ) goto insert_cleanup;
    for(i=0; i<column->n_id; i++){
      j = __column_index(tab, column->a[i].name);
      if( j>=0 ){
        if( tab_col_maps[j]==0 ) tab_col_maps[j] = i+1;
        if( i!=j ) id_list_in_order = 0;
        if( j==tab->i_p_key ){
          ipk_column = i;  assert( !without_rowid );
        }
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
        if( tab->cols[j].col_flags & (COLFLAG_STORED|COLFLAG_VIRTUAL) ){
          __error_msg(parser,
             "cannot INSERT into generated column \"%s\"",
             tab->cols[j].cn_name);
          goto insert_cleanup;
        }
#endif
      }else{
        if( __is_rowid(column->a[i].name) && !without_rowid ){
          ipk_column = i;
          id_list_in_order = 0;
        }else{
          __error_msg(parser, "table %S has no column named %s",
              tab_list->a, column->a[i].name);
          parser->check_schema = 1;
          goto insert_cleanup;
        }
      }
    }
  }

  /* Figure out how many columns of data are supplied.  If the data
  ** is coming from a SELECT statement, then generate a co-routine that
  ** produces a single row of the SELECT on each invocation.  The
  ** co-routine is the common header to the 3rd and 4th templates.
  */
  if( p_select ){
    /* Data is coming from a SELECT or from a multi-row VALUES clause.
    ** Generate a co-routine to run the SELECT. */
    int rc;             /* Result code */

    if( p_select->src->n_src==1 
     && p_select->src->a[0].fg.via_coroutine 
     && p_select->prior==0
    ){
      src_item_t *item = &p_select->src->a[0];
      subquery_t *subq;
      assert( item->fg.is_subquery );
      subq = item->u4.subq;
      dest.i_sd_parm = subq->reg_return;
      reg_from_select = subq->reg_result;
      assert( subq->p_select!=0 );
      assert( subq->p_select->e_list!=0 );
      n_column = subq->p_select->e_list->n_expr;
      EXPLAIN_QUERY_PLAN((parser, 0, "SCAN %S", item));
      if( id_list_in_order && n_column==tab->n_col ){
        reg_data = reg_from_select;
        reg_rowid = reg_data - 1;
        reg_ins = reg_rowid - (IS_VIRTUAL(tab) ? 1 : 0);
      }
    }else{
      int addr_top;        /* Top of the co-routine */
      int reg_yield = ++parser->n_mem;
      addr_top = __vdbe_current_addr(v) + 1;
      __vdbe_add_op3(v, OP_InitCoroutine, reg_yield, 0, addr_top);
      __select_dest_init(&dest, SRT_Coroutine, reg_yield);
      dest.i_sdst = id_list_in_order ? reg_data : 0;
      dest.n_sdst = tab->n_col;
      rc = __select(parser, p_select, &dest);
      reg_from_select = dest.i_sdst;
      assert( db->parser==parser );
      if( rc || parser->n_err ) goto insert_cleanup;
      assert( db->malloc_failed==0 );
      __vdbe_end_coroutine(v, reg_yield);
      __vdbe_jump_here(v, addr_top - 1);                       /* label B: */
      assert( p_select->e_list );
      n_column = p_select->e_list->n_expr;
    }

    /* Set use_temp_table to TRUE if the result of the SELECT statement
    ** should be written into a temporary table (template 4).  Set to
    ** FALSE if each output row of the SELECT can be written directly into
    ** the destination table (template 3).
    **
    ** A temp table must be used if the table being updated is also one
    ** of the tables being read by the SELECT statement.  Also use a
    ** temp table in the case of row triggers.
    */
    if( P_TRIGGER || __reads_table(parser, i_db, tab) ){
      use_temp_table = 1;
    }

    if( use_temp_table ){
      /* Invoke the coroutine to extract information from the SELECT
      ** and add it to a transient table src_tab.  The code generated
      ** here is from the 4th template:
      **
      **      B: open temp table
      **      L: yield X, goto M at EOF
      **         insert row from R..R+n into temp table
      **         goto L
      **      M: ...
      */
      int reg_rec;          /* Register to hold packed record */
      int reg_temp_rowid;    /* Register to hold temp table ROWID */
      int addr_l;           /* Label "L" */

      src_tab = parser->n_tab++;
      reg_rec = __get_temp_reg(parser);
      reg_temp_rowid = __get_temp_reg(parser);
      __vdbe_add_op2(v, OP_OpenEphemeral, src_tab, n_column);
      addr_l = __vdbe_add_op1(v, OP_Yield, dest.i_sd_parm); VDBE_COVERAGE(v);
      __vdbe_add_op3(v, OP_MakeRecord, reg_from_select, n_column, reg_rec);
      __vdbe_add_op2(v, OP_NewRowid, src_tab, reg_temp_rowid);
      __vdbe_add_op3(v, OP_Insert, src_tab, reg_rec, reg_temp_rowid);
      __vdbe_goto(v, addr_l);
      __vdbe_jump_here(v, addr_l);
      __release_temp_reg(parser, reg_rec);
      __release_temp_reg(parser, reg_temp_rowid);
    }
  }else{
    /* This is the case if the data for the INSERT is coming from a
    ** single-row VALUES clause
    */
    name_context_t s_nc;
    memset(&s_nc, 0, sizeof(s_nc));
    s_nc.parser = parser;
    src_tab = -1;
    assert( use_temp_table==0 );
    if( list ){
      n_column = list->n_expr;
      if( __resolve_expr_list_names(&s_nc, list) ){
        goto insert_cleanup;
      }
    }else{
      n_column = 0;
    }
  }

  /* If there is no IDLIST term but the table has an integer primary
  ** key, the set the ipk_column variable to the integer primary key
  ** column index in the original table definition.
  */
  if( column==0 && n_column>0 ){
    ipk_column = tab->i_p_key;
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
    if( ipk_column>=0 && (tab->tab_flags & TF_HasGenerated)!=0 ){
      testcase( tab->tab_flags & TF_HasVirtual );
      testcase( tab->tab_flags & TF_HasStored );
      for(i=ipk_column-1; i>=0; i--){
        if( tab->cols[i].col_flags & COLFLAG_GENERATED ){
          testcase( tab->cols[i].col_flags & COLFLAG_VIRTUAL );
          testcase( tab->cols[i].col_flags & COLFLAG_STORED );
          ipk_column--;
        }
      }
    }
#endif

    /* Make sure the number of columns in the source data matches the number
    ** of columns to be inserted into the table.
    */
    assert( TF_HasHidden==COLFLAG_HIDDEN );
    assert( TF_HasGenerated==COLFLAG_GENERATED );
    assert( COLFLAG_NOINSERT==(COLFLAG_GENERATED|COLFLAG_HIDDEN) );
    if( (tab->tab_flags & (TF_HasGenerated|TF_HasHidden))!=0 ){
      for(i=0; i<tab->n_col; i++){
        if( tab->cols[i].col_flags & COLFLAG_NOINSERT ) n_hidden++;
      }
    }
    if( n_column!=(tab->n_col-n_hidden) ){
      __error_msg(parser,
         "table %S has %d columns but %d values were supplied",
         tab_list->a, tab->n_col-n_hidden, n_column);
     goto insert_cleanup;
    }
  }
  if( column!=0 && n_column!=column->n_id ){
    __error_msg(parser, "%d values for %d columns", n_column, column->n_id);
    goto insert_cleanup;
  }
   
  /* Initialize the count of rows to be inserted
  */
  if( (db->flags & DBSQL_CountRows)!=0
   && !parser->nested
   && !parser->trigger_tab
   && !parser->returning
  ){
    reg_row_count = ++parser->n_mem;
    __vdbe_add_op2(v, OP_Integer, 0, reg_row_count);
  }

  /* If this is not a view, open the table and and all indices */
  if( !isView ){
    int n_idx;
    n_idx = __open_table_and_indices(parser, tab, OP_OpenWrite, 0, -1, 0,
                                      &i_data_cur, &i_idx_cur);
    reg_idx = __db_malloc_raw_nn(db, sizeof(int)*(n_idx+2));
    if( reg_idx==0 ){
      goto insert_cleanup;
    }
    for(i=0, idx=tab->p_index; i<n_idx; idx=idx->next, i++){
      assert( idx );
      reg_idx[i] = ++parser->n_mem;
      parser->n_mem += idx->n_column;
    }
    reg_idx[i] = ++parser->n_mem;  /* Register to store the table record */
  }
#ifndef DBSQL_OMIT_UPSERT
  if( upsert ){
    upsert_t *nx;
    if( IS_VIRTUAL(tab) ){
      __error_msg(parser, "UPSERT not implemented for virtual table \"%s\"",
              tab->name);
      goto insert_cleanup;
    }
    if( IsView(tab) ){
      __error_msg(parser, "cannot UPSERT a view");
      goto insert_cleanup;
    }
    if( __has_explicit_nulls(parser, upsert->upsert_target) ){
      goto insert_cleanup;
    }
    tab_list->a[0].i_cursor = i_data_cur;
    nx = upsert;
    do{
      nx->upsert_src = tab_list;
      nx->reg_data = reg_data;
      nx->i_data_cur = i_data_cur;
      nx->i_idx_cur = i_idx_cur;
      if( nx->upsert_target ){
        if( __upsert_analyze_target(parser, tab_list, nx, upsert) ){
          goto insert_cleanup;
        }
      }
      nx = nx->next_upsert;
    }while( nx!=0 );
  }
#endif


  /* This is the top of the main insertion loop */
  if( use_temp_table ){
    /* This block codes the top of loop only.  The complete loop is the
    ** following pseudocode (template 4):
    **
    **         rewind temp table, if empty goto D
    **      C: loop over rows of intermediate table
    **           transfer values form intermediate table into <table>
    **         end loop
    **      D: ...
    */
    addr_ins_top = __vdbe_add_op1(v, OP_Rewind, src_tab); VDBE_COVERAGE(v);
    addr_cont = __vdbe_current_addr(v);
  }else if( p_select ){
    /* This block codes the top of loop only.  The complete loop is the
    ** following pseudocode (template 3):
    **
    **      C: yield X, at EOF goto D
    **         insert the select result into <table> from R..R+n
    **         goto C
    **      D: ...
    */
    __vdbe_release_registers(parser, reg_data, tab->n_col, 0, 0);
    addr_ins_top = addr_cont = __vdbe_add_op1(v, OP_Yield, dest.i_sd_parm);
    VDBE_COVERAGE(v);
    if( ipk_column>=0 ){
      /* tag-20191021-001: If the INTEGER PRIMARY KEY is being generated by the
      ** SELECT, go ahead and copy the value into the rowid slot now, so that
      ** the value does not get overwritten by a NULL at tag-20191021-002. */
      __vdbe_add_op2(v, OP_Copy, reg_from_select+ipk_column, reg_rowid);
    }
  }

  /* Compute data for ordinary columns of the new entry.  Values
  ** are written in storage order into registers starting with reg_data.
  ** Only ordinary columns are computed in this loop. The rowid
  ** (if there is one) is computed later and generated columns are
  ** computed after the rowid since they might depend on the value
  ** of the rowid.
  */
  n_hidden = 0;
  i_reg_store = reg_data;  assert( reg_data==reg_rowid+1 );
  for(i=0; i<tab->n_col; i++, i_reg_store++){
    int k;
    u_int32_t col_flags;
    assert( i>=n_hidden );
    if( i==tab->i_p_key ){
      /* tag-20191021-002: References to the INTEGER PRIMARY KEY are filled
      ** using the rowid. So put a NULL in the IPK slot of the record to avoid
      ** using excess space.  The file format definition requires this extra
      ** NULL - we cannot optimize further by skipping the column completely */
      __vdbe_add_op1(v, OP_SoftNull, i_reg_store);
      continue;
    }
    if( ((col_flags = tab->cols[i].col_flags) & COLFLAG_NOINSERT)!=0 ){
      n_hidden++;
      if( (col_flags & COLFLAG_VIRTUAL)!=0 ){
        /* Virtual columns do not participate in OP_MakeRecord.  So back up
        ** i_reg_store by one slot to compensate for the i_reg_store++ in the
        ** outer for() loop */
        i_reg_store--;
        continue;
      }else if( (col_flags & COLFLAG_STORED)!=0 ){
        /* Stored columns are computed later.  But if there are BEFORE
        ** triggers, the slots used for stored columns will be OP_Copy-ed
        ** to a second block of registers, so the register needs to be
        ** initialized to NULL to avoid an uninitialized register read */
        if( tmask & TRIGGER_BEFORE ){
          __vdbe_add_op1(v, OP_SoftNull, i_reg_store);
        }
        continue;
      }else if( column==0 ){
        /* Hidden columns that are not explicitly named in the INSERT
        ** get their default value */
        __expr_code_factorable(parser,
            __column_expr(tab, &tab->cols[i]),
            i_reg_store);
        continue;
      }
    }
    if( column ){
      j = tab_col_maps[i];
      assert( j>=0 && j<=column->n_id );   
      if( j==0 ){
        /* A column not named in the insert column list gets its
        ** default value */
        __expr_code_factorable(parser,
            __column_expr(tab, &tab->cols[i]),
            i_reg_store);
        continue;
      }
      k = j - 1;
    }else if( n_column==0 ){
      /* This is INSERT INTO ... DEFAULT VALUES.  Load the default value. */
      __expr_code_factorable(parser,
          __column_expr(tab, &tab->cols[i]),
          i_reg_store);
      continue;
    }else{
      k = i - n_hidden;
    }

    if( use_temp_table ){
      __vdbe_add_op3(v, OP_Column, src_tab, k, i_reg_store);
    }else if( p_select ){
      if( reg_from_select!=reg_data ){
        __vdbe_add_op2(v, OP_SCopy, reg_from_select+k, i_reg_store);
      }
    }else{
      expr_t *x = list->a[k].expr;
      int y = __expr_code_target(parser, x, i_reg_store);
      if( y!=i_reg_store ){
        __vdbe_add_op2(v,
          EXPR_HAS_PROPERTY(x, EP_Subquery) ? OP_Copy : OP_SCopy, y, i_reg_store);
      }
    }
  }


  /* Run the BEFORE and INSTEAD OF triggers, if there are any
  */
  end_of_loop = __vdbe_make_label(parser);
  if( tmask & TRIGGER_BEFORE ){
    int reg_cols = __get_temp_range(parser, tab->n_col+1);

    /* build the NEW.* reference row.  Note that if there is an INTEGER
    ** PRIMARY KEY into which a NULL is being inserted, that NULL will be
    ** translated into a unique ID for the row.  But on a BEFORE trigger,
    ** we do not know what the unique ID will be (because the insert has
    ** not happened yet) so we substitute a rowid of -1
    */
    if( ipk_column<0 ){
      __vdbe_add_op2(v, OP_Integer, -1, reg_cols);
    }else{
      int addr1;
      assert( !without_rowid );
      if( use_temp_table ){
        __vdbe_add_op3(v, OP_Column, src_tab, ipk_column, reg_cols);
      }else{
        assert( p_select==0 );  /* Otherwise use_temp_table is true */
        __expr_code(parser, list->a[ipk_column].expr, reg_cols);
      }
      addr1 = __vdbe_add_op1(v, OP_NotNull, reg_cols); VDBE_COVERAGE(v);
      __vdbe_add_op2(v, OP_Integer, -1, reg_cols);
      __vdbe_jump_here(v, addr1);
      __vdbe_add_op1(v, OP_MustBeInt, reg_cols); VDBE_COVERAGE(v);
    }

    /* Copy the new data already generated. */
    assert( tab->n_nv_col>0 || parser->n_err>0 );
    __vdbe_add_op3(v, OP_Copy, reg_rowid+1, reg_cols+1, tab->n_nv_col-1);

#ifndef DBSQL_OMIT_GENERATED_COLUMNS
    /* Compute the new value for generated columns after all other
    ** columns have already been computed.  This must be done after
    ** computing the ROWID in case one of the generated columns
    ** refers to the ROWID. */
    if( tab->tab_flags & TF_HasGenerated ){
      testcase( tab->tab_flags & TF_HasVirtual );
      testcase( tab->tab_flags & TF_HasStored );
      __compute_generated_columns(parser, reg_cols+1, tab);
    }
#endif

    /* If this is an INSERT on a view with an INSTEAD OF INSERT trigger,
    ** do not attempt any conversions before assembling the record.
    ** If this is a real table, attempt conversions as required by the
    ** table column affinities.
    */
    if( !isView ){
      __table_affinity(v, tab, reg_cols+1);
    }

    /* Fire BEFORE or INSTEAD OF triggers */
    __code_row_trigger(parser, P_TRIGGER, TK_INSERT, 0, TRIGGER_BEFORE,
        tab, reg_cols-tab->n_col-1, on_error, end_of_loop);

    __release_temp_range(parser, reg_cols, tab->n_col+1);
  }

  if( !isView ){
    if( IS_VIRTUAL(tab) ){
      /* The row that the VUpdate opcode will delete: none */
      __vdbe_add_op2(v, OP_Null, 0, reg_ins);
    }
    if( ipk_column>=0 ){
      /* Compute the new rowid */
      if( use_temp_table ){
        __vdbe_add_op3(v, OP_Column, src_tab, ipk_column, reg_rowid);
      }else if( p_select ){
        /* Rowid already initialized at tag-20191021-001 */
      }else{
        expr_t *ipk = list->a[ipk_column].expr;
        if( ipk->op==TK_NULL && !IS_VIRTUAL(tab) ){
          __vdbe_add_op3(v, OP_NewRowid, i_data_cur, reg_rowid, reg_autoinc);
          appendFlag = 1;
        }else{
          __expr_code(parser, list->a[ipk_column].expr, reg_rowid);
        }
      }
      /* If the PRIMARY KEY expression is NULL, then use OP_NewRowid
      ** to generate a unique primary key value.
      */
      if( !appendFlag ){
        int addr1;
        if( !IS_VIRTUAL(tab) ){
          addr1 = __vdbe_add_op1(v, OP_NotNull, reg_rowid); VDBE_COVERAGE(v);
          __vdbe_add_op3(v, OP_NewRowid, i_data_cur, reg_rowid, reg_autoinc);
          __vdbe_jump_here(v, addr1);
        }else{
          addr1 = __vdbe_current_addr(v);
          __vdbe_add_op2(v, OP_IsNull, reg_rowid, addr1+2); VDBE_COVERAGE(v);
        }
        __vdbe_add_op1(v, OP_MustBeInt, reg_rowid); VDBE_COVERAGE(v);
      }
    }else if( IS_VIRTUAL(tab) || without_rowid ){
      __vdbe_add_op2(v, OP_Null, 0, reg_rowid);
    }else{
      __vdbe_add_op3(v, OP_NewRowid, i_data_cur, reg_rowid, reg_autoinc);
      appendFlag = 1;
    }
    AUTO_INC_STEP(parser, reg_autoinc, reg_rowid);

#ifndef DBSQL_OMIT_GENERATED_COLUMNS
    /* Compute the new value for generated columns after all other
    ** columns have already been computed.  This must be done after
    ** computing the ROWID in case one of the generated columns
    ** is derived from the INTEGER PRIMARY KEY. */
    if( tab->tab_flags & TF_HasGenerated ){
      __compute_generated_columns(parser, reg_rowid+1, tab);
    }
#endif

    /* Generate code to check constraints and generate index keys and
    ** do the insertion.
    */
#ifndef DBSQL_OMIT_VIRTUALTABLE
    if( IS_VIRTUAL(tab) ){
      const char *v_tab = (const char *)__get_v_table(db, tab);
      __vtab_make_writable(parser, tab);
      __vdbe_add_op4(v, OP_VUpdate, 1, tab->n_col+2, reg_ins, v_tab, P4_VTAB);
      __vdbe_change_p5(v, on_error==OE_Default ? OE_Abort : on_error);
      __may_abort(parser);
    }else
#endif
    {
      int is_replace = 0;/* Set to true if constraints may cause a replace */
      int use_seek;     /* True to use OPFLAG_SEEKRESULT */
      __generate_constraint_checks(parser, tab, reg_idx, i_data_cur, i_idx_cur,
          reg_ins, 0, ipk_column>=0, on_error, end_of_loop, &is_replace, 0, upsert
      );
      if( db->flags & DBSQL_ForeignKeys ){
        __fk_check(parser, tab, 0, reg_ins, 0, 0);
      }

      /* Set the OPFLAG_USESEEKRESULT flag if either (a) there are no REPLACE
      ** constraints or (b) there are no triggers and this table is not a
      ** parent table in a foreign key constraint. It is safe to set the
      ** flag in the second case as if any REPLACE constraint is hit, an
      ** OP_Delete or OP_IdxDelete instruction will be executed on each
      ** cursor that is disturbed. And these instructions both clear the
      ** vdbe_cursor_t.seek_result variable, disabling the OPFLAG_USESEEKRESULT
      ** functionality.  */
      use_seek = (is_replace==0 || !__vdbe_has_sub_program(v));
      __complete_insertion(parser, tab, i_data_cur, i_idx_cur,
          reg_ins, reg_idx, 0, appendFlag, use_seek
      );
    }
#ifdef DBSQL_ALLOW_ROWID_IN_VIEW
  }else if( parser->returning ){
    /* If there is a RETURNING clause, populate the rowid register with
    ** constant value -1, in case one or more of the returned expressions
    ** refer to the "rowid" of the view.  */
    __vdbe_add_op2(v, OP_Integer, -1, reg_rowid);
#endif
  }

  /* Update the count of rows that are inserted
  */
  if( reg_row_count ){
    __vdbe_add_op2(v, OP_AddImm, reg_row_count, 1);
  }

  if( P_TRIGGER ){
    /* Code AFTER triggers */
    __code_row_trigger(parser, P_TRIGGER, TK_INSERT, 0, TRIGGER_AFTER,
        tab, reg_data-2-tab->n_col, on_error, end_of_loop);
  }

  /* The bottom of the main insertion loop, if the data source
  ** is a SELECT statement.
  */
  __vdbe_resolve_label(v, end_of_loop);
  if( use_temp_table ){
    __vdbe_add_op2(v, OP_Next, src_tab, addr_cont); VDBE_COVERAGE(v);
    __vdbe_jump_here(v, addr_ins_top);
    __vdbe_add_op1(v, OP_Close, src_tab);
  }else if( p_select ){
    __vdbe_goto(v, addr_cont);
#ifdef DBSQL_DEBUG
    /* If we are jumping back to an OP_Yield that is preceded by an
    ** OP_ReleaseReg, set the p5 flag on the OP_Goto so that the
    ** OP_ReleaseReg will be included in the loop. */
    if( __vdbe_get_op(v, addr_cont-1)->opcode==OP_ReleaseReg ){
      assert( __vdbe_get_op(v, addr_cont)->opcode==OP_Yield );
      __vdbe_change_p5(v, 1);
    }
#endif
    __vdbe_jump_here(v, addr_ins_top);
  }

#ifndef DBSQL_OMIT_XFER_OPT
insert_end:
#endif /* DBSQL_OMIT_XFER_OPT */
  /* Update the dbsql_sequence table by storing the content of the
  ** maximum rowid counter values recorded while inserting into
  ** autoincrement tables.
  */
  if( parser->nested==0 && parser->trigger_tab==0 ){
    __autoincrement_end(parser);
  }

  /*
  ** Return the number of rows inserted. If this routine is
  ** generating code because of a call to __nested_parse(), do not
  ** invoke the callback function.
  */
  if( reg_row_count ){
    __code_change_count(v, reg_row_count, "rows inserted");
  }

insert_cleanup:
  __src_list_delete(db, tab_list);
  __expr_list_delete(db, list);
  __upsert_delete(db, upsert);
  __select_delete(db, p_select);
  if( column ){
    __id_list_delete(db, column);
    __dbsql_db_free(db, tab_col_maps);
  }
  if( reg_idx ) __db_nn_free_nn(db, reg_idx);
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
#ifdef tmask
 #undef tmask
#endif

/*
** Meanings of bits in of walker->code for
** __expr_references_updated_column()
*/
#define CKCNSTRNT_COLUMN   0x01    /* CHECK constraint uses a changing column */
#define CKCNSTRNT_ROWID    0x02    /* CHECK constraint references the ROWID */

/* This is the walker_t callback from __expr_references_updated_column().
*  Set bit 0x01 of walker->code if walker->code to 0 and if this
** expression node references any of the
** columns that are being modified by an UPDATE statement.
*/
static int __check_constraint_expr_node(walker_t *walker, expr_t *expr){
  if( expr->op==TK_COLUMN ){
    assert( expr->i_column>=0 || expr->i_column==-1 );
    if( expr->i_column>=0 ){
      if( walker->u.cols[expr->i_column]>=0 ){
        walker->code |= CKCNSTRNT_COLUMN;
      }
    }else{
      walker->code |= CKCNSTRNT_ROWID;
    }
  }
  return WRC_Continue;
}

/*
** expr is a CHECK constraint on a row that is being UPDATE-ed.  The
** only columns that are modified by the UPDATE are those for which
** chngs[i]>=0, and also the ROWID is modified if chng_rowid is true.
**
** Return true if CHECK constraint expr uses any of the
** changing columns (or the rowid if it is changing).  In other words,
** return true if this CHECK constraint must be validated for
** the new row in the UPDATE statement.
**
** 2018-09-15: expr might also be an expression for an index-on-expressions.
** The operation of this routine is the same - return true if an only if
** the expression uses one or more of columns identified by the second and
** third arguments.
*/
int __expr_references_updated_column(
  expr_t *expr,    /* The expression to be checked */
  int *chngs,    /* chngs[x]>=0 if column x changed by the UPDATE */
  int chng_rowid   /* True if UPDATE changes the rowid */
){
  walker_t w;
  memset(&w, 0, sizeof(w));
  w.code = 0;
  w.expr_callback = __check_constraint_expr_node;
  w.u.cols = chngs;
  __walk_expr(&w, expr);
  if( !chng_rowid ){
    testcase( (w.code & CKCNSTRNT_ROWID)!=0 );
    w.code &= ~CKCNSTRNT_ROWID;
  }
  testcase( w.code==0 );
  testcase( w.code==CKCNSTRNT_COLUMN );
  testcase( w.code==CKCNSTRNT_ROWID );
  testcase( w.code==(CKCNSTRNT_ROWID|CKCNSTRNT_COLUMN) );
  return w.code!=0;
}

/*
** The __generate_constraint_checks() routine usually wants to visit
** the indexes of a table in the order provided in the table_t->p_index list.
** However, sometimes (rarely - when there is an upsert) it wants to visit
** the indexes in a different order.  The following data structures accomplish
** this.
**
** The index_iterator_t object is used to walk through all of the indexes
** of a table in either index_t.next order, or in some other order established
** by an array of index_list_term_t objects.
*/
typedef struct index_list_term_t index_list_term_t;
typedef struct index_iterator_t index_iterator_t;
struct index_iterator_t {
  int type;    /* 0 for index_t.next list.  1 for an array of index_list_term_t */
  int i;        /* index_t of the current item from the list */
  union {
    struct {    /* Use this object for type==0: A index_t.next list */
      index_t *idx;   /* The current index_t */
    } lx;     
    struct {    /* Use this object for type==1; Array of index_list_term_t */
      int n_idx;               /* Size of the array */
      index_list_term_t *idx;    /* Array of __index_list_terms */
    } ax;
  } u;
};

/* When index_iterator_t.type==1, then each index is an array of instances
** of the following object
*/
struct index_list_term_t {
  index_t *p;  /* The index */
  int ix;    /* Which entry in the original table_t.p_index list is this index*/
};

/* Return the first index on the list */
static index_t *__index_iterator_first(index_iterator_t *iter, int *ix){
  assert( iter->i==0 );
  if( iter->type ){
    *ix = iter->u.ax.idx[0].ix;
    return iter->u.ax.idx[0].p;
  }else{
    *ix = 0;
    return iter->u.lx.idx;
  }
}

/* Return the next index from the list.  Return NULL when out of indexes */
static index_t *__index_iterator_next(index_iterator_t *iter, int *ix){
  if( iter->type ){
    int i = ++iter->i;
    if( i>=iter->u.ax.n_idx ){
      *ix = i;
      return 0;
    }
    *ix = iter->u.ax.idx[i].ix;
    return iter->u.ax.idx[i].p;
  }else{
    ++(*ix);
    iter->u.lx.idx = iter->u.lx.idx->next;
    return iter->u.lx.idx;
  }
}
 
/*
** Generate code to do constraint checks prior to an INSERT or an UPDATE
** on table tab.
**
** The reg_new_data parameter is the first register in a range that contains
** the data to be inserted or the data after the update.  There will be
** tab->n_col+1 registers in this range.  The first register (the one
** that reg_new_data points to) will contain the new rowid, or NULL in the
** case of a WITHOUT ROWID table.  The second register in the range will
** contain the content of the first table column.  The third register will
** contain the content of the second table column.  And so forth.
**
** The reg_old_data parameter is similar to reg_new_data except that it contains
** the data prior to an UPDATE rather than afterwards.  reg_old_data is zero
** for an INSERT.  This routine can distinguish between UPDATE and INSERT by
** checking reg_old_data for zero.
**
** For an UPDATE, the pk_chng boolean is true if the true primary key (the
** rowid for a normal table or the PRIMARY KEY for a WITHOUT ROWID table)
** might be modified by the UPDATE.  If pk_chng is false, then the key of
** the i_data_cur content table is guaranteed to be unchanged by the UPDATE.
**
** For an INSERT, the pk_chng boolean indicates whether or not the rowid
** was explicitly specified as part of the INSERT statement.  If pk_chng
** is zero, it means that the either rowid is computed automatically or
** that the table is a WITHOUT ROWID table and has no rowid.  On an INSERT,
** pk_chng will only be true if the INSERT statement provides an integer
** value for either the rowid column or its INTEGER PRIMARY KEY alias.
**
** The code generated by this routine will store new index entries into
** registers identified by reg_idx[].  No index entry is created for
** indices where reg_idx[i]==0.  The order of indices in reg_idx[] is
** the same as the order of indices on the linked list of indices
** at tab->p_index.
**
** (2019-05-07) The generated code also creates a new record for the
** main table, if tab is a rowid table, and stores that record in the
** register identified by reg_idx[n_idx] - in other words in the first
** entry of reg_idx[] past the last index.  It is important that the
** record be generated during constraint checks to avoid affinity changes
** to the register content that occur after constraint checks but before
** the new record is inserted.
**
** The caller must have already opened writeable cursors on the main
** table and all applicable indices (that is to say, all indices for which
** reg_idx[] is not zero).  i_data_cur is the cursor for the main table when
** inserting or updating a rowid table, or the cursor for the PRIMARY KEY
** index when operating on a WITHOUT ROWID table.  i_idx_cur is the cursor
** for the first index in the tab->p_index list.  Cursors for other indices
** are at i_idx_cur+N for the N-th element of the tab->p_index list.
**
** This routine also generates code to check constraints.  NOT NULL,
** CHECK, and UNIQUE constraints are all checked.  If a constraint fails,
** then the appropriate action is performed.  There are five possible
** actions: ROLLBACK, ABORT, FAIL, REPLACE, and IGNORE.
**
**  Constraint type  Action       What Happens
**  ---------------  ----------   ----------------------------------------
**  any              ROLLBACK     The current transaction is rolled back and
**                                dbsql_step() returns immediately with a
**                                return code of DBSQL_CONSTRAINT.
**
**  any              ABORT        Back out changes from the current command
**                                only (do not do a complete rollback) then
**                                cause dbsql_step() to return immediately
**                                with DBSQL_CONSTRAINT.
**
**  any              FAIL         Sqlite3_step() returns immediately with a
**                                return code of DBSQL_CONSTRAINT.  The
**                                transaction is not rolled back and any
**                                changes to prior rows are retained.
**
**  any              IGNORE       The attempt in insert or update the current
**                                row is skipped, without throwing an error.
**                                Processing continues with the next row.
**                                (There is an immediate jump to ignore_dest.)
**
**  NOT NULL         REPLACE      The NULL value is replace by the default
**                                value for that column.  If the default value
**                                is NULL, the action is the same as ABORT.
**
**  UNIQUE           REPLACE      The other row that conflicts with the row
**                                being inserted is removed.
**
**  CHECK            REPLACE      Illegal.  The results in an exception.
**
** Which action to take is determined by the override_error parameter.
** Or if override_error==OE_Default, then the parser->on_error parameter
** is used.  Or if parser->on_error==OE_Default then the on_error value
** for the constraint is used.
*/
void __generate_constraint_checks(
  parser_t *parser,       /* The parser context */
  table_t *tab,         /* The table being inserted or updated */
  int *a_reg_idx,        /* Use register reg_idx[i] for index i.  0 for unused */
  int i_data_cur,        /* Canonical data cursor (main table or PK index) */
  int i_idx_cur,         /* First index cursor */
  int reg_new_data,      /* First register in a range holding values to insert */
  int reg_old_data,      /* Previous content.  0 for INSERTs */
  u_int8_t pk_chng,           /* Non-zero if the rowid or PRIMARY KEY changed */
  u_int8_t override_error,    /* Override on_error to this if not OE_Default */
  int ignore_dest,      /* Jump to this label on an OE_Ignore resolution */
  int *pb_may_replace,   /* OUT: Set to true if constraint may cause a replace */
  int *chngs,         /* column i is unchanged if chngs[i]<0 */
  upsert_t *upsert      /* ON CONFLICT clauses, if any.  NULL otherwise */
){
  vdbe_t *v;             /* VDBE under construction */
  index_t *idx;         /* Pointer to one of the indices */
  index_t *pk = 0;      /* The PRIMARY KEY index for WITHOUT ROWID tables */
  DBSQL *db;         /* Database connection */
  int i;               /* loop counter */
  int ix;              /* index_t loop counter */
  int n_col;            /* Number of columns */
  int on_error;         /* Conflict resolution strategy */
  int seen_replace = 0; /* True if REPLACE is used to resolve INT PK conflict */
  int n_pk_field;        /* Number of fields in PRIMARY KEY. 1 for ROWID tables */
  upsert_t *upsert_clause = 0;  /* The specific ON CONFLICT clause for idx */
  u_int8_t is_update;           /* True if this is an UPDATE operation */
  u_int8_t affinity_done = 0;  /* True if the OP_Affinity operation has been run */
  int upsert_ipk_return = 0; /* Address of Goto at end of IPK uniqueness check */
  int upsert_ipk_delay = 0;  /* Address of Goto to bypass initial IPK check */
  int ipk_top = 0;        /* Top of the IPK uniqueness check */
  int ipk_bottom = 0;     /* OP_Goto at the end of the IPK uniqueness check */
  /* Variables associated with retesting uniqueness constraints after
  ** replace triggers fire have run */
  int reg_trig_cnt;       /* Register used to count replace trigger invocations */
  int addr_recheck = 0;  /* Jump here to recheck all uniqueness constraints */
  int lbl_recheck_ok = 0; /* Each recheck jumps to this label if it passes */
  trigger_t *P_TRIGGER;    /* List of DELETE triggers on the table tab */
  int n_replace_trig = 0; /* Number of replace triggers coded */
  index_iterator_t s_idx_iter;  /* index_t iterator */

  is_update = reg_old_data!=0;
  db = parser->db;
  v = parser->vdbe;
  assert( v!=0 );
  assert( !IsView(tab) );  /* This table is not a VIEW */
  n_col = tab->n_col;
 
  /* pk is the PRIMARY KEY index for WITHOUT ROWID tables and NULL for
  ** normal rowid tables.  n_pk_field is the number of key fields in the
  ** pk index or 1 for a rowid table.  In other words, n_pk_field is the
  ** number of fields in the true primary key of the table. */
  if( HAS_ROWID(tab) ){
    pk = 0;
    n_pk_field = 1;
  }else{
    pk = __primary_key_index(tab);
    n_pk_field = pk->n_key_col;
  }

  /* Record that this module has started */
  VDBE_MODULE_COMMENT((v, "BEGIN: GenCnstCks(%d,%d,%d,%d,%d)",
                     i_data_cur, i_idx_cur, reg_new_data, reg_old_data, pk_chng));

  /* Test all NOT NULL constraints.
  */
  if( tab->tab_flags & TF_HasNotNull ){
    int b2nd_pass = 0;         /* True if currently running 2nd pass */
    int n_seen_replace = 0;     /* Number of ON CONFLICT REPLACE operations */
    int n_generated = 0;       /* Number of generated columns with NOT NULL */
    while(1){  /* Make 2 passes over columns. Exit loop via "break" */
      for(i=0; i<n_col; i++){
        int i_reg;                        /* Register holding column value */
        column_t *col = &tab->cols[i];   /* The column to check for NOT NULL */
        int is_generated;                 /* non-zero if column is generated */
        on_error = col->not_null;
        if( on_error==OE_None ) continue; /* No NOT NULL on this column */
        if( i==tab->i_p_key ){
          continue;        /* ROWID is never NULL */
        }
        is_generated = col->col_flags & COLFLAG_GENERATED;
        if( is_generated && !b2nd_pass ){
          n_generated++;
          continue;        /* Generated columns processed on 2nd pass */
        }
        if( chngs && chngs[i]<0 && !is_generated ){
          /* Do not check NOT NULL on columns that do not change */
          continue;
        }
        if( override_error!=OE_Default ){
          on_error = override_error;
        }else if( on_error==OE_Default ){
          on_error = OE_Abort;
        }
        if( on_error==OE_Replace ){
          if( b2nd_pass        /* REPLACE becomes ABORT on the 2nd pass */
           || col->i_dflt==0  /* REPLACE is ABORT if no DEFAULT value */
          ){
            testcase( col->col_flags & COLFLAG_VIRTUAL );
            testcase( col->col_flags & COLFLAG_STORED );
            testcase( col->col_flags & COLFLAG_GENERATED );
            on_error = OE_Abort;
          }else{
            assert( !is_generated );
          }
        }else if( b2nd_pass && !is_generated ){
          continue;
        }
        assert( on_error==OE_Rollback || on_error==OE_Abort || on_error==OE_Fail
            || on_error==OE_Ignore || on_error==OE_Replace );
        testcase( i!=__table_column_to_storage(tab, i) );
        i_reg = __table_column_to_storage(tab, i) + reg_new_data + 1;
        switch( on_error ){
          case OE_Replace: {
            int addr1 = __vdbe_add_op1(v, OP_NotNull, i_reg);
            VDBE_COVERAGE(v);
            assert( (col->col_flags & COLFLAG_GENERATED)==0 );
            n_seen_replace++;
            __expr_code_copy(parser,
               __column_expr(tab, col), i_reg);
            __vdbe_jump_here(v, addr1);
            break;
          }
          case OE_Abort:
            __may_abort(parser);
            /* no break */ deliberate_fall_through
          case OE_Rollback:
          case OE_Fail: {
            char *msg = __m_printf(db, "%s.%s", tab->name,
                                        col->cn_name);
            testcase( msg==0 && db->malloc_failed==0 );
            __vdbe_add_op3(v, OP_HaltIfNull, DBSQL_CONSTRAINT_NOTNULL,
                              on_error, i_reg);
            __vdbe_append_p4(v, msg, P4_DYNAMIC);
            __vdbe_change_p5(v, P5_ConstraintNotNull);
            VDBE_COVERAGE(v);
            break;
          }
          default: {
            assert( on_error==OE_Ignore );
            __vdbe_add_op2(v, OP_IsNull, i_reg, ignore_dest);
            VDBE_COVERAGE(v);
            break;
          }
        } /* end switch(on_error) */
      } /* end loop i over columns */
      if( n_generated==0 && n_seen_replace==0 ){
        /* If there are no generated columns with NOT NULL constraints
        ** and no NOT NULL ON CONFLICT REPLACE constraints, then a single
        ** pass is sufficient */
        break;
      }
      if( b2nd_pass ) break;  /* Never need more than 2 passes */
      b2nd_pass = 1;
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
      if( n_seen_replace>0 && (tab->tab_flags & TF_HasGenerated)!=0 ){
        /* If any NOT NULL ON CONFLICT REPLACE constraints fired on the
        ** first pass, recomputed values for all generated columns, as
        ** those values might depend on columns affected by the REPLACE.
        */
        __compute_generated_columns(parser, reg_new_data+1, tab);
      }
#endif
    } /* end of 2-pass loop */
  } /* end if( has-not-null-constraints ) */

  /* Test all CHECK constraints
  */
#ifndef DBSQL_OMIT_CHECK
  if( tab->check && (db->flags & DBSQL_IgnoreChecks)==0 ){
    expr_list_t *check = tab->check;
    parser->i_self_tab = -(reg_new_data+1);
    on_error = override_error!=OE_Default ? override_error : OE_Abort;
    for(i=0; i<check->n_expr; i++){
      int all_ok;
      expr_t *copy;
      expr_t *expr = check->a[i].expr;
      if( chngs
       && !__expr_references_updated_column(expr, chngs, pk_chng)
      ){
        /* The check constraints do not reference any of the columns being
        ** updated so there is no point it verifying the check constraint */
        continue;
      }
      if( affinity_done==0 ){
        __table_affinity(v, tab, reg_new_data+1);
        affinity_done = 1;
      }
      all_ok = __vdbe_make_label(parser);
      __vdbe_verify_abortable(v, on_error);
      copy = __expr_dup(db, expr, 0);
      if( !db->malloc_failed ){
        __expr_if_true(parser, copy, all_ok, DBSQL_JUMPIFNULL);
      }
      __expr_delete(db, copy);
      if( on_error==OE_Ignore ){
        __vdbe_goto(v, ignore_dest);
      }else{
        char *name = check->a[i].e_name;
        assert( name!=0 || parser->db->malloc_failed );
        if( on_error==OE_Replace ) on_error = OE_Abort; /* IMP: R-26383-51744 */
        __halt_constraint(parser, DBSQL_CONSTRAINT_CHECK,
                              on_error, name, P4_TRANSIENT,
                              P5_ConstraintCheck);
      }
      __vdbe_resolve_label(v, all_ok);
    }
    parser->i_self_tab = 0;
  }
#endif /* !defined(DBSQL_OMIT_CHECK) */

  /* UNIQUE and PRIMARY KEY constraints should be handled in the following
  ** order:
  **
  **   (1)  OE_Update
  **   (2)  OE_Abort, OE_Fail, OE_Rollback, OE_Ignore
  **   (3)  OE_Replace
  **
  ** OE_Fail and OE_Ignore must happen before any changes are made.
  ** OE_Update guarantees that only a single row will change, so it
  ** must happen before OE_Replace.  Technically, OE_Abort and OE_Rollback
  ** could happen in any order, but they are grouped up front for
  ** convenience.
  **
  ** 2018-08-14: Ticket https://sqlite.org/src/info/908f001483982c43
  ** The order of constraints used to have OE_Update as (2) and OE_Abort
  ** and so forth as (1). But apparently __postgre_sql checks the OE_Update
  ** constraint before any others, so it had to be moved.
  **
  ** Constraint checking code is generated in this order:
  **   (A)  The rowid constraint
  **   (B)  Unique index constraints that do not have OE_Replace as their
  **        default conflict resolution strategy
  **   (C)  Unique index that do use OE_Replace by default.
  **
  ** The ordering of (2) and (3) is accomplished by making sure the linked
  ** list of indexes attached to a table puts all OE_Replace indexes last
  ** in the list.  See __create_index() for where that happens.
  */
  s_idx_iter.type = 0;
  s_idx_iter.i = 0;
  s_idx_iter.u.ax.idx = 0;  /* Silence harmless compiler warning */
  s_idx_iter.u.lx.idx = tab->p_index;
  if( upsert ){
    if( upsert->upsert_target==0 ){
      /* There is just on ON CONFLICT clause and it has no constraint-target */
      assert( upsert->next_upsert==0 );
      if( upsert->is_do_update==0 ){
        /* A single ON CONFLICT DO NOTHING clause, without a constraint-target.
        ** Make all unique constraint resolution be OE_Ignore */
        override_error = OE_Ignore;
        upsert = 0;
      }else{
        /* A single ON CONFLICT DO UPDATE.  Make all resolutions OE_Update */
        override_error = OE_Update;
      }
    }else if( tab->p_index!=0 ){
      /* Otherwise, we'll need to run the index_list_term_t array version of the
      ** iterator to ensure that all of the ON CONFLICT conditions are
      ** checked first and in order. */
      int n_idx, jj;
      u_int64_t n_byte;
      upsert_t *term;
      u_int8_t *used;
      for(n_idx=0, idx=tab->p_index; idx; idx=idx->next, n_idx++){
         assert( a_reg_idx[n_idx]>0 );
      }
      s_idx_iter.type = 1;
      s_idx_iter.u.ax.n_idx = n_idx;
      n_byte = (sizeof(index_list_term_t)+1)*n_idx + n_idx;
      s_idx_iter.u.ax.idx = __db_malloc_zero(db, n_byte);
      if( s_idx_iter.u.ax.idx==0 ) return; /* OOM */
      used = (u_int8_t*)&s_idx_iter.u.ax.idx[n_idx];
      upsert->to_free = s_idx_iter.u.ax.idx;
      for(i=0, term=upsert; term; term=term->next_upsert){
        if( term->upsert_target==0 ) break;
        if( term->upsert_idx==0 ) continue;  /* Skip ON CONFLICT for the IPK */
        jj = 0;
        idx = tab->p_index;
        while( ALWAYS(idx!=0) && idx!=term->upsert_idx ){
           idx = idx->next;
           jj++;
        }
        if( used[jj] ) continue; /* Duplicate ON CONFLICT clause ignored */
        used[jj] = 1;
        s_idx_iter.u.ax.idx[i].p = idx;
        s_idx_iter.u.ax.idx[i].ix = jj;
        i++;
      }
      for(jj=0, idx=tab->p_index; idx; idx=idx->next, jj++){
        if( used[jj] ) continue;
        s_idx_iter.u.ax.idx[i].p = idx;
        s_idx_iter.u.ax.idx[i].ix = jj;
        i++;
      }
      assert( i==n_idx );
    }
  }

  /* Determine if it is possible that triggers (either explicitly coded
  ** triggers or FK resolution actions) might run as a result of deletes
  ** that happen when OE_Replace conflict resolution occurs. (Call these
  ** "replace triggers".)  If any replace triggers run, we will need to
  ** recheck all of the uniqueness constraints after they have all run.
  ** But on the recheck, the resolution is OE_Abort instead of OE_Replace.
  **
  ** If replace triggers are a possibility, then
  **
  **   (1) Allocate register reg_trig_cnt and initialize it to zero.
  **       That register will count the number of replace triggers that
  **       fire.  Constraint recheck only occurs if the number is positive.
  **   (2) Initialize P_TRIGGER to the list of all DELETE triggers on tab.
  **   (3) Initialize addr_recheck and lbl_recheck_ok
  **
  ** The uniqueness rechecking code will create a series of tests to run
  ** in a second pass.  The addr_recheck and lbl_recheck_ok variables are
  ** used to link together these tests which are separated from each other
  ** in the generate bytecode.
  */
  if( (db->flags & (DBSQL_RecTriggers|DBSQL_ForeignKeys))==0 ){
    /* There are not DELETE triggers nor FK constraints.  No constraint
    ** rechecks are needed. */
    P_TRIGGER = 0;
    reg_trig_cnt = 0;
  }else{
    if( db->flags&DBSQL_RecTriggers ){
      P_TRIGGER = __triggers_exist(parser, tab, TK_DELETE, 0, 0);
      reg_trig_cnt = P_TRIGGER!=0 || __fk_required(parser, tab, 0, 0);
    }else{
      P_TRIGGER = 0;
      reg_trig_cnt = __fk_required(parser, tab, 0, 0);
    }
    if( reg_trig_cnt ){
      /* Replace triggers might exist.  Allocate the counter and
      ** initialize it to zero. */
      reg_trig_cnt = ++parser->n_mem;
      __vdbe_add_op2(v, OP_Integer, 0, reg_trig_cnt);
      VDBE_COMMENT((v, "trigger count"));
      lbl_recheck_ok = __vdbe_make_label(parser);
      addr_recheck = lbl_recheck_ok;
    }
  }

  /* If rowid is changing, make sure the new rowid does not previously
  ** exist in the table.
  */
  if( pk_chng && pk==0 ){
    int addr_rowid_ok = __vdbe_make_label(parser);

    /* Figure out what action to take in case of a rowid collision */
    on_error = tab->key_conf;
    if( override_error!=OE_Default ){
      on_error = override_error;
    }else if( on_error==OE_Default ){
      on_error = OE_Abort;
    }

    /* figure out whether or not upsert applies in this case */
    if( upsert ){
      upsert_clause = __upsert_of_index(upsert,0);
      if( upsert_clause!=0 ){
        if( upsert_clause->is_do_update==0 ){
          on_error = OE_Ignore;  /* DO NOTHING is the same as INSERT OR IGNORE */
        }else{
          on_error = OE_Update;  /* DO UPDATE */
        }
      }
      if( upsert_clause!=upsert ){
        /* The first ON CONFLICT clause has a conflict target other than
        ** the IPK.  We have to jump ahead to that first ON CONFLICT clause
        ** and then come back here and deal with the IPK afterwards */
        upsert_ipk_delay = __vdbe_add_op0(v, OP_Goto);
      }
    }

    /* If the response to a rowid conflict is REPLACE but the response
    ** to some other UNIQUE constraint is FAIL or IGNORE, then we need
    ** to defer the running of the rowid conflict checking until after
    ** the UNIQUE constraints have run.
    */
    if( on_error==OE_Replace      /* IPK rule is REPLACE */
     && on_error!=override_error   /* Rules for other constraints are different */
     && tab->p_index             /* There exist other constraints */
     && !upsert_ipk_delay          /* IPK check already deferred by UPSERT */
    ){
      ipk_top = __vdbe_add_op0(v, OP_Goto)+1;
      VDBE_COMMENT((v, "defer IPK REPLACE until last"));
    }

    if( is_update ){
      /* pk_chng!=0 does not mean that the rowid has changed, only that
      ** it might have changed.  Skip the conflict logic below if the rowid
      ** is unchanged. */
      __vdbe_add_op3(v, OP_Eq, reg_new_data, addr_rowid_ok, reg_old_data);
      __vdbe_change_p5(v, DBSQL_NOTNULL);
      VDBE_COVERAGE(v);
    }

    /* Check to see if the new rowid already exists in the table.  Skip
    ** the following conflict logic if it does not. */
    VDBE_NOOP_COMMENT((v, "uniqueness check for ROWID"));
    __vdbe_verify_abortable(v, on_error);
    __vdbe_add_op3(v, OP_NotExists, i_data_cur, addr_rowid_ok, reg_new_data);
    VDBE_COVERAGE(v);

    switch( on_error ){
      default: {
        on_error = OE_Abort;
        /* no break */ deliberate_fall_through
      }
      case OE_Rollback:
      case OE_Abort:
      case OE_Fail: {
        testcase( on_error==OE_Rollback );
        testcase( on_error==OE_Abort );
        testcase( on_error==OE_Fail );
        __rowid_constraint(parser, on_error, tab);
        break;
      }
      case OE_Replace: {
        /* If there are DELETE triggers on this table and the
        ** recursive-triggers flag is set, call __generate_row_delete() to
        ** remove the conflicting row from the table. This will fire
        ** the triggers and remove both the table and index b-tree entries.
        **
        ** Otherwise, if there are no triggers or the recursive-triggers
        ** flag is not set, but the table has one or more indexes, call
        ** __generate_row_index_delete(). This removes the index b-tree entries
        ** only. The table b-tree entry will be replaced by the new entry
        ** when it is inserted. 
        **
        ** If either __generate_row_delete() or __generate_row_index_delete() is called,
        ** also invoke __multi_write() to indicate that this VDBE may require
        ** statement rollback (if the statement is aborted after the delete
        ** takes place). Earlier versions called __multi_write() regardless,
        ** but being more selective here allows statements like:
        **
        **   REPLACE INTO t(rowid) VALUES($newrowid)
        **
        ** to run without a statement journal if there are no indexes on the
        ** table.
        */
        if( reg_trig_cnt ){
          __multi_write(parser);
          __generate_row_delete(parser, tab, P_TRIGGER, i_data_cur, i_idx_cur,
                                   reg_new_data, 1, 0, OE_Replace, 1, -1);
          __vdbe_add_op2(v, OP_AddImm, reg_trig_cnt, 1); /* incr trigger cnt */
          n_replace_trig++;
        }else{
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
          assert( HAS_ROWID(tab) );
          /* This OP_Delete opcode fires the pre-update-hook only. It does
          ** not modify the b-tree. It is more efficient to let the coming
          ** OP_Insert replace the existing entry than it is to delete the
          ** existing entry and then insert a new one. */
          __vdbe_add_op2(v, OP_Delete, i_data_cur, OPFLAG_ISNOOP);
          __vdbe_append_p4(v, tab, P4_TABLE);
#endif /* DBSQL_ENABLE_PREUPDATE_HOOK */
          if( tab->p_index ){
            __multi_write(parser);
            __generate_row_index_delete(parser, tab, i_data_cur, i_idx_cur,0,-1);
          }
        }
        seen_replace = 1;
        break;
      }
#ifndef DBSQL_OMIT_UPSERT
      case OE_Update: {
        __upsert_do_update(parser, upsert, tab, 0, i_data_cur);
        /* no break */ deliberate_fall_through
      }
#endif
      case OE_Ignore: {
        testcase( on_error==OE_Ignore );
        __vdbe_goto(v, ignore_dest);
        break;
      }
    }
    __vdbe_resolve_label(v, addr_rowid_ok);
    if( upsert && upsert_clause!=upsert ){
      upsert_ipk_return = __vdbe_add_op0(v, OP_Goto);
    }else if( ipk_top ){
      ipk_bottom = __vdbe_add_op0(v, OP_Goto);
      __vdbe_jump_here(v, ipk_top-1);
    }
  }

  /* Test all UNIQUE constraints by creating entries for each UNIQUE
  ** index and making sure that duplicate entries do not already exist.
  ** Compute the revised record entries for indices as we go.
  **
  ** This loop also handles the case of the PRIMARY KEY index for a
  ** WITHOUT ROWID table.
  */
  for(idx = __index_iterator_first(&s_idx_iter, &ix);
      idx;
      idx = __index_iterator_next(&s_idx_iter, &ix)
  ){
    int reg_idx;          /* Range of registers holding content for idx */
    int reg_r;            /* Range of registers holding conflicting PK */
    int i_this_cur;        /* Cursor for this UNIQUE index */
    int addr_unique_ok;    /* Jump here if the UNIQUE constraint is satisfied */
    int addr_conflict_ck;  /* First opcode in the conflict check logic */

    if( a_reg_idx[ix]==0 ) continue;  /* Skip indices that do not change */
    if( upsert ){
      upsert_clause = __upsert_of_index(upsert, idx);
      if( upsert_ipk_delay && upsert_clause==upsert ){
        __vdbe_jump_here(v, upsert_ipk_delay);
      }
    }
    addr_unique_ok = __vdbe_make_label(parser);
    if( affinity_done==0 ){
      __table_affinity(v, tab, reg_new_data+1);
      affinity_done = 1;
    }
    VDBE_NOOP_COMMENT((v, "prep index %s", idx->name));
    i_this_cur = i_idx_cur+ix;


    /* Skip partial indices for which the WHERE clause is not true */
    if( idx->part_idx_where ){
      __vdbe_add_op2(v, OP_Null, 0, a_reg_idx[ix]);
      parser->i_self_tab = -(reg_new_data+1);
      __expr_if_false_dup(parser, idx->part_idx_where, addr_unique_ok,
                            DBSQL_JUMPIFNULL);
      parser->i_self_tab = 0;
    }

    /* Create a record for this index entry as it should appear after
    ** the insert or update.  Store that record in the reg_idx[ix] register
    */
    reg_idx = a_reg_idx[ix]+1;
    for(i=0; i<idx->n_column; i++){
      int i_field = idx->columns[i];
      int x;
      if( i_field==XN_EXPR ){
        parser->i_self_tab = -(reg_new_data+1);
        __expr_code_copy(parser, idx->col_exprs->a[i].expr, reg_idx+i);
        parser->i_self_tab = 0;
        VDBE_COMMENT((v, "%s column %d", idx->name, i));
      }else if( i_field==XN_ROWID || i_field==tab->i_p_key ){
        x = reg_new_data;
        __vdbe_add_op2(v, OP_IntCopy, x, reg_idx+i);
        VDBE_COMMENT((v, "rowid"));
      }else{
        testcase( __table_column_to_storage(tab, i_field)!=i_field );
        x = __table_column_to_storage(tab, i_field) + reg_new_data + 1;
        __vdbe_add_op2(v, OP_SCopy, x, reg_idx+i);
        VDBE_COMMENT((v, "%s", tab->cols[i_field].cn_name));
      }
    }
    __vdbe_add_op3(v, OP_MakeRecord, reg_idx, idx->n_column, a_reg_idx[ix]);
    VDBE_COMMENT((v, "for %s", idx->name));
#ifdef DBSQL_ENABLE_NULL_TRIM
    if( idx->idx_type==DBSQL_IDXTYPE_PRIMARYKEY ){
      __set_make_record_p5(v, idx->table);
    }
#endif
    __vdbe_release_registers(parser, reg_idx, idx->n_column, 0, 0);

    /* In an UPDATE operation, if this index is the PRIMARY KEY index
    ** of a WITHOUT ROWID table and there has been no change the
    ** primary key, then no collision is possible.  The collision detection
    ** logic below can all be skipped. */
    if( is_update && pk==idx && pk_chng==0 ){
      __vdbe_resolve_label(v, addr_unique_ok);
      continue;
    }

    /* Find out what action to take in case there is a uniqueness conflict */
    on_error = idx->on_error;
    if( on_error==OE_None ){
      __vdbe_resolve_label(v, addr_unique_ok);
      continue;  /* idx is not a UNIQUE index */
    }
    if( override_error!=OE_Default ){
      on_error = override_error;
    }else if( on_error==OE_Default ){
      on_error = OE_Abort;
    }

    /* Figure out if the upsert clause applies to this index */
    if( upsert_clause ){
      if( upsert_clause->is_do_update==0 ){
        on_error = OE_Ignore;  /* DO NOTHING is the same as INSERT OR IGNORE */
      }else{
        on_error = OE_Update;  /* DO UPDATE */
      }
    }

    /* Collision detection may be omitted if all of the following are true:
    **   (1) The conflict resolution algorithm is REPLACE
    **   (2) The table is a WITHOUT ROWID table
    **   (3) There are no secondary indexes on the table
    **   (4) No delete triggers need to be fired if there is a conflict
    **   (5) No FK constraint counters need to be updated if a conflict occurs.
    **
    ** This is not possible for ENABLE_PREUPDATE_HOOK builds, as the row
    ** must be explicitly deleted in order to ensure any pre-update hook
    ** is invoked.  */
    assert( IS_ORDINARY_TABLE(tab) );
#ifndef DBSQL_ENABLE_PREUPDATE_HOOK
    if( (ix==0 && idx->next==0)                   /* Condition 3 */
     && pk==idx                                   /* Condition 2 */
     && on_error==OE_Replace                         /* Condition 1 */
     && ( 0==(db->flags&DBSQL_RecTriggers) ||      /* Condition 4 */
          0==__triggers_exist(parser, tab, TK_DELETE, 0, 0))
     && ( 0==(db->flags&DBSQL_ForeignKeys) ||      /* Condition 5 */
         (0==tab->u.tab.f_key && 0==__fk_references(tab)))
    ){
      __vdbe_resolve_label(v, addr_unique_ok);
      continue;
    }
#endif /* ifndef DBSQL_ENABLE_PREUPDATE_HOOK */

    /* Check to see if the new index entry will be unique */
    __vdbe_verify_abortable(v, on_error);
    addr_conflict_ck =
      __vdbe_add_op4_int(v, OP_NoConflict, i_this_cur, addr_unique_ok,
                           reg_idx, idx->n_key_col); VDBE_COVERAGE(v);

    /* Generate code to handle collisions */
    reg_r = idx==pk ? reg_idx : __get_temp_range(parser, n_pk_field);
    if( is_update || on_error==OE_Replace ){
      if( HAS_ROWID(tab) ){
        __vdbe_add_op2(v, OP_IdxRowid, i_this_cur, reg_r);
        /* Conflict only if the rowid of the existing index entry
        ** is different from old-rowid */
        if( is_update ){
          __vdbe_add_op3(v, OP_Eq, reg_r, addr_unique_ok, reg_old_data);
          __vdbe_change_p5(v, DBSQL_NOTNULL);
          VDBE_COVERAGE(v);
        }
      }else{
        int x;
        /* Extract the PRIMARY KEY from the end of the index entry and
        ** store it in registers reg_r..reg_r+n_pk-1 */
        if( idx!=pk ){
          for(i=0; i<pk->n_key_col; i++){
            assert( pk->columns[i]>=0 );
            x = __table_column_to_index(idx, pk->columns[i]);
            __vdbe_add_op3(v, OP_Column, i_this_cur, x, reg_r+i);
            VDBE_COMMENT((v, "%s.%s", tab->name,
                         tab->cols[pk->columns[i]].cn_name));
          }
        }
        if( is_update ){
          /* If currently processing the PRIMARY KEY of a WITHOUT ROWID
          ** table, only conflict if the new PRIMARY KEY values are actually
          ** different from the old.  See TH3 withoutrowid04.test.
          **
          ** For a UNIQUE index, only conflict if the PRIMARY KEY values
          ** of the matched index row are different from the original PRIMARY
          ** KEY values of this row before the update.  */
          int addr_jump = __vdbe_current_addr(v)+pk->n_key_col;
          int op = OP_Ne;
          int reg_cmp = (IS_PRIMARY_KEY_INDEX(idx) ? reg_idx : reg_r);
 
          for(i=0; i<pk->n_key_col; i++){
            char *p4 = (char*)__locate_coll_seq(parser, pk->colls[i]);
            x = pk->columns[i];
            assert( x>=0 );
            if( i==(pk->n_key_col-1) ){
              addr_jump = addr_unique_ok;
              op = OP_Eq;
            }
            x = __table_column_to_storage(tab, x);
            __vdbe_add_op4(v, op,
                reg_old_data+1+x, addr_jump, reg_cmp+i, p4, P4_COLLSEQ
            );
            __vdbe_change_p5(v, DBSQL_NOTNULL);
            VDBE_COVERAGE_IF(v, op==OP_Eq);
            VDBE_COVERAGE_IF(v, op==OP_Ne);
          }
        }
      }
    }

    /* Generate code that executes if the new index entry is not unique */
    assert( on_error==OE_Rollback || on_error==OE_Abort || on_error==OE_Fail
        || on_error==OE_Ignore || on_error==OE_Replace || on_error==OE_Update );
    switch( on_error ){
      case OE_Rollback:
      case OE_Abort:
      case OE_Fail: {
        testcase( on_error==OE_Rollback );
        testcase( on_error==OE_Abort );
        testcase( on_error==OE_Fail );
        __unique_constraint(parser, on_error, idx);
        break;
      }
#ifndef DBSQL_OMIT_UPSERT
      case OE_Update: {
        __upsert_do_update(parser, upsert, tab, idx, i_idx_cur+ix);
        /* no break */ deliberate_fall_through
      }
#endif
      case OE_Ignore: {
        testcase( on_error==OE_Ignore );
        __vdbe_goto(v, ignore_dest);
        break;
      }
      default: {
        int n_conflict_ck;   /* Number of opcodes in conflict check logic */

        assert( on_error==OE_Replace );
        n_conflict_ck = __vdbe_current_addr(v) - addr_conflict_ck;
        assert( n_conflict_ck>0 || db->malloc_failed );
        testcase( n_conflict_ck<=0 );
        testcase( n_conflict_ck>1 );
        if( reg_trig_cnt ){
          __multi_write(parser);
          n_replace_trig++;
        }
        if( P_TRIGGER && is_update ){
          __vdbe_add_op1(v, OP_CursorLock, i_data_cur);
        }
        __generate_row_delete(parser, tab, P_TRIGGER, i_data_cur, i_idx_cur,
            reg_r, n_pk_field, 0, OE_Replace,
            (idx==pk ? ONEPASS_SINGLE : ONEPASS_OFF), i_this_cur);
        if( P_TRIGGER && is_update ){
          __vdbe_add_op1(v, OP_CursorUnlock, i_data_cur);
        }
        if( reg_trig_cnt ){
          int addr_bypass;  /* Jump destination to bypass recheck logic */

          __vdbe_add_op2(v, OP_AddImm, reg_trig_cnt, 1); /* incr trigger cnt */
          addr_bypass = __vdbe_add_op0(v, OP_Goto);  /* Bypass recheck */
          VDBE_COMMENT((v, "bypass recheck"));

          /* Here we insert code that will be invoked after all constraint
          ** checks have run, if and only if one or more replace triggers
          ** fired. */
          __vdbe_resolve_label(v, lbl_recheck_ok);
          lbl_recheck_ok = __vdbe_make_label(parser);
          if( idx->part_idx_where ){
            /* Bypass the recheck if this partial index is not defined
            ** for the current row */
            __vdbe_add_op2(v, OP_IsNull, reg_idx-1, lbl_recheck_ok);
            VDBE_COVERAGE(v);
          }
          /* Copy the constraint check code from above, except change
          ** the constraint-ok jump destination to be the address of
          ** the next retest block */
          while( n_conflict_ck>0 ){
            vdbe_op_t x;    /* Conflict check opcode to copy */
            /* The __vdbe_add_op4() call might reallocate the opcode array.
            ** Hence, make a complete copy of the opcode, rather than using
            ** a pointer to the opcode. */
            x = *__vdbe_get_op(v, addr_conflict_ck);
            if( x.opcode!=OP_IdxRowid ){
              int p2;      /* New P2 value for copied conflict check opcode */
              const char *z_p4;
              if( __opcode_property[x.opcode]&OPFLG_JUMP ){
                p2 = lbl_recheck_ok;
              }else{
                p2 = x.p2;
              }
              z_p4 = x.p4type==P4_INT32 ? DBSQL_INT_TO_PTR(x.p4.i) : x.p4.z;
              __vdbe_add_op4(v, x.opcode, x.p1, p2, x.p3, z_p4, x.p4type);
              __vdbe_change_p5(v, x.p5);
              VDBE_COVERAGE_IF(v, p2!=x.p2);
            }
            n_conflict_ck--;
            addr_conflict_ck++;
          }
          /* If the retest fails, issue an abort */
          __unique_constraint(parser, OE_Abort, idx);

          __vdbe_jump_here(v, addr_bypass); /* Terminate the recheck bypass */
        }
        seen_replace = 1;
        break;
      }
    }
    __vdbe_resolve_label(v, addr_unique_ok);
    if( reg_r!=reg_idx ) __release_temp_range(parser, reg_r, n_pk_field);
    if( upsert_clause
     && upsert_ipk_return
     && __upsert_next_is_ipk(upsert_clause)
    ){
      __vdbe_goto(v, upsert_ipk_delay+1);
      __vdbe_jump_here(v, upsert_ipk_return);
      upsert_ipk_return = 0;
    }
  }

  /* If the IPK constraint is a REPLACE, run it last */
  if( ipk_top ){
    __vdbe_goto(v, ipk_top);
    VDBE_COMMENT((v, "Do IPK REPLACE"));
    assert( ipk_bottom>0 );
    __vdbe_jump_here(v, ipk_bottom);
  }

  /* Recheck all uniqueness constraints after replace triggers have run */
  testcase( reg_trig_cnt!=0 && n_replace_trig==0 );
  assert( reg_trig_cnt!=0 || n_replace_trig==0 );
  if( n_replace_trig ){
    __vdbe_add_op2(v, OP_IfNot, reg_trig_cnt, lbl_recheck_ok);VDBE_COVERAGE(v);
    if( !pk ){
      if( is_update ){
        __vdbe_add_op3(v, OP_Eq, reg_new_data, addr_recheck, reg_old_data);
        __vdbe_change_p5(v, DBSQL_NOTNULL);
        VDBE_COVERAGE(v);
      }
      __vdbe_add_op3(v, OP_NotExists, i_data_cur, addr_recheck, reg_new_data);
      VDBE_COVERAGE(v);
      __rowid_constraint(parser, OE_Abort, tab);
    }else{
      __vdbe_goto(v, addr_recheck);
    }
    __vdbe_resolve_label(v, lbl_recheck_ok);
  }

  /* Generate the table record */
  if( HAS_ROWID(tab) ){
    int reg_rec = a_reg_idx[ix];
    __vdbe_add_op3(v, OP_MakeRecord, reg_new_data+1, tab->n_nv_col, reg_rec);
    __set_make_record_p5(v, tab);
    if( !affinity_done ){
      __table_affinity(v, tab, 0);
    }
  }

  *pb_may_replace = seen_replace;
  VDBE_MODULE_COMMENT((v, "END: GenCnstCks(%d)", seen_replace));
}

#ifdef DBSQL_ENABLE_NULL_TRIM
/*
** Change the P5 operand on the last opcode (which should be an OP_MakeRecord)
** to be the number of columns in table tab that must not be NULL-trimmed.
**
** Or if no columns of tab may be NULL-trimmed, leave P5 at zero.
*/
void __set_make_record_p5(vdbe_t *v, table_t *tab){
  u_int16_t i;

  /* Records with omitted columns are only allowed for schema format
  ** version 2 and later (dbsql version 3.1.4, 2005-02-20). */
  if( tab->schema->file_format<2 ) return;

  for(i=tab->n_col-1; i>0; i--){
    if( tab->cols[i].i_dflt!=0 ) break;
    if( tab->cols[i].col_flags & COLFLAG_PRIMKEY ) break;
  }
  __vdbe_change_p5(v, i+1);
}
#endif

/*
** table_t tab is a WITHOUT ROWID table that is being written to. The cursor
** number is i_cur, and register reg_data contains the new record for the
** PK index. This function adds code to invoke the pre-update hook,
** if one is registered.
*/
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
static void CODE_WITHOUT_ROWID_PREUPDATE(
  parser_t *parser,                  /* parser_t context */
  table_t *tab,                    /* table_t being updated */
  int i_cur,                       /* Cursor number for table */
  int reg_data                     /* Data containing new record */
){
  vdbe_t *v = parser->vdbe;
  int r = __get_temp_reg(parser);
  assert( !HAS_ROWID(tab) );
  assert( 0==(parser->db->m_db_flags & DBFLAG_Vacuum) || CORRUPT_DB );
  __vdbe_add_op2(v, OP_Integer, 0, r);
  __vdbe_add_op4(v, OP_Insert, i_cur, reg_data, r, (char*)tab, P4_TABLE);
  __vdbe_change_p5(v, OPFLAG_ISNOOP);
  __release_temp_reg(parser, r);
}
#else
# define CODE_WITHOUT_ROWID_PREUPDATE(a,b,c,d)
#endif

/*
** This routine generates code to finish the INSERT or UPDATE operation
** that was started by a prior call to __generate_constraint_checks.
** A consecutive range of registers starting at reg_new_data contains the
** rowid and the content to be inserted.
**
** The arguments to this routine should be the same as the first six
** arguments to __generate_constraint_checks.
*/
void __complete_insertion(
  parser_t *parser,      /* The parser context */
  table_t *tab,        /* the table into which we are inserting */
  int i_data_cur,       /* Cursor of the canonical data source */
  int i_idx_cur,        /* First index cursor */
  int reg_new_data,     /* Range of content */
  int *reg_idx,       /* Register used by each index.  0 for unused indices */
  int update_flags,   /* True for UPDATE, False for INSERT */
  int append_bias,     /* True if this is likely to be an append */
  int use_seek_result   /* True to set the USESEEKRESULT flag on OP_[Idx]Insert */
){
  vdbe_t *v;            /* Prepared statements under construction */
  index_t *idx;        /* An index being inserted or updated */
  u_int8_t pik_flags;       /* flag values passed to the btree insert */
  int i;              /* Loop counter */

  assert( update_flags==0
       || update_flags==OPFLAG_ISUPDATE
       || update_flags==(OPFLAG_ISUPDATE|OPFLAG_SAVEPOSITION)
  );

  v = parser->vdbe;
  assert( v!=0 );
  assert( !IsView(tab) );  /* This table is not a VIEW */
  for(i=0, idx=tab->p_index; idx; idx=idx->next, i++){
    /* All REPLACE indexes are at the end of the list */
    assert( idx->on_error!=OE_Replace
         || idx->next==0
         || idx->next->on_error==OE_Replace );
    if( reg_idx[i]==0 ) continue;
    if( idx->part_idx_where ){
      __vdbe_add_op2(v, OP_IsNull, reg_idx[i], __vdbe_current_addr(v)+2);
      VDBE_COVERAGE(v);
    }
    pik_flags = (use_seek_result ? OPFLAG_USESEEKRESULT : 0);
    if( IS_PRIMARY_KEY_INDEX(idx) && !HAS_ROWID(tab) ){
      pik_flags |= OPFLAG_NCHANGE;
      pik_flags |= (update_flags & OPFLAG_SAVEPOSITION);
      if( update_flags==0 ){
        CODE_WITHOUT_ROWID_PREUPDATE(parser, tab, i_idx_cur+i, reg_idx[i]);
      }
    }
    __vdbe_add_op4_int(v, OP_IdxInsert, i_idx_cur+i, reg_idx[i],
                         reg_idx[i]+1,
                         idx->uniq_not_null ? idx->n_key_col: idx->n_column);
    __vdbe_change_p5(v, pik_flags);
  }
  if( !HAS_ROWID(tab) ) return;
  if( parser->nested ){
    pik_flags = 0;
  }else{
    pik_flags = OPFLAG_NCHANGE;
    pik_flags |= (update_flags?update_flags:OPFLAG_LASTROWID);
  }
  if( append_bias ){
    pik_flags |= OPFLAG_APPEND;
  }
  if( use_seek_result ){
    pik_flags |= OPFLAG_USESEEKRESULT;
  }
  __vdbe_add_op3(v, OP_Insert, i_data_cur, reg_idx[i], reg_new_data);
  if( !parser->nested ){
    __vdbe_append_p4(v, tab, P4_TABLE);
  }
  __vdbe_change_p5(v, pik_flags);
}

/*
** Allocate cursors for the tab table and all its indices and generate
** code to open and initialized those cursors.
**
** The cursor for the object that contains the complete data (normally
** the table itself, but the PRIMARY KEY index in the case of a WITHOUT
** ROWID table) is returned in *pi_data_cur.  The first index cursor is
** returned in *pi_idx_cur.  The number of indices is returned.
**
** Use i_base as the first cursor (either the *pi_data_cur for rowid tables
** or the first index for WITHOUT ROWID tables) if it is non-negative.
** If i_base is negative, then allocate the next available cursor.
**
** For a rowid table, *pi_data_cur will be exactly one less than *pi_idx_cur.
** For a WITHOUT ROWID table, *pi_data_cur will be somewhere in the range
** of *pi_idx_curs, depending on where the PRIMARY KEY index appears on the
** tab->p_index list.
**
** If tab is a virtual table, then this routine is a no-op and the
** *pi_data_cur and *pi_idx_cur values are left uninitialized.
*/
int __open_table_and_indices(
  parser_t *parser,   /* Parsing context */
  table_t *tab,     /* table_t to be opened */
  int op,          /* OP_OpenRead or OP_OpenWrite */
  u_int8_t p5,           /* P5 value for OP_Open* opcodes (except on WITHOUT ROWID) */
  int i_base,       /* Use this for the table cursor, if there is one */
  u_int8_t *to_opens,     /* If not NULL: boolean for each table and index */
  int *pi_data_cur,  /* Write the database source cursor number here */
  int *pi_idx_cur    /* Write the first index cursor number here */
){
  int i;
  int i_db;
  int i_data_cur;
  index_t *idx;
  vdbe_t *v;

  assert( op==OP_OpenRead || op==OP_OpenWrite );
  assert( op==OP_OpenWrite || p5==0 );
  assert( pi_data_cur!=0 );
  assert( pi_idx_cur!=0 );
  if( IS_VIRTUAL(tab) ){
    /* This routine is a no-op for virtual tables. Leave the output
    ** variables *pi_data_cur and *pi_idx_cur set to illegal cursor numbers
    ** for improved error detection. */
    *pi_data_cur = *pi_idx_cur = -999;
    return 0;
  }
  i_db = __schema_to_index(parser->db, tab->schema);
  v = parser->vdbe;
  assert( v!=0 );
  if( i_base<0 ) i_base = parser->n_tab;
  i_data_cur = i_base++;
  *pi_data_cur = i_data_cur;
  if( HAS_ROWID(tab) && (to_opens==0 || to_opens[0]) ){
    __open_table(parser, i_data_cur, i_db, tab, op);
  }else if( parser->db->no_shared_cache==0 ){
    __table_lock(parser, i_db, tab->tnum, op==OP_OpenWrite, tab->name);
  }
  *pi_idx_cur = i_base;
  for(i=0, idx=tab->p_index; idx; idx=idx->next, i++){
    int i_idx_cur = i_base++;
    assert( idx->schema==tab->schema );
    if( IS_PRIMARY_KEY_INDEX(idx) && !HAS_ROWID(tab) ){
      *pi_data_cur = i_idx_cur;
      p5 = 0;
    }
    if( to_opens==0 || to_opens[i+1] ){
      __vdbe_add_op3(v, op, i_idx_cur, idx->tnum, i_db);
      __vdbe_set_p4_key_info(parser, idx);
      __vdbe_change_p5(v, p5);
      VDBE_COMMENT((v, "%s", idx->name));
    }
  }
  if( i_base>parser->n_tab ) parser->n_tab = i_base;
  return i;
}


#ifdef DBSQL_TEST
/*
** The following global variable is incremented whenever the
** transfer optimization is used.  This is used for testing
** purposes only - to make sure the transfer optimization really
** is happening when it is supposed to.
*/
int dbsql_xferopt_count;
#endif /* DBSQL_TEST */


#ifndef DBSQL_OMIT_XFER_OPT
/*
** Check to see if index src is compatible as a source of data
** for index dest in an insert transfer optimization.  The rules
** for a compatible index:
**
**    *   The index is over the same set of columns
**    *   The same DESC and ASC markings occurs on all columns
**    *   The same on_error processing (OE_Abort, OE_Ignore, etc)
**    *   The same collating sequence on each column
**    *   The index has the exact same WHERE clause
*/
static int __xfer_compatible_index(index_t *dest, index_t *src){
  int i;
  assert( dest && src );
  assert( dest->table!=src->table );
  if( dest->n_key_col!=src->n_key_col || dest->n_column!=src->n_column ){
    return 0;   /* Different number of columns */
  }
  if( dest->on_error!=src->on_error ){
    return 0;   /* Different conflict resolution strategies */
  }
  for(i=0; i<src->n_key_col; i++){
    if( src->columns[i]!=dest->columns[i] ){
      return 0;   /* Different columns indexed */
    }
    if( src->columns[i]==XN_EXPR ){
      assert( src->col_exprs!=0 && dest->col_exprs!=0 );
      if( __expr_compare(0, src->col_exprs->a[i].expr,
                             dest->col_exprs->a[i].expr, -1)!=0 ){
        return 0;   /* Different expressions in the index */
      }
    }
    if( src->sort_orders[i]!=dest->sort_orders[i] ){
      return 0;   /* Different sort orders */
    }
    if( dbsql_stricmp(src->colls[i],dest->colls[i])!=0 ){
      return 0;   /* Different collating sequences */
    }
  }
  if( __expr_compare(0, src->part_idx_where, dest->part_idx_where, -1) ){
    return 0;     /* Different WHERE clauses */
  }

  /* If no test above fails then the indices must be compatible */
  return 1;
}

/*
** Attempt the transfer optimization on INSERTs of the form
**
**     INSERT INTO tab1 SELECT * FROM tab2;
**
** The xfer optimization transfers raw records from tab2 over to tab1. 
** Columns are not decoded and reassembled, which greatly improves
** performance.  Raw index records are transferred in the same way.
**
** The xfer optimization is only attempted if tab1 and tab2 are compatible.
** There are lots of rules for determining compatibility - see comments
** embedded in the code for details.
**
** This routine returns TRUE if the optimization is guaranteed to be used.
** Sometimes the xfer optimization will only work if the destination table
** is empty - a factor that can only be determined at run-time.  In that
** case, this routine generates code for the xfer optimization but also
** does a test to see if the destination table is empty and jumps over the
** xfer optimization code if the test fails.  In that case, this routine
** returns FALSE so that the caller will know to go ahead and generate
** an unoptimized transfer.  This routine also returns FALSE if there
** is no chance that the xfer optimization can be applied.
**
** This optimization is particularly useful at making VACUUM run faster.
*/
static int __xfer_optimization(
  parser_t *parser,        /* Parser context */
  table_t *dest,         /* The table we are inserting into */
  select_t *p_select,      /* A SELECT statement to use as the data source */
  int on_error,          /* How to handle constraint errors */
  int i_db_dest           /* The database of dest */
){
  DBSQL *db = parser->db;
  expr_list_t *e_list;                /* The result set of the SELECT */
  table_t *src;                     /* The table in the FROM clause of SELECT */
  index_t *src_idx, *dest_idx;       /* Source and destination indices */
  src_item_t *item;                  /* An element of p_select->src */
  int i;                           /* Loop counter */
  int i_db_src;                      /* The database of src */
  int i_src, i_dest;                 /* Cursors from source and destination */
  int addr1, addr2;                /* Loop addresses */
  int empty_dest_test = 0;           /* Address of test for empty dest */
  int empty_src_test = 0;            /* Address of test for empty src */
  vdbe_t *v;                         /* The VDBE we are building */
  int reg_autoinc;                  /* Memory register used by AUTOINC */
  int dest_has_unique_idx = 0;        /* True if dest has a UNIQUE index */
  int reg_data, reg_rowid;           /* Registers holding data and rowid */

  assert( p_select!=0 );
  if( parser->with || p_select->with ){
    /* Do not attempt to process this query if there are an WITH clauses
    ** attached to it. Proceeding may generate a false "no such table: xxx"
    ** error if p_select reads from a CTE named "xxx".  */
    return 0;
  }
#ifndef DBSQL_OMIT_VIRTUALTABLE
  if( IS_VIRTUAL(dest) ){
    return 0;   /* tab1 must not be a virtual table */
  }
#endif
  if( on_error==OE_Default ){
    if( dest->i_p_key>=0 ) on_error = dest->key_conf;
    if( on_error==OE_Default ) on_error = OE_Abort;
  }
  assert(p_select->src);   /* allocated even if there is no FROM clause */
  if( p_select->src->n_src!=1 ){
    return 0;   /* FROM clause must have exactly one term */
  }
  if( p_select->src->a[0].fg.is_subquery ){
    return 0;   /* FROM clause cannot contain a subquery */
  }
  if( p_select->where ){
    return 0;   /* SELECT may not have a WHERE clause */
  }
  if( p_select->order_by ){
    return 0;   /* SELECT may not have an ORDER BY clause */
  }
  /* Do not need to test for a HAVING clause.  If HAVING is present but
  ** there is no ORDER BY, we will get an error. */
  if( p_select->group_by ){
    return 0;   /* SELECT may not have a GROUP BY clause */
  }
  if( p_select->limit ){
    return 0;   /* SELECT may not have a LIMIT clause */
  }
  if( p_select->prior ){
    return 0;   /* SELECT may not be a compound query */
  }
  if( p_select->sel_flags & SF_Distinct ){
    return 0;   /* SELECT may not be DISTINCT */
  }
  e_list = p_select->e_list;
  assert( e_list!=0 );
  if( e_list->n_expr!=1 ){
    return 0;   /* The result set must have exactly one column */
  }
  assert( e_list->a[0].expr );
  if( e_list->a[0].expr->op!=TK_ASTERISK ){
    return 0;   /* The result set must be the special operator "*" */
  }

  /* At this point we have established that the statement is of the
  ** correct syntactic form to participate in this optimization.  Now
  ** we have to check the semantics.
  */
  item = p_select->src->a;
  src = __locate_table_item(parser, 0, item);
  if( src==0 ){
    return 0;   /* FROM clause does not contain a real table */
  }
  if( src->tnum==dest->tnum && src->schema==dest->schema ){
    testcase( src!=dest ); /* Possible due to bad dbsql_schema.rootpage */
    return 0;   /* tab1 and tab2 may not be the same table */
  }
  if( HAS_ROWID(dest)!=HAS_ROWID(src) ){
    return 0;   /* source and destination must both be WITHOUT ROWID or not */
  }
  if( !IS_ORDINARY_TABLE(src) ){
    return 0;   /* tab2 may not be a view or virtual table */
  }
  if( dest->n_col!=src->n_col ){
    return 0;   /* Number of columns must be the same in tab1 and tab2 */
  }
  if( dest->i_p_key!=src->i_p_key ){
    return 0;   /* Both tables must have the same INTEGER PRIMARY KEY */
  }
  if( (dest->tab_flags & TF_Strict)!=0 && (src->tab_flags & TF_Strict)==0 ){
    return 0;   /* Cannot feed from a non-strict into a strict table */
  }
  for(i=0; i<dest->n_col; i++){
    column_t *dest_col = &dest->cols[i];
    column_t *src_col = &src->cols[i];
#ifdef DBSQL_ENABLE_HIDDEN_COLUMNS
    if( (db->m_db_flags & DBFLAG_Vacuum)==0
     && (dest_col->col_flags | src_col->col_flags) & COLFLAG_HIDDEN
    ){
      return 0;    /* Neither table may have __hidden__ columns */
    }
#endif
#ifndef DBSQL_OMIT_GENERATED_COLUMNS
    /* Even if tables t1 and t2 have identical schemas, if they contain
    ** generated columns, then this statement is semantically incorrect:
    **
    **     INSERT INTO t2 SELECT * FROM t1;
    **
    ** The reason is that generated column values are returned by the
    ** the SELECT statement on the right but the INSERT statement on the
    ** left wants them to be omitted.
    **
    ** Nevertheless, this is a useful notational shorthand to tell dbsql
    ** to do a bulk transfer all of the content from t1 over to t2.
    **
    ** We could, in theory, disable this (except for internal use by the
    ** VACUUM command where it is actually needed).  But why do that?  It
    ** seems harmless enough, and provides a useful service.
    */
    if( (dest_col->col_flags & COLFLAG_GENERATED) !=
        (src_col->col_flags & COLFLAG_GENERATED) ){
      return 0;    /* Both columns have the same generated-column type */
    }
    /* But the transfer is only allowed if both the source and destination
    ** tables have the exact same expressions for generated columns.
    ** This requirement could be relaxed for VIRTUAL columns, I suppose.
    */
    if( (dest_col->col_flags & COLFLAG_GENERATED)!=0 ){
      if( __expr_compare(0,
             __column_expr(src, src_col),
             __column_expr(dest, dest_col), -1)!=0 ){
        testcase( dest_col->col_flags & COLFLAG_VIRTUAL );
        testcase( dest_col->col_flags & COLFLAG_STORED );
        return 0;  /* Different generator expressions */
      }
    }
#endif
    if( dest_col->affinity!=src_col->affinity ){
      return 0;    /* Affinity must be the same on all columns */
    }
    if( dbsql_stricmp(__column_coll(dest_col),
                        __column_coll(src_col))!=0 ){
      return 0;    /* Collating sequence must be the same on all columns */
    }
    if( dest_col->not_null && !src_col->not_null ){
      return 0;    /* tab2 must be NOT NULL if tab1 is */
    }
    /* Default values for second and subsequent columns need to match. */
    if( (dest_col->col_flags & COLFLAG_GENERATED)==0 && i>0 ){
      expr_t *dest_expr = __column_expr(dest, dest_col);
      expr_t *src_expr = __column_expr(src, src_col);
      assert( dest_expr==0 || dest_expr->op==TK_SPAN );
      assert( dest_expr==0 || !EXPR_HAS_PROPERTY(dest_expr, EP_IntValue) );
      assert( src_expr==0 || src_expr->op==TK_SPAN );
      assert( src_expr==0 || !EXPR_HAS_PROPERTY(src_expr, EP_IntValue) );
      if( (dest_expr==0)!=(src_expr==0)
       || (dest_expr!=0 && strcmp(dest_expr->u.token,
                                       src_expr->u.token)!=0)
      ){
        return 0;    /* Default values must be the same for all columns */
      }
    }
  }
  for(dest_idx=dest->p_index; dest_idx; dest_idx=dest_idx->next){
    if( IS_UNIQUE_INDEX(dest_idx) ){
      dest_has_unique_idx = 1;
    }
    for(src_idx=src->p_index; src_idx; src_idx=src_idx->next){
      if( __xfer_compatible_index(dest_idx, src_idx) ) break;
    }
    if( src_idx==0 ){
      return 0;    /* dest_idx has no corresponding index in src */
    }
    if( src_idx->tnum==dest_idx->tnum && src->schema==dest->schema
         && __fault_sim(411)==DBSQL_OK ){
      /* The __fault_sim() call allows this corruption test to be
      ** bypassed during testing, in order to exercise other corruption tests
      ** further downstream. */
      return 0;   /* Corrupt schema - two indexes on the same btree */
    }
  }
#ifndef DBSQL_OMIT_CHECK
  if( dest->check
   && (db->m_db_flags & DBFLAG_Vacuum)==0
   && __expr_list_compare(src->check,dest->check,-1)
  ){
    return 0;   /* Tables have different CHECK constraints.  Ticket #2252 */
  }
#endif
#ifndef DBSQL_OMIT_FOREIGN_KEY
  /* Disallow the transfer optimization if the destination table contains
  ** any foreign key constraints.  This is more restrictive than necessary.
  ** But the main beneficiary of the transfer optimization is the VACUUM
  ** command, and the VACUUM command disables foreign key constraints.  So
  ** the extra complication to make this rule less restrictive is probably
  ** not worth the effort.  Ticket [6284df89debdfa61db8073e062908af0c9b6118e]
  */
  assert( IS_ORDINARY_TABLE(dest) );
  if( (db->flags & DBSQL_ForeignKeys)!=0 && dest->u.tab.f_key!=0 ){
    return 0;
  }
#endif
  if( (db->flags & DBSQL_CountRows)!=0 ){
    return 0;  /* xfer opt does not play well with PRAGMA count_changes */
  }

  /* If we get this far, it means that the xfer optimization is at
  ** least a possibility, though it might only work if the destination
  ** table (tab1) is initially empty.
  */
#ifdef DBSQL_TEST
  dbsql_xferopt_count++;
#endif
  i_db_src = __schema_to_index(db, src->schema);
  v = __get_vdbe(parser);
  __code_verify_schema(parser, i_db_src);
  i_src = parser->n_tab++;
  i_dest = parser->n_tab++;
  reg_autoinc = AUTO_INC_BEGIN(parser, i_db_dest, dest);
  reg_data = __get_temp_reg(parser);
  __vdbe_add_op2(v, OP_Null, 0, reg_data);
  reg_rowid = __get_temp_reg(parser);
  __open_table(parser, i_dest, i_db_dest, dest, OP_OpenWrite);
  assert( HAS_ROWID(dest) || dest_has_unique_idx );
  if( (db->m_db_flags & DBFLAG_Vacuum)==0 && (
      (dest->i_p_key<0 && dest->p_index!=0)          /* (1) */
   || dest_has_unique_idx                              /* (2) */
   || (on_error!=OE_Abort && on_error!=OE_Rollback)   /* (3) */
  )){
    /* In some circumstances, we are able to run the xfer optimization
    ** only if the destination table is initially empty. Unless the
    ** DBFLAG_Vacuum flag is set, this block generates code to make
    ** that determination. If DBFLAG_Vacuum is set, then the destination
    ** table is always empty.
    **
    ** Conditions under which the destination must be empty:
    **
    ** (1) There is no INTEGER PRIMARY KEY but there are indices.
    **     (If the destination is not initially empty, the rowid fields
    **     of index entries might need to change.)
    **
    ** (2) The destination has a unique index.  (The xfer optimization
    **     is unable to test uniqueness.)
    **
    ** (3) on_error is something other than OE_Abort and OE_Rollback.
    */
    addr1 = __vdbe_add_op2(v, OP_Rewind, i_dest, 0); VDBE_COVERAGE(v);
    empty_dest_test = __vdbe_add_op0(v, OP_Goto);
    __vdbe_jump_here(v, addr1);
  }
  if( HAS_ROWID(src) ){
    u_int8_t ins_flags;
    __open_table(parser, i_src, i_db_src, src, OP_OpenRead);
    empty_src_test = __vdbe_add_op2(v, OP_Rewind, i_src, 0); VDBE_COVERAGE(v);
    if( dest->i_p_key>=0 ){
      addr1 = __vdbe_add_op2(v, OP_Rowid, i_src, reg_rowid);
      if( (db->m_db_flags & DBFLAG_Vacuum)==0 ){
        __vdbe_verify_abortable(v, on_error);
        addr2 = __vdbe_add_op3(v, OP_NotExists, i_dest, 0, reg_rowid);
        VDBE_COVERAGE(v);
        __rowid_constraint(parser, on_error, dest);
        __vdbe_jump_here(v, addr2);
      }
      AUTO_INC_STEP(parser, reg_autoinc, reg_rowid);
    }else if( dest->p_index==0 && !(db->m_db_flags & DBFLAG_VacuumInto) ){
      addr1 = __vdbe_add_op2(v, OP_NewRowid, i_dest, reg_rowid);
    }else{
      addr1 = __vdbe_add_op2(v, OP_Rowid, i_src, reg_rowid);
      assert( (dest->tab_flags & TF_Autoincrement)==0 );
    }

    if( db->m_db_flags & DBFLAG_Vacuum ){
      __vdbe_add_op1(v, OP_SeekEnd, i_dest);
      ins_flags = OPFLAG_APPEND|OPFLAG_USESEEKRESULT|OPFLAG_PREFORMAT;
    }else{
      ins_flags = OPFLAG_NCHANGE|OPFLAG_LASTROWID|OPFLAG_APPEND|OPFLAG_PREFORMAT;
    }
#ifdef DBSQL_ENABLE_PREUPDATE_HOOK
    if( (db->m_db_flags & DBFLAG_Vacuum)==0 ){
      __vdbe_add_op3(v, OP_RowData, i_src, reg_data, 1);
      ins_flags &= ~OPFLAG_PREFORMAT;
    }else
#endif
    {
      __vdbe_add_op3(v, OP_RowCell, i_dest, i_src, reg_rowid);
    }
    __vdbe_add_op3(v, OP_Insert, i_dest, reg_data, reg_rowid);
    if( (db->m_db_flags & DBFLAG_Vacuum)==0 ){
      __vdbe_change_p4(v, -1, (char*)dest, P4_TABLE);
    }
    __vdbe_change_p5(v, ins_flags);

    __vdbe_add_op2(v, OP_Next, i_src, addr1); VDBE_COVERAGE(v);
    __vdbe_add_op2(v, OP_Close, i_src, 0);
    __vdbe_add_op2(v, OP_Close, i_dest, 0);
  }else{
    __table_lock(parser, i_db_dest, dest->tnum, 1, dest->name);
    __table_lock(parser, i_db_src, src->tnum, 0, src->name);
  }
  for(dest_idx=dest->p_index; dest_idx; dest_idx=dest_idx->next){
    u_int8_t idx_ins_flags = 0;
    for(src_idx=src->p_index; ALWAYS(src_idx); src_idx=src_idx->next){
      if( __xfer_compatible_index(dest_idx, src_idx) ) break;
    }
    assert( src_idx );
    __vdbe_add_op3(v, OP_OpenRead, i_src, src_idx->tnum, i_db_src);
    __vdbe_set_p4_key_info(parser, src_idx);
    VDBE_COMMENT((v, "%s", src_idx->name));
    __vdbe_add_op3(v, OP_OpenWrite, i_dest, dest_idx->tnum, i_db_dest);
    __vdbe_set_p4_key_info(parser, dest_idx);
    __vdbe_change_p5(v, OPFLAG_BULKCSR);
    VDBE_COMMENT((v, "%s", dest_idx->name));
    addr1 = __vdbe_add_op2(v, OP_Rewind, i_src, 0); VDBE_COVERAGE(v);
    if( db->m_db_flags & DBFLAG_Vacuum ){
      /* This INSERT command is part of a VACUUM operation, which guarantees
      ** that the destination table is empty. If all indexed columns use
      ** collation sequence BINARY, then it can also be assumed that the
      ** index will be populated by inserting keys in strictly sorted
      ** order. In this case, instead of seeking within the b-tree as part
      ** of every OP_IdxInsert opcode, an OP_SeekEnd is added before the
      ** OP_IdxInsert to seek to the point within the b-tree where each key
      ** should be inserted. This is faster.
      **
      ** If any of the indexed columns use a collation sequence other than
      ** BINARY, this optimization is disabled. This is because the user
      ** might change the definition of a collation sequence and then run
      ** a VACUUM command. In that case keys may not be written in strictly
      ** sorted order.  */
      for(i=0; i<src_idx->n_column; i++){
        const char *coll = src_idx->colls[i];
        if( dbsql_stricmp(__str_binary, coll) ) break;
      }
      if( i==src_idx->n_column ){
        idx_ins_flags = OPFLAG_USESEEKRESULT|OPFLAG_PREFORMAT;
        __vdbe_add_op1(v, OP_SeekEnd, i_dest);
        __vdbe_add_op2(v, OP_RowCell, i_dest, i_src);
      }
    }else if( !HAS_ROWID(src) && dest_idx->idx_type==DBSQL_IDXTYPE_PRIMARYKEY ){
      idx_ins_flags |= OPFLAG_NCHANGE;
    }
    if( idx_ins_flags!=(OPFLAG_USESEEKRESULT|OPFLAG_PREFORMAT) ){
      __vdbe_add_op3(v, OP_RowData, i_src, reg_data, 1);
      if( (db->m_db_flags & DBFLAG_Vacuum)==0
       && !HAS_ROWID(dest)
       && IS_PRIMARY_KEY_INDEX(dest_idx)
      ){
        CODE_WITHOUT_ROWID_PREUPDATE(parser, dest, i_dest, reg_data);
      }
    }
    __vdbe_add_op2(v, OP_IdxInsert, i_dest, reg_data);
    __vdbe_change_p5(v, idx_ins_flags|OPFLAG_APPEND);
    __vdbe_add_op2(v, OP_Next, i_src, addr1+1); VDBE_COVERAGE(v);
    __vdbe_jump_here(v, addr1);
    __vdbe_add_op2(v, OP_Close, i_src, 0);
    __vdbe_add_op2(v, OP_Close, i_dest, 0);
  }
  if( empty_src_test ) __vdbe_jump_here(v, empty_src_test);
  __release_temp_reg(parser, reg_rowid);
  __release_temp_reg(parser, reg_data);
  if( empty_dest_test ){
    __autoincrement_end(parser);
    __vdbe_add_op2(v, OP_Halt, DBSQL_OK, 0);
    __vdbe_jump_here(v, empty_dest_test);
    __vdbe_add_op2(v, OP_Close, i_dest, 0);
    return 0;
  }else{
    return 1;
  }
}
#endif /* DBSQL_OMIT_XFER_OPT */
