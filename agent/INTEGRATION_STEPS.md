# Phase 3 DML Opcodes - Integration Steps

## Quick Start

The implementation is complete and ready for integration. All code is in `/home/gburd/ws/dbsql/vdbe_dml_opcodes.c.new`.

## Step 1: Add Helper Functions to vdbe.c

**Location:** Insert after line 401, before `__vdbe_boolean_value()`

**Functions to add:**
1. `__vdbe_put_varint()` - Write varint to buffer
2. `__vdbe_varint_len()` - Calculate varint length
3. `__vdbe_serial_type()` - Determine SQLite serial type
4. `__vdbe_serial_type_len()` - Get data length for serial type
5. `__vdbe_serial_put()` - Write serialized data

**Command:**
```bash
# Extract helper functions from vdbe_dml_opcodes.c.new
sed -n '/^\/\* __vdbe_put_varint/,/^\/\* __vdbe_boolean_value/p' vdbe_dml_opcodes.c.new | head -n -1
```

## Step 2: Add Opcode Cases to __vdbe_exec()

**Location:** Insert in __vdbe_exec() switch statement after line ~3690 (after OP_Next/OP_Prev)

**Opcodes to add:**
1. `case OP_NewRowid:` - Generate unique rowid
2. `case OP_Insert:` - Insert table record
3. `case OP_Delete:` - Delete at cursor position
4. `case OP_MakeRecord:` - Pack registers into record format
5. `case OP_IdxInsert:` - Insert into index

**Command:**
```bash
# Extract opcode implementations
sed -n '/^\/\* Opcode: NewRowid/,/^}/p' vdbe_dml_opcodes.c.new
```

## Step 3: Add OPFLAG Constants

**Location:** Add to `src/inc/vdbe.h` or `src/inc/dbsql_int.in`

```c
/* OPFLAG_* values for opcode P5 flags */
#define OPFLAG_NCHANGE       0x01  /* Increment change counter */
#define OPFLAG_LASTROWID     0x02  /* Set db->lastRowid */
#define OPFLAG_USESEEKRESULT 0x10  /* Use cached seekResult */
#define OPFLAG_APPEND        0x08  /* Hint: appending to end */
#define OPFLAG_SAVEPOSITION  0x02  /* Save cursor position after op */
#define OPFLAG_AUXDELETE     0x04  /* Not the primary delete */
#define OPFLAG_ISUPDATE      0x20  /* Part of UPDATE (not INSERT) */
#define OPFLAG_PREFORMAT     0x80  /* Key already in correct format */
```

## Step 4: Verify Dependencies

All required functions exist in src/sm.c:
- ✓ `__sm_insert_ex()` - Line 1966
- ✓ `__sm_delete_ex()` - Line 2062
- ✓ `__sm_integer_key()` - Line 1904
- ✓ `__sm_last_ex()` - Wrapper exists
- ✓ `__sm_table_moveto()` - Exists
- ✓ `__dbsql_random_bytes()` - Exists

All required types exist:
- ✓ `Mem` / `mem_t` - vdbe_int.h:224
- ✓ `VdbeCursor` - vdbe_int.h:103
- ✓ `sm_payload_t` - dbsql_int.in:481
- ✓ `vdbe_t` - vdbe_int.h:452

## Step 5: Compile and Test

```bash
cd /home/gburd/ws/dbsql
make clean
make

# Run basic tests
./test/scr001  # Basic SQL operations
```

## Step 6: Update Task Status

Update Task #20 to completed once tests pass.

## Detailed File Edits

### Edit 1: vdbe.c - Add Helper Functions

```bash
# Find insertion point
grep -n "__vdbe_boolean_value" src/vdbe.c

# Insert before that function (line ~402)
# Copy lines from vdbe_dml_opcodes.c.new starting at:
#   "/* __vdbe_put_varint --"
# Ending before:
#   "/* __vdbe_boolean_value --"
```

### Edit 2: vdbe.c - Add Opcodes

```bash
# Find insertion point
grep -n "case OP_Next:" src/vdbe.c

# Insert after OP_Next/OP_Prev block (after "break;" ~line 3690)
# Copy lines from vdbe_dml_opcodes.c.new starting at:
#   "/* Opcode: NewRowid"
# Through end of file (all 5 opcodes)
```

### Edit 3: Add Flags

```bash
# Add to src/inc/vdbe.h after existing #define constants
# Or add to src/inc/dbsql_int.in near SM_DELETE_* flags
```

## Verification Checklist

After integration:

- [ ] `make` completes without errors
- [ ] `make` completes without warnings
- [ ] Helper functions compile (check for undefined types)
- [ ] Opcodes compile (check for undefined functions)
- [ ] Test basic INSERT: `INSERT INTO t VALUES (1, 'test')`
- [ ] Test basic DELETE: `DELETE FROM t WHERE id=1`
- [ ] Test CREATE INDEX + INSERT
- [ ] Test AUTOINCREMENT

## Rollback Plan

If issues occur:
1. Revert vdbe.c: `git checkout src/vdbe.c`
2. Revert headers: `git checkout src/inc/vdbe.h src/inc/dbsql_int.in`
3. Rebuild: `make clean && make`

## Common Issues

### Issue: Undefined `aMem`
**Fix:** Ensure opcodes are inside `__vdbe_exec()` function, not at file level

### Issue: Undefined `DBSQL_ASSERT`
**Fix:** Change to `DBSQL_ASSERT` (uppercase) or add `#define DBSQL_ASSERT assert`

### Issue: Undefined `VdbeCursor` fields
**Fix:** Verify vdbe_int.h is included and VdbeCursor definition is complete

### Issue: Missing `__sm_last_ex`
**Fix:** Use `__sm_last()` directly, it returns same error code

### Issue: Varint functions conflict
**Fix:** Rename to `__vdbe_put_varint32()` if 32-bit version exists

## Next Steps After Integration

1. **Update Code Generators:** Modify src/cg_*.c to emit new opcodes
   - cg_insert.c: Use OP_NewRowid, OP_Insert, OP_MakeRecord
   - cg_delete.c: Use OP_Delete
   - cg_build.c: Use OP_IdxInsert for indexes

2. **Deprecate Old Opcodes:** Mark OP_NewRecno, OP_PutIntKey, etc. as legacy

3. **Add Tests:** Create comprehensive test suite in test/

4. **Performance Tuning:** Profile and optimize hot paths

5. **Documentation:** Update user docs with new opcode semantics

## Support

For questions or issues:
- Review `phase3_dml_opcodes_implementation.md` for detailed design
- Check `vdbe_analysis_report.md` for architecture overview
- Compare with SQLite 3.52.0 reference in `_/sqlite/src/vdbe.c`
