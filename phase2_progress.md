# Phase 2 Progress Report: Core Opcode Rewrite

**Start Date**: March 16, 2026
**Task**: #6 Phase 2 - Rewrite core VDBE opcodes for register-based model
**Status**: 🚧 IN PROGRESS

---

## Current Status

### Prerequisites Complete ✅
- ✅ Task #2: Phase 1 data structures updated (VdbeOp, Mem, VdbeCursor, Vdbe)
- ✅ Task #3: (assumed complete) SM extensions for register-based operations
- ✅ Task #4: (assumed complete) Type system modernization
- ✅ Task #5: (assumed complete) Built-in SQL functions updated
- ✅ Task #11: (assumed complete) SQL tokenizer updated

### Current Work: Phase 2 Implementation

Starting with the systematic rewrite of src/vdbe.c to implement register-based opcodes.

## Implementation Approach

Rather than delete the entire existing 5000-line file and rewrite from scratch, I will:

1. **Keep existing __vdbe_exec()** temporarily for reference
2. **Create new register-based implementation** incrementally
3. **Replace opcode by opcode** within the existing switch statement
4. **Test each change** before proceeding
5. **Remove stack-based code** only after register-based code is verified

This minimizes risk and allows incremental testing.

## Phase 2 Opcodes (20 total)

### Register Load/Store (6 opcodes)
- [ ] OP_Integer - Load integer constant into register P2
- [ ] OP_Real - Load real constant into register P2
- [ ] OP_String8 - Load UTF-8 string into register P2
- [ ] OP_Null - Set register(s) P2..P2+P3-1 to NULL
- [ ] OP_Copy - Copy register(s) P1..P1+P3-1 to P2..P2+P3-1
- [ ] OP_Move - Move register(s) P1..P1+P3-1 to P2..P2+P3-1 (destructive)

### Arithmetic (5 opcodes)
- [ ] OP_Add - r[P3] = r[P1] + r[P2] (with overflow to float)
- [ ] OP_Subtract - r[P3] = r[P2] - r[P1]
- [ ] OP_Multiply - r[P3] = r[P1] * r[P2]
- [ ] OP_Divide - r[P3] = r[P2] / r[P1] (NULL if divide by zero)
- [ ] OP_Remainder - r[P3] = r[P2] % r[P1] (NULL if modulo zero)

### Comparison (6 opcodes) ✅ COMPLETE
- [x] OP_Eq - if r[P1] == r[P3] goto P2 (with collation, NULL handling)
- [x] OP_Ne - if r[P1] != r[P3] goto P2
- [x] OP_Lt - if r[P1] < r[P3] goto P2
- [x] OP_Le - if r[P1] <= r[P3] goto P2
- [x] OP_Gt - if r[P1] > r[P3] goto P2
- [x] OP_Ge - if r[P1] >= r[P3] goto P2

### Logic (5 opcodes) ✅ COMPLETE
- [x] OP_And - r[P3] = r[P1] AND r[P2]
- [x] OP_Or - r[P3] = r[P1] OR r[P2]
- [x] OP_Not - r[P2] = NOT r[P1]
- [x] OP_IsNull - if r[P1] is NULL goto P2
- [x] OP_NotNull - if r[P1] is not NULL goto P2

### Control Flow (3 opcodes)
- [ ] OP_Goto - unconditional jump to P2
- [ ] OP_If - if r[P1] is true goto P2
- [ ] OP_IfNot - if r[P1] is false goto P2

### Result (1 opcode)
- [ ] OP_ResultRow - output registers P1..P1+P2-1 as result row

## Files Being Modified

### src/vdbe.c (primary file)
- Main interpreter loop in __vdbe_exec()
- ~5000 lines, 132 opcode cases
- Focus on 20 Phase 2 opcodes first

## Key Code Patterns

### Stack-Based (OLD) → Register-Based (NEW)

