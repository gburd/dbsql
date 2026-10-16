# DBSQL — Agent Steering (live tracker)

Read `.agent/NORTH_STAR.md` first. Detailed design lives in
`.agent/steering/`: TRANSFORM.md (SQLite→dbsql naming and the transform
pipeline), STORAGE.md (the libdb storage manager), TESTING.md (tiers and
release gates), DIVERGENCES.md (justified differences from SQLite). Update
this file whenever a phase lands.

## Decisions of record (maintainer, 2026-10-07)

1. **Rebuild** the SQL layer from SQLite 3.53.4. The 2026-03 hybrid is frozen
   at tag `archive/hybrid-2026-03`, and the 2024 SQLite-2.8 code at
   `archive/sqlite28-2024`.
2. **Readable names**: fields and locals use plain readable names, not
   SQLite's Hungarian (see TRANSFORM.md).
3. **One libdb file per SQL database**, with tables and indexes as
   sub-databases.
4. **libdb pinned to `v2026.10.7`** (v2026.10.1 -> .2 on 2026-10-08, .2 -> .3 on 2026-10-09, .3 -> .4 on 2026-10-10, .4 -> .5 on 2026-10-11, .5 -> .7 on 2026-10-15, each at the maintainer's request when released).  The four libdb fixes dbsql found (R7, R9, R10, R11) were upstreamed in libdb v2026.10.6, so `deps/patches` is empty again as of the v2026.10.7 pin.
5. **No sqlite3 API**, not even a compatibility shim.
6. `.agent/` is tracked except `.agent/archive/` (old reports).
7. **Scope**: core SQL (3.53.4), FTS5, ICU, and the full shell as `dbsql`.
8. `src/py` (the Python 2 binding) is deleted.
9. Merged branches are deleted. Unmerged branches and stashes were reviewed;
   none had salvageable value (all were superseded hybrid-VDBE work), so they
   were deleted. A full bundle of every ref and stash is kept at
   `~/backups/dbsql-pre-rebuild-20261007.bundle`.
10. Work happens unsupervised on a short-lived EC2 host. When qualified:
    package, clean up, tag, and release.
11. **Versioning is semver**, not calver: tags are `vMAJOR.MINOR.PATCH`,
    annotated and signed (`v1.0.0`, `v1.1.0`, `v1.2.0`, `v1.3.0`,
    `v1.3.1`, `v1.3.2`, ...).  The
    release-tagging skill's date-based `vYYYY.MM.DD` format is for the
    maintainer's other repos and does not apply here.  1.1.0 was a MINOR
    release (libdb pin v2026.10.4 -> .5, four libdb fixes, the start of
    per-case test conversion).  1.2.0 is a MINOR release: a VACUUM
    page-size bug fix and the full per-case suite conversion (1111 of
    1181 files pass, known-failures 224 -> 70).  1.3.0 is a MINOR
    release: libdb pin -> v2026.10.7 (dbsql's fixes now upstream) and a
    heap-overflow fix in the sm filename, first qualified on FreeBSD.
    1.3.1 is a PATCH release: two test fixes (tkt2565 no longer aborts
    the testfixture -- dbsql has no SQLite page cache to swap; tkt2686
    skipped by design -- it needs an enforced PRAGMA max_page_count) and
    a strict-alignment check of the storage manager and libdb on riscv64
    under UBSan.  No code or on-disk change.
    1.3.2 is a PATCH release: the libdb pin advances to the upstream
    commit that declares v2026.10.7 the first libdb LTS release (same
    libdb code, only docs added upstream); no dbsql code or on-disk
    change.
    dbsql's on-disk format is unchanged across 1.0.0 through 1.3.2.

## libdb defects

dbsql does not work around libdb bugs (maintainer, 2026-10-09).  A
failure traced to libdb gets a reduced, controlled reproducer and a
write-up in `.agent/libdb-bugs/` (also handed over in /tmp/libdb-bugs),
and the affected SQLite tests are listed in test/tcl/known-failures.txt
against that report.  Using libdb as documented (sizing knobs, handle
lifetimes) is not a workaround.

Four of these defects now have fixes (maintainer, 2026-10-11): R7
(txn remove-event leak on abort), R9 (recovery panic on a foreign
file), R10 (env panic on EMFILE opening a log file) and R11 (compact
hang after a sibling dbremove).  Each has a source patch in
`deps/patches/`, a self-checking regression test (`.agent/libdb-bugs/`
and the libdb `test/db/`), and an A/B against stock v2026.10.5.  They
are carried as submodule patches, not dbsql workarounds.  R6 is
documented `DB_REGISTER` behavior (no code fix); R1-R5 remain reported
only.

## Status

- [x] Phase 0: cleanup, `.agent/` layout, north star and steering.
- [x] Phase 1: libdb submodule (v2026.10.5) and build glue.
- [x] Phase 2: transform pipeline (dist/transpile) and vendored lemon.
- [x] Phase 3: storage manager (src/sm/).
- [x] Phase 4: public API, Tcl testfixture, SQLite test-suite runner.
- [x] Phase 5: storage-dependent features (VACUUM, backup, blob, integrity).
- [x] Phase 6: FTS5, ICU, `dbsql` shell.
- [x] Phase 7: docs, packaging, qualification (.agent/qualification/),
      tag v1.0.0, release.
- [x] 1.1.0: libdb pin v2026.10.5 + four libdb fixes (deps/patches),
      re-qualify, tag v1.1.0.
- [x] 1.2.0: VACUUM page-size readonly fix; full per-case suite
      conversion (known-problems.txt, 3525 cases; 1111 of 1181 files
      pass, known-failures 224 -> 70); tag v1.2.0.
- [x] 1.3.0: libdb pin v2026.10.5 -> v2026.10.7 (the four dbsql fixes are
      upstream, deps/patches empty); fix the __database_name
      heap-overflow on the sm filename (ASan on FreeBSD); first FreeBSD
      qualification; tag v1.3.0.

## Build host

The build hosts are short-lived EC2 instances in burner AWS accounts;
nothing lives only on them.  The 1.0.0 qualification ran on
`dbsql-kiwi` (account `kiwi`, us-west-2, c7i.16xlarge, Debian 13).  The
earlier hosts (`dbsql-deb`, `dbsql-fbsd`, account `hotdog`) were
terminated on 2026-10-10 before that account expired.  1.3.0 was
qualified on a fresh FreeBSD 14.5 host (`dbsql-fbsd`, account `kiwi`,
us-west-2, c7i.8xlarge, ZFS); note FreeBSD is much slower than Debian
for dbsql's txn-heavy tests (process-shared-mutex `__umtx_op_shm` cost,
see QUALIFICATION) and triggers the libdb R6 recovery divergence far
more readily.  **Terminate the host when work is done** (WORKLOG.md has
the IDs).  1.3.1 added a strict-alignment check of the storage manager
and libdb on `lixr` (riscv64, clang, UBSan `-fsanitize=alignment`): the
full suite ran with zero misaligned-access reports; see QUALIFICATION.
