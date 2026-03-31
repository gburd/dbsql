/* Diagnostic test to identify Berkeley DB failure point */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dbsql.h"

int main() {
    DBSQL *db = NULL;
    int rc;
    char *errmsg = NULL;
    const char *dbfile = "diagnostic_test.db";

    /* Clean up any existing file */
    unlink(dbfile);

    printf("Step 1: Creating environment...\n");
    rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
    printf("  dbsql_create_env() returned %d\n", rc);
    if (rc != 0) {
        printf("  FAILED at create_env!\n");
        return 1;
    }
    printf("  SUCCESS\n\n");

    printf("Step 2: Opening database '%s'...\n", dbfile);
    rc = db->open(db, dbfile, 0664, &errmsg);
    printf("  db->open() returned %d\n", rc);
    if (rc != 0) {
        printf("  FAILED! Error message: %s\n", errmsg ? errmsg : "NULL");
        printf("\n");
        printf("This is the CANTOPEN error we're trying to fix.\n");
        printf("The error originates from __sm_init() in src/sm.c\n");
        printf("Specifically from __sm_seq_init() or __sm_meta_init() returning NULL\n");

        /* Try to close cleanly */
        db->close(db);
        unlink(dbfile);
        return 1;
    }
    printf("  SUCCESS\n\n");

    printf("Step 3: Closing database...\n");
    rc = db->close(db);
    printf("  db->close() returned %d\n", rc);
    if (rc != 0) {
        printf("  FAILED!\n");
        unlink(dbfile);
        return 1;
    }
    printf("  SUCCESS\n\n");

    /* Clean up */
    unlink(dbfile);

    printf("All steps completed successfully!\n");
    return 0;
}
