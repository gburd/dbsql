#!/usr/bin/env bash
#
# run_all_tests.sh - Execute comprehensive DBSQL test suite
#
# This script runs all tests and collects results for analysis.
#

set -euo pipefail

# Configuration
UTILITY_FIXES_DIR="../dbsql-utility-fixes"
BUILD_DIR="${UTILITY_FIXES_DIR}/build_unix"
DBSQL_EXEC="${BUILD_DIR}/dbsql"
TEST_DIR="./test/scr050/tests"
RESULTS_DIR="./test_results"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
RESULTS_FILE="${RESULTS_DIR}/test_run_${TIMESTAMP}.log"
SUMMARY_FILE="${RESULTS_DIR}/test_summary_${TIMESTAMP}.txt"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

log() {
    echo -e "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$RESULTS_FILE"
}

log_success() {
    log "${GREEN}$1${NC}"
}

log_info() {
    log "${YELLOW}$1${NC}"
}

log_error() {
    log "${RED}$1${NC}"
}

# Create results directory
mkdir -p "$RESULTS_DIR"

log_info "===================================================================="
log_info "DBSQL Test Suite Execution"
log_info "===================================================================="
log_info "Date: $(date)"
log_info "Executable: $DBSQL_EXEC"
log_info "Test directory: $TEST_DIR"
log_info "Results: $RESULTS_FILE"
log_info ""

# Verify executable exists
if [ ! -f "$DBSQL_EXEC" ]; then
    log_error "ERROR: dbsql executable not found: $DBSQL_EXEC"
    exit 1
fi

# Get executable info
log_info "Executable details:"
ls -lh "$DBSQL_EXEC" | tee -a "$RESULTS_FILE"
log_info ""

# Run smoke test
log_info "Running pre-test smoke tests..."
smoke_test_passed=0

if "$DBSQL_EXEC" :memory: "SELECT 1;" 2>&1 | grep -q "1"; then
    log_success "✓ Basic SELECT: PASSED"
    smoke_test_passed=$((smoke_test_passed + 1))
else
    log_error "✗ Basic SELECT: FAILED"
fi

if "$DBSQL_EXEC" :memory: "CREATE TABLE t(x INT); INSERT INTO t VALUES(42); SELECT x FROM t;" 2>&1 | grep -q "42"; then
    log_success "✓ CREATE/INSERT/SELECT: PASSED"
    smoke_test_passed=$((smoke_test_passed + 1))
else
    log_error "✗ CREATE/INSERT/SELECT: FAILED"
fi

if [ $smoke_test_passed -ne 2 ]; then
    log_error "Smoke tests failed! Continuing with test suite anyway..."
fi
log_info ""

# Initialize counters
total_test_files=0
passed_test_files=0
failed_test_files=0
skipped_test_files=0
total_test_cases=0
passed_test_cases=0
failed_test_cases=0

log_info "===================================================================="
log_info "Running Test Suite"
log_info "===================================================================="
log_info ""

# Find all test files
test_files=$(find "$TEST_DIR" -name "*.test" | sort)
total_test_files=$(echo "$test_files" | wc -l)

log_info "Found $total_test_files test files"
log_info ""

# Run each test file
for test_file in $test_files; do
    test_name=$(basename "$test_file")
    log_info "Running: $test_name"

    # Create individual test result file
    test_result_file="${RESULTS_DIR}/${test_name}_${TIMESTAMP}.log"

    # Check if this is the tester.tcl (skip it - it's a framework file)
    if [[ "$test_name" == "tester.tcl" ]]; then
        log_info "  SKIPPED (framework file)"
        skipped_test_files=$((skipped_test_files + 1))
        continue
    fi

    # Try to run the test
    # Note: This assumes dbsql_tclsh exists or we use standard tclsh
    # For now, we'll try running directly with dbsql
    test_output=""
    test_exit_code=0

    # Attempt different test execution methods
    if command -v tclsh &> /dev/null; then
        # Method 1: Use tclsh if available
        test_output=$(cd "$TEST_DIR" && tclsh "$test_name" 2>&1) || test_exit_code=$?
    else
        # Method 2: Direct execution (if test files are executable)
        log_info "  WARNING: tclsh not found, attempting direct execution"
        test_output=$("$test_file" 2>&1) || test_exit_code=$?
    fi

    # Save test output
    echo "$test_output" > "$test_result_file"

    # Analyze test output
    if echo "$test_output" | grep -q "FAIL:"; then
        # Test has failures
        fail_count=$(echo "$test_output" | grep -c "FAIL:" || true)
        log_error "  ✗ FAILED ($fail_count failures)"
        failed_test_files=$((failed_test_files + 1))
        failed_test_cases=$((failed_test_cases + fail_count))
    elif [ $test_exit_code -ne 0 ]; then
        # Test crashed or errored
        log_error "  ✗ ERROR (exit code: $test_exit_code)"
        failed_test_files=$((failed_test_files + 1))
    elif echo "$test_output" | grep -q "do_test"; then
        # Test ran successfully
        pass_count=$(echo "$test_output" | grep -c "^do_test" || true)
        log_success "  ✓ PASSED ($pass_count test cases)"
        passed_test_files=$((passed_test_files + 1))
        passed_test_cases=$((passed_test_cases + pass_count))
        total_test_cases=$((total_test_cases + pass_count))
    else
        # Unknown status
        log_info "  ? UNKNOWN STATUS"
        skipped_test_files=$((skipped_test_files + 1))
    fi
done

log_info ""
log_info "===================================================================="
log_info "Test Execution Complete"
log_info "===================================================================="
log_info ""

# Calculate statistics
pass_rate=0
if [ $total_test_files -gt 0 ]; then
    pass_rate=$(awk "BEGIN {printf \"%.1f\", ($passed_test_files / $total_test_files) * 100}")
fi

# Generate summary
{
    echo "========================================================================"
    echo "DBSQL Test Suite Summary"
    echo "========================================================================"
    echo ""
    echo "Execution Date: $(date)"
    echo "Executable: $DBSQL_EXEC"
    echo ""
    echo "Test Files:"
    echo "  Total:   $total_test_files"
    echo "  Passed:  $passed_test_files"
    echo "  Failed:  $failed_test_files"
    echo "  Skipped: $skipped_test_files"
    echo "  Pass Rate: ${pass_rate}%"
    echo ""
    echo "Test Cases (estimated):"
    echo "  Passed: $passed_test_cases"
    echo "  Failed: $failed_test_cases"
    echo ""
    echo "Results Location: $RESULTS_DIR"
    echo "Full Log: $RESULTS_FILE"
    echo ""
} | tee "$SUMMARY_FILE"

# Print summary to terminal
log_info ""
log_info "Test Summary:"
log_info "  Files: $passed_test_files/$total_test_files passed (${pass_rate}%)"
log_info "  Test cases: $passed_test_cases passed, $failed_test_cases failed"
log_info ""
log_info "Full results saved to: $RESULTS_FILE"
log_info "Summary saved to: $SUMMARY_FILE"
log_info ""

# Generate final report
log_info "Generating comprehensive report..."
./generate_report.sh "$SUMMARY_FILE" "$RESULTS_DIR"

# Exit with appropriate code
if [ $failed_test_files -gt 0 ]; then
    log_error "TESTS FAILED: $failed_test_files test files had failures"
    exit 1
else
    log_success "ALL TESTS PASSED!"
    exit 0
fi
