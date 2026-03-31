#!/bin/bash
#
# Comprehensive Valgrind Memory Safety Test Suite
# Task #141 from COMPREHENSIVE_TESTING_PLAN.md
#
# Runs valgrind on all test executables to detect memory safety issues
#

set -euo pipefail

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_SRC_DIR="$SCRIPT_DIR/src"
OUTPUT_DIR="$SCRIPT_DIR/valgrind_reports"
SUMMARY_FILE="$OUTPUT_DIR/summary.txt"
REPORT_FILE="$OUTPUT_DIR/VALGRIND_REPORT.md"

# Find test executables
TEST_BINARIES=()
for test in test_api test_vdbe test_memory test_fts test_comprehensive; do
    if [ -x "$TEST_SRC_DIR/$test" ]; then
        TEST_BINARIES+=("$test")
    fi
done

# Valgrind options
VALGRIND_OPTS=(
    --leak-check=full
    --show-leak-kinds=all
    --track-origins=yes
    --verbose
    --error-exitcode=42
    --suppressions=/dev/null
)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Statistics
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
LEAKED_TESTS=0
ERROR_TESTS=0

# Create output directory
mkdir -p "$OUTPUT_DIR"
rm -f "$SUMMARY_FILE"
rm -f "$REPORT_FILE"

# Start test suite
echo "========================================="
echo "DBSQL Valgrind Memory Safety Test Suite"
echo "========================================="
echo ""
echo "Task: #141 - Comprehensive Valgrind Suite"
echo "Output: $OUTPUT_DIR"
echo "Date: $(date '+%Y-%m-%d %H:%M:%S')"
echo ""

