#include <stdio.h>
#include "dbsql.h"

int main() {
    DBSQL *db;
    char *err;
    
    system("rm -f seq_test.db*");
    
    printf("=== SESSION 1: Create database and 3 tables ===\n");
    dbsql_create_env(&db, NULL, NULL, 0, 0);
    db->open(db, "seq_test.db", 0664, &err);
    db->exec(db, "CREATE TABLE t1 (x INT)", NULL, NULL, &err);
    db->exec(db, "CREATE TABLE t2 (x INT)", NULL, NULL, &err);
    db->exec(db, "CREATE TABLE t3 (x INT)", NULL, NULL, &err);
    db->close(db);
    
    printf("\n=== SESSION 2: Reopen same database, create 2 more tables ===\n");
    dbsql_create_env(&db, NULL, NULL, 0, 0);
    db->open(db, "seq_test.db", 0664, &err);
    db->exec(db, "CREATE TABLE t4 (x INT)", NULL, NULL, &err);
    db->exec(db, "CREATE TABLE t5 (x INT)", NULL, NULL, &err);
    db->close(db);
    
    printf("\n=== Files created ===\n");
    system("ls seq_test.db_tbl.* 2>/dev/null");
    
    return 0;
}
