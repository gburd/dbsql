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
 * UPSERT (INSERT ON CONFLICT) Support
 *
 * This file implements SQL:2011 UPSERT functionality following the
 * SQLite 3.52.0 model adapted for DBSQL's register-based VDBE architecture.
 *
 * Key Components:
 * - upsert structure management (__upsert_new, __upsert_delete)
 * - Conflict target resolution (__upsert_resolve_target)
 * - VDBE code generation (__upsert_code_do_test, __upsert_code_do_update)
 * - Expression resolution for "excluded.*" references
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __upsert_new --
 *	Create a new UPSERT clause structure.
 *
 * PUBLIC: upsert_t* __upsert_new __P((DBSQL *, id_list_t *, expr_t *,
 * PUBLIC:      expr_list_t *, expr_t *, int));
 *
 * dbp			Database connection
 * target		Conflict target columns (e.g., ON CONFLICT(id, name))
 * target_where		WHERE clause for partial index targets
 * set			SET clause for DO UPDATE
 * where		WHERE clause for UPDATE condition
 * is_do_update		1 for DO UPDATE, 0 for DO NOTHING
 */
upsert_t*
__upsert_new(dbp, target, target_where, set, where, is_do_update)
	DBSQL *dbp;
	id_list_t *target;
	expr_t *target_where;
	expr_list_t *set;
	expr_t *where;
	int is_do_update;
{
	upsert_t *upsert;

	if (__dbsql_calloc(dbp, 1, sizeof(upsert_t), &upsert) == ENOMEM)
		return NULL;

	upsert->pUpsertTarget = target ? __id_list_to_expr_list(target) : NULL;
	upsert->pUpsertTargetWhere = target_where;
	upsert->pUpsertSet = set;
	upsert->pUpsertWhere = where;
	upsert->isDoUpdate = (u8)is_do_update;
	upsert->pNextUpsert = NULL;
	upsert->pUpsertIdx = NULL;
	upsert->pUpsertSrc = NULL;
	upsert->regData = 0;
	upsert->iDataCur = 0;
	upsert->iIdxCur = 0;

	/* Clean up target list since we've converted it to expr_list */
	if (target)
		__id_list_delete(target);

	return upsert;
}

/*
 * __upsert_delete --
 *	Free an UPSERT clause structure and all associated data.
 *
 * PUBLIC: void __upsert_delete __P((DBSQL *, upsert_t *));
 *
 * dbp			Database connection
 * upsert		UPSERT structure to free
 */
void
__upsert_delete(dbp, upsert)
	DBSQL *dbp;
	upsert_t *upsert;
{
	if (upsert == NULL)
		return;

	/* Free chained UPSERT clauses first */
	if (upsert->pNextUpsert)
		__upsert_delete(dbp, upsert->pNextUpsert);

	/* Free expression lists and expressions */
	if (upsert->pUpsertTarget)
		__expr_list_delete(upsert->pUpsertTarget);
	if (upsert->pUpsertTargetWhere)
		__expr_delete(upsert->pUpsertTargetWhere);
	if (upsert->pUpsertSet)
		__expr_list_delete(upsert->pUpsertSet);
	if (upsert->pUpsertWhere)
		__expr_delete(upsert->pUpsertWhere);
	if (upsert->pUpsertSrc)
		__src_list_delete(upsert->pUpsertSrc);

	__dbsql_free(dbp, upsert);
}

/*
 * __upsert_resolve_target --
 *	Resolve the conflict target to a specific UNIQUE index or PRIMARY KEY.
 *
 * PUBLIC: int __upsert_resolve_target __P((parser_t *, table_t *, upsert_t *));
 *
 * parser		Parser context
 * table		Table being inserted into
 * upsert		UPSERT clause to resolve
 *
 * Returns 0 on success, non-zero on error.
 */
