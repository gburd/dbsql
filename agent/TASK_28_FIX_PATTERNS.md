# Task #28 - Quick Fix Pattern Reference

## Search & Replace Patterns

### 1. Database Index Access

**Search**: `table->iDb`
**Replace with**: `__schema_to_index(dbp, table->pSchema)`

**Search**: `index->iDb`
**Replace with**: `__schema_to_index(dbp, index->pSchema)`

**Note**: Make sure `dbp` variable is in scope. If not, use `parser->db`.

### 2. Column Name Access

**Search**: `->zName` (in column context)
**Replace with**: `->zCnName`

**Be careful**: Only change for `column_t` structures, not all structures!

### 3. View Detection

**Search**: `table->x.pSelect != 0`
**Replace with**: `table->eTabType == TABTYP_VIEW`

**Search**: `table->x.pSelect == 0`
**Replace with**: `table->eTabType != TABTYP_VIEW`

**To access view SELECT**:
- Old: `table->x.pSelect`
- New: `table->u.view.pSelect` (when `eTabType == TABTYP_VIEW`)

### 4. Column Default Values

**Old code**:
```c
__vdbe_add_op(v, OP_String, 0, 0);
__vdbe_change_p3(v, -1, table->aCol[i].zDflt, P3_STATIC);
```

**New code**:
```c
if (table->aCol[i].iDflt > 0) {
    int dflt_idx = table->aCol[i].iDflt - 1;
    expr_t *dflt_expr = table->u.tab.pDfltList->a[dflt_idx].pExpr;
    __expr_code(parser, dflt_expr);
} else {
    __vdbe_add_op(v, OP_String, 0, 0);
}
```

### 5. VDBE P3 → P4 for Strings

**Search**: `__vdbe_change_p3(v, -1, "string literal", P3_STATIC)`
**Replace with**: `__vdbe_change_p4(v, -1, "string literal", P3_STATIC)`

**Search**: `__vdbe_change_p3(v, -1, string_var, P3_DYNAMIC)`
**Replace with**: `__vdbe_change_p4(v, -1, string_var, P3_DYNAMIC)`

**Rule**: If the third parameter is a string (char*), use `__vdbe_change_p4`. P3 is now integers only.

### 6. Trigger Context Detection

**Search**: `parser->trigStack`
**Replace with**: `parser->eTriggerOp`

**Logic**:
- Old: `if (parser->trigStack)` meant "in trigger"
- New: `if (parser->eTriggerOp)` means "in trigger" (TK_UPDATE, TK_INSERT, or TK_DELETE)

### 7. ID List Item Mapping

**Problem**: `id_list_item` no longer has `idx` field.

**Solution**: Create local mapping array in INSERT functions:

```c
int *col_idx = 0;
if (column) {
    col_idx = NULL;
    if (__dbsql_calloc(dbp, column->nId, sizeof(int), &col_idx) != 0)
        col_idx = NULL;
    if (col_idx == 0)
        goto cleanup;

    // Initialize and populate
    for(i = 0; i < column->nId; i++) {
        col_idx[i] = -1;
    }
    for(i = 0; i < column->nId; i++) {
        for(j = 0; j < table->nCol; j++) {
            if (strcasecmp(column->a[i].zName,
                      table->aCol[j].zCnName) == 0) {
                col_idx[i] = j;
                break;
            }
        }
    }
}

// Use col_idx[j] instead of column->a[j].idx

cleanup:
    if (col_idx)
        __dbsql_free(dbp, col_idx);
```

## Common Error Messages → Fixes

### "has no member named 'iDb'"
→ Replace with `__schema_to_index(dbp, X->pSchema)` where X is table or index

### "has no member named 'zName'" (column context)
→ Replace with `zCnName`

### "has no member named 'zDflt'"
→ Replace with default value access pattern (see #4 above)

### "has no member named 'x'" (on table_t)
→ Check if accessing view: use `eTabType == TABTYP_VIEW` and `u.view.pSelect`

### "has no member named 'idx'" (on id_list_item)
→ Implement local col_idx array (see #7 above)

### "too many arguments to function '__vdbe_change_p3'"
→ If passing a string, change to `__vdbe_change_p4`

### "has no member named 'trigStack'"
→ Replace with `eTriggerOp`

## Memory Allocation Pattern

**Old**: `ptr = __dbsql_calloc(dbp, n, size)`
**New**: `if (__dbsql_calloc(dbp, n, size, &ptr) != 0) ptr = NULL;`

The new pattern passes a pointer-to-pointer and returns an error code.

## Verification Commands

After each fix:
```bash
cd /home/gburd/ws/dbsql/build_unix
make filename.lo 2>&1 | tee filename.log

# Count remaining errors:
grep -c "error:" filename.log

# If successful:
ls -la .libs/filename.o
```

## Priority Order for Fixes

1. cg_build.c (where helper function lives)
2. cg_delete.c (similar to cg_insert.c)
3. cg_update.c (similar to cg_insert.c)
4. cg_expr.c (critical for expressions)
5. cg_select.c (critical for SELECT)
6. Rest of cg_*.c files
7. vdbe.c and vdbe_method.c
8. api.c and api_table.c
9. Remaining files

## Notes

- Always compile after each logical group of changes
- Don't do global search/replace without verification
- Column name changes require careful context checking
- Some structures still use old field names (transitional period)
- VDBE internals may require deeper understanding
