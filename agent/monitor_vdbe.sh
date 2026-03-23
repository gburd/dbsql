#!/bin/bash
# Monitor vdbe.o creation and notify when ready
#
# Usage: ./monitor_vdbe.sh
# Description: Checks every 5 minutes if vdbe.o exists in vdbe-phase3 worktree

set -euo pipefail

VDBE_PATH="/home/gburd/ws/dbsql-vdbe-phase3/build_unix/vdbe.o"
CHECK_INTERVAL=300  # 5 minutes

echo "=== VDBE Monitor Started ==="
echo "Checking: $VDBE_PATH"
echo "Interval: ${CHECK_INTERVAL}s (5 minutes)"
echo ""

while true; do
    if [ -f "$VDBE_PATH" ]; then
        echo ""
        echo "✅✅✅ vdbe.o EXISTS! ✅✅✅"
        echo "Size: $(ls -lh $VDBE_PATH | awk '{print $5}')"
        echo "Modified: $(ls -l $VDBE_PATH | awk '{print $6, $7, $8}')"
        echo ""
        echo "=== READY TO START UTILITY FILE FIXES ==="
        echo ""
        echo "Next steps:"
        echo "1. cd /home/gburd/ws/dbsql-utility-fixes"
        echo "2. Follow UTILITY_FILES_ANALYSIS.md execution plan"
        echo "3. Start with Phase 1 (Category A files)"
        echo ""
        echo "Quick start:"
        echo "  cd /home/gburd/ws/dbsql-utility-fixes/build_unix"
        echo "  make memcmp.lo strcasecmp.lo strdup.lo"
        break
    else
        TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
        echo "[$TIMESTAMP] ⏳ Waiting for vdbe.o..."

        # Show latest commit in vdbe-phase3
        if [ -d "/home/gburd/ws/dbsql-vdbe-phase3" ]; then
            cd /home/gburd/ws/dbsql-vdbe-phase3
            LATEST_COMMIT=$(git log --oneline -1 2>/dev/null || echo "No git log")
            echo "  Latest: $LATEST_COMMIT"

            # Show build status if available
            if [ -f build_unix/build_current.log ]; then
                ERROR_COUNT=$(grep -c "error:" build_unix/build_current.log 2>/dev/null || echo "0")
                if [ "$ERROR_COUNT" -gt 0 ]; then
                    LAST_ERROR=$(grep "error:" build_unix/build_current.log | tail -1)
                    echo "  Build: $ERROR_COUNT errors (latest: ${LAST_ERROR:0:80}...)"
                else
                    echo "  Build: No errors in log"
                fi
            fi
        fi

        sleep $CHECK_INTERVAL
    fi
done

echo ""
echo "=== Monitor Complete ==="
