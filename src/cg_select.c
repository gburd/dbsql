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
 * This file contains C code routines that are called by the parser
 * to handle SELECT statements.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/* __select_new --
 *	Allocate a new Select structure and return a pointer to that
 *	structure.
 *
 * PUBLIC: select_t *__select_new __P((expr_list_t *, src_list_t *, expr_t *,
 * PUBLIC:                        expr_list_t *, expr_t *, expr_list_t *,
 * PUBLIC:                        int, int, int));
 *
 * result_cols			Which columns to include in the result
 * from_clause			The FROM clause -- which tables to scan
 * where_clause			The WHERE clause
 * groupby_clause		The GROUP BY clause
 * having_clause		The HAVING clause
 * orderby_clause		The ORDER BY clause
 * distinct_p			True if the DISTINCT keyword is present
 * limit			LIMIT value, -1 means not used
 * offset			OFFSET value, 0 means no offset
 */
select_t *
__select_new(result_cols, from_clause, where_clause, groupby_clause,
	     having_clause, orderby_clause, distinct_p, limit, offset)
	expr_list_t *result_cols;
	src_list_t *from_clause;
	expr_t *where_clause;
	expr_list_t *groupby_clause;
	expr_t *having_clause;
	expr_list_t *orderby_clause;
	int distinct_p;
	int limit;
	int offset;
{
	select_t *new;
	if (__dbsql_calloc(NULL, 1, sizeof(*new), &new) == ENOMEM) {
		__expr_list_delete(result_cols);
		__src_list_delete(from_clause);
		__expr_delete(where_clause);
		__expr_list_delete(groupby_clause);
		__expr_delete(having_clause);
		__expr_list_delete(orderby_clause);
	} else {
		if (result_cols == 0) {
			result_cols = __expr_list_append(0,
						 __expr(TK_ALL, 0, 0, 0), 0);
		}
		new->pEList = result_cols;
		new->pSrc = from_clause;
		new->pWhere = where_clause;
		new->pGroupBy = groupby_clause;
		new->pHaving = having_clause;
		new->pOrderBy = orderby_clause;
		new->isDistinct = distinct_p;
		new->op = TK_SELECT;
		new->nLimit = limit;
		new->nOffset = offset;
		new->iLimit = -1;
		new->iOffset = -1;
	}
	return new;
}

/*
 * __join_type
 *	Given 1 to 3 identifiers preceeding the JOIN keyword, determine the
 *	type of join.  Return an integer constant that expresses that type
 *	in terms of the following bit values:
 *
 *     JT_INNER
 *     JT_OUTER
 *     JT_NATURAL
 *     JT_LEFT
 *     JT_RIGHT
 *
 * A full outer join is the combination of JT_LEFT and JT_RIGHT.
 *
 * If an illegal or unsupported join type is seen, then still return
 * a join type, but put an error in the parser structure.
 *
 * PUBLIC: int __join_type __P((parser_t *, token_t *, token_t *, token_t *));
 */
int
__join_type(parser, a, b, c)
	parser_t *parser;
	token_t *a;
	token_t *b;
	token_t *c;
{
	int jointype = 0;
	token_t *ap_all[3];
	token_t *p;
	static struct {
		const char *keyword;
		int len;
		int code;
	} keywords[] = {
		{ "natural", 7, JT_NATURAL },
		{ "left",    4, JT_LEFT|JT_OUTER },
		{ "right",   5, JT_RIGHT|JT_OUTER },
		{ "full",    4, JT_LEFT|JT_RIGHT|JT_OUTER },
		{ "outer",   5, JT_OUTER },
		{ "inner",   5, JT_INNER },
		{ "cross",   5, JT_INNER },
	};
	static int num_keywords = sizeof(keywords) / sizeof(keywords[0]);
	static token_t dummy = { 0, 0 };
	char *sp1 = " ", *sp2 = " ";
	int i, j;

	ap_all[0] = a;
	ap_all[1] = b;
	ap_all[2] = c;
	for (i = 0; i < 3 && ap_all[i]; i++) {
		p = ap_all[i];
		for (j = 0; j < num_keywords; j++) {
			if (p->n == keywords[j].len &&
			    strncasecmp(p->z, keywords[j].keyword,
						   p->n) == 0) {
				jointype |= keywords[j].code;
				break;
			}
		}
		if (j >= num_keywords) {
			jointype |= JT_ERROR;
			break;
		}
	}
	if(((jointype & (JT_INNER | JT_OUTER)) == (JT_INNER | JT_OUTER)) ||
	   (jointype & JT_ERROR) != 0) {
		if (b == 0) {
			b = &dummy;
			sp1 = 0;
		}
		if (c == 0) {
			c = &dummy;
			sp2 = 0;
		}
		__str_nappend(&parser->zErrMsg,
			      "unknown or unsupported join type: ", 0,
			      a->z, a->n, sp1, 1, b->z, b->n, sp2, 1, c->z,
			      c->n, NULL);
		parser->nErr++;
		jointype = JT_INNER;
	} else if (jointype & JT_RIGHT) {
		__error_msg(parser, 
		    "RIGHT and FULL OUTER JOINs are not currently supported");
		jointype = JT_INNER;
	}
	return jointype;
}

/*
 * __column_index --
 *	Return the index of a column in a table.  Return -1 if the column
 *	is not contained in the table.
 *
 * STATIC: static int __column_index __P((table_t *, const char *));
 */
static int
__column_index(table, col)
	table_t *table;
	const char *col;
{
	int i;
	for (i = 0; i < table->nCol; i++) {
		if (strcasecmp(table->aCol[i].zName, col) == 0)
			return i;
	}
	return -1;
}

/*
 * __add_where_term --
 *	Add a term to the WHERE expression in *expr that requires the
 *	col column to be equal in the two tables table1 and table2.
 *
 * STATIC: static void __add_where_term __P((const char *, const table_t *,
 * STATIC:                              const table_t *, expr_t **));
 *
 * col				Name of the column
 * table1			First table
 * table2			Second table
 * expr				Add the equality term to this expression
 */
static void
__add_where_term(col, table1, table2, expr)
	const char *col;
	const table_t *table1;
	const table_t *table2;
	expr_t **expr;
{
	token_t dummy;
	expr_t *e_1a, *e_1b, *e_1c;
	expr_t *e_2a, *e_2b, *e_2c;
	expr_t *e;

	dummy.z = col;
	dummy.n = strlen(col);
	dummy.dyn = 0;
	e_1a = __expr(TK_ID, 0, 0, &dummy);
	e_2a = __expr(TK_ID, 0, 0, &dummy);
	dummy.z = table1->zName;
	dummy.n = strlen(dummy.z);
	e_1b = __expr(TK_ID, 0, 0, &dummy);
	dummy.z = table2->zName;
	dummy.n = strlen(dummy.z);
	e_2b = __expr(TK_ID, 0, 0, &dummy);
	e_1c = __expr(TK_DOT, e_1b, e_1a, 0);
	e_2c = __expr(TK_DOT, e_2b, e_2a, 0);
	e = __expr(TK_EQ, e_1c, e_2c, 0);
	ExprSetProperty(e, EP_FromJoin);
	if (*expr) {
		*expr = __expr(TK_AND, *expr, e, 0);
	} else {
		*expr = e;
	}
}

/*
 * __set_join_expr --
 *	Set the EP_FromJoin property on all terms of the given expression.
 *
 *	The EP_FromJoin property is used on terms of an expression to tell
 *	the LEFT OUTER JOIN processing logic that this term is part of the
 *	join restriction specified in the ON or USING clause and not a part
 *	of the more general WHERE clause.  These terms are moved over to the
 *	WHERE clause during join processing but we need to remember that they
 *	originated in the ON or USING clause.
 *
 * STATIC: static void __set_join_expr __P((expr_t *));
 */
static void
__set_join_expr(p)
	expr_t *p;
{
	while (p) {
		ExprSetProperty(p, EP_FromJoin);
		__set_join_expr(p->pLeft);
		p = p->pRight;
	} 
}

/*
 * __process_join --
 *	This routine processes the join information for a SELECT statement.
 *	ON and USING clauses are converted into extra terms of the WHERE
 *	clause.  NATURAL joins also create extra WHERE clause terms.
 *
 *	This routine returns the number of errors encountered.
 *
 * STATIC: static int __process_join __P((parser_t *, select_t *));
 */
static int
__process_join(parser, select)
	parser_t *parser;
	select_t *select;
{
	int i, j;
	src_list_t *src;
	struct src_list_item *term;
	struct src_list_item *other;
	table_t *table;
	id_list_t *list;

	src = select->pSrc;
	for (i = 0; i < (src->nSrc - 1); i++) {
		term = &src->a[i];
		other = &src->a[i+1];

		if (term->pTab == 0 || other->pTab == 0)
			continue;

		/*
		 * When the NATURAL keyword is present, add WHERE clause
		 * terms for every column that the two tables have in common.
		 */
		if (term->jointype & JT_NATURAL) {
			if (term->pOn || term->pUsing) {
				__error_msg(parser,
					    "a NATURAL join may not have "
					    "an ON or USING clause", 0);
				return 1;
			}
			table = term->pTab;
			for (j = 0; j < table->nCol; j++) {
				if (__column_index(other->pTab,
						  table->aCol[j].zName) >= 0) {
					__add_where_term(table->aCol[j].zName,
							 table, other->pTab,
							 &select->pWhere);
				}
			}
		}

		/*
		 * Disallow both ON and USING clauses in the same join.
		 */
		if (term->pOn && term->pUsing) {
			__error_msg(parser, "cannot have both ON and USING "
				    "clauses in the same join");
			return 1;
		}

		/*
		 * Add the ON clause to the end of the WHERE clause,
		 * connected by and AND operator.
		 */
		if (term->pOn) {
			__set_join_expr(term->pOn);
			if (select->pWhere == 0) {
				select->pWhere = term->pOn;
			} else {
				select->pWhere = __expr(TK_AND, select->pWhere,
							term->pOn, 0);
			}
			term->pOn = 0;
		}

		/*
		 * Create extra terms on the WHERE clause for each column named
		 * in the USING clause.  Example: If the two tables to be
		 * joined are A and B and the USING clause names X, Y, and Z,
		 * then add this to the WHERE clause:
		 *    A.X=B.X AND A.Y=B.Y AND A.Z=B.Z
		 * Report an error if any column mentioned in the USING clause
		 * is not contained in both tables to be joined.
		 */
		if (term->pUsing) {
			DBSQL_ASSERT(i < (src->nSrc - 1));
			list = term->pUsing;
			for (j = 0; j < list->nId; j++) {
				if (__column_index(term->pTab,
						   list->a[j].zName) < 0 ||
				    __column_index(other->pTab,
						   list->a[j].zName) < 0) {
					__error_msg(parser,
				        "cannot join using column %s - column "
				        "not present in both tables",
					list->a[j].zName);
					return 1;
				}
				__add_where_term(list->a[j].zName, term->pTab,
						 other->pTab, &select->pWhere);
			}
		}
	}
	return 0;
}

/*
 * __select_delete --
 *	Delete the given Select structure and all of its substructures.
 *
 * PUBLIC: void __select_delete __P((select_t *));
 */
void
__select_delete(select)
	select_t *select;
{
	if (select == 0)
		return;
	__expr_list_delete(select->pEList);
	__src_list_delete(select->pSrc);
	__expr_delete(select->pWhere);
	__expr_list_delete(select->pGroupBy);
	__expr_delete(select->pHaving);
	__expr_list_delete(select->pOrderBy);
	__select_delete(select->pPrior);
	__dbsql_free(NULL, select->zSelect);
	__dbsql_free(NULL, select);
}

/*
 * __aggregate_info_reset --
 *	Delete the aggregate information from the parse structure.
 *
 * STATIC: static void __aggregage_info_reset __P((parser_t *));
 */
static void
__aggregate_info_reset(parser)
	parser_t *parser;
{
	__dbsql_free(parser->db, parser->aAgg);
	parser->aAgg = 0;
	parser->nAgg = 0;
	parser->useAgg = 0;
}

/*
 * __push_onto_sorter --
 *	Insert code into "v" that will push the record on the top of the
 *	stack into the sorter.
 *
 * STATIC: static void __push_onto_sorter __P((parser_t *, vdbe_t *,
 * STATIC:                                expr_list_t *));
 */
