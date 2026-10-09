# Storage manager (src/sm/): SQL on libdb

This is the design of record. Change this file when the design changes.

## The seam

The VDBE and code generator reach storage only through `src/inc/sm.h`. That
header is SQLite's `btree.h`, renamed by the transform:
`sqlite3BtreeFoo` → `__sm_foo`, `Btree` → `sm_t`, `BtCursor` → `sm_cursor_t`.
The semantics are identical. Code outside storage calls 95 of its 96
functions, so keeping the seam means the transformed upstream code needs
no storage edits, and later SQLite releases merge mechanically.

The pager-level calls made by main.c, pragma.c, vacuum.c and the API files
(about 35 `__sm_pager_*` functions) are answered by a small shim in
`src/sm/sm_pager.c`. It reports fixed values for WAL, journal and mmap
(see DIVERGENCES.md).

`src/sm/` replaces SQLite's btree.c, btmutex.c, pager.c, pcache.c,
pcache1.c, wal.c, memdb.c, backup.c, dbstat.c and dbpage.c. Those files
are built only for the `--native` validation testfixture.

Compile-time options for the shipped build:

- `DBSQL_OMIT_SHARED_CACHE` (the `__sm_enter`/`__sm_leave` calls become
  macros)
- `DBSQL_OMIT_WAL`
- `DBSQL_OMIT_DESERIALIZE`
- dbstat and dbpage are not compiled

## Files, environments, layout

- **One environment per directory.** A process-wide registry maps a
  directory to one `DB_ENV`, refcounted and mutex-protected. Every database
  file is opened in the environment of its own directory. This is the only
  way two processes opening the same file are guaranteed to share regions,
  locks and the log.
  - Open flags: `DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL |
    DB_INIT_TXN | DB_THREAD | DB_REGISTER | DB_RECOVER`, plus
    `set_flags(DB_MULTIVERSION | DB_TXN_NOWAIT)` and
    `set_lk_detect(DB_LOCK_DEFAULT)`. `DB_REGISTER` makes libdb run recovery
    only when a previous user of the environment died.
  - `DB_LOG_AUTO_REMOVE` is set, and a checkpoint runs after any commit that
    has grown the log by more than 1 MB.
  - An environment created by an incompatible libdb (`DB_VERSION_MISMATCH`)
    yields `DBSQL_CANTOPEN` with a clear message. It must never crash.
- **One SQL database is one libdb file.** It contains:
  - `__dbsql_meta`, a btree holding:
    - the 16 SQLite meta integers, keyed by 4-byte big-endian index;
    - a format record (`"dbsql" 1`) that identifies a dbsql file. A file
      without it is `DBSQL_NOTADB`.
  - `t<N>`, one btree per table or index. N is the SQLite "root page"
    number stored in `dbsql_schema.rootpage`. N=1 is the schema table.
    New numbers come from meta `LARGEST_ROOT_PAGE` + 1. Numbers are never
    reused or moved, so `__sm_drop_table` always reports `*moved = 0`.
- **Per-connection private environment.** This is `DB_PRIVATE`, with an
  in-memory log, `DB_INIT_TXN`, `DB_INIT_LOCK` and `DB_INIT_MPOOL`. It holds:
  - `:memory:` main databases and the temp database (temp tables, triggers
    and views), as named in-memory databases, which are transactional;
  - ephemeral tables and sorter spill (`OP_OpenEphemeral`, `BTREE_SINGLE`),
    as unnamed, non-transactional databases. libdb pages them to its temp
    directory when the cache fills.

## Keys and records

- **Rowid tables** (`BTREE_INTKEY`): the key is 8 bytes, big-endian, with
  the sign bit flipped, so memcmp order equals integer order. The data is the
  SQLite record. These tables use libdb's default comparator.
- **Indexes and WITHOUT ROWID tables** (`BTREE_BLOBKEY`): the key is the
  whole SQLite record (all key columns plus the rowid or PK suffix) and the
  data is empty. Entries are unique as full records, so `DB_DUP` is not used.
  UNIQUE constraints are enforced by the VDBE (`OP_NoConflict`), exactly as
  upstream.
