#!/bin/bash
set -e

BUILD_DIR="/home/gburd/ws/dbsql/build_unix.old"
cd "$BUILD_DIR"

echo "=== Full clean ==="
make clean 2>/dev/null || true
rm -f *.lo *.o .libs/*.o .libs/*.so* dbsql lemon 2>/dev/null || true
rm -f opcodes.c opcodes.h sql_parser.c sql_parser.h 2>/dev/null || true

echo "=== Rebuild opcodes.h ==="
make opcodes.h

echo "=== Rebuild sql_parser ==="
make sql_parser.h

echo "=== Rebuild library ==="
make libdbsql-0.4.la

echo "=== Rebuild dbsql binary ==="
make dbsql

echo "=== Success ==="
ls -lh .libs/dbsql
./dbsql --version