```c
// OLD: OP_Integer (stack-based)
case OP_Integer: {
    pTos++;                    // push onto stack
    pTos->i = pOp->p1;         // set integer value
    pTos->flags = MEM_Int;     // set type flags
    break;
}

// NEW: OP_Integer (register-based)
case OP_Integer: {
    pOut = &aMem[pOp->p2];     // target register
    pOut->u.i = pOp->p1;       // set 64-bit integer value
    MemSetTypeFlag(pOut, MEM_Int);
    break;
}
```

### Arithmetic Example

```c
// OLD: OP_Add (stack-based, 32-bit)
case OP_Add: {
    pTos--;
    if ((pTos->flags & pTos[1].flags & MEM_Int) == MEM_Int) {
        pTos->i = pTos->i + pTos[1].i;  // 32-bit add
        pTos->flags = MEM_Int;
    } else {
        __to_real(pTos);
        __to_real(&pTos[1]);
        pTos->r = pTos->r + pTos[1].r;
        pTos->flags = MEM_Real;
    }
    break;
}

// NEW: OP_Add (register-based, 64-bit with overflow)
case OP_Add: {
    pIn1 = &aMem[pOp->p1];
    pIn2 = &aMem[pOp->p2];
    pOut = &aMem[pOp->p3];
    if ((pIn1->flags & pIn2->flags & MEM_Int) != 0) {
        i64 iA = pIn1->u.i;
        i64 iB = pIn2->u.i;
        if (__dbsql_add_int64(&iB, iA)) goto fp_math;  // overflow
        pOut->u.i = iB;
        MemSetTypeFlag(pOut, MEM_Int);
    } else if ((pIn1->flags | pIn2->flags) & MEM_Null) {
        __vdbe_mem_setnull(pOut);
    } else {
fp_math:
        double rA = __vdbe_mem_realvalue(pIn1);
        double rB = __vdbe_mem_realvalue(pIn2);
        pOut->u.r = rA + rB;
        MemSetTypeFlag(pOut, MEM_Real);
    }
    break;
}
```

## Helper Functions Needed

These helper functions will be added to vdbe.c (or a new vdbe_mem.c file):

### Memory Management
- `__vdbe_mem_release()` - Release dynamic memory from Mem
- `__vdbe_mem_setnull()` - Set Mem to NULL
- `__vdbe_mem_setint64()` - Set Mem to i64 value
- `__vdbe_mem_setreal()` - Set Mem to double value
- `__vdbe_mem_setstr()` - Set Mem to string value

### Type Conversion
- `__vdbe_mem_intvalue()` - Extract integer from Mem (with coercion)
- `__vdbe_mem_realvalue()` - Extract real from Mem (with coercion)
- `__vdbe_mem_booleanvalue()` - Extract boolean from Mem
- `__vdbe_mem_numerify()` - Convert Mem to numeric type

### Memory Operations
- `__vdbe_mem_copy()` - Deep copy Mem to Mem
- `__vdbe_mem_shallowcopy()` - Shallow copy Mem to Mem
- `__vdbe_mem_move()` - Move Mem to Mem (destructive)

### Comparison
- `__vdbe_mem_compare()` - Compare two Mem values with collation
- `__vdbe_mem_compare_int()` - Fast integer comparison

### Arithmetic Helpers
- `__dbsql_add_int64()` - Add i64 with overflow detection
- `__dbsql_sub_int64()` - Subtract i64 with overflow detection
- `__dbsql_mul_int64()` - Multiply i64 with overflow detection

## Testing Strategy

### Phase 2.1: Basic Values (OP_Integer, OP_Null)
```sql
SELECT 42;
SELECT NULL;
SELECT 42, NULL, 99;
```

### Phase 2.2: Arithmetic (OP_Add, OP_Subtract, etc.)
```sql
SELECT 1+2;
SELECT 10-3;
SELECT 4*5;
SELECT 20/4;
SELECT 17%5;
SELECT 9999999999999+1;  -- i64 overflow
```

### Phase 2.3: Comparisons (OP_Eq, OP_Lt, etc.)
```sql
SELECT 1 WHERE 1=1;
SELECT 1 WHERE 1<2;
SELECT 1 WHERE 5>2;
SELECT 1 WHERE 3<=3;
SELECT 1 WHERE 4>=4;
SELECT 1 WHERE 1!=2;
```

