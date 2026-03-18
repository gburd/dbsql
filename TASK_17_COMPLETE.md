# Task #17 Complete: Implement Phase 3 Cursor Open/Close Opcodes

## Summary

Successfully replaced legacy stack-based cursor open/close opcodes with register-based implementations in `/home/gburd/ws/dbsql/src/vdbe.c`.

## Changes Made

### 1. Helper Functions (`/home/gburd/ws/dbsql/src/vdbe.c`)

**Added `__allocate_cursor()`** (lines 491-520):
- Allocates VdbeCursor with proper size calculation using `SZ_VDBECURSOR(nField)`
- Initializes `eCurType`, `nField`, and `aOffset` fields
- Returns NULL on memory allocation failure

**Updated `__expand_cursor_array_size()`** (line 567-579):
- Changed from allocating inline `cursor_t` array to `VdbeCursor*` pointer array
- Uses `vm->apCsr` instead of deprecated `vm->aCsr`
- Properly zeros new pointer slots

**Updated `__vdbe_free_cursor()`** (`/home/gburd/ws/dbsql/src/vdbe_method.c`, lines 1468-1494):
- Changed signature from `cursor_t*` to `VdbeCursor*`
- Uses `switch` on `eCurType` field to handle different cursor types:
  - `CURTYPE_BTREE`: Closes SM cursor and ephemeral btree if needed
  - `CURTYPE_SORTER`: Placeholder for future sorter cleanup
  - `CURTYPE_VTAB`: Placeholder for virtual table cleanup
  - `CURTYPE_PSEUDO`: No cleanup needed
- Properly frees cursor memory

### 2. Opcode Implementations

**OP_OpenRead / OP_OpenWrite** (lines 2697-2762):
- Removed stack-based operand retrieval
- Uses P1 for cursor index, P2 for root page, P3 for database index
- Gets `KeyInfo` or field count from P4 parameter
- Allocates `VdbeCursor` with `CURTYPE_BTREE`
- Opens SM cursor with proper write flag
- Stores cursor in `p->apCsr[iCur]` pointer array
- Properly handles errors with cursor cleanup

**OP_OpenTemp** (lines 2798-2903):
- Implements ephemeral table creation (in-memory temporary tables)
- Reuses existing ephemeral cursor if possible (optimization)
- Creates ephemeral SM database with `__sm_create()`
- Handles both table (P4=NULL) and index (P4=KeyInfo) ephemeral tables
- Sets `isEphemeral` flag for proper cleanup
- Stores btree handle in `pCur->ub.pBtx` union field

**OP_OpenPseudo** (lines 2917-2943):
- Allocates `VdbeCursor` with `CURTYPE_PSEUDO`
- Sets `seekResult` to P2 (register holding the record)
- Used for trigger NEW/OLD pseudo-tables

**OP_Close** (lines 2952-2960):
- Simplified to single cursor pointer free operation
- Calls `__vdbe_free_cursor()` and nulls pointer

### 3. Header Updates

**`/home/gburd/ws/dbsql/src/inc/dbsql_ext.h`** (line 219):
- Updated `__vdbe_free_cursor` signature to use `VdbeCursor*` instead of `cursor_t*`

## Architecture Notes

### Register-Based vs Stack-Based

**Old (Stack-Based)**:
```c
cursor_t aCsr[N];  // Inline array
__vdbe_cleanup_cursor(&p->aCsr[i]);
```

**New (Register-Based)**:
```c
VdbeCursor **apCsr;  // Pointer array
__vdbe_free_cursor(p, p->apCsr[i]);
```

### Cursor Allocation Strategy

Following SQLite 3.52.0 pattern:
1. Cursors are dynamically allocated on demand
2. Each cursor is sized based on field count: `SZ_VDBECURSOR(nField)`
3. Pointer array `apCsr[]` holds cursor pointers, not inline cursors
4. Allows different cursor types via `eCurType` field

### VdbeCursor Union Fields

The VdbeCursor structure uses unions for type-specific data:
- `uc.pCursor` (sm_cursor_t*): For BTREE cursors
- `uc.pSorter` (vdbe_sorter_t*): For SORTER cursors (future)
- `ub.pBtx` (sm_t*): For ephemeral btree handle
- `ub.aAltMap` (u32*): For index column mapping

## Testing Considerations

### What Should Work
- Opening and closing table cursors
- Opening and closing index cursors with KeyInfo
- Creating ephemeral tables and indexes
- Pseudo-table cursors for triggers
- Cursor reuse optimization for ephemeral tables

### Known Limitations
- Other opcodes (OP_Column, OP_MoveTo, etc.) still use old `p->aCsr` syntax
- Will cause compilation errors until those are updated
- Old cursor fields like `lastRecno`, `recnoIsValid`, `keyAsData`, `pseudoTable` not in VdbeCursor
- Those opcodes need separate migration (Task #18+)

## Compilation Status

**Expected Issues:**
The code will not compile until all opcode implementations are updated from `p->aCsr[]` to `p->apCsr[]`. The affected opcodes include:
- OP_MoveTo, OP_Found, OP_NotFound
- OP_Column, OP_Rowid
- OP_Insert, OP_Delete
- OP_Next, OP_Prev
- Many others throughout vdbe.c

These will be addressed in subsequent tasks.

## References

- SQLite 3.52.0: `_/sqlite/src/vdbe.c` lines 4386-4621 (OP_OpenRead/OP_OpenEphemeral)
- SQLite 3.52.0: `_/sqlite/src/vdbe.c` lines 253-321 (allocateCursor)
- SQLite 3.52.0: `_/sqlite/src/vdbeaux.c` lines 2746-2787 (sqlite3VdbeFreeCursor)
- DBSQL vdbe_int.h: VdbeCursor structure definition (lines 102-165)

## Next Steps

**Task #18:** Update cursor movement opcodes (OP_MoveTo, OP_Next, OP_Prev, etc.)
**Task #19:** Update data access opcodes (OP_Column, OP_Rowid, etc.)
**Task #20:** Update DML opcodes (OP_Insert, OP_Delete, OP_Update, etc.)
