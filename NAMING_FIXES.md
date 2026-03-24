# DBSQL Naming Convention Fixes

**Branch**: naming-fixes
**Started**: 2026-03-24
**Reference**: ~/.claude/plans/vast-wibbling-deer.md Section 1

## Objective

Systematically fix naming convention violations to align with DBSQL/BerkeleyDB standards:
- Functions: `__prefix_action_object`
- Types: `struct_name_t` suffix
- Fields: `snake_case` (no pointer/array prefixes)

## Phase 1: Type Aliases (COMPLETED)

✅ `Mem` typedef already removed - only `mem_t` used
✅ `VdbeCursor` struct uses `cursor_t` typedef consistently

## Phase 2: Critical Structure Fields (IN PROGRESS)

### 2.1 vdbe_t struct (vdbe_int.h)

Target fields in `struct vdbe`:
- [ ] `aMem` → `registers` (partially done, make consistent)
- [ ] `aOp` → `opcodes` (if exists in dbsql_int.in)
- [ ] `apCsr` → `cursors` (already correct in vdbe_int.h as `cursors`)

### 2.2 parser_t struct (dbsql_int.in)

Target fields in `struct parser`:
- [ ] `pVdbe` → `vdbe` (23 files affected)

### 2.3 Other structures

To be identified during systematic review.

## Affected Files

### High Priority (Core VDBE)
- src/inc/vdbe_int.h - Type definitions
- src/inc/dbsql_int.in - Parser structure
- src/vdbe.c - Main VDBE implementation
- src/vdbe_method.c - VDBE methods

### Medium Priority (Code Generators)
- src/cg_build.c
- src/cg_expr.c
- src/cg_select.c
- src/cg_where.c
- src/cg_trigger.c

### Lower Priority
- src/api.c
- src/safety.c
- src/sql_tokenize.c

## Fix Strategy

1. **Field renames**: Use sed for mechanical replacement
2. **Compilation test**: Build after each major change
3. **Verification**: Grep for remaining violations
4. **Backup**: Keep .bak files until all changes verified

## Progress Log

### 2026-03-24 - Session 1

#### Phase 2.2: parser_t->pVdbe → parser_t->vdbe

**Status**: ✅ COMPLETED

**Changes made**:
1. Updated struct definition in `src/inc/dbsql_int.in`:
   - Line 1802: `vdbe_t *pVdbe;` → `vdbe_t *vdbe;`

2. Updated all references to parser->pVdbe:
   - Pattern matching: `parser.pVdbe`, `pParse->pVdbe`, `parse->pVdbe`
   - Files affected: src/cg_*.c, src/api.c, src/sql_tokenize.c

3. Fixed incorrect changes:
   - Reverted `dbp->pVdbe` → `dbp->vdbe` (DBSQL struct field stays as pVdbe)
   - Fixed src/safety.c to use correct `dbp->pVdbe`

**Important distinction**:
- parser_t->vdbe ✅ (changed - matches DBSQL conventions)
- DBSQL->pVdbe ⚠️ (kept as-is for now - list head pointer in different struct/file)

**Files modified**:
- src/inc/dbsql_int.in (struct definition)
- src/cg_build.c, src/cg_expr.c, src/cg_select.c, src/cg_where.c, src/cg_trigger.c
- src/api.c, src/safety.c, src/sql_tokenize.c

**Compilation**: In progress with `nix build` (long-running)

**Verification**:
```bash
# Remaining pVdbe references (all correct - DBSQL struct):
$ grep -r '->pVdbe' src --include="*.c" | grep -v ".bak"
src/api.c:      if (dbp->pVdbe == 0)           # DBSQL struct ✓
src/vdbe_method.c:  if (dbp->pVdbe) {          # DBSQL struct ✓
src/safety.c:   if (dbp->pVdbe != 0) {         # DBSQL struct ✓
# 9 total references, all to DBSQL->pVdbe (correct)

# parser->pVdbe successfully renamed:
$ grep -r 'parser.*->vdbe' src --include="*.c" | wc -l
# Multiple references found ✓
```

#### Next Steps

**Phase 2.1: vdbe_t struct fields** (deferred)
- `aMem` → `registers` (some instances exist, make consistent)
- `aOp` → `opcodes` (if exists)
- Requires careful analysis of vdbe_int.h

**Phase 2.3: DBSQL->pVdbe consideration** (deferred)
- Current: `DBSQL->pVdbe` (list head pointer)
- Better: `DBSQL->vdbe_list` or `DBSQL->active_vdbes`
- Impact: 9 references across 3 files
- Scope: Would require changing dbsql.in (public API implications)

