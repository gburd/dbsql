/**
 * DBSQL API Tests
 *
 * Tests for core API functions: create_env, open, close, exec, prepare, step, finalize
 */

#include "test_framework.h"
#include "dbsql.h"
#include <unistd.h>

/* Test database operations */
TEST(api_create_close) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;

	/* Create database */
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);
	ASSERT_NOT_NULL(db);

	/* Open in-memory database - required before close */
	rc = db->open(db, ":memory:", 0, &errmsg);
	ASSERT_EQ(0, rc);

	/* Close database */
	rc = db->close(db);
	ASSERT_EQ(0, rc);
}

TEST(api_open_database) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_api.db";

	/* Remove existing file */
	unlink(dbfile);

	/* Create database */
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	/* Open database file */
	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Close database */
	rc = db->close(db);
	ASSERT_EQ(0, rc);

	/* Cleanup */
	unlink(dbfile);
}

TEST(api_exec_create_table) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_api_create.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(api_insert_select) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_api_insert.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert data */
	rc = db->exec(db, "INSERT INTO test VALUES (1, 'Alice')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (2, 'Bob')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Query data - just check it doesn't crash */
	rc = db->exec(db, "SELECT * FROM test", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(api_multiple_tables) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_api_multi.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create multiple tables */
	rc = db->exec(db, "CREATE TABLE users (id INTEGER, name TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE products (id INTEGER, price REAL)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(api_error_handling_invalid_sql) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_api_error.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Execute invalid SQL - should fail */
	rc = db->exec(db, "INVALID SQL STATEMENT", NULL, NULL, &errmsg);
	ASSERT_NE(0, rc);

	if (errmsg) {
		errmsg = NULL;
	}

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(api_transaction_commit) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_api_txn.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Transaction */
	rc = db->exec(db, "BEGIN", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (1)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "COMMIT", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(api_transaction_rollback) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_api_rollback.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Transaction with rollback */
	rc = db->exec(db, "BEGIN", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (1)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "ROLLBACK", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Main test runner */
int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	TEST_INIT("DBSQL API Tests");
	test_run_all();
	TEST_FINISH();
}
