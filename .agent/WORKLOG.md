# Work log and handoff (read before working)

This is the operational state for agents working unsupervised on the build
host. Design lives in NORTH_STAR.md and steering/. This file records **where
things are and how to run them**. Append to it, and keep it accurate.

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
