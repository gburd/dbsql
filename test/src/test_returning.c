/**
 * DBSQL RETURNING Clause Tests
 *
 * Tests for RETURNING clause support in INSERT, UPDATE, and DELETE statements
 */

#include "test_framework.h"
#include "dbsql.h"
#include <unistd.h>
#include <string.h>

/* Helper structure to capture callback results */
typedef struct {
	int row_count;
	char last_id[64];
	char last_name[64];
	char last_value[64];
} returning_result_t;

/* Callback function to capture RETURNING results */
static int capture_returning_callback(void *arg, int argc, char **argv, char **col_names) {
	returning_result_t *result = (returning_result_t *)arg;
	int i;

	result->row_count++;

	for (i = 0; i < argc; i++) {
		if (col_names[i] && argv[i]) {
			if (strcmp(col_names[i], "id") == 0 || strcmp(col_names[i], "rowid") == 0) {
				strncpy(result->last_id, argv[i], sizeof(result->last_id) - 1);
			} else if (strcmp(col_names[i], "name") == 0) {
				strncpy(result->last_name, argv[i], sizeof(result->last_name) - 1);
			} else if (strcmp(col_names[i], "value") == 0) {
				strncpy(result->last_value, argv[i], sizeof(result->last_value) - 1);
			}
		}
	}

	return 0;
}

/* Test INSERT RETURNING with single column */
TEST(returning_insert_single_column) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_returning_insert.db";
	returning_result_t result = {0};

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert with RETURNING id */
	rc = db->exec(db, "INSERT INTO test (name) VALUES ('Alice') RETURNING id",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, result.row_count);
	ASSERT_STR_EQ("1", result.last_id);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test INSERT RETURNING with multiple columns */
TEST(returning_insert_multiple_columns) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_returning_insert_multi.db";
	returning_result_t result = {0};

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT, value INTEGER)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert with RETURNING multiple columns */
	rc = db->exec(db, "INSERT INTO test (name, value) VALUES ('Bob', 42) RETURNING id, name, value",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, result.row_count);
	ASSERT_STR_EQ("1", result.last_id);
	ASSERT_STR_EQ("Bob", result.last_name);
	ASSERT_STR_EQ("42", result.last_value);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test INSERT RETURNING * */
TEST(returning_insert_star) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_returning_insert_star.db";
	returning_result_t result = {0};

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert with RETURNING * */
	rc = db->exec(db, "INSERT INTO test (name) VALUES ('Charlie') RETURNING *",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, result.row_count);
	ASSERT_STR_EQ("1", result.last_id);
	ASSERT_STR_EQ("Charlie", result.last_name);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test UPDATE RETURNING */
TEST(returning_update) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_returning_update.db";
	returning_result_t result = {0};

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table and insert data */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT, value INTEGER)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test (name, value) VALUES ('Diana', 100)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Update with RETURNING */
	rc = db->exec(db, "UPDATE test SET value = 200 WHERE name = 'Diana' RETURNING id, name, value",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, result.row_count);
	ASSERT_STR_EQ("1", result.last_id);
	ASSERT_STR_EQ("Diana", result.last_name);
	ASSERT_STR_EQ("200", result.last_value);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test UPDATE RETURNING * */
TEST(returning_update_star) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_returning_update_star.db";
	returning_result_t result = {0};

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table and insert data */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test (name) VALUES ('Eve')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Update with RETURNING * */
	rc = db->exec(db, "UPDATE test SET name = 'Eve Updated' WHERE id = 1 RETURNING *",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, result.row_count);
	ASSERT_STR_EQ("1", result.last_id);
	ASSERT_STR_EQ("Eve Updated", result.last_name);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test DELETE RETURNING */
TEST(returning_delete) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_returning_delete.db";
	returning_result_t result = {0};

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table and insert data */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT, value INTEGER)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test (name, value) VALUES ('Frank', 333)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Delete with RETURNING */
	rc = db->exec(db, "DELETE FROM test WHERE name = 'Frank' RETURNING id, name, value",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, result.row_count);
	ASSERT_STR_EQ("1", result.last_id);
	ASSERT_STR_EQ("Frank", result.last_name);
	ASSERT_STR_EQ("333", result.last_value);

	/* Verify row was deleted */
	result.row_count = 0;
	rc = db->exec(db, "SELECT * FROM test",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(0, result.row_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test DELETE RETURNING * */
TEST(returning_delete_star) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_returning_delete_star.db";
	returning_result_t result = {0};

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table and insert data */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test (name) VALUES ('Grace')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Delete with RETURNING * */
	rc = db->exec(db, "DELETE FROM test WHERE id = 1 RETURNING *",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, result.row_count);
	ASSERT_STR_EQ("1", result.last_id);
	ASSERT_STR_EQ("Grace", result.last_name);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test UPDATE RETURNING with multiple rows */
TEST(returning_update_multiple_rows) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_returning_update_multi.db";
	returning_result_t result = {0};

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table and insert multiple rows */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT, status TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test (name, status) VALUES ('User1', 'active')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test (name, status) VALUES ('User2', 'active')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Update multiple rows with RETURNING */
	rc = db->exec(db, "UPDATE test SET status = 'inactive' RETURNING id, name, status",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, result.row_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test DELETE RETURNING with multiple rows */
TEST(returning_delete_multiple_rows) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_returning_delete_multi.db";
	returning_result_t result = {0};

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table and insert multiple rows */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT, type TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test (name, type) VALUES ('Item1', 'temp')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test (name, type) VALUES ('Item2', 'temp')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test (name, type) VALUES ('Item3', 'permanent')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Delete multiple rows with RETURNING */
	rc = db->exec(db, "DELETE FROM test WHERE type = 'temp' RETURNING id, name",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(2, result.row_count);

	/* Verify only permanent item remains */
	result.row_count = 0;
	rc = db->exec(db, "SELECT * FROM test",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, result.row_count);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Test INSERT RETURNING with explicit rowid */
TEST(returning_insert_explicit_rowid) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_returning_insert_rowid.db";
	returning_result_t result = {0};

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER PRIMARY KEY, name TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert with explicit ID and RETURNING */
	rc = db->exec(db, "INSERT INTO test (id, name) VALUES (100, 'Henry') RETURNING id, name",
	              capture_returning_callback, &result, &errmsg);
	ASSERT_EQ(0, rc);
	ASSERT_EQ(1, result.row_count);
	ASSERT_STR_EQ("100", result.last_id);
	ASSERT_STR_EQ("Henry", result.last_name);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Main test suite entry point */
int main(int argc, char **argv) {
	(void)argc;
	(void)argv;

	TEST_INIT("DBSQL RETURNING Clause Tests");

	/* Tests are automatically registered via TEST() macro */
	test_run_all();

	TEST_FINISH();
}
