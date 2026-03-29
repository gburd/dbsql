/**
 * Comprehensive test suite to increase code coverage
 * Targets: VDBE opcodes, SQL functions, INSERT/DELETE, date functions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_framework.h"
#include "dbsql.h"




static int callback_count = 0;

static int
count_callback(void *arg, int argc, char **argv, char **col_names)
{
	(void)arg; (void)argc; (void)argv; (void)col_names;
	callback_count++;
	return 0;
}

/* Test UPDATE statement */
TEST(sql_update) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_update.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (id INTEGER, val INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (1, 100)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "UPDATE t SET val = 200 WHERE id = 1", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT val FROM t WHERE id = 1", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test DELETE statement */
TEST(sql_delete) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_delete.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (id INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (1)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (2)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "DELETE FROM t WHERE id = 1", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test aggregate functions */
TEST(sql_aggregates) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_agg.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE nums (val INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO nums VALUES (10)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO nums VALUES (20)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO nums VALUES (30)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT COUNT(*) FROM nums", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT SUM(val) FROM nums", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT AVG(val) FROM nums", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT MIN(val) FROM nums", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT MAX(val) FROM nums", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test ORDER BY */
TEST(sql_order_by) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_order.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (val INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (30)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (10)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (20)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t ORDER BY val ASC", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(3, callback_count);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t ORDER BY val DESC", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(3, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test string functions */
TEST(sql_string_functions) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_str.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (text TEXT)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES ('hello')", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT LENGTH(text) FROM t", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT UPPER(text) FROM t", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT LOWER(text) FROM t", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT SUBSTR(text, 1, 3) FROM t", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test WHERE clause with various operators */
TEST(sql_where_operators) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_where.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (id INTEGER, val INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (1, 10)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (2, 20)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (3, 30)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE val > 15", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, callback_count);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE val >= 20", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, callback_count);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE val < 25", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, callback_count);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE val <= 20", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, callback_count);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE val = 20", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, callback_count);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE val != 20", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test NULL handling */
TEST(sql_null_handling) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_null.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (id INTEGER, val INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (1, NULL)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (2, 20)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE val IS NULL", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, callback_count);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE val IS NOT NULL", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test LIKE operator */
TEST(sql_like_operator) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_like.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (text TEXT)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES ('apple')", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES ('banana')", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES ('apricot')", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE text LIKE 'ap%'", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, callback_count);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE text LIKE '%an%'", count_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	TEST_INIT("Comprehensive SQL Tests");
	test_run_all();
	TEST_FINISH();
}
