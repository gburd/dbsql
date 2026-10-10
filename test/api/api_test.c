/*
 * test/api/api_test.c --
 *	The public C API on libdb storage: open flags, prepare/bind/step/
 *	column, error codes and messages, changes/rowid, transactions and
 *	savepoints, incremental blob I/O, online backup, and that a
 *	committed transaction survives reopening (recovery).  Every
 *	expectation is what stock SQLite 3.53.4 returns.
 *
 *	Usage: api_test [DIR]   (DIR defaults to a fresh temp directory)
 *	Exit status is the number of failed checks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dbsql.h"

static int failures, checks;

#define	CHECK(c, ...) do {						\
	checks++;							\
	if (!(c)) {							\
		failures++;						\
		printf("FAIL %s:%d: ", __FILE__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

static char dir[256];

static const char *
path(const char *name)
{
	static char buf[4][512];
	static int n;

	n = (n + 1) % 4;
	snprintf(buf[n], sizeof(buf[n]), "%s/%s", dir, name);
	return (buf[n]);
}

static int
exec(DBSQL *db, const char *sql)
{
	char *err = NULL;
	int rc;

	rc = dbsql_exec(db, sql, NULL, NULL, &err);
	if (rc != DBSQL_OK && getenv("API_TEST_VERBOSE"))
		printf("  exec(%s): %d %s\n", sql, rc, err ? err : "");
	dbsql_free(err);
	return (rc);
}

static int64_t
one_int(DBSQL *db, const char *sql)
{
	dbsql_stmt_t *st;
	int64_t v = -999;

	if (dbsql_prepare_v2(db, sql, -1, &st, NULL) != DBSQL_OK)
		return (-998);
	if (dbsql_step(st) == DBSQL_ROW)
		v = dbsql_column_int64(st, 0);
	dbsql_finalize(st);
	return (v);
}

static void
test_open(void)
{
	DBSQL *db;
	int rc;

	/* Without CREATE a missing file fails with CANTOPEN. */
	rc = dbsql_open_v2(path("missing.db"), &db, DBSQL_OPEN_READWRITE, NULL);
	CHECK(rc == DBSQL_CANTOPEN, "open missing without CREATE: %d", rc);
	dbsql_close(db);
	CHECK(access(path("missing.db"), F_OK) != 0, "file created anyway");

	/* With CREATE it exists from open, even if never written. */
	rc = dbsql_open_v2(path("new.db"), &db,
	    DBSQL_OPEN_READWRITE | DBSQL_OPEN_CREATE, NULL);
	CHECK(rc == DBSQL_OK, "open with CREATE: %d", rc);
	CHECK(dbsql_db_readonly(db, "main") == 0, "new db read-only");
	CHECK(strstr(dbsql_db_filename(db, "main"), "/new.db") != NULL,
	    "db_filename %s", dbsql_db_filename(db, "main"));
	dbsql_close(db);
	CHECK(access(path("new.db"), F_OK) == 0, "created file missing");

	/* A file that is not a database: NOTADB at the first read. */
	{
		FILE *fp = fopen(path("junk.db"), "w");
		fputs("This is not a database file at all.\n", fp);
		fclose(fp);
	}
	rc = dbsql_open(path("junk.db"), &db);
	CHECK(rc == DBSQL_OK, "open junk: %d", rc);
	rc = exec(db, "SELECT * FROM dbsql_schema");
	CHECK(rc == DBSQL_NOTADB, "read junk: %d", rc);
	CHECK(strcmp(dbsql_errmsg(db), "file is not a database") == 0,
	    "junk errmsg: %s", dbsql_errmsg(db));
	dbsql_close(db);

	/* Read-only: writes fail with READONLY. */
	rc = dbsql_open(path("ro.db"), &db);
	exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1)");
	dbsql_close(db);
	rc = dbsql_open_v2(path("ro.db"), &db, DBSQL_OPEN_READONLY, NULL);
	CHECK(rc == DBSQL_OK, "open readonly: %d", rc);
	CHECK(one_int(db, "SELECT x FROM t") == 1, "readonly read");
	rc = exec(db, "INSERT INTO t VALUES(2)");
	CHECK(rc == DBSQL_READONLY, "write readonly: %d", rc);
	CHECK(dbsql_db_readonly(db, "main") == 1, "db_readonly");
	dbsql_close(db);
}

