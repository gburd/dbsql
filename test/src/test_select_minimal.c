/**
 * Minimal SELECT test to debug the failure
 */

#include "test_framework.h"
#include "dbsql.h"
#include <stdio.h>
#include <unistd.h>

TEST(select_minimal) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_select_min.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);
	fprintf(stderr, "DEBUG: create_env succeeded\n");

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);
	fprintf(stderr, "DEBUG: open succeeded\n");

	/* Create table - this works */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)",
	              NULL, NULL, &errmsg);
	fprintf(stderr, "DEBUG: CREATE TABLE rc=%d, errmsg=%s\n", rc, errmsg ? errmsg : "NULL");
	ASSERT_EQ(0, rc);

	/* Insert - this works */
	rc = db->exec(db, "INSERT INTO test VALUES (1, 'first')",
	              NULL, NULL, &errmsg);
	fprintf(stderr, "DEBUG: INSERT rc=%d, errmsg=%s\n", rc, errmsg ? errmsg : "NULL");
	ASSERT_EQ(0, rc);

	/* SELECT - this fails */
	fprintf(stderr, "DEBUG: About to execute SELECT\n");
	rc = db->exec(db, "SELECT * FROM test", NULL, NULL, &errmsg);
	fprintf(stderr, "DEBUG: SELECT rc=%d, errmsg=%s\n", rc, errmsg ? errmsg : "NULL");

	if (rc != 0) {
		fprintf(stderr, "ERROR: SELECT failed with rc=%d\n", rc);
	}

	fprintf(stderr, "DEBUG: About to close\n");
	rc = db->close(db);
	fprintf(stderr, "DEBUG: close rc=%d\n", rc);

	unlink(dbfile);
}

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	TEST_INIT("SELECT Debug Test");
	test_run_all();
	TEST_FINISH();
}
