# Phase 5 Database Operations Conversion - Detailed Implementation Plan

**Date**: 2026-03-18
**Status**: Ready to begin
**Prerequisite**: Agent 3 work complete ✅
**File**: `/home/gburd/ws/dbsql/src/vdbe.c`

## Executive Summary

The VDBE currently has **425 stack-based opcode operations** (pTos/pNos references) that need conversion to **register-based** operations (aMem[reg]). Phase 3 cursor navigation opcodes have been completed (~73% of cursor ops done). The remaining work focuses on data manipulation, which is more complex due to multi-register operations.

## Current State Analysis

### What's Been Done (Phase 3 work)
- ✅ Cursor navigation: Rewind, Next, Prev, Last, SeekGE/GT/LE/LT
- ✅ Cursor management: OpenRead, OpenWrite, Close, Delete
- ✅ Transaction control: Transaction, Commit, Rollback
- ✅ Data access: Rowid (converted with aMem[pOp->p2])

### What Needs Conversion (Phase 5 work)
- ❌ **Column extraction**: OP_Column (most critical - used in every SELECT)
- ❌ **Record creation**: OP_MakeRecord (critical - used in every INSERT/UPDATE)
- ❌ **Data insertion**: OP_PutIntKey/PutStrKey (critical - used in every INSERT)
- ❌ **Rowid generation**: OP_NewRowid (critical - used in INSERT)
- ❌ **Index operations**: OP_IdxInsert, OP_IdxDelete, OP_MakeIdxKey, OP_IdxRowid, OP_IdxGE/GT/LT/LE
- ❌ **Seek/existence**: OP_SeekRowid, OP_NotExists, OP_Found/NotFound
- ❌ **Data retrieval**: OP_RowData, OP_FullKey
- ❌ **Schema operations**: OP_CreateTable, OP_CreateIndex
- ❌ **Sorting**: OP_SorterInsert, OP_SorterData, OP_Sort
- ❌ **Miscellaneous**: OP_Sequence, OP_KeyAsData

## Implementation Priority Queue

### Batch 1: Core SELECT Support (6-8 hours)
**Goal**: Make SELECT statements work

1. **OP_Column** (line 3786) - HIGHEST PRIORITY
   - Current: Uses `pTos++`, `pTos->z`, `pTos->zShort`, `pTos->flags`
   - Need: Use `aMem[pOp->p3]` for output
   - Pattern: Similar to OP_Rowid (already done)
   - Impact: Used in EVERY SELECT statement
   - Complexity: HIGH (handles pseudo-tables, null rows, key-as-data)

### Batch 2: Core INSERT Support (8-10 hours)
**Goal**: Make INSERT statements work

2. **OP_NewRowid** (line 3470)
   - Current: Uses `pTos++` for output
   - Need: Use `aMem[pOp->p2]` for output
   - Complexity: MEDIUM (generates unique rowids)

3. **OP_MakeRecord** (line 2212)
   - Current: Uses `pRec = &pTos[1-nField]`, `__pop_stack(&pTos, nField)`, `pTos++`
   - Need: Use register range `aMem[pOp->p2..p2+p1-1]` for inputs, `aMem[pOp->p3]` for output
   - Complexity: HIGH (processes multiple registers, complex encoding)

4. **OP_PutIntKey / OP_PutStrKey** (line 3599)
   - Current: Uses `pNos = &pTos[-1]`, `pTos`, `__pop_stack(&pTos, 2)`
   - Need: Use `aMem[pOp->p2]` for key, `aMem[pOp->p3]` for data
   - Complexity: MEDIUM (handles pseudo-tables, updates stats)

### Batch 3: Index Support (4-6 hours)
**Goal**: Make indexed queries and UPDATE/DELETE work

5. **OP_MakeIdxKey** (line 2391)
   - Similar to OP_MakeRecord
   - Complexity: HIGH

6. **OP_IdxInsert** (line 4193)
   - Current: Uses `pTos` for key
   - Need: Use `aMem[pOp->p2]` for key
   - Complexity: LOW

7. **OP_IdxDelete** (line 4243)
   - Similar to OP_IdxInsert
   - Complexity: LOW

8. **OP_IdxRowid** (line 4271)
   - Current: Uses `pTos++` for output
   - Need: Use `aMem[pOp->p2]` for output
   - Complexity: LOW

### Batch 4: Seek & Existence Checks (3-4 hours)
**Goal**: Make WHERE clauses with indexes work

