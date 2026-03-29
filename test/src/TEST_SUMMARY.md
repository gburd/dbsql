# Test Suite Execution Summary

**Date**: 2026-03-27
**Branch**: fix/vdbe-opcode-init
**Location**: /home/gburd/ws/dbsql/test/src

## Quick Results

| Test Suite | Status | Tests Passed | Tests Failed | Crash |
|------------|--------|--------------|--------------|-------|
| test_api   | ❌ FAIL | 10/40 (25%) | 30/40 (75%) | No    |
| test_vdbe  | ❌ CRASH | N/A | N/A | Yes (double-free) |
| test_memory| ❌ CRASH | N/A | N/A | Yes (invalid pointer) |
| test_fts   | ⚠️ NOT RUN | N/A | N/A | N/A   |

## Critical Blocking Issues

### Issue #1: P4 Memory Double-Free (BLOCKS: test_vdbe, test_memory)
**Severity**: CRITICAL
**Impact**: Prevents VDBE and memory tests from running

**Symptoms**:
```
[CREATE_P4_DYN #22] vdbe=0x4b2c40 opcode=147 addr=0 p4=0x5f31d0
...
free(): double free detected in tcache 2
```

**Analysis**:
- P4_DYNAMIC pointers allocated via __vdbe_str_nappend()
- Used in OP_ColumnName to set column names
- OP_ColumnName sets mem_t with MEM_Static flag (correct)
- However, P4 pointer is freed when opcodes are cleaned up
- Later, something tries to free the same memory again

**Debug Output**: System is emitting extensive debug traces showing:
- `[GROW_OPCODES]` - opcode array growth
- `[CREATE_P4_DYN]` - P4 allocation tracking
- `[FREE_P4]` - P4 deallocation tracking
- `[OP_ColumnName]` - column name operations
- `[MEM_RELEASE]` - memory release operations

This debug output is compiled into the library and makes test output unreadable.

**Next Actions**:
1. Find and remove/disable debug fprintf statements in library
2. Review P4 ownership in __vdbe_free_p4()
3. Check if column_names cleanup is freeing P4 pointers
4. Add reference counting or clarify ownership

### Issue #2: Database File Operations Failing (BLOCKS: 75% of test_api)
**Severity**: HIGH
**Impact**: Most API tests cannot execute SQL

**Error Codes**:
- `DBSQL_CANTOPEN (-50988)`: Cannot open database file
- `DBSQL_MISUSE (-50983)`: Library used incorrectly
- `DBSQL_ERROR (-50999)`: General SQL error

**Failing Operations**:
- Database open (after create)
- Table creation
- Data insertion
- Transaction management
- Database close (after failed operations)

**Passing Operations**:
- Database creation (dbsql_open with new file)
- Invalid SQL rejection (error handling works)

**Hypothesis**:
- Berkeley DB backend initialization issue
- File locking or permissions problem
- Incomplete integration after memory allocation changes
- Missing environment setup

**Next Actions**:
1. Add error message printing to test framework
2. Test with ":memory:" database instead of files
3. Check Berkeley DB initialization in dbsql_open
4. Verify file system permissions and paths

## Test Details

### test_api Results
```
Tests: 40
Passed: 10 (25%)
Failed: 30 (75%)
Time: 0.018 seconds

Test Breakdown:
- api_create_close: 33% pass (1/3)
- api_open_database: 33% pass (1/3)
- api_exec_create_table: 25% pass (1/4)
- api_insert_select: 14% pass (1/7)
- api_multiple_tables: 25% pass (1/4)
- api_error_handling_invalid_sql: 50% pass (2/4)
- api_transaction_commit: 14% pass (1/7)
- api_transaction_rollback: 14% pass (1/7)
```

**Pattern**:
- Database creation succeeds
- First operation after creation fails with CANTOPEN
- All subsequent operations fail
- Error handling partially works (can detect invalid SQL)

### Build Configuration

**Debug Build** (with ASAN):
- Crashes immediately in all tests
- Double-free detected by ASAN
- Extensive debug output

**Release Build** (no ASAN):
- test_api runs but fails tests
- test_vdbe crashes with double-free
- test_memory crashes with invalid pointer
- Less debug output but still excessive

## Action Plan

### Phase 1: Fix Debug Output (1 hour)
1. Find debug fprintf statements in library source
2. Remove or wrap in #ifdef DEBUG_P4_MEMORY
3. Rebuild library
4. Re-run tests with clean output

### Phase 2: Fix P4 Memory Bug (2-4 hours)
1. Trace P4 allocation/deallocation flow
2. Identify who owns P4 pointers after OP_ColumnName
3. Fix double-free in cleanup path
4. Add P4 pointer ownership documentation
5. Verify fix with ASAN enabled

### Phase 3: Fix Database Operations (2-4 hours)
1. Add dbsql_errmsg() output to test failures
2. Test with in-memory database
3. Check Berkeley DB initialization
4. Verify file system operations
5. Fix database open/close sequence

### Phase 4: Complete Test Suite (1-2 hours)
1. Build test_fts suite
2. Run all tests with fixes applied
3. Document any remaining failures
4. Generate coverage report

## Files Generated
- `/home/gburd/ws/dbsql/test/src/TEST_REPORT.md` - Detailed analysis
- `/home/gburd/ws/dbsql/test/src/TEST_SUMMARY.md` - This file

## Test Binaries
```
-rwxr-xr-x test_api (268K)
-rwxr-xr-x test_vdbe (312K)
-rwxr-xr-x test_memory (267K)
```

## Library Status
```
Library: /home/gburd/ws/dbsql/build_unix.old/.libs/libdbsql-0.4.so
Debug Output: ENABLED (excessive)
ASAN: Disabled in current build
Optimization: -O2 (release build)
```

## Conclusion

The test framework is functional and able to execute tests. Two critical bugs prevent meaningful results:

1. **P4 double-free** - Crashes VDBE and memory tests
2. **Database file operations** - Fails 75% of API tests

Both issues appear fixable with focused debugging. The P4 bug is well-documented in debug output, and the database issue has clear error codes pointing to the problem area.

Once these are fixed, the test suite should provide comprehensive validation of DBSQL functionality.
