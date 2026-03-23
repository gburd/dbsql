# Phase 2: Struct Field Naming Convention Fixes

**Date Started**: 2026-03-18
**Task**: Rename struct fields from SQLite conventions (pXxx, aXxx) to DBSQL snake_case conventions
**Status**: IN PROGRESS

## Overview

Phase 2 systematically renames struct fields to follow DBSQL snake_case conventions, eliminating the SQLite-style Hungarian notation (pXxx for pointers, aXxx for arrays).

## Naming Patterns

### Pointer Fields (pXxx → descriptive_name)

**Pattern**: Remove `p` prefix, use descriptive snake_case names

```c
/* BEFORE */
pVdbe     → vdbe
pParse    → parser
pNext     → next_vdbe / next_parser / next_table
pPrev     → prev_vdbe / prev_parser
pFrame    → parent_frame / current_frame
pIndex    → first_index / index_list
pTrigger  → first_trigger / trigger_list
pFree     → free_context
```

### Array Fields (aXxx → descriptive_array or plural)

**Pattern**: Remove `a` prefix, use plural or _array suffix

```c
/* BEFORE */
aMem      → registers / mem_registers
aOp       → opcodes / op_array
apCsr     → cursors / cursor_ptrs
aVar      → variables / var_array
aColName  → column_names
aLabel    → labels / label_array
aCol      → columns
```

### Context-Specific Naming

Choose names that clarify the field's role:
- List heads: `first_*` or `*_list`
- Navigation: `next_*`, `prev_*`
- Arrays: plural form or `*_array`
- Temporary: `temp_*`
- Cache: `cached_*`

## Implementation Plan

### Priority 1: vdbe_t Structure (High Impact)

**File**: `src/inc/vdbe_int.h` (struct vdbe, lines 452-529)

**Fields to Rename**:

| Current | New Name | Rationale |
|---------|----------|-----------|
| `pPrev` | `prev_vdbe` | Linked list navigation |
| `pNext` | `next_vdbe` | Linked list navigation |
| `pParse` | `parser` | Single pointer to parser |
| `aMem` | `registers` | Register array (core VDBE concept) |
| `apArg` | `arg_ptrs` | Array of argument pointers |
| `apCsr` | `cursors` | Array of cursor pointers |
| `aVar` | `variables` | Variable binding array |
| `aOp` | `opcodes` | Opcode instruction array |
| `aColName` | `column_names` | Column name array |
| `pResultRow` | `result_row` | Current result row pointer |
| `aLabel` | `labels` | Label address array |
| `pFrame` | `parent_frame` | Parent frame in call stack |
| `pDelFrame` | `frame_free_list` | Frame deletion list |
| `pProgram` | `sub_programs` | Sub-program linked list |
| `pAuxData` | `aux_data` | Auxiliary data linked list |

**Affected Files** (estimated 150+ references):
- `src/vdbe.c` - Core VDBE execution (100+ references)
- `src/vdbe_method.c` - VDBE API methods (30+ references)
- `src/api.c` - Public API functions (20+ references)
- `src/cg_*.c` - Code generators (30+ references)

**Estimated Effort**: 10-12 hours

### Priority 2: parser_t Structure (High Impact)

**File**: `src/inc/dbsql_int.in` (struct parser, lines 1793+)

**Fields to Rename**:

| Current | New Name | Rationale |
|---------|----------|-----------|
| `pVdbe` | `vdbe` | Single pointer to VDBE |
| `aLabel` | `labels` | Label array |

**Affected Files** (estimated 80+ references):
- `src/cg_*.c` - All code generators (60+ references)
- `src/sql_parser.y` - Parser grammar file (20+ references)

**Estimated Effort**: 8-10 hours

### Priority 3: table_t Structure (High Impact)

**File**: `src/inc/dbsql_int.in` (struct table, lines 824-876)

**Fields to Rename**:

| Current | New Name | Rationale |
|---------|----------|-----------|
| `aCol` | `columns` | Column array |
| `pIndex` | `first_index` | Index list head |
| `pTrigger` | `first_trigger` | Trigger list head |
| `pCheck` | `check_constraints` | CHECK constraint list |
| `pSchema` | `schema` | Schema pointer |
| `pFKey` | `first_foreign_key` | Foreign key list head (also in u.tab.pFKey) |
| `pSelect` | `view_select` | View SELECT (also in u.view.pSelect) |

