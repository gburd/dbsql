# ADR-001: Migration from Stack-Based to Register-Based VDBE

**Status**: Accepted

**Date**: March 2026

**Context Owner**: DBSQL Modernization Team

---

## Context

DBSQL 0.4.0 is based on SQLite 2.8.x, which uses a stack-based Virtual Database Engine (VDBE). To modernize to SQLite 3.52.0, we must migrate to a register-based VDBE architecture. This is the single largest architectural change in the modernization effort.

### Current State: Stack-Based VDBE (SQLite 2.8.x)

The DBSQL VDBE is a **stack machine** where all computation flows through an operand stack:

- **Stack pointer**: `mem_t *pTos` tracks the top of the operand stack
- **Instructions**: 3 operands only: `P1` (int), `P2` (int), `P3` (char*)
- **Data flow**: Opcodes push/pop values on `p->aStack[]`
- **Stack manipulation**: Dedicated opcodes `OP_Pop`, `OP_Dup`, `OP_Pull`, `OP_Push`
- **Result delivery**: `OP_Callback` invokes a callback with string arrays
- **Memory cells**: Limited `OP_MemStore`/`OP_MemLoad` for named memory locations

Typical code pattern:
```c
pTos++;                      // push
pTos->i = value;             // set integer
pTos->flags = MEM_Int;
// ... computation ...
__entity_release_mem(pTos);
pTos--;                      // pop
```

### Target State: Register-Based VDBE (SQLite 3.52.0)

The SQLite 3.52.0 VDBE is a **register machine** where values live in numbered registers:

- **Registers**: `Mem *aMem` array; opcodes reference registers by number
- **Instructions**: 5 operands: `P1` (int), `P2` (int), `P3` (int), `P4` (union), `P5` (u16 flags)
- **Data flow**: Opcodes read/write specific registers (e.g., `pIn1 = &aMem[pOp->p1]`)
- **No stack manipulation**: No Pop/Dup/Pull/Push opcodes needed
- **Result delivery**: `OP_ResultRow` points to a contiguous register range

Typical code pattern:
```c
pIn1 = &aMem[pOp->p1];       // read input register
pOut = &aMem[pOp->p3];       // write output register
pOut->u.i = pIn1->u.i + pIn2->u.i;
pOut->flags = MEM_Int;
```

### Architectural Differences

| Aspect | DBSQL (Stack) | SQLite 3.52.0 (Register) |
|--------|---------------|--------------------------|
| Operand count | 3 (P1, P2, P3) | 5 (P1, P2, P3, P4, P5) |
| P3 type | `char*` only | `int` (P4 is the rich union) |
| P4 type | N/A | Union: int, ptr, string, i64, real, FuncDef*, KeyInfo*, etc. |
| P5 type | N/A | `u16` flags byte |
| Data passing | Implicit via stack | Explicit register numbers |
| Integer size | 32-bit `int` | 64-bit `i64` |
| String encoding | Single (native) | UTF-8/UTF-16BE/UTF-16LE |
| BLOB support | No dedicated type | `MEM_Blob` flag |
| Subprogram support | Gosub/Return (fixed-depth stack[100]) | `OP_Program` (VdbeFrame-based, unlimited nesting) |
| Cursor storage | `cursor_t *aCsr` (inline array) | `VdbeCursor **apCsr` (pointer array) |
| Cursor types | 1 (btree + pseudo flag) | 4 (CURTYPE_BTREE, CURTYPE_SORTER, CURTYPE_VTAB, CURTYPE_PSEUDO) |

### Opcode Changes

**Total opcodes**:
- DBSQL: 132 opcodes
- SQLite 3.52.0: 190+ opcodes

