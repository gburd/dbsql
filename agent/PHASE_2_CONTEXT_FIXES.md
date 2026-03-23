# Phase 2 Context-Sensitivity Fixes

## Problem
The aggressive sed script used in Phase 2 renamed ALL `->pNext` to `->next_vdbe` regardless of struct type, causing compilation errors and semantic incorrectness.

## Root Cause
The sed script pattern `s/->pNext/->next_vdbe/g` was applied globally without considering the actual struct type. Only `vdbe_t` structures should use `next_vdbe`; all other structures should continue using `pNext`.

## Struct Field Naming Rules
Based on analysis of struct definitions in `src/inc/dbsql_int.in` and `src/inc/vdbe_int.h`:

### Structures Using `pNext` (NOT next_vdbe)
- **index_t** (line 1024 in dbsql_int.in): `index_t *pNext`
- **func_def_t** (line 661 in dbsql_int.in): `func_def_t *pNext`
- **trigger_t** (line 2040 in dbsql_int.in): `trigger_t *pNext`
- **trigger_step_t** (line 2072 in dbsql_int.in): `trigger_step_t *pNext`
- **trigger_stack_t** (line 2113 in dbsql_int.in): `trigger_stack_t *pNext`
- **trigger_prg_t** (line 1687 in dbsql_int.in): `trigger_prg_t *pNext`
- **sorter_t** (old stack-based VDBE structure): `sorter_t *pNext`
- **keylist_t** (old stack-based VDBE structure): `keylist_t *pNext`
- **expr_list_item_t**: `expr_list_item_t *pNext`

### Structure Using `next_vdbe` (CORRECT)
- **vdbe_t** (line 454 in vdbe_int.h): `vdbe_t *prev_vdbe, *next_vdbe`

## Files Fixed

### Commit 8ee5ee4 (bulk fixes)
1. **src/api.c** - Fixed func_def_t->pNext (2 occurrences)
2. **src/cg_build.c** - Fixed index_t->pNext and trigger_t->pNext (multiple occurrences)
3. **src/cg_copy.c** - Fixed index_t->pNext (1 occurrence)
4. **src/cg_delete.c** - Fixed index_t->pNext (5 occurrences)
5. **src/cg_delete_CONVERTED.c** - Fixed index_t->pNext (5 occurrences)
6. **src/cg_expr.c** - Fixed expr_list_item_t->pNext (3 occurrences)
7. **src/cg_insert.c** - Fixed index_t->pNext (6 occurrences)
8. **src/cg_insert_CONVERTED.c** - Fixed index_t->pNext (6 occurrences)
9. **src/cg_pragma.c** - Fixed index_t->pNext (1 occurrence)
10. **src/cg_select.c** - Fixed index_t->pNext (1 occurrence)
11. **src/cg_trigger.c** - Fixed trigger_step_t->pNext, trigger_t->pNext, trigger_stack_t->pNext (11 occurrences)
12. **src/cg_update.c** - Fixed index_t->pNext (8 occurrences)
13. **src/cg_where.c** - Fixed index_t->pNext (2 occurrences)
14. **src/vdbe.c** - Fixed sorter_t->pNext and keylist_t->pNext (12 occurrences)

### Current Changes (vdbe_method.c)
15. **src/vdbe_method.c** - Fixed vdbe_t->next_vdbe (was incorrectly changed to pNext)
    - Line 71: `vm->next_vdbe = dbp->pVdbe` (VDBE linked list insertion)
    - Lines 1796-1804: VDBE linked list unlinking operations

## Verification

### Pattern Analysis
After fixes:
```bash
# Only vdbe_t structures should have next_vdbe
$ rg '->next_vdbe' src/ -t c --color never
src/inc/vdbe_int.h:454:	vdbe_t *prev_vdbe, *next_vdbe;
src/vdbe_method.c:71:	vm->next_vdbe = dbp->pVdbe;
src/vdbe_method.c:1796:		vm->prev_vdbe->next_vdbe = vm->next_vdbe;
src/vdbe_method.c:1799:		vm->db->pVdbe = vm->next_vdbe;
src/vdbe_method.c:1801:	if (vm->next_vdbe) {
src/vdbe_method.c:1802:		vm->next_vdbe->prev_vdbe = vm->prev_vdbe;
src/vdbe_method.c:1804:	vm->prev_vdbe = vm->next_vdbe = 0;
```

All usages are for `vdbe_t` structures, which is correct.

### Compilation Status
After these fixes, the Phase 2 context-sensitivity errors are resolved. Remaining compilation errors are unrelated to the `pNext`/`next_vdbe` renaming issue (they are in cg_pragma.c related to vdbe_op_t initializer structure).

## Lessons Learned
1. **Context-aware sed scripts** - Always consider struct type when doing field renames
2. **Test incrementally** - Run compilation after each batch of changes
3. **Backup files** - The phase2.bak files saved the original state before fixes
4. **Type-based analysis** - Verify struct definitions before global search/replace operations

## Final Status
✅ **All Phase 2 context-sensitivity issues resolved**

- Zero errors related to incorrect `pNext`/`next_vdbe` usage
- All struct types now use the correct linked list field name
- Compilation proceeds past all affected files

## Next Steps
The next compilation errors to address are in `src/cg_pragma.c` (20 errors), which are related to incorrect vdbe_op_list_t initializer format. These errors are unrelated to the Phase 2 naming convention fixes.

### Remaining Error Summary
```
Total errors: 20 (all in cg_pragma.c)
Error type: "initializer element is not computable at load time"
Root cause: Static vdbe_op_list_t arrays trying to initialize integer fields with string constants
```

These errors indicate that the old stack-based VDBE instruction format is being used where the register-based format is expected. The vdbe_op_list_t structure needs either:
1. Dynamic initialization at runtime instead of static initialization, OR
2. Conversion to use the new P4 union structure for string operands
