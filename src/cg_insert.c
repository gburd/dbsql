/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * There are special exceptions to the terms and conditions of the GPL as it
 * is applied to this software. View the full text of the exception in file
 * LICENSE_EXCEPTIONS in the directory of this software distribution.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

/*
 * This file contains routines that handle INSERT statements.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __insert --
 *	This routine is call to handle SQL of the following forms:
 *
 *		insert into TABLE (IDLIST) values(EXPRLIST)
 *		insert into TABLE (IDLIST) select
 *
 *	The IDLIST following the table name is always optional.  If omitted,
 *	then a list of all columns for the table is substituted.  The IDLIST
 *	appears in the 'column' parameter.  'column' is NULL if IDLIST is
 *	omitted.
 *
 *	The 'list' parameter holds EXPRLIST in the first form of the INSERT
 *	statement above, and 'select' is NULL.  For the second form, 'list' is
 *	NULL and 'select' is a pointer to the select statement used to generate
 *	data for the insert.
 *
 *	The code generated follows one of three templates.  For a simple
 *	select with data coming from a VALUES clause, the code executes
 *	once straight down through.  The template looks like this:
 *
 *		open write cursor to <table> and its indices
 *		puts VALUES clause expressions onto the stack
 *		write the resulting record into <table>
 *		cleanup
 *
 *	If the statement is of the form
 *
 *	INSERT INTO <table> SELECT ...
 *
 *	And the SELECT clause does not read from <table> at any time, then
 *	the generated code follows this template:
 *
 *		   goto B
 *		A: setup for the SELECT
 *		   loop over the tables in the SELECT
 *		     gosub C
 *		   end loop
 *		   cleanup after the SELECT
 *		   goto D
 *		B: open write cursor to <table> and its indices
 *		   goto A
 *		C: insert the select result into <table>
 *		   return
 *		D: cleanup
 *
 *	The third template is used if the insert statement takes its
 *	values from a SELECT but the data is being inserted into a table
 *	that is also read as part of the SELECT.  In the third form,
 *	we have to use a intermediate table to store the results of
 *	the select.  The template is like this:
 *
 *		   goto B
 *		A: setup for the SELECT
 *		   loop over the tables in the SELECT
 *		     gosub C
 *		   end loop
 *		   cleanup after the SELECT
 *		   goto D
 *		C: insert the select result into the intermediate table
 *		   return
 *		B: open a cursor to an intermediate table
 *		   goto A
 *		D: open write cursor to <table> and its indices
 *		   loop over the intermediate table
 *		     transfer values form intermediate table into <table>
 *		   end the loop
 *		   cleanup
 *
 * PUBLIC: void __insert __P((parser_t *, src_list_t *, expr_list_t *,
 * PUBLIC:      select_t *, id_list_t *, int));
 *
 * parser			Parser context
 * tlist			Name of table into which we are inserting
 * vlist			List of values to be inserted
 * select			A SELECT statement to use as the data source
 * column			Column names corresponding to IDLIST
 * on_error			How to handle constraint errors
 */
