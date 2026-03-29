/**
 * Coverage boost test suite - simple tests that work
 * Based on test_api pattern that achieves 100% pass rate
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_framework.h"
#include "dbsql.h"

static int callback_count = 0;
static char callback_value[256];

static int
result_callback(void *arg, int argc, char **argv, char **col_names)
{
	(void)arg; (void)col_names;
	callback_count++;
	if (argc > 0 && argv[0]) {
		strncpy(callback_value, argv[0], sizeof(callback_value) - 1);
	}
	return 0;
}

/* Test multiple INSERT and SELECT */
TEST(multi_insert_select) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_multi.db";

	unlink(dbfile);
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);
	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (id INTEGER, val TEXT)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (1, 'one')", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (2, 'two')", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (3, 'three')", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT id FROM t WHERE id > 1", result_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);
	unlink(dbfile);
}

/* Test UPDATE operations */
TEST(update_ops) {
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

	rc = db->exec(db, "INSERT INTO t VALUES (1, 10)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "UPDATE t SET val = 20 WHERE id = 1", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT val FROM t WHERE id = 1", result_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, callback_count);
	ASSERT_STR_EQ("20", callback_value);

	rc = db->close(db);
	ASSERT_EQ(0, rc);
	unlink(dbfile);
}

/* Test DELETE operations */
TEST(delete_ops) {
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
	rc = db->exec(db, "SELECT * FROM t", result_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);
	unlink(dbfile);
}

/* Test WHERE clause with various operators */
TEST(where_clause_ops) {
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
	rc = db->exec(db, "SELECT * FROM t WHERE val >= 20", result_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, callback_count);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t WHERE val <> 20", result_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);
	unlink(dbfile);
}

/* Test ORDER BY clause */
TEST(order_by_clause) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_order.db";

	unlink(dbfile);
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);
	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (id INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (3)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	rc = db->exec(db, "INSERT INTO t VALUES (1)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	rc = db->exec(db, "INSERT INTO t VALUES (2)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	callback_value[0] = '\0';
	rc = db->exec(db, "SELECT id FROM t ORDER BY id ASC", result_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(3, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);
	unlink(dbfile);
}

/* Test LIMIT clause */
TEST(limit_clause) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_limit.db";

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
	rc = db->exec(db, "INSERT INTO t VALUES (3)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT * FROM t LIMIT 2", result_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, callback_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);
	unlink(dbfile);
}

/* Test aggregate functions */
TEST(aggregate_funcs) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_agg.db";

	unlink(dbfile);
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);
	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (val INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (10)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	rc = db->exec(db, "INSERT INTO t VALUES (20)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	rc = db->exec(db, "INSERT INTO t VALUES (30)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	callback_count = 0;
	rc = db->exec(db, "SELECT COUNT(*) FROM t", result_callback, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, callback_count);
	ASSERT_STR_EQ("3", callback_value);

	rc = db->close(db);
	ASSERT_EQ(0, rc);
	unlink(dbfile);
}

/* Test prepared statement execution */
TEST(prepared_stmt_ops) {
	DBSQL *db = NULL;
	dbsql_stmt_t *stmt = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_prep.db";
	const char *tail = NULL;

	unlink(dbfile);
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);
	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE t (id INTEGER, val TEXT)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO t VALUES (1, 'test')", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->prepare(db, "SELECT * FROM t", &tail, &stmt, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_NOT_NULL(stmt);

	int ncols = 0;
	const char **col_names = NULL, **col_values = NULL;
	rc = db->step(stmt, &ncols, &col_names, &col_values);
	ASSERT_EQ(DBSQL_ROW, rc);

	rc = db->finalize(stmt, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);
	unlink(dbfile);
}

int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	TEST_INIT("DBSQL Coverage Boost Tests");
	test_run_all();
	TEST_FINISH();
}
