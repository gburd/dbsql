# Phase 5: Database Operations Opcode Conversion Status

**Date**: 2026-03-18
**Task**: Convert database operation opcodes from stack-based to register-based architecture
**File**: `/home/gburd/ws/dbsql/src/vdbe.c`

## Overall Status

- **Total pTos/pNos references**: 425 occurrences (need conversion)
- **Register-based aMem[] usage**: 7 occurrences (already converted)
- **Conversion progress**: ~1.6% complete

## Phase 5 Opcode Status

### Category A: Cursor Operations (15 opcodes)

| # | Opcode | Line | Status | Notes |
|---|--------|------|--------|-------|
| 1 | OP_OpenRead | 2708 | ✅ DONE | Already register-based, uses p->cursors[] |
| 2 | OP_OpenWrite | 2709 | ✅ DONE | Same as OpenRead (FALLTHROUGH) |
| 3 | OP_Close | 2934 | ✅ DONE | No stack operations |
| 4 | OP_SeekGE | 3160 | ✅ DONE | Already register-based (Phase 3) |
| 5 | OP_SeekGT | 3161 | ✅ DONE | Same as SeekGE (FALLTHROUGH) |
| 6 | OP_SeekLE | 3159 | ✅ DONE | Same as SeekGE (FALLTHROUGH) |
| 7 | OP_SeekLT | 3158 | ✅ DONE | Same as SeekGE (FALLTHROUGH) |
| 8 | OP_SeekRowid | 3273 | ⚠️ PARTIAL | Uses some pTos operations |
| 9 | OP_NotExists | 3440 | ⚠️ PARTIAL | Uses pTos for key |
| 10 | OP_Found | 3063 | ⚠️ PARTIAL | Uses pTos for key |
| 11 | OP_NotFound | 3062 | ⚠️ PARTIAL | Same as Found (FALLTHROUGH) |
| 12 | OP_Rewind | 4058 | ✅ DONE | Already register-based (Phase 3) |
| 13 | OP_Next | 4150 | ✅ DONE | Already register-based (Phase 3) |
| 14 | OP_Prev | 4110 | ✅ DONE | Already register-based (Phase 3) |
| 15 | OP_Last | 4019 | ✅ DONE | Already register-based (Phase 3) |

**Category A Summary**: 11/15 done (73%)

### Category B: Data Access (10 opcodes)

| # | Opcode | Line | Status | Notes |
|---|--------|------|--------|-------|
| 1 | OP_Column | 3786 | ❌ TODO | Heavy pTos usage: pTos++, pTos->z, etc. |
| 2 | OP_Rowid | 3905 | ✅ DONE | Uses aMem[pOp->p2] register-based |
| 3 | OP_NewRowid | 3470 | ❌ TODO | Uses pTos for output |
| 4 | OP_IdxRowid | 4271 | ❌ TODO | Uses pTos++ |
| 5 | OP_Sequence | 5670 | ❌ TODO | Uses pTos for output |
| 6 | OP_IdxGE | 4322 | ❌ TODO | Uses pTos for comparison |
| 7 | OP_IdxGT | 4323 | ❌ TODO | Same as IdxGE (FALLTHROUGH) |
| 8 | OP_IdxLT | 4322 | ❌ TODO | Same as IdxGE (FALLTHROUGH) |
| 9 | OP_IdxLE | ~4322 | ❌ TODO | May be combined with IdxGE |
| 10 | OP_IdxInsert | 4193 | ❌ TODO | Uses pTos for key |

**Category B Summary**: 1/10 done (10%)

### Category C: Data Modification (10 opcodes)

