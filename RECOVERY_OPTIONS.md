# Recovery Options Analysis

## Executive Summary

**Status**: Recent commits (99f1a30) revealed transaction isolation issues that may have broken tests.

**Last Known Stable**: Commit 4743981 "Fix critical bugs and achieve 100% core test pass rate"

**Current Status**: Tests #129-#132 pending, indicating possible regressions in database operations.

## Commit Analysis (Last 10 Commits)

### From 0a55969 to 99f1a30 (Current HEAD)

```
99f1a30 Testing BDB cursor transaction fix - reveals deeper transaction isolation issue
5f687e2 Fix Berkeley DB cursor transaction isolation - root cause
34bc4eb Add OP_Transaction to SELECT queries - partial fix for data visibility
3e883bd Debug prepared statement issue: Fix OP_NullCallback and add tracing
83c9ce0 Merge fix/delete-where: Fix DELETE with WHERE clause
0a5e48f Fix DELETE with WHERE clause DBSQL_MISUSE error
4743981 Fix critical bugs and achieve 100% core test pass rate ← LAST KNOWN GOOD
0a55969 Fix P3_POINTER invalid free causing test crash
dcbe5e3 Merge fix/vdbe-opcode-init: Cleanup and critical bug fixes
8ca939e Clean up _CONVERTED files and fix memory/database issues
```

## Risk Assessment

### High Risk Commits (Behavioral Changes)

1. **99f1a30** - "reveals deeper transaction isolation issue"
   - Commit message indicates discovery of NEW problem
   - May have introduced test code that shows existing bugs
   - Risk: If this added debugging code, might affect normal operation

2. **5f687e2** - "Fix Berkeley DB cursor transaction isolation - root cause"
   - Changes transaction management in `src/sm.c`
   - Modified `__sm_cursor()` and `__sm_close_cursor()`
   - Risk: Core storage layer changes can break all database operations
   - Evidence: CURSOR_TRANSACTION_FIX.md shows this changed cursor transaction handling

3. **34bc4eb** - "Add OP_Transaction to SELECT queries"
   - Added transaction operations to SELECT (read) operations
   - Risk: Changes execution model for every SELECT query

### Medium Risk Commits (Debugging/Fixes)

4. **3e883bd** - "Debug prepared statement issue"
   - Added tracing and fixed OP_NullCallback
   - Risk: May have fprintf debug statements left in code

5. **0a5e48f** - "Fix DELETE with WHERE clause"
   - Fixed specific DELETE operation
   - Risk: Low - targeted fix for known issue

### Low Risk Commits (Known Good)

6. **4743981** - "100% core test pass rate"
   - Last verified working state
   - All tests passing at this point

7. **0a55969** - "Fix P3_POINTER invalid free"
   - Memory safety fix
   - Risk: Very low - fixes crash

## Pending Tasks Analysis

Tasks #129-#132 indicate regressions:
- #129: Database opening failure (DBSQL_CANTOPEN)
- #130: SQL execution failure (DBSQL_MISUSE)
- #131: Database closing failure (DBSQL_ERROR)
- #132: Database metadata system regression

**Root cause**: Transaction isolation changes in commits 5f687e2 and 34bc4eb likely caused these failures.

## Recovery Options

### Option 1: Full Rollback to 4743981 (SAFEST)

**What**: Revert to last known 100% working state

**Commands**:
```bash
cd /home/gburd/ws/dbsql
git checkout -b recovery/rollback-to-stable
git reset --hard 4743981
```

**Pros**:
- Guaranteed working state (100% tests passed)
- Fast recovery
- Known good baseline for debugging

**Cons**:
- Loses all fixes after 4743981:
  - DELETE with WHERE clause fix (0a5e48f)
  - Prepared statement debugging (3e883bd)
  - Transaction isolation attempts (34bc4eb, 5f687e2)
- Must re-apply fixes individually

**Timeline**: Immediate (< 5 minutes)

**Recommendation**: Use if other options fail within 2 hours

### Option 2: Selective Revert (RECOMMENDED)

