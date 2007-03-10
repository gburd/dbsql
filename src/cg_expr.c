/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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
 * http://creativecommons.org/licenses/GPL/2.0/
 *
 * $Id: cg_expr.c 7 2007-02-03 13:34:17Z gburd $
 */

/*
 * This file contains routines used for analyzing expressions and
 * for generating VDBE code that evaluates expressions in SQLite.
 */

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#endif

#include "dbsql_int.h"

/*
 * __expr --
 *	Construct a new expression node and return a pointer to it.  Memory
 *	for this node is obtained from __dbsql_calloc().  The calling function
 *	is responsible for making sure the node eventually gets freed.
 *
 * PUBLIC: expr_t *__expr __P((int, expr_t *, expr_t *, token_t *));
 */
expr_t *
__expr(op, left, right, token)
	int op;
	expr_t *left;
	expr_t *right;
	token_t *token;
{
	expr_t *new;
	if (__dbsql_calloc(NULL, 1, sizeof(expr_t), &new) == ENOMEM) {
		/* TODO If malloc fails, we leak memory from left and right */
		return 0;
	}
	new->op = op;
	new->pLeft = left;
	new->pRight = right;
	if (token) {
		DBSQL_ASSERT(token->dyn ==0);
		new->token = *token;
		new->span = *token;
	} else {
		DBSQL_ASSERT(new->token.dyn == 0);
		DBSQL_ASSERT(new->token.z == 0);
		DBSQL_ASSERT(new->token.n == 0);
		if (left && right) {
			__expr_span(new, &left->span, &right->span);
		} else {
			new->span = new->token;
		}
	}
	return new;
}

/*
 * __expr_span --
 *	Set the expr_t.span field of the given expression to span all
 *	text between the two given tokens.
 *
 * PUBLIC: void __expr_span __P((expr_t *, token_t *, token_t *));
 */
void
__expr_span(expr, left, right)
	expr_t *expr;
	token_t *left;
	token_t *right;
{
	DBSQL_ASSERT(right != 0);
	DBSQL_ASSERT(left != 0);
	/*
	 * Note: 'expr' might be NULL due to a prior __dbsql_malloc failure.
	 */
	if (expr && right->z && left->z) {
		if (left->dyn == 0 && right->dyn == 0) {
			expr->span.z = left->z;
			expr->span.n = right->n + P_TO_UINT32(right->z) -
				P_TO_UINT32(left->z);
		} else {
			expr->span.z = 0;
		}
	}
}

/*
 * __expr_function --
 *	Construct a new expression node for a function with multiple
 *	arguments.
 *
 * PUBLIC: expr_t *__expr_function __P((expr_list_t *, token_t *));
 */
expr_t *
__expr_function(list, token)
	expr_list_t *list;
	token_t *token;
{
	expr_t *new;
	if (__dbsql_calloc(NULL, 1, sizeof(expr_t), &new) == ENOMEM) {
#if 0  /* TODO Leak pList when malloc fails */
		__expr_list_delete(list);
#endif
		return 0;
	}
	new->op = TK_FUNCTION;
	new->pList = list;
	if (token) {
		DBSQL_ASSERT(token->dyn == 0);
		new->token = *token;
	} else {
		new->token.z = 0;
	}
	new->span = new->token;
	return new;
}

/*
 * __expr_delete --
 *	Recursively delete an expression tree.
 *
 * PUBLIC: void __expr_delete __P((expr_t *));
 */
void
__expr_delete(p)
	expr_t *p;
{
	if (p == 0)
		return;
	if (p->span.dyn)
		__dbsql_free(NULL, (char*)p->span.z);
	if (p->token.dyn)
		__dbsql_free(NULL, (char*)p->token.z);
	__expr_delete(p->pLeft);
	__expr_delete(p->pRight);
	__expr_list_delete(p->pList);
	__select_delete(p->pSelect);
	__dbsql_free(NULL, p);
}


/*
 * __expr_dup --
 *	The following group of routines make deep copies of expressions,
 *	expression lists, ID lists, and select statements.  The copies can
 *	be deleted (by being passed to their respective XXX_delete() routines)
 *	without effecting the originals.
 *	The expression list, ID, and source lists return by __expr_list_dup(),
 *	__id_list_dup(), and __src_list_dup() can not be further expanded 
 *	by subsequent calls to DBSQL* __XXX_list_append() routines.
 *	Any tables that the src_list_t might point to are not duplicated.
 *
 * PUBLIC: expr_t *__expr_dup __P((expr_t *));
 */
expr_t *
__expr_dup(p)
	expr_t *p;
{
	expr_t *new;
	if (p == 0)
		return 0;
	if (__dbsql_calloc(NULL, 1, sizeof(*p), &new) == ENOMEM)
		return 0;
	memcpy(new, p, sizeof(*new));
	if (p->token.z != 0) {
		__dbsql_strdup(NULL, p->token.z, &new->token.z);
		new->token.dyn = 1;
	} else {
		DBSQL_ASSERT(new->token.z == 0);
	}
	new->span.z = 0;
	new->pLeft = __expr_dup(p->pLeft);
	new->pRight = __expr_dup(p->pRight);
	new->pList = __expr_list_dup(p->pList);
	new->pSelect = __select_dup(p->pSelect);
	return new;
}

/*
 * __token_copy --
 *	Copy a token_t.
 *
 * PUBLIC: void __token_copy __P((token_t *, token_t *));
 */
void
__token_copy(to, from)
	token_t *to;
	token_t *from;
{
	if (to->dyn)
		__dbsql_free(NULL, (char*)to->z);
	if (from->z) {
		to->n = from->n;
		__dbsql_strndup(NULL, from->z, &to->z, from->n);
		to->dyn = 1;
	} else {
		to->z = 0;
	}
}

/*
 * __expr_list_dup --
 *	Copy a expr_list_t.
 *
 * PUBLIC: expr_list_t *__expr_list_dup __P((expr_list_t *));
 */
expr_list_t *
__expr_list_dup(p)
	expr_list_t *p;
{
	expr_list_t *new;
	int i;

	if (p == 0)
		return 0;
	if (__dbsql_calloc(NULL, 1, sizeof(*new), &new) == ENOMEM)
		return 0;
	new->nExpr = new->nAlloc = p->nExpr;
	if (__dbsql_calloc(NULL, p->nExpr, sizeof(p->a[0]), &new->a) == ENOMEM)
		return 0;
	for (i = 0; i < p->nExpr; i++) {
		expr_t *new_expr, *old_expr;
		new->a[i].pExpr = new_expr =
			__expr_dup(old_expr = p->a[i].pExpr);
		if (old_expr->span.z!=0 && new_expr) {
			/*
			 * Always make a copy of the span for top-level
			 * expressions in the expression list.  The logic
			 * in SELECT processing that determines the names
			 * of columns in the result set needs this
			 * information.
			 */
			__token_copy(&new_expr->span, &old_expr->span);
		}
		DBSQL_ASSERT(new_expr == 0 || new_expr->span.z != 0 ||
			     old_expr->span.z == 0);
		if (p->a[i].zName)
			__dbsql_strdup(NULL, p->a[i].zName, &new->a[i].zName);
		new->a[i].sortOrder = p->a[i].sortOrder;
		new->a[i].isAgg = p->a[i].isAgg;
		new->a[i].done = 0;
	}
	return new;
}

/*
 * __src_list_dup --
 *	Copy a src_list_t.
 *
 * PUBLIC: src_list_t *__src_list_dup __P((src_list_t *));
 */
src_list_t *
__src_list_dup(p)
	src_list_t *p;
{
	src_list_t *new;
	int i;
	int bytes;
	if (p == 0)
		return 0;
	bytes = sizeof(*p) + ((p->nSrc > 0) ?
			      (sizeof(p->a[0]) * (p->nSrc - 1)) : 0);
	if (__dbsql_calloc(NULL, 1, bytes, &new) == ENOMEM)
		return 0;
	new->nSrc = new->nAlloc = p->nSrc;
	for (i = 0; i < p->nSrc; i++) {
		struct src_list_item *new_item = &new->a[i];
		struct src_list_item *old_item = &p->a[i];
		__dbsql_strdup(NULL, old_item->zDatabase, &new_item->zDatabase);
		__dbsql_strdup(NULL, old_item->zName, &new_item->zName);
		__dbsql_strdup(NULL, old_item->zAlias, &new_item->zAlias);
		new_item->jointype = old_item->jointype;
		new_item->iCursor = old_item->iCursor;
		new_item->pTab = 0;
		new_item->pSelect = __select_dup(old_item->pSelect);
		new_item->pOn = __expr_dup(old_item->pOn);
		new_item->pUsing = __id_list_dup(old_item->pUsing);
	}
	return new;
}

