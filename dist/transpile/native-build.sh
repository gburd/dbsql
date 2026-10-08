#!/bin/sh
# dist/transpile/native-build.sh -- compile the transformed tree against
# SQLite's own (transformed) storage layers.  Validation only: proves the
# transform preserves semantics.  Usage: native-build.sh OUTDIR BUILDDIR
set -e
OUT=$(cd "$1" && pwd)
B=$2
mkdir -p "$B"
cd "$B"
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O1 -g}
DEFS="-DDBSQL_ENABLE_FTS5 -DDBSQL_ENABLE_MATH_FUNCTIONS -DDBSQL_ENABLE_PERCENTILE -DDBSQL_THREADSAFE=1 -DDBSQL_CORE -D_HAVE_DBSQL_CONFIG_H -DDBSQL_ENABLE_CARRAY"
INC="-I. -I$OUT/src -I$OUT/src/inc -I$OUT/src/ext/fts5 -I$OUT/native"

# dbsql.h for the validation build is the transformed upstream sqlite3.h.
cp "$OUT/src/inc/dbsql_api_ref.h" dbsql.h
: > dbsql_config.h
cat > dbsql_config.h <<'EOF'
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_USLEEP 1
#define HAVE_LOCALTIME_R 1
#define HAVE_GMTIME_R 1
#define HAVE_ISNAN 1
#define HAVE_MALLOC_USABLE_SIZE 0
#define HAVE_STRCHRNUL 1
#define HAVE_FDATASYNC 1
#define HAVE_PREAD 1
#define HAVE_PWRITE 1
EOF

# Parser: vendored upstream lemon on the transformed grammar.
LEMON_SRC=${LEMON_SRC:-$HOME/src/sqlite-3.53.4/tool}
[ -x lemon ] || $CC -O2 -o lemon "$LEMON_SRC/lemon.c"
cp "$LEMON_SRC/lempar.c" .
cp "$OUT/src/sql_parser.y" .
./lemon -S sql_parser.y >/dev/null
cp "$OUT/src/ext/fts5/fts5_parser.y" .
./lemon -S fts5_parser.y >/dev/null
# upstream mkfts5c.tcl renames the lemon yy* names in the fts5 parser so
# they cannot clash with the SQL parser's when amalgamated.
sed -e 's/yy/fts5yy/g' -e 's/YY/fts5YY/g' -e 's/TOKEN/FTS5TOKEN/g' \
    fts5_parser.c > fts5_parser.c.tmp && mv fts5_parser.c.tmp fts5_parser.c

SRCS=$(cd "$OUT" && ls src/*.c src/vdbe/*.c src/api/*.c src/common/*.c \
    src/os/*.c native/*.c src/ext/fts5/fts5.c | grep -v '/mem[0235]\.c$')
OBJS=
fail=0
for s in $SRCS sql_parser.c; do
	case $s in sql_parser.c) p=$s ;; *) p=$OUT/$s ;; esac
	o=$(echo "$s" | tr '/' '_' | sed 's/\.c$/.o/')
	OBJS="$OBJS $o"
	if [ ! -f "$o" ] || [ "$p" -nt "$o" ]; then
		$CC $CFLAGS $DEFS $INC -c "$p" -o "$o" 2>>errors.log || fail=$((fail+1))
	fi
done
echo "compile failures: $fail"
[ $fail -eq 0 ] || exit 1
ar rcs libdbsql_native.a $OBJS
echo "built libdbsql_native.a"
