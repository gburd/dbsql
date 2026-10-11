#!/bin/sh
# dist/transpile/suite.sh TESTFIXTURE TESTDIR OUTFILE [JOBS]
#   Run every *.test in TESTDIR (one testfixture process per file, JOBS in
#   parallel, 900 s timeout each, each in a private scratch copy of the
#   directory) and write sorted "name<TAB>result" lines:
#     PASS n | FAIL nerr/n | CRASH | TIMEOUT | NORESULT
#
#   If KNOWN_PROBLEMS is set to a file of divergent case names (whitespace
#   separated; see test/tcl/known-problems.txt), it is placed in each test's
#   testdir as known-problems.txt.  The transpiled tester.tcl then counts
#   those cases as "known errors" rather than new errors, so a file whose
#   only failures are listed divergences reports "0 new errors and K known
#   errors out of N tests" and PASSes here -- which means its other N-K
#   cases are actually run and verified, instead of the whole file skipped.
set -u
TF=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
TD=$(cd "$2" && pwd)
case $3 in /*) OUTF=$3 ;; *) OUTF=$(pwd)/$3 ;; esac
JOBS=${4:-28}
KP=${KNOWN_PROBLEMS:-}
[ -n "$KP" ] && KP=$(cd "$(dirname "$KP")" && pwd)/$(basename "$KP")
R=$(mktemp /tmp/run1.XXXXXX)
cat > "$R" <<'EOS'
#!/bin/sh
TF=$1; TD=$2; KP=$3; t=$4
d=$(mktemp -d /tmp/tf.XXXXXX)
# A private copy of the test directory (tests write next to themselves,
# e.g. through testdir/ and data files); ext/ is read-only and shared.
cp -R "$TD" "$d/test" || exit 1
[ -d "$TD/../ext" ] && ln -s "$TD/../ext" "$d/ext"
# tester.tcl cd's into --testdir and reads known-problems.txt from there.
mkdir -p "$d/t"
[ -n "$KP" ] && [ -f "$KP" ] && cp "$KP" "$d/t/known-problems.txt"
cd "$d/test" || exit 1
out=$(timeout 900 "$TF" "$t" --testdir="$d/t" 2>&1)
rc=$?
# Scratch copies are test output, not data: delete them (a trash can
# filled the disk once: 12k copies of the test directory).
cd / && rm -r -f -- "$d"
known=$(printf '%s\n' "$out" | grep -E '^[0-9]+ new errors and [0-9]+ known errors out of [0-9]+ tests' | tail -1)
line=$(printf '%s\n' "$out" | grep -E '^[0-9]+ errors out of [0-9]+ tests' | tail -1)
if [ $rc -eq 124 ]; then r=TIMEOUT
elif [ -n "$known" ]; then
	e=$(echo "$known" | awk '{print $1}'); n=$(echo "$known" | awk '{print $10}')
	k=$(echo "$known" | awk '{print $5}')
	if [ "$e" = 0 ]; then r="PASS $n ($k known)"; else r="FAIL $e/$n ($k known)"; fi
elif [ -n "$line" ]; then
	e=$(echo "$line" | awk '{print $1}'); n=$(echo "$line" | awk '{print $5}')
	if [ "$e" = 0 ]; then r="PASS $n"; else r="FAIL $e/$n"; fi
elif [ $rc -ge 128 ]; then r=CRASH
else r=NORESULT; fi
printf '%s\t%s\n' "${t%.test}" "$r"
EOS
chmod +x "$R"
cd "$TD"
ls *.test | grep -vE '^(all|veryquick|quick|full|extraquick|permutations|testrunner|soak|mallocAll|main)\.test$' |
	xargs -P "$JOBS" -n 1 "$R" "$TF" "$TD" "$KP" > "$OUTF"
rm -f -- "$R"
sort -o "$OUTF" "$OUTF"
awk -F'\t' '{split($2,a," "); c[a[1]]++} END {for (k in c) printf "%s=%d ", k, c[k]; print ""}' "$OUTF"