/*
 * __id_list_dup --
 *	Copy a id_list_t.
 *
 * PUBLIC: id_list_t *__id_list_dup __P((id_list_t *));
 */
id_list_t *
__id_list_dup(p)
	id_list_t *p;
{
	id_list_t *new;
	int i;
	if (p == 0)
		return 0;
	if (__dbsql_calloc(NULL, 1, sizeof(*new), &new) == ENOMEM)
		return 0;
	new->nId = new->nAlloc = p->nId;
	if (__dbsql_calloc(NULL, p->nId, sizeof(p->a[0]), &new->a) == ENOMEM)
		return 0;
	for (i = 0; i < p->nId; i++) {
		struct id_list_item *new_item = &new->a[i];
		struct id_list_item *old_item = &p->a[i];
		__dbsql_strdup(NULL, old_item->zName, &new_item->zName);
		new_item->idx = old_item->idx;
	}
	return new;
}

/*
 * __select_dup --
 *	Copy a select_t.
 *
 * PUBLIC: select_t *__select_dup __P((select_t *));
 */
select_t *
__select_dup(p)
	select_t *p;
{
	select_t *new;
	if (p == 0)
		return 0;
	if (__dbsql_calloc(NULL, 1, sizeof(*p), &new) == ENOMEM)
		return 0;
	new->isDistinct = p->isDistinct;
	new->pEList = __expr_list_dup(p->pEList);
	new->pSrc = __src_list_dup(p->pSrc);
	new->pWhere = __expr_dup(p->pWhere);
	new->pGroupBy = __expr_list_dup(p->pGroupBy);
	new->pHaving = __expr_dup(p->pHaving);
	new->pOrderBy = __expr_list_dup(p->pOrderBy);
	new->op = p->op;
	new->pPrior = __select_dup(p->pPrior);
	new->nLimit = p->nLimit;
	new->nOffset = p->nOffset;
	new->zSelect = 0;
	new->iLimit = -1;
	new->iOffset = -1;
	return new;
}


/*
 * __expr_list_append --
 *	Add a new element to the end of an expression list.  If 'list' is
 *	initially NULL, then create a new expression list.
 *
 * PUBLIC: expr_list_t *__expr_list_append __P((expr_list_t *, expr_t *,
 * PUBLIC:                                 token_t *));
 */
expr_list_t *
__expr_list_append(list, expr, name)
	expr_list_t *list;
	expr_t *expr;
	token_t *name;
{
	if (list == 0) {
		if (__dbsql_calloc(NULL, 1, sizeof(expr_list_t),
				&list) == ENOMEM) {
                        /* TODO Leak memory if malloc fails
			__expr_delete(expr); */
			return 0;
		}
		DBSQL_ASSERT(list->nAlloc == 0);
	}
	if (list->nAlloc <= list->nExpr) {
		list->nAlloc = (list->nAlloc * 2) + 4;
		if (__dbsql_realloc(NULL, (list->nAlloc * sizeof(list->a[0])),
				 &list->a) == ENOMEM) {
                        /* TODO Leak memory if malloc fails
			 __expr_delete(expr); */
			list->nExpr = list->nAlloc = 0;
			return list;
		}
	}
	DBSQL_ASSERT(list->a != 0);
	if (expr || name) {
		struct expr_list_item *item = &list->a[list->nExpr++];
		memset(item, 0, sizeof(*item));
		item->pExpr = expr;
		if (name) {
			__str_nappend(&item->zName, name->z, name->n, NULL);
			__str_unquote(item->zName);
		}
	}
	return list;
}

/*
 * __expr_list_delete --
 *	Delete an entire expression list.
 *
 * PUBLIC: void __expr_list_delete __P((expr_list_t *));
 */
void
__expr_list_delete(list)
	expr_list_t *list;
{
	int i;
	if (list == 0)
		return;
	for (i = 0; i < list->nExpr; i++) {
		__expr_delete(list->a[i].pExpr);
		__dbsql_free(NULL, list->a[i].zName);
	}
	__dbsql_free(NULL, list->a);
	__dbsql_free(NULL, list);
}

/*
 * __expr_is_constant --
 *	Walk an expression tree.  Return 1 if the expression is constant
 *	and 0 if it involves variables.
 *	For the purposes of this function, a double-quoted string (ex: "abc")
 *	is considered a variable but a single-quoted string (ex: 'abc') is
 *	a constant.
 *
 * PUBLIC: int __expr_is_constant __P((expr_t *));
 */
int
__expr_is_constant(p)
	expr_t *p;
{
	int rc = 0;

	switch(p->op) {
	case TK_ID:      /* FALLTHROUGH */
	case TK_COLUMN:  /* FALLTHROUGH */
	case TK_DOT:     /* FALLTHROUGH */
	case TK_FUNCTION:/* FALLTHROUGH */
		rc = 0;
		break;
	case TK_NULL:    /* FALLTHROUGH */
	case TK_STRING:  /* FALLTHROUGH */
	case TK_INTEGER: /* FALLTHROUGH */
	case TK_FLOAT:   /* FALLTHROUGH */
	case TK_VARIABLE:
		rc = 1;
		break;
	default:
		if (p->pLeft && !__expr_is_constant(p->pLeft)) {
			rc = 0;
		} else if (p->pRight && !__expr_is_constant(p->pRight)) {
			rc = 0;
		} else {
			if (p->pList) {
				int i = 0;
				while (i < p->pList->nExpr &&
				    __expr_is_constant(p->pList->a[i].pExpr)) {
					i++;
				}
				if (i != p->pList->nExpr) {
					rc = 0;
					break;
				}
			}
			if (p->pLeft != 0 || p->pRight != 0 ||
			    (p->pList && p->pList->nExpr > 0)) {
				rc = 1;
			} else {
				rc = 0;
				break;
			}
		}
		break;
	}
	return rc;
}

/*
 * __expr_is_integer --
 *	If the given expression codes a constant integer that is small enough
 *	to fit in a 32-bit integer, return 1 and put the value of the integer
 *	in 'value'.  If the expression is not an integer or if it is too big
 *	to fit in a signed 32-bit integer, return 0 and leave
 *	'value' unchanged.
 *
 * PUBLIC: int __expr_is_integer __P((expr_t *, int *));
 */
int
__expr_is_integer(p, value)
	expr_t *p;
	int *value;
{
	const char *z;
	int v, n;

	switch(p->op) {
	case TK_INTEGER:
		if (__str_int_in32b(p->token.z)) {
			*value = atoi(p->token.z);
			return 1;
		}
		break;
	case TK_STRING:
		z = p->token.z;
		n = p->token.n;
		if (n > 0 && z[0] == '-') {
			z++;
			n--;
		}
		while (n > 0 && *z && isdigit(*z)) {
			z++;
			n--;
		}
		if (n == 0 && __str_int_in32b(p->token.z)) {
			*value = atoi(p->token.z);
			return 1;
		}
		break;
	case TK_UPLUS:
		return __expr_is_integer(p->pLeft, value);
		break;
	case TK_UMINUS:
		if (__expr_is_integer(p->pLeft, &v)) {
			*value = -v;
			return 1;
		}
		break;
	default:
		break;
	}
	return 0;
}

/*
 * __is_row_id --
 *	Return TRUE if the given string is a row-id column name.
 *
 * PUBLIC: int __is_row_id __P((const char *));
 */
int
__is_row_id(z)
	const char *z;
{
	if (strcasecmp(z, "_ROWID_") == 0)
		return 1;
	if (strcasecmp(z, "ROWID") == 0)
		return 1;
	if (strcasecmp(z, "OID") == 0)
		return 1;
	return 0;
}

