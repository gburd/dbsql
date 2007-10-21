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
 * $Id: api_table.c 7 2007-02-03 13:34:17Z gburd $
 */

/*
 * This file contains the __api_get_table() and __api_free_table()
 * interface routines.  These are just wrappers around the main
 * interface routine of DBSQL->exec().
 *
 * These routines are in a separate files so that they will not be linked
 * if they are not used.
 */
#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <stdlib.h>
#include <string.h>
#endif

#include "dbsql_int.h"

/*
 * This structure is used to pass data from __get_table() through
 * to the user's callback function.
 */
typedef struct table_result {
	char **result;
	char *err_msgs;
	u_int32_t num_result;
	u_int32_t num_alloc;
	u_int32_t num_row;
	u_int32_t num_col;
	u_int32_t num_data;
	u_int32_t rc;
} table_result_t;

/*
 * __get_table_cb --
 *	This routine is called once for each row in the result table.  Its job
 *	is to fill in the table_result_t structure appropriately, allocating
 *	new memory as necessary.
 *
 * STATIC: static int __get_table_cb __P((void *, int, char **, char **));
 */
static int
__get_table_cb(arg, ncol, argv, colv)
	void *arg;
	int ncol;
	char **argv;
	char **colv;
{
	int rc;
	table_result_t *p = (table_result_t*)arg;
	int need;
	int i;
	char *z;

	/*
	 * Make sure there is enough space in p->result to hold everything
	 * we need to remember from this invocation of the callback.
	 */
	if (p->num_row == 0 && argv != 0) {
		need = ncol * 2;
	} else {
		need = ncol;
	}
	if (p->num_data + need >= p->num_alloc) {
		p->num_alloc = (p->num_alloc * 2) + need + 1;
		rc = __dbsql_realloc(NULL, sizeof(char*) * p->num_alloc,
				      &p->result);
		if (rc == ENOMEM) {
			p->rc = DBSQL_NOMEM;
			return DBSQL_ERROR;
		}
	}

	/*
	 * If this is the first row, then generate an extra row containing
	 * the names of all columns.
	 */
	if (p->num_row==0) {
		p->num_col = ncol;
		for (i = 0; i < ncol; i++) {
			if (colv[i] == 0) {
				z = 0;
			} else {
				int rc = __dbsql_malloc(NULL,
						      strlen(colv[i]) + 1, &z);
				if (rc == ENOMEM) {
					p->rc = DBSQL_NOMEM;
					return DBSQL_ERROR;
				}
				strcpy(z, colv[i]);
			}
			p->result[p->num_data++] = z;
		}
	} else if (p->num_col != ncol) {
		__str_append(&p->err_msgs,
			     "DBSQL->get_table() called with two or more "
			     "incompatible queries", (char*)0);
		p->rc = DBSQL_ERROR;
		return DBSQL_ERROR;
	}

	/*
	 * Copy over the row data.
	 */
	if (argv != 0) {
		for (i = 0; i < ncol; i++) {
			if (argv[i] == 0) {
				z = 0;
			} else {
				if (__dbsql_malloc(NULL, strlen(argv[i]) + 1,
						   &z) == ENOMEM) {
					p->rc = DBSQL_NOMEM;
					return DBSQL_ERROR;
				}
				strcpy(z, argv[i]);
			}
			p->result[p->num_data++] = z;
		}
		p->num_row++;
	}
	return DBSQL_SUCCESS;
}

/*
 * __api_get_table --
 *	Query the database.  But instead of invoking a callback for each row,
 *	__dbsql_malloc() space to hold the result and return the entire results
 *	at the conclusion of the call.
 *	The result that is written to ***results is held in memory obtained
 *	from __dbsql_malloc().  But the caller cannot free this memory directly
 *	Instead, the entire table should be passed to DBSQL->free_table() when
 *	the calling procedure is finished using it.
 *
 * PUBLIC: int __api_get_table __P((DBSQL *, const char *, char ***, int *,
 * PUBLIC:                     int *, char **));
 *
 * dbp			The database on which the SQL executes
 * sql			The SQL to be executed
 * results		Write the result table here
 * nrows		Write the number of rows in the result here
 * ncols		Write the number of columns of result here
 * err_msgs		Write error messages here
 */