| # | Opcode | Line | Status | Notes |
|---|--------|------|--------|-------|
| 1 | OP_Insert (PutIntKey) | 3599 | ❌ TODO | Heavy pTos usage: pNos = &pTos[-1], __pop_stack |
| 2 | OP_Delete | 3672 | ✅ DONE | No stack operations, pure cursor ops |
| 3 | OP_MakeRecord | 2212 | ❌ TODO | Complex: pRec = &pTos[1-nField], __pop_stack, pTos++ |
| 4 | OP_MakeIdxKey | 2391 | ❌ TODO | Similar to MakeRecord |
| 5 | OP_IdxDelete | 4243 | ❌ TODO | Uses pTos for key |
| 6 | OP_Clear | 4414 | ✅ DONE | No stack operations |
| 7 | OP_CreateTable | 4445 | ❌ TODO | Uses pTos for root page |
| 8 | OP_CreateIndex | 4444 | ❌ TODO | Same as CreateTable (FALLTHROUGH) |
| 9 | OP_DropTable | N/A | ⚠️ MISSING | Not found in vdbe.c |
| 10 | OP_DropIndex | N/A | ⚠️ MISSING | Not found in vdbe.c |

**Category C Summary**: 2/10 done (20%)

### Category D: Transaction & Sorting (15 opcodes)

| # | Opcode | Line | Status | Notes |
|---|--------|------|--------|-------|
| 1 | OP_Transaction | 2541 | ✅ DONE | No stack operations |
| 2 | OP_Commit | 2558 | ✅ DONE | No stack operations |
| 3 | OP_Rollback | 2593 | ✅ DONE | No stack operations |
| 4 | OP_AutoCommit | N/A | ⚠️ MISSING | Not found in vdbe.c |
| 5 | OP_SorterOpen | 5476 | ⚠️ PARTIAL | Allocates cursor, minimal stack use |
| 6 | OP_SorterInsert | 5518 | ❌ TODO | Uses pTos for data |
| 7 | OP_SorterSort | 5552 | ⚠️ PARTIAL | Stub implementation |
| 8 | OP_SorterData | 5596 | ❌ TODO | Uses pTos for output |
| 9 | OP_SorterNext | 5633 | ⚠️ PARTIAL | Jump-only logic, minimal stack |
| 10 | OP_SorterRewind | N/A | ⚠️ MISSING | Not found (may be part of SorterSort) |
| 11 | OP_SorterClose | N/A | ⚠️ MISSING | Likely uses OP_Close |
| 12 | OP_SorterCompare | N/A | ⚠️ MISSING | Not found in vdbe.c |
| 13 | OP_Sort | 4712 | ❌ TODO | Legacy sort, uses pTos |
| 14 | OP_KeyAsData | 3696 | ✅ DONE | No stack operations, just sets flag |
| 15 | OP_RowData | 3722 | ❌ TODO | Uses pTos++ for output |

**Category D Summary**: 4/15 done (27%)

## Priority Conversion List

### HIGH PRIORITY (Core Data Manipulation)

1. **OP_Column** (line 3786) - Most frequently used, reads data from cursors
2. **OP_MakeRecord** (line 2212) - Creates records for INSERT
3. **OP_PutIntKey** (line 3599) - Actually inserts data
4. **OP_NewRowid** (line 3470) - Generates rowids for INSERT

### MEDIUM PRIORITY (Index & Seek Operations)

5. **OP_MakeIdxKey** (line 2391) - Creates index keys
6. **OP_IdxInsert** (line 4193) - Inserts into indexes
7. **OP_IdxDelete** (line 4243) - Deletes from indexes
8. **OP_Found/NotFound** (line 3062-3063) - Index lookups
9. **OP_NotExists** (line 3440) - Existence checks
10. **OP_SeekRowid** (line 3273) - Rowid seeks

### LOWER PRIORITY (Auxiliary & Legacy)

11. **OP_FullKey** (line 3963) - Extract full key
12. **OP_RowData** (line 3722) - Get row data
13. **OP_IdxRowid** (line 4271) - Get rowid from index
14. **OP_Sequence** (line 5670) - Sequence numbers
15. **OP_CreateTable/CreateIndex** (line 4444-4445) - Schema ops
16. **OP_Sort** (line 4712) - Legacy sort
17. **OP_SorterData/SorterInsert** (lines 5518, 5596) - Modern sorter

