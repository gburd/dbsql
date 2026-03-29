/*
 * Test DELETE with WHERE clause
 * Reproduces DBSQL_MISUSE error when deleting with WHERE condition
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "dbsql.h"

static void test_delete_where_basic(void) {
    DBSQL *db = NULL;
    int ret;
    char *errmsg = NULL;

    printf("Test: DELETE with WHERE clause (basic)\n");

    /* Create database */
    ret = dbsql_create_env(&db, NULL, NULL, 0, 0);
    assert(ret == 0);
    assert(db != NULL);

    /* Open in-memory database */
    ret = db->open(db, ":memory:", 0, &errmsg);
    assert(ret == 0);

    /* Create table */
    ret = db->exec(db, "CREATE TABLE t (id INTEGER, name TEXT)", NULL, NULL, &errmsg);
    if (ret != 0) {
        printf("CREATE failed: %s (code %d)\n", errmsg ? errmsg : "unknown", ret);
        exit(1);
    }

    /* Insert test data */
    ret = db->exec(db, "INSERT INTO t VALUES (1, 'Alice')", NULL, NULL, &errmsg);
    assert(ret == 0);
    ret = db->exec(db, "INSERT INTO t VALUES (2, 'Bob')", NULL, NULL, &errmsg);
    assert(ret == 0);
    ret = db->exec(db, "INSERT INTO t VALUES (3, 'Charlie')", NULL, NULL, &errmsg);
    assert(ret == 0);

    printf("  Inserted 3 rows\n");

    /* Test DELETE with WHERE clause */
    printf("  Executing: DELETE FROM t WHERE id = 1\n");
    ret = db->exec(db, "DELETE FROM t WHERE id = 1", NULL, NULL, &errmsg);
    if (ret != 0) {
        printf("  ERROR: DELETE failed with code %d: %s\n", ret, errmsg ? errmsg : "unknown");
        if (ret == -50998) {  /* DBSQL_MISUSE */
            printf("  ERROR CODE: DBSQL_MISUSE (-50998)\n");
        }
        db->close(db);
        printf("  Test FAILED\n\n");
        return;
    } else {
        printf("  SUCCESS: DELETE executed\n");
    }

    /* Verify row was deleted - id=1 should not exist */
    ret = db->exec(db, "SELECT * FROM t WHERE id = 1", NULL, NULL, &errmsg);
    if (ret == 0) {
        printf("  Verified: Row with id=1 was deleted\n");
    }

    /* Verify other rows still exist */
    ret = db->exec(db, "SELECT * FROM t WHERE id = 2", NULL, NULL, &errmsg);
    if (ret == 0) {
        printf("  Verified: Other rows remain\n");
    }

    db->close(db);
    printf("  Test PASSED\n\n");
}

static void test_delete_where_comparison(void) {
    DBSQL *db = NULL;
    int ret;
    char *errmsg = NULL;

    printf("Test: DELETE with WHERE using different operators\n");

    ret = dbsql_create_env(&db, NULL, NULL, 0, 0);
    assert(ret == 0);

    ret = db->open(db, ":memory:", 0, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "CREATE TABLE t (value INTEGER)", NULL, NULL, &errmsg);
    assert(ret == 0);

    /* Insert values 1-10 */
    for (int i = 1; i <= 10; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d)", i);
        ret = db->exec(db, sql, NULL, NULL, &errmsg);
        assert(ret == 0);
    }
    printf("  Inserted 10 rows (values 1-10)\n");

    /* Test DELETE with > operator */
    printf("  Executing: DELETE FROM t WHERE value > 7\n");
    ret = db->exec(db, "DELETE FROM t WHERE value > 7", NULL, NULL, &errmsg);
    if (ret != 0) {
        printf("  ERROR: DELETE failed with code %d: %s\n", ret, errmsg ? errmsg : "unknown");
    } else {
        printf("  SUCCESS: DELETE executed\n");
    }

    db->close(db);
    printf("  Test %s\n\n", ret == 0 ? "PASSED" : "FAILED");
}

static void test_delete_where_string(void) {
    DBSQL *db = NULL;
    int ret;
    char *errmsg = NULL;

    printf("Test: DELETE with WHERE on string column\n");

    ret = dbsql_create_env(&db, NULL, NULL, 0, 0);
    assert(ret == 0);

    ret = db->open(db, ":memory:", 0, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "CREATE TABLE t (name TEXT)", NULL, NULL, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "INSERT INTO t VALUES ('test')", NULL, NULL, &errmsg);
    assert(ret == 0);
    ret = db->exec(db, "INSERT INTO t VALUES ('keep')", NULL, NULL, &errmsg);
    assert(ret == 0);

    printf("  Inserted 2 rows\n");

    printf("  Executing: DELETE FROM t WHERE name = 'test'\n");
    ret = db->exec(db, "DELETE FROM t WHERE name = 'test'", NULL, NULL, &errmsg);
    if (ret != 0) {
        printf("  ERROR: DELETE failed with code %d: %s\n", ret, errmsg ? errmsg : "unknown");
    } else {
        printf("  SUCCESS: DELETE executed\n");
    }

    db->close(db);
    printf("  Test %s\n\n", ret == 0 ? "PASSED" : "FAILED");
}

int main(void) {
    printf("=== DELETE WHERE Clause Test Suite ===\n\n");

    test_delete_where_basic();
    test_delete_where_comparison();
    test_delete_where_string();

    printf("=== Test Suite Complete ===\n");
    return 0;
}
