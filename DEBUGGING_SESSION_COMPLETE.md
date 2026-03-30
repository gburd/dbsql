# DBSQL INSERT/SELECT Bug - Debugging Session Complete

## Executive Summary

**Root Cause**: Transaction context mismatch between INSERT and SELECT operations when using `exec()` followed by `prepare()`/`step()`.

**Impact**: After successful INSERT via `exec()`, SELECT via `prepare()`/`step()` returns no rows.

**Status**: ROOT CAUSE IDENTIFIED with comprehensive debug evidence

---

## Investigation Process

### Stage 1: Initial Observation
- Ran test: CREATE TABLE, INSERT row, SELECT
- Result: `step()` returned -50978 (DBSQL_DONE) instead of -50979 (DBSQL_ROW)
- Data appeared lost

### Stage 2: OP_Rewind Cursor Debugging
- Added debugging to OP_Rewind opcode execution
- Found: `__sm_first()` returns `res=1` (empty/DB_NOTFOUND)
- Found: Cursor exists and pCrsr exists, but DB says table is empty

### Stage 3: Page Number Investigation  
- Added debugging to OP_OpenRead/OP_OpenWrite
- Found: CREATE TABLE uses pgnoRoot=2
- Found: INSERT uses pgnoRoot=3 (table migration happened)
- Found: SELECT correctly opens pgnoRoot=3
- Conclusion: Page number mismatch partially explains the issue, but not fully

### Stage 4: Transaction Context Analysis
- Added debugging to __sm_cursor and transaction creation
- Discovered: INSERT has `sm->txn=0x1f2f7f20` (active transaction)
- Discovered: SELECT has `sm->txn=NULL` (NO TRANSACTION!)
- Found: Both create new cursor transactions, but in different contexts
- **KEY FINDING**: SELECT doesn't generate OP_Transaction opcode

### Stage 5: Berkeley DB Behavior Analysis
- Understood: Berkeley DB uses MVCC (Multi-Version Concurrency Control)
- Each transaction creates snapshot at creation time
- New READ transaction with NULL parent ≠ previous WRITE transaction's context
- Result: New transaction cannot see data from different transaction context

---

## Technical Flow Diagram

```
PHASE 1: exec("INSERT...")
──────────────────────────

1. SQL Parser generates VDBE program
   - OP_Transaction (creates transaction)
   - OP_OpenWrite (opens cursor)
   - OP_Insert (writes data)
   - OP_Commit (commits transaction)
   - OP_Halt

2. OP_Transaction execution:
   __sm_begin_txn(db->aDb[0].pBt)
     └─ Creates sm->txn = 0x1f2f7f20 (root-level)

3. OP_OpenWrite execution:
   __sm_cursor(sm, table_id, write=1, &pCur->uc.pCursor)
     ├─ sm->txn = 0x1f2f7f20 (parent transaction)
     └─ Creates nested: dbenv->txn_begin(dbenv, 0x1f2f7f20, &smc->txn, 0)
        └─ smc->txn = 0x1f31de10 (nested cursor transaction)

4. OP_Insert execution:
   Writes data to smc->txn = 0x1f31de10

5. Cursor closes:
   smc->txn->commit(smc->txn, 0)  ← Commits to parent 0x1f2f7f20

6. OP_Commit execution:
   __sm_commit_txn(db->aDb[0].pBt)
     ├─ sm->txn->commit(sm->txn, 0)  ← Commits 0x1f2f7f20
     └─ sm->txn = NULL                ← CLEARS TRANSACTION!


PHASE 2: prepare("SELECT...") + step()
───────────────────────────────────────

1. SQL Parser generates VDBE program
   - OP_OpenRead (opens cursor)  ← NO OP_Transaction!
   - OP_Rewind (positions cursor)
   - OP_Column (extracts columns)
   - OP_Callback (sends results)
   - OP_Halt

2. OP_OpenRead execution:
   __sm_cursor(sm, table_id, write=0, &pCur->uc.pCursor)
     ├─ sm->txn = NULL              ← NO ACTIVE TRANSACTION!
     └─ Creates new root: dbenv->txn_begin(dbenv, NULL, &smc->txn, 0)
        └─ smc->txn = 0x1f2f7f20    ← DIFFERENT TRANSACTION (memory reused)
           (different object, different snapshot context)

3. OP_Rewind execution:
   __sm_first(pCrsr, &res)
     └─ smc->dbc->c_get(smc->dbc, &key, &data, DB_FIRST)
        └─ Returns DB_NOTFOUND       ← CANNOT SEE DATA FROM DIFFERENT TXN!
           res = 1                   ← Empty flag set

4. OP_Rewind jumps:
   if (res) pc = op->p2 - 1         ← SKIPS ENTIRE RESULT SET!

Result: SELECT returns DBSQL_DONE (no rows) instead of DBSQL_ROW
```

