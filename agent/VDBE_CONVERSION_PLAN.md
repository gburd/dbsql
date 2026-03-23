# VDBE Conversion Plan: Stack-Based to Register-Based Architecture

## Executive Summary

**File:** `/home/gburd/ws/dbsql/src/vdbe.c` (5,988 lines)
**Status:** Last file blocking full build (34/35 files compile)
**Task:** Modernize from legacy stack-based to SQLite 3.52.0 register-based execution
**Estimated Effort:** 100-150 hours

## Architecture Changes

### Stack → Register Transition

**OLD (Stack-Based):**
```c
mem_t *pTos;                     // Pointer to top of stack
mem_t aStack[MAX_STACK];         // Stack array
pTos++; pTos--;                  // Push/pop operations
pTos->i, pTos->r                 // Direct field access
```

**NEW (Register-Based):**
```c
Mem *aMem;                       // Register array
int iReg;                        // Register index
aMem[iReg].u.i, aMem[iReg].u.r  // Union field access
```

### Key Structural Changes

1. **Memory Cell Access:**
   - OLD: `pTos->i`, `pTos->r`
   - NEW: `pTos->u.i`, `pTos->u.r` (union members)

2. **String Storage:**
   - OLD: `mem_t->zShort[32]` (inline buffer)
   - NEW: `mem_t->zMalloc` (dynamic allocation via szMalloc)

3. **Column Names:**
   - OLD: `vm->azColName` (char** array)
   - NEW: `vm->aColName` (Mem* array)

4. **Result Columns:**
   - OLD: `vm->azResColumn` (char** array)
   - NEW: `vm->pResultRow` (Mem* pointer)

5. **Legacy Structures (REMOVED):**
   - `agg_t`, `agg_elem_t` (old aggregator)
   - `keylist_t` (replaced by RowSet)
   - `sorter_t` (replaced by vdbe_sorter_t)
   - `returnStack[100]`, `returnDepth` (Gosub uses registers)

## Error Categories from Build

### Category 1: Structure Member Errors (High Priority)
```
vdbe.c:157: 'vdbe_t' has no member 'azColName' → use aColName
vdbe.c:168: 'vdbe_t' has no member 'azResColumn' → use pResultRow
vdbe.c:690: 'vdbe_t' has no member 'pTos' → use aMem[iReg]
vdbe.c:697: 'vdbe_t' has no member 'aStack' → use aMem
vdbe.c:691: 'vdbe_t' has no member 'popStack' → removed
vdbe.c:802: 'vdbe_t' has no member 'returnStack' → removed
```

### Category 2: Legacy Aggregator (Should be Removed)
```
vdbe.c:194: incomplete type 'agg_elem_t'
vdbe.c:195: incomplete type 'agg_t'
vdbe.c:201: invalid use of 'agg_t->hash'
vdbe.c:210: invalid use of 'agg_t->pCurrent'
```

### Category 3: Mem Field Access (Union Migration)
```
vdbe.c:249: 'mem_t' has no member 'zShort' → use zMalloc
vdbe.c:250: 'mem_t' has no member 'r' → use u.r
vdbe.c:253: 'mem_t' has no member 'i' → use u.i
```

### Category 4: Hash Structure Access
```
vdbe.c:227: hash_elem->prev access pattern changed
```

## Conversion Strategy

### Phase 1: Remove Legacy Code (Priority 1)
**Functions to Remove/Rewrite:**
1. `__agg_insert()` - line 188 (legacy aggregator)
2. `__agg_in_focus()` - line 221 (legacy aggregator)
3. All references to `agg_t`, `agg_elem_t`, `keylist_t`

**Rationale:** These structures don't exist in vdbe_int.h (already modernized). Removes 100+ error lines immediately.

### Phase 2: Fix Mem Union Access (Priority 2)
**Pattern Replacements (34 occurrences):**
```c
// In __entity_as_string(), __entity_to_int(), __entity_to_real(), etc.
OLD: stack->i, stack->r, pTos->i, pTos->r
NEW: stack->u.i, stack->u.r, pTos->u.i, pTos->u.r
```

