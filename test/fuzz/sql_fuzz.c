/*
 * test/fuzz/sql_fuzz.c --
 *	libFuzzer target: the input is SQL, run against a fresh in-memory
 *	database and then against a file database (libdb storage, logs,
 *	recovery on the next open).  Any crash, sanitizer report or leak is
 *	a failure; SQL errors are expected.  A progress handler bounds the
 *	work per input.
 *
 *	build: clang -fsanitize=fuzzer,address,undefined ... (see run.sh)
 */
#define _GNU_SOURCE		/* strcasestr */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dbsql.h"

static int
progress(void *arg)
{
	return (++*(int *)arg > 100000);
}

static void
run(const char *name, const char *sql)
{
	DBSQL *db;
	int steps;

	if (dbsql_open(name, &db) != DBSQL_OK) {
		dbsql_close(db);
		return;
	}
	steps = 0;
	dbsql_progress_handler(db, 100, progress, &steps);
	dbsql_limit(db, DBSQL_LIMIT_LENGTH, 1000000);
	dbsql_limit(db, DBSQL_LIMIT_SQL_LENGTH, 100000);
	dbsql_db_config(db, DBSQL_DBCONFIG_DEFENSIVE, 1, NULL);
	(void)dbsql_exec(db, sql, NULL, NULL, NULL);
	dbsql_close(db);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static char dir[64];
	char path[96], *sql;

	if (size > 50000)
		return (0);
	if (dir[0] == '\0') {
		strcpy(dir, "/tmp/dbsql-fuzz.XXXXXX");
		if (mkdtemp(dir) == NULL)
			abort();
	}
	if ((sql = malloc(size + 1)) == NULL)
		return (0);
	memcpy(sql, data, size);
	sql[size] = '\0';
	/*
	 * Fuzz input must not write other files: skip any input naming
	 * ATTACH or VACUUM (VACUUM INTO), in any case.  (Extension loading
	 * is off by default.)
	 */
	if (strcasestr(sql, "attach") == NULL &&
	    strcasestr(sql, "vacuum") == NULL) {
		run(":memory:", sql);
		snprintf(path, sizeof(path), "%s/f.db", dir);
		run(path, sql);
	}
	free(sql);
	return (0);
}
