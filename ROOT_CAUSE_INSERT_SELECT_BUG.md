# DBSQL INSERT/SELECT Bug - Complete Root Cause Analysis

## Overview

**Bug**: After executing `INSERT` via `db->exec()`, subsequent `SELECT` statements via `db->prepare()` and `db->step()` return no rows.

**Status**: ROOT CAUSE IDENTIFIED

## Technical Analysis

### Transaction Flow Diagram

```
EXEC("INSERT..."):
  OP_Transaction → sm->txn = 0x1f2f7f20 (created)
  OP_OpenWrite → __sm_cursor() creates smc->txn = 0x1f31de10 (nested)
  [INSERT data into smc->txn]
  [Cursor closes, commits smc->txn]
  OP_Commit → sm->txn committed
  __sm_commit_txn() → sm->txn = NULL  ← CLEARS TRANSACTION

PREPARE("SELECT...") + STEP():
  [No OP_Transaction generated for SELECT]
  OP_OpenRead → __sm_cursor() called with sm->txn = NULL
  Creates NEW smc->txn at root level (different from INSERT's context)
  Berkeley DB cursor sees empty table ← DATA NOT VISIBLE
```

### Root Cause: Transaction Isolation Context Mismatch

**The Problem:**

1. **INSERT Phase**:
   - `exec()` calls generate OP_Transaction opcode
   - Creates `sm->txn` (root transaction)
   - Creates nested `smc->txn` for cursor operations
   - Data written to nested transaction
   - Cursor closes and commits nested transaction
   - OP_Commit commits `sm->txn`
   - **`__sm_commit_txn()` then sets `sm->txn = NULL`**

2. **SELECT Phase**:
   - `prepare()` compiles SELECT (NO OP_Transaction generated)
   - `step()` calls `__vdbe_exec()` with **`sm->txn = NULL`**
   - OP_OpenRead calls `__sm_cursor()` with NULL parent transaction
   - Creates NEW root-level transaction `smc->txn`
   - **This is a DIFFERENT transaction context than the INSERT's**

3. **Visibility Problem**:
   - Berkeley DB uses MVCC (Multi-Version Concurrency Control)
   - Data inserted and committed in one transaction context (nested under INSERT's sm->txn)
   - SELECT tries to read with a new transaction context (root-level, no parent)
   - New transaction cannot see data from different transaction context

### Evidence from Debugging

**From test execution:**
```
INSERT operation:
  OP_OpenWrite: PC=4, iCur=0, pgnoRoot=3, wrFlag=1
  __sm_cursor: sm->txn=0x1f2f7f20, creating cursor for db 3, write=1
  txn_begin result=0, smc->txn=0x1f31de10 (parent was 0x1f2f7f20)
                                            ↑ Nested transaction

SELECT operation:
  OP_OpenRead: PC=3, iCur=0, pgnoRoot=3, wrFlag=0
  __sm_cursor: sm->txn=(nil), creating cursor for db 3, write=0
  txn_begin result=0, smc->txn=0x1f2f7f20 (parent was (nil))
                                          ↑ New root-level transaction
```

**Note:** The memory address `0x1f2f7f20` appears for both, but they are **different transaction objects** (reused memory address). The INSERT created it and committed it; the SELECT created a new one.

### Why __sm_first() Returns Empty

In `/home/gburd/ws/dbsql/src/sm.c`, `__sm_first()` calls:
```c
switch(smc->dbc->c_get(smc->dbc, &key, &data, DB_FIRST)) {
    case 0:
        *result = 0;  // Found data
        break;
    case DB_NOTFOUND:
        *result = 1;  // No data found
        break;
```

Berkeley DB's cursor `c_get()` with DB_FIRST returns `DB_NOTFOUND` because the cursor is in a different transaction context that cannot see the data.

## Key Code Locations

1. **Transaction Creation** (INSERT):
   - `/home/gburd/ws/dbsql/src/vdbe.c:2515` - `OP_Transaction` calls `__sm_begin_txn()`
   - `/home/gburd/ws/dbsql/src/sm.c:451-456` - `__sm_begin_txn()` creates transaction

2. **Transaction Destruction** (After INSERT):
   - `/home/gburd/ws/dbsql/src/vdbe.c:2540` - `OP_Commit` calls `__sm_commit_txn()`
   - `/home/gburd/ws/dbsql/src/sm.c:467-474` - `__sm_commit_txn()` commits and sets `sm->txn = NULL`

3. **Cursor Creation** (SELECT):
   - `/home/gburd/ws/dbsql/src/vdbe.c:2677` - `OP_OpenRead` case
   - `/home/gburd/ws/dbsql/src/sm.c:506-555` - `__sm_cursor()` creates cursor with NULL parent

4. **READ Operation**:
   - `/home/gburd/ws/dbsql/src/sm.c:1013-1040` - `__sm_first()` returns empty

5. **SELECT Compilation**:
   - `/home/gburd/ws/dbsql/src/cg_select.c` - No OP_Transaction generated for SELECT

## Recommended Solution

**Approach 1: Add OP_Transaction to SELECT (Preferred)**

Ensure SELECT statements generate `OP_Transaction` opcode before opening cursors. This ensures `sm->txn` is properly initialized for READ operations, just like it is for WRITE operations.

Location: `/home/gburd/ws/dbsql/src/cg_select.c`

Benefits:
- Consistent transaction handling across all statement types
- Maintains MVCC semantics
- Most robust long-term solution

**Approach 2: Handle NULL sm->txn in __sm_cursor**

When `sm->txn` is NULL and opening a READ cursor, use Berkeley DB flags to read uncommitted/committed data.

Location: `/home/gburd/ws/dbsql/src/sm.c:__sm_cursor()`

Benefits:
- Simpler, more targeted fix
- Backward compatible
- Doesn't require changing compiler

## Test Case

```c
dbsql_create_env(&db, NULL, NULL, 0, 0);
db->open(db, ":memory:", 0664, &errmsg);
db->exec(db, "CREATE TABLE t (x INT)", NULL, NULL, &errmsg);
db->exec(db, "INSERT INTO t VALUES (42)", NULL, NULL, &errmsg);

db->prepare(db, "SELECT * FROM t", &tail, &stmt, &errmsg);
rc = db->step(stmt, &n_cols, &values, &cols);
// BUG: rc = -50978 (DBSQL_DONE, no rows)
// EXPECTED: rc = -50979 (DBSQL_ROW, with values[0]="42")
```

