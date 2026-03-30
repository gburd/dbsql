/**
 * Test to trigger VDBE error and identify failing opcode
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dbsql.h"

int main() {
	DBSQL *db = NULL;
	dbsql_stmt_t *stmt = NULL;
	int rc;
	char *errmsg = NULL;
	const char *tail = NULL;

	printf("=== VDBE Debug Test ===\n\n");

	/* Use in-memory database to avoid file I/O issues */
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "FAILED: create_env=%d\n", rc);
		return 1;
	}

	rc = db->open(db, ":memory:", 0664, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "FAILED: open=%d, err=%s\n", rc, errmsg);
		return 1;
	}

	printf("Creating table...\n");
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)", NULL, NULL, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "FAILED: CREATE TABLE=%d, err=%s\n", rc, errmsg);
		return 1;
	}

	printf("Inserting data...\n");
	rc = db->exec(db, "INSERT INTO test VALUES (1, 'Alice')", NULL, NULL, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "FAILED: INSERT=%d, err=%s\n", rc, errmsg);
		return 1;
	}

	printf("Preparing SELECT statement...\n");
	rc = db->prepare(db, "SELECT * FROM test", &tail, &stmt, &errmsg);
	if (rc != 0 || stmt == NULL) {
		fprintf(stderr, "FAILED: prepare=%d, err=%s\n", rc, errmsg);
		return 1;
	}
	printf("  prepare() succeeded, stmt=%p\n", (void*)stmt);

	printf("\nCalling step() - watch for VDBE ERROR output:\n");
	printf("================================================\n");

	int n_cols = 0;
	const char **values = NULL;
	const char **col_names = NULL;

	rc = db->step(stmt, &n_cols, &values, &col_names);

	printf("================================================\n");
	printf("step() returned: %d\n", rc);
	printf("  DBSQL_ROW=%d, DBSQL_DONE=%d, DBSQL_ERROR=%d\n",
	       DBSQL_ROW, DBSQL_DONE, DBSQL_ERROR);

	if (rc == DBSQL_ROW) {
		printf("SUCCESS: Got a row with %d columns\n", n_cols);
	} else if (rc == DBSQL_ERROR) {
		printf("ERROR: step() returned DBSQL_ERROR (check stderr for VDBE ERROR)\n");
	}

	db->finalize(stmt, &errmsg);
	db->close(db);

	return (rc == DBSQL_ROW || rc == DBSQL_DONE) ? 0 : 1;
}