int
__upsert_resolve_target(parser, table, upsert)
	parser_t *parser;
	table_t *table;
	upsert_t *upsert;
{
	index_t *idx;
	int i, j;
	int n_target;
	expr_list_t *target;

	if (upsert == NULL)
		return 0;

	target = upsert->pUpsertTarget;

	/*
	 * If no conflict target specified, use the PRIMARY KEY or first
	 * UNIQUE index.
	 */
	if (target == NULL || target->nExpr == 0) {
		/* Try PRIMARY KEY first */
		if (table->iPKey >= 0) {
			upsert->pUpsertIdx = NULL; /* NULL means PRIMARY KEY */
			return 0;
		}

		/* Fall back to first UNIQUE index */
		for (idx = table->first_index; idx; idx = idx->next_index) {
			if (idx->onError != OE_None) {
				upsert->pUpsertIdx = idx;
				return 0;
			}
		}

		__error_msg(parser, "ON CONFLICT clause without conflict "
		    "target requires PRIMARY KEY or UNIQUE constraint");
		return 1;
	}

	/*
	 * Conflict target specified. Match it to a UNIQUE index.
	 */
	n_target = target->nExpr;

	/* Check PRIMARY KEY */
	if (table->iPKey >= 0 && n_target == 1) {
		/* See if target matches PRIMARY KEY column */
		expr_t *e = target->a[0].pExpr;
		if (e && e->op == TK_ID) {
			if (strcasecmp(e->token.z, table->columns[table->iPKey].zName) == 0) {
				upsert->pUpsertIdx = NULL;
				return 0;
			}
		}
	}

	/* Check all UNIQUE indices */
	for (idx = table->first_index; idx; idx = idx->next_index) {
		if (idx->onError == OE_None)
			continue; /* Not a UNIQUE index */

		if (idx->nColumn != n_target)
			continue; /* Column count mismatch */

		/* Check if all target columns match index columns */
		for (i = 0; i < n_target; i++) {
			expr_t *e = target->a[i].pExpr;
			if (e == NULL || e->op != TK_ID)
				break;

			/* Find this column name in the index */
			for (j = 0; j < idx->nColumn; j++) {
				if (strcasecmp(e->token.z, idx->azColl[j]) == 0)
					break;
			}
			if (j >= idx->nColumn)
				break; /* Column not in this index */
		}

		if (i == n_target) {
			/* All columns matched */
			upsert->pUpsertIdx = idx;
			return 0;
		}
	}

	__error_msg(parser, "ON CONFLICT target does not match any "
	    "PRIMARY KEY or UNIQUE constraint");
	return 1;
}

/*
 * __upsert_code_do_test --
 *	Generate VDBE code to test for conflict and branch accordingly.
 *
 * PUBLIC: void __upsert_code_do_test __P((parser_t *, upsert_t *, int,
 * PUBLIC:      int, int, int));
 *
 * parser		Parser context
 * upsert		UPSERT clause
 * iTab			Cursor number for table
 * regData		First register holding data values
 * regRowid		Register holding rowid
 * on_error		Action on conflict (OE_Ignore for DO NOTHING, or jump label)
 */
void
__upsert_code_do_test(parser, upsert, iTab, regData, regRowid, on_error)
	parser_t *parser;
	upsert_t *upsert;
	int iTab;
	int regData;
	int regRowid;
	int on_error;
{
	vdbe_t *v;
	index_t *idx;
	int jump_label;

	if (upsert == NULL)
		return;

	v = parser->vdbe;
	if (v == NULL)
		return;

	idx = upsert->pUpsertIdx;

	/*
	 * For DO NOTHING, we want to skip the insert if a conflict exists.
	 * For DO UPDATE, we want to jump to the update code.
	 */
	if (on_error == OE_Ignore) {
		/* DO NOTHING: skip insert on conflict */
		jump_label = __vdbe_current_addr(v) + 3;
	} else {
		/* DO UPDATE: jump to update code */
		jump_label = on_error; /* on_error is the update label */
	}

	if (idx == NULL) {
		/*
		 * PRIMARY KEY conflict check.
		 * Use OP_NotExists to check if rowid already exists.
		 */
		__vdbe_add_op(v, OP_NotExists, iTab, jump_label);
		__vdbe_change_p3(v, -1, regRowid, P3_STATIC);
	} else {
		/*
		 * UNIQUE index conflict check.
		 * This is more complex - need to check the specific index.
		 * For now, implement a simplified version.
		 */
		/* TODO: Implement full index conflict checking */
		__vdbe_add_op(v, OP_IsUnique, iTab + idx->tnum, jump_label);
		__vdbe_change_p3(v, -1, regRowid, P3_STATIC);
	}
}

