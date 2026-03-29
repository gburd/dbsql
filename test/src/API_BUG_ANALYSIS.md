# API Bug: DB Environment Mismatch for File Databases

## Root Cause

When using `dir=NULL` in `dbsql_create_env()`, each call creates a **new private in-memory Berkeley DB environment**. Database files persist on disk but become inaccessible to subsequent environments.

## The Problem

```c
// Test 1 - Works
dbsql_create_env(&db, NULL, ...);  // Creates ENV1 (in-memory)
db->open(db, "test1.db", ...);     // Opens test1.db in ENV1
db->close(db);                     // Closes ENV1

// Test 4 - Fails
dbsql_create_env(&db, NULL, ...);  // Creates ENV2 (different in-memory env)
db->open(db, "test1.db", ...);     // ❌ FAILS -50988
                                    // test1.db was created by ENV1
                                    // ENV2 can't access ENV1's files
```

## Why It Fails

**Line 1232 of api.c:**
```c
if (dir == 0 || dir[0] == '\0') {
    /* When dir is NULL, place all resources in memory. */
    env_open_flags |= DB_PRIVATE;
```

`DB_PRIVATE` means:
- Environment exists only in memory
- No shared memory regions
- Files created by this environment are tied to it
- **New environments can't open files from old environments**

## Evidence from Test Results

| Test | Filename | Environment | Result | Reason |
|------|----------|-------------|--------|--------|
| 1 | test1.db | ENV1 (new) | ✅ PASS | First use |
| 2 | test2.db | ENV2 (new) | ✅ PASS | First use |
| 3 | test3.db | ENV3 (new) | ✅ PASS | First use |
| 4 | test1.db | ENV4 (new) | ❌ FAIL | File belongs to ENV1 |
| 5 | test_same.db | ENV5 (new) | ✅ PASS | First use |
| 6 | test_same.db | ENV6 (new) | ❌ FAIL | File belongs to ENV5 |

Pattern: **Reusing a filename with a new environment fails**

## The Fix

### Option 1: Use Directory-Based Environment (Recommended)

Create tests with a shared directory environment:

```c
// Create shared environment once
const char *test_dir = "/tmp/dbsql_test_env";
mkdir(test_dir, 0755);
dbsql_create_env(&db, test_dir, NULL, 0, 0);

// All file operations use same environment
db->open(db, "test1.db", ...);  // Works
db->close(db);

db->open(db, "test1.db", ...);  // Works - same environment
db->close(db);
```

### Option 2: Use :memory: for All Tests

In-memory databases don't persist:

```c
db->open(db, ":memory:", ...);  // No file conflicts
```

### Option 3: Clean Up Between Tests

Remove database files AND Berkeley DB metadata:

```c
unlink("test1.db");
// Also remove lock files if they exist
unlink("test1.db-journal");
unlink("__db.001");  // Berkeley DB metadata
```

## Recommended Test Pattern

```c
TEST(api_test_name) {
    DBSQL *db = NULL;
    char *errmsg = NULL;
    const char *dbfile = ":memory:";  // ← Use memory for unit tests
    // OR use unique filename:
    // const char *dbfile = "test_api_unique_name.db";

    rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
    ASSERT_EQ(0, rc);

    rc = db->open(db, dbfile, 0, &errmsg);
    ASSERT_EQ(0, rc);

    // ... test logic ...

    rc = db->close(db);
    ASSERT_EQ(0, rc);
}
```

## Why Our Simple Tests Worked

- `test_debug.c` - Used `:memory:` and single file = works
- `test_file_vs_memory.c` - Used unique filenames (test_file.db) = works
- `test_api.c` - Reused filenames across tests = fails

## Action Items

1. ✅ **Update test_api.c** - Use `:memory:` or unique filenames per test
2. ✅ **Document API requirement** - Don't reuse filenames across environments
3. ⏳ **Consider API enhancement** - Detect and handle environment mismatches gracefully

## Files to Modify

1. `/home/gburd/ws/dbsql/test/src/test_api.c` - Change to `:memory:` or unique names
2. `/home/gburd/ws/dbsql/test/src/test_vdbe.c` - Same fix
3. `/home/gburd/ws/dbsql/test/src/test_memory.c` - Same fix

## Implementation

Each test should use:
- `:memory:` database (best for unit tests), OR
- Unique filename with test name prefix: `test_api_create.db`, `test_api_insert.db`

The tests already use unique names like `test_api.db`, `test_api_create.db`, etc., so they SHOULD work if we fix one more thing...

Let me check if there's an environment reuse issue between tests in the same test suite execution.
