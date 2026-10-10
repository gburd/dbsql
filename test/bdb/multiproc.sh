#!/bin/sh
# test/bdb/multiproc.sh DBSQL [PROCS] [N]
#	Multi-process correctness.  PROCS shell processes each run N
#	increment transactions on one shared counter row and log one row of
#	their own per transaction, retrying on "database is locked" (the
#	busy timeout makes most conflicts wait instead).  At the end the
#	counter must equal the number of committed increments, every logged
#	row must be present, and integrity_check must pass: no lost update,
#	no torn result, no hang.
set -u
cli=$1
procs=${2:-8}
n=${3:-200}
w=$(mktemp -d "${TMPDIR:-/tmp}/dbsql-mp.XXXXXX")
trap 'rm -r -f "$w"' EXIT
db=$w/mp.db
"$cli" "$db" "CREATE TABLE c(id INTEGER PRIMARY KEY, n INTEGER);
    INSERT INTO c VALUES(1, 0);
    CREATE TABLE log(p INTEGER, k INTEGER, PRIMARY KEY(p, k));" || exit 1
p=1
while [ "$p" -le "$procs" ]; do
	awk -v p="$p" -v n="$n" 'BEGIN {
		print ".timeout 60000"
		for (k = 1; k <= n; k++) {
			print "BEGIN IMMEDIATE;"
			print "UPDATE c SET n = n + 1 WHERE id = 1;"
			printf "INSERT INTO log VALUES(%d, %d);\n", p, k
			print "COMMIT;"
		}
	}' | timeout 600 "$cli" -bail "$db" > "$w/out.$p" 2>&1 &
	p=$((p + 1))
done
wait
fail=0
for f in "$w"/out.*; do
	if [ -s "$f" ]; then
		echo "multiproc: $(basename "$f"): $(head -3 "$f")"
		fail=1
	fi
done
res=$("$cli" "$db" "PRAGMA integrity_check;
    SELECT n FROM c; SELECT count(*) FROM log;")
want=$((procs * n))
if [ "$res" != "$(printf 'ok\n%d\n%d' $want $want)" ]; then
	echo "multiproc: FAIL: want counter=log=$want, got:" $res
	fail=1
fi
[ "$fail" = 0 ] && echo "multiproc: ok ($procs processes x $n transactions)"
exit "$fail"
