#!/bin/bash
# DBSQL Repository Cleanup Script
# Removes agent cruft files, build artifacts, and test database files from Git

set -e

cd "$(dirname "$0")"

echo "=== DBSQL Repository Cleanup ==="
echo "This script will remove agent documentation and build artifacts from Git tracking."
echo ""

# Function to safely git rm files
safe_git_rm() {
    local pattern="$1"
    local description="$2"

    echo "Removing: $description"
    find . -path "./.git" -prune -o -name "$pattern" -type f -print | while read file; do
        if git ls-files --error-unmatch "$file" &>/dev/null; then
            echo "  - $file"
            git rm --cached "$file" 2>/dev/null || true
        fi
    done
}

# Function to remove files by pattern from git
remove_pattern() {
    local pattern="$1"
    local description="$2"

    echo "Removing: $description"
    git ls-files | grep -E "$pattern" | while read file; do
        if [ -f "$file" ]; then
            echo "  - $file"
            git rm --cached "$file" 2>/dev/null || true
        fi
    done
}

echo ""
echo "=== Phase 1: Agent Documentation Files ==="

# Remove agent cruft markdown files
echo "Removing agent FINAL/FIX/SUMMARY/STATUS/REPORT files..."

# Root directory agent cruft
for pattern in \
    "*FINAL*.md" \
    "*FIX*.md" \
    "*SUMMARY*.md" \
    "*STATUS*.md" \
    "*REPORT*.md" \
    "*ANALYSIS*.md" \
    "*COMPLETE*.md" \
    "*PROGRESS*.md" \
    "*PLAN*.md"; do
    safe_git_rm "$pattern" "Root $pattern files"
done

# Agent directory (remove entire directory from git)
if [ -d "agent" ]; then
    echo "Removing agent/ directory from Git..."
    git rm -r --cached agent/ 2>/dev/null || true
fi

# Specific agent cruft files
echo "Removing specific agent files..."
git rm --cached \
    BERKELEY_DB_FIX_FINAL.md \
    BERKELEY_DB_FIX_ANALYSIS.md \
    CURSOR_TRANSACTION_FIX.md \
    DATABASE_OPENING_FIX.md \
    MEMORY_FIXES_COMPLETE.md \
    ORDER_BY_FIX.md \
    PERSISTENCE_FIX.md \
    STR_FIX_SUMMARY.md \
    UNUSED_WARNINGS_FIXED.md \
    CLEANUP_SUMMARY.md \
    CLEANUP_SUMMARY_FINAL.md \
    DEBUG_CLEANUP_SUMMARY.md \
    RECOVERY_ANALYSIS_SUMMARY.md \
    EXECUTIVE_SUMMARY.md \
    FINAL_STATUS_REPORT.md \
    FINAL_STATUS_SUMMARY.md \
    FINAL_PROJECT_STATUS.md \
    PACKAGE_SUMMARY.md \
    ALTER_TABLE_SUMMARY.md \
    SAVEPOINT_SUMMARY.md \
    SESSION_SUMMARY_2026-03-31.md \
    TEST_RESULTS_AFTER_SET_STRINGN_FIX.md \
    2>/dev/null || true

echo ""
echo "=== Phase 2: Build Artifacts ==="

# Remove build artifacts from git
echo "Removing build artifacts (*.o, *.gcno, *.gcda, *.lo, *.a, *.so*)..."
remove_pattern "\.(o|gcno|gcda|lo|a|so|so\.[0-9])$" "Build artifacts"

# Remove build_unix.old directory from git (keep locally)
if [ -d "build_unix.old" ]; then
    echo "Removing build_unix.old/ from Git (keeping local copy)..."
    git rm -r --cached build_unix.old/ 2>/dev/null || true
fi

# Remove test/scr050 build artifacts
if [ -d "test/scr050" ]; then
    echo "Removing test/scr050/ from Git..."
    git rm -r --cached test/scr050/ 2>/dev/null || true
fi

echo ""
echo "=== Phase 3: Test Database Files ==="

# Remove test database files
echo "Removing test database files (*.db_tbl.*, test*.db, etc.)..."
safe_git_rm "*.db_tbl.*" "Berkeley DB table files"
safe_git_rm "test*.db" "Test database files"
safe_git_rm "test_*.db" "Test database files"
safe_git_rm "*_test.db" "Test database files"
safe_git_rm "__db.*" "Berkeley DB environment files"

