# DBSQL Post-Modernization Plan - Implementation Status

**Date**: 2025-03-17
**Plan Version**: 1.0
**Status**: IN PROGRESS

## Overview

This document tracks implementation progress for the comprehensive post-modernization plan covering:

1. **Naming Convention Violations** - Fix SQLite conventions to match DBSQL/BerkeleyDB standards
2. **Missing SQLite 3.52.0 Features** - Identify and prioritize unimplemented features
3. **Dist Tools Review** - Update transpilation tools for SQLite 3.52.0
4. **BerkeleyDB Integration** - Add git submodule for upstream BerkeleyDB

---

## Current Status Summary

| Section | Status | Progress | Priority |
|---------|--------|----------|----------|
| **1. Naming Conventions** | 🔄 BLOCKED | 0% | HIGH |
| **2. Feature Gaps** | ✅ DOCUMENTED | 100% | MEDIUM |
| **3. Dist Tools** | ⏸️ PENDING | 0% | MEDIUM |
| **4. BerkeleyDB Submodule** | ⚠️ BLOCKED | 5% | MEDIUM |

### Blocking Issue

**All sections blocked by incomplete code generator modernization**. The current build fails with numerous errors in code generators (`cg_*.c`), preventing systematic implementation of the plan.

**Required Action**: Complete ongoing modernization work (Tasks 21-28 from BUILD_STATE_SUMMARY.txt) before proceeding with this plan.

---

## Section 1: Naming Convention Violations

**Status**: 🔄 BLOCKED (requires working build)
**Progress**: 0/4 phases complete

### Issues Identified

- **100+ violations** of DBSQL/BerkeleyDB snake_case conventions
- SQLite naming (`pXxx` pointers, `aXxx` arrays, PascalCase types) adopted during modernization
- Conflicts with established DBSQL standards (`snake_case` functions, `_t` suffix types)

### Planned Phases

| Phase | Description | Effort | Status |
|-------|-------------|--------|--------|
| 1 | Type alias cleanup (`Mem` → `mem_t` only) | 10-15h | ⏸️ PENDING |
| 2 | Struct field renames (`pXxx` → `snake_case`) | 30-40h | ⏸️ PENDING |
| 3 | Local variable renames (code clarity) | 10-15h | ⏸️ PENDING |
| 4 | Function parameter expansion | 5-10h | ⏸️ PENDING |

**Total Estimated Effort**: 55-80 hours

### Files Requiring Changes

**Critical (Must Fix)**:
- `src/inc/vdbe_int.h` - Type definitions (100+ violations)
- `src/vdbe.c` - Local variables (200+ violations)
- `src/vdbe_method.c` - Field accesses (30+ violations)

**High Priority**:
- `src/api.c` - Public API (20+ violations)
- `src/inc/dbsql_ext.h` - Public header (15+ violations)

**Medium Priority**:
- `src/cg_*.c` (6 files) - Code generators (50+ violations)
- `src/sm.c` - Parameter naming (10+ violations)

### Next Actions

1. ⏸️ Wait for build to stabilize
2. ⏸️ Implement Phase 1 (type aliases) - Low risk, high visibility
3. ⏸️ Run automated sed/awk scripts for Phase 2 (struct fields)
4. ⏸️ Test compilation after each file change

---

## Section 2: Missing SQLite 3.52.0 Features

**Status**: ✅ DOCUMENTED
**Progress**: Feature gap analysis complete

### Documentation Created

✅ **FEATURE_STATUS.md** - Comprehensive feature gap analysis with:
- Feature coverage matrix (68% SQL, 93% functions, 37% opcodes)
- 12 prioritized missing features with implementation estimates
- 3-phase roadmap (1,400-2,000 hours total)
- VDBE opcode gap analysis (120+ missing opcodes)

### Critical Missing Features