/*
 * __upsert_code_do_update --
 *	Generate VDBE code for DO UPDATE action.
 *
 * PUBLIC: void __upsert_code_do_update __P((parser_t *, upsert_t *,
 * PUBLIC:      table_t *, int, int));
 *
 * parser		Parser context
 * upsert		UPSERT clause
 * table		Table being updated
 * iDataCur		Data cursor number
 * regData		First register holding new data values
 */
void
__upsert_code_do_update(parser, upsert, table, iDataCur, regData)
	parser_t *parser;
	upsert_t *upsert;
	table_t *table;
	int iDataCur;
	int regData;
{
	vdbe_t *v;
	expr_list_t *setlist;
	int i;

	if (upsert == NULL || !upsert->isDoUpdate)
		return;

	v = parser->vdbe;
	if (v == NULL)
		return;

	setlist = upsert->pUpsertSet;
	if (setlist == NULL)
		return;

	/*
	 * Store regData in upsert structure for excluded.* resolution.
	 */
	upsert->regData = regData;
	upsert->iDataCur = iDataCur;

	/*
	 * Generate UPDATE code.
	 * For each column in SET clause, evaluate the expression
	 * and update the column value.
	 *
	 * This is a simplified version - a complete implementation
	 * would reuse the UPDATE code generator.
	 */
	for (i = 0; i < setlist->nExpr; i++) {
		expr_t *expr = setlist->a[i].pExpr;
		token_t *col_name = setlist->a[i].zName;
		int iCol;

		if (col_name == NULL)
			continue;

		/* Find column index */
		for (iCol = 0; iCol < table->nCol; iCol++) {
			if (strcasecmp(col_name->z, table->columns[iCol].zName) == 0)
				break;
		}

		if (iCol >= table->nCol) {
			__error_msg(parser, "no such column: %s", col_name->z);
			continue;
		}

		/* Evaluate expression (handles excluded.* references) */
		__expr_code(parser, expr);

		/* Store result in data register for this column */
		__vdbe_add_op(v, OP_Copy, 0, regData + iCol);
	}

	/* Generate record and update */
	int regRowid = regData - 1;
	int regRec = regData + table->nCol;
	__vdbe_add_op3(v, OP_MakeRecord, regRowid, table->nCol + 1, regRec);
	__vdbe_add_op3(v, OP_Insert, iDataCur, regRec, regRowid);

	/* TODO: Update indices */
}

/*
 * __id_list_to_expr_list --
 *	Convert an id_list to an expr_list for UPSERT target processing.
 *
 * PUBLIC: expr_list_t* __id_list_to_expr_list __P((id_list_t *));
 *
 * idlist		ID list to convert
 *
 * Returns a new expr_list with TK_ID expressions for each identifier.
 */
expr_list_t*
__id_list_to_expr_list(idlist)
	id_list_t *idlist;
{
	expr_list_t *exprlist;
	int i;

	if (idlist == NULL)
		return NULL;

	exprlist = NULL;
	for (i = 0; i < idlist->nId; i++) {
		token_t tok;
		expr_t *e;

		tok.z = idlist->a[i].zName;
		tok.n = strlen(tok.z);

		e = __expr(TK_ID, 0, 0, &tok);
		exprlist = __expr_list_append(exprlist, e, 0);
	}

	return exprlist;
}
