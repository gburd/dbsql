# DBSQL Test Execution Plan

**Date:** 2026-03-24
**Branch:** fix/vdbe-opcode-init
**Commit:** 2040fd6
**Test Suite:** 60 test files, 2,870 test cases

## Executive Summary

This plan provides a systematic approach to testing DBSQL once the VDBE opcode initialization issue is resolved. The test suite is organized into phases progressing from basic smoke tests to the full test suite.

## Prerequisites

- [ ] VDBE opcode error fixed (unknown opcode 0)
- [ ] Binary builds successfully at `/home/gburd/ws/dbsql/result/bin/dbsql`
- [ ] TCL test runner available at `/home/gburd/ws/dbsql/test/scr050/dbsql_tclsh`
- [ ] Memory corruption fixes verified (malloc assertions eliminated)
- [ ] ORDER BY implementation ready (sorter opcodes implemented)

## Test Infrastructure

### Test Framework
- **Language:** TCL-based test framework
- **Runner:** `dbsql_tclsh` (TCL shell with DBSQL bindings)
- **Location:** `/home/gburd/ws/dbsql/test/scr050/tests/`
- **Test Format:** `.test` files using `do_test` procedure
- **Quick Test:** `./dbsql_tclsh tests/quick.test` (runs most tests)
- **All Tests:** `./dbsql_tclsh tests/all.test` (comprehensive suite)

### Test Execution Methods

**Method 1: Individual test file**
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/select1.test
```

**Method 2: Quick test suite**
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/quick.test
```

**Method 3: Full test suite**
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/all.test
```

**Method 4: Make target**
```bash
cd /home/gburd/ws/dbsql/test/scr050
make test
```

## Phase 1: Smoke Tests (15 minutes)

**Objective:** Verify basic functionality works before running comprehensive tests.

**Script:** `/home/gburd/ws/dbsql/test/smoke-test.sh`

**Tests:**
1. Version check
2. In-memory database creation
3. Simple SELECT (SELECT 1)
4. CREATE TABLE
5. INSERT
6. SELECT with WHERE
7. ORDER BY (validates sorter implementation)

**Exit Criteria:** All 7 smoke tests pass.

**If Failures:** Stop and investigate core issues before proceeding.

## Phase 2: Core SQL Tests (1-2 hours)

**Objective:** Validate fundamental SQL operations.

**Priority:** P0 (Blocking - must pass)

**Test Files (in order):**

1. **select1.test** (144 tests)
   - Basic SELECT operations
   - Column selection
   - Simple queries

2. **select2.test** (19 tests)
   - More SELECT variations
   - Complex column expressions

3. **insert.test** (42 tests)
   - INSERT operations
   - Multiple insert methods
   - Error handling

4. **update.test** (108 tests)
   - UPDATE statements
   - WHERE clauses in updates
   - Multi-row updates

5. **delete.test** (47 tests)
   - DELETE operations
   - Conditional deletes
   - Table clearing

6. **where.test** (135 tests)
   - WHERE clause functionality
   - Index usage
   - Complex conditions

7. **expr.test** (1 test)
   - Expression evaluation
   - Operators and functions

8. **table.test** (62 tests)
   - CREATE TABLE
   - Table constraints
   - Schema operations

**Execution:**
```bash
cd /home/gburd/ws/dbsql/test/scr050
for test in select1.test select2.test insert.test update.test delete.test where.test expr.test table.test; do
  echo "Running $test..."
  ./dbsql_tclsh tests/$test > results/${test%.test}.log 2>&1
done
```

**Exit Criteria:** 90%+ pass rate

**If Major Failures:** Investigate before proceeding to Phase 3.

## Phase 3: Ordering and Aggregation (30-45 minutes)

**Objective:** Validate ORDER BY, GROUP BY, and DISTINCT (critical for recent sorter implementation).

**Priority:** P1 (High - validates new sorter implementation)

**Test Files:**

9. **sort.test** (59 tests)
   - ORDER BY with various data types
   - ASC/DESC sorting
   - Multiple column sorts
   - **CRITICAL:** Tests new sorter opcodes

10. **select3.test** (33 tests)
    - GROUP BY operations
    - Aggregate functions (COUNT, SUM, AVG, MAX, MIN)
    - HAVING clauses

11. **select4.test** (52 tests)
    - DISTINCT keyword
    - Unique value selection
    - Combined with ORDER BY

12. **minmax.test** (38 tests)
    - MIN/MAX aggregate functions
    - Index optimization for min/max

**Execution:**
```bash
cd /home/gburd/ws/dbsql/test/scr050
for test in sort.test select3.test select4.test minmax.test; do
  echo "Running $test..."
  ./dbsql_tclsh tests/$test > results/${test%.test}.log 2>&1
