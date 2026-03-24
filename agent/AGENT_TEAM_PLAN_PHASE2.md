# Agent Team Phase 2 - Memory Fixes & Testing

**Deployed:** 2026-03-24
**Objective:** Fix memory corruption, complete testing, fix naming violations

---

## Agent Assignments

### Agent 1: Memory Audit (Worktree: memory-audit)
**Branch:** `fix/memory-audit`
**Objective:** Replace all standard C allocator calls with __os_* functions

**Tasks:**
1. Search entire codebase for:
   - `malloc(` → `__os_malloc(dbenv, ...)`
   - `calloc(` → `__os_calloc(dbenv, ...)`
   - `realloc(` → `__os_realloc(dbenv, ...)`
   - `free(` → `__os_free(dbenv, ...)`
   - `strdup(` → `__os_strdup(dbenv, ...)`
2. Audit src/common/dbsql_alloc.c - REVERT the NULL checks added
3. Ensure dbenv is initialized early enough for all allocations
4. Fix any code that tries to allocate before dbenv is ready
5. Build and verify compilation

**Estimated Time:** 3-4 hours

---

### Agent 2: Memory Debug (Worktree: memory-debug)
**Branch:** `fix/memory-debug`
**Objective:** Use debugging tools to find exact corruption point

**Tasks:**
1. Build with debug symbols and AddressSanitizer:
   ```nix
   CFLAGS="-g -O0 -fsanitize=address" nix build
   ```
2. Run with valgrind:
   ```bash
   valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all \
            ./result/bin/dbsql :memory: "SELECT 1;"
   ```
3. Try umem allocator (if available):
   ```bash
   LD_PRELOAD=/path/to/libumem.so UMEM_DEBUG=default ./result/bin/dbsql :memory: "SELECT 1;"
   ```
4. Document exact stack traces of:
   - First allocation that causes problem
   - Free that triggers corruption
   - Call paths involved
5. Create detailed report with line numbers

**Estimated Time:** 2-3 hours

---

### Agent 3: Naming Convention Review (Resume: naming-fixes)
**Branch:** `naming-fixes` (existing, resume)
**Objective:** Complete the remaining 70% of naming convention fixes

**Tasks:**
1. Resume from existing work (30% complete)
2. Continue Phase 2 fixes:
   - **Phase 2.1:** vdbe_t fields (`aMem` → `registers`, `aOp` → `opcodes`)
   - **Phase 2.3:** table_t fields (`pIndex` → `first_index`, `pNext` → `next_table`)
3. Fix remaining `pXxx` pointer prefixes (70 violations remaining)
4. Fix remaining `aXxx` array prefixes (35 violations remaining)
5. Build and verify after each file
6. Create sed scripts for reproducible renames

**Estimated Time:** 4-5 hours

---

### Agent 4: Test Suite & Bug Fixes (Worktree: testing)
**Branch:** `fix/testing-bugs`
**Objective:** Run tests, find bugs, fix them

**Tasks:**
1. Wait for Agent 1 & 2 to identify/fix memory issues
2. Once memory is fixed, test:
   - Basic SQL: `SELECT 1;`
   - CREATE TABLE, INSERT, SELECT
   - **ORDER BY** (sorter opcodes implemented by previous agent)
   - UPDATE, DELETE
   - Joins, WHERE clauses
3. Use test files from dbsql-test-suite worktree (68 files, 2,940 tests)
4. Document all failing tests with:
   - Test case
   - Expected result
   - Actual result
   - Error messages
5. Fix bugs found (prioritize by severity)

**Estimated Time:** 5-6 hours (depends on memory fixes)

---

### Agent 5: Integration & Validation (Worktree: integration)
**Branch:** `fix/integration`
**Objective:** Merge all fixes, validate complete system

**Tasks:**
1. Wait for Agents 1-4 to complete
2. Merge branches in order:
   - memory-audit (critical)
   - naming-fixes (cleanup)
   - testing-bugs (fixes found during testing)
3. Run full test suite (all 68 files)
4. Verify:
   - No memory leaks (valgrind clean)
   - No naming violations
   - All critical SQL works
   - ORDER BY works correctly
5. Create final integration report
6. Prepare for merge to main/master

**Estimated Time:** 2-3 hours

---

## Critical Dependencies

```
Agent 1 (Memory Audit) ──────┐
                              ├──> Agent 2 (Memory Debug)
                              │         │
                              │         ├──> Agent 4 (Testing)
                              │         │         │
Agent 3 (Naming) ─────────────┴─────────┴─────────┴──> Agent 5 (Integration)
```

**Critical Path:** Agent 1 → Agent 2 → Agent 4 → Agent 5

**Parallel Work:** Agent 3 (Naming) can work independently

---

## Tools & Resources

### Valgrind Command
```bash
valgrind --leak-check=full \
         --track-origins=yes \
         --show-leak-kinds=all \
         --log-file=valgrind.log \
         ./result/bin/dbsql :memory: "SELECT 1;"
```

### AddressSanitizer Build
```nix
# In flake.nix, modify CFLAGS:
CFLAGS = "-g -O1 -fsanitize=address -fno-omit-frame-pointer";
```

### Umem Allocator (Optional)
```bash
# Clone from Codeberg
git clone https://codeberg.org/gregburd/libumem.git
cd libumem && make
LD_PRELOAD=./libumem.so UMEM_DEBUG=default <command>
```

### Test Suite Location
```
/home/gburd/ws/dbsql-test-suite/
  - 68 test files
  - 2,940 test cases
  - Automation scripts ready
```

---

## Success Criteria

### Phase 2 Complete When:
- ✅ All allocations use __os_* functions
- ✅ No memory leaks (valgrind clean)
- ✅ No memory corruption (no malloc assertions)
- ✅ `SELECT 1;` works
- ✅ CREATE TABLE, INSERT, SELECT work
- ✅ ORDER BY works (using implemented sorter)
- ✅ 100% naming convention compliance
- ✅ Test suite passing rate > 80%

---

## Estimated Total Time

| Agent | Hours |
|-------|-------|
| 1. Memory Audit | 3-4 |
| 2. Memory Debug | 2-3 |
| 3. Naming Fixes | 4-5 |
| 4. Testing | 5-6 |
| 5. Integration | 2-3 |
| **Total** | **16-21 hours** |

**Wall Clock (Parallel):** ~6-7 hours (Agents 1,3 parallel; then 2,4 parallel; then 5)

---

## Agent Launch Commands

```bash
# Agent 1: Memory Audit
claude-agent --task "memory-audit" --worktree dbsql-memory-audit --branch fix/memory-audit

# Agent 2: Memory Debug
claude-agent --task "memory-debug" --worktree dbsql-memory-debug --branch fix/memory-debug

# Agent 3: Naming Fixes (Resume)
claude-agent --task "naming-fixes" --worktree dbsql --branch naming-fixes --resume

# Agent 4: Testing
claude-agent --task "testing" --worktree dbsql-testing --branch fix/testing-bugs

# Agent 5: Integration
claude-agent --task "integration" --worktree dbsql-integration --branch fix/integration
```

---

**Status:** Ready to Deploy
**Next Action:** Launch Agents 1 and 3 in parallel
