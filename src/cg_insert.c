/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file contains routines that handle INSERT statements.
 *
 * CONVERTED TO REGISTER-BASED VDBE (SQLite 3.52.0 architecture)
 * Task #22: Convert INSERT code generator from stack to register-based
 *
 * Key changes:
 * - Use register allocation (regRowid, regData) instead of stack
 * - OP_NewRecno with P2=target register instead of OP_NewRecno
 * - OP_MakeRecord with P1=first_reg, P2=count, P3=dest_reg
 * - OP_Insert with P2=data_reg, P3=key_reg instead of OP_PutIntKey
 * - Register-based trigger code generation
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __insert --
 *	This routine is call to handle SQL of the following forms:
 *
 *		insert into TABLE (IDLIST) values(EXPRLIST)
 *		insert into TABLE (IDLIST) select
 *		insert into TABLE (IDLIST) values(EXPRLIST) RETURNING columns
 *		insert into TABLE (IDLIST) select RETURNING columns
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
 *	The 'returning' parameter holds the column list for the RETURNING clause,
 *	or NULL if no RETURNING clause is present.
 *
 *	The code generated follows one of three templates.  For a simple
 *	select with data coming from a VALUES clause, the code executes
 *	once straight down through.  The template looks like this:
 *
 *		open write cursor to <table> and its indices
 *		allocate registers for rowid and data
 *		evaluate VALUES expressions into registers
 *		write the resulting record into <table>
 *		if RETURNING: output the inserted values
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
 *		   if RETURNING: output the inserted values
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
 *		     if RETURNING: output the inserted values
 *		   end the loop
 *		   cleanup
 *
 * PUBLIC: void __insert __P((parser_t *, src_list_t *, expr_list_t *,
 * PUBLIC:      select_t *, id_list_t *, int, expr_list_t *));
 *
 * parser			Parser context
 * tlist			Name of table into which we are inserting
 * vlist			List of values to be inserted
 * select			A SELECT statement to use as the data source
 * column			Column names corresponding to IDLIST
 * on_error			How to handle constraint errors
 * returning		Column list for RETURNING clause (or NULL)
 */
