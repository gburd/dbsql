# Disabled Code Analysis - DBSQL Codebase

## Executive Summary

This document catalogs all preprocessor-disabled code blocks (`#if 0`) in the DBSQL codebase and determines appropriate actions for each.

## Critical Issue: Missing dbsql_config.h

**Status**: BLOCKING COMPILATION

73 source files include `dbsql_config.h` but the file doesn't exist. This header is included by:
- `build_unix.old/dbsql_int.h` (line 29)
- All `.c` files that include `dbsql_config.h` directly (line 26)

**Analysis**:
- The file `dbsql_config.h` is referenced but never defined
- No `config.h.in` or generation mechanism found in build system
- This appears to be a legacy reference from autoconf-based build
- Current Nix build system doesn't generate this file

**Action**: CREATE empty stub header or remove includes

---

## #if 0 Blocks Found

### Category 1: Legacy VDBE Opcodes - CONFIRMED DEAD CODE

These blocks reference removed data structures and should be REMOVED.

#### Block 1: src/vdbe.c:224
```c
#if 0  /* DISABLED - agg_t/agg_elem_t removed */
```
**Purpose**: Legacy aggregator opcodes (OP_AggReset, OP_AggFocus, OP_AggNext)
**Reason**: SQLite 3.52.0 removed the legacy aggregator; these types no longer exist
**Data structures referenced**: `agg_t`, `agg_elem_t` (removed from codebase)
**Action**: REMOVE - Dead code, types don't exist

---

#### Block 2: src/vdbe.c:477
```c
#if 0  /* DISABLED - Legacy sorter helper for removed Sort opcodes (sorter_t type removed) */
```
**Purpose**: Helper function `__file_sorter` for old Sort opcodes
**Reason**: Modern SQLite uses OP_Sorter* family; old Sort opcodes removed
**Data structures referenced**: `sorter_t` (removed)
**Action**: REMOVE - Dead code, types don't exist

---

#### Block 3: src/vdbe.c:1099
```c
#if 0  /* DISABLED - Legacy callback opcodes (azColName/xCallback/nCallback removed) */
```
**Purpose**: OP_ColumnName, OP_ColumnCount opcodes
**Reason**: Legacy callback mechanism removed; uses removed fields `p->azColName`, `p->xCallback`
**Action**: REMOVE - Dead code, fields don't exist

---

#### Block 4: src/vdbe.c:1195
```c
#if 0  /* DISABLED - Legacy callback (xCallback/azColName removed) */
```
**Purpose**: OP_Callback opcode
**Reason**: Uses removed `p->xCallback` and `p->azColName` fields
**Action**: REMOVE - Dead code, fields don't exist

---

#### Block 5: src/vdbe.c:1437
```c
#if 0  /* DISABLED - Legacy stack-based OP_Function (uses removed p->zArgv) */
```
**Purpose**: Old stack-based OP_Function implementation
**Reason**: Uses removed `p->zArgv` field
**Action**: REMOVE - Dead code, modern OP_Function uses different mechanism

---

#### Block 6: src/vdbe.c:4488
```c
#if 0  /* DISABLED - keylistStack/x.expr_list removed, use RowSet instead */
```
**Purpose**: OP_ListWrite, OP_ListRewind, OP_ListRead opcodes
**Reason**: Legacy keylist mechanism replaced by RowSet; uses removed `keylistStack`, `x.pList`
**Action**: REMOVE - Dead code, replaced by RowSet opcodes

---

#### Block 7: src/vdbe.c:4612
```c
#if 0  /* DISABLED - Legacy Sort opcodes (p->pSort removed, use OP_Sorter* instead) */
```
**Purpose**: OP_SortPut, OP_SortMakeRec, OP_SortMakeKey, OP_Sort, OP_SortNext, OP_SortCallback, OP_SortReset
**Reason**: Legacy sort mechanism removed; uses `p->pSort` field (removed)
**Data structures referenced**: `sorter_t`, `p->pSort` (removed)
**Action**: REMOVE - Dead code, completely replaced by OP_Sorter* family

---

#### Block 8: src/vdbe.c:4816
```c
#if 0  /* DISABLED - Legacy callback (xCallback/azResColumn removed) */
```
**Purpose**: OP_SortCallback opcode continuation
**Reason**: Uses removed `p->xCallback`, `p->azResColumn` fields
**Action**: REMOVE - Dead code, callback mechanism removed

---

#### Block 9: src/vdbe.c:4863
```c
#if 0  /* DISABLED - Legacy File/COPY opcodes (pFile/azField/nField/zLine/zShort removed) */
```
**Purpose**: OP_FileOpen, OP_FileClose, OP_FileRead, OP_FileColumn, OP_MemStore, OP_MemLoad
**Reason**: Legacy file I/O for COPY command; uses removed fields
**Data structures referenced**: `p->pFile`, `p->azField`, `p->nField`, `p->zLine`, `p->zShort` (all removed)
**Action**: REMOVE - Dead code, COPY command mechanism changed

---

#### Block 10: src/vdbe.c:5152
```c
#if 0  /* DISABLED - Legacy aggregate opcodes (p->agg removed) */
```
**Purpose**: OP_AggReset, OP_AggFocus, OP_AggIncr, OP_AggNext, OP_AggSet, OP_AggGet
**Reason**: Legacy aggregate mechanism; uses removed `p->agg` field
**Action**: REMOVE - Dead code, aggregate mechanism completely redesigned

---

#### Block 11: src/vdbe.c:5175
```c
#if 0  /* DISABLED - Legacy stack-based OP_AggFunc (uses removed p->zArgv) */
```
**Purpose**: Old OP_AggFunc implementation
**Reason**: Uses removed `p->zArgv` field
**Action**: REMOVE - Dead code

