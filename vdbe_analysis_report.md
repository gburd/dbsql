# VDBE Analysis Report: DBSQL (SQLite 2.8.x) vs SQLite 3.52.0

**Date**: March 5, 2026
**Task**: #7 - Analyze VDBE changes and SM compatibility
**Purpose**: Document all VDBE differences and assess BerkeleyDB Storage Manager compatibility

---

## 1. Architecture Comparison: Stack-Based vs Register-Based VDBE

### 1.1 DBSQL Stack-Based VDBE (SQLite 2.8.x)

The DBSQL VDBE is a **stack machine**. All computation flows through an operand stack:

- **Stack pointer**: `mem_t *pTos` tracks the top of the operand stack
- **Instructions**: 3 operands only: `P1` (int), `P2` (int), `P3` (char*)
- **Data flow**: Opcodes push/pop values on `p->aStack[]`
- **Stack manipulation**: Dedicated opcodes `OP_Pop`, `OP_Dup`, `OP_Pull`, `OP_Push`
- **Result delivery**: `OP_Callback` invokes a callback with string arrays
- **Memory cells**: Limited `OP_MemStore`/`OP_MemLoad` for named memory locations
- **Instruction struct**: `vdbe_op_t` with `{opcode, p1, p2, *p3, p3type}`

Key code pattern (from `src/vdbe.c`):
```c
pTos++;              // push
pTos->i = value;     // set integer
pTos->flags = MEM_Int;
// ... later ...
__entity_release_mem(pTos);
pTos--;              // pop
```

### 1.2 SQLite 3.52.0 Register-Based VDBE

The SQLite 3.52.0 VDBE is a **register machine**. Values live in numbered registers:

- **Registers**: `Mem *aMem` array; opcodes reference registers by number
- **Instructions**: 5 operands: `P1` (int), `P2` (int), `P3` (int), `P4` (union), `P5` (u16 flags)
- **Data flow**: Opcodes read/write specific registers (e.g., `pIn1 = &aMem[pOp->p1]`)
- **No stack manipulation**: No Pop/Dup/Pull/Push opcodes needed
- **Result delivery**: `OP_ResultRow` points to a contiguous register range
- **Instruction struct**: `VdbeOp` with `{opcode, p4type, p5, p1, p2, p3, p4(union)}`

Key code pattern (from `_/sqlite/src/vdbe.c`):
```c
pIn1 = &aMem[pOp->p1];   // read input register
pOut = &aMem[pOp->p3];   // write output register
pOut->u.i = pIn1->u.i + pIn2->u.i;
pOut->flags = MEM_Int;
```

### 1.3 Fundamental Differences Summary

| Aspect | DBSQL (Stack) | SQLite 3.52.0 (Register) |
|--------|---------------|--------------------------|
| Operand count | 3 (P1, P2, P3) | 5 (P1, P2, P3, P4, P5) |
| P3 type | `char*` only | `int` (P4 is the rich union) |
| P4 type | N/A | Union: int, ptr, string, i64, real, FuncDef*, KeyInfo*, etc. |
| P5 type | N/A | `u16` flags byte |
| Data passing | Implicit via stack | Explicit register numbers |
| Value type | `mem_t {i, n, flags, r, *z, zShort[32]}` | `Mem {u{r,i,nZero,...}, *z, n, flags, enc, eSubtype, *db, szMalloc, *zMalloc, (*xDel)()}` |
| Integer size | 32-bit `int` | 64-bit `i64` |
| String encoding | Single (native) | UTF-8/UTF-16BE/UTF-16LE |
| BLOB support | No dedicated type | `MEM_Blob` flag |
| Subprogram support | Gosub/Return (fixed-depth stack[100]) | `OP_Program` (VdbeFrame-based, unlimited nesting) |
| Cursor storage | `cursor_t *aCsr` (inline array) | `VdbeCursor **apCsr` (pointer array) |
| Cursor types | 1 (btree + pseudo flag) | 4 (CURTYPE_BTREE, CURTYPE_SORTER, CURTYPE_VTAB, CURTYPE_PSEUDO) |

### 1.4 Impact Assessment

The stack-to-register transition is the **single largest architectural change**. It affects:

1. **Every code generator** (`cg_select.c`, `cg_expr.c`, `cg_where.c`, etc.) -- must emit register-targeted instructions instead of stack-push/pop sequences
2. **The VDBE interpreter** (`vdbe.c`) -- complete rewrite of the dispatch loop
3. **The instruction format** -- `vdbe_op_t` must gain P4 union and P5 fields
4. **The VM state** -- stack pointer replaced by register array management

---

## 2. Complete Opcode Mapping

### 2.1 DBSQL Opcodes (132 total)

```
OP_Goto(1), OP_Gosub(2), OP_Return(3), OP_Halt(4), OP_Integer(5),
OP_String(6), OP_Variable(7), OP_Pop(8), OP_Dup(9), OP_Pull(10),
OP_Push(11), OP_ColumnName(12), OP_Callback(13), OP_NullCallback(14),
OP_Concat(15), OP_Add(16), OP_Subtract(17), OP_Multiply(18),
OP_Divide(19), OP_Remainder(20), OP_Function(21), OP_BitAnd(22),
OP_BitOr(23), OP_ShiftLeft(24), OP_ShiftRight(25), OP_AddImm(26),
OP_ForceInt(27), OP_MustBeInt(28), OP_Eq(29), OP_Ne(30), OP_Lt(31),
OP_Le(32), OP_Gt(33), OP_Ge(34), OP_StrEq(35), OP_StrNe(36),
OP_StrLt(37), OP_StrLe(38), OP_StrGt(39), OP_StrGe(40), OP_And(41),
OP_Or(42), OP_Negative(43), OP_AbsValue(44), OP_Not(45), OP_BitNot(46),
OP_Noop(47), OP_If(48), OP_IfNot(49), OP_IsNull(50), OP_NotNull(51),
OP_MakeRecord(52), OP_MakeIdxKey(53), OP_MakeKey(54), OP_IncrKey(55),
OP_Checkpoint(56), OP_Transaction(57), OP_Commit(58), OP_Rollback(59),
OP_SetFormatVersion(60), OP_SetSchemaSignature(61),
OP_VerifySchemaSignature(62), OP_OpenRead(63), OP_OpenWrite(64),
OP_OpenTemp(65), OP_OpenPseudo(66), OP_Close(67), OP_MoveLt(68),
OP_MoveTo(69), OP_Distinct(70), OP_NotFound(71), OP_Found(72),
OP_IsUnique(73), OP_NotExists(74), OP_NewRecno(75), OP_PutIntKey(76),
OP_PutStrKey(77), OP_Delete(78), OP_KeyAsData(79), OP_RowKey(80),
OP_RowData(81), OP_Column(82), OP_Recno(83), OP_FullKey(84),
OP_NullRow(85), OP_Last(86), OP_Rewind(87), OP_Prev(88), OP_Next(89),
OP_IdxPut(90), OP_IdxDelete(91), OP_IdxRecno(92), OP_IdxLT(93),
OP_IdxGT(94), OP_IdxGE(95), OP_IdxIsNull(96), OP_Destroy(97),
OP_Clear(98), OP_CreateIndex(99), OP_CreateTable(100), OP_ListWrite(101),
OP_ListRewind(102), OP_ListRead(103), OP_ListReset(104),
OP_ListPush(105), OP_ListPop(106), OP_SortPut(107),
OP_SortMakeRec(108), OP_SortMakeKey(109), OP_Sort(110),
OP_SortNext(111), OP_SortCallback(112), OP_SortReset(113),
OP_FileOpen(114), OP_FileRead(115), OP_FileColumn(116),
OP_MemStore(117), OP_MemLoad(118), OP_MemIncr(119), OP_AggReset(120),
OP_AggInit(121), OP_AggFunc(122), OP_AggFocus(123), OP_AggSet(124),
OP_AggGet(125), OP_AggNext(126), OP_SetInsert(127), OP_SetFound(128),
OP_SetNotFound(129), OP_SetFirst(130), OP_SetNext(131), OP_Vacuum(132)
```

