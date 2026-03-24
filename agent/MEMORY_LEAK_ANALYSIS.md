# Memory Leak Analysis - DBSQL

**Date**: 2026-03-24
**Branch**: fix/memory-leaks
**Tool**: Valgrind 3.26.0

## Executive Summary

Valgrind analysis reveals memory leaks in early exit paths of the main() function in `src/dbsql/dbsql.c`. The Berkeley DB environment (`g.dbenv`) is created early but not properly cleaned up on all exit paths.

## Test Results

### Test 1: Version Check (`./result/bin/dbsql --version`)

```
HEAP SUMMARY:
    in use at exit: 2,600 bytes in 3 blocks
  total heap usage: 4 allocs, 1 frees, 6,696 bytes allocated

LEAK SUMMARY:
   definitely lost: 0 bytes in 0 blocks
   indirectly lost: 0 bytes in 0 blocks
     possibly lost: 0 bytes in 0 blocks
   still reachable: 2,600 bytes in 3 blocks
```

**Status**: ✅ No definite leaks (still reachable is acceptable for global DB structures)

### Test 2: Memory Database

Test hung during execution - indicates potential issues with VDBE initialization or database operations.

## Identified Leak Sources

### High Priority (Definite Leaks)

#### 1. Early Exit After --version (Line 1371)
```c
} else if (strcmp(argv[i], "--version") == 0) {
    int major, minor, patch;
    printf("%s\n", dbsql_version(&major, &minor, &patch));
    return 0;  // ❌ LEAKS g.dbenv
}
```

**Impact**: `g.dbenv` is created at line 1337 but never closed.

#### 2. Duplicate --version Handler (Line 1430)
```c
} else if (strcmp(z, "--version") == 0) {
    int major, minor, patch;
    printf("%s\n", dbsql_version(&major, &minor, &patch));
    return 1;  // ❌ LEAKS g.dbenv (also wrong exit code)
}
```

**Impact**: Same leak, plus incorrect exit code (should be 0 for success).

#### 3. Unknown Option Error (Line 1435)
```c
} else {
    fprintf(stderr,"%s: unknown option: %s\n", g.progname, z);
    fprintf(stderr,"Use --help for a list of options.\n");
    return 1;  // ❌ LEAKS g.dbenv
}
```

**Impact**: Error path doesn't cleanup.

#### 4. Meta Command Exit (Line 1445)
```c
if (first_cmd[0] == '.') {
    do_meta_command(first_cmd, &data);
    exit(0);  // ❌ LEAKS g.dbenv
}
```

**Impact**: Direct exit() bypasses cleanup at end of main().

#### 5. SQL Error Exit (Line 1453)
```c
if (rc != 0 && err_msgs != 0) {
    fprintf(stderr,"SQL error: %s\n", err_msgs);
    exit(1);  // ❌ LEAKS g.dbenv
}
```

**Impact**: Error path uses exit() instead of return.

### Medium Priority

#### 6. Usage Function (Line 1373)
```c
} else if (strcmp(argv[i], "--help") == 0) {
    usage(1);  // Calls exit(1) internally
}
```

The `usage()` function needs to be checked for cleanup.

#### 7. Other exit() Calls

Multiple other places call exit() that may be reached after db creation:
- Line 484: set_table_name error
- Line 584: open_db error
- Line 603: close_db error
- Line 858: quit command
- Line 1142, 1151: memory allocation failures
- Line 1251: home directory error
- Line 1300: usage error

### Low Priority (Acceptable)

#### Still Reachable Memory
- 2,600 bytes in 3 blocks from db_env_create
- These are internal Berkeley DB structures
- Acceptable for process exit scenarios
- Not actionable leaks

## Root Cause Analysis

The code creates `g.dbenv` very early (line 1337) to establish memory management, but:

1. **Inconsistent cleanup**: Uses return statements instead of goto cleanup pattern
2. **No cleanup wrapper**: Each exit point must manually cleanup
3. **exit() instead of return**: Several places use exit() which bypasses cleanup

## Recommended Fixes

### Strategy 1: Goto-Based Cleanup (Preferred)

Add a cleanup label at the end of main():

```c
cleanup:
    set_table_name(&data, 0);
    if (g.dbp) {
        g.dbp->close(g.dbp);
    } else if (g.dbenv) {
        g.dbenv->close(g.dbenv, 0);
    }
    return exit_code;
```

Then replace all `return N;` and `exit(N);` with:
```c
exit_code = N;
goto cleanup;
```

### Strategy 2: Cleanup Function

Create a dedicated cleanup function:
```c
static void cleanup_and_exit(int code) {
    set_table_name(&g_data, 0);
    if (g.dbp) {
        g.dbp->close(g.dbp);
    } else if (g.dbenv) {
        g.dbenv->close(g.dbenv, 0);
    }
    exit(code);
}
```

Replace all exits with `cleanup_and_exit(code);`

## Additional Cleanup Items

### data.crypt_key Leak
Line 1367 allocates but never frees:
```c
__dbsql_strdup(NULL, argv[i], &data.crypt_key);
```

This should be freed before exit.

### err_msgs Leak
Line 1321 declares but line 1452 may allocate without freeing:
```c
char *err_msgs = 0;
// ... later ...
rc = g.dbp->exec(g.dbp, first_cmd, callback, &data, &err_msgs);
if (rc != 0 && err_msgs != 0) {
    fprintf(stderr,"SQL error: %s\n", err_msgs);
    exit(1);  // err_msgs never freed
}
```

## Validation Plan

After fixes, verify with:

1. **Version check**: `valgrind --leak-check=full ./result/bin/dbsql --version`
2. **Help**: `valgrind --leak-check=full ./result/bin/dbsql --help`
3. **Unknown option**: `valgrind --leak-check=full ./result/bin/dbsql --invalid`
4. **Meta command**: `valgrind --leak-check=full ./result/bin/dbsql :memory: .quit`
5. **SQL error**: `valgrind --leak-check=full ./result/bin/dbsql :memory: "INVALID SQL"`

Expected result: **definitely lost: 0 bytes**

## Priority Order

1. Fix --version handlers (2 locations) ✅ Quick win
2. Fix unknown option error ✅ Quick win
3. Fix meta command exit ✅ Medium
4. Fix SQL error exit ✅ Medium
5. Review all other exit() calls 🔄 Requires broader analysis
6. Add crypt_key cleanup 🔄 Nice to have
7. Add err_msgs cleanup 🔄 Nice to have
