# Task #28 Complete: cg_trigger.c Compilation Fixed

## Status
✅ **COMPLETE** - cg_trigger.c now compiles successfully with ZERO errors

## File Fixed
- `/home/gburd/ws/dbsql/src/cg_trigger.c` - All 86 compilation errors resolved

## Changes Made

### 1. Updated trigger_t Structure Field Access
**Problem**: trigger_t fields were renamed/removed in SQLite 3.52.0
**Changes**:
- `trigger->name` → `trigger->zName` (all occurrences)
- `trigger->iDb` → `__schema_to_index(dbp, trigger->pSchema)`
- `trigger->iTabDb` → `__schema_to_index(dbp, trigger->pTabSchema)`
- `trigger->nameToken` → removed (no longer exists)
- `trigger->foreach` → removed (no longer used in SQLite 3.52.0)

### 2. Updated trigger_step_t Structure Field Access
**Problem**: trigger_step_t was completely redesigned
**Changes**:
- `step->target` (token_t) → `step->pSrc` (src_list_t*)
- `step->x.pSelect` → `step->pSelect` (no longer in union)
- All references to `step->target.z`, `step->target.n`, `step->target.dyn` replaced with src_list operations

### 3. Fixed View Detection
**Problem**: `table->x.pSelect` removed
**Changes**:
- `table->x.pSelect != 0` → `table->eTabType == TABTYP_VIEW`
- `table->x.pSelect == 0` → `table->eTabType != TABTYP_VIEW`
- `table->u.view.pSelect` used when accessing view's SELECT

### 4. Fixed Expression List Field Access
**Problem**: expr_list_item field renamed
**Changes**:
- `elist->a[e].zName` → `elist->a[e].zEName`

### 5. Removed parser->initFlag Checks
**Problem**: initFlag field removed from parser
**Changes**:
- Removed conditional wrapping of `__ref_normalizer_ctx_init` calls

### 6. Fixed VDBE Operation Code Generation
**Problem**: Static vdbe_op_t arrays with string initializers no longer work
**Changes**:
- Replaced static arrays with sequential `__vdbe_add_op()` calls
- Used `__vdbe_change_p4()` (not p3) for string parameters
- Fixed jump address handling with explicit label tracking

### 7. Added Temporary Parser Fields
**Problem**: Code still uses old trigger stack mechanism
**Changes** (temporary workaround in `/home/gburd/ws/dbsql/src/inc/dbsql_int.in`):
- Added `trigger_stack_t *trigStack` to parser struct
- Added `int iDb` to parser struct
- Note: These are marked TEMP and should be migrated to new SQLite 3.52.0 mechanisms

### 8. Fixed Header Template Variables
**Problem**: dbsql_int.in has @template@ variables for configure
**Changes**:
- Replaced `@uintmax_t_decl@` with `#include <stdint.h>`
- Replaced `@uintptr_t_decl@` with comment (defined in stdint.h)

## Build Verification
```bash
cd /home/gburd/ws/dbsql/build_unix
make cg_trigger.lo
ls -lh .libs/cg_trigger.o
# Output: -rw-r--r-- 1 gburd users 111K Mar 17 08:21 .libs/cg_trigger.o
```

## Files Modified
1. `/home/gburd/ws/dbsql/src/cg_trigger.c` - Main fixes
2. `/home/gburd/ws/dbsql/src/inc/dbsql_int.in` - Added temporary parser fields and fixed template variables
3. `/home/gburd/ws/dbsql/build_unix/dbsql_int.h` - Regenerated from .in file

## Next Steps (Future Tasks)
The following files still have compilation errors and need similar fixes:
- `cg_build.c` - Multiple errors with iDb, autoIndex, isTransient, etc.
- `cg_delete.c` - Similar patterns as cg_insert.c
- `cg_update.c` - Similar patterns as cg_insert.c
- `cg_expr.c` - Expression handling updates
- `cg_select.c` - SELECT statement handling
- Other cg_*.c files
- VDBE files (vdbe.c, vdbe_method.c)
- API files (api.c, api_table.c)

All fix patterns are documented in `/home/gburd/ws/dbsql/TASK_28_FIX_PATTERNS.md`.

## Technical Notes

### Trigger Stack Migration
The old DBSQL trigger mechanism used `parser->trigStack` to track recursive trigger invocations. SQLite 3.52.0 replaced this with:
- `parser->pTriggerPrg` - Compiled trigger programs
- `parser->pTriggerTab` - Table being triggered
- `parser->pToplevel` - Top-level parser context

The temporary `trigStack` field allows the old code to continue working, but should eventually be migrated to the new mechanism.

### Database Index Resolution
The old code stored database indices directly in structures (`table->iDb`, `index->iDb`, `trigger->iDb`). The new code uses schema pointers and resolves indices via:
```c
int db_idx = __schema_to_index(dbp, object->pSchema);
```

This provides better encapsulation and allows for dynamic schema management.

### VDBE Instruction Format
The vdbe_op_t structure changed from having P3 as char* to having a typed P4 union. String parameters now:
1. Use `__vdbe_add_op()` to create the instruction
2. Use `__vdbe_change_p4(v, addr, string, P4_STATIC/P4_DYNAMIC)` to set strings

Static initializer arrays with embedded strings no longer work due to union restrictions.

## Completion
Date: 2026-03-17
File: cg_trigger.c
Status: ✅ Compiles with ZERO errors
Object File: `/home/gburd/ws/dbsql/build_unix/.libs/cg_trigger.o` (111KB)
