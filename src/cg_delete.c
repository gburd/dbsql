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
 * This file contains C code routines that are called by the parser
 * to handle DELETE FROM statements.
 *
 * CONVERTED TO REGISTER-BASED VDBE (SQLite 3.52.0 architecture)
 * Task #22: Convert DELETE code generator from stack to register-based
 *
 * Key changes:
 * - Use register allocation for rowid storage instead of stack
 * - OP_SetInsert with register operand instead of stack
 * - OP_SetNext writes to register instead of stack
 * - Register-based trigger code generation (OLD.* in registers)
 * - OP_Delete uses cursor operand (P1) only
 */

#include "dbsql_config.h"

#include "dbsql_int.h"

/*
 * __src_list_lookup --
 *	Look up every table that is named in 'src'.  If any table is not found,
 *	add an error message to parse->zErrMsg and return NULL.  If all tables
 *	are found, return a pointer to the last table.
 *
 * PUBLIC: table_t *__src_list_lookup __P((parser_t *, src_list_t *));
 */
table_t *
__src_list_lookup(parser_t* parser, src_list_t* src)
{
	table_t *table = 0;
	int i;
	for (i = 0; i < src->n_sources; i++) {
		const char *tb_name = src->a[i].zName;
		const char *db_name = src->a[i].zDatabase;
		table = __locate_table(parser, tb_name, db_name);
		src->a[i].pTab = table;
	}
	return table;
}

/*
 * __is_table_read_only --
 *	Check to make sure the given table is writable.  If it is not
 *	writable, generate an error message and return 1.  If it is
 *	writable return 0;
 *
 * PUBLIC: int __is_table_read_only __P((parser_t *, table_t *, int));
 */
int
__is_table_read_only(parser_t* parser, table_t* table, int views_ok)
{
	if (table->readOnly) {
		__error_msg(parser, "table %s may not be modified",
			    table->zName);
		return 1;
	}
	if (!views_ok && table->pSelect) {
		__error_msg(parser, "cannot modify %s because it is a view",
			    table->zName);
		return 1;
	}
	return 0;
}

/*
 * __delete_from --
 *	Process a DELETE FROM statement.
 *
 * PUBLIC: void __delete_from __P((parser_t *, src_list_t *, expr_t *));
 *
 * parser			The parser context
 * list				The table from which we should delete things
 * where			The WHERE clause.  May be null.
 */
