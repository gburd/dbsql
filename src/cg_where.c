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
 * This file contains routines that handle WHERE statements.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * The query generator uses an array of instances of this structure to
 * help it analyze the subexpressions of the WHERE clause.  Each WHERE
 * clause subexpression is separated from the others by an AND operator.
 */
typedef struct expr_info {
	expr_t *p;              /* Pointer to the subexpression */
	u_int8_t indexable;     /* True if this subexprssion is usable
				   by an index */
	short int idxLeft;      /* p->pLeft is a column in this table
				   number. -1 if p->pLeft is not the
				   column of any table */
	short int idxRight;     /* p->pRight is a column in this table
				   number. -1 if p->pRight is not the
				   column of any table */
	unsigned prereqLeft;    /* Bitmask of tables referenced by p->pLeft */
	unsigned prereqRight;   /* Bitmask of tables referenced by p->pRight */
	unsigned prereqAll;     /* Bitmask of tables referenced by p */
} expr_info_t;

/*
 * An instance of the following structure keeps track of a mapping
 * between VDBE cursor numbers and bitmasks.  The VDBE cursor numbers
 * are small integers contained in src_list_item.iCursor and Expr.iTable
 * fields.  For any given WHERE clause, we want to track which cursors
 * are being used, so we assign a single bit in a 32-bit word to track
 * that cursor.  Then a 32-bit integer is able to show the set of all
 * cursors being used.
 */
typedef struct expr_mask_set {
	int n;          /* Number of assigned cursor values */
	int ix[32];     /* Cursor assigned to each bit */
} expr_mask_set_t;

/* __expr_split --
 *	This routine is used to divide the WHERE expression into subexpressions
 *	separated by the AND operator.
 *
 *	slot[] is an array of subexpressions structures.  There are num_slot
 *	spaces left in this array.  This routine attempts to split expr into
 *	subexpressions and fills slot[] with those subexpressions.
 *	The return value is the number of slots filled.
 *
 * STATIC: static int __expr_split __P((int, expr_info_t *, expr_t *));
 */
static int
__expr_split(num_slot, slot, expr)
	int num_slot;
	expr_info_t *slot;
	expr_t *expr;
{
	int cnt = 0;
	if (expr == 0 || num_slot < 1)
		return 0;
	if (num_slot == 1 || expr->op != TK_AND) {
		slot[0].p = expr;
		return 1;
	}
	if (expr->pLeft->op != TK_AND) {
		slot[0].p = expr->pLeft;
		cnt = 1 + __expr_split((num_slot - 1), &slot[1], expr->pRight);
	} else {
		cnt = __expr_split(num_slot, slot, expr->pLeft);
		cnt += __expr_split(num_slot-cnt, &slot[cnt], expr->pRight);
	}
	return cnt;
}

/*
 * __get_cursor_bitmask --
 *	Return the bitmask for the given cursor.  Assign a new bitmask
 *	if this is the first time the cursor has been seen.
 *
 * STATIC: static int __get_cursor_bitmask __P((expr_mask_set_t *, int));
 */
static int
__get_cursor_bitmask(mask_set, cursor)
	expr_mask_set_t *mask_set;
	int cursor;
{
	int i;
	for (i = 0; i < mask_set->n; i++) {
		if (mask_set->ix[i] == cursor)
			return (1 << i);
	}
	if (i == mask_set->n && i < ARRAY_SIZE(mask_set->ix)) {
		mask_set->n++;
		mask_set->ix[i] = cursor;
		return (1 << i);
	}
	return 0;
}

/*
 * __free_mask_set --
 *	Destroy an expression mask set.  This is a no-op.
 */
#define __free_mask_set(P)

/*
 * __expr_table_usage --
 *	This routine walks (recursively) an expression tree and generates
 *	a bitmask indicating which tables are used in that expression
 *	tree.
 *
 *	In order for this routine to work, the calling function must have
 *	previously invoked __expr_resolve_ids() on the expression.  See
 *	the header comment on that routine for additional information.
 *	The __expr_resolve_ids() routines looks for column names and
 *	sets their opcodes to TK_COLUMN and their expr_t.iTable fields to
 *	the VDBE cursor number of the table.
 *
 * STATIC: static int __expr_table_usage __P((expr_mask_set_t *, expr_t *));
 */
static int
__expr_table_usage(mask_set, p)
	expr_mask_set_t *mask_set;
	expr_t *p;
{
	int i;
	unsigned int mask = 0;
	if(p == 0)
		return 0;
	if (p->op == TK_COLUMN) {
		return __get_cursor_bitmask(mask_set, p->iTable);
	}
	if (p->pRight) {
		mask = __expr_table_usage(mask_set, p->pRight);
	}
	if (p->pLeft) {
		mask |= __expr_table_usage(mask_set, p->pLeft);
	}
	if (p->pList) {
		for(i = 0; i < p->pList->nExpr; i++) {
			mask |= __expr_table_usage(mask_set,
						   p->pList->a[i].pExpr);
		}
	}
	return mask;
}

/*
 * __allowed_op --
 *	Return TRUE if the given operator is one of the operators that is
 *	allowed for an indexable WHERE clause.  The allowed operators are
 *	"=", "<", ">", "<=", ">=", and "IN".
 *
 * STATIC: static int __allowed_op __P((int));
 */
static int
__allowed_op(op)
	int op;
{
	switch(op) {
	case TK_LT: /* FALLTHROUGH */
	case TK_LE: /* FALLTHROUGH */
	case TK_GT: /* FALLTHROUGH */
	case TK_GE: /* FALLTHROUGH */
	case TK_EQ: /* FALLTHROUGH */
	case TK_IN:
		return 1;
	default:
		return 0;
	}
}

/*
 * __expr_analyze --
 *	The input to this routine is an expr_info_t structure with only the
 *	"p" field filled in.  The job of this routine is to analyze the
 *	subexpression and populate all the other fields of the expr_info_t
 *	structure.
 *
 * STATIC: static void expr_analyze __P((expr_mask_set_t *, expr_info_t *));
 */
static void
__expr_analyze(mask_set, info)
	expr_mask_set_t *mask_set;
	expr_info_t *info;
{
	expr_t *expr = info->p;
	info->prereqLeft = __expr_table_usage(mask_set, expr->pLeft);
	info->prereqRight = __expr_table_usage(mask_set, expr->pRight);
	info->prereqAll = __expr_table_usage(mask_set, expr);
	info->indexable = 0;
	info->idxLeft = -1;
	info->idxRight = -1;
	if (__allowed_op(expr->op) &&
	    (info->prereqRight & info->prereqLeft) == 0) {
		if (expr->pRight && expr->pRight->op == TK_COLUMN) {
			info->idxRight = expr->pRight->iTable;
			info->indexable = 1;
		}
		if (expr->pLeft->op == TK_COLUMN) {
			info->idxLeft = expr->pLeft->iTable;
			info->indexable = 1;
		}
	}
}

