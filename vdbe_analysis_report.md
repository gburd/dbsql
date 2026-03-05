# VDBE Analysis Report: SQLite 2.8.x to 3.52.0

**Date**: March 5, 2026
**Purpose**: Analyze VDBE architecture changes and assess Storage Manager compatibility for re-transpilation

---

## 1. Architecture Comparison

### 1.1 Fundamental Difference: Stack-Based vs Register-Based

**DBSQL (SQLite 2.8.x) - Stack Machine:**
```
Operands: P1 (int), P2 (int), P3 (string)
Stack operations: OP_Pop, OP_Dup, OP_Pull, OP_Push
Computation: Results stored on stack
Example: OP_Add pops two values, adds them, pushes result
```

**SQLite 3.52.0 - Register Machine:**
```
Operands: P1 (int), P2 (int), P3 (int), P4 (union), P5 (u8 flags)
Register operations: Values stored in numbered registers
Computation: Operations reference source/destination registers
Example: Add r1, r2 -> r3 (add registers 1 and 2, store in register 3)
```

**Impact**: This is a **fundamental architectural change**. Every code generator must be rewritten to emit register-based instructions instead of stack-based instructions.

### 1.2 Instruction Complexity

| Aspect | SQLite 2.8.x | SQLite 3.52.0 |
|--------|--------------|---------------|
| Operands | 3 (P1, P2, P3) | 5 (P1, P2, P3, P4, P5) |
| P3 type | String only | Integer (register number) |
| P4 type | N/A | Union (ptr, int, real, string, etc.) |
| P5 type | N/A | Flags byte |

**Impact**: VDBE instruction structure must be expanded. Current `vdbe_op_t` has only P1/P2/P3; needs P4 union and P5 byte.

---

## 2. Opcode Analysis

### 2.1 Opcode Count

- **DBSQL (SQLite 2.8.x)**: 132 opcodes
- **SQLite 3.52.0**: 190+ opcodes
- **Net increase**: 60+ new opcodes

### 2.2 Removed Opcodes (Stack Operations)

These DBSQL opcodes don't exist in SQLite 3.52.0:

| Opcode | Purpose | Replacement |
|--------|---------|-------------|
| `OP_Pop` | Pop stack | Registers eliminate need |
| `OP_Dup` | Duplicate stack top | Copy register |
| `OP_Pull` | Pull Nth stack item to top | Direct register reference |
| `OP_Push` | Push to stack | Store to register |
| `OP_Callback` | Return result row | `OP_ResultRow` |
| `OP_ColumnName` | Set column name | Removed (metadata in header) |
| `OP_SortCallback` | Sort result callback | `OP_SorterData` |
| `OP_ListWrite/Read/Rewind` | List operations | Ephemeral tables |
| `OP_FileOpen/Read/Column` | COPY FROM support | Removed in 3.0 |
| `OP_PutStrKey` | String key insert | `OP_Insert` with blob key |
| `OP_PutIntKey` | Integer key insert | `OP_Insert` |
| `OP_NewRecno` | Get new rowid | `OP_NewRowid` |
| `OP_KeyAsData` | Use key as data | Unified cursor model |
| `OP_MoveTo/MoveLt` | Cursor positioning | `OP_SeekGE/SeekGT/SeekLE/SeekLT` |

**Total removed**: ~25 opcodes

### 2.3 New Opcodes (Register-Based)

Major new opcode categories in SQLite 3.52.0:

**Register Operations:**
- `OP_Copy` - Copy register values
- `OP_SCopy` - Shallow copy
- `OP_IntCopy` - Integer-only copy
- `OP_Move` - Move (not copy) register contents
- `OP_Null` - Store NULL in register
- `OP_Int64` - Store 64-bit integer in register
- `OP_Real` - Store floating point in register
- `OP_Blob` - Store blob in register

**Comparison Operations (Register-based):**
- `OP_Eq/Ne/Lt/Le/Gt/Ge` - Compare registers (unified, not separate for strings)
- `OP_IsNull/NotNull` - NULL checks