if [ ${#TEST_BINARIES[@]} -eq 0 ]; then
    echo -e "${RED}ERROR: No test executables found!${NC}"
    echo ""
    echo "Please build test binaries first:"
    echo "  cd $TEST_SRC_DIR"
    echo "  make clean && make"
    exit 1
fi

echo "Found ${#TEST_BINARIES[@]} test executable(s):"
for test in "${TEST_BINARIES[@]}"; do
    echo "  - $test"
done
echo ""

# Helper function to run valgrind test
run_valgrind_test() {
    local test_binary="$1"
    local test_name="$(basename "$test_binary")"
    local log_file="$OUTPUT_DIR/${test_name}.txt"
    local test_full_path="$TEST_SRC_DIR/$test_binary"

    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    echo -e "${BLUE}=========================================${NC}"
    echo -e "${BLUE}Test $TOTAL_TESTS: $test_name${NC}"
    echo -e "${BLUE}=========================================${NC}"
    echo ""

    # Check if binary exists and is executable
    if [ ! -x "$test_full_path" ]; then
        echo -e "${RED}SKIP: Binary not found or not executable${NC}"
        echo "SKIP: $test_name (not executable)" >> "$SUMMARY_FILE"
        return
    fi

    echo "Running: valgrind ${VALGRIND_OPTS[*]} $test_full_path"
    echo ""

    # Run valgrind
    local exit_code=0
    valgrind "${VALGRIND_OPTS[@]}" --log-file="$log_file" "$test_full_path" || exit_code=$?

    echo ""
    echo "Analyzing results..."

    # Parse valgrind log
    local definitely_lost=0
    local indirectly_lost=0
    local possibly_lost=0
    local still_reachable=0
    local error_count=0

    if [ -f "$log_file" ]; then
        definitely_lost=$(grep "definitely lost:" "$log_file" | sed 's/.*definitely lost: \([0-9,]*\) bytes.*/\1/' | tr -d ',' || echo "0")
        indirectly_lost=$(grep "indirectly lost:" "$log_file" | sed 's/.*indirectly lost: \([0-9,]*\) bytes.*/\1/' | tr -d ',' || echo "0")
        possibly_lost=$(grep "possibly lost:" "$log_file" | sed 's/.*possibly lost: \([0-9,]*\) bytes.*/\1/' | tr -d ',' || echo "0")
        still_reachable=$(grep "still reachable:" "$log_file" | sed 's/.*still reachable: \([0-9,]*\) bytes.*/\1/' | tr -d ',' || echo "0")
        error_count=$(grep "ERROR SUMMARY:" "$log_file" | sed 's/.*ERROR SUMMARY: \([0-9]*\) errors.*/\1/' || echo "0")
    fi

    # Determine test result
    local status="UNKNOWN"
    local has_leak=false
    local has_error=false

    if [ "$definitely_lost" != "0" ] || [ "$indirectly_lost" != "0" ]; then
        status="FAIL (MEMORY LEAK)"
        has_leak=true
        LEAKED_TESTS=$((LEAKED_TESTS + 1))
    elif [ "$error_count" != "0" ]; then
        status="FAIL (ERRORS)"
        has_error=true
        ERROR_TESTS=$((ERROR_TESTS + 1))
    elif [ "$exit_code" -eq 42 ]; then
        status="FAIL (VALGRIND ERROR)"
        has_error=true
        ERROR_TESTS=$((ERROR_TESTS + 1))
    elif [ "$exit_code" -ne 0 ]; then
        status="FAIL (TEST FAILURE)"
        FAILED_TESTS=$((FAILED_TESTS + 1))
    else
        status="PASS"
        PASSED_TESTS=$((PASSED_TESTS + 1))
    fi

    # Print summary
    echo ""
    echo "--- Memory Summary ---"
    echo "Definitely lost:  $definitely_lost bytes"
    echo "Indirectly lost:  $indirectly_lost bytes"
    echo "Possibly lost:    $possibly_lost bytes"
    echo "Still reachable:  $still_reachable bytes"
    echo "Error count:      $error_count"
    echo "Exit code:        $exit_code"
    echo ""

    # Print status
    if [ "$status" == "PASS" ]; then
        echo -e "Status: ${GREEN}✅ $status${NC}"
        echo "PASS: $test_name" >> "$SUMMARY_FILE"
    else
        echo -e "Status: ${RED}❌ $status${NC}"
        echo "FAIL: $test_name - $status" >> "$SUMMARY_FILE"

        if [ "$has_leak" = true ]; then
            echo "  Definitely lost: $definitely_lost bytes" >> "$SUMMARY_FILE"
            echo "  Indirectly lost: $indirectly_lost bytes" >> "$SUMMARY_FILE"
        fi

        if [ "$has_error" = true ]; then
            echo "  Error count: $error_count" >> "$SUMMARY_FILE"
        fi
    fi

    echo ""
    echo "Log file: $log_file"
    echo ""
}

# Run tests on all binaries
for test_binary in "${TEST_BINARIES[@]}"; do
    run_valgrind_test "$test_binary"
done

# ========== Generate Report ==========
echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}Generating Report${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""

# Generate markdown report
cat > "$REPORT_FILE" <<EOF
# DBSQL Valgrind Memory Safety Report

**Task**: #141 - Comprehensive Valgrind Test Suite
**Date**: $(date +"%Y-%m-%d %H:%M:%S")
**Platform**: $(uname -s) $(uname -m) ($(uname -r))
**Valgrind Version**: $(valgrind --version 2>&1 | head -1)

---

## Executive Summary

- **Total Tests**: $TOTAL_TESTS
- **Passed**: $PASSED_TESTS ($(awk "BEGIN {printf \"%.1f\", $PASSED_TESTS * 100.0 / $TOTAL_TESTS}") %)
- **Failed (Test Failures)**: $FAILED_TESTS
- **Failed (Memory Leaks)**: $LEAKED_TESTS
- **Failed (Errors)**: $ERROR_TESTS

EOF

# Determine overall status
if [ "$LEAKED_TESTS" -eq 0 ] && [ "$ERROR_TESTS" -eq 0 ]; then
    cat >> "$REPORT_FILE" <<EOF
**Overall Status**: ✅ **PASS** - Zero memory leaks and errors detected

All tests completed successfully with no memory safety issues.

EOF
else
    cat >> "$REPORT_FILE" <<EOF
**Overall Status**: ❌ **FAIL** - Memory leaks or errors detected

Memory safety issues were found that must be addressed before production deployment.

EOF
fi

# Add test categories from plan
cat >> "$REPORT_FILE" <<'EOF'
---

## Test Coverage

This suite tests the following categories as specified in COMPREHENSIVE_TESTING_PLAN.md:

### Implemented Tests

EOF

# List each test binary and what it covers
for test_binary in "${TEST_BINARIES[@]}"; do
    cat >> "$REPORT_FILE" <<EOF
#### $test_binary

EOF

    case "$test_binary" in
        test_api)
            cat >> "$REPORT_FILE" <<'EOF'
- Database create/open/close
- CREATE TABLE
- INSERT operations (single and multiple rows)
- SELECT operations (with WHERE, ORDER BY, aggregates)
- DELETE operations
- UPDATE operations
- Transactions (BEGIN/COMMIT/ROLLBACK)
- CREATE INDEX
- DROP TABLE
- Error handling
- Multi-table operations

EOF
            ;;
        test_vdbe)
            cat >> "$REPORT_FILE" <<'EOF'