void __insert(parser, tlist, vlist, select, column, on_error)
	parser_t *parser;
	src_list_t *tlist;
	expr_list_t *vlist;
	select_t *select;
	id_list_t *column;
	int on_error;
{
	int rc, init_code;
	int i, j, dx;        /* Loop counters */
	table_t *table;      /* The table to insert into */
	char *tname;         /* Name of the table into which we are inserting*/
	const char *dname;   /* Name of the database holding this table */
	vdbe_t *v;           /* Generate code into this virtual machine */
	index_t *idx;        /* For looping over indices of the table */
	int ncol;            /* Number of columns in the data */
	int base;            /* VDBE Cursor number for pTab */
	int cont, brk;       /* Beginning and end of the loop over srcTab */
	DBSQL *dbp;          /* The main database structure */
	int key_col = -1;    /* Column that is the INTEGER PRIMARY KEY */
	int end_of_loop;     /* Label for the end of the insertion loop */
	int use_temp_table;  /* Store SELECT results in intermediate table */
	int src_tab;         /* Data comes from this temporary cursor if >=0 */
	int select_loop;     /* Address of code that implements the SELECT */
	int cleanup;         /* Address of the cleanup code */
	int insert_block;    /* Address of the subroutine used to insert data*/
	int cnt_mem;         /* Memory cell used for the row counter */
	int view_p;          /* True if attempting to insert into a view */

	int row_triggers_p = 0; /* True if there are FOR EACH ROW triggers */
	int before_triggers_p;  /* True if there are BEFORE triggers */
	int after_triggers_p;   /* True if there are AFTER triggers */
	int nidx = -1;          /* Cursor for the NEW table */
	vdbe_op_t *op;
	src_list_t dummy;

	if (parser->nErr)
		goto insert_cleanup;

	dbp = parser->db;

	/*
	 * Locate the table into which we will be inserting new information.
	 */
	DBSQL_ASSERT(tlist->nSrc == 1);
	tname = tlist->a[0].zName;
	if (tname == 0)
		goto insert_cleanup;
	table = __src_list_lookup(parser, tlist);
	if (table == 0) {
		goto insert_cleanup;
	}
	DBSQL_ASSERT(table->iDb < dbp->nDb);
	dname = dbp->aDb[table->iDb].zName;
	if (__auth_check(parser, DBSQL_INSERT, table->zName, 0, dname)) {
		goto insert_cleanup;
	}

	/*
	 * Ensure that:
	 *  (a) the table is not read-only, 
	 *  (b) that if it is a view then ON INSERT triggers exist
	 */
	before_triggers_p = __triggers_exist(parser, table->pTrigger,
					     TK_INSERT, TK_BEFORE, TK_ROW, 0);
	after_triggers_p = __triggers_exist(parser, table->pTrigger,
					    TK_INSERT, TK_AFTER, TK_ROW, 0);
	row_triggers_p = before_triggers_p || after_triggers_p;
	view_p = (table->pSelect != 0);
	if (__is_table_read_only(parser, table, before_triggers_p)) {
		goto insert_cleanup;
	}
	if (table == 0)
		goto insert_cleanup;

	/*
	 * If 'table' is really a view, make sure it has been initialized.
	 */
	if (view_p && __view_get_column_names(parser, table)) {
		goto insert_cleanup;
	}

	/* 
	 * Allocate a VDBE
	 */
	v = __parser_get_vdbe(parser);
	if (v == 0)
		goto insert_cleanup;
	__vdbe_prepare_write(parser, select || row_triggers_p, table->iDb);

	/*
	 * If there are row triggers, allocate a temp table for new.*
	 * references.
	 */
	if (row_triggers_p) {
		nidx = parser->nTab++;
	}

	/*
	 * Figure out how many columns of data are supplied.  If the data
	 * is coming from a SELECT statement, then this step also generates
	 * all the code to implement the SELECT statement and invoke a
	 * subroutine to process each row of the result. (Template 2.) If
	 * the SELECT statement uses the the table that is being inserted
	 * into, then the subroutine is also coded here.  That subroutine
	 * stores the SELECT results in a temporary table. (Template 3.)
	 */
	if (select) {
		/*
		 * Data is coming from a SELECT.  Generate code to implement
		 * that SELECT.
		 */
		init_code = __vdbe_add_op(v, OP_Goto, 0, 0);
		select_loop = __vdbe_current_addr(v);
		insert_block = __vdbe_make_label(v);
		rc = __select(parser, select, SRT_Subroutine, insert_block,
			      0,0,0);
		if (rc || parser->nErr)
			goto insert_cleanup;
		cleanup = __vdbe_make_label(v);
		__vdbe_add_op(v, OP_Goto, 0, cleanup);
		DBSQL_ASSERT(select->pEList);
		ncol = select->pEList->nExpr;

		/*
		 * Set use_temp_table to TRUE if the result of the SELECT
		 * statement should be written into a temporary table.  Set
		 * to FALSE if each row of the SELECT can be written directly
		 * into the result table.
		 *
		 * A temp table must be used if the table being updated is
		 * also one of the tables being read by the SELECT statement.
		 * Also use a temp table in the case of row triggers.
		 */
		if (row_triggers_p) {
			use_temp_table = 1;
		} else {
			int addr = __vdbe_find_op(v, OP_OpenRead, table->tnum);
			use_temp_table = 0;
			if (addr > 0) {
				op = __vdbe_get_op(v, addr-2);
				if (op->opcode == OP_Integer &&
				    op->p1 == table->iDb) {
					use_temp_table = 1;
				}
			}
		}

		if (use_temp_table) {
			/*
			 * Generate the subroutine that SELECT calls to
			 * process each row of the result.  Store the result
			 * in a temporary table.
			 */
			src_tab = parser->nTab++;
			__vdbe_resolve_label(v, insert_block);
			__vdbe_add_op(v, OP_MakeRecord, ncol, 0);
			__vdbe_add_op(v, OP_NewRecno, src_tab, 0);
			__vdbe_add_op(v, OP_Pull, 1, 0);
			__vdbe_add_op(v, OP_PutIntKey, src_tab, 0);
			__vdbe_add_op(v, OP_Return, 0, 0);

			/*
			 * The following code runs first because the GOTO at
			 * the very top of the program jumps to it.  Create
			 * the temporary table, then jump back up and execute
			 * the SELECT code above.
			 */
			__vdbe_change_p2(v, init_code, __vdbe_current_addr(v));
			__vdbe_add_op(v, OP_OpenTemp, src_tab, 0);
			__vdbe_add_op(v, OP_Goto, 0, select_loop);
			__vdbe_resolve_label(v, cleanup);
		} else {
			__vdbe_change_p2(v, init_code, __vdbe_current_addr(v));
		}
	} else {
		/*
		 * This is the case if the data for the INSERT is coming from
		 * a VALUES clause.
		 */
		DBSQL_ASSERT(vlist != 0);
		src_tab = -1;
		use_temp_table = 0;
		DBSQL_ASSERT(vlist);
		ncol = vlist->nExpr;
		dummy.nSrc = 0;
		for(i = 0; i < ncol; i++) {
			if (__expr_resolve_ids(parser, &dummy, 0,
					       vlist->a[i].pExpr)) {
				goto insert_cleanup;
			}
			if (__expr_check(parser, vlist->a[i].pExpr, 0, 0)) {
				goto insert_cleanup;
			}
		}
	}

	/*
	 * Make sure the number of columns in the source data matches the
	 * number of columns to be inserted into the table.
	 */
	if (column == 0 && ncol != table->nCol) {
		__error_msg(parser,
			    "table %S has %d columns but %d values "
			    "were supplied", tlist, 0, table->nCol, ncol);
		goto insert_cleanup;
	}
	if (column != 0 && ncol != column->nId) {
		__error_msg(parser, "%d values for %d columns", ncol,
			    column->nId);
		goto insert_cleanup;
	}

	/*
	 * If the INSERT statement included an IDLIST term, then make sure
	 * all elements of the IDLIST really are columns of the table and 
	 * remember the column indices.
	 *
	 * If the table has an INTEGER PRIMARY KEY column and that column
	 * is named in the IDLIST, then record in the key_col variable
	 * the index into IDLIST of the primary key column.  key_col is
	 * the index of the primary key as it appears in IDLIST, not as
	 * is appears in the original table.  (The index of the primary
	 * key in the original table is table->iPKey.)
	 */
	if (column) {
		for(i = 0; i < column->nId; i++) {
			column->a[i].idx = -1;
		}
		for(i = 0; i < column->nId; i++) {
			for(j = 0; j < table->nCol; j++) {
				if (strcasecmp(column->a[i].zName,
					  table->aCol[j].zName) == 0) {
					column->a[i].idx = j;
					if (j == table->iPKey) {
						key_col = i;
					}
					break;
				}
			}
			if(j >= table->nCol) {
				if (__is_row_id(column->a[i].zName)) {
					key_col = i;
				} else {
					__error_msg(parser,
					    "table %S has no column named %s",
					    tlist, 0, column->a[i].zName);
					parser->nErr++;
					goto insert_cleanup;
				}
			}
		}
	}

	/*
	 * If there is no IDLIST term but the table has an integer primary
	 * key, then set the key_col variable to the primary key column
	 * index in the original table definition.
	 */
	if (column == 0) {
		key_col = table->iPKey;
	}

	/*
	 * Open the temp table for FOR EACH ROW triggers.
	 */
	if (row_triggers_p) {
		__vdbe_add_op(v, OP_OpenPseudo, nidx, 0);
	}
    
	/*
	 * Initialize the count of rows to be inserted.
	 */
	if (dbp->flags & DBSQL_CountRows) {
		cnt_mem = parser->nMem++;
		__vdbe_add_op(v, OP_Integer, 0, 0);
		__vdbe_add_op(v, OP_MemStore, cnt_mem, 1);
	}

	/*
	 * Open tables and indices if there are no row triggers.
	 */
	if (!row_triggers_p) {
		base = parser->nTab;
		__vdbe_add_op(v, OP_Integer, table->iDb, 0);
		__vdbe_add_op(v, OP_OpenWrite, base, table->tnum);
		__vdbe_change_p3(v, -1, table->zName, P3_STATIC);
		for (dx = 1, idx = table->pIndex; idx; idx = idx->pNext, dx++){
			__vdbe_add_op(v, OP_Integer, idx->iDb, 0);
			__vdbe_add_op(v, OP_OpenWrite, (dx + base), idx->tnum);
			__vdbe_change_p3(v, -1, idx->zName, P3_STATIC);
		}
		parser->nTab += dx;
	}

	/*
	 * If the data source is a temporary table, then we have to create
	 * a loop because there might be multiple rows of data.  If the data
	 * source is a subroutine call from the SELECT statement, then we need
	 * to launch the SELECT statement processing.
	 */
	if (use_temp_table) {
		brk = __vdbe_make_label(v);
		__vdbe_add_op(v, OP_Rewind, src_tab, brk);
		cont = __vdbe_current_addr(v);
	} else if (select) {
		__vdbe_add_op(v, OP_Goto, 0, select_loop);
		__vdbe_resolve_label(v, insert_block);
	}

	/*
	 * Run the BEFORE and INSTEAD OF triggers, if there are any.
	 */
	end_of_loop = __vdbe_make_label(v);
	if (before_triggers_p) {

		/*
		 * build the new.* reference row.  Note that if there is
		 * an INTEGER PRIMARY KEY into which a NULL is being
		 * inserted, that NULL will be translated into a unique ID
		 * for the row.  But on a BEFORE trigger, we do not know what
		 * the unique ID will be (because the insert has not happened
		 * yet) so we substitute a rowid of -1.
		 */
		if (key_col < 0) {
			__vdbe_add_op(v, OP_Integer, -1, 0);
		} else if (use_temp_table) {
			__vdbe_add_op(v, OP_Column, src_tab, key_col);
		} else if (select) {
			__vdbe_add_op(v, OP_Dup, ncol - key_col - 1, 1);
		} else {
			__expr_code(parser, vlist->a[key_col].pExpr);
			__vdbe_add_op(v, OP_NotNull, -1,
				      (__vdbe_current_addr(v) + 3));
			__vdbe_add_op(v, OP_Pop, 1, 0);
			__vdbe_add_op(v, OP_Integer, -1, 0);
			__vdbe_add_op(v, OP_MustBeInt, 0, 0);
		}

		/*
		 * Create the new column data.
		 */
		for (i = 0; i < table->nCol; i++) {
			if (column == 0) {
				j = i;
			} else {
				for (j = 0; j < column->nId; j++) {
					if (column->a[j].idx == i)
						break;
				}
			}
			if (column && j >= column->nId) {
				__vdbe_add_op(v, OP_String, 0, 0);
				__vdbe_change_p3(v, -1, table->aCol[i].zDflt,
						 P3_STATIC);
			} else if (use_temp_table) {
				__vdbe_add_op(v, OP_Column, src_tab, j); 
			} else if (select) {
				__vdbe_add_op(v, OP_Dup, (ncol - j - 1), 1);
			} else {
				__expr_code(parser, vlist->a[j].pExpr);
			}
		}
		__vdbe_add_op(v, OP_MakeRecord, table->nCol, 0);
		__vdbe_add_op(v, OP_PutIntKey, nidx, 0);

		/*
		 * Fire BEFORE or INSTEAD OF triggers.
		 */
		if (__code_row_trigger(parser, TK_INSERT, 0, TK_BEFORE, table, 
				       nidx, -1, on_error, end_of_loop)) {
			goto insert_cleanup;
		}
	}

	/*
	 * If any triggers exists, the opening of tables and indices is
	 * deferred until now.
	 */
	if (row_triggers_p && !view_p) {
		base = parser->nTab;
		__vdbe_add_op(v, OP_Integer, table->iDb, 0);
		__vdbe_add_op(v, OP_OpenWrite, base, table->tnum);
		__vdbe_change_p3(v, -1, table->zName, P3_STATIC);
		for (dx = 1, idx = table->pIndex; idx; idx = idx->pNext, dx++){
			__vdbe_add_op(v, OP_Integer, idx->iDb, 0);
			__vdbe_add_op(v, OP_OpenWrite, (dx + base), idx->tnum);
			__vdbe_change_p3(v, -1, idx->zName, P3_STATIC);
		}
		parser->nTab += dx;
	}

	/*
	 * Push the record number for the new entry onto the stack.  The
	 * record number is a randomly generate integer created by NewRecno
	 * except when the table has an INTEGER PRIMARY KEY column, in which
	 * case the record number is the same as that column. 
	 */
	if (!view_p) {
		if (key_col >= 0) {
			if (use_temp_table) {
				__vdbe_add_op(v, OP_Column, src_tab,
					      key_col);
			} else if (select) {
				__vdbe_add_op(v, OP_Dup,
					      (ncol - key_col - 1), 1);
			} else {
				__expr_code(parser, vlist->a[key_col].pExpr);
			}
			/*
			 * If the PRIMARY KEY expression is NULL, then use
			 * OP_NewRecno to generate a unique primary key value.
			 */
			__vdbe_add_op(v, OP_NotNull, -1,
				      (__vdbe_current_addr(v) + 3));
			__vdbe_add_op(v, OP_Pop, 1, 0);
			__vdbe_add_op(v, OP_NewRecno, base, 0);
			__vdbe_add_op(v, OP_MustBeInt, 0, 0);
		} else {
			__vdbe_add_op(v, OP_NewRecno, base, 0);
		}

		/*
		 * Push onto the stack, data for all columns of the new
		 * entry, beginning with the first column.
		 */
		for (i = 0; i < table->nCol; i++) {
			if (i == table->iPKey) {
				/*
				 * The value of the INTEGER PRIMARY KEY
				 * column is always a NULL.  Whenever this
				 * column is read, the record number will be
				 * substituted in its place.  So will fill
				 * this column with a NULL to avoid taking up
				 * data space with information that will never
				 * be used.
				 */
				__vdbe_add_op(v, OP_String, 0, 0);
				continue;
			}
			if (column == 0) {
				j = i;
			} else {
				for (j = 0; j < column->nId; j++) {
					if (column->a[j].idx == i)
						break;
				}
			}
			if (column && j >= column->nId) {
				__vdbe_add_op(v, OP_String, 0, 0);
				__vdbe_change_p3(v, -1, table->aCol[i].zDflt,
						 P3_STATIC);
			} else if (use_temp_table) {
				__vdbe_add_op(v, OP_Column, src_tab, j); 
			} else if (select) {
				__vdbe_add_op(v, OP_Dup, (i + ncol - j), 1);
			} else {
				__expr_code(parser, vlist->a[j].pExpr);
			}
		}

		/*
		 * Generate code to check constraints and generate index
		 * keys and do the insertion.
		 */
		__generate_constraint_checks(parser, table, base, 0,
					     key_col >= 0, 0, on_error,
					     end_of_loop);
		__complete_insertion(parser, table, base, 0, 0, 0,
				     after_triggers_p ? nidx : -1);
	}

	/*
	 * Update the count of rows that are inserted.
	 */
	if ((dbp->flags & DBSQL_CountRows) != 0) {
		__vdbe_add_op(v, OP_MemIncr, cnt_mem, 0);
	}

	if (row_triggers_p) {
		/* Close all tables opened */
		if (!view_p) {
			__vdbe_add_op(v, OP_Close, base, 0);
			for(dx = 1, idx = table->pIndex; idx;
			    idx = idx->pNext, dx++) {
				__vdbe_add_op(v, OP_Close, (dx + base), 0);
			}
		}

		/* Code AFTER triggers */
		if (__code_row_trigger(parser, TK_INSERT, 0, TK_AFTER, table,
				       nidx, -1, on_error, end_of_loop)) {
			goto insert_cleanup;
		}
	}

	/*
	 * The bottom of the loop, if the data source is a SELECT statement.
	 */
	__vdbe_resolve_label(v, end_of_loop);
	if (use_temp_table) {
		__vdbe_add_op(v, OP_Next, src_tab, cont);
		__vdbe_resolve_label(v, brk);
		__vdbe_add_op(v, OP_Close, src_tab, 0);
	} else if (select) {
		__vdbe_add_op(v, OP_Pop, ncol, 0);
		__vdbe_add_op(v, OP_Return, 0, 0);
		__vdbe_resolve_label(v, cleanup);
	}

	if (!row_triggers_p) {
		/* Close all tables opened */
		__vdbe_add_op(v, OP_Close, base, 0);
		for(dx = 1, idx = table->pIndex; idx; idx = idx->pNext, dx++){
			__vdbe_add_op(v, OP_Close, (dx + base), 0);
		}
	}

	__vdbe_conclude_write(parser);

	/*
	 * Return the number of rows inserted.
	 */
	if (dbp->flags & DBSQL_CountRows) {
		__vdbe_add_op(v, OP_ColumnName, 0, 0);
		__vdbe_change_p3(v, -1, "rows inserted", P3_STATIC);
		__vdbe_add_op(v, OP_MemLoad, cnt_mem, 0);
		__vdbe_add_op(v, OP_Callback, 1, 0);
	}

  insert_cleanup:
	__src_list_delete(tlist);
	if (vlist )
		__expr_list_delete(vlist);
	if (select)
		__select_delete(select);
	__id_list_delete(column);
}