static void
__push_onto_sorter(parser, v, orderby_clause)
	parser_t *parser;
	vdbe_t *v;
	expr_list_t *orderby_clause;
{
	int i, order, type, c;
	char *sort_order;

	if (__dbsql_calloc(parser->db, 1, orderby_clause->nExpr + 1,
			&sort_order) == ENOMEM)
		return;
	for (i = 0; i < orderby_clause->nExpr; i++) {
		order = orderby_clause->a[i].sortOrder;
		if ((order & DBSQL_SO_TYPEMASK) == DBSQL_SO_TEXT) {
			type = DBSQL_SO_TEXT;
		} else if ((order & DBSQL_SO_TYPEMASK) == DBSQL_SO_NUM) {
			type = DBSQL_SO_NUM;
		} else {
			type = __expr_type(orderby_clause->a[i].pExpr);
		}
		if ((order & DBSQL_SO_DIRMASK) == DBSQL_SO_ASC) {
			c = (type == DBSQL_SO_TEXT ? 'A' : '+');
		} else {
			c = (type == DBSQL_SO_TEXT ? 'D' : '-');
		}
		sort_order[i] = c;
		__expr_code(parser, orderby_clause->a[i].pExpr);
	}
	sort_order[orderby_clause->nExpr] = 0;
	__vdbe_add_op(v, OP_SortMakeKey, orderby_clause->nExpr, 0);
	__vdbe_change_p3(v, -1, sort_order, strlen(sort_order));
	__dbsql_free(parser->db, sort_order);
	__vdbe_add_op(v, OP_SortPut, 0, 0);
}

/*
 * __add_key_type --
 *	This routine adds a P3 argument to the last VDBE opcode that was
 *	inserted. The P3 argument added is a string suitable for the 
 *	OP_MakeKey or OP_MakeIdxKey opcodes.  The string consists of
 *	characters 't' or 'n' depending on whether or not the various
 *	fields of the key to be generated should be treated as numeric
 *	or as text.  See the OP_MakeKey and OP_MakeIdxKey opcode
 *	documentation for additional information about the P3 string.
 *	See also the __add_idx_key_type() routine.
 *
 * PUBLIC: void __add_key_type __P((vdbe_t *, expr_list_t *));
 */
void
__add_key_type(v, elist)
	vdbe_t *v;
	expr_list_t *elist;
{
	int i;
	int col = elist->nExpr;
	char *type;
	if (__dbsql_calloc(NULL, 1, col + 1, &type) == ENOMEM)
		return;
	for (i = 0; i < col; i++) {
		type[i] = (__expr_type(elist->a[i].pExpr) == DBSQL_SO_NUM) ?
			'n' : 't';
	}
	type[i] = 0;
	__vdbe_change_p3(v, -1, type, col);
	__dbsql_free(NULL, type);
}

/*
 * __select_inner_loop --
 *	This routine generates the code for the inside of the inner loop
 *	of a SELECT.
 *
 *	If src_table and num_cols are both zero, then the elist expressions
 *	are evaluated in order to get the data for this row.  If num_cols>0
 *	then data is pulled from src_table and elist is used only to get the
 *	datatypes for each column.
 *
 * STATIC: static int __select_inner_loop __P((parser_t *, select_t *,
 * STATIC:                                expr_list_t *, int, int,
 * STATIC:                                expr_list_t *, int, int, int, int,
 * STATIC:                                int));
 *
 * parser			The parser context
 * select			The complete select statement being coded
 * elist			List of values being extracted
 * src_table			Pull data from this table
 * num_cols			Number of columns in the source table
 * orderby_clause		If not NULL, sort results using this key
 * distinct			If >=0, make sure results are distinct
 * dest				How to dispose of the results
 * param			An argument to the disposal method
 * cont				Jump here to continue with next row
 * brk				Jump here to break out of the inner loop
 */
static int
__select_inner_loop(parser, select, elist, src_table, num_cols, orderby_clause,
		    distinct, dest, param, cont, brk)
	parser_t *parser;
	select_t *select;
	expr_list_t *elist;
	int src_table;
	int num_cols;
	expr_list_t *orderby_clause;
	int distinct;
	int dest;
	int param;
	int cont;
	int brk;
{
	int i, addr, addr1, addr2;
	vdbe_t *v = parser->pVdbe;

	if (v == 0)
		return 0;
	DBSQL_ASSERT(elist != 0);

	/*
	 * If there was a LIMIT clause on the SELECT statement, then do the
	 * check to see if this row should be output.
	 */
	if (orderby_clause == 0) {
		if (select->iOffset >= 0) {
			int addr = __vdbe_current_addr(v);
			__vdbe_add_op(v, OP_MemIncr, select->iOffset,
				      (addr + 2));
			__vdbe_add_op(v, OP_Goto, 0, cont);
		}
		if (select->iLimit >= 0) {
			__vdbe_add_op(v, OP_MemIncr, select->iLimit, brk);
		}
	}

	/*
	 * Pull the requested columns.
	 */
	if (num_cols > 0) {
		for (i = 0; i < num_cols; i++) {
			__vdbe_add_op(v, OP_Column, src_table, i);
		}
	} else {
		num_cols = elist->nExpr;
		for (i = 0; i < elist->nExpr; i++) {
			__expr_code(parser, elist->a[i].pExpr);
		}
	}

	/*
	 * If the DISTINCT keyword was present on the SELECT statement
	 * and this row has been seen before, then do not make this row
	 * part of the result.
	 */
	if (distinct >= 0 && elist && elist->nExpr > 0) {
#if NULL_ALWAYS_DISTINCT
		__vdbe_add_op(v, OP_IsNull, (- elist->nExpr),
			      (__vdbe_current_addr(v) + 7));
#endif
		__vdbe_add_op(v, OP_MakeKey, elist->nExpr, 1);
		__add_key_type(v, elist);
		__vdbe_add_op(v, OP_Distinct, distinct,
			      (__vdbe_current_addr(v) + 3));
		__vdbe_add_op(v, OP_Pop, (elist->nExpr + 1), 0);
		__vdbe_add_op(v, OP_Goto, 0, cont);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_add_op(v, OP_PutStrKey, distinct, 0);
	}

	switch(dest) {
		/*
		 * In this mode, write each query result to the key of the
		 * temporary table param.
		 */
	case SRT_Union:
		__vdbe_add_op(v, OP_MakeRecord, num_cols,
			      NULL_ALWAYS_DISTINCT);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_add_op(v, OP_PutStrKey, param, 0);
		break;
		/*
		 * Store the result as data using a unique key.
		 */
	case SRT_Table: /* FALLTHROUGH */
	case SRT_TempTable:
		__vdbe_add_op(v, OP_MakeRecord, num_cols, 0);
		if (orderby_clause) {
			__push_onto_sorter(parser, v, orderby_clause);
		} else {
			__vdbe_add_op(v, OP_NewRecno, param, 0);
			__vdbe_add_op(v, OP_Pull, 1, 0);
			__vdbe_add_op(v, OP_PutIntKey, param, 0);
		}
		break;

		/*
		 * Construct a record from the query result, but instead of
		 * saving that record, use it as a key to delete elements from
		 * the temporary table param.
		 */
	case SRT_Except:
		addr = __vdbe_add_op(v, OP_MakeRecord, num_cols,
				     NULL_ALWAYS_DISTINCT);
		__vdbe_add_op(v, OP_NotFound, param, (addr + 3));
		__vdbe_add_op(v, OP_Delete, param, 0);
		break;

		/*
		 * If we are creating a set for an "expr IN (SELECT ...)"
		 * construct, then there should be a single item on the
		 * stack.  Write this item into the set table with bogus data.
		 */
	case SRT_Set:
		addr1 = __vdbe_current_addr(v);
		DBSQL_ASSERT(num_cols == 1);
		__vdbe_add_op(v, OP_NotNull, -1, (addr1 + 3));
		__vdbe_add_op(v, OP_Pop, 1, 0);
		addr2 = __vdbe_add_op(v, OP_Goto, 0, 0);
		if (orderby_clause) {
			__push_onto_sorter(parser, v, orderby_clause);
		} else {
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_add_op(v, OP_PutStrKey, param, 0);
		}
		__vdbe_change_p2(v, addr2, __vdbe_current_addr(v));
		break;

		/*
		 * If this is a scalar select that is part of an expression,
		 * then store the results in the appropriate memory cell and
		 * break out of the scan loop.
		 */
	case SRT_Mem:
		DBSQL_ASSERT(num_cols == 1);
		if (orderby_clause) {
			__push_onto_sorter(parser, v, orderby_clause);
		} else {
			__vdbe_add_op(v, OP_MemStore, param, 1);
			__vdbe_add_op(v, OP_Goto, 0, brk);
		}
		break;

	/*
	 * Send the data to the callback function.
	 */
	case SRT_Callback: /* FALLTHROUGH */
	case SRT_Sorter:
		if (orderby_clause) {
			__vdbe_add_op(v, OP_SortMakeRec, num_cols, 0);
			__push_onto_sorter(parser, v, orderby_clause);
		} else {
			DBSQL_ASSERT(dest == SRT_Callback);
			__vdbe_add_op(v, OP_Callback, num_cols, 0);
		}
		break;

		/*
		 * Invoke a subroutine to handle the results.  The subroutine
		 * itself is responsible for popping the results off of the
		 * stack.
		 */
	case SRT_Subroutine:
		if (orderby_clause) {
			__vdbe_add_op(v, OP_MakeRecord, num_cols, 0);
			__push_onto_sorter(parser, v, orderby_clause);
		} else {
			__vdbe_add_op(v, OP_Gosub, 0, param);
		}
		break;

		/*
		 * Discard the results.  This is used for SELECT statements
		 * inside the body of a TRIGGER.  The purpose of such selects
		 * is to call user-defined functions that have side effects.
		 * We do not care about the actual results of the select.
		 */
	default:
		DBSQL_ASSERT(dest == SRT_Discard);
		__vdbe_add_op(v, OP_Pop, num_cols, 0);
		break;
	}
	return 0;
}

/*
 * __generate_sort_tail --
 *	If the inner loop was generated using a non-null orderby_clause
 *	argument, then the results were placed in a sorter.  After the
 *	loop is terminated we need to run the sorter and output the results.
 *	The following routine generates the code needed to do that.
 *
 * STATIC: static void __generate_sort_tail __P((select_t *, vdbe_t *, int,
 * STATIC:                                  int, int));
 *
 * select			The SELECT statement
 * v				Generate code into this VDBE
 * num_cols			Number of columns of data
 * dest				Write the sorted results here
 * param			Optional parameter associated with dest
 */
static void
__generate_sort_tail(select, v, num_cols, dest, param)
	select_t *select;
	vdbe_t *v;
	int num_cols;
	int dest;
	int param;
{
	int i, addr;
	int end = __vdbe_make_label(v);
	if (dest == SRT_Sorter)
		return;
	__vdbe_add_op(v, OP_Sort, 0, 0);
	addr = __vdbe_add_op(v, OP_SortNext, 0, end);
	if (select->iOffset >= 0) {
		__vdbe_add_op(v, OP_MemIncr, select->iOffset, (addr + 4));
		__vdbe_add_op(v, OP_Pop, 1, 0);
		__vdbe_add_op(v, OP_Goto, 0, addr);
	}
	if (select->iLimit >= 0) {
		__vdbe_add_op(v, OP_MemIncr, select->iLimit, end);
	}
	switch(dest) {
	case SRT_Callback:
		__vdbe_add_op(v, OP_SortCallback, num_cols, 0);
		break;
	case SRT_Table: /* FALLTHROUGH */
	case SRT_TempTable:
		__vdbe_add_op(v, OP_NewRecno, param, 0);
		__vdbe_add_op(v, OP_Pull, 1, 0);
		__vdbe_add_op(v, OP_PutIntKey, param, 0);
		break;
	case SRT_Set:
		DBSQL_ASSERT(num_cols == 1);
		__vdbe_add_op(v, OP_NotNull, -1, (__vdbe_current_addr(v) + 3));
		__vdbe_add_op(v, OP_Pop, 1, 0);
		__vdbe_add_op(v, OP_Goto, 0, (__vdbe_current_addr(v) + 3));
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_add_op(v, OP_PutStrKey, param, 0);
		break;
	case SRT_Mem:
		DBSQL_ASSERT(num_cols == 1);
		__vdbe_add_op(v, OP_MemStore, param, 1);
		__vdbe_add_op(v, OP_Goto, 0, end);
		break;
	case SRT_Subroutine:
		for (i = 0; i < num_cols; i++) {
			__vdbe_add_op(v, OP_Column, (-1 - i), i);
		}
		__vdbe_add_op(v, OP_Gosub, 0, param);
		__vdbe_add_op(v, OP_Pop, 1, 0);
		break;
	default:
		/* Do nothing. */
		break;
	}
	__vdbe_add_op(v, OP_Goto, 0, addr);
	__vdbe_resolve_label(v, end);
	__vdbe_add_op(v, OP_SortReset, 0, 0);
}

