# Work log and handoff (read before working)

This is the operational state for agents working unsupervised on the build
host. Design lives in NORTH_STAR.md and steering/. This file records **where
things are and how to run them**. Append to it, and keep it accurate.

## Hosts and paths

- **Build host:** `ssh dbsql-fbsd` (FreeBSD 15.1 amd64, 32 vCPU, 64 GB).
  The config is in `~/.ssh/ec2/dbsql-fbsd.conf` on the laptop. Use `gmake`,
  not `make`. Use `~/bin/trash` instead of `rm -rf`, because a safety hook
  blocks `rm -rf`.
- **Laptop checkout:** `~/ws/dbsql`, branch `rebuild/3.53`. Commits are made
  and **signed here** (SSH signing key; the host has no signing key).
  The flow is: edit on the laptop, `git push fbsd rebuild/3.53`, then
  `git -C ~/w/dbsql pull` on the host. Alternatively, edit on the host,
  `git diff > x.patch`, then scp it to the laptop and `git apply` + commit.
- **Host repo:** `~/w/dbsql` (clone of the bare `~/w/dbsql.git`).
  `deps/libdb` is a clone of `~/w/libdb.git`, branch `pin` = v2026.10.1.
- **Upstream SQLite:** `~/src/sqlite-3.53.4` (fossil checkout of tag
  version-3.53.4). Treat it as read-only. A stock reference build is in
  `~/src/sq-build`, with `sqlite3` (the differential-testing oracle) and
  `testfixture`.
- **libdb:** built from `deps/libdb/build_unix` with `--enable-debug
  --enable-diagnostic --enable-test --with-tcl`, and installed to
  `~/w/inst/libdb` (`libdb-2026.0.so`, `db.h`). It needs
  `deps/patches/libdb-v2026.10.1-freebsd15.patch` applied to build shared
  libraries on FreeBSD 10+.
- **Python:** `python3.11`. libclang bindings are in `~/w/venv` (not
  currently used).

## Transform (dist/transpile)

- `python3.11 dist/transpile/run --sqlite ~/src/sqlite-3.53.4 --out OUT
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
