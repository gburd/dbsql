# VDBE Phase 3-4: Arithmetic and Comparison Opcodes Conversion

## Task Summary
Convert 40 arithmetic and comparison opcodes from stack-based (pTos) to register-based (aMem) architecture.

## Progress Overview

### ✅ Already Converted (Before This Task)
These opcodes were already register-based:

**Arithmetic (5 opcodes):**
1. OP_Add (lines 1302-1401) - ✅ DONE
2. OP_Subtract (lines 1302-1401) - ✅ DONE
3. OP_Multiply (lines 1302-1401) - ✅ DONE
4. OP_Divide (lines 1302-1401) - ✅ DONE
5. OP_Remainder (lines 1302-1401) - ✅ DONE

**Control Flow (2 opcodes):**
6. OP_If (line 2118) - ✅ DONE (uses aMem[pOp->p1])
7. OP_IfNot (line 2128) - ✅ DONE (uses aMem[pOp->p1])

### ✅ Converted in This Session

**Bitwise Operations (4 opcodes):**
8. OP_BitAnd (lines 1488-1512) - ✅ CONVERTED
9. OP_BitOr (lines 1488-1512) - ✅ CONVERTED
10. OP_ShiftLeft (lines 1488-1512) - ✅ CONVERTED
11. OP_ShiftRight (lines 1488-1512) - ✅ CONVERTED

**Immediate Operations (1 opcode):**
12. OP_AddImm (line 1521) - ✅ CONVERTED (needs P1/P2 order fix)

**Simple Data Operations (converted by co-agent, 3 opcodes):**
13. OP_Integer (line 906) - ✅ CONVERTED
14. OP_String (line 924) - ✅ CONVERTED
15. OP_Dup (line 998) - ✅ CONVERTED

### ⏳ Still Need Conversion (Stack-Based with pTos)

**Comparison Opcodes (6 opcodes):**
16. OP_Eq (lines 1733-1786) - ❌ USES pTos, pNos
17. OP_Ne (lines 1733-1786) - ❌ USES pTos, pNos
18. OP_Lt (lines 1733-1786) - ❌ USES pTos, pNos
19. OP_Le (lines 1733-1786) - ❌ USES pTos, pNos
20. OP_Gt (lines 1733-1786) - ❌ USES pTos, pNos
21. OP_Ge (lines 1733-1786) - ❌ USES pTos, pNos

**Unary Operations (4 opcodes):**
22. OP_Negative (lines 2029-2055) - ❌ USES pTos
23. OP_AbsValue (lines 2029-2055) - ❌ USES pTos
24. OP_Not (lines 2063-2071) - ❌ USES pTos
25. OP_BitNot (lines 2079-2087) - ❌ USES pTos

**NULL Checks (2 opcodes):**
26. OP_IsNull (lines 2144-2161) - ❌ USES pTos, stack operations
27. OP_NotNull (lines 2169-2184) - ❌ USES pTos, stack operations

**String/Data Operations (remaining):**
28. OP_Concat (lines 1198+) - ❌ USES pTos
29. OP_Pull - ❌ USES pTos
30. OP_Pop (line 971) - ✅ CONVERTED (no-op in register-based)

**Other Operations:**
31. OP_MustBeInt (lines 1577+) - ❌ USES pTos
32. OP_ForceInt (lines 1540+) - ❌ USES pTos

## Conversion Patterns

### Binary Operators (e.g., OP_Add, OP_BitAnd)
```c
// Code generator uses: __vdbe_add_op3(v, OP, r2, r1, target)
case OP_BitAnd: {
    mem_t *pIn1 = &aMem[pOp->p2];  // First operand
    mem_t *pIn2 = &aMem[pOp->p1];  // Second operand
    mem_t *pOut = &aMem[pOp->p3];  // Result

    // Type conversions
    __entity_to_int(pIn1);
    __entity_to_int(pIn2);

    // Operation
    i64 result = pIn2->u.i & pIn1->u.i;

    // Store result
    __vdbe_mem_set_int64(pOut, result);
    break;
}
```