| Feature | Priority | Effort | ROI | Status |
|---------|----------|--------|-----|--------|
| RETURNING clause | CRITICAL | 30-40h | ⭐⭐⭐⭐⭐ | ⏸️ PENDING |
| UPSERT support | CRITICAL | 80-120h | ⭐⭐⭐⭐ | ⏸️ PENDING |
| Window Functions | HIGH | 200-300h | ⭐⭐⭐⭐ | ⏸️ PENDING |
| Recursive CTEs | HIGH | 150-200h | ⭐⭐⭐ | ⏸️ PENDING |
| ALTER TABLE ADD COLUMN | HIGH | 40-60h | ⭐⭐⭐⭐ | ⏸️ PENDING |

### Roadmap

**Phase 1: Critical SQL Compliance** (300-350 hours)
- Target: Baseline SQL:2011 support for modern applications
- Features: RETURNING, UPSERT, ALTER TABLE, Savepoints, PRAGMA integrity_check

**Phase 2: Modern SQL Features** (500-650 hours)
- Target: Analytical queries and semi-structured data
- Features: Window Functions, Recursive CTEs, Generated Columns, JSON1

**Phase 3: Extensions & Optimization** (600-1000 hours)
- Target: Full SQLite 3.52.0 parity
- Features: Virtual Tables, FTS5, R-Tree, additional opcodes

### Next Actions

1. ⏸️ Wait for build to stabilize
2. ⏸️ Implement RETURNING clause first (highest ROI: 30-40h effort)
3. ⏸️ Add tests for each new feature
4. ⏸️ Update FEATURE_STATUS.md as features are implemented

---

## Section 3: Dist Tools Review & Updates

**Status**: ⏸️ PENDING
**Progress**: 0/3 tools updated

### Tools Requiring Updates

| Tool | Status | Update Needed | Effort |
|------|--------|---------------|--------|
| `sqlite_to_dbsql.py` | ⏸️ PENDING | Add 50+ SQLite 3.52.0 field mappings | 8-12h |
| `verify_transpilation.py` | ⏸️ PENDING | Add VDBE validation rules | 8-12h |
| `name_mapping.json` | ⏸️ PENDING | Document 30+ new structure fields | 4-6h |

**Total Estimated Effort**: 20-30 hours

### Updates Required

**sqlite_to_dbsql.py**:
- Add VdbeOp structure mappings (p5, p4type fields)
- Add Mem structure mappings (szMalloc, uTemp, eSubtype fields)
- Add Parser structure mappings (nRangeReg, iRangeReg, nLabelAlloc fields)
- Add 50+ new opcode references

**verify_transpilation.py**:
- Add VDBE opcode validation rules
- Add Mem structure initialization checks
- Add register addressing validation

**name_mapping.json**:
- Document VdbeOp fields from SQLite 3.52.0
- Document Mem fields from SQLite 3.52.0
- Document Parser fields from SQLite 3.52.0

### Next Actions

1. ⏸️ Wait for build to stabilize
2. ⏸️ Update `sqlite_to_dbsql.py` with new mappings
3. ⏸️ Update `verify_transpilation.py` with VDBE rules
4. ⏸️ Test transpilation tools on SQLite 3.52.0 samples

---

## Section 4: BerkeleyDB Git Submodule Integration

**Status**: ⚠️ BLOCKED (BDB 4.8.30 not available on GitHub)
**Progress**: 5% (investigation complete)

### Investigation Findings

❌ **GitHub berkeleydb/libdb.git only has tags for v5.3.x and v1.85**
- BDB 4.8.30 was released in 2010, predates GitHub migration
- Oracle maintains official BDB downloads but not in git format
- Current dependency: BDB 4.8.30 from Nix store

### Alternative Approaches

| Approach | Pros | Cons | Recommendation |
|----------|------|------|----------------|
| **Vendor BDB 4.8.30 directly** | Source control, reproducible | Large repo size, no upstream tracking | ⚠️ Possible |
| **Upgrade to BDB 18.1.40** | Modern, maintained | API changes, SM layer testing required | ⚠️ Future work |
| **Keep Nix/system dependency** | No changes, works today | Less portable | ✅ **Recommended (short-term)** |
| **Use Debian source package** | Reliable, git-importable | Extra conversion step | ⚠️ Possible |

