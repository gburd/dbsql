# VDBE Phase 2 Readiness Assessment

**Date:** 2026-03-18
**Phase 1 Status:** ✅ COMPLETE
**Phase 2 Status:** Ready to begin

## Phase 1 Completion Summary

### Infrastructure Changes Complete
✅ Register array initialized (`aMem = p->registers`)
✅ Stack pointer removed (`pTos` eliminated from function signature)
✅ Stack operations removed (popStack, aStack assertions)
✅ API compatibility updated (__api_step with mem_t* extraction)
✅ Return address mechanism modernized (OP_Gosub/OP_Return)
✅ Variable access updated (OP_Variable uses registers)

### Compilation Status
✅ **vdbe.c errors:** 0 (was ~200)
✅ **Total errors:** 20 (all in cg_pragma.c, unrelated to VDBE)
✅ **Error reduction:** 90% overall, 100% in vdbe.c

## Phase 2 Work Scope

### Remaining Stack Operations

**pTos References:** 414 occurrences in vdbe.c

These are in opcode implementations that still use stack-based patterns:
- `pTos++` / `pTos--` (stack push/pop)
- `pTos[-1]` / `pTos[-2]` (stack-relative addressing)
- `pTos->i` / `pTos->r` / `pTos->z` (direct member access)
- `pNos = pTos - 1` (stack pointer arithmetic)

**Total Opcodes:** 148 case statements

Not all need conversion - some may already be register-compatible or removed.

### Conversion Pattern

**Stack-Based (Old):**
```c
case OP_Add: {
    pNos = pTos - 1;
    if ((pTos->flags | pNos->flags) & MEM_Null) {
        __entity_release_mem(pTos);
        pTos--;
        pTos->flags = MEM_Null;
    } else if ((pTos->flags & pNos->flags & MEM_Int) == MEM_Int) {
        pNos->i += pTos->i;
        __entity_release_mem(pTos);
        pTos--;
    }
    // ... more cases ...
    break;
}
```

**Register-Based (New):**
```c
case OP_Add: {
    mem_t *pIn1 = &aMem[pOp->p1];  /* Left operand */
    mem_t *pIn2 = &aMem[pOp->p2];  /* Right operand */
    mem_t *pOut = &aMem[pOp->p3];  /* Result */

    if ((pIn1->flags | pIn2->flags) & MEM_Null) {
        pOut->flags = MEM_Null;
    } else if ((pIn1->flags & pIn2->flags & MEM_Int) == MEM_Int) {
        pOut->u.i = pIn1->u.i + pIn2->u.i;
        pOut->flags = MEM_Int;
    }
    // ... more cases ...
    break;
}
```

### Conversion Strategy

#### Group 1: Simple Data Opcodes (2 hours, 10 opcodes)
Already partially done or trivial:
- OP_Integer
- OP_String
- OP_Null
- OP_Not
- OP_Blob

**Pattern:** Direct assignment to output register

#### Group 2: Arithmetic Opcodes (2 hours, 8 opcodes)
- OP_Add, OP_Subtract, OP_Multiply, OP_Divide
- OP_Remainder, OP_Concat
- OP_BitAnd, OP_BitOr

**Pattern:** 2 input registers → 1 output register

#### Group 3: Comparison Opcodes (2 hours, 6 opcodes)
- OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge

**Pattern:** 2 input registers → branch or boolean output

#### Group 4: Logic Opcodes (1 hour, 3 opcodes)
- OP_And, OP_Or, OP_Not

**Pattern:** Boolean operations on registers

#### Group 5: Stack Operations to Remove (1 hour, 5 opcodes)
- OP_Pop (remove or convert to no-op)
- OP_Dup (convert to register copy)
- OP_Pull (convert to register move)
- OP_Push (remove)
- OP_StackDepth (remove or convert)

**Pattern:** Eliminate or replace with register ops

#### Group 6: Control Flow (2 hours, 8 opcodes)
- OP_Goto (likely already correct)
- OP_If, OP_IfNot, OP_IfNeg, OP_IfPos
- OP_Halt (already fixed)
- OP_Gosub, OP_Return (already fixed)

**Pattern:** Read condition from register, adjust PC