# Remove specific test artifacts
echo "Removing test binaries and artifacts..."
git rm --cached \
    test/src/test_api \
    test/src/test_api_noasan \
    test/src/test_debug \
    test/src/test_file_vs_memory \
    test/src/test_fts \
    test/src/test_isolation_check \
    test/src/test_memory \
    test/src/test_select_minimal \
    test/src/test_vdbe \
    test/src/test_api_results.log \
    test/src/test_api_results.txt \
    test/src/test_run.log \
    test/src/test_output.txt \
    2>/dev/null || true

# Remove test directories
for dir in test.db test_db_fresh test_fresh; do
    if [ -d "$dir" ]; then
        echo "Removing $dir/ from Git..."
        git rm -r --cached "$dir" 2>/dev/null || true
    fi
done

echo ""
echo "=== Phase 4: Other Cruft ==="

# Remove valgrind output
safe_git_rm "vgcore.*" "Valgrind core dumps"

# Remove coverage reports
if [ -d "test/src/coverage_html" ]; then
    echo "Removing coverage_html/ from Git..."
    git rm -r --cached test/src/coverage_html/ 2>/dev/null || true
fi

# Remove scripts in test/src that shouldn't be tracked
echo "Removing test helper scripts from Git..."
git rm --cached \
    test/src/analyze_coverage.sh \
    test/src/analyze_coverage_gaps.sh \
    test/src/enable_coverage.sh \
    test/src/ci_test.sh \
    test/src/run_tests.sh \
    2>/dev/null || true

# Remove build logs
safe_git_rm "*.log" "Log files"
safe_git_rm "build.log" "Build log files"

# Remove backup files
safe_git_rm "*.backup" "Backup files"
safe_git_rm "*.bak" "Backup files"
safe_git_rm "*.old" "Old files"

echo ""
echo "=== Phase 5: Update .gitignore ==="

# Create comprehensive .gitignore
cat > .gitignore << 'EOF'
# Build artifacts
*.o
*.lo
*.a
*.so
*.so.*
*.la
*.gcno
*.gcda
*.gcov
.libs/
.deps/

# Build directories
build_unix.old/
build_unix/
!build_unix/.gitkeep

# Test artifacts
test/scr050/
test/src/coverage_html/
test/src/test_api
test/src/test_api_noasan
test/src/test_*
!test/src/test_*.c
!test/src/test_*.h
*.log
*.txt

# Database files
*.db
*.db_*
__db.*
*.db_tbl.*

# Valgrind
vgcore.*
*.valgrind

# Temporary files
*.swp
*.swo
*~
.*.swp
.*.swo

# Agent documentation (historical cruft)
agent/
*FINAL*.md
*FIX*.md
*SUMMARY*.md
*STATUS*.md
*REPORT*.md
*ANALYSIS*.md
*COMPLETE*.md
*PROGRESS*.md
*PLAN*.md

# Keep important documentation
!README.md
!FEATURES.md
!CHANGELOG.md
!MIGRATION_GUIDE.md
!FINAL_INTEGRATION_REPORT.md
!INTEGRATION_PLAN.md
!INTEGRATION_STATUS.md
!MISSION_COMPLETE.md
!PRAGMA_QUICK_REFERENCE.md
!PRAGMA_COMMANDS_SUMMARY.md
!VTAB_IMPLEMENTATION.md
!EXECUTION_SUMMARY.md
!COMPREHENSIVE_WORK_STATUS.md

# IDE
.vscode/
.idea/
*.iml
.project
.cproject
.settings/

# macOS
.DS_Store
.AppleDouble
.LSOverride

# Backup files
*.backup
*.bak
*.old

# Coverage
*.gcov
htmlcov/
.coverage
.coverage.*

# Python
__pycache__/
*.py[cod]
*$py.class
*.egg-info/
dist/
build/
.pytest_cache/

# Libtool
libtool

# Autotools
autom4te.cache/
config.status
config.log
configure
Makefile
Makefile.in

# Keep dist/Makefile.in (it's source)
!dist/Makefile.in
EOF

echo "Updated .gitignore"

# Stage .gitignore
git add .gitignore

echo ""
echo "=== Phase 6: Summary ==="

# Show what would be removed
echo "Files removed from Git tracking:"
git status --short | grep "^ D" | wc -l
echo ""

echo "The files are now untracked but still exist locally."
echo "To permanently delete them from disk, run:"
echo "  find . -name '*.gcno' -delete"
echo "  find . -name '*.gcda' -delete"
echo "  find . -name '*.db_tbl.*' -delete"
echo "  rm -rf agent/"
echo ""

echo "To commit these changes:"
echo "  git add -A"
echo "  git commit -m 'Clean up repository: remove agent cruft and build artifacts'"
echo ""

echo "=== Cleanup Complete ==="
