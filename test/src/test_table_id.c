/**
 * Test to trace table ID through CREATE and INSERT
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* BSD types for Berkeley DB compatibility */
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned char u_char;
typedef unsigned short u_short;

#include "../../build_unix.old/dbsql.h"

/* Enable VDBE tracing by setting environment variable */
int
main(void)
{
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_table_id.db";

	unlink(dbfile);

	/* Enable SQL tracing if available */
	setenv("DBSQL_DEBUG", "1", 1);

	printf("Creating environment and database...\n");
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "FAILED at create_env: %d\n", rc);
		return 1;
	}

	rc = db->open(db, dbfile, 0664, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "FAILED at open: %d - %s\n", rc, errmsg ? errmsg : "?");
		return 1;
	}

	printf("\n=== Step 1: CREATE TABLE ===\n");
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)",
	              NULL, NULL, &errmsg);
	printf("CREATE TABLE result: %d", rc);
	if (errmsg) printf(" (%s)", errmsg);
	printf("\n");
	if (rc != 0) {
		db->close(db);
		return 1;
	}

	printf("\n=== Step 2: INSERT ===\n");
	rc = db->exec(db, "INSERT INTO test VALUES (1, 'first')",
	              NULL, NULL, &errmsg);
	printf("INSERT result: %d", rc);
	if (errmsg) printf(" (%s)", errmsg);
	printf("\n");

	printf("\n=== Step 3: Closing ===\n");
	rc = db->close(db);
	printf("CLOSE result: %d\n", rc);

	unlink(dbfile);
	return (rc == 0) ? 0 : 1;
}
