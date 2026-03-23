# DBSQL C API Reference

DBSQL provides a C API compatible with SQLite's interface, making it easy to integrate into applications or migrate from SQLite.

## Overview

The DBSQL API follows SQLite's design:
1. Open a database connection
2. Prepare SQL statements
3. Bind parameters (optional)
4. Execute and retrieve results
5. Finalize statements
6. Close connection

**Key Difference**: DBSQL uses BerkeleyDB for storage, not SQLite's btree. This affects file organization and concurrency but not the API.

## Core Types

```c
typedef struct dbsql dbsql;           // Database connection
typedef struct dbsql_stmt dbsql_stmt; // Prepared statement
typedef int64_t dbsql_int64;          // 64-bit integer
typedef uint64_t dbsql_uint64;        // Unsigned 64-bit integer
```

## Return Codes

```c
#define DBSQL_OK           0   // Success
#define DBSQL_ERROR        1   // SQL error or missing database
#define DBSQL_INTERNAL     2   // Internal logic error
#define DBSQL_PERM         3   // Access permission denied
#define DBSQL_ABORT        4   // Callback routine requested an abort
#define DBSQL_BUSY         5   // The database file is locked
#define DBSQL_LOCKED       6   // A table in the database is locked
#define DBSQL_NOMEM        7   // A malloc() failed
#define DBSQL_READONLY     8   // Attempt to write a readonly database
#define DBSQL_INTERRUPT    9   // Operation terminated by interrupt
#define DBSQL_IOERR       10   // Some kind of disk I/O error occurred
#define DBSQL_CORRUPT     11   // The database disk image is malformed
#define DBSQL_NOTFOUND    12   // Unknown opcode or table/index not found
#define DBSQL_FULL        13   // Insertion failed because database is full
#define DBSQL_CANTOPEN    14   // Unable to open the database file
#define DBSQL_PROTOCOL    15   // Database lock protocol error
#define DBSQL_EMPTY       16   // Database is empty
#define DBSQL_SCHEMA      17   // The database schema changed
#define DBSQL_TOOBIG      18   // String or BLOB exceeds size limit
#define DBSQL_CONSTRAINT  19   // Abort due to constraint violation
#define DBSQL_MISMATCH    20   // Data type mismatch
#define DBSQL_MISUSE      21   // Library used incorrectly
#define DBSQL_NOLFS       22   // Uses OS features not supported
#define DBSQL_AUTH        23   // Authorization denied
#define DBSQL_FORMAT      24   // Auxiliary database format error
#define DBSQL_RANGE       25   // 2nd parameter to bind out of range
#define DBSQL_NOTADB      26   // File opened that is not a database
#define DBSQL_ROW         100  // dbsql_step() has another row ready
#define DBSQL_DONE        101  // dbsql_step() has finished executing
```

## Connection Management

### dbsql_open()

Open or create a database.

```c
int dbsql_open(
  const char *filename,  // Database filename (or ":memory:")
  dbsql **ppDb           // OUT: Database handle
);
```

**Parameters**:
- `filename`: Path to database file, or `:memory:` for in-memory database
- `ppDb`: Pointer to receive database handle

**Returns**: `DBSQL_OK` on success, error code otherwise

**Example**:
```c
dbsql *db;
int rc = dbsql_open("myapp.db", &db);
if (rc != DBSQL_OK) {
    fprintf(stderr, "Cannot open database: %s\n", dbsql_errmsg(db));
    return 1;
}
```

**Storage Note**: Creates BerkeleyDB environment and database files in the same directory.

### dbsql_close()

Close a database connection.

```c
int dbsql_close(dbsql *db);
```

**Parameters**:
- `db`: Database handle to close

**Returns**: `DBSQL_OK` on success

**Example**:
```c
dbsql_close(db);
```

**Important**: Finalize all prepared statements before closing the database.

### dbsql_errmsg()

Get error message for last operation.

```c
const char *dbsql_errmsg(dbsql *db);
```

**Returns**: UTF-8 encoded error message string

