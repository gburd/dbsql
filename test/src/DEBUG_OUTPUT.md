# Debug Test Results: File vs Memory Database Initialization

## Test Execution

Compiled and ran: `/home/gburd/ws/dbsql/test/src/test_file_vs_memory`

### Simple Test Results

```
Test 1: Memory database
  create_env: 0
  open(':memory:'): 0 
  exec(CREATE): 0 
  close: 0
  Memory test: PASS

Test 2: File database
  create_env: 0
  open('test_file.db'): 0 
  exec(CREATE): 0 
  close: 0
  File test: PASS
```

Both tests pass when run standalone without sanitizers.

### Test Suite with AddressSanitizer

When running the full test suite with Address Sanitizer enabled:

```
==338717==ERROR: AddressSanitizer: heap-use-after-free on address 0x615000001498
READ of size 8 at 0x615000001498 thread T0
    #0 0x402616 in test_api_open_database /home/gburd/ws/dbsql/test/src/test_api.c:49
    #1 0x409af4 in test_run_all /home/gburd/ws/dbsql/test/src/test_framework.c:46
    #2 0x409184 in main /home/gburd/ws/dbsql/test/src/test_api.c:250

freed by thread T0 here:
    #0 0x7ffff78bb0d8 in __interceptor_free.part.0
    #1 0x7ffff7f887fd in __api_open ../dist/../src/api.c:411
```

## Root Cause Analysis

### The Bug

In `/home/gburd/ws/dbsql/src/api.c`, the `__api_open()` function has THREE error paths that free the `dbp` structure:

1. **Line 411**: When `__sqldb_init()` fails
   ```c
   __dbsql_free(dbp, dbp);  // Frees the entire structure
   ```

2. **Lines 428 & 431**: When `__init_databases()` fails
   ```c
   dbp->close(dbp);  // Calls close which frees dbp
   ```

3. **Line 442**: Memory allocation failure path
   ```c
   __dbsql_free(NULL, dbp);  // Frees the structure again
   ```

### The Problem

When `db->open()` fails:
- The function frees `dbp` internally
- Returns error code to caller
- Caller still has their `db` pointer pointing to freed memory
- Caller tries to call `db->close(db)` 
- This accesses freed memory → heap-use-after-free

Additionally, there's a **double-free bug**:
- Line 428: Calls `dbp->close(dbp)` which frees `dbp`
- Then jumps to `no_mem_on_open2`
- Line 442: Tries to free `dbp` again

### The Design Flaw

Unclear ownership semantics:
- `dbsql_create_env()` creates the `DBSQL` structure
- `open()` sometimes frees it (on error), sometimes doesn't (on success)
- `close()` expects to free it
- Caller doesn't know whether to call `close()` after `open()` fails

## The Solution

Establish clear, consistent ownership:

```
dbsql_create_env(&db, ...);  // Creates db - caller owns it
db->open(db, ...);           // Initializes db - NEVER frees it
db->close(db);               // Always frees db
```

**Rules:**
1. `open()` never frees `dbp` - not on success, not on error
2. Caller ALWAYS calls `close()` to clean up - whether `open()` succeeded or failed
3. `close()` handles partially-initialized structures safely

## Required Changes

### File: `/home/gburd/ws/dbsql/src/api.c`

#### Change 1: Line 411 - Don't free dbp when __sqldb_init fails

**Before:**
```c
if (rc != DBSQL_SUCCESS) {
    __str_append(err_msgs, "unable to open database: ", filename, (char*)0);
    __dbsql_free(dbp, dbp);  // ← WRONG: frees dbp
    __str_urealloc(err_msgs);
    return DBSQL_CANTOPEN;
}
```

**After:**
```c
if (rc != DBSQL_SUCCESS) {
    __str_append(err_msgs, "unable to open database: ", filename, (char*)0);
    /* Don't free dbp - caller will call close() to cleanup */
    __str_urealloc(err_msgs);
    return DBSQL_CANTOPEN;
}
```

#### Change 2: Lines 427-432 - Don't call close() in open's error path

**Before:**
```c
if (rc == ENOMEM) {
    dbp->close(dbp);  // ← WRONG: frees dbp
    goto no_mem_on_open2;
} else if (rc != DBSQL_SUCCESS && rc != DBSQL_BUSY) {
    dbp->close(dbp);  // ← WRONG: frees dbp
    __str_urealloc(err_msgs);
    return DBSQL_CANTOPEN;
}
```

**After:**
```c
if (rc == ENOMEM) {
    /* Let caller cleanup via close() */
    __str_append(err_msgs, "out of memory", (char*)0);
    __str_urealloc(err_msgs);
    return DBSQL_NOMEM;
} else if (rc != DBSQL_SUCCESS && rc != DBSQL_BUSY) {
    /* Let caller cleanup via close() */
    __str_urealloc(err_msgs);
    return DBSQL_CANTOPEN;
}
```

#### Change 3: Line 442 - Don't free dbp at error label

**Before:**
```c
  no_mem_on_open2:
    __dbsql_free(NULL, dbp);  // ← WRONG: may double-free
  no_mem_on_open1:
```

**After:**
```c
  no_mem_on_open2:
    /* Don't free dbp - caller will call close() to cleanup */
  no_mem_on_open1:
```

#### Change 4: Around line 525 - Make close() handle NULL fns

**Before:**
```c
__dbsql_free(dbp, dbp->aDb);
__hash_clear((hash_t*)dbp->fns);  // ← WRONG: crashes if fns is NULL
__dbsql_free(dbp, dbp->fns);
```

**After:**
```c
__dbsql_free(dbp, dbp->aDb);
if (dbp->fns) {
    __hash_clear((hash_t*)dbp->fns);
    __dbsql_free(dbp, dbp->fns);
}
```

## Benefits of This Fix

1. **No more use-after-free**: `open()` never frees `dbp`
2. **No more double-free**: Only `close()` frees `dbp`, called once
3. **Clear ownership**: Caller creates, caller destroys (via close)
4. **Consistent behavior**: Always call `close()` to cleanup
5. **Handles partial init**: `close()` safely handles partially-initialized state

## Testing Strategy

After applying fixes:
1. Run simple file vs memory test - both should pass
2. Run full test suite with ASAN - no heap errors
3. Verify all API tests pass
4. Check that file databases work correctly in all scenarios

## Files Modified

- `/home/gburd/ws/dbsql/src/api.c` - 4 changes in `__api_open` and `__api_close`

## Conclusion

The root cause is NOT related to VDBE opcode initialization. The issue is **ownership and cleanup semantics in the database API**. 

The file-based database path triggers initialization code that can fail, exposing the ownership bugs that memory databases don't hit (because they skip certain initialization steps).

The fix establishes clear rules: `open()` initializes, `close()` cleans up. The caller manages the lifecycle consistently.