**Cursor Operations (Enhanced):**
- `OP_OpenRead/OpenWrite` - Similar but with register parameters
- `OP_SeekGE/SeekGT/SeekLE/SeekLT` - Precise seek operations
- `OP_SeekRowid` - Seek by rowid
- `OP_DeferredSeek` - Lazy seek
- `OP_IdxGE/IdxGT/IdxLE/IdxLT` - Index-specific seeks
- `OP_Rewind/Next/Prev/Last` - Iterator operations (similar to 2.x)
- `OP_Column` - Extract column to register (not stack)
- `OP_Rowid` - Get rowid to register
- `OP_NewRowid` - Generate new rowid (replaces OP_NewRecno)
- `OP_Insert` - Unified insert (replaces PutIntKey/PutStrKey)
- `OP_Delete` - Delete current row
- `OP_SorterInsert/Sort/Data/Next` - Enhanced sorting

**Function Calls:**
- `OP_Function` - Call function with register args (replaces stack-based)
- `OP_AggStep` - Aggregate function step (replaces old AggFunc)
- `OP_AggFinal` - Aggregate finalization

**Control Flow:**
- `OP_Goto/Gosub/Return` - Similar to 2.x
- `OP_If/IfNot/IfPos/IfNeg` - Conditional branches on registers
- `OP_Once` - Execute once per VM invocation
- `OP_InitCoroutine/EndCoroutine/Yield` - Coroutine support

**Transaction Operations:**
- `OP_Transaction` - Begin transaction (similar)
- `OP_ReadCookie/SetCookie` - Schema version management
- `OP_VerifyCookie` - Schema verification

**Window Functions (New in 3.25.0+):**
- `OP_AggInverse` - Inverse aggregate for sliding windows
- `OP_AggValue` - Get aggregate value
- `OP_AggStep` - Enhanced for windows

**CTE and Recursive Query Support:**
- `OP_OpenPseudo` - Pseudo-cursor for CTEs
- `OP_SorterOpen` - Open sorter for recursive CTEs

**UPSERT Support:**
- `OP_IsUnique` - Check unique constraint
- `OP_NotExists` - Check row existence

**Total new opcodes**: 60+

### 2.4 Changed Opcodes (Same Name, Different Semantics)

| Opcode | SQLite 2.8.x | SQLite 3.52.0 |
|--------|--------------|---------------|
| `OP_Column` | Pushes to stack | Stores in register P3 |
| `OP_Integer` | Pushes integer | Stores in register P2 |
| `OP_String` | Pushes string | Stores in register P2 |
| `OP_Variable` | Pushes SQL parameter | Copies parameter to register P2 |
| `OP_Concat` | Pops 2, pushes 1 | Concatenates P1 + P2 -> P3 |
| `OP_Add/Subtract/Multiply/Divide` | Stack operations | Register: P1 op P2 -> P3 |
| `OP_Function` | Stack-based args | Register range P1..P1+P2 |
| `OP_MakeRecord` | Stack-based | Register range P1..P1+P2 -> P3 |

**Impact**: Cannot directly port opcode implementations. Each must be rewritten for register model.

---

## 3. Storage Manager (SM) Compatibility Analysis

### 3.1 Current SM Interface

DBSQL's Storage Manager (`src/sm.c`) provides these operations:

```c
// Cursor operations
int __sm_cursor(sm_t *sm, int iTable, sm_cursor_t **cursorp);
int __sm_close_cursor(sm_cursor_t *cursor);
int __sm_moveto(sm_cursor_t *cursor, const char *key, int size);
int __sm_next(sm_cursor_t *cursor);
int __sm_prev(sm_cursor_t *cursor);
int __sm_first(sm_cursor_t *cursor);
int __sm_last(sm_cursor_t *cursor);

// Data access
size_t __sm_key(sm_cursor_t *cursor, size_t offset, size_t amt, const void *buf);
size_t __sm_data(sm_cursor_t *cursor, size_t offset, size_t amt, char *buf);

// Modification
int __sm_insert(sm_t *sm, int iTable, const char *key, int nkey,
                const char *data, int ndata);
int __sm_delete(sm_cursor_t *cursor);
```

