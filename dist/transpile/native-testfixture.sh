#!/bin/sh
# dist/transpile/native-testfixture.sh OUT BUILD
#   Build a Tcl testfixture from the transformed tree + transformed native
#   storage layers (validation only).  Mirrors upstream main.mk's
#   TESTFIXTURE_FLAGS, transformed (SQLITE_ -> DBSQL_).
set -e
OUT=$(cd "$1" && pwd)
B=$2
sh "$(dirname "$0")/native-build.sh" "$OUT" "$B" >/dev/null || true
cd "$B"
CC=${CC:-cc}
if [ -z "${TCLINC:-}" ]; then
	for c in /usr/lib/tcl8.6/tclConfig.sh /usr/local/lib/tcl8.6/tclConfig.sh; do
		[ -f "$c" ] && { . "$c"; TCLINC=$TCL_INCLUDE_SPEC; TCLLIB=$TCL_LIB_SPEC; break; }
	done
fi
TCLINC=${TCLINC:--I/usr/local/include/tcl8.6}
TCLLIB=${TCLLIB:--L/usr/local/lib -ltcl86}
DEFS="-DDBSQL_TEST=1 -DDBSQL_CRASH_TEST=1 -DTCLSH_INIT_PROC=__test_init \
 -DDBSQL_SERVER=1 -DDBSQL_PRIVATE= -DDBSQL_CORE -DBUILD_dbsql \
 -DDBSQL_SERIES_CONSTRAINT_VERIFY=1 -DDBSQL_DEFAULT_PAGE_SIZE=1024 \
 -DDBSQL_ENABLE_STMTVTAB -DDBSQL_ENABLE_DBPAGE_VTAB -DDBSQL_ENABLE_BYTECODE_VTAB \
 -DDBSQL_ENABLE_CARRAY -DDBSQL_ENABLE_PERCENTILE -DDBSQL_STATIC_RANDOMJSON \
 -DDBSQL_STRICT_SUBTYPE=1 -DDBSQL_CKSUMVFS_STATIC -DDBSQL_NO_SYNC=1 -DDBSQL_ENABLE_FTS5 \
 -DDBSQL_ENABLE_MATH_FUNCTIONS -DDBSQL_THREADSAFE=1 -D_HAVE_DBSQL_CONFIG_H \
 -DDBSQL_DEBUG=1 -DDBSQL_ENABLE_EXPLAIN_COMMENTS -DDBSQL_HAVE_ZLIB=1"
INC="-I. -I$OUT/test/tcl/ext -I$OUT/src -I$OUT/src/inc -I$OUT/src/ext/fts5 -I$OUT/native -I$OUT/test/tcl $TCLINC"
CFLAGS=${CFLAGS:--O1 -g}
mkdir -p tf
SRCS=$(cd "$OUT" && ls src/*.c src/vdbe/*.c src/api/*.c src/common/*.c \
    src/os/*.c native/*.c src/ext/fts5/fts5.c test/tcl/*.c test/tcl/ext/*.c \
    | grep -v '/mem[0235]\.c$' | grep -v '/base[68][45]\.c$')
cat > tf_stubs.c <<'STUB'
/* Out-of-scope upstream test modules (RBU, recover, intck): dbsql does
** not ship these extensions; their Tcl init hooks are no-ops here. */
#include <tcl.h>
int Dbsqltestintck_Init(Tcl_Interp *i){ (void)i; return TCL_OK; }
int DbsqlRbu_Init(Tcl_Interp *i){ (void)i; return TCL_OK; }
int __test_recover_init(Tcl_Interp *i){ (void)i; return TCL_OK; }
STUB
fail=0
: > tf/errors.log
OBJS=
for s in $SRCS sql_parser.c tf_stubs.c; do
	case $s in sql_parser.c|tf_stubs.c) p=$(pwd)/$s ;; *) p=$OUT/$s ;; esac
	o=tf/$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')
	OBJS="$OBJS $o"
	if [ ! -f "$o" ] || [ "$p" -nt "$o" ]; then
		$CC $CFLAGS $DEFS $INC -c "$p" -o "$o" 2>>tf/errors.log || fail=$((fail+1))
	fi
done
echo "testfixture compile failures: $fail"
[ $fail -eq 0 ] || exit 1
$CC -o testfixture $OBJS $TCLLIB -lm -lpthread -lz 2>tf/link.log || { echo link failed; head -30 tf/link.log; exit 1; }
echo built $B/testfixture
