# Storage manager (src/sm/): SQL on libdb

The VDBE talks to storage only through `src/inc/sm.h`. That header is
SQLite's btree.h interface (96 functions; the VDBE and code generator call 95
of them), renamed by the transform (`sqlite3BtreeFoo` becomes `__sm_foo`, `Btree` becomes `sm_t`, `BtCursor` becomes `sm_cursor_t`), with
**identical semantics**. It is implemented entirely on libdb's public API in
`src/sm/`. This file is the design of record. Change it when the design
changes.

**Why keep the btree.h seam.** It is the interface every layer above storage
was written against: the VDBE, code generation, and pager-adjacent calls in
main.c, pragma.c, vacuum.c and backup.c. Keeping it means the transformed
upstream code needs no storage edits, and every later SQLite release merges
mechanically. The pager-level calls those files make (`sqlite3Pager*`, about
25 functions) get a thin shim in `src/sm/sm_pager.c` that answers from libdb
or reports fixed values (see DIVERGENCES.md).

**Bring-up order.**
1. A transformed build that still uses SQLite's own btree, pager and os
   layers (`--with-storage=native`, build-only, never shipped). SQLite's test
   suite must pass on it. This proves the transform preserves semantics.
2. Swap in `src/sm/` (`--with-storage=libdb`, the default and the only
   shipped configuration). Failures from this point on are in sm/ or are
   listed divergences.

## Layout

- **Environment.** A `DBSQL` handle owns, or joins, a `DB_ENV`. Its home
  directory comes from `dbsql_create_env(&dbp, dir, ...)`, or by default is the
  directory of the first opened file. Flags are `DB_CREATE | DB_INIT_LOCK |
  DB_INIT_LOG | DB_INIT_MPOOL | DB_INIT_TXN | DB_THREAD`, plus
  `DB_REGISTER | DB_RECOVER` so that the first opener after a crash runs
  recovery automatically. `DB_PRIVATE`, an in-memory log and in-memory
  databases are used for `:memory:` and temp.
- **One SQL database is one libdb file.** Each table and index is a
  sub-database of that file, named `t<N>` (or `i<N>`), where N is the
  "root page" number the schema stores. N comes from a per-file
  `DB_SEQUENCE` held in the `__dbsql_meta` sub-database.
- **Meta.** The 16 SQLite meta integers (schema cookie, file format, user
  version, application id, …) are records in `__dbsql_meta`, keyed by a
  4-byte big-endian index.
- **Rowid tables** are `DB_BTREE` databases. The key is the rowid encoded as
  8 bytes big-endian with the sign bit flipped, so memcmp order equals integer
  order. The data is the SQLite record payload. They use the default (memcmp)
  comparator, which is fast and pure.
- **Indexes and WITHOUT ROWID tables** are `DB_BTREE` databases whose key is
  the SQLite record, with empty data. A custom comparator (`__sm_rec_compare`)
  decodes both records against the index's `key_info_t` (collations, DESC,
  number of fields) and compares them field by field.
  - The comparator is **pure and bounds-checked** (NORTH_STAR invariant 6):
    every varint and serial-type read is checked against `dbt->size`, a
    malformed record compares as greater than any well-formed one, and the
    comparator never allocates.
  - Collation callbacks run inside it, so user collations must be pure too.
    This is documented.
  - `key_info_t` is attached to the `DB` handle via `DB->app_private` before
    `DB->open` and is immutable for the lifetime of the handle.
- **Ephemeral tables and sorter.** These use in-memory, unnamed libdb
  databases in the connection's private temp environment. The sorter may
  keep using SQLite's vdbesort in-memory merge, spilling to a temp libdb
  database instead of a temp file.

## Transactions

| SQLite operation | libdb |
|---|---|
| read transaction | none needed for autocommit reads with `DB_READ_COMMITTED` cursors; inside BEGIN, the connection txn |
| `BEGIN` / first write | `txn_begin(env, NULL, &txn, 0)`. With `PRAGMA isolation=snapshot` the env is opened with `DB_MULTIVERSION` and reads use `DB_TXN_SNAPSHOT`; with `serializable`, `DB_TXN_SERIALIZABLE` (SSI) |
| statement journal (OP_Transaction with a statement id) | child txn of the connection txn, committed or aborted at statement end |
| `SAVEPOINT name` | child txn pushed on the savepoint stack |
| `RELEASE name` | commit the child txns up to and including name |
| `ROLLBACK TO name` | abort the child txns down to name, then begin a fresh child named name |
| `COMMIT` | `txn->commit(txn, 0)` (`DB_TXN_NOSYNC` when `PRAGMA synchronous=OFF`) |
| `DB_LOCK_DEADLOCK`, `DB_LOCK_NOTGRANTED`, `DB_SNAPSHOT_CONFLICT`, `DB_SNAPSHOT_UNSAFE` | `DBSQL_BUSY`. The statement rolls back to its start, and the busy handler or timeout decides whether to retry |
| `DB_RUNRECOVERY` | `DBSQL_RUNRECOVERY`. The connection becomes unusable |

The deadlock detector runs on every lock conflict
(`set_lk_detect(DB_LOCK_DEFAULT)`). `busy_timeout` maps to
`set_timeout(DB_SET_LOCK_TIMEOUT)` together with retry in the busy handler.

## Operations to libdb mapping (summary)

| sm.h | libdb |
|---|---|
| cursor open/close | `DB->cursor(txn, &dbc, DB_READ_COMMITTED or 0)` / `DBC->close` |
| table_moveto(rowid) | `DBC->get(DB_SET_RANGE)` on the encoded key, then compare |
| index_moveto(unpacked key) | `DB_SET_RANGE` with the packed probe, then compare with the comparator semantics |
| first/last/next/prev | `DB_FIRST`/`DB_LAST`/`DB_NEXT`/`DB_PREV` |
| payload, payload_fetch | the cursor's current DBT (`DB_DBT_USERMEM` buffer, grown on `DB_BUFFER_SMALL`) |
| insert | `DBC->put(DB_KEYLAST)` or `DB->put` |
| delete | `DBC->del(0)` |
| clear_table | `DB->truncate` |
| drop_table | close the handle, then `DB->remove(file, subdb)` inside the txn |
| count | `DB->stat(DB_FAST_STAT)` when exact; otherwise a cursor walk |
| integrity_check | `DB->verify` per sub-database, plus SQL-level checks |
| VACUUM | `DB->compact(DB_FREE_SPACE)` on each sub-database |
| VACUUM INTO / backup API | `DB_ENV->dbbackup` for the file, then rewrite the environment |
| incremental blob I/O | `DB_DBT_PARTIAL` get/put on the row |

## Concurrency

- Each connection uses its own `DB` handles, opened with `DB_THREAD`.
- `set_mutex_increment` is sized for (tables + indexes) × 9 mutexes per
  handle, because the master branch allocates 8 cursor-queue mutexes per
  threaded handle.
- Schema changes bump the schema cookie inside the writing transaction. Every
  statement verifies the cookie at OP_Transaction and returns
  `DBSQL_SCHEMA` (then re-prepares) on a mismatch, exactly as in SQLite.