---

## Debug Evidence

### Transaction State Changes

```
OP_OpenRead (CREATE TABLE):
  __sm_cursor: sm->txn=(nil), creating cursor for db 2, write=0
    txn_begin result=0, smc->txn=0x1f2f7f20 (parent was (nil))

OP_OpenWrite (INSERT):
  __sm_cursor: sm->txn=0x1f2f7f20, creating cursor for db 3, write=1
    txn_begin result=0, smc->txn=0x1f31de10 (parent was 0x1f2f7f20)
                                                    ↑ NESTED

OP_OpenRead (SELECT):
  __sm_cursor: sm->txn=(nil), creating cursor for db 3, write=0
    txn_begin result=0, smc->txn=0x1f2f7f20 (parent was (nil))
                                                    ↑ NEW ROOT-LEVEL
```

### OP_Rewind Output

```
OP_Rewind: PC=5, cursor[0]=0x1e55e610
  Cursor exists, eCurType=0 (CURTYPE_BTREE=0)
  pCrsr=0x1e53a360
  __sm_first returned rc=0, res=1  ← res=1 means DB_NOTFOUND (empty)
  Final res=1, jumping=YES         ← SKIPS RESULT SET PROCESSING
```

---

## Why This Happens

### Berkeley DB MVCC Semantics

Berkeley DB maintains Multi-Version Concurrency Control:

1. **Transaction Isolation**: Each transaction creates snapshot at start
2. **Nested Transactions**: Nested transactions inherit parent's view
3. **Different Contexts**: Transactions with different parents have different snapshots
4. **Visibility**: A transaction only sees committed data from its context

### The Mismatch

```
INSERT creates: sm->txn (parent) -> smc->txn (nested) -> DATA
                          ↓
                      commit()
                          ↓
                    sm->txn = NULL

SELECT creates: NULL (no parent) -> smc->txn (root) -> DATA??
                                                      
                    Cannot see data committed in
                    different transaction context
```

---

## Root Cause Analysis

### Why SELECT Doesn't Have OP_Transaction

Location: `/home/gburd/ws/dbsql/src/cg_select.c`

- SELECT is read-only, doesn't modify data
- Compiler optimizes out OP_Transaction for read operations
- Only write operations (INSERT/UPDATE/DELETE) get OP_Transaction
- **BUT**: This breaks visibility of data from previous transactions

### Why sm->txn Becomes NULL

Location: `/home/gburd/ws/dbsql/src/sm.c:__sm_commit_txn()`

```c
int __sm_commit_txn(sm_t* sm)
{
    DBSQL_ASSERT(sm);
    if (sm->txn)
        sm->txn->commit(sm->txn, 0);
    sm->txn = 0;  ← CLEARS THE TRANSACTION
    return DBSQL_SUCCESS;
}
```

After each `exec()` call, the transaction is committed and cleared for cleanup.
This is correct behavior, but leaves `sm->txn = NULL` for the next statement.

---

## Code Locations Summary

### Transaction Creation/Destruction
- `src/vdbe.c:2510` - OP_Transaction opcode
- `src/vdbe.c:2540` - OP_Commit opcode  
- `src/sm.c:451` - __sm_begin_txn()
- `src/sm.c:467` - __sm_commit_txn() sets sm->txn = NULL