#### Group 7: Database Operations (10-15 hours, 40+ opcodes)
Most complex group:
- OP_Column - Read column to register
- OP_Rowid - Get rowid to register
- OP_Insert - Insert from register range
- OP_Delete, OP_MakeRecord
- OP_OpenRead, OP_OpenWrite, OP_OpenEphemeral
- OP_Rewind, OP_Next, OP_Prev
- OP_Seek*, OP_NewRowid, OP_Last
- Many more...

**Pattern:** Varies widely, cursor + register interactions

#### Group 8: Functions and Aggregates (5 hours, 10 opcodes)
- OP_Function
- OP_AggStep, OP_AggFinal, OP_AggValue
- Others

**Pattern:** Register range for arguments, register for result

#### Group 9: String Operations (2 hours, 5 opcodes)
- OP_Concat (may be in Group 2)
- OP_Substr, OP_Length
- Others

**Pattern:** String register ops

#### Remaining Opcodes (10-20 hours)
~50+ opcodes not in above groups

## Estimated Phase 2 Timeline

### Conservative Estimate
- **Simple opcodes (Groups 1-4):** 7 hours
- **Stack removal (Group 5):** 1 hour
- **Control flow (Group 6):** 2 hours
- **Database ops (Group 7):** 15 hours
- **Functions (Group 8):** 5 hours
- **String ops (Group 9):** 2 hours
- **Remaining:** 15 hours
- **Testing/debugging:** 5 hours
- **Total:** 52 hours

### Optimistic Estimate
Many opcodes may be simpler than expected, or already partially converted:
- **Total:** 30-35 hours

### Realistic Estimate
**40 hours** over 5-7 work sessions

## Prerequisites for Phase 2

### Must Be Complete First
1. ✅ Phase 1 fixes (this task) - DONE
2. ⚠️  cg_pragma.c fixes - IN PROGRESS (Agent 1, Task #13)
3. ✅ Code generators produce register-based opcodes - DONE (Task #5)

### Nice to Have
- Reference SQLite 3.52.0 source for comparison
- Test framework for validating individual opcodes
- Debugging tools for register inspection

## Phase 2 Success Criteria

1. **Compilation:**
   - All 148 opcodes compile
   - Zero pTos references remain
   - No stack operations remain

2. **Runtime:**
   - Basic queries execute (SELECT, INSERT, UPDATE, DELETE)
   - No crashes or segfaults
   - Results match expected output

3. **Testing:**
   - Test suite passes (if exists)
   - No memory leaks detected
   - Performance within 5% of SQLite 3.52.0

## Recommendations

### For Starting Phase 2

1. **Wait for cg_pragma.c fix** from Agent 1
   - Once done, full project should build
   - Provides clean baseline for testing

2. **Start with simple opcodes** (Groups 1-2)
   - Build confidence with conversion pattern
   - Test compilation frequently

3. **Reference SQLite source**
   - Compare DBSQL opcodes to SQLite equivalents
   - Verify P1/P2/P3 usage patterns

4. **Test incrementally**
   - Convert 5-10 opcodes, test compile
   - Don't accumulate errors

5. **Document gotchas**
   - Track unusual patterns
   - Note opcodes that need special handling

### Tools and Commands

```bash
# Count remaining pTos references
grep -c 'pTos' src/vdbe.c
# Current: 414

# Find specific opcode
grep -n 'case OP_Add:' src/vdbe.c

# Compile just vdbe.c
cd build_unix && make ../src/vdbe.o

# Full build
cd build_unix && make

# Count errors
make 2>&1 | grep -c "error:"
```

## Coordination

### With Agent 1 (Task #13)
- Agent 1 should complete cg_pragma.c fixes first
- Once done, Agent 2 can begin Phase 2
- Avoid simultaneous edits to avoid conflicts

### Documentation
- Update VDBE_QUICK_CHECKLIST.md as opcodes are converted
- Track progress in OPCODES_DONE.txt
- Create TASK_29_COMPLETE.md when finished

## Conclusion

Phase 1 is complete and successful. The infrastructure for register-based VDBE is in place, and vdbe.c compiles cleanly. Phase 2 is ready to begin once cg_pragma.c is fixed, with clear scope (148 opcodes, 414 pTos references) and methodology (systematic group-by-group conversion).

**Estimated Phase 2 Duration:** 40 hours
**Recommended Approach:** Incremental, tested, documented
**Success Probability:** High (pattern is clear and repeatable)

---

**Status:** Ready for Phase 2
**Blocking Issue:** cg_pragma.c (Agent 1's task)
**Next Action:** Wait for Agent 1, then begin Group 1 opcodes
