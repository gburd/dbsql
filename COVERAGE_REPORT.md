# DBSQL Code Coverage Report

**Generated:** 2026-03-27
**Test Suite:** test_api (12/41 tests passing)
**Build:** Coverage instrumentation with --coverage flag

## Overall Coverage

**24.15%** (3,619 / 14,983 lines)

## Coverage by Module

| Module | Coverage | Lines Covered | Total Lines |
|--------|----------|---------------|-------------|
| API | 42.62% | 231 | 542 |
| Common/Utility | 24.41% | 239 | 979 |
| Storage Manager | 21.46% | 194 | 904 |
| SQL Parser | 15.55% | 338 | 2,173 |
| Code Generator | 13.61% | 732 | 5,380 |
| VDBE | 12.12% | 302 | 2,492 |
| C Library | 8.33% | 35 | 420 |
| OS Layer | 0.00% | 0 | 14 |

## Top Files by Coverage (>50%)

| File | Coverage | Lines |
|------|----------|-------|
| clib/strcasecmp.c | 100.00% | 17/17 |
| clib/memcmp.c | 100.00% | 8/8 |
| clib/snprintf.c | 100.00% | 10/10 |
| common/dbsql_alloc.c | 75.68% | 28/37 |
| common/hash.c | 71.98% | 131/182 |
| api.c | 51.10% | 231/454 |
| safety.c | 50.00% | 11/22 |

## Critical Files with Low Coverage (<20%)

These are core files with >100 lines that have very low test coverage:

| File | Coverage | Lines | Priority |
|------|----------|-------|----------|
| **vdbe.c** | 5.11% | 91/1,781 | CRITICAL |
| **cg_date.c** | 1.03% | 5/581 | HIGH |
| **sql_fns.c** | 4.69% | 25/554 | HIGH |
| **cg_insert.c** | 0.00% | 0/252 | HIGH |
| **cg_pragma.c** | 0.00% | 0/186 | MEDIUM |
| **cg_delete.c** | 0.00% | 0/183 | HIGH |
| **common/dbsql_err.c** | 3.57% | 6/196 | MEDIUM |
| **stubs.c** | 8.25% | 8/97 | LOW |

## Module Analysis

### API Layer (42.62%)
- api.c has decent coverage (51%)
- api_table.c is completely untested (0%)
- **Action:** Add tests for table operations

### Code Generator (13.61%)
- Most CG modules have <20% coverage
- cg_insert, cg_delete, cg_pragma, cg_copy, cg_vacuum are completely untested (0%)
- Only cg_build (20.69%) has minimal coverage
- **Action:** High priority - these modules generate SQL bytecode

### VDBE (12.12%)
- vdbe.c is critical with only 5.11% coverage
- vdbe_method.c has better coverage (29.68%)
- **Action:** CRITICAL - VDBE is the execution engine

### Storage Manager (21.46%)
- sm.c has 21.57% coverage
- **Action:** Needs more transaction and storage tests

### SQL Parser (15.55%)
- sql_parser.c: 22.11%
- sql_tokenize.c: 22.87%
- sql_fns.c: 4.69%
- **Action:** Add more SQL syntax tests

### Common/Utility (24.41%)
- Hash table well tested (71.98%)
- Memory allocation tested (75.68%)
- String utilities, error handling poorly tested
- **Action:** Add utility function tests

## Recommendations

### Immediate Actions (to reach 50% coverage)

1. **VDBE Tests** - Add opcode execution tests
   - Test basic opcodes (Open, Integer, String, ResultRow, Halt)
   - Test arithmetic operations
   - Test comparison operations
   - Target: 40% coverage of vdbe.c

2. **Code Generator Tests** - Add SQL statement tests
   - INSERT statements (currently 0%)
   - DELETE statements (currently 0%)
   - PRAGMA statements (currently 0%)
   - Target: 30% coverage of cg_* modules

3. **SQL Function Tests**
   - Test built-in functions (length, substr, etc.)
   - Test date/time functions
   - Target: 40% coverage of sql_fns.c and cg_date.c

### Medium-term Goals (to reach 80% coverage)

4. **Parser Tests** - Complex SQL statements
   - Subqueries
   - Joins
   - Complex WHERE clauses
   - Target: 60% coverage

5. **Storage Manager Tests**
   - Transaction handling
   - Rollback/commit
   - Concurrent access
   - Target: 60% coverage

6. **Error Handling Tests**
   - Invalid SQL
   - Resource exhaustion
   - Constraint violations
   - Target: 80% coverage of error paths

## Test Gaps

Current test suite (test_api) covers:
- Basic API operations (create, open, close)
- Simple CREATE TABLE
- Schema operations

Missing test coverage:
- INSERT/UPDATE/DELETE operations
- SELECT with WHERE clauses
- SQL functions
- Date/time operations
- PRAGMA commands
- Transactions
- Error conditions
- Complex queries

## Next Steps

1. Run with more comprehensive test suite when available
2. Add unit tests for individual modules
3. Focus on VDBE and Code Generator modules
4. Target: 50% overall coverage as first milestone