int
__api_get_table(dbp, sql, results, nrows, ncols, err_msgs)
	DBSQL *dbp;
	const char *sql;
	char ***results;
	int *nrows;
	int *ncols;
	char **err_msgs;
{
	int rc;
	table_result_t res;

	DBSQL_ASSERT(results);
	DBSQL_ASSERT(ncols);
	DBSQL_ASSERT(nrows);

	*results = 0;
	*ncols = 0;
	*nrows = 0;
	res.err_msgs = 0;
	res.num_result = 0;
	res.num_row = 0;
	res.num_col = 0;
	res.num_data = 1;
	res.num_alloc = 20;
	res.rc = DBSQL_SUCCESS;

	if (__dbsql_calloc(dbp, res.num_alloc, sizeof(char*),
			   &res.result) == ENOMEM)
		return DBSQL_NOMEM;
	rc = dbp->exec(dbp, sql, __get_table_cb, &res, err_msgs);
	if (rc == DBSQL_ABORT) {
		/* !!!
		 * We set this here because we're about to call free_table, but we
		 * have to set it again below because a realloc may have moved the
		 * memory around
		 */
		res.result[0] = res.result[1] + res.num_data;
		dbp->free_table(&res.result[1]);
		if (res.err_msgs) {
			if(err_msgs) {
				__dbsql_ufree(dbp, *err_msgs);
				*err_msgs = res.err_msgs;
				__str_urealloc(err_msgs);
			} else {
				__dbsql_free(dbp, res.err_msgs);
			}
		}
		return res.rc;
	}
	__dbsql_free(dbp, res.err_msgs);
	if (rc != DBSQL_SUCCESS) {
		dbp->free_table(&res.result[1]);
		return rc;
	}
	if (res.num_alloc > res.num_data) {
		if(__dbsql_realloc(dbp, sizeof(char*) * (res.num_data + 1),
				&res.result) == ENOMEM) {
			dbp->free_table(&res.result[1]);
			return DBSQL_NOMEM;
		}
		res.num_alloc = res.num_data + 1;
	}
	/* !!!
	 * free_table() will need to know when to stop freeing results.  We use result[0]
	 * as a pointer to the last allocated row to be free'd.  The number of rows to
	 * be free'd is 'result[0] - (&result[1])'.
	 */
	res.result[0] = res.result[1] + res.num_data;
	*results = &res.result[1];
	if (ncols)
		*ncols = res.num_col;
	if (nrows)
		*nrows = res.num_row;
	return rc;
}

/*
 * __api_free_table --
 *	This routine frees the space the DBSQL->get_table() __dbsql_malloc'ed.
 *	There are two things to be free'd, (a) each row (results[0..n]) and then (b) the
 * 	array 'results'.  The number of rows to be free'd is 'result[-1] - (&result[0])'.
 *
 * PUBLIC: void __api_free_table __P((char **));
 *
 * result		Result returned from from __api_get_table()
 */
void
__api_free_table(results)
	char **results;
{
	char *last_result;

	DBSQL_ASSERT(results);

	/* If this test is false then the result set is empty, only free (b) */
	if (*(results - 1) != *results) {
		/* !!!
		 * The address of the last results row is the value of the first element of
		 * the results array we created in get_table() but recall that we passed
		 * the address of the second element (&results[1]) to the user and so
		 * we need to read the memory of what used to be 'results[0]' which is
		 * now '*(results - 1)'.
		 */
		last_result = *(results - 1);
		while (*results <= last_result) {
			if (*results != NULL)
				__dbsql_ufree(NULL, *results);
			results++;
		}
	}
	__dbsql_ufree(NULL, results);
}