**Affected Files** (estimated 100+ references):
- `src/cg_*.c` - Code generators (40+ references)
- `src/sm.c` - Storage manager (30+ references)
- `src/table.c` - Table operations (30+ references)

**Estimated Effort**: 10-12 hours

### Priority 4: VdbeCursor Structure (Medium Impact)

**File**: `src/inc/vdbe_int.h` (struct VdbeCursor, lines 103-165)

**Fields to Rename**:

| Current | New Name | Rationale |
|---------|----------|-----------|
| `pAltCursor` | `alt_cursor` | Associated index cursor |
| `pCursor` | `sm_cursor` | Storage manager cursor (in uc union) |
| `pSorter` | `sorter` | Sorter object (in uc union) |
| `pKeyInfo` | `key_info` | Key information |
| `pCache` | `cache` | TEXT/BLOB cache |
| `aOffset` | `offsets` | Offset array |
| `aRow` | `row_data` | Current row data |
| `aType` | `types` | Type array |

**Note**: This structure also needs type renaming `VdbeCursor` → `vdbe_cursor_t`, but that's deferred until field renames are complete to reduce merge conflicts.

**Affected Files** (estimated 80+ references):
- `src/vdbe.c` - VDBE execution (60+ references)
- `src/vdbe_method.c` - VDBE methods (20+ references)

**Estimated Effort**: 8-10 hours

### Priority 5: Other Structures (Lower Impact)

**vdbe_frame_t** (`src/inc/vdbe_int.h`, line 341):
- `pParent` → `parent_frame`
- `aMem` → `registers`

**Estimated effort**: 2-3 hours

## Implementation Strategy

### Phase 2.1: vdbe_t Structure (Priority 1)

1. **Preparation**:
   - Search all uses: `rg '\b(pPrev|pNext|pParse|aMem|apArg|apCsr|aVar|aOp|aColName|pResultRow|aLabel|pFrame|pDelFrame|pProgram|pAuxData)\b' src/ -t c`
   - Create sed script for mechanical replacements
   - Identify complex cases requiring manual review

2. **Execution**:
   - Update struct definition in `vdbe_int.h`
   - Run sed script on implementation files
   - Manual review of context-sensitive cases
   - Compile after each major file
   - Commit when compilation succeeds

3. **Verification**:
   - Full compilation test
   - Verify no remaining old field names
   - Check for accessor macro conflicts

### Phase 2.2: parser_t Structure (Priority 2)

Same process as 2.1, focusing on code generator files.

### Phase 2.3: table_t Structure (Priority 3)

Same process, with special attention to union field aliases.

### Phase 2.4: VdbeCursor Structure (Priority 4)

Same process, preparing for future type rename.

### Phase 2.5: Cleanup (Priority 5)

Handle remaining structures and edge cases.

## Sed Scripts

### Script 1: vdbe_t field renames

```bash
#!/bin/bash
# phase2_vdbe_fields.sh - Rename vdbe_t struct fields

FILES="src/vdbe.c src/vdbe_method.c src/api.c src/cg_*.c"

for f in $FILES; do
    sed -i.bak '
        # Linked list navigation
        s/\bpPrev\b/prev_vdbe/g
        s/\bpNext\b/next_vdbe/g

        # Single pointers
        s/\bpParse\b/parser/g
        s/\bpResultRow\b/result_row/g
        s/\bpFrame\b/parent_frame/g
        s/\bpDelFrame\b/frame_free_list/g
        s/\bpProgram\b/sub_programs/g
        s/\bpAuxData\b/aux_data/g

        # Arrays
        s/\baMem\b/registers/g
        s/\bapArg\b/arg_ptrs/g
        s/\bapCsr\b/cursors/g
        s/\baVar\b/variables/g
        s/\baOp\b/opcodes/g
        s/\baColName\b/column_names/g
        s/\baLabel\b/labels/g
    ' "$f"
done
```

**Note**: This is a starting template. Actual implementation will use more sophisticated patterns to avoid false matches in comments and string literals.

### Script 2: parser_t field renames

```bash
#!/bin/bash
# phase2_parser_fields.sh - Rename parser_t struct fields

FILES="src/cg_*.c src/sql_parser.y"

for f in $FILES; do
    sed -i.bak '
        s/\bpVdbe\b/vdbe/g
        s/\baLabel\b/labels/g
    ' "$f"
done
```

