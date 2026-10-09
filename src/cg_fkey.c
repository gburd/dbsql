/*
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code used by the compiler to add foreign key
** support to compiled SQL statements.
*/
#include "dbsql_int.h"

#ifndef DBSQL_OMIT_FOREIGN_KEY
#ifndef DBSQL_OMIT_TRIGGER

/*
** Deferred and Immediate FKs
** --------------------------
**
** Foreign keys in dbsql come in two flavours: deferred and immediate.
** If an immediate foreign key constraint is violated,
** DBSQL_CONSTRAINT_FOREIGNKEY is returned and the current
** statement transaction rolled back. If a 
** deferred foreign key constraint is violated, no action is taken 
** immediately. However if the application attempts to commit the 
** transaction before fixing the constraint violation, the attempt fails.
**
** Deferred constraints are implemented using a simple counter associated
** with the database handle. The counter is set to zero each time a 
** database transaction is opened. Each time a statement is executed 
** that causes a foreign key violation, the counter is incremented. Each
** time a statement is executed that removes an existing violation from
** the database, the counter is decremented. When the transaction is
** committed, the commit fails if the current value of the counter is
** greater than zero. This scheme has two big drawbacks:
**
**   * When a commit fails due to a deferred foreign key constraint, 
**     there is no way to tell which foreign constraint is not satisfied,
**     or which row it is not satisfied for.
**
**   * If the database contains foreign key violations when the 
**     transaction is opened, this may cause the mechanism to malfunction.
**
** Despite these problems, this approach is adopted as it seems simpler
** than the alternatives.
**
** INSERT operations:
**
**   I.1) For each FK for which the table is the child table, search
**        the parent table for a match. If none is found increment the
**        constraint counter.
**
**   I.2) For each FK for which the table is the parent table, 
**        search the child table for rows that correspond to the new
**        row in the parent table. Decrement the counter for each row
**        found (as the constraint is now satisfied).
**
** DELETE operations:
**
**   D.1) For each FK for which the table is the child table, 
**        search the parent table for a row that corresponds to the 
**        deleted row in the child table. If such a row is not found, 
**        decrement the counter.
**
**   D.2) For each FK for which the table is the parent table, search 
**        the child table for rows that correspond to the deleted row 
**        in the parent table. For each found increment the counter.
**
** UPDATE operations:
**
**   An UPDATE command requires that all 4 steps above are taken, but only
**   for FK constraints for which the affected columns are actually 
**   modified (values must be compared at runtime).
**
** Note that I.1 and D.1 are very similar operations, as are I.2 and D.2.
** This simplifies the implementation a bit.
**
** For the purposes of immediate FK constraints, the OR REPLACE conflict
** resolution is considered to delete rows before the new row is inserted.
** If a delete caused by OR REPLACE violates an FK constraint, an exception
** is thrown, even if the FK constraint would be satisfied after the new 
** row is inserted.
**
** Immediate constraints are usually handled similarly. The only difference 
** is that the counter used is stored as part of each individual statement
** object (struct vdbe_t). If, after the statement has run, its immediate
** constraint counter is greater than zero,
** it returns DBSQL_CONSTRAINT_FOREIGNKEY
** and the statement transaction is rolled back. An exception is an INSERT
** statement that inserts a single row only (no triggers). In this case,
** instead of using a counter, an exception is thrown immediately if the
** INSERT violates a foreign key constraint. This is necessary as such
** an INSERT does not open a statement transaction.
**
** TODO: How should dropping a table be handled? How should renaming a 
** table be handled?
**
**
** Query API Notes
** ---------------
**
** Before coding an UPDATE or DELETE row operation, the code-generator
** for those two operations needs to know whether or not the operation
** requires any FK processing and, if so, which columns of the original
** row are required by the FK processing VDBE code (i.e. if FKs were
** implemented using triggers, which of the old.* columns would be 
** accessed). No information is required by the code-generator before
** coding an INSERT operation. The functions used by the UPDATE/DELETE
** generation code to query for this information are:
**
**   __fk_required() - Test to see if FK processing is required.
**   __fk_oldmask()  - Query for the set of required old.* columns.
**
**
** Externally accessible module functions
** --------------------------------------
**
**   __fk_check()    - Check for foreign key violations.
**   __fk_actions()  - Code triggers for ON UPDATE/ON DELETE actions.
**   __fk_delete()   - Delete an f_key_t structure.
*/

/*
** VDBE Calling Convention
** -----------------------
**
** Example:
**
**   For the following INSERT statement:
**
**     CREATE TABLE t1(a, b INTEGER PRIMARY KEY, c);
**     INSERT INTO t1 VALUES(1, 2, 3.1);
**
**   Register (x):        2    (type integer)
**   Register (x+1):      1    (type integer)
**   Register (x+2):      NULL (type NULL)
**   Register (x+3):      3.1  (type real)
*/

