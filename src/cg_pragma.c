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
 * This file contains code used to implement the PRAGMA command.
 */

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#endif

#include "dbsql_int.h"

/*
 * __get_boolean --
 *	Interpret the given string as a boolean value.
 *
 * STATIC: static int __get_boolean __P((char *));
 */
static int
__get_boolean(char* z)
{
	static char *true[] = { "yes", "on", "true" };
	int i;
	if (z[0] == 0)
		return 0;
	if (isdigit(z[0]) || (z[0]=='-' && isdigit(z[1]))) {
		return atoi(z);
	}
	for(i = 0; i < sizeof(true) / sizeof(true[0]); i++) {
		if (strcasecmp(z, true[i]) == 0)
			return 1;
	}
	return 0;
}

/*
 * __get_safety_level --
 *	Interpret the given string as a safety level.  Return 0 for OFF,
 *	1 for ON or NORMAL and 2 for FULL.  Return 1 for an empty or 
 *	unrecognized string argument.
 *
 *	Note that the values returned are one less that the values that
 *	should be passed into __sm_set_safety_level().  This is done
 *	to support legacy SQL code.  The safety level used to be boolean
 *	and older scripts may have used numbers 0 for OFF and 1 for ON.
 *
 * STATIC: static int __get_safety_level __P((char *));
 */
static int
__get_safety_level(char* z)
{
	static const struct {
		const char *word;
		int val;
	} key[] = {
		{ "no",    0 },
		{ "off",   0 },
		{ "false", 0 },
		{ "yes",   1 },
		{ "on",    1 },
		{ "true",  1 },
		{ "full",  2 },
	};
	int i;
	if (z[0] == 0)
		return 1;
	if (isdigit(z[0]) || (z[0] == '-' && isdigit(z[1]))) {
		return atoi(z);
	}
	for (i = 0; i < sizeof(key)/sizeof(key[0]); i++) {
		if (strcasecmp(z,key[i].word) == 0)
			return key[i].val;
	}
	return 1;
}

/*
 * __pragma --
 *	Process a pragma statement.  
 *
 *	Pragmas are of this form:
 *
 *	     PRAGMA id = value
 *
 *	The identifier might also be a string.  The value is a string, and
 *	identifier, or a number.  If 'minus_p' is true, then the value is
 *	a number that was preceded by a minus sign.
 *
 * PUBLIC: void __pragma __P((parser_t *, token_t *, token_t *, int));
 */
