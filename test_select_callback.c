/**
 * Test SELECT with callback to verify VDBE execution
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../build_unix.old/dbsql.h"

static int row_count = 0;

/* Callback function for SELECT results */
static int select_callback(void *arg, int ncols, char **values, char **names)
{
	int i;
	int *count = (int*)arg;

	printf("Row %d:\n", ++(*count));
	for (i = 0; i < ncols; i++) {
		printf("  %s = %s\n", names[i] ? names[i] : "NULL",
		       values[i] ? values[i] : "NULL");
	}
	return 0;
}

int main(void)
{
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_callback.db";

	/* Remove existing database */
	unlink(dbfile);

	/* Create database environment */
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "Failed to create database environment: %d\n", rc);
		return 1;
	}

	/* Open database */
	rc = db->open(db, dbfile, 0664, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "Failed to open database: %s\n",
		        errmsg ? errmsg : "unknown error");
		if (errmsg) free(errmsg);
		return 1;
	}

	/* Create table */
	printf("Creating table...\n");
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)",
	              NULL, NULL, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "Failed to create table: %s\n",
		        errmsg ? errmsg : "unknown error");
		if (errmsg) free(errmsg);
		db->close(db);
		unlink(dbfile);
		return 1;
	}

	/* Insert test data */
	printf("Inserting data...\n");
	rc = db->exec(db, "INSERT INTO test VALUES (1, 'Alice')",
	              NULL, NULL, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "Failed to insert data: %s\n",
		        errmsg ? errmsg : "unknown error");
		if (errmsg) free(errmsg);
		db->close(db);
		unlink(dbfile);
		return 1;
	}

	rc = db->exec(db, "INSERT INTO test VALUES (2, 'Bob')",
	              NULL, NULL, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "Failed to insert data: %s\n",
		        errmsg ? errmsg : "unknown error");
		if (errmsg) free(errmsg);
		db->close(db);
		unlink(dbfile);
		return 1;
	}

	rc = db->exec(db, "INSERT INTO test VALUES (3, 'Charlie')",
	              NULL, NULL, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "Failed to insert data: %s\n",
		        errmsg ? errmsg : "unknown error");
		if (errmsg) free(errmsg);
		db->close(db);
		unlink(dbfile);
		return 1;
	}

	/* Query data with callback */
	printf("\nQuerying data with callback...\n");
	row_count = 0;
	rc = db->exec(db, "SELECT * FROM test", select_callback, &row_count, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "Failed to query data: %s\n",
		        errmsg ? errmsg : "unknown error");
		if (errmsg) free(errmsg);
		db->close(db);
		unlink(dbfile);
		return 1;
	}

	printf("\nTotal rows retrieved: %d\n", row_count);

	if (row_count != 3) {
		fprintf(stderr, "ERROR: Expected 3 rows, got %d\n", row_count);
		db->close(db);
		unlink(dbfile);
		return 1;
	}

	/* Close database */
	rc = db->close(db);
	if (rc != 0) {
		fprintf(stderr, "Failed to close database: %d\n", rc);
		unlink(dbfile);
		return 1;
	}

	/* Cleanup */
	unlink(dbfile);

	printf("\nSUCCESS: SELECT with callback works correctly!\n");
	return 0;
}
