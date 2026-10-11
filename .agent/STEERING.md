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
4. **libdb pinned to `v2026.10.5`** (v2026.10.1 -> .2 on 2026-10-08, .2 -> .3 on 2026-10-09, .3 -> .4 on 2026-10-10, .4 -> .5 on 2026-10-11, each at the maintainer's request when released).  deps/patches carries four source-only fixes for libdb defects dbsql found (R7, R9, R10, R11), applied idempotently by deps/build_libdb.sh.
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
    annotated and signed (`v1.0.0`, `v1.1.0`, ...).  The release-tagging
    skill's date-based `vYYYY.MM.DD` format is for the maintainer's other
    repos and does not apply here.  A libdb-pin bump plus the per-case
    test conversion and the four libdb fixes is a MINOR release (1.1.0):
    dbsql's own on-disk format (databases, logs) is unchanged; only
    libdb's internal region format changed, which an upgrade note covers.

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
- [ ] 1.1.0: libdb pin v2026.10.5 + four libdb fixes (deps/patches),
      per-case test conversion (known-problems.txt), re-qualify, tag
      v1.1.0.

## Build host

The build hosts are short-lived EC2 instances in burner AWS accounts;
nothing lives only on them.  The 1.0.0 qualification ran on
`dbsql-kiwi` (account `kiwi`, us-west-2, c7i.16xlarge, Debian 13).  The
earlier hosts (`dbsql-deb`, `dbsql-fbsd`, account `hotdog`) were
terminated on 2026-10-10 before that account expired.  **Terminate the
host when work is done** (WORKLOG.md has the IDs).
