# Roadmap to 95% Code Coverage

**Current Status**: 29.17% line coverage, 40.14% branch coverage
**Target**: >95% function and branch coverage
**Gap**: Need 65.83% more line coverage

## Phase 1: Fix Existing Tests (Est. +10-15% coverage)

### Priority 1: Fix 7 Failing API Tests
**Current Impact**: 7/41 tests fail, blocking SELECT and transaction coverage

**Failing Tests**:
1. `api_insert_select` - SELECT queries fail with -50998
2. `api_transaction_commit` - Transaction commit fails
3. `api_transaction_rollback` - Transaction rollback fails

**Root Cause**: VDBE cursor operations or transaction state management

**Fix Steps**:
1. Debug VDBE cursor initialization in SELECT operations
2. Fix transaction region cleanup ("closing with active transactions")
3. Investigate Berkeley DB error codes -50998, -50999
4. Add better error reporting to identify exact failure points

**Expected Coverage Gain**: +5-8% (SELECT codegen, cursor operations, transaction handling)

### Priority 2: Run test_vdbe Suite
**Current Status**: Not executed yet
**Contains**: 8 VDBE opcode tests (arithmetic, comparisons, strings, table scans, WHERE, aggregates, ORDER BY)

**Fix Steps**:
1. Recompile test_vdbe with coverage library
2. Run and verify results
3. Fix any failures similar to test_api

**Expected Coverage Gain**: +3-5% (additional VDBE opcodes, sorter operations)

### Priority 3: Run test_memory Suite
**Current Status**: Ran earlier with 56.8% pass rate
**Contains**: 8 memory tests including concurrent access

**Fix Steps**:
1. Recompile test_memory with coverage library
2. Run and analyze failures
3. Fix any memory-related issues

**Expected Coverage Gain**: +2-3% (memory allocation paths, concurrency)

## Phase 2: Add Tests for Zero-Coverage Files (Est. +8-12% coverage)

### 12 Files with 0% Coverage

| File | Lines | Purpose | Test Strategy |
|------|-------|---------|---------------|
| api_table.c | 88 | Table operations | Add GET_TABLE tests |
| cg_pragma.c | 186 | PRAGMA statements | Add PRAGMA tests |
| cg_copy.c | 61 | COPY operations | Add COPY tests |
| xvprintf.c | 359 | Printf implementation | Add formatted output tests |
| util.c | 35 | Utility functions | Call from other tests |
| cg_date.c | 186 | Date/time functions | Add date/time tests |
| cg_vacuum.c | 59 | VACUUM operation | Add VACUUM test |
| cg_auth.c | 92 | Authorization | Add auth callback tests |
| snprintf.c | 162 | String formatting | Add snprintf tests |
| sql_parser.c | ~1500 | SQL parser | (Already covered by other tests) |

**Priority Order**:
1. **High Impact**: api_table.c, cg_pragma.c, cg_date.c (frequently used)
2. **Medium Impact**: xvprintf.c, snprintf.c (utility functions)
3. **Low Impact**: cg_copy.c, cg_vacuum.c, cg_auth.c (rare operations)

**Test Plan**:
```c
// Add to test_api.c or new test file
TEST(api_get_table) {
    // Tests api_table.c
    char **result;
    int rows, cols;
    rc = db->get_table(db, "SELECT * FROM test", &result, &rows, &cols, &errmsg);
    db->free_table(result);
}

TEST(api_pragma) {
    // Tests cg_pragma.c
    rc = db->exec(db, "PRAGMA table_info(test)", NULL, NULL, &errmsg);
}

TEST(api_date_functions) {
    // Tests cg_date.c
    rc = db->exec(db, "SELECT date('now')", NULL, NULL, &errmsg);
    rc = db->exec(db, "SELECT datetime('now')", NULL, NULL, &errmsg);
}
```

**Expected Coverage Gain**: +8-12% (zero-coverage files)

