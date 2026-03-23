# Task #28 Status Report - Systematic Compilation Error Fixes

**Date**: 2026-03-17
**Branch**: claude
**Status**: IN PROGRESS - First file successfully compiled

## Summary

Successfully fixed ALL compilation errors in `cg_insert.c` (first of 35 failing files). Established systematic patterns for fixing remaining files. Created helper function `__schema_to_index()` to replace removed `table->iDb` field access.

## What Was Accomplished

### 1. Created Helper Function for Schema-to-Database-Index Mapping

**Problem**: The `table->iDb` and `index->iDb` fields were removed in SQLite 3.52.0. Database index is now found via `pSchema` pointer.

**Solution**: Added `__schema_to_index()` function:
- **Location**: `/home/gburd/ws/dbsql/src/cg_build.c` (lines 193-209)
- **Signature**: `int __schema_to_index(DBSQL *dbp, schema_t *schema)`
- **Function**: Loops through `dbp->aDb[]` array to find matching schema
- **Returns**: Database index or -1 if not found

```c
int __schema_to_index(DBSQL *dbp, schema_t *schema)
{
	int i;
	if (schema == 0)
		return -1;
	for (i = 0; i < dbp->nDb; i++) {
		if (dbp->aDb[i].pSchema == schema) {
			return i;
		}
	}
	return -1;
}
```

### 2. Fixed cg_insert.c (100% Complete)

**File**: `/home/gburd/ws/dbsql/src/cg_insert.c`
**Result**: Compiles cleanly with ZERO errors
**Object file**: `/home/gburd/ws/dbsql/build_unix/.libs/cg_insert.o` (109KB)

#### Fixes Applied:

1. **Database Index Access** (`table->iDb` removed):
   - Old: `table->iDb`
   - New: `__schema_to_index(dbp, table->pSchema)`
   - Locations: Lines 159-163, 196, 249-255, 404-410, 508-513

2. **View Detection** (`table->x.pSelect` moved):
   - Old: `table->x.pSelect != 0`
   - New: `table->eTabType == TABTYP_VIEW`
   - Also changed assertions: `table->x.pSelect == 0` → `table->eTabType != TABTYP_VIEW`

3. **Column Name Access** (`zName` → `zCnName`):
   - Old: `table->aCol[i].zName`
   - New: `table->aCol[i].zCnName`
   - Locations: Lines 344, 837, 997

4. **Column Default Values** (`zDflt` removed):
   - Old: `table->aCol[i].zDflt` (string literal)
   - New: Access via `table->aCol[i].iDflt` (1-based index into `table->u.tab.pDfltList`)
   - Implementation:
     ```c
     if (table->aCol[i].iDflt > 0) {
         int dflt_idx = table->aCol[i].iDflt - 1;
         expr_t *dflt_expr = table->u.tab.pDfltList->a[dflt_idx].pExpr;
         __expr_code(parser, dflt_expr);
     } else {
         __vdbe_add_op(v, OP_String, 0, 0);
     }
     ```

5. **ID List Column Mapping** (`column->a[i].idx` removed):
   - Old: `id_list_item` had `idx` field
   - New: Created local `col_idx` array to track mapping
   - Allocated with: `__dbsql_calloc(dbp, column->nId, sizeof(int), &col_idx)`
   - Freed at cleanup: Added `__dbsql_free(dbp, col_idx)` at line 669

6. **VDBE P3 vs P4 Parameters** (API signature changed):
   - Old: `__vdbe_change_p3(v, addr, "string", P3_STATIC)`
   - New: `__vdbe_change_p4(v, addr, "string", P3_STATIC)`
   - Changed 7 occurrences at lines 406, 410, 509, 513, 657, 831, 1003

7. **Trigger Context Detection** (`parser->trigStack` removed):
   - Old: `parser->trigStack ? 0 : 1`
   - New: `parser->eTriggerOp ? 0 : 1`
   - Location: Line 1093

## Error Pattern Catalog

Based on fixing `cg_insert.c`, here are the systematic patterns needed for remaining 34 files:

### Category A: Schema/Database Mapping (HIGH PRIORITY)
**Pattern**: `table->iDb` or `index->iDb`
**Fix**: `__schema_to_index(dbp, table->pSchema)` or `__schema_to_index(dbp, index->pSchema)`
**Affected**: ~93 occurrences across all cg_*.c files

### Category B: Column Structure Changes (HIGH PRIORITY)
**Patterns**:
- `column->zName` → `column->zCnName`
- `column->zDflt` → Use `column->iDflt` to index into `table->u.tab.pDfltList`
**Affected**: ~69 occurrences

### Category C: View/Table Type Detection (MEDIUM PRIORITY)
**Pattern**: `table->x.pSelect`
**Fix**: `table->eTabType == TABTYP_VIEW` (check type field)
**Note**: Access view select via `table->u.view.pSelect` if needed
**Affected**: ~25 occurrences

### Category D: VDBE API Changes (MEDIUM PRIORITY)
**Pattern**: `__vdbe_change_p3(v, addr, "string", P3_*)`
**Fix**: `__vdbe_change_p4(v, addr, "string", P3_*)`
**Note**: P3 now takes integers only; strings use P4
**Affected**: ~71 occurrences