/*
 * __lookup_name --
 *	Given the name of a column of the form X.Y.Z or Y.Z or just Z, look up
 *	that name in the set of source tables in 'slist' and make the 'expr'
 *	expression node refer back to that source column.  The following
 *	changes are made to 'expr':
 *
 *	expr->iDb           Set the index in db->aDb[] of the database holding
 *	                    the table.
 *	expr->iTable        Set to the cursor number for the table obtained
 *	                    from pSrcList.
 *	expr->iColumn       Set to the column number within the table.
 *	expr->dataType      Set to the appropriate data type for the column.
 *	expr->op            Set to TK_COLUMN.
 *	expr->pLeft         Any expression this points to is deleted
 *	expr->pRight        Any expression this points to is deleted.
 *
 *	The 'db_token' is the name of the database (the "X").  This value may
 *	be NULL meaning that name is of the form Y.Z or Z.  Any available
 *	database can be used.  The table_token is the name of the table
 *	(the "Y").  This value can be NULL if db_token is also NULL.  If
 *	'table_token' is NULL it means that the form of the name is Z and
 *	that columns from any table can be used.
 *
 *	If the name cannot be resolved unambiguously, leave an error message
 *	in 'parser' and return non-zero.  Return zero on success.
 *
 * STATIC: static int __lookup_name __P((parser_t *, token_t *, token_t *,
 * STATIC:                          token_t *, src_list_t *, expr_list_t *,
 * STATIC:                          expr_t *));
 *
 * parser			The parsing context
 * db_token			Name of the database containing table, or NULL
 * table_token			Name of table containing column, or NULL
 * col_token			Name of the column.
 * slist			List of tables used to resolve column names
 * elist			List of expressions used to resolve "AS"
 * expr				Make this EXPR node point to the selected
 *				column
 */
static int
__lookup_name(parser, db_token, table_token, col_token, slist,
	      elist, expr)
	parser_t *parser;
	token_t *db_token;
	token_t *table_token;
	token_t *col_token;
	src_list_t *slist;
	expr_list_t *elist;
	expr_t *expr;
{
	char *db_name = 0;   /* Name of the database.  The "X" in X.Y.Z */
	char *table_name = 0;/* Name of the table.  The "Y" in X.Y.Z or Y.Z */
	char *col_name = 0;  /* Name of the column.  The "Z" */
	int i, j;            /* Loop counters */
	int cnt = 0;         /* Number of matching column names */
	int ntables = 0;     /* Number of matching table names */
	DBSQL *dbp = parser->db; /* The database */

	DBSQL_ASSERT(col_token && col_token->z); /* The Z in X.Y.Z cannot be NULL */
	if (db_token && db_token->z) {
		__dbsql_strndup(dbp, db_token->z, &db_name, db_token->n);
		__str_unquote(db_name);
	} else {
		db_name = 0;
	}
	if (table_token && table_token->z) {
		__dbsql_strndup(dbp, table_token->z, &table_name,
				table_token->n);
		__str_unquote(table_name);
	} else {
		DBSQL_ASSERT(db_name == 0);
		table_name = 0;
	}
	if (__dbsql_strndup(dbp, col_token->z, &col_name,
			    col_token->n) == ENOMEM){
                /* TODO Leak memory (db_name and table_name) if malloc fails */
		return 1;
	}
	__str_unquote(col_name);
	DBSQL_ASSERT(table_name == 0 || elist == 0);

	expr->iTable = -1;
	for (i = 0; i < slist->nSrc; i++) {
		struct src_list_item *item = &slist->a[i];
		table_t *table = item->pTab;
		column_t *column;

		if (table == 0)
			continue;
		DBSQL_ASSERT(table->nCol > 0);
		if (table_name) {
			if (item->zAlias) {
				char *tname = item->zAlias;
				if (strcasecmp(tname, table_name) != 0 ) {
					continue;
				}
			} else {
				char *tname = table->zName;
				if (tname == 0 ||
				    strcasecmp(tname, table_name) != 0) {
					continue;
				}
				if (db_name != 0 && strcasecmp(
					    dbp->aDb[table->iDb].zName,
					    db_name) != 0) {
					continue;
				}
			}
		}
		if (0 == (ntables++)) {
			expr->iTable = item->iCursor;
			expr->iDb = table->iDb;
		}
		for (j = 0, column = table->aCol; j < table->nCol;
		     j++, column++) {
			if (strcasecmp(column->zName,
						  col_name) == 0){
				cnt++;
				expr->iTable = item->iCursor;
				expr->iDb = table->iDb;
				/*
				 * Substitute the rowid (column -1) for the
				 * INTEGER PRIMARY KEY
				 */
				expr->iColumn = ((j == table->iPKey) ? -1 : j);
				expr->dataType = column->sortOrder &
					DBSQL_SO_TYPEMASK;
				break;
			}
		}
	}

	/*
	 * If we have not already resolved the name, then maybe 
	 * it is a new.* or old.* trigger argument reference
	 */
	if (db_name == 0 && table_name != 0 && cnt == 0 &&
	    parser->trigStack != 0) {
		trigger_stack_t *tstack = parser->trigStack;
		table_t *table = 0;
		if (tstack->newIdx != -1 &&
		    strcasecmp("new", table_name) == 0) {
			expr->iTable = tstack->newIdx;
			DBSQL_ASSERT(tstack->pTab);
			table = tstack->pTab;
		} else if (tstack->oldIdx != -1 &&
			   strcasecmp("old", table_name) == 0) {
			expr->iTable = tstack->oldIdx;
			DBSQL_ASSERT(tstack->pTab);
			table = tstack->pTab;
		}
		if(table) {
			int j;
			column_t *column = table->aCol;
			expr->iDb = table->iDb;
			ntables++;
			for (j = 0; j < table->nCol; j++, column++) {
				if (strcasecmp(column->zName,
							  col_name) == 0) {
					cnt++;
					expr->iColumn = ((j == table->iPKey) ?
							 -1 : j);
					expr->dataType = column->sortOrder &
						DBSQL_SO_TYPEMASK;
					break;
				}
			}
		}
	}

	/*
	 * Perhaps the name is a reference to the ROWID.
	 */
	if (cnt == 0 && ntables == 1 && __is_row_id(col_name)) {
		cnt = 1;
		expr->iColumn = -1;
		expr->dataType = DBSQL_SO_NUM;
	}

	/*
	 * If the input is of the form Z (not Y.Z or X.Y.Z) then the name Z
	 * might refer to an result-set alias.  This happens, for example, when
	 * we are resolving names in the WHERE clause of the following command:
	 *
	 *     SELECT a+b AS x FROM table WHERE x<10;
	 *
	 * In cases like this, replace expr with a copy of the expression that
	 * forms the result set entry ("a+b" in the example) and return
	 * immediately.  Note that the expression in the result set should
	 * have already been resolved by the time the WHERE clause is resolved.
	 */
	if (cnt == 0 && elist != 0) {
		for (j = 0; j < elist->nExpr; j++) {
			char *as = elist->a[j].zName;
			if (as != 0 && strcasecmp(as,
							     col_name) == 0) {
				DBSQL_ASSERT(expr->pLeft == 0 && expr->pRight == 0);
				expr->op = TK_AS;
				expr->iColumn = j;
				expr->pLeft = __expr_dup(elist->a[j].pExpr);
				__dbsql_free(NULL, col_name);
				DBSQL_ASSERT(table_name == 0 && db_name == 0);
				return 0;
			}
		} 
	}

	/*
	 * If X and Y are NULL (in other words if only the column name Z is
	 * supplied) and the value of Z is enclosed in double-quotes, then
	 * Z is a string literal if it doesn't match any column names.  In that
	 * case, we need to return right away and not make any changes to
	 * expr.
	 */
	if (cnt == 0 && table_name == 0 && col_token->z[0] == '"') {
		__dbsql_free(NULL, col_name);
		return 0;
	}

	/*
	 * Note, cnt==0 means there was not match.  Also cnt>1 means there were
	 * two or more matches.  Either way, we have an error.
	 */
	if (cnt != 1) {
		char *z = 0;
		char *err;
		err = (cnt == 0) ? "no such column: %s" :
			"ambiguous column name: %s";
		if (db_name) {
			__str_append(&z, db_name, ".", table_name, ".",
				     col_name, 0);
		} else if (table_name) {
			__str_append(&z, table_name, ".", col_name, 0);
		} else {
			__dbsql_strdup(NULL, col_name, &z);
		}
		__error_msg(parser, err, z);
		__dbsql_free(NULL, z);
	}

	/*
	 * Clean up and return
	 */
	__dbsql_free(NULL, db_name);
	__dbsql_free(NULL, table_name);
	__dbsql_free(NULL, col_name);
	__expr_delete(expr->pLeft);
	expr->pLeft = 0;
	__expr_delete(expr->pRight);
	expr->pRight = 0;
	expr->op = TK_COLUMN;
	__auth_read(parser, expr, slist);
	return cnt!=1;
}

