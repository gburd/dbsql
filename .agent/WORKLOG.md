# Work log and handoff (read before working)

This is the operational state for agents working unsupervised on the build
host. Design lives in NORTH_STAR.md and steering/. This file records **where
things are and how to run them**. Append to it, and keep it accurate.

## 2026-10-11 — 1.1.0: libdb fixes integrated, per-case tests, release prep

Four post-1.0.0 items addressed on `rebuild/3.53`:

1. **libdb fixes integrated.**  `deps/patches/` carries four source-only
   patches (R7, R9, R10, R11) applied idempotently by
   `deps/build_libdb.sh` to the v2026.10.5 submodule.  Built + installed
   to `~/w/inst/libdb-patched`; dbsql in `~/w/dbsql/build_patched`.
   A/B: autovacuum_ioerr2 now PASSES (R11 hang gone), autovacuum/avtrans
   run to completion, misc7 still fails (R10 __log_name fixed, but a 2nd
   EMFILE panic in __env_attach_regions on temp-env open remains),
   savepoint4 stays R6.  No regression on core tests.
2. **Per-case test conversion.**  tester.tcl reads known-problems.txt and
   counts listed cases as "known errors".  suite.sh accepts KNOWN_PROBLEMS
   and parses "N new errors and K known errors"; run.sh passes it.
   `test/tcl/known-problems.txt` = 2990 by-design divergent case names.
   Result: **1075/1181 files pass** (was 957), **0 unexpected**;
   known-failures.txt 224 -> 106.
3. **Semver 1.1.0.**  dist/RELEASE bumped; README/man/STEERING/QUALIFICATION
   updated; README "Upgrading to 1.1.0" note.  Tag + force-push pending
   final run.sh verify.
4. **Force-push origin/master.**  PI_ALLOW_FORCE_PUSH set; origin/master
   at 4e2cd21 (lease target valid).  At release: ff master to the tagged
   1.1.0 commit, then push --force-with-lease=master:4e2cd21... origin master.

Host: `~/w/inst/libdb-patched`, `~/w/dbsql/build_patched`,
`~/w/binp/{testfixture,dbsql,dbsql-static}`, `/tmp/tst/test`,
`/tmp/kprun2.txt`.  `~/w/chk1.sh TF TESTDIR TEST...` runs one test;
`~/w/cleanup.sh` kills stalled jobs.

## 2026-10-11 — libdb defect fixes R7, R9, R10, R11 (on 2026.10.5)

Worked in `~/w/libdb-fix` on host `dbsql-kiwi`, branch `fixes` off tag
`v2026.10.5` (553b649a1).  Four fixes committed, each with a self-checking
regression test under `test/db/` and a `test/MANIFEST` runner, verified A/B
on diagnostic (`build_unix`) and release (`build_rel`) builds:

| Commit | Defect | File | Test |
|---|---|---|---|
| a258ae7e8 | R7 leak | src/txn/txn_util.c | txn_remevent_leak.c |
| 61fd2a246 | R11 hang | src/db/db_compact.c | compact_subdb_remove.c |
| c889266d8 | R10 panic | src/log/log_put.c | log_emfile_nopanic.c |
| 078056988 | R9 panic/abort | src/dbreg/dbreg_{util,rec}.c | recover_foreign_file.c |

- R7: on abort, free a child txn's TXN_REMOVE event (only a commit hands it
  to the parent).  Counted with db_env_set_func_malloc/realloc/free.
- R11: __db_move_metadata took the moved-page handle lock under the dbp's
  own handle locker (READ) while inside a txn; when the lower page was one
  the same txn WRITE-held from a sibling dbremove, it self-deadlocked.
  Acquire under the txn locker (WRITE) + lockevent instead.
- R10: __log_name panicked on any non-ENOENT open error; EMFILE/ENFILE are
  transient, return them.  R10 now has a deterministic standalone repro
  (EMFILE injected via db_env_set_func_open).
- R9: recovery meeting a foreign/empty referenced file (EINVAL) failed all
  of recovery -> __env_panic (abort under DIAGNOSTIC).  Treat EINVAL like
  the already-tolerated ENOENT in __dbreg_do_open and
  __dbreg_register_recover.