9. **OP_SeekRowid** (line 3273)
   - Current: Partially uses pTos
   - Need: Use `aMem[pOp->p3]` for rowid
   - Complexity: MEDIUM

10. **OP_Found / OP_NotFound** (line 3062-3063)
    - Current: Uses `pTos` for key
    - Need: Use `aMem[pOp->p4]` for key
    - Complexity: MEDIUM

11. **OP_NotExists** (line 3440)
    - Current: Uses `pTos` for key
    - Need: Use `aMem[pOp->p3]` for key
    - Complexity: MEDIUM

12. **OP_IdxGE / OP_IdxGT / OP_IdxLT / OP_IdxLE** (line 4322-4323)
    - Current: Uses `pTos` for comparison
    - Need: Use `aMem[pOp->p3]` for key
    - Complexity: MEDIUM

### Batch 5: Auxiliary Data Operations (2-3 hours)
**Goal**: Complete data access functionality

13. **OP_RowData** (line 3722)
    - Current: Uses `pTos++` for output
    - Need: Use `aMem[pOp->p2]` for output
    - Complexity: LOW

14. **OP_FullKey** (line 3963)
    - Current: Uses `pTos++` for output
    - Need: Use `aMem[pOp->p2]` for output
    - Complexity: LOW

15. **OP_Sequence** (line 5670)
    - Current: Uses `pTos++` for output
    - Need: Use `aMem[pOp->p2]` for output
    - Complexity: LOW

### Batch 6: Sorting Support (3-4 hours)
**Goal**: Make ORDER BY work

16. **OP_SorterInsert** (line 5518)
    - Current: Uses `pTos` for data
    - Need: Use `aMem[pOp->p2]` for data
    - Complexity: MEDIUM

17. **OP_SorterData** (line 5596)
    - Current: Uses `pTos++` for output
    - Need: Use `aMem[pOp->p2]` for output
    - Complexity: MEDIUM

18. **OP_Sort** (line 4712) - LEGACY
    - Current: Uses `pTos` extensively
    - Need: May be deprecated, check if still used
    - Complexity: MEDIUM-HIGH

### Batch 7: Schema Operations (1-2 hours)
**Goal**: Make DDL statements work

19. **OP_CreateTable / OP_CreateIndex** (line 4444-4445)
    - Current: Uses `pTos++` for root page output
    - Need: Use `aMem[pOp->p2]` for output
    - Complexity: LOW

## Detailed Conversion Patterns

### Pattern 1: Single Output Register

**OLD Stack-based OP_Example**:
```c
case OP_Example: {
    int iCursor = pOp->p1;
    cursor_t *pC = p->cursors[iCursor];

    pTos++;  // Push onto stack
    pTos->i = computed_value;
    pTos->flags = MEM_Int;
    break;
}
```

**NEW Register-based OP_Example**:
```c
case OP_Example: {
    int iCursor = pOp->p1;
    int iDest = pOp->p2;      // Destination register (new parameter)
    cursor_t *pC = p->cursors[iCursor];
    mem_t *pOut;

    DBSQL_ASSERT(iDest > 0 && iDest <= p->nMem);
    pOut = &aMem[iDest];

    pOut->u.i = computed_value;  // Note: .u.i not .i (union member)
    pOut->flags = MEM_Int;
    break;
}
```

**Changes**:
- Remove `pTos++`
- Add destination register parameter (usually P2 or P3)
- Use `aMem[iDest]` instead of `pTos`
- Use `.u.i` instead of `.i` (union member)
- Add ASSERT for register bounds

### Pattern 2: Two Input Registers (Binary Operation)

**OLD Stack-based OP_Insert**:
```c
case OP_Insert: {
    mem_t *pNos = &pTos[-1];  // Second item on stack
    mem_t *pData = pTos;       // Top of stack

    // Use pNos as key, pData as data
    __sm_insert(cursor, pNos->z, pNos->n, pData->z, pData->n);

    __pop_stack(&pTos, 2);  // Pop both
    break;
}
```

**NEW Register-based OP_Insert**:
```c
case OP_Insert: {
    int iKey = pOp->p2;   // Key register
    int iData = pOp->p3;  // Data register
    mem_t *pKey = &aMem[iKey];
    mem_t *pData = &aMem[iData];

    // Use pKey, pData
    __sm_insert(cursor, pKey->z, pKey->n, pData->z, pData->n);

    // No popping needed - registers persist
    break;
}
```