/*
 * __expr_resolve_ids --
 *	This routine walks an expression tree and resolves references to
 *	table columns.  Nodes of the form ID.ID or ID resolve into an
 *	index to the table in the table list and a column offset.  The 
 *	 expr_t.opcode for such nodes is changed to TK_COLUMN.  The
 *	expr_t.iTable value is changed to the index of the referenced table
 *	in 'elist' plus the "base" value.  The base value will ultimately
 *	become the VDBE cursor number for a cursor that is pointing into
 *	the referenced table.  The expr_t.iColumn value is changed to the
 *	index of the column of the referenced table.  The expr_t.iColumn
 *	value for the special ROWID column is -1.  Any INTEGER PRIMARY KEY
 *	column is tried as an alias for ROWID.
 *
 *	We also check for instances of the IN operator.  IN comes in two
 *	forms:
 *
 *		expr IN (exprlist)
 *	and
 *		expr IN (SELECT ...)
 *
 *	The first form is handled by creating a set holding the list
 *	of allowed values.  The second form causes the SELECT to generate 
 *	a temporary table.
 *
 *	This routine also looks for scalar SELECTs that are part of an
 *	expression.
 *	If it finds any, it generates code to write the value of that select
 *	into a memory cell.
 *
 *	Unknown columns or tables provoke an error.  The function returns
 *	the number of errors seen and leaves an error message on
 *	parser->zErrMsg.
 *
 * PUBLIC: int __expr_resolve_ids __P((parser_t *, src_list_t *, expr_list_t *,
 * PUBLIC:                        expr_t *));
 *
 *	parser			The parser context
 *	slist			List of tables used to resolve column names
 *	elist			List of expressions used to resolve "AS"
 *	expr			The expression to be analyzed
 */
int __expr_resolve_ids(parser, slist, elist, expr)
	parser_t *parser;
	src_list_t *slist;
	expr_list_t *elist;
	expr_t *expr;
{
	int i, iset;
	vdbe_t *v;
	token_t *column;
	token_t *table;
	token_t *database;
	expr_t *right;
	int addr;
	expr_t *e2;

	if (expr == 0 || slist == 0)
		return 0;
	for (i = 0; i < slist->nSrc; i++) {
		DBSQL_ASSERT(slist->a[i].iCursor >= 0 &&
		       slist->a[i].iCursor<parser->nTab);
	}
	switch(expr->op) {
		/*
		 * Double-quoted strings (ex: "abc") are used as identifiers if
		 * possible.  Otherwise they remain as strings.  Single-quoted
		 * strings (ex: 'abc') are always string literals.
		 */
	case TK_STRING:
		if (expr->token.z[0] == '\'') {
			break;
		}
		/*
		 * FALLTHROUGH into the TK_ID case if this is a
		 * double-quoted string
		 */
	case TK_ID:
		/*
		 * A lone identifier is the name of a column.
		 */
		if (__lookup_name(parser, 0, 0, &expr->token, slist,
				  elist, expr)) {
			return 1;
		}
		break; 
	case TK_DOT:
		/*
		 * A table name and column name:     ID.ID
		 * Or a database, table and column:  ID.ID.ID
		 */
		right = expr->pRight;
		if (right->op == TK_ID) {
			database = 0;
			table = &expr->pLeft->token;
			column = &right->token;
		} else {
			DBSQL_ASSERT(right->op == TK_DOT);
			database = &expr->pLeft->token;
			table = &right->pLeft->token;
			column = &right->pRight->token;
		}
		if (__lookup_name(parser, database, table, column, slist,
				  0, expr)) {
			return 1;
		}
		break;
	case TK_IN:
		v = __parser_get_vdbe(parser);
		if (v == 0)
			return 1;
		if (__expr_resolve_ids(parser, slist, elist, expr->pLeft)) {
			return 1;
		}
		if (expr->pSelect) {
			/*
			 * Case 1:     expr IN (SELECT ...)
			 *
			 * Generate code to write the results of the select
			 * into a temporary table.  The cursor number of the
			 * temporary table has already been put in iTable by
			 * __expr_resolve_in_select().
			 */
			expr->iTable = parser->nTab++;
			__vdbe_add_op(v, OP_OpenTemp, expr->iTable, 1);
			__select(parser, expr->pSelect, SRT_Set,
				 expr->iTable, 0,0,0);
		} else if (expr->pList) {
			/*
			 * Case 2:     expr IN (exprlist)
			 *
			 * Create a set to put the exprlist values in.  The
			 * Set id is stored in iTable.
			 */
			for (i = 0; i < expr->pList->nExpr; i++) {
				e2 = expr->pList->a[i].pExpr;
				if (!__expr_is_constant(e2)) {
					__error_msg(parser,
						    "right-hand side of IN "
						    "operator must be "
						    "constant");
					return 1;
				}
				if (__expr_check(parser, e2, 0, 0)) {
					return 1;
				}
			}
			iset = expr->iTable = parser->nSet++;
			for (i = 0; i < expr->pList->nExpr; i++) {
				expr_t *e2 = expr->pList->a[i].pExpr;
				switch(e2->op) {
				case TK_FLOAT: /* FALLTHROUGH */
				case TK_INTEGER:
				case TK_STRING:
					addr = __vdbe_add_op(v, OP_SetInsert,
							     iset, 0);
					DBSQL_ASSERT(e2->token.z);
					__vdbe_change_p3(v, addr, e2->token.z,
							 e2->token.n);
					__vdbe_dequote_p3(v, addr);
					break;
				default:
					__expr_code(parser, e2);
					__vdbe_add_op(v, OP_SetInsert, iset,0);
					break;
				}
			}
		}
		break;
	case TK_SELECT:
		/*
		 * This has to be a scalar SELECT.  Generate code to put the
		 * value of this select in a memory cell and record the number
		 * of the memory cell in iColumn.
		 */
		expr->iColumn = parser->nMem++;
		if (__select(parser, expr->pSelect, SRT_Mem,
			     expr->iColumn,0,0,0)) {
			return 1;
		}
		break;
	default:
		/*
		 * In all other cases, recursively walk the tree.
		 */
		if (expr->pLeft &&
		    __expr_resolve_ids(parser, slist, elist, expr->pLeft)) {
			return 1;
		}
		if (expr->pRight &&
		    __expr_resolve_ids(parser, slist, elist, expr->pRight)) {
			return 1;
		}
		if (expr->pList) {
			int i;
			expr_list_t *el = expr->pList;
			for (i = 0; i < el->nExpr; i++) {
				expr_t *arg = el->a[i].pExpr;
				if (__expr_resolve_ids(parser, slist, elist,
						       arg)) {
					return 1;
				}
			}
		}
	}
	return 0;
}

/*
 * __get_function_name --
 *	'expr' is a node that defines a function of some kind.  It might
 *	be a syntactic function like "count(x)" or it might be a function
 *	that implements an operator, like "a LIKE b".  
 *	This routine makes 'name' point to the name of the function and 
 *	'len' hold the number of characters in the function name.
 *
 * STATIC: static void __get_function_name __P((expr_t *, const char **,
 * STATIC:                                 int *));
 */
static void
__get_function_name(expr, name, len)
	expr_t *expr;
	const char **name;
	int *len;
{
	switch(expr->op) {
	case TK_FUNCTION:
		*name = expr->token.z;
		*len = expr->token.n;
		break;
	case TK_LIKE:
		*name = "like";
		*len = 4;
		break;
	case TK_GLOB:
		*name = "glob";
		*len = 4;
		break;
	default:
		/* TODO shouldn't this do an DBSQL_ASSERT() or something? fail? */
		*name = "can't happen";
		*len = 12;
		break;
	}
}

/*
 * __expr_check --
 *	Error check the functions in an expression.  Make sure all
 *	function names are recognized and all functions have the correct
 *	number of arguments.  Leave an error message in parser->zErrMsg
 *	if anything is amiss.  Return the number of errors.
 *	If 'agg' is not null and this expression is an aggregate function
 *	(like count(*) or max(value)) then write a 1 into *agg.
 *
 * PUBLIC: int __expr_check __P((parser_t *, expr_t *, int, int *));
 */
