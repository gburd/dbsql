# CONVERTED Files Discovered!

**Date:** 2026-03-24
**Discovery:** Found pre-converted register-based code generator files

## Critical Finding

There are **already converted** versions of code generator files in the repository:

```
/home/gburd/ws/dbsql/src/cg_insert_CONVERTED.c  (22K)
/home/gburd/ws/dbsql/src/cg_delete_CONVERTED.c  (16K)
```

### File Headers

**cg_insert_CONVERTED.c** header states:
```c
/*
 * CONVERTED TO REGISTER-BASED VDBE (SQLite 3.52.0 architecture)
 * Task #22: Convert INSERT code generator from stack to register-based
 *
 * Key changes:
 * - Use register allocation (regRowid, regData) instead of stack
 * - OP_NewRowid with P2=target register instead of OP_NewRecno
 * - OP_MakeRecord with P1=first_reg, P2=count, P3=dest_reg
 * - OP_Insert with P2=data_reg, P3=key_reg instead of OP_PutIntKey
 * - Register-based trigger code generation
 */
```

This indicates someone already did the work to convert INSERT to register-based!

## Current Build Status

The Makefile currently compiles:
```makefile
$(srcdir)/cg_insert.c    # Stack-based version (original)
```

NOT:
```makefile
$(srcdir)/cg_insert_CONVERTED.c    # Register-based version (converted)
```

## Immediate Action Required

To test the converted code generator:

### Step 1: Update Makefile

Edit `dist/Makefile.in` line 117:
```makefile
# OLD:
$(srcdir)/cg_insert.c $(srcdir)/cg_pragma.c $(srcdir)/cg_select.c \

# NEW:
$(srcdir)/cg_insert_CONVERTED.c $(srcdir)/cg_pragma.c $(srcdir)/cg_select.c \
```

Edit `dist/Makefile.in` line 383:
```makefile
# OLD:
cg_insert@o@: $(srcdir)/cg_insert.c

# NEW:
cg_insert@o@: $(srcdir)/cg_insert_CONVERTED.c
```

### Step 2: Update srcfiles.in

Edit `dist/srcfiles.in` line 52:
```makefile
# OLD:
src/cg_insert.c                    dynamic static

# NEW:
src/cg_insert_CONVERTED.c          dynamic static
```

### Step 3: Rebuild

```bash
nix build
```

### Step 4: Test

```bash
# Test INSERT
echo "CREATE TABLE test(x INTEGER); INSERT INTO test VALUES (1);" | result/bin/dbsql :memory:

# Test SELECT after INSERT
echo "CREATE TABLE test(x); INSERT INTO test VALUES (1); SELECT * FROM test;" | result/bin/dbsql :memory:

# Test ORDER BY with data
echo "CREATE TABLE t(x); INSERT INTO t VALUES (3); INSERT INTO t VALUES (1); INSERT INTO t VALUES (2); SELECT * FROM t ORDER BY x;" | result/bin/dbsql :memory:
```

## What the CONVERTED Files Likely Do

Based on the header comments, the converted files:

1. **Register Allocation**
   - Allocate registers for INSERT values
   - Track register usage
   - Use explicit register numbers instead of stack

2. **New Opcode Parameters**
   - OP_NewRowid: Use P2 for target register (not stack push)
   - OP_MakeRecord: P1=first_reg, P2=count, P3=dest_reg (not stack)
   - OP_Insert: P2=data_reg, P3=key_reg (not stack pop)

3. **Updated Code Flow**
   ```
   OLD (Stack-based):
   1. OP_Integer/String → Push to stack
   2. OP_NewRecno → Push rowid to stack
   3. OP_MakeRecord → Pop N items, push record
   4. OP_PutIntKey → Pop key+data, write to DB

   NEW (Register-based):
   1. OP_Integer/String P2=reg → Store to register
   2. OP_NewRowid P2=reg → Store rowid to register
   3. OP_MakeRecord P1=first, P2=count, P3=dest → Read from registers
   4. OP_Insert P2=data, P3=key → Read from registers, write to DB
   ```

## Compatibility Note

The CONVERTED files expect the executor (vdbe.c) to support register-based opcodes. My earlier conversion of OP_String, OP_NewRecno to dual-mode should help, but the complex opcodes (MakeRecord, Column, PutIntKey) are still stack-only.

Two options:

### Option A: Use CONVERTED Generator + Convert Executor
1. Switch to cg_insert_CONVERTED.c
2. Convert OP_MakeRecord to register-based
3. Convert OP_PutIntKey to OP_Insert (register-based)
4. Convert OP_Column to register-based

### Option B: Keep Stack Generator + Fix Current Issues
1. Keep using cg_insert.c (stack-based)
2. Fix the bugs in current executor
3. Get stack-based INSERT working first
4. Then consider register conversion

## Recommendation

**Try Option A first!** The work is already done. Just need to:
1. Switch Makefile to use CONVERTED files
2. Update executor opcodes to match
3. Test

If Option A doesn't work (mismatches, missing pieces), fall back to Option B.

## Files to Investigate

1. **cg_insert_CONVERTED.c** (22K)
   - Read full file to understand register allocation
   - Identify which opcodes it emits
   - Check if it expects new opcode names (OP_Insert vs OP_PutIntKey)

2. **cg_delete_CONVERTED.c** (16K)
   - Check if DELETE also converted
   - May provide patterns for UPDATE conversion

3. **Original cg_insert.c**
   - Compare with CONVERTED version
   - Understand differences
   - Document conversion pattern

## Next Steps

1. Read cg_insert_CONVERTED.c in full
2. Document what opcodes it expects
3. Update Makefile to use CONVERTED version
4. Convert remaining executor opcodes (MakeRecord, Column, Insert/PutIntKey)
5. Rebuild and test
6. Fix any mismatches

---

**This is a game-changer!** The code generator work may already be done. Just need to integrate it.
