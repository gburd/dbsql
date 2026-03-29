/**
 * Debug test: File-based vs Memory database
 */

#include "dbsql.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;

	/* Test 1: Memory database */
	printf("Test 1: Memory database\n");
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("  create_env: %d\n", rc);

	rc = db->open(db, ":memory:", 0, &errmsg);
	printf("  open(':memory:'): %d %s\n", rc, errmsg ? errmsg : "");

	rc = db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg);
	printf("  exec(CREATE): %d %s\n", rc, errmsg ? errmsg : "");

	rc = db->close(db);
	printf("  close: %d\n", rc);
	printf("  Memory test: %s\n\n", rc == 0 ? "PASS" : "FAIL");

	/* Test 2: File database */
	printf("Test 2: File database\n");
	unlink("test_file.db");

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("  create_env: %d\n", rc);

	rc = db->open(db, "test_file.db", 0664, &errmsg);
	printf("  open('test_file.db'): %d %s\n", rc, errmsg ? errmsg : "");

	if (rc == 0) {
		rc = db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg);
		printf("  exec(CREATE): %d %s\n", rc, errmsg ? errmsg : "");

		rc = db->close(db);
		printf("  close: %d\n", rc);
	}
	printf("  File test: %s\n", rc == 0 ? "PASS" : "FAIL");

	unlink("test_file.db");
	return 0;
}
