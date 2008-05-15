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
 *
 * $Id: cg_pragma.c 7 2007-02-03 13:34:17Z gburd $
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
__get_boolean(z)
	char *z;
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
__get_safety_level(z)
	char *z;
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
__pragma(parser, left, right, minus_p)
	parser_t *parser;
	token_t *left;
	token_t *right;
	int minus_p;
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
		static vdbe_op_t table_info_preface[] = {
			{ OP_ColumnName,  0, 0,       "cid"},
			{ OP_ColumnName,  1, 0,       "name"},
			{ OP_ColumnName,  2, 0,       "type"},
			{ OP_ColumnName,  3, 0,       "notnull"},
			{ OP_ColumnName,  4, 0,       "dflt_value"},
			{ OP_ColumnName,  5, 0,       "pk"},
		};
		int i;
		__vdbe_add_op_list(v, ARRAY_SIZE(table_info_preface),
				   table_info_preface);
		__view_get_column_names(parser, table);
		for (i = 0; i < table->nCol; i++) {
			__vdbe_add_op(v, OP_Integer, i, 0);
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_change_p3(v, -1, table->aCol[i].zName,
					 P3_STATIC);
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_change_p3(v, -1, 
					 table->aCol[i].zType ?
					   table->aCol[i].zType :
					   "numeric", P3_STATIC);
			__vdbe_add_op(v, OP_Integer,
			      table->aCol[i].notNull, 0);
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_change_p3(v, -1, table->aCol[i].zDflt,
					 P3_STATIC);
			__vdbe_add_op(v, OP_Integer,
				      table->aCol[i].isPrimKey, 0);
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
		static vdbe_op_t table_info_preface[] = {
			{ OP_ColumnName,  0, 0,       "seqno"},
			{ OP_ColumnName,  1, 0,       "cid"},
			{ OP_ColumnName,  2, 0,       "name"},
		};
		int i;
		table = index->pTable;
		__vdbe_add_op_list(v, ARRAY_SIZE(table_info_preface),
				   table_info_preface);
		for (i = 0; i < index->nColumn; i++) {
			int cnum = index->aiColumn[i];
			__vdbe_add_op(v, OP_Integer, i, 0);
			__vdbe_add_op(v, OP_Integer, cnum, 0);
			__vdbe_add_op(v, OP_String, 0, 0);
			DBSQL_ASSERT(table->nCol > cnum);
			__vdbe_change_p3(v, -1,
					 table->aCol[cnum].zName,
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
		index = table->pIndex;
	}
	if (table && index) {
		int i = 0; 
		static vdbe_op_t index_list_preface[] = {
			{ OP_ColumnName,  0, 0,       "seq"},
			{ OP_ColumnName,  1, 0,       "name"},
			{ OP_ColumnName,  2, 0,       "unique"},
		};

		__vdbe_add_op_list(v, ARRAY_SIZE(index_list_preface),
				   index_list_preface);
		while(index) {
			__vdbe_add_op(v, OP_Integer, i, 0);
			__vdbe_add_op(v, OP_String, 0, 0);
			__vdbe_change_p3(v, -1, index->zName,
					 P3_STATIC);
			__vdbe_add_op(v, OP_Integer,
				      index->onError != OE_None, 0);
			__vdbe_add_op(v, OP_Callback, 3, 0);
			++i;
			index = index->pNext;
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
		static vdbe_op_t index_list_preface[] = {
			{ OP_ColumnName,  0, 0,       "id"},
			{ OP_ColumnName,  1, 0,       "seq"},
			{ OP_ColumnName,  2, 0,       "table"},
			{ OP_ColumnName,  3, 0,       "from"},
			{ OP_ColumnName,  4, 0,       "to"},
		};

		__vdbe_add_op_list(v, ARRAY_SIZE(index_list_preface),
				   index_list_preface);
		while(fk) {
			int j;
			for (j = 0; j < fk->nCol; j++) {
				__vdbe_add_op(v, OP_Integer, i, 0);
				__vdbe_add_op(v, OP_Integer, j, 0);
				__vdbe_add_op(v, OP_String, 0, 0);
				__vdbe_change_p3(v, -1, fk->zTo,
						 P3_STATIC);
				__vdbe_add_op(v, OP_String, 0, 0);
				__vdbe_change_p3(v, -1,
				  table->aCol[fk->aCol[j].iFrom].zName,
				  P3_STATIC);
				__vdbe_add_op(v, OP_String, 0, 0);
				__vdbe_change_p3(v, -1,
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
	static vdbe_op_t index_list_preface[] = {
		{ OP_ColumnName,  0, 0,       "seq"},
		{ OP_ColumnName,  1, 0,       "name"},
		{ OP_ColumnName,  2, 0,       "file"},
	};

	__vdbe_add_op_list(v, ARRAY_SIZE(index_list_preface),
			   index_list_preface);
	for (i = 0; i < dbp->nDb; i++) {
		if (dbp->aDb[i].pBt == 0)
			continue;
		DBSQL_ASSERT(dbp->aDb[i].zName != 0);
		__vdbe_add_op(v, OP_Integer, i, 0);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p3(v, -1, dbp->aDb[i].zName, P3_STATIC);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p3(v, -1,
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
