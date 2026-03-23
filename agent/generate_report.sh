#!/usr/bin/env bash
#
# generate_report.sh - Generate comprehensive test suite report
#
# Usage: ./generate_report.sh <summary_file> <results_dir>
#

set -euo pipefail

SUMMARY_FILE="${1:-}"
RESULTS_DIR="${2:-./test_results}"
REPORT_FILE="./TEST_SUITE_REPORT.md"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

if [ -z "$SUMMARY_FILE" ] || [ ! -f "$SUMMARY_FILE" ]; then
    echo "ERROR: Summary file not found or not specified"
    echo "Usage: $0 <summary_file> <results_dir>"
    exit 1
fi

echo "Generating comprehensive test report..."
echo "Summary file: $SUMMARY_FILE"
echo "Results directory: $RESULTS_DIR"
echo "Report output: $REPORT_FILE"

# Extract key metrics from summary
total_files=$(grep "Total:" "$SUMMARY_FILE" | head -1 | awk '{print $NF}')
passed_files=$(grep "Passed:" "$SUMMARY_FILE" | head -1 | awk '{print $NF}')
failed_files=$(grep "Failed:" "$SUMMARY_FILE" | head -1 | awk '{print $NF}')
skipped_files=$(grep "Skipped:" "$SUMMARY_FILE" | awk '{print $NF}')
pass_rate=$(grep "Pass Rate:" "$SUMMARY_FILE" | awk '{print $NF}')

# Generate markdown report
cat > "$REPORT_FILE" <<EOF
# DBSQL Test Suite Report

**Generated**: $TIMESTAMP
**Branch**: test-suite
**Status**: Test execution complete

---

## Executive Summary

### Overall Results

- **Total Test Files**: $total_files
- **Passed**: $passed_files
- **Failed**: $failed_files
- **Skipped**: $skipped_files
- **Pass Rate**: $pass_rate

### Assessment

EOF

# Add assessment based on pass rate
pass_rate_num=$(echo "$pass_rate" | sed 's/%//')
if (( $(echo "$pass_rate_num >= 95" | bc -l) )); then
    echo "✅ **EXCELLENT** - Pass rate exceeds 95%. Test suite is in excellent condition." >> "$REPORT_FILE"
elif (( $(echo "$pass_rate_num >= 85" | bc -l) )); then
    echo "✓ **GOOD** - Pass rate meets target (≥85%). Minor issues may exist." >> "$REPORT_FILE"
elif (( $(echo "$pass_rate_num >= 70" | bc -l) )); then
    echo "⚠ **FAIR** - Pass rate below target but acceptable for modernization. Fixes needed." >> "$REPORT_FILE"
else
    echo "❌ **POOR** - Pass rate below 70%. Significant issues need attention." >> "$REPORT_FILE"
fi

cat >> "$REPORT_FILE" <<EOF

---

## Test Results by Category

### Tests Passed

The following test files passed successfully:

EOF

