/**
 * Debug test to trace the exact failure point in DBSQL API
 */

#include "test_framework.h"
#include "dbsql.h"
#include <unistd.h>

/* Test the exact sequence that's failing */
TEST(dbsql_api_open_file_debug) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_debug.db";

	/* Remove existing file */
	unlink(dbfile);

	printf("\n=== STEP 1: dbsql_create_env(dir=NULL) ===\n");
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("dbsql_create_env returned: %d\n", rc);
	ASSERT_EQ(0, rc);
	ASSERT_NOT_NULL(db);

	printf("\n=== STEP 2: db->open(filename='%s') ===\n", dbfile);
	rc = db->open(db, dbfile, 0664, &errmsg);
	printf("db->open returned: %d (0x%x)\n", rc, rc);
	if (errmsg) {
		printf("Error message: %s\n", errmsg);
	}

	if (rc != 0) {
		printf("ERROR: db->open() failed with %d\n", rc);
		printf("Expected: 0 (DBSQL_SUCCESS)\n");
		printf("Got: %d\n", rc);

		/* -50988 is DBSQL_CANTOPEN */
		if (rc == -50988) {
			printf("This is DBSQL_CANTOPEN\n");
		}
	} else {
		printf("SUCCESS: db->open() succeeded\n");
		db->close(db);
	}

	unlink(dbfile);
}

/* Test with :memory: database (known to work) */
TEST(dbsql_api_open_memory_debug) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;

	printf("\n=== STEP 1: dbsql_create_env(dir=NULL) ===\n");
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("dbsql_create_env returned: %d\n", rc);
	ASSERT_EQ(0, rc);
	ASSERT_NOT_NULL(db);

	printf("\n=== STEP 2: db->open(filename=':memory:') ===\n");
	rc = db->open(db, ":memory:", 0, &errmsg);
	printf("db->open returned: %d (0x%x)\n", rc, rc);
	if (errmsg) {
		printf("Error message: %s\n", errmsg);
	}

	if (rc != 0) {
		printf("ERROR: db->open() failed with %d\n", rc);
	} else {
		printf("SUCCESS: db->open() succeeded\n");
		db->close(db);
	}
}

/* Test with a directory for the environment */
TEST(dbsql_api_open_file_with_env_dir) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *envdir = "/tmp/claude/test_dbsql_env";
	const char *dbfile = "test_debug_withdir.db";

	/* Create directory */
	mkdir(envdir, 0755);

	printf("\n=== STEP 1: dbsql_create_env(dir='%s') ===\n", envdir);
	rc = dbsql_create_env(&db, envdir, NULL, 0, 0);
	printf("dbsql_create_env returned: %d\n", rc);
	ASSERT_EQ(0, rc);
	ASSERT_NOT_NULL(db);

	printf("\n=== STEP 2: db->open(filename='%s') ===\n", dbfile);
	rc = db->open(db, dbfile, 0664, &errmsg);
	printf("db->open returned: %d (0x%x)\n", rc, rc);
	if (errmsg) {
		printf("Error message: %s\n", errmsg);
	}

	if (rc != 0) {
		printf("ERROR: db->open() failed with %d\n", rc);
		if (rc == -50988) {
			printf("This is DBSQL_CANTOPEN\n");
		}
	} else {
		printf("SUCCESS: db->open() succeeded\n");

		/* Try a simple operation */
		printf("\n=== STEP 3: CREATE TABLE ===\n");
		rc = db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg);
		printf("exec(CREATE TABLE) returned: %d\n", rc);
		if (errmsg) {
			printf("Error message: %s\n", errmsg);
		}

		db->close(db);
	}

	/* Cleanup */
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", envdir, dbfile);
	unlink(path);
}

/* Main test runner */
int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	TEST_INIT("DBSQL API Open Debug Tests");
	test_run_all();
	TEST_FINISH();
}