/*
** A foreign key constraint requires that the key columns in the parent
** table are collectively subject to a UNIQUE or PRIMARY KEY constraint.
** Given that parent is the parent table for foreign key constraint f_key, 
** search the schema for a unique index on the parent key columns. 
**
** If successful, zero is returned. If the parent key is an INTEGER PRIMARY 
** KEY column, then output variable *idx is set to NULL. Otherwise, *idx 
** is set to point to the unique index. 
** 
** If the parent key consists of a single column (the foreign key constraint
** is not a composite foreign key), output variable *pai_col is set to NULL.
** Otherwise, it is set to point to an allocated array of size N, where
** N is the number of columns in the parent key. The first element of the
** array is the index of the child table column that is mapped by the FK
** constraint to the parent table column stored in the left-most column
** of index *idx. The second element of the array is the index of the
** child table column that corresponds to the second left-most column of
** *idx, and so on.
**
** If the required index cannot be found, either because:
**
**   1) The named parent key columns do not exist, or
**
**   2) The named parent key columns do exist, but are not subject to a
**      UNIQUE or PRIMARY KEY constraint, or
**
**   3) No parent key columns were provided explicitly as part of the
**      foreign key definition, and the parent table does not have a
**      PRIMARY KEY, or
**
**   4) No parent key columns were provided explicitly as part of the
**      foreign key definition, and the PRIMARY KEY of the parent table 
**      consists of a different number of columns to the child key in 
**      the child table.
**
** then non-zero is returned, and a "foreign key mismatch" error loaded
** into parser. If an OOM error occurs, non-zero is returned and the
** parser->db->malloc_failed flag is set.
*/
int __fk_locate_index(
  parser_t *parser,                  /* parser_t context to store any error in */
  table_t *parent,                 /* Parent table of FK constraint f_key */
  f_key_t *f_key,                    /* Foreign key to find index for */
  index_t **pp_idx,                  /* OUT: Unique index on parent table */
  int **pai_col                    /* OUT: Map of index columns in f_key */
){
  index_t *idx = 0;                    /* Value to return via *idx */
  int *cols = 0;                     /* Value to return via *pai_col */
  int n_col = f_key->n_col;             /* Number of columns in parent key */
  char *key = f_key->cols[0].col;   /* Name of left-most parent key column */

  /* The caller is responsible for zeroing output parameters. */
  assert( pp_idx && *pp_idx==0 );
  assert( !pai_col || *pai_col==0 );
  assert( parser );

  /* If this is a non-composite (single column) foreign key, check if it 
  ** maps to the INTEGER PRIMARY KEY of table parent. If so, leave *idx 
  ** and *pai_col set to zero and return early. 
  **
  ** Otherwise, for a composite foreign key (more than one column), allocate
  ** space for the cols array (returned via output parameter *pai_col).
  ** Non-composite foreign keys do not require the cols array.
  */
  if( n_col==1 ){
    /* The FK maps to the IPK if any of the following are true:
    **
    **   1) There is an INTEGER PRIMARY KEY column and the FK is implicitly 
    **      mapped to the primary key of table parent, or
    **   2) The FK is explicitly mapped to a column declared as INTEGER
    **      PRIMARY KEY.
    */
    if( parent->i_p_key>=0 ){
      if( !key ) return 0;
      if( !__str_icmp(parent->cols[parent->i_p_key].cn_name, key) ){
        return 0;
      }
    }
  }else if( pai_col ){
    assert( n_col>1 );
    cols = (int *)__db_malloc_raw_nn(parser->db, n_col*sizeof(int));
    if( !cols ) return 1;
    *pai_col = cols;
  }

  for(idx=parent->p_index; idx; idx=idx->next){
    if( idx->n_key_col==n_col && IS_UNIQUE_INDEX(idx) && idx->part_idx_where==0 ){ 
      /* idx is a UNIQUE index (or a PRIMARY KEY) and has the right number
      ** of columns. If each indexed column corresponds to a foreign key
      ** column of f_key, then this index is a winner.  */

      if( key==0 ){
        /* If key is NULL, then this foreign key is implicitly mapped to 
        ** the PRIMARY KEY of table parent. The PRIMARY KEY index may be 
        ** identified by the test.  */
        if( IS_PRIMARY_KEY_INDEX(idx) ){
          if( cols ){
            int i;
            for(i=0; i<n_col; i++) cols[i] = f_key->cols[i].i_from;
          }
          break;
        }
      }else{
        /* If key is non-NULL, then this foreign key was declared to
        ** map to an explicit list of columns in table parent. Check if this
        ** index matches those columns. Also, check that the index uses
        ** the default collation sequences for each column. */
        int i, j;
        for(i=0; i<n_col; i++){
          int16_t i_col = idx->columns[i];     /* index_t of column in parent tbl */
          const char *dflt_coll;            /* Def. collation for column */
          char *idx_col;                    /* Name of indexed column */

          if( i_col<0 ) break; /* No foreign keys against expression indexes */

          /* If the index uses a collation sequence that is different from
          ** the default collation sequence for the column, this index is
          ** unusable. Bail out early in this case.  */
          dflt_coll = __column_coll(&parent->cols[i_col]);
          if( !dflt_coll ) dflt_coll = __str_binary;
          if( __str_icmp(idx->colls[i], dflt_coll) ) break;

          idx_col = parent->cols[i_col].cn_name;
          for(j=0; j<n_col; j++){
            if( __str_icmp(f_key->cols[j].col, idx_col)==0 ){
              if( cols ) cols[i] = f_key->cols[j].i_from;
              break;
            }
          }
          if( j==n_col ) break;
        }
        if( i==n_col ) break;      /* idx is usable */
      }
    }
  }

  if( !idx ){
    if( !parser->disable_triggers ){
      __error_msg(parser,
           "foreign key mismatch - \"%w\" referencing \"%w\"",
           f_key->from->name, f_key->to);
    }
    __dbsql_db_free(parser->db, cols);
    return 1;
  }

  *pp_idx = idx;
  return 0;
}

/*
** This function is called when a row is inserted into or deleted from the 
** child table of foreign key constraint f_key. If an SQL UPDATE is executed 
** on the child table of f_key, this function is invoked twice for each row
** affected - once to "delete" the old row, and then again to "insert" the
** new row.
**
** Each time it is called, this function generates VDBE code to locate the
** row in the parent table that corresponds to the row being inserted into 
** or deleted from the child table. If the parent row can be found, no 
** special action is taken. Otherwise, if the parent row can *not* be
** found in the parent table:
**
**   Operation | FK type   | Action taken
**   --------------------------------------------------------------------------
**   INSERT      immediate   Increment the "immediate constraint counter".
**
**   DELETE      immediate   Decrement the "immediate constraint counter".
**
**   INSERT      deferred    Increment the "deferred constraint counter".
**
**   DELETE      deferred    Decrement the "deferred constraint counter".
**
** These operations are identified in the comment at the top of this file 
** (fkey.c) as "I.1" and "D.1".
*/
static void __fk_lookup_parent(
  parser_t *parser,        /* parser_t context */
  int i_db,              /* index_t of database housing tab */
  table_t *tab,          /* Parent table of FK f_key */
  index_t *idx,          /* Unique index on parent key columns in tab */
  f_key_t *f_key,          /* Foreign key constraint */
  int *cols,           /* Map from parent key columns to child table columns */
  int reg_data,          /* Address of array containing child table row */
  int n_incr,            /* Increment constraint counter by this */
  int is_ignore          /* If true, pretend tab contains all NULL values */
){
  int i;                                    /* Iterator variable */
  vdbe_t *v = __get_vdbe(parser);         /* vdbe_t to add code to */
  int i_cur = parser->n_tab - 1;              /* Cursor number to use */
  int i_ok = __vdbe_make_label(parser);   /* jump here if parent key found */

  __vdbe_verify_abortable(v,
    (!f_key->is_deferred
      && !(parser->db->flags & DBSQL_DeferFKs)
      && !parser->toplevel 
      && !parser->is_multi_write) ? OE_Abort : OE_Ignore);

  /* If n_incr is less than zero, then check at runtime if there are any
  ** outstanding constraints to resolve. If there are not, there is no need
  ** to check if deleting this row resolves any outstanding violations.
  **
  ** Check if any of the key columns in the child table row are NULL. If 
  ** any are, then the constraint is considered satisfied. No need to 
  ** search for a matching row in the parent table.  */
  if( n_incr<0 ){
    __vdbe_add_op2(v, OP_FkIfZero, f_key->is_deferred, i_ok);
    VDBE_COVERAGE(v);
  }
  for(i=0; i<f_key->n_col; i++){
    int i_reg = __table_column_to_storage(f_key->from,cols[i]) + reg_data + 1;
    __vdbe_add_op2(v, OP_IsNull, i_reg, i_ok); VDBE_COVERAGE(v);
  }

  if( is_ignore==0 ){
    if( idx==0 ){
      /* If idx is NULL, then the parent key is the INTEGER PRIMARY KEY
      ** column of the parent table (table tab).  */
      int i_must_be_int;               /* Address of __must_be_int instruction */
      int reg_temp = __get_temp_reg(parser);
  
      /* Invoke __must_be_int to coerce the child key value to an integer (i.e. 
      ** apply the affinity of the parent key). If this fails, then there
      ** is no matching parent key. Before using __must_be_int, make a copy of
      ** the value. Otherwise, the value inserted into the child key column
      ** will have INTEGER affinity applied to it, which may not be correct.  */
      __vdbe_add_op2(v, OP_SCopy, 
        __table_column_to_storage(f_key->from,cols[0])+1+reg_data, reg_temp);
      i_must_be_int = __vdbe_add_op2(v, OP_MustBeInt, reg_temp, 0);
      VDBE_COVERAGE(v);
  
      /* If the parent table is the same as the child table, and we are about
      ** to increment the constraint-counter (i.e. this is an INSERT operation),
      ** then check if the row being inserted matches itself. If so, do not
      ** increment the constraint-counter.  */
      if( tab==f_key->from && n_incr==1 ){
        __vdbe_add_op3(v, OP_Eq, reg_data, i_ok, reg_temp); VDBE_COVERAGE(v);
        __vdbe_change_p5(v, DBSQL_NOTNULL);
      }
  
      __open_table(parser, i_cur, i_db, tab, OP_OpenRead);
      __vdbe_add_op3(v, OP_NotExists, i_cur, 0, reg_temp); VDBE_COVERAGE(v);
      __vdbe_goto(v, i_ok);
      __vdbe_jump_here(v, __vdbe_current_addr(v)-2);
      __vdbe_jump_here(v, i_must_be_int);
      __release_temp_reg(parser, reg_temp);
    }else{
      int n_col = f_key->n_col;
      int reg_temp = __get_temp_range(parser, n_col);
  
      __vdbe_add_op3(v, OP_OpenRead, i_cur, idx->tnum, i_db);
      __vdbe_set_p4_key_info(parser, idx);
      for(i=0; i<n_col; i++){
        __vdbe_add_op2(v, OP_Copy, 
               __table_column_to_storage(f_key->from, cols[i])+1+reg_data,
               reg_temp+i);
      }
  
      /* If the parent table is the same as the child table, and we are about
      ** to increment the constraint-counter (i.e. this is an INSERT operation),
      ** then check if the row being inserted matches itself. If so, do not
      ** increment the constraint-counter. 
      **
      ** If any of the parent-key values are NULL, then the row cannot match 
      ** itself. So set JUMPIFNULL to make sure we do the OP_Found if any
      ** of the parent-key values are NULL (at this point it is known that
      ** none of the child key values are).
      */
      if( tab==f_key->from && n_incr==1 ){
        int i_jump = __vdbe_current_addr(v) + n_col + 1;
        for(i=0; i<n_col; i++){
          int i_child = __table_column_to_storage(f_key->from,cols[i])
                              +1+reg_data;
          int i_parent = 1+reg_data;
          i_parent += __table_column_to_storage(idx->table,
                                                 idx->columns[i]);
          assert( idx->columns[i]>=0 );
          assert( cols[i]!=tab->i_p_key );
          if( idx->columns[i]==tab->i_p_key ){
            /* The parent key is a composite key that includes the IPK column */
            i_parent = reg_data;
          }
          __vdbe_add_op3(v, OP_Ne, i_child, i_jump, i_parent); VDBE_COVERAGE(v);
          __vdbe_change_p5(v, DBSQL_JUMPIFNULL);
        }
        __vdbe_goto(v, i_ok);
      }

      __vdbe_add_op4(v, OP_Affinity, reg_temp, n_col, 0,
                        __index_affinity_str(parser->db,idx), n_col);
      __vdbe_add_op4_int(v, OP_Found, i_cur, i_ok, reg_temp, n_col);
      VDBE_COVERAGE(v);
      __release_temp_range(parser, reg_temp, n_col);
    }
  }

  if( !f_key->is_deferred && !(parser->db->flags & DBSQL_DeferFKs)
   && !parser->toplevel 
   && !parser->is_multi_write 
  ){
    /* Special case: If this is an INSERT statement that will insert exactly
    ** one row into the table, raise a constraint immediately instead of
    ** incrementing a counter. This is necessary as the VM code is being
    ** generated for will not open a statement transaction.  */
    assert( n_incr==1 );
    __halt_constraint(parser, DBSQL_CONSTRAINT_FOREIGNKEY,
        OE_Abort, 0, P4_STATIC, P5_ConstraintFK);
  }else{
    if( n_incr>0 && f_key->is_deferred==0 ){
      __may_abort(parser);
    }
    __vdbe_add_op2(v, OP_FkCounter, f_key->is_deferred, n_incr);
  }

  __vdbe_resolve_label(v, i_ok);
  __vdbe_add_op1(v, OP_Close, i_cur);
}


