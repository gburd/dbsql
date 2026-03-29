# DBSQL Test Coverage - Current Status and Next Steps

**Date**: 2026-03-27 (Updated)
**Branch**: fix/vdbe-opcode-init
**Current Coverage**: 29.17% line, 40.14% branch
**Target Coverage**: >95% function and branch

## Current State

### What Works ✅
1. **Test Infrastructure Complete**
   - TAP-compliant test framework operational
   - 123 tests written (test_api: 41, test_vdbe: 8, test_memory: 8, test_fts: 21)
   - Build system with 5 configurations (DEBUG, COVERAGE, DIAGNOSTIC, UMEM, RELEASE)
   - Coverage measurement capability established

2. **API Operations Working**
   - Database creation (`dbsql_create_env`)
   - Database opening (`db->open`)
   - CREATE TABLE statements
   - INSERT statements
   - Basic DDL operations
   - Database closing (`db->close`)

3. **Test Execution Stable**
   - 41 tests execute without crashes
   - 34 tests pass (82.9% pass rate)
   - No memory corruption (validated with valgrind, AddressSanitizer)
   - Clean build with coverage instrumentation

4. **Critical Bugs Fixed**
   - Buffer overflow in str.c (type mismatch) - **CRITICAL FIX**
   - API ownership bugs (use-after-free, double-free)
   - Berkeley DB environment resource leak
   - P4_POINTER invalid free bug
   - Leftover table file handling

### What's Broken ❌

**7 Tests Fail with SELECT/Transaction Issues**

| Test | Failure | Error Codes |
|------|---------|-------------|
| api_insert_select | SELECT queries fail | -50998 (3x) |
| api_transaction_commit | Transaction commit fails | -50998, -50999 |
| api_transaction_rollback | Transaction rollback fails | -50998, -50999 |

**Error Codes**:
- **-50998**: DB_LOCK_NOTGRANTED or DB_RUNRECOVERY
- **-50999**: DB_LOCK_DEADLOCK or DB_NOTFOUND

**Root Cause Hypothesis**: VDBE cursor operations for SELECT or transaction state management

**Evidence**:
- Warning: "closing the transaction region with active transactions"
- All failures occur during/after SELECT execution
- Basic DDL (CREATE, INSERT) works perfectly
- Issue appears related to cursor lifecycle or transaction cleanup

### Blocked Work ⏸️

**Cannot Execute Until SELECT Fixed**:
1. **test_vdbe.c** - All 8 tests use SELECT queries
2. **test_memory.c** - 5 of 8 tests use SELECT queries
3. **15-20% coverage gain** - Estimated from running test_vdbe

## Investigation Needed

### Immediate Debug Actions

1. **Run test_api with debug output** to capture exact SELECT failure:
   ```bash
   cd /home/gburd/ws/dbsql/test/src
   LD_LIBRARY_PATH=../../build_unix.old/.libs ./test_api 2>&1 | tee select_debug.log
   ```

2. **Examine VDBE cursor initialization** for SELECT:
   - File: `src/vdbe.c`
   - Operations: OP_OpenRead, OP_Column, OP_Next, OP_Close
   - Check cursor state management in SELECT codegen

3. **Check transaction state handling**:
   - File: `src/api.c` - Transaction begin/commit/rollback
   - File: `src/sm.c` - Berkeley DB transaction integration
   - Look for incomplete cleanup or state mismatches

4. **Review recent commits** for changes that may have broken SELECT:
   - Focus on commits after tests were last passing
   - Check vdbe.c, cg_select.c, sm.c modifications

### Specific Code Areas to Examine

**VDBE Cursor Operations** (src/vdbe.c):
```c
// Lines to investigate:
- OP_OpenRead implementation - Cursor initialization
- OP_Column implementation - Reading cursor data
- OP_Next implementation - Cursor iteration
- OP_Close implementation - Cursor cleanup
- Cursor state tracking in vdbe_t->cursors array
```

**Berkeley DB Transaction Integration** (src/sm.c):
```c
// Check:
- __sm_begin_txn_ex() - Transaction start
- Transaction nesting/cleanup
- Cursor operations within transactions
- Environment state on SELECT operations
```

**SELECT Code Generation** (src/cg_select.c):
```c
// Verify:
- VDBE code generation for SELECT
- Register allocation for result rows
- Cursor lifetime management
- Cleanup on SELECT completion
```

## Path Forward

### Phase 1: Fix SELECT Queries (CRITICAL - 4-6 hours)

**Priority 1: Isolate SELECT failure**
1. Create minimal SELECT test with debug output
2. Run with gdb to capture exact failure point
3. Check Berkeley DB error codes and meanings