### 2.2 SQLite 3.52.0 Opcodes (extracted from `_/sqlite/src/vdbe.c`)

```
OP_Goto, OP_Gosub, OP_Return, OP_InitCoroutine, OP_EndCoroutine,
OP_Yield, OP_HaltIfNull, OP_Halt, OP_Integer, OP_Int64, OP_Real,
OP_String8, OP_String, OP_BeginSubrtn, OP_Null, OP_SoftNull, OP_Blob,
OP_Variable, OP_Move, OP_Copy, OP_SCopy, OP_IntCopy, OP_FkCheck,
OP_ResultRow, OP_Concat, OP_Add, OP_Subtract, OP_Multiply, OP_Divide,
OP_Remainder, OP_CollSeq, OP_BitAnd, OP_BitOr, OP_ShiftLeft,
OP_ShiftRight, OP_AddImm, OP_MustBeInt, OP_RealAffinity, OP_Cast,
OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge, OP_ElseEq, OP_Permutation,
OP_Compare, OP_Jump, OP_And, OP_Or, OP_IsTrue, OP_Not, OP_BitNot,
OP_Once, OP_If, OP_IfNot, OP_IsNull, OP_IsType, OP_ZeroOrNull,
OP_NotNull, OP_IfNullRow, OP_Offset, OP_Column, OP_TypeCheck,
OP_Affinity, OP_MakeRecord, OP_Count, OP_Savepoint, OP_AutoCommit,
OP_Transaction, OP_ReadCookie, OP_SetCookie, OP_ReopenIdx,
OP_OpenRead, OP_OpenWrite, OP_OpenDup, OP_OpenAutoindex,
OP_OpenEphemeral, OP_SorterOpen, OP_SequenceTest, OP_OpenPseudo,
OP_Close, OP_ColumnsUsed, OP_SeekLT, OP_SeekLE, OP_SeekGE, OP_SeekGT,
OP_SeekScan, OP_SeekHit, OP_IfNotOpen, OP_IfNoHope, OP_NoConflict,
OP_NotFound, OP_Found, OP_SeekRowid, OP_NotExists, OP_Sequence,
OP_NewRowid, OP_Insert, OP_RowCell, OP_Delete, OP_ResetCount,
OP_SorterCompare, OP_SorterData, OP_RowData, OP_Rowid, OP_NullRow,
OP_SeekEnd, OP_Last, OP_IfSizeBetween, OP_SorterSort, OP_Sort,
OP_Rewind, OP_IfEmpty, OP_SorterNext, OP_Prev, OP_Next, OP_IdxInsert,
OP_SorterInsert, OP_IdxDelete, OP_DeferredSeek, OP_IdxRowid,
OP_FinishSeek, OP_IdxLE, OP_IdxGT, OP_IdxLT, OP_IdxGE, OP_Destroy,
OP_Clear, OP_ResetSorter, OP_CreateBtree, OP_SqlExec, OP_ParseSchema,
OP_LoadAnalysis, OP_DropTable, OP_DropIndex, OP_DropTrigger,
OP_IntegrityCk, OP_RowSetAdd, OP_RowSetRead, OP_RowSetTest,
OP_Program, OP_Param, OP_FkCounter, OP_FkIfZero, OP_MemMax, OP_IfPos,
OP_OffsetLimit, OP_IfNotZero, OP_DecrJumpZero, OP_AggInverse,
OP_AggStep, OP_AggStep1, OP_AggValue, OP_AggFinal, OP_Checkpoint,
OP_JournalMode, OP_Vacuum, OP_IncrVacuum, OP_Expire, OP_CursorLock,
OP_CursorUnlock, OP_TableLock, OP_VBegin, OP_VCreate, OP_VDestroy,
OP_VOpen, OP_VCheck, OP_VInitIn, OP_VFilter, OP_VColumn, OP_VNext,
OP_VRename, OP_VUpdate, OP_Pagecount, OP_MaxPgcnt, OP_PureFunc,
OP_Function, OP_ClrSubtype, OP_GetSubtype, OP_SetSubtype,
OP_FilterAdd, OP_Filter, OP_Trace, OP_Init, OP_CursorHint,
OP_Abortable, OP_ReleaseReg, OP_Noop
```

### 2.3 Opcode Classification Table

#### PRESERVED: Same name, adaptable semantics (30 opcodes)

These opcodes exist in both systems. The semantics are similar but the operand
encoding changes (stack operands become register references).

| DBSQL Opcode | SQLite 3.52.0 Opcode | Semantic Changes |
|---|---|---|
| `OP_Goto` | `OP_Goto` | Identical: unconditional jump to P2 |
| `OP_Gosub` | `OP_Gosub` | Changed: stores return addr in register P1 instead of fixed stack |
| `OP_Return` | `OP_Return` | Changed: reads return addr from register P1 |
| `OP_Halt` | `OP_Halt` | Enhanced: P1=error code, P2=jump, P3=constraint type, P4=error msg, P5=constraint detail |
| `OP_Integer` | `OP_Integer` | Changed: writes to register P2 (was: push to stack) |
| `OP_String` | `OP_String` | Changed: P1=length, P2=dest register, P4=string |
| `OP_Variable` | `OP_Variable` | Changed: P1=var number, P2=dest register |
| `OP_Concat` | `OP_Concat` | Changed: in1, in2, out3 (register-based) |
| `OP_Add` | `OP_Add` | Changed: in1, in2, out3; 64-bit integer with overflow to float |
| `OP_Subtract` | `OP_Subtract` | Changed: in1, in2, out3 |
| `OP_Multiply` | `OP_Multiply` | Changed: in1, in2, out3 |
| `OP_Divide` | `OP_Divide` | Changed: in1, in2, out3 |
| `OP_Remainder` | `OP_Remainder` | Changed: in1, in2, out3 |
| `OP_Function` | `OP_Function` | Major change: P1=const mask, P2=first arg reg, P3=output reg, P4=FuncDef*, P5=num args |
| `OP_BitAnd` | `OP_BitAnd` | Changed: register-based in1, in2, out3 |
| `OP_BitOr` | `OP_BitOr` | Changed: register-based |
| `OP_ShiftLeft` | `OP_ShiftLeft` | Changed: register-based |
| `OP_ShiftRight` | `OP_ShiftRight` | Changed: register-based |
| `OP_AddImm` | `OP_AddImm` | Changed: adds P2 to register P1 (was: add P1 to TOS) |
| `OP_MustBeInt` | `OP_MustBeInt` | Changed: operates on register P1, jumps to P2 on failure |
| `OP_Eq/Ne/Lt/Le/Gt/Ge` | `OP_Eq/Ne/Lt/Le/Gt/Ge` | Major change: P1=left reg, P3=right reg, P5=flags for NULL handling; uses collation from P4 |
| `OP_And` | `OP_And` | Changed: in1, in2, out3 |
| `OP_Or` | `OP_Or` | Changed: in1, in2, out3 |
| `OP_Not` | `OP_Not` | Changed: in1, out2 |
| `OP_BitNot` | `OP_BitNot` | Changed: in1, out2 |
| `OP_If` | `OP_If` | Changed: reads register P1, jumps to P2 |
| `OP_IfNot` | `OP_IfNot` | Changed: reads register P1, jumps to P2 |
| `OP_IsNull` | `OP_IsNull` | Changed: tests register P1 |
| `OP_NotNull` | `OP_NotNull` | Changed: tests register P1 |
| `OP_Noop` | `OP_Noop` | Identical |
| `OP_Vacuum` | `OP_Vacuum` | Enhanced: P1=database index, P2=page count target |

