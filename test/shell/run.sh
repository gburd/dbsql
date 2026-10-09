#!/bin/sh -
# test/shell/run.sh DBSQL
#	Run the dbsql shell smoke test: test/shell/smoke.sql against a fresh
#	database in a scratch directory, compared with smoke.expected.
set -e
here=$(cd "$(dirname "$0")" && pwd)
shell=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
d=$(mktemp -d "${TMPDIR:-/tmp}/dbsql-shell.XXXXXX")
trap 'rm -r -f "$d"' EXIT
cp "$here/t.csv" "$d/"
cd "$d"
"$shell" -bail test.db < "$here/smoke.sql" > out.txt 2>&1 || {
	echo "shell: dbsql exited with an error"; cat out.txt; exit 1; }
if diff -u "$here/smoke.expected" out.txt; then
	echo "shell: smoke ok"
else
	echo "shell: smoke output differs"
	exit 1
fi