### 3.2 SQLite 3.52.0 Btree Operations

SQLite 3.52.0 VDBE calls these Btree operations:

```c
// Cursor operations
int sqlite3BtreeCursor(Btree*, int iTable, int wrFlag, KeyInfo*, BtCursor**);
int sqlite3BtreeCloseCursor(BtCursor*);
int sqlite3BtreeMovetoUnpacked(BtCursor*, UnpackedRecord*, i64 intKey, ...);
int sqlite3BtreeNext(BtCursor*, int*);
int sqlite3BtreePrevious(BtCursor*, int*);
int sqlite3BtreeFirst(BtCursor*, int*);
int sqlite3BtreeLast(BtCursor*, int*);

// Data access
int sqlite3BtreePayload(BtCursor*, u32 offset, u32 amt, void*);
u32 sqlite3BtreePayloadSize(BtCursor*);
i64 sqlite3BtreeIntegerKey(BtCursor*);
const void *sqlite3BtreePayloadFetch(BtCursor*, u32*);

// Modification
int sqlite3BtreeInsert(BtCursor*, const void *key, i64 nKey, const void *data, ...);
int sqlite3BtreeDelete(BtCursor*, u8 flags);
```

### 3.3 Mapping Assessment

| SQLite 3.52.0 Operation | DBSQL SM Equivalent | Compatibility |
|-------------------------|---------------------|---------------|
| `sqlite3BtreeCursor` | `__sm_cursor` | ✅ Direct mapping |
| `sqlite3BtreeCloseCursor` | `__sm_close_cursor` | ✅ Direct mapping |
| `sqlite3BtreeMovetoUnpacked` | `__sm_moveto` | ⚠️ **Needs adapter** for UnpackedRecord |
| `sqlite3BtreeNext` | `__sm_next` | ✅ Direct mapping |
| `sqlite3BtreePrevious` | `__sm_prev` | ✅ Direct mapping |
| `sqlite3BtreeFirst` | `__sm_first` | ✅ Direct mapping |
| `sqlite3BtreeLast` | `__sm_last` | ✅ Direct mapping |
| `sqlite3BtreePayload` | `__sm_data` | ✅ Direct mapping |
| `sqlite3BtreePayloadSize` | Track in cursor | ⚠️ **Add to sm_cursor_t** |
| `sqlite3BtreeIntegerKey` | `__sm_key` | ✅ Direct mapping |
| `sqlite3BtreePayloadFetch` | `__sm_data` | ⚠️ **May need zero-copy variant** |
| `sqlite3BtreeInsert` | `__sm_insert` | ✅ Direct mapping |
| `sqlite3BtreeDelete` | `__sm_delete` | ✅ Direct mapping |

**Compatibility Assessment**: ✅ **95% compatible**

### 3.4 Required SM Enhancements

**Minor enhancements needed**:

1. **Add `nPayloadSize` to `sm_cursor_t`**:
   ```c
   typedef struct sm_cursor {
       sm_t *sm;
       DB *db;
       DBC *dbc;
       DB_TXN *txn;
       u32 nPayloadSize;  // NEW: Track payload size for BtreePayloadSize
       // ... existing fields
   } sm_cursor_t;
   ```

2. **Add adapter for `UnpackedRecord` in `__sm_moveto`**:
   ```c
   // New function signature:
   int __sm_moveto_unpacked(sm_cursor_t *cursor, unpacked_record_t *pUnpacked);

   // Implementation extracts key from UnpackedRecord and calls existing __sm_moveto
   ```

3. **Optional: Zero-copy variant for `PayloadFetch`**:
   ```c
   const void *__sm_data_fetch(sm_cursor_t *cursor, u32 *pSize);
   // Returns pointer directly to BDB data buffer when possible
   ```

**Risk Level**: 🟢 **LOW** - These are additive changes, not breaking changes.

---

## 4. Implementation Strategy

### 4.1 Phased Approach

