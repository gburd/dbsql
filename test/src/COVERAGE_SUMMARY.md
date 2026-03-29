# DBSQL Test Coverage Report

**Generated:** 2026-03-27

## Executive Summary

| Metric | Value |
|--------|-------|
| .gcda files generated | 38 |
| **Overall Line Coverage** | **29.17%** |
| **Overall Branch Coverage** | **40.14%** |
| Total Lines | 29,932 |
| Lines Executed | 8,731 |
| Total Branches | 9,441 |
| Branches Executed | 3,789 |

## Coverage Report Location

Full detailed report: `/home/gburd/ws/dbsql/test/src/coverage_html/coverage_report.txt`

## Critical Findings

### Highest Coverage Files (>70%)
- `common/dbsql_alloc.c`: **83.78%** (37 lines) - Memory allocation
- `lemon/lemon.c`: **74.54%** (2074 lines) - Parser generator
- `common/hash.c`: **71.98%** (182 lines) - Hash tables

### Most Important Low Coverage Files
1. **vdbe.c**: 15.68% (1798 lines) - CORE virtual machine
2. **cg_expr.c**: 15.52% (1044 lines) - Expression codegen
3. **cg_select.c**: 21.71% (1170 lines) - SELECT codegen
4. **cg_where.c**: 16.15% (644 lines) - WHERE clause codegen

### Completely Untested (0% coverage)
- `api_table.c` (88 lines)
- `cg_pragma.c` (186 lines)
- `cg_copy.c` (61 lines)
- `xvprintf.c` (359 lines)
- `util.c` (35 lines)

## Test Execution Notes

Tests ran with some failures:
- Multiple -50998 errors (DB_LOCK_NOTGRANTED)
- Multiple -50999 errors (DB_LOCK_DEADLOCK)

These locking issues prevented some code paths from being executed.

## Recommendations

1. **CRITICAL**: Increase vdbe.c coverage - this is the execution engine core
2. **HIGH**: Test code generation modules (cg_expr, cg_select, cg_where)
3. **MEDIUM**: Add tests for zero-coverage modules
4. **Fix**: Resolve locking errors to enable more test scenarios

---

*Note: lcov and genhtml were not available, so coverage was analyzed using gcov directly.*
