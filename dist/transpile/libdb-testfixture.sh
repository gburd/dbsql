#!/bin/sh
# dist/transpile/libdb-testfixture.sh OUT BUILD [SMDIR]
#   Build a Tcl testfixture from a transformed tree OUT (normally produced
#   with --keep-sql-names so SQLite's own tests run unmodified) on the
#   libdb storage manager (SMDIR, default ../../src/sm next to this
#   script).  This is the shipped storage configuration plus test hooks.
set -e
OUT=$(cd "$1" && pwd)
B=$2
SM=${3:-$(cd "$(dirname "$0")/../../src/sm" && pwd)}
DBINC=${DBINC:-$HOME/w/inst/libdb/include}
DBLIB=${DBLIB:-$HOME/w/inst/libdb/lib}
LEMON_SRC=${LEMON_SRC:-$(cd "$(dirname "$0")/../../src/lemon" && pwd)}
mkdir -p "$B/tf"
cd "$B"
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O1 -g}
LDFLAGS=${LDFLAGS:-}
TCLINC=${TCLINC:--I/usr/local/include/tcl8.6}
TCLLIB=${TCLLIB:--L/usr/local/lib -ltcl86}
DEFS="-DDBSQL_TEST=1 -DTCLSH_INIT_PROC=__test_init \
 -DDBSQL_SERVER=1 -DDBSQL_PRIVATE= -DDBSQL_CORE -DBUILD_dbsql \
 -DDBSQL_SERIES_CONSTRAINT_VERIFY=1 -DDBSQL_DEFAULT_PAGE_SIZE=1024 \
 -DDBSQL_ENABLE_STMTVTAB -DDBSQL_ENABLE_BYTECODE_VTAB \
 -DDBSQL_ENABLE_CARRAY -DDBSQL_ENABLE_PERCENTILE -DDBSQL_STATIC_RANDOMJSON \
 -DDBSQL_STRICT_SUBTYPE=1 -DDBSQL_NO_SYNC=1 -DDBSQL_ENABLE_FTS5 \
 -DDBSQL_ENABLE_MATH_FUNCTIONS -DDBSQL_THREADSAFE=1 -D_HAVE_DBSQL_CONFIG_H \
 -DDBSQL_DEBUG=1 -DDBSQL_ENABLE_EXPLAIN_COMMENTS -DDBSQL_CKSUMVFS_STATIC \
 -DDBSQL_OMIT_SHARED_CACHE -DDBSQL_OMIT_WAL -DDBSQL_OMIT_DESERIALIZE -DDBSQL_SM_LIBDB"
INC="-I. -I$SM -I$OUT/test/tcl/ext -I$OUT/src -I$OUT/src/inc \
 -I$OUT/src/ext/fts5 -I$OUT/test/tcl -I$DBINC $TCLINC"

cp "$OUT/src/inc/dbsql_api_ref.h" dbsql.h
: > dbsql_config.h
[ -x lemon ] || $CC -O2 -o lemon "$LEMON_SRC/lemon.c"
cp "$LEMON_SRC/lempar.c" .
for y in "$OUT/src/sql_parser.y" "$OUT/src/ext/fts5/fts5_parser.y"; do
	n=$(basename "$y" .y)
	if [ ! -f "$n.c" ] || [ "$y" -nt "$n.c" ]; then
		cp "$y" . && ./lemon -S "$n.y" >/dev/null
	fi
done
sed -e 's/yy/fts5yy/g' -e 's/YY/fts5YY/g' -e 's/TOKEN/FTS5TOKEN/g' \
    fts5_parser.c > fts5_parser.c.tmp && mv fts5_parser.c.tmp fts5_parser.c

cat > tf_stubs.c <<'STUB'
/* Test modules dbsql does not ship (RBU, recover, intck, and the b-tree
** internals tests test3/test_btree): no-op Tcl inits. */
#include <tcl.h>
int Dbsqltestintck_Init(Tcl_Interp *i){ (void)i; return TCL_OK; }
int DbsqlRbu_Init(Tcl_Interp *i){ (void)i; return TCL_OK; }
int __test_recover_init(Tcl_Interp *i){ (void)i; return TCL_OK; }
int Dbsqltest3_Init(Tcl_Interp *i){ (void)i; return TCL_OK; }
STUB

SRCS=$(cd "$OUT" && ls src/*.c src/vdbe/*.c src/api/*.c src/common/*.c \
    src/os/*.c src/ext/fts5/fts5.c test/tcl/*.c test/tcl/ext/*.c \
    | grep -vE '/mem[035]\.c$|/base[68][45]\.c$|/api/(backup|dbstat)\.c$' \
    | grep -vE '^test/tcl/(test3|test_btree)\.c$|/api/notify\.c$')
fail=0
: > tf/errors.log
OBJS=
for s in $SRCS $(cd "$SM" && ls *.c | sed 's|^|SM/|') sql_parser.c tf_stubs.c
do
	case $s in
	sql_parser.c|tf_stubs.c) p=$(pwd)/$s ;;
	SM/*) p=$SM/${s#SM/} ;;
	*) p=$OUT/$s ;;
	esac
	o=tf/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')
	OBJS="$OBJS $o"
	if [ ! -f "$o" ] || [ "$p" -nt "$o" ]; then
		$CC $CFLAGS $DEFS $INC -c "$p" -o "$o" 2>>tf/errors.log ||
		    fail=$((fail+1))
	fi
done
echo "testfixture compile failures: $fail"
[ $fail -eq 0 ] || exit 1
$CC $LDFLAGS -o testfixture $OBJS -L"$DBLIB" -ldb-2026.0 -Wl,-rpath,"$DBLIB" \
    $TCLLIB -lm -lpthread -lz 2>tf/link.log ||
    { echo link failed; grep -E "undefined|error" tf/link.log | sort -u | head -40; exit 1; }
echo "built $B/testfixture"
