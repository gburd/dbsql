#!/bin/bash
# Verification script to demonstrate the test suite fix
# This script shows that the test suite now achieves 100% pass rate

set -e

echo "=================================================="
echo "DBSQL Test Suite Fix Verification"
echo "=================================================="
echo

# Build test if needed
if [ ! -f test_api_quick ]; then
    echo "Building test_api..."
    gcc -std=c99 -I../../build_unix.old -I../../src/inc \
        -o test_api_quick test_api.c test_framework.c \
        -L../../build_unix.old -ldbsql-test \
        -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib \
        -ldb-4.8 -lm -Wl,-rpath,../../build_unix.old
    echo
fi

echo "Running test suite 10 times to verify consistency..."
echo

TOTAL_RUNS=10
PASSED_RUNS=0

for i in $(seq 1 $TOTAL_RUNS); do
    echo -n "Run $i: "
    OUTPUT=$(./test_api_quick 2>&1)
    PASSED=$(echo "$OUTPUT" | grep "# Passed:" | awk '{print $3}')
    FAILED=$(echo "$OUTPUT" | grep "# Failed:" | awk '{print $3}')

    if [ "$FAILED" = "0" ] && [ "$PASSED" = "41" ]; then
        echo "✓ PASS (41/41 tests)"
        PASSED_RUNS=$((PASSED_RUNS + 1))
    else
        echo "✗ FAIL ($PASSED/41 tests passed, $FAILED failed)"
    fi
done

echo
echo "=================================================="
echo "Results Summary"
echo "=================================================="
echo "Total runs:          $TOTAL_RUNS"
echo "Successful runs:     $PASSED_RUNS"
echo "Pass rate:           $((PASSED_RUNS * 100 / TOTAL_RUNS))%"
echo
echo "Target:              95%+ pass rate"

if [ $PASSED_RUNS -eq $TOTAL_RUNS ]; then
    echo "Status:              ✓ TARGET EXCEEDED (100%)"
    echo
    echo "All tests pass consistently across multiple runs."
    echo "The database file cleanup fix is working correctly."
    exit 0
else
    echo "Status:              ✗ TARGET NOT MET"
    exit 1
fi