---

#### Block 12: src/vdbe.c:5805
```c
#if 0  /* DISABLED - Legacy Set opcodes (p->aSet removed) */
```
**Purpose**: OP_SetInsert, OP_SetFound, OP_SetNotFound, OP_SetFirst, OP_SetNext
**Reason**: Legacy set mechanism; uses removed `p->aSet` field
**Action**: REMOVE - Dead code, replaced by RowSet or other mechanisms

---

#### Block 13-16: src/vdbe.c:5807, 5845, 5872, 5909
```c
#if 0  /* DISABLED - aSet removed in modernization */
```
**Purpose**: Individual Set opcodes (SetInsert, SetFound, SetNotFound, SetClear)
**Reason**: Duplicate blocks, same as above
**Action**: REMOVE - Dead code

---

#### Block 17: src/vdbe.c:5992
```c
#if 0
	    fprintf(stdout, "%10lld ", elapse);
	    __vdbe_print_op(stdout, origPc, &p->opcodes[origPc]);
#endif
```
**Purpose**: Debug output for VDBE_PROFILE profiling
**Line context**: Inside VDBE_PROFILE block (line 5987-5997)
**Reason**: Optional verbose profiling output, disabled by default
**Action**: KEEP - This is intentional debug output that can be enabled if needed

---

### Category 2: Intentional Debugging Notes - KEEP WITH DOCUMENTATION

#### Block 18: src/cg_where.c:1428
```c
#if 0  /* NOTE: Never reuse a cursor */
```
**Purpose**: Comment explaining cursor reuse strategy
**Reason**: Design decision documentation
**Action**: KEEP - This is a design note, not disabled code

---

#### Block 19: src/cg_expr.c:110
```c
#if 0  /* TODO Leak pList when malloc fails */
```
**Purpose**: Known memory leak note
**Reason**: TODO marker for error handling improvement
**Action**: KEEP - Documents known issue that needs fixing

---

### Category 3: Tool Code - REMOVE (lemon.c)

#### Block 20: src/lemon/lemon.c:1930
```c
#if 0
  printf("%s:%d: Token=[%s] state=%d\n",psp->filename,psp->tokenlineno,
    x,psp->state);
#endif
```
**Purpose**: Debug trace output for Lemon parser generator token processing
**Reason**: Build tool debug output, disabled by default
**Action**: KEEP - Tool works correctly; this is intentional debug trace

---

### Category 4: Backup Files - IGNORE

Multiple `.bak`, `.pre-fix`, `.backup`, `.phase2.bak` files contain #if 0 blocks.
**Action**: IGNORE - These are backup files, not active code

---

## Summary of Actions

| Action | Count | Files |
|--------|-------|-------|
| REMOVE dead code | 15 blocks | src/vdbe.c (all legacy opcodes) |
| KEEP documented | 4 blocks | src/cg_where.c, src/cg_expr.c, src/vdbe.c:5992, src/lemon/lemon.c |
| FIX dbsql_config.h | 73 files | All source files - FIXED |
| IGNORE (backups) | ~30 blocks | *.bak, *.pre-fix, etc. |

---

## Priority Order

### Priority 1: Fix dbsql_config.h (CRITICAL) - ✅ COMPLETED
This blocks all compilation. Must be fixed first.

**Solution Applied**: Created minimal stub header at `src/inc/dbsql_config.h`

The stub satisfies all 73 #include directives with an empty but documented header.
Configuration is handled by the Nix build system, so no defines are needed.

### Priority 2: Remove VDBE Dead Code - ✅ COMPLETED
Removed 15 disabled opcode blocks from src/vdbe.c that referenced removed data structures.

**Blocks removed:**
1. __agg_insert and __agg_in_focus functions (agg_t/agg_elem_t removed)
2. __sorted_merge function (sorter_t removed)
3. OP_ColumnName, OP_Callback opcodes (azColName/xCallback removed)
4. OP_NullCallback opcode (kept fallback handler)
5. Legacy stack-based OP_Function
6. OP_ListWrite, OP_ListRewind, OP_ListRead opcodes (kept fallback handlers)
7. OP_SortPut, OP_SortMakeRec, OP_SortMakeKey, OP_Sort, OP_SortNext, OP_SortCallback, OP_SortReset
8. OP_SortCallback continuation (azResColumn removed)
9. OP_FileOpen, OP_FileClose, OP_FileRead, OP_FileColumn, OP_MemStore, OP_MemLoad
10. OP_AggReset, OP_AggFocus, OP_AggIncr, OP_AggNext, OP_AggSet, OP_AggGet
11. Legacy stack-based OP_AggFunc
12-15. OP_SetInsert, OP_SetFound, OP_SetNotFound, OP_SetClear (multiple blocks)

**Characters removed:** 23,367 bytes of dead code
**Only remaining #if 0:** Intentional debug block at line 4852 (VDBE_PROFILE output)

### Priority 3: Investigate Ambiguous Blocks
- src/vdbe.c:5992 - Check what's disabled
- src/lemon/lemon.c:1930 - Low priority tool code

### Priority 4: Document Kept Blocks
Improve comments on cg_where.c:1428 and cg_expr.c:110 if needed.

---

## Next Steps

1. Create dbsql_config.h stub header
2. Verify compilation improves
3. Remove dead VDBE opcode blocks
4. Test build after removals
5. Commit changes

---

## Build System Notes

- Current build: Nix-based (flake.nix)
- No autoconf/configure found
- No config.h.in template
- dbsql_config.h appears to be vestigial from old build system
