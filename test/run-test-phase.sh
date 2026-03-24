#!/usr/bin/env bash
set -euo pipefail

# Run DBSQL Test Phases
# Usage: ./test/run-test-phase.sh [phase_number|all]
# Example: ./test/run-test-phase.sh 2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="${SCRIPT_DIR}/scr050"
RUNNER="${TEST_DIR}/dbsql_tclsh"
TEST_FILES="${TEST_DIR}/tests"
RESULTS_DIR="${TEST_DIR}/results"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Check prerequisites
if [[ ! -x "$RUNNER" ]]; then
    echo -e "${RED}Error: Test runner not found or not executable: $RUNNER${NC}"
    echo "Build it with: cd $TEST_DIR && make"
    exit 1
fi

# Create results directory
mkdir -p "$RESULTS_DIR"

# Get timestamp for results
TIMESTAMP=$(date +%Y%m%d-%H%M%S)

# Function to run a test file
run_test_file() {
    local test_file="$1"
    local test_name=$(basename "$test_file" .test)
    local log_file="${RESULTS_DIR}/${test_name}-${TIMESTAMP}.log"

    echo -n "  Running $test_name ... "

    if "$RUNNER" "$test_file" > "$log_file" 2>&1; then
        # Check for errors in output
        if grep -q "errors out of" "$log_file"; then
            local errors=$(grep "errors out of" "$log_file" | tail -1)
            if echo "$errors" | grep -q "^0 errors"; then
                echo -e "${GREEN}PASS${NC}"
                return 0
            else
                echo -e "${RED}FAIL${NC} ($errors)"
                return 1
            fi
        else
            echo -e "${GREEN}PASS${NC}"
            return 0
        fi
    else
        echo -e "${RED}CRASH${NC}"
        return 1
    fi
}

# Phase definitions
run_phase_1() {
    echo -e "${BLUE}Phase 1: Smoke Tests${NC}"
    echo "Running smoke test script..."
    if [[ -x "${SCRIPT_DIR}/smoke-test.sh" ]]; then
        "${SCRIPT_DIR}/smoke-test.sh"
    else
        echo -e "${YELLOW}Warning: smoke-test.sh not found or not executable${NC}"
    fi
}

run_phase_2() {
    echo -e "${BLUE}Phase 2: Core SQL Tests (558 tests)${NC}"
    echo "Running 8 core SQL test files..."

    local tests=(
        select1.test
        select2.test
        insert.test
        update.test
        delete.test
        where.test
        expr.test
        table.test
    )

    for test in "${tests[@]}"; do
        run_test_file "${TEST_FILES}/$test"
    done
}

run_phase_3() {
    echo -e "${BLUE}Phase 3: Ordering and Aggregation (182 tests)${NC}"
    echo "Running 4 sorting/grouping test files..."

    local tests=(
        sort.test
        select3.test
        select4.test
        minmax.test
    )

    for test in "${tests[@]}"; do
        run_test_file "${TEST_FILES}/$test"
    done
}

run_phase_4() {
    echo -e "${BLUE}Phase 4: Advanced Features${NC}"
    echo "Running joins, subqueries, indexes, constraints, transactions, triggers, views..."

    local tests=(
        join.test
        join2.test
        subselect.test
        index.test
        unique.test
        notnull.test
        null.test
        trans.test
        trigger1.test
        trigger2.test
        trigger3.test
        trigger4.test
        view.test
    )

    for test in "${tests[@]}"; do
        run_test_file "${TEST_FILES}/$test"
    done
}

run_phase_5() {
    echo -e "${BLUE}Phase 5: Data Types and Functions${NC}"
    echo "Running function and data type test files..."

    local tests=(
        func.test
        printf.test
        date.test
        in.test
        bind.test
        select5.test
        select6.test
        insert2.test
    )

    for test in "${tests[@]}"; do
        run_test_file "${TEST_FILES}/$test"
    done
}

run_phase_6() {
    echo -e "${BLUE}Phase 6: System Features${NC}"
    echo "Running system-level test files..."

    local tests=(
        attach.test
        attach2.test
        pragma.test
        vacuum.test
        temptable.test
        memdb.test
        lock.test
        thread1.test
    )

    for test in "${tests[@]}"; do
        run_test_file "${TEST_FILES}/$test"
    done
}