- **Comparator `__sm_rec_cmp(DB*, DBT*, DBT*)`** (NORTH_STAR invariant 6):
  - **Bounded pre-validation.** Before any decode, it parses the page-side
    record's header with explicit bounds and checks that every serial-type
    length fits inside `dbt->size`. A malformed record compares greater than
    everything and reads nothing past its DBT. This keeps libdb's lock-free
    descent safe on torn pages.
  - **Semantics are delegated to upstream.** When one DBT carries the probe's
    `unpacked_record_t` in `app_data` (every VDBE seek and insert does this),
    the result is upstream's `__vdbe_record_compare` against it. Otherwise
    (libdb-internal paths such as split, delete and compaction) the two
    packed records are compared field by field with upstream's
    `__mem_compare` plus KeyInfo collations and sort flags. Either way the
    order is exactly SQLite's.
  - The comparator never calls libdb, never takes a libdb lock, and does not
    allocate. The one exception is a user collation registered for a
    different text encoding, which upstream also converts.
  - The index's `key_info_t` is copied into the `DB` handle's
    `app_private` before `DB->open` and is immutable for the handle's
    lifetime.

## Transactions: WAL-like semantics on MVCC

| SQLite | libdb |
|---|---|
| read transaction (`__sm_begin_trans(p, 0)`) | `txn_begin(DB_TXN_SNAPSHOT)`. Readers take no locks and see a snapshot from their start |
| write transaction (`__sm_begin_trans(p, 1)`) | the same txn, plus a **database write lock**: `lock_get(DB_LOCK_WRITE)` on the object `"<file>\0wr"`, taken with `DB_LOCK_NOWAIT` under the txn's locker and released at commit or abort. Only one writer per database at a time. A conflict returns `DBSQL_BUSY` from `__sm_begin_trans`, so upstream's busy-handler loop there works unchanged |
| upgrading a read txn whose snapshot is stale (another writer committed since it began) | `DBSQL_BUSY_SNAPSHOT`, as in SQLite WAL mode. A libdb update conflict on a newer page version maps to the same code |
| `__sm_begin_stmt` / `SAVEPOINT` | a stack of child txns. Level 0 is a child of the connection txn that holds all content. Each savepoint adds one level |
| `ROLLBACK TO i` | abort levels > i, then begin a fresh level i+1 |
| `RELEASE i` | commit levels ≥ i into their parents |
| rollback of the whole transaction while keeping it open (`iSavepoint < 0`) | abort level 0 and begin a new one |
| commit phase one / phase two | The connection has **one** top-level txn shared by every `sm_t` in the same environment (main and same-directory attachments). It commits once, at the first phase-two call. `vdbeCommit` always takes upstream's simple path; the super-journal path is not needed |
| `PRAGMA synchronous` | `OFF` → `DB_TXN_NOSYNC`, `NORMAL` → `DB_TXN_WRITE_NOSYNC`, `FULL`/`EXTRA` → sync |
| `DB_LOCK_DEADLOCK`, `DB_LOCK_NOTGRANTED` | `DBSQL_BUSY` |
| `DB_SNAPSHOT_CONFLICT`, `DB_SNAPSHOT_UNSAFE` | `DBSQL_BUSY_SNAPSHOT` |
| `DB_RUNRECOVERY` | `DBSQL_CORRUPT`. The connection is unusable until reopened |

Databases attached from another directory live in another environment. They
commit in that environment's own txn, so a transaction spanning two
directories is not atomic across them. This is listed in DIVERGENCES.md.

## Cursors

- Each `sm_cursor_t` has a `DBC` opened in the current innermost txn, plus
  per-cursor key and data buffers (`DB_DBT_REALLOC`) that hold the current
  entry. `__sm_payload_fetch` returns a pointer into the buffer that stays
  valid until the cursor moves, as the VDBE expects.