/*
 * __generate_column_types --
 *	Generate code that will tell the VDBE the datatypes of
 *	columns in the result set.
 *
 *	This routine only generates code if the "PRAGMA show_datatypes=on"
 *	has been executed.  The datatypes are reported out in the azCol
 *	parameter to the callback function.  The first N azCol[] entries
 *	are the names of the columns, and the second N entries are the
 *	datatypes for the columns.
 *
 *	The "datatype" for a result that is a column of a type is the
 *	datatype definition extracted from the CREATE TABLE statement.
 *	The datatype for an expression is either TEXT or NUMERIC.  The
 *	datatype for a ROWID field is INTEGER.
 *
 * STATIC: static void __generate_column_types __P((parser_t *, src_list_t *,
 * STATIC:                                     expr_list_t *));
 *
 * parser			Parser context
 * tables			List of tables
 * elist			Expressions defining the result set
 */
static void
__generate_column_types(parser, tables, elist)
	parser_t *parser;
	src_list_t *tables;
	expr_list_t *elist;
{
	int i, j, col;
	expr_t *p;
	char *type;
	table_t *table;
	vdbe_t *v = parser->pVdbe;
	if (parser->useCallback &&
	    (parser->db->flags & DBSQL_ReportTypes) == 0) {
		return;
	}
	for (i = 0; i < elist->nExpr; i++) {
		p = elist->a[i].pExpr;
		type = 0;
		if (p == 0)
			continue;
		if (p->op == TK_COLUMN && tables) {
			col = p->iColumn;
			j = 0;
			while (j < tables->nSrc &&
			       tables->a[j].iCursor != p->iTable) {
				j++;
			}
			DBSQL_ASSERT(j < tables->nSrc);
			table = tables->a[j].pTab;
			if (col < 0)
				col = table->iPKey;
			DBSQL_ASSERT(col == -1 || (col >= 0 && col < table->nCol));
			if (col < 0) {
				type = "INTEGER";
			} else {
				type = table->aCol[col].zType;
			}
		} else {
			if (__expr_type(p) == DBSQL_SO_TEXT) {
				type = "TEXT";
			} else {
				type = "NUMERIC";
			}
		}
		__vdbe_add_op(v, OP_ColumnName, (i + elist->nExpr), 0);
		__vdbe_change_p3(v, -1, type, P3_STATIC);
	}
}

/*
 * __generate_column_names --
 *	Generate code that will tell the VDBE the names of columns
 *	in the result set.  This information is used to provide the
 *	azCol[] values in the callback.
 *
 * STATIC: static void __generate_column_names __P((parser_t *, src_list_t *,
 * STATIC:                                     expr_list_t *));
 *
 * parser			Parser context
 * tables			List of tables
 * elist			Expressions defining the result set
 */
static void
__generate_column_names(parser, tables, elist)
	parser_t *parser;
	src_list_t *tables;
	expr_list_t *elist;
{
	int i, j, show_full_names, icol, addr;
	char *type, *name, *col, *tab;
	char namebuf[30]; /* TODO consider eliminating this */
	expr_t *p;
	table_t *table;
	vdbe_t *v = parser->pVdbe;
	if (parser->colNamesSet || v == 0 || parser->rc == ENOMEM)
		return;
	parser->colNamesSet = 1;
	for (i = 0; i < elist->nExpr; i++) {
		type = 0;
		p = elist->a[i].pExpr;
		if (p == 0)
			continue;
		if (elist->a[i].zName) {
			name = elist->a[i].zName;
			__vdbe_add_op(v, OP_ColumnName, i, 0);
			__vdbe_change_p3(v, -1, name, strlen(name));
			continue;
		}
		show_full_names = (parser->db->flags & DBSQL_FullColNames) !=0;
		if (p->op == TK_COLUMN && tables) {
			icol = p->iColumn;
			j = 0;
			while (j < tables->nSrc &&
			       tables->a[j].iCursor != p->iTable) {
				j++;
			}
			DBSQL_ASSERT(j < tables->nSrc);
			table = tables->a[j].pTab;
			if (icol < 0)
				icol = table->iPKey;
			DBSQL_ASSERT(icol == -1 ||(icol >= 0 && icol < table->nCol));
			if (icol < 0) {
				col = "_ROWID_";
				type = "INTEGER";
			} else {
				col = table->aCol[icol].zName;
				type = table->aCol[icol].zType;
			}
			if (p->span.z && p->span.z[0] && !show_full_names) {
				addr = __vdbe_add_op(v,OP_ColumnName, i, 0);
				__vdbe_change_p3(v, -1, p->span.z, p->span.n);
				__vdbe_compress_space(v, addr);
			} else if (tables->nSrc > 1 || show_full_names) {
				name = 0;
				tab = 0;
				tab = tables->a[j].zAlias;
				if (show_full_names || tab == 0)
					tab = table->zName;
				__str_append(&name, tab, ".", col, 0);
				__vdbe_add_op(v, OP_ColumnName, i, 0);
				__vdbe_change_p3(v, -1, name, strlen(name));
				__dbsql_free(NULL, name);
			} else {
				__vdbe_add_op(v, OP_ColumnName, i, 0);
				__vdbe_change_p3(v, -1, col, 0);
			}
		} else if (p->span.z && p->span.z[0]) {
			addr = __vdbe_add_op(v, OP_ColumnName, i, 0);
			__vdbe_change_p3(v, -1, p->span.z, p->span.n);
			__vdbe_compress_space(v, addr);
		} else {
			DBSQL_ASSERT( p->op!=TK_COLUMN || tables==0 );
			sprintf(namebuf, "column%d", i+1);
			__vdbe_add_op(v, OP_ColumnName, i, 0);
			__vdbe_change_p3(v, -1, namebuf, strlen(namebuf));
		}
	}
}

/*
 * __select_op_name --
 *	Name of the connection operator, used for error messages.
 *
 * STATIC: static const char *__select_op_name __P((int));
 */
static const char *
__select_op_name(id)
	int id;
{
	char *z;
	switch(id) {
	case TK_ALL:       z = "UNION ALL";   break;
	case TK_INTERSECT: z = "INTERSECT";   break;
	case TK_EXCEPT:    z = "EXCEPT";      break;
	default:           z = "UNION";       break;
	}
	return z;
}

/*
 * __fill_in_column_list --
 *	For the given SELECT statement, do three things.
 *
 *    (1)  Fill in the tables->a[].pTab fields in the SrcList that 
 *         defines the set of tables that should be scanned.  For views,
 *         fill tables->a[].pSelect with a copy of the SELECT statement
 *         that implements the view.  A copy is made of the view's SELECT
 *         statement so that we can freely modify or delete that statement
 *         without worrying about messing up the presistent representation
 *         of the view.
 *
 *    (2)  Add terms to the WHERE clause to accomodate the NATURAL keyword
 *         on joins and the ON and USING clause of joins.
 *
 *    (3)  Scan the list of columns in the result set (pEList) looking
 *         for instances of the "*" operator or the TABLE.* operator.
 *         If found, expand each "*" to be every column in every table
 *         and TABLE.* to be every column in TABLE.
 *
 *	Return 0 on success.  If there are problems, leave an error message
 *	in parser and return non-zero.
 *
 * STATIC: static int __file_in_column_list __P((parser_t *, select_t *));
 */
static int
__fill_in_column_list(parser, select)
	parser_t *parser;
	select_t *select;
{
	int i, j, k, rc;
	src_list_t *tables;
	expr_list_t *elist;
	table_t *table;
	expr_t *e;
	struct expr_list_item *a;
	expr_list_t *new;
	int table_seen;				
        token_t *name;
	table_t *tab;
	char *tab_name, *jname;
	expr_t *expr, *left, *right;
	char fake_name[60]; /* TODO consider replacing this */

	if (select == 0 || select->pSrc == 0)
		return 1;
	tables = select->pSrc;
	elist = select->pEList;

	/*
	 * Look up every table in the table list.
	 */
	for(i = 0; i < tables->nSrc; i++) {
		if (tables->a[i].pTab) {
			/* This routine has run before! No need to continue. */
			return 0;
		}
		if (tables->a[i].zName == 0) {
			/* A sub-query in the FROM clause of a SELECT. */
			DBSQL_ASSERT(tables->a[i].pSelect != 0);
			if (tables->a[i].zAlias == 0) {
				sprintf(fake_name, "___subquery_%p_",
					(void*)tables->a[i].pSelect);
				__str_append(&tables->a[i].zAlias,
					     fake_name, 0);
			}
			tables->a[i].pTab = table = 
				__select_result_set(parser,
						    tables->a[i].zAlias,
						    tables->a[i].pSelect);
			if (table == 0) {
				return 1;
			}
			/*
			 * The isTransient flag indicates that the table_t
			 * structure has been dynamically allocated and may
			 * be freed at any time.  In other words, table is
			 * not pointing to a persistent table structure that
			 * defines part of the schema.
			 */
			table->isTransient = 1;
		} else {
			/* An ordinary table or view name in the FROM clause.*/
			tables->a[i].pTab = table = 
				__locate_table(parser, tables->a[i].zName,
					       tables->a[i].zDatabase);
			if (table == 0) {
				return 1;
			}
			if (table->pSelect) {
				/*
				 * We reach here if the named table is a
				 * really a view.
				 */
				if (__view_get_column_names(parser, table)) {
					return 1;
				}
				/*
				 * If tables->a[i].pSelect!=0 it means we are
				 * dealing with a view within a view.  The
				 * SELECT structure has already been copied by
				 * the outer view so we can skip the copy step
				 * here in the inner view.
				 */
				if (tables->a[i].pSelect == 0) {
					tables->a[i].pSelect =
						__select_dup(table->pSelect);
				}
			}
		}
	}

	/*
	 * Process NATURAL keywords, and ON and USING clauses of joins.
	 */
	if (__process_join(parser, select))
		return 1;

	/*
	 * For every "*" that occurs in the column list, insert the names of
	 * all columns in all tables.  And for every TABLE.* insert the names
	 * of all columns in TABLE.  The parser inserted a special expression
	 * with the TK_ALL operator for each "*" that it found in the column
	 * list.  The following code just has to locate the TK_ALL expressions
	 * and expand each one to the list of all columns in all tables.
	 *
	 * The first loop just checks to see if there are any "*" operators
	 * that need expanding.
	 */
	for (k = 0; k < elist->nExpr; k++) {
		e = elist->a[k].pExpr;
		if (e->op == TK_ALL)
			break;
		if (e->op == TK_DOT && e->pRight && e->pRight->op == TK_ALL
		    && e->pLeft && e->pLeft->op == TK_ID)
			break;
	}
	rc = 0;
	if (k < elist->nExpr) {
		/*
		 * If we get here it means the result set contains one or
		 * more "*" operators that need to be expanded.  Loop through
		 * each expression in the result set and expand them one by
		 * one.
		 */
		a = elist->a;
		new = 0;
		for (k = 0; k < elist->nExpr; k++) {
			e = a[k].pExpr;
			if (e->op != TK_ALL && (e->op != TK_DOT ||
						e->pRight == 0 ||
						e->pRight->op != TK_ALL)) {
				/*
				 * This particular expression does not need
				 * to be expanded.
				 */
				new = __expr_list_append(new, a[k].pExpr, 0);
				new->a[new->nExpr-1].zName = a[k].zName;
				a[k].pExpr = 0;
				a[k].zName = 0;
			} else {
				/*
				 * This expression is a "*" or a "TABLE.*" and
				 * needs to be expanded.
				 */
				/* 'name' is the text of name of TABLE */
				table_seen = 0; /* 1 when TABLE matches */
				if (e->op == TK_DOT && e->pLeft) {
					name = &e->pLeft->token;
				} else {
					name = 0;
				}
				for (i = 0; i < tables->nSrc; i++) {
					tab = tables->a[i].pTab;
					tab_name = tables->a[i].zAlias;
					if (tab_name==0 || tab_name[0]==0 ){ 
						tab_name = tab->zName;
					}
					if (name &&
					    (tab_name == 0 ||
					     tab_name[0] == 0 ||
					     strncasecmp(name->z,
								    tab_name,
								    name->n) !=
					     0 || tab_name[name->n] != 0)) {
						continue;
					}
					table_seen = 1;
					for (j = 0; j < tab->nCol; j++) {
					    jname = tab->aCol[j].zName;

					    if (i > 0 &&
						(tables->a[i-1].jointype &
						 JT_NATURAL) != 0 &&
						__column_index(
							tables->a[i-1].pTab,
							jname) >= 0) {
						    /*
						     * In a NATURAL join, omit
						     * the join columns from
						     * the table on the right.
						     */
						    continue;
					    }
					    if(i > 0 &&
					       __id_list_index(
						       tables->a[i-1].pUsing,
						       jname) >= 0) {
						    /*
						     * In a join with a USING
						     * clause, omit columns in
						     * the using clause from
						     * the table on the right.
						     */
						    continue;
					    }
					    right = __expr(TK_ID, 0, 0, 0);
					    if (right == 0)
						    break;
					    right->token.z = jname;
					    right->token.n = strlen(jname);
					    right->token.dyn = 0;
					    if (tab_name && tables->nSrc > 1) {
						    left = __expr(TK_ID, 0,
								  0, 0);
						    expr = __expr(TK_DOT, left,
								  right, 0);
						    if (expr == 0)
							    break;
						    left->token.z = tab_name;
						    left->token.n =
							    strlen(tab_name);
						    left->token.dyn = 0;
						    __str_append((char**)&expr->span.z, tab_name, ".", jname, 0);
						    expr->span.n = strlen(expr->span.z);
						    expr->span.dyn = 1;
						    expr->token.z = 0;
						    expr->token.n = 0;
						    expr->token.dyn = 0;
					    } else {
						    expr = right;
						    expr->span = expr->token;
					    }
					    new = __expr_list_append(new, expr,
								     0);
					}
				}
				if (!table_seen) {
					if (name) {
						__error_msg(parser,
							  "no such table: %T",
							  name);
					} else {
						__error_msg(parser,
						        "no tables specified");
					}
					rc = 1;
				}
			}
		}
		__expr_list_delete(elist);
		select->pEList = new;
	}
	return rc;
}