**zShort Removal (34 occurrences):**
```c
// In __entity_as_string() and similar functions
OLD: snprintf(stack->zShort, sizeof(stack->zShort), ...)
     stack->z = stack->zShort;
     stack->flags |= MEM_Short;

NEW: char buf[32];
     snprintf(buf, sizeof(buf), ...);
     if (__vdbe_mem_set_str(stack, buf, -1, DBSQL_UTF8_ENCODED, DBSQL_TRANSIENT) != 0)
         goto no_mem;
```

### Phase 3: Update API Function (Priority 3)
**Function:** `__api_step()` (line 133)

```c
OLD:
    *col_names = (const char**)vm->azColName;
    *values = (const char**)vm->azResColumn;

NEW:
    // aColName is now Mem* array, need to extract z pointers
    static const char **colNameCache = NULL;
    if (!colNameCache) {
        colNameCache = malloc(vm->nResColumn * sizeof(char*));
        for (int i = 0; i < vm->nResColumn; i++) {
            colNameCache[i] = vm->aColName[i].z;
        }
    }
    *col_names = colNameCache;

    // pResultRow is Mem* pointer
    if (vm->pResultRow) {
        static const char **valueCache = NULL;
        if (!valueCache) {
            valueCache = malloc(vm->nResColumn * sizeof(char*));
        }
        for (int i = 0; i < vm->nResColumn; i++) {
            valueCache[i] = vm->pResultRow[i].z;
        }
        *values = valueCache;
    }
```

### Phase 4: Update __vdbe_exec() Function (Priority 4)
**Main Changes:**

1. **Stack to Register Variables:**
```c
OLD:
    mem_t *pTos;               // Top of stack pointer
    pTos = p->pTos;            // Restore from vdbe_t
    pTos++; pTos--;            // Push/pop
    p->pTos = pTos;            // Save to vdbe_t

NEW:
    Mem *aMem = p->aMem;       // Register array
    // Register index is implicit in opcode operands (p1, p2, p3)
    // No need to track "top of stack" - use opcode parameters directly
```

2. **Remove popStack Logic:**
```c
OLD:
    if (p->popStack) {
        __pop_stack(&pTos, p->popStack);
        p->popStack = 0;
    }

NEW:
    // Register-based VDBE doesn't need this - remove entirely
```

3. **Remove returnStack Logic:**
```c
OLD:
    p->returnStack[p->returnDepth++] = pc+1;  // OP_Gosub
    pc = p->returnStack[--p->returnDepth];    // OP_Return

NEW:
    // OP_Gosub now uses register to store return address
    // Implementation depends on opcode parameters
```

4. **Fix Assertions:**
```c
OLD:
    DBSQL_ASSERT(pTos <= &p->aStack[pc]);

NEW:
    // Remove - no longer meaningful in register architecture
```

### Phase 5: Convert Opcode Implementations (Priority 5)
**239 pTos references across ~150 opcodes**

Each opcode case needs review and potential conversion:

**Example - OP_Integer:**
```c
OLD:
    case OP_Integer: {
        pTos++;
        pTos->i = pOp->p1;
        pTos->flags = MEM_Int;
        break;
    }

NEW:
    case OP_Integer: {
        int iReg = pOp->p2;  // Target register from operand
        aMem[iReg].u.i = pOp->p1;
        aMem[iReg].flags = MEM_Int;
        break;
    }
```