/*
** Return an expr_t object that refers to a memory register corresponding
** to column i_col of table tab.
**
** reg_base is the first of an array of register that contains the data
** for tab.  reg_base itself holds the rowid.  reg_base+1 holds the first
** column.  reg_base+2 holds the second column, and so forth.
*/
static expr_t *__expr_table_register(
  parser_t *parser,     /* Parsing and code generating context */
  table_t *tab,       /* The table whose content is at r[reg_base]... */
  int reg_base,       /* Contents of table tab */
  int16_t i_col           /* Which column of tab is desired */
){
  expr_t *expr;
  column_t *col;
  const char *coll;
  DBSQL *db = parser->db;

  expr = __expr(db, TK_REGISTER, 0);
  if( expr ){
    if( i_col>=0 && i_col!=tab->i_p_key ){
      col = &tab->cols[i_col];
      expr->i_table = reg_base + __table_column_to_storage(tab,i_col) + 1;
      expr->aff_expr = col->affinity;
      coll = __column_coll(col);
      if( coll==0 ) coll = db->dflt_coll->name;
      expr = __expr_add_collate_string(parser, expr, coll);
    }else{
      expr->i_table = reg_base;
      expr->aff_expr = DBSQL_AFF_INTEGER;
    }
  }
  return expr;
}

/*
** Return an expr_t object that refers to column i_col of table tab which
** has cursor i_cur.
*/
static expr_t *__expr_table_column(
  DBSQL *db,      /* The database connection */
  table_t *tab,      /* The table whose column is desired */
  int i_cursor,      /* The open cursor on the table */
  int16_t i_col          /* The column that is wanted */
){
  expr_t *expr = __expr(db, TK_COLUMN, 0);
  if( expr ){
    assert( EXPR_USE_Y_TAB(expr) );
    expr->y.tab = tab;
    expr->i_table = i_cursor;
    expr->i_column = i_col;
  }
  return expr;
}