**Changes**:
- Replace `pTos[-1]` with `aMem[pOp->p2]`
- Replace `pTos` with `aMem[pOp->p3]`
- Remove `__pop_stack()` call
- Registers persist (no cleanup needed)

### Pattern 3: Register Range (MakeRecord)

**OLD Stack-based OP_MakeRecord**:
```c
case OP_MakeRecord: {
    int nField = pOp->p1;  // Number of fields
    mem_t *pRec = &pTos[1 - nField];  // Start N fields down from TOS

    for (i = 0; i < nField; i++, pRec++) {
        // Process pRec (iterates up toward pTos)
        process_field(pRec);
    }

    // Create record in zNewRecord

    __pop_stack(&pTos, nField);  // Pop all inputs
    pTos++;                       // Push result
    pTos->z = zNewRecord;
    pTos->n = nByte;
    pTos->flags = MEM_Str | MEM_Dyn;
    break;
}
```

**NEW Register-based OP_MakeRecord**:
```c
case OP_MakeRecord: {
    int nField = pOp->p1;  // Number of fields
    int iReg = pOp->p2;     // First input register
    int iOut = pOp->p3;     // Output register

    for (i = 0; i < nField; i++) {
        mem_t *pRec = &aMem[iReg + i];  // Sequential registers
        // Process pRec
        process_field(pRec);
    }

    // Create record in zNewRecord

    mem_t *pOut = &aMem[iOut];
    pOut->z = zNewRecord;
    pOut->n = nByte;
    pOut->flags = MEM_Str | MEM_Dyn;
    break;
}
```

**Changes**:
- Replace `&pTos[1-nField]` with `&aMem[iReg]`
- Use `aMem[iReg + i]` for sequential access
- Replace pop+push with direct output register assignment
- Registers `iReg..iReg+nField-1` remain intact (can be reused)

### Pattern 4: Conditional Input (Pseudo-table vs Cursor)

**OLD Stack-based OP_Column**:
```c
case OP_Column: {
    int iCur = pOp->p1;
    int iCol = pOp->p2;

    pTos++;  // Allocate output on stack

    if (iCur < 0) {
        // Legacy: record is on stack at position pTos[iCur]
        mem_t *pRec = &pTos[iCur];
        extract_from_memory(pRec, iCol, pTos);
    } else {
        // Normal: record is in cursor
        cursor_t *pC = p->cursors[iCur];
        extract_from_cursor(pC, iCol, pTos);
    }
    break;
}
```

**NEW Register-based OP_Column**:
```c
case OP_Column: {
    int iCur = pOp->p1;   // Cursor number (or negative for legacy)
    int iCol = pOp->p2;   // Column index
    int iOut = pOp->p3;   // Output register
    mem_t *pOut = &aMem[iOut];

    if (iCur < 0) {
        // Legacy stack mode: NOT RECOMMENDED, but keep for compatibility
        // Could map to a register: iCur = -1 means aMem[iOut-1], etc.
        // Better: Phase out this feature
        mem_t *pRec = &aMem[some_reg];  // Map somehow
        extract_from_memory(pRec, iCol, pOut);
    } else {
        // Normal: record is in cursor
        cursor_t *pC = p->cursors[iCur];
        extract_from_cursor(pC, iCol, pOut);
    }
    break;
}
```

**Changes**:
- If negative cursor is used, need to decide on register mapping
- Most code paths ignore negative cursor (can remove)
- Output goes to explicit register, not pushed onto stack

## mem_t Structure Changes

### OLD Stack-based Fields (DO NOT USE):
```c
struct mem_t {
    char *z;          // String value
    int n;            // String length
    int i;            // Integer value (WRONG - now u.i)
    double r;         // Real value (WRONG - now u.r)
    char zShort[32];  // Short string buffer (REMOVED)
    u32 flags;        // Type flags
};
```

### NEW Register-based Fields (USE THESE):
```c
struct mem_t {
    union {
        i64 i;        // Integer value (CORRECT - via union)
        double r;     // Real value (CORRECT - via union)
        void *p;      // Pointer value
    } u;
    char *z;          // String value (same)
    char *zMalloc;    // Dynamically allocated string (NEW)
    int n;            // String length (same)
    int szMalloc;     // Allocated size (NEW)
    u32 flags;        // Type flags (same)
};
```