**Example**:
```c
if (rc != DBSQL_OK) {
    fprintf(stderr, "Error: %s\n", dbsql_errmsg(db));
}
```

### dbsql_errcode()

Get error code for last operation.

```c
int dbsql_errcode(dbsql *db);
```

**Returns**: Error code (e.g., `DBSQL_ERROR`, `DBSQL_BUSY`)

## Statement Lifecycle

### dbsql_prepare()

Compile SQL into bytecode.

```c
int dbsql_prepare(
  dbsql *db,               // Database handle
  const char *zSql,        // SQL statement (UTF-8)
  int nByte,               // Length of zSql (-1 = null-terminated)
  dbsql_stmt **ppStmt,     // OUT: Statement handle
  const char **pzTail      // OUT: Pointer to unused portion of zSql
);
```

**Parameters**:
- `db`: Database handle
- `zSql`: SQL statement text
- `nByte`: Length of SQL, or -1 for null-terminated string
- `ppStmt`: Pointer to receive statement handle
- `pzTail`: Receives pointer to remainder of SQL (NULL if all consumed)

**Returns**: `DBSQL_OK` on success

**Example**:
```c
dbsql_stmt *stmt;
const char *sql = "SELECT name, age FROM users WHERE age > ?";
int rc = dbsql_prepare(db, sql, -1, &stmt, NULL);
if (rc != DBSQL_OK) {
    fprintf(stderr, "Prepare failed: %s\n", dbsql_errmsg(db));
    return rc;
}
```

**Internal**: Parser generates VDBE bytecode, stored in statement object.

### dbsql_step()

Execute one step of a prepared statement.

```c
int dbsql_step(dbsql_stmt *pStmt);
```

**Returns**:
- `DBSQL_ROW`: Row of data is ready (call dbsql_column_*())
- `DBSQL_DONE`: Statement completed successfully
- Error code: Statement failed

**Example**:
```c
while ((rc = dbsql_step(stmt)) == DBSQL_ROW) {
    const char *name = (const char *)dbsql_column_text(stmt, 0);
    int age = dbsql_column_int(stmt, 1);
    printf("Name: %s, Age: %d\n", name, age);
}
if (rc != DBSQL_DONE) {
    fprintf(stderr, "Step failed: %s\n", dbsql_errmsg(db));
}
```

**Behavior**:
- First call starts execution
- Subsequent calls return next row (SELECT) or continue execution
- Returns `DBSQL_DONE` when complete

### dbsql_reset()

Reset a statement to its initial state.

```c
int dbsql_reset(dbsql_stmt *pStmt);
```

**Returns**: `DBSQL_OK` on success

**Use Case**: Execute the same statement multiple times with different parameters.

**Example**:
```c
// Execute once
dbsql_bind_int(stmt, 1, 25);
dbsql_step(stmt);

// Reset and execute again
dbsql_reset(stmt);
dbsql_bind_int(stmt, 1, 30);
dbsql_step(stmt);
```

### dbsql_finalize()

Destroy a prepared statement.

```c
int dbsql_finalize(dbsql_stmt *pStmt);
```

**Returns**: `DBSQL_OK` or error code from last step

**Example**:
```c
dbsql_finalize(stmt);
```

**Important**: Always finalize statements to avoid memory leaks.

## Parameter Binding

Bind values to `?` placeholders in SQL.

### dbsql_bind_int()

```c
int dbsql_bind_int(dbsql_stmt *pStmt, int index, int value);
```

### dbsql_bind_int64()

```c
int dbsql_bind_int64(dbsql_stmt *pStmt, int index, dbsql_int64 value);
```

### dbsql_bind_double()

```c
int dbsql_bind_double(dbsql_stmt *pStmt, int index, double value);
```

### dbsql_bind_text()

```c
int dbsql_bind_text(
  dbsql_stmt *pStmt,
  int index,
  const char *value,
  int n,
  void (*destructor)(void*)
);
```

**Parameters**:
- `index`: Parameter index (1-based)
- `value`: String value
- `n`: Length of string, or -1 for null-terminated
- `destructor`: `DBSQL_STATIC`, `DBSQL_TRANSIENT`, or custom destructor