/*
 * __find_sorting_index --
 *	'orderby_clause' is an ORDER BY clause from a SELECT statement.
 *	'table' is the left-most table in the FROM clause of that same
 *	SELECT statement and the 'table' has a cursor number of 'base'.
 *
 *	This routine attempts to find an index for table that generates the
 *	correct record sequence for the given ORDER BY clause.  The return
 *	value is a pointer to an index that does the job.  NULL is returned
 *	if the table has no index that will generate the correct sort order.
 *
 *	If there are two or more indices that generate the correct sort order
 *	and 'preferred_idx' is one of those indices, then return
 *	'preferred_idx'.
 *
 *	'num_eq_col' is the number of columns of 'preferred_idx' that are
 *	used as equality constraints.  Any index returned must have exactly
 *	this same set of columns.  The ORDER BY clause only matches index
 *	columns beyond the the first 'num_eq_col columns'.
 *
 *	All terms of the ORDER BY clause must be either ASC or DESC.  The
 *	'*orderby_desc_p' value is set to 1 if the ORDER BY clause is all
 *	DESC and it is set to 0 if the ORDER BY clause is all ASC.
 *
 * STATIC: static index_t *__find_sorting_index __P((table_t *, int,
 * STATIC:                                      expr_list_t *, index_t *, int,
 * STATIC:                                      int));
 *
 * table			The table to be sorted
 * base				Cursor number for table
 * orderby_clause		The ORDER BY clause
 * preferred_idx		Use this index, if possible and not NULL
 * num_eq_col			Number of index columns used with
 *				== constraints
 * orderby_desc_p		Set to 1 if ORDER BY is DESC
 */
static index_t *__find_sorting_index(table, base, orderby_clause,
				     preferred_idx, num_eq_col, orderby_desc_p)
	table_t *table;
	int base;
	expr_list_t *orderby_clause;
	index_t *preferred_idx;
	int num_eq_col;
	int *orderby_desc_p;
{
	int i, j;
	index_t *match;
	index_t *idx;
	int sort_order;
	expr_t *p;
	int num_expr;

	DBSQL_ASSERT(orderby_clause != 0);
	DBSQL_ASSERT(orderby_clause->nExpr > 0);
	sort_order = (orderby_clause->a[0].sortOrder & DBSQL_SO_DIRMASK);
	for (i = 0; i < orderby_clause->nExpr; i++) {
		if ((orderby_clause->a[i].sortOrder & DBSQL_SO_DIRMASK) !=
		    sort_order) {
			/*
			 * Indices can only be used if all ORDER BY terms are
			 * either DESC or ASC.  Indices cannot be used on a
			 * mixture.
			 */
			return 0;
		}
		if ((orderby_clause->a[i].sortOrder & DBSQL_SO_TYPEMASK) !=
		    DBSQL_SO_UNK) {
			/* Do not sort by index if there is a COLLATE clause */
			return 0;
		}
		p = orderby_clause->a[i].pExpr;
		if (p->op != TK_COLUMN || p->iTable != base) {
			/*
			 * Can not use an index sort on anything that is not
			 * a column in the left-most table of the FROM clause.
			 */
			return 0;
		}
	}
  
	/*
	 * If we get this far, it means the ORDER BY clause consists only of
	 * ascending columns in the left-most table of the FROM clause.  Now
	 * check for a matching index.
	 */
	match = 0;
	for (idx = table->pIndex; idx; idx = idx->pNext) {
		num_expr = orderby_clause->nExpr;
		if (idx->nColumn < num_eq_col || idx->nColumn < num_expr)
			continue;
		for (i = j = 0; i < num_eq_col; i++) {
			if (preferred_idx->aiColumn[i] != idx->aiColumn[i])
				break;
			if (j < num_expr &&
			    (orderby_clause->a[j].pExpr->iColumn ==
			     idx->aiColumn[i])) {
				j++;
			}
		}
		if (i < num_eq_col)
			continue;
		for (i = 0; (i + j) < num_expr; i++) {
			if (orderby_clause->a[(i + j)].pExpr->iColumn !=
			    idx->aiColumn[(i + num_eq_col)])
				break;
		}
		if ((i + j) >= num_expr) {
			match = idx;
			if (idx == preferred_idx)
				break;
		}
	}
	if (match && orderby_desc_p) {
		*orderby_desc_p = (sort_order==DBSQL_SO_DESC);
	}
	return match;
}

/*
 * __where_begin --
 *	Generate the beginning of the loop used for WHERE clause processing.
 *	The return value is a pointer to an (opaque) structure that contains
 *	information needed to terminate the loop.  Later, the calling routine
 *	should invoke __where_end() with the return value of this function
 *	in order to complete the WHERE clause processing.
 *
 *	If an error occurs, this routine returns NULL.
 *
 *	The basic idea is to do a nested loop, one loop for each table in
 *	the FROM clause of a select.  (INSERT and UPDATE statements are the
 *	same as a SELECT with only a single table in the FROM clause.)  For
 *	example, if the SQL is this:
 *
 *	      SELECT * FROM t1, t2, t3 WHERE ...;
 *
 *	Then the code generated is conceptually like the following:
 *
 *	     foreach row1 in t1 do       \    Code generated
 *	        foreach row2 in t2 do      |-- by __where_begin()
 *	          foreach row3 in t3 do   /
 *	            ...
 *	          end                     \    Code generated
 *	        end                        |-- by __where_end()
 *	      end                         /
 *
 *	There are Btree cursors associated with each table.  t1 uses cursor
 *	number tab_list->a[0].iCursor.  t2 uses the cursor
 *	tab_list->a[1].iCursor.  And so forth.  This routine generates code
 *	to open those VDBE cursors and __where_end() generates the code to
 *	close them.
 *
 *	If the WHERE clause is empty, the foreach loops must each scan their
 *	entire tables.  Thus a three-way join is an O(N^3) operation.  But if
 *	the tables have indices and there are terms in the WHERE clause that
 *	refer to those indices, a complete table scan can be avoided and the
 *	code will run much faster.  Most of the work of this routine is
 *	checking to see if there are indices that can be used to speed up
 *	the loop.
 *
 *	Terms of the WHERE clause are also used to limit which rows actually
 *	make it to the "..." in the middle of the loop.  After each "foreach",
 *	terms of the WHERE clause that use only terms in that loop and outer
 *	loops are evaluated and if false a jump is made around all subsequent
 *	inner loops (or around the "..." if the test occurs within the inner-
 *	most loop)
 *
 *	OUTER JOINS
 *
 *	An outer join of tables t1 and t2 is conceptally coded as follows:
 *
 *	    foreach row1 in t1 do
 *	      flag = 0
 *	      foreach row2 in t2 do
 *	        start:
 *	          ...
 *	          flag = 1
 *	      end
 *	      if flag==0 then
 *	        move the row2 cursor to a null row
 *	        goto start
 *	      fi
 *	    end
 *
 *	 ORDER BY CLAUSE PROCESSING
 *
 *	*orderby_clause is a pointer to the ORDER BY clause of a SELECT
 *	statement, if there is one.  If there is no ORDER BY clause or if
 *	this routine is called from an UPDATE or DELETE statement, then
 *	orderby_clause is NULL.
 *
 *	If an index can be used so that the natural output order of the table
 *	scan is correct for the ORDER BY clause, then that index is used and
 *	*orderby_clause is set to NULL.  This is an optimization that prevents
 *	an unnecessary sort of the result set if an index appropriate for the
 *	ORDER BY clause already exists.
 *
 *	If the where clause loops cannot be arranged to provide the correct
 *	output order, then the *orderby_clause is unchanged.
 *
 * PUBLIC: where_info_t *__where_begin __P((parser_t *, src_list_t *, expr_t *,
 * PUBLIC:                             int, expr_list_t **));
 *
 * parser			The parser context
 * tab_list			A list of all tables to be scanned
 * where_clause			The WHERE clause
 * push_key_p			If TRUE, leave the table key on the stack
 * orderby_clause		An ORDER BY clause, or NULL
 */