int
__expr_check(parser, expr, agg_allowed, agg)
	parser_t *parser;
	expr_t *expr;
	int agg_allowed;
	int *agg;
{
	int nerr, i, n, no_such_func, is_type_of, wrong_num_args, is_agg, nid;
	const char *id;
	func_def_t *def;
	expr_t *e2;

	if (expr == 0)
		return 0;

	nerr = 0;

	switch(expr->op) {
	case TK_GLOB: /* FALLTHROUGH */
	case TK_LIKE: /* FALLTHROUGH */
	case TK_FUNCTION:
		n = (expr->pList) ?
			expr->pList->nExpr : 0;  /* Number of arguments */
		no_such_func = 0; /* True if no such function exists */
		is_type_of = 0;   /* True if is the special TypeOf() func */
		wrong_num_args = 0;/* True if wrong number of arguments */
		is_agg = 0;       /* True if is an aggregate function */
		nid = 0;          /* Number of characters in function name */
		id = "";          /* The function name. */

		__get_function_name(expr, &id, &nid);
		def = __find_function(parser->db, id, nid, n, 0);
		if (def == 0) {
			def = __find_function(parser->db, id, nid, -1, 0);
			if (def == 0) {
				if (n == 1 && nid == 6 &&
			         strncasecmp(id, "typeof",6) == 0) {
					is_type_of = 1;
				} else {
					no_such_func = 1;
				}
			} else {
				wrong_num_args = 1;
			}
		} else {
			is_agg = (def->xFunc == 0);
		}
		if (is_agg && !agg_allowed) {
			__str_nappend(&parser->zErrMsg,
				      "misuse of aggregate function ", -1,
				      id, nid, "()", 2, NULL);
			parser->nErr++;
			nerr++;
			is_agg = 0;
		} else if (no_such_func) {
			__str_nappend(&parser->zErrMsg,
				      "no such function: ", -1, id, nid, NULL);
			parser->nErr++;
			nerr++;
		} else if (wrong_num_args) {
			__str_nappend(&parser->zErrMsg, 
				      "wrong number of arguments to function ",
				      -1, id, nid, "()", 2, NULL);
			parser->nErr++;
			nerr++;
		}
		if (is_agg)
			expr->op = TK_AGG_FUNCTION;
		if (is_agg && agg)
			*agg = 1;
		for (i = 0; nerr == 0 && i < n; i++) {
			nerr = __expr_check(parser, expr->pList->a[i].pExpr,
					    agg_allowed && !is_agg, agg);
		}
		if (def == 0) {
			if (is_type_of) {
				expr->op = TK_STRING;
				if (__expr_type(expr->pList->a[0].pExpr) ==
				    DBSQL_SO_NUM) {
					expr->token.z = "numeric";
					expr->token.n = 7;
				} else {
					expr->token.z = "text";
					expr->token.n = 4;
				}
			}
		} else if (def->dataType >= 0) {
			if (def->dataType < n) {
				expr->dataType = 
					__expr_type(expr->pList->a[def->dataType].pExpr);
			} else {
				expr->dataType = DBSQL_SO_NUM;
			}
		} else if (def->dataType == DBSQL_ARGS) {
			def->dataType = DBSQL_SO_TEXT;
			for (i = 0; i < n; i++) {
				if (__expr_type(expr->pList->a[i].pExpr) ==
				    DBSQL_SO_NUM) {
					expr->dataType = DBSQL_SO_NUM;
					break;
				}
			}
		} else if (def->dataType == DBSQL_NUMERIC) {
			expr->dataType = DBSQL_SO_NUM;
		} else {
			expr->dataType = DBSQL_SO_TEXT;
		}
		break;
	default:
		if (expr->pLeft) {
			nerr = __expr_check(parser, expr->pLeft,
					    agg_allowed, agg);
		}
		if (nerr == 0 && expr->pRight) {
			nerr = __expr_check(parser, expr->pRight,
					    agg_allowed, agg);
		}
		if (nerr == 0 && expr->pList) {
			n = expr->pList->nExpr;
			for (i = 0; nerr == 0 && i < n; i++) {
				e2 = expr->pList->a[i].pExpr;
				nerr = __expr_check(parser, e2,
						    agg_allowed, agg);
			}
		}
		break;
	}
	return nerr;
}

/*
 * __expr_type --
 *	Return either DBSQL_SO_NUM or DBSQL_SO_TEXT to indicate whether the
 *	given expression should sort as numeric values or as text.
 *	The __expr_resolve_ids() and __expr_check() routines must have
 *	both been called on the expression before it is passed to this routine.
 *
 * PUBLIC: int __expr_type __P((expr_t *));
 */
int
__expr_type(p)
	expr_t *p;
{
	int i;
	expr_list_t *list;

	if (p == 0)
		return DBSQL_SO_NUM;

	while(p)
		switch(p->op) {
		case TK_PLUS: /* FALLTHROUGH */
		case TK_MINUS: /* FALLTHROUGH */
		case TK_STAR: /* FALLTHROUGH */
		case TK_SLASH: /* FALLTHROUGH */
		case TK_AND: /* FALLTHROUGH */
		case TK_OR: /* FALLTHROUGH */
		case TK_ISNULL: /* FALLTHROUGH */
		case TK_NOTNULL: /* FALLTHROUGH */
		case TK_NOT: /* FALLTHROUGH */
		case TK_UMINUS: /* FALLTHROUGH */
		case TK_UPLUS: /* FALLTHROUGH */
		case TK_BITAND: /* FALLTHROUGH */
		case TK_BITOR: /* FALLTHROUGH */
		case TK_BITNOT: /* FALLTHROUGH */
		case TK_LSHIFT: /* FALLTHROUGH */
		case TK_RSHIFT: /* FALLTHROUGH */
		case TK_REM: /* FALLTHROUGH */
		case TK_INTEGER: /* FALLTHROUGH */
		case TK_FLOAT: /* FALLTHROUGH */
		case TK_IN: /* FALLTHROUGH */
		case TK_BETWEEN: /* FALLTHROUGH */
		case TK_GLOB: /* FALLTHROUGH */
		case TK_LIKE:
			return DBSQL_SO_NUM;
		case TK_STRING: /* FALLTHROUGH */
		case TK_NULL: /* FALLTHROUGH */
		case TK_CONCAT: /* FALLTHROUGH */
		case TK_VARIABLE:
			return DBSQL_SO_TEXT;
		case TK_LT: /* FALLTHROUGH */
		case TK_LE: /* FALLTHROUGH */
		case TK_GT: /* FALLTHROUGH */
		case TK_GE: /* FALLTHROUGH */
		case TK_NE: /* FALLTHROUGH */
		case TK_EQ:
			if (__expr_type(p->pLeft) == DBSQL_SO_NUM) {
				return DBSQL_SO_NUM;
			}
			p = p->pRight;
			break;
		case TK_AS:
			p = p->pLeft;
			break;
		case TK_COLUMN: /* FALLTHROUGH */
		case TK_FUNCTION: /* FALLTHROUGH */
		case TK_AGG_FUNCTION:
			return p->dataType;
		case TK_SELECT:
			DBSQL_ASSERT(p->pSelect);
			DBSQL_ASSERT(p->pSelect->pEList);
			DBSQL_ASSERT(p->pSelect->pEList->nExpr > 0);
			p = p->pSelect->pEList->a[0].pExpr;
			break;
		case TK_CASE:
			if (p->pRight &&
			    __expr_type(p->pRight) == DBSQL_SO_NUM) {
				return DBSQL_SO_NUM;
			}
			if (p->pList) {
				list = p->pList;
				for (i = 1; i < list->nExpr; i += 2) {
					if(__expr_type(list->a[i].pExpr)
					   == DBSQL_SO_NUM) {
						return DBSQL_SO_NUM;
					}
				}
			}
			return DBSQL_SO_TEXT;
		default:
			DBSQL_ASSERT(p->op == TK_ABORT);  /* Can't Happen */
			break;
		}
	return DBSQL_SO_NUM;
}

/*
 * __expr_code --
 *	Generate code into the current Vdbe to evaluate the given
 *	expression and leave the result on the top of stack.
 *
 * PUBLIC: void __expr_code __P((parser_t *, expr_t *));
 */