/*
 * __generate_constraint_checks --
 *	Generate code to do a constraint check prior to an INSERT or an UPDATE.
 *
 *	When this routine is called, the stack contains (from bottom to top)
 *	the following values:
 *
 *    1.  The recno of the row to be updated before the update.  This
 *        value is omitted unless we are doing an UPDATE that involves a
 *        change to the record number.
 *
 *    2.  The recno of the row after the update.
 *
 *    3.  The data in the first column of the entry after the update.
 *
 *    i.  Data from middle columns...
 *
 *    N.  The data in the last column of the entry after the update.
 *
 *	The old recno shown as entry (1) above is omitted unless both
 *	'update_p' and 'will_recno_chng_p' are 1.  'update_p' is true for
 *	UPDATEs and false for INSERTs and 'will_recno_chng_p' is true if
 *	the record number is being changed.
 *
 *	The code generated by this routine pushes additional entries onto
 *	the stack which are the keys for new index entries for the new record.
 *	The order of index keys is the same as the order of the indices on
 *	the table->pIndex list.  A key is only created for index i if 
 *	used_indicies!=0 and used_indicies[i]!=0.
 *
 *	This routine also generates code to check constraints.  NOT NULL,
 *	CHECK, and UNIQUE constraints are all checked.  If a constraint fails,
 *	then the appropriate action is performed.  There are five possible
 *	actions: ROLLBACK, ABORT, FAIL, REPLACE, and IGNORE.
 *
 *  Constraint type  Action       What Happens
 *  ---------------  ----------   ----------------------------------------
 *  any              ROLLBACK     The current transaction is rolled back and
 *                                dbsql_exec() returns immediately with a
 *                                return code of DBSQL_CONSTRAINT.
 *
 *  any              ABORT        Back out changes from the current command
 *                                only (do not do a complete rollback) then
 *                                cause dbsql_exec() to return immediately
 *                                with DBSQL_CONSTRAINT.
 *
 *  any              FAIL         dbsql_exec() returns immediately with a
 *                                return code of DBSQL_CONSTRAINT.  The
 *                                transaction is not rolled back and any
 *                                prior changes are retained.
 *
 *  any              IGNORE       The record number and data is popped from
 *                                the stack and there is an immediate jump
 *                                to label 'ignore_dest'.
 *
 *  NOT NULL         REPLACE      The NULL value is replace by the default
 *                                value for that column.  If the default value
 *                                is NULL, the action is the same as ABORT.
 *
 *  UNIQUE           REPLACE      The other row that conflicts with the row
 *                                being inserted is removed.
 *
 *  CHECK            REPLACE      Illegal.  The results in an exception.
 *
 *	Which action to take is determined by the override_error parameter.
 *	Or if override_error==OE_Default, then the parser->onError parameter
 *	is used.  Or if parser->onError==OE_Default then the onError value
 *	for the constraint is used.
 *
 *	The calling routine must open a read/write cursor for table with
 *	cursor number 'base'.  All indices of table must also have open
 *	read/write cursors with cursor number base+i for the i-th cursor.
 *	Except, if there is no possibility of a REPLACE action then
 *	cursors do not need to be open for indices where used_indicies[i]==0.
 *
 *	If the 'update_p' flag is true, it means that the 'base' cursor is
 *	initially pointing to an entry that is being updated.  The 'update_p'
 *	flag causes extra code to be generated so that the 'base' cursor
 *	is still pointing at the same entry after the routine returns.
 *	Without the 'update_p' flag, the 'base' cursor might be moved.
 *
 * PUBLIC: void __generate_constraint_checks __P((parser_t *, table_t *, int,
 * PUBLIC:                                   char *, int, int, int, int));
 *
 * parser			The parser context
 * table			The table into which we are inserting
 * base				Index of a read/write cursor pointing at table
 * used_indicies		Which indices are used.  If NULL, all are used
 * will_recno_chng_p		True if the record number will change
 * update_p			True for UPDATE, False for INSERT
 * override_error		Override onError to this if not OE_Default
 * ignore_dest			Jump to this label on an OE_Ignore resolution
 */
