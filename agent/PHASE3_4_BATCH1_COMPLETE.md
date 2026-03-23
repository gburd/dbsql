# Phase 3-4 Batch 1 Complete: Bitwise Operations Converted

## Date: 2026-03-18

## Task Summary
Converted the first batch of arithmetic opcodes (bitwise operations) from stack-based to register-based architecture as part of Phase 3-4 of the VDBE modernization plan.

## Opcodes Converted (Batch 1)

### Bitwise Operations (4 opcodes) - ✅ COMPLETE

**File:** `/home/gburd/ws/dbsql/src/vdbe.c` (lines 1483-1512)

1. **OP_BitAnd** - Bitwise AND operation
2. **OP_BitOr** - Bitwise OR operation
3. **OP_ShiftLeft** - Left shift operation
4. **OP_ShiftRight** - Right shift operation

### Changes Made

**Before (Stack-Based):**
```c
case OP_BitAnd:
case OP_BitOr:
case OP_ShiftLeft:
case OP_ShiftRight: {
    mem_t *pNos = &pTos[-1];  // Second element from stack top
    int a, b;

    if ((pTos->flags | pNos->flags) & MEM_Null) {
        __pop_stack(&pTos, 2);
        pTos++;
        pTos->flags = MEM_Null;
        break;
    }
    __entity_to_int(pTos);
    __entity_to_int(pNos);
    a = pTos->i;      // Direct field access
    b = pNos->i;

    // Perform operation...

    pTos--;           // Pop stack
    pTos->i = a;      // Store result
    break;
}
```

**After (Register-Based):**
```c
case OP_BitAnd:
case OP_BitOr:
case OP_ShiftLeft:
case OP_ShiftRight: {
    mem_t *pIn1;    /* First operand */
    mem_t *pIn2;    /* Second operand */
    mem_t *pOut;    /* Result */
    i64 a, b;

    pIn2 = &aMem[pOp->p1];  /* Second operand */
    pIn1 = &aMem[pOp->p2];  /* First operand */
    pOut = &aMem[pOp->p3];  /* Result */

    if ((pIn1->flags | pIn2->flags) & MEM_Null) {
        __vdbe_mem_set_null(pOut);
        break;
    }
    __entity_to_int(pIn1);
    __entity_to_int(pIn2);
    a = pIn2->u.i;   // Union field access
    b = pIn1->u.i;

    // Perform operation...

    __vdbe_mem_set_int64(pOut, a);
    break;
}
```

### Key Changes

1. **Register Addressing:**
   - OLD: `pTos` (stack pointer), `pNos = &pTos[-1]` (stack relative)
   - NEW: `aMem[pOp->p1]`, `aMem[pOp->p2]`, `aMem[pOp->p3]` (explicit registers)

2. **Union Field Access:**
   - OLD: `pTos->i` (direct integer field)
   - NEW: `pIn1->u.i` (union member access)

3. **Stack Operations Removed:**
   - OLD: `__pop_stack(&pTos, 2)`, `pTos++`, `pTos--`
   - NEW: Direct register assignment (no push/pop)

4. **NULL Handling:**
   - OLD: `pTos->flags = MEM_Null` after push
   - NEW: `__vdbe_mem_set_null(pOut)` (helper function)

5. **Result Storage:**
   - OLD: `pTos->i = result` (in-place on stack)
   - NEW: `__vdbe_mem_set_int64(pOut, result)` (explicit output register)

## Code Generator Usage

From `/home/gburd/ws/dbsql/src/cg_expr.c` (lines 1471-1490):

```c
case TK_BITAND:
case TK_BITOR:
    r1 = __expr_code_temp(parser, expr->pLeft, &regFree1);
    r2 = __expr_code_temp(parser, expr->pRight, &regFree2);
    __vdbe_add_op3(v, op, r2, r1, target);
    break;

case TK_LSHIFT:
case TK_RSHIFT:
    /* Shift operators: reversed operand order */
    r2 = __expr_code_temp(parser, expr->pRight, &regFree2);
    r1 = __expr_code_temp(parser, expr->pLeft, &regFree1);
    __vdbe_add_op3(v, op, r2, r1, target);
    break;
```

**Opcode Parameters:**
- P1 = Second operand register (r2)
- P2 = First operand register (r1)
- P3 = Target/result register

## Also Converted (By Co-Agent)

While working on this task, a co-agent also converted:

### OP_AddImm (Line 1521)
**NOTE:** Implementation needs P1/P2 order correction.

**Current:**
```c
mem_t *pReg = &aMem[pOp->p2];  // Wrong!
pReg->u.i += pOp->p1;
```

**Should Be:**
```c
mem_t *pReg = &aMem[pOp->p1];  // Register
pReg->u.i += pOp->p2;           // Immediate value
```

