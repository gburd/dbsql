# Fix for cg_pragma.c Static Initializer Errors

## Problem

The file `src/cg_pragma.c` had 20 compilation errors caused by attempting to use string literals in static array initializers for `vdbe_op_t` structures.

### Error Pattern
```c
static vdbe_op_t table_info_preface[] = {
    { OP_ColumnName, 0, 0, "cid"},  // ERROR: string assigned to integer field
    ...
};
```

### Root Cause

The `vdbe_op_t` structure (defined in `src/inc/vdbe.h` lines 69-96) has the following layout:

```c
struct vdbe_op {
    u8 opcode;              // Field 1
    signed char p4type;     // Field 2
    u16 p5;                 // Field 3
    int p1;                 // Field 4 (integer!)
    int p2;                 // Field 5
    int p3;                 // Field 6
    union p4union {         // Field 7 (union containing string pointer)
        char *z;
        int i;
        // ... other union members
    } p4;
};
```

The static initializers `{ OP_ColumnName, 0, 0, "string" }` were attempting to initialize:
1. `opcode` = OP_ColumnName
2. `p4type` = 0
3. `p5` = 0
4. `p1` = "string" **← ERROR: assigning string to integer field**

The compiler error:
```
error: initialization of 'int' from 'char *' makes integer from pointer without a cast
error: initializer element is not computable at load time
```

## Solution

Convert static array initialization to dynamic initialization using `__vdbe_add_op()` + `__vdbe_change_p4()` pattern.

### Before (Broken)
```c
static vdbe_op_t table_info_preface[] = {
    { OP_ColumnName, 0, 0, "cid"},
    { OP_ColumnName, 1, 0, "name"},
    { OP_ColumnName, 2, 0, "type"},
};
__vdbe_add_op_list(v, ARRAY_SIZE(table_info_preface), table_info_preface);
```

### After (Fixed)
```c
__vdbe_add_op(v, OP_ColumnName, 0, 0);
__vdbe_change_p4(v, -1, "cid", P3_STATIC);
__vdbe_add_op(v, OP_ColumnName, 1, 0);
__vdbe_change_p4(v, -1, "name", P3_STATIC);
__vdbe_add_op(v, OP_ColumnName, 2, 0);
__vdbe_change_p4(v, -1, "type", P3_STATIC);
```

## Changes Made

Fixed 5 locations in `src/cg_pragma.c`:

1. **Lines 203-213**: `table_info` pragma - 6 column name operations
2. **Lines 244-252**: `index_info` pragma - 3 column name operations
3. **Lines 279-286**: `index_list` pragma - 3 column name operations
4. **Lines 313-322**: `foreign_key_list` pragma - 5 column name operations
5. **Lines 350-357**: `database_list` pragma - 3 column name operations

## Pattern for Future Use

When you need to create VDBE operations with string parameters:

**DO NOT** use static initialization:
```c
// WRONG - will not compile
static vdbe_op_t ops[] = {
    { OP_Something, p1, p2, "string" }
};
```

**DO** use dynamic initialization:
```c
// CORRECT - compiles and works
__vdbe_add_op(v, OP_Something, p1, p2);
__vdbe_change_p4(v, -1, "string", P3_STATIC);
```

Where:
- `v` is the `vdbe_t*` virtual machine
- `-1` refers to the last added operation
- `P3_STATIC` indicates the string is a static constant (not dynamically allocated)

## Why This Pattern Works

1. `__vdbe_add_op()` creates a VDBE operation with integer parameters
2. `__vdbe_change_p4()` separately sets the P4 union field to point to the string
3. This avoids the static initializer limitation where unions and complex types cannot be initialized with non-constant expressions

## Alternative Approaches Considered

### 1. Designated Initializers (not viable)
```c
{ .opcode = OP_ColumnName, .p1 = 0, .p2 = 0, .p4.z = "cid" }
```
Doesn't work because the union requires the p4type field to be set correctly, and the string would still need to be a compile-time constant.

### 2. Runtime Initialization Functions (unnecessary complexity)
```c
static void init_table_info_preface(vdbe_op_t *ops) { ... }
```
More complex than the simple `__vdbe_add_op` + `__vdbe_change_p4` pattern.

### 3. Using vdbe_op_list_t (wrong type)
The `vdbe_op_list_t` structure (lines 121-126 in vdbe.h) only has 4 signed char fields and is meant for a different use case.

## Result

- All 20 errors in `src/cg_pragma.c` resolved
- File now compiles cleanly with no errors or warnings
- Code is more maintainable and follows existing patterns in the codebase

## Related Files

- `/home/gburd/ws/dbsql/src/cg_pragma.c` - Fixed file
- `/home/gburd/ws/dbsql/src/inc/vdbe.h` - Structure definitions
- `/home/gburd/ws/dbsql/src/vdbe_method.c` - Implementation of `__vdbe_add_op()` and `__vdbe_change_p4()`