**Destructors**:
- `DBSQL_STATIC`: Value is constant, no copy needed
- `DBSQL_TRANSIENT`: Make a copy of value (default)
- Custom function: Called to free value when done

### dbsql_bind_blob()

```c
int dbsql_bind_blob(
  dbsql_stmt *pStmt,
  int index,
  const void *value,
  int n,
  void (*destructor)(void*)
);
```

### dbsql_bind_null()

```c
int dbsql_bind_null(dbsql_stmt *pStmt, int index);
```

### Example: Parameterized Query

```c
const char *sql = "INSERT INTO users (name, email, age) VALUES (?, ?, ?)";
dbsql_stmt *stmt;

dbsql_prepare(db, sql, -1, &stmt, NULL);
dbsql_bind_text(stmt, 1, "Alice", -1, DBSQL_STATIC);
dbsql_bind_text(stmt, 2, "alice@example.com", -1, DBSQL_STATIC);
dbsql_bind_int(stmt, 3, 30);

int rc = dbsql_step(stmt);
if (rc != DBSQL_DONE) {
    fprintf(stderr, "Insert failed: %s\n", dbsql_errmsg(db));
}

dbsql_finalize(stmt);
```

## Result Access

### dbsql_column_count()

Get number of columns in result set.

```c
int dbsql_column_count(dbsql_stmt *pStmt);
```

**Returns**: Number of columns

### dbsql_column_name()

Get column name.

```c
const char *dbsql_column_name(dbsql_stmt *pStmt, int N);
```

**Parameters**:
- `N`: Column index (0-based)

**Returns**: UTF-8 column name

### dbsql_column_type()

Get storage class of column value.

```c
int dbsql_column_type(dbsql_stmt *pStmt, int iCol);
```

**Returns**: One of:
- `DBSQL_INTEGER`
- `DBSQL_FLOAT`
- `DBSQL_TEXT`
- `DBSQL_BLOB`
- `DBSQL_NULL`

### dbsql_column_int()

```c
int dbsql_column_int(dbsql_stmt *pStmt, int iCol);
```

**Returns**: Column value as 32-bit integer

### dbsql_column_int64()

```c
dbsql_int64 dbsql_column_int64(dbsql_stmt *pStmt, int iCol);
```

**Returns**: Column value as 64-bit integer

### dbsql_column_double()

```c
double dbsql_column_double(dbsql_stmt *pStmt, int iCol);
```

**Returns**: Column value as floating point

### dbsql_column_text()

```c
const unsigned char *dbsql_column_text(dbsql_stmt *pStmt, int iCol);
```

**Returns**: UTF-8 encoded text (null-terminated)

**Lifetime**: Valid until next dbsql_step(), dbsql_reset(), or dbsql_finalize()

### dbsql_column_blob()

```c
const void *dbsql_column_blob(dbsql_stmt *pStmt, int iCol);
```

**Returns**: Pointer to BLOB data

### dbsql_column_bytes()

```c
int dbsql_column_bytes(dbsql_stmt *pStmt, int iCol);
```

**Returns**: Length of TEXT or BLOB in bytes

### Example: Reading Results

```c
dbsql_stmt *stmt;
dbsql_prepare(db, "SELECT id, name, email, age FROM users", -1, &stmt, NULL);

printf("Columns: %d\n", dbsql_column_count(stmt));

while (dbsql_step(stmt) == DBSQL_ROW) {
    int id = dbsql_column_int(stmt, 0);
    const char *name = (const char *)dbsql_column_text(stmt, 1);
    const char *email = (const char *)dbsql_column_text(stmt, 2);
    int age = dbsql_column_int(stmt, 3);

    printf("%d: %s <%s> (age %d)\n", id, name, email, age);
}

dbsql_finalize(stmt);
```

## Convenience Functions

### dbsql_exec()

Execute SQL without result rows.

```c
int dbsql_exec(
  dbsql *db,
  const char *sql,
  int (*callback)(void*,int,char**,char**),
  void *arg,
  char **errmsg
);
```

