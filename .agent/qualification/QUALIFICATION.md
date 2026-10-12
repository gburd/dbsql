# Qualification: dbsql 1.2.0 (2026-10-11)

Host: EC2 (64 vCPU), Debian 13 (trixie) amd64, clang 19.1.7, Tcl 8.6,
account `kiwi`, us-west-2.  **libdb v2026.10.5** (553b649a1) **plus the
four `deps/patches` fixes** (R7, R9, R10, R11), built `--enable-diagnostic`.
SQLite 3.53.4 source for the transform and tests.  Measured on
`rebuild/3.53` at the 1.2.0 commits, dbsql built against the patched
libdb (`~/w/inst/libdb-patched`).

## What changed since 1.0.0

1.1.0:

- libdb pin v2026.10.4 -> v2026.10.5; four libdb defect fixes carried as
  source patches in `deps/patches/` (see `.agent/libdb-bugs/`): R7
  (remove-event leak on abort), R9 (recovery panic on a foreign file),
  R10 (env panic on EMFILE opening a log file), R11 (compact hang after a
  sibling dbremove).  Each has a self-checking regression test and an A/B
  against stock v2026.10.5.
- The SQLite suite began being run **per test case**: `known-problems.txt`
  lists by-design divergent case names, so files that formerly were
  skipped whole now pass and their other cases are verified.

1.2.0 (this qualification):

- A VACUUM page-size bug fix: a repeated `PRAGMA page_size=N; VACUUM`
  wrongly flipped the database read-only (`__sm_set_page_size` returned
  DBSQL_READONLY on a fixed size instead of a silent no-op).
- The per-case conversion completed across three triage rounds:
  `known-problems.txt` now lists 3525 by-design divergent case names;
  1111 of 1181 files pass (up from 957 at 1.0.0), known-failures 224 ->
  70, 0 unexpected.  One limitation surfaced and documented, not masked
  (`vacuummem`, `[vacuumoom]`): in-place VACUUM of a very large database
  under a small cache can exhaust memory.

## Gate 1: SQLite tcl suite (patched libdb) — measured

`dist/transpile/suite.sh` with `KNOWN_PROBLEMS=test/tcl/known-problems.txt`
on the patched diagnostic build:

| | files |
|---|---|
| PASS | **1111** of 1181 (was 957 before per-case conversion; +154) |
| FAIL | 33 |
| NORESULT | 31 |
| CRASH | 4 (crash-VFS tests; the crash never reaches libdb) |
| TIMEOUT | 2 (unenforced max_page_count loop; cross-process wait on a -journal) |
| **unexpected** (not in known-failures.txt) | **0** |

The 70 files that still do not pass as a whole are each in
`test/tcl/known-failures.txt` with a reason; none is `[triage]`:

| Reason | Files |
|---|---|
| native (also fails on stock SQLite / fault-injection) | 20 |
| fileformat (SQLite file format, page images, corrupted-file tests) | 17 |
| journal (rollback journal / WAL sidecar files, hot-journal rollback) | 13 |
| vfs (test VFS / fault injector sees libdb I/O) | 9 |
| memdb (serialize/deserialize not built) | 4 |
| pager (page counts and page-level pragmas) | 2 |
| concurrency (WAL-mode: a reader does not block a commit) | 1 |
| droptable (retired table numbers) | 1 |
| vacuumoom (in-place VACUUM memory ceiling) | 1 |

The per-case conversion (`known-problems.txt`, 3525 by-design divergent
case names) plus three rounds of per-file triage flipped 154 files from
whole-file skips to passing, verifying their other cases.  The triage
also surfaced one genuine dbsql bug (`vacuum-11.3`: a repeated
`PRAGMA page_size=N; VACUUM` wrongly flipped the database read-only),
now fixed in `__sm_set_page_size`, and one documented limitation
(`vacuummem-1.2`: in-place VACUUM of a very large database under a small
cache can exhaust memory, tagged `[vacuumoom]`).  No new libdb defect
was found; the two documented unreduced libdb crashes on corrupt pages
(corruptC, pagerfault) were confirmed unchanged.

The R11 fix removed three former hangs: autovacuum_ioerr2 now passes,
and autovacuum / avtrans run to completion (their remaining failures are
pager page-counts and a WAL self-reparse lock, not the hang).

## libdb defect fixes verified (A/B, patched vs stock v2026.10.5)

| # | unpatched v2026.10.5 | patched |
|---|---|---|
| R7  | leaks 6 blocks (in-memory) / 5 (disk) per child-abort remove | no leak |
| R9  | recovery aborts (DIAGNOSTIC) / DB_RUNRECOVERY on a foreign file | recovers; a second database keeps its committed record |
| R10 | EMFILE opening a log file panics the environment (aborts) | returns EMFILE; environment stays alive |
| R11 | compact after a sibling dbremove hangs forever | commits; survivors read back, removed db is gone |

