#!/bin/bash
set -euo pipefail

# Integration Agent Monitoring Script
# Waits for all agent dependencies, then performs final integration

LOG_FILE="/home/gburd/ws/dbsql/integration_monitor.log"
CHECK_INTERVAL=300  # 5 minutes

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

check_completion() {
    local utility_complete=false
    local test_complete=false
    
    if [ -f "/home/gburd/ws/dbsql-utility-fixes/UTILITY_FIXES_COMPLETE.md" ]; then
        utility_complete=true
    fi
    
    if [ -f "/home/gburd/ws/dbsql-test-suite/TEST_EXECUTION_COMPLETE.md" ]; then
        test_complete=true
    fi
    
    if $utility_complete && $test_complete; then
        return 0
    else
        log "Status: Utility=$utility_complete, Tests=$test_complete"
        return 1
    fi
}

log "=== Integration Agent Monitor Started ==="
log "Waiting for completion markers..."
log "  - /home/gburd/ws/dbsql-utility-fixes/UTILITY_FIXES_COMPLETE.md"
log "  - /home/gburd/ws/dbsql-test-suite/TEST_EXECUTION_COMPLETE.md"

while true; do
    if check_completion; then
        log "✅ All dependencies complete! Starting integration..."
        break
    fi
    
    log "⏳ Waiting $CHECK_INTERVAL seconds before next check..."
    sleep $CHECK_INTERVAL
done

log "=== Proceeding with integration ==="
log "Trigger integration by creating marker file..."
echo "$(date)" > /home/gburd/ws/dbsql/READY_FOR_INTEGRATION.marker

log "✅ Integration can now proceed"
log "Run: git checkout feature/sqlite-3.52-modernization"
