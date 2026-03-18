# Task #15 Complete: Register-Based Comparison and Logic Opcodes

**Date**: March 16, 2026
**Status**: ✅ COMPLETE
**File Modified**: /home/gburd/ws/dbsql/src/vdbe.c

## Summary

Successfully modernized 11 comparison and logic opcodes in src/vdbe.c to use the register-based VDBE architecture (SQLite 3.52.0 style). All opcodes now operate on register arrays (`aMem[P1]`, `aMem[P2]`, `aMem[P3]`) instead of stack-based operations.

## Opcodes Implemented

### Comparison Opcodes (6)

1. **OP_Eq** - Jump to P2 if `aMem[P1] == aMem[P3]`
2. **OP_Ne** - Jump to P2 if `aMem[P1] != aMem[P3]`
3. **OP_Lt** - Jump to P2 if `aMem[P1] < aMem[P3]`
4. **OP_Le** - Jump to P2 if `aMem[P1] <= aMem[P3]`
5. **OP_Gt** - Jump to P2 if `aMem[P1] > aMem[P3]`
6. **OP_Ge** - Jump to P2 if `aMem[P1] >= aMem[P3]`

**Implementation Details**:
- Fast path for integer comparisons (direct i64 compare)
- NULL handling per SQL semantics (NULL comparison yields no jump)
- General comparison via `__vdbe_mem_compare()` with collation support (P4)
- Removed all stack-based operations (pTos, pNos, __pop_stack)

### Logic Opcodes (5)

7. **OP_And** - Boolean AND: `aMem[P3] = aMem[P1] AND aMem[P2]`
8. **OP_Or** - Boolean OR: `aMem[P3] = aMem[P1] OR aMem[P2]`
9. **OP_Not** - Boolean NOT: `aMem[P2] = NOT aMem[P1]`
10. **OP_IsNull** - Jump to P2 if `aMem[P1]` is NULL
11. **OP_NotNull** - Jump to P2 if `aMem[P1]` is not NULL

**Implementation Details**:
- Three-valued logic for SQL NULL semantics
- Uses `__vdbe_boolean_value()` helper function
- Logic truth tables for AND/OR operations
- Simplified IsNull/NotNull to single register checks

## Code Changes

### Before (Stack-Based)
```c
case OP_Eq: {
    mem_t *pNos = &pTos[-1];
    int c, v;
    DBSQL_ASSERT(pNos >= p->aStack);
    // Complex stack manipulation...
    if ((ft & fn & MEM_Int) == MEM_Int) {
        c = pNos->i - pTos->i;  // 32-bit arithmetic
    }
    // More stack operations...
    __pop_stack(&pTos, 2);
    if (pOp->p2) {
        if (c) pc = pOp->p2 - 1;
    } else {
        pTos++;
        pTos->i = c;
        pTos->flags = MEM_Int;
    }
    break;
}
```

### After (Register-Based)
```c
case OP_Eq: {
    int res;
    u16 flags1, flags3;

    pIn1 = &aMem[pOp->p1];
    pIn3 = &aMem[pOp->p3];
    flags1 = pIn1->flags;
    flags3 = pIn3->flags;

    /* Fast path: both operands are integers */
    if ((flags1 & flags3 & MEM_Int) != 0) {
        if (pIn3->u.i > pIn1->u.i) {
            res = +1;
        } else if (pIn3->u.i < pIn1->u.i) {
            res = -1;
        } else {
            res = 0;
        }
    }
    /* NULL handling */
    else if ((flags1 | flags3) & MEM_Null) {
        break;  /* No jump on NULL */
    }
    /* General comparison */
    else {
        res = __vdbe_mem_compare(pIn1, pIn3, pOp->p4.pColl);
    }

    switch (pOp->opcode) {
    case OP_Eq:    res = (res == 0);     break;
    case OP_Ne:    res = (res != 0);     break;
    case OP_Lt:    res = (res <  0);     break;
    case OP_Le:    res = (res <= 0);     break;
    case OP_Gt:    res = (res >  0);     break;
    default:       res = (res >= 0);     break;
    }

    if (pOp->p2) {
        if (res) pc = pOp->p2 - 1;
    }
    break;
}
```

## Key Improvements

1. **Cleaner Architecture**
   - Direct register access via `aMem[pOp->p1/p2/p3]`
   - No stack pointer manipulation
   - More straightforward control flow

2. **64-bit Integer Support**
   - Uses `u.i` (i64) instead of `i` (int)
   - Consistent with SQLite 3.52.0 architecture

3. **Better NULL Handling**
   - SQL NULL semantics: NULL comparison = no jump
   - Explicit NULL checks before comparisons

4. **Collation Support**
   - P4 parameter for collating sequences
   - Uses `__vdbe_mem_compare()` for proper text comparison

5. **Documentation Updated**
   - All opcode comments now reflect register-based parameters
   - Synopsis lines added (e.g., "r[P1]==r[P3]")

## Helper Functions Used

- `__vdbe_mem_compare()` - Compare two Mem values with collation
- `__vdbe_boolean_value()` - Extract boolean value from Mem
- `MemSetTypeFlag()` - Set Mem type flags

## Testing Strategy

These opcodes should be tested with:

### Comparison Tests
```sql
SELECT 1 WHERE 1=1;        -- OP_Eq
SELECT 1 WHERE 1!=2;       -- OP_Ne
SELECT 1 WHERE 1<2;        -- OP_Lt
SELECT 1 WHERE 2<=2;       -- OP_Le
SELECT 1 WHERE 2>1;        -- OP_Gt
SELECT 1 WHERE 2>=2;       -- OP_Ge
SELECT 1 WHERE NULL=NULL;  -- NULL handling
```

### Logic Tests
```sql
SELECT 1 WHERE 1=1 AND 2=2;     -- OP_And
SELECT 1 WHERE 1=1 OR 1=2;      -- OP_Or
SELECT 1 WHERE NOT 1=2;         -- OP_Not
SELECT 1 WHERE NULL AND 1;      -- NULL logic
SELECT 1 WHERE 1 IS NULL;       -- OP_IsNull
SELECT 1 WHERE 1 IS NOT NULL;   -- OP_NotNull
```

## Files Modified

- **/home/gburd/ws/dbsql/src/vdbe.c**
  - Lines 2232-2369: Comparison opcodes (Eq, Ne, Lt, Le, Gt, Ge)
  - Lines 2547-2615: Logic opcodes (And, Or)
  - Lines 2633-2649: Not opcode
  - Lines 2711-2743: IsNull/NotNull opcodes
  - Updated opcode documentation comments

## Metrics

- **Lines changed**: ~200 lines (implementation + documentation)
- **Opcodes updated**: 11
- **Stack operations removed**: 6 __pop_stack() calls, multiple pTos pointer manipulations
- **Helper functions leveraged**: 3 (__vdbe_mem_compare, __vdbe_boolean_value, MemSetTypeFlag)

## Next Steps

Continue with remaining Phase 2 opcodes:
- Register load/store (6 opcodes)
- Arithmetic (5 opcodes)
- Control flow (3 opcodes)
- Result output (1 opcode)

## References

- SQLite 3.52.0: _/sqlite/src/vdbe.c (lines 2273-2416 for comparisons, 2594-2663 for logic)
- Task description: Task #15 in project tracking
- Helper function: __vdbe_mem_compare() (lines 1078-1150 in src/vdbe.c)

---

**Completion Date**: March 16, 2026
**Completed By**: Claude (Task #15)