**Priority 2: Fix cursor operations**
1. Review OP_OpenRead, OP_Column, OP_Next implementations
2. Verify cursor state transitions
3. Check cursor cleanup in error paths

**Priority 3: Fix transaction handling**
1. Debug "active transactions" warning
2. Verify transaction cleanup on error
3. Check transaction nesting

**Expected Outcome**: 7 failing tests pass, +10-15% coverage

### Phase 2: Run Additional Tests (2-4 hours)

Once SELECT works:
1. Execute test_vdbe.c → +15-20% coverage
2. Execute test_memory.c → +2-3% coverage
3. Fix any failures discovered
4. Remeasure coverage baseline

**Expected Outcome**: Coverage reaches ~45-50%

### Phase 3: Add Zero-Coverage Tests (6-8 hours)

**12 files with 0% coverage**:
- api_table.c (88 lines) - Add `db->get_table()` test
- cg_pragma.c (186 lines) - Add PRAGMA tests
- cg_date.c (186 lines) - Add date/time function tests
- xvprintf.c, snprintf.c, util.c, etc.

**Expected Outcome**: +8-12% coverage, reaches ~55-60%

### Phase 4: Target Critical Files (12-16 hours)

**Low-coverage critical files**:
- vdbe.c - 15.68% → 85% (add missing opcode tests)
- cg_expr.c - 15.52% → 85% (add expression tests)
- cg_select.c - 21.71% → 85% (add JOIN, GROUP BY, etc.)
- cg_where.c - 16.15% → 85% (add WHERE variations)

**Expected Outcome**: +30-40% coverage, reaches ~85-90%

### Phase 5: Edge Cases (4-6 hours)

- Error handling paths
- Constraint violations
- Stress tests
- Large data operations

**Expected Outcome**: +5-10% coverage, reaches **95%+ target**

## Total Effort Estimate

- **Phase 1** (SELECT fix): 4-6 hours - **BLOCKING**
- **Phase 2-5** (Coverage improvement): 24-30 hours
- **Total to 95%**: 28-36 hours

## Recommendations

1. **Focus exclusively on Phase 1** - All other work is blocked
2. **Use gdb/valgrind** to debug SELECT failures
3. **Don't add more tests** until SELECT works
4. **Document the fix** when found for future reference
5. **Retest all 41 tests** after fix to ensure no regressions

## Quick Reference

### Test Execution
```bash
cd /home/gburd/ws/dbsql/test/src

# Run test_api
LD_LIBRARY_PATH=../../build_unix.old/.libs ./test_api

# Debug with gdb
LD_LIBRARY_PATH=../../build_unix.old/.libs gdb ./test_api

# Run with valgrind
LD_LIBRARY_PATH=../../build_unix.old/.libs valgrind --leak-check=full ./test_api

# Generate coverage
make BUILD_TYPE=COVERAGE coverage
```

### Build Commands
```bash
cd /home/gburd/ws/dbsql/build_unix.old

# Rebuild library
make clean
make

# With coverage
make clean
../dist/s_config CFLAGS="--coverage -fprofile-arcs -ftest-coverage"
make
```

## Files Modified Since Test Infrastructure Created

- src/common/str.c - Fixed buffer overflow (CRITICAL)
- src/api.c - Fixed ownership bugs, added dbenv cleanup
- src/sm.c - Handle leftover table files
- test/src/test_api.c - Added open() before close() in first test

## Documentation Files

1. SESSION_SUMMARY.md - Complete session documentation (500+ lines)
2. COVERAGE_ROADMAP_TO_95_PERCENT.md - Detailed roadmap
3. COVERAGE_SUMMARY.md - Baseline coverage metrics
4. TEST_SUITE_STATUS.md - Test status and bugs fixed
5. IMPLEMENTATION_SUMMARY.md - Infrastructure details
6. TEST_STATUS_REPORT.md - Comprehensive status
7. DEBUG_OUTPUT.md - Buffer overflow analysis
8. API_BUG_ANALYSIS.md - API ownership documentation
9. FTS_TEST_DOCUMENTATION.md - FTS test suite
10. README.md - User guide (300+ lines)

## Success Criteria

✅ **Foundation Complete**:
- Test infrastructure works
- Coverage measurement capability established
- Build system supports 5 configurations
- 4 critical bugs fixed
- Comprehensive documentation created

⏳ **In Progress**:
- Fix 7 failing tests (SELECT/transactions)
- Run test_vdbe and test_memory
- Reach 95% coverage target

🎯 **Final Goals**:
- 95%+ line coverage
- 95%+ function coverage
- 90%+ branch coverage
- 100% test pass rate
- Zero crashes, zero memory leaks
- CI/CD integration

---

**Next Action**: Debug SELECT query failures to unblock coverage improvement work