where_info_t *__where_begin(parser, tab_list, where_clause, push_key_p,
			    orderby_clause)
	parser_t *parser;
	src_list_t *tab_list;
	expr_t *where_clause;
	int push_key_p;
	expr_list_t **orderby_clause;
{
	int i;                    /* Loop counter */
	where_info_t *where_info; /* Will become the return value of this
				     function */
	vdbe_t *v = parser->pVdbe;/* The virtual database engine */
	int brk, cont = 0;        /* Addresses used during code generation */
	int num_expr;             /* Number of subexpressions in the WHERE
				     clause */
	int loop_mask;            /* One bit set for each outer loop */
	int have_key_p;           /* True if KEY is on the stack */
	expr_mask_set_t mask_set; /* The expression mask set */
	int direct_eq[32];        /* Term of the form ROWID==X for the
				     N-th table */
	int direct_lt[32];        /* Term of the form ROWID<X or ROWID<=X */
	int direct_gt[32];        /* Term of the form ROWID>X or ROWID>=X */
	expr_info_t wc_exprs[101];/* The WHERE clause is divided into these
				     expressions */
	char buf[50];
	int x, mask, j, cur, best_score;
	table_t *table;
	index_t *idx, *best_idx;
	int eq_mask;  /* Index columns covered by an x=... term */
	int lt_mask;  /* Index columns covered by an x<... term */
	int gt_mask;  /* Index columns covered by an x>... term */
	int in_mask;  /* Index columns covered by an x IN .. term */
	int num_eq, m, score;
	int col, k;
	index_t *sort_idx;
	int rev = 0;
	int num_col_eq;
	where_level_t *level;
	expr_t *ex, *expr;
	int start, test_op, col_num;
	int eqcols;
	int le_flag, ge_flag;

	/*
	 * 'push_key_p' is only allowed if there is a single table
	 * (as in an INSERT or UPDATE statement).
	 */
	DBSQL_ASSERT(push_key_p == 0 || tab_list->nSrc == 1);

	/*
	 * Split the WHERE clause into separate subexpressions where each
	 * subexpression is separated by an AND operator.  If the wc_exprs[]
	 * array fills up, the last entry might point to an expression which
	 * contains additional unfactored AND operators.
	 */
	memset(&mask_set, 0, sizeof(mask_set));
	memset(wc_exprs, 0, sizeof(wc_exprs));
	num_expr = __expr_split(ARRAY_SIZE(wc_exprs), wc_exprs, where_clause);
	if (num_expr == ARRAY_SIZE(wc_exprs)) {
		sprintf(buf, "%d", (int)(ARRAY_SIZE(wc_exprs) - 1));
		__str_append(&parser->zErrMsg,
			     "WHERE clause too complex - no more "
			     "than ", buf, " terms allowed", (char*)0);
		parser->nErr++;
		return 0;
	}
  
	/*
	 * Allocate and initialize the where_info_t structure that will
	 * become the return value.
	 */
	if (__dbsql_calloc(parser->db, 1, sizeof(where_info_t) +
		    (tab_list->nSrc * sizeof(where_level_t)),
			&where_info) == ENOMEM) {
		__dbsql_free(parser->db, where_info);
		return 0;
	}
	where_info->pParse = parser;
	where_info->pTabList = tab_list;
	where_info->peakNTab = where_info->savedNTab = parser->nTab;
	where_info->iBreak = __vdbe_make_label(v);

	/*
	 * Special case: a WHERE clause that is constant.  Evaluate the
	 * expression and either jump over all of the code or fall thru.
	 */
	if (where_clause &&
	    (tab_list->nSrc == 0 || __expr_is_constant(where_clause))) {
		__expr_if_false(parser, where_clause, where_info->iBreak, 1);
		where_clause = 0;
	}

	/*
	 * Analyze all of the subexpressions.
	 */
	for(i = 0; i < num_expr; i++) {
		__expr_analyze(&mask_set, &wc_exprs[i]);

		/*
		 * If we are executing a trigger body, remove all references
		 * to new.* and old.* tables from the prerequisite masks.
		 */
		if (parser->trigStack) {
			if ((x = parser->trigStack->newIdx) >= 0 ) {
				mask = ~__get_cursor_bitmask(&mask_set, x);
				wc_exprs[i].prereqRight &= mask;
				wc_exprs[i].prereqLeft &= mask;
				wc_exprs[i].prereqAll &= mask;
			}
			if ((x = parser->trigStack->oldIdx) >= 0) {
				mask = ~__get_cursor_bitmask(&mask_set, x);
				wc_exprs[i].prereqRight &= mask;
				wc_exprs[i].prereqLeft &= mask;
				wc_exprs[i].prereqAll &= mask;
			}
		}
	}

	/*
	 * Figure out what index to use (if any) for each nested loop.
	 * Make where_info->a[i].pIdx point to the index to use for the i-th
	 * nested loop where i==0 is the outer loop and i==tab_list->nSrc-1
	 * is the inner loop. 
	 *
	 * If terms exist that use the ROWID of any table, then set the
	 * direct_eq[], direct_lt[], or direct_gt[] elements for that table
	 * to the index of the term containing the ROWID.  We always prefer
	 * to use a ROWID which can directly access a table rather than an
	 * index which requires reading an index first to get the rowid then
	 * doing a second read of the actual database table.
	 *
	 * Actually, if there are more than 32 tables in the join, only the
	 * first 32 tables are candidates for indices.  This is (again) due
	 * to the limit of 32 bits in an integer bitmask.
	 */
	loop_mask = 0;
	for(i = 0; i < tab_list->nSrc && i < ARRAY_SIZE(direct_eq); i++) {
		cur = tab_list->a[i].iCursor; /* The cursor for this table */
		mask = __get_cursor_bitmask(&mask_set, cur);
		table = tab_list->a[i].pTab;
		best_idx = 0;
		best_score = 0;

		/*
		 * Check to see if there is an expression that uses only the
		 * ROWID field of this table.  For terms of the form
		 * ROWID==expr set direct_eq[i] to the index of the term.
		 * For terms of the form ROWID<expr or ROWID<=expr set
		 * direct_lt[i] to the term index.
		 * For terms like ROWID>expr or ROWID>=expr set direct_gt[i].
		 *
		 * Additionally, treat ROWID IN expr like ROWID=expr.
		 */
		 where_info->a[i].iCur = -1;
		 direct_eq[i] = -1;
		 direct_lt[i] = -1;
		 direct_gt[i] = -1;
		 for (j = 0; j < num_expr; j++) {
			 if (wc_exprs[j].idxLeft == cur &&
			     wc_exprs[j].p->pLeft->iColumn < 0 &&
			     ((wc_exprs[j].prereqRight & loop_mask) ==
			      wc_exprs[j].prereqRight)) {
				 switch(wc_exprs[j].p->op) {
				 case TK_IN: /* FALLTHROUGH */
				 case TK_EQ:
					 direct_eq[i] = j;
					 break;
				 case TK_LE: /* FALLTHROUGH */
				 case TK_LT:
					 direct_lt[i] = j;
					 break;
				 case TK_GE: /* FALLTHROUGH */
				 case TK_GT:
					 direct_gt[i] = j;
					 break;
				 }
			 }
			 if (wc_exprs[j].idxRight == cur &&
			     wc_exprs[j].p->pRight->iColumn < 0 &&
			     ((wc_exprs[j].prereqLeft & loop_mask) ==
			      wc_exprs[j].prereqLeft)) {
				 switch(wc_exprs[j].p->op) {
				 case TK_EQ:
					 direct_eq[i] = j;
					 break;
				 case TK_LE:  /* FALLTHROUGH */
				 case TK_LT:
					 direct_gt[i] = j;
					 break;
				 case TK_GE: /* FALLTHROUGH */
				 case TK_GT:
					 direct_lt[i] = j;
					 break;
				 }
			 }
		 }
		 if (direct_eq[i] >= 0) {
			 loop_mask |= mask;
			 where_info->a[i].pIdx = 0;
			 continue;
		 }

		 /*
		  * Do a search for usable indices.  Leave 'best_idx' pointing
		  * to the "best" index.  'best_idx' is left set to NULL if no
		  * indices are usable.
		  *
		  * The best index is determined as follows.  For each of the
		  * left-most terms that is fixed by an equality operator, add
		  * 8 to the score.  The right-most term of the index may be
		  * constrained by an inequality.  Add 1 if for an
		  * "x<..." constraint and add 2 for an "x>..." constraint.
		  * Chose the index that gives the best score.
		  *
		  * This scoring system is designed so that the score can
		  * later be used to determine how the index is used.  If the
		  * score&7 is 0 then all constraints are equalities.  If
		  * score&1 is not 0 then there is an inequality used as a
		  * termination key.  (ex: "x<...") If score&2 is not 0 then
		  * there is an inequality used as the start key.
		  * (ex: "x>...").  A score or 4 is the special case of an IN
		  * operator constraint.  (ex:  "x IN ...").
		  *
		  * The IN operator (as in "<expr> IN (...)") is treated the
		  * same as an equality comparison except that it can only be
		  * used on the left-most column of an index and other terms
		  * of the WHERE clause cannot be used in conjunction with the
		  * IN operator to help satisfy other columns of the index.
		  */
		 for(idx = table->pIndex; idx; idx = idx->pNext) {
			 eq_mask = 0;
			 lt_mask = 0;
			 gt_mask = 0;
			 in_mask = 0;
			 if (idx->nColumn > 32)
				 continue;/* Ignore indices too many columns */
			 for (j = 0; j < num_expr; j++) {
				 if (wc_exprs[j].idxLeft == cur &&
				     ((wc_exprs[j].prereqRight & loop_mask) ==
				      wc_exprs[j].prereqRight)) {
					 col = wc_exprs[j].p->pLeft->iColumn;
					 for (k = 0; k < idx->nColumn; k++) {
					 if (idx->aiColumn[k] == col) {
						 switch(wc_exprs[j].p->op){
						 case TK_IN:
							 if(k == 0)
								 in_mask |= 1;
							 break;
						 case TK_EQ:
							 eq_mask |= 1<<k;
							 break;
						 case TK_LE: /* FALLTHROUGH */
						 case TK_LT:
							 lt_mask |= 1<<k;
							 break;
						 case TK_GE: /* FALLTHROUGH */
						 case TK_GT:
							 gt_mask |= 1<<k;
							 break;
						 default:
							 DBSQL_ASSERT(0);
							 break;
						 }
						 break;
					 }
					 }
				 }
				 if (wc_exprs[j].idxRight == cur && 
				     ((wc_exprs[j].prereqLeft & loop_mask) ==
				      wc_exprs[j].prereqLeft)) {
					 col = wc_exprs[j].p->pRight->iColumn;
					 for (k = 0; k < idx->nColumn; k++) {
					 if (idx->aiColumn[k] == col) {
						 switch(wc_exprs[j].p->op) {
						 case TK_EQ:
							 eq_mask |= 1<<k;
							 break;
						 case TK_LE: /* FALLTHROUGH */
						 case TK_LT:
							 gt_mask |= 1<<k;
							 break;
						 case TK_GE: /* FALLTHROUGH */
						 case TK_GT:
							 lt_mask |= 1<<k;
							 break;
						 default:
							 DBSQL_ASSERT(0);
							 break;
						 }
						 break;
					 }
					 }
				 }
			 }

			 /*
			  * The following loop ends with num_eq set to the
			  * number of columns on the left of the index with
			  * == constraints.
			  */
			 for (num_eq = 0; num_eq < idx->nColumn; num_eq++) {
				 m = (1 << (num_eq + 1)) - 1;
				 if ((m & eq_mask) != m)
					 break;
			 }
			 score = (num_eq * 8);  /* Base score is 8 times
						   number of == constraints. */
			 m = (1 << num_eq);
			 if (m & lt_mask)
				 score++;       /* Increase score for a
						   < constraint. */
			 if (m & gt_mask)
				 score+=2;      /* Increase score for a
						   > constraint. */
			 if (score == 0 && in_mask)
				 score = 4;     /* Default score for IN
						   constraint. */
			 if (score > best_score ) {
				 best_idx = idx;
				 best_score = score;
			 }
		 }
		 where_info->a[i].pIdx = best_idx;
		 where_info->a[i].score = best_score;
		 where_info->a[i].bRev = 0;
		 loop_mask |= mask;
		 if (best_idx) {
			 where_info->a[i].iCur = parser->nTab++;
			 where_info->peakNTab = parser->nTab;
		 }
	}

	/*
	 * Check to see if the ORDER BY clause is or can be satisfied by the
	 * use of an index on the first table.
	 */
	if (orderby_clause && *orderby_clause && tab_list->nSrc > 0) {
		rev = 0;
		table = tab_list->a[0].pTab;
		idx = where_info->a[0].pIdx;
		if (idx && where_info->a[0].score == 4) {
			/*
			 * If there is already an IN index on the left-most
			 * table, it will not give the correct sort order.
			 * So, pretend that no suitable index is found.
			 */
			sort_idx = 0;
		} else if (direct_eq[0] >= 0 || direct_lt[0] >= 0 ||
			   direct_gt[0] >= 0) {
			/*
			 * If the left-most column is accessed using its
			 * ROWID, then donot try to sort by index.
			 */
			sort_idx = 0;
		} else {
			num_col_eq = (where_info->a[0].score + 4) / 8;
			sort_idx = __find_sorting_index(table,
						tab_list->a[0].iCursor, 
							*orderby_clause, idx,
							num_col_eq, &rev);
		}
		if (sort_idx && (idx == 0 || idx == sort_idx)) {
			if (idx == 0) {
				where_info->a[0].pIdx = sort_idx;
				where_info->a[0].iCur = parser->nTab++;
				where_info->peakNTab = parser->nTab;
			}
			where_info->a[0].bRev = rev;
			*orderby_clause = 0;
		}
	}

	/*
	 * Open all tables in the tab_list and all indices used by those
	 * tables.
	 */
	for (i = 0; i < tab_list->nSrc; i++) {
		table = tab_list->a[i].pTab;
		if (table->isTransient || table->pSelect )
			continue;
		__vdbe_add_op(v, OP_Integer, table->iDb, 0);
		__vdbe_add_op(v, OP_OpenRead, tab_list->a[i].iCursor,
			      table->tnum);
		__vdbe_change_p3(v, -1, table->zName, P3_STATIC);
		__code_verify_schema(parser, table->iDb);
		if (where_info->a[i].pIdx != 0) {
			__vdbe_add_op(v, OP_Integer,
				      where_info->a[i].pIdx->iDb, 0);
			__vdbe_add_op(v, OP_OpenRead, where_info->a[i].iCur,
				      where_info->a[i].pIdx->tnum);
			__vdbe_change_p3(v, -1, where_info->a[i].pIdx->zName,
					 P3_STATIC);
		}
	}

	/*
	 * Generate the code to do the search.
	 */
	loop_mask = 0;
	for (i = 0; i < tab_list->nSrc; i++) {
		cur = tab_list->a[i].iCursor;
		level = &where_info->a[i];

		/*
		 * If this is the right table of a LEFT OUTER JOIN, allocate
		 * and initialize a memory cell that records if this table
		 * matches any row of the left table of the join.
		 */
		if (i > 0 && (tab_list->a[(i - 1)].jointype & JT_LEFT) != 0) {
			if (!parser->nMem)
				parser->nMem++;
			level->iLeftJoin = parser->nMem++;
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_add_op(v, OP_MemStore, level->iLeftJoin, 1);
		}

		idx = level->pIdx;
		level->inOp = OP_Noop;
		if (i < ARRAY_SIZE(direct_eq) && direct_eq[i] >= 0) {
			/*
			 * Case 1:  We can directly reference a single row
			 * using an equality comparison against the ROWID
			 * field.  Or we reference multiple rows using a
			 * "rowid IN (...)" construct.
			 */
			k = direct_eq[i];
			DBSQL_ASSERT(k < num_expr);
			DBSQL_ASSERT(wc_exprs[k].p != 0);
			DBSQL_ASSERT(wc_exprs[k].idxLeft == cur ||
			       wc_exprs[k].idxRight == cur);
			brk = level->brk = __vdbe_make_label(v);
			if (wc_exprs[k].idxLeft == cur) {
				ex = wc_exprs[k].p;
				if (ex->op != TK_IN) {
					__expr_code(parser,
						    wc_exprs[k].p->pRight);
				} else if (ex->pList) {
					__vdbe_add_op(v, OP_SetFirst,
						      ex->iTable, brk);
					level->inOp = OP_SetNext;
					level->inP1 = ex->iTable;
					level->inP2 = __vdbe_current_addr(v);
				} else {
					DBSQL_ASSERT(ex->pSelect);
					__vdbe_add_op(v, OP_Rewind,
						      ex->iTable, brk);
					__vdbe_add_op(v, OP_KeyAsData,
						      ex->iTable, 1);
					level->inP2 =
						__vdbe_add_op(v, OP_FullKey,
							      ex->iTable, 0);
					level->inOp = OP_Next;
					level->inP1 = ex->iTable;
				}
			} else {
				__expr_code(parser, wc_exprs[k].p->pLeft);
			}
			wc_exprs[k].p = 0;
			cont = level->cont = __vdbe_make_label(v);
			__vdbe_add_op(v, OP_MustBeInt, 1, brk);
			have_key_p = 0;
			__vdbe_add_op(v, OP_NotExists, cur, brk);
			level->op = OP_Noop;
		} else if (idx != 0 && level->score > 0 &&
			   ((level->score % 4) == 0)) {
			/*
			 * Case 2:  There is an index and all terms of the
			 * WHERE clause that refer to the index use the
			 * "==" or "IN" operators.
			 */
			col_num = (level->score+4)/8;
			brk = level->brk = __vdbe_make_label(v);
			for (j = 0; j < col_num; j++) {
				for (k = 0; k < num_expr; k++) {
					ex = wc_exprs[k].p;
					if (ex == 0)
						continue;
					if (wc_exprs[k].idxLeft == cur &&
					    ((wc_exprs[k].prereqRight &
					      loop_mask) ==
					     wc_exprs[k].prereqRight) &&
					    (ex->pLeft->iColumn == 
					     idx->aiColumn[j])) {
						if (ex->op == TK_EQ) {
							__expr_code(parser,
								   ex->pRight);
							wc_exprs[k].p = 0;
							break;
						}
						if (ex->op == TK_IN &&
						    col_num == 1) {
							if (ex->pList) {
								__vdbe_add_op(v, OP_SetFirst, ex->iTable, brk);
								level->inOp = OP_SetNext;
								level->inP1 = ex->iTable;
								level->inP2 = __vdbe_current_addr(v);
							} else {
								DBSQL_ASSERT(ex->pSelect);
								__vdbe_add_op(v, OP_Rewind, ex->iTable, brk);
								__vdbe_add_op(v, OP_KeyAsData, ex->iTable, 1);
								level->inP2 = __vdbe_add_op(v, OP_FullKey, ex->iTable, 0);
								level->inOp = OP_Next;
								level->inP1 = ex->iTable;
							}
							wc_exprs[k].p = 0;
							break;
						}
					}
					if (wc_exprs[k].idxRight ==cur &&
					    wc_exprs[k].p->op == TK_EQ &&
					    ((wc_exprs[k].prereqLeft &
					      loop_mask) ==
					     wc_exprs[k].prereqLeft) &&
					    (wc_exprs[k].p->pRight->iColumn ==
					     idx->aiColumn[j])) {
						__expr_code(parser, wc_exprs[k].p->pLeft);
						wc_exprs[k].p = 0;
						break;
					}
				}
			}
			level->iMem = parser->nMem++;
			cont = level->cont = __vdbe_make_label(v);
			__vdbe_add_op(v, OP_NotNull, -col_num,
				      (__vdbe_current_addr(v) + 3));
			__vdbe_add_op(v, OP_Pop, col_num, 0);
			__vdbe_add_op(v, OP_Goto, 0, brk);
			__vdbe_add_op(v, OP_MakeKey, col_num, 0);
			__add_idx_key_type(v, idx);
			if (col_num == idx->nColumn || level->bRev) {
				__vdbe_add_op(v, OP_MemStore, level->iMem, 0);
				test_op = OP_IdxGT;
			} else {
				__vdbe_add_op(v, OP_Dup, 0, 0);
				__vdbe_add_op(v, OP_IncrKey, 0, 0);
				__vdbe_add_op(v, OP_MemStore, level->iMem, 1);
				test_op = OP_IdxGE;
			}
			if (level->bRev) {
				/* Scan in reverse order */
				__vdbe_add_op(v, OP_IncrKey, 0, 0);
				__vdbe_add_op(v, OP_MoveLt, level->iCur, brk);
				start = __vdbe_add_op(v, OP_MemLoad,
						      level->iMem, 0);
				__vdbe_add_op(v, OP_IdxLT, level->iCur, brk);
				level->op = OP_Prev;
			} else {
				/* Scan in the forward order */
				__vdbe_add_op(v, OP_MoveTo, level->iCur, brk);
				start = __vdbe_add_op(v, OP_MemLoad,
						      level->iMem, 0);
				__vdbe_add_op(v, test_op, level->iCur, brk);
				level->op = OP_Next;
			}
			__vdbe_add_op(v, OP_RowKey, level->iCur, 0);
			__vdbe_add_op(v, OP_IdxIsNull, col_num, cont);
			__vdbe_add_op(v, OP_IdxRecno, level->iCur, 0);
			if (i == tab_list->nSrc-1 && push_key_p) {
				have_key_p = 1;
			} else {
				__vdbe_add_op(v, OP_MoveTo, cur, 0);
				have_key_p = 0;
			}
			level->p1 = level->iCur;
			level->p2 = start;
		} else if (i < ARRAY_SIZE(direct_lt) &&
			   (direct_lt[i] >= 0 || direct_gt[i] >= 0)) {
			/*
			 * Case 3:  We have an inequality comparison against
			 * the ROWID field.
			 */
			test_op = OP_Noop;
			brk = level->brk = __vdbe_make_label(v);
			cont = level->cont = __vdbe_make_label(v);
			if (direct_gt[i] >= 0) {
				k = direct_gt[i];
				DBSQL_ASSERT(k < num_expr);
				DBSQL_ASSERT(wc_exprs[k].p != 0);
				DBSQL_ASSERT(wc_exprs[k].idxLeft == cur ||
				       wc_exprs[k].idxRight == cur);
				if (wc_exprs[k].idxLeft == cur) {
					__expr_code(parser,
						    wc_exprs[k].p->pRight);
				} else {
					__expr_code(parser,
						    wc_exprs[k].p->pLeft);
				}
				__vdbe_add_op(v, OP_ForceInt,
					      ((wc_exprs[k].p->op == TK_LT) ||
					       (wc_exprs[k].p->op == TK_GT)),
					      brk);
				__vdbe_add_op(v, OP_MoveTo, cur, brk);
				wc_exprs[k].p = 0;
			} else {
				__vdbe_add_op(v, OP_Rewind, cur, brk);
			}
			if (direct_lt[i] >= 0) {
				k = direct_lt[i];
				DBSQL_ASSERT(k < num_expr);
				DBSQL_ASSERT(wc_exprs[k].p != 0);
				DBSQL_ASSERT(wc_exprs[k].idxLeft == cur ||
				       wc_exprs[k].idxRight == cur);
				if (wc_exprs[k].idxLeft == cur) {
					__expr_code(parser,
						    wc_exprs[k].p->pRight);
				} else {
					__expr_code(parser,
						    wc_exprs[k].p->pLeft);
				}
				/*TODO: __vdbe_add_op(v, OP_MustBeInt, 0, __vdbe_current_addr(v)+1); */
				level->iMem = parser->nMem++;
				__vdbe_add_op(v, OP_MemStore, level->iMem, 1);
				if (((wc_exprs[k].p->op == TK_LT) ||
				     (wc_exprs[k].p->op == TK_GT))) {
					test_op = OP_Ge;
				} else {
					test_op = OP_Gt;
				}
				wc_exprs[k].p = 0;
			}
			start = __vdbe_current_addr(v);
			level->op = OP_Next;
			level->p1 = cur;
			level->p2 = start;
			if (test_op != OP_Noop) {
				__vdbe_add_op(v, OP_Recno, cur, 0);
				__vdbe_add_op(v, OP_MemLoad, level->iMem, 0);
				__vdbe_add_op(v, test_op, 0, brk);
			}
			have_key_p = 0;
		} else if (idx == 0) {
			/*
			 * Case 4:  There is no usable index.  We must do a
			 * complete scan of the entire database table.
			 */
			brk = level->brk = __vdbe_make_label(v);
			cont = level->cont = __vdbe_make_label(v);
			__vdbe_add_op(v, OP_Rewind, cur, brk);
			start = __vdbe_current_addr(v);
			level->op = OP_Next;
			level->p1 = cur;
			level->p2 = start;
			have_key_p = 0;
		} else {
			/*
			 * Case 5: The WHERE clause term that refers to the
			 * right-most column of the index is an inequality.
			 * For example, if the index is on (x,y,z) and the
			 * WHERE clause is of the form "x=5 AND y<10" then
			 * this case is used.  Only the right-most column
			 * can be an inequality - the rest must use the
			 * "==" operator.
			 *
			 * This case is also used when there are no WHERE
			 * clause constraints but an index is selected anyway,
			 * in order to force the output order to conform to an
			 * ORDER BY.
			 */

			score = level->score;
			eqcols = score/8;

			/*
			 * Evaluate the equality constraints
			 */
			for (j = 0; j < eqcols; j++) {
				for (k = 0; k < num_expr; k++) {
					if (wc_exprs[k].p == 0)
						continue;
					if (wc_exprs[k].idxLeft == cur &&
					    wc_exprs[k].p->op == TK_EQ &&
					    ((wc_exprs[k].prereqRight &
					      loop_mask) ==
					     wc_exprs[k].prereqRight) &&
					    (wc_exprs[k].p->pLeft->iColumn ==
					     idx->aiColumn[j])) {
						__expr_code(parser,
							wc_exprs[k].p->pRight);
						wc_exprs[k].p = 0;
						break;
					}
					if (wc_exprs[k].idxRight == cur &&
					    wc_exprs[k].p->op == TK_EQ &&
					    ((wc_exprs[k].prereqLeft &
					      loop_mask) ==
					     wc_exprs[k].prereqLeft) &&
					    (wc_exprs[k].p->pRight->iColumn ==
					     idx->aiColumn[j])) {
						__expr_code(parser,
							 wc_exprs[k].p->pLeft);
						wc_exprs[k].p = 0;
						break;
					}
				}
			}

			/*
			 * Duplicate the equality term values because they
			 * will all be used twice: once to make the termination
			 * key and once to make the start key.
			 */
			for (j = 0; j < eqcols; j++) {
				__vdbe_add_op(v, OP_Dup, eqcols-1, 0);
			}

			/*
			 * Labels for the beginning and end of the loop.
			 */
			cont = level->cont = __vdbe_make_label(v);
			brk = level->brk = __vdbe_make_label(v);

			/*
			 * Generate the termination key.  This is the key
			 * value that will end the search.  There is no
			 * termination key if there are no equality terms and
			 * no "X<..." term.
			 *
			 * On a reverse-order scan, the so-called "termination"
			 * key computed here really ends up being the start
			 * key.
			 */
			if ((score & 1) != 0) {
				for (k = 0; k < num_expr; k++) {
					expr = wc_exprs[k].p;
					if (expr == 0)
						continue;
					if (wc_exprs[k].idxLeft == cur &&
					    (expr->op == TK_LT ||
					     expr->op == TK_LE) &&
					    ((wc_exprs[k].prereqRight &
					      loop_mask) ==
					     wc_exprs[k].prereqRight) &&
					    (expr->pLeft->iColumn ==
					     idx->aiColumn[j])) {
						__expr_code(parser,
							    expr->pRight);
						le_flag = expr->op==TK_LE;
						wc_exprs[k].p = 0;
						break;
					}
					if (wc_exprs[k].idxRight == cur &&
					    ((expr->op == TK_GT ||
					      expr->op == TK_GE)) &&
					    ((wc_exprs[k].prereqLeft &
					      loop_mask) ==
					     wc_exprs[k].prereqLeft) &&
					    (expr->pRight->iColumn ==
					     idx->aiColumn[j])) {
						__expr_code(parser,
							    expr->pLeft);
						le_flag = expr->op==TK_GE;
						wc_exprs[k].p = 0;
						break;
					}
				}
				test_op = OP_IdxGE;
			} else {
				test_op = (eqcols > 0) ? OP_IdxGE : OP_Noop;
				le_flag = 1;
			}
			if (test_op != OP_Noop) {
				col = eqcols + (score & 1);
				level->iMem = parser->nMem++;
				__vdbe_add_op(v, OP_NotNull, -col,
					      (__vdbe_current_addr(v) + 3));
				__vdbe_add_op(v, OP_Pop, col, 0);
				__vdbe_add_op(v, OP_Goto, 0, brk);
				__vdbe_add_op(v, OP_MakeKey, col, 0);
				__add_idx_key_type(v, idx);
				if (le_flag) {
					__vdbe_add_op(v, OP_IncrKey, 0, 0);
				}
				if (level->bRev) {
					__vdbe_add_op(v, OP_MoveLt,
						      level->iCur, brk);
				} else {
					__vdbe_add_op(v, OP_MemStore,
						      level->iMem, 1);
				}
			} else if (level->bRev) {
				__vdbe_add_op(v, OP_Last, level->iCur, brk);
			}

			/*
			 * Generate the start key.  This is the key that
			 * defines the lower bound on the search.  There is
			 * no start key if there are no equality terms and if
			 * there is no "X>..." term.  In that case, generate
			 * a "Rewind" instruction in place of the start key
			 * search.
			 *
			 * In the case of a reverse-order search, the so-called
			 * "start" key really ends up being used as the
			 * termination key.
			 */
			if ((score & 2) != 0) {
				for (k = 0; k < num_expr; k++) {
					expr = wc_exprs[k].p;
					if (expr == 0)
						continue;
					if (wc_exprs[k].idxLeft == cur &&
					    ((expr->op == TK_GT ||
					      expr->op == TK_GE)) &&
					    ((wc_exprs[k].prereqRight &
					      loop_mask) ==
					     wc_exprs[k].prereqRight) &&
					    (expr->pLeft->iColumn ==
					     idx->aiColumn[j])) {
						__expr_code(parser,
							    expr->pRight);
						ge_flag = expr->op==TK_GE;
						wc_exprs[k].p = 0;
						break;
					}
					if (wc_exprs[k].idxRight == cur &&
					    ((expr->op == TK_LT ||
					      expr->op == TK_LE)) &&
					    ((wc_exprs[k].prereqLeft &
					      loop_mask) ==
					     wc_exprs[k].prereqLeft) &&
					    (expr->pRight->iColumn ==
					     idx->aiColumn[j])) {
						__expr_code(parser,
							    expr->pLeft);
						ge_flag = expr->op==TK_LE;
						wc_exprs[k].p = 0;
						break;
					}
				}
			} else {
				ge_flag = 1;
			}
			if (eqcols > 0 || (score & 2) != 0) {
				col = eqcols + ((score & 2) != 0);
				__vdbe_add_op(v, OP_NotNull, -col,
					      (__vdbe_current_addr(v) + 3));
				__vdbe_add_op(v, OP_Pop, col, 0);
				__vdbe_add_op(v, OP_Goto, 0, brk);
				__vdbe_add_op(v, OP_MakeKey, col, 0);
				__add_idx_key_type(v, idx);
				if (!ge_flag) {
					__vdbe_add_op(v, OP_IncrKey, 0, 0);
				}
				if (level->bRev) {
					level->iMem = parser->nMem++;
					__vdbe_add_op(v, OP_MemStore,
						      level->iMem, 1);
					test_op = OP_IdxLT;
				} else {
					__vdbe_add_op(v, OP_MoveTo,
						      level->iCur, brk);
				}
			} else if (level->bRev) {
				test_op = OP_Noop;
			} else {
				__vdbe_add_op(v, OP_Rewind, level->iCur, brk);
			}

			/*
			 * Generate the the top of the loop.  If there is a
			 * termination key we have to test for that key and
			 * abort at the top of the loop.
			 */
			start = __vdbe_current_addr(v);
			if (test_op != OP_Noop) {
				__vdbe_add_op(v, OP_MemLoad, level->iMem, 0);
				__vdbe_add_op(v, test_op, level->iCur, brk);
			}
			__vdbe_add_op(v, OP_RowKey, level->iCur, 0);
			__vdbe_add_op(v, OP_IdxIsNull, (eqcols + (score & 1)),
				      cont);
			__vdbe_add_op(v, OP_IdxRecno, level->iCur, 0);
			if (i == (tab_list->nSrc - 1) && push_key_p) {
				have_key_p = 1;
			} else {
				__vdbe_add_op(v, OP_MoveTo, cur, 0);
				have_key_p = 0;
			}

			/*
			 * Record the instruction used to terminate the loop.
			 */
			level->op = level->bRev ? OP_Prev : OP_Next;
			level->p1 = level->iCur;
			level->p2 = start;
		}
		loop_mask |= __get_cursor_bitmask(&mask_set, cur);

		/*
		 * Insert code to test every subexpression that can be
		 * completely computed using the current set of tables.
		 */
		for (j = 0; j < num_expr; j++) {
			if (wc_exprs[j].p == 0)
				continue;
			if ((wc_exprs[j].prereqAll & loop_mask) !=
			    wc_exprs[j].prereqAll)
				continue;
			if (level->iLeftJoin &&
			    !ExprHasProperty(wc_exprs[j].p, EP_FromJoin)) {
				continue;
			}
			if (have_key_p) {
				have_key_p = 0;
				__vdbe_add_op(v, OP_MoveTo, cur, 0);
			}
			__expr_if_false(parser, wc_exprs[j].p, cont, 1);
			wc_exprs[j].p = 0;
		}
		brk = cont;

		/*
		 * For a LEFT OUTER JOIN, generate code that will record the
		 * fact that at least one row of the right table has matched
		 * the left table.
		 */
		if (level->iLeftJoin) {
			level->top = __vdbe_current_addr(v);
			__vdbe_add_op(v, OP_Integer, 1, 0);
			__vdbe_add_op(v, OP_MemStore, level->iLeftJoin, 1);
			for (j = 0; j < num_expr; j++) {
				if (wc_exprs[j].p == 0)
					continue;
				if ((wc_exprs[j].prereqAll & loop_mask) !=
				    wc_exprs[j].prereqAll )
					continue;
				if (have_key_p) {
					/*
					 * Cannot happen.  'have_key_p' can
					 * only be true if push_key_p is true
					 * an push_key_p can only be true for
					 * DELETE and UPDATE and there are no
					 * outer joins with DELETE and UPDATE.
					 */
					have_key_p = 0;
					__vdbe_add_op(v, OP_MoveTo, cur, 0);
				}
				__expr_if_false(parser, wc_exprs[j].p, cont,1);
				wc_exprs[j].p = 0;
			}
		}
	}
	where_info->iContinue = cont;
	if (push_key_p && !have_key_p) {
		__vdbe_add_op(v, OP_Recno, tab_list->a[0].iCursor, 0);
	}
	__free_mask_set(&mask_set);
	return where_info;
}

