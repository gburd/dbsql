# DBSQL Code Coverage Status

**Assessment Date**: 2026-03-27
**Status**: ⚠️ Measurement infrastructure not operational
**Current Coverage**: Unknown (cannot measure)
**Target Coverage**: 95% line and function coverage

## Summary

Code coverage cannot currently be measured for DBSQL due to:

1. **Library not instrumented** - DBSQL library was not built with `--coverage` flags
2. **Unit tests broken** - Test code uses incorrect API (needs rewrite)
3. **TCL tests not instrumented** - Test runner not linked against instrumented library

## Quick Start: Enable Coverage

Run the provided script to enable coverage collection:

```bash
cd /home/gburd/ws/dbsql/test/src
bash enable_coverage.sh
```

This will:
- Modify Makefile to add coverage flags
- Rebuild library with instrumentation
- Rebuild test runner
- Provide instructions for collecting coverage data

## Current Test Infrastructure

### ✅ TCL Test Suite (Operational)
- **Location**: `/home/gburd/ws/dbsql/test/scr050/tests/`
- **Test count**: 64 .test files
- **Runner**: `dbsql_tclsh` (TCL shell with DBSQL extensions)
- **Status**: Can run tests, but no coverage collection
- **Use case**: Baseline coverage measurement after instrumentation

### ❌ Unit Test Suite (Non-operational)
- **Location**: `/home/gburd/ws/dbsql/test/src/`
- **Files**: test_api.c, test_vdbe.c, test_memory.c, test_framework.c
- **Issue**: Uses incorrect API (direct calls instead of function pointers)
- **Status**: Requires rewrite (~800 lines across 4 files)
- **Use case**: Component-level testing after API fix

## Source Files to Measure

Key components requiring coverage measurement:

| Component | Files | Est. LOC | Priority |
|-----------|-------|----------|----------|
| VDBE | vdbe.c, vdbe_method.c | 5,200 | Critical |
| Code Gen | cg_*.c (15 files) | 9,500 | High |
| API | api.c, api_table.c | 1,100 | Critical |
| Storage | sm.c | 1,300 | High |
| SQL Functions | sql_fns.c | 1,200 | Medium |
| Parser/Lexer | sql_parser.y, sql_tokenize.c | 1,500 | High |
| **Total** | **~25 files** | **~19,800** | |

## Estimated Coverage Gaps

Without actual measurements, based on typical database engine coverage patterns:

- **Overall estimate**: 60-70% (assuming comprehensive TCL test suite)
- **Error paths**: 30-40% (typically under-tested)
- **Edge cases**: 40-50% (NULL handling, type coercion, etc.)
- **Berkeley DB integration**: 50-60% (HA features, advanced locking)

### High-Priority Uncovered Areas (Estimated)

1. **Error handling** - Exception paths, allocation failures
2. **Edge cases** - NULL values, type mismatches, boundary conditions
3. **Complex queries** - Nested subqueries, complex joins, optimization
4. **Transactions** - Savepoints, nested transactions, deadlock handling
5. **Berkeley DB features** - Replication, distributed transactions
6. **Memory management** - Allocation failure paths, leak prevention
7. **Authorization** - Access control, authentication
8. **Triggers** - Trigger execution, cascading updates

## Action Plan

### Phase 1: Enable Measurement (3 hours)
1. Run `enable_coverage.sh` script
2. Verify library rebuilt with coverage flags
3. Run TCL test suite: `cd test/scr050 && ./dbsql_tclsh tests/quick.test`
4. Generate baseline coverage report with lcov

### Phase 2: Analyze Baseline (1 hour)
1. Review HTML coverage report
2. Identify functions with 0% coverage
3. Identify files with < 80% coverage
4. Prioritize based on criticality

### Phase 3: Close Gaps (40-80 hours)
1. Write tests for uncovered critical paths (VDBE, API)
2. Add error path tests (allocation failures, invalid inputs)
3. Add edge case tests (NULL, boundaries, type coercion)
4. Add integration tests (transactions, multi-DB)

### Phase 4: Fix Unit Tests (6 hours, parallel)
1. Update test_api.c to use correct API
2. Update test_vdbe.c to use correct API
3. Update test_memory.c to use correct API
4. Integrate into coverage measurement

## Tools Required

- **lcov** - Coverage visualization (`nix-shell -p lcov`)
- **genhtml** - HTML report generation (included with lcov)
- **gcov** - Coverage data processor (included with gcc)
- **browser** - View HTML reports

## Expected Coverage Targets

| Component | Baseline (est.) | Target | Tests Required |
|-----------|-----------------|--------|----------------|
| VDBE core | 65% | 95% | +30% error paths, edge cases |
| Code gen | 70% | 95% | +25% complex queries, DDL |
| SQL functions | 75% | 95% | +20% edge cases, NULL handling |
| API layer | 80% | 95% | +15% error callbacks, edge cases |
| Error handling | 35% | 95% | +60% allocation failures, invalid inputs |
| **Overall** | **65%** | **95%** | **+30%** |

## Time Investment

- **Setup coverage**: 3-4 hours (one-time)
- **Baseline measurement**: 30 minutes
- **Reach 80% coverage**: 20-30 hours
- **Reach 95% coverage**: 40-80 hours (includes error paths, edge cases)

## Benefits

1. **Bug prevention** - Find untested code paths before production
2. **Regression detection** - Ensure new code is tested
3. **Refactoring confidence** - Verify behavior preserved
4. **Code quality metric** - Objective measure of test completeness
5. **Compliance** - Meet QA requirements for production deployment

## Next Steps

**Immediate (< 4 hours)**:
1. Run `bash enable_coverage.sh` to set up infrastructure
2. Execute `cd test/scr050 && ./dbsql_tclsh tests/quick.test`
3. Generate report: `lcov --capture --directory ../../build_unix.old --output-file coverage.info`
4. Review HTML report to identify gaps

**This will provide actual coverage percentages and specific uncovered functions**, replacing the estimates in this document with real data.

## Files

- **Report**: `/home/gburd/ws/dbsql/test/src/coverage_analysis_report.md` (detailed analysis)
- **Script**: `/home/gburd/ws/dbsql/test/src/enable_coverage.sh` (automation)
- **Status**: This file (executive summary)
