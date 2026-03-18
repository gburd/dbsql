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
 * This file contains routines that handle UPDATE statements.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __update --
 *	Process an UPDATE statement.
 *
 *   UPDATE OR IGNORE table_wxyz SET a=b, c=d WHERE e<5 AND f NOT NULL;
 *          \_______/ \________/     \______/       \________________/
 *           on_error   tab_list      changes              where
 *
 * PUBLIC: void __update __P((parser_t *, src_list_t *, expr_list_t *,
 * PUBLIC:               expr_t *, int));
 *
 * parser			The parser context
 * tab_list			The table in which we should change things
 * changes			Things to be changed
 * where			The WHERE clause.  May be null
 * on_error			How to handle constraint errors
 */
void __update(parser, tab_list, changes, where, on_error)
	parser_t *parser;
	src_list_t *tab_list;
	expr_list_t *changes;
	expr_t *where;
	int on_error;
{
	int rc;
	int i, j;             /* Loop counters */
	table_t *table;       /* The table to be updated */
	int addr;             /* VDBE instruction address of the start of
				 the loop */
	where_info_t *winfo;  /* Information about the WHERE clause */
	vdbe_t *v;            /* The virtual database engine */
	index_t *idx;         /* For looping over indices */
	int num_idx;          /* Number of indices that need updating */
	int total_num_idx;    /* Total number of indices */
	int cur;              /* VDBE Cursor number of table */
	int data_cur;         /* Main table cursor */
	int idx_cur;          /* First index cursor */
	DBSQL *dbp;           /* The database structure */
	index_t **indices = 0;/* An array of indices that need updating too */
	char *idx_used = 0;   /* idx_used[i]==1 if the i-th index is used */
	int *xref = 0;        /* xref[i] is the index in changes->a[] of the
				 an expression for the i-th column of the
				 table. xref[i]==-1 if the i-th column is not
				 changed. */
	int *areg_idx = 0;    /* Register allocation for each index */
	int recno_will_change_p;/* True if the record number is being changed*/
	expr_t *recno;        /* Expression defining the new record number */
	int open_all_p;       /* True if all indices need to be opened */
	int is_view_p;        /* Trying to update a view */
	auth_context_t auth;  /* The authorization context */

	int before_triggers;  /* True if there are any BEFORE triggers */
	int after_triggers;   /* True if there are any AFTER triggers */
	int row_triggers_exist = 0;/* True if any row triggers exist */

	/* Register allocations for register-based VDBE */
	int reg_row_count = 0;   /* Row counter register */
	int reg_old_rowid = 0;   /* Old rowid register */
	int reg_new_rowid = 0;   /* New rowid register */
	int reg_old = 0;         /* First register for old column values */
	int reg_new = 0;         /* First register for new column values */
	int reg_rowset = 0;      /* RowSet for multi-pass updates */
	int new_idx = 0;         /* Cursor for NEW pseudo-table */
	int old_idx = 0;         /* Cursor for OLD pseudo-table */

	select_t *view;

	auth.pParse = 0;
	if (parser->nErr || parser->rc == ENOMEM)
		goto update_cleanup;
	dbp = parser->db;
	DBSQL_ASSERT(tab_list->nSrc == 1);

	/*
	 * Locate the table which we want to update. 
	 */
	table = __src_list_lookup(parser, tab_list);
	if (table == 0)
		goto update_cleanup;
	before_triggers = __triggers_exist(parser, table->pTrigger,
					   TK_UPDATE, TK_BEFORE, TK_ROW,
					   changes);
	after_triggers = __triggers_exist(parser, table->pTrigger, 
					  TK_UPDATE, TK_AFTER, TK_ROW,
					  changes);
	row_triggers_exist = before_triggers || after_triggers;
	is_view_p = (table->u.view.pSelect != 0);
	if (__is_table_read_only(parser, table, before_triggers)) {
		goto update_cleanup;
	}
	if (is_view_p) {
		if (__view_get_column_names(parser, table)) {
			goto update_cleanup;
		}
	}
	if (__dbsql_calloc(dbp, table->nCol, sizeof(int), &xref) == ENOMEM)
		goto update_cleanup;
	for (i = 0; i < table->nCol; i++) {
		xref[i] = -1;
	}

	/*
	 * If there are FOR EACH ROW triggers, allocate cursors for the
	 * special OLD and NEW tables.
	 */
	if (row_triggers_exist) {
		new_idx = parser->nTab++;
		old_idx = parser->nTab++;
	}

	/*
	 * Allocate cursors for the main database table and for all indices.
	 * The index cursors might not be used, but if they are used they
	 * need to occur right after the database cursor. So go ahead and
	 * allocate enough space, just in case.
	 */
	cur = parser->nTab++;
	data_cur = cur;
	idx_cur = parser->nTab;
	tab_list->a[0].iCursor = data_cur;
	for (idx = table->pIndex; idx; idx = idx->next_vdbe) {
		parser->nTab++;
	}

	/*
	 * Resolve the column names in all the expressions of the
	 * of the UPDATE statement.  Also find the column index
	 * for each column to be updated in the changes array.  For each
	 * column to be updated, make sure we have authorization to change
	 * that column.
	 */
	recno_will_change_p = 0;
	for (i = 0; i < changes->nExpr; i++) {
		if (__expr_resolve_ids(parser, tab_list, 0,
				       changes->a[i].pExpr)) {
			goto update_cleanup;
		}
		if (__expr_check(parser, changes->a[i].pExpr, 0, 0)) {
			goto update_cleanup;
		}
		for (j = 0; j < table->nCol; j++) {
			if (strcasecmp(table->aCol[j].zName,
						  changes->a[i].zEName) == 0) {
				if (j == table->iPKey) {
					recno_will_change_p = 1;
					recno = changes->a[i].pExpr;
				}
				xref[j] = i;
				break;
			}
		}
		if (j >= table->nCol) {
			if (__is_row_id(changes->a[i].zEName)) {
				recno_will_change_p = 1;
				recno = changes->a[i].pExpr;
			} else {
				__error_msg(parser, "no such column: %s",
					    changes->a[i].zEName);
				goto update_cleanup;
			}
		}
#ifndef DBSQL_NO_AUTH
		rc = __auth_check(parser, DBSQL_UPDATE, table->zName,
				  table->aCol[j].zName,
				  dbp->aDb[table->iDb].zName);
		if (rc == DBSQL_DENY) {
			goto update_cleanup;
		} else if (rc == DBSQL_IGNORE) {
			xref[j] = -1;
		}
#endif
	}

	/*
	 * Allocate memory for the array indices[] and fill it with pointers
	 * to every index that needs to be updated. Indices only need
	 * updating if their key includes one of the columns named in changes
	 * or if the record number of the original table entry is changing.
	 *
	 * Also allocate register space for each index that needs updating.
	 */
	for (num_idx = total_num_idx = 0, idx = table->pIndex;
	     idx; idx = idx->next_vdbe, total_num_idx++) {
		if (recno_will_change_p) {
			i = 0;
		} else {
			for (i = 0; i < idx->nColumn; i++) {
				if (xref[idx->aiColumn[i]] >= 0)
					break;
			}
		}
		if (i < idx->nColumn)
			num_idx++;
	}
	if (total_num_idx > 0) {
		/* Allocate space for indices array, idx_used flags, and register array */
		if (__dbsql_calloc(dbp, num_idx + total_num_idx +
				   (total_num_idx + 1) * sizeof(int),
				sizeof(index_t *), &indices) == ENOMEM)
			goto update_cleanup;
		idx_used = (char*)&indices[num_idx];
		areg_idx = (int*)&idx_used[total_num_idx];
	}
	for (num_idx = j = 0, idx = table->pIndex; idx; idx = idx->next_vdbe, j++){
		if (recno_will_change_p) {
			i = 0;
		} else {
			for (i = 0; i < idx->nColumn; i++) {
				if (xref[idx->aiColumn[i]] >= 0)
					break;
			}
		}
		if (i < idx->nColumn) {
			indices[num_idx++] = idx;
			idx_used[j] = 1;
			/* Allocate registers for this index key */
			areg_idx[j] = parser->nMem + 1;
			parser->nMem += idx->nColumn + 1;
		} else {
			idx_used[j] = 0;
			areg_idx[j] = 0;
		}
	}
	/* Register for the main table record */
	areg_idx[total_num_idx] = parser->nMem + 1;
	parser->nMem++;

	/*
	 * Resolve the column names in all the expressions in the
	 * WHERE clause.
	 */
	if (where) {
		if (__expr_resolve_ids(parser, tab_list, 0, where)) {
			goto update_cleanup;
		}
		if (__expr_check(parser, where, 0, 0)) {
			goto update_cleanup;
		}
	}

	/*
	 * Start the view context.
	 */
	if (is_view_p) {
		__auth_context_push(parser, &auth, table->zName);
	}

	/*
	 * Begin generating code.
	 */
	v = __parser_get_vdbe(parser);
	if (v == 0)
		goto update_cleanup;
	__vdbe_prepare_write(parser, 1, table->iDb);

	/*
	 * Allocate registers for storing old and new values.
	 * For register-based VDBE, we need:
	 *   - reg_old_rowid and reg_new_rowid for rowid values
	 *   - reg_old for old column values (if triggers or foreign keys)
	 *   - reg_new for new column values
	 */
	reg_old_rowid = parser->nMem + 1;
	reg_new_rowid = reg_old_rowid;
	parser->nMem++;

	if (recno_will_change_p || row_triggers_exist) {
		reg_new_rowid = parser->nMem + 1;
		parser->nMem++;
	}

	if (row_triggers_exist) {
		reg_old = parser->nMem + 1;
		parser->nMem += table->nCol;
	}

	reg_new = parser->nMem + 1;
	parser->nMem += table->nCol;

	/*
	 * If we are trying to update a view, construct that view into
	 * a temporary table.
	 */
	if (is_view_p) {
		view = __select_dup(table->u.view.pSelect);
		__select(parser, view, SRT_TempTable, cur, 0, 0, 0);
		__select_delete(view);
	}

	/*
	 * Begin the database scan.
	 */
	winfo = __where_begin(parser, tab_list, where, 1, 0);
	if (winfo == 0)
		goto update_cleanup;

	/*
	 * Remember the index of every item to be updated.
	 */
	__vdbe_add_op(v, OP_ListWrite, 0, 0);

	/*
	 * End the database scan loop.
	 */
	__where_end(winfo);

	/*
	 * Initialize the count of updated rows.
	 */
	if (dbp->flags & DBSQL_CountRows && !parser->trigStack) {
		if (reg_row_count == 0) {
			reg_row_count = parser->nMem + 1;
			parser->nMem++;
		}
		__vdbe_add_op3(v, OP_Integer, 0, reg_row_count, 0);
	}

	if (row_triggers_exist) {
		/*
		 * For triggers, we need to read old values into registers.
		 * The top of the update loop for when there are triggers.
		 */
		__vdbe_add_op(v, OP_ListRewind, 0, 0);
		addr = __vdbe_add_op(v, OP_ListRead, 0, 0);

		/*
		 * Open a cursor and make it point to the record that is
		 * being updated.
		 */
		if(!is_view_p) {
			__vdbe_add_op3(v, OP_OpenRead, data_cur, table->tnum,
				       table->iDb);
		}
		__vdbe_add_op3(v, OP_NotExists, data_cur, addr, reg_old_rowid);

		/*
		 * Read the old rowid from the list into register.
		 */
		__vdbe_add_op3(v, OP_Rowid, data_cur, reg_old_rowid, 0);

		/*
		 * Read OLD column values into registers for triggers.
		 */
		for(i = 0; i < table->nCol; i++) {
			if (i == table->iPKey) {
				__vdbe_add_op3(v, OP_Integer, 0, reg_old + i, 0);
			} else {
				__vdbe_add_op3(v, OP_Column, data_cur, i, reg_old + i);
			}
		}

		/*
		 * Compute NEW values into registers.
		 */
		if (recno_will_change_p) {
			int expr_reg;
			__expr_code(parser, recno);
			/* Result is at top of register stack */
			expr_reg = parser->nMem;
			__vdbe_add_op3(v, OP_Dup, expr_reg, reg_new_rowid, 0);
		} else {
			__vdbe_add_op3(v, OP_Dup, reg_old_rowid, reg_new_rowid, 0);
		}

		for(i = 0; i < table->nCol; i++) {
			if (i == table->iPKey) {
				__vdbe_add_op3(v, OP_Integer, 0, reg_new + i, 0);
				continue;
			}
			j = xref[i];
			if (j < 0) {
				__vdbe_add_op3(v, OP_Dup, reg_old + i, reg_new + i, 0);
			}else{
				int expr_reg;
				__expr_code(parser, changes->a[j].pExpr);
				/* Result is at top of register stack */
				expr_reg = parser->nMem;
				__vdbe_add_op3(v, OP_Dup, expr_reg, reg_new + i, 0);
			}
		}

		if (!is_view_p) {
			__vdbe_add_op(v, OP_Close, data_cur, 0);
		}

		/*
		 * Fire the BEFORE and INSTEAD OF triggers.
		 * Pass reg_old and reg_new as the OLD and NEW pseudo-table base registers.
		 */
		if (__code_row_trigger(parser, TK_UPDATE, changes,
				       TK_BEFORE, table, reg_new, reg_old,
				       on_error, addr)) {
			goto update_cleanup;
		}
	}

	if (!is_view_p) {
		/*
		* Open every index that needs updating. Note that if any
		* index could potentially invoke a REPLACE conflict resolution
		* action, then we need to open all indices because we might
		* need to be deleting some records.
		*/
		__vdbe_add_op3(v, OP_OpenWrite, data_cur, table->tnum, table->iDb);
		if (on_error == OE_Replace) {
			open_all_p = 1;
		} else {
			open_all_p = 0;
			for (idx = table->pIndex; idx; idx = idx->next_vdbe) {
				if (idx->onError == OE_Replace) {
					open_all_p = 1;
					break;
				}
			}
		}
		for(i = 0, idx = table->pIndex; idx; idx = idx->next_vdbe, i++) {
			if (open_all_p || idx_used[i]) {
				__vdbe_add_op3(v, OP_OpenWrite, (idx_cur + i),
					       idx->tnum, idx->iDb);
				DBSQL_ASSERT(parser->nTab > (idx_cur + i));
			}
		}

		/*
		 * Loop over every record that needs updating. We have to load
		 * the old data for each record to be updated because some
		 * columns might not change and we will need to copy the old
		 * value. Also, the old data is needed to delete the old index
		 * entries. So make the cursor point at the old record.
		 */
		if (!row_triggers_exist) {
			__vdbe_add_op(v, OP_ListRewind, 0, 0);
			addr = __vdbe_add_op(v, OP_ListRead, 0, 0);
		}
		__vdbe_add_op3(v, OP_NotExists, data_cur, addr, reg_old_rowid);

		/*
		 * Read the old rowid.
		 */
		__vdbe_add_op3(v, OP_Rowid, data_cur, reg_old_rowid, 0);

		/*
		 * If the record number will change, compute the new rowid.
		 */
		if (recno_will_change_p) {
			int expr_reg;
			__expr_code(parser, recno);
			expr_reg = parser->nMem;
			__vdbe_add_op3(v, OP_Dup, expr_reg, reg_new_rowid, 0);
			__vdbe_add_op(v, OP_MustBeInt, reg_new_rowid, 0);
		} else {
			__vdbe_add_op3(v, OP_Dup, reg_old_rowid, reg_new_rowid, 0);
		}

		/*
		 * Compute new data for this record into registers.
		 */
		for (i = 0; i < table->nCol; i++) {
			if (i == table->iPKey) {
				__vdbe_add_op3(v, OP_Integer, 0, reg_new + i, 0);
				continue;
			}
			j = xref[i];
			if (j < 0) {
				__vdbe_add_op3(v, OP_Column, data_cur, i, reg_new + i);
			} else {
				int expr_reg;
				__expr_code(parser, changes->a[j].pExpr);
				expr_reg = parser->nMem;
				__vdbe_add_op3(v, OP_Dup, expr_reg, reg_new + i, 0);
			}
		}

		/*
		 * Do constraint checks.
		 * Updated signature: (parser, table, base_cursor, idx_used,
		 *                     recno_change, is_update, on_error, addr)
		 */
		__generate_constraint_checks(parser, table, data_cur, idx_used,
					     recno_will_change_p, 1, on_error,
					     addr);

		/*
		 * Delete the old indices for the current record.
		 * Updated signature: (dbp, v, table, cursor, idx_used)
		 */
		__generate_row_index_delete(dbp, v, table, data_cur, idx_used);

		/*
		 * If changing the record number, delete the old record.
		 */
		if (recno_will_change_p) {
			__vdbe_add_op(v, OP_Delete, data_cur, 0);
		}

		/*
		 * Create the new index entries and the new record.
		 * The record is now in registers reg_new through reg_new+nCol-1.
		 * Use OP_MakeRecord to pack it, then OP_PutIntKey to write it.
		 */
		__vdbe_add_op3(v, OP_MakeRecord, reg_new, table->nCol,
			       areg_idx[total_num_idx]);
		if (recno_will_change_p) {
			__vdbe_add_op3(v, OP_PutIntKey, data_cur,
				       areg_idx[total_num_idx], reg_new_rowid);
		} else {
			__vdbe_add_op3(v, OP_PutIntKey, data_cur,
				       areg_idx[total_num_idx], reg_old_rowid);
		}

		/*
		 * Update indices.
		 */
		for(i = 0, idx = table->pIndex; idx; idx = idx->next_vdbe, i++) {
			if (idx_used[i]) {
				int reg_idx = areg_idx[i];
				/* Generate index key in registers */
				for (j = 0; j < idx->nColumn; j++) {
					int col = idx->aiColumn[j];
					if (col < 0) {
						/* Rowid column */
						__vdbe_add_op3(v, OP_Dup,
							       recno_will_change_p ?
							       reg_new_rowid : reg_old_rowid,
							       reg_idx + j, 0);
					} else {
						__vdbe_add_op3(v, OP_Dup,
							       reg_new + col,
							       reg_idx + j, 0);
					}
				}
				__vdbe_add_op3(v, OP_MakeRecord, reg_idx,
					       idx->nColumn, reg_idx + idx->nColumn);
				__vdbe_add_op3(v, OP_IdxPut, idx_cur + i,
					       reg_idx + idx->nColumn, 0);
			}
		}
	}

	/*
	 * Increment the row counter.
	 */
	if (reg_row_count) {
		__vdbe_add_op3(v, OP_AddImm, reg_row_count, 1, 0);
	}

	/*
	 * If there are triggers, close all the cursors after each iteration
	 * through the loop. Then fire the after triggers.
	 */
	if (row_triggers_exist) {
		if (!is_view_p) {
			for(i = 0, idx = table->pIndex; idx;
			    idx = idx->next_vdbe, i++) {
				if (open_all_p || idx_used[i]) {
					__vdbe_add_op(v, OP_Close,
						      (idx_cur + i), 0);
				}
			}
			__vdbe_add_op(v, OP_Close, data_cur, 0);
			parser->nTab = cur;
		}
		if (__code_row_trigger(parser, TK_UPDATE, changes,
				       TK_AFTER, table, reg_new, reg_old,
				       on_error, addr)) {
			goto update_cleanup;
		}
	}

	/*
	 * Repeat the above with the next record to be updated, until
	 * all record selected by the WHERE clause have been updated.
	 */
	__vdbe_add_op(v, OP_Goto, 0, addr);
	__vdbe_change_p2(v, addr, __vdbe_current_addr(v));
	__vdbe_add_op(v, OP_ListReset, 0, 0);

	/*
	 * Close all tables if there were no FOR EACH ROW triggers.
	 */
	if (!row_triggers_exist) {
		for (i = 0, idx = table->pIndex; idx; idx = idx->next_vdbe, i++) {
			if (open_all_p || idx_used[i]) {
				__vdbe_add_op(v, OP_Close, (idx_cur + i), 0);
			}
		}
		__vdbe_add_op(v, OP_Close, data_cur, 0);
		parser->nTab = cur;
	}

	__vdbe_conclude_write(parser);

	/*
	 * Return the number of rows that were changed.
	 */
	if (reg_row_count) {
		__vdbe_add_op(v, OP_ColumnName, 0, 0);
		__vdbe_change_p4(v, -1, "rows updated", P4_STATIC);
		__vdbe_add_op3(v, OP_Callback, 1, reg_row_count, 0);
	}

  update_cleanup:
	__auth_context_pop(&auth);
	__dbsql_free(NULL, indices);
	__dbsql_free(NULL, xref);
	__src_list_delete(tab_list);
	__expr_list_delete(changes);
	__expr_delete(where);
	return;
}
