/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc.
 *
 * Test suite for SAVEPOINT functionality (nested transactions).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "dbsql.h"
#include "test_framework.h"

static int callback_count = 0;

static int
callback(void *arg, int argc, char **argv, char **col_names)
{
	int i;
	callback_count++;
	for (i = 0; i < argc; i++) {
		printf("%s = %s\n", col_names[i], argv[i] ? argv[i] : "NULL");
	}
	printf("\n");
	return 0;
}

/*
 * Test basic savepoint create/rollback/release.
 */
static int
test_basic_savepoint(void)
{
	DBSQL *db;
	char *errmsg = NULL;
	int rc;

	printf("Testing basic savepoint functionality...\n");

	rc = dbsql_open(":memory:", &db);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "Cannot open database: %s\n", errmsg);
		return -1;
	}

	rc = dbsql_exec(db, "BEGIN TRANSACTION", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "BEGIN failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "CREATE TABLE test (id INTEGER, value TEXT)",
			NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "CREATE TABLE failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "INSERT INTO test VALUES (1, 'one')",
			NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "INSERT failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "SAVEPOINT sp1", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "SAVEPOINT sp1 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "INSERT INTO test VALUES (2, 'two')",
			NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "INSERT 2 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "ROLLBACK TO sp1", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "ROLLBACK TO sp1 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	callback_count = 0;
	rc = dbsql_exec(db, "SELECT * FROM test", callback, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "SELECT failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	if (callback_count != 1) {
		fprintf(stderr, "Expected 1 row, got %d\n", callback_count);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "COMMIT", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "COMMIT failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	dbsql_close(db);
	printf("Basic savepoint test PASSED\n\n");
	return 0;
}

/*
 * Test nested savepoints.
 */
static int
test_nested_savepoints(void)
{
	DBSQL *db;
	char *errmsg = NULL;
	int rc;

	printf("Testing nested savepoints...\n");

	rc = dbsql_open(":memory:", &db);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "Cannot open database\n");
		return -1;
	}

	rc = dbsql_exec(db, "BEGIN TRANSACTION", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "BEGIN failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "CREATE TABLE test (id INTEGER, value TEXT)",
			NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "CREATE TABLE failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "INSERT INTO test VALUES (1, 'one')",
			NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "INSERT 1 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "SAVEPOINT sp1", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "SAVEPOINT sp1 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "INSERT INTO test VALUES (2, 'two')",
			NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "INSERT 2 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "SAVEPOINT sp2", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "SAVEPOINT sp2 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "INSERT INTO test VALUES (3, 'three')",
			NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "INSERT 3 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "ROLLBACK TO sp2", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "ROLLBACK TO sp2 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	callback_count = 0;
	rc = dbsql_exec(db, "SELECT * FROM test", callback, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "SELECT after sp2 rollback failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	if (callback_count != 2) {
		fprintf(stderr, "Expected 2 rows after sp2 rollback, got %d\n",
			callback_count);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "ROLLBACK TO sp1", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "ROLLBACK TO sp1 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	callback_count = 0;
	rc = dbsql_exec(db, "SELECT * FROM test", callback, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "SELECT after sp1 rollback failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	if (callback_count != 1) {
		fprintf(stderr, "Expected 1 row after sp1 rollback, got %d\n",
			callback_count);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "COMMIT", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "COMMIT failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	dbsql_close(db);
	printf("Nested savepoints test PASSED\n\n");
	return 0;
}

/*
 * Test RELEASE savepoint.
 */
static int
test_release_savepoint(void)
{
	DBSQL *db;
	char *errmsg = NULL;
	int rc;

	printf("Testing RELEASE savepoint...\n");

	rc = dbsql_open(":memory:", &db);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "Cannot open database\n");
		return -1;
	}

	rc = dbsql_exec(db, "BEGIN TRANSACTION", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "BEGIN failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "CREATE TABLE test (id INTEGER, value TEXT)",
			NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "CREATE TABLE failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "INSERT INTO test VALUES (1, 'one')",
			NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "INSERT 1 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "SAVEPOINT sp1", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "SAVEPOINT sp1 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "INSERT INTO test VALUES (2, 'two')",
			NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "INSERT 2 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "RELEASE sp1", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "RELEASE sp1 failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	callback_count = 0;
	rc = dbsql_exec(db, "SELECT * FROM test", callback, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "SELECT after RELEASE failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	if (callback_count != 2) {
		fprintf(stderr, "Expected 2 rows after RELEASE, got %d\n",
			callback_count);
		dbsql_close(db);
		return -1;
	}

	rc = dbsql_exec(db, "COMMIT", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		fprintf(stderr, "COMMIT failed: %s\n", errmsg);
		dbsql_freemem(errmsg);
		dbsql_close(db);
		return -1;
	}

	dbsql_close(db);
	printf("RELEASE savepoint test PASSED\n\n");
	return 0;
}

int
main(int argc, char **argv)
{
	int failures = 0;

	printf("=== DBSQL Savepoint Test Suite ===\n\n");

	if (test_basic_savepoint() != 0)
		failures++;

	if (test_nested_savepoints() != 0)
		failures++;

	if (test_release_savepoint() != 0)
		failures++;

	if (failures == 0) {
		printf("All savepoint tests PASSED\n");
		return 0;
	} else {
		printf("%d savepoint test(s) FAILED\n", failures);
		return 1;
	}
}
