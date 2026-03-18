# Phase 2 Naming Fixes - Partial Progress Report

**Date**: 2026-03-18
**Status**: PARTIALLY COMPLETE - Header files done, implementation files need manual review
**Approach**: Automated sed scripts + manual fixes required

## Completed Work

### 1. Header File Updates (COMPLETE)

Successfully renamed all struct fields in header files:

#### vdbe_t Structure (`src/inc/vdbe_int.h`)
- ✅ `pPrev`, `pNext` → `prev_vdbe`, `next_vdbe`
- ✅ `pParse` → `parser`
- ✅ `aMem` → `registers`
- ✅ `apArg` → `arg_ptrs`
- ✅ `apCsr` → `cursors`
- ✅ `aVar` → `variables`
- ✅ `aOp` → `opcodes`
- ✅ `aColName` → `column_names`
- ✅ `pResultRow` → `result_row`
- ✅ `aLabel` → `labels`
- ✅ `pFrame` → `parent_frame`
- ✅ `pDelFrame` → `frame_free_list`
- ✅ `pProgram` → `sub_programs`
- ✅ `pAuxData` → `aux_data`
- ✅ `pFree` → `free_context`

#### vdbe_frame_t Structure (`src/inc/vdbe_int.h`)
- ✅ `pParent` → `parent_frame`
- ✅ `aOp` → `opcodes`
- ✅ `aMem` → `registers`
- ✅ `apCsr` → `cursors`
- ✅ `pAuxData` → `aux_data`

#### sub_program_t Structure (`src/inc/vdbe.h`)
- ✅ `aOp` → `opcodes`
- ✅ `pNext` → `next_program`

#### db_fixer Structure (`src/inc/dbsql_int.in`)
- ✅ `pParse` → `parser`

#### Type Fix (`src/inc/vdbe.h`)
- ✅ `Mem *pMem` → `mem_t *pMem` (in vdbe_op_t.p4 union)

### 2. Implementation File Updates (PARTIAL)

Files successfully processed by sed script:
- ✅ `src/cg_attach.c` - Compiles successfully
- ⚠️ `src/cg_insert.c` - Has `next_vdbe` errors (index_t pNext incorrectly renamed)
- ⚠️ Other `src/cg_*.c` - Similar issues likely
- ⚠️ `src/vdbe.c`, `src/vdbe_method.c` - Need verification

### 3. Config Files
- ✅ Regenerated `build_unix/dbsql_int.h` from `src/inc/dbsql_int.in`

## Issues Identified

### Context-Sensitivity Problem

The sed-based approach cannot distinguish between:
- `vdbe_t->pNext` (should be `next_vdbe`) ✅
- `index_t->pNext` (should stay `pNext` or become `next_index`) ❌
- `func_def_t->pNext` (should stay `pNext` or become `next_func`) ❌
- `trigger_step_t->pNext` (should stay `pNext` or become `next_step`) ❌

The sed script pattern `s/->pNext\b/->next_vdbe/g` was too greedy and renamed ALL `->pNext` references, regardless of context.

### Files Requiring Manual Fix

Files with incorrect `next_vdbe` renames detected:
- `src/cg_insert.c` - 6 occurrences (index_t loops)
- `src/cg_update.c` - 8 occurrences (index_t loops)
- `src/api.c` - 2 occurrences (func_def_t loops)
- `src/vdbe.c` - 8+ occurrences (various structures)

**Estimated**: 20-30 occurrences across 15+ files

## Next Steps

### Option A: Complete Manual Fixes (Recommended)

1. **Identify all incorrect renames**:
   ```bash
   rg 'next_vdbe' src/ -t c | grep -v 'vdbe *prev_vdbe, \*next_vdbe'
   ```

2. **Fix each file manually**:
   - For `index_t`: `s/->next_vdbe/->pNext/g`
   - For `func_def_t`: `s/->next_vdbe/->pNext/g`
   - For other structures: context-dependent

3. **Test compilation after each file**

4. **Estimated effort**: 2-4 hours