void __insert(parser, tlist, vlist, select, column, on_error, returning)
	parser_t *parser;
	src_list_t *tlist;
	expr_list_t *vlist;
	select_t *select;
	id_list_t *column;
	int on_error;
	expr_list_t *returning;
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

	/* Register allocations for register-based VDBE */
	int regRowid;        /* Register holding the rowid */
	int regData;         /* First register in array of data */
	int regRec;          /* Register holding assembled record */
	int *aRegIdx = NULL; /* Registers for each index */

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
	before_triggers_p = __triggers_exist(parser, table->first_trigger,
					     TK_INSERT, TK_BEFORE, TK_ROW, 0);
	after_triggers_p = __triggers_exist(parser, table->first_trigger,
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
			regRec = parser->nMem++;
			__vdbe_add_op3(v, OP_MakeRecord, 0, ncol, regRec);
			__vdbe_add_op(v, OP_NewRecno, src_tab, 0);
			__vdbe_add_op3(v, OP_Insert, src_tab, regRec, 0);
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
					  table->columns[j].zName) == 0) {
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
		__vdbe_add_op(v, OP_Integer, 0, cnt_mem);
	}

	/*
	 * REGISTER ALLOCATION BLOCK (new for register-based VDBE)
	 * Allocate registers for rowid and data columns.
	 */
	regRowid = parser->nMem++;
	regData = parser->nMem;
	parser->nMem += table->nCol;
	regRec = parser->nMem++;

	/* Allocate registers for index keys */
	if (table->first_index) {
		int nIdx = 0;
		for(idx = table->first_index; idx; idx = idx->next_index) nIdx++;
		if (__dbsql_calloc(dbp, nIdx, sizeof(int), &aRegIdx) == ENOMEM)
			goto insert_cleanup;
		for(i = 0, idx = table->first_index; idx; idx = idx->next_index, i++) {
			aRegIdx[i] = parser->nMem++;
		}
	}

	/*
	 * Open tables and indices if there are no row triggers.
	 */
	if (!row_triggers_p) {
		base = parser->nTab;
		__vdbe_add_op(v, OP_Integer, table->iDb, 0);
		__vdbe_add_op(v, OP_OpenWrite, base, table->tnum);
		__vdbe_change_p4(v, -1, table->zName, P3_STATIC);
		for (dx = 1, idx = table->first_index; idx; idx = idx->next_index, dx++){
			__vdbe_add_op(v, OP_Integer, idx->iDb, 0);
			__vdbe_add_op(v, OP_OpenWrite, (dx + base), idx->tnum);
			__vdbe_change_p4(v, -1, idx->zName, P3_STATIC);
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
			__vdbe_add_op(v, OP_Integer, -1, regRowid);
		} else if (use_temp_table) {
			__vdbe_add_op(v, OP_Column, src_tab, key_col);
			__vdbe_add_op(v, OP_Copy, 0, regRowid);
		} else if (select) {
			/* Use value from SELECT result */
			__vdbe_add_op(v, OP_Copy, regData + key_col, regRowid);
		} else {
			__expr_code(parser, vlist->a[key_col].pExpr);
			__vdbe_add_op(v, OP_NotNull, -1,
				      (__vdbe_current_addr(v) + 3));
			__vdbe_add_op(v, OP_Pop, 1, 0);
			__vdbe_add_op(v, OP_Integer, -1, regRowid);
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
				/* Use DEFAULT value */
				__vdbe_add_op(v, OP_String, 0, regData + i);
				__vdbe_change_p4(v, -1, table->columns[i].zDflt,
						 P3_STATIC);
			} else if (use_temp_table) {
				__vdbe_add_op(v, OP_Column, src_tab, j);
				__vdbe_add_op(v, OP_Copy, 0, regData + i);
			} else if (select) {
				/* Already in registers from SELECT */
				__vdbe_add_op(v, OP_Copy, regData + j, regData + i);
			} else {
				__expr_code(parser, vlist->a[j].pExpr);
				__vdbe_add_op(v, OP_Copy, 0, regData + i);
			}
		}
		__vdbe_add_op3(v, OP_MakeRecord, regRowid, table->nCol + 1, regRec);
		__vdbe_add_op3(v, OP_Insert, nidx, regRec, regRowid);

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
		__vdbe_change_p4(v, -1, table->zName, P3_STATIC);
		for (dx = 1, idx = table->first_index; idx; idx = idx->next_index, dx++){
			__vdbe_add_op(v, OP_Integer, idx->iDb, 0);
			__vdbe_add_op(v, OP_OpenWrite, (dx + base), idx->tnum);
			__vdbe_change_p4(v, -1, idx->zName, P3_STATIC);
		}
		parser->nTab += dx;
	}

	/*
	 * REGISTER-BASED INSERT GENERATION
	 * Generate the rowid and assemble data into registers, then insert.
	 */
	if (!view_p) {
		/*
		 * Generate the rowid for the new entry.
		 */
		if (key_col >= 0) {
			if (use_temp_table) {
				__vdbe_add_op(v, OP_Column, src_tab, key_col);
				__vdbe_add_op(v, OP_Copy, 0, regRowid);
			} else if (select) {
				__vdbe_add_op(v, OP_Copy, regData + key_col, regRowid);
			} else {
				__expr_code(parser, vlist->a[key_col].pExpr);
				__vdbe_add_op(v, OP_Copy, 0, regRowid);
			}
			/*
			 * If the PRIMARY KEY expression is NULL, then use
			 * OP_NewRecno to generate a unique primary key value.
			 */
			__vdbe_add_op(v, OP_NotNull, regRowid,
				      (__vdbe_current_addr(v) + 2));
			__vdbe_add_op(v, OP_NewRecno, base, regRowid);
			__vdbe_add_op(v, OP_MustBeInt, regRowid, 0);
		} else {
			__vdbe_add_op(v, OP_NewRecno, base, regRowid);
		}

		/*
		 * Load data values into registers.
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
				__vdbe_add_op(v, OP_Null, 0, regData + i);
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
				/* Use DEFAULT value */
				__vdbe_add_op(v, OP_String, 0, regData + i);
				__vdbe_change_p4(v, -1, table->columns[i].zDflt,
						 P3_STATIC);
			} else if (use_temp_table) {
				__vdbe_add_op(v, OP_Column, src_tab, j);
				__vdbe_add_op(v, OP_Copy, 0, regData + i);
			} else if (select) {
				/* Already in registers from SELECT */
				if (i != j) {
					__vdbe_add_op(v, OP_Copy, regData + j, regData + i);
				}
			} else {
				__expr_code(parser, vlist->a[j].pExpr);
				__vdbe_add_op(v, OP_Copy, 0, regData + i);
			}
		}

		/*
		 * Generate OP_MakeRecord to pack rowid and all data columns
		 * into a single database record.
		 * P1 = first register (rowid), P2 = count (nCol+1), P3 = dest register
		 */
		__vdbe_add_op3(v, OP_MakeRecord, regRowid, table->nCol + 1, regRec);

		/*
		 * Generate OP_Insert to write the record into the table.
		 * P1 = cursor number, P2 = data register, P3 = key register
		 */
		__vdbe_add_op3(v, OP_Insert, base, regRec, regRowid);

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
	 * If RETURNING clause is present, generate code to output the inserted values.
	 */
	if (returning && !view_p) {
		int i, j;
		int regOut = parser->nMem + 1;
		parser->nMem += returning->nExpr;

		/* Generate column names for RETURNING output */
		for (i = 0; i < returning->nExpr; i++) {
			const char *col_name;
			if (returning->a[i].zEName && returning->a[i].zEName[0]) {
				col_name = returning->a[i].zEName;
			} else if (returning->a[i].pExpr->op == TK_ID) {
				col_name = returning->a[i].pExpr->u.zToken.z;
			} else {
				col_name = "column";
			}
			__vdbe_add_op(v, OP_ColumnName, i, 0);
			__vdbe_change_p4(v, -1, col_name, P3_STATIC);
		}

		/* Evaluate each RETURNING expression and output */
		for (i = 0; i < returning->nExpr; i++) {
			expr_t *expr = returning->a[i].pExpr;

			if (expr->op == TK_ALL) {
				/* RETURNING * - output all columns */
				__vdbe_add_op(v, OP_Copy, regRowid, regOut + i);
				for (j = 0; j < table->nCol; j++) {
					__vdbe_add_op(v, OP_Copy, regData + j, regOut + i + j + 1);
				}
				i += table->nCol;
			} else if (expr->op == TK_ID) {
				/* Column reference - find column index */
				const char *col_name = expr->u.zToken.z;
				if (__is_row_id(col_name)) {
					__vdbe_add_op(v, OP_Copy, regRowid, regOut + i);
				} else {
					for (j = 0; j < table->nCol; j++) {
						if (strcasecmp(table->columns[j].zName, col_name) == 0) {
							__vdbe_add_op(v, OP_Copy, regData + j, regOut + i);
							break;
						}
					}
				}
			} else {
				/* Expression - evaluate it */
				__expr_code(parser, expr);
				__vdbe_add_op(v, OP_Copy, 0, regOut + i);
			}
		}

		/* Output the row */
		__vdbe_add_op(v, OP_Callback, returning->nExpr, 0);
	}

	/*
	 * Update the count of rows that are inserted.
	 */
	if ((dbp->flags & DBSQL_CountRows) != 0) {
		__vdbe_add_op(v, OP_AddImm, cnt_mem, 1);
	}

	if (row_triggers_p) {
		/* Close all tables opened */
		if (!view_p) {
			__vdbe_add_op(v, OP_Close, base, 0);
			for(dx = 1, idx = table->first_index; idx;
			    idx = idx->next_index, dx++) {
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
		__vdbe_add_op(v, OP_Return, 0, 0);
		__vdbe_resolve_label(v, cleanup);
	}

	if (!row_triggers_p) {
		/* Close all tables opened */
		__vdbe_add_op(v, OP_Close, base, 0);
		for(dx = 1, idx = table->first_index; idx; idx = idx->next_index, dx++){
			__vdbe_add_op(v, OP_Close, (dx + base), 0);
		}
	}

	__vdbe_conclude_write(parser);

	/*
	 * Return the number of rows inserted.
	 */
	if (dbp->flags & DBSQL_CountRows) {
		__vdbe_add_op(v, OP_ColumnName, 0, 0);
		__vdbe_change_p4(v, -1, "rows inserted", P3_STATIC);
		__vdbe_add_op(v, OP_Copy, cnt_mem, 0);
		__vdbe_add_op(v, OP_Callback, 1, 0);
	}

  insert_cleanup:
	if (aRegIdx) __dbsql_free(dbp, aRegIdx);
	__src_list_delete(tlist);
	if (vlist )
		__expr_list_delete(vlist);
	if (select)
		__select_delete(select);
	__id_list_delete(column);
	if (returning)
		__expr_list_delete(returning);
}

/* NOTE: __generate_constraint_checks and __complete_insertion functions
 * will need similar register-based updates. These are shared with UPDATE
 * so they should be converted carefully to maintain compatibility. */