#### PRESERVED: Cursor & storage opcodes with semantic changes (22 opcodes)

| DBSQL Opcode | SQLite 3.52.0 Opcode | Semantic Changes |
|---|---|---|
| `OP_OpenRead` | `OP_OpenRead` | Enhanced: P1=cursor, P2=root page, P3=db index, P4=KeyInfo*, P5=flags |
| `OP_OpenWrite` | `OP_OpenWrite` | Enhanced: same as OpenRead but writable |
| `OP_OpenPseudo` | `OP_OpenPseudo` | Changed: P1=cursor, P2=content register, P3=num columns |
| `OP_Close` | `OP_Close` | Identical semantics |
| `OP_NotFound` | `OP_NotFound` | Changed: P1=cursor, P2=jump, P3=key register; uses unpacked record |
| `OP_Found` | `OP_Found` | Changed: same as NotFound but inverse |
| `OP_NotExists` | `OP_NotExists` | Changed: P1=cursor, P2=jump, P3=rowid register |
| `OP_Delete` | `OP_Delete` | Enhanced: P1=cursor, P2=update count, P3=record reg, P4=table, P5=flags |
| `OP_RowData` | `OP_RowData` | Changed: P1=cursor, P2=dest register (was: push blob to stack) |
| `OP_Column` | `OP_Column` | Major change: P1=cursor, P2=column#, P3=dest register; uses header-based record format |
| `OP_NullRow` | `OP_NullRow` | Identical: P1=cursor |
| `OP_Last` | `OP_Last` | Changed: P1=cursor, P2=jump if empty |
| `OP_Rewind` | `OP_Rewind` | Changed: P1=cursor, P2=jump if empty |
| `OP_Prev` | `OP_Prev` | Changed: P1=cursor, P2=jump, P3=flags |
| `OP_Next` | `OP_Next` | Changed: P1=cursor, P2=jump, P3=flags |
| `OP_IdxDelete` | `OP_IdxDelete` | Changed: P1=cursor, P2=start reg, P3=num fields |
| `OP_IdxGE` | `OP_IdxGE` | Changed: P1=cursor, P2=jump, P3=key reg, P4=unpacked record |
| `OP_IdxGT` | `OP_IdxGT` | Same pattern |
| `OP_IdxLT` | `OP_IdxLT` | Same pattern |
| `OP_Destroy` | `OP_Destroy` | Changed: P1=root page, P2=write to reg, P3=db index |
| `OP_Clear` | `OP_Clear` | Changed: P1=root page, P2=db index, P3=change count dest |
| `OP_MakeRecord` | `OP_MakeRecord` | Major change: P1=first data reg, P2=num fields, P3=dest reg; creates SQLite3 record format |
| `OP_Checkpoint` | `OP_Checkpoint` | Changed: now WAL checkpoint (different purpose) |
| `OP_Transaction` | `OP_Transaction` | Enhanced: P1=db index, P2=write flag, P3=schema cookie, P4=schema generation, P5=flags |

#### REMOVED: DBSQL opcodes with no SQLite 3.52.0 equivalent (52 opcodes)

These opcodes exist only in DBSQL and must be completely removed:

