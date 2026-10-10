#!/bin/sh -
# test/fuzz/run.sh BUILD_ASAN SECONDS
#	Build and run the libFuzzer targets against the ASan+UBSan library
#	in BUILD_ASAN (a build_unix configured with -fsanitize=address,
#	undefined --disable-shared).  Each target runs for SECONDS with one
#	job per CPU; any crash, sanitizer report or leak fails the run.  The
#	SQL corpus is seeded from SQLite's test SQL if SQLITE_SRC is set.
set -u
b=$(cd "$1" && pwd)
secs=${2:-1800}
top=$(cd "$(dirname "$0")/../.." && pwd)
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
w=${FUZZ_DIR:-$b/fuzz}
mkdir -p "$w/corpus-sql" "$w/corpus-cmp" "$w/artifacts"
inc="-I$b -I$top/src -I$top/src/inc -I$top/src/sm $(sed -n 's/^DB_CPPFLAGS=[	 ]*//p' "$b/Makefile")"
libs="$b/.libs/libdbsql-1.0.a $(sed -n 's/^DB_LIBS=[	 ]*//p' "$b/Makefile") \
    $(sed -n 's/^ICU_LIBS=[	 ]*//p' "$b/Makefile") -lz -lm -lpthread -ldl"
flags="-g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
for t in sql cmp; do
	clang $flags $inc -DDBSQL_CORE -o "$w/${t}_fuzz" "$top/test/fuzz/${t}_fuzz.c" \
	    $libs || exit 1
done
if [ -n "${SQLITE_SRC:-}" ] && [ -z "$(ls "$w/corpus-sql")" ]; then
	# One seed per SQL block from the Tcl tests' execsql/do_execsql_test.
	python3 - "$SQLITE_SRC/test" "$w/corpus-sql" <<'PY'
import os, re, sys, hashlib
src, out = sys.argv[1], sys.argv[2]
for f in sorted(os.listdir(src)):
	if not f.endswith('.test'):
		continue
	t = open(os.path.join(src, f), errors='replace').read()
	for m in re.finditer(r'(?:execsql|catchsql|do_execsql_test\s+\S+|do_catchsql_test\s+\S+)\s*\{([^{}]{8,2000})\}', t):
		sql = re.sub(r'\bsqlite_', 'dbsql_', m.group(1)).encode()
		open(os.path.join(out, hashlib.sha1(sql).hexdigest()), 'wb').write(sql)
PY
fi
fail=0
# Known libdb leaks (reported, see lsan.supp) must not mask new ones.
# libdb is built without frame pointers: unwind leak stacks the slow way
# so a suppressed libdb frame is actually seen.
export LSAN_OPTIONS="suppressions=$top/test/fuzz/lsan.supp${LSAN_OPTIONS:+:$LSAN_OPTIONS}"
export ASAN_OPTIONS="fast_unwind_on_malloc=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
cd "$w/artifacts" || exit 1
for t in sql cmp; do
	echo "fuzz: $t for ${secs}s on $jobs jobs"
	"$w/${t}_fuzz" -max_total_time="$secs" -jobs="$jobs" -workers="$jobs" \
	    -max_len=8192 -timeout=30 -rss_limit_mb=4096 -report_slow_units=20 \
	    -artifact_prefix="$w/artifacts/${t}-" "$w/corpus-$t" \
	    > "$w/$t.log" 2>&1
	if ls "$w/artifacts/${t}-"* > /dev/null 2>&1; then
		echo "fuzz: $t: FAILED, artifacts in $w/artifacts"
		fail=1
	else
		echo "fuzz: $t: no crashes ($(grep -ho 'cov: [0-9]*' "$w"/artifacts/fuzz-*.log 2>/dev/null | tail -1))"
	fi
done
exit $fail
