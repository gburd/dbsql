/**
 * Diagnostic test to confirm the DB_ENV / file-based database mismatch
 */

#include "test_framework.h"
#include "dbsql.h"
#include <db.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Test 1: Confirm that in-memory DB_ENV cannot create file-based DBs */
TEST(db_env_inmem_file_mismatch) {
	DB_ENV *dbenv;
	DB *db;
	DB_TXN *txn;
	int rc;
	const char *testfile = "test_diagnosis.db";

	/* Remove existing file */
	unlink(testfile);

	/* Create in-memory DB_ENV (same as dbsql_create_env with dir=NULL) */
	rc = db_env_create(&dbenv, 0);
	ASSERT_EQ(0, rc);

	/* Configure as in-memory */
	rc = dbenv->log_set_config(dbenv, DB_LOG_IN_MEMORY, 1);
	ASSERT_EQ(0, rc);

	rc = dbenv->set_lg_bsize(dbenv, 10 * 1024 * 1024);
	ASSERT_EQ(0, rc);

	int env_flags = DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL |
	                DB_INIT_TXN | DB_CREATE | DB_PRIVATE;

	rc = dbenv->open(dbenv, NULL, env_flags, 0);
	ASSERT_EQ(0, rc);

	/* Try to create a file-based database within the in-memory environment */
	rc = db_create(&db, dbenv, 0);
	ASSERT_EQ(0, rc);

	rc = dbenv->txn_begin(dbenv, NULL, &txn, 0);
	ASSERT_EQ(0, rc);

	/* This SHOULD FAIL with -50988 (DBSQL_CANTOPEN) or similar */
	rc = db->open(db, txn, testfile, NULL, DB_BTREE, DB_CREATE, 0);
	printf("DEBUG: db->open(file='%s', db=NULL) returned: %d (%s)\n",
	       testfile, rc, db_strerror(rc));

	/* Expected: rc != 0 (failure) */
	if (rc == 0) {
		printf("UNEXPECTED: File-based DB created in in-memory environment!\n");
		txn->commit(txn, 0);
		db->close(db, 0);
	} else {
		printf("EXPECTED: Cannot create file-based DB in in-memory environment\n");
		txn->abort(txn);
		db->close(db, 0);
	}

	dbenv->close(dbenv, 0);
	unlink(testfile);
}

/* Test 2: Confirm that file-based DB_ENV CAN create file-based DBs */
TEST(db_env_file_works) {
	DB_ENV *dbenv;
	DB *db;
	DB_TXN *txn;
	int rc;
	const char *envdir = "/tmp/claude/test_dbenv";
	const char *testfile = "test_diagnosis2.db";
	char fullpath[256];

	/* Create directory */
	mkdir(envdir, 0755);
	snprintf(fullpath, sizeof(fullpath), "%s/%s", envdir, testfile);
	unlink(fullpath);

	/* Create file-based DB_ENV */
	rc = db_env_create(&dbenv, 0);
	ASSERT_EQ(0, rc);

	int env_flags = DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL |
	                DB_INIT_TXN | DB_CREATE;

	rc = dbenv->open(dbenv, envdir, env_flags, 0);
	ASSERT_EQ(0, rc);

	/* Create a file-based database */
	rc = db_create(&db, dbenv, 0);
	ASSERT_EQ(0, rc);

	rc = dbenv->txn_begin(dbenv, NULL, &txn, 0);
	ASSERT_EQ(0, rc);

	/* This SHOULD SUCCEED */
	rc = db->open(db, txn, testfile, NULL, DB_BTREE, DB_CREATE, 0);
	printf("DEBUG: db->open(file='%s', db=NULL) returned: %d (%s)\n",
	       testfile, rc, db_strerror(rc));

	if (rc == 0) {
		printf("SUCCESS: File-based DB created in file-based environment\n");
		txn->commit(txn, 0);
		db->close(db, 0);
	} else {
		printf("UNEXPECTED FAILURE: %s\n", db_strerror(rc));
		txn->abort(txn);
		db->close(db, 0);
	}

	dbenv->close(dbenv, 0);

	/* Cleanup */
	unlink(fullpath);
	rmdir(envdir);
}

/* Test 3: Confirm that in-memory DB_ENV CAN create in-memory DBs */
TEST(db_env_inmem_works) {
	DB_ENV *dbenv;
	DB *db;
	DB_TXN *txn;
	int rc;

	/* Create in-memory DB_ENV */
	rc = db_env_create(&dbenv, 0);
	ASSERT_EQ(0, rc);

	rc = dbenv->log_set_config(dbenv, DB_LOG_IN_MEMORY, 1);
	ASSERT_EQ(0, rc);

	rc = dbenv->set_lg_bsize(dbenv, 10 * 1024 * 1024);
	ASSERT_EQ(0, rc);

	int env_flags = DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL |
	                DB_INIT_TXN | DB_CREATE | DB_PRIVATE;

	rc = dbenv->open(dbenv, NULL, env_flags, 0);
	ASSERT_EQ(0, rc);

	/* Create an in-memory database (NULL filename) */
	rc = db_create(&db, dbenv, 0);
	ASSERT_EQ(0, rc);

	rc = dbenv->txn_begin(dbenv, NULL, &txn, 0);
	ASSERT_EQ(0, rc);

	/* This SHOULD SUCCEED */
	rc = db->open(db, txn, NULL, NULL, DB_BTREE, DB_CREATE, 0);
	printf("DEBUG: db->open(file=NULL, db=NULL) returned: %d (%s)\n",
	       rc, db_strerror(rc));

	if (rc == 0) {
		printf("SUCCESS: In-memory DB created in in-memory environment\n");
		txn->commit(txn, 0);
		db->close(db, 0);
	} else {
		printf("UNEXPECTED FAILURE: %s\n", db_strerror(rc));
		txn->abort(txn);
		db->close(db, 0);
	}

	dbenv->close(dbenv, 0);
}

/* Main test runner */
int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	TEST_INIT("DB_ENV Diagnosis Tests");
	test_run_all();
	TEST_FINISH();
}