void
__expr_code(parser, expr)
	parser_t *parser;
	expr_t *expr;
{
	vdbe_t *v = parser->pVdbe;
	int i, op, dest, nexpr, nid, addr, expr_end_lable, jump_inst;
	int expr_end_label;
	char *z;
	expr_list_t *list;
	func_def_t *def;
	const char *id;

	if (v == 0 || expr == 0)
		return;

	switch(expr->op) {
	case TK_PLUS:     op = OP_Add;      break;
	case TK_MINUS:    op = OP_Subtract; break;
	case TK_STAR:     op = OP_Multiply; break;
	case TK_SLASH:    op = OP_Divide;   break;
	case TK_AND:      op = OP_And;      break;
	case TK_OR:       op = OP_Or;       break;
	case TK_LT:       op = OP_Lt;       break;
	case TK_LE:       op = OP_Le;       break;
	case TK_GT:       op = OP_Gt;       break;
	case TK_GE:       op = OP_Ge;       break;
	case TK_NE:       op = OP_Ne;       break;
	case TK_EQ:       op = OP_Eq;       break;
	case TK_ISNULL:   op = OP_IsNull;   break;
	case TK_NOTNULL:  op = OP_NotNull;  break;
	case TK_NOT:      op = OP_Not;      break;
	case TK_UMINUS:   op = OP_Negative; break;
	case TK_BITAND:   op = OP_BitAnd;   break;
	case TK_BITOR:    op = OP_BitOr;    break;
	case TK_BITNOT:   op = OP_BitNot;   break;
	case TK_LSHIFT:   op = OP_ShiftLeft;  break;
	case TK_RSHIFT:   op = OP_ShiftRight; break;
	case TK_REM:      op = OP_Remainder;  break;
	default: break;
	}

	switch(expr->op) {
	case TK_COLUMN:
		if( parser->useAgg ) {
			__vdbe_add_op(v, OP_AggGet, 0, expr->iAgg);
		} else if (expr->iColumn >= 0) {
			__vdbe_add_op(v, OP_Column, expr->iTable,
				      expr->iColumn);
		} else {
			__vdbe_add_op(v, OP_Recno, expr->iTable, 0);
		}
		break;
	case TK_STRING:
	case TK_FLOAT:
	case TK_INTEGER:
		if (expr->op == TK_INTEGER &&
		    __str_int_in32b(expr->token.z)) {
			__vdbe_add_op(v, OP_Integer, atoi(expr->token.z), 0);
		} else {
			__vdbe_add_op(v, OP_String, 0, 0);
		}
		DBSQL_ASSERT(expr->token.z);
		__vdbe_change_p3(v, -1, expr->token.z, expr->token.n);
		__vdbe_dequote_p3(v, -1);
		break;
	case TK_NULL:
		__vdbe_add_op(v, OP_String, 0, 0);
		break;
	case TK_VARIABLE:
		__vdbe_add_op(v, OP_Variable, expr->iTable, 0);
		break;
	case TK_LT: /* FALLTHROUGH */
	case TK_LE: /* FALLTHROUGH */
	case TK_GT: /* FALLTHROUGH */
	case TK_GE: /* FALLTHROUGH */
	case TK_NE: /* FALLTHROUGH */
	case TK_EQ:
		if (__expr_type(expr) == DBSQL_SO_TEXT) {
			op += 6;  /* Convert numeric opcodes to text opcodes */
		}
		/* FALLTHROUGH */
	case TK_AND: /* FALLTHROUGH */
	case TK_OR: /* FALLTHROUGH */
	case TK_PLUS: /* FALLTHROUGH */
	case TK_STAR: /* FALLTHROUGH */
	case TK_MINUS: /* FALLTHROUGH */
	case TK_REM: /* FALLTHROUGH */
	case TK_BITAND: /* FALLTHROUGH */
	case TK_BITOR: /* FALLTHROUGH */
	case TK_SLASH:
		__expr_code(parser, expr->pLeft);
		__expr_code(parser, expr->pRight);
		__vdbe_add_op(v, op, 0, 0);
		break;
	case TK_LSHIFT: /* FALLTHROUGH */
	case TK_RSHIFT:
		__expr_code(parser, expr->pRight);
		__expr_code(parser, expr->pLeft);
		__vdbe_add_op(v, op, 0, 0);
		break;
	case TK_CONCAT:
		__expr_code(parser, expr->pLeft);
		__expr_code(parser, expr->pRight);
		__vdbe_add_op(v, OP_Concat, 2, 0);
		break;
	case TK_UMINUS:
		DBSQL_ASSERT(expr->pLeft);
		if (expr->pLeft->op == TK_FLOAT ||
		    expr->pLeft->op == TK_INTEGER) {
			token_t *p = &expr->pLeft->token;
			__dbsql_calloc(parser->db, 1, p->n + 2, &z);
			sprintf(z, "-%.*s", p->n, p->z);
			if (expr->pLeft->op == TK_INTEGER &&
			    __str_int_in32b(z)) {
				__vdbe_add_op(v, OP_Integer, atoi(z), 0);
			} else {
				__vdbe_add_op(v, OP_String, 0, 0);
			}
			__vdbe_change_p3(v, -1, z, (p->n + 1));
			__dbsql_free(NULL, z);
			break;
		}
		/* FALLTHROUGH */
	case TK_BITNOT: /* FALLTHROUGH */
	case TK_NOT:
		__expr_code(parser, expr->pLeft);
		__vdbe_add_op(v, op, 0, 0);
		break;
	case TK_ISNULL:
	case TK_NOTNULL:
		__vdbe_add_op(v, OP_Integer, 1, 0);
		__expr_code(parser, expr->pLeft);
		dest = __vdbe_current_addr(v) + 2;
		__vdbe_add_op(v, op, 1, dest);
		__vdbe_add_op(v, OP_AddImm, -1, 0);
		break;
	case TK_AGG_FUNCTION:
		__vdbe_add_op(v, OP_AggGet, 0, expr->iAgg);
		break;
	case TK_GLOB: /* FALLTHROUGH */
	case TK_LIKE: /* FALLTHROUGH */
	case TK_FUNCTION:
		list = expr->pList;
		nexpr = list ? list->nExpr : 0;
		__get_function_name(expr, &id, &nid);
		def = __find_function(parser->db, id, nid, nexpr, 0);
		DBSQL_ASSERT(def != 0);
		for (i = 0; i < nexpr; i++) {
			__expr_code(parser, list->a[i].pExpr);
		}
		__vdbe_add_op(v, OP_Function, nexpr, 0);
		__vdbe_change_p3(v, -1, (char*)def, P3_POINTER);
		break;
	case TK_SELECT:
		__vdbe_add_op(v, OP_MemLoad, expr->iColumn, 0);
		break;
	case TK_IN:
		__vdbe_add_op(v, OP_Integer, 1, 0);
		__expr_code(parser, expr->pLeft);
		addr = __vdbe_current_addr(v);
		__vdbe_add_op(v, OP_NotNull, -1, addr+4);
		__vdbe_add_op(v, OP_Pop, 1, 0);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_add_op(v, OP_Goto, 0, (addr + 6));
		if (expr->pSelect) {
			__vdbe_add_op(v, OP_Found, expr->iTable, (addr + 6));
		} else {
			__vdbe_add_op(v, OP_SetFound, expr->iTable,(addr + 6));
		}
		__vdbe_add_op(v, OP_AddImm, -1, 0);
		break;
	case TK_BETWEEN:
		__expr_code(parser, expr->pLeft);
		__vdbe_add_op(v, OP_Dup, 0, 0);
		__expr_code(parser, expr->pList->a[0].pExpr);
		__vdbe_add_op(v, OP_Ge, 0, 0);
		__vdbe_add_op(v, OP_Pull, 1, 0);
		__expr_code(parser, expr->pList->a[1].pExpr);
		__vdbe_add_op(v, OP_Le, 0, 0);
		__vdbe_add_op(v, OP_And, 0, 0);
		break;
	case TK_UPLUS: /* FALLTHROUGH */
	case TK_AS:
		__expr_code(parser, expr->pLeft);
		break;
	case TK_CASE:
		DBSQL_ASSERT(expr->pList);
		DBSQL_ASSERT((expr->pList->nExpr % 2) == 0);
		DBSQL_ASSERT(expr->pList->nExpr > 0);
		nexpr = expr->pList->nExpr;
		expr_end_label = __vdbe_make_label(v);
		if (expr->pLeft) {
			__expr_code(parser, expr->pLeft);
		}
		for (i = 0; i < nexpr; i += 2) {
			__expr_code(parser, expr->pList->a[i].pExpr);
			if (expr->pLeft) {
				__vdbe_add_op(v, OP_Dup, 1, 1);
				jump_inst = __vdbe_add_op(v, OP_Ne, 1, 0);
				__vdbe_add_op(v, OP_Pop, 1, 0);
			} else {
				jump_inst = __vdbe_add_op(v, OP_IfNot, 1, 0);
			}
			__expr_code(parser, expr->pList->a[i+1].pExpr);
			__vdbe_add_op(v, OP_Goto, 0, expr_end_label);
			addr = __vdbe_current_addr(v);
			__vdbe_change_p2(v, jump_inst, addr);
		}
		if (expr->pLeft) {
			__vdbe_add_op(v, OP_Pop, 1, 0);
		}
		if (expr->pRight) {
			__expr_code(parser, expr->pRight);
		} else {
			__vdbe_add_op(v, OP_String, 0, 0);
		}
		__vdbe_resolve_label(v, expr_end_label);
		break;
	case TK_RAISE:
		if (!parser->trigStack) {
			__error_msg(parser,
				    "RAISE() may only be used within a "
				    "trigger-program");
			parser->nErr++;
			return;
		}
		if (expr->iColumn == OE_Rollback ||
		    expr->iColumn == OE_Abort ||
		    expr->iColumn == OE_Fail ){
			char *msg;
			__dbsql_strndup(parser->db, expr->token.z, &msg,
				     expr->token.n);
			__vdbe_add_op(v, OP_Halt, DBSQL_CONSTRAINT,
				      expr->iColumn);
			__str_unquote(msg);
			__vdbe_change_p3(v, -1, msg, 0);
			__dbsql_free(parser->db, msg);
		} else {
			DBSQL_ASSERT(expr->iColumn == OE_Ignore);
			__vdbe_add_op(v, OP_Goto, 0,
				      parser->trigStack->ignoreJump);
			__vdbe_change_p3(v, -1, "(IGNORE jump)", 0);
		}
		break;
	}
}

