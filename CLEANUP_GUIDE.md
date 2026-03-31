# Repository Cleanup Guide

## Overview

The repository has accumulated agent documentation files and build artifacts that should not be in version control. This guide explains what to remove and what to keep.

## Quick Cleanup

Run the provided script:

```bash
cd /home/gburd/ws/dbsql
chmod +x cleanup_repo.sh
./cleanup_repo.sh
```

This will:
1. Remove agent documentation from Git tracking
2. Remove build artifacts from Git tracking
3. Remove test database files from Git tracking
4. Update .gitignore with comprehensive patterns
5. Keep important documentation

## What to Remove

### Agent Documentation (Historical Cruft)

**Entire agent/ directory** - Remove all files in this directory:
- agent/AGENT*.md (all agent-related files)
- agent/FINAL*.md (final reports)
- agent/*SUMMARY*.md (summary files)
- agent/*FIX*.md (fix documentation)
- agent/*STATUS*.md (status reports)

**Root directory agent cruft** - Remove these patterns:
- `*FINAL*.md` (except FINAL_INTEGRATION_REPORT.md)
- `*FIX*.md` (Berkeley DB, cursor, memory, order by fixes, etc.)
- `*SUMMARY*.md` (except PRAGMA_COMMANDS_SUMMARY.md, EXECUTION_SUMMARY.md, COMPREHENSIVE_WORK_STATUS.md)
- `*STATUS*.md` (except INTEGRATION_STATUS.md)
- `*REPORT*.md` (except FINAL_INTEGRATION_REPORT.md)
- `*ANALYSIS*.md`
- `*COMPLETE*.md`
- `*PROGRESS*.md`
- `*PLAN*.md` (except INTEGRATION_PLAN.md)

**Specific files to remove:**
- BERKELEY_DB_FIX_FINAL.md
- BERKELEY_DB_FIX_ANALYSIS.md
- CURSOR_TRANSACTION_FIX.md
- DATABASE_OPENING_FIX.md
- MEMORY_FIXES_COMPLETE.md
- ORDER_BY_FIX.md
- PERSISTENCE_FIX.md
- STR_FIX_SUMMARY.md
- UNUSED_WARNINGS_FIXED.md
- CLEANUP_SUMMARY.md
- CLEANUP_SUMMARY_FINAL.md
- DEBUG_CLEANUP_SUMMARY.md
- RECOVERY_ANALYSIS_SUMMARY.md
- EXECUTIVE_SUMMARY.md
- FINAL_STATUS_REPORT.md
- FINAL_STATUS_SUMMARY.md
- FINAL_PROJECT_STATUS.md
- PACKAGE_SUMMARY.md
- ALTER_TABLE_SUMMARY.md
- SAVEPOINT_SUMMARY.md
- SESSION_SUMMARY_2026-03-31.md
- TEST_RESULTS_AFTER_SET_STRINGN_FIX.md

### Build Artifacts

**Build directories:**
- build_unix.old/ (keep locally, remove from git)
- test/scr050/

**Build artifacts:**
- *.o (object files)
- *.lo (libtool object files)
- *.a (static libraries)
- *.so, *.so.* (shared libraries)
- *.la (libtool libraries)
- *.gcno (coverage notes)
- *.gcda (coverage data)
- *.gcov (coverage reports)
- .libs/ directories
- .deps/ directories

**Build logs:**
- *.log
- build.log

### Test Artifacts

**Test database files:**
- *.db (all test databases)
- *.db_tbl.* (Berkeley DB table files)
- *.db_* (Berkeley DB related files)
- __db.* (Berkeley DB environment files)

**Test directories:**
- test.db/
- test_db_fresh/
- test_fresh/
- test/src/coverage_html/

**Test binaries:**
- test/src/test_api
- test/src/test_api_noasan
- test/src/test_debug
- test/src/test_file_vs_memory
- test/src/test_fts
- test/src/test_isolation_check
- test/src/test_memory
- test/src/test_select_minimal
- test/src/test_vdbe

**Test output:**
- test/src/test_api_results.log
- test/src/test_api_results.txt
- test/src/test_run.log
- test/src/test_output.txt

**Test scripts (generated):**
- test/src/analyze_coverage.sh
- test/src/analyze_coverage_gaps.sh
- test/src/enable_coverage.sh
- test/src/ci_test.sh
- test/src/run_tests.sh

### Other Cruft

**Valgrind:**
- vgcore.* (core dumps)

**Backup files:**
- *.backup
- *.bak
- *.old

**Editor files:**
- *.swp, *.swo
- *~
- .*.swp, .*.swo

## What to Keep

### Essential Documentation

**Project documentation:**
- README.md - Main project documentation
- FEATURES.md - Feature list
- CHANGELOG.md - Version history
- MIGRATION_GUIDE.md - Upgrade guide
- LICENSE - License information

**Integration documentation (from final session):**
- FINAL_INTEGRATION_REPORT.md - Complete achievement summary
- INTEGRATION_PLAN.md - Detailed integration steps
- INTEGRATION_STATUS.md - Current status
- MISSION_COMPLETE.md - Executive summary
- EXECUTION_SUMMARY.md - Parallel agent execution summary
- COMPREHENSIVE_WORK_STATUS.md - Full work status

**Feature-specific documentation:**
- PRAGMA_QUICK_REFERENCE.md - PRAGMA user guide
- PRAGMA_COMMANDS_SUMMARY.md - PRAGMA executive summary
- PRAGMA_IMPLEMENTATION.md - PRAGMA technical details (test/src/)
- VTAB_IMPLEMENTATION.md - Virtual tables guide
- ACHIEVEMENT_REPORT.md - Test suite success (test/src/)

### Source Code

**All source files:**
- src/*.c, src/*.h
- src/inc/*.h
- dist/ (configuration and build system)
- test/src/*.c, test/src/*.h (test source code)

### Build System

**Keep these:**
- dist/Makefile.in - Source Makefile template
- dist/configure - Configuration script
- dist/aclocal/*.m4 - Autoconf macros
- All dist/ scripts and tools

### Test Source

**Keep test source code:**
- test/src/*.c (all test source files)
- test/src/*.h (all test headers)
- test/src/Makefile (if it's a source file)

## Verification

After cleanup, verify:

```bash
# Check git status
git status

# Verify important files are still tracked
git ls-files | grep -E "README|FINAL_INTEGRATION|PRAGMA|VTAB"

# Verify cruft is removed
git ls-files | grep -E "agent/|_FIX\.md|_SUMMARY\.md" || echo "Cruft removed"

# Verify no build artifacts tracked
git ls-files | grep -E "\.(o|gcno|gcda)$" || echo "Build artifacts removed"

# Verify no test databases tracked
git ls-files | grep -E "\.db_tbl\." || echo "Test databases removed"
```

## Commit Changes

After running the cleanup script:

```bash
# Review changes
git status

# Stage all changes
git add -A

# Commit
git commit -m "Clean up repository: remove agent cruft and build artifacts

- Remove agent/ directory documentation
- Remove historical fix/summary/status/report documentation
- Remove build artifacts (*.o, *.gcno, *.gcda, build_unix.old/)
- Remove test database files (*.db_tbl.*, test databases)
- Remove test binaries and generated scripts
- Update .gitignore with comprehensive patterns
- Keep essential documentation (FINAL_INTEGRATION_REPORT, PRAGMA guides, etc.)
"
```

## Future Prevention

The updated .gitignore will prevent these files from being accidentally committed again. Key patterns:

```gitignore
# Agent cruft
agent/
*FINAL*.md
*FIX*.md
*SUMMARY*.md
# ... but keep important docs

# Build artifacts
*.o
*.gcno
*.gcda
build_unix.old/

# Test artifacts
*.db_tbl.*
test/src/test_*
!test/src/test_*.c
```

## Manual Cleanup (Optional)

To also delete files from disk (not just from Git):

```bash
# Remove build artifacts
find . -name "*.gcno" -delete
find . -name "*.gcda" -delete
find . -name "*.o" -delete

# Remove test databases
find . -name "*.db_tbl.*" -delete

# Remove agent documentation
rm -rf agent/

# Remove specific cruft files
rm -f *FIX*.md *SUMMARY*.md
# (Be careful with wildcards - verify first with ls)
```

## Summary

**Total files to remove:** ~200+ agent documentation files, build artifacts, and test databases

**Files to keep:** ~50 source files, ~15 documentation files, build system

**Result:** Clean repository with only essential files tracked in Git
