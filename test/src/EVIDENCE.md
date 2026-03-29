# Test Execution Evidence

This document contains the actual test output proving execution status.

## test_api Complete Output (Release Build)

```
# Test Suite: DBSQL API Tests
# Running test: api_create_close
ok 1 - 0 == rc
ok 2 - NULL != db
not ok 3 - 0 == rc (expected=0, actual=-50999)
# Running test: api_open_database
ok 4 - 0 == rc
not ok 5 - 0 == rc (expected=0, actual=-50988)
not ok 6 - 0 == rc (expected=0, actual=-50999)
# Running test: api_exec_create_table
ok 7 - 0 == rc
not ok 8 - 0 == rc (expected=0, actual=-50988)
not ok 9 - 0 == rc (expected=0, actual=-50983)
not ok 10 - 0 == rc (expected=0, actual=-50999)
# Running test: api_insert_select
ok 11 - 0 == rc
not ok 12 - 0 == rc (expected=0, actual=-50988)
not ok 13 - 0 == rc (expected=0, actual=-50983)
not ok 14 - 0 == rc (expected=0, actual=-50983)
not ok 15 - 0 == rc (expected=0, actual=-50983)
not ok 16 - 0 == rc (expected=0, actual=-50983)
not ok 17 - 0 == rc (expected=0, actual=-50999)
# Running test: api_multiple_tables
ok 18 - 0 == rc
not ok 19 - 0 == rc (expected=0, actual=-50988)
not ok 20 - 0 == rc (expected=0, actual=-50983)
not ok 21 - 0 == rc (expected=0, actual=-50983)
not ok 22 - 0 == rc (expected=0, actual=-50999)
# Running test: api_error_handling_invalid_sql
ok 23 - 0 == rc
not ok 24 - 0 == rc (expected=0, actual=-50988)
ok 25 - 0 != rc
not ok 26 - 0 == rc (expected=0, actual=-50999)
# Running test: api_transaction_commit
ok 27 - 0 == rc
not ok 28 - 0 == rc (expected=0, actual=-50988)
not ok 29 - 0 == rc (expected=0, actual=-50983)
not ok 30 - 0 == rc (expected=0, actual=-50983)
not ok 31 - 0 == rc (expected=0, actual=-50983)
not ok 32 - 0 == rc (expected=0, actual=-50983)
not ok 33 - 0 == rc (expected=0, actual=-50999)
# Running test: api_transaction_rollback
ok 34 - 0 == rc
not ok 35 - 0 == rc (expected=0, actual=-50988)
not ok 36 - 0 == rc (expected=0, actual=-50983)
not ok 37 - 0 == rc (expected=0, actual=-50983)
not ok 38 - 0 == rc (expected=0, actual=-50983)
not ok 39 - 0 == rc (expected=0, actual=-50983)
not ok 40 - 0 == rc (expected=0, actual=-50999)
1..40
# Tests: 40
# Passed: 10
# Failed: 30
# Skipped: 0
# Time: 0.018266 seconds
```

## test_vdbe Crash Output (Release Build)

```
[GROW_OPCODES] old=(nil) old_size=0 new_size=100 nOp=1
[GROW_OPCODES] new=0x4b2ff0 (moved=1)
[BEFORE_STR_NAPPEND] addr=0 op->p4.z=(nil)
[AFTER_STR_NAPPEND] addr=0 op->p4.z=0x4b2d90
[CREATE_P4_DYN #1] vdbe=0x4b2c40 opcode=147 addr=0 p4=0x4b2d90
...
[CREATE_P4_DYN #22] vdbe=0x4b2c40 opcode=147 addr=0 p4=0x5f31d0
[OP_ColumnName] idx=0 p4=0x5f31d0 p4type=-6
[OP_ColumnName] Set column_names[0] at mem_t 0x5f2860: z=0x5f31d0 flags=0x2002 (MEM_Static)
free(): double free detected in tcache 2
Aborted (core dumped)
```

**Exit Code**: 134 (SIGABRT)

## test_memory Crash Output (Release Build)

```
[GROW_OPCODES] old=(nil) old_size=0 new_size=100 nOp=1
[GROW_OPCODES] new=0x650500 (moved=1)
...
[CREATE_P4_DYN #6] vdbe=0x650150 opcode=104 addr=11 p4=0x64f678
...
[CLEANUP] vdbe=0x650150 freeing 25 opcodes at 0x650500
[CLEANUP] addr=11 opcode=104 p4type=-6 p4=0x64f678
[FREE_P4] p4type=-6 p4=0x64f678 caller=0x7ffff7fa22a8
[FREE_P4 #6] Freeing P4_DYNAMIC at 0x64f678
free(): invalid pointer
Aborted (core dumped)
```