void
__generate_constraint_checks(parser, table, base, used_indicies,
			     will_recno_chng_p, update_p, override_error,
			     ignore_dest)
	parser_t *parser;
	table_t *table;
	int base;
	char *used_indicies;
	int will_recno_chng_p;
	int update_p;
	int override_error;
	int ignore_dest;
{
	int i;
	vdbe_t *v;
	int num_col;
	int on_error;
	int addr;
	int extra;
	int cur;
	index_t *index;
	int seen_replace_p = 0;
	int jmp_inst_1, jmp_inst_2;
	int cont_addr;
	int two_recnos_p = (update_p && will_recno_chng_p);
	char *msg = 0;
	char *dup_msg;
        int j, n1, n2;
        char err_msg[200];
	char *col;

	v = __parser_get_vdbe(parser);
	DBSQL_ASSERT(v != 0);
	DBSQL_ASSERT(table->pSelect == 0);  /* This table is not a VIEW */
	num_col = table->nCol;

	/*
	 * Test all NOT NULL constraints.
	 */
	for (i = 0; i < num_col; i++) {
		if (i == table->iPKey) {
			continue;
		}
		on_error = table->aCol[i].notNull;
		if (on_error == OE_None)
			continue;
		if (override_error != OE_Default) {
			on_error = override_error;
		} else if (parser->db->onError != OE_Default) {
			on_error = parser->db->onError;
		} else if (on_error == OE_Default) {
			on_error = OE_Abort;
		}
		if (on_error == OE_Replace && table->aCol[i].zDflt == 0) {
			on_error = OE_Abort;
		}
		__vdbe_add_op(v, OP_Dup, (num_col - 1 - i), 1);
		addr = __vdbe_add_op(v, OP_NotNull, 1, 0);
		switch(on_error) {
		case OE_Rollback: /* FALLTHROUGH */
		case OE_Abort: /* FALLTHROUGH */
		case OE_Fail:
			msg = 0; /* Reset msg or set_string does bad things. */
			__vdbe_add_op(v, OP_Halt, DBSQL_CONSTRAINT, on_error);
			__str_append(&msg, table->zName, ".",
				     table->aCol[i].zName,
				     " may not be NULL", (char*)0);
			__vdbe_change_p3(v, -1, msg, P3_DYNAMIC);
			break;
		case OE_Ignore:
			__vdbe_add_op(v, OP_Pop, (num_col + 1 + two_recnos_p),
				      0);
			__vdbe_add_op(v, OP_Goto, 0, ignore_dest);
			break;
		case OE_Replace:
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_change_p3(v, -1, table->aCol[i].zDflt,
					 P3_STATIC);
			__vdbe_add_op(v, OP_Push, (num_col - i), 0);
			break;
		default:
			DBSQL_ASSERT(0);
			break;
		}
		__vdbe_change_p2(v, addr, __vdbe_current_addr(v));
	}

	/*
	 * Test all CHECK constraints
	 */

	/*
	 * If we have an INTEGER PRIMARY KEY, make sure the primary key
	 * of the new record does not previously exist.  Except, if this
	 * is an UPDATE and the primary key is not changing, that is OK.
	 */
	if (will_recno_chng_p) {
		on_error = table->keyConf;
		if (override_error != OE_Default) {
			on_error = override_error;
		} else if (parser->db->onError != OE_Default) {
			on_error = parser->db->onError;
		} else if (on_error == OE_Default) {
			on_error = OE_Abort;
		}
    
		if (update_p) {
			__vdbe_add_op(v, OP_Dup, (num_col + 1), 1);
			__vdbe_add_op(v, OP_Dup, (num_col + 1), 1);
			jmp_inst_1 = __vdbe_add_op(v, OP_Eq, 0, 0);
		}
		__vdbe_add_op(v, OP_Dup, num_col, 1);
		jmp_inst_2 = __vdbe_add_op(v, OP_NotExists, base, 0);
		switch(on_error) {
		default:
			on_error = OE_Abort; /* FALLTHROUGH */
		case OE_Rollback: /* FALLTHROUGH */
		case OE_Abort: /* FALLTHROUGH */
		case OE_Fail:
			__vdbe_add_op(v, OP_Halt, DBSQL_CONSTRAINT, on_error);
			__vdbe_change_p3(v, -1, "PRIMARY KEY must be unique",
					 P3_STATIC);
			break;
		case OE_Replace:
			__generate_row_index_delete(parser->db, v, table,
						    base, 0);
			if (update_p) {
				__vdbe_add_op(v, OP_Dup, (num_col +
							  two_recnos_p), 1);
				__vdbe_add_op(v, OP_MoveTo, base, 0);
			}
			seen_replace_p = 1;
			break;
		case OE_Ignore:
			DBSQL_ASSERT(seen_replace_p == 0);
			__vdbe_add_op(v, OP_Pop, (num_col + 1 + two_recnos_p),
				      0);
			__vdbe_add_op(v, OP_Goto, 0, ignore_dest);
			break;
		}
		cont_addr = __vdbe_current_addr(v);
		__vdbe_change_p2(v, jmp_inst_2, cont_addr);
		if (update_p) {
			__vdbe_change_p2(v, jmp_inst_1, cont_addr);
			__vdbe_add_op(v, OP_Dup, (num_col + 1), 1);
			__vdbe_add_op(v, OP_MoveTo, base, 0);
		}
	}

	/*
	 * Test all UNIQUE constraints by creating entries for each UNIQUE
	 * index and making sure that duplicate entries do not already exist.
	 * Add the new records to the indices as we go.
	 */
	extra = -1;
	for (cur = 0, index = table->pIndex; index;
	     index = index->pNext, cur++) {
		if (used_indicies && used_indicies[cur] == 0)
			continue;  /* Skip unused indices */
		extra++;

		/*
		 * Create a key for accessing the index entry.
		 */
		__vdbe_add_op(v, OP_Dup, (num_col + extra), 1);
		for (i = 0; i < index->nColumn; i++) {
			int idx = index->aiColumn[i];
			if (idx == table->iPKey) {
				__vdbe_add_op(v, OP_Dup, (i + extra +
							  num_col + 1), 1);
			} else {
				__vdbe_add_op(v, OP_Dup, (i + extra +
							  num_col - idx), 1);
			}
		}
		jmp_inst_1 = __vdbe_add_op(v, OP_MakeIdxKey, index->nColumn,0);
		__add_idx_key_type(v, index);

		/*
		 * Find out what action to take in case there is an
		 * indexing conflict.
		 */
		on_error = index->onError;
		if (on_error == OE_None)
			continue;  /* index is not a UNIQUE index */
		if (override_error != OE_Default) {
			on_error = override_error;
		} else if (parser->db->onError != OE_Default) {
			on_error = parser->db->onError;
		} else if (on_error == OE_Default) {
			on_error = OE_Abort;
		}
		if (seen_replace_p) {
			if (on_error == OE_Ignore)
				on_error = OE_Replace;
			else if (on_error == OE_Fail)
				on_error = OE_Abort;
		}

		/*
		 * Check to see if the new index entry will be unique.
		 */
		__vdbe_add_op(v, OP_Dup, (extra + num_col + 1 +
					  two_recnos_p), 1);
		jmp_inst_2 = __vdbe_add_op(v, OP_IsUnique, (base + cur + 1),
					   0);

		/*
		 * Generate code that executes if the new index entry
		 * is not unique.
		 */
		switch(on_error) {
		case OE_Rollback: /* FALLTHROUGH */
		case OE_Abort: /* FALLTHROUGH */
		case OE_Fail:
			strcpy(err_msg, (index->nColumn>1 ?
					 "columns " : "column "));
			n1 = strlen(err_msg);
			for (j = 0; j < index->nColumn &&
				     n1 < (sizeof(err_msg) - 30); j++) {
				col = table->aCol[index->aiColumn[j]].zName;
				n2 = strlen(col);
				if (j > 0) {
					strcpy(&err_msg[n1], ", ");
					n1 += 2;
				}
				if ((n1 + n2) > (sizeof(err_msg) - 30)) {
					strcpy(&err_msg[n1], "...");
					n1 += 3;
					break;
				} else {
					strcpy(&err_msg[n1], col);
					n1 += n2;
				}
			}
			strcpy(&err_msg[n1], (index->nColumn > 1 ?
				      " are not unique" : " is not unique"));
			__vdbe_add_op(v, OP_Halt, DBSQL_CONSTRAINT, on_error);
			__dbsql_strdup(parser->db, err_msg, &dup_msg);
			__vdbe_change_p3(v, -1, dup_msg, P3_DYNAMIC);
			break;
		case OE_Ignore:
			DBSQL_ASSERT(seen_replace_p == 0);
			__vdbe_add_op(v, OP_Pop, (num_col + extra + 3 +
						  two_recnos_p), 0);
			__vdbe_add_op(v, OP_Goto, 0, ignore_dest);
			break;
		case OE_Replace:
			__generate_row_delete(parser->db, v, table, base, 0);
			if (update_p) {
				__vdbe_add_op(v, OP_Dup, (num_col + extra + 1 +
							  two_recnos_p), 1);
				__vdbe_add_op(v, OP_MoveTo, base, 0);
			}
			seen_replace_p = 1;
			break;
		default:
			DBSQL_ASSERT(0);
			break;
		}
		cont_addr = __vdbe_current_addr(v);
#if NULL_DISTINCT_FOR_UNIQUE
		__vdbe_change_p2(v, jmp_inst_1, cont_addr);
#endif
		__vdbe_change_p2(v, jmp_inst_2, cont_addr);
	}
}

