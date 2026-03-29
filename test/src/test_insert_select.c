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
	const char *dbfile = "test.db";
	int row_count = 0;

	unlink(dbfile);

	printf("Creating database...\n");
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	if (rc != 0) {
		printf("FAILED: create_env rc=%d\n", rc);
		return 1;
	}

	rc = db->open(db, dbfile, 0664, &errmsg);
	if (rc != 0) {
		printf("FAILED: open rc=%d %s\n", rc, errmsg ? errmsg : "");
		db->close(db);
		return 1;
	}

	printf("Creating table...\n");
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)", NULL, NULL, &errmsg);
	printf("CREATE TABLE rc=%d%s\n", rc, errmsg ? errmsg : "");
	if (rc != 0) {
		db->close(db);
		return 1;
	}

	printf("Inserting row 1...\n");
	rc = db->exec(db, "INSERT INTO test VALUES (1, 'Alice')", NULL, NULL, &errmsg);
	printf("INSERT 1 rc=%d%s\n", rc, errmsg ? errmsg : "");
	if (rc != 0) {
		db->close(db);
		return 1;
	}

	printf("Inserting row 2...\n");
	rc = db->exec(db, "INSERT INTO test VALUES (2, 'Bob')", NULL, NULL, &errmsg);
	printf("INSERT 2 rc=%d%s\n", rc, errmsg ? errmsg : "");
	if (rc != 0) {
		db->close(db);
		return 1;
	}

	printf("\nSelecting all rows...\n");
	rc = db->exec(db, "SELECT * FROM test", callback, &row_count, &errmsg);
	printf("SELECT rc=%d%s\n", rc, errmsg ? errmsg : "");

	printf("\nResult: %d rows returned\n", row_count);
	
	db->close(db);
	unlink(dbfile);
	
	return (row_count == 2) ? 0 : 1;
}