| DBSQL Opcode | Reason for Removal | Replacement Strategy |
|---|---|---|
| **Stack manipulation** | | |
| `OP_Pop(8)` | No stack in register VM | Remove; register allocation replaces stack management |
| `OP_Dup(9)` | No stack | Use `OP_SCopy` or `OP_Copy` |
| `OP_Pull(10)` | No stack | Use register addressing |
| `OP_Push(11)` | No stack | Use register addressing |
| **Legacy result delivery** | | |
| `OP_ColumnName(12)` | Column names stored differently | Use `sqlite3VdbeSetColName()` at prepare time |
| `OP_Callback(13)` | Stack-based result delivery | Replace with `OP_ResultRow` |
| `OP_NullCallback(14)` | Stack-based | No equivalent needed; handled by result row logic |
| `OP_SortCallback(112)` | Stack-based sort result | Remove; `OP_SorterData` + `OP_ResultRow` |
| **String comparison (typeless)** | | |
| `OP_StrEq(35)` | Typeless comparison | Merged into `OP_Eq` with collation in P4 |
| `OP_StrNe(36)` | Typeless comparison | Merged into `OP_Ne` |
| `OP_StrLt(37)` | Typeless comparison | Merged into `OP_Lt` |
| `OP_StrLe(38)` | Typeless comparison | Merged into `OP_Le` |
| `OP_StrGt(39)` | Typeless comparison | Merged into `OP_Gt` |
| `OP_StrGe(40)` | Typeless comparison | Merged into `OP_Ge` |
| **Legacy arithmetic/value** | | |
| `OP_ForceInt(27)` | Legacy integer coercion | Use `OP_MustBeInt` or `OP_Cast` |
| `OP_Negative(43)` | Unary negate on stack | Use `OP_Subtract` (0 - value) or code gen change |
| `OP_AbsValue(44)` | Abs on stack | Use built-in `abs()` function via `OP_Function` |
| **Legacy key/record ops** | | |
| `OP_MakeKey(54)` | String-based key construction | Replaced by unified `OP_MakeRecord` |
| `OP_MakeIdxKey(53)` | Index key with rowid appended | Replaced by `OP_MakeRecord` |
| `OP_IncrKey(55)` | Increment key for range scans | Replaced by `OP_SeekGT`/`OP_SeekGE` semantics |
| `OP_KeyAsData(79)` | Toggle cursor key-as-data mode | Removed; cursor type (table vs index) is fixed at open |
| `OP_RowKey(80)` | Push entire key to stack | Replaced by `OP_RowData` (for tables) or register reads |
| `OP_Recno(83)` | Push record number to stack | Replaced by `OP_Rowid` (writes to register) |
| `OP_FullKey(84)` | Full key extraction | Replaced by `OP_RowData` |
| **Legacy cursor seek** | | |
| `OP_MoveTo(69)` | Seek by key on stack | Replaced by `OP_SeekGE`/`OP_SeekRowid` |
| `OP_MoveLt(68)` | Seek less-than | Replaced by `OP_SeekLT` |
| `OP_Distinct(70)` | Like NotFound but for DISTINCT | Replaced by `OP_NoConflict` |
| `OP_IsUnique(73)` | Unique constraint check | Replaced by `OP_NoConflict` + `OP_IdxRowid` |
| **Legacy insert** | | |
| `OP_PutIntKey(76)` | Insert with integer key | Replaced by unified `OP_Insert` |
| `OP_PutStrKey(77)` | Insert with string key | Replaced by `OP_IdxInsert` (for index) or `OP_Insert` |
| `OP_NewRecno(75)` | Generate new rowid | Replaced by `OP_NewRowid` |
| `OP_IdxPut(90)` | Insert into index | Replaced by `OP_IdxInsert` |
| `OP_IdxRecno(92)` | Get rowid from index entry | Replaced by `OP_IdxRowid` |
| `OP_IdxIsNull(96)` | Check if index key is null | Handled by `OP_IsNull` on extracted column |
| **List operations (rowid lists)** | | |
| `OP_ListWrite(101)` | Write to keylist | Replaced by `OP_RowSetAdd` |
| `OP_ListRewind(102)` | Rewind keylist | No equivalent needed (RowSet handles this) |
| `OP_ListRead(103)` | Read from keylist | Replaced by `OP_RowSetRead` |
| `OP_ListReset(104)` | Free keylist | No explicit equivalent; RowSet is memory-managed |
| `OP_ListPush(105)` | Save keylist state | Replaced by `OP_RowSetTest` nested handling |
| `OP_ListPop(106)` | Restore keylist state | Same |
| **Sort operations (linked-list sort)** | | |
| `OP_SortPut(107)` | Add to sort list | Replaced by `OP_SorterInsert` |
| `OP_SortMakeRec(108)` | Make sort record | Replaced by `OP_MakeRecord` |
| `OP_SortMakeKey(109)` | Make sort key | Replaced by `OP_MakeRecord` + KeyInfo |
| `OP_SortNext(111)` | Get next sorted | Replaced by `OP_SorterNext` |
| `OP_SortReset(113)` | Free sort list | Replaced by `OP_ResetSorter` |
| **File I/O (COPY FROM -- removed in SQLite 3.0)** | | |
| `OP_FileOpen(114)` | Open file for COPY | Completely removed (COPY statement removed) |
| `OP_FileRead(115)` | Read file line | Completely removed |
| `OP_FileColumn(116)` | Extract column from file line | Completely removed |
| **Legacy aggregate** | | |
| `OP_AggReset(120)` | Reset hash-based aggregator | Replaced by register-based aggregation |
| `OP_AggInit(121)` | Init aggregate column | No equivalent; handled at compile time |
| `OP_AggFunc(122)` | Execute step function (string args) | Replaced by `OP_AggStep` / `OP_AggStep1` |
| `OP_AggFocus(123)` | Select aggregate group | Replaced by cursor-based sorting + grouping |
| `OP_AggSet(124)` | Store into aggregate | Replaced by register writes |
| `OP_AggGet(125)` | Load from aggregate | Replaced by register reads |
| `OP_AggNext(126)` | Iterate aggregate groups | Replaced by `OP_AggFinal` + cursor iteration |
| **Set operations** | | |
| `OP_SetInsert(127)` | Insert into hash set | Replaced by `OP_RowSetAdd` or ephemeral tables |
| `OP_SetFound(128)` | Check membership in set | Replaced by `OP_Found` on ephemeral table |
| `OP_SetNotFound(129)` | Check non-membership | Replaced by `OP_NotFound` on ephemeral table |
| `OP_SetFirst(130)` | Iterate set | Replaced by cursor on ephemeral table |
| `OP_SetNext(131)` | Continue set iteration | Same |
| **Legacy metadata** | | |
| `OP_SetFormatVersion(60)` | Set file format | Replaced by `OP_SetCookie` |
| `OP_SetSchemaSignature(61)` | Set schema cookie | Replaced by `OP_SetCookie` |
| `OP_VerifySchemaSignature(62)` | Verify schema | Handled by `OP_Transaction` P3 schema cookie check |
| `OP_Commit(58)` | Commit transaction | Replaced by `OP_AutoCommit` |
| `OP_Rollback(59)` | Rollback transaction | Replaced by `OP_AutoCommit` |
| **Legacy memory** | | |
| `OP_MemStore(117)` | Store stack to memory cell | Replaced by register writes (direct register addressing) |
| `OP_MemLoad(118)` | Load memory cell to stack | Replaced by register reads |
| `OP_MemIncr(119)` | Increment memory cell | Use `OP_AddImm` on a register |
| `OP_OpenTemp(65)` | Open temporary table | Replaced by `OP_OpenEphemeral` |
| `OP_Sort(110)` | Sort linked list | Replaced by `OP_Sort` (different: triggers sorter) |

#### NEW: SQLite 3.52.0 opcodes not in DBSQL (90+ opcodes)