/*
 * __complete_insertion --
 *	This routine generates code to finish the INSERT or UPDATE operation
 *	that was started by a prior call to __generate_constraint_checks.
 *	The stack must contain keys for all active indices followed by data
 *	and the recno for the new entry.  This routine creates the new
 *	entries in all indices and in the main table.
 *
 *	The arguments to this routine should be the same as the first six
 *	arguments to __generate_constraint_checks.
 *
 * PUBLIC: void __complete_insertion __P((parser_t *, table_t *, int, char *,
 * PUBLIC:                           int, int, int));
 *
 * parser			The parser context
 * table			The table into which we are inserting
 * base				Index of a read/write cursor pointing at table
 * used_indicies		Which indices are used.  If NULL, all are used
 * will_recno_chng_p		True if the record number will change
 * update_p			True for UPDATE, False for INSERT
 * new_idx			Index of NEW table for triggers,  -1 if none 
 */
void __complete_insertion(parser, table, base, used_indicies,
			  will_recno_chng_p, update_p, new_idx)
	parser_t *parser;
	table_t *table;
	int base;
	char *used_indicies;
	int will_recno_chng_p;
	int update_p;
	int new_idx;
{
	int i;
	vdbe_t *v;
	int idx;
	index_t *index;

	v = __parser_get_vdbe(parser);
	DBSQL_ASSERT(v != 0);
	DBSQL_ASSERT(table->pSelect == 0);  /* This table is not a VIEW */
	idx = 0;
	index = table->pIndex;
	while (index) {
		index = index->pNext;
		idx++;
	}
	for (i = (idx - 1); i >= 0; i--) {
		if (used_indicies && used_indicies[i] == 0)
			continue;
		__vdbe_add_op(v, OP_IdxPut, base + i + 1, 0);
	}
	__vdbe_add_op(v, OP_MakeRecord, table->nCol, 0);
	if (new_idx >= 0) {
		__vdbe_add_op(v, OP_Dup, 1, 0);
		__vdbe_add_op(v, OP_Dup, 1, 0);
		__vdbe_add_op(v, OP_PutIntKey, new_idx, 0);
	}
	__vdbe_add_op(v, OP_PutIntKey, base, (parser->trigStack ? 0 : 1));
	if (update_p && will_recno_chng_p) {
		__vdbe_add_op(v, OP_Pop, 1, 0);
	}
}
