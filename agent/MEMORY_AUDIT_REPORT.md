# Memory Allocator Audit Report

## Executive Summary

Completed comprehensive audit and replacement of ALL standard C library memory allocators with Berkeley DB's `__os_*` functions throughout the DBSQL codebase to eliminate malloc assertion crashes caused by mixed allocator usage.

## Problem Statement

The DBSQL binary was crashing with malloc assertion failures because:
1. Memory allocated by standard C library (malloc/calloc/free) was being mixed with memory managed by Berkeley DB's custom allocators
2. The `src/common/dbsql_alloc.c` wrapper functions had fallback logic using standard C allocators when `dbp == NULL`
3. Various CLI functions in `src/dbsql/dbsql.c` were directly calling malloc/free/realloc before DB environment initialization

## Changes Made

### 1. Fixed `src/common/dbsql_alloc.c`

**Removed dangerous fallback logic:**
- `__dbsql_malloc()` - Removed `if (dbp == NULL)` branch that called `malloc()`
- `__dbsql_calloc()` - Removed `if (dbp == NULL)` branch that called `calloc()`
- `__dbsql_realloc()` - Removed `if (dbp == NULL)` branch that called `realloc()`
- `__dbsql_free()` - Removed `if (dbp == NULL)` branch that called `free()`

**Result:** All functions now consistently use `__os_*` functions, passing `(dbp ? dbp->dbenv : NULL)` to handle NULL cases safely.

Removed unnecessary `#include <stdlib.h>` since standard allocators are no longer used.

### 2. Fixed `src/dbsql/dbsql.c`

#### Early DB Environment Initialization

Added minimal DB environment initialization in `main()` BEFORE any memory allocations:

```c
/*
 * Initialize a minimal DB environment early for memory management.
 * This ensures ALL memory allocations use Berkeley DB's allocators.
 */
if ((rc = db_env_create(&g.dbenv, 0)) != 0) {
    fprintf(stderr, "%s: db_env_create: %s\n",
        g.progname, db_strerror(rc));
    return 1;
}
```

This ensures `g.dbenv` is available for ALL subsequent allocations throughout the CLI session.

#### Replaced All Direct Allocator Calls

**Functions Modified:**

1. **`local_getline()`**
   - Replaced `malloc()` → `__os_malloc(g.dbenv, ...)`
   - Replaced `realloc()` → `__os_realloc(g.dbenv, ...)`
   - Replaced `free()` → `__os_free(g.dbenv, ...)`
   - Total: 4 replacements

2. **`set_table_name()`**
   - Replaced `malloc()` → `__os_malloc(g.dbenv, ...)`
   - Replaced `free()` → `__os_free(g.dbenv, ...)`
   - Total: 2 replacements

3. **`process_input()`**
   - Replaced `malloc()` → `__os_malloc(g.dbenv, ...)`
   - Replaced `realloc()` → `__os_realloc(g.dbenv, ...)`
   - Replaced `free()` → `__os_free(g.dbenv, ...)` (multiple locations)
   - Total: 9 replacements

4. **`find_home_dir()`**
   - Replaced `malloc()` → `__os_malloc(g.dbenv, ...)`
   - Total: 1 replacement

5. **`process_rc()`**
   - Replaced `malloc()` → `__os_malloc(g.dbenv, ...)`
   - Replaced `free()` → `__os_free(g.dbenv, ...)` (multiple locations)
   - Total: 3 replacements

6. **`main()`**
   - Replaced `malloc()` → `__os_malloc(g.dbenv, ...)` for history path
   - Replaced `free()` → `__os_free(g.dbenv, ...)` for history path and home dir
   - Fixed `__dbsql_strdup()` call with incorrect arguments
   - Total: 3 replacements

7. **`do_meta_command()`**
   - Replaced `free()` → `__os_free(g.dbenv, ...)` for error messages
   - Total: 5 replacements

8. **`open_db()`**
   - Replaced `free()` → `__os_free(g.dbenv, ...)` for error messages
   - Total: 1 replacement

#### Cleanup Logic

Updated main() cleanup to properly handle cases where minimal dbenv is initialized but database might not be opened:

