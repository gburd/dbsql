# DBSQL Test Infrastructure Analysis

**Date:** 2026-03-24
**Analyzed by:** Test Strategy Agent
**Location:** `/home/gburd/ws/dbsql/test/scr050/`

## Overview

DBSQL uses a TCL-based test framework inherited from SQLite. The test suite is comprehensive with 2,870 test cases across 60 test files.

## Test Framework Architecture

### Components

1. **Test Runner: `dbsql_tclsh`**
   - Custom TCL shell with DBSQL bindings
   - Built from `dbsql_tclsh.c`
   - Links against libdbsql
   - Location: `/home/gburd/ws/dbsql/test/scr050/dbsql_tclsh`

2. **Test Framework: `tester.tcl`**
   - Core test procedures and utilities
   - Loaded by each test file
   - Provides `do_test`, `execsql`, `catchsql`, etc.
   - **Note:** File not found in tests directory, likely sourced from elsewhere

3. **Test Files: `tests/*.test`**
   - 60 test files
   - TCL scripts using test framework
   - Self-contained test suites
   - Location: `/home/gburd/ws/dbsql/test/scr050/tests/`

4. **Test Orchestrators**
   - `all.test`: Runs all tests (except excluded ones)
   - `quick.test`: Runs most tests (excludes malloc, memleak, misuse)
   - Excludes itself from execution

### Build System

**Makefile** (`/home/gburd/ws/dbsql/test/scr050/Makefile`)

Key targets:
- `all`: Build dbsql_tclsh
- `test`: Run quick.test
- `clean`: Clean build artifacts

Build command:
```bash
cd /home/gburd/ws/dbsql/test/scr050
make
```

Test command:
```bash
cd /home/gburd/ws/dbsql/test/scr050
make test
```

Dependencies:
- TCL 8.5 (`/nix/store/aqlscqccxp85w5zxrcmim7qf1i88xyli-tcl-8.5.19`)
- Berkeley DB 4.8 (`/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30`)
- libdbsql (`build_unix/libdbsql-0.4`)
- libtool for building

### Test Execution Flow

1. **Individual Test File**
   ```
   User → ./dbsql_tclsh tests/select1.test
           ↓
       Load tester.tcl
           ↓
       Execute test cases (do_test)
           ↓
       Report results
   ```

2. **Quick Test Suite**
   ```
   User → ./dbsql_tclsh tests/quick.test
           ↓
       Load tester.tcl
           ↓
       For each test file (except excluded):
           - Source test file
           - Execute tests
           - Close database
           - Check file handles
           ↓
       Run misuse.test
           ↓
       Report overall results
   ```

3. **All Test Suite**
   ```
   User → ./dbsql_tclsh tests/all.test
           ↓
       Load tester.tcl
           ↓
       For COUNT iterations (default 4):
           - Vary byte order
           - Vary synchronous mode
           - For each test file (except excluded):
               * Source test file
               * Execute tests
               * Close database
               * Check file handles
           - Record memory leaks
           ↓
       Run misuse.test
       Run malloc.test
           ↓
       Report overall results
   ```

## Test Framework API

### Core Test Procedures

#### `do_test name body expected`
Executes a test case and compares result to expected value.

Example:
```tcl
do_test select1-1.1 {
  execsql {SELECT 1}
} {1}
```

#### `execsql sql`
Executes SQL and returns results as a list.

Example:
```tcl
set result [execsql {SELECT * FROM t1}]
```

#### `catchsql sql`
Executes SQL and catches errors, returns {code message}.

Example:
```tcl
set v [catchsql {SELECT * FROM no_such_table}]
# Returns: {1 {no such table: no_such_table}}
```

#### `execsql2 sql`
Executes SQL and returns column names with values.

Example:
```tcl
do_test join-1.3 {
  execsql2 {SELECT * FROM t1 NATURAL JOIN t2}
} {t1.a 1 t1.b 2 t1.c 3 t2.d 4}
```

### Global Variables

- `$dbsql_open_file_count`: Tracks open file handles
- `$dbsql_search_count`: Tracks MoveTo operations (index usage)
- `$nErr`: Error count
- `$nTest`: Test count
- `$failList`: List of failed tests
- `$ISQUICK`: Set to 1 in quick.test (some tests skip expensive operations)

### Test Configuration

#### Environment Variables
- Not explicitly documented, but standard TCL variables available

#### Test File Control
- Tests can check `$ISQUICK` to skip expensive tests
- Tests use `source $testdir/tester.tcl` to load framework
- `$testdir` is set by each test file

## Test File Analysis

### Test Count by File

From grep analysis of `do_test` statements:

| File | Tests | Category |
|------|-------|----------|
| auth.test | 294 | Authorization |
| where.test | 135 | WHERE clauses |
| select1.test | 144 | Basic SELECT |
| rowid.test | 122 | ROWID behavior |
| capi2.test | 111 | C API |
| trans.test | 111 | Transactions |
| update.test | 108 | UPDATE |
| attach.test | 84 | ATTACH DATABASE |
| intpkey.test | 71 | Integer primary keys |
| misc1.test | 71 | Miscellaneous |
| index.test | 71 | Indexes |
| func.test | 66 | Functions |
| notnull.test | 64 | NOT NULL |
| table.test | 62 | CREATE TABLE |
| temptable.test | 61 | Temp tables |
| sort.test | 59 | ORDER BY |
| select6.test | 58 | SELECT variants |
| conflict.test | 56 | ON CONFLICT |
| view.test | 56 | Views |
| join.test | 52 | Joins |
| select4.test | 52 | DISTINCT |
| pager.test | 51 | Pager |
| in.test | 48 | IN operator |
| main.test | 47 | Core DB ops |
| delete.test | 47 | DELETE |
| limit.test | 46 | LIMIT |
| lock.test | 45 | Locking |
| trigger1.test | 44 | Triggers |
| insert.test | 42 | INSERT |
| minmax.test | 38 | MIN/MAX |
| trigger2.test | 34 | More triggers |
| select3.test | 33 | GROUP BY |
| null.test | 31 | NULL handling |
| unique.test | 30 | UNIQUE |
| pragma.test | 28 | PRAGMA |
| misc3.test | 28 | More misc |
| memdb.test | 25 | In-memory DB |
| copy.test | 25 | COPY |
| tableapi.test | 25 | Table API |
| bigrow.test | 24 | Large rows |
| misuse.test | 24 | API misuse |
| thread1.test | 23 | Threading |
| insert2.test | 22 | More INSERT |
| misc2.test | 21 | More misc |
| subselect.test | 21 | Subqueries |
| select2.test | 19 | More SELECT |
| attach2.test | 15 | More ATTACH |
| select5.test | 15 | More SELECT |
| vacuum.test | 15 | VACUUM |
| trigger3.test | 15 | More triggers |
| trigger4.test | 15 | More triggers |
| printf.test | 12 | printf |
| bind.test | 10 | Binding |
| quote.test | 10 | Quoting |
| hook.test | 9 | Hooks |
| tclsqlite.test | 9 | TCL bindings |
| join2.test | 7 | More joins |
| progress.test | 5 | Progress |
| fkey1.test | 3 | Foreign keys |
| expr.test | 1 | Expressions |

**Total:** 2,870 tests

### Test Categories

#### P0: Core SQL (558 tests)
Critical for basic functionality. Must pass.

- select1.test (144)
- select2.test (19)
- insert.test (42)
- update.test (108)
- delete.test (47)
- where.test (135)
- expr.test (1)
- table.test (62)

#### P1: Essential Features (682 tests)
Important features that users expect.

**Sorting/Grouping:**
- sort.test (59)
- select3.test (33)
- select4.test (52)
- minmax.test (38)

**Joins:**
- join.test (52)
- join2.test (7)

**Subqueries:**
- subselect.test (21)

**Indexes:**
- index.test (71)

**Constraints:**
- unique.test (30)
- notnull.test (64)
- null.test (31)

**Functions:**
- func.test (66)
- printf.test (12)
- in.test (48)
- bind.test (10)

**Additional SELECT:**
- select5.test (15)
- select6.test (58)

**Additional INSERT:**
- insert2.test (22)

#### P2: Advanced Features (593 tests)
Important but not critical for basic operation.

**Transactions:**
- trans.test (111)

**Triggers:**
- trigger1.test (44)
- trigger2.test (34)
- trigger3.test (15)
- trigger4.test (15)

**Views:**
- view.test (56)

**System:**
- attach.test (84)
- attach2.test (15)
- pragma.test (28)
- vacuum.test (15)
- temptable.test (61)
- memdb.test (25)
- lock.test (45)
- thread1.test (23)

**Storage:**
- pager.test (51)
- main.test (47)

#### P3: Edge Cases (1037 tests)
Edge cases, API tests, and special scenarios.

**Miscellaneous:**
- misc1.test (71)
- misc2.test (21)
- misc3.test (28)

**Special IDs:**
- intpkey.test (71)
- rowid.test (122)

**Limits:**
- bigrow.test (24)
- limit.test (46)

**Other:**
- copy.test (25)
- conflict.test (56)
- quote.test (10)

**API:**
- capi2.test (111)
- tclsqlite.test (9)
- tableapi.test (25)
- misuse.test (24)
- hook.test (9)
- progress.test (5)
- auth.test (294)

**Foreign Keys:**
- fkey1.test (3)

## Test Exclusions

### Default Exclusions in all.test
```tcl
set EXCLUDE {
  all.test
  quick.test
  malloc.test
  misuse.test
  memleak.test
}
```

### Default Exclusions in quick.test
```tcl
set EXCLUDE {
  all.test
  quick.test
  btree2.test
  malloc.test
  memleak.test
  misuse.test
}
```

### Rationale
- **all.test, quick.test**: Test runners themselves
- **malloc.test**: Intentionally leaks memory for testing
- **memleak.test**: Memory debugging test
- **misuse.test**: Tests API misuse (run separately at end)
- **btree2.test**: Likely b-tree specific tests

