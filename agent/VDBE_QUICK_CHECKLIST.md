# VDBE Modernization - Quick Checklist

**Start Date:** _________
**Target:** Get `/home/gburd/ws/dbsql/src/vdbe.c` compiling (35/35 files)

---

## Pre-Flight Check
- [ ] Read `TASK_29_VDBE_MODERNIZATION.md` (overview)
- [ ] Read `VDBE_IMMEDIATE_FIXES.md` (detailed fixes)
- [ ] Backup: `cp src/vdbe.c src/vdbe.c.stack_based`
- [ ] Note current error count: `cd build_unix && make 2>&1 | grep -c "error:"` = ______

---

## Phase 1: Immediate Fixes (2-3 hours)

### ✓ Fix #1: Add vdbe_return Label (2 min)
- [ ] Open `src/vdbe.c`
- [ ] Find line 5936 (`vdbe_halt:`)
- [ ] Insert before it:
  ```c
  vdbe_return:
      p->pc = pc;
      /* Fall through to vdbe_halt */
  ```
- [ ] Save file
- [ ] **Test:** `cd build_unix && make 2>&1 | grep "vdbe_return"` = 0

### ✓ Fix #2: Disable Legacy Aggregator (5 min)
- [ ] Find line 179 (before `__agg_insert`)
- [ ] Insert: `#if 0  /* LEGACY AGGREGATOR - REMOVED */`
- [ ] Find line 234 (after `__agg_in_focus`)
- [ ] Insert: `#endif /* LEGACY AGGREGATOR */`
- [ ] Save file
- [ ] **Test:** `cd build_unix && make 2>&1 | grep -c "agg_elem_t"` = 0

### ✓ Fix #3a: Update __entity_as_string() (10 min)
- [ ] Line 249: Change `stack->r` → `stack->u.r`
- [ ] Line 253: Change `stack->i` → `stack->u.i`, `%d` → `%lld`
- [ ] Lines 249-259: Replace zShort logic with dynamic allocation (see VDBE_IMMEDIATE_FIXES.md)
- [ ] Save file

### ✓ Fix #3b: Update __entity_to_int() (5 min)
- [ ] Line 366: Change `stack->i` → `stack->u.i`, `stack->r` → `stack->u.r`
- [ ] Line 369: Change `__dbsql_atoi` → `__dbsql_atoi64`, `&stack->i` → `&val`, then `stack->u.i = val`
- [ ] Line 372: Change `stack->i` → `stack->u.i`
- [ ] Save file

### ✓ Fix #3c: Update __entity_to_real() (5 min)
- [ ] Line 391: Change `stack->r` → `stack->u.r`, `stack->i` → `stack->u.i`
- [ ] Line 395: Change `stack->r` → `stack->u.r`
- [ ] Line 398: Change `stack->r` → `stack->u.r`
- [ ] Line 401: Change `stack->r` → `stack->u.r`
- [ ] Save file

### ✓ Fix #3d: Update __vdbe_boolean_value() (5 min)
- [ ] Line 414: Change `pMem->i` → `pMem->u.i`
- [ ] Line 416: Change `pMem->r` → `pMem->u.r`
- [ ] Save file
- [ ] **Test:** `cd build_unix && make 2>&1 | grep "has no member 'i'"` = much lower

### ✓ Fix #4: Update __api_step() (20 min) - COMPLETED
- [x] Lines 156-169: Replace `azColName`/`azResColumn` logic (see VDBE_IMMEDIATE_FIXES.md)
- [x] Add Mem* → char** conversion
- [x] Save file
- [x] **Test:** `cd build_unix && make 2>&1 | grep "azColName\|azResColumn"` = 0

### ✓ Fix #5: Update __vdbe_exec() (30 min) - COMPLETED
- [x] Line 670: Change `mem_t *pTos;` → `Mem *aMem;`
- [x] Line 690: Remove `pTos = p->pTos;`
- [x] Lines 691-693: Remove popStack logic
- [x] After removals, add: `aMem = p->registers;`
- [x] Line 697: Remove aStack assertion
- [x] Line 5944: Remove `p->pTos = pTos;`
- [x] Save file
- [x] **Test:** `cd build_unix && make 2>&1 | grep "pTos\|popStack\|aStack"` = much lower

### ✓ Fix #6: Fix OP_Variable (10 min) - COMPLETED
- [x] Lines 909-920: Rewrite to use `p->variables` (Mem* array) and register addressing
- [x] See VDBE_IMMEDIATE_FIXES.md for complete code
- [x] Save file
- [x] **Test:** `cd build_unix && make 2>&1 | grep "azVar"` = 0

### ✓ Fix #7: Fix OP_Gosub and OP_Return (15 min) - COMPLETED
- [x] Lines 801-812 (OP_Gosub): Use register P1 to store return address
- [x] Lines 820-830 (OP_Return): Read return address from register P1
- [x] See VDBE_IMMEDIATE_FIXES.md for complete code
- [x] Save file
- [x] **Test:** `cd build_unix && make 2>&1 | grep "returnStack\|returnDepth"` = 0

