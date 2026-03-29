/**
 * Minimal INSERT test copied from test_api
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* BSD types for Berkeley DB compatibility with -std=c99 */
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned char u_char;
typedef unsigned short u_short;

#include "../../build_unix.old/dbsql.h"

int
main(void)
{
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_insert_minimal.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "create_env failed: %d\n", rc);
		return 1;
	}

	rc = db->open(db, dbfile, 0664, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "open failed: %s\n", errmsg ? errmsg : "unknown");
		return 1;
	}

	/* Create table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, name TEXT)",
	              NULL, NULL, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "CREATE failed: %s\n", errmsg ? errmsg : "unknown");
		return 1;
	}

	/* Insert data */
	rc = db->exec(db, "INSERT INTO test VALUES (1, 'Alice')",
	              NULL, NULL, &errmsg);
	if (rc != 0) {
		fprintf(stderr, "INSERT failed (rc=%d): %s\n", rc, errmsg ? errmsg : "unknown");
		return 1;
	}

	printf("SUCCESS: INSERT worked\n");

	rc = db->close(db);
	unlink(dbfile);
	return 0;
}
