# libdb defects found while building dbsql on libdb

Reporter: dbsql (SQLite 3.53.4 SQL layer on libdb).  Found by SQLite's
test suite running on the dbsql storage manager, then reduced to plain
libdb programs with controls.

* libdb: **2026.10.3 (October 9, 2026)**, tag `v2026.10.3` = 98749ce0c,
  built `--enable-debug --enable-diagnostic`.
* Host: Debian 13 (trixie) amd64, clang 19.1.7, glibc 2.41,
  Linux 6.12.111.  Also seen on FreeBSD 15.1 with v2026.10.2 (R4, R5).
* Every reproducer is a single C file in this directory.  Build line is
  in each file's header; `LIBDB=` is the install prefix.  Each one prints
  the libdb version and runs its controls first.

| # | Severity | Summary | File |
|---|---|---|---|
| R1 | data integrity | Btree put of a key the comparator calls equal to a deleted-but-still-referenced item revives that item with its **old key bytes** | r1_revive.c |
| R2 | robustness / docs | With `DB_MULTIVERSION`, ~3400 small committed write txns on default sizing exhaust the txn region: `ENOMEM` ("BDB4525 Unable to allocate memory for transaction detail") with nothing active | r2_mvcc_enomem.c |
| R3 | performance | `txn_checkpoint(kbytes)` below its threshold costs ~210 µs/call (47x a one-row write txn): `__lock_sicleanup` runs before the threshold test | r3_ckpt_cost.c |
| R4 | crash (environment panic) | Abort of a txn that did `dbremove` of one sub-database then `DB->compact(DB_FREE_SPACE)` of another in the same file: assertion `old_lp->gen == old_lock->gen` in `__lock_change` during undo | r4_compact_abort.c |
| R5 | crash (environment panic) | With `DB_MULTIVERSION` + `DB_TXN_SNAPSHOT`, a txn whose put fails with `ENOMEM` (cache / mutex region exhausted) cannot be aborted: undo panics (`BDB0057 unable to create/retrieve page`, `BDB0061 PANIC: Cannot allocate memory`) | r5_abort_after_enomem.c |