static void
test_statements(void)
{
	DBSQL *db;
	dbsql_stmt_t *st;
	const char *tail;
	int rc, i;

	dbsql_open(path("stmt.db"), &db);
	rc = exec(db, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT, c BLOB,"
	    " d REAL)");
	CHECK(rc == DBSQL_OK, "create: %d", rc);

	rc = dbsql_prepare_v2(db, "INSERT INTO t(b, c, d) VALUES(?, ?, ?);"
	    " SELECT 1", -1, &st, &tail);
	CHECK(rc == DBSQL_OK, "prepare insert: %d", rc);
	CHECK(strcmp(tail, " SELECT 1") == 0, "tail '%s'", tail);
	CHECK(dbsql_bind_parameter_count(st) == 3, "param count");
	for (i = 0; i < 100; i++) {
		char text[32];
		snprintf(text, sizeof(text), "row %d", i);
		dbsql_bind_text(st, 1, text, -1, DBSQL_TRANSIENT);
		dbsql_bind_blob(st, 2, "\0\1\2\3", 4, DBSQL_STATIC);
		dbsql_bind_double(st, 3, i / 4.0);
		rc = dbsql_step(st);
		CHECK(rc == DBSQL_DONE, "step insert %d: %d", i, rc);
		CHECK(dbsql_changes(db) == 1, "changes");
		CHECK(dbsql_last_insert_rowid(db) == i + 1, "rowid %lld",
		    (long long)dbsql_last_insert_rowid(db));
		dbsql_reset(st);
	}
	CHECK(dbsql_bind_int(st, 4, 1) == DBSQL_RANGE, "bind out of range");
	dbsql_finalize(st);
	CHECK(dbsql_total_changes(db) == 100, "total_changes %d",
	    dbsql_total_changes(db));

	rc = dbsql_prepare_v2(db, "SELECT a, b, c, d, typeof(c) FROM t"
	    " WHERE a = ?1", -1, &st, NULL);
	CHECK(rc == DBSQL_OK, "prepare select: %d", rc);
	dbsql_bind_int64(st, 1, 42);
	CHECK(dbsql_step(st) == DBSQL_ROW, "row 42");
	CHECK(dbsql_column_count(st) == 5, "column count");
	CHECK(dbsql_column_int64(st, 0) == 42, "a");
	CHECK(strcmp((const char *)dbsql_column_text(st, 1), "row 41") == 0,
	    "b = %s", dbsql_column_text(st, 1));
	CHECK(dbsql_column_bytes(st, 2) == 4 &&
	    memcmp(dbsql_column_blob(st, 2), "\0\1\2\3", 4) == 0, "c");
	CHECK(dbsql_column_double(st, 3) == 41 / 4.0, "d");
	CHECK(dbsql_column_type(st, 2) == DBSQL_BLOB, "type c");
	CHECK(strcmp(dbsql_column_name(st, 1), "b") == 0, "column name");
	CHECK(dbsql_step(st) == DBSQL_DONE, "done");
	dbsql_finalize(st);

	/* Errors: codes, messages, offsets. */
	rc = dbsql_prepare_v2(db, "SELECT nosuch FROM t", -1, &st, NULL);
	CHECK(rc == DBSQL_ERROR && st == NULL, "bad column: %d", rc);
	CHECK(strcmp(dbsql_errmsg(db), "no such column: nosuch") == 0,
	    "errmsg %s", dbsql_errmsg(db));
	CHECK(dbsql_error_offset(db) == 7, "error offset %d",
	    dbsql_error_offset(db));
	rc = exec(db, "INSERT INTO t(a) VALUES(1)");
	CHECK(rc == DBSQL_CONSTRAINT, "pk conflict: %d", rc);
	CHECK(dbsql_extended_errcode(db) == DBSQL_CONSTRAINT_PRIMARYKEY,
	    "extended %d", dbsql_extended_errcode(db));
	CHECK(strcmp(dbsql_errstr(DBSQL_BUSY), "database is locked") == 0,
	    "errstr BUSY");
	CHECK(strcmp(dbsql_errstr(DBSQL_NOTADB), "file is not a database")
	    == 0, "errstr NOTADB");
	for (i = 0; i <= DBSQL_WARNING; i++)
		CHECK(dbsql_errstr(i) != NULL && dbsql_errstr(i)[0] != '\0',
		    "errstr(%d)", i);
	dbsql_close(db);
}

