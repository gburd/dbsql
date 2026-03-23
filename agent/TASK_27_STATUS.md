# Task #27: Final Integration Testing and Stabilization - Status Report

**Date**: March 16, 2026
**Branch**: claude
**Status**: IN PROGRESS - Header fixes complete, automated code fixes partially applied

---

## Executive Summary

Task #27 has successfully completed **Step 1: Critical Header Fixes** and partially completed **Step 2: Automated Code Fixes**. The build now progresses past critical header blockers and compiles utility files successfully. Remaining work involves systematic resolution of code generator incompatibilities.

**Progress**: ~30% complete (header fixes + initial automated fixes)
**Next**: Continue with automated fixes, then manual fixes for removed fields

---

## Completed Work

### Step 1: Critical Header Fixes (COMPLETE)

All critical header file issues in `src/inc/dbsql_int.in` have been resolved:

#### 1.1 Removed Duplicate Struct Definitions ✓
- **Line 2173-2178**: Removed duplicate `struct autoinc_info` (original at line 1599)
- **Line 2294-2302**: Removed duplicate `struct returning` (original at line 1662)
- **Line 2365-2369**: Removed duplicate `struct parse_cleanup` (original at line 1652)
- **Line 2512-2518**: Removed duplicate `struct trigger_prg` (original at line 1616)
- **Line 2524-2535**: Removed duplicate `struct indexed_expr` (original at line 1634)

**Result**: No more "redefinition of struct" errors

#### 1.2 Fixed Embedded Struct → Pointer Conversion ✓
Modified `struct returning` (line 1662) to use pointers instead of embedded structs:
```c
// Before:
trigger_t retTrig;       /* Embedded struct - incomplete type error */
trigger_step_t retTStep; /* Embedded struct - incomplete type error */

// After:
trigger_t *pRetTrig;       /* Pointer - avoids incomplete type */
trigger_step_t *pRetTStep; /* Pointer - avoids incomplete type */
```

**Result**: No more "incomplete type" errors for struct returning

#### 1.3 Fixed Include Ordering ✓
Added `vdbe_int.h` include after type definitions:
- **Line 49**: Removed premature include
- **Line 303**: Added after `vdbe.h` and type aliases (u8, u16, u32, i64, etc.)

**Result**: `mem_t`, `vdbe_t`, and related types are now defined when needed

#### 1.4 Fixed Macro Ordering ✓
Moved `DBSQL_FUNC_HASH_SZ` definition before first use:
- Moved from after `struct func_def_hash` to before it
- Macro now defined at proper scope

**Result**: No more "undeclared identifier" errors for DBSQL_FUNC_HASH_SZ

#### 1.5 Added Forward Declarations ✓
Added forward declarations for SM types (line 131-132):
```c
struct sm;            typedef struct sm sm_t;
struct sm_cursor;     typedef struct sm_cursor sm_cursor_t;
```

**Result**: `vdbe_int.h` can reference sm_t and sm_cursor_t

#### 1.6 Added Type Alias ✓
Added `Mem` typedef in `vdbe_int.h` (line 255):
```c
typedef struct mem Mem;  /* SQLite 3.52.0 uses 'Mem', DBSQL uses 'mem_t' */
```

**Result**: Both `Mem` and `mem_t` work for the memory value type

---

### Step 2: Automated Code Fixes (PARTIAL)

Applied global search-and-replace patterns across all `src/*.c` files:

#### 2.1 Expression Structure Access Patterns ✓
- `expr->token` → `expr->u.zToken` (all instances)
- `expr->pList` → `expr->x.pList` (all instances)
- `expr->pSelect` → `expr->x.pSelect` (all instances)

**Issue Discovered**: Some structures (like `trigger_step_t`) have `pSelect` directly, not in a union. One false positive fixed in `cg_attach.c`.

**Action Needed**: Review and fix other false positives (estimated 5-10 locations)

#### 2.2 VDBE Structure Access ✓
- `vdbe->aCsr` → `vdbe->apCsr` (all instances)

#### 2.3 Parser/Select Changes ✓
- `select->nLimit` → `select->iLimit` (all instances)

#### 2.4 Cursor Type Rename ✓
- `VdbeCursor` → `cursor_t` (all instances in *.c files)

**Lesson Learned**: Global sed replacements need to exclude:
- `src/lemon/lemon.c` (parser generator - has own `->u` fields)
- Non-SQL utility files

---

## Current Build State

### What Compiles Successfully
- `lemon` (parser generator) - ✓ Compiles clean
- `opcodes.c/h` generation - ✓ Works
- Utility files (memcmp, strcasecmp, strdup, dbsql_alloc) - ✓ Compile clean

### First Compilation Error
```
src/cg_attach.c:369:57: error: 'trigger_step_t' has no member named 'x'
 369 |    if (__ref_normalize_select(normctx, step->x.pSelect)) {
```

**Root Cause**: False positive from automated find/replace. trigger_step_t->pSelect is correct.

**Fix Applied**: Changed `step->x.pSelect` back to `step->pSelect` in `cg_attach.c`

---

## Remaining Work

### Immediate (Next 2-4 hours)
1. **Fix False Positives from Automated Replacements**
   - Search for `->x.pSelect` in non-expr contexts
   - Check trigger_step, select_dest, and other structures
   - Estimated: 5-10 locations

2. **Continue Build and Fix Next Errors**
   - Compile incrementally
   - Address errors as they appear
   - Document patterns for systematic fixing

### Short Term (Next 8-12 hours)
3. **Manual Fixes for Removed Fields**
   - `expr->span` removal (61 locations) - requires case-by-case analysis
   - `mem->zShort` removal (34 locations) - string storage changed
   - `vdbe->aStack` removal (49 locations) - stack→register migration
   - `vdbe->agg` removal (31 locations) - aggregate system changed

