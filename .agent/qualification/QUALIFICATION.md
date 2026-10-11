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

## Gate 3: fuzzing — see FUZZ section

Coverage-guided libFuzzer, library built with
`-fsanitize=address,undefined,fuzzer-no-link` (build_fuzz), on libdb
2026.10.4.  `fuzz-20261010.txt` holds the final run.  The run on libdb
2026.10.3 (40 jobs x 1800 s): sql_fuzz 607,705 executions, 21,758
coverage edges, 0 crashes / sanitizer reports / leaks; cmp_fuzz 0
crashes.

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
