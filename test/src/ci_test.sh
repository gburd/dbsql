#!/bin/bash
#
# DBSQL Continuous Integration Test Script
#
# Runs comprehensive test suite with multiple configurations:
# - Debug build with sanitizers
# - Coverage analysis
# - Diagnostic build (maximum warnings)
# - libumem memory debugging
# - Release build
#
# Exit code: 0 if all tests pass, 1 if any fail

set -e

# Configuration
LOG_DIR="ci_logs"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SUMMARY_FILE="$LOG_DIR/ci_summary_$TIMESTAMP.txt"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Track results
TOTAL_CONFIGS=0
PASSED_CONFIGS=0
FAILED_CONFIGS=()

# Setup
mkdir -p "$LOG_DIR"

echo -e "${BLUE}======================================"
echo "DBSQL Continuous Integration Tests"
echo "Started: $(date)"
echo "======================================${NC}"
echo ""

# Log configuration
log_config() {
	local config=$1
	local status=$2
	local logfile=$3

	echo "" | tee -a "$SUMMARY_FILE"
	echo "======================================"  | tee -a "$SUMMARY_FILE"
	echo "Configuration: $config" | tee -a "$SUMMARY_FILE"
	echo "Status: $status" | tee -a "$SUMMARY_FILE"
	echo "Log: $logfile" | tee -a "$SUMMARY_FILE"
	echo "======================================" | tee -a "$SUMMARY_FILE"
}

# Run test configuration
run_config() {
	local name=$1
	local build_type=$2
	local make_target=$3
	local logfile="$LOG_DIR/${name}_$TIMESTAMP.log"

	TOTAL_CONFIGS=$((TOTAL_CONFIGS + 1))

	echo -e "${BLUE}[$TOTAL_CONFIGS] Testing: $name${NC}"
	echo "  Build type: $build_type"
	echo "  Target: $make_target"
	echo "  Log: $logfile"

	# Clean before each configuration
	make clean >/dev/null 2>&1 || true

	# Run tests
	if make BUILD_TYPE="$build_type" "$make_target" > "$logfile" 2>&1; then
		echo -e "  ${GREEN}✓ PASSED${NC}"
		log_config "$name" "PASSED" "$logfile"
		PASSED_CONFIGS=$((PASSED_CONFIGS + 1))
		return 0
	else
		echo -e "  ${RED}✗ FAILED${NC}"
		log_config "$name" "FAILED" "$logfile"
		FAILED_CONFIGS+=("$name")

		# Show last 20 lines of log on failure
		echo "  Last 20 lines of log:"
		tail -20 "$logfile" | sed 's/^/    /'
		return 1
	fi
}

# Initialize summary
{
	echo "DBSQL CI Test Summary"
	echo "Started: $(date)"
	echo "Host: $(hostname)"
	echo "OS: $(uname -s) $(uname -r)"
	echo "Compiler: $(gcc --version | head -1)"
	echo ""
} > "$SUMMARY_FILE"

# Configuration 1: Debug build with sanitizers
run_config "Debug + Sanitizers" "DEBUG" "test" || true

# Configuration 2: Coverage analysis
run_config "Coverage Analysis" "COVERAGE" "coverage" || true

# Configuration 3: Coverage check (95% target)
if [ -f coverage.info ]; then
	TOTAL_CONFIGS=$((TOTAL_CONFIGS + 1))
	echo -e "${BLUE}[$TOTAL_CONFIGS] Checking: Coverage Target (>= 95%)${NC}"

	if ./analyze_coverage.sh --threshold 95 > "$LOG_DIR/coverage_check_$TIMESTAMP.log" 2>&1; then
		echo -e "  ${GREEN}✓ PASSED${NC}"
		log_config "Coverage Target Check" "PASSED" "$LOG_DIR/coverage_check_$TIMESTAMP.log"
		PASSED_CONFIGS=$((PASSED_CONFIGS + 1))
	else
		echo -e "  ${YELLOW}⚠ BELOW TARGET${NC}"
		log_config "Coverage Target Check" "BELOW TARGET" "$LOG_DIR/coverage_check_$TIMESTAMP.log"
		# Don't fail CI for coverage below target, just warn
	fi
