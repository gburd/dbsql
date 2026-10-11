# Qualification: dbsql 1.0.0 (2026-10-10)

Host: EC2 c7i.16xlarge (64 vCPU, 123 GB), Debian 13 (trixie) amd64,
Linux 6.12.111, clang 19.1.7, Tcl 8.6.16, ICU 76.  Account `kiwi`,
us-west-2.  libdb v2026.10.3 (98749ce0c), built `--enable-debug
--enable-diagnostic`.  SQLite 3.53.4 source for the transform and tests.
Every number here was measured on that host from the commit named.

## Gate 1: make check

`make check` (build_unix, release configuration with
`--enable-test --enable-diagnostic`):

| Tier | Result |
|---|---|
| unit | sm_cmp_test: 200000 upstream comparisons agree; 1,000,000 garbage comparisons, no fault, antisymmetric |
| api | 391 checks, 0 failures |
| shell | smoke ok |
| bdb | snapshot: 6/6 ok (+ peer-exit-without-close KNOWN, libdb R6); crash: 10 SIGKILL rounds, 0 lost commits, 0 torn rows, integrity ok; multiproc: 8 processes x 200 transactions, no lost update |
| sqlite-tcl | 956 of 1181 files pass; 0 unexpected failures, 0 listed files now passing (`make-check-tcl.txt`); `make check` exit 0 (`make-check-20261010.log`) |

SQLite test suite, 1181 files (run k12 at 5693092, then make check):
956 pass.  The other 225 are each in test/tcl/known-failures.txt with a
reason; none is `[triage]`:

| Reason | Files |
|---|---|
| journal (rollback journal / WAL files and modes) | 69 |
| fileformat (SQLite file format, page images, corrupted-file tests) | 53 |
| vfs (database I/O through test VFSes, crash VFS, fault injection) | 26 |
| pager (page counts, cache, `max_page_count`) | 25 |
| native (fails on stock SQLite 3.53.4 too, same harness) | 20 |
| concurrency (WAL-mode concurrency: readers never block a commit) | 12 |
| memdb (serialize/deserialize not built) | 5 |
| lockstatus (`lock_status` reports `unknown`) | 4 |
| naming (`dbsql_autoindex_*` sorts before user names) | 2 |
| filecopy, droptable | 1 each |
| libdb defects: R1 (2), R6 (1), R10 (1), R11 (3) | 7 |

The native control is the same transform on SQLite's own storage, with
zlib, run with the same harness: 1155 of 1181 pass.

## Gate 1b: ASan + UBSan build

build_asan: `CFLAGS="-O1 -g -fsanitize=address,undefined
-fno-omit-frame-pointer -fno-sanitize-recover=undefined"
--enable-diagnostic --disable-shared`.  unit, shell smoke: clean.  The
fuzz gate below runs this configuration.  Targeted ASan runs found and
fixed two use-after-free bugs (5693092: cursors of attached databases on
a shared aborted txn).

## Gate 2: valgrind

`valgrind --leak-check=full --errors-for-leak-kinds=definite`: api
(391 checks), shell smoke, bdb multiproc (4 processes) and snapshot:
0 errors, 0 definitely lost bytes in every process.  See
`valgrind-20261010.txt`.

## Gate 3: fuzzing

Coverage-guided libFuzzer, library built with
`-fsanitize=address,undefined,fuzzer-no-link`, 40 jobs x 1800 s:

- sql_fuzz (each input on `:memory:` and on a file database): 607,705
  executions, 21,758 coverage edges, **0 crashes, 0 sanitizer reports,
  0 leaks** (libdb R7 suppressed by test/fuzz/lsan.supp).  3 slow
  inputs (512-way nested UNION views; stock sqlite3 0.76 s, dbsql 1.10 s
  on the release build).
- cmp_fuzz (the libdb comparator on raw bytes): 40 x 1800 s, 0 crashes.

Earlier fuzz runs found and fixed: `PRAGMA data_version` of a `:memory:`
database read 0 (FTS5 aborted), and an upstream ICU collator leak.

## Gate 4: reproducible transform

`dist/transpile/run --check`: transform reproducible.
`check-names.sh src`: 0.  `check-lengths.py`: 0 suspicious sites.

## Gate 5: no sqlite3 in the API

`nm -D` of libdbsql-1.0.so: 0 symbols matching `sqlite`; 286 exported
`dbsql_*` functions.  Installed headers (dbsql.h, dbsql_ext.h): 0
`sqlite3` identifiers outside comments.  (Internal `__*` functions are
exported as libdb exports its own; that is the codebase's convention.)

## Known libdb defects (reported, not worked around)

R1..R11 in `.agent/libdb-bugs/README.md`, each with a reproducer.

## Not qualified

- FreeBSD: the original FreeBSD 15.1 host's disk filled; the release is
  qualified on Debian only.
- libdb v2026.10.4: not tagged in any checked remote (berkeleydb/libdb,
  gburd/libdb) at qualification time; the pin is v2026.10.3.