Details follow.  "Expected" is quoted from libdb's own documentation
where it says so (docs_src/api/c/*.md in the release tree).

---

## R1 — equal-key put revives a deleted item with its old key bytes

**Observed.**  B-tree database with a custom `bt_compare` under which
`"abc"` and `"ABC"` compare equal (case-insensitive).  Put `"abc"`.
Delete it through cursor A.  Put `"ABC"` through cursor B with
`DB_KEYLAST`.  The database then contains **`"abc"`**, the deleted
item's bytes, whenever any cursor still references the deleted item:
either another open cursor that was positioned on it, or cursor A itself
when the put is made through A.

```
$ ./r1
libdb 2026.10.3 (October 9, 2026)
control (deleter closed)       no-env stored key: "ABC"  (expected)
another cursor on the item     no-env stored key: "abc"  (UNEXPECTED: old bytes)
same cursor deletes and puts   no-env stored key: "abc"  (UNEXPECTED: old bytes)
control (deleter closed)       txn    stored key: "ABC"  (expected)
another cursor on the item     txn    stored key: "abc"  (UNEXPECTED: old bytes)
same cursor deletes and puts   txn    stored key: "abc"  (UNEXPECTED: old bytes)
```

**Expected.**  The item was deleted; the put inserts a new key/data pair
(`DBcursor->put` DB_KEYLAST: "Insert the specified key/data pair into the
database"), so the stored key is `"ABC"`, the same as the control where
no cursor references the deleted slot.  Whether a deleted item is still
physically on the page (because a cursor references it) must not be
observable through a later insert.

**Analysis.**  `__bamc_put` (btree/bt_cursor.c ~2143) searches with
`__bamc_search`; on an exact match it sets `iiop = DB_CURRENT` and
`__bam_iitem` overwrites the slot's **data** item in place, keeping the
key item (bt_put.c ~403, "Delete and re-add the data item" / "Overwrite
the data item").  For a live item that is libdb's documented "replace
the existing key" behaviour (the stored key bytes are kept; documented
as the same key).  But the matched slot here is a **deleted** item
(`B_DISSET`, kept on the page only because a cursor references it,
`IS_CUR_DELETED`).  Reviving it with the old key bytes makes a
cursor-position artefact visible: the result of an insert depends on
whether some unrelated cursor happened to be open.  Suggested fix: when
the exact match is a deleted item, replace the key item too (or insert
as for a non-match).

**Impact on dbsql.**  SQL `UPDATE t SET a = 'ABC'` on a `COLLATE NOCASE`
indexed or `WITHOUT ROWID` primary-key column keeps the old spelling
`'abc'` (SQLite tests without_rowid1-2.1.2, -2.4.2, upfrom2-1.2.13).
dbsql deletes the old entry and inserts the new one, which is the
natural pattern for any application with a collating comparator.

---

## R2 — MVCC write transactions exhaust the txn region (ENOMEM) at default sizing

**Observed.**  `DB_MULTIVERSION` environment, 32 MB cache, 1 KB pages.
A loop of one-row write transactions (each committed) fails at the
3395th `txn_begin` with `ENOMEM`, "BDB4525 Unable to allocate memory for
transaction detail", while no transaction is active:

```
$ for m in nomvcc plain snapshot ckpt txmax; do ./r2 $m; done
nomvcc    20000/20000 commits; committed snapshot txn details held: 0;     mutexes in use 14850 of 15057
plain     txn_begin #3395: Cannot allocate memory
plain      3395/20000 commits; committed snapshot txn details held: 2115;  mutexes in use 9906 of 15057
snapshot  txn_begin #3395: Cannot allocate memory
snapshot   3395/20000 commits; committed snapshot txn details held: 2115;  mutexes in use 9906 of 15057
ckpt      txn_begin #5188: Cannot allocate memory
txmax     20000/20000 commits; committed snapshot txn details held: 18698; mutexes in use 43529 of 50815
```

* `plain` (MVCC environment, writers **without** `DB_TXN_SNAPSHOT`) fails
  the same way as `snapshot`.
* Periodic `txn_checkpoint(DB_FORCE)` only delays it.
* `mutex_set_max(262144)` has **no** effect (variant `mutexmax`).
* `set_tx_max(40000)` avoids it.

**Expected / documentation.**  The programmer's reference
(transapp_read.md) does say "the transactions are retained until the
last page they created is evicted from cache, so in the extreme case, an
additional transaction may be needed for each page in the cache", and
that `set_tx_max` must allow for it.  So this may be "working as
documented", but:

1. The default (`set_tx_max` unset) fails a trivially simple workload
   (one-row commits in a loop) within seconds, with an error that names
   memory rather than the txn table.  Either the default should scale
   with the cache when `DB_MULTIVERSION` is set, or the region should
   grow (the environment has `set_memory_max` headroom).
2. Retained details are not reclaimed when they could be: 2115 of 3395
   committed writers are still held with an idle environment and a cache
   far from full (32 MB, ~3.7 MB written).  If no snapshot reader can see
   a page version older than the newest committed one, the older
   versions (and their txn details) are reclaimable.
3. The error text should say which limit was hit (tx_max) and how to
   raise it.

**Impact on dbsql.**  "out of memory" after a few thousand autocommit
INSERTs (SQLite test backup.test).  dbsql now sizes `set_tx_max` from its
cache size as documented.

---

## R3 — `txn_checkpoint(kbytes)` is expensive even when it does nothing

**Observed.**  2000 one-row `DB_TXN_SNAPSHOT` write txns; after each,
optionally call `txn_checkpoint`:

```
$ ./r3
libdb 2026.10.3 (October 9, 2026)
no txn_checkpoint call           4.5 us per write txn
txn_checkpoint(4096) (never)   214.2 us per write txn
txn_checkpoint(0) (always)     237.5 us per write txn
```

With a 4 MB threshold that is never reached (the whole run writes
~0.6 MB of log), each call still costs ~210 µs, 90% of what a real
checkpoint costs.  perf: `__lock_sicleanup` and the mutexes it takes
dominate (17.5% self in `__lock_sicleanup`, ~50% in
`__db_tas_mutex_lock/unlock` under it).

**Analysis.**  `__txn_checkpoint` (txn/txn_chkpt.c ~150) calls
`__lock_sicleanup(env)` ("a convenient, infrequent point at which to
reclaim SIREAD markers") **before** taking `mtx_ckp` and before the
`kbytes`/`minutes` threshold test (~180).  With a threshold the call is
meant to be cheap when it does nothing (the usual "call after every
commit, checkpoint every N KB" idiom from the documentation), and it is
not "infrequent" in that use.  Suggested fix: do the threshold test
first and run the SIREAD cleanup only when a checkpoint is actually
taken (or rate-limit it independently).

**Impact on dbsql.**  dbsql calls `txn_checkpoint(256, 0, 0)` after each
write commit so the database file tracks its contents; that makes a
one-row autocommit INSERT ~20x slower (2000 rows: 1.1 s vs 0.06 s).

---

## R4 — abort after dbremove + compact in one txn: assertion/panic in `__lock_change`

**Observed.**  File `f.db` with sub-databases `b` (created first, low
pages) and `a`, 4000 rows each.  In one transaction T: `dbremove(f.db,
b)`, then `a->compact(T, …, DB_FREE_SPACE)`, close `a`, `T->abort()`:

```
$ for m in commit nocompact noremove full; do ./r4 $m; done
libdb 2026.10.3 (October 9, 2026): commit    commit: ok
libdb 2026.10.3 (October 9, 2026): nocompact abort: ok
libdb 2026.10.3 (October 9, 2026): noremove  abort: ok
BDB0059 assert failure: ../src/lock/lock.c/2669: "old_lp->gen == old_lock->gen"
Aborted
```

```
#5  __lock_change            ../src/lock/lock.c:2669
#6  __db_merge_recover       ../src/db/db_rec.c:2683
#7  __db_dispatch            ../src/db/db_dispatch.c:313
#8  __txn_dispatch_undo      ../src/txn/txn.c:2083
#9  __txn_undo               ../src/txn/txn.c:2177
#10 __txn_abort              ../src/txn/txn.c:1362
```

Each step alone, and the same steps committed, are fine.

**Analysis.**  Compaction with `DB_FREE_SPACE` moves `a`'s metadata page
into the space `b` freed and moves the handle lock with it
(`__db_move_metadata`, `__lock_change`).  Undoing the merge
(`__db_merge_recover`, db_rec.c ~2660-2685) re-locks the handle
(`__fop_lock_handle`) and calls `__lock_change(&handle_lock,
&file_dbp->handle_lock)` with a `handle_lock` whose lock entry has been
released or reused (generation mismatch): the handle the undo uses is
the recovery file handle, whose `handle_lock` is not the lock the
forward operation moved, and by then `a`'s user handle has been closed.
Release builds skip the assertion and corrupt the lock table.

**Impact on dbsql.**  Rolling back a VACUUM, `PRAGMA
incremental_vacuum`, or an `auto_vacuum=FULL` commit after a `DROP TABLE`
in the same transaction panics the environment (SQLite test
vacuum.test).

---

## R5 — MVCC txn cannot be aborted after a put fails with ENOMEM

**Observed.**  Default environment sizing.  One transaction inserts
200-byte rows (1 KB pages) until a put fails, then aborts:

```
$ for m in abortbig abortbig-nested abortbig-mvcc abortbig-nested-mvcc; do ./r5 $m; done
abortbig        14066 puts (Cannot allocate memory), abort: ok
abortbig-nested 14050 puts (Cannot allocate memory), abort: ok
abortbig-mvcc         1892 puts, last put: Cannot allocate memory; now aborting
                      BDB2034 unable to allocate memory for mutex; resize mutex region  (x182)
                      BDB3017 unable to allocate space from the buffer cache
                      BDB0057 unable to create/retrieve page 622
                      BDB0061 PANIC: Cannot allocate memory
abortbig-nested-mvcc  (same, inside parent/child/grandchild txns)
```

```
#5  __env_panic              ../src/common/db_err.c:237
#6  __db_pgerr               ../src/common/db_err.c:111
#7  __db_addrem_recover      ../src/db/db_rec.c:59
#8  __db_dispatch            ../src/db/db_dispatch.c:313
#9  __txn_dispatch_undo      ../src/txn/txn.c:2083
#10 __txn_undo / __txn_abort ../src/txn/txn.c:1362
```

Without `DB_MULTIVERSION` the same txn (14k puts, fails on lock entries)
aborts cleanly.

**Expected.**  An operation that fails for lack of resources leaves the
transaction abortable; abort must not need more of the resource that ran
out.  Undo here needs a fresh page version (MVCC copy-on-write) for every
page it touches and fails to get one because the cache and mutex region
are exhausted by the txn's own copies, then panics.  The abort path
should be able to reuse the aborting txn's own page versions (they are
being discarded) or reserve what undo needs.

**Impact on dbsql.**  A large statement that runs out of cache panics the
whole environment (every connection, `DB_RUNRECOVERY`) instead of
failing the statement.  Before dbsql sized its regions, SQLite's
index4.test (a 64k-row INSERT…SELECT) and closure01 hit this; with larger
regions the threshold moves but the failure mode remains.
