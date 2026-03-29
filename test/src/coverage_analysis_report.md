# DBSQL Code Coverage Analysis Report

**Date**: 2026-03-27
**Status**: Coverage Measurement Blocked
**Target**: 95% line and function coverage

## Executive Summary

Code coverage measurement for DBSQL is currently blocked by infrastructure issues. The test suite exists but requires significant setup to enable coverage collection.

## Current State

### Test Infrastructure Status

1. **Unit Test Suite** (`/home/gburd/ws/dbsql/test/src/`)
   - Status: Non-functional
   - Issue: Test code uses incorrect API (direct function calls instead of API structure)
   - Files: test_api.c, test_vdbe.c, test_memory.c, test_framework.c
   - Required fix: Rewrite tests to use `dbsql_create()` and function pointers

2. **TCL Test Suite** (`/home/gburd/ws/dbsql/test/scr050/tests/`)
   - Status: Available but not coverage-instrumented
   - Test files: 64 .test files
   - Runner: dbsql_tclsh (TCL shell with DBSQL extensions)
   - Coverage instrumentation: Not enabled in library build

3. **DBSQL Library Build**
   - Location: `/home/gburd/ws/dbsql/build_unix.old/.libs/libdbsql-0.4.so`
   - Build flags: `-g -DCONFIG_TEST` (debug symbols, but no coverage)
   - Missing: `--coverage -fprofile-arcs -ftest-coverage` flags

## Blocking Issues

### Issue 1: API Mismatch in Unit Tests

The unit tests attempt to call DBSQL functions directly:
```c
rc = dbsql_open(":memory:", &db);  // WRONG
rc = dbsql_close(db);              // WRONG
rc = dbsql_exec(db, "SELECT 1", NULL, NULL, &errmsg);  // WRONG
```

DBSQL uses a Berkeley DB-style API with function pointers:
```c
DBSQL *dbp;
dbsql_create(&dbp, NULL, 0);    // Initialize handle
dbp->open(dbp, ":memory:", 0, NULL);  // Correct usage
dbp->exec(dbp, "SELECT 1", NULL, NULL, NULL);
dbp->close(dbp);
```

**Resolution Required**: Rewrite 4 test files (~800 lines) to use correct API.

### Issue 2: Library Not Built with Coverage

The DBSQL library in `build_unix.old` was not compiled with coverage instrumentation.

**Current flags**: `-c $(CPPFLAGS) -DCONFIG_TEST -g`
**Required flags**: `-c $(CPPFLAGS) -DCONFIG_TEST -g -O0 --coverage -fprofile-arcs -ftest-coverage`

**Resolution Required**:
1. Modify `build_unix.old/Makefile` CFLAGS
2. Clean and rebuild library: `make clean && make`
3. Rebuild test runner: `cd test/scr050 && make clean && make`

### Issue 3: Test Suite Coverage Integration

The TCL test suite can run tests but doesn't generate `.gcda` coverage files because the library isn't instrumented.

**Resolution Required**:
1. Build library with coverage flags (Issue 2)
2. Run test suite: `cd test/scr050 && ./dbsql_tclsh tests/all.test`
3. Collect coverage: `lcov --capture --directory ../../build_unix.old --output-file coverage.info`

## Source File Inventory

Based on the source tree, here are the key files that would be measured:

### Core Components (`/home/gburd/ws/dbsql/src/`)

| File | LOC (est.) | Component | Priority |
|------|------------|-----------|----------|
| vdbe.c | 3,900 | Virtual Database Engine | Critical |
| vdbe_method.c | 1,300 | VDBE methods | Critical |
| cg_select.c | 2,200 | SELECT code generation | High |
| cg_expr.c | 1,500 | Expression code generation | High |
| cg_build.c | 2,000 | DDL code generation | High |
| cg_where.c | 1,100 | WHERE clause code generation | High |
| sm.c | 1,300 | Storage manager | High |
| sql_fns.c | 1,200 | SQL functions | Medium |
| api.c | 950 | Public API | Critical |
| cg_date.c | 700 | Date/time functions | Medium |
| cg_insert.c | 550 | INSERT code generation | High |
| cg_delete.c | 400 | DELETE code generation | Medium |
| cg_update.c | 450 | UPDATE code generation | Medium |
| sql_tokenize.c | 700 | Lexer | Medium |
| sql_parser.y | 800 | Parser grammar | High |

**Total estimated LOC**: ~19,050 lines

### Support Libraries

- `src/clib/`: C library utilities (~500 LOC)
- `src/common/`: Common utilities (~300 LOC)
- `src/os/`: OS abstraction layer (~400 LOC)

## Coverage Gap Analysis (Estimated)

Without actual measurements, based on test suite scope:

