# Phase 2 VDBE Implementation Plan

**Task**: Rewrite core opcodes from stack-based to register-based architecture

## Phase 2 Scope (from vdbe_analysis_report.md)

### Register Load/Store
- OP_Integer: Load integer constant into register
- OP_Real: Load real constant into register
- OP_String8: Load UTF-8 string into register
- OP_Null: Set register(s) to NULL
- OP_Copy: Copy register to register (with range support)
- OP_Move: Move register to register (destructive, with range support)

### Arithmetic
- OP_Add: r[P3] = r[P1] + r[P2] (with overflow to float)
- OP_Subtract: r[P3] = r[P2] - r[P1]
- OP_Multiply: r[P3] = r[P1] * r[P2]
- OP_Divide: r[P3] = r[P2] / r[P1]
- OP_Remainder: r[P3] = r[P2] % r[P1]

### Comparison
- OP_Eq: if r[P1] == r[P3] goto P2 (with NULL handling, collation)
- OP_Ne: if r[P1] != r[P3] goto P2
- OP_Lt: if r[P1] < r[P3] goto P2
- OP_Le: if r[P1] <= r[P3] goto P2
- OP_Gt: if r[P1] > r[P3] goto P2
- OP_Ge: if r[P1] >= r[P3] goto P2

### Logic
- OP_And: r[P3] = r[P1] AND r[P2]
- OP_Or: r[P3] = r[P1] OR r[P2]
- OP_Not: r[P2] = NOT r[P1]
- OP_IsNull: if r[P1] is NULL goto P2
- OP_NotNull: if r[P1] is not NULL goto P2

### Control Flow
- OP_Goto: unconditional jump to P2
- OP_If: if r[P1] is true goto P2
- OP_IfNot: if r[P1] is false goto P2

### Result
- OP_ResultRow: output registers P1..P1+P2-1 as result row

## Implementation Strategy

1. **Preserve existing code** - Don't delete the old implementation yet
2. **Create new __vdbe_exec2()** - New register-based interpreter
3. **Add register helpers** - Functions to access and manipulate registers
4. **Implement Phase 2 opcodes** - Start with simplest (Integer, Null, Copy, Move)
5. **Add arithmetic** - Implement with proper overflow handling
6. **Add comparisons** - Implement with collation and NULL handling
7. **Add control flow** - Implement with proper branching
8. **Test incrementally** - Verify each opcode works before moving on

## Key Architectural Changes

### From Stack to Registers
```c
// OLD (stack-based):
pTos++;
pTos->i = pOp->p1;
pTos->flags = MEM_Int;

// NEW (register-based):
pOut = &aMem[pOp->p2];
pOut->u.i = pOp->p1;
MemSetTypeFlag(pOut, MEM_Int);
```

### Register Access Macros (from SQLite)
```c
#define out2Prerelease(P,O)  (&aMem[(O)->p2])
#define in1              (&aMem[pOp->p1])
#define in2              (&aMem[pOp->p2])
#define in3              (&aMem[pOp->p3])
#define out3             (&aMem[pOp->p3])
```

### Main Loop Changes
- Remove: pTos pointer and stack management
- Add: aMem register array access
- Add: Register prerelease optimization
- Add: Memory change tracking (for debug)

## File Changes Required

### src/vdbe.c
1. Update __vdbe_exec() function signature (keep Vdbe* parameter)
2. Replace stack locals with register locals
3. Rewrite opcode implementations
4. Update helper functions

### Helper Functions Needed
- `__vdbe_mem_copy()` - copy Mem to Mem
- `__vdbe_mem_move()` - move Mem to Mem (destructive)
- `__vdbe_mem_setint64()` - set i64 value
- `__vdbe_mem_setreal()` - set double value
- `__vdbe_mem_setstr()` - set string value
- `__vdbe_mem_setnull()` - set NULL value
- `__vdbe_mem_compare()` - compare two Mem values with collation
- `__vdbe_mem_numerify()` - convert to numeric type
- `__vdbe_mem_intvalue()` - extract integer value
- `__vdbe_mem_realvalue()` - extract real value
- `__vdbe_mem_booleanvalue()` - extract boolean value

## Testing Approach

1. **Unit test each opcode** - Create minimal VDBE programs
2. **Start with simple queries** - SELECT 1, SELECT 1+2, etc.
3. **Add comparisons** - SELECT 1 WHERE 1=1
4. **Add logic** - SELECT 1 WHERE 1=1 AND 2=2
5. **Test NULL handling** - SELECT NULL, SELECT 1 WHERE NULL
6. **Test type conversions** - SELECT '1' + 2

## Success Criteria

Phase 2 is complete when:
1. All 20 core opcodes are implemented
2. Simple arithmetic queries work (SELECT 1+2*3)
3. Comparison queries work (SELECT 1 WHERE 2>1)
4. Logic queries work (SELECT 1 WHERE 1=1 AND 2=2)
5. NULL handling works correctly
6. No memory leaks in register operations
7. Test suite passes for covered opcodes

## Notes

- Preserve backward compatibility where possible
- Use existing SM (storage manager) API - don't modify yet
- Focus on correctness, not optimization
- Document any deviations from SQLite 3.52.0 semantics
- Keep legacy opcodes functional for now (will remove in later phase)
