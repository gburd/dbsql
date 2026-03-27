#!/bin/bash
set -e
cd /home/gburd/ws/dbsql/build_unix.old
echo "=== Recompiling api.c ==="
./libtool --mode=compile gcc -c \
  -I. -I../src \
  -I/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/include \
  -D_GNU_SOURCE -D_REENTRANT -DCONFIG_TEST -g \
  ../src/api.c -o api.lo

echo "=== Relinking library ==="
./libtool --mode=link gcc -avoid-version \
  -rpath /usr/local/DBSQL.0.4/lib \
  -o libdbsql-0.4.la \
  *.lo \
  -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib -ldb-4.8 -lm

echo "=== Relinking dbsql binary ==="
./libtool --mode=link gcc -o dbsql \
  dbsql.lo libdbsql-0.4.la -lm

echo "=== Done ==="
ls -lh .libs/dbsql