/*
 * __select_unbind --
 *	This routine recursively unlinks the select_t.pSrc.a[].pTab pointers
 *	in a select structure.  It just sets the pointers to NULL.  This
 *	routine is recursive in the sense that if the select_t.pSrc.a[].pSelect
 *	pointer is not NULL, this routine is called recursively on that
 *	pointer.
 *
 *	This routine is called on the select_t structure that defines a
 *	VIEW in order to undo any bindings to tables.  This is necessary
 *	because those tables might be DROPed by a subsequent SQL command.
 *	If the bindings are not removed, then the select_t.pSrc->a[].pTab
 *	field will be left pointing to a deallocated table_t structure after
 *	the DROP and a coredump will occur the next time the VIEW is used.
 *
 * PUBLIC: void __select_unbind __P((select_t *));
 */
void
__select_unbind(select)
	select_t *select;
{
	int i;
	table_t *table;
	src_list_t *src = select->pSrc;

	if (select == 0)
		return;
	for (i = 0; i < src->nSrc; i++) {
		if ((table = src->a[i].pTab) != 0) {
			if (table->isTransient) {
				__vdbe_delete_table(0, table);
			}
			src->a[i].pTab = 0;
			if (src->a[i].pSelect) {
				__select_unbind(src->a[i].pSelect);
			}
		}
	}
}

/*
 * __match_orderby_to_column --
 *	This routine associates entries in an ORDER BY expression list with
 *	columns in a result.  For each ORDER BY expression, the opcode of
 *	the top-level node is changed to TK_COLUMN and the iColumn value of
 *	the top-level node is filled in with column number and the iTable
 *	value of the top-level node is filled with iTable parameter.
 *
 *	If there are prior SELECT clauses, they are processed first.  A match
 *	in an earlier SELECT takes precedence over a later SELECT.
 *
 *	Any entry that does not match is flagged as an error.  The number
 *	of errors is returned.
 *
 *	This routine does NOT correctly initialize the expr_t.dataType  field
 *	of the ORDER BY expressions.  The __multi_select_sort_order() routine
 *	must be called to do that after the individual select statements
 *	have all been analyzed.  This routine is unable to compute
 *	expr_t.dataType because it must be called before the individual
 *	select statements have been analyzed.
 *
 * STATIC: static int __match_orderby_to_column __P((parser_t *, select_t *,
 * STATIC:                                      expr_list_t *, int, int));
 *
 * parser			A place to leave error messages
 * select			Match to result columns of this SELECT
 * orderby_clause		The ORDER BY values to match against columns
 * table_idx			Insert this value in table_idx
 * must_complete_p		If TRUE all ORDER BYs must match
 */
static int
__match_orderby_to_column(parser, select, orderby_clause, table_idx,
			  must_complete_p)
	parser_t *parser;
	select_t *select;
	expr_list_t *orderby_clause;
	int table_idx;
	int must_complete_p;
{
	int i, j, col;
	int num_err = 0;
	expr_list_t *elist;
	expr_t *e;
	char *name, *label;

	if (select == 0 || orderby_clause == 0)
		return 1;
	if (must_complete_p) {
		for (i = 0; i < orderby_clause->nExpr; i++) {
			orderby_clause->a[i].done = 0;
		}
	}
	if (__fill_in_column_list(parser, select)) {
		return 1;
	}
	if (select->pPrior) {
		if (__match_orderby_to_column(parser, select->pPrior,
					      orderby_clause, table_idx, 0)) {
			return 1;
		}
	}
	elist = select->pEList;
	for (i = 0; i < orderby_clause->nExpr; i++) {
		e = orderby_clause->a[i].pExpr;
		col = -1;
		if (orderby_clause->a[i].done)
			continue;
		if (__expr_is_integer(e, &col)) {
			if (col <= 0 || col > elist->nExpr) {
				__error_msg(parser, "ORDER BY position %d "
					    "should be between 1 and %d",
					    col, elist->nExpr);
				num_err++;
				break;
			}
			if (!must_complete_p)
				continue;
			col--;
		}
		for (j = 0; col < 0 && j < elist->nExpr; j++) {
			if (elist->a[j].zName &&
			    (e->op == TK_ID || e->op == TK_STRING)) {
				name = elist->a[j].zName;
				DBSQL_ASSERT(e->token.z);
				__dbsql_strndup(NULL, e->token.z, &label,
					     e->token.n);
				__str_unquote(label);
				if (strcasecmp(name, label) == 0) { 
					col = j;
				}
				__dbsql_free(NULL, label);
			}
			if (col < 0 && __expr_compare(e, elist->a[j].pExpr)) {
				col = j;
			}
		}
		if (col >= 0) {
			e->op = TK_COLUMN;
			e->iColumn = col;
			e->iTable = table_idx;
			orderby_clause->a[i].done = 1;
		}
		if (col < 0 && must_complete_p) {
			__error_msg(parser, "ORDER BY term number %d does not "
				    "match any result column", (i + 1));
			num_err++;
			break;
		}
	}
	return num_err;  
}

/*
 * __parser_get_vdbe --
 *	Get a VDBE for the given parser context.  Create a new one if
 *	necessary.  If an error occurs, return NULL and leave a message
 *	in parser.
 *
 * PUBLIC: vdbe_t *__parser_get_vdbe __P((parser_t *));
 */
vdbe_t *
__parser_get_vdbe(parser)
	parser_t *parser;
{
	vdbe_t *v = parser->pVdbe;
	if (v == 0) {
		v = parser->pVdbe = __vdbe_create(parser->db);
	}
	return v;
}

/*
 * __multi_select_sort_order --
 *	This routine sets the expr_t.dataType field on all elements of
 *	the orderby_clause expression list.  The orderby_clause list will
 *	have been set up by __match_orderby_to_column().  Hence each
 *	expression has a TK_COLUMN as its root node.  The expr_t.iColumn
 *	refers to a column in the result set.   The datatype is set to
 *	DBSQL_SO_TEXT if the corresponding column in p and every SELECT to
 *	the left of 'select' has a datatype of DBSQL_SO_TEXT.  If the
 *	cooressponding column in 'select' or any of the left SELECTs is
 *	DBSQL_SO_NUM, then the datatype of the order-by expression is set
 *	to DBSQL_SO_NUM.
 *
 *	Examples:
 *
 *	     CREATE TABLE one(a INTEGER, b TEXT);
 *	     CREATE TABLE two(c VARCHAR(5), d FLOAT);
 *
 *	     SELECT b, b FROM one UNION SELECT d, c FROM two ORDER BY 1, 2;
 *
 *	The primary sort key will use DBSQL_SO_NUM because the "d" in
 *	the second SELECT is numeric.  The 1st column of the first SELECT
 *	is text but that does not matter because a numeric always overrides
 *	a text.
 *
 *	The secondary key will use the DBSQL_SO_TEXT sort order because
 *	both the (second) "b" in the first SELECT and the "c" in the second
 *	SELECT have a datatype of text.
 *
 * STATIC: static void __multi_select_sort_order __P((select_t *,
 * STATIC:                                       expr_list_t *));
 */ 
static void
__multi_select_sort_order(select, orderby_clause)
	select_t *select;
	expr_list_t *orderby_clause;
{
	int i;
	expr_list_t *elist;
	expr_t *e;

	if (orderby_clause == 0)
		return;
	if (select == 0) {
		for (i = 0; i < orderby_clause->nExpr; i++) {
			orderby_clause->a[i].pExpr->dataType = DBSQL_SO_TEXT;
		}
		return;
	}
	__multi_select_sort_order(select->pPrior, orderby_clause);
	elist = select->pEList;
	for (i = 0; i < orderby_clause->nExpr; i++) {
		e = orderby_clause->a[i].pExpr;
		if (e->dataType == DBSQL_SO_NUM)
			continue;
		DBSQL_ASSERT(e->iColumn >= 0);
		if (elist->nExpr > e->iColumn) {
			e->dataType = __expr_type(elist->a[e->iColumn].pExpr);
		}
	}
}

/*
 * __compute_limit_registers --
 *	Compute the iLimit and iOffset fields of the SELECT based on the
 *	nLimit and nOffset fields.  nLimit and nOffset hold the integers
 *	that appear in the original SQL statement after the LIMIT and OFFSET
 *	keywords.  Or that hold -1 and 0 if those keywords are omitted.
 *	iLimit and iOffset are the integer memory register numbers for
 *	counters used to compute the limit and offset.  If there is no
 *	limit and/or offset, then iLimit and iOffset are negative.
 *
 *	This routine changes the values if iLimit and iOffset only if
 *	a limit or offset is defined by nLimit and nOffset.  iLimit and
 *	iOffset should have been preset to appropriate default values
 *	(usually but not always -1) prior to calling this routine.
 *	Only if nLimit>=0 or nOffset>0 do the limit registers get
 *	redefined.  The UNION ALL operator uses this property to force
 *	the reuse of the same limit and offset registers across multiple
 *	SELECT statements.
 *
 * STATIC: static void __compute_limit_registers __P((parser_t *, select_t *));
 */
static void
__compute_limit_registers(parser, select)
	parser_t *parser;
	select_t *select;
{
	int mem;
	vdbe_t *v;

	/*
	 * If the comparison is select->nLimit>0 then "LIMIT 0" shows
	 * all rows.  It is the same as no limit. If the comparision is
	 * select->nLimit>=0 then "LIMIT 0" show no rows at all.
	 * "LIMIT -1" always shows all rows.  There is some
	 * contraversy about what the correct behavior should be.
	 * The current implementation interprets "LIMIT 0" to mean
	 * no rows.
	 */
	if (select->nLimit >= 0) {
		mem = parser->nMem++;
		v = __parser_get_vdbe(parser);
		if (v == 0)
			return;
		__vdbe_add_op(v, OP_Integer, (-select->nLimit), 0);
		__vdbe_add_op(v, OP_MemStore, mem, 1);
		select->iLimit = mem;
	}
	if (select->nOffset > 0) {
		mem = parser->nMem++;
		v = __parser_get_vdbe(parser);
		if (v == 0)
			return;
		__vdbe_add_op(v, OP_Integer, (- select->nOffset), 0);
		__vdbe_add_op(v, OP_MemStore, mem, 1);
		select->iOffset = mem;
	}
}

