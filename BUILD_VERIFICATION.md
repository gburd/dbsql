# Build Verification - Final Integration Phase

**Date**: 2026-03-18
**Branch**: feature/sqlite-3.52-modernization
**Task**: Agent 3 - Final integration, testing, and PR creation

## Current Build Status

### Initial Build Attempt
```bash
cd /home/gburd/ws/dbsql/build_unix
make clean
make 2>&1 | tee /tmp/claude/build.log
```

**Results**:
- Total compilation errors: 17
- Files compiling: 2 object files created
- Main error categories: 2

## Error Analysis

### Error Category 1: Incorrect Phase 2 Field Renames (7 occurrences)
**Pattern**: `index->next_vdbe`
**Issue**: Phase 2 rename incorrectly changed `index->pNext` to `index->next_vdbe`
**Correct**: Should be `index->pNext`

**Affected files**:
- src/cg_insert.c (5 occurrences)
- Plus 15 other files found by grep

### Error Category 2: VDBE API Change - P3 to P4 (10 occurrences)
**Pattern**: `__vdbe_change_p3(v, addr, "string", P3_STATIC)`
**Issue**: SQLite 3.52.0 changed string parameters from P3 to P4
**Correct**: `__vdbe_change_p4(v, addr, "string", P3_STATIC)`

**Affected files**:
- src/cg_insert.c (7 occurrences in current build)
- src/cg_build.c
- src/cg_copy.c
- src/cg_delete.c (both versions)
- src/cg_insert.c (both versions)

## Systematic Fix Plan

### Fix 1: Restore Correct index->pNext Field Name
```bash
cd /home/gburd/ws/dbsql/src
# Find all incorrect next_vdbe references in index_t context
rg "index.*->next_vdbe" -l
# Replace with correct pNext
sed -i 's/->next_vdbe/->pNext/g' *.c
```

**Files to fix** (16 files):
- api.c
- cg_where.c
- cg_update.c
- cg_trigger.c
- cg_select.c
- cg_pragma.c
- cg_insert_CONVERTED.c
- cg_insert.c
- cg_expr.c
- cg_delete_CONVERTED.c
- cg_delete.c
- cg_copy.c
- cg_build.c
- vdbe_method.c
- vdbe.c
- inc/vdbe_int.h

### Fix 2: Update __vdbe_change_p3 to __vdbe_change_p4
```bash
cd /home/gburd/ws/dbsql/src
# Find all p3 calls with string arguments
rg '__vdbe_change_p3.*\(.*,.*,.*"' -l
# Replace with p4
sed -i 's/__vdbe_change_p3\(([^,]*,[^,]*,\s*"[^"]*",\)/__vdbe_change_p4\1/g' *.c
```

**Files to fix** (5 files):
- cg_insert_CONVERTED.c
- cg_insert.c
- cg_delete_CONVERTED.c
- cg_copy.c
- cg_build.c

## Estimated Fix Time

- **Fix 1** (next_vdbe → pNext): 5 minutes (automated sed)
- **Fix 2** (p3 → p4): 5 minutes (automated sed)
- **Build verification**: 10 minutes
- **Total**: 20 minutes

## Expected Outcome

After applying these fixes:
- Compilation errors: 17 → 0 (estimated)
- All code generator files should compile
- Ready for final integration testing

## Next Steps After Fixes

1. Apply Fix 1 (next_vdbe → pNext)
2. Apply Fix 2 (__vdbe_change_p3 → __vdbe_change_p4)
3. Rebuild and verify zero errors
4. Document final build status
5. Create comprehensive commit
6. Push to remote
7. Create PR on Codeberg

## Risk Assessment

**LOW RISK**:
- Both fixes are mechanical search/replace
- Patterns are well-defined and unambiguous
- No manual code analysis required
- Can be verified immediately with build