done
```

**Exit Criteria:** 95%+ pass rate (ORDER BY must work perfectly)

**Special Focus:** ORDER BY tests validate the sorter opcodes (OP_SorterOpen, OP_SorterInsert, OP_SorterNext, etc.)

## Phase 4: Advanced Features (2-3 hours)

**Objective:** Test complex SQL features.

**Priority:** P1-P2 (Essential to Important)

### Joins (P1)

13. **join.test** (52 tests)
    - INNER JOIN
    - NATURAL JOIN
    - Multiple table joins

14. **join2.test** (7 tests)
    - Additional join scenarios
    - Cross joins

### Subqueries (P1)

15. **subselect.test** (21 tests)
    - Scalar subqueries
    - Subqueries in WHERE
    - Correlated subqueries

### Indexes (P1)

16. **index.test** (71 tests)
    - CREATE INDEX
    - Index usage in queries
    - Multi-column indexes

### Constraints (P1)

17. **unique.test** (30 tests)
    - UNIQUE constraints
    - Constraint violations

18. **notnull.test** (64 tests)
    - NOT NULL constraints
    - NULL value handling

19. **null.test** (31 tests)
    - NULL comparisons
    - NULL in expressions

### Transactions (P2)

20. **trans.test** (111 tests)
    - BEGIN/COMMIT/ROLLBACK
    - Transaction isolation
    - Savepoints

### Triggers (P2)

21. **trigger1.test** (44 tests)
    - CREATE/DROP TRIGGER
    - BEFORE/AFTER triggers

22. **trigger2.test** (34 tests)
    - Trigger execution
    - Multiple triggers

23. **trigger3.test** (15 tests)
    - Trigger edge cases

24. **trigger4.test** (15 tests)
    - Complex trigger scenarios

### Views (P2)

25. **view.test** (56 tests)
    - CREATE VIEW
    - View queries
    - Updatable views

**Execution:**
```bash
cd /home/gburd/ws/dbsql/test/scr050
for test in join.test join2.test subselect.test index.test unique.test notnull.test null.test trans.test trigger1.test trigger2.test trigger3.test trigger4.test view.test; do
  echo "Running $test..."
  ./dbsql_tclsh tests/$test > results/${test%.test}.log 2>&1
done
```

**Exit Criteria:** 80%+ pass rate

## Phase 5: Data Types and Functions (1-2 hours)

**Objective:** Validate data type handling and built-in functions.

**Priority:** P1-P2

26. **func.test** (66 tests)
    - Built-in SQL functions
    - String functions
    - Numeric functions

27. **printf.test** (12 tests)
    - printf-style formatting
    - String interpolation

28. **date.test** (varies)
    - Date/time functions
    - Date arithmetic

29. **in.test** (48 tests)
    - IN operator
    - Subquery in IN clause

30. **bind.test** (10 tests)
    - Parameter binding
    - Prepared statements

**Execution:**
```bash
cd /home/gburd/ws/dbsql/test/scr050
for test in func.test printf.test date.test in.test bind.test; do
  echo "Running $test..."
  ./dbsql_tclsh tests/$test > results/${test%.test}.log 2>&1
done
```

**Exit Criteria:** 85%+ pass rate

## Phase 6: System Features (1-2 hours)

**Objective:** Test database-level features and special cases.

**Priority:** P2-P3

31. **attach.test** (84 tests)
    - ATTACH DATABASE
    - Multi-database queries

32. **attach2.test** (15 tests)
    - Additional attach scenarios

33. **pragma.test** (28 tests)
    - PRAGMA statements
    - Database settings

34. **vacuum.test** (15 tests)
    - VACUUM operation
    - Database defragmentation

35. **temptable.test** (61 tests)
    - Temporary tables
    - Session-scoped tables

36. **memdb.test** (25 tests)
    - In-memory databases
    - Performance tests

37. **lock.test** (45 tests)
    - Locking behavior
    - Concurrency

38. **thread1.test** (23 tests)
    - Thread safety
    - Concurrent access

**Execution:**
```bash
cd /home/gburd/ws/dbsql/test/scr050
for test in attach.test attach2.test pragma.test vacuum.test temptable.test memdb.test lock.test thread1.test; do
  echo "Running $test..."
  ./dbsql_tclsh tests/$test > results/${test%.test}.log 2>&1
