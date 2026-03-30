# DBSQL INSERT/SELECT Bug - Debug Analysis Index

## Quick Reference

**Bug**: SELECT returns no rows after INSERT (DBSQL_DONE instead of DBSQL_ROW)

**Root Cause**: Transaction context mismatch - INSERT uses nested transaction context, SELECT uses new root-level transaction context

**Status**: FULLY DIAGNOSED with complete evidence

---

## Documents

### 1. DEBUGGING_SESSION_COMPLETE.md
**Comprehensive debugging session report**
- Investigation process (5 stages)
- Complete technical flow diagram
- Debug evidence with timestamps
- Berkeley DB MVCC explanation
- Three solution approaches
- Full test case

**Read this for**: Complete understanding of the investigation

### 2. ROOT_CAUSE_INSERT_SELECT_BUG.md
**Technical analysis with code locations**
- Transaction flow diagram
- Root cause explanation
- Debug evidence snippets
- Key code file locations
- Recommended solutions with benefits

**Read this for**: Code-level details and locations

### 3. INSERT_SELECT_BUG_ROOT_CAUSE.txt
**Summary report**
- Critical findings (6 items)
- Key code locations
- Recommended fix
- Files for reference

**Read this for**: Quick overview

---

## Key Findings

### Transaction State Issue
```
INSERT: sm->txn=0x1f2f7f20 (active)
        smc->txn=0x1f31de10 (nested)
        Data written to nested context

SELECT: sm->txn=NULL (no transaction)
        smc->txn=0x1f2f7f20 (NEW root-level)
        Cannot see data from different context
```

### Evidence
- OP_Rewind debug output shows `__sm_first() returns res=1` (empty)
- Transaction debug shows INSERT nested, SELECT root-level
- Berkeley DB cursor `c_get(DB_FIRST)` returns `DB_NOTFOUND`

### Root Cause
- SELECT doesn't generate OP_Transaction opcode (read-only optimization)
- sm->txn set to NULL after each exec() call
- New READ transaction can't see data from previous WRITE transaction context

---

## Code Locations

| Component | Location | Issue |
|-----------|----------|-------|
| OP_Transaction | vdbe.c:2510-2520 | Creates sm->txn for writes |
| OP_Commit | vdbe.c:2540-2548 | Clears sm->txn after commit |
| OP_OpenRead/Write | vdbe.c:2677-2750 | Opens cursor with current sm->txn |
| __sm_begin_txn | sm.c:451-456 | Creates sm->txn |
| __sm_commit_txn | sm.c:467-474 | **Sets sm->txn = NULL** |
| __sm_cursor | sm.c:506-555 | Creates nested cursor txn |
| __sm_first | sm.c:1013-1040 | Returns DB_NOTFOUND |
| SELECT codegen | cg_select.c | **NO OP_Transaction** |

---

## Recommended Fixes

### Option 1: Add OP_Transaction to SELECT
- **File**: `src/cg_select.c`
- **Change**: Generate OP_Transaction for SELECT statements
- **Benefit**: Consistent transaction handling
- **Complexity**: Medium

### Option 2: Handle NULL sm->txn in __sm_cursor
- **File**: `src/sm.c`
- **Change**: Use DB_READ_UNCOMMITTED flag when sm->txn is NULL
- **Benefit**: Simpler, backward compatible
- **Complexity**: Low

---

## Test Files

### test_prep_rows.c
Original failing test case demonstrating the bug.

### test_insert_debug.c
Comprehensive test showing:
- CREATE TABLE success
- INSERT success but data unreachable
- SELECT returns no rows
- Multiple INSERT tests

Located in: `/home/gburd/ws/dbsql/test/src/`

---

## Debugging Tools Used

1. **OP_Rewind Debugging** - Shows cursor state and __sm_first() results
2. **OP_OpenRead/OpenWrite Debugging** - Shows pgnoRoot and transaction state
3. **__sm_cursor Debugging** - Tracks transaction creation and nesting
4. **Transaction State Logging** - Shows sm->txn lifecycle

All debugging code is in the source files (not removed).

---

## Build Instructions

```bash
# Rebuild library
cd /home/gburd/ws/dbsql/build_unix.old
rm -f .libs/vdbe.o .libs/sm.o vdbe.o sm.o
make libdbsql-0.4.la

# Rebuild test
cd /home/gburd/ws/dbsql/test/src
rm -f test_prep_rows test_insert_debug
gcc -I../../build_unix.old -I../../src/inc test_prep_rows.c \
    -L../../build_unix.old/.libs -ldbsql-0.4 -ldb-4.8 -lm -g -o test_prep_rows

# Run with debugging
LD_LIBRARY_PATH=../../build_unix.old/.libs ./test_prep_rows 2>&1
```

---

## Summary

The DBSQL INSERT/SELECT bug is fundamentally a **transaction context visibility issue**:

1. INSERT operates in active transaction context (sm->txn set)
2. SELECT operates with NULL transaction context (sm->txn cleared)
3. Berkeley DB MVCC prevents cross-context visibility
4. Result: SELECT sees empty table despite successful INSERT

The fix requires either:
- Adding transaction support to SELECT, OR
- Handling NULL transaction context for READ operations

Both are valid architectural choices. The investigation has identified the exact mechanisms, provided reproducible test cases, and documented the fix locations.

