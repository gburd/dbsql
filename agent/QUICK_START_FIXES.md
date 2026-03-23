# Quick Start: Fixing Build Errors

This is a condensed guide for fixing the build. For full details, see:
- `build_status_report.md` - Complete error analysis
- `fix_priority_list.md` - Detailed fix instructions
- `TASK_26_COMPLETE.md` - Task summary

## Current State

Build FAILS with ~1,700 errors across 35 source files. Root cause: Phase 2+3 modernized VDBE but code generators not yet updated.

## Fix in 4 Steps

### Step 1: Fix Headers (1 hour, CRITICAL)

**File**: `/home/gburd/ws/dbsql/src/inc/dbsql_int.in`

**Edit 1**: Remove duplicate struct definitions
- Delete lines 2173-2178 (duplicate `struct autoinc_info`)
- Delete lines 2294-2303 (duplicate `struct returning`)
- Delete lines 2384-2388 (duplicate `struct parse_cleanup`)
- Delete lines 2532-2540 (duplicate `struct trigger_prg`)
- Delete lines 2544-2564 (duplicate `struct indexed_expr`)

**Edit 2**: Fix incomplete types in `struct returning` (around line 1662)
```c
// Change these two lines:
trigger_t retTrig;        → trigger_t *pRetTrig;
trigger_step_t retTStep;  → trigger_step_t *pRetTStep;
```

**Edit 3**: Add VDBE types include (around line 48, after `globals.h`)
```c
#include "inc/globals.h"
#include "inc/vdbe_int.h"  // <-- ADD THIS LINE
```

**Edit 4**: Move macro definition (around line 2560-2567)
```c
// Move these 3 lines BEFORE the struct func_def_hash definition:
#ifndef DBSQL_FUNC_HASH_SZ
#define DBSQL_FUNC_HASH_SZ 61
#endif
```

**Regenerate**:
```bash
cd /home/gburd/ws/dbsql/build_unix
./config.status
```

**Verify**: All 4 critical blockers should be gone.

---

### Step 2: Run Automated Fixes (30 minutes)

**From**: `/home/gburd/ws/dbsql` directory

```bash
# Fix expression structure access
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->token/\1->u.zToken/g' {} \;
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->pList/\1->x.pList/g' {} \;
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->pSelect/\1->x.pSelect/g' {} \;

# Fix memory value structure access
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->i\([^a-zA-Z0-9_]\)/\1->u.i\2/g' {} \;
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->r\([^a-zA-Z0-9_]\)/\1->u.r\2/g' {} \;

# Fix cursor type names
find src -name "*.c" -type f -exec sed -i 's/\bVdbeCursor\b/cursor_t/g' {} \;

# Fix VDBE structure access
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->aCsr/\1->apCsr/g' {} \;

# Fix schema structure access
find src -name "*.c" -exec sed -i 's/\([a-zA-Z_][a-zA-Z0-9_]*\)->nLimit/\1->iLimit/g' {} \;
```

**Test**: Try to build - many errors will remain but several hundred should be gone.

---

### Step 3: Manual Fixes (16-20 hours)

These require case-by-case analysis. Work through in order:

**3a. Remove expr->span references** (61 locations)
- Field removed in SQLite 3.52.0
- Check each usage context
- Usually can be deleted or replaced with token offset

**3b. Remove mem->zShort references** (34 locations)
- Field removed - string storage changed
- Replace with proper string handling

**3c. Remove vdbe->aStack references** (49 locations)
- Stack-based model removed
- Reimplement using register-based model

**3d. Remove vdbe->agg references** (31 locations)
- Aggregate handling changed
- Update to new aggregate API

**3e. Remove table->iDb, index->iDb references** (93 locations)
- Database index tracking moved
- Get database context differently

**3f. Remove table->pSelect references** (25 locations)
- View handling changed
- Update view detection logic

**3g. Remove parser->trigStack references** (27 locations)
- Trigger handling changed
- Update trigger implementation

**3h. Fix __vdbe_change_p3() calls** (71 locations)
- Function signature changed
- Update call sites

---

### Step 4: Test (2-4 hours)

After each major fix batch:
```bash
cd /home/gburd/ws/dbsql/build_unix
make clean
make 2>&1 | tee build_test.log
```

When build succeeds:
```bash
./dbsql :memory: "SELECT 1;"
./dbsql :memory: "CREATE TABLE t(x); INSERT INTO t VALUES(42); SELECT * FROM t;"
```

---

## Error Count Tracker

Use this to track progress:

| Fix Stage | Expected Remaining Errors |
|-----------|---------------------------|
| Start | ~1,700 |
| After Step 1 (headers) | ~900 |
| After Step 2 (automated) | ~400 |
| After Step 3a-3d (partial manual) | ~200 |
| After Step 3e-3h (complete manual) | 0 |

---

## If You Get Stuck

1. **Headers won't regenerate**: Check syntax in dbsql_int.in
2. **sed commands fail**: Run them one at a time to find problematic patterns
3. **Too many errors**: Focus on one file at a time (start with cg_pragma.c - simplest)
4. **Build still fails after all fixes**: Check build_full.log for new error patterns

---

## Files You'll Edit Most

Core code generators (most work needed):
- `src/cg_expr.c` - Expression handling
- `src/cg_select.c` - SELECT queries
- `src/cg_insert.c` - INSERT statements
- `src/cg_update.c` - UPDATE statements
- `src/cg_delete.c` - DELETE statements
- `src/vdbe.c` - VDBE implementation
- `src/vdbe_method.c` - VDBE methods

---

## Success Criteria

Build completes without errors:
```bash
cd /home/gburd/ws/dbsql/build_unix
make clean && make
echo $?  # Should print 0
```

Binary works for basic SQL:
```bash
./dbsql :memory: "SELECT 1;"
# Should print: 1
```

---

## Time Estimate Breakdown

- Step 1 (headers): 1 hour
- Step 2 (automated): 30 minutes
- Step 3a-3d (expression/mem/vdbe): 8-10 hours
- Step 3e-3h (schema/parser/api): 8-10 hours
- Step 4 (testing): 2-4 hours
- **Total**: 20-29 hours

---

## Questions?

Check the detailed documentation:
- Error specifics: `build_status_report.md`
- Line numbers: `fix_priority_list.md`
- Context: `TASK_26_COMPLETE.md`
- Full build output: `build_full.log`
