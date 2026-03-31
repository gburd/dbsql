/*
 * Test suite for JSON1 extension functions.
 *
 * Tests:
 *   - json_valid() - JSON validation
 *   - json_type() - Type detection
 *   - json_extract() - Path-based value extraction
 *   - json_object() - Object creation
 *   - json_array() - Array creation
 */

#include "test_framework.h"
#include "munit.h"
#include <string.h>

/*
 * Test json_valid() function.
 */
static MunitResult
test_json_valid(const MunitParameter params[], void *fixture)
{
	DBSQL *dbp;
	dbsql_stmt_t *stmt;
	char *err_msg = NULL;
	int rc;
	const char **col_names;
	const char **values;

	/* Open database */
	rc = dbsql_open("", &dbp);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_not_null(dbp);

	/* Test valid JSON object */
	rc = dbsql_prepare(dbp, "SELECT json_valid('{\"key\":\"value\"}')",
	    -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "1");
	dbp->finalize(stmt, NULL);

	/* Test valid JSON array */
	rc = dbsql_prepare(dbp, "SELECT json_valid('[1,2,3]')", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "1");
	dbp->finalize(stmt, NULL);

	/* Test valid JSON string */
	rc = dbsql_prepare(dbp, "SELECT json_valid('\"hello\"')", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "1");
	dbp->finalize(stmt, NULL);

	/* Test valid JSON number */
	rc = dbsql_prepare(dbp, "SELECT json_valid('123')", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "1");
	dbp->finalize(stmt, NULL);

	/* Test valid JSON boolean */
	rc = dbsql_prepare(dbp, "SELECT json_valid('true')", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "1");
	dbp->finalize(stmt, NULL);

	/* Test valid JSON null */
	rc = dbsql_prepare(dbp, "SELECT json_valid('null')", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "1");
	dbp->finalize(stmt, NULL);

	/* Test invalid JSON */
	rc = dbsql_prepare(dbp, "SELECT json_valid('{invalid}')", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "0");
	dbp->finalize(stmt, NULL);

	/* Test unterminated string */
	rc = dbsql_prepare(dbp, "SELECT json_valid('\"unterminated')", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "0");
	dbp->finalize(stmt, NULL);

	/* Test unterminated array */
	rc = dbsql_prepare(dbp, "SELECT json_valid('[1,2,3')", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "0");
	dbp->finalize(stmt, NULL);

	dbsql_close(dbp);
	return MUNIT_OK;
}

/*
 * Test json_type() function.
 */
static MunitResult
test_json_type(const MunitParameter params[], void *fixture)
{
	DBSQL *dbp;
	dbsql_stmt_t *stmt;
	char *err_msg = NULL;
	int rc;
	const char **col_names;
	const char **values;

	/* Open database */
	rc = dbsql_open("", &dbp);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_not_null(dbp);

	/* Test object type */
	rc = dbsql_prepare(dbp, "SELECT json_type('{\"key\":\"value\"}')",
	    -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "object");
	dbp->finalize(stmt, NULL);

	/* Test array type */
	rc = dbsql_prepare(dbp, "SELECT json_type('[1,2,3]')", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "array");
	dbp->finalize(stmt, NULL);

	/* Test string type */
	rc = dbsql_prepare(dbp, "SELECT json_type('\"hello\"')", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "string");
	dbp->finalize(stmt, NULL);

	/* Test number type */
	rc = dbsql_prepare(dbp, "SELECT json_type('123')", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "number");
	dbp->finalize(stmt, NULL);

	/* Test boolean true type */
	rc = dbsql_prepare(dbp, "SELECT json_type('true')", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "true");
	dbp->finalize(stmt, NULL);

	/* Test boolean false type */
	rc = dbsql_prepare(dbp, "SELECT json_type('false')", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "false");
	dbp->finalize(stmt, NULL);

	/* Test null type */
	rc = dbsql_prepare(dbp, "SELECT json_type('null')", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "null");
	dbp->finalize(stmt, NULL);

	dbsql_close(dbp);
	return MUNIT_OK;
}

/*
 * Test json_extract() function.
 */
static MunitResult
test_json_extract(const MunitParameter params[], void *fixture)
{
	DBSQL *dbp;
	dbsql_stmt_t *stmt;
	char *err_msg = NULL;
	int rc;
	const char **col_names;
	const char **values;

	/* Open database */
	rc = dbsql_open("", &dbp);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_not_null(dbp);

	/* Test root path extraction */
	rc = dbsql_prepare(dbp, "SELECT json_extract('{\"key\":\"value\"}', '$')",
	    -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "{\"key\":\"value\"}");
	dbp->finalize(stmt, NULL);

	dbsql_close(dbp);
	return MUNIT_OK;
}