/*
 * __where_end --
 *	Generate the end of the WHERE loop.  See comments on 
 *	__where_begin() for additional information.
 *
 * PUBLIC: void __where_end __P((where_info_t *));
 */
void
__where_end(winfo)
	where_info_t *winfo;
{
	int i, addr;
	where_level_t *level;
	table_t *table;
	vdbe_t *v = winfo->pParse->pVdbe;
	src_list_t *tab_list = winfo->pTabList;

	for (i = (tab_list->nSrc - 1); i >= 0; i--) {
		level = &winfo->a[i];
		__vdbe_resolve_label(v, level->cont);
		if (level->op != OP_Noop) {
			__vdbe_add_op(v, level->op, level->p1, level->p2);
		}
		__vdbe_resolve_label(v, level->brk);
		if (level->inOp != OP_Noop) {
			__vdbe_add_op(v, level->inOp, level->inP1,
				      level->inP2);
		}
		if (level->iLeftJoin) {
			addr = __vdbe_add_op(v, OP_MemLoad,
					     level->iLeftJoin, 0);
			__vdbe_add_op(v, OP_NotNull, 1,
				      (addr + 4 + (level->iCur >= 0)));
			__vdbe_add_op(v, OP_NullRow, tab_list->a[i].iCursor,
				      0);
			if (level->iCur >= 0) {
				__vdbe_add_op(v, OP_NullRow, level->iCur, 0);
			}
			__vdbe_add_op(v, OP_Goto, 0, level->top);
		}
	}
	__vdbe_resolve_label(v, winfo->iBreak);
	for (i = 0; i < tab_list->nSrc; i++) {
		table = tab_list->a[i].pTab;
		DBSQL_ASSERT(table != 0);
		if (table->isTransient || table->pSelect)
			continue;
		level = &winfo->a[i];
		__vdbe_add_op(v, OP_Close, tab_list->a[i].iCursor, 0);
		if (level->pIdx != 0) {
			__vdbe_add_op(v, OP_Close, level->iCur, 0);
		}
	}
#if 0  /* NOTE: Never reuse a cursor */
	if( winfo->pParse->nTab==winfo->peakNTab ){
		winfo->pParse->nTab = winfo->savedNTab;
	}
#endif
	__dbsql_free(winfo->pParse->db, winfo);
	return;
}
