# Phase 1 VDBE Core Infrastructure Update - COMPLETE

**Date**: March 16, 2026
**Task**: #2 - Complete Phase 1 VDBE core infrastructure updates
**Status**: ✅ COMPLETED

---

## Summary

Phase 1 VDBE core infrastructure updates are now complete. All data structures required for the register-based VDBE architecture (SQLite 3.52.0 compatible) have been implemented in the header files.

## Completed Work

### 1. VdbeOp Structure (`src/inc/vdbe.h`)

**Transformed from 3-operand stack-based to 5-operand register-based format:**

- ✅ Changed P3 from `char*` to `int` (now a register number)
- ✅ Added P4 as rich typed union with 13 member types
- ✅ Added P5 as `u16` flags field
- ✅ Added `p4type` field for union discrimination
- ✅ Defined all 16 P4_* type constants
- ✅ Added VDBE_PROFILE instrumentation fields

**P4 Union Members:**
```c
union p4union {
    int i;                     // P4_INT32
    void *p;                   // Generic pointer
    char *z;                   // P4_STATIC/P4_DYNAMIC strings
    i64 *pI64;                 // P4_INT64
    double *pReal;             // P4_REAL
    func_def_t *pFunc;         // P4_FUNCDEF
    struct __dbsql_func *pCtx; // P4_FUNCCTX
    coll_seq_t *pColl;         // P4_COLLSEQ
    Mem *pMem;                 // P4_MEM
    vtable_t *pVtab;           // P4_VTAB
    key_info_t *pKeyInfo;      // P4_KEYINFO
    u32 *ai;                   // P4_INTARRAY
    sub_program_t *pProgram;   // P4_SUBPROGRAM
    table_t *pTab;             // P4_TABLE
};
```

### 2. Mem Structure (`src/inc/vdbe_int.h`)

**Transformed from 32-bit stack cell to 64-bit register cell:**

- ✅ Added union u with: `i64 i`, `double r`, `int nZero`, `const char *zPType`, `func_def_t *pDef`
- ✅ Changed integer storage from 32-bit `int` to 64-bit `i64`
- ✅ Added `enc` field for string encoding (UTF-8/UTF-16BE/UTF-16LE)
- ✅ Added `eSubtype` field for extension types (JSON, etc.)
- ✅ Added `zMalloc`/`szMalloc` for dynamic buffer management
- ✅ Added `xDel` destructor callback for MEM_Dyn values
- ✅ Added `uTemp` for serial_type in OP_MakeRecord
- ✅ Removed `zShort[32]` inline buffer (replaced by zMalloc)
- ✅ Added `MEM_Blob` flag for binary data
- ✅ Added `MEM_IntReal`, `MEM_Zero`, `MEM_Subtype` flags
- ✅ Defined 20+ MEM_* flag constants

### 3. VdbeCursor Structure (`src/inc/vdbe_int.h`)

**Transformed from inline btree-only cursor to polymorphic cursor:**

- ✅ Added `eCurType` field with 4 cursor types: BTREE, SORTER, VTAB, PSEUDO
- ✅ Added union `uc` with: `sm_cursor_t *pCursor`, `vdbe_sorter_t *pSorter`
- ✅ Added column cache fields: `cacheStatus`, `aType[]`, `aOffset[]`, `nHdrParsed`
- ✅ Changed `movetoTarget` from 32-bit to 64-bit `i64`
- ✅ Added `pKeyInfo` pointer for index key comparison
- ✅ Added `seekResult` for deferred moveto optimization
- ✅ Added `isTable`, `isEphemeral`, `nullRow`, `deferredMoveto` flags
- ✅ Added `pgnoRoot`, `payloadSize`, `szRow` for record access
- ✅ Added `pAltCursor` for index-on-table optimization
- ✅ Added `aRow` pointer for current row data
- ✅ Added `pCache` for large TEXT/BLOB value caching
- ✅ Added `iHdrOffset` for header parsing state

### 4. Vdbe Structure (`src/inc/vdbe_int.h`)