/*
 * __expr_if_true --
 *	Generate code for a boolean expression such that a jump is made
 *	to the label "dest" if the expression is true but execution
 *	continues straight thru if the expression is false.
 *	If the expression evaluates to NULL (neither true nor false), then
 *	take the jump if the jump_if_null flag is true.
 *
 * PUBILC: void __expr_if_true __P((parser_t *, expr_t *, int, int));
 */
void
__expr_if_true(parser, expr, dest, jump_if_null)
	parser_t *parser;
	expr_t *expr;
	int dest;
	int jump_if_null;
{
	vdbe_t *v = parser->pVdbe;
	int d2, addr, op = 0;

	if (v == 0 || expr == 0)
		return;
	switch(expr->op) {
	case TK_LT:       op = OP_Lt;       break;
	case TK_LE:       op = OP_Le;       break;
	case TK_GT:       op = OP_Gt;       break;
	case TK_GE:       op = OP_Ge;       break;
	case TK_NE:       op = OP_Ne;       break;
	case TK_EQ:       op = OP_Eq;       break;
	case TK_ISNULL:   op = OP_IsNull;   break;
	case TK_NOTNULL:  op = OP_NotNull;  break;
	default:  break;
	}
	switch(expr->op) {
	case TK_AND:
		d2 = __vdbe_make_label(v);
		__expr_if_false(parser, expr->pLeft, d2, !jump_if_null);
		__expr_if_true(parser, expr->pRight, dest, jump_if_null);
		__vdbe_resolve_label(v, d2);
		break;
	case TK_OR:
		__expr_if_true(parser, expr->pLeft, dest, jump_if_null);
		__expr_if_true(parser, expr->pRight, dest, jump_if_null);
		break;
	case TK_NOT:
		__expr_if_false(parser, expr->pLeft, dest, jump_if_null);
		break;
	case TK_LT: /* FALLTHROUGH */
	case TK_LE: /* FALLTHROUGH */
	case TK_GT: /* FALLTHROUGH */
	case TK_GE: /* FALLTHROUGH */
	case TK_NE: /* FALLTHROUGH */
	case TK_EQ:
		__expr_code(parser, expr->pLeft);
		__expr_code(parser, expr->pRight);
		if (__expr_type(expr) == DBSQL_SO_TEXT) {
			op += 6; /* Convert numeric opcodes to text opcodes */
		}
		__vdbe_add_op(v, op, jump_if_null, dest);
		break;
	case TK_ISNULL: /* FALLTHROUGH */
	case TK_NOTNULL:
		__expr_code(parser, expr->pLeft);
		__vdbe_add_op(v, op, 1, dest);
		break;
	case TK_IN:
		__expr_code(parser, expr->pLeft);
		addr = __vdbe_current_addr(v);
		__vdbe_add_op(v, OP_NotNull, -1, addr + 3);
		__vdbe_add_op(v, OP_Pop, 1, 0);
		__vdbe_add_op(v, OP_Goto, 0, jump_if_null ? dest : addr + 4);
		if (expr->pSelect) {
			__vdbe_add_op(v, OP_Found, expr->iTable, dest);
		} else {
			__vdbe_add_op(v, OP_SetFound, expr->iTable, dest);
		}
		break;
	case TK_BETWEEN:
		__expr_code(parser, expr->pLeft);
		__vdbe_add_op(v, OP_Dup, 0, 0);
		__expr_code(parser, expr->pList->a[0].pExpr);
		addr = __vdbe_add_op(v, OP_Lt, !jump_if_null, 0);
		__expr_code(parser, expr->pList->a[1].pExpr);
		__vdbe_add_op(v, OP_Le, jump_if_null, dest);
		__vdbe_add_op(v, OP_Integer, 0, 0);
		__vdbe_change_p2(v, addr, __vdbe_current_addr(v));
		__vdbe_add_op(v, OP_Pop, 1, 0);
		break;
	default:
		__expr_code(parser, expr);
		__vdbe_add_op(v, OP_If, jump_if_null, dest);
		break;
	}
}

/*
 * __expr_if_false --
 *	Generate code for a boolean expression such that a jump is made
 *	to the label "dest" if the expression is false but execution
 *	continues straight thru if the expression is true.
 *	If the expression evaluates to NULL (neither true nor false) then
 *	jump if jump_if_null is true or fall through if jump_if_null is false.
 *
 * PUBLIC: void __expr_if_false __P((parser_t *, expr_t *, int, int));
 */
void
__expr_if_false(parser, expr, dest, jump_if_null)
	parser_t *parser;
	expr_t *expr;
	int dest;
	int jump_if_null;
{
	vdbe_t *v = parser->pVdbe;
	int addr, d2, op = 0;

	if (v == 0 || expr == 0)
		return;

	switch(expr->op) {
	case TK_LT:       op = OP_Ge;       break;
	case TK_LE:       op = OP_Gt;       break;
	case TK_GT:       op = OP_Le;       break;
	case TK_GE:       op = OP_Lt;       break;
	case TK_NE:       op = OP_Eq;       break;
	case TK_EQ:       op = OP_Ne;       break;
	case TK_ISNULL:   op = OP_NotNull;  break;
	case TK_NOTNULL:  op = OP_IsNull;   break;
	default:  break;
	}

	switch(expr->op) {
	case TK_AND:
		__expr_if_false(parser, expr->pLeft, dest, jump_if_null);
		__expr_if_false(parser, expr->pRight, dest, jump_if_null);
		break;
	case TK_OR:
		d2 = __vdbe_make_label(v);
		__expr_if_true(parser, expr->pLeft, d2, !jump_if_null);
		__expr_if_false(parser, expr->pRight, dest, jump_if_null);
		__vdbe_resolve_label(v, d2);
		break;
	case TK_NOT:
		__expr_if_true(parser, expr->pLeft, dest, jump_if_null);
		break;
	case TK_LT: /* FALLTHROUGH */
	case TK_LE: /* FALLTHROUGH */
	case TK_GT: /* FALLTHROUGH */
	case TK_GE: /* FALLTHROUGH */
	case TK_NE: /* FALLTHROUGH */
	case TK_EQ:
		if (__expr_type(expr)==DBSQL_SO_TEXT) {
			/* 
			 *Convert numeric comparison opcodes into text
			 * comparison opcodes.  This step depends on the
			 * fact that the text comparision opcodes are
			 * always 6 greater than their corresponding
			 * numeric comparison opcodes.
			 */
			DBSQL_ASSERT(OP_Eq + 6 == OP_StrEq);
			op += 6;
		}
		__expr_code(parser, expr->pLeft);
		__expr_code(parser, expr->pRight);
		__vdbe_add_op(v, op, jump_if_null, dest);
		break;
	case TK_ISNULL: /* FALLTHROUGH */
	case TK_NOTNULL:
		__expr_code(parser, expr->pLeft);
		__vdbe_add_op(v, op, 1, dest);
		break;
	case TK_IN:
		__expr_code(parser, expr->pLeft);
		addr = __vdbe_current_addr(v);
		__vdbe_add_op(v, OP_NotNull, -1, addr + 3);
		__vdbe_add_op(v, OP_Pop, 1, 0);
		__vdbe_add_op(v, OP_Goto, 0, (jump_if_null ? dest : addr + 4));
		if (expr->pSelect) {
			__vdbe_add_op(v, OP_NotFound, expr->iTable, dest);
		} else {
			__vdbe_add_op(v, OP_SetNotFound, expr->iTable, dest);
		}
		break;
	case TK_BETWEEN:
		__expr_code(parser, expr->pLeft);
		__vdbe_add_op(v, OP_Dup, 0, 0);
		__expr_code(parser, expr->pList->a[0].pExpr);
		addr = __vdbe_current_addr(v);
		__vdbe_add_op(v, OP_Ge, !jump_if_null, addr + 3);
		__vdbe_add_op(v, OP_Pop, 1, 0);
		__vdbe_add_op(v, OP_Goto, 0, dest);
		__expr_code(parser, expr->pList->a[1].pExpr);
		__vdbe_add_op(v, OP_Gt, jump_if_null, dest);
		break;
	default:
		__expr_code(parser, expr);
		__vdbe_add_op(v, OP_IfNot, jump_if_null, dest);
		break;
	}
}

