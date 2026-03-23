# Task #29: VDBE Modernization (Stack-Based → Register-Based)

## Status: ANALYZED - Ready for Implementation

**File:** `/home/gburd/ws/dbsql/src/vdbe.c`
**Lines:** 5,988
**Current State:** FAILS compilation (blocking full build at 34/35 files)
**Target:** SQLite 3.52.0 register-based architecture
**Estimated Total Effort:** 100-150 hours

---

## Problem Statement

The VDBE (Virtual Database Engine) executor in `vdbe.c` uses legacy stack-based architecture while the code generators (`cg_expr.c`, `cg_insert.c`, etc.) were already modernized to generate register-based opcodes. This architectural mismatch causes compilation failures and blocks the full build.

### Key Architectural Differences:

| Aspect | Old (Stack-Based) | New (Register-Based) |
|--------|-------------------|----------------------|
| **Data Storage** | `mem_t *pTos` (stack pointer) | `Mem *aMem` (register array) |
| **Operations** | `pTos++`, `pTos--` (push/pop) | `aMem[iReg]` (direct addressing) |
| **Operands** | Implicit (top N stack items) | Explicit (P1, P2, P3 registers) |
| **Value Access** | `pTos->i`, `pTos->r` | `pTos->u.i`, `pTos->u.r` (union) |
| **Strings** | `mem_t->zShort[32]` (inline) | `mem_t->zMalloc` (dynamic) |
| **Aggregates** | `agg_t` hash structure | Mem-based accumulators |
| **Subroutines** | `returnStack[100]` array | Register storage |

---

## Current Build Errors

**Total Errors:** ~200
**Error Categories:**

1. **Structure Member Errors (60%):** No members `pTos`, `aStack`, `popStack`, `returnStack`, `azColName`, `azResColumn`, `azVar`, `anVar`
2. **Legacy Code Errors (20%):** Undefined `agg_t`, `agg_elem_t` types
3. **Field Access Errors (15%):** No members `.i`, `.r`, `zShort` (should use union `.u.i`, `.u.r`, and `zMalloc`)
4. **Control Flow Errors (5%):** Missing `vdbe_return` label

---

## Analysis Documents Created

Three detailed analysis documents have been created:

### 1. VDBE_CONVERSION_PLAN.md
**Purpose:** Complete strategic roadmap
**Contents:**
- Architecture changes explained
- All 6 conversion phases defined
- Per-opcode conversion strategy (148 opcodes)
- Risk mitigation and testing strategy
- 60-day timeline breakdown

### 2. VDBE_CURRENT_STATE_ANALYSIS.md
**Purpose:** Current state assessment
**Contents:**
- Partial modernization detected (some opcodes already converted)
- Detailed error categorization with line numbers
- Quantified remaining work (80 pTos++, 22 pTos--, 34 zShort, etc.)
- Immediate action plan for compilation
- Success criteria definition

### 3. VDBE_IMMEDIATE_FIXES.md (MOST ACTIONABLE)
**Purpose:** Get to compilation ASAP
**Contents:**
- 8 prioritized immediate fixes
- Before/after code for each fix
- Expected error reduction for each step
- Testing commands
- 2-3 hour implementation plan

---

## Recommended Approach

### Phase 1: Quick Wins (2-3 hours) → Reduce errors by 75%

Follow the 8 fixes in `VDBE_IMMEDIATE_FIXES.md`:

1. **Add `vdbe_return` label** (2 min) → Fixes 1 error
2. **Disable legacy aggregator** (5 min) → Fixes 40 errors
3. **Fix Mem union access** (30 min) → Fixes 50 errors
4. **Fix `__api_step()`** (20 min) → Fixes 2 errors
5. **Update `__vdbe_exec()` structure** (30 min) → Fixes 10 errors
6. **Fix `OP_Variable`** (10 min) → Fixes 3 errors
7. **Fix `OP_Gosub`/`OP_Return`** (15 min) → Fixes 6 errors
8. **Remove `aStack` assertions** (10 min) → Fixes 5 errors

**Result:** 200 errors → ~50 errors remaining

### Phase 2: Opcode Conversion (20-40 hours) → Full compilation

Convert remaining 140+ opcodes systematically:

**Group 1: Simple Data (2 hours)**
- OP_Integer, OP_String, OP_Null, OP_Not, etc.
- Pattern: `pTos++` → `aMem[pOp->p2]`, `pTos->i` → `pOut->u.i`

**Group 2: Arithmetic/Logic (4 hours)**
- OP_Add, OP_Subtract, OP_Multiply, OP_Divide, etc.
- Pattern: `pTos[-1]` → `aMem[pOp->p1]`, result in `aMem[pOp->p3]`

**Group 3: Comparison (3 hours)**
- OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge, etc.

**Group 4: Database Operations (10-15 hours)**
- OP_Column, OP_Insert, OP_Delete, OP_Rowid, etc.
- More complex due to cursor interaction

**Group 5: Control Flow (3 hours)**
- OP_If, OP_IfNot, OP_Goto, OP_Halt, etc.

**Group 6: Complex Operations (10-20 hours)**
- OP_MakeRecord, OP_Concat, OP_Function, OP_AggStep, etc.

### Phase 3: Testing & Validation (5-10 hours)

- Compile successfully (35/35 files)
- Run existing test suite
- Fix runtime issues
- Memory leak testing (valgrind)
- Performance validation

---

## Key Insights from Analysis

### Good News:

1. **Code generators already modernized** - `cg_expr.c`, `cg_insert.c`, `cg_select.c` already generate register-based opcodes. We're just aligning the executor.

