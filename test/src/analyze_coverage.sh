#!/bin/bash
#
# DBSQL Coverage Analysis Script
#
# Analyzes code coverage and identifies gaps requiring additional tests.
# Generates suggestions for new tests to improve coverage.
#
# Usage: ./analyze_coverage.sh [options]
#
# Options:
#   --threshold N    Set coverage threshold (default: 95)
#   --html           Open HTML report in browser
#   --suggest        Generate test suggestions for uncovered code
#   --functions      Show uncovered functions
#   --branches       Show uncovered branches
#   --report FILE    Output report to file

set -e

# Configuration
THRESHOLD=95
SHOW_HTML=0
SUGGEST=0
SHOW_FUNCTIONS=0
SHOW_BRANCHES=0
REPORT_FILE=""
SRC_DIR="../../src"
BUILD_DIR="../../build_unix.old"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Parse arguments
while [[ $# -gt 0 ]]; do
	case $1 in
		--threshold)
			THRESHOLD="$2"
			shift 2
			;;
		--html)
			SHOW_HTML=1
			shift
			;;
		--suggest)
			SUGGEST=1
			shift
			;;
		--functions)
			SHOW_FUNCTIONS=1
			shift
			;;
		--branches)
			SHOW_BRANCHES=1
			shift
			;;
		--report)
			REPORT_FILE="$2"
			shift 2
			;;
		--help)
			echo "Usage: $0 [options]"
			echo "Options:"
			echo "  --threshold N    Set coverage threshold (default: 95)"
			echo "  --html           Open HTML report in browser"
			echo "  --suggest        Generate test suggestions for uncovered code"
			echo "  --functions      Show uncovered functions"
			echo "  --branches       Show uncovered branches"
			echo "  --report FILE    Output report to file"
			exit 0
			;;
		*)
			echo "Unknown option: $1"
			exit 1
			;;
	esac
done

# Check if coverage data exists
if [ ! -f coverage.info ]; then
	echo -e "${RED}Error: No coverage data found${NC}"
	echo "Run 'make coverage' first to generate coverage data"
	exit 1
fi

# Function to extract coverage percentage
get_coverage_pct() {
	local metric=$1
	local pct=$(lcov --summary coverage.info 2>/dev/null | grep "$metric" | sed 's/.*: \([0-9.]*\)%.*/\1/')
	echo "$pct"
}

# Generate report header
report() {
	local msg="$1"
	echo ""
	echo "========================================"
	echo "$msg"
	echo "========================================"
	if [ -n "$REPORT_FILE" ]; then
		echo "" >> "$REPORT_FILE"
		echo "=======================================" >> "$REPORT_FILE"
		echo "$msg" >> "$REPORT_FILE"
		echo "=======================================" >> "$REPORT_FILE"
	fi
}

# Main analysis
echo -e "${BLUE}=== DBSQL Coverage Analysis ===${NC}"

# Get coverage metrics
lines_pct=$(get_coverage_pct "lines")
funcs_pct=$(get_coverage_pct "functions")
branches_pct=$(get_coverage_pct "branches" || echo "N/A")

report "Coverage Summary"
echo "Line coverage:     $lines_pct%"
echo "Function coverage: $funcs_pct%"
echo "Branch coverage:   $branches_pct%"
echo "Target:            $THRESHOLD%"

if [ -n "$REPORT_FILE" ]; then
	echo "Line coverage:     $lines_pct%" >> "$REPORT_FILE"
	echo "Function coverage: $funcs_pct%" >> "$REPORT_FILE"
	echo "Branch coverage:   $branches_pct%" >> "$REPORT_FILE"
fi

# Check against threshold
lines_below=$(echo "$lines_pct < $THRESHOLD" | bc)
funcs_below=$(echo "$funcs_pct < $THRESHOLD" | bc)

if [ "$lines_below" -eq 1 ] || [ "$funcs_below" -eq 1 ]; then
	echo -e "${RED}FAIL: Coverage below $THRESHOLD% threshold${NC}"
	if [ "$lines_below" -eq 1 ]; then
		gap=$(echo "$THRESHOLD - $lines_pct" | bc)
		echo -e "${YELLOW}  Line coverage gap: ${gap}%${NC}"
	fi
	if [ "$funcs_below" -eq 1 ]; then
		gap=$(echo "$THRESHOLD - $funcs_pct" | bc)
		echo -e "${YELLOW}  Function coverage gap: ${gap}%${NC}"
	fi
else
	echo -e "${GREEN}PASS: Coverage meets $THRESHOLD% target${NC}"