### Unary Operators (e.g., OP_Not, OP_Negative)
```c
// Code generator uses: __vdbe_add_op(v, OP, source, target)
case OP_Not: {
    mem_t *pIn = &aMem[pOp->p1];
    mem_t *pOut = &aMem[pOp->p2];  // Or same register if in-place

    __entity_to_int(pIn);
    __vdbe_mem_set_int64(pOut, !pIn->u.i);
    break;
}
```

### Comparison Operators (e.g., OP_Eq, OP_Lt)
```c
// Code generator pattern TBD - check cg_expr.c
case OP_Eq: {
    mem_t *pIn1 = &aMem[pOp->p1];
    mem_t *pIn2 = &aMem[pOp->p3];
    int result;

    if ((pIn1->flags | pIn2->flags) & MEM_Null) {
        // NULL handling
    } else {
        result = __vdbe_mem_compare(pIn1, pIn2, pOp->p4.pColl);
        if (result == 0) {
            pc = pOp->p2 - 1;  // Jump if equal
        }
    }
    break;
}
```

## Issues Found

### OP_AddImm P1/P2 Order
Current implementation:
```c
mem_t *pReg = &aMem[pOp->p2];  // Wrong!
pReg->u.i += pOp->p1;
```

Should be:
```c
mem_t *pReg = &aMem[pOp->p1];  // Register
pReg->u.i += pOp->p2;           // Immediate value
```

Code generator uses: `__vdbe_add_op3(v, OP_AddImm, reg_row_count, 1, 0)`
- P1 = target register
- P2 = immediate value to add
- P3 = unused

## Next Steps

1. ✅ Fix OP_AddImm P1/P2 order
2. ⏳ Convert comparison opcodes (OP_Eq through OP_Ge) - batch of 6
3. ⏳ Convert unary ops (OP_Negative, OP_AbsValue, OP_Not, OP_BitNot) - batch of 4
4. ⏳ Convert NULL checks (OP_IsNull, OP_NotNull) - batch of 2
5. ⏳ Convert remaining ops (OP_Concat, OP_MustBeInt, OP_ForceInt) - batch of 3
6. ⏳ Test compilation after each batch
7. ⏳ Update opcode comments to reflect register-based semantics

## Key Helper Functions

- `__vdbe_mem_set_int64(mem_t*, i64)` - Set integer value
- `__vdbe_mem_set_double(mem_t*, double)` - Set real value
- `__vdbe_mem_set_null(mem_t*)` - Set NULL value
- `__entity_to_int(mem_t*)` - Convert to integer
- `__entity_to_real(mem_t*)` - Convert to real
- `__entity_as_string(mem_t*)` - Convert to string
- `__vdbe_mem_compare(mem_t*, mem_t*, CollSeq*)` - Compare values

## Compilation Status

After BitAnd/BitOr/ShiftLeft/ShiftRight conversion:
- No errors in converted opcodes ✅
- Remaining errors are in other unconverted opcodes
- File compiles with warnings about pTos usage in unconverted opcodes

## Estimated Remaining Work

- Comparison opcodes: 2 hours (6 opcodes, similar pattern)
- Unary operations: 1 hour (4 opcodes, simple)
- NULL checks: 1 hour (2 opcodes, need careful handling)
- Remaining ops: 1 hour (3 opcodes, mixed complexity)
- Testing & fixes: 1 hour

**Total: ~6 hours remaining for Phase 3-4**

## Reference Files

- Opcode definitions: `/home/gburd/ws/dbsql/src/vdbe.c`
- Code generation: `/home/gburd/ws/dbsql/src/cg_expr.c` (lines 1396-1497)
- Conversion plan: `/home/gburd/ws/dbsql/VDBE_CONVERSION_PLAN.md`
- Quick checklist: `/home/gburd/ws/dbsql/VDBE_QUICK_CHECKLIST.md`