**Parameters**:
- `db`: Database handle
- `sql`: SQL statements (multiple allowed, separated by `;`)
- `callback`: Called for each result row (or NULL)
- `arg`: First argument to callback
- `errmsg`: Error message output (call dbsql_free() when done)

**Example**:
```c
char *errmsg;
int rc = dbsql_exec(db,
    "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);"
    "INSERT INTO users VALUES (1, 'Alice');",
    NULL, NULL, &errmsg);
if (rc != DBSQL_OK) {
    fprintf(stderr, "SQL error: %s\n", errmsg);
    dbsql_free(errmsg);
}
```

### dbsql_get_table()

Execute query and get all results at once.

```c
int dbsql_get_table(
  dbsql *db,
  const char *zSql,
  char ***pazResult,
  int *pnRow,
  int *pnColumn,
  char **pzErrmsg
);
```

**Example**:
```c
char **result;
int nrow, ncol;
char *errmsg;

int rc = dbsql_get_table(db,
    "SELECT name, age FROM users",
    &result, &nrow, &ncol, &errmsg);

if (rc == DBSQL_OK) {
    // result[0..ncol-1]: column names
    // result[ncol..]: row data
    for (int i = 0; i < nrow; i++) {
        char *name = result[ncol + i*ncol + 0];
        char *age = result[ncol + i*ncol + 1];
        printf("%s: %s\n", name, age);
    }
    dbsql_free_table(result);
}
```

### dbsql_free_table()

Free memory from dbsql_get_table().

```c
void dbsql_free_table(char **result);
```

## Transaction Control

### Manual Transactions

```c
// Begin transaction
dbsql_exec(db, "BEGIN", NULL, NULL, NULL);

// Execute statements
dbsql_exec(db, "INSERT INTO ...", NULL, NULL, NULL);
dbsql_exec(db, "UPDATE ...", NULL, NULL, NULL);

// Commit
dbsql_exec(db, "COMMIT", NULL, NULL, NULL);

// OR rollback on error
dbsql_exec(db, "ROLLBACK", NULL, NULL, NULL);
```

### Automatic Transactions

Each statement runs in its own transaction unless you explicitly use BEGIN.

**Storage Note**: Transactions use BerkeleyDB's transaction system, not SQLite's locking.

## Complete Example

```c
#include <stdio.h>
#include <dbsql.h>

int main() {
    dbsql *db;
    dbsql_stmt *stmt;
    int rc;

    // Open database
    rc = dbsql_open("example.db", &db);
    if (rc != DBSQL_OK) {
        fprintf(stderr, "Cannot open database: %s\n", dbsql_errmsg(db));
        return 1;
    }

    // Create table
    rc = dbsql_exec(db,
        "CREATE TABLE IF NOT EXISTS users ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  email TEXT UNIQUE,"
        "  age INTEGER"
        ")",
        NULL, NULL, NULL);
    if (rc != DBSQL_OK) {
        fprintf(stderr, "Create table failed: %s\n", dbsql_errmsg(db));
        goto cleanup;
    }

    // Insert data with RETURNING clause
    rc = dbsql_prepare(db,
        "INSERT INTO users (name, email, age) "
        "VALUES (?, ?, ?) RETURNING id",
        -1, &stmt, NULL);
    if (rc != DBSQL_OK) {
        fprintf(stderr, "Prepare failed: %s\n", dbsql_errmsg(db));
        goto cleanup;
    }

    dbsql_bind_text(stmt, 1, "Alice", -1, DBSQL_STATIC);
    dbsql_bind_text(stmt, 2, "alice@example.com", -1, DBSQL_STATIC);
    dbsql_bind_int(stmt, 3, 30);

    rc = dbsql_step(stmt);
    if (rc == DBSQL_ROW) {
        int id = dbsql_column_int(stmt, 0);
        printf("Inserted user with id: %d\n", id);
    }
    dbsql_finalize(stmt);

    // Query data
    rc = dbsql_prepare(db,
        "SELECT name, email, age FROM users WHERE age > ?",
        -1, &stmt, NULL);
    if (rc != DBSQL_OK) {
        fprintf(stderr, "Prepare failed: %s\n", dbsql_errmsg(db));
        goto cleanup;
    }

    dbsql_bind_int(stmt, 1, 25);

    printf("\nUsers over 25:\n");
    while ((rc = dbsql_step(stmt)) == DBSQL_ROW) {
        const char *name = (const char *)dbsql_column_text(stmt, 0);
        const char *email = (const char *)dbsql_column_text(stmt, 1);
        int age = dbsql_column_int(stmt, 2);
        printf("  %s <%s> (age %d)\n", name, email, age);
    }

    if (rc != DBSQL_DONE) {
        fprintf(stderr, "Query failed: %s\n", dbsql_errmsg(db));
    }

    dbsql_finalize(stmt);

cleanup:
    dbsql_close(db);
    return (rc == DBSQL_OK || rc == DBSQL_DONE) ? 0 : 1;
}
```

