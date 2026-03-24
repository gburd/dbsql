#!/bin/bash
# Revert incorrect fk->columns back to fk->aCol
# The foreign_key structure's aCol field should NOT be renamed
set -euo pipefail

FILES=$(find /home/gburd/ws/dbsql/src -name "*.c" -o -name "*.h" -o -name "*.in" -o -name "*.y" | \
        grep -v "\.bak$" | \
        grep -v "\.backup$" | \
        grep -v "_CONVERTED\.c$" | \
        grep -v "\.pre-fix$" | \
        grep -v "\.phase2\.bak$")

echo "Reverting fk->columns back to fk->aCol"
echo "======================================"

for f in $FILES; do
    if [ -f "$f" ]; then
        # Revert fk->columns back to fk->aCol
        sed -i 's/\(fk->columns\)/fk->aCol/g' "$f"
        sed -i 's/\(fk\.columns\)/fk.aCol/g' "$f"
        # Also catch patterns like pFk->columns, etc.
        sed -i 's/\([[:alnum:]_]*[Ff][Kk]->columns\)/\1/g; s/->columns/->aCol/g' "$f"
    fi
done

# Also need to revert the struct definition
echo "Reverting struct definition in dbsql_int.in"
sed -i 's/} aCol\[FLEXARRAY\];/} aCol[FLEXARRAY];/g' /home/gburd/ws/dbsql/src/inc/dbsql_int.in

echo "✓ Reverted fk->aCol changes"
