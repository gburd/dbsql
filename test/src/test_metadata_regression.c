/*
 * Test for database metadata system regression
 *
 * Problem: dbsql_master table is not accessible after CREATE TABLE operations.
 * - Database files are created (test.db, test.db_tbl.*)
 * - But queries report "no such table" errors
 * - Even system table dbsql_master cannot be accessed
 */

#include "dbsql.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int callback(void *arg, int argc, char **argv, char **col_names) {
    int i;
    printf("Callback called with %d columns:\n", argc);
    for (i = 0; i < argc; i++) {
        printf("  %s = %s\n", col_names[i], argv[i] ? argv[i] : "NULL");
    }
    return 0;
}

int main(void) {
    DBSQL *dbp = NULL;
    char *errmsg = NULL;
    int rc;

    printf("=== Metadata System Regression Test ===\n\n");

    /* Remove existing test database */
    system("rm -f /tmp/metadata_test.db /tmp/metadata_test.db_tbl.*");

    /* Open database */
    printf("1. Opening database...\n");
    rc = dbsql_open("/tmp/metadata_test.db", &dbp);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "ERROR: Failed to open database: %d\n", rc);
        return 1;
    }
    printf("   SUCCESS: Database opened\n\n");

    /* Create a table */
    printf("2. Creating table 't1'...\n");
    rc = dbsql_exec(dbp, "CREATE TABLE t1(id INTEGER, name TEXT);",
                    callback, NULL, &errmsg);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "ERROR: CREATE TABLE failed: %d - %s\n", rc,
                errmsg ? errmsg : "unknown");
        if (errmsg) free(errmsg);
        dbsql_close(dbp);
        return 1;
    }
    printf("   SUCCESS: Table created\n\n");

    /* Try to query the master table */
    printf("3. Querying dbsql_master...\n");
    errmsg = NULL;
    rc = dbsql_exec(dbp, "SELECT type, name, tbl_name, sql FROM dbsql_master;",
                    callback, NULL, &errmsg);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "ERROR: SELECT from dbsql_master failed: %d - %s\n", rc,
                errmsg ? errmsg : "unknown");
        if (errmsg) free(errmsg);
        dbsql_close(dbp);
        return 1;
    }
    printf("   SUCCESS: dbsql_master accessible\n\n");

    /* Try to query the created table */
    printf("4. Querying created table 't1'...\n");
    errmsg = NULL;
    rc = dbsql_exec(dbp, "SELECT * FROM t1;", callback, NULL, &errmsg);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "ERROR: SELECT from t1 failed: %d - %s\n", rc,
                errmsg ? errmsg : "unknown");
        if (errmsg) free(errmsg);
        dbsql_close(dbp);
        return 1;
    }
    printf("   SUCCESS: Table t1 accessible\n\n");

    /* Close database */
    printf("5. Closing database...\n");
    rc = dbsql_close(dbp);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "ERROR: Failed to close database: %d\n", rc);
        return 1;
    }
    printf("   SUCCESS: Database closed\n\n");

    /* Reopen and verify persistence */
    printf("6. Reopening database to verify persistence...\n");
    rc = dbsql_open("/tmp/metadata_test.db", &dbp);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "ERROR: Failed to reopen database: %d\n", rc);
        return 1;
    }
    printf("   SUCCESS: Database reopened\n\n");

    printf("7. Querying dbsql_master after reopen...\n");
    errmsg = NULL;
    rc = dbsql_exec(dbp, "SELECT type, name, tbl_name, sql FROM dbsql_master;",
                    callback, NULL, &errmsg);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "ERROR: SELECT from dbsql_master after reopen failed: %d - %s\n",
                rc, errmsg ? errmsg : "unknown");
        if (errmsg) free(errmsg);
        dbsql_close(dbp);
        return 1;
    }
    printf("   SUCCESS: dbsql_master still accessible\n\n");

    dbsql_close(dbp);

    printf("=== All tests passed! ===\n");
    return 0;
}