/*
 * __multi_select --
 *	This routine is called to process a query that is really the union
 *	or intersection of two or more separate queries.
 *
 *	'select' points to the right-most of the two queries.  The query on the
 *	left is select->pPrior.  The left query could also be a compound query
 *	in which case this routine will be called recursively. 
 *
 *	The results of the total query are to be written into a destination
 *	of type eDest with parameter iParm.
 *
 * Example 1:  Consider a three-way compound SQL statement.
 *
 *     SELECT a FROM t1 UNION SELECT b FROM t2 UNION SELECT c FROM t3
 *
 * This statement is parsed up as follows:
 *
 *     SELECT c FROM t3
 *      |
 *      `----->  SELECT b FROM t2
 *                |
 *                `------>  SELECT a FROM t1
 *
 *	The arrows in the diagram above represent the select_t.pPrior pointer.
 *	So if this routine is called with p equal to the t3 query, then
 *	pPrior will be the t2 query.  select->op will be TK_UNION in this case.
 *
 *	Notice that because of the way we parse compound SELECTs, the
 *	individual selects always group from left to right.
 *
 * STATIC: static int __multi_select __P((parser_t *, select_t *, int, int));
 *
 */
static int
__multi_select(parser, select, dest, param)
	parser_t *parser;
	select_t *select;
	int dest;
	int param;
{
	int tab1, tab2;
	int cont, brk, start;
	int rc;              /* Success code from a subroutine */
	select_t *prior;     /* Another SELECT immediately to our left */
	vdbe_t *v;           /* Generate code to this VDBE */
	int union_tab;       /* Cursor number of the temporary table
                                holding result */
	int op;              /* One of the SRT_ operations to apply to self */
	int prior_op;        /* The SRT_ operation to apply to prior selects */
	int limit, offset;   /* Saved values of select->nLimit and
                                select->nOffset */
	expr_list_t *orderby_clause; /* The ORDER BY clause for the right
                                        SELECT */

	/*
	 * Make sure there is no ORDER BY or LIMIT clause on prior SELECTs.
	 * Only the last SELECT in the series may have an ORDER BY or LIMIT.
	 */
	if (select == 0 || select->pPrior == 0)
		return 1;
	prior = select->pPrior;
	if (prior->pOrderBy) {
		__error_msg(parser,
			    "ORDER BY clause should come after %s not before",
			    __select_op_name(select->op));
		return 1;
	}
	if (prior->nLimit >= 0 || prior->nOffset > 0) {
		__error_msg(parser,
			    "LIMIT clause should come after %s not before",
			    __select_op_name(select->op));
		return 1;
	}

	/*
	 * Make sure we have a valid query engine.  If not, create a new one.
	 */
	v = __parser_get_vdbe(parser);
	if (v == 0)
		return 1;

	/*
	 * Create the destination temporary table if necessary.
	 */
	if (dest == SRT_TempTable) {
		__vdbe_add_op(v, OP_OpenTemp, param, 0);
		dest = SRT_Table;
	}

	/*
	 * Generate code for the left and right SELECT statements.
	 */
	switch(select->op) {
	case TK_ALL:
		if (select->pOrderBy == 0) {
			prior->nLimit = select->nLimit;
			prior->nOffset = select->nOffset;
			rc = __select(parser, prior, dest, param, 0, 0, 0);
			if (rc)
				return rc;
			select->pPrior = 0;
			select->iLimit = prior->iLimit;
			select->iOffset = prior->iOffset;
			select->nLimit = -1;
			select->nOffset = 0;
			rc = __select(parser, select, dest, param, 0, 0, 0);
			select->pPrior = prior;
			if (rc)
				return rc;
			break;
		}
		/* For UNION ALL ... ORDER BY fall through to the next case. */
	case TK_EXCEPT: /* FALLTHROUGH */
	case TK_UNION:
		prior_op = (select->op == TK_ALL) ? SRT_Table : SRT_Union;
		if (dest == prior_op && select->pOrderBy == 0 &&
		    select->nLimit < 0 && select->nOffset == 0) {
			/*
			 * We can reuse a temporary table generated by a
			 * SELECT to our right.
			 */
			union_tab = param;
		} else {
			/*
			 * We will need to create our own temporary table
			 * to hold the intermediate results.
			 */
			union_tab = parser->nTab++;
			if (select->pOrderBy &&
			    __match_orderby_to_column(parser, select,
						      select->pOrderBy,
						      union_tab, 1)) {
				return 1;
			}
			if (select->op != TK_ALL) {
				__vdbe_add_op(v, OP_OpenTemp, union_tab, 1);
				__vdbe_add_op(v, OP_KeyAsData, union_tab, 1);
			} else {
				__vdbe_add_op(v, OP_OpenTemp, union_tab, 0);
			}
		}

		/*
		 * Code the SELECT statements to our left.
		 */
		rc = __select(parser, prior, prior_op, union_tab, 0, 0, 0);
		if (rc)
			return rc;

		/*
		 * Code the current SELECT statement.
		 */
		switch(select->op) {
		case TK_EXCEPT:  op = SRT_Except;   break;
		case TK_UNION:   op = SRT_Union;    break;
		case TK_ALL:     op = SRT_Table;    break;
		}
		select->pPrior = 0;
		orderby_clause = select->pOrderBy;
		select->pOrderBy = 0;
		limit = select->nLimit;
		select->nLimit = -1;
		offset = select->nOffset;
		select->nOffset = 0;
		rc = __select(parser, select, op, union_tab, 0, 0, 0);
		select->pPrior = prior;
		select->pOrderBy = orderby_clause;
		select->nLimit = limit;
		select->nOffset = offset;
		if (rc)
			return rc;

		/*
		 * Convert the data in the temporary table into whatever form
		 * it is that we currently need.
		 */      
		if (dest != prior_op || union_tab != param) {
			DBSQL_ASSERT(select->pEList);
			if (dest == SRT_Callback) {
				__generate_column_names(parser, 0,
							select->pEList);
				__generate_column_types(parser,
							select->pSrc,
							select->pEList);
			}
			brk = __vdbe_make_label(v);
			cont = __vdbe_make_label(v);
			__vdbe_add_op(v, OP_Rewind, union_tab, brk);
			__compute_limit_registers(parser, select);
			start = __vdbe_current_addr(v);
			__multi_select_sort_order(select, select->pOrderBy);
			rc = __select_inner_loop(parser, select,
						 select->pEList, union_tab,
						 select->pEList->nExpr,
						 select->pOrderBy, -1,
						 dest, param, cont, brk);
			if (rc)
				return 1;
			__vdbe_resolve_label(v, cont);
			__vdbe_add_op(v, OP_Next, union_tab, start);
			__vdbe_resolve_label(v, brk);
			__vdbe_add_op(v, OP_Close, union_tab, 0);
			if (select->pOrderBy) {
				__generate_sort_tail(select, v,
						     select->pEList->nExpr,
						     dest, param);
			}
		}
		break;
	case TK_INTERSECT:

		/*
		 * INTERSECT is different from the others since it requires
		 * two temporary tables.  Hence it has its own case.  Begin
		 * by allocating the tables we will need.
		 */
		tab1 = parser->nTab++;
		tab2 = parser->nTab++;
		if (select->pOrderBy &&
		    __match_orderby_to_column(parser, select,
					      select->pOrderBy, tab1, 1)) {
			return 1;
		}
		__vdbe_add_op(v, OP_OpenTemp, tab1, 1);
		__vdbe_add_op(v, OP_KeyAsData, tab1, 1);

		/*
		 * Code the SELECTs to our left into temporary table "tab1".
		 */
		rc = __select(parser, prior, SRT_Union, tab1, 0, 0, 0);
		if (rc)
			return rc;

		/*
		 * Code the current SELECT into temporary table "tab2".
		 */
		__vdbe_add_op(v, OP_OpenTemp, tab2, 1);
		__vdbe_add_op(v, OP_KeyAsData, tab2, 1);
		select->pPrior = 0;
		limit = select->nLimit;
		select->nLimit = -1;
		offset = select->nOffset;
		select->nOffset = 0;
		rc = __select(parser, select, SRT_Union, tab2, 0, 0, 0);
		select->pPrior = prior;
		select->nLimit = limit;
		select->nOffset = offset;
		if (rc)
			return rc;

		/*
		 * Generate code to take the intersection of the two temporary
		 * tables.
		 */
		DBSQL_ASSERT(select->pEList);
		if (dest == SRT_Callback) {
			__generate_column_names(parser, 0, select->pEList);
			__generate_column_types(parser, select->pSrc,
						select->pEList);
		}
		brk = __vdbe_make_label(v);
		cont = __vdbe_make_label(v);
		__vdbe_add_op(v, OP_Rewind, tab1, brk);
		__compute_limit_registers(parser, select);
		start = __vdbe_add_op(v, OP_FullKey, tab1, 0);
		__vdbe_add_op(v, OP_NotFound, tab2, cont);
		__multi_select_sort_order(select, select->pOrderBy);
		rc = __select_inner_loop(parser, select, select->pEList,
					 tab1, select->pEList->nExpr,
					 select->pOrderBy, -1, dest, param, 
					 cont, brk);
		if (rc)
			return 1;
		__vdbe_resolve_label(v, cont);
		__vdbe_add_op(v, OP_Next, tab1, start);
		__vdbe_resolve_label(v, brk);
		__vdbe_add_op(v, OP_Close, tab2, 0);
		__vdbe_add_op(v, OP_Close, tab1, 0);
		if (select->pOrderBy) {
			__generate_sort_tail(select, v, select->pEList->nExpr,
					     dest, param);
		}
		break;
	}
	DBSQL_ASSERT(select->pEList && prior->pEList);
	if (select->pEList->nExpr != prior->pEList->nExpr) {
		__error_msg(parser, "SELECTs to the left and right of %s"
			    " do not have the same number of result columns",
			    __select_op_name(select->op));
		return 1;
	}

	/*
	 * Issue a null callback if that is what the user wants.
	 */
	if (dest == SRT_Callback &&
	    (parser->useCallback==0 ||
	     (parser->db->flags & DBSQL_NullCallback) != 0)) {
		__vdbe_add_op(v, OP_NullCallback, select->pEList->nExpr, 0);
	}
	return 0;
}

static void __subst_expr_list(expr_list_t*,int,expr_list_t*);

/*
 * __subst_expr --
 *	Scan through the expression expr.  Replace every reference to
 *	a column in table number table with a copy of the iColumn-th
 *	entry in elist.  (But leave references to the ROWID column 
 *	unchanged.)
 *
 *	This routine is part of the flattening procedure.  A subquery
 *	whose result set is defined by pEList appears as entry in the
 *	FROM clause of a SELECT such that the VDBE cursor assigned to that
 *	FORM clause entry is iTable.  This routine make the necessary 
 *	changes to pExpr so that it refers directly to the source table
 *	of the subquery rather the result set of the subquery.
 *
 * STATIC: void __subst_expr __P((expr_t *, int, expr_list_t *));
 */
static void
__subst_expr(expr, table, elist)
	expr_t *expr;
	int table;
	expr_list_t *elist;
{
	expr_t *new;

	if (expr == 0)
		return;
	if (expr->op == TK_COLUMN &&
	    expr->iTable == table &&
	    expr->iColumn >= 0) {
		DBSQL_ASSERT(elist != 0 && expr->iColumn < elist->nExpr);
		DBSQL_ASSERT(expr->pLeft == 0 &&
		       expr->pRight == 0 &&
		       expr->pList == 0);
		new = elist->a[expr->iColumn].pExpr;
		DBSQL_ASSERT(new != 0);
		expr->op = new->op;
		expr->dataType = new->dataType;
		DBSQL_ASSERT(expr->pLeft == 0);
		expr->pLeft = __expr_dup(new->pLeft);
		DBSQL_ASSERT(expr->pRight == 0);
		expr->pRight = __expr_dup(new->pRight);
		DBSQL_ASSERT(expr->pList == 0);
		expr->pList = __expr_list_dup(new->pList);
		expr->iTable = new->iTable;
		expr->iColumn = new->iColumn;
		expr->iAgg = new->iAgg;
		__token_copy(&expr->token, &new->token);
		__token_copy(&expr->span, &new->span);
	} else {
		__subst_expr(expr->pLeft, table, elist);
		__subst_expr(expr->pRight, table, elist);
		__subst_expr_list(expr->pList, table, elist);
	}
}

