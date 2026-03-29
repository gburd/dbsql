# Debug Findings - DBSQL_CANTOPEN Investigation

## Summary

Investigation reveals that **the basic DBSQL API is working correctly**. A minimal test program (`test_debug.c`) successfully completes all operations without errors.

## Test Program Results

### test_debug.c - ALL TESTS PASS ✅

```c
Step 1: Creating environment with NULL dir (in-memory)...
Result: 0
SUCCESS: db = 0x490330

Step 2: Opening database file 'test_debug.db'...
Result: 0
SUCCESS

Step 3: Executing CREATE TABLE...
Result: 0
SUCCESS

Step 4: Closing database...
Result: 0
SUCCESS

All steps completed successfully!
```

**Key Operations Verified:**
- `dbsql_create_env(&db, NULL, NULL, 0, 0)` - Returns 0 (DBSQL_SUCCESS)
- `db->open(db, "test_debug.db", 0664, &errmsg)` - Returns 0 (DBSQL_SUCCESS)
- `db->exec(db, "CREATE TABLE test (id INTEGER)", NULL, NULL, &errmsg)` - Returns 0 (DBSQL_SUCCESS)
- `db->close(db)` - Returns 0 (DBSQL_SUCCESS)

## API Call Chain Analysis

### Successful Path (test_debug.c)

1. **dbsql_create_env**(dbpp, dir=NULL, crypt=NULL, mode=0, flags=0)
   - Creates DB_ENV with db_env_create()
   - Configures for in-memory operation (DB_PRIVATE, DB_LOG_IN_MEMORY)
   - Sets cache size: 1MB
   - Opens environment: dbenv->open(dbenv, dir=NULL, flags, mode)
   - Calls dbsql_create(dbpp, dbenv, flags)
   - **Result**: DBSQL struct allocated, dbp->dbenv set

2. **db->open**(db, filename, mode, &errmsg)
   - Maps to __api_open() in api.c:382
   - Allocates dbp->aDb[2] (main and temp databases)
   - Calls __sqldb_init() for each database
     - Calls __sm_create() which calls __sm_init()
       - db_create() - Creates Berkeley DB handle
       - dbenv->txn_begin() - Starts transaction
       - db->open() with DB_CREATE | DB_EXCL - Creates database file
       - __sm_seq_init() - Initializes sequence generator
       - __sm_meta_init() - Initializes metadata
       - txn->commit() - Commits transaction
     - Calls __sm_create_table() or __sm_open_table()
   - Calls __init_databases()
   - **Result**: Database file created, schema initialized

3. **db->exec**(db, sql, callback, ctx, &errmsg)
   - Maps to __api_exec() in api.c
   - Parses SQL and executes VDBE bytecode
   - **Result**: Table created successfully

4. **db->close**(db)
   - Maps to __api_close()
   - Closes all cursors and frees memory
   - **Result**: Clean shutdown

### Code References

- **dbsql_create_env**: api.c:1210-1300
- **dbsql_create**: api.c:1310-1376 (sets dbp->dbenv at line 1338)
- **__api_open**: api.c:382-448
- **__sqldb_init**: api.c:330-360 (calls __sm_create at line 354)
- **__sm_create**: sm.c:311-372 (calls __sm_init at line 346)
- **__sm_init**: sm.c:223-290 (uses dbp->dbenv at line 235)

## Hypothesis: Why Test Suite Fails

### Possible Causes

1. **Different Environment Setup**
   - Test suite may call dbsql_create_env with different parameters
   - Directory parameter may cause filesystem issues

2. **Multiple Database Opens**
   - Tests may open multiple databases sequentially
   - Berkeley DB environment may not handle rapid open/close cycles

3. **File Permissions**
   - Tests create files in current directory
   - May lack write permissions in test directory

4. **Transaction State**
   - Previous test failures may leave transactions open
   - Berkeley DB environment in inconsistent state

5. **Memory Cleanup**
   - Previous tests may not properly free resources
   - Memory leaks affecting subsequent operations

## Recommended Actions

### 1. Update Test Framework

Add detailed error reporting to test framework:

```c
#define ASSERT_EQ_MSG(expected, actual, msg) \
    do { \
        if ((expected) != (actual)) { \
            fprintf(stderr, "FAILED: %s (expected=%ld, actual=%ld)\n", \
                    msg, (long)(expected), (long)(actual)); \
            if (errmsg) fprintf(stderr, "Error: %s\n", errmsg); \
        } \
    } while (0)
```

### 2. Fix Test Isolation

Ensure each test:
- Creates fresh database environment
- Uses unique database filenames
- Cleans up completely on exit
- Catches and reports all errors

### 3. Add Diagnostic Tests

Create tests that verify:
- Multiple sequential database opens/closes
- Database creation in various directories
- Error handling when files already exist
- Memory cleanup between operations

### 4. Review Test Execution

- Check if tests run in parallel (may conflict)
- Verify working directory permissions
- Ensure Berkeley DB environment properly initialized
- Check for leftover .db files from previous runs

## Next Steps

1. **Review test_api.c** - Compare to working test_debug.c
2. **Add error logging** - Capture actual failure points
3. **Run tests individually** - Identify which specific tests fail
4. **Check for resource leaks** - Ensure proper cleanup
5. **Verify file permissions** - Test directory must be writable

## Compilation Command (Working)

```bash
gcc -o test_debug test_debug.c \
    -I/home/gburd/ws/dbsql/build_unix.old \
    -I/home/gburd/ws/dbsql/src/inc \
    -L/home/gburd/ws/dbsql/build_unix.old/.libs \
    -ldbsql-0.4 -lm -lpthread \
    -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib \
    -ldb-4.8 \
    -Wl,-rpath,/home/gburd/ws/dbsql/build_unix.old/.libs
```

## Status

- ✅ **Basic API Verified** - test_debug.c passes all tests
- ⏳ **Test Suite Investigation** - In progress
- ⏳ **Root Cause Analysis** - Pending test execution logs
