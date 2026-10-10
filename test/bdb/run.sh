#!/bin/sh
# test/bdb/run.sh TESTFIXTURE DBSQL
#	dbsql-specific tests of what libdb changes: snapshot isolation and
#	writer exclusion (snapshot.test), SIGKILL then recovery (crash.sh),
#	concurrent writer processes (multiproc.sh).
set -u
here=$(cd "$(dirname "$0")" && pwd)
tf=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
cli=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
w=$(mktemp -d "${TMPDIR:-/tmp}/dbsql-bdb.XXXXXX")
trap 'rm -r -f "$w"' EXIT
fail=0
(cd "$w" && DBSQL_CLI=$cli "$tf" "$here/snapshot.test") || fail=1
sh "$here/crash.sh" "$cli" 10 || fail=1
sh "$here/multiproc.sh" "$cli" 8 200 || fail=1
[ $fail = 0 ] && echo "bdb: ok"
exit $fail