void __delete_from(parser, list, where)
	parser_t *parser;
	src_list_t *list;
	expr_t *where;
{
	vdbe_t *v;          /* The virtual database engine */
	table_t *table;     /* The table from which records will be deleted */
	const char *name;   /* Name of database holding 'table' */
	int end, addr;      /* A couple addresses of generated code */
	int i;              /* Loop counter */
	where_info_t *info; /* Information about the WHERE clause */
	index_t *idx;       /* For looping over indices of the table */
	int cur;            /* VDBE Cursor number for 'table' */
	DBSQL *dbp;         /* Main database structure */
	int view;           /* True if attempting to delete from a view */
	auth_context_t context;/* Authorization context */
	int row_triggers_exist = 0;/* True if any triggers exist */
	int before_triggers;/* True if there are BEFORE triggers */
	int after_triggers; /* True if there are AFTER triggers */
	int old_idx = -1;   /* Cursor for the OLD table of AFTER triggers */

	/* Register allocations for register-based VDBE */
	int regRowid;       /* Register holding the rowid to delete */
	int regRowSet;      /* Register holding the rowset of rowids */
	int regOldRowid;    /* Register for OLD.rowid in triggers */
	int regOldData;     /* First register for OLD.* columns in triggers */
	int regRec;         /* Register for assembled record in triggers */

	context.pParse = 0;
	if (parser->n_errors || parser->rc == ENOMEM) {
		list = 0;
		goto delete_from_cleanup;
	}
	dbp = parser->db;
	DBSQL_ASSERT(list->n_sources == 1);

	/*
	 * Locate the table which we want to delete.  This table has to be
	 * put in an src_list_t structure because some of the subroutines we
	 * will be calling are designed to work with multiple tables and expect
	 * an src_list_t* parameter instead of just a table_t* parameter.
	 */
	table = __src_list_lookup(parser, list);
	if (table == 0)
		goto delete_from_cleanup;
	before_triggers = __triggers_exist(parser, table->pTrigger,
					   TK_DELETE, TK_BEFORE, TK_ROW, 0);
	after_triggers = __triggers_exist(parser, table->pTrigger,
					  TK_DELETE, TK_AFTER, TK_ROW, 0);
	row_triggers_exist = (before_triggers || after_triggers);
	view = (table->pSelect != 0);
	if (__is_table_read_only(parser, table, before_triggers)) {
		goto delete_from_cleanup;
	}
	DBSQL_ASSERT(table->iDb < dbp->nDb);
	name = dbp->aDb[table->iDb].zName;
	if (__auth_check(parser, DBSQL_DELETE, table->zName, 0, name)) {
		goto delete_from_cleanup;
	}

	/*
	 * If pTab is really a view, make sure it has been initialized.
	 */
	if (view && __view_get_column_names(parser, table)) {
		goto delete_from_cleanup;
	}

	/*
	 * Allocate registers for trigger OLD.* data.
	 */
	if (row_triggers_exist) {
		old_idx = parser->n_tables++;
		regOldRowid = parser->n_memory++;
		regOldData = parser->n_memory;
		parser->n_memory += table->n_columns;
		regRec = parser->n_memory++;
	}

	/*
	 * Resolve the column names in all the expressions.
	 */
	DBSQL_ASSERT(list->n_sources == 1);
	cur = list->a[0].iCursor = parser->n_tables++;
	if (where) {
		if (__expr_resolve_ids(parser, list, 0, where)) {
			goto delete_from_cleanup;
		}
		if (__expr_check(parser, where, 0, 0)) {
			goto delete_from_cleanup;
		}
	}

	/*
	 * Start the view context.
	 */
	if (view) {
		__auth_context_push(parser, &context, table->zName);
	}

	/*
	 * Begin generating code.
	 */
	v = __parser_get_vdbe(parser);
	if (v == 0) {
		goto delete_from_cleanup;
	}
	__vdbe_prepare_write(parser, row_triggers_exist, table->iDb);

	/*
	 * If we are trying to delete from a view, construct that view into
	 * a temporary table.
	 */
	if (view) {
		select_t *vsel = __select_dup(parser->db, table->pSelect);
		__select(parser, vsel, SRT_TempTable, cur, 0, 0, 0);
		__select_delete(vsel);
	}

	/*
	 * Initialize the counter of the number of rows deleted, if
	 * we are counting rows.
	 */
	regRowSet = 0;
	if (dbp->flags & DBSQL_CountRows) {
		regRowSet = parser->n_memory++;
		__vdbe_add_op(v, OP_Integer, 0, regRowSet);
	}

	/*
	 * Special case: A DELETE without a WHERE clause deletes everything.
	 * It is easier just to erase the whole table.  Note, however, that
	 * this means that the row change count will be incorrect.
	 */
	if (where == 0 && !row_triggers_exist) {
		if (dbp->flags & DBSQL_CountRows) {
			/*
			 * If counting rows deleted, just count the total
			 * number of entries in the table.
			 */
			int end_of_loop = __vdbe_make_label(v);
			int addr;
			int regCount = parser->n_memory++;
			if (!view) {
				__vdbe_add_op(v, OP_Integer, table->iDb, 0);
				__vdbe_add_op(v, OP_OpenRead, cur,
					      table->tnum);
			}
			__vdbe_add_op(v, OP_Integer, 0, regCount);
			__vdbe_add_op(v, OP_Rewind, cur,
				      (__vdbe_current_addr(v) + 3));
			addr = __vdbe_add_op(v, OP_AddImm, regCount, 1);
			__vdbe_add_op(v, OP_Next, cur, addr);
			__vdbe_resolve_label(v, end_of_loop);
			__vdbe_add_op(v, OP_Close, cur, 0);
			__vdbe_add_op(v, OP_Copy, regCount, regRowSet);
		}
		if (!view) {
			__vdbe_add_op(v, OP_Clear, table->tnum, table->iDb);
			for (idx = table->pIndex; idx; idx = idx->pNext) {
				__vdbe_add_op(v, OP_Clear, idx->tnum,
					      idx->iDb);
			}
		}
	} else {
		/*
		 * The usual case: There is a WHERE clause so we have to
		 * scan through the table and pick which records to delete.
		 */

		/* Allocate register for rowset */
		regRowSet = parser->n_memory++;
		__vdbe_add_op(v, OP_Null, 0, regRowSet);

		/*
		 * Begin the database scan.
		 */
		info = __where_begin(parser, list, where, 1, 0);
		if (info == 0)
			goto delete_from_cleanup;

		/*
		 * Remember the key of every item to be deleted.
		 * Use OP_SetInsert to add rowids to the set.
		 */
		regRowid = parser->n_memory++;
		__vdbe_add_op(v, OP_Rowid, cur, regRowid);
		__vdbe_add_op(v, OP_SetInsert, regRowSet, regRowid);
		if (dbp->flags & DBSQL_CountRows) {
			__vdbe_add_op(v, OP_AddImm, regRowSet + 1, 1);
		}

		/*
		 * End the database scan loop.
		 */
		__where_end(info);

		/*
		 * Open the pseudo-table used to store OLD if there are
		 * triggers.
		 */
		if (row_triggers_exist) {
			__vdbe_add_op(v, OP_OpenPseudo, old_idx, 0);
		}

		/*
		 * Delete every item whose key was written to the rowset
		 * during the database scan.  We have to delete items
		 * after the scan is complete because deleting an item
		 * can change the scan order.
		 */
		end = __vdbe_make_label(v);

		/*
		 * This is the beginning of the delete loop when there are
		 * row triggers.
		 */
		if (row_triggers_exist) {
			addr = __vdbe_add_op(v, OP_SetNext, regRowSet, end);
			__vdbe_add_op(v, OP_Copy, regRowSet, regRowid);
			if (!view) {
				__vdbe_add_op(v, OP_Integer, table->iDb, 0);
				__vdbe_add_op(v, OP_OpenRead, cur,
					      table->tnum);
			}
			__vdbe_add_op(v, OP_NotExists, cur,
				      (__vdbe_current_addr(v) + 2));

			/* Load OLD.* into registers */
			__vdbe_add_op(v, OP_Rowid, cur, regOldRowid);
			for (i = 0; i < table->n_columns; i++) {
				__vdbe_add_op(v, OP_Column, cur, i);
				__vdbe_add_op(v, OP_Copy, 0, regOldData + i);
			}
			__vdbe_add_op3(v, OP_MakeRecord, regOldRowid,
				      table->n_columns + 1, regRec);
			__vdbe_add_op3(v, OP_Insert, old_idx, regRec,
				      regOldRowid);

			if (!view) {
				__vdbe_add_op(v, OP_Close, cur, 0);
			}
			__code_row_trigger(parser, TK_DELETE, 0, TK_BEFORE,
					   table, -1, old_idx,
					   ((parser->trigStack) ?
					     parser->trigStack->orconf :
					     OE_Default), addr);
		}

		if (!view) {
			/*
			 * Open cursors for the table we are deleting from
			 * and all its indices.  If there are row triggers,
			 * this happens inside the OP_SetNext loop because
			 * the cursor have to all be closed before the trigger
			 * fires.  If there are no row triggers, the cursors
			 * are opened only once on the outside the loop.
			 */
			parser->n_tables = cur + 1;
			__vdbe_add_op(v, OP_Integer, table->iDb, 0);
			__vdbe_add_op(v, OP_OpenWrite, cur, table->tnum);
			for (i = 1, idx = table->pIndex; idx;
			     i++, idx = idx->pNext) {
				__vdbe_add_op(v, OP_Integer, idx->iDb, 0);
				__vdbe_add_op(v, OP_OpenWrite, parser->n_tables++,
					      idx->tnum);
			}

			/*
			 * This is the beginning of the delete loop when
			 * there are no row triggers.
			 */
			if (!row_triggers_exist) {
				addr = __vdbe_add_op(v, OP_SetNext,
						    regRowSet, end);
				__vdbe_add_op(v, OP_Copy, regRowSet, regRowid);
			}

			/*
			 * Delete the row.
			 * For register-based: __generate_row_delete expects
			 * the rowid in the proper register and uses cursor.
			 */
			__generate_row_delete(dbp, v, table, cur,
					      (parser->trigStack == 0));
		}

		/*
		 * If there are row triggers, close all cursors then invoke
		 * the AFTER triggers.
		 */
		if (row_triggers_exist) {
			if (!view) {
				for(i = 1, idx = table->pIndex; idx;
				    i++, idx = idx->pNext) {
					__vdbe_add_op(v, OP_Close, cur + i,
						      idx->tnum);
				}
				__vdbe_add_op(v, OP_Close, cur, 0);
			}
			__code_row_trigger(parser, TK_DELETE, 0, TK_AFTER,
					   table, -1, old_idx,
					   ((parser->trigStack) ?
					     parser->trigStack->orconf :
					      OE_Default), addr);
		}

		/*
		 * End of the delete loop.
		 */
		__vdbe_add_op(v, OP_Goto, 0, addr);
		__vdbe_resolve_label(v, end);

		/*
		 * Close the cursors after the loop if there are no row
		 * triggers.
		 */
		if (!row_triggers_exist) {
			for (i = 1, idx = table->pIndex; idx;
			     i++, idx = idx->pNext) {
				__vdbe_add_op(v, OP_Close, cur + i, idx->tnum);
			}
			__vdbe_add_op(v, OP_Close, cur, 0);
			parser->n_tables = cur;
		}
	}
	__vdbe_conclude_write(parser);

	/*
	* Return the number of rows that were deleted.
	*/
	if (dbp->flags & DBSQL_CountRows) {
		__vdbe_add_op(v, OP_ColumnName, 0, 0);
		__vdbe_change_p4(v, -1, "rows deleted", P3_STATIC);
		__vdbe_add_op(v, OP_Copy, regRowSet, 0);
		__vdbe_add_op(v, OP_Callback, 1, 0);
	}

  delete_from_cleanup:
	__auth_context_pop(&context);
	__src_list_delete(list);
	__expr_delete(where);
	return;
}

