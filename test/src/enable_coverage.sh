#!/usr/bin/env bash
#
# enable_coverage.sh - Enable code coverage collection for DBSQL
#
# This script:
# 1. Backs up the current Makefile
# 2. Modifies build flags to enable coverage
# 3. Rebuilds the library with coverage instrumentation
# 4. Rebuilds the test runner
# 5. Provides instructions for running tests and collecting coverage

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Project paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR/../.."
BUILD_DIR="$PROJECT_ROOT/build_unix.old"
TEST_DIR="$PROJECT_ROOT/test/scr050"
MAKEFILE="$BUILD_DIR/Makefile"
BACKUP_MAKEFILE="$MAKEFILE.backup-$(date +%Y%m%d-%H%M%S)"

echo "========================================="
echo "DBSQL Code Coverage Enablement Script"
echo "========================================="
echo ""

# Check if lcov is available
if ! command -v lcov &> /dev/null; then
    echo -e "${YELLOW}Warning: lcov not found. Install with:${NC}"
    echo "  nix-shell -p lcov"
    echo "  # or"
    echo "  apt-get install lcov  # Ubuntu/Debian"
    echo "  brew install lcov     # macOS"
    echo ""
fi

# Step 1: Backup Makefile
echo "Step 1: Backing up Makefile..."
if [ ! -f "$BACKUP_MAKEFILE" ]; then
    cp "$MAKEFILE" "$BACKUP_MAKEFILE"
    echo -e "${GREEN}✓${NC} Backup created: $BACKUP_MAKEFILE"
else
    echo -e "${YELLOW}!${NC} Backup already exists: $BACKUP_MAKEFILE"
fi
echo ""

# Step 2: Modify Makefile for coverage
echo "Step 2: Checking Makefile for coverage flags..."
if grep -q "\-\-coverage" "$MAKEFILE"; then
    echo -e "${GREEN}✓${NC} Coverage flags already present"
else
    echo -e "${YELLOW}!${NC} Coverage flags not found. Modifying Makefile..."

    # Modify CFLAGS to add coverage flags
    sed -i.bak 's/^\(CFLAGS=.*-g\)\(.*\)$/\1 -O0 --coverage -fprofile-arcs -ftest-coverage\2/' "$MAKEFILE"

    # Modify LDFLAGS to add coverage flags
    if grep -q "^LDFLAGS=" "$MAKEFILE"; then
        sed -i.bak 's/^\(LDFLAGS=.*\)$/\1 --coverage/' "$MAKEFILE"
    else
        sed -i.bak '/^CC=/a LDFLAGS=\t\t--coverage' "$MAKEFILE"
    fi

    echo -e "${GREEN}✓${NC} Makefile modified for coverage"
fi
echo ""

# Step 3: Rebuild library
echo "Step 3: Rebuilding DBSQL library with coverage..."
echo "This may take several minutes..."
cd "$BUILD_DIR"
make clean > /dev/null 2>&1 || true
if make -j4; then
    echo -e "${GREEN}✓${NC} Library rebuilt successfully"
    echo "Library location: $BUILD_DIR/.libs/libdbsql-0.4.so"
else
    echo -e "${RED}✗${NC} Library build failed"
    echo "Check build logs for errors"
    exit 1
fi
echo ""

# Step 4: Check for .gcno files (coverage notes)
echo "Step 4: Verifying coverage instrumentation..."
GCNO_COUNT=$(find "$BUILD_DIR" -name "*.gcno" 2>/dev/null | wc -l)
if [ "$GCNO_COUNT" -gt 0 ]; then
    echo -e "${GREEN}✓${NC} Found $GCNO_COUNT coverage note files (.gcno)"
else
    echo -e "${RED}✗${NC} No coverage note files found"
    echo "Coverage instrumentation may have failed"
    exit 1
fi
echo ""

# Step 5: Rebuild test runner
echo "Step 5: Rebuilding test runner..."
cd "$TEST_DIR"
if make clean && make; then
    echo -e "${GREEN}✓${NC} Test runner rebuilt successfully"
else
    echo -e "${YELLOW}!${NC} Test runner build encountered issues"
    echo "You may still be able to use existing test runner"
fi
echo ""

# Step 6: Instructions
echo "========================================="
echo -e "${GREEN}Coverage Enabled Successfully!${NC}"
echo "========================================="
echo ""
echo "Next Steps:"
echo ""
echo "1. Run the test suite:"
echo "   cd $TEST_DIR"
echo "   export LD_LIBRARY_PATH=$BUILD_DIR/.libs:\$LD_LIBRARY_PATH"
echo "   ./dbsql_tclsh tests/quick.test    # Quick smoke test"
echo "   ./dbsql_tclsh tests/all.test      # Full suite (may take 15-30 min)"
echo ""
echo "2. Generate coverage report:"
echo "   cd $BUILD_DIR"
echo "   lcov --capture --directory . --output-file coverage.info"
echo "   lcov --remove coverage.info '/usr/*' '/nix/*' --output-file coverage_filtered.info"
echo "   lcov --list coverage_filtered.info"
echo ""
echo "3. Generate HTML report (optional):"
echo "   genhtml coverage_filtered.info --output-directory coverage_html"
echo "   firefox coverage_html/index.html"
echo ""
echo "4. View coverage summary:"
echo "   lcov --summary coverage_filtered.info"
echo ""
echo "To restore original Makefile:"
echo "   cp $BACKUP_MAKEFILE $MAKEFILE"
echo ""
echo "Coverage data files (.gcda) will be created in:"
echo "   $BUILD_DIR/"
echo "   $BUILD_DIR/.libs/"
echo ""
