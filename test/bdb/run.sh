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
# Four concurrent writers, not eight: on FreeBSD the libdb DB_REGISTER
# recovery (R6) that peers run on each other's close gets pathological at
# high writer counts (a peer can block on the registry until its own
# timeout).  Four exercises multi-process correctness and the R6 path
# while completing promptly; multiproc.sh checks the durability invariant,
# not that every commit lands.
sh "$here/multiproc.sh" "$cli" 4 100 || fail=1
[ $fail = 0 ] && echo "bdb: ok"
exit $fail