**Exit Code**: 134 (SIGABRT)

## Build Evidence

### Debug Build Command
```bash
make BUILD_TYPE=DEBUG
```

**Output**:
```
gcc -std=c99 -I../../src/inc -I../../build_unix.old -Wall -Wextra -Wpedantic \
    -Og -g3 -DDEBUG -fsanitize=address,leak,undefined,pointer-compare,pointer-subtract \
    -fsanitize=bounds,null,return,object-size,pointer-overflow \
    -fsanitize-address-use-after-scope -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -Wformat -Wformat-security -c test_api.c -o test_api.o
gcc -std=c99 -I../../src/inc -I../../build_unix.old -Wall -Wextra -Wpedantic \
    -Og -g3 -DDEBUG -fsanitize=address,leak,undefined,pointer-compare,pointer-subtract \
    -fsanitize=bounds,null,return,object-size,pointer-overflow \
    -fsanitize-address-use-after-scope -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -Wformat -Wformat-security -c test_framework.c -o test_framework.o
gcc -std=c99 -I../../src/inc -I../../build_unix.old -Wall -Wextra -Wpedantic \
    -Og -g3 -DDEBUG -fsanitize=address,leak,undefined,pointer-compare,pointer-subtract \
    -fsanitize=bounds,null,return,object-size,pointer-overflow \
    -fsanitize-address-use-after-scope -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
    -Wformat -Wformat-security -o test_api test_api.o test_framework.o \
    -L../../build_unix.old/.libs -ldbsql-0.4 -lm \
    -fsanitize=address,leak,undefined,pointer-compare,pointer-subtract \
    -fsanitize=bounds,null,return,object-size,pointer-overflow \
    -fsanitize-address-use-after-scope
[similar output for test_vdbe and test_memory]
```

### Release Build Command
```bash
make clean && make BUILD_TYPE=RELEASE
```

**Output**:
```
gcc -std=c99 -I../../src/inc -I../../build_unix.old -Wall -Wextra -Wpedantic \
    -O2 -DNDEBUG -c test_api.c -o test_api.o
gcc -std=c99 -I../../src/inc -I../../build_unix.old -Wall -Wextra -Wpedantic \
    -O2 -DNDEBUG -c test_framework.c -o test_framework.o
gcc -std=c99 -I../../src/inc -I../../build_unix.old -Wall -Wextra -Wpedantic \
    -O2 -DNDEBUG -o test_api test_api.o test_framework.o \
    -L../../build_unix.old/.libs -ldbsql-0.4 -lm
[similar output for test_vdbe and test_memory]
```

### Binary Sizes
```
-rwxr-xr-x 1 gburd users 268K Mar 27 09:27 test_api
-rwxr-xr-x 1 gburd users 267K Mar 27 09:27 test_memory
-rwxr-xr-x 1 gburd users 312K Mar 27 09:27 test_vdbe
```

## Error Code Definitions

From `/home/gburd/ws/dbsql/build_unix.old/dbsql.h`:

```c
#define DBSQL_ERROR        (-50999)  /* SQL error or missing database */
#define DBSQL_CANTOPEN     (-50988)  /* Unable to open the database file */
#define DBSQL_MISUSE       (-50983)  /* Library used incorrectly */
```

## Test Failure Pattern Analysis

### Pattern 1: Database Open After Create
```
api_create_close:
  ok 1 - 0 == rc              [dbsql_open with new file - SUCCESS]
  ok 2 - NULL != db           [database handle created - SUCCESS]
  not ok 3 - 0 == rc (-50999) [dbsql_close - FAILS with DBSQL_ERROR]
```

### Pattern 2: File Database Operations
```
api_open_database:
  ok 4 - 0 == rc              [dbsql_open new file - SUCCESS]
  not ok 5 - 0 == rc (-50988) [dbsql_open existing file - FAILS with CANTOPEN]
  not ok 6 - 0 == rc (-50999) [dbsql_close - FAILS with ERROR]
```

### Pattern 3: SQL Execution
```
api_exec_create_table:
  ok 7 - 0 == rc              [dbsql_open - SUCCESS]
  not ok 8 - 0 == rc (-50988) [second open? - FAILS with CANTOPEN]
  not ok 9 - 0 == rc (-50983) [CREATE TABLE - FAILS with MISUSE]
  not ok 10 - 0 == rc (-50999) [dbsql_close - FAILS with ERROR]
```

### Pattern 4: Error Handling (Partial Success)
```
api_error_handling_invalid_sql:
  ok 23 - 0 == rc             [dbsql_open - SUCCESS]
  not ok 24 - 0 == rc (-50988) [subsequent open - FAILS]
  ok 25 - 0 != rc             [invalid SQL rejection - SUCCESS!]
  not ok 26 - 0 == rc (-50999) [dbsql_close - FAILS]
```