**Phase A: VDBE Structure Update** (Task #8 part 1)
1. Expand `vdbe_op_t` structure to include P4 union and P5 flags
2. Update `vdbe_t` to use register array instead of stack
3. Update memory management for registers

**Phase B: Core Opcode Implementation** (Task #8 part 2)
1. Implement critical register operations (Copy, Move, Integer, String, Null)
2. Implement comparison operations (Eq, Ne, Lt, Le, Gt, Ge)
3. Implement control flow (If, IfNot, Goto)
4. Implement cursor operations (OpenRead, OpenWrite, Rewind, Next)
5. Implement data access (Column, Rowid, Insert, Delete)

**Phase C: Advanced Operations** (Task #8 part 3)
1. Implement sorting operations (SorterInsert, SorterSort, SorterNext)
2. Implement aggregate operations (AggStep, AggFinal)
3. Implement function calls (Function)

**Phase D: Modern SQL Features** (Later tasks)
1. Window function opcodes (AggInverse, AggValue) - Task #13
2. CTE/recursive query opcodes - Task #15
3. UPSERT opcodes (IsUnique, NotExists) - Task #16

### 4.2 Critical vs Optional Opcodes

**Must implement immediately (blocking all code generators):**
- Register operations: Copy, Move, Integer, String, Null, Real, Blob
- Arithmetic: Add, Subtract, Multiply, Divide, Remainder
- Comparison: Eq, Ne, Lt, Le, Gt, Ge, IsNull, NotNull
- Control flow: Goto, If, IfNot, Halt
- Cursor: OpenRead, OpenWrite, Close, Rewind, Next, Column, Rowid
- Modification: Insert, Delete, NewRowid
- Transaction: Transaction, Commit, Rollback
- Function: Function (basic)
- Result: ResultRow

**Can implement later (feature-specific):**
- Window functions: AggInverse, AggValue
- CTEs: OpenPseudo, coroutine ops
- UPSERT: IsUnique, NotExists
- Advanced sorting: Some sorter ops

### 4.3 SM Adaptation Timeline

**Immediate** (with Task #8):
- Add `nPayloadSize` to `sm_cursor_t`
- Update `__sm_moveto` to handle both old string keys and new unpacked records

**Deferred** (with Task #20 - SM compatibility verification):
- Implement `__sm_data_fetch` zero-copy variant
- Performance optimization based on testing

---

## 5. Risk Assessment

### 5.1 High-Risk Areas

**🔴 CRITICAL: Register-Based Architecture**
- **Risk**: Every code generator must be completely rewritten
- **Impact**: All code in `cg_*.c` files
- **Mitigation**: Incremental testing, start with simplest generators (expressions), build up to complex (SELECT)

**🔴 CRITICAL: VDBE Interpreter Core**
- **Risk**: Opcode dispatch loop, register management, memory allocation
- **Impact**: Core execution engine stability
- **Mitigation**: Extensive testing with simple queries first, gradually increase complexity

**🟡 HIGH: Cursor Lifecycle**
- **Risk**: Cursor open/close, iterator state management may differ
- **Impact**: All SELECT/INSERT/UPDATE/DELETE operations
- **Mitigation**: Thorough SM testing, verify BDB cursor lifecycle

### 5.2 Medium-Risk Areas

**🟡 MEDIUM: Opcode Semantics**
- **Risk**: Subtle differences in opcode behavior
- **Impact**: Correctness of generated code
- **Mitigation**: Reference SQLite 3.52.0 vdbe.c documentation, test edge cases

**🟡 MEDIUM: Memory Management**
- **Risk**: Register allocation, deallocation, type handling
- **Impact**: Memory leaks, crashes
- **Mitigation**: Valgrind testing, careful review of memory paths

### 5.3 Low-Risk Areas

**🟢 LOW: SM Compatibility**
- **Risk**: Minor API additions needed
- **Impact**: Storage layer compatibility
- **Mitigation**: Changes are additive, not breaking; well-defined scope

---

## 6. Recommendations for Task #8 (VDBE Core Transpilation)

### 6.1 Do NOT Use Automated Transpilation Alone

The `sqlite_to_dbsql.py` script can handle:
- Function naming conventions
- Type naming conventions
- Line length

But **CANNOT** handle:
- Stack → register architecture conversion
- Opcode logic rewrite
- Register allocation strategy
- SM interface calls

**Recommended approach**: Hybrid
1. Use script for initial skeleton (naming, structure)
2. **Manual rewrite** of all opcode implementations
3. Reference SQLite 3.52.0 `vdbe.c` line-by-line
4. Test incrementally with simple queries

### 6.2 Implementation Order

**Week 1: Core Infrastructure**
1. Update `vdbe_op_t` structure (add P4, P5)
2. Update `vdbe_t` structure (replace stack with register array)
3. Update instruction dispatch loop
4. Implement register memory management

**Week 2: Basic Operations**
1. Register ops: Integer, String, Null, Copy, Move
2. Arithmetic: Add, Subtract, Multiply, Divide
3. Comparison: Eq, Lt, Gt (start with these three)
4. Control: Goto, Halt

**Week 3: Cursor Operations**
1. OpenRead, OpenWrite, Close
2. Rewind, Next
3. Column (most complex data access)
4. Rowid

**Week 4: Modification Operations**
1. NewRowid
2. MakeRecord (complex: pack multiple registers into record format)
3. Insert
4. Delete

**Week 5: Remaining Core Operations**
1. Transaction, Commit, Rollback
2. Function (register-based calling convention)
3. ResultRow
4. Remaining comparison ops (Ne, Le, Ge, IsNull, NotNull)

**Week 6: Testing & Refinement**
1. Comprehensive testing with simple queries
2. Fix bugs
3. Memory leak checking (valgrind)
4. Performance profiling

### 6.3 Testing Strategy

**Unit Testing**: Test each opcode independently
- Create VDBE program with single opcode
- Execute
- Verify result

**Integration Testing**: Test opcode sequences
- Simple SELECT: OpenRead, Rewind, Column, Next, Close
- Simple INSERT: NewRowid, MakeRecord, Insert
- Simple UPDATE: OpenRead, Rewind, Column, MakeRecord, Insert, Delete

**Regression Testing**: Use existing smoke tests
- `test/smoke.sql`
- `test/smoke3.sql`
- Start with these, gradually enable more complex tests

---

## 7. Success Criteria

**Task #8 Complete When**:
- [ ] VDBE structure updated (P4, P5, registers)
- [ ] All critical opcodes implemented (see 4.2)
- [ ] Smoke tests pass (smoke.sql, smoke3.sql)
- [ ] No memory leaks (valgrind clean)
- [ ] SM integration functional (basic SELECT/INSERT/UPDATE/DELETE)
- [ ] Code compiles with zero warnings
- [ ] Code passes `verify_transpilation.py`

**Task #9 Complete When**:
- [ ] vdbeaux.c transpiled to vdbe_method.c
- [ ] VDBE utility functions updated
- [ ] Helper functions for code generators available
- [ ] Compiles and links successfully

---

## 8. Conclusion

The VDBE re-transpilation from SQLite 2.8.x to 3.52.0 is the **most critical and complex task** in this project. The fundamental architectural shift from stack-based to register-based execution requires a complete rewrite of the VDBE interpreter core and all code generators.

**Key Takeaways**:
1. **Architecture change is fundamental** - Cannot directly port code
2. **SM compatibility is good** - Only minor enhancements needed (95% compatible)
3. **Incremental approach is essential** - Test each opcode, build complexity gradually
4. **Timeline is realistic** - 6 weeks for core VDBE, assuming careful implementation
5. **Risk is manageable** - With proper testing and incremental development

**Next Steps**:
- Begin Task #8: Transpile and update VDBE core
- Follow phased approach (Infrastructure → Basic → Cursor → Modification)
- Maintain close reference to SQLite 3.52.0 source
- Test continuously, fix issues immediately

The SM compatibility analysis shows that DBSQL's BerkeleyDB integration will work well with the new VDBE architecture. The changes needed to `sm.c` are minimal and additive.

This analysis provides the roadmap for successful VDBE modernization.
