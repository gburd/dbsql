# libdb defects found while building dbsql on libdb

Reporter: dbsql (SQLite 3.53.4 SQL layer on libdb).  Found by SQLite's
test suite running on the dbsql storage manager, then reduced to plain
libdb programs with controls.

* libdb: **2026.10.3 (October 9, 2026)**, tag `v2026.10.3` = 98749ce0c,
  built `--enable-debug --enable-diagnostic`.
* **Re-verified on 2026.10.4 (October 10, 2026)**, tag `v2026.10.4` =
  669f7ec31, same build: R1, R2, R3, R4, R5, R6, R7, R9 and R11
  reproduce unchanged, and every control still passes.  R10 has no
  standalone reproducer yet (source path documented).
* **Re-verified and fixed on 2026.10.5 (October 10, 2026)**, tag
  `v2026.10.5` = 553b649a1.  R1, R6, R7, R9, R10 and R11 all reproduce.
  R10 now has a deterministic standalone reproducer (EMFILE injected
  through `db_env_set_func_open`).  **Fixes for R7, R9, R10 and R11** are
  in `patches/` (git-format-patch, branch `fixes` off `v2026.10.5`),
  each with a self-checking regression test under `test/db/` and a
  runner registered in `test/MANIFEST`; each "Fix" subsection below
  gives the detail.  R6 is a documented design behavior (see its Fix
  note).  Verified with an A/B of unpatched vs patched, diagnostic and
  release builds, and a txn/fop/recd/sdb/compact suite slice shows no
  regression.
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
| R6 | crash (environment panic) | With `DB_REGISTER \| DB_RECOVER` (the documented multi-process recipe), a process that exits without closing the environment makes the next opener recover the environment out from under every live process: their next operation fails with `DB_RUNRECOVERY` / PANIC.  The documented hybrid (`DB_FAILCHK` + `is_alive`) avoids that but trips diagnostic assertions in single-process use | r6_register.c |
| R9 | crash (process abort) | `DB_REGISTER \| DB_RECOVER` open that must recover (a registrant died) meets a log record for a file that is now a non-libdb (e.g. empty) file: recovery fails, and `__env_attach_regions` **panics and aborts the process** instead of returning an error | r9_recover_foreign_file.c |
| R10 | crash (environment panic) | Out of file descriptors (`EMFILE`) when a log write must (re)open the log file panics the environment: `__log_name` treats any open error other than `ENOENT` as fatal (`"log file unreadable"` -> `__env_panic`) | r10_log_emfile.c (partial) |
| R11 | hang | In one txn: `dbremove` a sub-database, then `DB->compact(DB_FREE_SPACE)` the others while a second handle on one of them is open.  Compaction moves a metadata page into the removed one's page and `__db_move_metadata` waits forever on the handle lock (`__fop_lock_handle` -> blocking `__lock_get`) despite `DB_TXN_NOWAIT` and the deadlock detector | r11_compact_after_remove.c |
| R7 | memory leak | `DB_ENV->dbremove` of an in-memory named database inside a txn that is then **aborted** leaks the txn event `__db_inmem_remove` queued with `__txn_remevent` (249 bytes, 6 allocations, per remove) | r7_inmem_remove_leak.c |

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

---

## R6 — DB_REGISTER recovery runs under live processes

**Observed.**  Process A opens an environment with `DB_CREATE |
DB_INIT_* | DB_THREAD | DB_REGISTER | DB_RECOVER`, creates a database and
writes a record, and keeps the environment open.  Process B opens the
same environment and exits *without* `DB_ENV->close` (an ordinary
`exit(0)`; the same happens on SIGKILL).  Process C then opens it: its
registry scan finds B's slot unlocked, so it runs recovery.  A's next
`DB->get` returns `DB_RUNRECOVERY` and the environment panics.

```
$ ./r6 noclose          # B exits without close
noclose: A get after B,C: -30973 (BDB0087 DB_RUNRECOVERY: Fatal error, run database recovery)
BDB0060 PANIC: fatal region error detected; run recovery
$ ./r6 close            # control: B closes the env
close: A get after B,C: 0 (ok)
$ ./r6 noclose hybrid   # DB_FAILCHK + set_isalive + set_thread_count
noclose: A get after B,C: 0 (ok)          (6/6 runs)
```

**Expected.**  `transapp_app.md` describes `DB_REGISTER` as the
built-in way for groups of unrelated processes to share an environment
and recover after one dies.  It says recovery discards the environment
"from underneath" live processes and that they should notice and exit
(`DB_EVENT_REG_PANIC`).  So this may be working as documented, but the
consequence is severe: any process that exits without closing (a shell
`exit()` with the handle still open, a crash) makes every other
process's open handles unusable.  The documented hybrid
(`DB_REGISTER | DB_FAILCHK` with `is_alive`) avoids the full recovery.
Two questions for libdb:
1. Is it intended that a *clean* `exit()` (no crash, no held locks, no
   active transactions) of a registered process forces full recovery
   under live peers?  `__envreg_register` could tell a dead process with
   no outstanding state (failchk-able) from one that needs recovery.
