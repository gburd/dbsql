#!/bin/bash
set -e

BUILD_DIR="/home/gburd/ws/dbsql/build_unix.old"
cd "$BUILD_DIR"

echo "=== Compiling api.c ==="
gcc -c -I. -I../src -I/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/include \
  -D_GNU_SOURCE -D_REENTRANT -DCONFIG_TEST -g \
  -fPIC -DPIC \
  ../src/api.c -o .libs/api.o

echo "=== Compiling dbsql.c ==="
gcc -c -I. -I../src -I/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/include \
  -D_GNU_SOURCE -D_REENTRANT -DCONFIG_TEST -g \
  -fPIC -DPIC \
  ../src/dbsql/dbsql.c -o .libs/dbsql.o

echo "=== Updating library ==="
cd .libs
ar cr libdbsql-0.4.a *.o 2>&1 | grep -v ".*" || true
ranlib libdbsql-0.4.a

echo "=== Relinking dbsql binary ==="
gcc -o dbsql dbsql.o -L. -ldbsql-0.4 \
  -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib -ldb-4.8 -lm \
  -Wl,-rpath,/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib

echo "=== Success ==="
ls -lh dbsql
LD_LIBRARY_PATH=/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib ./dbsql --version
