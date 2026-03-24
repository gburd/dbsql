# Cursor Segfault Analysis and Fixes

**Date:** 2026-03-24
**Issue:** Segfault when executing SQL (initially reported at sm.c:518)
**Status:** Partially fixed - 3 bugs resolved, 1 remaining

## Summary of Fixes Applied

### Fix 1: NULL pointer check in __sm_cursor()
**File:** `src/sm.c` lines 509-511
**Problem:** Accessing `sm->dbp->dbenv` without checking if `sm->dbp` is NULL
**Root Cause:** If `sm_t` structure is not properly initialized, `dbp` field may be NULL
**Fix Applied:**
```c
*smcp = 0;
if (sm->dbp == NULL)
    return DBSQL_ERROR;
dbenv = sm->dbp->dbenv;
```

**Impact:** Prevents segfault when `sm->dbp` is NULL

---

### Fix 2: Incorrect allocation error checking in __dbsql_calloc()
**Files:** `src/sm.c` line 515, `src/vdbe_method.c` line 65
**Problem:** Checking `== ENOMEM` instead of `!= 0`
**Root Cause:**
- Berkeley DB's `__os_malloc()` can return various error codes on failure
- Returns `ENOMEM` by default, but can return other errno values
- Checking only `== ENOMEM` misses other allocation failures
- When allocation fails with non-ENOMEM error, pointer remains NULL but code continues

**Locations Fixed:**

1. **sm.c:515**
```c
// BEFORE (WRONG):
if (__dbsql_calloc(sm->dbp, 1, sizeof(sm_cursor_t), &smc) == ENOMEM)
    return DBSQL_NOMEM;

// AFTER (CORRECT):
if (__dbsql_calloc(sm->dbp, 1, sizeof(sm_cursor_t), &smc) != 0)
    return DBSQL_NOMEM;
```

2. **vdbe_method.c:65**
```c
// BEFORE (WRONG):
if (__dbsql_calloc(dbp, 1, sizeof(vdbe_t), &vm) == ENOMEM)
    return 0;

// AFTER (CORRECT):
if (__dbsql_calloc(dbp, 1, sizeof(vdbe_t), &vm) != 0)
    return 0;
```

**Impact:** Properly catches ALL allocation failures, not just ENOMEM

**Note:** This pattern appears 103 times throughout the codebase. Only critical paths fixed so far.

---

### Fix 3: Invalid opcode iteration in __vdbe_delete()
**File:** `src/vdbe_method.c` lines 1773-1781
**Problem:** Accessing NULL opcodes array when nOpAlloc==0 but nOp>0
**Root Cause:**
```c
if (vm->nOpAlloc == 0) {
    vm->opcodes = 0;  // Set to NULL
    vm->nOp = 0;
}
for (i = 0; i < vm->nOp; i++) {
    __vdbe_free_p4(vm->opcodes[i]...);  // Dereference NULL if nOpAlloc was 0
}
```

**Fix Applied:**
```c
if (vm->nOpAlloc == 0) {
    vm->opcodes = 0;
    vm->nOp = 0;
} else {
    for (i = 0; i < vm->nOp; i++) {
        __vdbe_free_p4(vm->opcodes[i].p4type, vm->opcodes[i].p4.p);
    }
}
```

**Impact:** Prevents NULL pointer dereference when freeing opcodes

---

### Fix 4: Changed cursor free context to NULL
**File:** `src/vdbe_method.c` line 1527
**Problem:** Using potentially invalid `vm->db` pointer as allocator context
**Fix Applied:**
```c
// BEFORE:
__dbsql_free(vm->db, pCur);

// AFTER:
__dbsql_free(NULL, pCur);
```

**Impact:** Safer cleanup - doesn't depend on vm->db validity

---

## Remaining Issue

**Error:** `free(): invalid pointer` in `__run_sql_parser()`
**Location:** Stack trace shows crash in `src/sql_tokenize.c` around line 762
**Suspected Cause:** `__sql_parser_free(dbp, engine, __dbsql_free)` freeing invalid pointer

**Analysis:**
- The error occurs BEFORE `__vdbe_delete()` is called
- Disabling all VDBE cleanup code doesn't prevent the error
- Stack trace:
  ```
  #6  free()
  #7  __run_sql_parser()  <-- CRASH HERE
  #8  __init_db_file()
  #9  __init_databases()
  #10 __api_open()
  ```

**Likely Causes:**
1. Lemon parser's `__sql_parser_free()` freeing statically allocated memory
2. Lemon parser's state structure contains invalid pointers
3. `__sql_parser_alloc()` succeeded but returned partially initialized structure
4. Mixing of allocation methods (malloc vs __os_malloc)

**Next Steps:**
1. Check generated Lemon parser code (`parse.c`)
2. Verify `__sql_parser_alloc()` and `__sql_parser_free()` implementations
3. Check if Lemon parser was regenerated after Berkeley DB integration
4. Consider adding debug output to identify exact pointer being freed

---

## Test Results

**Current Status:** Build succeeds ✅, but segfault persists ❌

```bash
$ echo "SELECT 1;" | result/bin/dbsql :memory:
free(): invalid pointer
Aborted (core dumped)
```

---

## Additional Findings

### Systemic Issue: Incorrect Error Checking Pattern
The pattern `if (__dbsql_calloc(...) == ENOMEM)` appears **103 times** in the codebase.
This is a systemic bug that should be addressed in a separate cleanup pass.

**Affected Functions Include:**
- Various allocation sites in vdbe.c
- Table and index creation functions
- Expression list building
- String manipulation functions

**Recommendation:** Global search-and-replace:
```bash
# Find all instances:
grep -rn "== ENOMEM" src/*.c | wc -l
# Result: 103 instances

# Should be changed to:
!= 0  # or != DBSQL_SUCCESS depending on context
```

---

## Commit Message

```
Fix cursor allocation and VDBE cleanup bugs

1. Add NULL check for sm->dbp in __sm_cursor()
   - Prevents segfault when sm structure not fully initialized

2. Fix allocation error checking in __dbsql_calloc() calls
   - Change from `== ENOMEM` to `!= 0`
   - Berkeley DB allocators return various error codes
   - Fixes silent failures when allocation returns non-ENOMEM errors

3. Fix opcode iteration in __vdbe_delete()
   - Only iterate opcodes if nOpAlloc > 0
   - Prevents NULL pointer dereference

4. Use NULL context for cursor cleanup
   - Safer than using potentially invalid vm->db pointer

Issue: Segfault in __sm_cursor() preventing SQL execution
Status: Partially fixed - cursor allocation safe, but parser cleanup issue remains
```

---

## Files Modified

1. `src/sm.c` - Added NULL check, fixed allocation error check
2. `src/vdbe_method.c` - Fixed opcode iteration, changed free context

**Total Changes:** 4 fixes across 2 files