- R6 is documented DB_REGISTER behavior (recovery discards the env under
  live peers); no safe surgical code fix.  Consultation item, left in
  known-failures under libdb-R6.

Patches (git-format-patch) and test sources mirrored to
`.agent/libdb-bugs/patches/` and `.agent/libdb-bugs/tests/` (and
`/tmp/libdb-bugs/`); README.md updated with a "Fix" subsection per defect.
Suite slice (txn, fop, recd001-024, sdb001-012, test001-062, test111-117,
compact) shows no base-vs-fix difference; the one FAIL (recd015, out of
lockers) is pre-existing and identical on both.

Still open: consume these in dbsql (deps/patches applied by
build_libdb.sh, or an upstream libdb release) and retest the affected
known-failures (savepoint4, misc7, autovacuum*, avtrans); force-push
origin/master; convert the 225 known-failure tests; semver release.

## Deadline: AWS account `hotdog` expires 2026-10-10 14:43:47 UTC

Everything on the hosts is disposable: commits live on the laptop
(`~/ws/dbsql`, pushed to `deb` as a convenience), host-only results are
copied to `~/backups/dbsql-hosts-20261009/` (suite result files, the
classifier output, the prepared SQLite source tarball).  Plan:
- stop starting new long runs at ~12:00 UTC on 2026-10-10;
- by ~13:30 UTC: final backup, terminate `dbsql-deb` and `dbsql-fbsd`,
  delete both key pairs and security group sg-0ac1cb145a7e27331, drop
  the `Include ~/.ssh/ec2/dbsql-*.conf` lines from ~/.ssh/config;
- the release itself (merge/tag/push to origin) happens from the laptop
  and does not need AWS.

## 2026-10-10 01:45 UTC — nested readers and test harness

- `812dcb8`: preserve a shared read transaction when a nested statement
  aborts; after ending a write transaction, reopen read transactions/meta
  handles for surviving readers. Preserve SKIPNEXT when closing a DBC at a
  transaction boundary. Also report persistent database filenames as full
  paths, reusing the allocated path.
- Debian targeted checks: progress 12/12, tkt2820 17/17, rollback2 138/138,
  misc8 16/16, returning1 89/89, dataversion1 9/9, trans2 408/408,
  savepoint6 8007/8007. These do not establish a green suite. strict2-3.0,
  the existing 32 savepoint failures and trans (NORESULT) remain.
- `217fac3`: build dbsql-static for Tcl shell tests; the normal `dbsql`
  is a libtool wrapper and cannot be copied alone. The test runner now
  requires the shell instead of silently continuing without it. Objects
  and testfixture depend on dbsql_config.h.
- Validation trap: host dist/config.hin was stale. `dist/s_config` plus
  configure regenerated it; DBSQL_HAVE_ZLIB is now defined. Previous
  make8 zipfile/shell8 results with only 1 test were skips, not coverage.
  With fresh config, zipfile runs 116 tests and still fails 14.10.
- `vtab_err-3-oom-transient.432` fails with both HEAD-before-fix storage
  sources and the new sources (same binary configuration, 2967 tests).
  Its earlier 2937-test pass is not an equivalent control.
- Full suites: make8 uses the stale config; make9 uses fresh config,
  relocated testfixture + dbsql-static, and the current fixes. Results
  `~/w/tp/make{8,9}.txt`; logs `/tmp/suite-make{8,9}.log`. make9 started
  about 01:43 UTC. Completed: make9 PASS=935, FAIL=197, CRASH=7,
  TIMEOUT=5, NORESULT=37 (1181 files). Shell8 still reports only 1 test;
  investigate that skip before claiming shell coverage. vtab_err is the
  only formerly passing file now nonpassing, but OOM iteration counts
  differ between suite and isolated runs; retain the baseline control
  above and investigate rather than calling it fixed or a regression.
  Do not count untriaged failures as divergences.
- libdb v2026.10.4 is still absent from origin, github.com/gburd/libdb,
  and the FreeBSD bare repo's tags; pin remains v2026.10.3, pending the
  requested release becoming available.
- Host build tree contains synced changes, not yet pulled commits;
  never discard differences without checking them against laptop HEAD.
  All temporary ERRDBG instrumentation was removed before validation.

