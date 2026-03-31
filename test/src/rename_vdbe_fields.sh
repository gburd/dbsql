#!/bin/bash
#
# Phase 2: Rename VdbeCursor struct fields to follow snake_case convention
# This script performs mechanical renames across the codebase

set -euo pipefail

REPO_ROOT="/home/gburd/ws/dbsql"
cd "$REPO_ROOT"

echo "=== Phase 2: VdbeCursor Field Renames ==="
echo ""

# Create backup
echo "Creating backup of vdbe_int.h..."
cp src/inc/vdbe_int.h src/inc/vdbe_int.h.backup

# Step 1: Rename VdbeCursor struct fields in header
echo "Step 1: Renaming VdbeCursor fields in vdbe_int.h..."

sed -i 's/\bpAltCursor\b/alt_cursor/g' src/inc/vdbe_int.h
sed -i 's/\bpCursor\b/sm_cursor/g' src/inc/vdbe_int.h
sed -i 's/\bpSorter\b/sorter/g' src/inc/vdbe_int.h
sed -i 's/\bpKeyInfo\b/key_info/g' src/inc/vdbe_int.h
sed -i 's/\bpCache\b/cache/g' src/inc/vdbe_int.h
sed -i 's/\baOffset\b/offsets/g' src/inc/vdbe_int.h
sed -i 's/\baRow\b/row_data/g' src/inc/vdbe_int.h
sed -i 's/\baType\b/type_array/g' src/inc/vdbe_int.h
sed -i 's/\baAltMap\b/alt_map/g' src/inc/vdbe_int.h
sed -i 's/\bpBtx\b/btree/g' src/inc/vdbe_int.h

echo "Step 2: Updating references in source files..."

# Update all source files
for file in src/*.c src/inc/*.h src/inc/*.in; do
    if [ -f "$file" ]; then
        echo "  Processing $file..."
        sed -i 's/\bpAltCursor\b/alt_cursor/g' "$file"
        sed -i 's/\bpCursor\b/sm_cursor/g' "$file"
        sed -i 's/\bpSorter\b/sorter/g' "$file"
        sed -i 's/\bpKeyInfo\b/key_info/g' "$file"
        sed -i 's/\bpCache\b/cache/g' "$file"
        sed -i 's/\baOffset\b/offsets/g' "$file"
        sed -i 's/\baRow\b/row_data/g' "$file"
        sed -i 's/\baType\b/type_array/g' "$file"
        sed -i 's/\baAltMap\b/alt_map/g' "$file"
        sed -i 's/\bpBtx\b/btree/g' "$file"
    fi
done

echo ""
echo "Step 3: Test compilation..."
cd build_unix.old
if make > /tmp/rename_compile.log 2>&1; then
    echo "  ✓ Compilation successful"
else
    echo "  ✗ Compilation failed - check /tmp/rename_compile.log"
    echo "  Restoring backup..."
    cp "$REPO_ROOT/src/inc/vdbe_int.h.backup" "$REPO_ROOT/src/inc/vdbe_int.h"
    exit 1
fi

echo ""
echo "=== Phase 2 Complete ==="
echo "Review changes with: git diff src/inc/vdbe_int.h"
