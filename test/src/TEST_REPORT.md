# DBSQL Test Suite Execution Report
Date: 2026-03-27
Working Directory: /home/gburd/ws/dbsql/test/src
Branch: fix/vdbe-opcode-init

## Build Status

### Debug Build (with ASAN)
- **Status**: Successful compilation
- **Binaries Created**:
  - test_api (268K)
  - test_vdbe (312K)
  - test_memory (267K)
- **Compiler Flags**: -Og -g3 -DDEBUG + full ASAN suite
- **Result**: All binaries crash immediately with double-free error

### Release Build (no ASAN)
- **Status**: Successful compilation
- **Binaries Created**:
  - test_api (optimized)
  - test_vdbe (optimized)
  - test_memory (optimized)
- **Compiler Flags**: -O2 -DNDEBUG
- **Result**: Mixed - test_api runs but fails tests, others crash

## Test Execution Results

### test_api (Release Build)
**Status**: Runs to completion but with failures
**Total Tests**: 40
**Passed**: 10 (25%)
**Failed**: 30 (75%)
**Skipped**: 0
**Execution Time**: 0.018 seconds

#### Error Analysis
All failures related to three error codes:
- **DBSQL_ERROR (-50999)**: SQL error or missing database
- **DBSQL_CANTOPEN (-50988)**: Unable to open the database file
- **DBSQL_MISUSE (-50983)**: Library used incorrectly

#### Test Breakdown
1. **api_create_close**: 1/3 passed (database close fails)
2. **api_open_database**: 1/3 passed (open and exec fail)
3. **api_exec_create_table**: 1/4 passed (all SQL operations fail)
4. **api_insert_select**: 1/7 passed (all data operations fail)
5. **api_multiple_tables**: 1/4 passed (table creation fails)
6. **api_error_handling_invalid_sql**: 2/4 passed (error handling partially works)
7. **api_transaction_commit**: 1/7 passed (transaction operations fail)
8. **api_transaction_rollback**: 1/7 passed (rollback operations fail)

### test_vdbe (Release Build)
**Status**: CRASH - Double free in P4 memory management
**Failure Point**: During test execution, after multiple VDBE operations
**Error**: `free(): double free detected in tcache 2`

#### Crash Analysis
- Successfully creates and frees P4_DYNAMIC memory for initial operations
- Crashes after ~22 P4 allocations
- Debug output shows proper allocation tracking
- Double-free occurs in __vdbe_free_p4() cleanup path

### test_memory (Release Build)
**Status**: CRASH - Invalid pointer in P4 memory management
**Failure Point**: During cleanup of VDBE opcodes
**Error**: `free(): invalid pointer`

#### Crash Analysis
- Creates 6 P4_DYNAMIC allocations successfully
- Crashes when attempting to free P4 memory at address 0x64f678
- Likely caused by memory corruption or aliasing
- Debug shows "SKIPPING free" messages for aliased memory

## Critical Issues Identified

### 1. P4 Memory Corruption Bug (CRITICAL)
**Impact**: Prevents all test execution with debug builds
**Symptoms**:
- Double-free errors in __vdbe_free_p4()
- Invalid pointer errors during cleanup
- Occurs even in release builds without ASAN

**Root Cause Analysis**:
The debug output reveals the bug pattern:
```
[FREE_P4] p4type=-6 p4=0x4b2d90 caller=0x7ffff7fa22a8
[FREE_P4 #6] Freeing P4_DYNAMIC at 0x4b2d90
[FREE_P4 #6] Successfully freed 0x4b2d90
...
[CREATE_P4_DYN #22] vdbe=0x4b2c40 opcode=147 addr=0 p4=0x5f31d0
...
free(): double free detected in tcache 2
```

The P4 pointers are being:
1. Allocated correctly via __vdbe_str_nappend()
2. Freed once during opcode cleanup
3. Freed again later (double-free)

**Likely Cause**: P4 pointers in column_names array are marked as MEM_Static
but point to dynamically allocated memory. When VDBE frees opcodes, it frees
the P4 pointers. Later, when freeing the mem_t structures, they attempt to
free the same memory again.

### 2. Database File Operations Failing
**Impact**: 75% of API tests fail
**Symptoms**:
- DBSQL_CANTOPEN (-50988) on file operations
- DBSQL_MISUSE (-50983) on SQL execution
- DBSQL_ERROR (-50999) on database close

**Possible Causes**:
- Berkeley DB backend not properly initialized
- File permissions or path issues
- Missing environment setup
- Incomplete Berkeley DB integration after memory allocation changes

### 3. Test Framework Limitations
**Observation**: Tests don't provide detailed failure information
**Needed**: Error messages from DBSQL API explaining why operations fail

## Debug Output Analysis

The extensive debug output shows:
- VDBE opcode growth working correctly
- P4 memory allocation tracking functional
- OP_ColumnName operations setting up column metadata
- Cleanup path correctly identifying P4 types
- Successful free operations followed by later double-frees

## Recommendations

### Immediate (Priority 1)
1. **Fix P4 double-free bug**:
   - Review __vdbe_free_p4() logic for P4_DYNAMIC type
   - Check if column_names array should use P4_DYNAMIC or P4_STATIC
   - Verify ownership semantics: who owns the memory after OP_ColumnName?
   - Add reference counting or clear ownership transfer

2. **Disable debug fprintf statements**:
   - Remove or ifdef all GROW_OPCODES, CREATE_P4_DYN, FREE_P4 debug output
   - Current output makes test results unreadable

### High Priority (Priority 2)
3. **Investigate database file operations**:
   - Add error message output to test failures
   - Check Berkeley DB initialization in test setup
   - Verify file paths and permissions
   - Test with in-memory database first (":memory:")

4. **Add better test diagnostics**:
   - Print DBSQL error messages on failure
   - Add test_framework function to print dbsql_errmsg()
   - Show which specific operation failed and why

### Medium Priority (Priority 3)
5. **Build test_fts suite** (not attempted yet)
6. **Run coverage builds** (blocked by crashes)
7. **Create minimal reproducer** for P4 bug

## Test Environment
- Platform: Linux 6.12.76
- Compiler: GCC with -std=c99
- Library: libdbsql-0.4 (Berkeley DB backend)
- ASAN: Full sanitizer suite in debug builds
- Working Directory: /home/gburd/ws/dbsql/test/src

## Files Generated
- test_api_results.log (empty due to redirect issues)
- Core dumps (not created - core limit may be 0)

## Next Steps

1. Fix P4 memory bug (BLOCKING all other tests)
2. Remove debug output statements
3. Rebuild and re-run test suite
4. Investigate database file operation failures
5. Add better error reporting to tests
6. Document test failures with actual error messages

## Summary

The test suite infrastructure is working correctly and can execute tests.
However, two critical bugs prevent meaningful test execution:

1. **P4 memory corruption** causes crashes in all test suites
2. **Database file operations fail** causing 75% of API tests to fail

The P4 bug is well-documented in debug output and should be fixable by
reviewing the memory ownership semantics in __vdbe_free_p4() and the
OP_ColumnName implementation.

Once these bugs are fixed, the test framework appears solid and ready for
comprehensive testing.