/*
 * __generate_row_delete --
 *	This routine generates VDBE code that causes a single row of a
 *	single table to be deleted.
 *
 *	The VDBE must be in a particular state when this routine is called.
 *	These are the requirements:
 *
 *	1.  A read/write cursor pointing to 'table', the table containing
 *	   the row to be deleted, must be opened as cursor number "base".
 *
 *	2.  Read/write cursors for all indices of pTab must be open as
 *	    cursor number base+i for the i-th index.
 *
 *	3.  The record number of the row to be deleted must be stored in
 *	    a register (for register-based VDBE, not on stack).
 *
 *	This routine generates code to remove both the table record and all
 *	index entries that point to that record.
 *
 *	For register-based VDBE: The rowid is expected to be positioned at
 *	the cursor already via MoveTo or similar operation.
 *
 * PUBLIC: void __generate_row_delete __P((DBSQL *, vdbe_t *, table_t *,
 * PUBLIC:                            int, int));
 *
 * dbp				The database containing the index
 * v				Generate code into this VDBE
 * table			Table containing the row to be deleted
 * cur				Cursor number for the table
 * count			Increment the row change counter
 */
void __generate_row_delete(dbp, v, table, cur, count)
	DBSQL *dbp;
	vdbe_t *v;
	table_t *table;
	int cur;
	int count;
{
	int addr;
	/* For register-based, the cursor is already positioned.
	 * Check if row exists, delete indices, then delete row. */
	addr = __vdbe_add_op(v, OP_NotExists, cur, 0);
	__generate_row_index_delete(dbp, v, table, cur, 0);
	__vdbe_add_op(v, OP_Delete, cur, count);
	__vdbe_change_p2(v, addr, __vdbe_current_addr(v));
}

