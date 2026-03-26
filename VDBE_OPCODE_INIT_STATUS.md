# VDBE Opcode Initialization Fix Status

**Date**: March 26, 2026
**Branch**: fix/vdbe-opcode-init
**Commit**: dfe003e

## Problem Statement

SELECT operations crash with "free(): invalid pointer" error during parser cleanup.

## Investigation

### Root Cause Hypothesis

Initial investigation suggested the issue was in `__vdbe_add_op_list` (vdbe_method.c:350-357), where opcodes copied from templates might have uninitialized `p4type` values, leading to invalid pointer frees during cleanup.

### Fixes Applied

1. **__vdbe_add_op_list p4type initialization** (vdbe_method.c:355-358)
   - Simplified logic to set `p4type = P4_NOTUSED` when `p4.p == NULL`
   - Removed incorrect conditional that was clearing valid p4type values for P4_FUNCDEF, P4_KEYINFO, P4_EXPR, etc.

2. **expr_list NULL realloc fix** (cg_expr.c:378-390)
   - Split allocation path: use `__dbsql_malloc` for initial allocation (when `old_allocated == 0`)
   - Use `__dbsql_realloc` only for growth (when `old_allocated > 0`)
   - Berkeley DB's `__os_realloc` doesn't properly handle NULL pointer reallocations

## Current Status: STILL CRASHING

Despite the fixes, SELECT operations still crash with the same error:

```
free(): invalid pointer
```

### Crash Location

- **Function**: `__expr_delete` in cg_expr.c:154
- **Called from**: `__expr_list_delete` in cg_expr.c:409
- **Context**: Parser cleanup during database initialization (init_script SELECT)

### Debug Output Analysis

```
[DEBUG] __expr_list_delete: list=0x418a50, n_expressions=5, a=0x418a58
[DEBUG]   item[0]: pExpr=0x418d60, zEName=(nil)
[DEBUG] __expr_delete: p=0x418d60, op=21 (TK_COLUMN), pLeft=(nil), pRight=(nil), pList=(nil)
[DEBUG] __expr_delete: about to free p=0x418d60  # SUCCESS

[DEBUG]   item[1]: pExpr=0x418b20, zEName=(nil)
[DEBUG] __expr_delete: p=0x418b20, op=21 (TK_COLUMN), pLeft=(nil), pRight=(nil), pList=(nil)
[DEBUG] __expr_delete: about to free p=0x418b20  # CRASH: "free(): invalid pointer"
```

### Key Observations

1. **First expression frees successfully** (0x418d60)
2. **Second expression crashes** (0x418b20)
3. **Both expressions have identical structure**:
   - `op = 21 (TK_COLUMN)`
   - `pLeft = NULL`, `pRight = NULL`, `pList = NULL`
4. **Init script context**: Crash occurs during:
   ```sql
   SELECT type, name, rootpage, sql, 1 FROM temp_master
   UNION ALL
   SELECT type, name, rootpage, sql, 0 FROM master
   ```
   - 5 expressions in select list (type, name, rootpage, sql, 1)
   - First 2 are TK_COLUMN references

### Hypothesis

The pointer 0x418b20 is **invalid** for one of these reasons:

1. **Never properly allocated** - Expression created without proper memory allocation
2. **Already freed** - Double free scenario (freed elsewhere before __expr_list_delete)
3. **Stack/static memory** - Pointer refers to stack or static storage that can't be freed
4. **Corrupt allocation metadata** - Memory allocator's internal structures corrupted

## Next Steps for Investigation

1. **Check expression allocation** in parser grammar (sql_parser.y)
   - Verify all TK_COLUMN expressions use `__dbsql_calloc`/`__dbsql_malloc`
   - Check for stack-allocated expression temporaries

2. **Track allocation/free pairs** for expressions
   - Add allocation tracking to `__dbsql_malloc`/`__dbsql_calloc`
   - Add free tracking to `__dbsql_free`
   - Identify if 0x418b20 was ever allocated

3. **Check __expr_dup** (cg_expr.c:175)
   - Verify expression duplication properly allocates new memory
   - Check if shallow copies create aliased pointers

4. **Use valgrind memcheck** for detailed memory tracking
   ```bash
   valgrind --track-origins=yes --leak-check=full ./dbsql test.db < test.sql
   ```

5. **Check Berkeley DB allocator** (__os_malloc, __os_free)
   - Verify it properly handles the pointer-to-pointer interface
   - Check if there are known issues with BDB 4.8.30 memory allocators

## Files Modified

- `src/vdbe_method.c` - Lines 355-358 (p4type initialization)
- `src/cg_expr.c` - Lines 378-390 (expr_list allocation fix)

## Testing

```bash
# Current test case that triggers crash:
./dbsql test.db <<EOF
CREATE TABLE t1(id INTEGER, name TEXT);
INSERT INTO t1 VALUES(1, 'Alice');
SELECT * FROM t1;
EOF

# Crash occurs during database initialization before user SQL even runs
```

## References

- Task #82: Fix P4_STATIC pointer freeing in __vdbe_free_p4
- Commit 9aa5d69: Previous expr_list structure definition fix
- MEMORY_ISSUES_STATUS.md: Comprehensive memory issue tracking
- DOUBLE_FREE_ANALYSIS.md: Analysis of Issue #1 double free bug
