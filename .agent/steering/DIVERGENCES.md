# Divergences from SQLite 3.53.4

Every intentional behavioural difference is listed here with its reason and
the test that pins it. Anything not listed is a bug.

| Area | SQLite | dbsql | Why | Test |
|---|---|---|---|---|
| File format | SQLite b-tree file | libdb file with sub-databases | libdb is the storage engine | test/bdb/layout.sh |
| `PRAGMA journal_mode`, `wal_*`, `mmap_size`, `locking_mode`, `page_count`, `freelist_count` | supported | accepted; report fixed values; no effect | libdb's log and mpool replace them | test/tcl/known-failures.txt |
| Lock conflicts | file locks, BUSY at statement start | page locks plus the deadlock detector; BUSY may arrive mid-statement, and the statement is rolled back | libdb locking | test/bdb/busy.sh |
| `sqlite_dbpage`, `sqlite_offset()`, the RBU and recover extensions | present | absent | page-level access means nothing on libdb | — |
| C API | `sqlite3_*` | `DBSQL` method tables and `dbsql_*` functions | project decision 5 | test/api |
| Shell `--append` / appendvfs, `.archive` on SQLite-format files | operate on SQLite file images | compiled in, but they only apply to SQLite-format files; a dbsql database is a libdb file | file format | — |
| `.recover`, `.dbinfo` page-level details | read SQLite pages | `.recover` is not built (DBSQL_SHELL_HAVE_RECOVER undefined); `.dbinfo` reports what the storage manager provides | file format | test/shell |