/*
** This function is called to generate code executed when a row is deleted
** from the parent table of foreign key constraint f_key and, if f_key is 
** deferred, when a row is inserted into the same table. When generating
** code for an SQL UPDATE operation, this function may be called twice -
** once to "delete" the old row and once to "insert" the new row.
**
** Parameter n_incr is passed -1 when inserting a row (as this may decrease
** the number of FK violations in the db) or +1 when deleting one (as this
** may increase the number of FK constraint problems).
**
** The code generated by this function scans through the rows in the child
** table that correspond to the parent table row being deleted or inserted.
** For each child row found, one of the following actions is taken:
**
**   Operation | FK type   | Action taken
**   --------------------------------------------------------------------------
**   DELETE      immediate   Increment the "immediate constraint counter".
**
**   INSERT      immediate   Decrement the "immediate constraint counter".
**
**   DELETE      deferred    Increment the "deferred constraint counter".
**
**   INSERT      deferred    Decrement the "deferred constraint counter".
**
** These operations are identified in the comment at the top of this file 
** (fkey.c) as "I.2" and "D.2".
*/
static void __fk_scan_children(
  parser_t *parser,                  /* parser_t context */
  src_list_t *src,                  /* The child table to be scanned */
  table_t *tab,                    /* The parent table */
  index_t *idx,                    /* index_t on parent covering the foreign key */
  f_key_t *f_key,                    /* The foreign key linking src to tab */
  int *cols,                     /* Map from idx cols to child table cols */
  int reg_data,                    /* Parent row data starts here */
  int n_incr                       /* Amount to increment deferred counter by */
){
  DBSQL *db = parser->db;       /* Database handle */
  int i;                          /* Iterator variable */
  expr_t *where = 0;               /* WHERE clause to scan with */
  name_context_t s_name_context;       /* Context used to resolve WHERE clause */
  where_info_t *w_info;              /* Context used by __where_xxx() */
  int i_fk_if_zero = 0;              /* Address of OP_FkIfZero */
  vdbe_t *v = __get_vdbe(parser);

  assert( idx==0 || idx->table==tab );
  assert( idx==0 || idx->n_key_col==f_key->n_col );
  assert( idx!=0 || f_key->n_col==1 );
  assert( idx!=0 || HAS_ROWID(tab) );

  if( n_incr<0 ){
    i_fk_if_zero = __vdbe_add_op2(v, OP_FkIfZero, f_key->is_deferred, 0);
    VDBE_COVERAGE(v);
  }

  /* Create an expr_t object representing an SQL expression like:
  **
  **   <parent-key1> = <child-key1> AND <parent-key2> = <child-key2> ...
  **
  ** The collation sequence used for the comparison should be that of
  ** the parent key columns. The affinity of the parent key column should
  ** be applied to each child key value before the comparison takes place.
  */
  for(i=0; i<f_key->n_col; i++){
    expr_t *left;                  /* Value from parent table row */
    expr_t *right;                 /* column_t ref to child table */
    expr_t *eq;                    /* Expression (left = right) */
    int16_t i_col;                     /* index_t of column in child table */ 
    const char *col;             /* Name of column in child table */

    i_col = idx ? idx->columns[i] : -1;
    left = __expr_table_register(parser, tab, reg_data, i_col);
    i_col = cols ? cols[i] : f_key->cols[0].i_from;
    assert( i_col>=0 );
    col = f_key->from->cols[i_col].cn_name;
    right = __expr(db, TK_ID, col);
    eq = __p_expr(parser, TK_EQ, left, right);
    where = __expr_and(parser, where, eq);
  }

  /* If the child table is the same as the parent table, then add terms
  ** to the WHERE clause that prevent this entry from being scanned.
  ** The added WHERE clause terms are like this:
  **
  **     $current_rowid!=rowid
  **     NOT( $current_a==a AND $current_b==b AND ... )
  **
  ** The first form is used for rowid tables.  The second form is used
  ** for WITHOUT ROWID tables. In the second form, the *parent* key is
  ** (a,b,...). Either the parent or primary key could be used to 
  ** uniquely identify the current row, but the parent key is more convenient
  ** as the required values have already been loaded into registers
  ** by the caller.
  */
  if( tab==f_key->from && n_incr>0 ){
    expr_t *ne;                    /* Expression (left != right) */
    expr_t *left;                  /* Value from parent table row */
    expr_t *right;                 /* column_t ref to child table */
    if( HAS_ROWID(tab) ){
      left = __expr_table_register(parser, tab, reg_data, -1);
      right = __expr_table_column(db, tab, src->a[0].i_cursor, -1);
      ne = __p_expr(parser, TK_NE, left, right);
    }else{
      expr_t *eq, *all = 0;
      assert( idx!=0 );
      for(i=0; i<idx->n_key_col; i++){
        int16_t i_col = idx->columns[i];
        assert( i_col>=0 );
        left = __expr_table_register(parser, tab, reg_data, i_col);
        right = __expr(db, TK_ID, tab->cols[i_col].cn_name);
        eq = __p_expr(parser, TK_IS, left, right);
        all = __expr_and(parser, all, eq);
      }
      ne = __p_expr(parser, TK_NOT, all, 0);
    }
    where = __expr_and(parser, where, ne);
  }

  /* Resolve the references in the WHERE clause. */
  memset(&s_name_context, 0, sizeof(name_context_t));
  s_name_context.src_list = src;
  s_name_context.parser = parser;
  __resolve_expr_names(&s_name_context, where);

  /* Create VDBE to loop through the entries in src that match the WHERE
  ** clause. For each row found, increment either the deferred or immediate
  ** foreign key constraint counter. */
  if( parser->n_err==0 ){
    w_info = __where_begin(parser, src, where, 0, 0, 0, 0, 0);
    __vdbe_add_op2(v, OP_FkCounter, f_key->is_deferred, n_incr);
    if( w_info ){
      __where_end(w_info);
    }
  }

  /* Clean up the WHERE clause constructed above. */
  __expr_delete(db, where);
  if( i_fk_if_zero ){
    __vdbe_jump_here_or_pop_inst(v, i_fk_if_zero);
  }
}

/*
** This function returns a linked list of f_key_t objects (connected by
** f_key_t.next_to) holding all children of table tab.  For example,
** given the following schema:
**
**   CREATE TABLE t1(a PRIMARY KEY);
**   CREATE TABLE t2(b REFERENCES t1(a);
**
** Calling this function with table "t1" as an argument returns a pointer
** to the f_key_t structure representing the foreign key constraint on table
** "t2". Calling this function with "t2" as the argument would return a
** NULL pointer (as there are no FK constraints for which t2 is the parent
** table).
*/
f_key_t *__fk_references(table_t *tab){
  return (f_key_t *)__hash_find(&tab->schema->fkey_hash, tab->name);
}

/*
** The second argument is a trigger_t structure allocated by the 
** __fk_action_trigger() routine. This function deletes the trigger_t structure
** and all of its sub-components.
**
** The trigger_t structure or any of its sub-components may be allocated from
** the lookaside buffer belonging to database handle db_mem.
*/
static void __fk_trigger_delete(DBSQL *db_mem, trigger_t *p){
  if( p ){
    trigger_step_t *step = p->step_list;
    __src_list_delete(db_mem, step->src);
    __expr_delete(db_mem, step->where);
    __expr_list_delete(db_mem, step->expr_list);
    __select_delete(db_mem, step->p_select);
    __expr_delete(db_mem, p->when);
    __dbsql_db_free(db_mem, p);
  }
}

/*
** Clear the triggers[] cache of CASCADE triggers for all foreign keys
** in a particular database.  This needs to happen when the schema
** changes.
*/
void __fk_clear_trigger_cache(DBSQL *db, int i_db){
  hash_elem_t *k;
  hash_t *hash = &db->dbs[i_db].schema->tbl_hash;
  for(k=dbsql_hash_first(hash); k; k=dbsql_hash_next(k)){
    table_t *tab = dbsql_hash_data(k);
    f_key_t *f_key;
    if( !IS_ORDINARY_TABLE(tab) ) continue;
    for(f_key=tab->u.tab.f_key; f_key; f_key=f_key->next_from){
      __fk_trigger_delete(db, f_key->triggers[0]); f_key->triggers[0] = 0;
      __fk_trigger_delete(db, f_key->triggers[1]); f_key->triggers[1] = 0;
    }
  }
}