| Category | New Opcodes | Purpose |
|---|---|---|
| **Coroutines** | `OP_InitCoroutine`, `OP_EndCoroutine`, `OP_Yield` | Coroutine-based subquery evaluation |
| **Enhanced control** | `OP_HaltIfNull`, `OP_Once`, `OP_Jump`, `OP_ElseEq` | Richer control flow |
| **Register data** | `OP_Int64`, `OP_Real`, `OP_String8`, `OP_Blob`, `OP_Null`, `OP_SoftNull`, `OP_BeginSubrtn` | Typed constant loading |
| **Register manipulation** | `OP_Move`, `OP_Copy`, `OP_SCopy`, `OP_IntCopy` | Register-to-register operations |
| **Result** | `OP_ResultRow` | Register-based result delivery |
| **Type system** | `OP_RealAffinity`, `OP_Cast`, `OP_TypeCheck`, `OP_Affinity`, `OP_CollSeq`, `OP_IsType` | Manifest typing support |
| **Comparison** | `OP_Compare`, `OP_Permutation`, `OP_IsTrue`, `OP_ZeroOrNull` | Multi-register comparison, three-valued logic |
| **Null handling** | `OP_IfNullRow`, `OP_IfNotOpen` | Enhanced null/cursor-state checks |
| **Seek operations** | `OP_SeekLT`, `OP_SeekLE`, `OP_SeekGE`, `OP_SeekGT`, `OP_SeekRowid`, `OP_SeekScan`, `OP_SeekHit`, `OP_SeekEnd`, `OP_DeferredSeek`, `OP_FinishSeek` | Rich B-tree seek operations |
| **Index** | `OP_IdxInsert`, `OP_IdxRowid`, `OP_IdxLE`, `OP_NoConflict`, `OP_IfNoHope` | Enhanced index operations |
| **Cursor management** | `OP_ReopenIdx`, `OP_OpenDup`, `OP_OpenAutoindex`, `OP_OpenEphemeral`, `OP_SorterOpen`, `OP_ColumnsUsed`, `OP_SequenceTest`, `OP_CursorLock`, `OP_CursorUnlock`, `OP_CursorHint` | Richer cursor lifecycle |
| **Insert/Rowid** | `OP_Insert`, `OP_NewRowid`, `OP_Rowid`, `OP_Sequence`, `OP_RowCell`, `OP_Offset`, `OP_Count` | Unified insert, enhanced rowid |
| **Sorter** | `OP_SorterInsert`, `OP_SorterData`, `OP_SorterCompare`, `OP_SorterSort`, `OP_SorterNext`, `OP_ResetSorter` | External merge-sort engine |
| **Size/emptiness** | `OP_IfSizeBetween`, `OP_IfEmpty` | Table size estimation |
| **RowSet** | `OP_RowSetAdd`, `OP_RowSetRead`, `OP_RowSetTest` | B-tree based row set |
| **Subprograms** | `OP_Program`, `OP_Param` | Trigger sub-programs (VdbeFrame) |
| **FK constraints** | `OP_FkCheck`, `OP_FkCounter`, `OP_FkIfZero` | Foreign key enforcement |
| **Loop control** | `OP_IfPos`, `OP_OffsetLimit`, `OP_IfNotZero`, `OP_DecrJumpZero`, `OP_MemMax`, `OP_ResetCount` | LIMIT/OFFSET, loop counting |
| **Aggregation** | `OP_AggStep`, `OP_AggStep1`, `OP_AggFinal`, `OP_AggValue`, `OP_AggInverse` | Register-based aggregation + window functions |
| **Transaction** | `OP_Savepoint`, `OP_AutoCommit`, `OP_ReadCookie`, `OP_SetCookie`, `OP_TableLock` | Enhanced transaction control |
| **Schema** | `OP_CreateBtree`, `OP_SqlExec`, `OP_ParseSchema`, `OP_LoadAnalysis`, `OP_DropTable`, `OP_DropIndex`, `OP_DropTrigger`, `OP_IntegrityCk` | Schema management |
| **Journal** | `OP_JournalMode`, `OP_IncrVacuum`, `OP_Expire` | Journal/WAL management |
| **Virtual tables** | `OP_VBegin`, `OP_VCreate`, `OP_VDestroy`, `OP_VOpen`, `OP_VCheck`, `OP_VInitIn`, `OP_VFilter`, `OP_VColumn`, `OP_VNext`, `OP_VRename`, `OP_VUpdate` | Virtual table support |
| **Page management** | `OP_Pagecount`, `OP_MaxPgcnt` | Page count queries |
| **Functions** | `OP_PureFunc` | Pure (deterministic) function optimization |
| **Subtypes** | `OP_ClrSubtype`, `OP_GetSubtype`, `OP_SetSubtype` | JSON/extension subtype support |
| **Bloom filter** | `OP_FilterAdd`, `OP_Filter` | Bloom filter optimization for joins |
| **Tracing** | `OP_Trace`, `OP_Init` | Debug/trace support |
| **Debug** | `OP_Abortable`, `OP_ReleaseReg` | Debug-mode assertions |

---

## 3. Storage Manager (SM) Compatibility Assessment

### 3.1 Current SM API Inventory (from `src/sm.c`)

The DBSQL Storage Manager wraps Berkeley DB with these functions:

#### Database Lifecycle
| SM Function | Signature | Purpose |
|---|---|---|
| `__sm_create` | `(DBSQL*, name, is_temp, in_memory, sm_t**)` | Create/open database |
| `__sm_close_db` | `(sm_t*)` | Close database |
| `__sm_init` | `(sm_t*, int*)` | Initialize SM internal structures |
| `__sm_get_database_name` | `(sm_t*)` | Get database filename |

#### Transaction Control
| SM Function | Signature | Purpose |
|---|---|---|
| `__sm_begin_txn` | `(sm_t*)` | Begin transaction |
| `__sm_commit_txn` | `(sm_t*)` | Commit transaction |
| `__sm_abort_txn` | `(sm_t*)` | Rollback transaction |
| `__sm_checkpoint` | `(sm_t*)` | Checkpoint (sync) |

#### Table/Index Management
| SM Function | Signature | Purpose |
|---|---|---|
| `__sm_create_table` | `(sm_t*, int*)` | Create table, return id |
| `__sm_create_index` | `(sm_t*, int*)` | Create index, return id |
| `__sm_open_table` | `(sm_t*, int*)` | Open existing table |
| `__sm_drop_table` | `(sm_t*, int)` | Drop table by id |
| `__sm_clear_table` | `(sm_t*, int)` | Truncate table by id |

#### Cursor Operations
| SM Function | Signature | Purpose |
|---|---|---|
| `__sm_cursor` | `(sm_t*, int id, int write, sm_cursor_t**)` | Open cursor on table/index |
| `__sm_close_cursor` | `(sm_cursor_t*)` | Close cursor |
| `__sm_moveto` | `(sm_cursor_t*, const void* key, int len, int* result)` | Seek to key |
| `__sm_first` | `(sm_cursor_t*, int* result)` | Move to first entry |
| `__sm_last` | `(sm_cursor_t*, int* result)` | Move to last entry |
| `__sm_next` | `(sm_cursor_t*, int* result)` | Move to next entry |
| `__sm_prev` | `(sm_cursor_t*, int* result)` | Move to previous entry |

#### Data Access
| SM Function | Signature | Purpose |
|---|---|---|
| `__sm_key` | `(sm_cursor_t*, offset, len, void*)` | Read key data |
| `__sm_data` | `(sm_cursor_t*, offset, len, char*)` | Read value data |
| `__sm_key_size` | `(sm_cursor_t*, int*)` | Get key length |
| `__sm_data_size` | `(sm_cursor_t*, int*)` | Get value length |
| `__sm_key_compare` | `(sm_cursor_t*, void*, int, int ignore, int* result)` | Compare key with value |

#### Write Operations
| SM Function | Signature | Purpose |
|---|---|---|
| `__sm_insert` | `(sm_cursor_t*, key, klen, val, vlen)` | Insert key-value pair |
| `__sm_delete` | `(sm_cursor_t*)` | Delete current entry |

#### Metadata
| SM Function | Signature | Purpose |
|---|---|---|
| `__sm_set_format_version` | `(sm_t*, int, u_int32_t)` | Set format version |
| `__sm_get_format_version` | `(sm_t*, u_int32_t*)` | Get format version |
| `__sm_set_schema_sig` | `(sm_t*, u_int32_t)` | Set schema cookie |
| `__sm_get_schema_sig` | `(sm_t*, u_int32_t*)` | Get schema cookie |

#### Internal/Utility
| SM Function | Signature | Purpose |
|---|---|---|
| `__sm_bt_compare` | `(DB*, DBT*, DBT*)` | BDB comparison callback |
| `__sm_cmp_values` | `(a, sz_a, b, sz_b)` | Raw value comparison |
| `__sm_is_threaded` | `(sm_t*)` | Check threading mode |
| `__sm_next_from_seq` | `(sm_t*, DB_TXN*)` | Get next sequence number |
| `__sm_seq_init` | `(sm_t*, DB*, DB_TXN*, name, init)` | Initialize sequence |
| `__sm_meta_init` | `(sm_t*, DB_TXN*, name, init)` | Initialize metadata store |
| `__sm_resource` | `(sm_t*, id*, type, init)` | Generic resource creation |

### 3.2 SQLite 3.52.0 Btree API (as called from vdbe.c)