/*
 * __expr_compare --
 *	Do a deep comparison of two expression trees.  Return TRUE (non-zero)
 *	if they are identical and return FALSE if they differ in any way.
 *
 * PUBLIC: int __expr_compare __P((expr_t *, expr_t *));
 */
int
__expr_compare(a, b)
	expr_t *a;
	expr_t *b;
{
	int i;
	if (a == 0) {
		return (b == 0);
	} else if (b == 0) {
		return 0;
	}
	if (a->op != b->op)
		return 0;
	if (!__expr_compare(a->pLeft, b->pLeft))
		return 0;
	if (!__expr_compare(a->pRight, b->pRight))
		return 0;
	if (a->pList) {
		if (b->pList == 0)
			return 0;
		if (a->pList->nExpr != b->pList->nExpr)
			return 0;
		for(i = 0; i < a->pList->nExpr; i++) {
			if(!__expr_compare(a->pList->a[i].pExpr,
					   b->pList->a[i].pExpr)) {
				return 0;
			}
		}
	} else if (b->pList) {
		return 0;
	}
	if (a->pSelect || b->pSelect)
		return 0;
	if (a->iTable!=b->iTable || a->iColumn!=b->iColumn)
		return 0;
	if (a->token.z) {
		if (b->token.z == 0)
			return 0;
		if (b->token.n != a->token.n)
			return 0;
		if (strncasecmp(a->token.z,
					   b->token.z,
					   b->token.n) != 0)
			return 0;
	}
	return 1;
}

/*
 * __append_agg_info --
 *	Add a new element to the parser->aAgg[] array and return its index.
 *
 * STATIC: static int __append_agg_info __P((parser_t *));
 */
static int
__append_agg_info(parser)
	parser_t *parser;
{
	agg_expr_t *agg;
	if ((parser->nAgg & 0x7) == 0) {
		int amt = parser->nAgg + 8;
		if (__dbsql_realloc(parser->db, amt * sizeof(parser->aAgg[0]),
				 &parser->aAgg) == ENOMEM)
			return -1;
	}
	memset(&parser->aAgg[parser->nAgg], 0, sizeof(parser->aAgg[0]));
	return parser->nAgg++;
}

/*
 * __expr_analyze_aggregates --
 *	Analyze the given expression looking for aggregate functions and
 *	for variables that need to be added to the parser->aAgg[] array.
 *	Make additional entries to the parser->aAgg[] array as necessary.
 *	This routine should only be called after the expression has been
 *	analyzed by __expr_resolve_ids() and __expr_check().
 *	Return the number of errors.
 *
 * PUBLIC: int __expr_analyze_aggregates __P((parser_t *, expr_t *));
 */
int
__expr_analyze_aggregates(parser, expr)
	parser_t *parser;
	expr_t *expr;
{
	int i, n, nexpr, nerr = 0;
	agg_expr_t *agg;

	if (expr == 0)
		return 0;

	switch(expr->op) {
	case TK_COLUMN:
		agg = parser->aAgg;
		for (i = 0; i < parser->nAgg; i++) {
			if (agg[i].isAgg)
				continue;
			if (agg[i].pExpr->iTable == expr->iTable &&
			    agg[i].pExpr->iColumn==expr->iColumn) {
				break;
			}
		}
		if (i >= parser->nAgg) {
			i = __append_agg_info(parser);
			if (i < 0)
				return 1;
			parser->aAgg[i].isAgg = 0;
			parser->aAgg[i].pExpr = expr;
		}
		expr->iAgg = i;
		break;
	case TK_AGG_FUNCTION:
		agg = parser->aAgg;
		for(i = 0; i < parser->nAgg; i++) {
			if (!agg[i].isAgg)
				continue;
			if (__expr_compare(agg[i].pExpr, expr)) {
				break;
			}
		}
		if (i >= parser->nAgg) {
			i = __append_agg_info(parser);
			if (i < 0)
				return 1;
			parser->aAgg[i].isAgg = 1;
			parser->aAgg[i].pExpr = expr;
			nexpr = (expr->pList ? expr->pList->nExpr : 0);
			parser->aAgg[i].pFunc = __find_function(parser->db,
								expr->token.z,
								expr->token.n,
								nexpr, 0);
		}
		expr->iAgg = i;
		break;
	default:
		if (expr->pLeft) {
			nerr = __expr_analyze_aggregates(parser, expr->pLeft);
		}
		if (nerr == 0 && expr->pRight) {
			nerr = __expr_analyze_aggregates(parser, expr->pRight);
		}
		if (nerr == 0 && expr->pList) {
			n = expr->pList->nExpr;
			for (i = 0; nerr == 0 && i < n; i++) {
				nerr = __expr_analyze_aggregates(parser,
						expr->pList->a[i].pExpr);
			}
		}
		break;
	}
	return nerr;
}

/*
 * __find_function --
 *	Locate a user function given a name and a number of arguments.
 *	Return a pointer to the func_def_t structure that defines that
 *	function, or return NULL if the function does not exist.
 *	If the 'create' argument is true, then a new (blank) func_def_t
 *	structure is created and liked into the 'dbp' structure if a
 *	no matching function previously existed.  When 'create' is true
 *	and the 'nargs' parameter is -1, then only a function that accepts
 *	any number of arguments will be returned.
 *	If 'create' is false and 'nargs' is -1, then the first valid
 *	function found is returned.  A function is valid if either 'func'
 *	or 'step' is non-zero.
 *
 * PUBLIC: func_def_t *__find_function __P((DBSQL *, const char *, int,
 * PUBLIC:                             int, int));
 *
 * dbp				An open database.
 * name				Name of the function.  Not null-terminated.
 * len				Number of characters in the name.
 * nargs			Number of arguments.  -1 means any number.
 * create			Create new entry if true and does not otherwise
 *				exist.
 */
func_def_t *
__find_function(dbp, name, len, nargs, create)
	DBSQL *dbp;
	const char *name;
	int len;
	int nargs;
	int create;
{
	func_def_t *first, *p, *maybe;
	first = p = (func_def_t *)__hash_find((hash_t*)dbp->aFunc, name, len);
	if (p && !create && nargs < 0) {
		while (p && p->xFunc==0 && p->xStep == 0) {
			p = p->pNext;
		}
		return p;
	}
	maybe = 0;
	while(p && p->nArg != nargs) {
		if (p->nArg < 0 && !create && (p->xFunc || p->xStep))
			maybe = p;
		p = p->pNext;
	}
	if (p && !create && p->xFunc == 0 && p->xStep == 0) {
		return 0;
	}
	if (p == 0 && maybe) {
		DBSQL_ASSERT(create == 0);
		return maybe;
	}
	if (p == 0 && create &&
	    __dbsql_calloc(dbp, 1, sizeof(*p), &p) != ENOMEM) {
		p->nArg = nargs;
		p->pNext = first;
		p->dataType = first ? first->dataType : DBSQL_NUMERIC;
		__hash_insert((hash_t*)dbp->aFunc, name, len, (void*)p);
	}
	return p;
}