fi

# Configuration 4: Diagnostic build (maximum warnings)
run_config "Diagnostic + Warnings" "DIAGNOSTIC" "test" || true

# Configuration 5: libumem memory debugging
if [ -f ../../libumem/libumem.so ] || [ -f ../libumem/libumem.so ]; then
	run_config "libumem Memory Debugging" "UMEM" "umem-test" || true
else
	echo -e "${YELLOW}Skipping libumem tests (not built)${NC}"
	echo "Note: Build libumem first with: cd ../../libumem && make"
fi

# Configuration 6: Release build
run_config "Release Build" "RELEASE" "test" || true

# Configuration 7: AddressSanitizer with special options
TOTAL_CONFIGS=$((TOTAL_CONFIGS + 1))
echo -e "${BLUE}[$TOTAL_CONFIGS] Testing: AddressSanitizer (verbose)${NC}"
logfile="$LOG_DIR/asan_verbose_$TIMESTAMP.log"

make clean >/dev/null 2>&1 || true
if make BUILD_TYPE=DEBUG test >/dev/null 2>&1; then
	if ASAN_OPTIONS="detect_leaks=1:check_initialization_order=1:verbosity=1" \
	   LSAN_OPTIONS="verbosity=1:log_threads=1" \
	   make BUILD_TYPE=DEBUG test > "$logfile" 2>&1; then
		echo -e "  ${GREEN}✓ PASSED${NC}"
		log_config "AddressSanitizer (verbose)" "PASSED" "$logfile"
		PASSED_CONFIGS=$((PASSED_CONFIGS + 1))
	else
		echo -e "  ${RED}✗ FAILED${NC}"
		log_config "AddressSanitizer (verbose)" "FAILED" "$logfile"
		FAILED_CONFIGS+=("AddressSanitizer (verbose)")
	fi
else
	echo -e "  ${YELLOW}⚠ SKIPPED (build failed)${NC}"
fi

# Generate final summary
echo "" | tee -a "$SUMMARY_FILE"
echo "======================================" | tee -a "$SUMMARY_FILE"
echo "CI Test Results Summary" | tee -a "$SUMMARY_FILE"
echo "======================================" | tee -a "$SUMMARY_FILE"
echo "Completed: $(date)" | tee -a "$SUMMARY_FILE"
echo "" | tee -a "$SUMMARY_FILE"
echo "Total configurations: $TOTAL_CONFIGS" | tee -a "$SUMMARY_FILE"
echo "Passed: $PASSED_CONFIGS" | tee -a "$SUMMARY_FILE"
echo "Failed: ${#FAILED_CONFIGS[@]}" | tee -a "$SUMMARY_FILE"

if [ ${#FAILED_CONFIGS[@]} -gt 0 ]; then
	echo "" | tee -a "$SUMMARY_FILE"
	echo "Failed configurations:" | tee -a "$SUMMARY_FILE"
	for config in "${FAILED_CONFIGS[@]}"; do
		echo "  - $config" | tee -a "$SUMMARY_FILE"
	done
fi

echo "" | tee -a "$SUMMARY_FILE"

# Final verdict
if [ ${#FAILED_CONFIGS[@]} -eq 0 ]; then
	echo -e "${GREEN}======================================"
	echo "✓ ALL CI TESTS PASSED"
	echo "======================================${NC}"
	echo ""
	echo "Summary saved to: $SUMMARY_FILE"
	exit 0
else
	echo -e "${RED}======================================"
	echo "✗ CI TESTS FAILED"
	echo "======================================${NC}"
	echo ""
	echo "Failed configurations: ${#FAILED_CONFIGS[@]}/$TOTAL_CONFIGS"
	echo "Summary saved to: $SUMMARY_FILE"
	echo ""
	echo "Review logs in: $LOG_DIR/"
	exit 1
fi