**Removed from DBSQL** (52 opcodes):
- Stack manipulation: `OP_Pop`, `OP_Dup`, `OP_Pull`, `OP_Push`
- Legacy result delivery: `OP_ColumnName`, `OP_Callback`, `OP_NullCallback`, `OP_SortCallback`
- String comparison: `OP_StrEq`, `OP_StrNe`, `OP_StrLt`, `OP_StrLe`, `OP_StrGt`, `OP_StrGe`
- Legacy key/record ops: `OP_MakeKey`, `OP_MakeIdxKey`, `OP_IncrKey`, `OP_KeyAsData`, `OP_RowKey`, `OP_Recno`, `OP_FullKey`
- Legacy insert: `OP_PutIntKey`, `OP_PutStrKey`, `OP_NewRecno`, `OP_IdxPut`, `OP_IdxRecno`
- List operations: `OP_ListWrite`, `OP_ListRead`, `OP_ListRewind`, `OP_ListReset`, `OP_ListPush`, `OP_ListPop`
- Sort operations: `OP_SortPut`, `OP_SortMakeRec`, `OP_SortMakeKey`, `OP_SortNext`, `OP_SortReset`
- File I/O: `OP_FileOpen`, `OP_FileRead`, `OP_FileColumn` (COPY statement removed in SQLite 3.0)
- Legacy aggregate: `OP_AggReset`, `OP_AggInit`, `OP_AggFunc`, `OP_AggFocus`, `OP_AggSet`, `OP_AggGet`, `OP_AggNext`
- Set operations: `OP_SetInsert`, `OP_SetFound`, `OP_SetNotFound`, `OP_SetFirst`, `OP_SetNext`

**New in SQLite 3.52.0** (90+ opcodes):
- Coroutines: `OP_InitCoroutine`, `OP_EndCoroutine`, `OP_Yield`
- Register manipulation: `OP_Move`, `OP_Copy`, `OP_SCopy`, `OP_IntCopy`
- Type system: `OP_RealAffinity`, `OP_Cast`, `OP_TypeCheck`, `OP_Affinity`, `OP_CollSeq`, `OP_IsType`
- Enhanced seeks: `OP_SeekLT`, `OP_SeekLE`, `OP_SeekGE`, `OP_SeekGT`, `OP_SeekRowid`, `OP_SeekScan`, `OP_SeekHit`
- Sorter engine: `OP_SorterOpen`, `OP_SorterInsert`, `OP_SorterData`, `OP_SorterCompare`, `OP_SorterSort`, `OP_SorterNext`
- Subprograms: `OP_Program`, `OP_Param` (for triggers)
- Window functions: `OP_AggInverse`, `OP_AggValue`
- Virtual tables: 11 `OP_V*` opcodes
- And many more...

---

## Decision

We will migrate DBSQL from a stack-based VDBE to a register-based VDBE, adopting the SQLite 3.52.0 architecture.

This decision encompasses:

1. **Instruction format change**: Update `vdbe_op_t` to `VdbeOp` format with P4 union and P5 fields
2. **Memory cell update**: Update `mem_t` to `Mem` format with i64, encoding, BLOB support
3. **Cursor update**: Update `cursor_t` to `VdbeCursor` format with cursor types and cache fields
4. **VM state update**: Update `vdbe_t` to `Vdbe` format, removing stack and adding register array
5. **Complete opcode rewrite**: Implement all 190+ opcodes with register-based semantics
6. **Code generator rewrite**: Rewrite all code generators to emit register-targeted instructions

---

## Rationale

### Why Register-Based?

1. **Performance**: Register machines are faster than stack machines for database operations. SQLite's benchmarks showed 20-30% performance improvements after the 2.8.x to 3.0 transition.

2. **Memory efficiency**: Explicit register allocation eliminates redundant stack pushes/pops. The compiler (code generator) can reuse registers, reducing memory traffic.

3. **Optimization opportunities**: Register machines enable better optimization:
   - Dead register elimination
   - Register coalescing
   - Constant folding across registers
   - Better locality of reference

4. **Clearer data flow**: Register-targeted instructions make the data flow explicit, improving:
   - Debuggability (you can see exactly where each value comes from)
   - Testability (you can inspect specific registers)
   - Analyzability (query planners can reason about register lifetimes)

5. **Modern SQL features**: Many SQLite 3.x features (window functions, CTEs, subprograms) were designed for register-based execution and would be difficult to retrofit onto a stack machine.

### Why Not Preserve Stack-Based?

Preserving the stack-based VDBE was considered but rejected:

1. **Code duplication**: Maintaining two separate execution models (stack for legacy, register for new features) would double the maintenance burden.

