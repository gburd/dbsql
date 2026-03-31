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
 * This file contains code for database integrity checking and optimization.
 */

#include "inc/dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <string.h>
#include <stdio.h>
#endif

#include "dbsql_int.h"

/*
 * __integrity_check_table --
 *	Check the integrity of a single table.
 *
 * STATIC: static int __integrity_check_table __P((DBSQL *, table_t *, char **));
 */
static int
__integrity_check_table(DBSQL *dbp, table_t *table, char **errmsg)
{
	int rc = DBSQL_SUCCESS;
	sm_cursor_t *cursor = NULL;
	sm_t *sm;
	int res;
	int row_count = 0;
	char *msg = NULL;

	if (table == NULL || table->pSelect != NULL) {
		return DBSQL_SUCCESS;
	}

	sm = dbp->aDb[0].pBt;
	if (sm == NULL) {
		__str_append(&msg, "No database open", (char *)0);
		*errmsg = msg;
		return DBSQL_ERROR;
	}

	rc = __sm_cursor(sm, table->tnum, 0, &cursor);
	if (rc != DBSQL_SUCCESS) {
		__str_append(&msg, "Cannot open cursor for table ",
			     table->zName, (char *)0);
		*errmsg = msg;
		return rc;
	}

	rc = __sm_first(cursor, &res);
	while (rc == DBSQL_SUCCESS && res == 0) {
		row_count++;
		rc = __sm_next(cursor, &res);
	}

	if (cursor) {
		__sm_close_cursor(cursor);
	}

	if (rc != DBSQL_SUCCESS && rc != DBSQL_DONE) {
		__str_append(&msg, "Error scanning table ",
			     table->zName, (char *)0);
		*errmsg = msg;
		return rc;
	}

	return DBSQL_SUCCESS;
}

/*
 * __integrity_check_index --
 *	Check the integrity of an index.
 *
 * STATIC: static int __integrity_check_index __P((DBSQL *, index_t *, char **));
 */
static int
__integrity_check_index(DBSQL *dbp, index_t *index, char **errmsg)
{
	int rc = DBSQL_SUCCESS;
	sm_cursor_t *cursor = NULL;
	sm_t *sm;
	int res;
	int entry_count = 0;
	char *msg = NULL;

	if (index == NULL) {
		return DBSQL_SUCCESS;
	}

	sm = dbp->aDb[0].pBt;
	if (sm == NULL) {
		__str_append(&msg, "No database open", (char *)0);
		*errmsg = msg;
		return DBSQL_ERROR;
	}

	rc = __sm_cursor(sm, index->tnum, 0, &cursor);
	if (rc != DBSQL_SUCCESS) {
		__str_append(&msg, "Cannot open cursor for index ",
			     index->zName, (char *)0);
		*errmsg = msg;
		return rc;
	}

	rc = __sm_first(cursor, &res);
	while (rc == DBSQL_SUCCESS && res == 0) {
		entry_count++;
		rc = __sm_next(cursor, &res);
	}

	if (cursor) {
		__sm_close_cursor(cursor);
	}

	if (rc != DBSQL_SUCCESS && rc != DBSQL_DONE) {
		__str_append(&msg, "Error scanning index ",
			     index->zName, (char *)0);
		*errmsg = msg;
		return rc;
	}

	return DBSQL_SUCCESS;
}

/*
 * __dbsql_integrity_check --
 *	Perform a comprehensive integrity check on the database.
 *	Returns an array of error messages, or "ok" if no errors found.
 *
 * PUBLIC: int __dbsql_integrity_check __P((DBSQL *, vdbe_t *));
 */
int
__dbsql_integrity_check(DBSQL *dbp, vdbe_t *v)
{
	int rc = DBSQL_SUCCESS;
	hash_ele_t *k;
	table_t *table;
	index_t *index;
	char *errmsg = NULL;
	int error_count = 0;
	sm_t *sm;

	if (dbp == NULL || v == NULL) {
		return DBSQL_ERROR;
	}

	sm = dbp->aDb[0].pBt;
	if (sm == NULL) {
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p4(v, -1, "No database open", P4_STATIC);
		__vdbe_add_op(v, OP_Callback, 1, 0);
		return DBSQL_ERROR;
	}

	for (k = __hash_first(&dbp->aDb[0].tblHash); k;
	     k = __hash_next(k)) {
		table = (table_t *)__hash_data(k);
		if (table->pSelect != NULL) {
			continue;
		}

		errmsg = NULL;
		rc = __integrity_check_table(dbp, table, &errmsg);
		if (rc != DBSQL_SUCCESS || errmsg != NULL) {
			__vdbe_add_op(v, OP_String, 0, 0);
			if (errmsg) {
				__vdbe_change_p4(v, -1, errmsg, P4_DYNAMIC);
			} else {
				__vdbe_change_p4(v, -1, "Error checking table",
						 P4_STATIC);
			}
			__vdbe_add_op(v, OP_Callback, 1, 0);
			error_count++;
			continue;
		}

		for (index = table->first_index; index;
		     index = index->next_index) {
			errmsg = NULL;
			rc = __integrity_check_index(dbp, index, &errmsg);
			if (rc != DBSQL_SUCCESS || errmsg != NULL) {
				__vdbe_add_op(v, OP_String, 0, 0);
				if (errmsg) {
					__vdbe_change_p4(v, -1, errmsg,
							 P4_DYNAMIC);
				} else {
					__vdbe_change_p4(v, -1,
						 "Error checking index",
						 P4_STATIC);
				}
				__vdbe_add_op(v, OP_Callback, 1, 0);
				error_count++;
			}
		}
	}

	if (error_count == 0) {
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p4(v, -1, "ok", P4_STATIC);
		__vdbe_add_op(v, OP_Callback, 1, 0);
	}

	return DBSQL_SUCCESS;
}

/*
 * __dbsql_optimize --
 *	Optimize the database by analyzing tables and updating statistics.
 *
 * PUBLIC: int __dbsql_optimize __P((DBSQL *));
 */
int
__dbsql_optimize(DBSQL *dbp)
{
	int rc = DBSQL_SUCCESS;
	hash_ele_t *k;
	table_t *table;
	char *sql = NULL;
	char *errmsg = NULL;

	if (dbp == NULL) {
		return DBSQL_ERROR;
	}

	for (k = __hash_first(&dbp->aDb[0].tblHash); k;
	     k = __hash_next(k)) {
		table = (table_t *)__hash_data(k);
		if (table->pSelect != NULL) {
			continue;
		}

		__str_append(&sql, "ANALYZE ", table->zName, (char *)0);
		rc = dbsql_exec(dbp, sql, 0, 0, &errmsg);
		if (sql) {
			__dbsql_free(dbp, sql);
			sql = NULL;
		}
		if (errmsg) {
			__dbsql_free(dbp, errmsg);
			errmsg = NULL;
		}
	}

	return rc;
}
