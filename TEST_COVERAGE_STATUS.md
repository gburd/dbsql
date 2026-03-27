# DBSQL Test Coverage Status

**Date**: 2026-03-27
**Branch**: fix/vdbe-opcode-init
**Current Status**: Test infrastructure ready, blocked by API failures

## Summary

Test infrastructure is now operational with the following test suites:
- `test_api.c` - Core API functionality tests
- `test_vdbe.c` - VDBE execution tests
- `test_memory.c` - Memory management tests
- `test_fts.c` - Full-text search tests

However, **95% coverage target is currently unachievable** due to fundamental API failures preventing test execution.

## Critical Fix Applied

### P3_POINTER Invalid Free Bug (Task #90)
**Problem**: Tests crashed with "free(): invalid pointer" during VDBE cleanup.

**Root Cause**: `P3_POINTER` was incorrectly defined as `P4_DYNAMIC`, causing non-heap pointers (like `&table->tnum` in struct fields) to be freed.

**Fix Applied**:
1. Defined new `P4_POINTER (-17)` type for non-owning pointers
2. Changed `P3_POINTER` to map to `P4_POINTER` instead of `P4_DYNAMIC`
3. Updated `__vdbe_free_p4()` to skip freeing P4_POINTER types
4. Added P4_POINTER to the "no deallocation needed" section

**Files Modified**:
- `/home/gburd/ws/dbsql/src/inc/vdbe.h` - Added P4_POINTER definition
- `/home/gburd/ws/dbsql/src/vdbe_method.c` - Added P4_POINTER case to __vdbe_free_p4

**Result**: Tests no longer crash, but now encounter actual API errors.

## Current Test Results

### test_api - 10/40 passing (25%)
**Failures**: DBSQL_CANTOPEN (-50988), DBSQL_MISUSE (-50983), DBSQL_ERROR (-50999)

All tests fail after `dbsql_create_env()` succeeds due to:
- `db->open()` - CANTOPEN (-50988) - Cannot open database files
- `db->exec()` - MISUSE (-50983) - SQL execution not working
- `db->close()` - ERROR (-50999) - Cleanup issues

### test_vdbe - Not run (blocked by db->open failure)
Requires working database connection to test VDBE opcodes.

### test_memory - Not run (blocked by db->open failure)
Requires working database operations to test memory management.

### test_fts - Not run (blocked by db->open failure)
Requires working database and SQL to test full-text search.

## Blocked Coverage Targets

### Cannot Test Until Fixed:
1. **Database Opening** (DBSQL_CANTOPEN)
   - File-based databases
   - In-memory databases
   - Connection management

2. **SQL Execution** (DBSQL_MISUSE)
   - CREATE TABLE
   - INSERT/UPDATE/DELETE
   - SELECT queries
   - Transactions (BEGIN/COMMIT/ROLLBACK)

3. **Database Closing** (DBSQL_ERROR)
   - Resource cleanup
   - File handle management

### High-Value Untested Code Paths:
Once database operations work, priority should be:

**API Layer** (`test_api.c` additions):
- Error callbacks (`db->set_errcall()`)
- Prepared statements (prepare, bind, step, finalize)
- Row metadata (`db->last_insert_rowid()`, `db->changes()`)
- Memory exhaustion scenarios

**VDBE Layer** (`test_vdbe.c` additions):
- NULL comparisons (IS NULL, IS NOT NULL)
- CASE expressions
- COALESCE and IFNULL
- DISTINCT operations
- LIMIT and OFFSET
- JOIN variants (LEFT JOIN, INNER JOIN, CROSS JOIN)
- Subqueries in WHERE clause
- GROUP BY with aggregate functions
- HAVING clause filtering
- ORDER BY with multiple columns

**Memory Management** (`test_memory.c` additions):
- VDBE reuse across queries
- Prepared statement reuse
- Bind parameter memory
- Large BLOB handling
- Memory limits and exhaustion

**SQL Parser** (new `test_sql_parser.c`):
- Parser error recovery
- Syntax variations (single/double quotes, etc.)
- Edge cases (empty statements, comments)
- DDL statements (ALTER, DROP, CREATE INDEX)
- Complex expressions and precedence

## Coverage Infrastructure

### Build System
- Makefile supports BUILD_TYPE=COVERAGE
- Uses `--coverage -fprofile-arcs -ftest-coverage`
- Generates .gcda/.gcno files for gcov/lcov

### Coverage Tools Available
- `make coverage` - Generate HTML report
- `make coverage-check` - Verify 95% target
- lcov/genhtml for detailed analysis

### Test Framework
- TAP-compliant output format
- Assertion macros (ASSERT_EQ, ASSERT_NOT_NULL, etc.)
- Test timing and statistics

## Path Forward

### Phase 1: Fix API Failures (CRITICAL)
1. Debug DBSQL_CANTOPEN - Database opening mechanism
2. Debug DBSQL_MISUSE - SQL execution path
3. Debug DBSQL_ERROR - Cleanup/closing logic

**Estimated Effort**: 3-5 days
**Impact**: Unblocks ALL test coverage work

### Phase 2: Expand Test Coverage (After Phase 1)
1. Add API tests (prepared statements, callbacks, metadata)
2. Add VDBE tests (NULL ops, CASE, JOIN, subqueries)
3. Add memory tests (reuse, limits, large data)
4. Add parser tests (error recovery, edge cases)

**Estimated Effort**: 2-3 days
**Target Coverage**: 95% function and branch coverage

### Phase 3: Integration Testing
1. Run against SQLite test suite
2. Stress testing with large datasets
3. Concurrent access testing
4. Memory leak verification (valgrind)

**Estimated Effort**: 2-3 days

## Recommendations

1. **DO NOT add more tests** until database operations work
   - Current tests are sufficient to identify blocking issues
   - More tests will just fail the same way

2. **Focus on fixing CANTOPEN error first**
   - Root cause likely in `__dbsql_open()` or file operations
   - Check Berkeley DB backend initialization
   - Verify file path handling

3. **Document error code origins**
   - Map error codes to source locations
   - Add diagnostic logging to error paths

4. **Consider test-driven development**
   - Fix one error at a time
   - Verify tests pass before moving to next

## Test Execution

```bash
cd /home/gburd/ws/dbsql/test/src

# Run all tests (currently fails)
make BUILD_TYPE=COVERAGE test

# Run individual test (for debugging)
LD_LIBRARY_PATH=../../build_unix.old/.libs ./test_api

# Generate coverage (when tests pass)
make BUILD_TYPE=COVERAGE coverage

# Check coverage target
make BUILD_TYPE=COVERAGE coverage-check
```

## Notes

- All test source files are in `/home/gburd/ws/dbsql/test/src/`
- Test framework is minimal but functional
- P3_POINTER crash fix committed to current branch
- Debug output (fprintf to stderr) should be removed before coverage analysis
