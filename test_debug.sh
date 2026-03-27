#!/bin/bash
# Debug script to identify where the crash occurs

set -e

DBSQL="/home/gburd/ws/dbsql/build_unix.old/.libs/dbsql"
export LD_LIBRARY_PATH="/home/gburd/ws/dbsql/build_unix.old/.libs"

echo "=== Test 1: Just version check (already works) ==="
$DBSQL --version && echo "SUCCESS" || echo "FAILED"

echo ""
echo "=== Test 2: Help (should work - exits early) ==="
$DBSQL --help 2>&1 | head -5 && echo "SUCCESS" || echo "FAILED"

echo ""
echo "=== Test 3: Open database with no SQL input ==="
echo "" | timeout 2 $DBSQL /home/gburd/ws/dbsql/build_unix.old/test.db 2>&1 && echo "SUCCESS" || echo "FAILED with exit code $?"

echo ""
echo "=== Test 4: Try CREATE TABLE ==="
echo "CREATE TABLE test (id INTEGER);" | timeout 2 $DBSQL /home/gburd/ws/dbsql/build_unix.old/test.db 2>&1 || echo "FAILED with exit code $?"

echo ""
echo "=== DONE ==="