## Phase 3: Target Low-Coverage Critical Files (Est. +30-40% coverage)

### Critical Files Needing Attention

| File | Current | Target | Gap | Strategy |
|------|---------|--------|-----|----------|
| **vdbe.c** | 15.68% | 85%+ | 69% | Add opcode tests |
| **cg_expr.c** | 15.52% | 85%+ | 69% | Add expression tests |
| **cg_select.c** | 21.71% | 85%+ | 63% | Fix SELECT tests |
| **cg_where.c** | 16.15% | 85%+ | 69% | Add WHERE tests |
| **cg_insert.c** | 32.91% | 85%+ | 52% | Add INSERT tests |

### vdbe.c Coverage Strategy (1,798 lines - CORE)

**Currently Covered Opcodes** (from passing tests):
- OP_Init, OP_Goto, OP_Transaction, OP_VerifyCookie
- OP_TableLock, OP_OpenWrite, OP_NewRowid
- OP_MakeRecord, OP_Insert, OP_Halt

**Missing Opcodes** (need tests):
- **Arithmetic**: OP_Add, OP_Subtract, OP_Multiply, OP_Divide
- **Comparison**: OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge
- **String**: OP_Concat
- **Cursor**: OP_OpenRead, OP_Column, OP_Rowid, OP_Next, OP_Close
- **Aggregate**: OP_AggStep, OP_AggFinal
- **Sorter**: OP_SorterOpen, OP_SorterInsert, OP_SorterNext
- **Jump**: OP_IfNull, OP_If, OP_IfNot

**Test Plan**:
```c
// These tests already exist in test_vdbe.c - just need to run them!
TEST(vdbe_arithmetic) {
    SELECT 1 + 2, 10 - 3, 4 * 5, 20 / 4
}

TEST(vdbe_comparison) {
    SELECT 1 < 2, 5 > 3, 4 = 4, 2 != 3
}

TEST(vdbe_string_operations) {
    SELECT 'Hello' || ' ' || 'World'
}

TEST(vdbe_table_scan) {
    SELECT * FROM test  // Tests OP_OpenRead, OP_Column, OP_Next
}

TEST(vdbe_where_clause) {
    SELECT * FROM test WHERE id = 1  // Tests OP_Eq, conditional jumps
}

TEST(vdbe_aggregate_functions) {
    SELECT COUNT(*), SUM(value) FROM test  // Tests OP_AggStep, OP_AggFinal
}

TEST(vdbe_order_by) {
    SELECT * FROM test ORDER BY id  // Tests OP_SorterOpen, etc.
}
```

**Expected Coverage Gain**: +15-20% just from running test_vdbe.c

### cg_expr.c Coverage Strategy (1,044 lines)

**Currently Missing**:
- Complex expressions (nested operators)
- Function calls (MAX, MIN, LENGTH, SUBSTR, etc.)
- CASE expressions
- Subqueries in expressions

**Test Plan**:
```c
TEST(expr_complex) {
    SELECT (1 + 2) * 3, id * 2 + 5 FROM test
}

TEST(expr_functions) {
    SELECT MAX(id), MIN(id), COUNT(*) FROM test
    SELECT LENGTH(name), SUBSTR(name, 1, 3) FROM test
}

TEST(expr_case) {
    SELECT CASE WHEN id > 5 THEN 'high' ELSE 'low' END FROM test
}
```

**Expected Coverage Gain**: +8-12%

### cg_select.c Coverage Strategy (1,170 lines)

**Currently at 21.71%** - Blocked by failing SELECT tests

**After fixing SELECT tests**:
- JOIN operations
- GROUP BY
- HAVING
- DISTINCT
- LIMIT/OFFSET
- Nested SELECT

**Test Plan**:
```c
TEST(select_join) {
    SELECT * FROM t1 JOIN t2 ON t1.id = t2.id
}

TEST(select_group_by) {
    SELECT category, COUNT(*) FROM test GROUP BY category
}

TEST(select_distinct) {
    SELECT DISTINCT category FROM test
}

TEST(select_limit) {
    SELECT * FROM test LIMIT 10 OFFSET 5
}
```

