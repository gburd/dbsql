/**
 * Debug SELECT failures
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../../build_unix.old/dbsql.h"

int
main(void)
{
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_select_debug.db";

	unlink(dbfile);

	printf("1. Creating environment...\n");
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("   Result: %d\n", rc);
	if (rc != 0) {
		fprintf(stderr, "FAILED at create_env\n");
		return 1;
	}

	printf("2. Opening database...\n");
	rc = db->open(db, dbfile, 0664, &errmsg);
	printf("   Result: %d, errmsg: %s\n", rc, errmsg ? errmsg : "NULL");
	if (rc != 0) {
		fprintf(stderr, "FAILED at open\n");
		return 1;
	}

	printf("3. Creating table...\n");
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)",
	              NULL, NULL, &errmsg);
	printf("   Result: %d, errmsg: %s\n", rc, errmsg ? errmsg : "NULL");
	if (rc != 0) {
		fprintf(stderr, "FAILED at CREATE TABLE\n");
		return 1;
	}

	printf("4. Inserting data...\n");
	rc = db->exec(db, "INSERT INTO test VALUES (1, 'first')",
	              NULL, NULL, &errmsg);
	printf("   Result: %d, errmsg: %s\n", rc, errmsg ? errmsg : "NULL");
	if (rc != 0) {
		fprintf(stderr, "FAILED at INSERT\n");
		return 1;
	}

	printf("5. Selecting data...\n");
	rc = db->exec(db, "SELECT * FROM test", NULL, NULL, &errmsg);
	printf("   Result: %d, errmsg: %s\n", rc, errmsg ? errmsg : "NULL");
	if (rc != 0) {
		fprintf(stderr, "FAILED at SELECT (error %d)\n", rc);
	} else {
		printf("   SUCCESS!\n");
	}

	printf("6. Closing database...\n");
	rc = db->close(db);
	printf("   Result: %d\n", rc);

	unlink(dbfile);
	return 0;
}
