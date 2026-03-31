#!/bin/bash
# Investigation commands for DBSQL API failures
# Date: 2026-03-31
# Purpose: Identify what changed to cause test regression

set -euo pipefail

echo "==================================="
echo "DBSQL API Failure Investigation"
echo "==================================="
echo ""

# 1. Show recent commit history
echo "1. Recent Commits (Last 10):"
echo "----------------------------"
git log --oneline --graph --decorate -10
echo ""

# 2. Show what changed in critical files
echo "2. Changes in Critical Files (since dcbe5e3):"
echo "--------------------------------------------"
echo "Files: api.c, sm.c, cg_build.c"
echo ""
git diff dcbe5e3..HEAD --stat -- src/api.c src/sm.c src/cg_build.c
echo ""

# 3. Detailed diff of api.c
echo "3. Detailed Changes in api.c:"
echo "-----------------------------"
git diff dcbe5e3..HEAD -- src/api.c | head -100
echo ""
echo "[... truncated, run 'git diff dcbe5e3..HEAD -- src/api.c' for full output]"
echo ""

# 4. Look for field renames that might have been missed
echo "4. Checking for Potential Field Naming Issues:"
echo "-----------------------------------------------"
echo "Looking for old Hungarian notation that might have been missed..."
git grep -n "->p[A-Z]" src/api.c src/sm.c src/cg_build.c 2>/dev/null || echo "None found"
git grep -n "->a[A-Z]" src/api.c src/sm.c src/cg_build.c 2>/dev/null || echo "None found"
git grep -n "->n[A-Z]" src/api.c src/sm.c src/cg_build.c 2>/dev/null || echo "None found"
echo ""

# 5. Check for uncommitted changes
echo "5. Uncommitted Changes:"
echo "-----------------------"
git status --short src/api.c src/sm.c src/cg_build.c
echo ""

# 6. Find commits that touched these files
echo "6. Commits Touching API Files (Last 10):"
echo "-----------------------------------------"
git log --oneline --follow -10 -- src/api.c src/sm.c src/cg_build.c
echo ""

# 7. Show current test results summary
echo "7. Current Test Results:"
echo "------------------------"
if [ -f test/src/test_run.log ]; then
    tail -10 test/src/test_run.log
else
    echo "No test results found. Run: cd test/src && ./test_api"
fi
echo ""

# 8. Recommendations
echo "==================================="
echo "NEXT STEPS:"
echo "==================================="
echo ""
echo "1. Review the diffs above for suspicious changes"
echo "2. Run git bisect to find the exact breaking commit:"
echo "   git bisect start"
echo "   git bisect bad HEAD"
echo "   git bisect good dcbe5e3"
echo "   # Then for each commit: cd build_unix.old && make && cd ../test/src && ./test_api"
echo ""
echo "3. Or execute recovery procedure:"
echo "   See RECOVERY_OPTIONS.md for detailed instructions"
echo ""
echo "4. Or examine specific commits:"
echo "   git show <commit-hash> -- src/api.c"
echo ""
