#!/bin/sh
# sync laptop src/sm + dist to dbsql-deb:~/w/dbsql and rebuild the libdb testfixture
R="/run/current-system/sw/bin/rsync --no-times --checksum"
cd ~/ws/dbsql && (cd dist && sh s_include >/dev/null) &&
$R -rlp --exclude __pycache__ src/sm/ dbsql-deb:w/dbsql/src/sm/ &&
$R -rlp --exclude __pycache__ dist/ dbsql-deb:w/dbsql/dist/ &&
$R -rlp deps/patches/ deps/build_libdb.sh dbsql-deb:w/dbsql/deps/ 2>/dev/null; true &&
ssh dbsql-deb 'cd ~/w && CC=clang DBINC=$HOME/w/inst/libdb/include DBLIB=$HOME/w/inst/libdb/lib sh ~/w/dbsql/dist/transpile/libdb-testfixture.sh ~/w/tp/lo ~/w/'${1:-lb}' ~/w/dbsql/src/sm 2>&1 | tail -1; grep -E "error|warning:" ~/w/'${1:-lb}'/tf/errors.log | grep "/sm/" | head -5'
