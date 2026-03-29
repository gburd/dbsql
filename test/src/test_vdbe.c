/**
 * DBSQL VDBE (Virtual Database Engine) Tests
 *
 * Tests for the register-based bytecode execution engine
 */

#include "test_framework.h"
#include "dbsql.h"

TEST(vdbe_simple_integer) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_vdbe_int.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Test basic integer operations */
	rc = db->exec(db, "SELECT 42", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(vdbe_arithmetic) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_vdbe_arith.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Test arithmetic opcodes */
	rc = db->exec(db, "SELECT 1 + 2", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "SELECT 10 - 3", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "SELECT 4 * 5", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "SELECT 20 / 4", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(vdbe_comparison) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_vdbe_cmp.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Test comparison opcodes */
	rc = db->exec(db, "SELECT 1 < 2", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "SELECT 5 > 3", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "SELECT 4 = 4", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "SELECT 2 != 3", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(vdbe_string_operations) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_vdbe_str.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Test string opcodes */
	rc = db->exec(db, "SELECT 'Hello' || ' ' || 'World'", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(vdbe_table_scan) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_vdbe_scan.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create and populate table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, value TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (1, 'first')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (2, 'second')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Test full table scan */
	rc = db->exec(db, "SELECT * FROM test", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(vdbe_where_clause) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_vdbe_where.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create and populate table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, value TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (1, 'first')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (2, 'second')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Test WHERE clause (conditional jump opcodes) */
	rc = db->exec(db, "SELECT * FROM test WHERE id = 1", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "SELECT * FROM test WHERE id > 0", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(vdbe_aggregate_functions) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_vdbe_agg.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create and populate table */
	rc = db->exec(db, "CREATE TABLE test (value INTEGER)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (10)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (20)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (30)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Test aggregate functions */
	rc = db->exec(db, "SELECT COUNT(*) FROM test", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "SELECT SUM(value) FROM test", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(vdbe_order_by) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_vdbe_order.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create and populate table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (3, 'Charlie')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (1, 'Alice')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (2, 'Bob')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Test ORDER BY (sorter opcodes) */
	rc = db->exec(db, "SELECT * FROM test ORDER BY id", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "SELECT * FROM test ORDER BY name", NULL, NULL, &errmsg);
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

	TEST_INIT("DBSQL VDBE Tests");
	test_run_all();
	TEST_FINISH();
}
