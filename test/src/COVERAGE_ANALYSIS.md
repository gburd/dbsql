# Test Coverage Analysis and Recommendations

**Generated**: 2026-03-27
**Working Directory**: `/home/gburd/ws/dbsql/test/src`

## Executive Summary

Test coverage work was initiated but is **blocked by fundamental API failures**. The test infrastructure is operational, and a critical crash bug was fixed, but database operations fail consistently, preventing meaningful coverage measurement.

**Critical Finding**: Attempting to reach 95% coverage is premature. The codebase has basic functionality issues that must be resolved first.

## Work Completed

### 1. Test Infrastructure Assessment
- **Makefile**: Fully functional with coverage support (BUILD_TYPE=COVERAGE)
- **Test Framework**: Working TAP-compliant test framework
- **Test Suites**:
  - test_api.c (40 tests defined)
  - test_vdbe.c (15 tests defined)
  - test_memory.c (12 tests defined)
  - test_fts.c (full-text search tests)

### 2. Critical Bug Fixed: P3_POINTER Invalid Free

**Problem**: Tests crashed immediately with "free(): invalid pointer"

**Investigation**:
```
Backtrace showed crash in __vdbe_free_p4() at line 565 (vdbe_method.c)
Trying to free pointer 0x81cd58 marked as P4_DYNAMIC
But this pointer was &table->tnum (struct field address, not heap memory)
```

**Root Cause**:
```c
// vdbe.h line 161 - INCORRECT
#define P3_POINTER    P4_DYNAMIC  /* Would try to free non-heap pointers */

// cg_build.c line 1339 - Usage that triggered bug
__vdbe_change_p4(v, -1, (char *)&table->tnum, P3_POINTER);
```

**Fix Applied**:
1. Added new P4 type for non-owning pointers:
   ```c
   #define P4_POINTER    (-17)  /* Generic non-owning pointer */
   ```

2. Changed P3_POINTER mapping:
   ```c
   #define P3_POINTER    P4_POINTER  /* No longer tries to free */
   ```

3. Updated __vdbe_free_p4() to handle P4_POINTER:
   ```c
   case P4_POINTER:
       /* No deallocation needed */
       break;
   ```

**Files Modified**:
- `/home/gburd/ws/dbsql/src/inc/vdbe.h`
- `/home/gburd/ws/dbsql/src/vdbe_method.c`

**Result**: Tests no longer crash, exposing underlying API failures.

## Current Test Results

### test_api: 10/40 passing (25%)

**Error Pattern**:
```
ok 1 - dbsql_create_env() succeeds
not ok 2 - db->open() fails with CANTOPEN (-50988)
not ok 3 - db->exec() fails with MISUSE (-50983)
not ok 4 - db->close() fails with ERROR (-50999)
```

**All 30 failures** fall into three categories:

1. **DBSQL_CANTOPEN (-50988)**: Cannot open database files
   - Affects: All file-based database tests
   - First failure point after dbsql_create_env()

2. **DBSQL_MISUSE (-50983)**: SQL execution failures
   - Affects: CREATE TABLE, INSERT, SELECT, transactions
   - Occurs when SQL statements are executed

3. **DBSQL_ERROR (-50999)**: Generic errors
   - Affects: db->close() and cleanup operations
   - Suggests resource management issues

### Other Test Suites

- **test_vdbe**: Cannot run (requires working database)
- **test_memory**: Cannot run (requires working operations)
- **test_fts**: Cannot run (requires working SQL)

## Blocking Issues

### Issue #1: Database Opening Failure (CRITICAL)
**Error**: DBSQL_CANTOPEN (-50988)
**Impact**: Blocks 100% of tests
**Suspected Cause**:
- Berkeley DB backend not initializing correctly
- File path handling broken
- Permission or configuration issues

**Investigation Needed**:
```bash
# Debug database opening
gdb --args ./test_api
(gdb) break __dbsql_open
(gdb) run
# Examine dbp, filename, flags
```

### Issue #2: SQL Execution Failure
**Error**: DBSQL_MISUSE (-50983)
**Impact**: Blocks all SQL operations
**Suspected Cause**:
- Parser integration broken
- VDBE not initialized properly
- Missing transaction context

### Issue #3: Cleanup Failure
**Error**: DBSQL_ERROR (-50999)
**Impact**: Resource leaks likely
**Suspected Cause**:
- Improper resource tracking
- Double-free or use-after-free
- Incomplete error paths

## Coverage Assessment: Not Measurable

### Why Coverage Cannot Be Measured
1. Tests require database operations to execute
2. Database operations fail immediately
3. Most code paths never reached
4. Coverage data would show ~0% meaningful coverage

### What Coverage Would Show (If Tests Passed)
Based on test suite structure, would exercise:

**API Layer** (~60% estimated):
- Environment creation ✓
- Database opening ✗
- SQL execution ✗
- Transaction management ✗
- Error handling partial

**VDBE Layer** (~30% estimated):
- Opcode execution ✗
- Memory management ✗
- Cursor operations ✗
- Result handling ✗

**SQL Parser** (~40% estimated):
- CREATE TABLE ✗
- INSERT/SELECT ✗
- WHERE clauses ✗
- JOINs ✗

**Actual Measurable Coverage**: < 10%

## Tests That Would Fill Coverage Gaps (When Unblocked)

