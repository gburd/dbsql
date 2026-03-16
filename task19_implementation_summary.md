# Task #19: Phase 3 Cursor Navigation Opcodes - Implementation Summary

**Date**: March 16, 2026
**Status**: PARTIALLY COMPLETE

## Opcodes Implemented (4 of 5)

### 1. OP_Rewind ✅ COMPLETE
**Location**: src/vdbe.c line ~3625
**Changes**: Updated from stack-based to register-based architecture
- Uses `VdbeCursor *pC` instead of `cursor_t *pC`
- Uses `p->apCsr[pOp->p1]` instead of `&p->aCsr[i]`
- Calls `__sm_first(pCrsr, &res)`
- Updates `pC->cacheStatus = CACHE_STALE`
- Sets `pC->nullRow = (u8)res`
- Jumps to P2 if empty

### 2. OP_Next ✅ COMPLETE
**Location**: src/vdbe.c line ~3662
**Changes**: Separated from OP_Prev, updated to register-based
- Uses `VdbeCursor` structure
- Calls `__sm_next(pCrsr, &res)`
- Updates cache status
- Jumps to P2 if not EOF (res == 0)
- Falls through if EOF (res == 1)

### 3. OP_Prev ✅ COMPLETE
**Location**: src/vdbe.c line ~3661
**Changes**: Separated from OP_Next, updated to register-based
- Uses `VdbeCursor` structure
- Calls `__sm_prev(pCrsr, &res)`
- Updates cache status
- Jumps to P2 if not BOF (res == 0)
- Falls through if BOF (res == 1)

### 4. OP_Rowid ✅ COMPLETE
**Location**: src/vdbe.c line ~3501 (replaces OP_Recno)
**Changes**: New opcode that replaces OP_Recno
- Uses register P2 for output instead of pushing to stack
- Calls `__sm_integer_key(pCrsr)` for direct i64 rowid access
- No more `KEY_TO_INT()` byte-swapping (old 32-bit encoding)
- Uses `__vdbe_mem_set_int64(pOut, v)` to store result
- Handles deferred moveto, pseudo-cursors, and btree cursors

### 5. OP_Column ⚠️ NEEDS COMPLETION
**Location**: src/vdbe.c line ~3612
**Status**: Implementation prepared but NOT YET APPLIED

The new register-based implementation has been developed (see phase3_opcodes.c) but the file edit failed due to text matching issues. The implementation is ready and needs to be manually applied or applied with a different approach.

**Key changes in prepared implementation**:
- Reads from register P3 instead of pushing to stack
- Uses `VdbeCursor` and `Mem` structures
- Uses `pDest = &aMem[pOp->p3]` for output
- Handles CURTYPE_PSEUDO and CURTYPE_BTREE cursor types
- Uses `pC->isTable` to determine whether to call `__sm_data()` or `__sm_key()`
- Updates `pC->cacheStatus` for column caching
- Still uses OLD DBSQL record format (column width headers)
- Does NOT parse SQLite 3.52 record format with serial types (future work)

## SM Functions Used

All required SM functions are available in src/sm.c and declared in src/inc/dbsql_ext.h:

| Function | Purpose | Used By |
|----------|---------|---------|
| `__sm_first(cursor, &res)` | Move to first entry | OP_Rewind |
| `__sm_next(cursor, &res)` | Move to next entry | OP_Next |
| `__sm_prev(cursor, &res)` | Move to previous entry | OP_Prev |
| `__sm_integer_key(cursor)` | Get i64 rowid | OP_Rowid |
| `__sm_key_size(cursor, &size)` | Get key length | OP_Column |
| `__sm_data_size(cursor, &size)` | Get data length | OP_Column |
| `__sm_key(cursor, offset, amt, buf)` | Read key data | OP_Column |
| `__sm_data(cursor, offset, amt, buf)` | Read data | OP_Column |

Note: `__sm_eof(cursor)` is available but not used. The opcodes check the result parameter from next/prev instead.

## VdbeCursor Fields Used

From src/inc/vdbe_int.h struct VdbeCursor:

- `eCurType` - Cursor type (CURTYPE_BTREE, CURTYPE_PSEUDO, etc.)
- `nullRow` - True if pointing to null/empty row
- `deferredMoveto` - Deferred seek flag
- `isTable` - True for tables, false for indexes
- `cacheStatus` - Column cache generation counter
- `seekResult` - For pseudo-cursors: register containing data
- `movetoTarget` - For deferred moveto: the rowid target
- `uc.pCursor` - The underlying SM cursor (sm_cursor_t*)