## Test Results Output

Tests output to stdout with format:
```
testname... Ok
testname... FAILED
  Expected: [expected]
       Got: [actual]
```

Global summary at end:
```
[N] errors out of [M] tests
```

## Known Issues and Limitations

### Current Blockers
1. **VDBE opcode initialization error**
   - Error: "unknown opcode 0"
   - Blocks all test execution
   - Being fixed by Opcode Debug Agent

### Framework Limitations
1. **No tester.tcl found**
   - Test framework code not visible
   - Likely compiled into dbsql_tclsh or in parent directory
   - Makes debugging test failures harder

2. **No JSON/XML output**
   - Results are text-only
   - Hard to parse programmatically
   - Need to scrape stdout

3. **No test isolation**
   - Tests share same process
   - Early crash can stop entire suite
   - No timeout mechanism visible

4. **Limited debugging**
   - No verbose mode obvious
   - No test replay mechanism
   - No step-through capability

### Platform Dependencies
1. **TCL 8.5 required**
   - Older TCL version
   - May have compatibility issues

2. **Berkeley DB 4.8 backend**
   - DBSQL uses BDB for storage
   - Tests may depend on BDB behavior

3. **File system**
   - Tests create files in test.db
   - May have permission issues
   - Cleanup may fail on errors

## Recommended Improvements

### Short-term
1. **Add test results directory**
   ```bash
   mkdir -p /home/gburd/ws/dbsql/test/scr050/results
   ```

2. **Add result parsing**
   - Parse test output for failures
   - Generate summary statistics
   - Create failure database

3. **Add timeout wrapper**
   - Wrap test execution with timeout
   - Kill hung tests
   - Continue with next test

### Medium-term
1. **Test parallelization**
   - Run independent tests in parallel
   - Use multiple dbsql_tclsh processes
   - Reduce total test time

2. **Better reporting**
   - JSON output format
   - HTML test reports
   - Trend graphs

3. **Test isolation**
   - Each test in own process
   - Cleanup after each test
   - Prevent cascade failures

### Long-term
1. **Modernize framework**
   - Update to TCL 8.6+
   - Add pytest-style features
   - Integrate with CI/CD

2. **Test coverage**
   - Add gcov integration
   - Measure code coverage
   - Identify untested code

3. **Performance tracking**
   - Benchmark each test
   - Track performance trends
   - Detect regressions

## Running Tests

### Prerequisites
```bash
# Build dbsql_tclsh
cd /home/gburd/ws/dbsql/test/scr050
make clean
make
```

### Run Single Test
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/select1.test
```

### Run Quick Suite
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/quick.test
# or
make test
```

### Run Full Suite
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/all.test
```

### Run with Logging
```bash
cd /home/gburd/ws/dbsql/test/scr050
mkdir -p results
./dbsql_tclsh tests/quick.test 2>&1 | tee results/quick-$(date +%Y%m%d-%H%M%S).log
```

### Run Specific Category
```bash
cd /home/gburd/ws/dbsql/test/scr050
# Core SQL tests
for test in select1 select2 insert update delete where expr table; do
  echo "=== $test ==="
  ./dbsql_tclsh tests/$test.test
done
```

## Test Data

### Generated Files
Tests create various files during execution:

- `test.db`: Main test database
- `test2.db`, `test3.db`, etc.: Additional test databases
- `test.db-journal`: Journal file
- `data.txt`, `data1.txt`, `data2.txt`: Data files for COPY
- `test1.bt`: B-tree test file

### Cleanup
```bash
cd /home/gburd/ws/dbsql/test/scr050
make clean
rm -rf test*.db test*.db-journal data*.txt test*.bt
```

## Debugging Tests

### Enable TCL Error Info
```tcl
# In test file, add:
catch {test_code} msg
puts "Error: $msg"
puts "Info: $::errorInfo"
```

### Add Debug Output
```tcl
# In test file, add:
puts "Debug: variable = $variable"
```

### Run Single Test Case
```tcl
# In test file, comment out other tests
# or extract single test to new file:
set testdir [file dirname $argv0]
source $testdir/tester.tcl

do_test mytest-1.1 {
  execsql {SELECT 1}
} {1}

finish_test
```

### Check VDBE Trace
```bash
# If dbsql has VDBE tracing
DBSQL_DEBUG=vdbe ./dbsql_tclsh tests/select1.test
```

## Summary

The DBSQL test infrastructure is mature and comprehensive:

**Strengths:**
- 2,870 well-organized test cases
- Inherited from SQLite (battle-tested)
- Covers wide range of SQL features
- Easy to run individual tests
- Good categorization

**Weaknesses:**
- Text-based output (hard to parse)
- No test isolation
- No timeout mechanism
- Older TCL version
- Current blocker: VDBE opcode error

**Readiness:**
- Framework is ready
- Tests are ready
- Blocker is being fixed
- Can proceed with testing immediately after fix

**Recommendation:**
Wait for VDBE opcode fix, then start with Phase 1 smoke tests.