/*
** This function is called to generate code that runs when table tab is
** being dropped from the database. The src_list_t passed as the second argument
** to this function contains a single entry guaranteed to resolve to
** table tab.
**
** Normally, no code is required. However, if either
**
**   (a) The table is the parent table of a FK constraint, or
**   (b) The table is the child table of a deferred FK constraint and it is
**       determined at runtime that there are outstanding deferred FK 
**       constraint violations in the database,
**
** then the equivalent of "DELETE FROM <tbl>" is executed before dropping
** the table from the database. Triggers are disabled while running this
** DELETE, but foreign key actions are not.
*/
void __fk_drop_table(parser_t *parser, src_list_t *name, table_t *tab){
  DBSQL *db = parser->db;
  if( (db->flags&DBSQL_ForeignKeys) && IS_ORDINARY_TABLE(tab) ){
    int i_skip = 0;
    vdbe_t *v = __get_vdbe(parser);

    assert( v );                  /* VDBE has already been allocated */
    assert( IS_ORDINARY_TABLE(tab) );
    if( __fk_references(tab)==0 ){
      /* Search for a deferred foreign key constraint for which this table
      ** is the child table. If one cannot be found, return without 
      ** generating any VDBE code. If one can be found, then jump over
      ** the entire DELETE if there are no outstanding deferred constraints
      ** when this statement is run.  */
      f_key_t *p;
      for(p=tab->u.tab.f_key; p; p=p->next_from){
        if( p->is_deferred || (db->flags & DBSQL_DeferFKs) ) break;
      }
      if( !p ) return;
      i_skip = __vdbe_make_label(parser);
      __vdbe_add_op2(v, OP_FkIfZero, 1, i_skip); VDBE_COVERAGE(v);
    }

    parser->disable_triggers = 1;
    __delete_from(parser, __src_list_dup(db, name, 0), 0, 0, 0);
    parser->disable_triggers = 0;

    /* If the DELETE has generated immediate foreign key constraint 
    ** violations, halt the VDBE and return an error at this point, before
    ** any modifications to the schema are made. This is because statement
    ** transactions are not able to rollback schema changes.  
    **
    ** If the DBSQL_DeferFKs flag is set, then this is not required, as
    ** the statement transaction will not be rolled back even if FK
    ** constraints are violated.
    */
    if( (db->flags & DBSQL_DeferFKs)==0 ){
      __vdbe_verify_abortable(v, OE_Abort);
      __vdbe_add_op2(v, OP_FkIfZero, 0, __vdbe_current_addr(v)+2);
      VDBE_COVERAGE(v);
      __halt_constraint(parser, DBSQL_CONSTRAINT_FOREIGNKEY,
          OE_Abort, 0, P4_STATIC, P5_ConstraintFK);
    }

    if( i_skip ){
      __vdbe_resolve_label(v, i_skip);
    }
  }
}


/*
** The second argument points to an f_key_t object representing a foreign key
** for which tab is the child table. An UPDATE statement against tab
** is currently being processed. For each column of the table that is 
** actually updated, the corresponding element in the changes[] array
** is zero or greater (if a column is unmodified the corresponding element
** is set to -1). If the rowid column is modified by the UPDATE statement
** the chng_rowid argument is non-zero.
**
** This function returns true if any of the columns that are part of the
** child key for FK constraint *p are modified.
*/
static int __fk_child_is_modified(
  table_t *tab,                    /* table_t being updated */
  f_key_t *p,                        /* Foreign key for which tab is the child */
  int *changes,                   /* Array indicating modified columns */
  int chng_rowid                  /* True if rowid is modified by this update */
){
  int i;
  for(i=0; i<p->n_col; i++){
    int i_child_key = p->cols[i].i_from;
    if( changes[i_child_key]>=0 ) return 1;
    if( i_child_key==tab->i_p_key && chng_rowid ) return 1;
  }
  return 0;
}

/*
** The second argument points to an f_key_t object representing a foreign key
** for which tab is the parent table. An UPDATE statement against tab
** is currently being processed. For each column of the table that is 
** actually updated, the corresponding element in the changes[] array
** is zero or greater (if a column is unmodified the corresponding element
** is set to -1). If the rowid column is modified by the UPDATE statement
** the chng_rowid argument is non-zero.
**
** This function returns true if any of the columns that are part of the
** parent key for FK constraint *p are modified.
*/
static int __fk_parent_is_modified(
  table_t *tab, 
  f_key_t *p, 
  int *changes, 
  int chng_rowid
){
  int i;
  for(i=0; i<p->n_col; i++){
    char *key = p->cols[i].col;
    int i_key;
    for(i_key=0; i_key<tab->n_col; i_key++){
      if( changes[i_key]>=0 || (i_key==tab->i_p_key && chng_rowid) ){
        column_t *col = &tab->cols[i_key];
        if( key ){
          if( 0==__str_icmp(col->cn_name, key) ) return 1;
        }else if( col->col_flags & COLFLAG_PRIMKEY ){
          return 1;
        }
      }
    }
  }
  return 0;
}

/*
** Return true if the parser passed as the first argument is being
** used to code a trigger that is really a "SET NULL" action belonging
** to trigger f_key.
*/
static int __is_set_null_action(parser_t *parser, f_key_t *f_key){
  parser_t *top = __parse_toplevel(parser);
  if( top->trigger_prg ){
    trigger_t *p = top->trigger_prg->P_TRIGGER;
    if( (p==f_key->triggers[0] && f_key->actions[0]==OE_SetNull)
     || (p==f_key->triggers[1] && f_key->actions[1]==OE_SetNull)
    ){
      assert( (top->db->flags & DBSQL_FkNoAction)==0 );
      return 1;
    }
  }
  return 0;
}