### Script 3: table_t field renames

```bash
#!/bin/bash
# phase2_table_fields.sh - Rename table_t struct fields

FILES="src/cg_*.c src/sm.c src/table.c"

for f in $FILES; do
    sed -i.bak '
        s/\baCol\b/columns/g
        s/\bpIndex\b/first_index/g
        s/\bpTrigger\b/first_trigger/g
        s/\bpCheck\b/check_constraints/g
        s/\bpSchema\b/schema/g
        # Note: pFKey and pSelect need context-aware handling due to union aliases
    ' "$f"
done
```

## Risk Mitigation

### Low-Risk Approach

1. **Test compilation frequently** - After each structure, compile the full codebase
2. **Small commits** - One structure per commit
3. **Backup branches** - Keep working backup before each phase
4. **Incremental verification** - Run `make` after each sed script execution

### Collision Detection

Before running sed scripts, check for naming conflicts:

```bash
# Check if new names already exist in the codebase
rg '\bregisters\b' src/ -t c  # Should only find comments, not struct fields
rg '\bopcodes\b' src/ -t c    # Check for existing uses
rg '\bcursors\b' src/ -t c    # etc.
```

If conflicts exist, choose alternative names (e.g., `mem_registers`, `op_array`).

### Manual Review Required

Some patterns need manual verification:
- Macro arguments that shadow field names
- Function parameters with same names
- Comments and string literals (should generally be left unchanged)
- Complex pointer chains like `p->pVdbe->pParse`

## Testing Strategy

### Compilation Testing

After each priority level:
```bash
cd /home/gburd/ws/dbsql/build_unix
make clean
make 2>&1 | tee build.log
# Expect: 0 errors
```

### Regression Testing

If test suite exists:
```bash
make test 2>&1 | tee test.log
```

### Manual Verification

After all renames:
```bash
# Verify no old field names remain
rg '\b(pPrev|pNext|pParse|aMem|apArg|apCsr|aVar|aOp|aColName)\b' src/ -t c \
   | grep -v '\.bak:' \
   | grep -v 'PHASE_2_NAMING_FIXES.md'
# Expect: 0 results (or only comments/strings)
```

## Rollback Plan

If critical issues emerge:
```bash
# Restore from backup
git checkout HEAD -- src/inc/vdbe_int.h src/vdbe.c src/vdbe_method.c

# Or revert specific commit
git revert <commit-hash>
```

## Estimated Timeline

| Phase | Effort | Calendar Time |
|-------|--------|---------------|
| 2.1 vdbe_t | 10-12h | 2 days |
| 2.2 parser_t | 8-10h | 1.5 days |
| 2.3 table_t | 10-12h | 2 days |
| 2.4 VdbeCursor | 8-10h | 1.5 days |
| 2.5 Cleanup | 2-3h | 0.5 days |
| **Total** | **38-47h** | **7-8 days** |

With testing and documentation: **40-50 hours** (8-10 days calendar time)

## Success Criteria

- [ ] All struct fields follow snake_case conventions
- [ ] No pXxx or aXxx field names remain (except in backups/docs)
- [ ] Full codebase compiles without errors
- [ ] All tests pass (if test suite exists)
- [ ] Code is more readable and maintainable
- [ ] PHASE_2_NAMING_FIXES.md documents all changes

## Document Maintenance

**Created**: 2026-03-18
**Last Updated**: 2026-03-18
**Next Review**: After each priority level completes

---

## Progress Log

### 2026-03-18: Phase 2 Planning Complete

- Analyzed vdbe_t, parser_t, table_t, VdbeCursor structures
- Identified 15+ fields in vdbe_t requiring rename
- Created implementation plan with 5 priority levels
- Estimated 40-50 hours total effort
- Ready to begin Phase 2.1 (vdbe_t structure)

**Status**: Planning complete, ready to execute Phase 2.1

---

## References

- **STYLE_GUIDE.md**: DBSQL naming conventions reference
- **PHASE_1_NAMING_FIX_SUMMARY.md**: Phase 1 completion documentation
- **POST_MODERNIZATION_PLAN_STATUS.md**: Overall modernization plan
- **SQLite 3.52.0**: vdbeInt.h and sqliteInt.h for field mapping