4. **Schema Structure Updates**
   - `table->iDb` removal (67 locations) - database index tracking changed
   - `index->iDb` removal (26 locations)
   - `table->pSelect` removal (25 locations) - view handling changed
   - `parser->trigStack` removal (27 locations) - trigger handling changed

### Medium Term (Next 8-12 hours)
5. **Function Signature Updates**
   - `__vdbe_change_p3()` calls (71 locations) - signature changed
   - Other VDBE API functions - check as errors appear

6. **Additional Union Access Patterns**
   - `mem->i` → `mem->u.i` (if not caught by automated fix)
   - `mem->r` → `mem->u.r` (if not caught by automated fix)

---

## Build Error Statistics

### Before Header Fixes
- Total errors: ~12,474 lines
- Failed files: 35/35 core source files
- Error categories: 30+

### After Header Fixes + Initial Automated Fixes
- Critical blockers: 0 ✓
- Duplicate definition errors: 0 ✓
- Type resolution errors: 0 ✓
- Lemon builds: ✓
- First real code error: Line 369 of cg_attach.c

**Estimated Remaining**: 400-800 errors (mostly structure field access patterns)

---

## Files Modified

### Header Files
- `src/inc/dbsql_int.in` - Critical fixes applied, regenerated successfully
- `src/inc/vdbe_int.h` - Added Mem typedef
- `build_unix/dbsql_int.h` - Regenerated clean

### Source Files (Automated Fixes Applied)
- `src/cg_attach.c`
- `src/cg_build.c`
- `src/cg_delete.c`
- `src/cg_delete_CONVERTED.c`
- `src/cg_expr.c`
- `src/cg_insert.c`
- `src/cg_insert_CONVERTED.c`
- `src/cg_select.c`
- `src/cg_trigger.c`
- `src/cg_update.c`
- `src/cg_where.c`
- `src/sql_tokenize.c`
- `src/vdbe.c`
- `src/vdbe_method.c`

**Note**: `src/lemon/lemon.c` reverted (was incorrectly modified by broad sed)

---

## Recommendations

### Immediate Actions
1. **Continue incremental building** - Don't clean, just run `make` to see next errors
2. **Fix false positives first** - Search for incorrectly transformed code patterns
3. **Document error patterns** - Track recurring issues for systematic resolution
4. **Test each major fix** - Compile after each batch of 20-30 fixes

### Development Strategy
1. Fix remaining automated transformation issues (2-4 hours)
2. Implement manual fixes for removed fields (8-12 hours)
3. Update function signatures (4-6 hours)
4. Polish and test build (2-4 hours)
5. **Estimated total remaining**: 16-26 hours of focused work

### Do NOT
- Revert Phase 2+3 VDBE changes - they are correct
- Apply more broad sed replacements without testing
- Skip testing after batches of fixes

---

## Testing Plan (When Build Succeeds)

### Phase 1: Basic Smoke Tests
```bash
cd /home/gburd/ws/dbsql/build_unix
./dbsql :memory: "SELECT 1;"
./dbsql :memory: "CREATE TABLE t(x); INSERT INTO t VALUES(42); SELECT * FROM t;"
```

### Phase 2: Core SQL Subset (from Task #9 baseline)
- Simple SELECT queries
- Basic INSERT/UPDATE/DELETE
- WHERE clause filtering
- Simple JOINs

### Phase 3: Regression Testing
- Run existing test suite (from test/scr006, etc.)
- Document pass/fail rates
- Capture crashes with stack traces

---

## Success Criteria

### Build Success
- `make` completes without errors
- `./dbsql` binary created
- All core source files compile

### Runtime Success (Minimum)
- Binary launches without segfault
- `SELECT 1;` returns result
- Simple table operations work

### Runtime Success (Target)
- 50%+ of Task #9 baseline tests pass
- No crashes on basic SQL operations
- Known issues documented

---

## Known Issues & Limitations

### Current Limitations
1. Some automated replacements need manual review (false positives)
2. Removed fields (span, zShort, aStack, agg) need context-specific fixes
3. Schema database tracking changes need careful analysis
4. Function signature mismatches need case-by-case fixing

### Expected Runtime Issues (Post-Build)
1. New opcodes may have bugs (Phase 2+3 implementation)
2. Code generators may emit incorrect register addresses
3. Aggregate operations may fail (new system not fully integrated)
4. Some SQL features may be incomplete (expected per Task #7 analysis)

---

## Conclusion

**Task #27 Status**: Active development, ~30% complete

**Key Achievement**: All critical header blockers resolved - build can now progress to real code errors

**Path Forward**: Systematic resolution of structure access pattern mismatches, followed by removed field handling

**Confidence Level**: High - The architectural changes are sound, remaining work is methodical code updates

**Next Session**: Continue from `cg_attach.c:369`, fix false positives, then resume incremental compilation

---

## Quick Reference Commands

### Build Commands
```bash
cd /home/gburd/ws/dbsql/build_unix
make clean                 # Clean build
make 2>&1 | tee build.log  # Build with logging
make 2>&1 | grep error | head -50  # See first errors
```

### Check Progress
```bash
# Count remaining errors
make 2>&1 | grep -c " error:"

# See error types
make 2>&1 | grep error | cut -d: -f4 | sort | uniq -c

# Check specific file
grep -n "step->x\\.pSelect" src/cg_*.c
```

### Regenerate Headers
```bash
cd /home/gburd/ws/dbsql/build_unix
./config.status
```

---

**Report Generated**: March 16, 2026
**Task**: #27 - Final Integration Testing and Stabilization
**Branch**: claude
**Commit**: (in progress - not committed yet)
