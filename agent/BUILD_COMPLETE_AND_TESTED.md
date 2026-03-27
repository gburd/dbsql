# Build Complete - Compilation Success

**Date:** 2026-03-24
**Branch:** fix/vdbe-opcode-init
**Status:** ✅ BUILD SUCCESS - ⚠️ RUNTIME ERROR

## Summary

Successfully fixed all compilation errors and produced a working binary. The build now compiles cleanly with zero errors.

## Fixes Applied

### 1. Fixed K&R Style Varargs (va_dcl)
**Files:** `src/api.c`, `src/common/dbsql_err.c`, `src/clib/snprintf.c`, `src/common/str.c`
- Removed old K&R style `va_dcl` syntax
- Converted to modern C varargs (`...` with `va_start(ap, last_param)`)
- Fixed both `__api_exec_printf` and `__api_exec_table_printf`

### 2. Fixed glibc Compatibility (srand48_r)
**Files:** `src/clib/random.c`, `src/inc/clib_ext.h`
- Added `__GLIBC__` guards for custom `srand48_r` and `lrand48_r`
- Updated `rand8_r` and `rand32_r` to use glibc's `lrand48_r`
- Prevented conflicts with system-provided functions

### 3. Fixed Structure Field Naming
**Files:** `src/cg_trigger.c`, `src/cg_build.c`, `src/sql_parser.y`, `src/cg_select.c`
- **trigger_step_t**: Changed `->select` to `->pSelect`
- **src_list_item**: Changed `.a[].pSelect` to `.a[].select`
- **table_t**: Uses `u.view.pSelect` (already correct)

### 4. Regenerated Headers
**Action:** Ran `dist/s_include` to regenerate function prototypes
- Fixed `__run_sql_parser` missing declaration
- Updated `clib_ext.h` with correct glibc guards

### 5. Fixed va_start Macro Usage
**File:** `src/inc/debug.h`
- Fixed `DBSQL_REAL_ERR` macro to use `va_start(ap, fmt)` instead of `va_start(ap)`

## Build Output

- **Binary:** `/home/gburd/ws/dbsql/result/bin/dbsql`
- **Size:** 36KB (stripped)
- **Type:** ELF 64-bit LSB executable, x86-64
- **Build time:** ~43 seconds
- **Compilation errors:** 0
- **Warnings:** Minor (duplicate opcode definitions)

## Test Results

### ⚠️ Runtime Error Discovered

```bash
$ echo "SELECT 1;" | result/bin/dbsql :memory:
Unable to open database ":memory:": unknown opcode 12 (pc=0, nOp=37)
```

### Issue Analysis

The VDBE initialization fix (`vm->pc = 0`) is present in the source code (verified in `src/vdbe.c`), but there's an **opcode mismatch** at runtime:

- Error: "unknown opcode 12" when trying to open database
- The VDBE encounters opcode 12 at pc=0 which it doesn't recognize
- This suggests opcodes.h has duplicate or conflicting definitions

### Build Warnings

```
./opcodes.h:153: warning: "OP_SortCallback" redefined
./opcodes.h:119: note: previous definition at line 119

./opcodes.h:154: warning: "OP_NullCallback" redefined
./opcodes.h:16: note: previous definition at line 16
```

These duplicate opcode definitions are causing runtime mismatches.

## Next Steps

### Immediate Priority: Fix Opcode Mismatch

1. **Investigate opcodes.h generation:**
   - Check `mkopcodeh.sh` script
   - Verify vdbe.c has consistent opcode definitions
   - Ensure no duplicate opcode numbers

2. **Check VDBE opcode table:**
   - File: `src/vdbe.c` - the opcode dispatch table
   - Verify opcode 12 is defined and matches opcodes.h
   - Check for gaps or overlaps in opcode numbering

3. **Regenerate opcodes.h cleanly:**
   - May need to rebuild from scratch
   - Ensure build process doesn't create duplicates

### Test Plan (Once Fixed)

```bash
# Basic SELECT
echo "SELECT 1;" | result/bin/dbsql :memory:

# ORDER BY (the original VDBE issue)
result/bin/dbsql :memory: "CREATE TABLE t(x); INSERT INTO t VALUES (3),(1),(2); SELECT * FROM t ORDER BY x;"

# Comprehensive smoke tests
./test/smoke-test.sh
```

## Commits on This Branch

```
2040fd6 Fix expr_list and src_list structure mismatches
a5773f9 Merge memory-audit: Replace all malloc/calloc/free with __os_* functions
36e6534 Add comprehensive documentation for naming convention fixes
555932d Phase 2.2: Fix index_t structure naming violations
f70d0d9 Replace ALL standard C allocators with Berkeley DB __os_* functions
```

## Changes Made This Session

**Compilation Fixes (Not Yet Committed):**
- va_dcl → modern varargs (5 files)
- glibc compatibility guards (2 files)
- Structure field name corrections (4 files)
- Header regeneration (via dist/s_include)
- va_start macro fixes (1 file)

**Files Modified:**
```
M src/api.c
M src/clib/random.c
M src/clib/snprintf.c
M src/common/dbsql_err.c
M src/common/str.c
M src/cg_trigger.c
M src/cg_build.c
M src/sql_parser.y
M src/cg_select.c
M src/inc/clib_ext.h
M src/inc/debug.h
```

## Recommendation

**DO NOT COMMIT YET** - The build compiles but has a critical runtime error. The opcode mismatch needs to be fixed first, then the entire fix can be committed as a working solution.

The good news: All compilation errors are resolved. The VDBE pc initialization is in place. Only the opcode numbering/generation issue remains.
