#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned char u_char;
typedef unsigned short u_short;

#include "../../build_unix.old/dbsql.h"

static int callback(void *arg, int argc, char **argv, char **colnames) {
	int *count = (int*)arg;
	(*count)++;
	printf("Row %d: ", *count);
	for (int i = 0; i < argc; i++) {
		printf("%s=%s ", colnames[i], argv[i] ? argv[i] : "NULL");
	}
	printf("\n");
	return 0;
}

int main(void) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_select.db";
	int row_count = 0;

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	if (rc != 0) return 1;

	rc = db->open(db, dbfile, 0664, &errmsg);
	if (rc != 0) {
		db->close(db);
		return 1;
	}

	/* Create and populate table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)", NULL, NULL, &errmsg);
	if (rc != 0) {
		printf("CREATE failed: %d %s\n", rc, errmsg ? errmsg : "");
		db->close(db);
		return 1;
	}

	rc = db->exec(db, "INSERT INTO test VALUES (1, 'Alice')", NULL, NULL, &errmsg);
	if (rc != 0) {
		printf("INSERT 1 failed: %d %s\n", rc, errmsg ? errmsg : "");
		db->close(db);
		return 1;
	}

	rc = db->exec(db, "INSERT INTO test VALUES (2, 'Bob')", NULL, NULL, &errmsg);
	if (rc != 0) {
		printf("INSERT 2 failed: %d %s\n", rc, errmsg ? errmsg : "");
		db->close(db);
		return 1;
	}

	/* Test SELECT */
	printf("\n=== Testing SELECT ===\n");
	rc = db->exec(db, "SELECT * FROM test", callback, &row_count, &errmsg);
	if (rc != 0) {
		printf("SELECT failed: %d %s\n", rc, errmsg ? errmsg : "");
		db->close(db);
		return 1;
	}

	printf("\nTotal rows: %d\n", row_count);
	
	if (row_count == 2) {
		printf("SUCCESS: SELECT returned 2 rows\n");
	} else {
		printf("FAILURE: Expected 2 rows, got %d\n", row_count);
	}

	db->close(db);
	unlink(dbfile);
	return (row_count == 2) ? 0 : 1;
}
