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
 * Test suite for PRAGMA commands focusing on:
 * - PRAGMA integrity_check
 * - PRAGMA optimize
 * - PRAGMA analysis_limit
 * - PRAGMA trusted_schema
 * - PRAGMA writable_schema
 */

#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int callback_count = 0;
static char last_result[1024] = {0};

static int
test_callback(void *not_used, int argc, char **argv, char **col_name)
{
	int i;
	callback_count++;
	if (argc > 0 && argv[0]) {
		strncpy(last_result, argv[0], sizeof(last_result) - 1);
		last_result[sizeof(last_result) - 1] = '\0';
	}
	return 0;
}

static void
test_pragma_integrity_check_clean(void)
{
	DBSQL *dbp;
	char *errmsg = NULL;
	int rc;

	TEST_START("PRAGMA integrity_check on clean database");

	unlink("test_pragma_integrity.db_tbl.0000000002");

	rc = dbsql_open("test_pragma_integrity.db", &dbp, 0, NULL);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	rc = dbsql_exec(dbp, "CREATE TABLE test (id INTEGER PRIMARY KEY, "
			     "name TEXT)", NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	if (errmsg) {
		free(errmsg);
		errmsg = NULL;
	}

	rc = dbsql_exec(dbp, "INSERT INTO test VALUES (1, 'Alice')",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	if (errmsg) {
		free(errmsg);
		errmsg = NULL;
	}

	callback_count = 0;
	last_result[0] = '\0';
	rc = dbsql_exec(dbp, "PRAGMA integrity_check",
			test_callback, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	ASSERT_INT_GT(callback_count, 0);
	ASSERT_STR_EQ(last_result, "ok");

	if (errmsg) {
		free(errmsg);
	}
	dbsql_close(dbp);
	unlink("test_pragma_integrity.db_tbl.0000000002");

	TEST_END();
}

static void
test_pragma_optimize(void)
{
	DBSQL *dbp;
	char *errmsg = NULL;
	int rc;

	TEST_START("PRAGMA optimize");

	unlink("test_pragma_optimize.db_tbl.0000000002");

	rc = dbsql_open("test_pragma_optimize.db", &dbp, 0, NULL);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	rc = dbsql_exec(dbp, "CREATE TABLE test (id INTEGER PRIMARY KEY, "
			     "name TEXT)", NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	if (errmsg) {
		free(errmsg);
		errmsg = NULL;
	}

	rc = dbsql_exec(dbp, "INSERT INTO test VALUES (1, 'Alice')",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	if (errmsg) {
		free(errmsg);
		errmsg = NULL;
	}

	rc = dbsql_exec(dbp, "INSERT INTO test VALUES (2, 'Bob')",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	if (errmsg) {
		free(errmsg);
		errmsg = NULL;
	}

	rc = dbsql_exec(dbp, "PRAGMA optimize", NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	if (errmsg) {
		free(errmsg);
	}
	dbsql_close(dbp);
	unlink("test_pragma_optimize.db_tbl.0000000002");

	TEST_END();
}

static void
test_pragma_analysis_limit(void)
{
	DBSQL *dbp;
	char *errmsg = NULL;
	int rc;

	TEST_START("PRAGMA analysis_limit");

	unlink("test_pragma_analysis.db_tbl.0000000002");

	rc = dbsql_open("test_pragma_analysis.db", &dbp, 0, NULL);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	callback_count = 0;
	last_result[0] = '\0';
	rc = dbsql_exec(dbp, "PRAGMA analysis_limit",
			test_callback, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	rc = dbsql_exec(dbp, "PRAGMA analysis_limit = 1000",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	callback_count = 0;
	last_result[0] = '\0';
	rc = dbsql_exec(dbp, "PRAGMA analysis_limit",
			test_callback, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	if (errmsg) {
		free(errmsg);
	}
	dbsql_close(dbp);
	unlink("test_pragma_analysis.db_tbl.0000000002");

	TEST_END();
}

static void
test_pragma_trusted_schema(void)
{
	DBSQL *dbp;
	char *errmsg = NULL;
	int rc;

	TEST_START("PRAGMA trusted_schema");

	unlink("test_pragma_trusted.db_tbl.0000000002");

	rc = dbsql_open("test_pragma_trusted.db", &dbp, 0, NULL);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	callback_count = 0;
	last_result[0] = '\0';
	rc = dbsql_exec(dbp, "PRAGMA trusted_schema",
			test_callback, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	rc = dbsql_exec(dbp, "PRAGMA trusted_schema = 1",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	callback_count = 0;
	last_result[0] = '\0';
	rc = dbsql_exec(dbp, "PRAGMA trusted_schema",
			test_callback, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	ASSERT_STR_EQ(last_result, "1");

	rc = dbsql_exec(dbp, "PRAGMA trusted_schema = 0",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	callback_count = 0;
	last_result[0] = '\0';
	rc = dbsql_exec(dbp, "PRAGMA trusted_schema",
			test_callback, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	ASSERT_STR_EQ(last_result, "0");

	if (errmsg) {
		free(errmsg);
	}
	dbsql_close(dbp);
	unlink("test_pragma_trusted.db_tbl.0000000002");

	TEST_END();
}

static void
test_pragma_writable_schema(void)
{
	DBSQL *dbp;
	char *errmsg = NULL;
	int rc;

	TEST_START("PRAGMA writable_schema");

	unlink("test_pragma_writable.db_tbl.0000000002");

	rc = dbsql_open("test_pragma_writable.db", &dbp, 0, NULL);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	callback_count = 0;
	last_result[0] = '\0';
	rc = dbsql_exec(dbp, "PRAGMA writable_schema",
			test_callback, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	rc = dbsql_exec(dbp, "PRAGMA writable_schema = 1",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	callback_count = 0;
	last_result[0] = '\0';
	rc = dbsql_exec(dbp, "PRAGMA writable_schema",
			test_callback, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	ASSERT_STR_EQ(last_result, "1");

	rc = dbsql_exec(dbp, "PRAGMA writable_schema = 0",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	callback_count = 0;
	last_result[0] = '\0';
	rc = dbsql_exec(dbp, "PRAGMA writable_schema",
			test_callback, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	ASSERT_STR_EQ(last_result, "0");

	if (errmsg) {
		free(errmsg);
	}
	dbsql_close(dbp);
	unlink("test_pragma_writable.db_tbl.0000000002");

	TEST_END();
}

static void
test_pragma_multiple_tables(void)
{
	DBSQL *dbp;
	char *errmsg = NULL;
	int rc;

	TEST_START("PRAGMA integrity_check with multiple tables");

	unlink("test_pragma_multi.db_tbl.0000000002");
	unlink("test_pragma_multi.db_tbl.0000000003");

	rc = dbsql_open("test_pragma_multi.db", &dbp, 0, NULL);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);

	rc = dbsql_exec(dbp, "CREATE TABLE users (id INTEGER PRIMARY KEY, "
			     "name TEXT)", NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	if (errmsg) {
		free(errmsg);
		errmsg = NULL;
	}

	rc = dbsql_exec(dbp, "CREATE TABLE orders (id INTEGER PRIMARY KEY, "
			     "user_id INTEGER, total REAL)",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	if (errmsg) {
		free(errmsg);
		errmsg = NULL;
	}

	rc = dbsql_exec(dbp, "INSERT INTO users VALUES (1, 'Alice')",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	if (errmsg) {
		free(errmsg);
		errmsg = NULL;
	}

	rc = dbsql_exec(dbp, "INSERT INTO orders VALUES (1, 1, 99.99)",
			NULL, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	if (errmsg) {
		free(errmsg);
		errmsg = NULL;
	}

	callback_count = 0;
	last_result[0] = '\0';
	rc = dbsql_exec(dbp, "PRAGMA integrity_check",
			test_callback, NULL, &errmsg);
	ASSERT_INT_EQ(rc, DBSQL_SUCCESS);
	ASSERT_INT_GT(callback_count, 0);
	ASSERT_STR_EQ(last_result, "ok");

	if (errmsg) {
		free(errmsg);
	}
	dbsql_close(dbp);
	unlink("test_pragma_multi.db_tbl.0000000002");
	unlink("test_pragma_multi.db_tbl.0000000003");

	TEST_END();
}

int
main(int argc, char **argv)
{
	TEST_SUITE_START("PRAGMA Commands Test Suite");

	test_pragma_integrity_check_clean();
	test_pragma_optimize();
	test_pragma_analysis_limit();
	test_pragma_trusted_schema();
	test_pragma_writable_schema();
	test_pragma_multiple_tables();

	TEST_SUITE_END();

	return 0;
}
