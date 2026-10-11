# DBSQL North Star

DBSQL is SQLite's SQL language running on Berkeley DB (libdb) storage, in the
naming and coding style of Berkeley DB.

## The goal

Every SQL feature of **SQLite 3.53.4** (plus the FTS5 and ICU extensions and the
full interactive shell, here named `dbsql`), executing on **libdb v2026.10.5**
as the only storage engine, with **no `sqlite3` symbols, types, or headers** in
the public API. Production quality: every feature tested, sanitizer-clean,
packaged, and released.

## Non-negotiable invariants

A change that violates any of these is wrong, however much it simplifies
something else.

1. **libdb is the storage engine, accessed only through its public API**
   (`db.h`). No libdb-internal symbols, headers, or struct layouts. SQLite's
   btree, pager, WAL and page cache are not shipped. SQLite's Unix VFS is kept
   only for OS services: sorter temp files, time, randomness, sleep and
   dynamic loading. No database file is ever read or written through it.
2. **ACID and crash recovery come from libdb.** A committed transaction
   survives `kill -9` and power loss. Opening an environment that needs
   recovery runs recovery. An uncommitted transaction is never visible after a
   crash.
3. **One SQL database is one libdb database file.** Its tables and indexes
   are sub-databases in that file, so `ATTACH`, backup and copy work on a
   single file. The environment directory holds the libdb regions and logs.
4. **SQL semantics match SQLite 3.53.4.** For any statement that does not
   depend on SQLite's file format or pager, dbsql returns the same rows, types,
   errors and side effects as stock SQLite. SQLite's own test suite (the
   storage-independent subset) is the specification. Divergences are listed,
   justified and tested in `.agent/steering/DIVERGENCES.md`.
5. **Multi-process and multi-thread correctness.** Concurrent connections,
   whether in one process or many, never corrupt data or return torn results.
   Lock conflicts surface as `DBSQL_BUSY` or `DBSQL_LOCKED`, never as a hang or
   a crash.
6. **libdb callbacks are pure and bounds-safe.** The B-tree comparison
   function is invoked by libdb's lock-free descent on unvalidated page bytes.
   It must never read outside its two DBTs, allocate, lock, or fault on
   arbitrary input.
7. **Berkeley DB style throughout.** `lower_snake_case` identifiers, `_t`
   typedefs, `__module_verb()` internal functions, `DBSQL_*` constants, KNF
   with tabs, a `name --` comment block on every function, and readable local
   and field names. Public handles are method tables (`dbp->prepare(...)`), as
   in libdb.
8. **Upstream remains mergeable.** SQLite-derived code is produced by a
   repeatable transform (`dist/transpile`) from a pinned SQLite tag. Moving to
   a later SQLite release is a re-run plus a merge, not a port.

## What "done" means for a feature

- It is implemented in the transformed source or the storage manager.
- SQLite's tests for it pass under dbsql, or each failing case is a listed and
  justified divergence.
- It has a dbsql-specific test wherever libdb changes the behaviour
  (transactions, locking, recovery, backup, file layout).
- It is clean under ASan and UBSan.
- The documentation states what was measured, not what was hoped.

## Explicit non-goals

- No SQLite on-disk file format compatibility. dbsql files are libdb files.
- No `sqlite3_*` API and no `sqlite3.h` compatibility shim.
- WAL, journal modes, mmap, shared cache, `sqlite_dbpage`, the RBU and recover
  extensions, and custom VFSes for database files are out of scope. libdb
  provides logging, caching and recovery instead.
- FTS3/4, R-Tree/Geopoly and Session are out of scope for this release.