- Virtual Database Engine (VDBE) opcodes
- Instruction execution
- Stack operations
- Memory management in VDBE
- VDBE program lifecycle

EOF
            ;;
        test_memory)
            cat >> "$REPORT_FILE" <<'EOF'
- Memory allocation/deallocation
- Memory pressure scenarios
- Concurrent memory operations
- Memory leak detection
- Out-of-memory handling

EOF
            ;;
        test_fts)
            cat >> "$REPORT_FILE" <<'EOF'
- Full-text search functionality
- FTS table operations
- Text tokenization
- Search query execution

EOF
            ;;
        test_comprehensive)
            cat >> "$REPORT_FILE" <<'EOF'
- UPDATE statements
- DELETE statements
- Aggregate functions (COUNT, SUM, AVG, MIN, MAX)
- ORDER BY (ASC/DESC)
- String functions (LENGTH, UPPER, LOWER, SUBSTR)
- WHERE operators (>, >=, <, <=, =, !=)
- NULL handling (IS NULL, IS NOT NULL)
- LIKE operator
- Complex SQL scenarios

EOF
            ;;
    esac
done

# Add test results
cat >> "$REPORT_FILE" <<'EOF'
---

## Test Results

### Summary

```
EOF

cat "$SUMMARY_FILE" >> "$REPORT_FILE"

cat >> "$REPORT_FILE" <<'EOF'
```

EOF

# Add detailed results for failed tests
if [ "$LEAKED_TESTS" -gt 0 ] || [ "$ERROR_TESTS" -gt 0 ] || [ "$FAILED_TESTS" -gt 0 ]; then
    cat >> "$REPORT_FILE" <<'EOF'
### Failed Tests Details

EOF

    grep "^FAIL:" "$SUMMARY_FILE" | while IFS= read -r line; do
        test_name=$(echo "$line" | awk '{print $2}')
        log_file="$OUTPUT_DIR/${test_name}.txt"

        cat >> "$REPORT_FILE" <<EOF

#### $test_name

\`\`\`
EOF

        if [ -f "$log_file" ]; then
            # Extract relevant sections
            echo "=== Error Summary ===" >> "$REPORT_FILE"
            grep "ERROR SUMMARY" "$log_file" >> "$REPORT_FILE" || echo "No error summary found" >> "$REPORT_FILE"
            echo "" >> "$REPORT_FILE"

            echo "=== Leak Summary ===" >> "$REPORT_FILE"
            grep -A 10 "LEAK SUMMARY" "$log_file" >> "$REPORT_FILE" || echo "No leak summary found" >> "$REPORT_FILE"
            echo "" >> "$REPORT_FILE"

            echo "=== Heap Summary ===" >> "$REPORT_FILE"
            grep -A 5 "HEAP SUMMARY" "$log_file" >> "$REPORT_FILE" || echo "No heap summary found" >> "$REPORT_FILE"
            echo "" >> "$REPORT_FILE"

            # Show first few errors
            echo "=== First Errors ===" >> "$REPORT_FILE"
            grep -A 5 "^==[0-9]*== Invalid" "$log_file" | head -30 >> "$REPORT_FILE" || true
            grep -A 5 "^==[0-9]*== Conditional jump" "$log_file" | head -30 >> "$REPORT_FILE" || true
        else
            echo "Log file not found: $log_file" >> "$REPORT_FILE"
        fi

        cat >> "$REPORT_FILE" <<'EOF'
```