# List passed tests
if [ -d "$RESULTS_DIR" ]; then
    for result_file in "$RESULTS_DIR"/*.log; do
        if [ -f "$result_file" ]; then
            if ! grep -q "FAIL:" "$result_file" 2>/dev/null; then
                test_name=$(basename "$result_file" | sed 's/_[0-9]*\.log$//')
                echo "- ✓ $test_name" >> "$REPORT_FILE"
            fi
        fi
    done
fi

cat >> "$REPORT_FILE" <<EOF

### Tests Failed

The following test files had failures:

EOF

# List failed tests
failure_found=false
if [ -d "$RESULTS_DIR" ]; then
    for result_file in "$RESULTS_DIR"/*.log; do
        if [ -f "$result_file" ]; then
            if grep -q "FAIL:" "$result_file" 2>/dev/null; then
                test_name=$(basename "$result_file" | sed 's/_[0-9]*\.log$//')
                fail_count=$(grep -c "FAIL:" "$result_file" || echo "0")
                echo "- ✗ $test_name ($fail_count failures)" >> "$REPORT_FILE"
                failure_found=true
            fi
        fi
    done
fi

if [ "$failure_found" = false ]; then
    echo "None - all tests passed!" >> "$REPORT_FILE"
fi

cat >> "$REPORT_FILE" <<EOF

---

## Failure Analysis

### Common Failure Patterns

EOF

# Analyze failure patterns
if [ -d "$RESULTS_DIR" ]; then
    # Count API-related failures
    api_failures=$(grep -r "no such function\|no such table\|no such column" "$RESULTS_DIR"/*.log 2>/dev/null | wc -l || echo "0")

    # Count opcode-related failures
    opcode_failures=$(grep -r "Callback\|ColumnName\|Recno" "$RESULTS_DIR"/*.log 2>/dev/null | wc -l || echo "0")

    # Count constraint failures
    constraint_failures=$(grep -r "constraint\|UNIQUE\|PRIMARY KEY" "$RESULTS_DIR"/*.log 2>/dev/null | wc -l || echo "0")

    cat >> "$REPORT_FILE" <<EOF
**Category A: API Changes**
- Count: $api_failures potential API-related failures
- Cause: Column name changes (zName → zCnName), function updates
- Fix: Update test expectations for new API

**Category B: Opcode Changes**
- Count: $opcode_failures potential opcode-related failures
- Cause: OP_Callback → OP_ResultRow, stack-based → register-based
- Fix: Update EXPLAIN output expectations

**Category C: Constraint Handling**
- Count: $constraint_failures potential constraint failures
- Cause: Enhanced constraint checking in SQLite 3.52.0
- Fix: Update test expectations or fix constraint bugs

EOF
fi

cat >> "$REPORT_FILE" <<EOF
---

## New Features Tested

### RETURNING Clause

**Test File**: \`returning.test\`
**Status**: $(if [ -f "$RESULTS_DIR/returning.test"*".log" ]; then echo "Executed"; else echo "Pending"; fi)
**Coverage**:
- INSERT RETURNING (single & multiple rows)
- UPDATE RETURNING (with/without matches)
- DELETE RETURNING (all scenarios)
- RETURNING * (all columns)
- RETURNING with expressions & functions

### Register-Based VDBE

**Test File**: \`register_vdbe.test\`
**Status**: $(if [ -f "$RESULTS_DIR/register_vdbe.test"*".log" ]; then echo "Executed"; else echo "Pending"; fi)
**Coverage**:
- OP_ResultRow validation
- Legacy opcode removal verification
- Register addressing tests
- EXPLAIN output validation

### Modernization Features

**Test File**: \`modernization.test\`
**Status**: $(if [ -f "$RESULTS_DIR/modernization.test"*".log" ]; then echo "Executed"; else echo "Pending"; fi)
**Coverage**:
- Modern SQL functions (math, string, aggregate)
- Type affinity rules (SQLite 3.x)
- Enhanced error messages
- Transaction semantics

---

## Test Coverage Assessment

### Core SQL Operations

EOF

# Check coverage for key areas
for area in "select" "insert" "update" "delete" "join" "where"; do
    count=$(find "$RESULTS_DIR" -name "${area}*.log" 2>/dev/null | wc -l || echo "0")
    if [ "$count" -gt 0 ]; then
        echo "- ✓ ${area^}: $count test files" >> "$REPORT_FILE"
    else
        echo "- ⚠ ${area^}: No tests found" >> "$REPORT_FILE"
    fi
done

cat >> "$REPORT_FILE" <<EOF

### Data Types & Expressions

EOF

for area in "expr" "func" "date" "type"; do
    count=$(find "$RESULTS_DIR" -name "*${area}*.log" 2>/dev/null | wc -l || echo "0")
    if [ "$count" -gt 0 ]; then
        echo "- ✓ ${area^}: $count test files" >> "$REPORT_FILE"
    else
        echo "- ⚠ ${area^}: No tests found" >> "$REPORT_FILE"
    fi
done

cat >> "$REPORT_FILE" <<EOF

### Transactions & Schema

EOF

for area in "trans" "index" "trigger" "view"; do
    count=$(find "$RESULTS_DIR" -name "*${area}*.log" 2>/dev/null | wc -l || echo "0")
    if [ "$count" -gt 0 ]; then
        echo "- ✓ ${area^}: $count test files" >> "$REPORT_FILE"
    else
        echo "- ⚠ ${area^}: No tests found" >> "$REPORT_FILE"
    fi
done

cat >> "$REPORT_FILE" <<EOF

---

## Recommendations

### High Priority

EOF

# Generate recommendations based on results
if (( $(echo "$pass_rate_num < 85" | bc -l) )); then
    cat >> "$REPORT_FILE" <<EOF
1. **Fix failing tests** - Pass rate is below target (85%)
   - Focus on Category A (API changes) and Category B (opcode changes)
   - Review failure logs in \`$RESULTS_DIR/\`

2. **Update test expectations** - Many failures likely due to modernization
   - Update EXPLAIN output patterns
   - Update column name access patterns

EOF
fi

cat >> "$REPORT_FILE" <<EOF
3. **Port additional SQLite tests** - Enhance coverage
   - types.test, types2.test, types3.test (type system)
   - with.test (Common Table Expressions)
   - Enhanced transaction tests

### Medium Priority

4. **Performance testing** - Establish baselines
   - Run speed.test if available
   - Benchmark key operations (INSERT, SELECT, UPDATE, DELETE)

5. **Edge case testing** - Improve robustness
   - Port boundary.test from SQLite
   - Add corruption detection tests

### Low Priority

6. **Documentation** - Keep tests maintainable
   - Document expected failures (BerkeleyDB differences)
   - Create test maintenance guide

---

## Files Generated

- **Full test log**: \`$RESULTS_DIR/test_run_*.log\`
- **Summary**: \`$SUMMARY_FILE\`
- **This report**: \`$REPORT_FILE\`
- **Individual test logs**: \`$RESULTS_DIR/*.log\`

---

## Next Steps

1. Review failure logs for actionable items
2. Fix high-priority test failures
3. Port additional SQLite tests as needed
4. Re-run test suite to verify fixes
5. Update this report with new results

---

**Report Generated**: $TIMESTAMP
**Total Test Files**: $total_files
**Pass Rate**: $pass_rate

EOF

echo "Report generated successfully: $REPORT_FILE"
cat "$REPORT_FILE"
