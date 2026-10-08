#!/bin/sh -
# Build the libdb submodule (deps/libdb, pinned release) for dbsql.
#
#   sh deps/build_libdb.sh [configure-args...]
#
# Applies deps/patches/*.patch (idempotently), configures in
# deps/libdb/build_unix and builds.  dbsql's configure finds the result
# by default (--with-berkeleydb defaults to deps/libdb/build_unix).
set -e
here=$(cd "$(dirname "$0")" && pwd)
db=$here/libdb
[ -f "$db/dist/configure" ] || {
	echo "deps/libdb is empty: git submodule update --init deps/libdb" >&2
	exit 1
}
MAKE=${MAKE:-$(command -v gmake || echo make)}
for p in "$here"/patches/*.patch; do
	[ -f "$p" ] || continue
	if (cd "$db" && patch -p1 -R -s -f --dry-run < "$p") >/dev/null 2>&1; then
		echo "already applied: $(basename "$p")"
	else
		(cd "$db" && patch -p1 -s < "$p") && echo "applied: $(basename "$p")"
	fi
done
cd "$db/build_unix"
[ -f Makefile ] || ../dist/configure --enable-shared "$@"
$MAKE -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
ls .libs/libdb-*.so* >/dev/null 2>&1 || ls libdb-*.a >/dev/null
echo "libdb built in $db/build_unix: $(grep 'define	DB_VERSION_STRING' db.h)"
