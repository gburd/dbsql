# Runtime Fixes Applied - March 24, 2026

## Summary

Applied critical runtime fixes identified by the 5-agent parallel team (see AGENT_TEAM_FINAL_REPORT.md). The fixes enable `--version` and `--help` to work, but SQL execution still crashes due to memory allocator mixing.

---

## Changes Applied

### 1. Memory Allocation Layer (`src/common/dbsql_alloc.c`)

**Problem:** Passing NULL to Berkeley DB's `__os_*` allocator functions caused crashes.

**Fix:** Modified 6 allocation functions to use standard C library functions when `dbp == NULL`:

```c
// Before:
int __dbsql_calloc(DBSQL *dbp, size_t num, size_t size, void *storep) {
    return (__os_calloc((dbp ? dbp->dbenv : NULL), num, size, storep));
}

// After:
int __dbsql_calloc(DBSQL *dbp, size_t num, size_t size, void *storep) {
    if (dbp == NULL) {
        void **result = (void **)storep;
        *result = calloc(num, size);
        return (*result == NULL) ? ENOMEM : 0;
    }
    return (__os_calloc(dbp->dbenv, num, size, storep));
}
```

**Functions Modified:**
- `__dbsql_calloc()` - Use `calloc()` when dbp is NULL
- `__dbsql_malloc()` - Use `malloc()` when dbp is NULL
- `__dbsql_realloc()` - Use `realloc()` when dbp is NULL
- `__dbsql_free()` - Use `free()` when dbp is NULL
- `__dbsql_strdup()` - Use `__dbsql_malloc()` wrapper instead of `__os_strdup()`
- `__dbsql_strndup()` - Use `__dbsql_calloc()` wrapper instead of `__os_calloc()`

**Added Includes:**
- `#include <errno.h>` for ENOMEM
- `#include <stdlib.h>` for malloc/calloc/realloc/free

### 2. Early Command Processing (`src/dbsql/dbsql.c`)

**Problem:** `--version` was checked in second argument pass, AFTER database initialization.

**Fix:** Moved `--version` check to first argument pass (lines 1350-1353):

```c
for(i = 1; i <= argc - 1; i++) {
    // ... other checks ...
    } else if (strcmp(argv[i], "--version") == 0) {
        int major, minor, patch;
        printf("%s\n", dbsql_version(&major, &minor, &patch));
        return 0;  // Exit before opening database
    } else if (strcmp(argv[i], "--help") == 0) {
        usage(1);
    }
}
```

**Impact:** `--version` and `--help` now work without requiring database initialization.

### 3. Missing Utility Functions (`src/util.c`)

**Problem:** Linker errors for undefined functions.

**Fix:** Created `src/util.c` (126 lines) with:

1. **`__dbsql_atoi64(const char *z, i64 *pNum)`**
   - Converts string to 64-bit signed integer
   - Handles negative numbers and whitespace
   - Returns 1 if valid conversion, 0 otherwise

2. **`__sql_parser_fallback(int iToken)`**
   - Token fallback function for SQL parser
   - Currently stub implementation (returns 0)
   - Prevents linker errors in sql_tokenize.c

3. **`__vdbe_dequote_p3(vdbe_t *v, int addr)`**
   - Removes quotes from P3 parameter of VDBE instruction
   - Modifies instruction's P3 string in-place
   - Handles both single and double quotes

### 4. Build System (`dist/Makefile.in`)

**Changes:**
- Added `util@o@` to `C_OBJS` list (line 143)
- Added build rule for `util@o@` (after line 410):
  ```makefile
  util@o@: $(srcdir)/util.c
       $(CC) $(CFLAGS) $?
  ```

---

## Test Results

### ✅ Working Commands

```bash
$ result/bin/dbsql --version
DBSQL 0.3.1: (pre20260324_dirty)

$ result/bin/dbsql --help
Usage: dbsql [OPTIONS] FILENAME [SQL]
...
```

### ❌ Still Broken

```bash
$ result/bin/dbsql :memory: "SELECT 1;"
Fatal glibc error: malloc.c:2593 (sysmalloc): assertion failed:
(old_top == initial_top (av) && old_size == 0) ||
((unsigned long) (old_size) >= MINSIZE && prev_inuse (old_top) &&
((unsigned long) old_end & (pagesize - 1)) == 0)
```

---

## Root Cause Analysis

### The Remaining Problem

Memory allocated by one system is being freed by another system, causing heap corruption:

1. **Scenario A:** Memory allocated with standard `malloc()` (when dbp is NULL) gets freed later by Berkeley DB's `__os_free()`

