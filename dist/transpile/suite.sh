#!/bin/sh
# dist/transpile/suite.sh TESTFIXTURE TESTDIR OUTFILE [JOBS]
#   Run every *.test in TESTDIR (one testfixture process per file, JOBS in
#   parallel, 900 s timeout each, each in a private scratch copy of the
#   directory) and write sorted "name<TAB>result" lines:
#     PASS n | FAIL nerr/n | CRASH | TIMEOUT | NORESULT
set -u
TF=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
TD=$(cd "$2" && pwd)
case $3 in /*) OUTF=$3 ;; *) OUTF=$(pwd)/$3 ;; esac
JOBS=${4:-28}
R=$(mktemp /tmp/run1.XXXXXX)
cat > "$R" <<'EOS'
#!/bin/sh
TF=$1; TD=$2; t=$3
d=$(mktemp -d /tmp/tf.XXXXXX)
# A private copy of the test directory (tests write next to themselves,
# e.g. through testdir/ and data files); ext/ is read-only and shared.
cp -R "$TD" "$d/test" || exit 1
[ -d "$TD/../ext" ] && ln -s "$TD/../ext" "$d/ext"
cd "$d/test" || exit 1
out=$(timeout 900 "$TF" "$t" --testdir="$d/t" 2>&1)
rc=$?
# Scratch copies are test output, not data: delete them (a trash can
# filled the disk once: 12k copies of the test directory).
cd / && rm -r -f -- "$d"
line=$(printf '%s\n' "$out" | grep -E '^[0-9]+ errors out of [0-9]+ tests' | tail -1)
if [ $rc -eq 124 ]; then r=TIMEOUT
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
	xargs -P "$JOBS" -n 1 "$R" "$TF" "$TD" > "$OUTF"
rm -f -- "$R"
sort -o "$OUTF" "$OUTF"
awk -F'\t' '{split($2,a," "); c[a[1]]++} END {for (k in c) printf "%s=%d ", k, c[k]; print ""}' "$OUTF"