done
```

**Exit Criteria:** 70%+ pass rate

## Phase 7: Edge Cases and Special Tests (1-2 hours)

**Objective:** Test corner cases, error handling, and special scenarios.

**Priority:** P3

39. **misc1.test** (71 tests)
    - Miscellaneous SQL features

40. **misc2.test** (21 tests)
    - Additional edge cases

41. **misc3.test** (28 tests)
    - More corner cases

42. **intpkey.test** (71 tests)
    - Integer primary keys
    - ROWID behavior

43. **rowid.test** (122 tests)
    - ROWID column
    - Implicit rowid

44. **bigrow.test** (24 tests)
    - Large row handling
    - Size limits

45. **limit.test** (46 tests)
    - LIMIT clause
    - OFFSET

46. **copy.test** (25 tests)
    - COPY command
    - Bulk data loading

47. **conflict.test** (56 tests)
    - ON CONFLICT clauses
    - Conflict resolution

48. **quote.test** (10 tests)
    - SQL quoting
    - Identifier escaping

**Execution:**
```bash
cd /home/gburd/ws/dbsql/test/scr050
for test in misc1.test misc2.test misc3.test intpkey.test rowid.test bigrow.test limit.test copy.test conflict.test quote.test; do
  echo "Running $test..."
  ./dbsql_tclsh tests/$test > results/${test%.test}.log 2>&1
done
```

**Exit Criteria:** 60%+ pass rate

## Phase 8: API and Error Handling (1 hour)

**Objective:** Test C API and error conditions.

**Priority:** P2-P3

49. **capi2.test** (111 tests)
    - C API functions
    - API correctness

50. **tclsqlite.test** (9 tests)
    - TCL bindings
    - Scripting interface

51. **tableapi.test** (25 tests)
    - Table introspection API

52. **misuse.test** (24 tests)
    - API misuse detection
    - Error handling

53. **hook.test** (9 tests)
    - Callback hooks
    - Event notifications

54. **progress.test** (5 tests)
    - Progress callbacks
    - Long-running queries

55. **auth.test** (294 tests)
    - Authorization callbacks
    - Access control

**Execution:**
```bash
cd /home/gburd/ws/dbsql/test/scr050
for test in capi2.test tclsqlite.test tableapi.test misuse.test hook.test progress.test auth.test; do
  echo "Running $test..."
  ./dbsql_tclsh tests/$test > results/${test%.test}.log 2>&1
done
```

**Exit Criteria:** 70%+ pass rate

## Phase 9: Storage Layer (30-45 minutes)

**Objective:** Test pager and storage layer.

**Priority:** P2

56. **pager.test** (51 tests)
    - Pager functionality
    - Page management
    - Cache behavior

57. **main.test** (47 tests)
    - Core database operations
    - File format

**Execution:**
```bash
cd /home/gburd/ws/dbsql/test/scr050
for test in pager.test main.test; do
  echo "Running $test..."
  ./dbsql_tclsh tests/$test > results/${test%.test}.log 2>&1
done
```

**Exit Criteria:** 75%+ pass rate

## Phase 10: Full Test Suite (4-6 hours)

**Objective:** Run complete test suite including excluded tests.

**Method 1: Quick test (excludes malloc, memleak, misuse)**
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/quick.test | tee results/quick-test-full.log
```