## Conversion Patterns

### Pattern 1: Simple Output to Register

**OLD (Stack-based)**:
```c
case OP_Rowid: {
    pTos++;
    pTos->i = rowid;
    pTos->flags = MEM_Int;
    break;
}
```

**NEW (Register-based)**:
```c
case OP_Rowid: {
    mem_t *pOut = &aMem[pOp->p2];  // P2 = destination register
    pOut->u.i = rowid;
    pOut->flags = MEM_Int;
    break;
}
```

### Pattern 2: Input from Register

**OLD (Stack-based)**:
```c
case OP_Insert: {
    mem_t *pKey = &pTos[-1];  // Key is second on stack
    mem_t *pData = pTos;       // Data is top of stack
    // ... use pKey, pData ...
    __pop_stack(&pTos, 2);     // Pop both
    break;
}
```

**NEW (Register-based)**:
```c
case OP_Insert: {
    mem_t *pKey = &aMem[pOp->p2];   // P2 = key register
    mem_t *pData = &aMem[pOp->p3];  // P3 = data register
    // ... use pKey, pData ...
    // No popping needed - registers stay
    break;
}
```

### Pattern 3: Range of Registers (MakeRecord)

**OLD (Stack-based)**:
```c
case OP_MakeRecord: {
    int nField = pOp->p1;
    mem_t *pRec = &pTos[1 - nField];  // Start N fields down
    for (i = 0; i < nField; i++, pRec++) {
        // Process pRec[i]
    }
    __pop_stack(&pTos, nField);  // Pop all inputs
    pTos++;                       // Push result
    pTos->z = zRecord;
    break;
}
```

**NEW (Register-based)**:
```c
case OP_MakeRecord: {
    int nField = pOp->p1;          // Number of fields
    int iReg = pOp->p2;             // Start register
    int iOut = pOp->p3;             // Output register
    for (i = 0; i < nField; i++) {
        mem_t *pRec = &aMem[iReg + i];
        // Process pRec
    }
    mem_t *pOut = &aMem[iOut];
    pOut->z = zRecord;
    break;
}
```

## Key Changes Needed

1. **Remove `pTos` variable usage** - It's not declared anywhere in __vdbe_exec()
2. **Use `aMem[pOp->pN]` instead** - Access registers by opcode parameters
3. **Remove `__pop_stack()` calls** - Registers don't need popping
4. **Update opcode parameter meanings**:
   - P1: Often cursor ID or count
   - P2: Often source/destination register or jump target
   - P3: Often second register or flags
   - P4: Pointer/string data (unchanged)

## Estimated Effort

- HIGH PRIORITY opcodes: 8-12 hours (complex, frequently used)
- MEDIUM PRIORITY opcodes: 6-8 hours (moderate complexity)
- LOWER PRIORITY opcodes: 4-6 hours (simpler or less critical)

**Total estimated**: 18-26 hours

## Testing Strategy

After each conversion batch:
1. Compile and fix errors
2. Test basic SQL operations:
   - `SELECT * FROM table` (OP_Column, OP_Rowid, OP_Next)
   - `INSERT INTO table VALUES (...)` (OP_MakeRecord, OP_PutIntKey, OP_NewRowid)
   - `UPDATE table SET ...` (OP_Column, OP_MakeRecord, OP_PutIntKey)
   - `DELETE FROM table` (OP_Delete)
3. Test with indexes (OP_Idx* opcodes)
4. Test ORDER BY (OP_Sorter* opcodes)

## References

- VDBE_CONVERSION_PLAN.md - Overall conversion strategy
- src/vdbe.c (lines 3905-3950) - OP_Rowid as example of converted opcode
- SQLite 3.52.0 vdbe.c - Reference implementation