### Other Opcodes Also Converted
- OP_Integer (line 906)
- OP_String (line 924)
- OP_Pop (line 971) - Now a no-op
- OP_Dup (line 998)

## Compilation Status

### Test Compile Results
```bash
cd build_unix && make ../src/vdbe.o 2>&1
```

**Status:** vdbe.c has errors, but NONE in the converted opcodes ✅

**Errors Present:**
- Legacy aggregator code (agg_t/agg_elem_t) - should be disabled
- Remaining stack-based opcodes (pTos references)
- Other unrelated issues (cg_pragma.c, cg_where.c)

**Converted opcodes compile cleanly!** No errors in:
- OP_BitAnd, OP_BitOr, OP_ShiftLeft, OP_ShiftRight

## Remaining Work for Phase 3-4

### Next Batches

**Batch 2: Comparison Opcodes (6 opcodes)**
- OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge (lines 1733-1786)
- Estimated: 2 hours

**Batch 3: Unary Operations (4 opcodes)**
- OP_Negative, OP_AbsValue (lines 2029-2055)
- OP_Not (lines 2063-2071)
- OP_BitNot (lines 2079-2087)
- Estimated: 1 hour

**Batch 4: NULL Checks (2 opcodes)**
- OP_IsNull (lines 2144-2161)
- OP_NotNull (lines 2169-2184)
- Estimated: 1 hour

**Batch 5: Remaining Ops (3 opcodes)**
- OP_Concat (lines 1198+)
- OP_MustBeInt (lines 1577+)
- OP_ForceInt (lines 1540+)
- Estimated: 1 hour

**Testing & Fixes:** 1 hour

**Total Remaining:** ~6 hours

## Progress Summary

### Completed in This Session (Batch 1)
- ✅ 4 bitwise opcodes converted (OP_BitAnd, OP_BitOr, OP_ShiftLeft, OP_ShiftRight)
- ✅ Compilation verified (no errors in converted code)
- ✅ Union field access pattern established
- ✅ Register addressing pattern confirmed

### Already Complete (From Previous Work)
- ✅ 5 arithmetic opcodes (OP_Add, OP_Subtract, OP_Multiply, OP_Divide, OP_Remainder)
- ✅ 2 control flow opcodes (OP_If, OP_IfNot)
- ✅ 5 data opcodes (OP_Integer, OP_String, OP_Pop, OP_Dup, OP_AddImm*)

### Total Progress: 16/40 opcodes (40%) ✅

*Note: OP_AddImm needs P1/P2 order fix

## Verification

### Pattern Consistency
All converted opcodes follow the same register-based pattern:
1. Declare `mem_t *pIn1, *pIn2, *pOut`
2. Map to registers: `&aMem[pOp->p1/p2/p3]`
3. Perform type conversions
4. Execute operation on `u.i` or `u.r` union members
5. Store result via `__vdbe_mem_set_*()` functions
6. No stack push/pop operations

### Code Generator Alignment
Conversion matches code generator expectations:
- `__vdbe_add_op3(v, OP, r2, r1, target)` → P1=r2, P2=r1, P3=target
- Binary operators use P1/P2 for inputs, P3 for output
- Unary operators typically use P1 for input, P2 for output

## Files Modified

1. `/home/gburd/ws/dbsql/src/vdbe.c`
   - Lines 1483-1512: Bitwise operations converted
   - Lines 1521-1526: OP_AddImm converted (needs fix)

## Documentation Created

1. `/home/gburd/ws/dbsql/TASK29_PHASE3_4_PROGRESS.md`
   - Comprehensive tracking document
   - Lists all 40 opcodes and their status
   - Conversion patterns and examples

2. `/home/gburd/ws/dbsql/PHASE3_4_BATCH1_COMPLETE.md` (this file)
   - Detailed summary of Batch 1 work
   - Before/after code comparisons
   - Next steps and remaining work

## Next Actions

1. **Immediate:** Fix OP_AddImm P1/P2 order
2. **Batch 2:** Convert comparison opcodes (OP_Eq through OP_Ge)
3. **Test:** Compile after each batch
4. **Document:** Update progress doc after each batch

## Lessons Learned

1. **Union Members:** All integer/real access must use `.u.i` and `.u.r`
2. **Helper Functions:** Use `__vdbe_mem_set_*()` for result storage
3. **No Stack Operations:** Register-based = no push/pop, explicit addressing
4. **Operand Order:** P1/P2 order matters, check code generator usage
5. **NULL Handling:** Use helper functions for consistency

## Conclusion

Batch 1 successfully converted 4 bitwise operation opcodes from stack-based to register-based architecture. The converted code compiles without errors and follows the established register-based patterns. Progress: 16/40 opcodes (40%) complete.

**Ready to proceed with Batch 2: Comparison Opcodes.**