**Example - OP_Add (binary operation):**
```c
OLD:
    case OP_Add: {
        Mem *pTos2 = &pTos[-1];  // Second operand
        if ((pTos->flags & pTos2->flags & MEM_Int) != 0) {
            pTos2->i += pTos->i;  // Result in pTos2
        } else {
            __entity_to_real(pTos);
            __entity_to_real(pTos2);
            pTos2->r += pTos->r;
            pTos2->flags = MEM_Real;
        }
        __entity_release_mem(pTos);
        pTos--;  // Pop top, result stays in new top
        break;
    }

NEW:
    case OP_Add: {
        int iReg1 = pOp->p1;  // First operand register
        int iReg2 = pOp->p2;  // Second operand register
        int iDest = pOp->p3;  // Destination register
        Mem *pMem1 = &aMem[iReg1];
        Mem *pMem2 = &aMem[iReg2];
        Mem *pOut = &aMem[iDest];

        if ((pMem1->flags & pMem2->flags & MEM_Int) != 0) {
            pOut->u.i = pMem1->u.i + pMem2->u.i;
            pOut->flags = MEM_Int;
        } else {
            __entity_to_real(pMem1);
            __entity_to_real(pMem2);
            pOut->u.r = pMem1->u.r + pMem2->u.r;
            pOut->flags = MEM_Real;
        }
        break;
    }
```

### Phase 6: Control Flow Updates (Priority 6)
**Add missing label:**
```c
// Before vdbe_halt label (line 5936), add:
vdbe_return:
    // Normal completion path
    // (may need logic here depending on opcode usage)
```

## Implementation Checklist

### Pre-Conversion Preparation
- [ ] Backup vdbe.c to vdbe.c.stack_based
- [ ] Review vdbe_int.h to confirm all structures match expectations
- [ ] Check hash.h for hash_elem structure definition
- [ ] Review SQLite 3.52.0 vdbe.c for reference patterns

### Phase 1: Remove Legacy Aggregator (Day 1-2)
- [ ] Remove `__agg_insert()` function (line 188-212)
- [ ] Remove `__agg_in_focus()` function (line 221-234)
- [ ] Grep for all `agg_t`, `agg_elem_t` references
- [ ] Comment out all aggregator code with `/* LEGACY AGG - REMOVED */`
- [ ] Compile and verify error count reduction

### Phase 2: Fix Mem Union Access (Day 3-4)
- [ ] Update `__entity_as_string()` (line 244)
  - [ ] Replace `stack->r` with `stack->u.r`
  - [ ] Replace `stack->i` with `stack->u.i`
  - [ ] Replace zShort buffer with zMalloc approach
- [ ] Update `__entity_to_int()` (line 362)
  - [ ] Replace `stack->i` with `stack->u.i`
  - [ ] Replace `stack->r` with `stack->u.r`
- [ ] Update `__entity_to_real()` (line 387)
  - [ ] Replace `stack->r` with `stack->u.r`
  - [ ] Replace `stack->i` with `stack->u.i`
- [ ] Update `__vdbe_boolean_value()` (line 410)
- [ ] Compile and verify union access errors resolved

### Phase 3: Fix API Function (Day 5)
- [ ] Update `__api_step()` (line 133)
  - [ ] Convert azColName to aColName Mem* access
  - [ ] Convert azResColumn to pResultRow Mem* access
  - [ ] Handle Mem→char* conversion for API
- [ ] Test API compatibility

### Phase 4: Update __vdbe_exec() Skeleton (Day 6-7)
- [ ] Remove pTos variable declaration (line 670)
- [ ] Add aMem local variable
- [ ] Remove popStack logic (line 691-693)
- [ ] Update magic number checks
- [ ] Remove aStack assertions (line 697)
- [ ] Remove pTos save/restore (line 690, 5944)
- [ ] Compile and fix basic structural issues

### Phase 5: Convert Opcodes (Day 8-50)
**Strategy: Convert in functional groups**

Group 1: Simple Data Movement (10 opcodes)
- [ ] OP_Integer, OP_String, OP_Null, OP_Variable
- [ ] OP_Move, OP_Copy, OP_SCopy

Group 2: Arithmetic/Logic (15 opcodes)
- [ ] OP_Add, OP_Subtract, OP_Multiply, OP_Divide, OP_Remainder
- [ ] OP_And, OP_Or, OP_Not
- [ ] OP_BitAnd, OP_BitOr, OP_ShiftLeft, OP_ShiftRight

Group 3: Comparison (12 opcodes)
- [ ] OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge
- [ ] OP_IsNull, OP_NotNull, OP_IsType