/*
** This function is called when inserting, deleting or updating a row of
** table tab to generate VDBE code to perform foreign key constraint 
** processing for the operation.
**
** For a DELETE operation, parameter reg_old is passed the index of the
** first register in an array of (tab->n_col+1) registers containing the
** rowid of the row being deleted, followed by each of the column values
** of the row being deleted, from left to right. Parameter reg_new is passed
** zero in this case.
**
** For an INSERT operation, reg_old is passed zero and reg_new is passed the
** first register of an array of (tab->n_col+1) registers containing the new
** row data.
**
** For an UPDATE operation, this function is called twice. Once before
** the original record is deleted from the table using the calling convention
** described for DELETE. Then again after the original record is deleted
** but before the new record is inserted using the INSERT convention. 
*/
void __fk_check(
  parser_t *parser,                  /* parser_t context */
  table_t *tab,                    /* Row is being deleted from this table */ 
  int reg_old,                     /* Previous row data is stored here */
  int reg_new,                     /* New row data is stored here */
  int *changes,                   /* Array indicating UPDATEd columns (or 0) */
  int chng_rowid                  /* True if rowid is UPDATEd */
){
  DBSQL *db = parser->db;       /* Database handle */
  f_key_t *f_key;                    /* Used to iterate through FKs */
  int i_db;                        /* index_t of database containing tab */
  const char *z_db;                /* Name of database containing tab */
  int is_ignore_errors = parser->disable_triggers;

  /* Exactly one of reg_old and reg_new should be non-zero. */
  assert( (reg_old==0)!=(reg_new==0) );

  /* If foreign-keys are disabled, this function is a no-op. */
  if( (db->flags&DBSQL_ForeignKeys)==0 ) return;
  if( !IS_ORDINARY_TABLE(tab) ) return;

  i_db = __schema_to_index(db, tab->schema);
  assert( i_db>=00 && i_db<db->n_db );
  z_db = db->dbs[i_db].db_s_name;

  /* Loop through all the foreign key constraints for which tab is the
  ** child table (the table that the foreign key definition is part of).  */
  for(f_key=tab->u.tab.f_key; f_key; f_key=f_key->next_from){
    table_t *to;                   /* Parent table of foreign key f_key */
    index_t *idx = 0;              /* index_t on key columns in to */
    int *frees = 0;
    int *cols;
    int i_col;
    int i;
    int ignore = 0;

    if( changes 
     && dbsql_stricmp(tab->name, f_key->to)!=0
     && __fk_child_is_modified(tab, f_key, changes, chng_rowid)==0 
    ){
      continue;
    }

    /* Find the parent table of this foreign key. Also find a unique index 
    ** on the parent key columns in the parent table. If either of these 
    ** schema items cannot be located, set an error in parser and return 
    ** early.  */
    if( parser->disable_triggers ){
      to = __find_table(db, f_key->to, z_db);
    }else{
      to = __locate_table(parser, 0, f_key->to, z_db);
    }
    if( !to || __fk_locate_index(parser, to, f_key, &idx, &frees) ){
      assert( is_ignore_errors==0 || (reg_old!=0 && reg_new==0) );
      if( !is_ignore_errors || db->malloc_failed ) return;
      if( to==0 ){
        /* If is_ignore_errors is true, then a table is being dropped. In this
        ** case dbsql runs a "DELETE FROM xxx" on the table being dropped
        ** before actually dropping it in order to check FK constraints.
        ** If the parent table of an FK constraint on the current table is
        ** missing, behave as if it is empty. i.e. decrement the relevant
        ** FK counter for each row of the current table with non-NULL keys.
        */
        vdbe_t *v = __get_vdbe(parser);
        int i_jump = __vdbe_current_addr(v) + f_key->n_col + 1;
        for(i=0; i<f_key->n_col; i++){
          int i_from_col, i_reg;
          i_from_col = f_key->cols[i].i_from;
          i_reg = __table_column_to_storage(f_key->from,i_from_col) + reg_old+1;
          __vdbe_add_op2(v, OP_IsNull, i_reg, i_jump); VDBE_COVERAGE(v);
        }
        __vdbe_add_op2(v, OP_FkCounter, f_key->is_deferred, -1);
      }
      continue;
    }
    assert( f_key->n_col==1 || (frees && idx) );

    if( frees ){
      cols = frees;
    }else{
      i_col = f_key->cols[0].i_from;
      cols = &i_col;
    }
    for(i=0; i<f_key->n_col; i++){
      if( cols[i]==tab->i_p_key ){
        cols[i] = -1;
      }
      assert( idx==0 || idx->columns[i]>=0 );
#ifndef DBSQL_OMIT_AUTHORIZATION
      /* Request permission to read the parent key columns. If the 
      ** authorization callback returns DBSQL_IGNORE, behave as if any
      ** values read from the parent table are NULL. */
      if( db->auth ){
        int rcauth;
        char *col = to->cols[idx ? idx->columns[i] : to->i_p_key].cn_name;
        rcauth = __auth_read_col(parser, to->name, col, i_db);
        ignore = (rcauth==DBSQL_IGNORE);
      }
#endif
    }

    /* Take a shared-cache advisory read-lock on the parent table. Allocate 
    ** a cursor to use to search the unique index on the parent key columns 
    ** in the parent table.  */
    __table_lock(parser, i_db, to->tnum, 0, to->name);
    parser->n_tab++;

    if( reg_old!=0 ){
      /* A row is being removed from the child table. Search for the parent.
      ** If the parent does not exist, removing the child row resolves an 
      ** outstanding foreign key constraint violation. */
      __fk_lookup_parent(parser, i_db, to, idx, f_key, cols, reg_old, -1, ignore);
    }
    if( reg_new!=0 && !__is_set_null_action(parser, f_key) ){
      /* A row is being added to the child table. If a parent row cannot
      ** be found, adding the child row has violated the FK constraint. 
      **
      ** If this operation is being performed as part of a trigger program
      ** that is actually a "SET NULL" action belonging to this very 
      ** foreign key, then omit this scan altogether. As all child key
      ** values are guaranteed to be NULL, it is not possible for adding
      ** this row to cause an FK violation.  */
      __fk_lookup_parent(parser, i_db, to, idx, f_key, cols, reg_new, +1, ignore);
    }

    __dbsql_db_free(db, frees);
  }

  /* Loop through all the foreign key constraints that refer to this table.
  ** (the "child" constraints) */
  for(f_key = __fk_references(tab); f_key; f_key=f_key->next_to){
    index_t *idx = 0;              /* Foreign key index for f_key */
    src_list_t *src;
    int *cols = 0;

    if( changes && __fk_parent_is_modified(tab, f_key, changes, chng_rowid)==0 ){
      continue;
    }

    if( !f_key->is_deferred && !(db->flags & DBSQL_DeferFKs) 
     && !parser->toplevel && !parser->is_multi_write 
    ){
      assert( reg_old==0 && reg_new!=0 );
      /* Inserting a single row into a parent table cannot cause (or fix)
      ** an immediate foreign key violation. So do nothing in this case.  */
      continue;
    }

    if( __fk_locate_index(parser, tab, f_key, &idx, &cols) ){
      if( !is_ignore_errors || db->malloc_failed ) return;
      continue;
    }
    assert( cols || f_key->n_col==1 );

    /* Create a src_list_t structure containing the child table.  We need the
    ** child table as a src_list_t for __where_begin() */
    src = __src_list_append(parser, 0, 0, 0);
    if( src ){
      src_item_t *item = src->a;
      item->s_tab = f_key->from;
      item->name = f_key->from->name;
      item->s_tab->n_tab_ref++;
      item->i_cursor = parser->n_tab++;
  
      if( reg_new!=0 ){
        __fk_scan_children(parser, src, tab, idx, f_key, cols, reg_new, -1);
      }
      if( reg_old!=0 ){
        int action = f_key->actions[changes!=0];
        if( (db->flags & DBSQL_FkNoAction) ) action = OE_None;

        __fk_scan_children(parser, src, tab, idx, f_key, cols, reg_old, 1);
        /* If this is a deferred FK constraint, or a CASCADE or SET NULL
        ** action applies, then any foreign key violations caused by
        ** removing the parent key will be rectified by the action trigger.
        ** So do not set the "may-abort" flag in this case.
        **
        ** Note 1: If the FK is declared "ON UPDATE CASCADE", then the
        ** may-abort flag will eventually be set on this statement anyway
        ** (when this function is called as part of processing the UPDATE
        ** within the action trigger).
        **
        ** Note 2: At first glance it may seem like dbsql could simply omit
        ** all OP_FkCounter related scans when either CASCADE or SET NULL
        ** applies. The trouble starts if the CASCADE or SET NULL action 
        ** trigger causes other triggers or action rules attached to the 
        ** child table to fire. In these cases the fk constraint counters
        ** might be set incorrectly if any OP_FkCounter related scans are 
        ** omitted.  */
        if( !f_key->is_deferred && action!=OE_Cascade && action!=OE_SetNull ){
          __may_abort(parser);
        }
      }
      item->name = 0;
      __src_list_delete(db, src);
    }
    __dbsql_db_free(db, cols);
  }
}

#define COLUMN_MASK(x) (((x)>31) ? 0xffffffff : ((u_int32_t)1<<(x)))

/*
** This function is called before generating code to update or delete a 
** row contained in table tab.
*/
u_int32_t __fk_oldmask(
  parser_t *parser,                  /* parser_t context */
  table_t *tab                     /* table_t being modified */
){
  u_int32_t mask = 0;
  if( parser->db->flags&DBSQL_ForeignKeys && IS_ORDINARY_TABLE(tab) ){
    f_key_t *p;
    int i;
    for(p=tab->u.tab.f_key; p; p=p->next_from){
      for(i=0; i<p->n_col; i++) mask |= COLUMN_MASK(p->cols[i].i_from);
    }
    for(p=__fk_references(tab); p; p=p->next_to){
      index_t *idx = 0;
      __fk_locate_index(parser, tab, p, &idx, 0);
      if( idx ){
        for(i=0; i<idx->n_key_col; i++){
          assert( idx->columns[i]>=0 );
          mask |= COLUMN_MASK(idx->columns[i]);
        }
      }
    }
  }
  return mask;
}