**What**: Keep bug fixes (#126-#128), revert transaction changes

**Strategy**:
1. Create recovery branch from current HEAD
2. Revert transaction isolation commits in reverse order
3. Keep memory safety and DELETE fixes
4. Test incrementally

**Commands**:
```bash
cd /home/gburd/ws/dbsql
git checkout -b recovery/selective-revert

# Revert transaction changes in reverse order
git revert --no-commit 99f1a30  # Remove test that revealed issues
git revert --no-commit 5f687e2  # Remove cursor transaction fix
git revert --no-commit 34bc4eb  # Remove OP_Transaction for SELECT

# Keep these commits (already in history before reverts):
# - 0a5e48f (DELETE fix)
# - 3e883bd (OP_NullCallback fix)
# - 0a55969 (P3_POINTER fix)

git commit -m "Revert transaction isolation changes (34bc4eb, 5f687e2, 99f1a30)"
```

**Pros**:
- Keeps verified bug fixes (#126-#128)
- Removes only problematic changes
- Easier to re-apply transaction fixes with correct approach

**Cons**:
- May have conflicts during revert
- Requires testing to verify stability
- SELECT may return no rows after INSERT (known issue pre-fix)

**Timeline**: 30 minutes to resolve conflicts + 1 hour testing

**Recommendation**: Try this first before Option 1

### Option 3: Forward Fix (CURRENT APPROACH)

**What**: Fix regressions in place without reverting

**Status**: Tasks #129-#132 assigned to other agents

**Risks**:
- Transaction isolation is complex
- May introduce new bugs while fixing current ones
- Unknown timeline

**Decision Criteria**:
- If fixes complete within 2 hours → Continue
- If fixes incomplete or cause new issues → Switch to Option 2
- If Option 2 fails → Switch to Option 1

**Recommendation**: Give current effort 2 hours max, then pivot

### Option 4: Cherry-Pick Approach (HYBRID)

**What**: Start from 4743981, cherry-pick only verified fixes

**Commands**:
```bash
cd /home/gburd/ws/dbsql
git checkout -b recovery/cherry-pick 4743981

# Cherry-pick ONLY verified safe fixes
git cherry-pick 0a5e48f  # DELETE with WHERE fix
git cherry-pick 3e883bd  # OP_NullCallback fix (check for debug code first)

# Skip transaction isolation changes (34bc4eb, 5f687e2, 99f1a30)
# These need redesign based on current failure analysis
```

**Pros**:
- Clean slate with verified fixes
- Avoids known problematic commits
- Maintains git history clarity

**Cons**:
- Most conservative approach
- Loses transaction visibility work
- Requires re-implementing transaction fixes from scratch

**Timeline**: 1 hour to cherry-pick + verify

**Recommendation**: Use if Option 2 has merge conflicts

## Detailed Analysis: Transaction Isolation Changes

### The Core Problem

**File**: `src/sm.c`, function `__sm_cursor()`

**Change in 5f687e2**:
- Before: All cursors create child transactions
- After: Only write cursors create child transactions, read cursors use parent

**Intent**: Fix SELECT seeing no rows after INSERT

**Actual Effect**: May have broken transaction semantics:
- DBSQL_CANTOPEN: Database transaction system initialization failure?
- DBSQL_MISUSE: Transaction state mismanagement?
- DBSQL_ERROR: Transaction cleanup failure?

### Evidence from CURSOR_TRANSACTION_FIX.md

The document shows:
1. Design change was significant (child vs parent transaction)
2. Changed both cursor open AND close logic
3. Added new cursor flags (SMC_RW_CURSOR, SMC_RO_CURSOR)
4. Modified transaction commit logic

**Hypothesis**: The fix may have introduced edge cases:
- Cursor opened with one transaction model, closed with another
- Transaction state not properly inherited
- Metadata operations using wrong transaction context

## Features Lost in Each Recovery Option

### Option 1 Rollback (4743981)
**Lost features**:
- DELETE with WHERE clause (Issue #110)
- Prepared statement improvements
- Transaction isolation attempt
- Any debug improvements after 0a55969

**Impact**: DELETE operations revert to broken state

### Option 2 Selective Revert
**Lost features**:
- Transaction isolation fixes (SELECT after INSERT may fail)

**Impact**: Known issue (SELECT returns no rows) returns, but overall stability maintained

### Option 3 Forward Fix
**Lost features**: None (if successful)

**Impact**: Unknown - depends on fix quality

### Option 4 Cherry-Pick
**Lost features**:
- Transaction isolation work (SELECT after INSERT issue)
- Possibly prepared statement debug improvements

**Impact**: Similar to Option 2 but with cleaner history

## Recommended Recovery Procedure

### Phase 1: Immediate Assessment (5 minutes)

1. Check task status:
```bash
# Check if tasks #129-#132 have progress
# If no progress after 30 minutes, move to Phase 2
```

2. Quick test current state:
```bash
cd /home/gburd/ws/dbsql/test/src
./test_api 2>&1 | head -20
# If DBSQL_CANTOPEN or segfault → immediate recovery needed
```

### Phase 2: Decision Point (2 hours from task assignment)

**If by 2 hours from now:**
- Tasks #129-#132 still pending → Execute Option 2
- Tasks showing new failures → Execute Option 2
- Tasks completed successfully → Continue, no recovery needed

### Phase 3: Execute Recovery (30-60 minutes)

**Recommended**: Option 2 (Selective Revert)

```bash
cd /home/gburd/ws/dbsql

# Create recovery branch
git checkout -b recovery/selective-revert-$(date +%Y%m%d-%H%M)

# Revert problematic commits
git revert --no-commit 99f1a30 5f687e2 34bc4eb

# Resolve any conflicts (focus on src/sm.c and src/vdbe.c)
# Priority: Keep DELETE fix, remove transaction changes

# Commit revert
git commit -m "Revert transaction isolation changes

Reverts commits:
- 99f1a30 (transaction test revealing deeper issues)
- 5f687e2 (cursor transaction isolation changes)
- 34bc4eb (OP_Transaction for SELECT)

Reason: Changes caused regressions in tasks #129-#132:
- DBSQL_CANTOPEN (database opening)
- DBSQL_MISUSE (SQL execution)
- DBSQL_ERROR (database closing)
- Metadata system failures

Keeps verified fixes:
- 0a5e48f (DELETE with WHERE)
- 3e883bd (OP_NullCallback)
- 0a55969 (P3_POINTER fix)

Known issue reintroduced: SELECT may return no rows after INSERT
(requires different transaction isolation approach)
"

# Build and test
cd build_unix.old
make clean && make

# Run test suite
cd ../test/src
./test_api > /tmp/recovery_test_results.txt 2>&1
cat /tmp/recovery_test_results.txt
```

**Success criteria**: Tests pass without DBSQL_CANTOPEN/MISUSE/ERROR

### Phase 4: Fallback Plan

**If Option 2 fails** (conflicts can't be resolved in 30 minutes):
```bash
# Execute Option 4 (Cherry-Pick)
cd /home/gburd/ws/dbsql
git checkout -b recovery/cherry-pick-$(date +%Y%m%d-%H%M) 4743981
git cherry-pick 0a5e48f  # DELETE fix only
# Test before proceeding
```

**If Option 4 fails**:
```bash
# Execute Option 1 (Full Rollback)
cd /home/gburd/ws/dbsql
git checkout -b recovery/full-rollback-$(date +%Y%m%d-%H%M) 4743981
# This is guaranteed to work (100% tests passed at this commit)
```

## Post-Recovery Actions

### After Successful Recovery

1. **Document what was reverted**:
```bash
echo "Reverted commits: 34bc4eb, 5f687e2, 99f1a30" > RECOVERY_LOG.md
echo "Reason: Transaction isolation regressions" >> RECOVERY_LOG.md
echo "Recovery method: [Option 1/2/3/4]" >> RECOVERY_LOG.md
```

2. **Tag recovery point**:
```bash
git tag -a recovery/stable-$(date +%Y%m%d-%H%M) -m "Stable state after recovery"
```

3. **Update status**:
```bash
# Mark tasks #129-#132 as resolved (if tests pass)
# Document known issues (e.g., SELECT after INSERT if Option 2 used)
```

4. **Plan transaction isolation redesign**:
- Study Berkeley DB nested transaction semantics more carefully
- Design comprehensive test for transaction isolation
- Implement fix incrementally with testing at each step

### Transaction Isolation Redesign (Future Work)

**Requirements**:
1. SELECT must see data from committed INSERT in same session
2. Concurrent transactions must be properly isolated
3. Read cursors must not create unnecessary child transactions
4. Write cursors must properly commit to parent transaction

**Approach** (for future implementation):
1. Create test case that captures exact failure scenario
2. Study Berkeley DB documentation on transaction visibility
3. Prototype fix in isolated branch
4. Verify fix doesn't break existing tests
5. Merge only after full test pass

**Reference**: See CURSOR_TRANSACTION_FIX.md for previous attempt analysis

## File Change Summary by Recovery Option

### Option 1: Full Rollback
**Files reverted**: All changes after 4743981
- src/cg_delete.c (DELETE fix lost)
- src/sm.c (transaction changes removed)
- src/vdbe.c (OP_Transaction changes removed)
- Multiple debug additions removed

### Option 2: Selective Revert
**Files reverted**:
- src/sm.c (transaction isolation changes only)
- src/vdbe.c (OP_Transaction for SELECT only)

**Files kept**:
- src/cg_delete.c (DELETE fix preserved)
- Memory safety fixes preserved

### Option 3: Forward Fix
**Files to fix** (based on task descriptions):
- src/api.c (database opening - task #129)
- src/sm.c (transaction handling - task #130)
- src/vdbe.c (execution flow - task #130)
- Metadata system (task #132)

### Option 4: Cherry-Pick
**Files modified** (clean cherry-pick of 0a5e48f):
- src/cg_delete.c only

## Success Metrics

### Minimum Success (Any Recovery Option)
- ✅ Tests run without DBSQL_CANTOPEN errors
- ✅ Tests run without DBSQL_MISUSE errors
- ✅ Tests run without DBSQL_ERROR errors
- ✅ Database can open, execute SQL, close cleanly

### Ideal Success (Options 2, 3, 4)
- ✅ All minimum success criteria
- ✅ DELETE with WHERE clause still works
- ✅ Memory safety fixes preserved
- ✅ Known issues documented

### Perfect Success (Option 3 only)
- ✅ All ideal success criteria
- ✅ SELECT sees data from INSERT
- ✅ Transaction isolation works correctly
- ✅ All original features preserved

## Timeline Expectations

| Option | Time to Execute | Time to Verify | Total | Risk |
|--------|----------------|----------------|-------|------|
| Option 1 | 5 min | 15 min | 20 min | None |
| Option 2 | 30 min | 30 min | 60 min | Low |
| Option 3 | Unknown | Unknown | 2+ hrs | High |
| Option 4 | 15 min | 30 min | 45 min | Low |

## Decision Matrix

| Situation | Action |
|-----------|--------|
| Tests pass, tasks complete in < 2 hours | Continue (Option 3) |
| Tests fail, conflicts resolvable | Option 2 (Selective Revert) |
| Tests fail, complex conflicts | Option 4 (Cherry-Pick) |
| Critical production issue | Option 1 (Full Rollback) |
| Unknown state, need safety | Option 1 (Full Rollback) |

## Conclusion

**Recommended Strategy**:
1. Give current fix effort 2 hours
2. If unsuccessful, execute Option 2 (Selective Revert)
3. If Option 2 fails, execute Option 4 (Cherry-Pick)
4. If Option 4 fails, execute Option 1 (Full Rollback)

**Rationale**:
- Preserves maximum verified fixes
- Minimizes risk exposure
- Clear escalation path
- Guarantees recovery (Option 1 fallback)

**Next Review**: Check status in 2 hours from task #129-#132 assignment

---

**Document Created**: 2026-03-31
**Last Known Good Commit**: 4743981
**Current HEAD**: 99f1a30
**Status**: Awaiting task completion or timeout for recovery decision
