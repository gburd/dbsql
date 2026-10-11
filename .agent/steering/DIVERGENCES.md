# Divergences from SQLite 3.53.4

Every intentional behavioural difference is listed here with its reason
and the test that pins it.  Anything not listed is a bug.  Each SQLite
test file that fails because of a row here is listed in
`test/tcl/known-failures.txt` with that row's tag (`[tag]`).

| Tag | Area | SQLite | dbsql | Why | Test |
|---|---|---|---|---|---|
| fileformat | File format | SQLite b-tree file with a 100-byte header | a libdb btree file with one sub-database per table and index | libdb is the storage engine | test/bdb, `hexio`/`dbpage`-based SQLite tests |
| journal | Rollback journal and WAL | `-journal`, `-wal`, `-shm` files; `PRAGMA journal_mode`, `wal_checkpoint`, `wal_autocheckpoint`, `locking_mode`, `journal_size_limit` | no such files; the pragmas are accepted and report `delete` / fixed values, with no effect.  Concurrency is always WAL mode's (below) | libdb's log, mpool and recovery replace them | test/bdb/snapshot.test |
| concurrency | Readers and writers | rollback-journal mode: a writer cannot commit while a reader is open (`database is locked`) | always WAL-mode semantics: readers see the snapshot their read transaction started with and never block a commit; one writer per database; a stale snapshot cannot upgrade to write (`BUSY`) | libdb MVCC (`DB_MULTIVERSION` + `DB_TXN_SNAPSHOT`) | test/bdb/snapshot.test (checked against SQLite in `journal_mode=WAL`) |
| pager | Page-level pragmas and counters | `page_count`, `freelist_count`, `page_size` changes, `cache_size`/`cache_spill`, `mmap_size`, `max_page_count`, `dbstat`, `PRAGMA shrink_memory` effects, `sqlite3_db_status` cache counters | accepted; report what libdb provides (page size is libdb's, 4096 by default here).  `max_page_count` is recorded but not enforced: a database grows until the disk is full (`DBSQL_FULL`) | no SQLite pager | — |
| vfs | Custom VFSes for database files | test VFSes (`testvfs`, `crash`, `devsym`, `multiplex`, `quota`, `cksumvfs`) and fault injection see every database read and write | database files are read and written by libdb, not through the VFS; the VFS still serves temp files, time, randomness and sleep | libdb does its own I/O | — |
| memdb | `sqlite3_serialize` / `deserialize`, `memdb` VFS | present | not built (`DBSQL_OMIT_DESERIALIZE`) | page-image API over SQLite's format | — |
| lockstatus | `sqlite3_file_control(LOCK_STATUS)`, `db_lock_status` | unlocked / shared / reserved / exclusive | `unknown` | libdb page and database locks have no SQLite lock levels | — |
| offset | `sqlite_offset()` | byte offset of a record in the file | NULL | no page offsets | — |
| droptable | `DROP TABLE` | the b-tree pages are freed at once and their numbers are reused by the next `CREATE` | the table is emptied and its number retired (never reused); its empty libdb sub-database is removed when no handle is open on it, else at the next `VACUUM`.  So a table whose schema row was deliberately aliased to another's root (`writable_schema`) stays unreadable after the alias is dropped, where SQLite's page reuse happens to make it readable again (strict2-3.0) | libdb `dbremove` needs every handle closed; reuse would let a stale handle see a new table | sm.c `__sm_drop_table` |
| autovacuum | `auto_vacuum` | moves pages; `incremental_vacuum(N)` frees N pages | the mode is fixed at creation as upstream; FULL compacts at commit and INCREMENTAL on request, through `DB->compact`; page counts reported are libdb's | libdb compaction | autovacuum*.test results |
| peerexit | A process exits without closing a database another process has open | no effect on the other process | the next process to open it runs libdb recovery, and connections open elsewhere then fail (`database disk image is malformed`) until reopened | libdb `DB_REGISTER` (.agent/libdb-bugs R6) | test/bdb/snapshot.test (`KNOWN`) |
| filecopy | Copying a database by copying its file (`cp test.db`, tests' `db_save`/`db_restore`, `forcecopy`) | the copy is a complete database (with its `-journal`) | the database is the file plus the environment's log until a checkpoint; copy it with `.backup`, `VACUUM INTO` or the backup API, or with the environment closed | libdb's log holds committed changes not yet in the file | — |
| attach | A transaction spanning databases in different directories | atomic (super-journal) | each directory is its own libdb environment; the commit is not atomic across them | one environment per directory | — |
| C API | `sqlite3_*` | `DBSQL` method tables and `dbsql_*` functions | project decision 5 | test/api |
| shell | `--append`/appendvfs, `.archive` on SQLite files, `.recover`, `.dbinfo` | operate on SQLite file images | `.archive`/`--append` only apply to SQLite-format (zip) files; `.recover` is not built; `.dbinfo` reports what the storage manager provides | file format | test/shell |

## Known libdb defects

These are not divergences; they are libdb bugs, reported upstream with
reproducers in `.agent/libdb-bugs/`.  Four now have source fixes carried
in `deps/patches/` (R7, R9, R10, R11); the rest are reported only.  Tests
they break are listed in `test/tcl/known-failures.txt` as `[libdb-Rn]`.

| # | Effect on dbsql |
|---|---|
| R1 | after a delete, an insert of an equal key through another cursor can keep the old key bytes (WITHOUT ROWID; `without_rowid1-2.1.2`, `upfrom2-1.2.13`) |
| R2 | sized around: `set_tx_max`, as libdb documents for MVCC |
| R3 | small write transactions cost ~200 µs extra (checkpoint threshold test) |
| R4 | abort after `DROP TABLE` + compaction in one transaction panics the environment (VACUUM in a rolled-back transaction) |
| R5 | a write that fails with ENOMEM under MVCC cannot be rolled back (OOM tests) |
| R6 | see `peerexit` above |
| R7 | `SAVEPOINT; DROP TABLE; ROLLBACK` on `:memory:` leaks ~250 bytes (fixed, deps/patches) |
| R9 | recovery meeting a foreign/empty data file panics/aborts (fixed, deps/patches) |
| R10 | out of file descriptors (EMFILE) opening a log file panics the environment (misc7; fixed in `__log_name`, deps/patches -- a second EMFILE site in temp-env open remains) |
| R11 | compact after a sibling `dbremove` in one transaction hangs (autovacuum FULL / VACUUM after DROP; fixed, deps/patches) |

## Running the SQLite suite per test case

The SQLite test files mix cases that apply to dbsql with cases that
exercise a divergence above (the SQLite file format, a test VFS, pager
page counts, rollback-journal concurrency).  Rather than skip a whole
file for a handful of divergent cases, `test/tcl/known-problems.txt`
lists the individual case names that fail by design.  The transpiled
`tester.tcl` reads it from the test directory and counts those as "known
errors", so a file whose only failures are listed cases passes and its
other cases are actually run and verified (`dist/transpile/suite.sh`
reports `PASS n (k known)`).  Files that fail on stock SQLite too
(`[native]`) and fault-injection files whose failing set is
nondeterministic (malloc/ioerr/*fault) stay whole-file entries in
`known-failures.txt` instead.
