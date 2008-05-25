/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
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
 *
 * $Id: cg_update.c 7 2007-02-03 13:34:17Z gburd $
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
	DBSQL *dbp;           /* The database structure */
	index_t **indices = 0;/* An array of indices that need updating too */
	char *idx_used = 0;   /* idx_used[i]==1 if the i-th index is used */
	int *xref = 0;        /* xref[i] is the index in changes->a[] of the
				 an expression for the i-th column of the
				 table. xref[i]==-1 if the i-th column is not
				 changed. */
	int recno_will_change_p;/* True if the record number is being changed*/
	expr_t *recno;        /* Expression defining the new record number */
	int open_all_p;       /* True if all indices need to be opened */
	int is_view_p;        /* Trying to update a view */
	auth_context_t auth;  /* The authorization context */

	int before_triggers;  /* True if there are any BEFORE triggers */
	int after_triggers;   /* True if there are any AFTER triggers */
	int row_triggers_exist = 0;/* True if any row triggers exist */

	int new_idx      = -1;/* index of trigger "new" temp table */
	int old_idx      = -1;/* index of trigger "old" temp table */
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
	is_view_p = (table->pSelect != 0);
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
	 * Allocate a cursors for the main database table and for all indices.
	 * The index cursors might not be used, but if they are used they
	 * need to occur right after the database cursor.  So go ahead and
	 * allocate enough space, just in case.
	 */
	tab_list->a[0].iCursor = cur = parser->nTab++;
	for (idx = table->pIndex; idx; idx = idx->pNext) {
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
						  changes->a[i].zName) == 0) {
				if (j == table->iPKey) {
					recno_will_change_p = 1;
					recno = changes->a[i].pExpr;
				}
				xref[j] = i;
				break;
			}
		}
		if (j >= table->nCol) {
			if (__is_row_id(changes->a[i].zName)) {
				recno_will_change_p = 1;
				recno = changes->a[i].pExpr;
			} else {
				__error_msg(parser, "no such column: %s",
					    changes->a[i].zName);
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
	 * to every index that needs to be updated.  Indices only need
	 * updating if their key includes one of the columns named in changes
	 * or if the record number of the original table entry is changing.
	 */
	for (num_idx = total_num_idx = 0, idx = table->pIndex;
	     idx; idx = idx->pNext, total_num_idx++) {
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
	if (total_num_idx > 0) { /* TODO, is this the right amt to alloc? */
		if (__dbsql_calloc(dbp, num_idx + total_num_idx,
				sizeof(index_t *), &indices) == ENOMEM)
			goto update_cleanup;
		idx_used = (char*)&indices[num_idx];
	}
	for (num_idx = j = 0, idx = table->pIndex; idx; idx = idx->pNext, j++){
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
		} else {
			idx_used[j] = 0;
		}
	}

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
	 * If we are trying to update a view, construct that view into
	 * a temporary table.
	 */
	if (is_view_p) {
		view = __select_dup(table->pSelect);
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
		__vdbe_add_op(v, OP_Integer, 0, 0);
	}

	if (row_triggers_exist) {
		/*
		 * Create pseudo-tables for NEW and OLD
		 */
		__vdbe_add_op(v, OP_OpenPseudo, old_idx, 0);
		__vdbe_add_op(v, OP_OpenPseudo, new_idx, 0);

		/* 
		 * The top of the update loop for when there are triggers.
		 */
		__vdbe_add_op(v, OP_ListRewind, 0, 0);
		addr = __vdbe_add_op(v, OP_ListRead, 0, 0);
		__vdbe_add_op(v, OP_Dup, 0, 0);

		/*
		 * Open a cursor and make it point to the record that is
		 * being updated.
		 */
		__vdbe_add_op(v, OP_Dup, 0, 0);
		if(!is_view_p) {
			__vdbe_add_op(v, OP_Integer, table->iDb, 0);
			__vdbe_add_op(v, OP_OpenRead, cur, table->tnum);
		}
		__vdbe_add_op(v, OP_MoveTo, cur, 0);

		/*
		 * Generate the OLD table.
		 */
		__vdbe_add_op(v, OP_Recno, cur, 0);
		__vdbe_add_op(v, OP_RowData, cur, 0);
		__vdbe_add_op(v, OP_PutIntKey, old_idx, 0);

		/*
		 * Generate the NEW table.
		 */
		if (recno_will_change_p) {
			__expr_code(parser, recno);
		} else {
			__vdbe_add_op(v, OP_Recno, cur, 0);
		}
		for(i = 0; i < table->nCol; i++) {
			if (i == table->iPKey) {
				__vdbe_add_op(v, OP_String, 0, 0);
				continue;
			}
			j = xref[i];
			if (j < 0) {
				__vdbe_add_op(v, OP_Column, cur, i);
			}else{
				__expr_code(parser, changes->a[j].pExpr);
			}
		}
		__vdbe_add_op(v, OP_MakeRecord, table->nCol, 0);
		__vdbe_add_op(v, OP_PutIntKey, new_idx, 0);
		if (!is_view_p) {
			__vdbe_add_op(v, OP_Close, cur, 0);
		}

		/*
		 * Fire the BEFORE and INSTEAD OF triggers.
		 */
		if (__code_row_trigger(parser, TK_UPDATE, changes,
				       TK_BEFORE, table, new_idx, old_idx,
				       on_error, addr)) {
			goto update_cleanup;
		}
	}

	if (!is_view_p) {
		/* 
		* Open every index that needs updating.  Note that if any
		* index could potentially invoke a REPLACE conflict resolution 
		* action, then we need to open all indices because we might
		* need to be deleting some records.
		*/
		__vdbe_add_op(v, OP_Integer, table->iDb, 0);
		__vdbe_add_op(v, OP_OpenWrite, cur, table->tnum);
		if (on_error == OE_Replace) {
			open_all_p = 1;
		} else {
			open_all_p = 0;
			for (idx = table->pIndex; idx; idx = idx->pNext) {
				if (idx->onError == OE_Replace) {
					open_all_p = 1;
					break;
				}
			}
		}
		for(i = 0, idx = table->pIndex; idx; idx = idx->pNext, i++) {
			if (open_all_p || idx_used[i]) {
				__vdbe_add_op(v, OP_Integer, idx->iDb, 0);
				__vdbe_add_op(v, OP_OpenWrite, (cur + i + 1),
					      idx->tnum);
				DBSQL_ASSERT(parser->nTab > (cur + i + 1));
			}
		}

		/*
		 * Loop over every record that needs updating.  We have to load
		 * the old data for each record to be updated because some
		 * columns might not change and we will need to copy the old
		 * value. Also, the old data is needed to delete the old index
		 * entires.  So make the cursor point at the old record.
		 */
		if (!row_triggers_exist) {
			__vdbe_add_op(v, OP_ListRewind, 0, 0);
			addr = __vdbe_add_op(v, OP_ListRead, 0, 0);
			__vdbe_add_op(v, OP_Dup, 0, 0);
		}
		__vdbe_add_op(v, OP_NotExists, cur, addr);

		/*
		 * If the record number will change, push the record number
		 * as it will be after the update. (The old record number is
		 * currently on top of the stack.)
		 */
		if (recno_will_change_p) {
			__expr_code(parser, recno);
			__vdbe_add_op(v, OP_MustBeInt, 0, 0);
		}

		/*
		 * Compute new data for this record.  
		 */
		for (i = 0; i < table->nCol; i++) {
			if (i == table->iPKey) {
				__vdbe_add_op(v, OP_String, 0, 0);
				continue;
			}
			j = xref[i];
			if (j < 0) {
				__vdbe_add_op(v, OP_Column, cur, i);
			} else {
				__expr_code(parser, changes->a[j].pExpr);
			}
		}

		/*
		 * Do constraint checks.
		 */
		__generate_constraint_checks(parser, table, cur, idx_used,
					     recno_will_change_p, 1, on_error,
					     addr);

		/*
		 * Delete the old indices for the current record.
		 */
		__generate_row_index_delete(dbp, v, table, cur, idx_used);

		/*
		 * If changing the record number, delete the old record.
		 */
		if (recno_will_change_p) {
			__vdbe_add_op(v, OP_Delete, cur, 0);
		}

		/*
		 * Create the new index entries and the new record.
		 */
		__complete_insertion(parser, table, cur, idx_used,
				     recno_will_change_p, 1, -1);
	}

	/*
	 * Increment the row counter.
	 */
	if (dbp->flags & DBSQL_CountRows && !parser->trigStack) {
		__vdbe_add_op(v, OP_AddImm, 1, 0);
	}

	/*
	 * If there are triggers, close all the cursors after each iteration
	 * through the loop.  The fire the after triggers.
	 */
	if (row_triggers_exist) {
		if (!is_view_p) {
			for(i = 0, idx = table->pIndex; idx;
			    idx = idx->pNext, i++) {
				if (open_all_p || idx_used[i]) {
					__vdbe_add_op(v, OP_Close,
						      (cur + i + 1), 0);
				}
			}
			__vdbe_add_op(v, OP_Close, cur, 0);
			parser->nTab = cur;
		}
		if (__code_row_trigger(parser, TK_UPDATE, changes,
				       TK_AFTER, table, new_idx, old_idx,
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
		for (i = 0, idx = table->pIndex; idx; idx = idx->pNext, i++) {
			if (open_all_p || idx_used[i]) {
				__vdbe_add_op(v, OP_Close, (cur + i + 1), 0);
			}
		}
		__vdbe_add_op(v, OP_Close, cur, 0);
		parser->nTab = cur;
	} else {
		__vdbe_add_op(v, OP_Close, new_idx, 0);
		__vdbe_add_op(v, OP_Close, old_idx, 0);
	}

	__vdbe_conclude_write(parser);

	/*
	 * Return the number of rows that were changed.
	 */
	if (dbp->flags & DBSQL_CountRows && !parser->trigStack) {
		__vdbe_add_op(v, OP_ColumnName, 0, 0);
		__vdbe_change_p3(v, -1, "rows updated", P3_STATIC);
		__vdbe_add_op(v, OP_Callback, 1, 0);
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
