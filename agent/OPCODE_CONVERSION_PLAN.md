# Opcode Conversion Plan

**Date:** 2026-03-24
**Purpose:** Convert remaining stack-based opcodes to register-based model
**Goal:** Make CREATE TABLE, INSERT, UPDATE, DELETE work correctly

## Current Status

### Already Converted
- OP_Integer (partially - has dual mode with P2==0 fallback)
- OP_Variable (already register-based)
- OP_ResultRow (works with registers)

### Stack-Based Opcodes Found

Total opcodes using pTos++: 22 occurrences
Total opcodes using pTos--: 14 occurrences

## Priority Conversion List

### Priority 1: Critical for INSERT (Must convert first)

1. **OP_String** (line 880)
   - Current: Pushes string onto stack (pTos++)
   - Needs: Write to register P2
   - Used by: All INSERT with string values

2. **OP_NewRecno** (line 3293)
   - Current: Pushes generated rowid onto stack (pTos++)
   - Needs: Write to register P2
   - Used by: INSERT without explicit rowid

3. **OP_MakeRecord** (line 2033)
   - Current: Reads from pTos[1-nField] to pTos[0], writes to pTos
   - Needs: Read from aMem[P1] to aMem[P1+P2-1], write to aMem[P3]
   - Used by: INSERT, UPDATE (creates database record)
   - COMPLEX: 100+ lines, handles variable fields

4. **OP_Column** (line 3591)
   - Current: Pushes column value onto stack (pTos++)
   - Needs: Write to register P3
   - Used by: SELECT, WHERE clauses
   - COMPLEX: 100+ lines, reads from cursor

5. **OP_PutIntKey / OP_PutStrKey** (line 3416)
   - Current: Reads key from pTos[-1], data from pTos, pops both
   - Needs: Read key from aMem[P2], data from aMem[P3]
   - Used by: INSERT (actual write to database)

### Priority 2: Arithmetic and Comparison (For WHERE clauses)

6. **OP_Add / OP_Subtract / OP_Multiply / OP_Divide / OP_Remainder** (line 1177)
   - Current: Pop two, push result
   - Needs: Read P1, P2, write P3

7. **OP_Eq / OP_Ne / OP_Lt / OP_Le / OP_Gt / OP_Ge** (line 1559)
   - Current: Compare top two stack items, pop both
   - Needs: Compare aMem[P1] and aMem[P3]

8. **OP_And / OP_Or** (line 1805)
   - Current: Stack-based boolean operations
   - Needs: Register-based

### Priority 3: Utility Opcodes (For complex queries)

9. **OP_Dup** (line 951)
   - Current: Duplicate stack entry, push copy
   - Needs: Copy aMem[P1] to aMem[P2]

10. **OP_Concat** (line 1073)
    - Current: Stack-based string concatenation
    - Needs: Register-based

11. **OP_IsNull / OP_NotNull** (line 1970, 1995)
    - Current: Check stack top, conditional branch
    - Needs: Check aMem[P1], conditional branch

### Priority 4: Complex Operations (Later)

12. **OP_MakeKey / OP_MakeIdxKey** (line 2214)
13. **OP_RowKey / OP_RowData** (line 3524)
14. **OP_Rowid** (line 3712)
15. **OP_IdxRecno** (line 4080)

## Conversion Pattern

### Simple Opcodes (String, NewRecno, Rowid)
```c
// OLD:
case OP_Something: {
    pTos++;
    pTos->flags = MEM_Int;
    pTos->u.i = value;
    break;
}

// NEW:
case OP_Something: {
    mem_t *pOut = &aMem[pOp->p2];
    pOut->flags = MEM_Int;
    pOut->u.i = value;
    break;
}
```

### Binary Operations (Add, Eq, And)
```c
// OLD:
case OP_Add: {
    mem_t *pNos = &pTos[-1];
    // compute result
    pTos--;  // pop one
    pTos->u.i = result;  // top becomes result
    break;
}

// NEW:
case OP_Add: {
    mem_t *pIn1 = &aMem[pOp->p1];
    mem_t *pIn2 = &aMem[pOp->p2];
    mem_t *pOut = &aMem[pOp->p3];
    // compute result
    pOut->u.i = result;
    break;
}
```

### Complex Opcodes (MakeRecord, Column)
```c
// OLD:
case OP_MakeRecord: {
    nField = pOp->p1;
    pRec = &pTos[1 - nField];  // Read multiple from stack
    // ... process ...
    __pop_stack(&pTos, nField);
    pTos++;
    pTos->z = result;
    break;
}

// NEW:
case OP_MakeRecord: {
    nField = pOp->p2;           // Field count
    pRec = &aMem[pOp->p1];      // Start register
    // ... process aMem[p1] to aMem[p1+p2-1] ...
    mem_t *pOut = &aMem[pOp->p3];
    pOut->z = result;
    break;
}
```

## Testing Strategy

### After Each Conversion
```bash
nix build
```

### Milestone Tests

**Milestone 1: String and NewRecno**
```bash
result/bin/dbsql :memory: "SELECT 'hello';"
```

**Milestone 2: MakeRecord**
```bash
result/bin/dbsql :memory: "CREATE TABLE t(x);"
```

**Milestone 3: PutIntKey**
```bash
result/bin/dbsql :memory: "CREATE TABLE t(x); INSERT INTO t VALUES (42);"
```

**Milestone 4: Column**
```bash
result/bin/dbsql :memory: "CREATE TABLE t(x); INSERT INTO t VALUES (42); SELECT * FROM t;"
```

**Milestone 5: Arithmetic**
```bash
result/bin/dbsql :memory: "CREATE TABLE t(x); INSERT INTO t VALUES (1); INSERT INTO t VALUES (2); SELECT x+10 FROM t;"
```

## Implementation Order

1. Convert OP_String (simple, high impact)
2. Convert OP_NewRecno (simple, required for INSERT)
3. Convert OP_MakeRecord (complex, critical for INSERT)
4. Convert OP_PutIntKey/PutStrKey (medium, writes to DB)
5. Test INSERT
6. Convert OP_Column (complex, required for SELECT after INSERT)
7. Test full INSERT + SELECT cycle
8. Convert comparison ops (for WHERE)
9. Convert arithmetic ops (for expressions)
10. Run full smoke tests

## Success Criteria

- CREATE TABLE works
- INSERT works
- SELECT after INSERT returns correct data
- No segfaults or memory corruption
- Clean exit codes
- At least 5 of 7 smoke tests pass