### Category E: Parser/Trigger Changes (LOW PRIORITY)
**Pattern**: `parser->trigStack`
**Fix**: `parser->eTriggerOp` (non-zero indicates trigger context)
**Affected**: ~27 occurrences

### Category F: ID List Item Changes (AS NEEDED)
**Pattern**: `id_list_item->idx`
**Fix**: Create local `int*` array to track column mappings
**Affected**: Only in INSERT operations (cg_insert.c already fixed)

## Remaining Work

### Files Still Failing: 34

Based on build log analysis:
1. **Code Generators** (highest priority):
   - cg_attach.c
   - cg_auth.c
   - cg_build.c (ironically, where we added helper function!)
   - cg_copy.c
   - cg_date.c
   - cg_delete.c
   - cg_expr.c (MOST ERRORS - fix this next)
   - cg_pragma.c
   - cg_select.c (MANY ERRORS)
   - cg_trigger.c
   - cg_update.c
   - cg_vacuum.c
   - cg_where.c

2. **VDBE Implementation**:
   - vdbe.c (core engine - many low-level fixes needed)
   - vdbe_method.c

3. **API Layer**:
   - api.c
   - api_table.c

4. **Parser/Tokenizer**:
   - sql_tokenize.c
   - sql_fns.c
   - sql_parser.c (generated, may auto-fix)

5. **Other Core**:
   - sm.c (storage manager)
   - All remaining utility files

### Estimated Time Remaining

- **cg_build.c**: 1-2 hours (should be similar to cg_insert.c)
- **cg_expr.c**: 3-4 hours (most complex - expression handling)
- **cg_select.c**: 3-4 hours (complex - query processing)
- **cg_delete.c**: 1 hour (similar to cg_insert.c)
- **cg_update.c**: 2 hours (similar to cg_insert.c but more complex)
- **Other cg_*.c files**: 1 hour each × 8 = 8 hours
- **vdbe.c + vdbe_method.c**: 4-6 hours (low-level VDBE internals)
- **api.c + api_table.c**: 2-3 hours
- **Other files**: 2-3 hours

**Total Estimate**: 27-37 hours remaining

## Recommended Next Steps

### Immediate (Next Session):

1. **Fix cg_build.c** - Since we added the helper function there, it should be straightforward
2. **Fix cg_delete.c** - Very similar structure to cg_insert.c
3. **Fix cg_update.c** - Similar to cg_insert.c but slightly more complex

### Short Term:

4. **Fix cg_expr.c** - Critical for all expression code generation
5. **Fix cg_select.c** - Critical for SELECT statements

### Medium Term:

6. **Fix remaining cg_*.c files** one by one
7. **Fix VDBE files** (vdbe.c, vdbe_method.c)
8. **Fix API files** (api.c, api_table.c)

### Final:

9. **Verify full build** completes
10. **Test basic SQL** queries

## Key Insights

1. **Pattern-based fixes work well**: Once the pattern is established (as in cg_insert.c), similar files can be fixed efficiently

2. **Helper functions are essential**: `__schema_to_index()` will be heavily reused across all files

3. **Column defaults changed significantly**: The move from `zDflt` string literals to `iDflt` indices into `pDfltList` requires careful handling

4. **Type safety improved**: SQLite 3.52.0 uses more enums and type checking (e.g., `eTabType`)

5. **P3 vs P4 distinction**: VDBE parameters now strictly typed - integers in P3, strings/pointers in P4

## Files Modified This Session

1. `/home/gburd/ws/dbsql/src/cg_build.c` - Added `__schema_to_index()` helper
2. `/home/gburd/ws/dbsql/src/inc/dbsql_ext.h` - Regenerated with new function declaration
3. `/home/gburd/ws/dbsql/src/cg_insert.c` - Fixed ALL compilation errors

## Testing

### Compilation Test
```bash
cd /home/gburd/ws/dbsql/build_unix
make cg_insert.lo
# SUCCESS: Object file created
```

### Verification
```bash
ls -la /home/gburd/ws/dbsql/build_unix/.libs/cg_insert.o
# -rw-r--r-- 1 gburd users 109424 Mar 17 07:56 cg_insert.o
```

## Confidence Assessment

**High Confidence** that remaining files can be fixed following these patterns:
- ✅ Established working helper function
- ✅ Fixed most complex DML code generator (INSERT)
- ✅ Documented all error patterns
- ✅ Clear path forward

## Commands for Next Session

```bash
# Continue systematic fixing
cd /home/gburd/ws/dbsql/build_unix

# Try next files in order:
make cg_build.lo 2>&1 | head -100
make cg_delete.lo 2>&1 | head -100
make cg_update.lo 2>&1 | head -100

# Full build attempt (to see progress):
make 2>&1 | grep -c "error:"

# When complete:
make
./dbsql :memory: "SELECT 1;"
```

---

**Status**: Task #28 is ~3% complete (1 of 35 files fixed)
**Next**: Fix cg_build.c, cg_delete.c, cg_update.c
**Confidence**: HIGH - Clear path forward with established patterns
