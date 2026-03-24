#!/bin/bash
# Fix naming violations in index_t structure
# Phase 2.2: index_t struct fields
set -euo pipefail

echo "Phase 2.2: Fixing index_t structure field names"
echo "==============================================="
echo ""

# First update the header file
echo "Step 1: Updating struct definition in dbsql_int.in"
sed -i 's/\ttable_t \*pTable;/\ttable_t *table;/g' /home/gburd/ws/dbsql/src/inc/dbsql_int.in
sed -i 's/\tindex_t \*pNext;/\tindex_t *next_index;/g' /home/gburd/ws/dbsql/src/inc/dbsql_int.in
sed -i 's/\tschema_t \*pSchema;.*\/\* Schema containing this index/\tschema_t *schema;         /* Schema containing this index/g' /home/gburd/ws/dbsql/src/inc/dbsql_int.in
sed -i 's/\texpr_t \*pPartIdxWhere;/\texpr_t *partial_index_where;/g' /home/gburd/ws/dbsql/src/inc/dbsql_int.in
sed -i 's/\texpr_list_t \*aColExpr;/\texpr_list_t *column_expressions;/g' /home/gburd/ws/dbsql/src/inc/dbsql_int.in

echo "  ✓ Header updated"

# Target files (excluding backup files)
FILES=$(find /home/gburd/ws/dbsql/src -name "*.c" -o -name "*.y" | \
        grep -v "\.bak$" | \
        grep -v "\.backup$" | \
        grep -v "_CONVERTED\.c$" | \
        grep -v "\.pre-fix$" | \
        grep -v "\.phase2\.bak$")

# Step 2: Fix index->pTable → index->table (but NOT pTable in other contexts)
echo "Step 2: Fixing index_t->pTable → index_t->table"
for f in $FILES; do
    if [ -f "$f" ]; then
        # Be careful: only change when preceded by index-like variable
        # Match patterns like: index->pTable, pIndex->pTable, pIdx->pTable
        sed -i 's/\(index->pTable\)/index->table/g' "$f"
        sed -i 's/\(pIndex->pTable\)/pIndex->table/g' "$f"
        sed -i 's/\(pIdx->pTable\)/pIdx->table/g' "$f"
        sed -i 's/\(idx->pTable\)/idx->table/g' "$f"
    fi
done
echo "  ✓ pTable → table"

# Step 3: Fix index->pNext → index->next_index
echo "Step 3: Fixing index_t->pNext → index_t->next_index"
for f in $FILES; do
    if [ -f "$f" ]; then
        # Only change pNext for index_t contexts
        sed -i 's/\(index->pNext\)/index->next_index/g' "$f"
        sed -i 's/\(pIndex->pNext\)/pIndex->next_index/g' "$f"
        sed -i 's/\(pIdx->pNext\)/pIdx->next_index/g' "$f"
        sed -i 's/\(idx->pNext\)/idx->next_index/g' "$f"
    fi
done
echo "  ✓ pNext → next_index"

# Step 4: Fix index->pSchema → index->schema
echo "Step 4: Fixing index_t->pSchema → index_t->schema"
for f in $FILES; do
    if [ -f "$f" ]; then
        sed -i 's/\(index->pSchema\)/index->schema/g' "$f"
        sed -i 's/\(pIndex->pSchema\)/pIndex->schema/g' "$f"
        sed -i 's/\(pIdx->pSchema\)/pIdx->schema/g' "$f"
        sed -i 's/\(idx->pSchema\)/idx->schema/g' "$f"
    fi
done
echo "  ✓ pSchema → schema"

echo ""
echo "Phase 2.2 Complete!"
echo "==================="
echo ""
echo "Next: Verify build with 'nix build'"
