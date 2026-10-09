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

## Validation status (2026-10-08)

- `run --native --keep-sql-names` + `native-testfixture.sh` +
  `tests --keep-sql-names` + `suite.sh` (all in dist/transpile, copied to
  `~/w/tp` on the host):
  - The transformed tree on SQLite's own storage passes **1147/1181** test
    files.
  - Stock 3.53.4 passes 1173. Every file that passes on stock also passes on
    the transform except qrf06 (the shell's QRF formatter is not linked into
    the testfixture yet).
  - Results are in `~/w/tp/native.txt` and `~/w/tp/stock.txt`.
- `check-names.sh` reports 0 occurrences of sqlite outside comments in
  out/src and out/test (default mode).
- `check-lengths.py` reports 0 unhandled prefix-length sites.

## Next steps (in order)

1. **Native validation testfixture.** Transform `src/tclsqlite.c`,
   `src/test*.c` and `ext/misc` (as the test inputs; they are not shipped).
   Build a testfixture on the native tree. Run SQLite's `test/*.test`
   (rewritten the same way: `sqlite3` → `dbsql`, `sqlite_` → `dbsql_`) and
   compare with the stock testfixture's results on the same tests. This
   proves the transform preserves semantics. Keep a list of tests that pass
   on stock and fail on the transformed build. Each one is a transform bug.
2. **Storage manager src/sm/** (see steering/STORAGE.md), which implements
   `src/inc/sm.h` on libdb. Then switch the default build to it.
3. **dist/Makefile.in + configure.ac** for the real tree (libdbsql,
   `dbsql` shell, testfixture, `gmake check`).
4. FTS5, ICU, shell (shell.c.in → `dbsql`), packaging, docs, release.