2. **Scenario B:** Memory allocated with Berkeley DB's `__os_malloc()` gets freed by standard C `free()`

### Why This Happens

The current fix handles allocations when `dbp == NULL`, but:
- Memory allocated early (with standard allocators) may be freed later (after dbp is initialized)
- Memory allocated later (with BDB allocators) may be freed in cleanup code that checks `if (dbp == NULL)`
- The two allocator systems maintain separate heap metadata, causing corruption when mixed

### Evidence from Agent Reports

From **runtime-debug agent (a78a826)**:
> "The malloc crash still occurs during database initialization when running actual SQL commands. This suggests memory corruption is happening despite the allocation fixes."

From **test-suite agent (a69701d)** in CRITICAL_RUNTIME_ISSUE.md:
> "Root Cause: Code expects DBSQL->vdbe field (doesn't exist)"

**NOTE:** The vdbe field issue was found to be a red herring. The structure already has `pVdbe` (line 363 of src/dbsql.in), and code already uses it correctly. The real issue is memory allocator mixing.

---

## Next Steps (Estimated Effort)

### Option A: Initialize BDB Environment Earlier (3-5 hours)

**Approach:** Ensure Berkeley DB environment is initialized before any memory allocations.

**Steps:**
1. Move `dbenv` initialization earlier in startup sequence
2. Ensure `dbp->dbenv` is always valid before any `__dbsql_*` calls
3. Remove NULL checks in allocation functions (always use BDB allocators)

**Risk:** Low - clean architectural fix

### Option B: Memory Debugging Tools (2-3 hours to find, varies to fix)

**Approach:** Use valgrind or AddressSanitizer to find exact corruption point.

**Steps:**
1. Build with debug symbols and sanitizers:
   ```nix
   nix build --arg enableDebugging true
   ```
2. Run with valgrind:
   ```bash
   valgrind --leak-check=full --track-origins=yes ./result/bin/dbsql :memory: "SELECT 1;"
   ```
3. Analyze output to find:
   - Which memory was allocated by which system
   - Where it's being freed by the wrong system
   - Call stack at time of corruption

**Risk:** Medium - requires analysis skills

### Option C: Complete Memory Domain Separation (5-8 hours)

**Approach:** Audit all code paths to ensure strict separation.

**Steps:**
1. Tag all memory with source (add metadata or use separate pools)
2. Audit every `__dbsql_free()` call to ensure matching allocator
3. Add assertions to catch mismatches in debug builds
4. Consider maintaining two separate allocation tracking systems

**Risk:** High - invasive changes, many code paths to audit

### Recommended Approach

**Hybrid: B then A** (5-8 hours total)
1. Use valgrind to find exact corruption point (2-3 hours)
2. Based on findings, either:
   - Initialize BDB environment earlier if corruption is early (2-3 hours)
   - Or fix specific problematic code paths (varies)

---

## Agent Team Credits

These fixes were identified and documented by the 5-agent parallel team:

1. **Test Suite Agent (a69701d)** - Identified root cause and documented in CRITICAL_RUNTIME_ISSUE.md
2. **Runtime Debug Agent (a78a826)** - Developed and tested these fixes in runtime-debug worktree
3. **Code Review Agent (a00706f)** - Identified 3 BLOCKING issues including thread safety
4. **Opcode Implementation Agent (a0ee502)** - Implemented ORDER BY support (ready to use once runtime fixed)
5. **Naming Fixes Agent (a4ba7e0)** - Fixed 30% of naming violations

**Total Agent Work:** ~20 hours compressed into 5 hours via parallel execution.

See `AGENT_TEAM_FINAL_REPORT.md` for complete analysis.

---

## Files Modified in This Session

```
 dist/Makefile.in         |  18 +++++++++---
 src/common/dbsql_alloc.c |  60 ++++++++++++++++++++++++++++++++++---
 src/dbsql/dbsql.c        |   4 +++
 src/util.c               | 126 +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 4 files changed, 194 insertions(+), 13 deletions(-)
```

**Commit:** a73373b "Apply runtime fixes from agent team analysis"

---

## Current Status

**Compilation:** ✅ 100% success (all files compile cleanly)
**--version:** ✅ Works without crash
**--help:** ✅ Works without crash
**SQL Execution:** ❌ Crashes with malloc assertion

**Blocker:** Memory allocator mixing between standard C and Berkeley DB allocators

**Path Forward:** Use valgrind to identify exact corruption point, then initialize BDB environment earlier or fix specific problematic code paths.

---

**Last Updated:** 2026-03-24 10:30 UTC
**Session:** Continuation after 5-agent parallel team completion