### Cursor Operations
- `src/vdbe.c:2677` - OP_OpenRead/OP_OpenWrite
- `src/sm.c:506` - __sm_cursor() creates cursor with sm->txn context
- `src/sm.c:534` - dbenv->txn_begin() creates nested/root transaction
- `src/sm.c:1013` - __sm_first() returns DB_NOTFOUND

### SELECT Compilation
- `src/cg_select.c` - SELECT code generation (no OP_Transaction!)

---

## Solution Approaches

### Option 1: Add OP_Transaction to SELECT (Recommended)

Ensure every SELECT statement generates `OP_Transaction` opcode.

**Location**: `src/cg_select.c`

**Change**: Call a transaction preparation function for SELECT statements

**Benefits**:
- Consistent transaction handling across all statement types
- Maintains proper MVCC context
- Allows data from previous transactions to be visible

**Example**:
```c
// In SELECT code generation, before opening cursors:
__vdbe_prepare_transaction(parser, iDb);  // Adds OP_Transaction
```

### Option 2: Handle NULL sm->txn in __sm_cursor

Allow cursor creation with NULL parent transaction for READ operations.

**Location**: `src/sm.c:__sm_cursor()`

**Change**: Use Berkeley DB flags (DB_READ_UNCOMMITTED) for read cursors with NULL parent

**Benefits**:
- Simpler, more targeted fix
- Doesn't require changing compiler
- Less code change
- Backward compatible

**Example**:
```c
int flags = 0;
if (!write && sm->txn == NULL) {
    flags = DB_READ_UNCOMMITTED;  // See committed data from other txns
}
if (smr->db->cursor(smr->db, smc->txn, &smc->dbc, flags) != 0) {
    // error handling
}
```

### Option 3: Hybrid Approach

Begin a read-only transaction for SELECT if sm->txn is NULL.

**Pros & Cons**: Middle ground between Options 1 and 2

---

## Test Case

```c
#include <stdio.h>
#include "dbsql.h"

int main() {
    DBSQL *db;
    dbsql_stmt_t *stmt;
    char *errmsg = NULL;
    const char *tail;
    int rc, n_cols;
    const char **values, **cols;

    dbsql_create_env(&db, NULL, NULL, 0, 0);
    db->open(db, ":memory:", 0664, &errmsg);
    
    // Create table
    db->exec(db, "CREATE TABLE t (x INT)", NULL, NULL, &errmsg);
    
    // Insert data
    db->exec(db, "INSERT INTO t VALUES (42)", NULL, NULL, &errmsg);
    
    // Try to select
    db->prepare(db, "SELECT * FROM t", &tail, &stmt, &errmsg);
    rc = db->step(stmt, &n_cols, &values, &cols);
    
    // BUG: rc = -50978 (DBSQL_DONE, no rows)
    // EXPECTED: rc = -50979 (DBSQL_ROW, with data)
    printf("Result: %d (DONE=%d, ROW=%d)\n", rc, DBSQL_DONE, DBSQL_ROW);
    
    if (rc == DBSQL_ROW && values) {
        printf("SUCCESS: Got row with value=%s\n", values[0]);
    } else {
        printf("FAILED: No row returned\n");
    }
    
    db->finalize(stmt, &errmsg);
    db->close(db);
    return 0;
}
```

---

## Conclusion

The bug is not random memory corruption or a missing feature. It's a **transaction context mismatch**:

1. INSERT commits data in one transaction context
2. SELECT tries to read with a different (new) transaction context  
3. Berkeley DB's MVCC correctly prevents cross-context visibility
4. Result: SELECT sees an empty table

The fix is to ensure SELECT statements operate in a proper transaction context, either by:
- Generating OP_Transaction for SELECT statements, OR
- Handling NULL sm->txn with appropriate Berkeley DB flags for READ operations

Both solutions are correct and are architectural choices about transaction semantics.