### Option B: Revert and Use AST-Based Tool (Future)

1. Revert all `.c` file changes:
   ```bash
   git checkout -- src/*.c src/cg_*.c
   ```

2. Use `ast-grep` or similar AST-aware tool to:
   - Parse C code into AST
   - Identify field accesses by structure type
   - Rename only vdbe_t/vdbe_frame_t fields
   - Leave other structures untouched

3. **Estimated effort**: 6-8 hours (including tool setup)

### Option C: Incremental Manual Approach (Safest)

1. Keep header files as-is (already correct)
2. Fix implementation files one at a time
3. Compile after each file
4. Document patterns for future reference
5. **Estimated effort**: 4-6 hours

## Recommendations

**Short-term** (Next session):
- Adopt Option A (manual fixes)
- Use targeted search-and-replace per file
- Test compilation frequently
- This phase is 70% complete, finish the remaining 30%

**Long-term** (Future phases):
- For Phase 2.2+ (parser_t, table_t), use Option B (AST tools)
- Safer and more maintainable for complex renames
- Worth the upfront investment

## Files Status

| File | Status | Notes |
|------|--------|-------|
| `src/inc/vdbe_int.h` | ✅ COMPLETE | All vdbe_t fields renamed |
| `src/inc/vdbe.h` | ✅ COMPLETE | sub_program_t fields renamed |
| `src/inc/dbsql_int.in` | ✅ COMPLETE | db_fixer renamed |
| `src/cg_attach.c` | ✅ VERIFIED | Compiles successfully |
| `src/cg_insert.c` | ⚠️ NEEDS FIX | 6 index_t->next_vdbe errors |
| `src/cg_update.c` | ⚠️ NEEDS FIX | 8 index_t->next_vdbe errors |
| `src/api.c` | ⚠️ NEEDS FIX | 2 func_def_t->next_vdbe errors |
| `src/vdbe.c` | ⚠️ NEEDS REVIEW | Unknown errors |
| `src/vdbe_method.c` | ⚠️ NEEDS REVIEW | Unknown errors |
| Other `cg_*.c` | ⚠️ NEEDS REVIEW | Likely similar issues |

## Lessons Learned

1. **Text-based sed insufficient** for C struct field renames
2. **Context matters**: `->pNext` means different things on different structures
3. **AST-aware tools required** for safe, scalable refactoring
4. **Incremental verification critical**: Compile after each major change
5. **Manual review unavoidable**: Even with good tools, edge cases exist

## Time Investment

| Activity | Time Spent |
|----------|------------|
| Planning & analysis | 1 hour |
| Header file updates | 1 hour |
| Sed script development | 1 hour |
| Implementation file updates | 1 hour |
| Debugging & fixes | 2 hours |
| **Total so far** | **6 hours** |

| Activity | Time Remaining |
|----------|----------------|
| Fix index_t->next_vdbe errors | 1-2 hours |
| Fix func_def_t errors | 0.5 hours |
| Fix other structure errors | 1-2 hours |
| Verification & testing | 0.5 hours |
| **Estimated to complete** | **3-5 hours** |

**Total Phase 2.1 estimate**: 9-11 hours (within original 10-12 hour estimate)

## Success Criteria (Updated)

- [x] All vdbe_t fields renamed in headers
- [x] All vdbe_frame_t fields renamed in headers
- [x] sub_program_t fields renamed in headers
- [x] db_fixer fields renamed in headers
- [x] Config files regenerated
- [x] At least one file (cg_attach.c) compiles successfully
- [ ] All index_t->next_vdbe errors fixed
- [ ] All func_def_t->next_vdbe errors fixed
- [ ] Full codebase compiles without errors
- [ ] All tests pass (if test suite exists)

**Current Progress**: 60% complete (6/10 criteria met)

---

## References

- **PHASE_2_NAMING_FIXES.md**: Original plan
- **STYLE_GUIDE.md**: Naming conventions reference
- **PHASE_1_NAMING_FIX_SUMMARY.md**: Phase 1 completion
