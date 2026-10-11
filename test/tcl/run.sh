#!/bin/sh -
# test/tcl/run.sh TESTFIXTURE TOPDIR [JOBS]
#	Run SQLite 3.53.4's Tcl test suite on a dbsql testfixture.
#
#	The tests are not vendored: they are prepared from a SQLite source
#	tree (SQLITE_SRC, default $TOPDIR/../sqlite-3.53.4) with
#	dist/transpile/tests, which applies the same renames as the source
#	transform.  Every test file is run (dist/transpile/suite.sh, one
#	testfixture per file); a file listed in test/tcl/known-failures.txt
#	may fail, any other non-passing file fails the run, and a listed file
#	that now passes is reported so the list only shrinks.
set -eu
tf=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
top=$(cd "$2" && pwd)
jobs=${3:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
sqlite=${SQLITE_SRC:-$top/../sqlite-3.53.4}
here=$top/test/tcl
[ -d "$sqlite/test" ] || {
	echo "tcl: SQLite 3.53.4 source not found at $sqlite (set SQLITE_SRC)"
	exit 1
}
w=$(mktemp -d "${TMPDIR:-/tmp}/dbsql-tcl.XXXXXX")
trap 'rm -r -f "$w"' EXIT
# shell*.test run the CLI found next to the testfixture: give them a
# one without a libtool wrapper (dbsql-static, built with --enable-test).
mkdir "$w/bin"
cp "$tf" "$w/bin/testfixture"
cp "$(dirname "$tf")/dbsql-static" "$w/bin/dbsql"
tf=$w/bin/testfixture
mkdir "$w/tst"
python3 "$top/dist/transpile/tests" "$sqlite" "$w/tst/test" >/dev/null ||
	exit 1
# Per-test-case expected failures: cases that diverge by design (see
# known-problems.txt) are counted as "known errors" by the transpiled
# tester.tcl, so a file whose only failures are listed divergences PASSes
# here and its other cases are actually run and verified.
KP=$here/known-problems.txt
[ -f "$KP" ] && export KNOWN_PROBLEMS="$KP"
sh "$top/dist/transpile/suite.sh" "$tf" "$w/tst/test" "$w/results.txt" \
	"$jobs" >/dev/null
cp "$w/results.txt" "${RESULTS:-./tcl-results.txt}"
awk -F'\t' '
	FILENAME == ARGV[1] { if ($0 !~ /^#/ && NF) known[$1] = $2; next }
	{
		pass = ($2 ~ /^PASS/)
		if (pass && ($1 in known)) fixed[++nf] = $1
		else if (!pass && !($1 in known)) bad[++nb] = $1 "\t" $2
		total++; if (pass) np++
	}
	END {
		printf "tcl: %d of %d test files pass\n", np, total
		for (i = 1; i <= nf; i++)
			printf "tcl: now passes (remove from known-failures): %s\n", fixed[i]
		for (i = 1; i <= nb; i++)
			printf "tcl: UNEXPECTED: %s\n", bad[i]
		exit nb > 0
	}' "$here/known-failures.txt" "$w/results.txt"