| SQLite 3.52.0 Btree Function | SM Equivalent | Gap Assessment |
|---|---|---|
| **Database lifecycle** | | |
| `sqlite3BtreeOpen(vfs, name, db, &pBt, flags, vfsFlags)` | `__sm_create()` | SM needs VFS-like flag support |
| `sqlite3BtreeClose(pBt)` | `__sm_close_db()` | Compatible |
| **Transaction** | | |
| `sqlite3BtreeBeginTrans(pBt, wrFlag, &schemaCookie)` | `__sm_begin_txn()` | **GAP**: SM has no write-flag or schema cookie return |
| `sqlite3BtreeCommitPhaseOne(pBt, zSuper)` | N/A | **GAP**: SM has no two-phase commit |
| `sqlite3BtreeCommitPhaseTwo(pBt, bCleanup)` | N/A | **GAP**: SM has no two-phase commit |
| `sqlite3BtreeCommit(pBt)` | `__sm_commit_txn()` | Compatible (convenience wrapper) |
| `sqlite3BtreeRollback(pBt, tripCode, writeOnly)` | `__sm_abort_txn()` | **GAP**: SM has no tripCode/writeOnly params |
| `sqlite3BtreeSavepoint(pBt, op, iSavepoint)` | N/A | **GAP**: SM has no savepoint support |
| `sqlite3BtreeBeginStmt(pBt, iStmt)` | N/A | **GAP**: SM has no statement journals |
| **Table management** | | |
| `sqlite3BtreeCreateTable(pBt, &pgno, flags)` | `__sm_create_table()` / `__sm_create_index()` | **GAP**: SM uses separate calls; 3.52 uses flags (BTREE_INTKEY, BTREE_BLOBKEY) |
| `sqlite3BtreeDropTable(pBt, pgno, &iMoved)` | `__sm_drop_table()` | **GAP**: SM doesn't return moved page |
| `sqlite3BtreeClearTable(pBt, pgno, &nChange)` | `__sm_clear_table()` | **GAP**: SM doesn't return change count |
| `sqlite3BtreeClearTableOfCursor(pCur)` | N/A | **GAP**: New function, clear via cursor |
| **Cursor** | | |
| `sqlite3BtreeCursor(pBt, pgno, wrFlag, pKeyInfo, pCur)` | `__sm_cursor()` | **GAP**: SM has no KeyInfo parameter |
| `sqlite3BtreeCloseCursor(pCur)` | `__sm_close_cursor()` | Compatible |
| `sqlite3BtreeTableMoveto(pCur, intKey, bias, &res)` | `__sm_moveto()` | **GAP**: SM uses byte key; 3.52 uses i64 integer key directly for tables |
| `sqlite3BtreeIndexMoveto(pCur, pIdxKey, &res)` | `__sm_moveto()` | **GAP**: SM uses raw bytes; 3.52 uses UnpackedRecord |
| `sqlite3BtreeFirst(pCur, &res)` | `__sm_first()` | Compatible |
| `sqlite3BtreeLast(pCur, &res)` | `__sm_last()` | Compatible |
| `sqlite3BtreeNext(pCur, flags)` | `__sm_next()` | **Minor GAP**: SM has no flags parameter |
| `sqlite3BtreePrevious(pCur, flags)` | `__sm_prev()` | **Minor GAP**: SM has no flags parameter |
| `sqlite3BtreeEof(pCur)` | N/A | **GAP**: SM signals EOF via result parameter on next/prev |
| `sqlite3BtreeCursorHasMoved(pCur)` | N/A | **GAP**: Cursor stability check |
| `sqlite3BtreeCursorRestore(pCur, &differentRow)` | N/A | **GAP**: Cursor restore after move |
| **Data access** | | |
| `sqlite3BtreePayload(pCur, offset, amt, buf)` | `__sm_key()` + `__sm_data()` | Unified in 3.52; SM separates key/data |
| `sqlite3BtreePayloadSize(pCur)` | `__sm_key_size()` + `__sm_data_size()` | Unified in 3.52 |
| `sqlite3BtreePayloadFetch(pCur, &amt)` | N/A | **GAP**: Zero-copy payload access |
| `sqlite3BtreeIntegerKey(pCur)` | `__sm_key()` (then byte-swap) | **GAP**: SM uses encoded byte key; 3.52 returns i64 directly |
| `sqlite3BtreeOffset(pCur)` | N/A | **GAP**: Returns file offset of current row |
| **Write** | | |
| `sqlite3BtreeInsert(pCur, &payload, flags, seekResult)` | `__sm_insert()` | **GAP**: SM uses (key,klen,val,vlen); 3.52 uses BtreePayload struct + flags + seek hint |
| `sqlite3BtreeDelete(pCur, flags)` | `__sm_delete()` | **GAP**: SM has no flags (BTREE_SAVEPOSITION, BTREE_AUXDELETE, etc.) |
| `sqlite3BtreeTransferRow(pDest, pSrc, iKey)` | N/A | **GAP**: Bulk row transfer between cursors |
| **Metadata** | | |
| `sqlite3BtreeGetMeta(pBt, idx, &val)` | `__sm_get_schema_sig()` etc. | **GAP**: SM has specific getters; 3.52 has generic meta cookie API (16 cookies) |
| `sqlite3BtreeUpdateMeta(pBt, idx, val)` | `__sm_set_schema_sig()` etc. | Same gap |
| **Info/Config** | | |
| `sqlite3BtreeRowCountEst(pCur)` | N/A | **GAP**: Row count estimation |
| `sqlite3BtreeCount(db, pCur, &count)` | N/A | **GAP**: Exact row count |
| `sqlite3BtreeTripAllCursors(pBt, errCode, writeOnly)` | N/A | **GAP**: Invalidate all cursors |
| `sqlite3BtreeTxnState(pBt)` | N/A | **GAP**: Query transaction state |
| `sqlite3BtreeCursorHasHint(pCur, mask)` | N/A | **GAP**: Query optimizer hint |
| `sqlite3BtreeCursorPin/Unpin(pCur)` | N/A | **GAP**: Cursor pinning |
| `sqlite3BtreeFakeValidCursor()` | N/A | **GAP**: Pseudo-cursor factory |
| `sqlite3BtreeIntegrityCheck(...)` | N/A | **GAP**: PRAGMA integrity_check |
| `sqlite3BtreeSetVersion(pBt, ver)` | `__sm_set_format_version()` | Roughly compatible |
| `sqlite3BtreeLastPage(pBt)` | N/A | Not applicable (BDB manages pages internally) |
| `sqlite3BtreeIncrVacuum(pBt)` | N/A | Not applicable (BDB has own space management) |

### 3.3 SM API Extensions Required

Based on the gap analysis, the following SM API extensions are needed:

#### Critical (must have for basic operation)

1. **`__sm_begin_txn_ex(sm, wrFlag, schemaCookie*)`** -- Enhanced transaction begin with write flag and schema cookie return
2. **`__sm_table_moveto(cursor, i64Key, bias, result*)`** -- Integer key seek without byte encoding (tables store i64 rowids directly in 3.52)
3. **`__sm_index_moveto(cursor, pIdxKey, result*)`** -- Seek using UnpackedRecord for index comparison
4. **`__sm_payload(cursor, offset, amt, buf)`** -- Unified key+data access
5. **`__sm_payload_size(cursor)`** -- Unified size query
6. **`__sm_integer_key(cursor)`** -- Return i64 rowid directly
7. **`__sm_insert_ex(cursor, pPayload, flags, seekResult)`** -- Enhanced insert with BtreePayload struct
8. **`__sm_delete_ex(cursor, flags)`** -- Enhanced delete with flags
9. **`__sm_eof(cursor)`** -- Explicit EOF check
10. **`__sm_create_btree(sm, pgno*, flags)`** -- Unified table/index creation with flags