**Key Differences**:
- Integer/real accessed via `mem->u.i` and `mem->u.r` (union)
- No more `zShort` inline buffer
- Use `zMalloc` and `szMalloc` for dynamic strings
- Helper functions: `__vdbe_mem_set_int64()`, `__vdbe_mem_set_str()`, `__vdbe_mem_set_null()`

## Testing Strategy

After each batch:

1. **Compilation Test**
   ```bash
   cd build_unix
   make vdbe.lo 2>&1 | tee errors.log
   grep -c "error:" errors.log
   ```

2. **Basic SQL Test** (after Batch 1+2 done)
   ```sql
   SELECT 1;                           -- Tests: OP_Integer, OP_ResultRow
   CREATE TABLE t(a, b);               -- Tests: OP_CreateTable
   INSERT INTO t VALUES (1, 'test');   -- Tests: OP_NewRowid, OP_MakeRecord, OP_PutIntKey
   SELECT * FROM t;                    -- Tests: OP_Column, OP_Rowid, OP_Next
   ```

3. **Index Test** (after Batch 3 done)
   ```sql
   CREATE INDEX idx ON t(a);           -- Tests: OP_CreateIndex
   SELECT * FROM t WHERE a=1;          -- Tests: OP_SeekGE, OP_IdxRowid
   ```

4. **Sort Test** (after Batch 6 done)
   ```sql
   SELECT * FROM t ORDER BY a;         -- Tests: OP_SorterOpen, OP_SorterInsert, OP_SorterData
   ```

## Risk Assessment

### HIGH RISK
- **OP_Column**: Most frequently used, complex logic, handles multiple cases
- **OP_MakeRecord**: Complex encoding, multi-register operation
- **OP_PutIntKey**: Critical for INSERT, handles pseudo-tables and real cursors

### MEDIUM RISK
- Index operations (OP_IdxInsert, etc.): Less tested than main table ops
- Seek operations with registers: May have off-by-one errors in register indices
- Sorter operations: Complex state management

### LOW RISK
- Simple output opcodes (OP_Sequence, OP_FullKey): Straightforward conversions
- Schema operations: Rarely executed, simple logic

## Estimated Timeline

| Batch | Description | Hours | Cumulative |
|-------|-------------|-------|------------|
| 1 | SELECT support (OP_Column) | 6-8 | 6-8 |
| 2 | INSERT support (3 opcodes) | 8-10 | 14-18 |
| 3 | Index support (4 opcodes) | 4-6 | 18-24 |
| 4 | Seek & checks (6 opcodes) | 3-4 | 21-28 |
| 5 | Auxiliary ops (3 opcodes) | 2-3 | 23-31 |
| 6 | Sorting (3 opcodes) | 3-4 | 26-35 |
| 7 | Schema ops (2 opcodes) | 1-2 | 27-37 |
| Testing | Integration testing | 3-5 | 30-42 |

**Total Estimated Effort**: 30-42 hours (1-2 weeks of full-time work, or 2-4 weeks part-time)

## Success Criteria

- [ ] All 425 pTos/pNos references converted to aMem[]
- [ ] vdbe.c compiles without errors
- [ ] Basic SQL operations work:
  - [ ] SELECT (with WHERE, ORDER BY)
  - [ ] INSERT
  - [ ] UPDATE
  - [ ] DELETE
  - [ ] CREATE TABLE/INDEX
- [ ] No memory leaks (valgrind clean)
- [ ] Test suite passes (when available)

## Notes for Future Agent

If resuming this work:

1. Start with PHASE5_CONVERSION_STATUS.md to see current progress
2. Pick next opcode from priority queue
3. Use conversion patterns above as templates
4. Test after each opcode or small batch
5. Update PHASE5_CONVERSION_STATUS.md with completed opcodes
6. Commit frequently with clear messages like "Convert OP_Column to register-based (Phase 5)"

## References

- VDBE_CONVERSION_PLAN.md - Original high-level plan
- PHASE5_CONVERSION_STATUS.md - Current status tracking
- src/vdbe.c lines 3905-3950 - OP_Rowid as reference (already converted)
- src/vdbe.c line 705 - aMem register array declaration
- src/inc/vdbe_int.h - mem_t structure definition
- SQLite 3.52.0 vdbe.c - Reference implementation

---

**Created**: 2026-03-18
**Author**: Agent 4 (Database Operations Conversion)
**Status**: Ready to begin implementation
