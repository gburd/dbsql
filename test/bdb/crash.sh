#!/bin/sh
# test/bdb/crash.sh DBSQL [ROUNDS]
#	Durability and recovery.  Each round a writer commits rows max+1,
#	max+2, ... (one autocommit statement each, printing the row number
#	after the commit returns) and is killed with SIGKILL at a random
#	moment.  The reopen runs libdb recovery; the database must then pass
#	integrity_check and hold exactly rows 1..n, all well-formed, with n
#	at least the last commit the writer reported: no torn rows and no
#	lost commits.
set -u
cli=$1
rounds=${2:-20}
w=$(mktemp -d "${TMPDIR:-/tmp}/dbsql-crash.XXXXXX")
trap 'rm -r -f "$w"' EXIT
db=$w/crash.db
"$cli" "$db" "CREATE TABLE t(i INTEGER PRIMARY KEY, v TEXT NOT NULL);
    CREATE INDEX tv ON t(v);" || exit 1
fail=0
mx=0
r=1
while [ "$r" -le "$rounds" ]; do
	awk -v s="$mx" 'BEGIN {
		for (i = s + 1; i <= s + 200000; i++)
			printf "INSERT INTO t VALUES(%d, hex(randomblob(40)));\nSELECT %d;\n", i, i
	}' | "$cli" "$db" > "$w/out" 2>&1 &
	pid=$!
	sleep "$(awk -v r="$r" 'BEGIN { srand(r); printf "%.2f", 0.2 + rand() }')"
	kill -9 "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	committed=$(grep -E '^[0-9]+$' "$w/out" | tail -1)
	committed=${committed:-$mx}
	res=$("$cli" "$db" "PRAGMA integrity_check;
	    SELECT count(*) || ' ' || coalesce(max(i), 0) FROM t;
	    SELECT count(*) FROM t WHERE length(v) <> 80;" 2>&1)
	ic=$(echo "$res" | sed -n 1p)
	set -- $(echo "$res" | sed -n 2p)
	n=${1:-x}
	mx=${2:-0}
	bad=$(echo "$res" | sed -n 3p)
	if [ "$ic" != ok ] || [ "$n" != "$mx" ] || [ "$bad" != 0 ] ||
	    [ "$mx" -lt "$committed" ]; then
		echo "crash: round $r FAIL: integrity=$ic rows=$n max=$mx" \
		    "malformed=$bad reported-committed=$committed"
		fail=1
	else
		echo "crash: round $r ok: rows=$n (reported committed $committed)"
	fi
	r=$((r + 1))
done
[ "$fail" = 0 ] && echo "crash: ok ($rounds rounds)"
exit "$fail"