2. If yes, the docs for `DB_REGISTER` should say plainly that it is
   only safe when every process always closes the environment, and point
   to the hybrid.

**dbsql impact.**  dbsql uses `DB_REGISTER | DB_RECOVER`.  Any process
that exits with a dbsql database open (the shell's `usage()` path, a
Tcl script that exits without `db close`, a crash) makes the next
opener recover the environment, and a connection open in a third
process then reports "database disk image is malformed".  SQLite's
shell1.test (shell1-3.24.1) and savepoint4.test hit this;
test/bdb/snapshot.test reports it as KNOWN.

**The hybrid does not help in practice.**  dbsql tried
`DB_REGISTER | DB_RECOVER | DB_FAILCHK` with `set_isalive` and
`set_thread_count(256)` (commit 089065a).  R6's reproducer passes, but
with thread tracking on, libdb (built `--enable-diagnostic`) aborts in
ordinary single-process use: SQLite's select1, malloc, ioerr,
writecrash, symlink, tkt4018 and the crash* tests crash.  Two of the
assertions, not yet reduced to standalone programs:

```
__memp_fget mp_fget.c:1261   (DIAGNOSTIC: dirty page pinned without a
  DB_LOCK_WRITE page lock, __db_has_pagelock) from
  __db_master_update(MU_REMOVE) <- __db_subdb_remove <- DB_ENV->dbremove
  of a sub-database inside a child txn  (select1: DROP TABLE)
lo_report mut_order.c:409 "same latch acquired twice by one thread
  (self-deadlock)" from __dbc_cleanup <- __db_master_update(MU_OPEN)
  <- __fop_subdb_setup <- DB->open of a sub-database after failchk
  freed a dead process's mutexes ("BDB2017 Freeing mutex")
```

dbsql therefore stays on `DB_REGISTER | DB_RECOVER` and lists the
affected tests as known failures against R6.

---

## R7 — dbremove of an in-memory database in an aborted txn leaks

**Observed.**  In a private environment with an in-memory log, create
an in-memory named database (`DB->open(..., NULL, "sub", ...)`) and
commit.  Begin a txn, begin a child, `DB_ENV->dbremove(child, NULL,
"sub")`, then abort the parent.  LeakSanitizer reports the event
allocated by `__txn_remevent` (from `__db_inmem_remove`):

```
$ ./r7 top            # dbremove in the top txn, commit: no leak
$ ./r7 child-commit   # child, commit both: no leak
$ ./r7 child-abort    # child, abort parent:
Direct leak of 73 byte(s) in 1 object(s) allocated from:
    #3 __txn_remevent txn/txn_util.c:98
    #4 __db_inmem_remove db/db_remove.c:371
    #5 __fop_inmem_swap fileops/fop_util.c:1852
SUMMARY: AddressSanitizer: 249 byte(s) leaked in 6 allocation(s).
```

**Expected.**  An aborted txn frees its queued events (the commit path
does).

**dbsql impact.**  `SAVEPOINT x; DROP TABLE t; ROLLBACK` on a
`:memory:` database leaks ~250 bytes per drop.  Found by dbsql's
libFuzzer target.  Listed as a known leak; not worked around.

**Fix** (`patches/0001-fix-txn-*.patch`).  `__txn_doevents`, at the
`dofree:` label (`src/txn/txn_util.c`), kept a child transaction's
`TXN_REMOVE` event "for the parent" on abort as well as on commit, but
only a commit hands the event over.  On abort the event, its name and
file id were in no list and leaked.  Keep the event only when a
committing child hands it to its parent; free it on abort.  Regression
test `test/db/txn_remevent_leak.c` counts every libdb allocation
through `db_env_set_func_malloc/realloc/free` (no leak detector
needed): in-memory and on-disk removes in a child, aborted via the
parent or the child, leak nothing; controls (commit through child and
parent, top-level abort, no remove) are unchanged and a committed
remove still removes.

---

## R9 — recovery that meets a foreign file aborts the process

**Observed.**  A writes `f.db` in an environment opened with
`DB_REGISTER | DB_RECOVER`; child B joins, begins a write txn on `f.db`
and is SIGKILLed.  A closes, replaces `f.db` with a zero-length file,
and reopens the environment.  The open runs recovery (B is dead),
recovery cannot read `f.db`, and the process aborts:

```
$ ./r9 keep     # control
keep: reopen: ok
$ ./r9 delete   # f.db removed: recovery copes
delete: reopen: ok
$ ./r9 empty    # f.db replaced by an empty file
BDB0004 fop_read_meta: /tmp/r9.../f.db: unexpected file type or format
BDB1521 Recovery function for LSN 1 8589 failed
BDB0061 PANIC: Invalid argument
(abort)
```

**Expected.**  `DB_ENV->open` returns an error (`DB_RUNRECOVERY` or
`EINVAL`) to the caller.  `__env_attach_regions`'s error path calls
`__env_panic`, which aborts under `DB_ENV->set_flags(DB_PANIC_ENVIRONMENT)`
semantics even though the application never asked for that.

**dbsql impact.**  dbsql created an empty placeholder file at open
before opening the environment, so a reopen after a crashed writer and
a deleted database aborted (SQLite's crash8, ioerr, malloc).  dbsql now
creates the placeholder only after the environment is open, which is
correct regardless; the libdb defect remains for any foreign file.

**Fix** (`patches/0004-fix-recover-*.patch`).  Recovery already
tolerates a *missing* referenced file (ENOENT): `__dbreg_open_file`
records the failed open and marks the fileid deleted so later records
for it are skipped, and `__dbreg_register_recover` maps ENOENT to
success.  A file that still exists but is foreign/empty surfaces as
EINVAL from `__fop_read_meta` and was *not* tolerated, so it failed all
of recovery and the environment-open path panicked (abort, in
diagnostic builds).  Treat EINVAL exactly like ENOENT in both places:
`__dbreg_do_open` (`src/dbreg/dbreg_util.c`) skips the in-memory retry
and records the failed open / marks the fileid deleted; and
`__dbreg_register_recover` (`src/dbreg/dbreg_rec.c`) maps EINVAL to
`ret = 0`.  Everything else recovers; only the foreign file is skipped.
Regression test `test/db/recover_foreign_file.c` SIGKILLs a registered
peer so recovery runs, replaces one database with an empty file,
reopens, and checks the reopen succeeds, a second untouched database
kept its committed record, and the environment was not panicked.

---

## R10 — EMFILE while (re)opening the log panics the environment

**Observed (dbsql).**  SQLite's misc7.test exhausts the process's file
descriptors (`do_fileopen_test`) and runs a write transaction.  When the
commit's log flush needs to open a log file, libdb aborts the process:

```
#5 __env_panic (errval=24 /* EMFILE */)
#6 __log_name (...) log_put.c:1794
#7 __log_valid
#8 __log_newfh (create=0)
#9 __log_write
#10 __log_flush_int
...  __txn_commit
```

**Cause (source).**  `src/log/log_put.c`, `__log_name`:

```c
	if ((ret = __os_open(env, *namep, 0, flags, mode, fhpp)) == 0)
		return (0);
	/* If the open failed for reason other than the file not being
	 * there, complain loudly, the wrong user probably started up the
	 * application. */
	if (ret != ENOENT) {
		__db_err(env, ret, DB_STR_A("2520", "%s: log file unreadable", ...));
		return (__env_panic(env, ret));
	}
```

`EMFILE`/`ENFILE` (and `ENOMEM`) are transient resource errors, not a
permission problem; they should fail the operation (`ENOMEM`/`EMFILE`
to the caller) and leave the environment usable.

**Reproducer.**  `r10_log_emfile.c` now injects the condition
deterministically with `db_env_set_func_open`: the installed open
returns `EMFILE` for log files only, so no real descriptor exhaustion
is needed and the test harness is not perturbed.  It forces a log roll
and confirms the write returns `EMFILE` and a subsequent read does not
return `DB_RUNRECOVERY` (the environment survived).  On 2026.10.5 the
unpatched library panics the environment (aborts, under DIAGNOSTIC);
the control (`fdok`) is clean.

**dbsql impact.**  misc7 (out-of-descriptors tests) aborts; listed in
known-failures against R10.

**Fix** (`patches/0003-fix-log-*.patch`).  In `__log_name`, return
`EMFILE`/`ENFILE` to the caller (as the enclosing commit / log-flush
error) instead of panicking: `__os_open` already retries these three
times, and when one finally surfaces it is a transient, process-local
descriptor shortage, not corruption and not a wrong-user mistake.
Panicking would take down every thread and process sharing the
environment.  Genuine "wrong user / corruption" opens (any other
non-ENOENT error) still panic as before.  Regression test
`test/db/log_emfile_nopanic.c` injects EMFILE through
`db_env_set_func_open`, forces a log roll, and checks the write returns
EMFILE and the environment is not panicked.

---

## Unreduced: crashes on deliberately corrupted files

SQLite's corruption tests overwrite bytes of the database file
(`corruptC`, `pagerfault`, `fuzz3`).  dbsql's file is a libdb file, so
these test libdb's own robustness against corrupt pages.  Two of them
abort inside libdb instead of returning `DB_RUNRECOVERY` / an error.
Recorded here for libdb; not reduced to standalone programs.

```
corruptC-3.5.(0).502.6  (ASan, --enable-diagnostic)
  SEGV in lo_alloc_id mut_order.c:272   (fname->mutex = 0xdbdb... freed)
  <- __db_lo_check <- __txn_dref_fname txn_util.c:492
  <- __txn_end <- __txn_abort <- DB_TXN->abort
pagerfault
  __os_abort <- __db_goff <- __db_moff <- __bam_cmp <- __bam_search
  (an overflow item on a corrupted page)
```

---

## R11 — compact after dbremove waits forever on a handle lock

**Observed.**  Environment with `DB_MULTIVERSION | DB_TXN_NOWAIT`,
`set_lk_detect(DB_LOCK_DEFAULT)`.  File `f.db` with sub-databases s0..s3
(one record each), committed.  A second handle on s2 is opened (as
another connection would).  Then, in one txn: `dbremove(s1)` in a
child (committed into the parent), then `DB->compact(DB_FREE_SPACE)`
on s0, s2, s3.  The compact of s2 never returns:

```
$ ./r11 noremove      # control
noremove: compact s0 0 / s2 0 / s3 0, commit 0
$ ./r11 remove
remove: dbremove s1 0
remove: compact s0 0
HUNG: compact did not return within 10 s
```

Stack (dbsql, same shape): `__db_compact_pp` -> `__bam_truncate_ipages`
-> `__db_move_metadata` -> `__fop_lock_handle` -> `__lock_get` ->
`__db_tas_mutex_lock` (a blocking wait).  `db_stat -Co` shows the
compacting txn's locker holding the new metadata page's handle lock
WRITE and the second handle's locker WAITing READ on it, and vice versa
on the old one.

**Expected.**  `DB_LOCK_NOTGRANTED` / `DB_LOCK_DEADLOCK` (the
environment asked for no waiting and runs the detector), or compaction
that does not move a sub-database's metadata page while other handles
hold its handle lock.

**dbsql impact.**  `auto_vacuum=FULL` compacts at commit, so dropping a
table while another connection has the database open hangs
(autovacuum.test 2.5.4).  Listed against R11.

**Fix** (`patches/0002-fix-compact-*.patch`).  Root cause: when
`__db_move_metadata` moves a sub-database's metadata page to a lower
page number and the compact runs inside a transaction but the dbp was
opened in an *earlier* transaction (`dbp->cur_txn == NULL`), it
re-acquired the handle lock keyed on the new page under the dbp's own
handle locker as `DB_LOCK_READ`.  That self-deadlocks when the new
(lower) page is one this same transaction already WRITE-holds the
handle lock on -- e.g. a page freed by a `dbremove` of a sibling
sub-database earlier in the same transaction, whose WRITE handle lock
the transaction holds until commit (`TXN_REMOVE` is deferred).  The
READ get then blocks the one thread against a lock it already owns
under a different locker id; it is not a cycle, so neither
`DB_TXN_NOWAIT` nor the deadlock detector breaks it.  Acquire the lock
under the transaction locker (WRITE) whenever a transaction is in
effect (`dbp->cur_txn` if the dbp is still in its opening txn, else
`dbc->txn`) and register a lock event so it is traded to the handle
locker at txn end, mirroring the `cur_txn != NULL` path; only the
no-transaction case keeps READ-under-handle-locker.  Regression test
`test/db/compact_subdb_remove.c` removes one sub-database and compacts
the survivors in one transaction (with a second handle open) and
verifies every survivor reads back and the removed one is gone; it is
SIGALRM-guarded so a regression is a reported failure, not a hung
suite.  Control: compaction with no prior remove.

---

## R6 status — documented behavior; no code fix

R6 is `DB_REGISTER` recovery discarding the environment from under live
processes when a registrant is found gone.  `transapp_app.md` describes
this as the intended mechanism (peers get `DB_EVENT_REG_PANIC` and are
expected to exit), so it is **working as documented** rather than a
discrete bug with a safe, localized fix.  The two actionable items --
(1) distinguishing a clean `exit()` of a registrant with no outstanding
state from one that truly needs recovery, and (2) the
`DB_REGISTER | DB_FAILCHK` + `is_alive` hybrid tripping diagnostic
assertions in ordinary single-process use -- are a design change to the
register/failchk interaction, not a surgical patch, and carry
regression risk across the replication and failchk suites.  No patch is
included; this is a consultation item for the libdb team (docs plus the
failchk interaction).  dbsql keeps the affected tests in
known-failures under `libdb-R6`.