run_phase_7() {
    echo -e "${BLUE}Phase 7: Edge Cases and Special Tests${NC}"
    echo "Running edge case test files..."

    local tests=(
        misc1.test
        misc2.test
        misc3.test
        intpkey.test
        rowid.test
        bigrow.test
        limit.test
        copy.test
        conflict.test
        quote.test
    )

    for test in "${tests[@]}"; do
        run_test_file "${TEST_FILES}/$test"
    done
}

run_phase_8() {
    echo -e "${BLUE}Phase 8: API and Error Handling${NC}"
    echo "Running API test files..."

    local tests=(
        capi2.test
        tclsqlite.test
        tableapi.test
        misuse.test
        hook.test
        progress.test
        auth.test
    )

    for test in "${tests[@]}"; do
        run_test_file "${TEST_FILES}/$test"
    done
}

run_phase_9() {
    echo -e "${BLUE}Phase 9: Storage Layer${NC}"
    echo "Running storage test files..."

    local tests=(
        pager.test
        main.test
    )

    for test in "${tests[@]}"; do
        run_test_file "${TEST_FILES}/$test"
    done
}

run_phase_10() {
    echo -e "${BLUE}Phase 10: Full Test Suite${NC}"
    echo "Running complete test suite..."

    local log_file="${RESULTS_DIR}/quick-test-${TIMESTAMP}.log"

    echo "Running quick.test (excludes malloc, memleak)..."
    if "$RUNNER" "${TEST_FILES}/quick.test" > "$log_file" 2>&1; then
        echo -e "${GREEN}Quick test completed${NC}"
        tail -20 "$log_file"
    else
        echo -e "${RED}Quick test failed${NC}"
        tail -50 "$log_file"
    fi
}

# Parse command line
if [[ $# -eq 0 ]]; then
    echo "Usage: $0 [phase_number|all|quick|full]"
    echo ""
    echo "Phases:"
    echo "  1    - Smoke tests (7 basic tests)"
    echo "  2    - Core SQL (8 files, 558 tests)"
    echo "  3    - Sorting & Aggregation (4 files, 182 tests)"
    echo "  4    - Advanced Features (13 files, ~500 tests)"
    echo "  5    - Functions & Data Types (8 files, ~200 tests)"
    echo "  6    - System Features (8 files, ~280 tests)"
    echo "  7    - Edge Cases (10 files, ~450 tests)"
    echo "  8    - API & Error Handling (7 files, ~500 tests)"
    echo "  9    - Storage Layer (2 files, 98 tests)"
    echo "  10   - Full Suite (quick.test)"
    echo ""
    echo "Special:"
    echo "  all    - Run phases 1-9 sequentially"
    echo "  quick  - Run quick.test (phase 10)"
    echo "  full   - Run all.test (comprehensive)"
    echo ""
    echo "Examples:"
    echo "  $0 1              # Run smoke tests only"
    echo "  $0 2              # Run core SQL tests"
    echo "  $0 all            # Run phases 1-9"
    echo "  $0 quick          # Run quick.test"
    exit 0
fi

PHASE="$1"

case "$PHASE" in
    1) run_phase_1 ;;
    2) run_phase_2 ;;
    3) run_phase_3 ;;
    4) run_phase_4 ;;
    5) run_phase_5 ;;
    6) run_phase_6 ;;
    7) run_phase_7 ;;
    8) run_phase_8 ;;
    9) run_phase_9 ;;
    10) run_phase_10 ;;

    all)
        echo "Running all phases 1-9..."
        for i in {1..9}; do
            echo ""
            run_phase_$i
            echo ""
        done
        ;;

    quick)
        run_phase_10
        ;;

    full)
        echo -e "${BLUE}Running Full Test Suite (all.test)${NC}"
        local log_file="${RESULTS_DIR}/all-test-${TIMESTAMP}.log"
        echo "This will take 4-6 hours..."
        if "$RUNNER" "${TEST_FILES}/all.test" > "$log_file" 2>&1; then
            echo -e "${GREEN}Full test completed${NC}"
            tail -20 "$log_file"
        else
            echo -e "${RED}Full test failed${NC}"
            tail -50 "$log_file"
        fi
        ;;

    *)
        echo -e "${RED}Error: Unknown phase '$PHASE'${NC}"
        echo "Run without arguments for usage information."
        exit 1
        ;;
esac

echo ""
echo "Results saved in: $RESULTS_DIR"
echo ""