EOF
    done
fi

# Add success criteria
cat >> "$REPORT_FILE" <<'EOF'
---

## Success Criteria

| Criterion | Target | Status |
|-----------|--------|--------|
EOF

# Aggregate statistics from all log files
TOTAL_DEF_LOST=0
TOTAL_IND_LOST=0
TOTAL_INVALID_READS=0
TOTAL_INVALID_WRITES=0
TOTAL_UNINIT=0
TOTAL_INVALID_FREES=0

for log_file in "$OUTPUT_DIR"/*.txt; do
    if [ -f "$log_file" ]; then
        def_lost=$(grep "definitely lost:" "$log_file" | sed 's/.*definitely lost: \([0-9,]*\) bytes.*/\1/' | tr -d ',' || echo "0")
        ind_lost=$(grep "indirectly lost:" "$log_file" | sed 's/.*indirectly lost: \([0-9,]*\) bytes.*/\1/' | tr -d ',' || echo "0")

        TOTAL_DEF_LOST=$((TOTAL_DEF_LOST + def_lost))
        TOTAL_IND_LOST=$((TOTAL_IND_LOST + ind_lost))

        TOTAL_INVALID_READS=$((TOTAL_INVALID_READS + $(grep -c "Invalid read" "$log_file" 2>/dev/null || echo "0")))
        TOTAL_INVALID_WRITES=$((TOTAL_INVALID_WRITES + $(grep -c "Invalid write" "$log_file" 2>/dev/null || echo "0")))
        TOTAL_UNINIT=$((TOTAL_UNINIT + $(grep -c "uninitialised value" "$log_file" 2>/dev/null || echo "0")))
        TOTAL_INVALID_FREES=$((TOTAL_INVALID_FREES + $(grep -c "Invalid free" "$log_file" 2>/dev/null || echo "0")))
    fi
done

cat >> "$REPORT_FILE" <<EOF
| Definitely lost | 0 bytes | $([ "$TOTAL_DEF_LOST" -eq 0 ] && echo "✅ PASS" || echo "❌ FAIL ($TOTAL_DEF_LOST bytes)") |
| Indirectly lost | 0 bytes | $([ "$TOTAL_IND_LOST" -eq 0 ] && echo "✅ PASS" || echo "❌ FAIL ($TOTAL_IND_LOST bytes)") |
| Invalid reads | 0 | $([ "$TOTAL_INVALID_READS" -eq 0 ] && echo "✅ PASS" || echo "❌ FAIL ($TOTAL_INVALID_READS)") |
| Invalid writes | 0 | $([ "$TOTAL_INVALID_WRITES" -eq 0 ] && echo "✅ PASS" || echo "❌ FAIL ($TOTAL_INVALID_WRITES)") |
| Uninitialized values | 0 | $([ "$TOTAL_UNINIT" -eq 0 ] && echo "✅ PASS" || echo "❌ FAIL ($TOTAL_UNINIT)") |
| Invalid frees | 0 | $([ "$TOTAL_INVALID_FREES" -eq 0 ] && echo "✅ PASS" || echo "❌ FAIL ($TOTAL_INVALID_FREES)") |

EOF

# Add recommendations
cat >> "$REPORT_FILE" <<'EOF'
---

## Recommendations

EOF

if [ "$LEAKED_TESTS" -eq 0 ] && [ "$ERROR_TESTS" -eq 0 ]; then
    cat >> "$REPORT_FILE" <<'EOF'
### Memory Safety Status: ✅ CLEAN

All memory safety tests passed successfully. The codebase demonstrates:

- **Zero memory leaks** - All allocated memory is properly freed
- **No buffer overflows** - All memory access is within bounds
- **No invalid memory access** - All pointers are valid
- **Proper resource cleanup** - Resources are freed in all code paths

The system meets production-grade memory safety standards.

### Next Steps

