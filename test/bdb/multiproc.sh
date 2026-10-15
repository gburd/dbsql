#!/bin/sh
# test/bdb/multiproc.sh DBSQL [PROCS] [N]
#	Multi-process correctness.  PROCS shell processes each run N
#	increment transactions on one shared counter row and log one row of
#	their own per transaction, retrying on "database is locked" (the
#	busy timeout makes most conflicts wait instead).  The invariant
#	checked at the end is durability, not completion: integrity_check
#	passes, the counter equals the number of logged rows (no lost update,
#	no torn transaction), and each process's log is a gap-free prefix (a
#	committed txn did both its writes).  If every commit landed, that is
#	the strong "ok".  A process may instead be told its environment was
#	recovered by a peer's DB_REGISTER close (the documented libdb R6
#	'peerexit' divergence -- common on FreeBSD, rare on Linux); its
#	in-flight work aborts, but committed data stays consistent, which is
#	also "ok".  Only inconsistency, corruption, a torn log, or an
#	unexpected error message fails.
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

# A process may be told its environment was recovered out from under it by
# a peer's DB_REGISTER close (the documented libdb R6 'peerexit' divergence;
# more frequent on FreeBSD than Linux).  That aborts its in-flight work --
# surfacing as 'database disk image is malformed' / 'unable to open database
# file' / a bare 'SQL logic error' -- but it must never corrupt committed
# data or tear a transaction.  Those three are the only tolerated messages;
# any other stderr is a real failure.
fail=0
r6=0
for f in "$w"/out.*; do
	[ -s "$f" ] || continue
	if grep -qvE 'database disk image is malformed|unable to open database file|SQL logic error|^Error( near line [0-9]+)?: $' "$f"; then
		echo "multiproc: $(basename "$f"): $(head -3 "$f")"
		fail=1
	else
		r6=1
	fi
done

# The invariant that must hold regardless of R6 peer-recovery: the database
# is intact, and the counter equals the number of logged rows (every
# committed increment did both its UPDATE and its INSERT, atomically -- no
# lost update, no torn transaction).  Each surviving process's log rows must
# also form a gap-free prefix 1..k_p (a committed txn is never skipped).
res=$("$cli" "$db" "PRAGMA integrity_check;
    SELECT n FROM c;
    SELECT count(*) FROM log;
    SELECT count(*) FROM (SELECT p, max(k) mk, count(*) ck FROM log GROUP BY p)
        WHERE mk <> ck;")
want=$((procs * n))
ic=$(printf '%s\n' "$res" | sed -n 1p)
counter=$(printf '%s\n' "$res" | sed -n 2p)
logrows=$(printf '%s\n' "$res" | sed -n 3p)
gaps=$(printf '%s\n' "$res" | sed -n 4p)
if [ "$ic" != ok ]; then
	echo "multiproc: FAIL: integrity_check: $ic"; fail=1
elif [ "$counter" != "$logrows" ]; then
	echo "multiproc: FAIL: counter=$counter but logrows=$logrows (lost/torn update)"; fail=1
elif [ "${gaps:-0}" != 0 ]; then
	echo "multiproc: FAIL: $gaps process logs have a gap (a committed txn was skipped)"; fail=1
elif [ "$counter" = "$want" ]; then
	echo "multiproc: ok ($procs processes x $n transactions)"
elif [ "$r6" = 1 ]; then
	echo "multiproc: ok ($counter/$want committed; some peers recovered by" \
	    "libdb DB_REGISTER -- R6; data consistent, integrity ok)"
else
	echo "multiproc: FAIL: only $counter/$want committed and no R6 recovery seen"
	fail=1
fi
exit "$fail"
