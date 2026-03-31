/**
 * Simple test to isolate DBSQL_CANTOPEN issue
 */

#include "dbsql.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_simple.db";

	unlink(dbfile);

	printf("Step 1: Creating environment...\n");
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("Result: %d (db=%p)\n", rc, (void*)db);
	if (rc != 0) {
		printf("FAILED at dbsql_create_env\n");
		return 1;
	}

	printf("\nStep 2: Opening database file '%s'...\n", dbfile);
	rc = db->open(db, dbfile, 0664, &errmsg);
	printf("Result: %d\n", rc);
	if (rc != 0) {
		printf("FAILED: %s\n", errmsg ? errmsg : "no error message");
		printf("Error code: %d\n", rc);
		if (rc == -50988) printf("This is DBSQL_CANTOPEN\n");

		/* Try to close anyway */
		printf("\nAttempting to close...\n");
		rc = db->close(db);
		printf("Close result: %d\n", rc);
		return 1;
	}
	printf("SUCCESS\n");

	printf("\nStep 3: Closing database...\n");
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