Each has a regression test under the libdb `test/db/` (and
`.agent/libdb-bugs/tests/`) that passes on diagnostic and release builds;
a txn/fop/recd/sdb/compact suite slice shows no regression.

## Regressions against 1.0.0 — none observed

Core tests pass identically on patched and stock v2026.10.5 (select1
192, insert 84, trans2 408, rollback2 138, dataversion1 9; savepoint
31/140 and incrvacuum 1000/1053 unchanged on both).

---

# Qualification: dbsql 1.0.0 (2026-10-11)

Host: EC2 c7i.16xlarge (64 vCPU, 123 GB), Debian 13 (trixie) amd64,
Linux 6.12.111, clang 19.1.7, Tcl 8.6.16, ICU 76.  Account `kiwi`,
us-west-2.  **libdb v2026.10.4** (669f7ec31), built `--enable-debug
--enable-diagnostic`.  SQLite 3.53.4 source for the transform and tests.
Every number here was measured on that host at commit 0ea2fc4 + the
libdb pin (aa90cb6), with both build trees rebuilt from clean.

## Gate 1: make check — PASS (release and ASan+UBSan)

| Tier | Release build | ASan+UBSan build |
|---|---|---|
| unit (sm_cmp_test) | 200000 upstream comparisons agree; 1,000,000 garbage comparisons, no fault, antisymmetric | same, no sanitizer report |
| api | 391 checks, 0 failures | 391 checks, 0 failures |
| shell | smoke ok | smoke ok |
| bdb | snapshot 6/6 (+ peer-exit KNOWN, libdb R6); crash: 10 SIGKILL rounds, no lost commit, no torn row, integrity ok; multiproc: 8 processes x 200 txns, no lost update | same |
| sqlite-tcl | 956 of 1181 files pass; 0 unexpected | 956 of 1181; 0 unexpected |
| exit status | **0** | **0** |

Logs: `make-check-20261010.log`, `make-check-asan-20261010.log`; per-file
results `make-check-tcl.txt`, `make-check-asan-tcl.txt`.

The 225 SQLite test files that do not pass are each in
test/tcl/known-failures.txt with a reason; none is `[triage]`:

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

## Gate 2: valgrind — PASS

`valgrind --leak-check=full --errors-for-leak-kinds=definite` on
libdb 2026.10.4: api (391 checks), shell smoke, bdb multiproc (4
processes) and snapshot: 9 processes, **0 errors, 0 definitely lost
bytes**.  `valgrind-20261010.txt`.

## Gate 3: fuzzing — PASS

Coverage-guided libFuzzer, library built with
`-fsanitize=address,undefined,fuzzer-no-link` (build_fuzz), on libdb
2026.10.4, 60 jobs x 1800 s per target (`fuzz-20261010.txt`):

- sql_fuzz (each input on `:memory:` and on a file database): 810,274
  executions, 21,767 coverage edges, **0 crashes, 0 ASan/UBSan reports,
  0 leaks** (libdb R7 suppressed by test/fuzz/lsan.supp).  3 slow inputs,
  all planning-heavy SQL (512-way nested UNION views): 1.28 s on the
  release build vs 0.94 s for stock sqlite3.
- cmp_fuzz (the libdb comparator on raw bytes): 1,727,926,631
  executions, **0 crashes**.

## Gate 4: reproducible transform — PASS

`dist/transpile/run --check`: transform reproducible.
`check-names.sh src`: 0.  `check-lengths.py`: 0 suspicious sites.

## Gate 5: no sqlite3 in the API — PASS

`nm -D` of libdbsql-1.0.so: 0 symbols matching `sqlite`; 286 exported
`dbsql_*` functions.  Installed headers (dbsql.h, dbsql_ext.h): 0
`sqlite3` identifiers outside comments.  (Internal `__*` functions are
exported, as libdb exports its own.)

## Bugs found and fixed during qualification

From the SQLite suite, the new tiers, ASan and the fuzzer (see git log
2026-10-10): nested readers across a statement rollback; writers locking
before their first read (lost increments under contention); a failed
BEGIN leaving a dead meta handle; one writer per file within a
connection; open/create/NOTADB/CANTOPEN semantics; data_version;
integrity_check counts and NOMEM; blob handles vs DELETE; temp-database
NOMEM; meta reopened after ROLLBACK TO; the placeholder file and
recovery; use-after-free of attached databases' cursors on a shared
aborted txn; cursor buffers padded for record overreads; upstream bugs
in ICU (collator leak) and unionvtab (NULL member access).

## Known libdb defects (reported, not worked around)

R1..R11 in `.agent/libdb-bugs/README.md`, each with a reproducer; all
reproducible ones re-verified on 2026.10.4.

## Not qualified

- FreeBSD: the original FreeBSD 15.1 host's disk filled; the release is
  qualified on Debian only.
