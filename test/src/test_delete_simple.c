/*
 * Simple DELETE WHERE tests - focus on testing DELETE succeeds
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "dbsql.h"

static void test_delete_basic(void) {
    DBSQL *db = NULL;
    int ret;
    char *errmsg = NULL;

    printf("Test 1: DELETE single row\n");

    ret = dbsql_create_env(&db, NULL, NULL, 0, 0);
    assert(ret == 0);
    ret = db->open(db, ":memory:", 0, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "CREATE TABLE t (id INTEGER, name TEXT)", NULL, NULL, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "INSERT INTO t VALUES (1, 'Alice')", NULL, NULL, &errmsg);
    assert(ret == 0);
    ret = db->exec(db, "INSERT INTO t VALUES (2, 'Bob')", NULL, NULL, &errmsg);
    assert(ret == 0);
    ret = db->exec(db, "INSERT INTO t VALUES (3, 'Charlie')", NULL, NULL, &errmsg);
    assert(ret == 0);

    /* This is the key test - DELETE with WHERE should work */
    ret = db->exec(db, "DELETE FROM t WHERE id = 2", NULL, NULL, &errmsg);
    if (ret != 0) {
        printf("  FAILED: %s (code %d)\n", errmsg ? errmsg : "unknown", ret);
        db->close(db);
        exit(1);
    }
    printf("  SUCCESS: DELETE FROM t WHERE id = 2\n");

    db->close(db);
    printf("  PASSED\n\n");
}

static void test_delete_multiple(void) {
    DBSQL *db = NULL;
    int ret;
    char *errmsg = NULL;

    printf("Test 2: DELETE multiple rows\n");

    ret = dbsql_create_env(&db, NULL, NULL, 0, 0);
    assert(ret == 0);
    ret = db->open(db, ":memory:", 0, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "CREATE TABLE nums (x INTEGER)", NULL, NULL, &errmsg);
    assert(ret == 0);

    for (int i = 1; i <= 10; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO nums VALUES (%d)", i);
        ret = db->exec(db, sql, NULL, NULL, &errmsg);
        assert(ret == 0);
    }

    ret = db->exec(db, "DELETE FROM nums WHERE x > 5", NULL, NULL, &errmsg);
    if (ret != 0) {
        printf("  FAILED: %s (code %d)\n", errmsg ? errmsg : "unknown", ret);
        db->close(db);
        exit(1);
    }
    printf("  SUCCESS: DELETE FROM nums WHERE x > 5\n");

    db->close(db);
    printf("  PASSED\n\n");
}

static void test_delete_string_match(void) {
    DBSQL *db = NULL;
    int ret;
    char *errmsg = NULL;

    printf("Test 3: DELETE with string comparison\n");

    ret = dbsql_create_env(&db, NULL, NULL, 0, 0);
    assert(ret == 0);
    ret = db->open(db, ":memory:", 0, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "CREATE TABLE items (name TEXT, type TEXT)", NULL, NULL, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "INSERT INTO items VALUES ('apple', 'fruit')", NULL, NULL, &errmsg);
    assert(ret == 0);
    ret = db->exec(db, "INSERT INTO items VALUES ('carrot', 'vegetable')", NULL, NULL, &errmsg);
    assert(ret == 0);
    ret = db->exec(db, "INSERT INTO items VALUES ('banana', 'fruit')", NULL, NULL, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "DELETE FROM items WHERE type = 'fruit'", NULL, NULL, &errmsg);
    if (ret != 0) {
        printf("  FAILED: %s (code %d)\n", errmsg ? errmsg : "unknown", ret);
        db->close(db);
        exit(1);
    }
    printf("  SUCCESS: DELETE FROM items WHERE type = 'fruit'\n");

    db->close(db);
    printf("  PASSED\n\n");
}

static void test_delete_and_condition(void) {
    DBSQL *db = NULL;
    int ret;
    char *errmsg = NULL;

    printf("Test 4: DELETE with AND condition\n");

    ret = dbsql_create_env(&db, NULL, NULL, 0, 0);
    assert(ret == 0);
    ret = db->open(db, ":memory:", 0, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "CREATE TABLE products (id INTEGER, price INTEGER)", NULL, NULL, &errmsg);
    assert(ret == 0);

    for (int i = 1; i <= 10; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO products VALUES (%d, %d)", i, i * 10);
        ret = db->exec(db, sql, NULL, NULL, &errmsg);
        assert(ret == 0);
    }

    ret = db->exec(db, "DELETE FROM products WHERE id > 5 AND price < 90", NULL, NULL, &errmsg);
    if (ret != 0) {
        printf("  FAILED: %s (code %d)\n", errmsg ? errmsg : "unknown", ret);
        db->close(db);
        exit(1);
    }
    printf("  SUCCESS: DELETE FROM products WHERE id > 5 AND price < 90\n");

    db->close(db);
    printf("  PASSED\n\n");
}

static void test_delete_no_match(void) {
    DBSQL *db = NULL;
    int ret;
    char *errmsg = NULL;

    printf("Test 5: DELETE with no matching rows\n");

    ret = dbsql_create_env(&db, NULL, NULL, 0, 0);
    assert(ret == 0);
    ret = db->open(db, ":memory:", 0, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "CREATE TABLE t (x INTEGER)", NULL, NULL, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "INSERT INTO t VALUES (1)", NULL, NULL, &errmsg);
    assert(ret == 0);
    ret = db->exec(db, "INSERT INTO t VALUES (2)", NULL, NULL, &errmsg);
    assert(ret == 0);

    ret = db->exec(db, "DELETE FROM t WHERE x > 100", NULL, NULL, &errmsg);
    if (ret != 0) {
        printf("  FAILED: %s (code %d)\n", errmsg ? errmsg : "unknown", ret);
        db->close(db);
        exit(1);
    }
    printf("  SUCCESS: DELETE FROM t WHERE x > 100 (no matches)\n");

    db->close(db);
    printf("  PASSED\n\n");
}

int main(void) {
    printf("=== Simple DELETE WHERE Test Suite ===\n\n");

    test_delete_basic();
    test_delete_multiple();
    test_delete_string_match();
    test_delete_and_condition();
    test_delete_no_match();

    printf("=== All 5 Tests PASSED ===\n");
    return 0;
}