**Expected Coverage Gain**: +10-15% (once SELECT works)

## Phase 4: Edge Cases and Error Paths (Est. +5-10% coverage)

### Error Handling Coverage

**Currently Undertested**:
- Out of memory conditions
- Invalid SQL syntax variations
- Constraint violations (UNIQUE, NOT NULL, CHECK)
- Foreign key errors
- Nested transaction errors

**Test Plan**:
```c
TEST(error_constraints) {
    CREATE TABLE t (id INTEGER UNIQUE);
    INSERT INTO t VALUES (1);
    INSERT INTO t VALUES (1);  // Should fail UNIQUE constraint
}

TEST(error_syntax) {
    // Various invalid SQL to test parser error paths
    db->exec(db, "SELECT FROM WHERE", ...);  // Invalid
    db->exec(db, "CREATE TABLE", ...);       // Incomplete
}

TEST(error_foreign_keys) {
    // If foreign keys implemented
    INSERT INTO child VALUES (999);  // Non-existent parent
}
```

**Expected Coverage Gain**: +3-5%

## Phase 5: Performance and Stress Tests (Est. +2-5% coverage)

### Additional Code Paths

**Large Data Tests**:
- 1000+ row tables
- Long strings (>1000 chars)
- Many columns (>50)

**Concurrent Tests**:
- Multiple threads (already in test_memory.c)
- Simultaneous readers/writers

**Expected Coverage Gain**: +2-5%

## Summary Timeline

| Phase | Tasks | Est. Effort | Coverage Gain | Cumulative |
|-------|-------|-------------|---------------|------------|
| **Baseline** | - | - | - | 29.17% |
| **Phase 1** | Fix tests | 4-6 hours | +10-15% | ~42% |
| **Phase 2** | Zero-coverage files | 6-8 hours | +8-12% | ~52% |
| **Phase 3** | Critical files | 12-16 hours | +30-40% | ~87% |
| **Phase 4** | Error paths | 4-6 hours | +5-10% | ~94% |
| **Phase 5** | Stress tests | 2-4 hours | +2-5% | **~97%** |
| **TOTAL** | **All phases** | **28-40 hours** | **+65-80%** | **95-97%** |

## Immediate Next Steps

1. ✅ **Baseline measured**: 29.17% coverage
2. ⏳ **Fix 7 failing tests**: Debug SELECT and transaction issues
3. ⏳ **Run test_vdbe**: Should add ~15% coverage immediately
4. ⏳ **Run test_memory**: Should add ~3% coverage
5. ⏳ **Add zero-coverage tests**: Focus on api_table, cg_pragma, cg_date first
6. ⏳ **Target vdbe.c**: Add missing opcode tests
7. ⏳ **Iterate**: Remeasure after each phase, adjust strategy

## Quick Win Opportunities

**Highest ROI Tests** (most coverage per test):
1. ✅ **Run test_vdbe.c** - Already written, just needs to execute (+15-20%)
2. **Fix SELECT tests** - Unblocks cg_select.c coverage (+10-15%)
3. **Add api_get_table test** - Covers api_table.c completely (+0.3%)
4. **Add PRAGMA test** - Covers cg_pragma.c (+0.6%)

**Total from quick wins**: +25-35% → Gets us to ~55-65% coverage

Then focus on Phase 3 (vdbe.c, cg_expr.c) to reach 95%.

## Success Criteria

- ✅ Line coverage >95%
- ✅ Function coverage >95%
- ✅ Branch coverage >90%
- ✅ All tests pass (no failures)
- ✅ No crashes or memory leaks
- ✅ CI integration complete

---

**Current Status**: Foundation complete, ready for systematic coverage improvement
**Next Action**: Fix 7 failing API tests to unblock SELECT coverage