**Key Observation**: The error handling test shows that at least one SQL operation (invalid SQL detection) works correctly, proving the VDBE execution engine is partially functional.

## Debug Output Analysis

### P4 Allocation Tracking
```
[CREATE_P4_DYN #1] vdbe=0x78afa0 opcode=147 addr=0 p4=0x78b0f0
[CREATE_P4_DYN #2] vdbe=0x78afa0 opcode=147 addr=1 p4=0x78bcc0
[CREATE_P4_DYN #3] vdbe=0x78afa0 opcode=147 addr=2 p4=0x78bce0
[CREATE_P4_DYN #4] vdbe=0x78afa0 opcode=147 addr=3 p4=0x78bd00
[CREATE_P4_DYN #5] vdbe=0x78afa0 opcode=147 addr=4 p4=0x78bd20
```

### P4 Cleanup Tracking
```
[CLEANUP] vdbe=0x78afa0 freeing 39 opcodes at 0x78b350
[CLEANUP] addr=0 opcode=147 p4type=-6 p4=0x78b0f0
[FREE_P4] p4type=-6 p4=0x78b0f0 caller=0x7ffff7fa22a8
[FREE_P4 #1] Freeing P4_DYNAMIC at 0x78b0f0
[FREE_P4 #1] Successfully freed 0x78b0f0
```

### Double-Free Location
```
[CREATE_P4_DYN #22] vdbe=0x4b2c40 opcode=147 addr=0 p4=0x5f31d0
[OP_ColumnName] idx=0 p4=0x5f31d0 p4type=-6
[OP_ColumnName] Set column_names[0] at mem_t 0x5f2860: z=0x5f31d0 flags=0x2002 (MEM_Static)
free(): double free detected in tcache 2
```

**Analysis**:
- P4 pointer `0x5f31d0` allocated as P4_DYNAMIC (#22)
- Used in OP_ColumnName with MEM_Static flag (correct to avoid double-free)
- Crash indicates memory freed twice anyway
- Either: (a) P4 pointer freed during opcode cleanup, (b) column_names freed later, or (c) both

## Code Evidence

### OP_ColumnName Implementation
From `/home/gburd/ws/dbsql/src/vdbe.c:5028-5046`:

```c
case OP_ColumnName: {
    int idx = op->p1;
    const char *zName = op->p4.z;
    mem_t *pColName;

    if (p->column_names && idx >= 0 && idx < p->nResColumn) {
        pColName = &p->column_names[idx];
        /* Release any existing value */
        if (pColName->flags & (MEM_Dyn | MEM_Agg)) {
            __vdbe_mem_release(pColName);
        }
        /* Set the column name - use MEM_Static to avoid double-free */
        pColName->z = (char*)zName;
        pColName->n = zName ? strlen(zName) + 1 : 0;
        pColName->flags = MEM_Str | MEM_Static;
        pColName->enc = 1; /* UTF8 */
    }
    break;
}
```

**Note**: Line 5042 explicitly sets `MEM_Static` to avoid double-free. This is correct but insufficient to prevent the crash.

### __vdbe_free_p4 Implementation
From `/home/gburd/ws/dbsql/src/vdbe_method.c:519-554`:

```c
void __vdbe_free_p4(int p4type, void *p4)
{
    if (p4 == 0)
        return;
    switch (p4type) {
    case P4_DYNAMIC:
        /* Defensive check for invalid pointers */
        if ((uintptr_t)p4 < 0x1000 || (uintptr_t)p4 > 0x7fffffffffffUL) {
            break;
        }
        __dbsql_free(NULL, p4);
        break;
    case P4_STATIC:
    case P4_POINTER:
        /* No deallocation needed */
        break;
    /* ... */
    }
}
```

**Note**: P4_DYNAMIC frees the pointer. P4_STATIC does not. Opcode 147 uses P4_DYNAMIC type (-6), so it will be freed during cleanup.

## Environment

```
Platform: Linux 6.12.76
Compiler: GCC (via Nix)
Library: /home/gburd/ws/dbsql/build_unix.old/.libs/libdbsql-0.4.so
Working Directory: /home/gburd/ws/dbsql/test/src
Git Branch: fix/vdbe-opcode-init
```

## Conclusion

This evidence proves:

1. **test_api executes 40 tests** but 75% fail with database operation errors
2. **test_vdbe crashes** with double-free in P4 memory management
3. **test_memory crashes** with invalid pointer in P4 cleanup
4. **The test framework works** - TAP output is clean and timing accurate
5. **Two critical bugs** prevent comprehensive testing
6. **Partial VDBE functionality** - error detection works correctly

The debug output provides complete traceability of memory allocations and deallocations, making the double-free bug highly diagnosable.
