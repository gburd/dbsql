# VDBE Compilation Fixes - Status Report

## Summary
Working through systematic compilation errors in `src/vdbe.c` after SQLite 3.52.0 modernization. The register-based VDBE architecture removed several legacy structures and fields.

## Fixes Completed ✅

### 1. Legacy Opcodes Wrapped with #if 0
All legacy SQLite 2.x opcodes disabled and replaced with stub implementations:
- **Aggregate opcodes** (OP_AggReset through OP_AggNext) - lines 5142+
- **Set opcodes** (OP_SetInsert through OP_SetNext) - lines 5792+
- **Sort opcodes** (OP_SortPut through OP_SortNext) - lines 4600+
- **Callback opcodes** (OP_NullCallback, OP_SortCallback) - lines 1186+, 4804+
- **File/COPY opcodes** (OP_FileOpen through OP_MemStore) - lines 4851+
- **List opcodes** (OP_ListWrite through OP_ListPop) - lines 4491+

### 2. Type System Updates
- Replaced `cursor_t` with `VdbeCursor` globally
- Fixed `mem_t` union accesses: `mem->i` → `mem->u.i`, `mem->r` → `mem->u.r`
- Added vdbe_return label for mid-execution returns

### 3. Helper Functions
- Wrapped aggregate helper functions (__agg_insert, __agg_in_focus) - lines 216-272
- Fixed __api_step malloc calls to use 3-argument signature
- Updated __entity_as_string to not use removed `zShort` field

### 4. Other Compilation Fixes
- Fixed trigger structure initializers in cg_trigger.c (converted static arrays to dynamic)
- Fixed flexible array member in cg_build.c
- Fixed structure field renames in cg_where.c, cg_select.c
- Fixed opcode renames (OP_Recno → OP_Rowid)
- Added legacy compatibility fields to trigger structures in dbsql_int.in

## Remaining Compilation Errors ⚠️

### Critical Issues (Block Compilation)

1. **More mem_t field accesses** (~20 locations)
   - Lines 410, 413, 416, 435, 437, 439, 457, 461, 464: `mem->i` / `mem->r` need `u.` prefix
   - Lines 1021, 1027-1028, 1069, 1076, 1098: `mem->zShort` removed (use dynamic allocation)
   - These are in helper functions: __entity_compare, __vdbe_sorter_compare, __vdbe_list

2. **vdbe_t callback fields removed** (~10 locations)
   - Lines 1112-1113: `azColName`, `nCallback`
   - Lines 1125: `zArgv`
   - Lines 1139-1149: `xCallback`, `pCbArg`, `azResColumn`
   - These are in __vdbe_list function (EXPLAIN output)

3. **sorter_t type removed** (lines 477-478)
   - In __sorted_merge helper function
   - Need to wrap or rewrite using VdbeSorter

4. **Unknown NBFS constant** (line 1021)
   - Was inline buffer size for `zShort`
   - Need to define or replace

5. **Static declaration conflict** (line 283)
   - __entity_as_string has non-static forward declaration somewhere
   - Need to make declarations consistent

## Estimated Remaining Work

### Phase 1: Fix mem_t Accesses (2-3 hours)
- Update all remaining `.i` → `.u.i` and `.r` → `.u.r` (systematic regex)
- Replace all `zShort` usage with dynamic allocation
- Fix NBFS references

### Phase 2: Handle Removed vdbe_t Fields (2-3 hours)
- Wrap __vdbe_list function (EXPLAIN functionality) or update for new structure
- Remove/update code that accesses azColName, xCallback, etc.

### Phase 3: Fix Helper Functions (1-2 hours)
- Wrap __sorted_merge or update to use VdbeSorter
- Fix __entity_compare to use new mem_t structure
- Fix static/non-static declaration conflicts

### Phase 4: Final Compilation (1 hour)
- Address any remaining warnings
- Test that modernized opcodes work

**Total: ~6-9 hours to complete vdbe.c compilation**

## Testing Requirements

Once vdbe.c compiles:
1. Build all source files (35 files total)
2. Run test suite baseline
3. Test basic SQL operations work with new VDBE
4. Verify no regressions in existing functionality

## Files Modified So Far

- src/vdbe.c (extensive changes)
- src/vdbe_method.c
- src/cg_where.c
- src/cg_select.c
- src/cg_trigger.c
- src/cg_build.c
- src/inc/dbsql_int.in
- src/inc/vdbe_int.h (verified structure changes)

## Next Steps

1. Apply systematic regex fixes for remaining mem_t accesses
2. Wrap or update __vdbe_list (EXPLAIN functionality)
3. Wrap __sorted_merge and related sorter helpers
4. Final compilation pass
5. Address format warnings (i64 vs int in fprintf)