/*
 * __subst_expr_list --
 *
 * STATIC: static void __subst_expr_list __P((expr_list_t *, int,
 * STATIC:                               expr_list_t *));
 */
static void
__subst_expr_list(list, table, elist)
	expr_list_t *list;
	int table;
	expr_list_t *elist;
{
	int i;
	if (list == 0)
		return;
	for (i = 0; i < list->nExpr; i++) {
		__subst_expr(list->a[i].pExpr, table, elist);
	}
}

/*
 * __flatten_subquery --
 *	This routine attempts to flatten subqueries in order to speed
 *	execution.  It returns 1 if it makes changes and 0 if no flattening
 *	occurs.
 *
 *	To understand the concept of flattening, consider the following
 *	query:
 *
 *	     SELECT a FROM (SELECT x+y AS a FROM t1 WHERE z<100) WHERE a>5
 *
 *	The default way of implementing this query is to execute the
 *	subquery first and store the results in a temporary table, then
 *	run the outer query on that temporary table.  This requires two
 *	passes over the data.  Furthermore, because the temporary table
 *	has no indices, the WHERE clause on the outer query cannot be
 *	optimized.
 *
 *	This routine attempts to rewrite queries such as the above into
 *	a single flat select, like this:
 *
 *	     SELECT x+y AS a FROM t1 WHERE z<100 AND a>5
 *
 *	The code generated for this simpification gives the same result
 *	but only has to scan the data once.  And because indices might 
 *	exist on the table t1, a complete scan of the data might be
 *	avoided.
 *
 * Flattening is only attempted if all of the following are true:
 *
 *   (1)  The subquery and the outer query do not both use aggregates.
 *
 *   (2)  The subquery is not an aggregate or the outer query is not a join.
 *
 *   (3)  The subquery is not the right operand of a left outer join, or
 *        the subquery is not itself a join.  (Ticket #306)
 *
 *   (4)  The subquery is not DISTINCT or the outer query is not a join.
 *
 *   (5)  The subquery is not DISTINCT or the outer query does not use
 *        aggregates.
 *
 *   (6)  The subquery does not use aggregates or the outer query is not
 *        DISTINCT.
 *
 *   (7)  The subquery has a FROM clause.
 *
 *   (8)  The subquery does not use LIMIT or the outer query is not a join.
 *
 *   (9)  The subquery does not use LIMIT or the outer query does not use
 *        aggregates.
 *
 *  (10)  The subquery does not use aggregates or the outer query does not
 *        use LIMIT.
 *
 *  (11)  The subquery and the outer query do not both have ORDER BY clauses.
 *
 *  (12)  The subquery is not the right term of a LEFT OUTER JOIN or the
 *        subquery has no WHERE clause.  (added by ticket #350)
 *
 *	In this routine, the 'select' parameter is a pointer to the outer
 *	query.  The subquery is p->pSrc->a[iFrom].  isAgg is true if the
 *	outer query uses aggregates and subqueryIsAgg is true if the subquery
 *	uses aggregates.
 *
 *	If flattening is not attempted, this routine is a no-op and returns 0.
 *	If flattening is attempted this routine returns 1.
 *
 *	All of the expression analysis must occur on both the outer query and
 *	the subquery before this routine runs.
 *
 * STATIC: static int __flatten_subquery __P((parser_t *, select_t *, int,
 * STATIC:                               int, int));
 *
 * parser			The parsing context
 * select			The parent or outer SELECT statement
 * from				Index in p->pSrc->a[] of the inner subquery
 * agg_p			True if outer SELECT uses aggregate functions
 * subquery_agg_p		True if the subquery uses aggregate functions
 */
static int
__flatten_subquery(parser, select, from, agg_p, subquery_agg_p)
	parser_t *parser;
	select_t *select;
	int from;
	int agg_p;
	int subquery_agg_p;
{
	select_t *sub_select;          /* The inner query or "subquery" */
	src_list_t *outer_from_clause; /* The FROM clause of the outer query */
	src_list_t *sub_from_clause;   /* The FROM clause of the subquery */
	expr_list_t *outer_query_results;/* The result set of the outer query*/
	int parent;                    /* VDBE cursor number of the pSub
                                          result set temp table */
	int i, nsub_src, jointype, extra;
	expr_t *where, *expr, *having;

	/*
	 * Check to see if flattening is permitted.  Return 0 if not.
	 */
	if (select == 0)
		return 0;
	outer_from_clause = select->pSrc;
	DBSQL_ASSERT(outer_from_clause &&
	       from >= 0 &&
	       from < outer_from_clause->nSrc);
	sub_select = outer_from_clause->a[from].pSelect;
	DBSQL_ASSERT(sub_select != 0);
	if (agg_p && subquery_agg_p)
		return 0;
	if (subquery_agg_p && outer_from_clause->nSrc > 1)
		return 0;
	sub_from_clause = sub_select->pSrc;
	DBSQL_ASSERT(sub_from_clause);
	if (sub_from_clause->nSrc == 0)
		return 0;
	if ((sub_select->isDistinct || sub_select->nLimit >= 0) &&
	    (outer_from_clause->nSrc > 1 || agg_p)) {
		return 0;
	}
	if ((select->isDistinct || select->nLimit >= 0)
	    && subquery_agg_p )
		return 0;
	if (select->pOrderBy && sub_select->pOrderBy)
		return 0;

	/*
	 * Restriction 3:  If the subquery is a join, make sure the subquery
	 * is not used as the right operand of an outer join.  Examples of
	 * why this is not allowed:
	 *
	 *         t1 LEFT OUTER JOIN (t2 JOIN t3)
	 *
	 * If we flatten the above, we would get
	 *
	 *         (t1 LEFT OUTER JOIN t2) JOIN t3
	 *
	 * which is not at all the same thing.
	 */
	if (sub_from_clause->nSrc > 1 && from > 0 &&
	    (outer_from_clause->a[from-1].jointype & JT_OUTER) != 0) {
		return 0;
	}

	/*
	 * Restriction 12:  If the subquery is the right operand of a left
	 * outer join, make sure the subquery has no WHERE clause.
	 * An examples of why this is not allowed:
	 *
	 *         t1 LEFT OUTER JOIN (SELECT * FROM t2 WHERE t2.x>0)
	 *
	 * If we flatten the above, we would get
	 *
	 *         (t1 LEFT OUTER JOIN t2) WHERE t2.x>0
	 *
	 * But the t2.x>0 test will always fail on a NULL row of t2, which
	 * effectively converts the OUTER JOIN into an INNER JOIN.
	 */
	if (from > 0 &&
	    (outer_from_clause->a[from-1].jointype & JT_OUTER) != 0 &&
	    sub_select->pWhere != 0) {
		return 0;
	}

	/*
	 * If we reach this point, it means flattening is permitted for the
	 * from-th entry of the FROM clause in the outer query.
	 */

	/*
	 * Move all of the FROM elements of the subquery into the
	 * the FROM clause of the outer query.  Before doing this, remember
	 * the cursor number for the original outer query FROM element in
	 * parent.  The parent cursor will never be used.  Subsequent code
	 * will scan expressions looking for parent references and replace
	 * those references with expressions that resolve to the subquery FROM
	 * elements we are now copying in.
	 */
	parent = outer_from_clause->a[from].iCursor;
	nsub_src = sub_from_clause->nSrc;
	jointype = outer_from_clause->a[from].jointype;

	if (outer_from_clause->a[from].pTab &&
	    outer_from_clause->a[from].pTab->isTransient) {
		__vdbe_delete_table(0, outer_from_clause->a[from].pTab);
	}
	__dbsql_free(NULL, outer_from_clause->a[from].zDatabase);
	__dbsql_free(NULL, outer_from_clause->a[from].zName);
	__dbsql_free(NULL, outer_from_clause->a[from].zAlias);
	if (nsub_src > 1) {
		extra = nsub_src - 1;
		for (i = 1; i < nsub_src; i++) {
			outer_from_clause =
				__src_list_append(outer_from_clause, 0, 0);
		}
		select->pSrc = outer_from_clause;
		for (i = (outer_from_clause->nSrc - 1);
		     (i - extra) >= from; i--) {
			outer_from_clause->a[i] =outer_from_clause->a[i-extra];
		}
	}
	for(i = 0; i < nsub_src; i++) {
		outer_from_clause->a[i+from] = sub_from_clause->a[i];
		memset(&sub_from_clause->a[i], 0,
		       sizeof(sub_from_clause->a[i]));
	}
	outer_from_clause->a[(from + nsub_src - 1)].jointype = jointype;

	/*
	 * Now begin substituting subquery result set expressions for 
	 * references to the parent in the outer query.
	 * 
	 * Example:
	 *
	 *SELECT a+5, b*10 FROM (SELECT x*3 AS a, y+10 AS b FROM t1) WHERE a>b;
	 *\                     \_____________ subquery __________/          /
	 * \_____________________ outer query ______________________________/
	 *
	 * We look at every expression in the outer query and every place we
	 * see "a" we substitute "x*3" and every place we see "b" we
	 * substitute "y+10".
	 */
	__subst_expr_list(select->pEList, parent, sub_select->pEList);
	outer_query_results = select->pEList;
	for(i = 0; i < outer_query_results->nExpr; i++) {
		if (outer_query_results->a[i].zName == 0 &&
		    (expr = outer_query_results->a[i].pExpr)->span.z != 0) {
			__dbsql_strndup(NULL, expr->span.z,
				     &outer_query_results->a[i].zName,
				     expr->span.n);
		}
	}
	if (agg_p) {
		__subst_expr_list(select->pGroupBy, parent,
				  sub_select->pEList);
		__subst_expr(select->pHaving, parent, sub_select->pEList);
	}
	if (sub_select->pOrderBy) {
		DBSQL_ASSERT(select->pOrderBy == 0);
		select->pOrderBy = sub_select->pOrderBy;
		sub_select->pOrderBy = 0;
	} else if (select->pOrderBy) {
		__subst_expr_list(select->pOrderBy, parent,
				  sub_select->pEList);
	}
	if (sub_select->pWhere) {
		where = __expr_dup(sub_select->pWhere);
	} else {
		where = 0;
	}
	if (subquery_agg_p) {
		DBSQL_ASSERT(select->pHaving == 0);
		select->pHaving = select->pWhere;
		select->pWhere = where;
		__subst_expr(select->pHaving, parent, sub_select->pEList);
		if (sub_select->pHaving) {
			having = __expr_dup(sub_select->pHaving);
			if (select->pHaving) {
				select->pHaving = __expr(TK_AND,
							 select->pHaving,
							 having, 0);
			} else {
				select->pHaving = having;
			}
		}
		DBSQL_ASSERT(select->pGroupBy == 0);
		select->pGroupBy = __expr_list_dup(sub_select->pGroupBy);
	} else if (select->pWhere == 0) {
		select->pWhere = where;
	} else {
		__subst_expr(select->pWhere, parent, sub_select->pEList);
		if (where) {
			select->pWhere = __expr(TK_AND, select->pWhere,
						where, 0);
		}
	}

	/*
	 * The flattened query is distinct if either the inner or the
	 * outer query is distinct. 
	 */
	select->isDistinct = select->isDistinct || sub_select->isDistinct;

	/*
	 * Transfer the limit expression from the subquery to the outer
	 * query.
	 */
	if (sub_select->nLimit >= 0) {
		if (select->nLimit < 0) {
			select->nLimit = sub_select->nLimit;
		} else if ((select->nLimit + select->nOffset) >
			   (sub_select->nLimit + sub_select->nOffset)) {
			select->nLimit = sub_select->nLimit +
				sub_select->nOffset - select->nOffset;
		}
	}
	select->nOffset += sub_select->nOffset;

	/*
	 * Finially, delete what is left of the subquery and return success.
	 */
	__select_delete(sub_select);
	return 1;
}