### Likely Well-Covered Areas
- Basic SELECT/INSERT/UPDATE/DELETE operations
- Table creation and schema management
- Expression evaluation
- Basic SQL functions
- Transaction handling

### Likely Poorly-Covered Areas
- Error handling paths (~30-40% coverage estimated)
- Edge cases (NULL handling, type coercion)
- Complex query optimization
- Attachment and multi-database operations
- Berkeley DB integration layer
- Memory allocation failure paths
- Authorization and authentication
- Trigger execution
- Complex WHERE clauses with subqueries

### Critical Uncovered Functions (Estimated)

Based on code structure, these functions likely have < 50% coverage:

1. **vdbe.c**:
   - `__vdbe_sort_compare()` - Complex sorting logic
   - `__vdbe_make_record()` - Binary record serialization
   - Error handling in opcode execution

2. **cg_where.c**:
   - `__where_begin()` - WHERE clause optimization
   - Index selection logic

3. **cg_select.c**:
   - `__select_flatten_subquery()` - Query flattening
   - Join optimization

4. **sm.c**:
   - Berkeley DB cursor management
   - Transaction savepoints

5. **api.c**:
   - Error callback mechanisms
   - Progress callbacks

## Recommended Next Steps

### Phase 1: Enable Coverage Collection (2-3 hours)

1. Modify `/home/gburd/ws/dbsql/build_unix.old/Makefile`:
   ```makefile
   CFLAGS = -c $(CPPFLAGS) -DCONFIG_TEST -g -O0 --coverage -fprofile-arcs -ftest-coverage
   LDFLAGS = --coverage
   ```

2. Rebuild library:
   ```bash
   cd /home/gburd/ws/dbsql/build_unix.old
   make clean
   make -j4
   ```

3. Rebuild test runner:
   ```bash
   cd /home/gburd/ws/dbsql/test/scr050
   make clean
   make
   ```

### Phase 2: Generate Baseline Coverage (30 minutes)

1. Run test suite:
   ```bash
   cd /home/gburd/ws/dbsql/test/scr050
   export LD_LIBRARY_PATH=../../build_unix.old/.libs:$LD_LIBRARY_PATH
   ./dbsql_tclsh tests/quick.test  # Quick smoke test
   ./dbsql_tclsh tests/all.test    # Full suite
   ```

2. Generate coverage report:
   ```bash
   cd /home/gburd/ws/dbsql/build_unix.old
   lcov --capture --directory . --output-file coverage.info
   lcov --remove coverage.info '/usr/*' '/nix/*' --output-file coverage_filtered.info
   genhtml coverage_filtered.info --output-directory coverage_html
   ```

3. View report:
   ```bash
   firefox coverage_html/index.html
   ```

### Phase 3: Fix Unit Tests (4-6 hours)

Rewrite `/home/gburd/ws/dbsql/test/src/test_*.c` files to use correct API:

```c
// Old (incorrect)
dbsql_open(":memory:", &db);

// New (correct)
DBSQL *dbp;
dbsql_create(&dbp, NULL, 0);
dbp->open(dbp, ":memory:", 0, NULL);
```

### Phase 4: Analyze and Close Gaps (ongoing)

1. Identify functions with < 95% coverage
2. Write targeted tests for uncovered paths
3. Focus on error handling and edge cases
4. Add tests for Berkeley DB-specific features

## Estimated Coverage Targets

Based on typical database engine coverage:

| Component | Current (est.) | Target | Gap |
|-----------|----------------|--------|-----|
| VDBE core | 60% | 95% | +35% |
| Code generation | 70% | 95% | +25% |
| SQL functions | 75% | 95% | +20% |
| API layer | 80% | 95% | +15% |
| Error handling | 35% | 95% | +60% |
| **Overall** | **65%** | **95%** | **+30%** |

## Cost-Benefit Analysis

**Effort to enable coverage**: 3-4 hours
**Effort to reach 95%**: 40-80 hours
**High-value activities**:
1. Enable coverage collection (3 hours) → Immediate visibility
2. Fix unit tests (6 hours) → Focused component testing
3. Add error path tests (20 hours) → Critical for stability
4. Add edge case tests (20 hours) → Prevents production bugs

## Conclusion

Code coverage measurement is currently blocked but fixable with moderate effort. The test infrastructure exists (64 TCL test files) but needs:

1. Library rebuild with coverage flags
2. Test suite execution with instrumented library
3. Coverage report generation with lcov

The unit tests are broken and need API corrections but can be deferred - the TCL test suite is sufficient for baseline coverage measurement.

**Immediate Action**: Rebuild library with coverage flags and run TCL test suite to establish baseline metrics.

## Files Modified

- `/home/gburd/ws/dbsql/test/src/Makefile` - Fixed library path to use `.libs/` subdirectory