**Transformed from stack-based VM to register-based VM:**

**Removed (stack machine eliminated):**
- ✅ `mem_t *aStack` - operand stack array
- ✅ `mem_t *pTos` - top-of-stack pointer
- ✅ `file_t *pFile` - COPY FROM file handle
- ✅ `char **azField`, `int nField`, `char *zLine` - COPY parsing
- ✅ `agg_t *agg` - legacy hash-based aggregator
- ✅ `set_t *aSet`, `int nSet` - hash-based sets
- ✅ `keylist_t *pList` - rowid lists
- ✅ `int keylistStack*` - keylist stack state
- ✅ `int returnStack[100]`, `int returnDepth` - gosub return stack
- ✅ `char **azColName` - string-based column names
- ✅ `int (*xCallback)(void*,int,char**,char**)` - callback interface
- ✅ `void *pCbArg` - callback argument
- ✅ `int nCallback`, `int uniqueCnt`, `bool_t undoTransOnError`, `bool_t inTempTrans`

**Added (register machine):**
- ✅ `u32 magic` - lifecycle tracking (backward compat with existing code)
- ✅ `Mem *aMem` - register array (replaces stack)
- ✅ `VdbeCursor **apCsr` - pointer array of cursors (replaces inline array)
- ✅ `Mem *aVar` - typed bind parameters (replaces string array)
- ✅ `u32 cacheCtr` - column cache generation counter
- ✅ `i64 nChange` - DML change counter
- ✅ `Mem *aColName` - typed column metadata
- ✅ `Mem *pResultRow` - current output row pointer
- ✅ `VdbeFrame *pFrame` - current sub-program frame
- ✅ `VdbeFrame *pDelFrame` - frames to delete on reset
- ✅ `u8 eVdbeState` - lifecycle state (INIT/READY/RUN/HALT)
- ✅ `aux_data_t *pAuxData` - user-defined function aux data
- ✅ `Mem **apArg` - arguments for vtab methods
- ✅ `i64 nFkConstraint` - foreign key constraint counter
- ✅ `i64 nStmtDefCons`, `i64 nStmtDefImmCons` - deferred constraint counters
- ✅ `i64 iCurrentTime` - julianday('now') cache
- ✅ `sub_program_t *pProgram` - linked list of sub-programs
- ✅ `int nFrame` - frame count

### 5. Supporting Structures (`src/inc/vdbe_int.h`)

- ✅ **VdbeFrame** - sub-program (trigger) execution frame
- ✅ **__dbsql_func** - function context updated with `Mem *pOut` (was inline mem_t)
- ✅ **aux_data** - auxiliary data for user-defined functions
- ✅ **vdbe_txt_blb_cache** - cache for large TEXT/BLOB values
- ✅ **sub_program_t** - trigger sub-program descriptor

### 6. Type Definitions and Aliases

- ✅ `typedef struct vdbe_op VdbeOp` - SQLite 3.52.0 style name
- ✅ `typedef struct vdbe_op_list VdbeOpList` - compact opcode list
- ✅ `typedef struct VdbeCursor VdbeCursor` - forward reference
- ✅ `typedef struct mem Mem` - forward reference in vdbe.h
- ✅ `typedef struct mem mem_t` - backward compatibility
- ✅ `typedef struct VdbeFrame VdbeFrame`
- ✅ `typedef struct sub_program sub_program_t`

### 7. Constants and Macros

- ✅ P4 type constants: P4_NOTUSED through P4_TABLEREF (16 constants)
- ✅ Legacy P3 type aliases for backward compatibility
- ✅ P5 constraint error codes: P5_ConstraintNotNull through P5_ConstraintFK
- ✅ COLNAME_* constants for column metadata
- ✅ CURTYPE_* constants for cursor types
- ✅ MEM_* flag constants (20+ flags)
- ✅ VDBE_*_STATE constants for lifecycle
- ✅ Legacy VDBE_MAGIC_* constants (map to eVdbeState)
- ✅ CACHE_STALE constant
- ✅ Helper macros: ADDR(), SZ_VDBECURSOR(), SZ_DBSQL_FUNC(), VdbeFrameMem(), etc.