## 2026-10-11 — 1.0.0 released

- Tag `v1.0.0` (annotated, signed) on 4676583, pushed to origin
  (Codeberg) with branch `rebuild/3.53`.  Local `master` fast-forwarded
  to it.  origin's `master` (4e2cd21) is a rewritten copy of the same
  2026-03-31 history as the old local master (34f4153; same 99 commits,
  messages and dates, unsigned, plus the removal of two stray log
  files); it is not an ancestor of the release, so origin/master was
  **not** moved.  Moving it is the maintainer's call (force-push, or
  make `rebuild/3.53` the default branch).
- EC2: `dbsql-kiwi` terminated, its key pair and security group deleted,
  ssh config cleaned; no dbsql instances remain in `kiwi`.  `hotdog`
  credentials are gone (account expired); its resources were removed
  before expiry.  Host results: `~/backups/dbsql-kiwi-20261011/`.

## 2026-10-11 — 1.0.0 qualified

- AWS: `hotdog` torn down 14:25 UTC Oct 10 (both instances terminated,
  key pairs and security group deleted, ssh config cleaned); work moved
  to `kiwi`, us-west-2 (us-east-2's vCPU quota there was 32):
  instance in `~/.ssh/ec2/dbsql-kiwi.env`, `ssh dbsql-kiwi`.
- libdb pinned to v2026.10.4 (released Oct 10); all reproducible defect
  reports re-verified on it.
- New tiers: test/api (391 checks), test/bdb (snapshot, SIGKILL crash
  recovery, multi-process), test/fuzz (libFuzzer, coverage-instrumented
  ASan+UBSan library).  test/tcl/known-failures.txt: 225 files, every
  one with a reason, none [triage].
- Gates (QUALIFICATION.md): make check exit 0 on release and ASan+UBSan
  builds (956/1181 SQLite files, 0 unexpected); valgrind 0 errors, 0
  definite leaks; fuzz; transform reproducible; no sqlite3 symbols.
- libdb defects R6 (DB_REGISTER recovery under live processes), R7
  (inmem dbremove leak), R9 (recovery meeting a foreign file aborts),
  R10 (EMFILE on log reopen panics), R11 (compact after dbremove hangs)
  added; the DB_FAILCHK hybrid tried for R6 was reverted (libdb
  diagnostic aborts).
- Host workflow traps hit again: pkill/pgrep patterns in an ssh command
  line match that ssh command itself (use ~/w/stopsuite.sh,
  ~/w/stopall.sh); rsync'd untracked files block `git pull` (sync code
  only through git); force-pushes need `git reset --hard origin/...` on
  the host; a stale generated config.hin silently disabled zlib.

## Hosts and paths

- **Build host:** `ssh dbsql-deb` -- Debian 13 amd64, c7i.16xlarge
  (64 vCPU, 123 GB, 400 GB gp3), instance `i-0950a15f95412ede7`, key pair `dbsql-deb-20261009T004900Z`,
  account `hotdog`, us-east-2.  Config: `~/.ssh/ec2/dbsql-deb.conf`.
  It replaced the FreeBSD host `dbsql-fbsd` (`i-025a08857fb2c27b8`)
  on 2026-10-09: that host's 1 TB pool filled with test scratch that
  suite.sh had moved to a trash can.  The FreeBSD host is kept only to
  check the FreeBSD build before release; terminate both at the end.
- **Laptop checkout:** `~/ws/dbsql`, branch `rebuild/3.53`.  Commits are
  made and **signed here** (SSH signing key; the hosts have none), then
  `git push deb rebuild/3.53` (bare repo `~/w/dbsql.git` on the host)
  and `git -C ~/w/dbsql pull` there.  For edit/test loops before a
  commit, `.agent/sync-deb.sh [builddir]` rsyncs src/sm and dist to the
  host and rebuilds the libdb testfixture in `~/w/<builddir>`.