### ✓ Fix #8: Remove aStack Assertions (10 min) - COMPLETED
- [x] Run: `grep -n "aStack" src/vdbe.c`
- [x] For each line found, comment out or remove assertion
- [x] All ~45 aStack assertions removed/commented
- [x] Save file
- [x] **Test:** `cd build_unix && make 2>&1 | grep "aStack"` = 0

---

## Phase 1 Complete Check
- [x] **Error count:** `cd build_unix && make 2>&1 | grep -c "error:"` = 20 (target: <50) ✓
- [x] **Reduction:** 90% (200 → 20 errors) (target: >75%) ✓
- [x] **vdbe.c status:** NO compilation errors in vdbe.c! ✓
- [x] **Remaining errors:** All 20 errors are in cg_pragma.c (unrelated to VDBE fixes)
- [x] **Ready for Phase 2:** YES - vdbe.c compiles cleanly, ready for opcode conversion

---

## Phase 2: Opcode Conversion (20-40 hours)

### Setup
- [x] Create tracking file: `VDBE_PHASE2_PROGRESS.md` ✓
- [x] List all opcodes: Analyzed 148 opcodes in vdbe.c ✓

### Group 1: Simple Data Opcodes (2 hours) - COMPLETE ✓
Convert these opcodes to register-based:

- [x] **OP_Integer** (line 905) ✓
  - Pattern: `pTos++` → `aMem[pOp->p2]`, `pTos->i` → `pOut->u.i`
- [x] **OP_String** (line 922) ✓
- [x] **OP_Pop** (line 971) - converted to no-op ✓
- [x] **OP_Dup** (line 998) ✓
- [x] **OP_Pull** (line 1034) ✓
- [x] **OP_Push** (line 1070) ✓
- [x] Test compile after group ✓ (0 errors in vdbe.c)

### Group 2: Arithmetic Opcodes (2 hours) - COMPLETE ✓
- [x] **OP_Add** (line 1302) - already converted ✓
  - Pattern: Uses `p->registers[pOp->p1]`, `p->registers[pOp->p2]`, result → `p->registers[pOp->p3]`
- [x] **OP_Subtract** (line 1302) - already converted ✓
- [x] **OP_Multiply** (line 1302) - already converted ✓
- [x] **OP_Divide** (line 1302) - already converted ✓
- [x] **OP_Remainder** (line 1302) - already converted ✓
- [x] **OP_AddImm** (line 1521) ✓
- [x] **OP_ForceInt** (line 1539) ✓
- [x] **OP_MustBeInt** (line 1576) ✓
- [x] **OP_Negative** (line 2029) ✓
- [x] **OP_AbsValue** (line 2029) ✓
- [x] Test compile after group ✓ (0 errors)

### Group 3: Comparison Opcodes (2 hours) - COMPLETE ✓
- [x] **OP_Eq** (line 1733) ✓
- [x] **OP_Ne** (line 1733) ✓
- [x] **OP_Lt** (line 1733) ✓
- [x] **OP_Le** (line 1733) ✓
- [x] **OP_Gt** (line 1733) ✓
- [x] **OP_Ge** (line 1733) ✓
- [x] Test compile after group ✓ (0 errors)

### Group 4: Logic Opcodes (1 hour) - COMPLETE ✓
- [x] **OP_And** (line 1982) ✓
- [x] **OP_Or** (line 1982) ✓
- [x] **OP_Not** (line 2063) ✓
- [x] **OP_BitNot** (line 2079) ✓
- [x] **OP_BitAnd** (line 1483) - already converted ✓
- [x] **OP_BitOr** (line 1483) - already converted ✓
- [x] **OP_ShiftLeft** (line 1483) - already converted ✓
- [x] **OP_ShiftRight** (line 1483) - already converted ✓
- [x] Test compile after group ✓ (0 errors)

### Group 5: Stack Operations to Remove (1 hour) - COMPLETE ✓
These opcodes are stack-specific and may need removal or complete rewrite:
- [x] **OP_Pop** (line 971) - converted to no-op ✓
- [x] **OP_Dup** (line 998) - converted to register copy ✓
- [x] **OP_Pull** (line 1034) - converted to register move ✓
- [x] **OP_Push** (line 1070) - converted to register copy with release ✓
- [x] Test compile after group ✓ (0 errors)

### Group 5.5: NULL Checks and String Operations - COMPLETE ✓
- [x] **OP_IsNull** (line 2153) ✓
- [x] **OP_NotNull** (line 2178) ✓
- [x] **OP_Concat** (line 1197) ✓
- [x] Test compile after group ✓ (0 errors)

### Group 6: Control Flow (2 hours)
- [ ] **OP_Goto** (probably already correct)
- [ ] **OP_If**
- [ ] **OP_IfNot**
- [ ] **OP_IfNeg**
- [ ] **OP_IfPos**
- [ ] **OP_Halt** (probably already correct)
- [ ] Test compile after group

### Group 7: Database Operations (10-15 hours)
This is the largest and most complex group. Convert carefully:

- [ ] **OP_Column** - Read column from cursor to register
- [ ] **OP_Rowid** - Get rowid to register
- [ ] **OP_Insert** - Insert row from registers
- [ ] **OP_Delete** - Delete current row
- [ ] **OP_MakeRecord** - Create record from register range
- [ ] **OP_OpenRead** - Open cursor (probably already correct)
- [ ] **OP_OpenWrite** - Open cursor (probably already correct)
- [ ] **OP_OpenEphemeral**
- [ ] **OP_Rewind**
- [ ] **OP_Next**
- [ ] **OP_Prev**
- [ ] **OP_Seek**
- [ ] **OP_SeekGE**
- [ ] **OP_SeekGT**
- [ ] **OP_SeekLE**
- [ ] **OP_SeekLT**
- [ ] **OP_NewRowid**
- [ ] **OP_Last**
- [ ] Test compile after every 5 opcodes

### Group 8: Functions and Aggregates (5 hours)
- [ ] **OP_Function**
- [ ] **OP_AggStep**
- [ ] **OP_AggFinal**
- [ ] **OP_AggValue**
- [ ] Test compile after group

### Group 9: String Operations (2 hours)
- [ ] **OP_Concat** (line ~1153)
- [ ] **OP_Substr**
- [ ] **OP_Length**
- [ ] Test compile after group

### Group 10: Remaining Opcodes (10-20 hours)
Work through remaining opcodes systematically:
- [ ] List remaining: `make 2>&1 | grep "pTos" | grep "case OP_"`
- [ ] Convert one by one
- [ ] Test compile frequently

---

## Phase 2 Complete Check
- [ ] **Error count:** `cd build_unix && make 2>&1 | grep -c "error:"` = 0
- [ ] **Files compiling:** Should show "35/35" in build summary
- [ ] **Success:** vdbe.c compiles cleanly

---

## Phase 3: Testing & Validation (5-10 hours)

### Compilation Validation
- [ ] Clean build: `cd build_unix && make clean && make`
- [ ] No warnings: `make 2>&1 | grep warning | wc -l` = 0 (or very low)
- [ ] All files compile: Check for "35/35" or similar

### Runtime Testing
- [ ] Run test suite: `make test` (if exists)
- [ ] Or run dbsql binary: `./dbsql test.db "SELECT 1+1;"`
- [ ] Check for crashes or errors

### Memory Testing
- [ ] Install valgrind: `sudo apt install valgrind` (if not installed)
- [ ] Run with valgrind: `valgrind --leak-check=full ./dbsql test.db "SELECT 1;"`
- [ ] Fix any memory leaks found

### Regression Testing
- [ ] Test basic queries (SELECT, INSERT, UPDATE, DELETE)
- [ ] Test JOIN operations
- [ ] Test aggregates (SUM, AVG, COUNT)
- [ ] Test string operations (CONCAT, SUBSTR)
- [ ] Test NULL handling
- [ ] Test type conversions

### Performance Testing (Optional)
- [ ] Benchmark simple queries
- [ ] Compare with SQLite 3.52.0 if available
- [ ] Ensure performance is within 5% of reference

---

## Phase 3 Complete Check
- [ ] All tests pass
- [ ] No memory leaks
- [ ] No crashes
- [ ] Performance acceptable

---

## Final Deliverables

### Code
- [ ] `src/vdbe.c` - Fully modernized, compiles, tests pass
- [ ] Remove backup: `rm src/vdbe.c.stack_based`

### Documentation
- [ ] Update `TASK_29_VDBE_MODERNIZATION.md` with completion date
- [ ] Create `TASK_29_COMPLETE.md` with summary of changes
- [ ] Document any gotchas or issues encountered

### Commit
- [ ] Stage changes: `git add src/vdbe.c`
- [ ] Commit: `git commit -m "Task #29: Modernize VDBE to register-based architecture (SQLite 3.52.0)"`
- [ ] Push: `git push origin claude`

---

## Success!

**Completion Date:** _________
**Total Time:** _______ hours
**Final Build Status:** 35/35 files compile ✓
**Tests Pass:** ✓

You've successfully modernized the VDBE from stack-based to register-based architecture!

---

## Quick Reference Commands

```bash
# Check error count
cd build_unix && make 2>&1 | grep -c "error:"

# Check specific error type
cd build_unix && make 2>&1 | grep "error_keyword"

# Find specific pattern in vdbe.c
grep -n "pattern" src/vdbe.c

# Count occurrences
grep -c "pattern" src/vdbe.c

# Compile just vdbe.c
cd build_unix && make ../src/vdbe.o

# Full build
cd build_unix && make

# Test (if available)
cd build_unix && make test
```

---

## If You Get Stuck

1. **Reference SQLite 3.52.0:** Look at equivalent opcode in SQLite source
2. **Check Headers:** Review `src/inc/vdbe_int.h` for structure definitions
3. **Review Code Generators:** See `src/cg_expr.c` for how opcodes are generated
4. **Read Analysis Docs:** Detailed info in `VDBE_IMMEDIATE_FIXES.md`
5. **Take Breaks:** Complex conversions need fresh eyes

**Remember:** This is mechanical work. Each opcode follows similar patterns. Progress = opcodes converted.