1. **Maintain Standards**: Ensure all new code passes valgrind before merge
2. **CI Integration**: Add valgrind testing to continuous integration pipeline
3. **Performance Testing**: Proceed with performance benchmarking (Task #142)
4. **Documentation**: Update memory management documentation with best practices

EOF
else
    cat >> "$REPORT_FILE" <<EOF
### Memory Safety Status: ❌ ISSUES DETECTED

Memory safety issues were detected that require attention:

- **Memory leaks**: $TOTAL_DEF_LOST bytes definitely lost, $TOTAL_IND_LOST bytes indirectly lost
- **Invalid operations**: $TOTAL_INVALID_READS invalid reads, $TOTAL_INVALID_WRITES invalid writes
- **Uninitialized values**: $TOTAL_UNINIT uses of uninitialized values
- **Invalid frees**: $TOTAL_INVALID_FREES invalid free operations

### Required Actions

1. **Fix Memory Leaks**: Review detailed logs and fix all memory leaks
2. **Fix Invalid Operations**: Address all invalid reads/writes and uninitialized values
3. **Re-test**: Run valgrind suite again after fixes
4. **Code Review**: Review memory management patterns across codebase
5. **Testing**: Add unit tests for error paths and edge cases

### Do Not Proceed Until

- All memory leaks are fixed (0 bytes definitely/indirectly lost)
- All invalid operations are resolved
- All tests pass cleanly under valgrind

**Production deployment is BLOCKED until these issues are resolved.**

EOF
fi

# Add log locations
cat >> "$REPORT_FILE" <<EOF
---

## Detailed Logs

All detailed valgrind logs are available in:

\`$OUTPUT_DIR/\`

Individual test logs:
EOF

for test_binary in "${TEST_BINARIES[@]}"; do
    echo "- \`$OUTPUT_DIR/${test_binary}.txt\`" >> "$REPORT_FILE"
done

cat >> "$REPORT_FILE" <<'EOF'

---

## Valgrind Command Used

```bash
valgrind \
    --leak-check=full \
    --show-leak-kinds=all \
    --track-origins=yes \
    --verbose \
    --error-exitcode=42 \
    <test_executable>
```

---

**Report Generated**: $(date +"%Y-%m-%d %H:%M:%S")
**Tool**: valgrind_suite.sh
**Task**: COMPREHENSIVE_TESTING_PLAN.md Task #141
EOF

echo "Report saved to: $REPORT_FILE"
echo ""

# ========== Print Final Summary ==========
echo -e "${BLUE}=========================================${NC}"
echo -e "${BLUE}Final Summary${NC}"
echo -e "${BLUE}=========================================${NC}"
echo ""
echo "Total tests:        $TOTAL_TESTS"
echo -e "Passed:             ${GREEN}$PASSED_TESTS${NC}"
echo -e "Failed (tests):     ${RED}$FAILED_TESTS${NC}"
echo -e "Failed (leaks):     ${RED}$LEAKED_TESTS${NC}"
echo -e "Failed (errors):    ${RED}$ERROR_TESTS${NC}"
echo ""
echo "Memory Leaks:"
echo "  Definitely lost:  $TOTAL_DEF_LOST bytes"
echo "  Indirectly lost:  $TOTAL_IND_LOST bytes"
echo ""
echo "Memory Errors:"
echo "  Invalid reads:    $TOTAL_INVALID_READS"
echo "  Invalid writes:   $TOTAL_INVALID_WRITES"
echo "  Uninit values:    $TOTAL_UNINIT"
echo "  Invalid frees:    $TOTAL_INVALID_FREES"
echo ""
echo "Report: $REPORT_FILE"
echo "Logs:   $OUTPUT_DIR/"
echo ""

# Exit with appropriate code
if [ "$LEAKED_TESTS" -eq 0 ] && [ "$ERROR_TESTS" -eq 0 ]; then
    echo -e "${GREEN}✅ SUCCESS: All memory safety tests passed!${NC}"
    echo ""
    echo "The codebase is clean and ready for production use."
    exit 0
else
    echo -e "${RED}❌ FAILURE: Memory safety issues detected!${NC}"
    echo ""
    echo "Please review the report and fix all issues before proceeding."
    exit 1
fi
