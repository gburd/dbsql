/*-
 * DBSQL - A SQL database engine.
 *
 * Test Suite for Virtual Table Framework
 *
 * Tests:
 *   1. Module registration
 *   2. CREATE VIRTUAL TABLE
 *   3. SELECT from virtual table
 *   4. Virtual table with WHERE clause
 *   5. CSV virtual table example
 *   6. Error handling
 *   7. Transaction support
 *   8. Multiple virtual tables
 */

#include "test_framework.h"
#include "../src/inc/vtab.h"

/* External CSV module registration function */
extern int __dbsql_register_csv_module(DBSQL *db);

/*
 * Simple test virtual table module for demonstration.
 * Returns a fixed set of rows (numbers 1-10).
 */

typedef struct test_vtab test_vtab;
struct test_vtab {
	sqlite3_vtab base;
	int nRow;
};

typedef struct test_cursor test_cursor;
struct test_cursor {
	sqlite3_vtab_cursor base;
	int iRow;
};

static int
test_connect(DBSQL *db, void *pAux, int argc, const char *const *argv,
             sqlite3_vtab **ppVtab, char **pzErr)
{
	test_vtab *pTab;
	int rc;

	pTab = calloc(1, sizeof(test_vtab));
	if (pTab == NULL) {
		return DBSQL_NOMEM;
	}

	pTab->nRow = 10;  /* Return 10 rows */

	rc = __dbsql_declare_vtab(db, "CREATE TABLE x(id INTEGER, value TEXT)");
	if (rc != DBSQL_SUCCESS) {
		free(pTab);
		return rc;
	}

	*ppVtab = (sqlite3_vtab *)pTab;
	return DBSQL_SUCCESS;
}

static int
test_disconnect(sqlite3_vtab *pVtab)
{
	free(pVtab);
	return DBSQL_SUCCESS;
}

static int
test_best_index(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo)
{
	pInfo->estimatedCost = 100.0;
	pInfo->estimatedRows = 10;
	return DBSQL_SUCCESS;
}

static int
test_open(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor)
{
	test_cursor *pCur = calloc(1, sizeof(test_cursor));
	if (pCur == NULL) {
		return DBSQL_NOMEM;
	}
	pCur->base.pVtab = pVtab;
	pCur->iRow = -1;
	*ppCursor = (sqlite3_vtab_cursor *)pCur;
	return DBSQL_SUCCESS;
}

static int
test_close(sqlite3_vtab_cursor *pCursor)
{
	free(pCursor);
	return DBSQL_SUCCESS;
}

static int
test_filter(sqlite3_vtab_cursor *pCursor, int idxNum, const char *idxStr,
            int argc, mem_t **argv)
{
	test_cursor *pCur = (test_cursor *)pCursor;
	pCur->iRow = 0;
	return DBSQL_SUCCESS;
}

static int
test_next(sqlite3_vtab_cursor *pCursor)
{
	test_cursor *pCur = (test_cursor *)pCursor;
	pCur->iRow++;
	return DBSQL_SUCCESS;
}

static int
test_eof(sqlite3_vtab_cursor *pCursor)
{
	test_cursor *pCur = (test_cursor *)pCursor;
	test_vtab *pTab = (test_vtab *)pCursor->pVtab;
	return pCur->iRow >= pTab->nRow;
}

static int
test_column(sqlite3_vtab_cursor *pCursor, func_context_t *pCtx, int iCol)
{
	test_cursor *pCur = (test_cursor *)pCursor;
	char buf[32];

	if (iCol == 0) {
		/* id column */
		__set_result_int(pCtx, pCur->iRow + 1);
	} else if (iCol == 1) {
		/* value column */
		snprintf(buf, sizeof(buf), "row_%d", pCur->iRow + 1);
		__set_result_text(pCtx, buf, -1, DBSQL_TRANSIENT);
	}

	return DBSQL_SUCCESS;
}

static int
test_rowid(sqlite3_vtab_cursor *pCursor, i64 *pRowid)
{
	test_cursor *pCur = (test_cursor *)pCursor;
	*pRowid = pCur->iRow + 1;
	return DBSQL_SUCCESS;
}

static sqlite3_module test_module = {
	0,                 /* iVersion */
	test_connect,      /* xCreate */
	test_connect,      /* xConnect */
	test_best_index,   /* xBestIndex */
	test_disconnect,   /* xDisconnect */
	test_disconnect,   /* xDestroy */
	test_open,         /* xOpen */
	test_close,        /* xClose */
	test_filter,       /* xFilter */
	test_next,         /* xNext */
	test_eof,          /* xEof */
	test_column,       /* xColumn */
	test_rowid,        /* xRowid */
	NULL,              /* xUpdate */
	NULL,              /* xBegin */
	NULL,              /* xSync */
	NULL,              /* xCommit */
	NULL,              /* xRollback */
	NULL,              /* xFindFunction */
	NULL,              /* xRename */
};

/*
 * Test 1: Module registration
 */