**Method 2: All tests (includes everything)**
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/all.test | tee results/all-test-full.log
```

**Remaining Tests:**
58. **malloc.test** - Memory allocation testing
59. **memleak.test** - Memory leak detection
60. **select5.test** - Additional select tests
61. **select6.test** - More select scenarios
62. **insert2.test** - Additional insert tests
63. **fkey1.test** - Foreign key constraints

**Exit Criteria:** 80%+ overall pass rate

## Success Metrics

### By Priority Level

- **P0 (Core SQL):** 95%+ pass rate
- **P1 (Essential):** 85%+ pass rate
- **P2 (Important):** 70%+ pass rate
- **P3 (Edge cases):** 60%+ pass rate

### Critical Feature Validation

- **ORDER BY:** 100% pass rate (new sorter implementation)
- **Basic CRUD:** 95%+ pass rate
- **Joins:** 85%+ pass rate
- **Transactions:** 80%+ pass rate

### Overall Target

- **Total Tests:** 2,870
- **Minimum Pass:** 2,200 (77%)
- **Target Pass:** 2,400 (84%)
- **Stretch Goal:** 2,600 (91%)

## Failure Categorization

When tests fail, categorize by:

### 1. Opcode Issues
- Missing opcode implementations
- Incorrect opcode behavior
- Opcode initialization problems

### 2. Sorter Issues
- ORDER BY failures
- Sorting incorrect results
- Sorter opcode bugs

### 3. Memory Issues
- Segmentation faults
- Memory leaks
- Use-after-free

### 4. Data Type Issues
- Type coercion problems
- NULL handling
- Integer/float conversions

### 5. Parser Issues
- SQL syntax not recognized
- Parser crashes
- Incorrect parse trees

### 6. Pager Issues
- File I/O errors
- Page corruption
- Cache problems

### 7. API Issues
- C API incorrect behavior
- TCL binding problems
- Return value errors

## Test Result Tracking

For each phase, record:

1. **Summary Statistics**
   - Tests run
   - Tests passed
   - Tests failed
   - Tests skipped

2. **Failure Details**
   - Test name
   - Failure message
   - Category (see above)
   - Priority (P0/P1/P2/P3)

3. **Action Items**
   - Bugs to fix
   - Features to implement
   - Tests to skip (if justified)

## Continuous Testing

Once baseline established:

1. **Regression Testing**
   - Run Phase 1-3 after every fix
   - Run full suite daily
   - Track pass rate trends

2. **Automated Testing**
   - Set up CI/CD pipeline
   - Run quick.test on every commit
   - Run all.test nightly

3. **Performance Testing**
   - Track execution time
   - Identify slow tests
   - Measure improvement over time

## Known Issues and Workarounds

### Current Blockers
- VDBE opcode 0 initialization error (being fixed)

### Expected Issues
- Some tests may rely on SQLite-specific behavior not in DBSQL
- Foreign key support may be incomplete (fkey1.test)
- Authorization may be stubbed (auth.test)
- Threading tests may have platform dependencies

### Test Exclusions
The test framework excludes by default:
- `all.test` (test runner itself)
- `quick.test` (test runner itself)
- `malloc.test` (leaks memory intentionally)
- `memleak.test` (memory debugging)
- `misuse.test` (API misuse detection)

## Next Steps After Testing

1. **Bug Triage**
   - Prioritize failures by impact
   - Group related failures
   - Assign ownership

2. **Fix Verification**
   - Re-run failed tests
   - Run related tests
   - Check for regressions

3. **Documentation**
   - Document known issues
   - Update test plan
   - Track progress

4. **Performance Optimization**
   - Profile slow queries
   - Optimize hot paths
   - Tune cache sizes

## Appendix A: Quick Reference

### Running a Single Test
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/TESTNAME.test
```

### Running Quick Suite
```bash
cd /home/gburd/ws/dbsql/test/scr050
make test
```

### Viewing Results
```bash
cd /home/gburd/ws/dbsql/test/scr050/results
ls -ltr  # Most recent first
```

### Common Test Patterns

**do_test:** Basic test with expected result
```tcl
do_test test-1.1 {
  execsql {SELECT 1}
} {1}
```

**execsql:** Execute SQL, return result
```tcl
set result [execsql {SELECT * FROM t1}]
```

**catchsql:** Execute SQL, catch errors
```tcl
set v [catchsql {SELECT * FROM no_such_table}]
# Returns: {1 {error message}}
```

## Appendix B: Test File Categories

### Core SQL (P0)
select1, select2, insert, update, delete, where, expr, table

### Sorting/Grouping (P1)
sort, select3, select4, minmax

### Joins (P1)
join, join2

### Subqueries (P1)
subselect

### Indexes (P1)
index

### Constraints (P1)
unique, notnull, null

### Transactions (P2)
trans

### Triggers (P2)
trigger1, trigger2, trigger3, trigger4

### Views (P2)
view

### Functions (P1)
func, printf, date, in, bind

### System (P2)
attach, attach2, pragma, vacuum, temptable, memdb, lock, thread1

### Edge Cases (P3)
misc1, misc2, misc3, intpkey, rowid, bigrow, limit, copy, conflict, quote

### API (P2-P3)
capi2, tclsqlite, tableapi, misuse, hook, progress, auth

### Storage (P2)
pager, main

### Special (P3)
malloc, memleak, select5, select6, insert2, fkey1
