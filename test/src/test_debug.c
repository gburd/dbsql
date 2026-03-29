/**
 * Debug program to identify DBSQL_CANTOPEN issue
 */

#include "dbsql.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_debug.db";

	/* Remove existing file */
	unlink(dbfile);

	printf("Step 1: Creating environment with NULL dir (in-memory)...\n");
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("Result: %d\n", rc);
	if (rc != 0) {
		printf("FAILED at dbsql_create_env\n");
		return 1;
	}
	printf("SUCCESS: db = %p\n", (void*)db);

	printf("\nStep 2: Opening database file '%s'...\n", dbfile);
	rc = db->open(db, dbfile, 0664, &errmsg);
	printf("Result: %d\n", rc);
	if (rc != 0) {
		printf("FAILED: %s\n", errmsg ? errmsg : "no error message");
		return 1;
	}
	printf("SUCCESS\n");

	printf("\nStep 3: Executing CREATE TABLE...\n");
	rc = db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg);
	printf("Result: %d\n", rc);
	if (rc != 0) {
		printf("FAILED: %s\n", errmsg ? errmsg : "no error message");
		return 1;
	}
	printf("SUCCESS\n");

	printf("\nStep 4: Closing database...\n");
	rc = db->close(db);
	printf("Result: %d\n", rc);
	if (rc != 0) {
		printf("FAILED\n");
		return 1;
	}
	printf("SUCCESS\n");

	unlink(dbfile);
	printf("\nAll steps completed successfully!\n");
	return 0;
}