void
__pragma(parser_t* parser, token_t* left, token_t* right, int minus_p)
{
	char *left_name = 0;
	char *right_name = 0;
	DBSQL *dbp = parser->db;
	vdbe_t *v = __parser_get_vdbe(parser);
	if (v == 0)
		return;

	__dbsql_strndup(dbp, left->z, &left_name, left->n);
	__str_unquote(left_name);
	if (minus_p) {
		right_name = 0;
		__str_nappend(&right_name, "-", 1, right->z, right->n, NULL);
	} else {
		__dbsql_strndup(dbp, right->z, &right_name, right->n);
		__str_unquote(right_name);
	}
	if (__auth_check(parser, DBSQL_PRAGMA, left_name, right_name, NULL)) {
		__dbsql_free(dbp, left_name);
		__dbsql_free(dbp, right_name);
		return;
	}
 
/*
 *   PRAGMA trigger_overhead_test
 */
if (strcasecmp(left_name, "trigger_overhead_test") == 0) {
	if (__get_boolean(right_name)) {
		always_code_trigger_setup = 1;
	} else {
		always_code_trigger_setup = 0;
	}
} else
/*
 *   PRAGMA vdbe_trace
 */
if (strcasecmp(left_name, "vdbe_trace") == 0) {
	if (__get_boolean(right_name)) {
		dbp->flags |= DBSQL_VdbeTrace;
	} else {
		dbp->flags &= ~DBSQL_VdbeTrace;
	}
} else
/*
 *   PRAGMA full_column_names
 */
if (strcasecmp(left_name, "full_column_names") == 0) {
	if (__get_boolean(right_name)) {
		dbp->flags |= DBSQL_FullColNames;
	} else {
		dbp->flags &= ~DBSQL_FullColNames;
	}
} else
/*
 *   PRAGMA show_datatypes
 */
if (strcasecmp(left_name, "show_datatypes") == 0) {
	if( __get_boolean(right_name)) {
		dbp->flags |= DBSQL_ReportTypes;
	} else {
		dbp->flags &= ~DBSQL_ReportTypes;
	}
} else
/*
 *   PRAGMA count_changes
 */
if (strcasecmp(left_name, "count_changes") == 0) {
	if (__get_boolean(right_name)) {
		dbp->flags |= DBSQL_CountRows;
	} else {
		dbp->flags &= ~DBSQL_CountRows;
	}
} else
/*
 *   PRAGMA empty_result_callbacks
 */
if (strcasecmp(left_name, "empty_result_callbacks") == 0) {
	if ( __get_boolean(right_name)) {
		dbp->flags |= DBSQL_NullCallback;
	} else {
		dbp->flags &= ~DBSQL_NullCallback;
	}
} else
/*
 *   PRAGMA table_info
 */
if (strcasecmp(left_name, "table_info") == 0) {
	table_t *table;
	table = __find_table(dbp, right_name, 0);
	if (table) {
		int i;
		/* Add column names dynamically (can't use static initializer with p4 strings) */
		__vdbe_add_op(v, OP_ColumnName, 0, 0);
		__vdbe_change_p4(v, -1, "cid", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 1, 0);
		__vdbe_change_p4(v, -1, "name", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 2, 0);
		__vdbe_change_p4(v, -1, "type", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 3, 0);
		__vdbe_change_p4(v, -1, "notnull", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 4, 0);
		__vdbe_change_p4(v, -1, "dflt_value", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 5, 0);
		__vdbe_change_p4(v, -1, "pk", P4_STATIC);
		__view_get_column_names(parser, table);
		for (i = 0; i < table->nCol; i++) {
			__vdbe_add_op(v, OP_Integer, i, 0);
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_change_p4(v, -1, table->columns[i].zName,
					 P3_STATIC);
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_change_p4(v, -1, 
					 table->columns[i].zType ?
					   table->columns[i].zType :
					   "numeric", P3_STATIC);
			__vdbe_add_op(v, OP_Integer,
			      table->columns[i].notNull, 0);
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_change_p4(v, -1, table->columns[i].zDflt,
					 P3_STATIC);
			__vdbe_add_op(v, OP_Integer,
				      table->columns[i].isPrimKey, 0);
			__vdbe_add_op(v, OP_Callback, 6, 0);
		}
	}
} else
/*
 *   PRAGMA index_info
 */
if (strcasecmp(left_name, "index_info") == 0) {
	index_t *index;
	table_t *table;
	index = __find_index(dbp, right_name, 0);
	if (index) {
		int i;
		table = index->table;
		/* Add column names dynamically (can't use static initializer with p4 strings) */
		__vdbe_add_op(v, OP_ColumnName, 0, 0);
		__vdbe_change_p4(v, -1, "seqno", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 1, 0);
		__vdbe_change_p4(v, -1, "cid", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 2, 0);
		__vdbe_change_p4(v, -1, "name", P4_STATIC);
		for (i = 0; i < index->nColumn; i++) {
			int cnum = index->aiColumn[i];
			__vdbe_add_op(v, OP_Integer, i, 0);
			__vdbe_add_op(v, OP_Integer, cnum, 0);
			__vdbe_add_op(v, OP_String, 0, 0);
			DBSQL_ASSERT(table->nCol > cnum);
			__vdbe_change_p4(v, -1,
					 table->columns[cnum].zName,
					 P3_STATIC);
			__vdbe_add_op(v, OP_Callback, 3, 0);
		}
	}
} else
/*
 *   PRAGMA index_list
 */
if (strcasecmp(left_name, "index_list") == 0) {
	index_t *index;
	table_t *table;
	table = __find_table(dbp, right_name, 0);
	if (table) {
		v = __parser_get_vdbe(parser);
		index = table->first_index;
	}
	if (table && index) {
		int i = 0;
		/* Add column names dynamically (can't use static initializer with p4 strings) */
		__vdbe_add_op(v, OP_ColumnName, 0, 0);
		__vdbe_change_p4(v, -1, "seq", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 1, 0);
		__vdbe_change_p4(v, -1, "name", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 2, 0);
		__vdbe_change_p4(v, -1, "unique", P4_STATIC);
		while(index) {
			__vdbe_add_op(v, OP_Integer, i, 0);
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_change_p4(v, -1, index->zName,
					 P3_STATIC);
			__vdbe_add_op(v, OP_Integer,
				      index->onError != OE_None, 0);
			__vdbe_add_op(v, OP_Callback, 3, 0);
			++i;
			index = index->next_index;
		}
	}
} else
/*
 *   PRAGMA foreign_key_list
 */
if (strcasecmp(left_name, "foreign_key_list") == 0) {
	foreign_key_t *fk;
	table_t *table;
	table = __find_table(dbp, right_name, 0);
	if (table) {
		v = __parser_get_vdbe(parser);
		fk = table->pFKey;
	}
	if (table && fk) {
		int i = 0;
		/* Add column names dynamically (can't use static initializer with p4 strings) */
		__vdbe_add_op(v, OP_ColumnName, 0, 0);
		__vdbe_change_p4(v, -1, "id", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 1, 0);
		__vdbe_change_p4(v, -1, "seq", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 2, 0);
		__vdbe_change_p4(v, -1, "table", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 3, 0);
		__vdbe_change_p4(v, -1, "from", P4_STATIC);
		__vdbe_add_op(v, OP_ColumnName, 4, 0);
		__vdbe_change_p4(v, -1, "to", P4_STATIC);
		while(fk) {
			int j;
			for (j = 0; j < fk->nCol; j++) {
				__vdbe_add_op(v, OP_Integer, i, 0);
				__vdbe_add_op(v, OP_Integer, j, 0);
				__vdbe_add_op(v, OP_String, 0, 0);
				__vdbe_change_p4(v, -1, fk->zTo,
						 P3_STATIC);
				__vdbe_add_op(v, OP_String, 0, 0);
				__vdbe_change_p4(v, -1,
				  table->columns[fk->aCol[j].iFrom].zName,
				  P3_STATIC);
				__vdbe_add_op(v, OP_String, 0, 0);
				__vdbe_change_p4(v, -1,
				  fk->aCol[j].zCol, P3_STATIC);
				__vdbe_add_op(v, OP_Callback, 5, 0);
			}
			++i;
			fk = fk->pNextFrom;
		}
	}
} else
/*
 *   PRAGMA database_list
 */
if (strcasecmp(left_name, "database_list") == 0) {
	int i;
	/* Add column names dynamically (can't use static initializer with p4 strings) */
	__vdbe_add_op(v, OP_ColumnName, 0, 0);
	__vdbe_change_p4(v, -1, "seq", P4_STATIC);
	__vdbe_add_op(v, OP_ColumnName, 1, 0);
	__vdbe_change_p4(v, -1, "name", P4_STATIC);
	__vdbe_add_op(v, OP_ColumnName, 2, 0);
	__vdbe_change_p4(v, -1, "file", P4_STATIC);
	for (i = 0; i < dbp->nDb; i++) {
		if (dbp->aDb[i].pBt == 0)
			continue;
		DBSQL_ASSERT(dbp->aDb[i].zName != 0);
		__vdbe_add_op(v, OP_Integer, i, 0);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p4(v, -1, dbp->aDb[i].zName, P3_STATIC);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p4(v, -1,
			 __sm_get_database_name(dbp->aDb[i].pBt), P3_STATIC);
		__vdbe_add_op(v, OP_Callback, 3, 0);
	}
} else
#ifndef NDEBUG
 /*
  * PRAGMA parser_trace
  */
 if (strcasecmp(left_name, "parser_trace") == 0) {
	 extern void __sql_parser_trace(FILE*, char *);
	 if (__get_boolean(right_name)) {
		 __sql_parser_trace(stdout, "parser: ");
	 } else {
		 __sql_parser_trace(0, 0);
	 }
 }
#endif
 else {}
__dbsql_free(dbp, left_name);
__dbsql_free(dbp, right_name);
}