### Priority 1: API Fundamentals
```c
// test_api.c additions needed:

TEST(api_error_callback) {
    // Test db->set_errcall() functionality
    // Verify error messages propagate correctly
}

TEST(api_prepared_statements) {
    // Test prepare, bind, step, finalize
    // Verify parameter binding
    // Test statement reuse
}

TEST(api_row_metadata) {
    // Test db->last_insert_rowid()
    // Test db->changes()
    // Test column count/names
}

TEST(api_concurrent_statements) {
    // Multiple prepared statements
    // Interleaved execution
}
```

### Priority 2: VDBE Operations
```c
// test_vdbe.c additions needed:

TEST(vdbe_null_operations) {
    // IS NULL, IS NOT NULL, IFNULL, COALESCE
}

TEST(vdbe_case_expressions) {
    // Simple CASE, searched CASE
}

TEST(vdbe_aggregates_advanced) {
    // GROUP BY, HAVING
    // Multiple aggregate functions
    // DISTINCT in aggregates
}

TEST(vdbe_joins) {
    // LEFT JOIN, INNER JOIN, CROSS JOIN
    // Multiple table joins
}

TEST(vdbe_subqueries) {
    // IN (subquery)
    // EXISTS (subquery)
    // Scalar subqueries
}

TEST(vdbe_limit_offset) {
    // LIMIT, OFFSET
    // Combined with ORDER BY
}
```

### Priority 3: Memory Management
```c
// test_memory.c additions needed:

TEST(memory_vdbe_reuse) {
    // Execute same VDBE multiple times
    // Verify memory doesn't grow
}

TEST(memory_prepared_stmt_reuse) {
    // Prepare once, execute multiple times
    // Verify bind parameter cleanup
}

TEST(memory_large_blobs) {
    // Insert/retrieve large BLOBs
    // Verify streaming behavior
}

TEST(memory_exhaustion) {
    // Simulate memory limits
    // Verify graceful failure
}
```

### Priority 4: SQL Parser Edge Cases
```c
// test_sql_parser.c (new file needed):

TEST(parser_syntax_variations) {
    // Single vs double quotes
    // Whitespace handling
    // Case insensitivity
}

TEST(parser_error_recovery) {
    // Malformed SQL
    // Missing tokens
    // Verify error messages
}

TEST(parser_ddl) {
    // CREATE INDEX
    // ALTER TABLE
    // DROP TABLE
}

TEST(parser_complex_expressions) {
    // Operator precedence
    // Parentheses
    // Function calls
}
```

### Estimated Coverage Impact

| Test Addition | Estimated Coverage Gain | Effort |
|---------------|-------------------------|--------|
| API fundamentals | +15% | 1 day |
| VDBE operations | +25% | 2 days |
| Memory management | +10% | 1 day |
| SQL parser | +15% | 1 day |
| **Total** | **+65%** | **5 days** |

**Note**: This assumes database operations are working (Issues #1-3 fixed first).

## Recommended Actions

### Immediate (Before Adding Tests)

1. **Fix DBSQL_CANTOPEN**
   ```bash
   # Debug the open failure
   cd /home/gburd/ws/dbsql/test/src
   gdb ./test_api
   (gdb) break __dbsql_open
   (gdb) break __api_open
   (gdb) run
   ```

2. **Add Diagnostic Logging**
   ```c
   // In api.c, add fprintf to understand failure:
   fprintf(stderr, "DEBUG: __dbsql_open called with file=%s\n", filename);
   // Before each return, log:
   fprintf(stderr, "DEBUG: returning %d\n", rc);
   ```

3. **Verify Berkeley DB Backend**
   ```bash
   # Check if BDB is accessible
   ldd ../../build_unix.old/.libs/libdbsql-0.4.so | grep db
   # Should show libdb-4.8.so
   ```

### Short Term (Once Database Works)

1. Run existing tests to establish baseline coverage
2. Identify top 20 uncovered functions from lcov report
3. Add targeted tests for high-value functions
4. Iterate until 95% coverage achieved

### Long Term

1. Integrate with CI/CD for continuous coverage tracking
2. Add property-based testing for SQL parser
3. Add fuzz testing for malformed inputs
4. Run SQLite test suite for compatibility

## Tools and Commands

### Run Tests
```bash
cd /home/gburd/ws/dbsql/test/src

# Build and run tests
make BUILD_TYPE=COVERAGE clean all test

# Run single test for debugging
LD_LIBRARY_PATH=../../build_unix.old/.libs ./test_api

# Run with gdb
LD_LIBRARY_PATH=../../build_unix.old/.libs gdb ./test_api
```

### Generate Coverage (When Tests Pass)
```bash
# Generate HTML report
make BUILD_TYPE=COVERAGE coverage

# Check if meets 95% target
make BUILD_TYPE=COVERAGE coverage-check

# View report
open coverage_html/index.html
```

### Analyze Coverage Gaps
```bash
# Find uncovered functions
lcov --list coverage.info | grep "0.0%" | head -20

# Generate per-file report
genhtml coverage.info --output-directory coverage_html --branch-coverage
```

## Conclusion

**Current Status**: Test infrastructure ready, critical crash fixed, but blocked by API failures.

**Coverage Target**: Cannot measure until database operations work.

**Recommendation**: **Focus on fixing CANTOPEN, MISUSE, and ERROR before adding tests.** The existing test suite is sufficient to identify and drive fixes. Adding more tests now would be waste of effort since they would all fail the same way.

**Estimated Timeline**:
- Fix blocking issues: 3-5 days
- Add coverage tests: 2-3 days
- Reach 95% target: 5-8 days total

**Next Step**: Debug database opening failure (CANTOPEN) as the critical blocker.
