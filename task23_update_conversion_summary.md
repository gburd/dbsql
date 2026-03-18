# Task #23: UPDATE Statement Register-Based Conversion

## Summary
Converted `/home/gburd/ws/dbsql/src/cg_update.c` from stack-based to register-based code generation, following SQLite 3.52.0 architecture.

## Key Changes

### 1. Register Allocation (Lines 83-89)
Added register variables to replace stack-based operations:
- `reg_row_count`: Counter for rows updated
- `reg_old_rowid` / `reg_new_rowid`: Old and new rowid values
- `reg_old`: Base register for old column values (for triggers/FK)
- `reg_new`: Base register for new column values
- `reg_rowset`: RowSet register for multi-pass updates

### 2. Cursor Management (Lines 63-64, 133-144)
- Added `data_cur`: Main table cursor
- Added `idx_cur`: First index cursor
- Properly separated cursor allocation from base cursor number

### 3. Index Register Allocation (Lines 72, 187-251)
- Added `areg_idx[]`: Array of register bases for each index
- Each index gets registers for its key columns
- Main table record gets its own register

### 4. Register-Based Value Loading (Lines 288-303, 335-403)

**Old rowid reading:**
```c
__vdbe_add_op3(v, OP_Rowid, data_cur, reg_old_rowid, 0);
```

**Column reading (triggers):**
```c
for(i = 0; i < table->nCol; i++) {
    if (i == table->iPKey) {
        __vdbe_add_op3(v, OP_Null, 0, reg_old + i, 0);
    } else {
        __vdbe_add_op3(v, OP_Column, data_cur, i, reg_old + i);
    }
}
```

**New value computation:**
```c
for(i = 0; i < table->nCol; i++) {
    j = xref[i];
    if (j < 0) {
        // Column not changed - copy from old or read from cursor
        __vdbe_add_op3(v, OP_Column, data_cur, i, reg_new + i);
    } else {
        // Column changed - evaluate expression
        __expr_code(parser, changes->a[j].pExpr);
        expr_reg = parser->nMem;
        __vdbe_add_op3(v, OP_SCopy, expr_reg, reg_new + i, 0);
    }
}
```

### 5. Register-Based Record Creation (Lines 485-502)

**Old stack-based approach:**
```c
// Push columns onto stack
for (i = 0; i < table->nCol; i++) {
    __vdbe_add_op(v, OP_Column, cur, i);
}
// Stack implicitly forms record
```

**New register-based approach:**
```c
// Values already in reg_new through reg_new+nCol-1
__vdbe_add_op3(v, OP_MakeRecord, reg_new, table->nCol,
               areg_idx[total_num_idx]);
```

### 6. Register-Based Index Updates (Lines 504-527)
```c
for(i = 0, idx = table->pIndex; idx; idx = idx->pNext, i++) {
    if (idx_used[i]) {
        int reg_idx = areg_idx[i];
        // Build index key in registers
        for (j = 0; j < idx->nColumn; j++) {
            int col = idx->aiColumn[j];
            if (col < 0) {
                // Rowid column
                __vdbe_add_op3(v, OP_Copy,
                               recno_will_change_p ?
                               reg_new_rowid : reg_old_rowid,
                               reg_idx + j, 0);
            } else {
                __vdbe_add_op3(v, OP_Copy,
                               reg_new + col,
                               reg_idx + j, 0);
            }
        }
        // Create index record
        __vdbe_add_op3(v, OP_MakeRecord, reg_idx,
                       idx->nColumn, reg_idx + idx->nColumn);
        // Insert into index
        __vdbe_add_op3(v, OP_IdxInsert, idx_cur + i,
                       reg_idx + idx->nColumn, 0);
    }
}
```

### 7. Trigger Integration (Lines 341-403)
Triggers now receive register base pointers instead of pseudo-table cursor numbers:
- `reg_old`: Base of OLD pseudo-table (old column values)
- `reg_new`: Base of NEW pseudo-table (new column values)

Old triggers accessed values via:
```c
__vdbe_add_op(v, OP_OpenPseudo, old_idx, 0);
__vdbe_add_op(v, OP_PutIntKey, old_idx, 0);
```

New triggers use register ranges directly:
```c
__code_row_trigger(parser, TK_UPDATE, changes,
                   TK_BEFORE, table, reg_new, reg_old,
                   on_error, addr);
```

### 8. Opcode Changes

**Stack-based → Register-based opcodes:**
- `OP_Column` now takes P3 (output register) parameter
- `OP_Integer` uses P2 (target register) parameter
- `OP_AddImm` operates on register in P1
- `OP_MakeRecord` uses P1 (base register), P2 (count), P3 (output)
- `OP_Insert` uses P2 (record register), P3 (key register)
- `OP_SCopy` / `OP_Copy` for register-to-register moves

### 9. Function Signature Updates
Updated calls to match new register-based signatures:
- `__generate_constraint_checks()`: Now uses register ranges
- `__generate_row_index_delete()`: Uses data_cur instead of cur
- Removed `__complete_insertion()` in favor of direct `OP_MakeRecord` + `OP_Insert`

## Architecture Pattern

**Before (Stack-based):**
1. Push old rowid
2. Push/compute new values onto stack
3. Stack top = composite record
4. Write from stack

**After (Register-based):**
1. Allocate register ranges upfront
2. Read old values into reg_old + i
3. Compute new values into reg_new + i
4. Use OP_MakeRecord to pack registers into record
5. Use OP_Insert with explicit register references

## Benefits

1. **Explicit data flow**: All values have known register locations
2. **Better optimization**: Register allocation enables better code generation
3. **Clearer semantics**: No implicit stack manipulations
4. **SQLite 3.52.0 compatible**: Matches modern SQLite architecture
5. **Trigger efficiency**: Direct register access instead of pseudo-table overhead

## Files Modified

- `/home/gburd/ws/dbsql/src/cg_update.c`: Complete conversion from stack to register-based

## Verification Needed

1. Compile to verify no syntax errors
2. Test UPDATE statements with:
   - Simple updates (single column)
   - Multi-column updates
   - Updates with WHERE clauses
   - Updates with BEFORE/AFTER triggers
   - Updates affecting indexed columns
   - Updates changing primary key/rowid
3. Test interaction with foreign keys
4. Test view updates

## Status

✅ Code conversion complete
⏳ Compilation testing pending
⏳ Runtime testing pending