static MunitResult
test_module_registration(const MunitParameter params[], void *fixture)
{
	DBSQL *db;
	int rc;

	rc = dbsql_create_env(0, 0, &db, 0);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Register test module */
	rc = __dbsql_create_module(db, "test_vtab", &test_module, NULL, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Register again - should replace */
	rc = __dbsql_create_module(db, "test_vtab", &test_module, NULL, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	dbsql_close_all(db);
	return MUNIT_OK;
}

/*
 * Test 2: CREATE VIRTUAL TABLE
 */
static MunitResult
test_create_virtual_table(const MunitParameter params[], void *fixture)
{
	DBSQL *db;
	int rc;
	char *errmsg = NULL;

	rc = dbsql_create_env(0, 0, &db, 0);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Register module */
	rc = __dbsql_create_module(db, "test_vtab", &test_module, NULL, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Create virtual table */
	rc = dbsql_exec(db, "CREATE VIRTUAL TABLE temp.test USING test_vtab", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS && errmsg != NULL) {
		printf("Error: %s\n", errmsg);
		free(errmsg);
	}
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	dbsql_close_all(db);
	return MUNIT_OK;
}

/*
 * Test 3: SELECT from virtual table
 */
static int
count_callback(void *pArg, int argc, char **argv, char **colNames)
{
	int *pCount = (int *)pArg;
	(*pCount)++;
	return 0;
}

static MunitResult
test_select_from_vtab(const MunitParameter params[], void *fixture)
{
	DBSQL *db;
	int rc;
	char *errmsg = NULL;
	int count = 0;

	rc = dbsql_create_env(0, 0, &db, 0);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Register module */
	rc = __dbsql_create_module(db, "test_vtab", &test_module, NULL, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Create virtual table */
	rc = dbsql_exec(db, "CREATE VIRTUAL TABLE temp.test USING test_vtab", NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		printf("Create error: %s\n", errmsg);
		free(errmsg);
	}
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Query virtual table */
	rc = dbsql_exec(db, "SELECT * FROM test", count_callback, &count, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		printf("Select error: %s\n", errmsg);
		free(errmsg);
	}
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_int(count, ==, 10);  /* Should return 10 rows */

	dbsql_close_all(db);
	return MUNIT_OK;
}

/*
 * Test 4: Virtual table with WHERE clause
 */
static MunitResult
test_vtab_where_clause(const MunitParameter params[], void *fixture)
{
	DBSQL *db;
	int rc;
	char *errmsg = NULL;
	int count = 0;

	rc = dbsql_create_env(0, 0, &db, 0);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Register module */
	rc = __dbsql_create_module(db, "test_vtab", &test_module, NULL, NULL);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Create virtual table */
	rc = dbsql_exec(db, "CREATE VIRTUAL TABLE temp.test USING test_vtab", NULL, NULL, &errmsg);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Query with WHERE clause - note: WHERE is handled by DBSQL, not vtab */
	rc = dbsql_exec(db, "SELECT * FROM test WHERE id > 5", count_callback, &count, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		printf("Select error: %s\n", errmsg);
		free(errmsg);
	}
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_int(count, ==, 5);  /* Rows 6-10 */

	dbsql_close_all(db);
	return MUNIT_OK;
}

/*
 * Test 5: CSV virtual table
 */
static MunitResult
test_csv_vtab(const MunitParameter params[], void *fixture)
{
	DBSQL *db;
	int rc;
	char *errmsg = NULL;
	FILE *fp;
	int count = 0;

	/* Create test CSV file */
	fp = fopen("/tmp/test_vtab.csv", "w");
	munit_assert_not_null(fp);
	fprintf(fp, "name,age,city\n");
	fprintf(fp, "Alice,30,NYC\n");
	fprintf(fp, "Bob,25,LA\n");
	fprintf(fp, "Charlie,35,Chicago\n");
	fclose(fp);

	rc = dbsql_create_env(0, 0, &db, 0);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Register CSV module */
	rc = __dbsql_register_csv_module(db);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Create CSV virtual table */
	rc = dbsql_exec(db,
		"CREATE VIRTUAL TABLE temp.people USING csv("
		"filename=/tmp/test_vtab.csv, header=yes, separator=,)",
		NULL, NULL, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		printf("CSV create error: %s\n", errmsg);
		free(errmsg);
	}
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Query CSV table */
	rc = dbsql_exec(db, "SELECT * FROM people", count_callback, &count, &errmsg);
	if (rc != DBSQL_SUCCESS) {
		printf("CSV select error: %s\n", errmsg);
		free(errmsg);
	}
	munit_assert_int(rc, ==, DBSQL_SUCCESS);
	munit_assert_int(count, ==, 3);

	dbsql_close_all(db);
	unlink("/tmp/test_vtab.csv");
	return MUNIT_OK;
}

/*
 * Test 6: Error handling
 */
static MunitResult
test_vtab_error_handling(const MunitParameter params[], void *fixture)
{
	DBSQL *db;
	int rc;
	char *errmsg = NULL;

	rc = dbsql_create_env(0, 0, &db, 0);
	munit_assert_int(rc, ==, DBSQL_SUCCESS);

	/* Try to create virtual table with non-existent module */
	rc = dbsql_exec(db,
		"CREATE VIRTUAL TABLE temp.bad USING nonexistent_module",
		NULL, NULL, &errmsg);
	munit_assert_int(rc, !=, DBSQL_SUCCESS);
	if (errmsg) free(errmsg);

	dbsql_close_all(db);
	return MUNIT_OK;
}

/*
 * Test suite definition
 */
static MunitTest vtab_tests[] = {
	{"/module_registration", test_module_registration, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/create_virtual_table", test_create_virtual_table, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/select_from_vtab", test_select_from_vtab, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/vtab_where_clause", test_vtab_where_clause, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/csv_vtab", test_csv_vtab, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{"/error_handling", test_vtab_error_handling, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL},
	{NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL}
};

static const MunitSuite vtab_suite = {
	"/vtab",
	vtab_tests,
	NULL,
	1,
	MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&vtab_suite, NULL, argc, argv);
}
