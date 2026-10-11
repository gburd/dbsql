#!/bin/sh
# dist/transpile/classify.sh TESTFIXTURE TESTDIR TEST
#	Print "TEST<TAB>category<TAB>first failure" for one SQLite test file:
#	the category comes from the storage interfaces the test file uses
#	(file format, journal/WAL, test VFSes, memdb, pager, lock_status) and
#	is input to dist/transpile/mk-known-failures, which applies its own
#	traced overrides.  Run over every non-passing file, e.g.
#	  xargs -P 24 -I{} sh classify.sh TF TESTDIR {} < names | sort
tf=$1; td=$2; t=$3
f=$td/$t.test
d=$(mktemp -d /tmp/cls.XXXXXX)
cp -R "$td" "$d/test"; ln -s "$td/../ext" "$d/ext"
cd "$d/test"
out=$(timeout 300 "$tf" "$t.test" 2>&1)
cd /; rm -r -f "$d"
first=$(printf "%s\n" "$out" | grep -m1 -E "^! .* got:|^Error:|Assertion|dumped core|^[^ ]*testfixture: " | cut -c1-160 | tr '\t' ' ')
uses() { grep -qE -e "$1" "$f"; }
if uses 'hexio_(write|read)|sqlite_dbpage|dbsql_dbpage|db_page|file_pages|btree_pager_stats'; then c=fileformat
elif uses '-journal|journal_mode|jrnlmode|journal_size_limit|-wal\b|_wal\b|wal_|-shm\b|locking_mode'; then c=journal
elif uses 'testvfs|tvfs|crashsql|io_error|diskfull|faultsim|do_faultsim_test|test_syscall|multiplex|quota|devsym|jt_|cksumvfs|test_vfs|simulated'; then c=vfs
elif uses 'memdb|deserialize|serialize'; then c=memdb
elif uses 'dbstat|page_count|freelist_count|PRAGMA (main\.)?page_size|cache_size|cache_spill|mmap|file size|cacheflush|sync_count|db_status|forcecopy|copy_file|rootpage'; then c=pager
elif uses 'lock_status'; then c=lockstatus
else c=other; fi
printf '%s\t%s\t%s\n' "$t" "$c" "$first"