/*
 * __min_max_query --
 *	Analyze the SELECT statement passed in as an argument to see if it
 *	is a simple min() or max() query.  If it is and this query can be
 *	satisfied using a single seek to the beginning or end of an index,
 *	then generate the code for this SELECT and return 1.  If this is not a 
 *	simple min() or max() query, then return 0;
 *
 *	A simply min() or max() query looks like this:
 *
 *	   SELECT min(a) FROM table;
 *	   SELECT max(a) FROM table;
 *
 *	The query may have only a single table in its FROM argument.  There
 *	can be no GROUP BY or HAVING or WHERE clauses.  The result set must
 *	be the min() or max() of a single column of the table.  The column
 *	in the min() or max() function must be indexed.
 *
 *	The parameters to this routine are the same as for __select().
 *	See the header comment on that routine for additional information.
 *
 * STATIC: static int __min_max_query __P((parser_t *, select_t *, int, int));
 */
static int
__min_max_query(parser, select, dest, param)
	parser_t *parser;
	select_t *select;
	int dest;
	int param;
{
	expr_t *expr;
	int col;
	table_t *table;
	index_t *index;
	int base;
	vdbe_t *v;
	int seek_op;
	int cont;
	expr_list_t elist;
	struct expr_list_item list_item;

	/*
	 * Check to see if this query is a simple min() or max() query.  Return
	 * zero if it is  not.
	 */
	if (select->pGroupBy || select->pHaving || select->pWhere)
		return 0;
	if (select->pSrc->nSrc != 1)
		return 0;
	if (select->pEList->nExpr != 1)
		return 0;
	expr = select->pEList->a[0].pExpr;
	if (expr->op != TK_AGG_FUNCTION)
		return 0;
	if (expr->pList == 0 || expr->pList->nExpr != 1)
		return 0;
	if (expr->token.n != 3)
		return 0;
	if (strncasecmp(expr->token.z, "min", 3) == 0) {
		seek_op = OP_Rewind;
	} else if (strncasecmp(expr->token.z, "max", 3) == 0) {
		seek_op = OP_Last;
	} else {
		return 0;
	}
	expr = expr->pList->a[0].pExpr;
	if (expr->op != TK_COLUMN)
		return 0;
	col = expr->iColumn;
	table = select->pSrc->a[0].pTab;

	/*
	 * If we get to here, it means the query is of the correct form.
	 * Check to make sure we have an index and make index point to the
	 * appropriate index.  If the min() or max() is on an INTEGER PRIMARY
	 * key column, no index is necessary so set index to NULL.  If no
	 * usable index is found, return 0.
	 */
	if (col < 0) {
		index = 0;
	} else {
		for (index = table->pIndex; index; index = index->pNext) {
			DBSQL_ASSERT(index->nColumn >= 1);
			if (index->aiColumn[0] == col)
				break;
		}
		if (index == 0)
			return 0;
	}

	/*
	 * Identify column types if we will be using the callback.  This
	 * step is skipped if the output is going to a table or a memory cell.
	 * The column names have already been generated in the calling
	 * function.
	 */
	v = __parser_get_vdbe(parser);
	if (v == 0)
		return 0;
	if (dest == SRT_Callback) {
		__generate_column_types(parser, select->pSrc, select->pEList);
	}

	/*
	 * If the output is destined for a temporary table, open that table.
	 */
	if (dest == SRT_TempTable) {
		__vdbe_add_op(v, OP_OpenTemp, param, 0);
	}

	/*
	 * Generating code to find the min or the max.  Basically all we have
	 * to do is find the first or the last entry in the chosen index.  If
	 * the min() or max() is on the INTEGER PRIMARY KEY, then find the
	 * first or last entry in the main table.
	 */
	__code_verify_schema(parser, table->iDb);
	base = select->pSrc->a[0].iCursor;
	__compute_limit_registers(parser, select);
	__vdbe_add_op(v, OP_Integer, table->iDb, 0);
	__vdbe_add_op(v, OP_OpenRead, base, table->tnum);
	__vdbe_change_p3(v, -1, table->zName, P3_STATIC);
	cont = __vdbe_make_label(v);
	if (index == 0) {
		__vdbe_add_op(v, seek_op, base, 0);
	} else {
		__vdbe_add_op(v, OP_Integer, index->iDb, 0);
		__vdbe_add_op(v, OP_OpenRead, (base + 1), index->tnum);
		__vdbe_change_p3(v, -1, index->zName, P3_STATIC);
		__vdbe_add_op(v, seek_op, (base + 1), 0);
		__vdbe_add_op(v, OP_IdxRecno, (base + 1), 0);
		__vdbe_add_op(v, OP_Close, (base + 1), 0);
		__vdbe_add_op(v, OP_MoveTo, base, 0);
	}
	elist.nExpr = 1;
	memset(&list_item, 0, sizeof(list_item));
	elist.a = &list_item;
	elist.a[0].pExpr = expr;
	__select_inner_loop(parser, select, &elist, 0, 0, 0, -1, dest, param,
			    cont, cont);
	__vdbe_resolve_label(v, cont);
	__vdbe_add_op(v, OP_Close, base, 0);
	return 1;
}

/*
 * __select --
 *	Generate code for the given SELECT statement.
 *
 *	The results are distributed in various ways depending on the
 *	value of dest and param.
 *
 *     dest Value       Result
 *     ------------    -------------------------------------------
 *     SRT_Callback    Invoke the callback for each row of the result.
 *
 *     SRT_Mem         Store first result in memory cell param
 *
 *     SRT_Set         Store results as keys of a table with cursor param
 *
 *     SRT_Union       Store results as a key in a temporary table param
 *
 *     SRT_Except      Remove results from the temporary table param.
 *
 *     SRT_Table       Store results in temporary table param
 *
 *	NOTE: The table above is incomplete.  Additional dist value have be
 *	added since this comment was written.  See the __select_inner_loop()
 *	function for a complete listing of the allowed values of dest and
 *	their meanings.
 *
 *	This routine returns the number of errors.  If any errors are
 *	encountered, then an appropriate error message is left in
 *	parser->zErrMsg.
 *
 *	This routine does NOT free the select_t structure passed in.  The
 *	calling function needs to do that.
 *
 *	The pParent, parentTab, and *pParentAgg fields are filled in if this
 *	SELECT is a subquery.  This routine may try to combine this SELECT
 *	with its parent to form a single flat query.  In so doing, it might
 *	change the parent query from a non-aggregate to an aggregate query.
 *	For that reason, the pParentAgg flag is passed as a pointer, so it
 *	can be changed.
 *
 * Example 1:   The meaning of the pParent parameter.
 *
 *    SELECT * FROM t1 JOIN (SELECT x, count(*) FROM t2) JOIN t3;
 *    \                      \_______ subquery _______/        /
 *     \                                                      /
 *      \____________________ outer query ___________________/
 *
 *	This routine is called for the outer query first.   For that call,
 *	pParent will be NULL.  During the processing of the outer query, this 
 *	routine is called recursively to handle the subquery.  For the
 *	recursive call, pParent will point to the outer query.  Because the
 *	subquery is the second element in a three-way join, the parentTab
 *	parameter will be 1 (the 2nd value of a 0-indexed array.)
 *
 * PUBLIC: int __select __P((parser_t *, select_t *, int, int, select_t *,
 * PUBLIC:              int, int *));
 *
 * parser			The parser context
 * select			The SELECT statement being coded
 * dest				How to dispose of the results
 * param			A parameter used by the dest disposal method
 * parent			Another SELECT for which this is a sub-query
 * parent_tab			Index in parent->pSrc of this query
 * parent_agg_p			True if parent uses aggregate functions
 */
