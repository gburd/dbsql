/**
 * DBSQL Memory Management Tests
 *
 * Tests for memory allocation, deallocation, and leak detection
 */

#include "test_framework.h"
#include "dbsql.h"
#include <pthread.h>
#include <unistd.h>

/* Test basic memory operations */
TEST(memory_basic_allocation) {
	DBSQL *db = NULL;
	int rc;

	/* Open database (allocates memory) */
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);
	ASSERT_NOT_NULL(db);

	/* Close database (frees memory) */
	rc = db->close(db);
	ASSERT_EQ(0, rc);
}

TEST(memory_multiple_databases) {
	DBSQL *db1 = NULL;
	DBSQL *db2 = NULL;
	int rc;

	/* Create two separate databases */
	rc = dbsql_create_env(&db1, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = dbsql_create_env(&db2, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	/* Close both */
	rc = db1->close(db1);
	ASSERT_EQ(0, rc);

	rc = db2->close(db2);
	ASSERT_EQ(0, rc);
}

TEST(memory_large_table) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_memory_large.db";
	int i;

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER, data TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert many rows to stress memory */
	for (i = 0; i < 100; i++) {
		char sql[256];
		snprintf(sql, sizeof(sql),
		         "INSERT INTO test VALUES (%d, 'data-%d')", i, i);
		rc = db->exec(db, sql, NULL, NULL, &errmsg);
		if (rc != 0) break;
	}

	/* Query all rows */
	rc = db->exec(db, "SELECT * FROM test", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(memory_repeated_operations) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_memory_repeat.db";
	int i;

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create table once */
	rc = db->exec(db, "CREATE TABLE test (counter INTEGER)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Repeated insert and select */
	for (i = 0; i < 50; i++) {
		char sql[256];
		snprintf(sql, sizeof(sql),
		         "INSERT INTO test VALUES (%d)", i);
		rc = db->exec(db, sql, NULL, NULL, &errmsg);
		if (rc != 0) break;

		rc = db->exec(db, "SELECT * FROM test", NULL, NULL, &errmsg);
		if (rc != 0) break;
	}

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(memory_string_allocation) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_memory_string.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Test string memory allocation */
	rc = db->exec(db, "CREATE TABLE test (text TEXT)",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert various length strings */
	rc = db->exec(db, "INSERT INTO test VALUES ('short')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES ('A longer string with more characters')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES ('Another test string for memory allocation testing purposes')",
	              NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Query strings back */
	rc = db->exec(db, "SELECT * FROM test", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

TEST(memory_transaction_rollback) {
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_memory_txn.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Transaction with rollback - should clean up memory */
	rc = db->exec(db, "BEGIN", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (1)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "INSERT INTO test VALUES (2)", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "ROLLBACK", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Verify table is empty */
	rc = db->exec(db, "SELECT * FROM test", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Thread test data structure */
struct thread_data {
	int thread_id;
	int result;
};

/* Thread function for concurrent memory test */
static void *
thread_func(void *arg)
{
	struct thread_data *data = (struct thread_data *)arg;
	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	char dbfile[256];

	snprintf(dbfile, sizeof(dbfile), "test_memory_thread_%d.db", data->thread_id);
	unlink(dbfile);

	/* Each thread creates its own database */
	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	if (rc != 0) {
		data->result = -1;
		return NULL;
	}

	rc = db->open(db, dbfile, 0664, &errmsg);
	if (rc != 0) {
		db->close(db);
		data->result = -1;
		return NULL;
	}

	/* Perform operations */
	rc = db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg);
	if (rc == 0) {
		rc = db->exec(db, "INSERT INTO test VALUES (1)", NULL, NULL, &errmsg);
	}
	if (rc == 0) {
		rc = db->exec(db, "SELECT * FROM test", NULL, NULL, &errmsg);
	}

	db->close(db);
	unlink(dbfile);

	data->result = rc;
	return NULL;
}

TEST(memory_concurrent_access) {
	pthread_t threads[4];
	struct thread_data thread_data[4];
	int i;

	/* Create multiple threads accessing separate databases */
	for (i = 0; i < 4; i++) {
		thread_data[i].thread_id = i;
		thread_data[i].result = -1;
		pthread_create(&threads[i], NULL, thread_func, &thread_data[i]);
	}

	/* Wait for all threads */
	for (i = 0; i < 4; i++) {
		pthread_join(threads[i], NULL);
		/* Check thread succeeded */
		ASSERT_EQ(0, thread_data[i].result);
	}
}

/* Main test runner */
int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	TEST_INIT("DBSQL Memory Tests");
	test_run_all();
	TEST_FINISH();
}
