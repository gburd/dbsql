#!/bin/bash
set -euo pipefail

# DBSQL Naming Convention Fix - Phase 2 (Careful version)
# Only renames parser_t->pVdbe to parser_t->vdbe
# Does NOT rename DBSQL->pVdbe (that's in a different struct)

echo "=== Phase 2: Careful parser_t->pVdbe rename ==="

# Step 1: Fix the struct definition
echo "Step 1: Updating struct parser definition..."
sed -i 's/^\(\s*\)vdbe_t \*pVdbe;\(\s*\/\* Engine for executing database bytecode \*\/\)/\1vdbe_t *vdbe;\2/' src/inc/dbsql_int.in

# Step 2: Fix references in a context-aware manner
# We need to change parser->pVdbe, pParse->pVdbe, parse->pVdbe, p->pVdbe (when p is a parser)
# But NOT dbp->pVdbe, db->pVdbe, vm->db->pVdbe

echo "Step 2: Updating parser_t->pVdbe references..."

# Find all C files (excluding backups)
find src -type f \( -name "*.c" -o -name "*.h" \) | \
    grep -Ev '\.(bak|backup|pre-fix|phase2\.bak)$' | \
while read file; do
    # Change parser->pVdbe, pParse->pVdbe, parse->pVdbe
    sed -i \
        -e 's/\bparser\.pVdbe\b/parser.vdbe/g' \
        -e 's/\bpParse->pVdbe\b/pParse->vdbe/g' \
        -e 's/\bparse->pVdbe\b/parse->vdbe/g' \
        "$file"
done

# Step 3: Fix the comment reference in vdbe_int.h
sed -i 's/TRUE after OP_ColumnName issued to pVdbe/TRUE after OP_ColumnName issued to vdbe/' src/inc/dbsql_int.in

echo ""
echo "✓ Phase 2 fixes applied (careful version)"
echo ""
echo "Summary:"
echo "  - Changed parser_t->pVdbe to parser_t->vdbe"
echo "  - Preserved DBSQL->pVdbe (different struct)"
echo ""
echo "Next: Test compilation with 'nix build'"