- Before a savepoint level is committed or aborted, every `DBC` opened in
  that level saves its key and is closed (the cursor state becomes
  `CURSOR_REQUIRESEEK`). The next access reopens it in the current level and
  seeks back with `DB_SET_RANGE`. On a rollback, `__sm_trip_all_cursors`
  faults cursors as upstream does.
- `moveto`: seek with `DB_SET_RANGE` to the smallest key ≥ the probe, falling
  back to `DB_LAST`. `*res` is the sign of the comparison between the
  cursor's entry and the probe, exactly as upstream.
- `insert`: `DBC->put(DB_KEYFIRST)` positions the cursor on the new entry.
  `delete`: `DBC->del`, and the cursor stays on the deleted slot, so next and
  prev behave as upstream.
- `count`: a cursor walk under the snapshot, exact.
- Incremental blob writes: read the row, patch it, `DBC->put(DB_CURRENT)`.

## Whole-database operations

| Operation | Implementation |
|---|---|
| `__sm_copy_file` (in-place VACUUM) | in the destination's write txn, drop every `t<N>`, copy each source sub-database and the meta |
| VACUUM INTO | upstream's SQL-level copy into the new file; no extra storage work |
| `dbsql_backup_*` | logical copy, one sub-database per step unit. `remaining` and `pagecount` count sub-databases (DIVERGENCES.md) |
| `PRAGMA incremental_vacuum` | `DB->compact(DB_FREE_SPACE)` |
| `PRAGMA integrity_check` | logical: keys strictly increasing under the comparator, rowid keys decode, entry counts. libdb's own page checks happen on every page read |
| `PRAGMA page_size` | the libdb page size for the file, settable until the first table exists |
| `page_count`, `freelist_count` | from `DB->stat(DB_FAST_STAT)` |

## Verified libdb behaviours (test/unit/probe_libdb.c, run on the build host)

- An environment opens with `DB_MULTIVERSION | DB_TXN_NOWAIT` and
  `DB_REGISTER | DB_RECOVER`.
- A `DB_TXN_SNAPSHOT` txn fixes its snapshot at its **first read**, not at
  `txn_begin`. An SQL read transaction always reads the schema cookie
  first, so this is equivalent.
- A stale snapshot txn that writes a page updated after its snapshot gets
  `DB_LOCK_DEADLOCK`, which maps to `DBSQL_BUSY_SNAPSHOT`.
- `lock_get(txn->id(), DB_LOCK_NOWAIT, obj, DB_LOCK_WRITE)` gives the
  database write lock. A second txn gets `DB_LOCK_NOTGRANTED`. The lock is
  released when the owning txn commits.
- Aborting a child txn undoes only the child's writes.
- A named in-memory database in a private env with an in-memory log honours
  txn abort.
- An unnamed temporary database in an mpool-only private env spills to its
  temp directory when the cache fills (10 MB into a 256 KB cache).

## Measured libdb constraints that shape the implementation (2026-10-08)

1. **Handle lifetime vs. txns.** A `DB` handle opened inside a txn that later
   **aborts** becomes unusable (`EINVAL`). A sub-database created inside an
   aborted txn does not exist afterwards (`ENOENT`). If the txn commits, the
   handle stays valid.
   → Cached sub-database handles are tagged with the txn level that opened
   them. When that level aborts, every handle it opened is discarded
   (closed without use) and reopened lazily on next use.
2. **Parent locked while a child is active.** While a child txn is active,
   `DB->open` and `put` on the parent return `EPERM`; `get` on the parent
   still works.
   → All opens and writes use the innermost txn (`stack[depth-1]`).
   Together with (1), this is why handles track their opening level.
3. **Opening a sub-database another txn is creating.** With `DB_TXN_NOWAIT`,
   an auto-commit `DB->open` of an existing sub-database fails with
   `DB_LOCK_DEADLOCK` when another txn holds an uncommitted `DB_CREATE` in
   the same file.
   → Opens always run inside the connection's current txn, never
   auto-commit. A `DB_LOCK_DEADLOCK` or `DB_LOCK_NOTGRANTED` from open maps
   to `DBSQL_BUSY`.
