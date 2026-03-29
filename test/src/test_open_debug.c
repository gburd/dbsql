/**
 * Debug db->open() failure
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
	const char *dbfile = "test_open_debug.db";

	unlink(dbfile);

	printf("1. Creating environment...\n");
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	printf("   Result: %d\n", rc);
	if (rc != 0) {
		fprintf(stderr, "FAILED at create_env\n");
		return 1;
	}

	printf("2. Opening database file '%s'...\n", dbfile);
	rc = db->open(db, dbfile, 0664, &errmsg);
	printf("   Result: %d\n", rc);
	printf("   Error message: %s\n", errmsg ? errmsg : "NULL");
	if (rc != 0) {
		fprintf(stderr, "FAILED at open (error %d)\n", rc);
		if (errmsg) {
			fprintf(stderr, "Error details: %s\n", errmsg);
		}
		/* Try to close anyway */
		db->close(db);
		unlink(dbfile);
		return 1;
	}

	printf("3. Closing database...\n");
	rc = db->close(db);
	printf("   Result: %d\n", rc);

	unlink(dbfile);

	printf("\nSUCCESS!\n");
	return 0;
}
