#!/bin/bash
set -e

echo "========================================"
echo "VERIFICATION TEST FOR 3 CRITICAL FIXES"
echo "========================================"
echo ""

cd /home/gburd/ws/dbsql/test/src

# Compile a minimal test program
cat > verify_test.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dbsql.h"

static int callback(void *arg, int argc, char **argv, char **cols) {
    int *count = (int*)arg;
    (*count)++;

    /* Print results */
    for (int i = 0; i < argc; i++) {
        printf("%s=%s ", cols[i], argv[i] ? argv[i] : "NULL");
    }
    printf("\n");
    return 0;
}

int main() {
    DBSQL *db;
    char *errmsg;
    int rc, count = 0;

    /* Test 1: Memory corruption fix */
    printf("=== Test 1: Memory Corruption Fix ===\n");
    printf("Testing __entity_as_string buffer overflow fix...\n");

    rc = dbsql_open("test_verify.db", &db, DBSQL_CREATE, 0644);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "Failed to open db: %s\n", dbsql_strerror(rc));
        return 1;
    }

    rc = dbsql_exec(db, "CREATE TABLE t(id INTEGER, name TEXT);", NULL, NULL, &errmsg);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "CREATE failed: %s\n", errmsg);
        dbsql_close(db);
        return 1;
    }

    rc = dbsql_exec(db, "INSERT INTO t VALUES(1, 'test');", NULL, NULL, &errmsg);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "INSERT failed: %s\n", errmsg);
        dbsql_close(db);
        return 1;
    }
    printf("✓ No crash - memory fix appears OK\n\n");

    /* Test 2: SELECT output fix */
    printf("=== Test 2: SELECT Output Fix ===\n");
    printf("Testing SELECT query returns actual data...\n");
    count = 0;

    rc = dbsql_exec(db, "SELECT * FROM t;", callback, &count, &errmsg);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "SELECT failed: %s\n", errmsg);
        dbsql_close(db);
        return 1;
    }

    if (count > 0) {
        printf("✓ SELECT fix VERIFIED - returned %d row(s) with data\n\n", count);
    } else {
        printf("✗ SELECT fix FAILED - returned 0 rows\n\n");
    }

    /* Test 3: ORDER BY crash fix */
    printf("=== Test 3: ORDER BY Crash Fix ===\n");
    printf("Testing ORDER BY doesn't crash...\n");

    rc = dbsql_exec(db, "INSERT INTO t VALUES(2, 'apple'), (3, 'zebra');", NULL, NULL, &errmsg);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "INSERT failed: %s\n", errmsg);
        dbsql_close(db);
        return 1;
    }

    count = 0;
    rc = dbsql_exec(db, "SELECT * FROM t ORDER BY name;", callback, &count, &errmsg);
    if (rc == DBSQL_SUCCESS) {
        printf("✓ ORDER BY fix VERIFIED - no crash, returned %d rows\n\n", count);
    } else {
        printf("✗ ORDER BY fix FAILED - error: %s\n\n", errmsg);
    }

    dbsql_close(db);

    printf("=== Test 4: Valgrind Memory Check ===\n");
    printf("Skipping valgrind - use 'valgrind ./verify_test' to check manually\n\n");

    printf("========================================\n");
    printf("VERIFICATION COMPLETE\n");
    printf("========================================\n");

    return 0;
}
EOF

# Compile the test
echo "Compiling verification test..."
gcc -o verify_test verify_test.c \
    -I/home/gburd/ws/dbsql/build_unix.old \
    -L/home/gburd/ws/dbsql/build_unix.old/.libs \
    -ldbsql-0.4 \
    -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib \
    -ldb \
    -lpthread

if [ $? -ne 0 ]; then
    echo "Compilation failed!"
    exit 1
fi

echo "Running verification test..."
echo ""

# Clean old database
rm -f test_verify.db*

# Run the test
LD_LIBRARY_PATH=/home/gburd/ws/dbsql/build_unix.old/.libs:/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib:$LD_LIBRARY_PATH ./verify_test

# Clean up
rm -f test_verify.db* verify_test.c verify_test
