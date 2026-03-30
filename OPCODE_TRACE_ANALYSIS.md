# DBSQL Prepared Statement Test Results - Opcode Tracing

## Test Execution
```
cd /home/gburd/ws/dbsql/build_unix.old
rm -f vdbe.o .libs/vdbe.o vdbe.lo .libs/vdbe.lo
make clean
# [Skipped - used git restore to get binary instead]

cd /home/gburd/ws/dbsql/test/src
gcc -I../../build_unix.old -I../../src/inc test_prep_rows.c -L../../build_unix.old -ldbsql-test -ldb-4.8 -lm -g -o test_prep_rows
LD_LIBRARY_PATH=../../build_unix.old ./test_prep_rows
```

## Results

### Return Code
- **step() returned:** -50978 (DBSQL_DONE)
- **Expected:** -50979 (DBSQL_ROW)
- **Status:** FAILED - No row returned

### Column Information
- **n_cols:** 1
- **Expected n_cols:** 1
- **Status:** CORRECT - Column count is correct

### Opcodes Executed for SELECT Statement

**Query:** `SELECT * FROM t`  
**Table:** `t` with one inserted row: `(42)`

Complete opcode sequence:
```
PC=0,  opcode=147 (OP_ColumnName), p1=0, p2=0, p3=0
PC=1,  opcode=147 (OP_ColumnName), p1=1, p2=0, p3=0
PC=2,  opcode=5   (OP_Integer),    p1=0, p2=0, p3=0
PC=3,  opcode=62  (OP_OpenRead),   p1=0, p2=2, p3=0
PC=4,  opcode=61  (OP_Rewind),     p1=0, p2=5, p3=0  ← Rewind table, jump to PC=5
PC=5,  opcode=91  (OP_If),         p1=0, p2=10, p3=0 ← JUMP TO PC=10 (WRONG!)
PC=10, opcode=66  (OP_Close),      p1=0, p2=0, p3=0
PC=11, opcode=14  (OP_NullCallback), p1=1, p2=0, p3=0 ← OP_NullCallback EXECUTED
PC=12, opcode=4   (OP_Halt),       p1=0, p2=0, p3=0
```

**Key Observation:** OP_ResultRow (opcode=13) was NEVER executed

### Critical Issue Found

At PC=5, an **OP_If** instruction with p2=10 jumps forward to PC=10, completely bypassing 
the row processing code (which should be at PC=6-9).

**Expected behavior:**
- OP_Rewind sets a flag if table is empty
- OP_If should jump to PC=10 (close table) IF the table IS empty
- If table is NOT empty, execution should continue to PC=6 to fetch and return rows

**Actual behavior:**
- OP_If unconditionally jumps to PC=10 regardless of table contents
- OP_NullCallback executes instead of OP_ResultRow
- Result set appears empty even though data exists

### Root Cause

The SELECT query is not being compiled with the correct opcode sequence. 
The OP_If condition appears to be inverted or the jump targets are incorrect.
The compiled program skips the row iteration logic entirely.

This affects all prepared SELECT statements that should return rows.