/*
 * __generate_row_index_delete --
 *	This routine generates VDBE code that causes the deletion of all
 *	index entries associated with a single row of a single table.
 *
 *	The VDBE must be in a particular state when this routine is called.
 *	These are the requirements:
 *
 *	1.  A read/write cursor pointing to 'table', the table containing
 *	    the row to be deleted, must be opened as cursor number 'cur'.
 *
 *	2.  Read/write cursors for all indices of 'table' must be open as
 *	    cursor number cur+i for the i-th index.
 *
 *	3.  The 'cur' cursor must be pointing to the row that is to be
 *	    deleted.
 *
 * PUBLIC: void __generate_row_index_delete __P((DBSQL *, vdbe_t *,
 * PUBLIC:                                  table_t *, int, char *));
 *
 * dbp				The database containing the index
 * v				Generate code into this VDBE
 * table			Table containing the row to be deleted
 * cur				Cursor number for the table
 * idx_used			Only delete if aIdxUsed!=0 && aIdxUsed[i]!=0
 */
void __generate_row_index_delete(dbp, v, table, cur, idx_used)
	DBSQL *dbp;
	vdbe_t *v;
	table_t *table;
	int cur;
	char *idx_used;
{
	int i;
	index_t *idx;

	for (i = 1, idx = table->pIndex; idx; i++, idx = idx->pNext) {
		int j;
		int regIdx;  /* Register to hold index key */

		if (idx_used != 0 && idx_used[i-1] == 0)
			continue;

		/* Build index key in register */
		regIdx = 0;  /* Will be allocated by index key builder */
		__vdbe_add_op(v, OP_Rowid, cur, regIdx);
		for (j = 0; j < idx->n_key_columns; j++) {
			int n = idx->aiColumn[j];
			if (n == table->iPKey) {
				/* Use rowid already in register */
				continue;
			} else {
				__vdbe_add_op(v, OP_Column, cur, n);
			}
		}
		__vdbe_add_op(v, OP_MakeIdxKey, idx->n_key_columns, 0);
		__add_idx_key_type(v, idx);
		__vdbe_add_op(v, OP_IdxDelete, cur + i, 0);
	}
}