**Compile**:
```bash
gcc -o example example.c -ldbsql-0.4 -ldb-5.3
```

## Thread Safety

DBSQL can be compiled with thread support:

**Thread Modes**:
1. **Single-threaded**: No mutex protection (default)
2. **Multi-threaded**: One connection per thread
3. **Serialized**: Connection can be shared across threads

**Best Practice**: Use one connection per thread for best performance.

## Memory Management

### dbsql_malloc()

```c
void *dbsql_malloc(int size);
```

Allocate memory using DBSQL's allocator.

### dbsql_free()

```c
void dbsql_free(void *ptr);
```

Free memory allocated by DBSQL.

### Example

```c
char *errmsg;
dbsql_exec(db, "SELECT ...", NULL, NULL, &errmsg);
if (errmsg) {
    fprintf(stderr, "Error: %s\n", errmsg);
    dbsql_free(errmsg);
}
```

## Utility Functions

### dbsql_changes()

Get number of rows modified by last statement.

```c
int dbsql_changes(dbsql *db);
```

**Example**:
```c
dbsql_exec(db, "UPDATE users SET age = age + 1 WHERE active = 1", NULL, NULL, NULL);
printf("Updated %d rows\n", dbsql_changes(db));
```

### dbsql_last_insert_rowid()

Get rowid of last inserted row.

```c
dbsql_int64 dbsql_last_insert_rowid(dbsql *db);
```

**Example**:
```c
dbsql_exec(db, "INSERT INTO users (name) VALUES ('Alice')", NULL, NULL, NULL);
dbsql_int64 rowid = dbsql_last_insert_rowid(db);
printf("Inserted rowid: %lld\n", rowid);
```

## Error Handling Best Practices

Always check return codes:

```c
int rc;

rc = dbsql_open("mydb.db", &db);
if (rc != DBSQL_OK) {
    fprintf(stderr, "Open failed: %s\n", dbsql_errmsg(db));
    return 1;
}

rc = dbsql_prepare(db, sql, -1, &stmt, NULL);
if (rc != DBSQL_OK) {
    fprintf(stderr, "Prepare failed: %s\n", dbsql_errmsg(db));
    dbsql_close(db);
    return 1;
}

while ((rc = dbsql_step(stmt)) == DBSQL_ROW) {
    // Process row
}

if (rc != DBSQL_DONE) {
    fprintf(stderr, "Step failed: %s\n", dbsql_errmsg(db));
}

dbsql_finalize(stmt);
dbsql_close(db);
```

## Differences from SQLite API

DBSQL's C API is intentionally compatible with SQLite, but note:

1. **Storage**: BerkeleyDB backend (multiple files vs single file)
2. **Threading**: Follows BDB threading model
3. **Extensions**: Custom functions/collations supported
4. **VFS**: Uses BDB's file I/O (no SQLite VFS)

**Migration**: Most SQLite code works unchanged with DBSQL.

## Related Documentation

- [SQL Reference](../reference/SQL_REFERENCE.md) - SQL syntax
- [Storage Guide](../guides/STORAGE.md) - BerkeleyDB details
- [Architecture](../architecture/ARCHITECTURE.md) - Internal design
- [TCL API](TCL_API.md) - TCL extension API