#### Important (needed for correctness)

11. **`__sm_cursor_has_moved(cursor)`** -- Detect if cursor was invalidated
12. **`__sm_cursor_restore(cursor, differentRow*)`** -- Restore invalidated cursor
13. **`__sm_get_meta(sm, idx, val*)`** -- Generic meta cookie access (16 cookies)
14. **`__sm_update_meta(sm, idx, val)`** -- Generic meta cookie update
15. **`__sm_clear_table_ex(sm, id, nChange*)`** -- Clear with change count
16. **`__sm_drop_table_ex(sm, id, movedPage*)`** -- Drop with moved page info
17. **`__sm_txn_state(sm)`** -- Query transaction state
18. **`__sm_row_count_est(cursor)`** -- Estimated row count for query planner

#### Optional (can stub or ignore initially)

19. **`__sm_savepoint(sm, op, iSavepoint)`** -- Savepoint support (maps to BDB nested txns)
20. **`__sm_begin_stmt(sm, iStmt)`** -- Statement journal
21. **`__sm_trip_all_cursors(sm, errCode, writeOnly)`** -- Invalidate cursors
22. **`__sm_payload_fetch(cursor, amt*)`** -- Zero-copy access (optimization)
23. **`__sm_cursor_pin/unpin(cursor)`** -- Cursor pinning (optimization)
24. **`__sm_cursor_hint_flags(cursor, flags)`** -- Query optimizer hints
25. **`__sm_integrity_check(...)`** -- PRAGMA integrity_check support
26. **`__sm_transfer_row(dest, src, key)`** -- Bulk row transfer

---

## 4. Risk Analysis

### 4.1 Critical Risks

| Risk | Impact | Description |
|---|---|---|
| **Stack-to-register rewrite** | ALL code generators | Every code generator (`cg_select.c`, `cg_expr.c`, `cg_where.c`, `cg_build.c`, `cg_insert.c`, `cg_delete.c`, `cg_update.c`, `cg_trigger.c`) emits stack-based VDBE instructions and must be rewritten for register targeting |
| **Record format change** | Data compatibility | DBSQL uses string-based records with custom encoding; SQLite 3.52 uses a header-based binary format with serial types. The `OP_MakeRecord` and `OP_Column` opcodes are completely different |
| **Integer key encoding** | All table operations | DBSQL encodes integer keys as 4-byte big-endian with sign-bit flip (`INT_TO_KEY`/`KEY_TO_INT`); SQLite 3.52 uses i64 natively. All insert/seek/comparison operations change |
| **Aggregate architecture** | All GROUP BY queries | DBSQL uses a hash-based aggregator (`agg_t` with `AggFocus`/`AggNext`); SQLite 3.52 uses sort-based grouping with register accumulation |
| **Comparison semantics** | All WHERE clauses | DBSQL has separate string and numeric comparisons (`OP_StrEq` vs `OP_Eq`); SQLite 3.52 uses unified comparison with type affinity and collation |

### 4.2 High Risks

| Risk | Impact | Description |
|---|---|---|
| **Cursor lifecycle** | SM interface | SQLite 3.52 has cursor stability guarantees (`CursorHasMoved`, `CursorRestore`) that BDB does not natively provide. Need shim layer. |
| **Two-phase commit** | Transaction safety | SQLite 3.52 uses two-phase commit (`CommitPhaseOne`/`CommitPhaseTwo`) for crash recovery. BDB has its own crash recovery but API differs. |
| **Savepoints** | Nested transactions | `OP_Savepoint` requires nested transaction support. BDB supports nested transactions but API mapping needed. |
| **Payload unification** | All data access | SQLite 3.52 treats key+data as unified "payload". SM separates them. Need adapter layer. |
| **KeyInfo/UnpackedRecord** | Index operations | All index seeks in 3.52 use `UnpackedRecord` for comparison. SM uses raw byte comparison via `__sm_bt_compare`. Need to either pass comparison function to SM or implement comparison at VDBE level. |

### 4.3 Medium Risks

| Risk | Impact | Description |
|---|---|---|
| **Virtual tables** | Extension support | 11 new VTable opcodes. Can be initially stubbed/disabled. |
| **Window functions** | SQL compliance | `OP_AggInverse`, `OP_AggValue` for window functions. Can defer. |
| **Bloom filters** | Query optimization | `OP_FilterAdd`/`OP_Filter`. Can stub with no-op (performance only). |
| **Sorter engine** | Large sorts | External merge-sort (`OP_SorterOpen`, etc.) replaces in-memory linked list sort. Need to implement or use simpler sort. |
| **Subtype system** | JSON support | `OP_ClrSubtype`/`OP_GetSubtype`/`OP_SetSubtype` for JSON. Can defer. |

---

## 5. Implementation Recommendations

### 5.1 Phased Approach

**Phase 1: Core Infrastructure** (must complete first)
1. Update `vdbe_op_t` to `VdbeOp` format (add P4 union, P5 field, change P3 to int)
2. Update `mem_t` to `Mem` format (add i64, encoding, BLOB support, xDel)
3. Update `cursor_t` to `VdbeCursor` format (add cursor types, cache fields)
4. Update `vdbe_t` to `Vdbe` format (remove stack, add register array)
5. Implement the 10 critical SM extensions listed in section 3.3

**Phase 2: Core Opcodes** (basic query execution)
1. Register data loading: `OP_Integer`, `OP_Int64`, `OP_Real`, `OP_String8`, `OP_Null`, `OP_Blob`
2. Register manipulation: `OP_Move`, `OP_Copy`, `OP_SCopy`
3. Arithmetic: `OP_Add/Sub/Mul/Div/Rem` (register-based)
4. Comparison: `OP_Eq/Ne/Lt/Le/Gt/Ge` (with collation, NULL handling)
5. Logic: `OP_And`, `OP_Or`, `OP_Not`, `OP_If`, `OP_IfNot`
6. Control: `OP_Goto`, `OP_Gosub`, `OP_Return`, `OP_Halt`
7. Result: `OP_ResultRow`, `OP_Column`, `OP_MakeRecord`

**Phase 3: Cursor Operations** (table access)
1. Open/Close: `OP_OpenRead`, `OP_OpenWrite`, `OP_OpenEphemeral`, `OP_OpenPseudo`, `OP_Close`
2. Seek: `OP_SeekGE/GT/LE/LT`, `OP_SeekRowid`, `OP_NotExists`
3. Navigation: `OP_Next`, `OP_Prev`, `OP_Rewind`, `OP_Last`
4. Index: `OP_Found`, `OP_NotFound`, `OP_IdxInsert`, `OP_IdxDelete`, `OP_IdxRowid`, `OP_IdxGE/GT/LE/LT`
5. DML: `OP_Insert`, `OP_Delete`, `OP_NewRowid`, `OP_RowData`, `OP_Rowid`

**Phase 4: Transaction & Schema** (data integrity)
1. `OP_Transaction`, `OP_AutoCommit`, `OP_Savepoint`
2. `OP_ReadCookie`, `OP_SetCookie`
3. `OP_CreateBtree`, `OP_Destroy`, `OP_Clear`
4. `OP_ParseSchema`, `OP_DropTable`, `OP_DropIndex`, `OP_DropTrigger`

