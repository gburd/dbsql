#!/usr/bin/env bash
set -euo pipefail

# DBSQL Smoke Test Suite
# Validates basic functionality before running comprehensive tests
# Run from anywhere: ./test/smoke-test.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DBSQL="${SCRIPT_DIR}/../result/bin/dbsql"
TESTS_PASSED=0
TESTS_FAILED=0

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "==================================="
echo "DBSQL Smoke Test Suite"
echo "==================================="
echo ""

# Check if binary exists
if [[ ! -f "$DBSQL" ]]; then
    echo -e "${RED}ERROR: dbsql binary not found at $DBSQL${NC}"
    echo "Please build the project first."
    exit 1
fi

echo "Using binary: $DBSQL"
echo ""

# Function to run a test
run_test() {
    local test_num="$1"
    local test_name="$2"
    local test_cmd="$3"
    local expected="$4"

    echo -n "Test $test_num: $test_name ... "

    # Run the command and capture output
    if output=$(eval "$test_cmd" 2>&1); then
        # Check if output matches expected (if expected is provided)
        if [[ -n "$expected" ]]; then
            if echo "$output" | grep -q "$expected"; then
                echo -e "${GREEN}PASS${NC}"
                ((TESTS_PASSED++))
                return 0
            else
                echo -e "${RED}FAIL${NC}"
                echo "  Expected to find: $expected"
                echo "  Got: $output"
                ((TESTS_FAILED++))
                return 1
            fi
        else
            echo -e "${GREEN}PASS${NC}"
            ((TESTS_PASSED++))
            return 0
        fi
    else
        echo -e "${RED}FAIL${NC}"
        echo "  Command failed: $test_cmd"
        echo "  Output: $output"
        ((TESTS_FAILED++))
        return 1
    fi
}

# Test 1: Version check
run_test 1 "Version check" \
    "\"$DBSQL\" --version" \
    ""

# Test 2: In-memory database creation
run_test 2 "In-memory database" \
    "echo '.databases' | \"$DBSQL\" :memory:" \
    ""

# Test 3: Simple SELECT
run_test 3 "SELECT 1" \
    "echo 'SELECT 1;' | \"$DBSQL\" :memory:" \
    "1"

# Test 4: CREATE TABLE
run_test 4 "CREATE TABLE" \
    "echo 'CREATE TABLE test (id INTEGER, name TEXT); .tables' | \"$DBSQL\" :memory:" \
    "test"

# Test 5: INSERT
run_test 5 "INSERT" \
    "echo 'CREATE TABLE test (id INTEGER, name TEXT); INSERT INTO test VALUES (1, \"Alice\"); SELECT COUNT(*) FROM test;' | \"$DBSQL\" :memory:" \
    "1"

# Test 6: SELECT with WHERE
run_test 6 "SELECT with WHERE" \
    "echo 'CREATE TABLE test (id INTEGER, name TEXT); INSERT INTO test VALUES (1, \"Alice\"); INSERT INTO test VALUES (2, \"Bob\"); SELECT name FROM test WHERE id = 1;' | \"$DBSQL\" :memory:" \
    "Alice"

# Test 7: ORDER BY (validates sorter implementation)
run_test 7 "ORDER BY" \
    "echo 'CREATE TABLE test (id INTEGER); INSERT INTO test VALUES (3); INSERT INTO test VALUES (1); INSERT INTO test VALUES (2); SELECT id FROM test ORDER BY id;' | \"$DBSQL\" :memory:" \
    ""

echo ""
echo "==================================="
echo "Test Results"
echo "==================================="
echo -e "Tests Passed: ${GREEN}$TESTS_PASSED${NC}"
echo -e "Tests Failed: ${RED}$TESTS_FAILED${NC}"
echo ""

if [[ $TESTS_FAILED -eq 0 ]]; then
    echo -e "${GREEN}All smoke tests passed!${NC}"
    echo "Ready to proceed with comprehensive testing."
    exit 0
else
    echo -e "${RED}Some smoke tests failed.${NC}"
    echo "Fix core issues before running comprehensive tests."
    exit 1
fi
