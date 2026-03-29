# DBSQL Test Baseline Results

**Date:** March 27, 2026
**Branch:** fix/vdbe-opcode-init
**Working Directory:** /home/gburd/ws/dbsql/test/src

## Executive Summary

The test suite in `/home/gburd/ws/dbsql/test/src/` **cannot be built** because it uses a non-existent API. The tests were written for wrapper functions like `dbsql_open()`, `dbsql_close()`, `dbsql_exec()` that do not exist in the current codebase.

The actual DBSQL API uses function pointers within the `DBSQL` structure (e.g., `db->open()`, `db->close()`, `db->exec()`).

## Build Status

### Test Suite in `/home/gburd/ws/dbsql/test/src/`

**Status:** ❌ **FAILED TO BUILD**

**Error:** Undefined references to:
- `dbsql_open`
- `dbsql_close`
- `dbsql_exec`
- `dbsql_prepare`
- `dbsql_finalize`
- `dbsql_step`

**Root Cause:** The test files include `dbsql.h` which defines the DBSQL API, but this API uses function pointers, not standalone functions:

```c
struct __dbsql {
    int (*open) __P((DBSQL *, const char *, int, char **));
    int (*close) __P((DBSQL *));
    int (*exec) __P((DBSQL *, const char *, dbsql_callback, void *, char **));
    // ... other function pointers
};
```

The correct usage is:
```c
DBSQL *db;
dbsql_create_env(&db, ...);  // Create database handle
db->open(db, "test.db", 0, NULL);  // Use function pointers
db->exec(db, "SELECT 1", NULL, NULL, NULL);
db->close(db);
```

### TCL Test Suite in `/home/gburd/ws/dbsql/test/scr050/`

**Status:** ⚠️ **PARTIALLY WORKING**

**Issues Found:**
1. Library path mismatch: `dbsql_tclsh` wrapper references `/home/gburd/ws/dbsql/build_unix/.libs/` but actual library is in `/home/gburd/ws/dbsql/build_unix.old/.libs/`
2. Segmentation fault when running simple TCL tests
3. File-based database operations crash with SIGABRT

## Manual Testing via `dbsql` Binary

Tested the compiled `dbsql` command-line tool at `/home/gburd/ws/dbsql/build_unix.old/dbsql`:

### Test Results

| Test | Status | Notes |
|------|--------|-------|
| SELECT 1 (in-memory) | ✅ PASS | Basic query execution works |
| CREATE TABLE (in-memory) | ❌ FAIL | Master table query fails |
| INSERT + SELECT (in-memory) | ✅ PASS | Data persistence works in memory |
| File-based database | ❌ FAIL | Crashes with SIGABRT (core dumped) |

### Debug Output

The binary has extensive debug output enabled, showing:
- VDBE opcode allocation (`[GROW_OPCODES]`)
- P4 parameter tracking (`[CREATE_P4_DYN]`, `[FREE_P4]`)
- Column name operations (`[OP_ColumnName]`)
- Memory operations (`[CLEANUP]`)

This debug output should be disabled for production testing.

## Critical Issues Identified

### 1. File-Based Database Crash (CRITICAL)

**Severity:** CRITICAL
**Impact:** Cannot create or use persistent databases
**Error:** SIGABRT (process aborted)

```bash
$ echo "CREATE TABLE test (id INT);" | /home/gburd/ws/dbsql/build_unix.old/dbsql /tmp/test.db 2>/dev/null
Aborted (core dumped)
```

**Likely Causes:**
- Memory corruption during database file initialization
- Berkeley DB environment setup failure
- __os_* memory allocation issue in file operations

### 2. TCL Test Harness Segfault (HIGH)

**Severity:** HIGH
**Impact:** Cannot run the comprehensive TCL test suite (64 test files)
**Error:** Segmentation fault (exit code 139)

```bash
$ LD_LIBRARY_PATH=/home/gburd/ws/dbsql/build_unix.old/.libs \
  /home/gburd/ws/dbsql/test/scr050/.libs/dbsql_tclsh \
  /home/gburd/ws/dbsql/test/scr050/test_simple.tcl
Segmentation fault (core dumped)
```

### 3. Test Suite API Mismatch (MEDIUM)

**Severity:** MEDIUM
**Impact:** Cannot use test suite in `/home/gburd/ws/dbsql/test/src/`
**Issue:** Tests written for non-existent wrapper API

The test suite needs to be either:
- Rewritten to use the function pointer API
- Wrapper functions need to be implemented

### 4. Master Table Query Failure (MEDIUM)

**Severity:** MEDIUM
**Impact:** Cannot enumerate tables via `SELECT name FROM master`
**Error:** Silent failure (no output)

## Test Files Inventory

### C Test Suite (`/home/gburd/ws/dbsql/test/src/`)

- `test_api.c` - 13 tests for core API functions ❌
- `test_vdbe.c` - VDBE-specific tests ❌
- `test_memory.c` - Memory allocation tests ❌
- `test_framework.c` - Test harness infrastructure ❌

**Total:** 3 test files, all unbuildable

### TCL Test Suite (`/home/gburd/ws/dbsql/test/scr050/tests/`)

64 TCL test files covering:
- Core SQL: select, insert, update, delete, join, subselect
- Schema: table, index, view, trigger
- Features: transactions, attach, bind, pragma
- Edge cases: null, unique, conflict, limit
- Advanced: thread1, malloc, memleak

**Total:** 64 test files, status unknown (harness crashes)

## Recommended Actions

### Immediate (Fix Critical Issues)

1. **Debug file-based database crash**
   - Run with gdb to get stack trace
   - Check Berkeley DB environment initialization
   - Verify __os_malloc calls during file open

2. **Debug TCL harness segfault**
   - Run dbsql_tclsh under gdb
   - Check TCL binding initialization
   - Verify library version compatibility

### Short-Term (Enable Testing)

3. **Fix or rebuild test harness**
   - Option A: Rebuild dbsql_tclsh against current library
   - Option B: Create new test harness with correct paths

4. **Disable debug output**
   - Remove or conditionally compile debug fprintf statements
   - Allows clean test output for validation

### Medium-Term (Comprehensive Testing)

5. **Rewrite C test suite**
   - Convert tests to use function pointer API
   - Or implement wrapper functions for backwards compatibility

6. **Run TCL test suite**
   - Once harness is fixed, run all 64 tests
   - Document pass/fail for each test
   - Categorize failures by severity

## Test Environment

- **Platform:** Linux 6.12.76
- **Build Directory:** `/home/gburd/ws/dbsql/build_unix.old`
- **Library:** `/home/gburd/ws/dbsql/build_unix.old/.libs/libdbsql-0.4.so`
- **Binary:** `/home/gburd/ws/dbsql/build_unix.old/dbsql`
- **Compiler:** gcc with sanitizers enabled (DEBUG build)

## Files Modified/Created

- `/tmp/claude/test_baseline.sh` - Baseline test script
- `/tmp/claude/test_baseline_results.txt` - Raw test output
- This document

## Next Steps

1. Debug and fix file-based database crash (CRITICAL priority)
2. Debug and fix TCL test harness segfault
3. Once crashes are resolved, run comprehensive test suite
4. Document all test failures with categorization
5. Create prioritized fix list based on severity

---

**Note:** The test suite infrastructure exists but is not currently functional. Both the C test suite (API mismatch) and TCL test suite (crashes) need fixes before baseline testing can proceed.