/*
 * Test json_object() function.
 */
static MunitResult
test_json_object(const MunitParameter params[], void *fixture)
{
	DBSQL *dbp;
	dbsql_stmt_t *stmt;
	char *err_msg = NULL;
	int rc;
	const char **col_names;
	const char **values;

	/* Open database */
	rc = dbsql_open("", &dbp);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_not_null(dbp);

	/* Test empty object */
	rc = dbsql_prepare(dbp, "SELECT json_object()", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "{}");
	dbp->finalize(stmt, NULL);

	/* Test single key-value pair */
	rc = dbsql_prepare(dbp, "SELECT json_object('key', 'value')", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "{\"key\":\"value\"}");
	dbp->finalize(stmt, NULL);

	/* Test multiple key-value pairs */
	rc = dbsql_prepare(dbp,
	    "SELECT json_object('name', 'Alice', 'age', '30')", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0],
	    "{\"name\":\"Alice\",\"age\":\"30\"}");
	dbp->finalize(stmt, NULL);

	/* Test null value */
	rc = dbsql_prepare(dbp, "SELECT json_object('key', NULL)", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "{\"key\":null}");
	dbp->finalize(stmt, NULL);

	dbsql_close(dbp);
	return MUNIT_OK;
}

/*
 * Test json_array() function.
 */
static MunitResult
test_json_array(const MunitParameter params[], void *fixture)
{
	DBSQL *dbp;
	dbsql_stmt_t *stmt;
	char *err_msg = NULL;
	int rc;
	const char **col_names;
	const char **values;

	/* Open database */
	rc = dbsql_open("", &dbp);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_not_null(dbp);

	/* Test empty array */
	rc = dbsql_prepare(dbp, "SELECT json_array()", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "[]");
	dbp->finalize(stmt, NULL);

	/* Test single element */
	rc = dbsql_prepare(dbp, "SELECT json_array('hello')", -1, &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "[\"hello\"]");
	dbp->finalize(stmt, NULL);

	/* Test multiple elements */
	rc = dbsql_prepare(dbp, "SELECT json_array('1', '2', '3')", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "[\"1\",\"2\",\"3\"]");
	dbp->finalize(stmt, NULL);

	/* Test null element */
	rc = dbsql_prepare(dbp, "SELECT json_array('a', NULL, 'b')", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "[\"a\",null,\"b\"]");
	dbp->finalize(stmt, NULL);

	dbsql_close(dbp);
	return MUNIT_OK;
}

/*
 * Test JSON with table data.
 */
static MunitResult
test_json_with_table(const MunitParameter params[], void *fixture)
{
	DBSQL *dbp;
	dbsql_stmt_t *stmt;
	char *err_msg = NULL;
	int rc;
	const char **col_names;
	const char **values;

	/* Open database */
	rc = dbsql_open("", &dbp);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_not_null(dbp);

	/* Create test table */
	rc = dbp->exec(dbp,
	    "CREATE TABLE users (id INTEGER, data TEXT)", NULL, NULL, &err_msg);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Insert JSON data */
	rc = dbp->exec(dbp,
	    "INSERT INTO users VALUES (1, '{\"name\":\"Alice\",\"age\":30}')",
	    NULL, NULL, &err_msg);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->exec(dbp,
	    "INSERT INTO users VALUES (2, '{\"name\":\"Bob\",\"age\":25}')",
	    NULL, NULL, &err_msg);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Validate stored JSON */
	rc = dbsql_prepare(dbp,
	    "SELECT id, json_valid(data) FROM users ORDER BY id", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "1");
	munit_assert_string_equal(values[1], "1");

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "2");
	munit_assert_string_equal(values[1], "1");

	dbp->finalize(stmt, NULL);

	/* Get type of stored JSON */
	rc = dbsql_prepare(dbp,
	    "SELECT id, json_type(data) FROM users ORDER BY id", -1,
	    &stmt, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "1");
	munit_assert_string_equal(values[1], "object");

	rc = dbp->step(stmt, &values, &col_names);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_string_equal(values[0], "2");
	munit_assert_string_equal(values[1], "object");

	dbp->finalize(stmt, NULL);

	dbsql_close(dbp);
	return MUNIT_OK;
}

/*
 * Test suite definition.
 */
static MunitTest json_tests[] = {
	{
		"/json_valid",
		test_json_valid,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/json_type",
		test_json_type,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/json_extract",
		test_json_extract,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/json_object",
		test_json_object,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/json_array",
		test_json_array,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/json_with_table",
		test_json_with_table,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite json_suite = {
	"/json",
	json_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&json_suite, NULL, argc, argv);
}
