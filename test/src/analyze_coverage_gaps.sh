#!/usr/bin/env bash
#
# analyze_coverage_gaps.sh - Analyze code coverage and identify gaps
#
# Prerequisites:
# 1. Library built with coverage flags (run enable_coverage.sh first)
# 2. Test suite has been executed (generates .gcda files)
# 3. lcov is installed
#
# Usage:
#   ./analyze_coverage_gaps.sh [--html] [--report FILE]

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Options
GENERATE_HTML=0
REPORT_FILE=""

# Parse arguments
while [[ $# -gt 0 ]]; then
    case $1 in
        --html)
            GENERATE_HTML=1
            shift
            ;;
        --report)
            REPORT_FILE="$2"
            shift 2
            ;;
        *)
            echo "Usage: $0 [--html] [--report FILE]"
            echo "  --html         Generate HTML coverage report"
            echo "  --report FILE  Write analysis to FILE"
            exit 1
            ;;
    esac
done

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR/../.."
BUILD_DIR="$PROJECT_ROOT/build_unix.old"
SRC_DIR="$PROJECT_ROOT/src"

echo "========================================="
echo "DBSQL Coverage Gap Analysis"
echo "========================================="
echo ""

# Check prerequisites
if ! command -v lcov &> /dev/null; then
    echo -e "${RED}Error: lcov not found${NC}"
    echo "Install with: nix-shell -p lcov"
    exit 1
fi

# Check for .gcda files
GCDA_COUNT=$(find "$BUILD_DIR" -name "*.gcda" 2>/dev/null | wc -l)
if [ "$GCDA_COUNT" -eq 0 ]; then
    echo -e "${RED}Error: No coverage data files (.gcda) found${NC}"
    echo ""
    echo "Coverage data is generated when you run tests with an instrumented library."
    echo ""
    echo "Steps to generate coverage data:"
    echo "  1. Build library with coverage: bash enable_coverage.sh"
    echo "  2. Run tests: cd $PROJECT_ROOT/test/scr050"
    echo "  3.            ./dbsql_tclsh tests/quick.test"
    echo ""
    exit 1
fi

echo -e "${GREEN}✓${NC} Found $GCDA_COUNT coverage data files"
echo ""

# Capture coverage data
echo "Step 1: Capturing coverage data..."
cd "$BUILD_DIR"
lcov --quiet --capture --directory . --output-file coverage_raw.info 2>/dev/null
echo -e "${GREEN}✓${NC} Coverage data captured"
echo ""

# Filter out system files
echo "Step 2: Filtering coverage data..."
lcov --quiet --remove coverage_raw.info \
    '/usr/*' '/nix/*' '*/test/*' '*/deps/*' \
    --output-file coverage.info 2>/dev/null
echo -e "${GREEN}✓${NC} Coverage data filtered"
echo ""

# Generate summary
echo "Step 3: Generating coverage summary..."
echo ""
echo "========================================="
echo "OVERALL COVERAGE SUMMARY"
echo "========================================="
lcov --summary coverage.info 2>&1 | grep -E "lines\.\.\.\.\.\.|functions\.\.\.\.|branches\.\.\.\."
echo ""

# Extract percentages
LINES_PCT=$(lcov --summary coverage.info 2>&1 | grep "lines" | sed 's/.*: \([0-9.]*\)%.*/\1/')
FUNCS_PCT=$(lcov --summary coverage.info 2>&1 | grep "functions" | sed 's/.*: \([0-9.]*\)%.*/\1/')
BRANCH_PCT=$(lcov --summary coverage.info 2>&1 | grep "branches" | sed 's/.*: \([0-9.]*\)%.*/\1/' || echo "N/A")

# Check against 95% target
echo "========================================="
echo "TARGET ANALYSIS (95% Coverage Goal)"
echo "========================================="
echo ""

check_target() {
    local name=$1
    local value=$2
    local target=95.0

    if [ "$value" = "N/A" ]; then
        echo -e "${YELLOW}$name${NC}: Not measured"
        return 0
    fi

    local gap=$(echo "scale=1; $target - $value" | bc)
    if (( $(echo "$value >= $target" | bc -l) )); then
        echo -e "${GREEN}✓ $name${NC}: $value% (MEETS TARGET)"
    else
        echo -e "${RED}✗ $name${NC}: $value% (GAP: +$gap% needed)"
    fi
}

check_target "Line coverage    " "$LINES_PCT"
check_target "Function coverage" "$FUNCS_PCT"
check_target "Branch coverage  " "$BRANCH_PCT"
echo ""

# Analyze per-file coverage
echo "========================================="
echo "LOW COVERAGE FILES (< 80%)"
echo "========================================="
echo ""

lcov --list coverage.info 2>/dev/null | \
    awk '/^\/.*\.c/ {
        if ($2 != "" && $2 < 80) {
            gsub(/.*\//, "", $1);
            printf "%-30s %5s%%  %s\n", $1, $2, ($2 < 50 ? "CRITICAL" : $2 < 70 ? "HIGH" : "MEDIUM")
        }
    }' | sort -k2 -n | head -20

