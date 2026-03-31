/**
 * Test to isolate issue with multiple open/close cycles
 */

#include "dbsql.h"
#include <stdio.h>
#include <unistd.h>

int test_in_memory() {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;

	printf("=== Test 1: In-memory database ===\n");

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("create_env: %d\n", rc);
	if (rc != 0) return rc;

	rc = db->open(db, ":memory:", 0, &errmsg);
	printf("open :memory:: %d\n", rc);
	if (rc != 0) {
		printf("Error: %s\n", errmsg ? errmsg : "none");
		return rc;
	}

	rc = db->close(db);
	printf("close: %d\n", rc);
	return rc;
}

int test_file_db() {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_simple2.db";

	printf("\n=== Test 2: File database ===\n");
	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("create_env: %d\n", rc);
	if (rc != 0) return rc;

	rc = db->open(db, dbfile, 0664, &errmsg);
	printf("open %s: %d\n", dbfile, rc);
	if (rc != 0) {
		printf("Error: %s\n", errmsg ? errmsg : "none");
		return rc;
	}

	rc = db->close(db);
	printf("close: %d\n", rc);

	unlink(dbfile);
	return rc;
}

int main(void) {
	int rc;

	rc = test_in_memory();
	if (rc != 0) {
		printf("\nTest 1 FAILED with code %d\n", rc);
		return 1;
	}

	rc = test_file_db();
	if (rc != 0) {
		printf("\nTest 2 FAILED with code %d\n", rc);
		return 1;
	}

	printf("\nAll tests PASSED\n");
	return 0;
}