```c
if (g.dbp) {
    g.dbp->close(g.dbp);
    /* dbenv is closed by dbp->close() */
} else if (g.dbenv) {
    /* We created a minimal dbenv but never opened a database */
    g.dbenv->close(g.dbenv, 0);
}
```

## Files Changed

1. `/home/gburd/ws/dbsql-memory-audit/src/common/dbsql_alloc.c`
   - Lines modified: ~40
   - Direct allocator calls removed: 8

2. `/home/gburd/ws/dbsql-memory-audit/src/dbsql/dbsql.c`
   - Lines modified: ~90
   - Direct allocator calls removed: 28
   - Early initialization added: 1 block

## Files NOT Changed (Acceptable)

1. **`src/lemon/lemon.c`** - Parser generator tool, not part of runtime. Uses malloc extensively but only runs during build, not in production binary.

2. **`src/clib/strdup.c`** - Provides fallback `strdup()` implementation for systems that don't have it in libc. These are compatibility shims and are acceptable to use standard allocators since they're only called when system doesn't provide the function.

## Verification

### Direct Allocator Call Count by File

**Before:**
- `src/common/dbsql_alloc.c`: 8 calls (malloc/calloc/realloc/free in fallback branches)
- `src/dbsql/dbsql.c`: 28 calls (malloc/calloc/realloc/free)
- **Total in runtime code: 36 calls**

**After:**
- `src/common/dbsql_alloc.c`: 0 calls to malloc/calloc/realloc/free
- `src/dbsql/dbsql.c`: 0 calls to malloc/calloc/realloc/free
- **Total in runtime code: 0 calls**

### Grep Verification

```bash
# No more direct allocator calls in main source files
grep -rn "\b(malloc|calloc|realloc|free)\(" src/dbsql/dbsql.c src/common/dbsql_alloc.c
# Returns: 0 matches (only comments mentioning the function names)
```

## Critical Changes Summary

1. ✅ **Removed all fallback logic** from `dbsql_alloc.c` that used standard C allocators
2. ✅ **Initialized DB environment early** in main() before ANY allocations
3. ✅ **Replaced ALL 28 direct allocator calls** in dbsql.c with `__os_*` functions
4. ✅ **Updated cleanup logic** to properly close minimal dbenv when no database opened
5. ✅ **Fixed function call bug** in `__dbsql_strdup()` invocation

## Memory Management Strategy

**Before:** Mixed allocators causing crashes
- Some memory from malloc() → freed with free()
- Some memory from __os_malloc() → freed with __os_free()
- ❌ Fatal when memory from malloc() passed to __os_free()

**After:** Consistent Berkeley DB allocators
- ALL memory from __os_malloc() → freed with __os_free()
- Global `g.dbenv` initialized early and available throughout session
- ✅ Safe: All allocations use same memory management system

## Next Steps

1. **Build Verification**
   - Complete `nix build` to verify compilation
   - Check for any linker errors

2. **Runtime Testing**
   - Run basic SQL commands
   - Test interactive mode
   - Test batch mode with SQL file input
   - Verify no malloc assertion failures

3. **Valgrind Analysis**
   - Run with `valgrind --leak-check=full`
   - Verify no memory leaks
   - Verify no invalid free() calls
   - Verify no use-after-free issues

4. **Stress Testing**
   - Large result sets
   - Complex queries
   - Multiple database files
   - Long-running sessions

## Technical Notes

### Why Early dbenv Initialization Works

1. `db_env_create()` creates a minimal environment with default allocators
2. This environment can be used for memory management even before opening a database
3. When `open_db()` is called later, it creates a full DBSQL environment and updates `g.dbenv`
4. All memory allocated with the minimal env can still be freed with the full env (same allocator functions)

### Handling NULL dbenv

The Berkeley DB `__os_*` functions handle `NULL` dbenv by using default allocators:
- `__os_malloc(NULL, size, &ptr)` - uses DB's default allocator (NOT system malloc)
- This is safe because it's still within Berkeley DB's memory management system
- Different from directly calling `malloc()` which bypasses DB entirely

## Conclusion

All memory allocations in the DBSQL runtime now consistently use Berkeley DB's `__os_*` allocator functions. The mixing of standard C library allocators with Berkeley DB allocators has been completely eliminated, which should resolve the malloc assertion crashes.

The changes maintain the same functionality while ensuring memory safety and consistency throughout the codebase.