echo ""

# Find completely uncovered functions
echo "========================================="
echo "TOP 20 UNCOVERED FUNCTIONS"
echo "========================================="
echo ""

# Generate function coverage details
lcov --list-full-path coverage.info 2>/dev/null | \
    awk '/^Function/ {next}
         /^File/ {file=$0; next}
         /^[a-zA-Z_]/ && $NF == "0.0%" {
             gsub(/.*\//, "", file);
             gsub(/File.*:/, "", file);
             printf "%-40s  %s\n", $1, file
         }' | head -20

echo ""

# Generate HTML report if requested
if [ "$GENERATE_HTML" -eq 1 ]; then
    echo "========================================="
    echo "GENERATING HTML REPORT"
    echo "========================================="
    echo ""

    if command -v genhtml &> /dev/null; then
        genhtml --quiet coverage.info --output-directory coverage_html 2>/dev/null
        echo -e "${GREEN}✓${NC} HTML report generated: $BUILD_DIR/coverage_html/index.html"
        echo ""
        echo "To view:"
        echo "  firefox $BUILD_DIR/coverage_html/index.html"
        echo "  # or"
        echo "  python3 -m http.server 8000 -d $BUILD_DIR/coverage_html"
        echo "  # then open http://localhost:8000"
    else
        echo -e "${YELLOW}Warning: genhtml not found${NC}"
    fi
    echo ""
fi

# Generate detailed report if requested
if [ -n "$REPORT_FILE" ]; then
    echo "========================================="
    echo "GENERATING DETAILED REPORT"
    echo "========================================="
    echo ""

    {
        echo "# DBSQL Code Coverage Analysis Report"
        echo ""
        echo "**Generated**: $(date)"
        echo "**Coverage data files**: $GCDA_COUNT"
        echo ""
        echo "## Overall Coverage"
        echo ""
        echo "| Metric | Coverage | Target | Gap |"
        echo "|--------|----------|--------|-----|"
        printf "| Lines | %.1f%% | 95.0%% | " "$LINES_PCT"
        if (( $(echo "$LINES_PCT >= 95" | bc -l) )); then
            echo "✓ MEETS TARGET |"
        else
            echo "+$(echo "95 - $LINES_PCT" | bc)% |"
        fi
        printf "| Functions | %.1f%% | 95.0%% | " "$FUNCS_PCT"
        if (( $(echo "$FUNCS_PCT >= 95" | bc -l) )); then
            echo "✓ MEETS TARGET |"
        else
            echo "+$(echo "95 - $FUNCS_PCT" | bc)% |"
        fi
        if [ "$BRANCH_PCT" != "N/A" ]; then
            printf "| Branches | %.1f%% | 95.0%% | " "$BRANCH_PCT"
            if (( $(echo "$BRANCH_PCT >= 95" | bc -l) )); then
                echo "✓ MEETS TARGET |"
            else
                echo "+$(echo "95 - $BRANCH_PCT" | bc)% |"
            fi
        fi
        echo ""
        echo "## Low Coverage Files (< 80%)"
        echo ""
        echo "| File | Coverage | Priority |"
        echo "|------|----------|----------|"
        lcov --list coverage.info 2>/dev/null | \
            awk '/^\/.*\.c/ {
                if ($2 != "" && $2 < 80) {
                    gsub(/.*\//, "", $1);
                    priority = ($2 < 50 ? "CRITICAL" : $2 < 70 ? "HIGH" : "MEDIUM")
                    printf "| %s | %.1f%% | %s |\n", $1, $2, priority
                }
            }' | sort -t'|' -k3 -n
        echo ""
        echo "## Recommended Actions"
        echo ""
        echo "1. Review files with < 50% coverage (CRITICAL priority)"
        echo "2. Add tests for completely uncovered functions"
        echo "3. Focus on error handling paths (typically under-tested)"
        echo "4. Add edge case tests (NULL values, boundaries, type mismatches)"
        echo ""
    } > "$REPORT_FILE"

    echo -e "${GREEN}✓${NC} Detailed report saved: $REPORT_FILE"
    echo ""
fi

# Summary recommendations
echo "========================================="
echo "NEXT STEPS"
echo "========================================="
echo ""
echo "1. Review low-coverage files and identify missing tests"
echo "2. Focus on CRITICAL and HIGH priority files first"
echo "3. Write tests for completely uncovered functions"
echo "4. Add error path tests (allocation failures, invalid inputs)"
echo "5. Re-run this script after adding tests to track progress"
echo ""

if [ "$GENERATE_HTML" -eq 0 ]; then
    echo "To generate HTML report:"
    echo "  $0 --html"
    echo ""
fi

if [ -z "$REPORT_FILE" ]; then
    echo "To save detailed analysis:"
    echo "  $0 --report coverage_gaps_$(date +%Y%m%d).md"
    echo ""
fi