**Phase 5: Advanced Features** (SQL compliance)
1. Aggregation: `OP_AggStep`, `OP_AggFinal`
2. Sorting: `OP_SorterOpen`, `OP_SorterInsert`, `OP_SorterSort`, `OP_SorterNext`, `OP_SorterData`
3. Subprograms: `OP_Program`, `OP_Param` (for triggers)
4. Functions: `OP_Function`, `OP_PureFunc`
5. RowSet: `OP_RowSetAdd`, `OP_RowSetRead`, `OP_RowSetTest`
6. FK: `OP_FkCheck`, `OP_FkCounter`, `OP_FkIfZero`

**Phase 6: Optional Features** (can defer)
1. Virtual tables (11 `OP_V*` opcodes)
2. Window functions (`OP_AggInverse`, `OP_AggValue`)
3. Bloom filters (`OP_FilterAdd`, `OP_Filter`)
4. Subtypes (`OP_ClrSubtype`, `OP_GetSubtype`, `OP_SetSubtype`)
5. Incremental vacuum (`OP_IncrVacuum`)
6. Journal mode (`OP_JournalMode`)

### 5.2 SM Adaptation Strategy

The recommended approach for SM compatibility is a **shim layer**:

1. **Keep the existing `__sm_*` API** -- do not rewrite Berkeley DB integration
2. **Create `__sm_btree_*` adapter functions** that present the `sqlite3Btree*` interface but call through to `__sm_*` internally
3. **Handle key encoding at the shim level** -- translate between i64 rowids and BDB's byte-key encoding
4. **Implement UnpackedRecord comparison in VDBE layer** -- do not push this into BDB; compare decoded values in the VDBE, then use `__sm_moveto()` with raw encoded keys
5. **For payload unification** -- the shim determines whether to call `__sm_key()` or `__sm_data()` based on cursor type (table vs index)
6. **For cursor stability** -- wrap BDB cursors with generation counters to detect invalidation

### 5.3 Files Requiring Changes

| File | Change Scope | Priority |
|---|---|---|
| `src/inc/vdbe.h` | Major rewrite: VdbeOp struct, P4 types | Phase 1 |
| `src/inc/vdbe_int.h` | Major rewrite: Mem, VdbeCursor, Vdbe structs | Phase 1 |
| `src/vdbe.c` | Complete rewrite: all 132 opcodes become 190+ | Phase 1-5 |
| `src/vdbe_method.c` | Major rewrite: VM lifecycle, memory management | Phase 1 |
| `src/sm.c` | Extend: add 10-18 new adapter functions | Phase 1-3 |
| `src/cg_select.c` | Major rewrite: register-targeted code gen | Phase 2-5 |
| `src/cg_expr.c` | Major rewrite: register-targeted code gen | Phase 2-3 |
| `src/cg_where.c` | Major rewrite: register-targeted code gen | Phase 3 |
| `src/cg_build.c` | Major rewrite: register-targeted code gen | Phase 4 |
| `src/cg_insert.c` | Major rewrite | Phase 3 |
| `src/cg_delete.c` | Major rewrite | Phase 3 |
| `src/cg_update.c` | Major rewrite | Phase 3 |
| `src/cg_trigger.c` | Major rewrite (subprograms) | Phase 5 |
| `src/sql_fns.c` | Moderate: update function calling convention | Phase 2 |

---

## 6. Summary

The VDBE transition from stack-based (132 opcodes) to register-based (190+ opcodes) is the most impactful single change in the DBSQL-to-SQLite-3.52.0 upgrade. Key findings:

1. **52 DBSQL opcodes are removed** (stack manipulation, legacy aggregation, file I/O, set operations, string comparisons, legacy key/record format)
2. **30 opcodes are preserved** with semantic changes (stack operands become register references)
3. **22 cursor/storage opcodes** are preserved with enhanced interfaces
4. **90+ new opcodes** are added (coroutines, type system, seek operations, sorter, subprograms, virtual tables, bloom filters, etc.)
5. **The SM requires 10 critical API extensions** and up to 16 additional extensions for full compatibility
6. **A shim layer approach** is recommended to preserve the BDB integration while presenting the SQLite 3.52 Btree interface
7. **The key encoding change** (4-byte encoded int to native i64) affects every table operation and is a primary risk area
8. **The record format change** (string-based to header-based binary) affects all data storage and retrieval

The implementation should proceed in 6 phases, with core infrastructure and basic opcodes first, followed by cursor operations, then transactions, and finally advanced features.

---

## 7. Implementation Task Tracking

### Phase 2: Core Opcodes - Register Load/Store Operations

#### Task #12: Implement helper functions (COMPLETED - March 16, 2026)
Implemented register manipulation helper functions in `src/vdbe.c` and `src/vdbe_method.c`:
- `__vdbe_mem_set_null()` - Set register to NULL
- `__vdbe_mem_set_int64()` - Set register to integer value
- `__vdbe_mem_set_double()` - Set register to floating-point value
- `__vdbe_mem_set_str()` - Set register to string value
- `__vdbe_mem_copy()` - Deep copy between registers
- `__vdbe_mem_move()` - Move content between registers (leaving source as NULL)

#### Task #13: Implement Phase 2 register load/store opcodes (COMPLETED - March 16, 2026)
Replaced stack-based opcodes with register-based versions in `src/vdbe.c`:

1. **OP_Integer** (P1 P2 * * *) - Load integer constant from P1 into register P2
   - Location: line ~1454
   - Uses: `__vdbe_mem_set_int64()`

2. **OP_Real** (* P2 * P4 *) - Load floating-point constant from P4.pReal into register P2
   - Location: line ~1467
   - Uses: `__vdbe_mem_set_double()`
   - Protected by `#ifndef DBSQL_OMIT_FLOATING_POINT`

3. **OP_String8** (* P2 * P4 *) - Load string constant from P4.z into register P2
   - Location: line ~1483
   - Uses: `__vdbe_mem_set_str()` with DBSQL_UTF8_ENCODED and STATIC flag

4. **OP_Null** (P1 P2 P3 * *) - Load NULL into registers P2..P3
   - Location: line ~1530
   - Uses: `__vdbe_mem_set_null()` in a loop
   - Supports MEM_Cleared flag when P1 is non-zero

5. **OP_Copy** (P1 P2 P3 * *) - Deep copy registers P1..P1+P3 to P2..P2+P3
   - Location: line ~1553
   - Uses: `__vdbe_mem_copy()`

6. **OP_Move** (P1 P2 P3 * *) - Move registers P1..P1+P3-1 to P2..P2+P3-1
   - Location: line ~1575
   - Uses: `__vdbe_mem_move()`
   - Leaves source registers as NULL

**Changes made:**
- Added register pointer variables to `__vdbe_exec()`: `aMem`, `pIn1`, `pIn2`, `pIn3`, `pOut`
- Initialized `aMem = p->aMem` to access register array
- Access patterns: `pOut = &aMem[pOp->p2]` instead of `pTos++`
- All 6 opcodes now use register-based addressing instead of stack manipulation

**Testing status:** Code compiles. Functional testing pending code generator updates.
