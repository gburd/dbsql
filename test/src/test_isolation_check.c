/**
 * Test isolation check - Run file database tests sequentially
 */

#include "dbsql.h"
#include <stdio.h>
#include <unistd.h>

int test_file_db(const char *name, const char *filename) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;

	printf("%s: ", name);

	unlink(filename);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	if (rc != 0) {
		printf("FAIL create_env=%d\n", rc);
		return 1;
	}

	rc = db->open(db, filename, 0664, &errmsg);
	if (rc != 0) {
		printf("FAIL open=%d %s\n", rc, errmsg ? errmsg : "");
		db->close(db);
		return 1;
	}

	rc = db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg);
	if (rc != 0) {
		printf("FAIL exec=%d %s\n", rc, errmsg ? errmsg : "");
		db->close(db);
		return 1;
	}

	rc = db->close(db);
	if (rc != 0) {
		printf("FAIL close=%d\n", rc);
		return 1;
	}

	unlink(filename);
	printf("PASS\n");
	return 0;
}

int main(void) {
	int failures = 0;

	printf("=== Sequential File Database Tests ===\n\n");

	failures += test_file_db("Test 1", "test1.db");
	failures += test_file_db("Test 2", "test2.db");
	failures += test_file_db("Test 3", "test3.db");
	failures += test_file_db("Test 4 (reuse)", "test1.db");
	failures += test_file_db("Test 5 (same name)", "test_same.db");
	failures += test_file_db("Test 6 (same name)", "test_same.db");

	printf("\n=== Results ===\n");
	printf("Passed: %d/6\n", 6 - failures);
	printf("Failed: %d/6\n", failures);

	if (failures == 0) {
		printf("\n✓ All sequential file database tests pass\n");
		printf("  Issue is likely test framework isolation\n");
	} else {
		printf("\n✗ Sequential tests fail\n");
		printf("  Issue is with file database operations\n");
	}

	return failures;
}
