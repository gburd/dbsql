#!/bin/bash
# Rebuild dbsql binary with all fixes

set -e

BUILD_DIR="/home/gburd/ws/dbsql/build_unix.old"
cd "$BUILD_DIR"

echo "=== Cleaning old object files ==="
rm -f dbsql.lo dbsql.o .libs/dbsql.o .libs/dbsql 2>/dev/null || true

echo "=== Recompiling dbsql.c ==="
./libtool --mode=compile gcc -c \
  -I. -I../src \
  -I/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/include \
  -D_GNU_SOURCE -D_REENTRANT -DCONFIG_TEST -g \
  ../src/dbsql/dbsql.c -o dbsql.lo

echo "=== Relinking dbsql binary ==="
./libtool --mode=link gcc -o dbsql \
  dbsql.lo libdbsql-0.4.la -lm

echo "=== Build complete ==="
ls -lh dbsql .libs/dbsql 2>/dev/null || true