- **Host layout:**
  - `~/w/dbsql`: clone; `deps/libdb` is the submodule at the pin.
  - `~/w/inst/libdb`: libdb installed from deps/libdb
    (`sh deps/build_libdb.sh --enable-debug --enable-diagnostic
    --prefix=$HOME/w/inst/libdb && make -C deps/libdb/build_unix install`).
  - `~/src/sqlite-3.53.4`: upstream source, read-only.
  - `~/w/tp`: copy of dist/transpile; `lo/` = `run --keep-sql-names`
    output (libdb build), `out/` = `--native --keep-sql-names`, `nb/`
    = native testfixture, `tst/test` = prepared SQLite tests, `*.txt`
    = suite results.
  - `~/w/lb*`: libdb testfixture builds (libdb-testfixture.sh).
  - `~/w/one.sh TF TEST [N]`: run one test file in a scratch copy.
- **Toolchain:** clang 19 (`CC=clang`), tcl8.6-dev, python3 3.13, gdb,
  valgrind, ICU.  Scratch copies under /tmp are deleted with `rm`.
- **libdb pin:** v2026.10.3 (98749ce0c).  No local patches.

## Transform (dist/transpile)

- `python3 dist/transpile/run --sqlite ~/src/sqlite-3.53.4 --out OUT
  [--native] --report report.txt` writes OUT/src (and OUT/native when
  `--native` is given). `report.txt` lists every collision fallback and the
  full identifier map (`MAP old new` lines).
- `sh dist/transpile/check-names.sh OUT/src` must print 0. It is the
  "no sqlite outside comments" gate.
- `sh dist/transpile/native-build.sh OUT BUILD` compiles OUT against the
  transformed SQLite storage layers, producing libdbsql_native.a. Status:
  **0 compile errors**. A smoke program runs CTEs, window functions, JSON
  and joins correctly.
- The pieces:
  - Rule logic is in `names.py`.
  - Per-name overrides are in `names.map`.
  - Explicit text edits are in `fixups.py`. Each fixup must match at least
    once, or the run fails.
  - Scopes are computed by `scan.py`.
  - `reserved.txt` holds libc, libdb and system-header `__` names, which
    renamed globals must avoid.

## Validation status (2026-10-09, Debian host, libdb 2026.10.3)

SQLite 3.53.4 test files (1181), one testfixture process per file,
`dist/transpile/suite.sh`, results in `~/w/tp/*.txt` on the host:

| Build | Pass | File |
|---|---|---|
| native (SQLite storage, transformed) | 1164 | deb-native.txt |
| libdb, first full run | 810 | deb-libdb1.txt |
| libdb, latest full run | 927 | deb-libdb8.txt |

Of the libdb non-passes that pass natively (~240): ~220 test SQLite's
file format, journal/WAL, VFS fault injection or page counts (expected
divergences, to be listed in test/tcl/known-failures.txt); ~17 are still
unexplained; R1 accounts for without_rowid1 and upfrom2; R4 for the
VACUUM rollback crash.  Classifier output: `/tmp/kf.txt` on the host.

- libdb defects: `.agent/libdb-bugs/` (R1-R5), not worked around.
- Gates: `run --check` reproducible; check-names 0; check-lengths 0.
- `configure --with-berkeleydb=... --enable-test && make` builds
  libdbsql, `dbsql` (reports libdb 2026.10.3) and the testfixture.
- Performance: a 98k-row txn is ~6x upstream; a one-row autocommit
  INSERT ~20x (R3).

## Next steps (in order)

1. test/tcl/run.sh + known-failures.txt (name, reason) so `make check`
   runs the suite; test/shell/run.sh for smoke.sql.  Note: the committed
   tree uses dbsql_ SQL names; the suite so far ran the --keep-sql-names
   build, so run `tests` (without --keep-sql-names) against `make`'s
   testfixture for the shipped names.
2. The ~17 unexplained failures, 9 crashes, 6 timeouts (classify:
   dbsql bug -> fix; libdb bug -> report in .agent/libdb-bugs).
3. Remaining TESTING.md tiers: api, diff vs stock sqlite3, bdb (kill -9
   recovery, multi-process, BUSY), fuzz, unit (savepoints, snapshot).
4. Qualification: ASan/UBSan build, valgrind, 30-min fuzz, nm -D gate.
5. Packaging/docs: dbsql.1, Tcl package, flake.nix, DIVERGENCES, README.
6. Release: merge, tag, push origin; terminate both hosts.
