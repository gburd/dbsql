#include <stdio.h>
#include "dbsql.h"

int main() {
	DBSQL *db;
	dbsql_stmt_t *stmt;
	char *errmsg = NULL;
	const char *tail;
	int rc, n_cols;
	const char **values, **cols;

	dbsql_create_env(&db, NULL, NULL, 0, 0);
	db->open(db, ":memory:", 0664, &errmsg);
	db->exec(db, "CREATE TABLE t (x INT)", NULL, NULL, &errmsg);
	db->exec(db, "INSERT INTO t VALUES (42)", NULL, NULL, &errmsg);
	
	printf("Testing prepared statement row retrieval:\n");
	db->prepare(db, "SELECT * FROM t", &tail, &stmt, &errmsg);
	
	rc = db->step(stmt, &n_cols, &values, &cols);
	printf("step() = %d (want DBSQL_ROW=%d)\n", rc, DBSQL_ROW);
	printf("n_cols = %d\n", n_cols);
	if (rc == DBSQL_ROW && values) {
		printf("SUCCESS: Got row with value=%s\n", values[0]);
	} else {
		printf("FAILED: No row returned\n");
	}
	
	db->finalize(stmt, &errmsg);
	db->close(db);
	return (rc == DBSQL_ROW) ? 0 : 1;
}