fi

# Show uncovered functions
if [ "$SHOW_FUNCTIONS" -eq 1 ]; then
	report "Uncovered Functions"

	if command -v lcov >/dev/null 2>&1; then
		# Extract uncovered functions from gcov files
		if [ -d coverage_html ]; then
			grep -r "0 of.*calls executed" coverage_html/ | \
				sed 's/.*>\([^<]*\)<.*0 of \([0-9]*\) calls.*/Function: \1 (0\/\2 calls)/' | \
				sort -u | head -20
		fi
	fi
fi

# Show uncovered branches
if [ "$SHOW_BRANCHES" -eq 1 ]; then
	report "Uncovered Branches"

	if [ -d coverage_html ]; then
		grep -r "branch.*never executed" coverage_html/*.gcov.html 2>/dev/null | \
			sed 's/.*gcov.html://; s/<[^>]*>//g' | \
			head -20 || echo "No branch data available"
	fi
fi

# Generate test suggestions
if [ "$SUGGEST" -eq 1 ]; then
	report "Test Suggestions"

	echo "Based on coverage analysis, consider adding tests for:"
	echo ""

	# Analyze uncovered source files
	if [ -d coverage_html ]; then
		for file in $(find $SRC_DIR -name "*.c" | head -10); do
			basename=$(basename "$file")
			coverage_file="coverage_html/$basename.gcov.html"

			if [ -f "$coverage_file" ]; then
				# Check coverage percentage for this file
				file_pct=$(grep -o "Lines executed:[0-9.]*%" "$coverage_file" | sed 's/Lines executed://; s/%//' || echo "100")

				if [ $(echo "$file_pct < $THRESHOLD" | bc) -eq 1 ]; then
					echo -e "${YELLOW}File: $basename (${file_pct}% coverage)${NC}"

					# Find uncovered functions in this file
					grep -B5 "0 of.*calls executed" "$coverage_file" 2>/dev/null | \
						grep "function.*called" | \
						sed 's/.*function \([^ ]*\) called.*/  - Add test for: \1()/' | \
						head -5 || true

					echo ""
				fi
			fi
		done
	fi

	# Suggest test patterns
	echo "Suggested test patterns:"
	echo "  1. Error handling tests (invalid inputs, NULL pointers)"
	echo "  2. Edge case tests (empty strings, zero values, max values)"
	echo "  3. Resource exhaustion tests (out of memory, too many connections)"
	echo "  4. Concurrency tests (race conditions, deadlocks)"
	echo "  5. Integration tests (multiple operations in sequence)"
	echo ""

	# Check for specific uncovered areas
	echo "Focus areas based on coverage:"

	# Check SQL parser coverage
	parser_cov=$(grep "cg_.*\.c" coverage.info 2>/dev/null | wc -l)
	if [ "$parser_cov" -lt 5 ]; then
		echo "  - SQL parser (cg_*.c files): Add more parsing tests"
	fi

	# Check VDBE coverage
	vdbe_cov=$(grep "vdbe\.c" coverage.info 2>/dev/null | wc -l)
	if [ "$vdbe_cov" -lt 100 ]; then
		echo "  - VDBE opcodes: Test more opcode combinations"
	fi

	# Check API coverage
	api_cov=$(grep "api\.c" coverage.info 2>/dev/null | wc -l)
	if [ "$api_cov" -lt 50 ]; then
		echo "  - API functions: Add more API test scenarios"
	fi

	echo ""
fi

# Open HTML report
if [ "$SHOW_HTML" -eq 1 ]; then
	if [ -d coverage_html ]; then
		if command -v xdg-open >/dev/null 2>&1; then
			xdg-open coverage_html/index.html
		elif command -v open >/dev/null 2>&1; then
			open coverage_html/index.html
		elif command -v firefox >/dev/null 2>&1; then
			firefox coverage_html/index.html &
		else
			echo "HTML report available at: coverage_html/index.html"
		fi
	else
		echo "No HTML report found. Run 'make coverage' first."
	fi
fi

# Generate detailed report if requested
if [ -n "$REPORT_FILE" ]; then
	report "Detailed File Coverage" >> "$REPORT_FILE"
	lcov --list coverage.info >> "$REPORT_FILE" 2>/dev/null || true
	echo "" >> "$REPORT_FILE"
	echo "Report saved to: $REPORT_FILE"
fi

# Exit with appropriate code
if [ "$lines_below" -eq 1 ] || [ "$funcs_below" -eq 1 ]; then
	exit 1
else
	exit 0
fi