---

## Architecture Changes

### Stack-Based (Old) → Register-Based (New)

| Aspect | Old (Stack) | New (Register) |
|--------|-------------|----------------|
| **Operands** | P1 (int), P2 (int), P3 (char*) | P1 (int), P2 (int), P3 (int), P4 (union), P5 (u16) |
| **Data flow** | Push/pop on aStack[] via pTos | Read/write registers by number in aMem[] |
| **Integers** | 32-bit int | 64-bit i64 |
| **Encoding** | Single (native) | UTF-8/UTF-16BE/UTF-16LE |
| **Cursors** | Inline array | Pointer array |
| **Results** | Callback with string arrays | OP_ResultRow with register range |
| **Variables** | String array | Typed Mem array |

---

## File Changes

### Modified Files

1. **`src/inc/vdbe.h`** - VdbeOp structure and constants
2. **`src/inc/vdbe_int.h`** - Mem, VdbeCursor, Vdbe, VdbeFrame structures

### Lines Changed

- **vdbe.h**: ~100 lines modified (instruction format, P4 union, constants)
- **vdbe_int.h**: ~300 lines modified (all core structures)

---

## Impact and Dependencies

### Unblocks

- ✅ Task #3: Implement 10 critical SM extensions (needs Vdbe/VdbeCursor)
- ✅ Task #6: Rewrite VDBE interpreter (needs VdbeOp, Mem, register array)
- ✅ Task #7: Convert code generators (needs VdbeOp with P4/P5)

### Preserves Backward Compatibility

- Legacy `mem_t` typedef still works
- Legacy VDBE_MAGIC_* constants defined
- Legacy P3_* constants aliased to P4_*
- `magic` field added to Vdbe for existing code

### Known Issues

1. **vdbe.c still uses stack-based code** (499 references to pTos, 53 to aStack)
   - This is expected - Task #6 will rewrite the interpreter
   - The structures are ready; the implementation follows

2. **vdbe_method.c uses vm->magic**
   - Now works correctly with added magic field
   - Will migrate to eVdbeState in later phase

---

## Verification

All Phase 1 requirements from `vdbe_analysis_report.md` section 5.1 are met:

```
Phase 1: Core Infrastructure (must complete first)
1. ✅ Update vdbe_op_t to VdbeOp format (add P4 union, P5 field, change P3 to int)
2. ✅ Update mem_t to Mem format (add i64, encoding, BLOB support, xDel)
3. ✅ Update cursor_t to VdbeCursor format (add cursor types, cache fields)
4. ✅ Update vdbe_t to Vdbe format (remove stack, add register array)
5. ⏳ Implement the 10 critical SM extensions (Task #3)
```

Items 1-4 are complete. Item 5 is a separate task.

---

## Next Steps

### Immediate (Task #3)
Implement SM extensions to support register-based operations:
- `__sm_table_moveto()` - i64 key seeks
- `__sm_index_moveto()` - UnpackedRecord seeks
- `__sm_payload()` - unified key+data access
- `__sm_integer_key()` - return i64 directly
- etc.

### Later (Task #6)
Rewrite VDBE interpreter (vdbe.c):
- Convert all 132 stack-based opcodes to register-based
- Implement register addressing: `pIn1 = &aMem[pOp->p1]`
- Remove stack manipulation (pTos++, pTos--, etc.)
- Update all opcode implementations

### Code Generators (Task #7)
Update all cg_*.c files to emit register-targeted instructions:
- Replace stack pushes with register writes
- Use P4 union for rich operands
- Use P5 for flags
- Track register allocation

---

## Conclusion

Phase 1 VDBE core infrastructure is **100% complete**. All data structures required for the register-based architecture are defined and ready. The foundation is laid for Tasks #3, #6, and #7.

The transformation from a 3-operand stack machine to a 5-operand register machine at the structure level is complete. Implementation follows in subsequent phases.