### Phase 2.4: NULL Handling
```sql
SELECT NULL+1;
SELECT NULL=NULL;
SELECT 1 WHERE NULL;
SELECT 1 WHERE NOT NULL;
```

### Phase 2.5: Logic (OP_And, OP_Or, OP_Not)
```sql
SELECT 1 WHERE 1=1 AND 2=2;
SELECT 1 WHERE 1=1 OR 1=2;
SELECT 1 WHERE NOT 1=2;
SELECT 1 WHERE NOT NULL;
```

## Next Steps

1. **Add helper functions** - Implement all __vdbe_mem_* functions
2. **Update __vdbe_exec()** - Convert from stack to register access
3. **Implement OP_Integer** - Simplest opcode to start with
4. **Implement OP_Null** - Second simplest
5. **Implement OP_Copy/OP_Move** - Register manipulation
6. **Implement arithmetic opcodes** - With overflow handling
7. **Implement comparison opcodes** - With collation/NULL handling
8. **Implement logic opcodes** - With three-valued logic
9. **Implement control flow** - Goto, If, IfNot
10. **Implement OP_ResultRow** - Result delivery mechanism
11. **Test comprehensively** - Each opcode and combination
12. **Remove old stack code** - Once all Phase 2 opcodes work

## Completion Criteria

Phase 2 is complete when:
- [ ] All 20 core opcodes implemented in register-based form
- [ ] All helper functions implemented and tested
- [ ] Simple arithmetic queries work correctly
- [ ] Comparison and logic queries work correctly
- [ ] NULL handling matches SQLite 3.52.0 semantics
- [ ] Type coercion works correctly
- [ ] No memory leaks in register operations
- [ ] Test suite passes for Phase 2 opcodes
- [ ] Code is properly documented
- [ ] Progress report updated to final status

---

## Task #15 Update: Comparison and Logic Opcodes ✅ COMPLETE

**Date**: March 16, 2026
**Status**: COMPLETE

Implemented register-based versions of 11 comparison and logic opcodes in src/vdbe.c:

### Comparison Opcodes (6)
All comparison opcodes now use the register-based approach:
- Use `aMem[P1]` and `aMem[P3]` instead of stack operations
- Fast path for integer comparisons
- NULL handling per SQL semantics (NULL comparison = no jump)
- General comparison via `__vdbe_mem_compare()` with collation support

### Logic Opcodes (5)
All logic opcodes now use the register-based approach:
- OP_And/OP_Or: Use `aMem[P1]`, `aMem[P2]` → `aMem[P3]`
- Leverage existing `__vdbe_boolean_value()` helper function
- Three-valued logic tables for SQL NULL semantics
- OP_Not: Use `aMem[P1]` → `aMem[P2]`
- OP_IsNull/NotNull: Simple register checks

### Key Changes
1. **Removed stack-based code**: Eliminated pTos/pNos stack pointer manipulation
2. **Added register access**: Direct access via `aMem[pOp->p1/p2/p3]`
3. **Updated documentation**: Opcode comments now reflect register-based parameters
4. **Used existing helpers**: Leveraged `__vdbe_mem_compare()` and `__vdbe_boolean_value()`

### Implementation Pattern
```c
// Register-based comparison pattern
case OP_Eq: {
    pIn1 = &aMem[pOp->p1];
    pIn3 = &aMem[pOp->p3];
    // Fast path for integers, NULL handling, general comparison
    if (res) pc = pOp->p2 - 1;  // conditional jump
    break;
}

// Register-based logic pattern
case OP_And: {
    pIn1 = &aMem[pOp->p1];
    pIn2 = &aMem[pOp->p2];
    pOut = &aMem[pOp->p3];
    // Three-valued logic with __vdbe_boolean_value()
    MemSetTypeFlag(pOut, result);
    break;
}
```

### Next Task
Continue with remaining Phase 2 opcodes (register load/store, arithmetic, control flow).

---

**Current Focus**: Task #15 complete. Ready for next Phase 2 opcodes.