### Recommended Short-Term Action

**Document current external dependency clearly**, defer submodule/vendoring until build is stable.

Create `DEPENDENCIES.md`:
- Document BDB 4.8.30 requirement
- Explain Nix store dependency
- Note Sleepycat License implications
- Provide build instructions for non-Nix systems

**Estimated Effort**: 2-3 hours (when unblocked)

### Next Actions

1. ✅ Document BDB unavailability on GitHub
2. ⏸️ Create DEPENDENCIES.md documenting current external dependency
3. ⏸️ Re-evaluate submodule approach after build stabilizes
4. ⏸️ Consider vendoring BDB 4.8.30 source in future release

---

## Overall Plan Status

### Completed Work

✅ **FEATURE_STATUS.md created** - Comprehensive SQLite 3.52.0 feature gap analysis
✅ **BerkeleyDB investigation** - Identified GitHub availability issue

### In Progress

🔄 **Code generator modernization** - Blocking all other work

### Blocked/Pending

⏸️ **Naming convention fixes** - Requires working build
⏸️ **Dist tools updates** - Requires working build
⏸️ **BerkeleyDB submodule** - Requires alternative approach

### Critical Path Forward

1. **IMMEDIATE**: Complete code generator modernization (Tasks 21-28)
   - Fix structure member access errors
   - Implement missing opcodes (e.g., OP_Recno)
   - Resolve type mismatches
   - **Estimated effort**: 60-80 hours

2. **SHORT-TERM** (after build stabilizes):
   - Create DEPENDENCIES.md documenting BDB dependency
   - Implement naming convention Phase 1 (type aliases)
   - Update dist transpilation tool docs
   - **Estimated effort**: 40-50 hours

3. **MEDIUM-TERM** (after naming fixes):
   - Implement RETURNING clause (highest ROI feature)
   - Implement naming convention Phase 2 (struct fields)
   - **Estimated effort**: 60-80 hours

4. **LONG-TERM** (feature additions):
   - Implement remaining critical features (UPSERT, Window Functions, etc.)
   - **Estimated effort**: 1,200-1,800 hours

---

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| **Build instability blocks all work** | 🔴 HIGH | Prioritize code generator completion |
| **Naming fixes break existing code** | 🟡 MEDIUM | Systematic testing after each file change |
| **BDB submodule unavailable** | 🟡 MEDIUM | Document external dependency clearly |
| **Feature scope creep** | 🟢 LOW | Follow phased roadmap strictly |

---

## Metrics

### Implementation Progress

- **Completed Tasks**: 1/7 (14%)
- **Documentation**: 2/3 documents created (67%)
- **Build Status**: ❌ FAILING (blocking progress)
- **Estimated Total Effort Remaining**: 1,500-2,000 hours

### Phase Distribution

| Phase | Tasks | Effort | Status |
|-------|-------|--------|--------|
| **Immediate** | Code generator fixes | 60-80h | 🔄 IN PROGRESS |
| **Short-term** | Docs, naming Phase 1 | 40-50h | ⏸️ PENDING |
| **Medium-term** | RETURNING, naming Phase 2 | 60-80h | ⏸️ PENDING |
| **Long-term** | Feature additions | 1,200-1,800h | ⏸️ PENDING |

---

## Document Maintenance

**Created**: 2025-03-17
**Last Updated**: 2025-03-17
**Next Review**: After code generator modernization completes

**Change Log**:
- 2025-03-17: Initial status document created
- 2025-03-17: FEATURE_STATUS.md completed
- 2025-03-17: BerkeleyDB submodule investigation completed (blocked)

---

## References

- **Original Plan**: (provided as user input to this session)
- **BUILD_STATE_SUMMARY.txt**: Current build status and error analysis
- **FEATURE_STATUS.md**: SQLite 3.52.0 feature gap analysis
- **Task Tracking**: Claude Code task list (#1-7)