int __select(parser, select, dest, param, parent, parent_tab, parent_agg_p)
	parser_t *parser;
	select_t *select;
	int dest;
	int param;
	select_t *parent;
	int parent_tab;
	int *parent_agg_p;
{
	int i, j, col;
	vdbe_t *v;
	where_info_t *where_info;
	int agg_p = 0;              /* True for select lists like "count(*)" */
	expr_list_t *elist;         /* List of columns to extract. */
	src_list_t *tables;         /* List of tables to select from */
	expr_t *where;              /* The WHERE clause.  May be NULL */
	expr_list_t *orderby_clause;/* The ORDER BY clause.  May be NULL */
	expr_list_t *groupby_clause;/* The GROUP BY clause.  May be NULL */
	expr_t *having_clause;      /* The HAVING clause.  May be NULL */
	int distinct_p;             /* True if DISTINCT keyword is present */
	int distinct;               /* Table to use for the distinct set */
	int rc = 1;                 /* Value to return from this function */
	expr_t *e;
	const char *saveauth_context;
	int need_restore_context_p;
	func_def_t *func;
	int lbl1;
	int endagg, startagg;

	if (parser->rc == ENOMEM || parser->nErr || select == 0)
		return 1;
	if (__auth_check(parser, DBSQL_SELECT, 0, 0, 0))
		return 1;

	/*
	 * If there is are a sequence of queries, do the earlier ones first.
	 */
	if (select->pPrior) {
		return __multi_select(parser, select, dest, param);
	}

	/*
	 * Make local copies of the parameters for this query.
	 */
	tables = select->pSrc;
	where = select->pWhere;
	orderby_clause = select->pOrderBy;
	groupby_clause = select->pGroupBy;
	having_clause = select->pHaving;
	distinct_p = select->isDistinct;

	/*
	 * Allocate VDBE cursors for each table in the FROM clause.
	 */
	__src_list_assign_cursors(parser, tables);

	/*
	 * Do not even attempt to generate any code if we have already seen
	 * errors before this routine starts.
	 */
	if (parser->nErr > 0)
		goto select_end;

	/*
	 * Expand any "*" terms in the result set.  (For example the "*" in
	 * "SELECT * FROM t1")  The fill_in_column_list() routine also does
	 * some other housekeeping - see the header comment for details.
	 */
	if (__fill_in_column_list(parser, select)) {
		goto select_end;
	}
	where = select->pWhere;
	elist = select->pEList;
	if (elist == 0)
		goto select_end;

	/*
	 * If writing to memory or generating a set
	 * only a single column may be output.
	 */
	if ((dest == SRT_Mem || dest == SRT_Set) && elist->nExpr > 1) {
		__error_msg(parser, "only a single result allowed for "
			    "a SELECT that is part of an expression");
		goto select_end;
	}

	/*
	 * ORDER BY is ignored for some destinations.
	 */
	switch(dest) {
	case SRT_Union: /* FALLTHROUGH */
	case SRT_Except: /* FALLTHROUGH */
	case SRT_Discard:
		orderby_clause = 0;
		break;
	default:
		break;
	}

	/*
	 * At this point, we should have allocated all the cursors that we
	 * need to handle subquerys and temporary tables.  
	 *
	 * Resolve the column names and do a semantics check on all the
	 * expressions.
	 */
	for (i = 0; i < elist->nExpr; i++) {
		if (__expr_resolve_ids(parser, tables, 0, elist->a[i].pExpr)) {
			goto select_end;
		}
		if (__expr_check(parser, elist->a[i].pExpr, 1, &agg_p)) {
			goto select_end;
		}
	}
	if (where) {
		if (__expr_resolve_ids(parser, tables, elist, where)) {
			goto select_end;
		}
		if (__expr_check(parser, where, 0, 0)) {
			goto select_end;
		}
	}
	if (having_clause) {
		if (groupby_clause == 0) {
			__error_msg(parser,
			    "a GROUP BY clause is required before HAVING");
			goto select_end;
		}
		if (__expr_resolve_ids(parser, tables, elist, having_clause)) {
			goto select_end;
		}
		if (__expr_check(parser, having_clause, 1, &agg_p)) {
			goto select_end;
		}
	}
	if (orderby_clause) {
		for (i = 0; i < orderby_clause->nExpr; i++) {
			e = orderby_clause->a[i].pExpr;
			if (__expr_is_integer(e, &col) && col > 0 &&
			    col <= elist->nExpr) {
				__expr_delete(e);
				e = orderby_clause->a[i].pExpr =
					__expr_dup(elist->a[col-1].pExpr);
			}
			if (__expr_resolve_ids(parser, tables, elist, e)) {
				goto select_end;
			}
			if (__expr_check(parser, e, agg_p, 0)) {
				goto select_end;
			}
			if (__expr_is_constant(e)) {
				if (__expr_is_integer(e, &col) == 0) {
					__error_msg(parser,
						   "ORDER BY terms must not "
						   "be non-integer constants");
					goto select_end;
				} else if (col <= 0 || col > elist->nExpr) {
					__error_msg(parser,
						    "ORDER BY column number "
						    "%d out of range - should "
						    "be between 1 and %d",
						    col, elist->nExpr);
					goto select_end;
				}
			}
		}
	}
	if (groupby_clause) {
		for (i = 0; i < groupby_clause->nExpr; i++) {
			e = groupby_clause->a[i].pExpr;
			if (__expr_is_integer(e, &col) && col > 0 &&
			    col <= elist->nExpr) {
				__expr_delete(e);
				e = groupby_clause->a[i].pExpr =
					__expr_dup(elist->a[col-1].pExpr);
			}
			if (__expr_resolve_ids(parser, tables, elist, e)) {
				goto select_end;
			}
			if (__expr_check(parser, e, agg_p, 0)) {
				goto select_end;
			}
			if (__expr_is_constant(e)) {
				if (__expr_is_integer(e, &col) == 0) {
					__error_msg(parser,
						   "GROUP BY terms must not "
						   "be non-integer constants");
					goto select_end;
				} else if (col <= 0 || col > elist->nExpr) {
					__error_msg(parser,
						    "GROUP BY column number "
						    "%d out of range - should "
						    "be between 1 and %d",
						    col, elist->nExpr);
					goto select_end;
				}
			}
		}
	}

	/*
	 * Begin generating code.
	 */
	v = __parser_get_vdbe(parser);
	if (v == 0)
		goto select_end;

	/*
	 * Identify column names if we will be using them in a callback.  This
	 * step is skipped if the output is going to some other destination.
	 */
	if (dest == SRT_Callback) {
		__generate_column_names(parser, tables, elist);
	}

	/*
	 * Check for the special case of a min() or max() function by itself
	 * in the result set.
	 */
	if (__min_max_query(parser, select, dest, param)) {
		rc = 0;
		goto select_end;
	}

	/*
	 * Generate code for all sub-queries in the FROM clause
	 */
	for (i = 0; i < tables->nSrc; i++) {
		if (tables->a[i].pSelect == 0)
			continue;
		if (tables->a[i].zName != 0) {
			saveauth_context = parser->zAuthContext;
			parser->zAuthContext = tables->a[i].zName;
			need_restore_context_p = 1;
		} else {
			need_restore_context_p = 0;
		}
		__select(parser, tables->a[i].pSelect, SRT_TempTable, 
			 tables->a[i].iCursor, select, i, &agg_p);
		if (need_restore_context_p) {
			parser->zAuthContext = saveauth_context;
		}
		tables = select->pSrc;
		where = select->pWhere;
		if (dest != SRT_Union && dest != SRT_Except &&
		    dest != SRT_Discard) {
			orderby_clause = select->pOrderBy;
		}
		groupby_clause = select->pGroupBy;
		having_clause = select->pHaving;
		distinct_p = select->isDistinct;
	}

	/*
	 * Check to see if this is a subquery that can be "flattened" into
	 * its parent.  If flattening is a possiblity, do so and return
	 * immediately.  
	 */
	if (parent && parent_agg_p &&
	    __flatten_subquery(parser, parent, parent_tab,
			       *parent_agg_p, agg_p)) {
		if (agg_p)
			*parent_agg_p = 1;
		return rc;
	}

	/*
	 * Set the limiter.
	 */
	__compute_limit_registers(parser, select);

	/*
	 * Identify column types if we will be using a callback.  This
	 * step is skipped if the output is going to a destination other
	 * than a callback.
	 *
	 * We have to do this separately from the creation of column names
	 * above because if the tables contains views then they will not
	 * have been resolved and we will not know the column types until
	 * now.
	 */
	if (dest == SRT_Callback) {
		__generate_column_types(parser, tables, elist);
	}

	/*
	 * If the output is destined for a temporary table, open that table.
	 */
	if (dest == SRT_TempTable) {
		__vdbe_add_op(v, OP_OpenTemp, param, 0);
	}

	/*
	 * Do an analysis of aggregate expressions.
	 */
	__aggregate_info_reset(parser);
	if (agg_p || groupby_clause) {
		DBSQL_ASSERT(parser->nAgg == 0);
		agg_p = 1;
		for(i = 0; i < elist->nExpr; i++) {
			if (__expr_analyze_aggregates(parser,
						      elist->a[i].pExpr)) {
				goto select_end;
			}
		}
		if (groupby_clause) {
			for (i = 0; i < groupby_clause->nExpr; i++) {
				if (__expr_analyze_aggregates(parser,
						 groupby_clause->a[i].pExpr)) {
					goto select_end;
				}
			}
		}
		if (having_clause && __expr_analyze_aggregates(parser,
					         having_clause)) {
			goto select_end;
		}
		if (orderby_clause) {
			for (i = 0; i < orderby_clause->nExpr; i++) {
				if (__expr_analyze_aggregates(parser,
						 orderby_clause->a[i].pExpr)) {
					goto select_end;
				}
			}
		}
	}

	/*
	 * Reset the aggregator.
	 */
	if (agg_p) {
		__vdbe_add_op(v, OP_AggReset, 0, parser->nAgg);
		for (i = 0; i < parser->nAgg; i++) {
			if ((func = parser->aAgg[i].pFunc) != 0 &&
			    func->xFinalize != 0) {
				__vdbe_add_op(v, OP_AggInit, 0, i);
				__vdbe_change_p3(v, -1, (char*)func,
						 P3_POINTER);
			}
		}
		if (groupby_clause == 0) {
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_add_op(v, OP_AggFocus, 0, 0);
		}
	}

	/*
	 * Initialize the memory cell to NULL.
	 */
	if (dest == SRT_Mem) {
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_add_op(v, OP_MemStore, param, 1);
	}

	/*
	 * Open a temporary table to use for the distinct set.
	 */
	if (distinct_p) {
		distinct = parser->nTab++;
		__vdbe_add_op(v, OP_OpenTemp, distinct, 1);
	} else {
		distinct = -1;
	}

	/*
	 * Begin the database scan.
	 */
	where_info = __where_begin(parser, tables, where, 0, 
				   (groupby_clause ? 0 : &orderby_clause));
	if (where_info == 0)
		goto select_end;

	/*
	 * Use the standard inner loop if we are not dealing with aggregates.
	 */
	if (!agg_p) {
		if (__select_inner_loop(parser, select, elist, 0, 0,
					orderby_clause, distinct, dest,
					param, where_info->iContinue,
					where_info->iBreak)) {
			goto select_end;
		}
	} else {
		/*
		 * If we are dealing with aggregates, then do the special
		 * aggregate processing.  
		 */
		if (groupby_clause) {
			for (i = 0; i < groupby_clause->nExpr; i++) {
				__expr_code(parser,
					    groupby_clause->a[i].pExpr);
			}
			__vdbe_add_op(v, OP_MakeKey, groupby_clause->nExpr, 0);
			__add_key_type(v, groupby_clause);
			lbl1 = __vdbe_make_label(v);
			__vdbe_add_op(v, OP_AggFocus, 0, lbl1);
			for (i = 0; i < parser->nAgg; i++) {
				if (parser->aAgg[i].isAgg)
					continue;
				__expr_code(parser, parser->aAgg[i].pExpr);
				__vdbe_add_op(v, OP_AggSet, 0, i);
			}
			__vdbe_resolve_label(v, lbl1);
		}
		for (i = 0; i < parser->nAgg; i++) {
			if (!parser->aAgg[i].isAgg)
				continue;
			e = parser->aAgg[i].pExpr;
			DBSQL_ASSERT(e->op == TK_AGG_FUNCTION);
			if (e->pList) {
				for(j = 0; j < e->pList->nExpr; j++) {
					__expr_code(parser,
						    e->pList->a[j].pExpr);
				}
			}
			__vdbe_add_op(v, OP_Integer, i, 0);
			__vdbe_add_op(v, OP_AggFunc, 0,
				      (e->pList ? e->pList->nExpr : 0));
			DBSQL_ASSERT(parser->aAgg[i].pFunc != 0);
			DBSQL_ASSERT(parser->aAgg[i].pFunc->xStep != 0);
			__vdbe_change_p3(v, -1, (char*)parser->aAgg[i].pFunc,
					 P3_POINTER);
		}
	}

	/*
	 * End the database scan loop.
	 */
	__where_end(where_info);

	/*
	 * If we are processing aggregates, we need to set up a second loop
	 * over all of the aggregate values and process them.
	 */
	if (agg_p) {
		endagg = __vdbe_make_label(v);
		startagg = __vdbe_add_op(v, OP_AggNext, 0, endagg);
		parser->useAgg = 1;
		if (having_clause) {
			__expr_if_false(parser, having_clause, startagg, 1);
		}
		if (__select_inner_loop(parser, select, elist, 0, 0,
					orderby_clause, distinct, dest,
					param, startagg, endagg)) {
			goto select_end;
		}
		__vdbe_add_op(v, OP_Goto, 0, startagg);
		__vdbe_resolve_label(v, endagg);
		__vdbe_add_op(v, OP_Noop, 0, 0);
		parser->useAgg = 0;
	}

	/*
	 * If there is an ORDER BY clause, then we need to sort the results
	 * and send them to the callback one by one.
	 */
	if (orderby_clause) {
		__generate_sort_tail(select, v, elist->nExpr, dest, param);
	}


	/*
	 * Issue a null callback if that is what the user wants.
	 */
	if (dest == SRT_Callback &&
	    (parser->useCallback == 0 ||
	     (parser->db->flags & DBSQL_NullCallback) != 0)) {
		__vdbe_add_op(v, OP_NullCallback, elist->nExpr, 0);
	}

	/*
	 * The SELECT was successfully coded.   Set the return code to 0
	 * to indicate no errors.
	 */
	rc = 0;

	/*
	 * Control jumps to here if an error is encountered above, or upon
	 * successful coding of the SELECT.
	 */
  select_end:
	__aggregate_info_reset(parser);
	return rc;
}

/*
 * __select_result_set --
 *	Given a SELECT statement, generate a table_t structure that describes
 *	the result set of that SELECT.
 *
 * PUBLIC: table_t *__select_result_set __P((parser_t *, char *, select_t *));
 */
table_t *
__select_result_set(parser, tab_name, select)
	parser_t *parser;
	char *tab_name;
	select_t *select;
{
	int i, j, n, cnt;
	table_t *table;
	expr_list_t *elist;
	column_t *column;
	expr_t *p, *pr;
	char buf[30]; /* TODO, consider replacing */

	if (__fill_in_column_list(parser, select)) {
		return 0;
	}
	if (__dbsql_calloc(NULL, 1, sizeof(table_t), &table) == ENOMEM) {
		return 0;
	}
	table->zName = 0;
	if (tab_name)
		if (__dbsql_strdup(NULL, tab_name, &table->zName) == ENOMEM) {
			__dbsql_free(NULL, table);
			return 0;
		}
	elist = select->pEList;
	table->nCol = elist->nExpr;
	DBSQL_ASSERT(table->nCol > 0);
	__dbsql_calloc(NULL, table->nCol, sizeof(table->aCol[0]),
		       &table->aCol);
	column = table->aCol;
	for (i = 0; i < table->nCol; i++) {
		if (elist->a[i].zName) {
			__dbsql_strdup(NULL, elist->a[i].zName, &column[i].zName);
		} else if ((p = elist->a[i].pExpr)->op == TK_DOT &&
			   (pr = p->pRight) != 0 &&
			   pr->token.z && pr->token.z[0]) {
			__str_nappend(&column[i].zName, pr->token.z,
				      pr->token.n, NULL);
			for (j = cnt = 0; j < i; j++) {
				if (strcasecmp(column[j].zName,
						  column[i].zName) == 0) {
					sprintf(buf, "_%d", ++cnt);
					n = strlen(buf);
					__str_nappend(&column[i].zName,
						      pr->token.z,
						      pr->token.n, buf,
						      n, NULL);
					j = -1;
				}
			}
		} else if (p->span.z && p->span.z[0]) {
			__str_nappend(&table->aCol[i].zName, p->span.z,
				      p->span.n, NULL);
		} else {
			sprintf(buf, "column%d", i+1);
			__dbsql_strdup(NULL, buf, &table->aCol[i].zName);
		}
	}
	table->iPKey = -1;
	return table;
}

