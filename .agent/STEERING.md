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
4. **libdb pinned to `v2026.10.5`** (v2026.10.1 -> .2 on 2026-10-08, .2 -> .3 on 2026-10-09, .3 -> .4 on 2026-10-10, .4 -> .5 on 2026-10-11, each at the maintainer's request when released; deps/patches is empty).
5. **No sqlite3 API**, not even a compatibility shim.
6. `.agent/` is tracked except `.agent/archive/` (old reports).
7. **Scope**: core SQL (3.53.4), FTS5, ICU, and the full shell as `dbsql`.
8. `src/py` (the Python 2 binding) is deleted.
9. Merged branches are deleted. Unmerged branches and stashes were reviewed;
   none had salvageable value (all were superseded hybrid-VDBE work), so they
   were deleted. A full bundle of every ref and stash is kept at
   `~/backups/dbsql-pre-rebuild-20261007.bundle`.
10. Work happens unsupervised on a FreeBSD 15.1 EC2 host. When qualified:
    package, clean up, tag, and release.

## libdb defects

dbsql does not work around libdb bugs (maintainer, 2026-10-09).  A
failure traced to libdb gets a reduced, controlled reproducer and a
write-up in `.agent/libdb-bugs/` (also handed over in /tmp/libdb-bugs),
and the affected SQLite tests are listed in test/tcl/known-failures.txt
against that report.  Using libdb as documented (sizing knobs, handle
lifetimes) is not a workaround.

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

## Build host

The build hosts are short-lived EC2 instances in burner AWS accounts;
nothing lives only on them.  The 1.0.0 qualification ran on
`dbsql-kiwi` (account `kiwi`, us-west-2, c7i.16xlarge, Debian 13).  The
earlier hosts (`dbsql-deb`, `dbsql-fbsd`, account `hotdog`) were
terminated on 2026-10-10 before that account expired.  **Terminate the
host when work is done** (WORKLOG.md has the IDs).