Group 4: Control Flow (8 opcodes)
- [ ] OP_Goto, OP_If, OP_IfNot, OP_IfNeg, OP_IfPos
- [ ] OP_Gosub, OP_Return (needs register-based return address)
- [ ] OP_Halt

Group 5: Function Calls (5 opcodes)
- [ ] OP_Function, OP_AggStep, OP_AggFinal, OP_AggValue

Group 6: Database Operations (30 opcodes)
- [ ] OP_OpenRead, OP_OpenWrite, OP_OpenEphemeral
- [ ] OP_Column, OP_MakeRecord, OP_Insert, OP_Delete
- [ ] OP_Rowid, OP_NewRowid, OP_Last, OP_Rewind
- [ ] OP_Next, OP_Prev, OP_Seek, OP_SeekGE, OP_SeekGT
- [ ] (Continue through all DB opcodes)

Group 7: Transaction/Schema (10 opcodes)
- [ ] OP_Transaction, OP_ReadCookie, OP_SetCookie
- [ ] OP_CreateBtree, OP_DropTable, OP_Clear

Group 8: Result Set (5 opcodes)
- [ ] OP_ResultRow, OP_Concat, OP_Yield

Group 9: Remaining Special Cases (70+ opcodes)
- [ ] OP_Sort, OP_SorterOpen, OP_SorterInsert, OP_SorterNext
- [ ] (All other opcodes)

**Per-Opcode Conversion Steps:**
1. Identify stack operations (pTos++, pTos--, pTos[-n])
2. Determine register operands from pOp->p1, p2, p3
3. Replace pTos pointer arithmetic with aMem[reg] indexing
4. Update .i/.r to .u.i/.u.r
5. Test compile after each group

### Phase 6: Testing (Day 51-60)
- [ ] Compile successfully (35/35 files)
- [ ] Run test suite
- [ ] Fix runtime errors
- [ ] Validate against SQLite 3.52.0 behavior
- [ ] Performance testing

## Risk Mitigation

### High Risk Areas
1. **Opcode Parameter Mapping:** Old stack model vs new register operands
   - Mitigation: Reference SQLite 3.52.0 vdbe.c for each opcode
   - Verify opcode.h definitions match expected parameters

2. **Aggregate Functions:** Complete architectural change
   - Mitigation: Check if already handled in cg_expr.c conversion (Task #24)
   - May need to implement OP_AggStep/AggFinal differently

3. **Result Row Handling:** API compatibility
   - Mitigation: Maintain backward-compatible char** interface
   - Internal conversion from Mem* to char**

4. **Memory Management:** zShort → zMalloc transition
   - Mitigation: Ensure proper allocation/deallocation
   - Watch for memory leaks in string operations

### Testing Strategy
1. **Incremental Compilation:** Test after each phase
2. **Regression Tests:** Use existing test suite
3. **Comparison Testing:** Run queries against SQLite 3.52.0
4. **Valgrind:** Check for memory leaks
5. **Performance:** Benchmark against previous version

## Success Criteria
- [ ] Zero compilation errors (35/35 files compile)
- [ ] All existing tests pass
- [ ] No memory leaks (valgrind clean)
- [ ] Performance within 5% of SQLite 3.52.0
- [ ] Code review passes

## Reference Materials
- SQLite 3.52.0 source: `vdbe.c` (register-based implementation)
- DBSQL headers: `vdbe_int.h` (already modernized)
- Previous conversions: `cg_expr.c`, `cg_insert.c` (register-based code generation)
- Build errors: Compilation output from `make`

## Timeline Estimate
- **Phase 1 (Remove Legacy):** 2 days
- **Phase 2 (Mem Union):** 2 days
- **Phase 3 (API Fix):** 1 day
- **Phase 4 (__vdbe_exec):** 2 days
- **Phase 5 (Opcodes):** 43 days (6 weeks)
- **Phase 6 (Testing):** 10 days

**Total: ~60 days (12 weeks) of focused work**

*This assumes 2-3 hours per day, targeting 100-150 total hours*
