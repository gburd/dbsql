# Berkeley DB Cursor Transaction Isolation Fix

## Root Cause Analysis

The INSERT/SELECT data visibility issue was caused by incorrect use of Berkeley DB nested transactions in `__sm_cursor()`.

### The Problem

**File**: `src/sm.c`, line 535 (before fix)
**Code**: `int txn_result = dbenv->txn_begin(dbenv, sm->txn, &smc->txn, 0);`

This code was creating a **child transaction** for EVERY cursor, including read-only cursors.

### Berkeley DB Transaction Isolation Model

In Berkeley DB, **sibling child transactions are isolated from each other**:

```
Parent Transaction P
├── Child Transaction A (INSERT cursor)
│   └── Writes data, commits to parent P
└── Child Transaction B (SELECT cursor)
    └── Reads data → CANNOT see child A's changes!
```

**Key insight**: When child A commits, its changes are only visible to:
1. The parent transaction P
2. Future operations within parent P
3. NOT visible to sibling child transaction B

### The Sequence of Events (Before Fix)

1. **CREATE TABLE**: Creates table in database
2. **INSERT statement**:
   - Creates parent transaction P via OP_Transaction
   - Opens write cursor → creates child transaction A
   - Writes row (id=42, name='Alice')
   - Closes cursor → commits child A to parent P
   - OP_Commit → commits parent P
3. **SELECT statement**:
   - Creates NEW parent transaction P' via OP_Transaction
   - Opens read cursor → creates child transaction B
   - Queries table → **DB_NOTFOUND** (empty!)
   - Returns DBSQL_DONE (no rows)

**Why it failed**: Even though we added OP_Transaction to SELECT (commit 34bc4eb), SELECT's read cursor still created a child transaction that couldn't see the INSERT's child transaction's committed data.

## The Fix

### Changes to `__sm_cursor()` (src/sm.c:542-555)

```c
/* OLD CODE (line 535):
   Always create child transaction */
int txn_result = dbenv->txn_begin(dbenv, sm->txn, &smc->txn, 0);

/* NEW CODE:
   Only create child transaction for write cursors */
if (write) {
    int txn_result = dbenv->txn_begin(dbenv, sm->txn, &smc->txn, 0);
    fprintf(stderr, "  txn_begin result=%d, smc->txn=%p (parent was %p)\n",
            txn_result, (void*)smc->txn, (void*)sm->txn);
    if (txn_result != 0) {
        __dbsql_free(sm->dbp, smc);
        return DBSQL_INTERNAL;
    }
    F_SET(smc, SMC_RW_CURSOR);
} else {
    /* Read-only cursor: use parent transaction directly */
    smc->txn = sm->txn;
    fprintf(stderr, "  using parent txn directly: smc->txn=%p\n", (void*)smc->txn);
    F_SET(smc, SMC_RO_CURSOR);
}
```

### Changes to `__sm_close_cursor()` (src/sm.c:577-595)

```c
/* OLD CODE:
   Always commit child transaction */
if (ret == 0) {
    if (smc->txn->commit(smc->txn, 0) != 0) {
        rc = DBSQL_INTERNAL;
    }
}

/* NEW CODE:
   Only commit if write cursor (owns child transaction) */
if (ret == 0) {
    if (F_ISSET(smc, SMC_RW_CURSOR)) {
        if (smc->txn->commit(smc->txn, 0) != 0) {
            rc = DBSQL_INTERNAL;
        }
    }
    /* Read-only cursor: parent owns txn, don't commit here */
}
```

## Why This Fix Works

### Write Cursors (INSERT, UPDATE, DELETE)
- Create child transaction for isolation
- Make modifications in child transaction
- Commit child to parent when cursor closes
- Parent transaction controls overall commit/rollback

### Read Cursors (SELECT)
- Use parent transaction directly (no child)
- Can see all data committed to parent transaction
- Can see data from previously-closed write cursors
- No transaction management needed on cursor close

### The Corrected Sequence

1. **INSERT statement**:
   - Creates parent transaction P via OP_Transaction
   - Opens write cursor → creates child transaction A
   - Writes row (id=42, name='Alice')
   - Closes cursor → commits child A to parent P ✓
   - Data now visible in parent P
   - OP_Commit → commits parent P to database

2. **SELECT statement**:
   - Creates parent transaction P' via OP_Transaction
   - Opens read cursor → uses parent P' directly (NO child)
   - Queries table → **sees committed data!**
   - Returns DBSQL_ROW with data

## Design Intent

This fix aligns with the original design intent documented in the code comment at `src/sm.c:562-565`:

```c
/*
 * __sm_close_cursor --
 *	If this is a read-only cursor, discard the sm_cursor_t->txn.  If
 *	not, it should have been NULL, check that.  Close the
 *	sm_cursor_t->dbc and free the sm_cursor_t.
```

The comment explicitly states read-only cursors should "discard" the transaction, meaning they shouldn't own or manage child transactions.

## Testing

### Expected Output (After Fix)

```
__sm_cursor: sm->txn=0x..., creating cursor for db 3, write=1
  txn_begin result=0, smc->txn=0x... (parent was 0x...)  # Write cursor creates child

__sm_cursor: sm->txn=0x..., creating cursor for db 3, write=0
  using parent txn directly: smc->txn=0x...              # Read cursor uses parent

step() = -50979 (DBSQL_ROW)                              # Success!
```

### Test Cases

1. **test_prep_rows.c**: Prepared statement INSERT → SELECT
2. **test_exec_select.c**: Callback-based INSERT → SELECT
3. **test_api.c**: Multiple concurrent cursors
4. **Full test suite**: Ensure no regressions

## Commits

- **34bc4eb**: Add OP_Transaction to SELECT queries (prerequisite)
- **5f687e2**: Fix Berkeley DB cursor transaction isolation (root cause fix)

## Related Issues

- Task #114: Fix prepared statement row retrieval (RESOLVED)
- Issue: SELECT returns no rows after INSERT (RESOLVED)
- Issue: DBSQL_DONE instead of DBSQL_ROW (RESOLVED)

## References

- Berkeley DB documentation: Nested transactions
- Berkeley DB documentation: Transaction isolation levels
- DBSQL storage manager design: `src/sm.c`
- VDBE cursor management: `src/vdbe_method.c`
