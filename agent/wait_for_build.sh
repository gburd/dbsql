#!/usr/bin/env bash
#
# wait_for_build.sh - Monitor for build completion and auto-start tests
#
# This script monitors the ../dbsql-utility-fixes worktree for build completion.
# When the dbsql executable is detected, it automatically starts test execution.
#

set -euo pipefail

# Configuration
UTILITY_FIXES_DIR="../dbsql-utility-fixes"
BUILD_DIR="${UTILITY_FIXES_DIR}/build_unix"
DBSQL_EXEC="${BUILD_DIR}/dbsql"
CHECK_INTERVAL=600  # 10 minutes
MAX_WAIT_TIME=259200  # 72 hours (3 days)

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Function to print timestamped messages
log() {
    echo -e "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
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

# Check if utility-fixes directory exists
if [ ! -d "$UTILITY_FIXES_DIR" ]; then
    log_error "ERROR: Utility-fixes directory not found: $UTILITY_FIXES_DIR"
    log_error "Expected worktree at: $(cd .. && pwd)/dbsql-utility-fixes"
    exit 1
fi

log_info "===================================================================="
log_info "DBSQL Build Monitor - Waiting for compilation to complete"
log_info "===================================================================="
log_info "Monitoring: $DBSQL_EXEC"
log_info "Check interval: ${CHECK_INTERVAL}s ($(($CHECK_INTERVAL / 60)) minutes)"
log_info "Maximum wait time: $(($MAX_WAIT_TIME / 3600)) hours"
log_info ""

start_time=$(date +%s)
check_count=0

while true; do
    check_count=$((check_count + 1))
    current_time=$(date +%s)
    elapsed=$((current_time - start_time))

    log_info "Check #${check_count}: Elapsed time: $((elapsed / 3600))h $((($elapsed % 3600) / 60))m"

    # Check for executable
    if [ -f "$DBSQL_EXEC" ]; then
        log_success "✓ Build COMPLETE! dbsql executable found!"
        log_info ""
        log_info "Executable details:"
        ls -lh "$DBSQL_EXEC"
        log_info ""

        # Quick smoke test
        log_info "Running smoke test..."
        if "$DBSQL_EXEC" :memory: "SELECT 1;" 2>&1 | grep -q "1"; then
            log_success "✓ Smoke test PASSED!"
        else
            log_error "⚠ Smoke test FAILED, but continuing..."
        fi

        log_info ""
        log_success "===================================================================="
        log_success "BUILD COMPLETE - Starting test execution"
        log_success "===================================================================="

        # Run tests
        exec ./run_all_tests.sh
        exit 0
    fi

    # Check for shared library (alternative completion indicator)
    if [ -f "${BUILD_DIR}/libdbsql"*.so ]; then
        log_success "✓ Shared library found, but executable not yet built"
        log_info "Continuing to wait for dbsql executable..."
    fi

    # Check if we've exceeded maximum wait time
    if [ $elapsed -gt $MAX_WAIT_TIME ]; then
        log_error "ERROR: Maximum wait time exceeded ($((MAX_WAIT_TIME / 3600)) hours)"
        log_error "Build may have failed or stalled."
        log_error "Check build logs in: $BUILD_DIR/"
        exit 1
    fi

    # Log status update
    if [ $((check_count % 6)) -eq 0 ]; then
        log_info "Still waiting for build completion..."
        log_info "Time remaining before timeout: $(( (MAX_WAIT_TIME - elapsed) / 3600 ))h"
    fi

    # Wait before next check
    sleep $CHECK_INTERVAL
done
