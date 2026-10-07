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
4. **libdb pinned to `v2026.10.1`.**
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

## Status

- [x] Phase 0: cleanup, `.agent/` layout, north star and steering.
- [ ] Phase 1: libdb v2026.10.1 submodule and build glue.
- [ ] Phase 2: transform pipeline (dist/transpile) and vendored lemon.
- [ ] Phase 3: storage manager (src/sm/).
- [ ] Phase 4: public API, TCL testfixture, SQLite test-suite runner.
- [ ] Phase 5: storage-dependent features (VACUUM, backup, blob, integrity).
- [ ] Phase 6: FTS5, ICU, `dbsql` shell.
- [ ] Phase 7: docs, packaging, qualification, tag, release.

## Build host

FreeBSD 15.1-RELEASE amd64, c7i.8xlarge, us-east-2, account `hotdog`.
Tagged Owner=gburd, Purpose=dbsql-rebuild, Expires=2026-10-14. Access is via
`ssh dbsql-fbsd` (config in `~/.ssh/ec2/dbsql-fbsd.conf`). **Terminate it when
the release is done.**