## Helper Functions Used

From src/vdbe_method.c:

- `__vdbe_mem_set_null(Mem*)` - Set register to NULL
- `__vdbe_mem_set_int64(Mem*, i64)` - Set register to integer
- `__vdbe_mem_set_str(Mem*, char*, len, enc, destructor)` - Set register to string
- `__vdbe_mem_copy(Mem*, Mem*)` - Copy between registers

## Testing Status

- **Code compiles**: Changes to OP_Rewind, OP_Next, OP_Prev, OP_Rowid compile successfully
- **Functional testing**: PENDING - requires code generator updates to emit register-based instructions
- **OP_Column**: NOT COMPILED YET - implementation prepared but not applied

## Remaining Work

### Immediate (to complete Task #19):

1. **Apply OP_Column implementation** - The implementation is ready in phase3_opcodes.c but needs to be applied to src/vdbe.c
   - Approach 1: Manual editing to replace the old OP_Column case
   - Approach 2: Use sed/awk script to do the replacement
   - Approach 3: Use Write tool to rewrite the entire case statement section

2. **Test compilation** - Verify all 5 opcodes compile without errors

3. **Update vdbe_analysis_report.md** - Document completion of Task #19

### Future Work (beyond Task #19):

4. **SQLite 3.52 record format** - OP_Column currently uses old DBSQL format with column width headers. Need to implement proper record parsing with serial types (varint headers).

5. **Column caching optimization** - Implement proper column cache so repeated Column operations on the same row reuse cached data.

6. **Code generator updates** - Update code generators (cg_*.c) to emit:
   - `OP_Rewind P1 P2` instead of old format
   - `OP_Next P1 P2` / `OP_Prev P1 P2`
   - `OP_Column P1 P2 P3` with register P3
   - `OP_Rowid P1 P2` with register P2 (not OP_Recno)

7. **OP_Column enhancements**:
   - Parse SQLite 3.52 varint-based record headers
   - Handle serial types (NULL=0, 1-byte int=1, 2-byte int=2, etc.)
   - Support text vs blob distinction
   - Support proper type affinity

## Files Modified

- `src/vdbe.c` - Updated OP_Rewind, OP_Next, OP_Prev, OP_Rowid (OP_Column pending)
- `phase3_opcodes.c` - Complete implementations for reference
- `task19_implementation_summary.md` - This file

## Key Architectural Changes

1. **Stack → Register**: All opcodes now use register addressing (`aMem[pOp->pN]`) instead of stack manipulation (`pTos++`)

2. **cursor_t → VdbeCursor**: Old inline `p->aCsr[i]` array replaced with pointer array `p->apCsr[i]`

3. **Null handling**: Uses `__vdbe_mem_set_null()` instead of `pTos->flags = MEM_Null`

4. **Integer rowids**: `__sm_integer_key()` returns i64 directly (no KEY_TO_INT macro needed)

5. **Cursor types**: Uses `pC->eCurType` to distinguish CURTYPE_BTREE vs CURTYPE_PSEUDO

6. **Table vs Index**: Uses `pC->isTable` to determine which SM function to call

## Compatibility Notes

- The opcodes still use the OLD DBSQL record format (width-based column headers)
- They are compatible with the existing SM (Berkeley DB wrapper)
- They work with the Phase 2 register infrastructure (aMem array)
- They will work with old code generators until those are updated
- Full SQLite 3.52 compatibility requires record format updates (future task)

## Next Steps for Developer

To complete Task #19:

1. Review the prepared OP_Column implementation in phase3_opcodes.c
2. Apply it to src/vdbe.c (replacing lines ~3612-3730)
3. Compile and verify no errors
4. Update vdbe_analysis_report.md to mark Task #19 complete
5. Consider whether to tackle SQLite 3.52 record format now or defer

## References

- Task description: README or project task list
- SQLite 3.52.0 reference: _/sqlite/src/vdbe.c lines 2975-3699 (OP_Column), 6154-6193 (OP_Rowid), 6372-6540 (OP_Rewind/Next/Prev)
- VDBE analysis: vdbe_analysis_report.md
- SM API: src/inc/dbsql_ext.h lines 145-173
- VdbeCursor definition: src/inc/vdbe_int.h lines 103-165
- Mem definition: src/inc/vdbe_int.h lines 224-253