2. **Some opcodes already converted** - Line 1103-1107 shows `OP_ResultRow` already uses `aMem[pOp->p1]` and `goto vdbe_return`, indicating partial modernization.

3. **Header files updated** - `vdbe_int.h` already has modern `struct vdbe` with `aMem`, `apCsr`, etc. No structure definitions need changing.

4. **Clear patterns** - Every opcode conversion follows similar patterns, making the work mechanical rather than architectural.

### Challenges:

1. **Mixed state** - File has both stack-based and register-based code, requiring careful attention to which pattern each opcode uses.

2. **148 opcodes** - Large number of opcodes to convert, but most are simple (5-20 lines each).

3. **Testing coverage** - Need comprehensive tests to ensure behavioral compatibility with SQLite 3.52.0.

4. **API compatibility** - `__api_step()` must maintain backward-compatible char** interface while internally using Mem* arrays.

---

## Success Criteria

- [ ] Zero compilation errors (35/35 files compile)
- [ ] All existing tests pass
- [ ] No new memory leaks (valgrind clean)
- [ ] Performance within 5% of SQLite 3.52.0
- [ ] Behavior matches SQLite 3.52.0 for standard queries

---

## Risk Assessment

### Low Risk:
- Helper function fixes (__entity_as_string, __entity_to_int, etc.)
- Simple opcodes (OP_Integer, OP_String, OP_Null)
- Structure member updates

### Medium Risk:
- Complex opcodes (OP_MakeRecord, OP_Column)
- Aggregate functions (need to verify code generator already handles)
- API compatibility layer in __api_step()

### High Risk:
- Control flow changes (OP_Gosub/Return using registers)
- Cursor operations (need to ensure proper register addressing)
- Edge cases in type conversion and NULL handling

**Mitigation:** Incremental testing after each opcode group conversion.

---

## Timeline Estimates

### Optimistic (experienced developer, 3-4 hours/day):
- **Phase 1:** 2 hours (1 day)
- **Phase 2:** 20 hours (1 week)
- **Phase 3:** 5 hours (1 day)
- **Total:** ~27 hours (~2 weeks)

### Realistic (learning while implementing, 2-3 hours/day):
- **Phase 1:** 3 hours (1 day)
- **Phase 2:** 40 hours (3 weeks)
- **Phase 3:** 10 hours (1 week)
- **Total:** ~53 hours (~4 weeks)

### Conservative (careful validation, edge cases):
- **Phase 1:** 5 hours (2 days)
- **Phase 2:** 60 hours (5 weeks)
- **Phase 3:** 15 hours (1 week)
- **Total:** ~80 hours (~6 weeks)

---

## Reference Materials

### SQLite 3.52.0 Source:
- `src/vdbe.c` - Modern register-based implementation
- `src/vdbeInt.h` - Structure definitions

### DBSQL Headers (Already Modernized):
- `src/inc/vdbe_int.h` - Modern Vdbe, VdbeCursor, Mem structures
- `src/inc/dbsql_int.in` - Type definitions

### Previous Conversions (Reference):
- `src/cg_expr.c` (Task #24) - Expression code generator (register-based)
- `src/cg_insert.c` (Task #23) - Insert code generator (register-based)
- `src/cg_select.c` - Select code generator (partially modernized)

### Build System:
- `build_unix/Makefile` - Compilation configuration
- `build_unix/.claude/` - Build state tracking

---

## Next Steps

### Immediate (Today):
1. Review `VDBE_IMMEDIATE_FIXES.md` line by line
2. Backup `vdbe.c` to `vdbe.c.stack_based`
3. Begin implementing Fix #1 (add vdbe_return label)
4. Test compilation after each fix

### Short-term (This Week):
1. Complete all 8 immediate fixes
2. Verify error count drops to ~50
3. Begin opcode conversion with Group 1 (simple data opcodes)
4. Test compilation after each opcode group

### Medium-term (This Month):
1. Complete all opcode conversions
2. Achieve successful compilation (35/35 files)
3. Run full test suite
4. Fix any runtime issues

### Long-term (Next Month):
1. Performance testing and optimization
2. Edge case validation
3. Documentation updates
4. Code review and cleanup

---

## Contact Points

If stuck or needing clarification:

1. **Architecture Questions:** Reference SQLite 3.52.0 `vdbe.c` lines for equivalent opcodes
2. **Structure Questions:** Check `src/inc/vdbe_int.h` for field definitions
3. **Opcode Parameters:** Check `src/inc/vdbe.h` for opcode definitions
4. **Code Generation:** Reference `src/cg_expr.c` to see how opcodes are generated

---

## Conclusion

This task is **well-scoped and achievable**. The analysis shows:

- Clear path from current state to working implementation
- Mechanical conversion patterns for most changes
- Already-modernized infrastructure (headers, code generators)
- Manageable risk with incremental testing

**The file is 80% stack-based, 20% register-based. We need to complete the transition that was already started.**

Start with the immediate fixes in `VDBE_IMMEDIATE_FIXES.md` - they're prioritized by impact and will quickly reduce the error count, making the remaining work much clearer.

---

## Files Created

1. **VDBE_CONVERSION_PLAN.md** - Complete strategic plan
2. **VDBE_CURRENT_STATE_ANALYSIS.md** - Detailed current state
3. **VDBE_IMMEDIATE_FIXES.md** - Actionable next steps ⭐ START HERE
4. **TASK_29_VDBE_MODERNIZATION.md** - This summary document

**All documents are in:** `/home/gburd/ws/dbsql/`