/*
** This function is called before generating code to update or delete a 
** row contained in table tab. If the operation is a DELETE, then
** parameter changes is passed a NULL value. For an UPDATE, changes points
** to an array of size N, where N is the number of columns in table tab.
** If the i'th column is not modified by the UPDATE, then the corresponding 
** entry in the changes[] array is set to -1. If the column is modified,
** the value is 0 or greater. Parameter chng_rowid is set to true if the
** UPDATE statement modifies the rowid fields of the table.
**
** If any foreign key processing will be required, this function returns
** non-zero. If there is no foreign key related processing, this function 
** returns zero.
**
** For an UPDATE, this function returns 2 if:
**
**   * There are any FKs for which tab is the child and the parent table
**     and any FK processing at all is required (even of a different FK), or
**
**   * the UPDATE modifies one or more parent keys for which the action is
**     not "NO ACTION" (i.e. is CASCADE, SET DEFAULT or SET NULL).
**
** Or, assuming some other foreign key processing is required, 1.
*/
int __fk_required(
  parser_t *parser,                  /* parser_t context */
  table_t *tab,                    /* table_t being modified */
  int *changes,                   /* Non-NULL for UPDATE operations */
  int chng_rowid                   /* True for UPDATE that affects rowid */
){
  int ret = 1;                   /* Value to return if have_fk is true */
  int have_fk = 0;                /* If FK processing is required */
  if( parser->db->flags&DBSQL_ForeignKeys && IS_ORDINARY_TABLE(tab) ){
    if( !changes ){
      /* A DELETE operation. Foreign key processing is required if the 
      ** table in question is either the child or parent table for any 
      ** foreign key constraint.  */
      have_fk = (__fk_references(tab) || tab->u.tab.f_key);
    }else{
      /* This is an UPDATE. Foreign key processing is only required if the
      ** operation modifies one or more child or parent key columns. */
      f_key_t *p;

      /* Check if any child key columns are being modified. */
      for(p=tab->u.tab.f_key; p; p=p->next_from){
        if( __fk_child_is_modified(tab, p, changes, chng_rowid) ){
          if( 0==dbsql_stricmp(tab->name, p->to) ) ret = 2;
          have_fk = 1;
        }
      }

      /* Check if any parent key columns are being modified. */
      for(p=__fk_references(tab); p; p=p->next_to){
        if( __fk_parent_is_modified(tab, p, changes, chng_rowid) ){
          if( (parser->db->flags & DBSQL_FkNoAction)==0 
           && p->actions[1]!=OE_None 
          ){
            return 2;
          }
          have_fk = 1;
        }
      }
    }
  }
  return have_fk ? ret : 0;
}

/*
** This function is called when an UPDATE or DELETE operation is being 
** compiled on table tab, which is the parent table of foreign-key f_key.
** If the current operation is an UPDATE, then the changes parameter is
** passed a pointer to the list of columns being modified. If it is a
** DELETE, changes is passed a NULL pointer.
**
** It returns a pointer to a trigger_t structure containing a trigger
** equivalent to the ON UPDATE or ON DELETE action specified by f_key.
** If the action is "NO ACTION" then a NULL pointer is returned (these actions
** require no special handling by the triggers sub-system, code for them is
** created by __fk_scan_children()).
**
** For example, if f_key is the foreign key and tab is table "p" in 
** the following schema:
**
**   CREATE TABLE p(pk PRIMARY KEY);
**   CREATE TABLE c(ck REFERENCES p ON DELETE CASCADE);
**
** then the returned trigger structure is equivalent to:
**
**   CREATE TRIGGER ... DELETE ON p BEGIN
**     DELETE FROM c WHERE ck = old.pk;
**   END;
**
** The returned pointer is cached as part of the foreign key object. It
** is eventually freed along with the rest of the foreign key object by 
** __fk_delete().
*/
static trigger_t *__fk_action_trigger(
  parser_t *parser,                  /* parser_t context */
  table_t *tab,                    /* table_t being updated or deleted from */
  f_key_t *f_key,                    /* Foreign key to get action for */
  expr_list_t *changes              /* Change-list for UPDATE, NULL for DELETE */
){
  DBSQL *db = parser->db;       /* Database handle */
  int action;                     /* One of OE_None, OE_Cascade etc. */
  trigger_t *P_TRIGGER;              /* trigger_t definition to return */
  int i_action = (changes!=0);    /* 1 for UPDATE, 0 for DELETE */

  action = f_key->actions[i_action];
  if( (db->flags & DBSQL_FkNoAction) ) action = OE_None;
  if( action==OE_Restrict && (db->flags & DBSQL_DeferFKs) ){
    return 0;
  }
  P_TRIGGER = f_key->triggers[i_action];

  if( action!=OE_None && !P_TRIGGER ){
    char const *from;            /* Name of child table */
    int n_from;                    /* Length in bytes of from */
    index_t *idx = 0;              /* Parent key index for this FK */
    int *cols = 0;               /* child table cols -> parent key cols */
    trigger_step_t *step = 0;        /* First (only) step of trigger program */
    expr_t *where = 0;             /* WHERE clause of trigger step */
    expr_list_t *list = 0;          /* Changes list if ON UPDATE CASCADE */
    select_t *p_select = 0;          /* If RESTRICT, "SELECT RAISE(...)" */
    int i;                        /* Iterator variable */
    expr_t *when = 0;              /* WHEN clause for the trigger */

    if( __fk_locate_index(parser, tab, f_key, &idx, &cols) ) return 0;
    assert( cols || f_key->n_col==1 );

    for(i=0; i<f_key->n_col; i++){
      Token t_old = { "old", 3 };  /* Literal "old" token */
      Token t_new = { "new", 3 };  /* Literal "new" token */
      Token t_from_col;             /* Name of column in child table */
      Token t_to_col;               /* Name of column in parent table */
      int i_from_col;               /* Idx of column in child table */
      expr_t *eq;                  /* t_from_col = OLD.t_to_col */

      i_from_col = cols ? cols[i] : f_key->cols[0].i_from;
      assert( i_from_col>=0 );
      assert( idx!=0 || (tab->i_p_key>=0 && tab->i_p_key<tab->n_col) );
      assert( idx==0 || idx->columns[i]>=0 );
      __token_init(&t_to_col,
                   tab->cols[idx ? idx->columns[i] : tab->i_p_key].cn_name);
      __token_init(&t_from_col, f_key->from->cols[i_from_col].cn_name);

      /* Create the expression "OLD.to_col = from_col". It is important
      ** that the "OLD.to_col" term is on the LHS of the = operator, so
      ** that the affinity and collation sequence associated with the
      ** parent table are used for the comparison. */
      eq = __p_expr(parser, TK_EQ,
          __p_expr(parser, TK_DOT, 
            __expr_alloc(db, TK_ID, &t_old, 0),
            __expr_alloc(db, TK_ID, &t_to_col, 0)),
          __expr_alloc(db, TK_ID, &t_from_col, 0)
      );
      where = __expr_and(parser, where, eq);

      /* For ON UPDATE, construct the next term of the WHEN clause.
      ** The final WHEN clause will be like this:
      **
      **    WHEN NOT(old.col1 IS new.col1 AND ... AND old.col_n IS new.col_n)
      */
      if( changes ){
        eq = __p_expr(parser, TK_IS,
            __p_expr(parser, TK_DOT, 
              __expr_alloc(db, TK_ID, &t_old, 0),
              __expr_alloc(db, TK_ID, &t_to_col, 0)),
            __p_expr(parser, TK_DOT, 
              __expr_alloc(db, TK_ID, &t_new, 0),
              __expr_alloc(db, TK_ID, &t_to_col, 0))
            );
        when = __expr_and(parser, when, eq);
      }
  
      if( action!=OE_Restrict && (action!=OE_Cascade || changes) ){
        expr_t *p_new;
        if( action==OE_Cascade ){
          p_new = __p_expr(parser, TK_DOT, 
            __expr_alloc(db, TK_ID, &t_new, 0),
            __expr_alloc(db, TK_ID, &t_to_col, 0));
        }else if( action==OE_SetDflt ){
          column_t *col = f_key->from->cols + i_from_col;
          expr_t *dflt;
          if( col->col_flags & COLFLAG_GENERATED ){
            testcase( col->col_flags & COLFLAG_VIRTUAL );
            testcase( col->col_flags & COLFLAG_STORED );
            dflt = 0;
          }else{
            dflt = __column_expr(f_key->from, col);
          }
          if( dflt ){
            p_new = __expr_dup(db, dflt, 0);
          }else{
            p_new = __expr_alloc(db, TK_NULL, 0, 0);
          }
        }else{
          p_new = __expr_alloc(db, TK_NULL, 0, 0);
        }
        list = __expr_list_append(parser, list, p_new);
        __expr_list_set_name(parser, list, &t_from_col, 0);
      }
    }
    __dbsql_db_free(db, cols);

    from = f_key->from->name;
    n_from = __strlen30(from);

    if( action==OE_Restrict ){
      src_list_t *src;
      expr_t *raise; 

      raise = __expr(db, TK_STRING, "FOREIGN KEY constraint failed"),
      raise = __p_expr(parser, TK_RAISE, raise, 0);
      if( raise ){
        raise->aff_expr = OE_Abort;
      }
      src = __src_list_append(parser, 0, 0, 0);
      if( src ){
        src_item_t *item = &src->a[0];
        item->name = __db_str_dup(db, from);
        item->fg.fixed_schema = 1;
        item->u4.schema = tab->schema;
      }
      p_select = __select_new(parser, 
          __expr_list_append(parser, 0, raise),
          src,
          where,
          0, 0, 0, 0, 0
      );
      where = 0;
    }

    /* Disable lookaside memory allocation */
    DISABLE_LOOKASIDE;

    P_TRIGGER = (trigger_t *)__db_malloc_zero(db, 
        sizeof(trigger_t) +         /* struct trigger_t */
        sizeof(trigger_step_t)       /* Single step in trigger program */
    );
    if( P_TRIGGER ){
      step = P_TRIGGER->step_list = (trigger_step_t *)&P_TRIGGER[1];
      step->src = __src_list_append(parser, 0, 0, 0);
      if( step->src ){
        src_item_t *item = &step->src->a[0];
        item->name = __db_str_n_dup(db, from, n_from);
        item->u4.schema = tab->schema;
        item->fg.fixed_schema = 1;
      }
      step->where = __expr_dup(db, where, EXPRDUP_REDUCE);
      step->expr_list = __expr_list_dup(db, list, EXPRDUP_REDUCE);
      step->p_select = __select_dup(db, p_select, EXPRDUP_REDUCE);
      if( when ){
        when = __p_expr(parser, TK_NOT, when, 0);
        P_TRIGGER->when = __expr_dup(db, when, EXPRDUP_REDUCE);
      }
    }

    /* Re-enable the lookaside buffer, if it was disabled earlier. */
    ENABLE_LOOKASIDE;

    __expr_delete(db, where);
    __expr_delete(db, when);
    __expr_list_delete(db, list);
    __select_delete(db, p_select);
    if( db->malloc_failed==1 ){
      __fk_trigger_delete(db, P_TRIGGER);
      return 0;
    }
    assert( step!=0 );
    assert( P_TRIGGER!=0 );

    switch( action ){
      case OE_Restrict:
        step->op = TK_SELECT;
        break;
      case OE_Cascade: 
        if( !changes ){ 
          step->op = TK_DELETE; 
          break; 
        }
        /* no break */ deliberate_fall_through
      default:
        step->op = TK_UPDATE;
    }
    step->trig = P_TRIGGER;
    P_TRIGGER->schema = tab->schema;
    P_TRIGGER->tab_schema = tab->schema;
    f_key->triggers[i_action] = P_TRIGGER;
    P_TRIGGER->op = (changes ? TK_UPDATE : TK_DELETE);
  }

  return P_TRIGGER;
}

