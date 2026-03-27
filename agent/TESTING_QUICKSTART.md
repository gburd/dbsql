# DBSQL Testing Quick Start Guide

**Date:** 2026-03-24
**Status:** Ready to test once VDBE opcode issue is resolved

## Quick Start (5 minutes)

### Step 1: Run Smoke Tests
```bash
cd /home/gburd/ws/dbsql
./test/smoke-test.sh
```

This runs 7 basic tests to verify core functionality.

### Step 2: Run Core SQL Tests
```bash
cd /home/gburd/ws/dbsql
./test/run-test-phase.sh 2
```

This runs 558 tests covering basic SQL operations.

### Step 3: Run Full Quick Test Suite
```bash
cd /home/gburd/ws/dbsql
./test/run-test-phase.sh quick
```

This runs most tests (excludes malloc, memleak tests).

## Available Commands

### Test by Phase
```bash
./test/run-test-phase.sh 1     # Smoke tests
./test/run-test-phase.sh 2     # Core SQL
./test/run-test-phase.sh 3     # ORDER BY & sorting (critical!)
./test/run-test-phase.sh 4     # Joins, triggers, views
./test/run-test-phase.sh all   # All phases 1-9
```

### Run Individual Test Files
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/select1.test
./dbsql_tclsh tests/sort.test       # ORDER BY tests
./dbsql_tclsh tests/join.test
```

### Run Test Suites
```bash
cd /home/gburd/ws/dbsql/test/scr050
./dbsql_tclsh tests/quick.test      # Quick suite (~2 hours)
./dbsql_tclsh tests/all.test        # Full suite (~4-6 hours)
```

## Understanding Results

### Test Output Format
```
testname-1.1... Ok
testname-1.2... FAILED
  Expected: {1}
       Got: {0}

N errors out of M tests
```

### Success Criteria
- **0 errors**: Perfect!
- **< 5% errors**: Good (for P1-P2 tests)
- **< 10% errors**: Acceptable (for P3 tests)
- **> 20% errors**: Major issues, investigate

## Critical Tests for ORDER BY

The sorter implementation MUST pass these:

```bash
cd /home/gburd/ws/dbsql/test/scr050

# Primary ORDER BY test
./dbsql_tclsh tests/sort.test

# Secondary tests with ORDER BY
./dbsql_tclsh tests/select1.test
./dbsql_tclsh tests/select3.test
./dbsql_tclsh tests/select4.test
```

## Test Results Location

All test logs are saved to:
```
/home/gburd/ws/dbsql/test/scr050/results/
```

View recent results:
```bash
cd /home/gburd/ws/dbsql/test/scr050/results
ls -ltr | tail -10
```

## Current Blocker

**VDBE Opcode Initialization Error**
- Error: "unknown opcode 0"
- Being fixed by Opcode Debug Agent
- ALL tests blocked until fixed

## After Fix: Recommended Testing Sequence

1. **Smoke Tests (15 min)**
   ```bash
   ./test/smoke-test.sh
   ```
   Exit criteria: All pass

2. **Core SQL (1 hour)**
   ```bash
   ./test/run-test-phase.sh 2
   ```
   Exit criteria: 90%+ pass rate

3. **ORDER BY Tests (30 min)**
   ```bash
   ./test/run-test-phase.sh 3
   ```
   Exit criteria: 95%+ pass rate (critical!)

4. **Quick Test Suite (2 hours)**
   ```bash
   ./test/run-test-phase.sh quick
   ```
   Exit criteria: 80%+ overall pass rate

5. **Full Investigation**
   - Analyze failures
   - Fix bugs
   - Re-run failed tests

## Documentation

Comprehensive documentation available:

- **TEST_EXECUTION_PLAN.md** - Detailed test plan (all phases)
- **TEST_INFRASTRUCTURE_ANALYSIS.md** - Test framework deep dive
- **test-categories.txt** - Test file categorization
- **test-results-template.md** - Template for recording results

## Common Issues

### Test Runner Not Found
```bash
cd /home/gburd/ws/dbsql/test/scr050
make clean
make
```

### Permission Denied
```bash
chmod +x /home/gburd/ws/dbsql/test/smoke-test.sh
chmod +x /home/gburd/ws/dbsql/test/run-test-phase.sh
```

### Tests Create Files
Tests create temporary files in `/home/gburd/ws/dbsql/test/scr050/`:
- test.db, test2.db, etc.
- test.db-journal
- data.txt, data1.txt, etc.

Cleanup:
```bash
cd /home/gburd/ws/dbsql/test/scr050
make clean
```

## Next Steps

1. **Wait for VDBE opcode fix**
   - Being resolved by another agent
   - Blocks all testing

2. **Run smoke tests immediately after fix**
   - Verify basic functionality
   - Takes only 15 minutes

3. **Progressive testing**
   - Phase 2: Core SQL
   - Phase 3: ORDER BY (critical)
   - Phase 10: Quick suite

4. **Document failures**
   - Use test-results-template.md
   - Categorize by type
   - Prioritize fixes

5. **Regression testing**
   - Re-run after each fix
   - Track pass rate trends
   - Prevent breakage

## Test Statistics

- **Total Test Files:** 60
- **Total Test Cases:** 2,870
- **Test Framework:** TCL
- **Test Runner:** dbsql_tclsh
- **Estimated Time (quick):** 2 hours
- **Estimated Time (full):** 4-6 hours

## Priority Summary

- **P0 (Core SQL):** 558 tests - Must pass 95%+
- **P1 (Essential):** 667 tests - Must pass 85%+
- **P2 (Important):** 855 tests - Must pass 75%+
- **P3 (Nice to have):** 790 tests - Must pass 60%+

## Contact

For questions or issues with testing, refer to the comprehensive documentation or check the test infrastructure code.

---

**Remember:** Don't run tests until the VDBE opcode issue is fixed!
The smoke tests will tell you if the fix is working.
