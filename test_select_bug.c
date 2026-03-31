#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dbsql.h"

int callback(void *arg, int ncol, char **values, char **names) {
    int i;
    printf("Callback invoked: ncol=%d\n", ncol);
    for (i = 0; i < ncol; i++) {
        printf("  [%d] %s = '%s'\n", i, names[i], values[i] ? values[i] : "(null)");
    }
    return 0;
}

int main() {
    DBSQL *db = NULL;
    char *errmsg = NULL;
    int rc;
    const char *dbfile = "test_select_bug.db";

    /* Remove existing file */
    unlink(dbfile);

    /* Create database */
    rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
    if (rc != 0) {
        fprintf(stderr, "Failed to create env: %d\n", rc);
        return 1;
    }

    /* Open database */
    rc = db->open(db, dbfile, 0664, &errmsg);
    if (rc != 0) {
        fprintf(stderr, "Failed to open: %s\n", errmsg);
        return 1;
    }

    /* Create table */
    printf("=== CREATE TABLE ===\n");
    rc = db->exec(db, "CREATE TABLE t(id INTEGER, name TEXT);", callback, NULL, &errmsg);
    if (rc != 0) {
        fprintf(stderr, "CREATE failed: %s\n", errmsg);
        return 1;
    }

    /* Insert data */
    printf("\n=== INSERT ===\n");
    rc = db->exec(db, "INSERT INTO t VALUES(1, 'test');", callback, NULL, &errmsg);
    if (rc != 0) {
        fprintf(stderr, "INSERT failed: %s\n", errmsg);
        return 1;
    }

    /* Select data */
    printf("\n=== SELECT ===\n");
    rc = db->exec(db, "SELECT * FROM t;", callback, NULL, &errmsg);
    if (rc != 0) {
        fprintf(stderr, "SELECT failed: %s\n", errmsg);
        return 1;
    }

    printf("\n=== Done ===\n");
    db->close(db);

    /* Cleanup */
    unlink(dbfile);
    return 0;
}