2. **Feature incompatibility**: New SQLite 3.x features assume register-based execution. Adapting them to a stack machine would require reimplementing core logic.

3. **Dead end**: SQLite abandoned the stack-based model in 2004. No active development or bug fixes exist for that architecture.

4. **Test coverage**: SQLite's test suite (millions of test cases) validates the register-based VDBE. The stack-based VDBE has no modern test coverage.

### Alternative Considered: Gradual Hybrid Approach

We considered a gradual approach where stack-based and register-based opcodes coexist during migration:

**Rejected because**:
- The code generators must emit one or the other consistently. A hybrid would require complex mode tracking.
- The VM dispatch loop would need dual code paths, increasing complexity.
- Testing would be exponentially harder (interactions between stack and register ops).
- The migration would take longer with no clear intermediate deliverable.

---

## Consequences

### Positive

1. **Feature parity**: Unlocks all SQLite 3.52.0 features (window functions, CTEs, UPSERT, JSON, etc.)

2. **Performance improvement**: Expected 20-30% performance gain for query execution

3. **Future-proof**: Register-based architecture is the foundation for all future SQLite development

4. **Better tooling**: SQLite's explain plan and debugging tools work with register-based VDBE

5. **Community alignment**: DBSQL aligns with SQLite's modern architecture, making community contributions easier

### Negative

1. **Complete rewrite**: Every code generator must be rewritten. Estimated 15,000+ lines of code affected.

2. **No incremental testing**: The migration is all-or-nothing. We cannot run mixed stack/register code.

3. **Learning curve**: Developers must learn register-based code generation patterns.

4. **Debugging difficulty**: During migration, bugs may be hard to isolate (is it the code generator or the VDBE interpreter?).

5. **Risk of data incompatibility**: Record format changes may break existing databases (mitigation: migration tool required).

### Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Code generator bugs | HIGH | HIGH | Extensive test suite from SQLite 3.52.0; phase implementation to isolate bugs |
| Performance regression | MEDIUM | HIGH | Benchmark each phase; profile before/after |
| Data corruption | LOW | CRITICAL | Comprehensive migration tool with validation; backups required |
| Schedule overrun | HIGH | MEDIUM | Phased approach with clear milestones; prioritize core features |
| Incomplete migration | MEDIUM | HIGH | Clear definition of "done" for each phase; automated testing |

---

## Implementation Plan

The migration is divided into 6 phases:

**Phase 1: Core Infrastructure** (4-6 weeks)
- Update data structures: `VdbeOp`, `Mem`, `VdbeCursor`, `Vdbe`
- No functional code yet, just structure definitions

**Phase 2: Core Opcodes** (6-8 weeks)
- Basic register operations, arithmetic, logic, control flow
- Result row delivery
- First executable queries possible

**Phase 3: Cursor Operations** (8-10 weeks)
- Table/index access, seeks, navigation
- DML operations (INSERT, UPDATE, DELETE)
- Basic SQL operations work

**Phase 4: Transaction & Schema** (4-6 weeks)
- Transaction control, metadata management
- DDL operations (CREATE, DROP)

**Phase 5: Advanced Features** (8-12 weeks)
- Aggregation, sorting, subprograms
- Functions, RowSets, foreign keys

**Phase 6: Optional Features** (deferred)
- Virtual tables, window functions, bloom filters
- Can be implemented as needed

**Total estimated effort**: 30-42 weeks for phases 1-5

---

## References

- [VDBE Analysis Report](/home/gburd/ws/dbsql/vdbe_analysis_report.md) - Complete opcode mapping and gap analysis
- [Type Comparison Report](/home/gburd/ws/dbsql/type_comparison_report.md) - Data structure changes
- [Baseline Report](/home/gburd/ws/dbsql/baseline_report.md) - Current state assessment
- SQLite 3.0 Release Notes (2004) - Original register-based VDBE introduction
- SQLite 3.52.0 Source Code (`_/sqlite/src/vdbe.c`) - Reference implementation

---

## Decision Log

- **2026-03-05**: Decision to modernize DBSQL to SQLite 3.52.0
- **2026-03-07**: Analysis completed; register-based VDBE confirmed as only viable path
- **2026-03-16**: ADR-001 written and accepted