static void
test_transactions(void)
{
	DBSQL *db, *db2;
	int rc;

	dbsql_open(path("txn.db"), &db);
	exec(db, "CREATE TABLE t(x)");
	CHECK(dbsql_get_autocommit(db) == 1, "autocommit");
	CHECK(dbsql_txn_state(db, "main") == DBSQL_TXN_NONE, "txn none");
	exec(db, "BEGIN; INSERT INTO t VALUES(1)");
	CHECK(dbsql_get_autocommit(db) == 0, "in txn");
	CHECK(dbsql_txn_state(db, "main") == DBSQL_TXN_WRITE, "txn write");
	exec(db, "SAVEPOINT a; INSERT INTO t VALUES(2); ROLLBACK TO a;"
	    " INSERT INTO t VALUES(3); RELEASE a");
	exec(db, "COMMIT");
	CHECK(one_int(db, "SELECT group_concat(x) = '1,3' FROM t") == 1,
	    "savepoint rollback");
	exec(db, "BEGIN; DELETE FROM t; ROLLBACK");
	CHECK(one_int(db, "SELECT count(*) FROM t") == 2, "rollback");

	/* A second connection: one writer at a time, BUSY not a hang. */
	dbsql_open(path("txn.db"), &db2);
	dbsql_busy_timeout(db2, 50);
	exec(db, "BEGIN IMMEDIATE");
	rc = exec(db2, "BEGIN IMMEDIATE");
	CHECK(rc == DBSQL_BUSY, "second writer: %d", rc);
	CHECK(one_int(db2, "SELECT count(*) FROM t") == 2, "reader reads");
	exec(db, "INSERT INTO t VALUES(4); COMMIT");
	CHECK(one_int(db2, "SELECT count(*) FROM t") == 3, "sees commit");
	rc = exec(db2, "BEGIN IMMEDIATE; INSERT INTO t VALUES(5); COMMIT");
	CHECK(rc == DBSQL_OK, "second writer after commit: %d", rc);
	dbsql_close(db2);
	dbsql_close(db);

	/* Committed data survives reopening (recovery on open). */
	dbsql_open(path("txn.db"), &db);
	CHECK(one_int(db, "SELECT count(*) FROM t") == 4, "after reopen");
	CHECK(one_int(db, "SELECT count(*) FROM pragma_integrity_check"
	    " WHERE integrity_check <> 'ok'") == 0, "integrity");
	dbsql_close(db);
}

static void
test_blob_backup(void)
{
	DBSQL *db, *dst;
	dbsql_blob_t *b;
	dbsql_backup_t *bk;
	char buf[16];
	int rc;

	dbsql_open(path("blob.db"), &db);
	exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB);"
	    " INSERT INTO t VALUES(7, zeroblob(10))");
	rc = dbsql_blob_open(db, "main", "t", "data", 7, 1, &b);
	CHECK(rc == DBSQL_OK, "blob_open: %d", rc);
	CHECK(dbsql_blob_bytes(b) == 10, "blob_bytes");
	rc = dbsql_blob_write(b, "hello", 5, 3);
	CHECK(rc == DBSQL_OK, "blob_write: %d", rc);
	rc = dbsql_blob_read(b, buf, 5, 3);
	CHECK(rc == DBSQL_OK && memcmp(buf, "hello", 5) == 0, "blob_read");
	rc = dbsql_blob_read(b, buf, 5, 8);
	CHECK(rc == DBSQL_ERROR, "blob read past end: %d", rc);
	dbsql_blob_close(b);
	CHECK(one_int(db, "SELECT substr(data, 4, 5) = CAST('hello' AS BLOB)"
	    " FROM t") == 1,
	    "blob persisted");

	exec(db, "CREATE TABLE u(x); INSERT INTO u VALUES(1),(2),(3)");
	dbsql_open(path("copy.db"), &dst);
	bk = dbsql_backup_init(dst, "main", db, "main");
	CHECK(bk != NULL, "backup_init");
	if (bk != NULL) {
		rc = dbsql_backup_step(bk, -1);
		CHECK(rc == DBSQL_DONE, "backup_step: %d", rc);
		CHECK(dbsql_backup_finish(bk) == DBSQL_OK, "backup_finish");
	}
	CHECK(one_int(dst, "SELECT sum(x) FROM u") == 6, "backup copy");
	CHECK(one_int(dst, "SELECT length(data) FROM t") == 10, "copy blob");
	dbsql_close(dst);
	dbsql_close(db);
}

int
main(int argc, char **argv)
{
	if (argc > 1)
		snprintf(dir, sizeof(dir), "%s", argv[1]);
	else {
		snprintf(dir, sizeof(dir), "/tmp/dbsql-api.XXXXXX");
		if (mkdtemp(dir) == NULL)
			return (perror("mkdtemp"), 1);
	}
	CHECK(dbsql_threadsafe() != 0, "threadsafe");
	CHECK(strcmp(dbsql_libversion(), DBSQL_VERSION) == 0, "libversion");
	CHECK(strstr(dbsql_sourceid(), "libdb") != NULL, "sourceid");
	test_open();
	test_statements();
	test_transactions();
	test_blob_backup();
	printf("api: %d checks, %d failures\n", checks, failures);
	return (failures);
}
