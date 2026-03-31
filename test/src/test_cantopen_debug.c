/**
 * Minimal test to debug DBSQL_CANTOPEN issue
 */

#include "dbsql.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main() {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_debug_cantopen.db";

	printf("Starting CANTOPEN debug test...\n");

	/* Remove existing file */
	unlink(dbfile);
	printf("Removed existing database file\n");

	/* Step 1: Create environment */
	printf("Step 1: Creating DB_ENV...\n");
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("  dbsql_create_env() returned: %d\n", rc);
	if (rc != 0) {
		printf("  FAILED at create_env\n");
		return 1;
	}
	printf("  db pointer: %p\n", (void*)db);
	if (db == NULL) {
		printf("  FAILED: db is NULL after create_env\n");
		return 1;
	}

	/* Step 2: Open database */
	printf("\nStep 2: Opening database '%s'...\n", dbfile);
	rc = db->open(db, dbfile, 0664, &errmsg);
	printf("  db->open() returned: %d\n", rc);
	if (rc != 0) {
		printf("  FAILED at open: %s\n", errmsg ? errmsg : "no error message");
		if (errmsg) {
			printf("  Error message: %s\n", errmsg);
		}
		return 1;
	}
	printf("  SUCCESS: database opened\n");

	/* Step 3: Execute simple SQL */
	printf("\nStep 3: Executing CREATE TABLE...\n");
	rc = db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg);
	printf("  db->exec() returned: %d\n", rc);
	if (rc != 0) {
		printf("  FAILED at exec: %s\n", errmsg ? errmsg : "no error message");
		if (db->close) {
			db->close(db);
		}
		return 1;
	}
	printf("  SUCCESS: table created\n");

	/* Step 4: Close database */
	printf("\nStep 4: Closing database...\n");
	rc = db->close(db);
	printf("  db->close() returned: %d\n", rc);
	if (rc != 0) {
		printf("  FAILED at close\n");
		return 1;
	}
	printf("  SUCCESS: database closed\n");

	/* Cleanup */
	unlink(dbfile);
	printf("\nAll tests PASSED\n");
	return 0;
}
