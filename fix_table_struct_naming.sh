#!/bin/bash
# Fix naming violations in table_t structure
# Phase 2.1: table_t struct fields
set -euo pipefail

# Target files (excluding backup files)
FILES=$(find /home/gburd/ws/dbsql/src -name "*.c" -o -name "*.h" -o -name "*.in" -o -name "*.y" | \
        grep -v "\.bak$" | \
        grep -v "\.backup$" | \
        grep -v "_CONVERTED\.c$" | \
        grep -v "\.pre-fix$" | \
        grep -v "\.phase2\.bak$")

echo "Phase 2.1: Fixing table_t structure field names"
echo "=============================================="
echo ""

# Step 1: Fix table_t->aCol → table_t->columns
echo "Step 1: aCol → columns"
for f in $FILES; do
    if [ -f "$f" ]; then
        # Match pattern: ->aCol (preceded by any identifier or closing bracket/paren)
        sed -i 's/\(->aCol\)\b/->columns/g' "$f"
        sed -i 's/\(\.aCol\)\b/.columns/g' "$f"
    fi
done
echo "  ✓ aCol → columns"

# Step 2: Fix table_t->pIndex → table_t->first_index
echo "Step 2: pIndex → first_index"
for f in $FILES; do
    if [ -f "$f" ]; then
        sed -i 's/\(->pIndex\)\b/->first_index/g' "$f"
        sed -i 's/\(\.pIndex\)\b/.first_index/g' "$f"
    fi
done
echo "  ✓ pIndex → first_index"

# Step 3: Fix table_t->pTrigger → table_t->first_trigger
echo "Step 3: pTrigger → first_trigger"
for f in $FILES; do
    if [ -f "$f" ]; then
        sed -i 's/\(->pTrigger\)\b/->first_trigger/g' "$f"
        sed -i 's/\(\.pTrigger\)\b/.first_trigger/g' "$f"
    fi
done
echo "  ✓ pTrigger → first_trigger"

# Step 4: Fix table_t->pCheck → table_t->check_constraints
echo "Step 4: pCheck → check_constraints"
for f in $FILES; do
    if [ -f "$f" ]; then
        sed -i 's/\(->pCheck\)\b/->check_constraints/g' "$f"
        sed -i 's/\(\.pCheck\)\b/.check_constraints/g' "$f"
    fi
done
echo "  ✓ pCheck → check_constraints"

# Step 5: Fix table_t->pFKey (both the union field and the legacy alias)
echo "Step 5: pFKey → foreign_keys (in u.tab.pFKey)"
for f in $FILES; do
    if [ -f "$f" ]; then
        # This is tricky - we have both u.tab.pFKey and the legacy alias
        # Let's be conservative and only change the context-specific one
        sed -i 's/\(\.tab\.pFKey\)\b/.tab.foreign_keys/g' "$f"
    fi
done
echo "  ✓ u.tab.pFKey → u.tab.foreign_keys"

echo ""
echo "Phase 2.1 Complete!"
echo "==================="
echo ""
echo "Next: Update header file struct definitions"
