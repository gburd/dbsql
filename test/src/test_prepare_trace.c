/**
 * Test prepared statements with VDBE tracing enabled
 */
#include <stdio.h>
#include <stdlib.h>
#include "dbsql.h"
#include <unistd.h>

int main() {
	DBSQL *db = NULL;
	dbsql_stmt_t *stmt = NULL;
	int rc;
	char *errmsg = NULL;
	const char *tail = NULL;
	const char *dbfile = ":memory:";

	printf("=== Prepared Statement VDBE Trace Test ===\n\n");

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	if (rc \!= 0) {
		printf("FAILED: create_env returned %d\n", rc);
		return 1;
	}

	rc = db->open(db, dbfile, 0664, &errmsg);
	if (rc \!= 0) {
		printf("FAILED: open returned %d\n", rc);
		return 1;
	}

	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)", NULL, NULL, &errmsg);
	if (rc \!= 0) {
		printf("FAILED: CREATE TABLE returned %d\n", rc);
		return 1;
	}

	rc = db->exec(db, "INSERT INTO test VALUES (1, 'Alice')", NULL, NULL, &errmsg);
	if (rc \!= 0) {
		printf("FAILED: INSERT returned %d\n", rc);
		return 1;
	}

	printf("Preparing SELECT statement...\n");
	rc = db->prepare(db, "SELECT * FROM test", &tail, &stmt, &errmsg);
	if (rc \!= 0 || stmt == NULL) {
		printf("FAILED: prepare returned %d\n", rc);
		return 1;
	}

	/* Enable VDBE tracing */
	stmt->trace = stderr;
	printf("\n--- Starting VDBE trace ---\n");

	int n_cols = 0;
	const char **values = NULL;
	const char **col_names = NULL;

	rc = db->step(stmt, &n_cols, &values, &col_names);
	printf("--- End VDBE trace ---\n\n");
	printf("step() returned: %d (DBSQL_ROW=%d, DBSQL_DONE=%d, DBSQL_ERROR=%d)\n", 
	       rc, DBSQL_ROW, DBSQL_DONE, DBSQL_ERROR);

	db->finalize(stmt, &errmsg);
	db->close(db);

	return (rc == DBSQL_ROW || rc == DBSQL_DONE) ? 0 : 1;
}