/*
** This function is called when deleting or updating a row to implement
** any required CASCADE, SET NULL or SET DEFAULT actions.
*/
void __fk_actions(
  parser_t *parser,                  /* parser_t context */
  table_t *tab,                    /* table_t being updated or deleted from */
  expr_list_t *p_changes,             /* Change-list for UPDATE, NULL for DELETE */
  int reg_old,                     /* Address of array containing old row */
  int *changes,                   /* Array indicating UPDATEd columns (or 0) */
  int chng_rowid                  /* True if rowid is UPDATEd */
){
  /* If foreign-key support is enabled, iterate through all FKs that 
  ** refer to table tab. If there is an action associated with the FK 
  ** for this operation (either update or delete), invoke the associated 
  ** trigger sub-program.  */
  if( parser->db->flags&DBSQL_ForeignKeys ){
    f_key_t *f_key;                  /* Iterator variable */
    for(f_key = __fk_references(tab); f_key; f_key=f_key->next_to){
      if( changes==0 || __fk_parent_is_modified(tab, f_key, changes, chng_rowid) ){
        trigger_t *act = __fk_action_trigger(parser, tab, f_key, p_changes);
        if( act ){
          __code_row_trigger_direct(parser, act, tab, reg_old, OE_Abort, 0);
        }
      }
    }
  }
}

#endif /* ifndef DBSQL_OMIT_TRIGGER */

/*
** Free all memory associated with foreign key definitions attached to
** table tab. Remove the deleted foreign keys from the schema_t.fkey_hash
** hash table.
*/
void __fk_delete(DBSQL *db, table_t *tab){
  f_key_t *f_key;                    /* Iterator variable */
  f_key_t *next;                    /* Copy of f_key->next_from */

  assert( IS_ORDINARY_TABLE(tab) );
  assert( db!=0 );
  for(f_key=tab->u.tab.f_key; f_key; f_key=next){
    assert( db==0 || __schema_mutex_held(db, 0, tab->schema) );

    /* Remove the FK from the fkey_hash hash table. */
    if( db->pn_bytes_freed==0 ){
      if( f_key->prev_to ){
        f_key->prev_to->next_to = f_key->next_to;
      }else{
        const char *z = (f_key->next_to ? f_key->next_to->to : f_key->to);
        __hash_insert(&tab->schema->fkey_hash, z, f_key->next_to);
      }
      if( f_key->next_to ){
        f_key->next_to->prev_to = f_key->prev_to;
      }
    }

    /* EV: R-30323-21917 Each foreign key constraint in dbsql is
    ** classified as either immediate or deferred.
    */
    assert( f_key->is_deferred==0 || f_key->is_deferred==1 );

    /* Delete any triggers created to implement actions for this FK. */
#ifndef DBSQL_OMIT_TRIGGER
    __fk_trigger_delete(db, f_key->triggers[0]);
    __fk_trigger_delete(db, f_key->triggers[1]);
#endif

    next = f_key->next_from;
    __dbsql_db_free(db, f_key);
  }
}
#endif /* ifndef DBSQL_OMIT_FOREIGN_KEY */
