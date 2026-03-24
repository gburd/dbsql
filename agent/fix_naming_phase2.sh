#!/bin/bash
set -euo pipefail

# DBSQL Naming Convention Fix - Phase 2: Struct Field Renames
# Reference: ~/.claude/plans/vast-wibbling-deer.md Section 1
# Branch: naming-fixes

echo "=== Phase 2: Critical Structure Field Renames ==="
echo "Starting naming convention fixes..."
echo ""

# Backup strategy: Skip .bak files
EXCLUDE_PATTERN='.*\.(bak|backup|pre-fix|phase2\.bak)$'

# Find all C source and header files (excluding backups)
find_src_files() {
    find src -type f \( -name "*.c" -o -name "*.h" -o -name "*.in" \) | \
        grep -Ev "$EXCLUDE_PATTERN"
}

echo "Step 1: Renaming parser_t->pVdbe to parser_t->vdbe"
echo "  Affected: Field definition in struct parser"
echo ""

# Fix the struct definition first
sed -i 's/^\(\s*\)vdbe_t \*pVdbe;/\1vdbe_t *vdbe;/' src/inc/dbsql_int.in

# Fix all references: parser->pVdbe becomes parser->vdbe
# Also handle: pParse->pVdbe, parse->pVdbe, p->pVdbe
find_src_files | while read file; do
    # Match: ->pVdbe (most common pattern)
    # This catches: parser->pVdbe, pParse->pVdbe, p->pVdbe, etc.
    sed -i 's/\(->pVdbe\b\)/->vdbe/g' "$file"
done

echo "  ✓ Renamed pVdbe to vdbe in parser_t struct"
echo ""

echo "Step 2: Checking for other naming violations..."
echo ""

# Check for remaining violations
echo "Remaining pointer prefix violations (pXxx fields):"
grep -r '\->p[A-Z]' src --include="*.c" --include="*.h" --include="*.in" | \
    grep -Ev "$EXCLUDE_PATTERN" | \
    grep -v '->parser' | \
    grep -v '->prev_vdbe' | \
    grep -v '->parent_frame' | \
    grep -v '->prepFlags' | \
    head -20 || echo "  None found (or too many to list)"

echo ""
echo "Remaining array prefix violations (aXxx fields):"
grep -r '\->a[A-Z]' src --include="*.c" --include="*.h" --include="*.in" | \
    grep -Ev "$EXCLUDE_PATTERN" | \
    head -20 || echo "  None found (or too many to list)"

echo ""
echo "=== Phase 2 fixes applied ==="
echo "Next: Test compilation with 'nix build' or 'make'"