4. **Dropping a sub-database.** `dbremove` returns `DB_LOCK_NOTGRANTED`
   while any other handle on that sub-database is open (persistent and
   private envs alike), and other connections keep their handles.
   `DB->truncate` works with other handles open, and a reader's snapshot
   still sees the old rows (measured 2026-10-09).
   → `__sm_drop_table` truncates the table, deletes its kind record and
   retires its number (LARGEST_ROOT_PAGE only grows), then tries
   `dbremove` in a child txn; if another handle is open the empty
   sub-database stays until VACUUM. `__sm_clear_table` uses `truncate`.
5. **Page size and statistics.**
   - New btrees default to a 16 KB page. dbsql applies `PRAGMA page_size`
     (stored in meta) with `set_pagesize` before `DB->open` creates each
     sub-database.
   - `DB_FAST_STAT` returns 0 for `bt_nkeys` on a new btree, so counts use a
     cursor walk.
   - `DB_SET_RANGE` with an empty key positions on the first key.
6. **The comparator sees many entries per seek.** libdb's descent calls
   `bt_compare` on entries far from the landing position, and the record
   compare sets `unpacked_record_t.eq_seen` on any equal one.
   → `__sm_index_moveto` clears `eq_seen` after the descent and recomputes
   it from the landing entry and the one before it (an equal entry
   compares as `default_rc`, so it is one of the two).
7. **MVCC retains committed writers.** Under `DB_MULTIVERSION` a committed
   txn is retained until the last page it created leaves the cache, so
   "an additional transaction may be needed for each page in the cache"
   (libdb's transapp_read guide). With the default transaction table,
   ~3400 one-row write txns on a 32 MB cache fail with ENOMEM
   (`BDB4525 Unable to allocate memory for transaction detail`);
   `mutex_set_max` does not help, `set_tx_max` does (measured).
   → `set_tx_max(SM_TX_MAX)`: one per 512-byte page of cache, +25%.
8. **In-memory logs bound transaction size.** With `DB_LOG_IN_MEMORY` a txn
   larger than the log buffer fails with `DB_LOG_BUFFER_FULL`.
   → `:memory:` uses a 64 MB buffer (the error maps to `DBSQL_FULL`); temp
   databases get a private on-disk environment in a fresh directory,
   removed on close.
9. **The data file lags the log.** Committed pages reach the file at a
   checkpoint or on cache eviction.
   → After a write commit, `txn_checkpoint(SM_CKPT_KBYTES)`. A call below
   the threshold costs ~210 us (SIREAD cleanup runs before the threshold
   test; reported to libdb, .agent/libdb-bugs (copy in /tmp/libdb-bugs) R3), which dominates small
   autocommit writes until fixed upstream.
10. **An "equal" put revives a referenced deleted item (libdb bug,
    reported: .agent/libdb-bugs (copy in /tmp/libdb-bugs) R1).** A btree put of a key the comparator
    calls equal to a just-deleted item stores the deleted item's old key
    bytes when any cursor (including the deleting one) still refers to
    it. Visible as NOCASE or WITHOUT ROWID key updates keeping the old
    spelling (without_rowid1-2.1.2, upfrom2). Not worked around.
11. **`DB->compact` then abort (libdb bug, reported: .agent/libdb-bugs (copy in /tmp/libdb-bugs) R4).**
    Aborting a txn that ran `DB->compact(DB_FREE_SPACE)` on a
    sub-database after another sub-database of the file was `dbremove`d
    in the same txn fails "old_lp->gen == old_lock->gen" in
    `__lock_change` during undo (panic in release builds). dbsql reaches
    it when a VACUUM, `incremental_vacuum` or an auto_vacuum=FULL commit
    is rolled back after DROP TABLE. Not worked around; waiting on libdb.
