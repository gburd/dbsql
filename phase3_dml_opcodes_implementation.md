# Phase 3 DML Opcodes Implementation

## Overview

This document describes the implementation of 5 register-based DML opcodes for the DBSQL VDBE, matching SQLite 3.52.0 architecture.

## Implemented Opcodes

### 1. OP_NewRowid (P1, P2, P3)
**Purpose:** Generate a unique rowid for table insertion
- **P1:** Cursor number
- **P2:** Output register for generated rowid
- **P3:** Optional register with max rowid (for AUTOINCREMENT)

**Algorithm:**
1. Try to use max(existing_rowid) + 1
2. If max rowid reached (2^63-1), fall back to random generation
3. Random generation tries up to 100 times to find unused rowid
4. For AUTOINCREMENT tables (P3 set), enforce sequential allocation

**Key SM Functions Used:**
- `__sm_last_ex()` - Move to last record
- `__sm_integer_key()` - Read current rowid
- `__sm_table_moveto()` - Check if random rowid exists
- `__dbsql_random_bytes()` - Generate random values

### 2. OP_Insert (P1, P2, P3, P4, P5)
**Purpose:** Insert a record into a table btree
- **P1:** Cursor number
- **P2:** Register with data (MEM_Blob)
- **P3:** Register with rowid (MEM_Int)
- **P4:** Optional table pointer (for hooks)
- **P5:** Flags (NCHANGE, LASTROWID, USESEEKRESULT, APPEND, SAVEPOSITION)

**Implementation:**
```c
sm_payload_t x;
x.nKey = rowid;           // From register P3
x.pKey = NULL;            // Tables use nKey as rowid
x.pData = data->z;        // From register P2
x.nData = data->n;
x.nZero = data->u.nZero;  // For BLOB with trailing zeros
```

**Key SM Functions Used:**
- `__sm_insert_ex(cursor, &payload, flags, seekResult)`

**Flags Mapping:**
- P5 & 0x01 → OPFLAG_NCHANGE (increment change counter)
- P5 & 0x02 → OPFLAG_LASTROWID/SAVEPOSITION
- P5 & 0x08 → OPFLAG_APPEND (append to end optimization)
- P5 & 0x10 → OPFLAG_USESEEKRESULT (reuse seek result)

### 3. OP_Delete (P1, P2, *, *, P5)
**Purpose:** Delete record at current cursor position
- **P1:** Cursor number
- **P2:** Flags (NCHANGE, SAVEPOSITION, AUXDELETE)
- **P5:** Additional flags for __sm_delete_ex

**Implementation:**
- Calls `__sm_delete_ex(cursor, P5)`
- Invalidates cursor cache
- Increments change counter if NCHANGE flag set

**Key SM Functions Used:**
- `__sm_delete_ex(cursor, flags)`

**Flags:**
- P2 & 0x01 → OPFLAG_NCHANGE
- P2 & 0x02 → OPFLAG_SAVEPOSITION (cursor to next/prev after delete)
- P2 & 0x04 → OPFLAG_AUXDELETE (auxiliary delete, not primary)

### 4. OP_MakeRecord (P1, P2, P3, P4, P5)
**Purpose:** Pack P2 registers starting at P1 into SQLite record format, store in P3
- **P1:** First input register
- **P2:** Number of fields
- **P3:** Output register
- **P4:** Optional affinity string (length P2)
- **P5:** File format version (4 for SQLite 3.3.0+)

**SQLite Record Format:**
```
[varint: header_size] [varint: type0] ... [varint: typeN-1] [data0] ... [dataN-1]
```

**Serial Type Encoding:**
- 0: NULL
- 1: 1-byte signed integer
- 2: 2-byte signed integer
- 3: 3-byte signed integer
- 4: 4-byte signed integer
- 5: 6-byte signed integer
- 6: 8-byte signed integer
- 7: IEEE 754 float (8 bytes)
- 8,9: Special constants (0 and 1) when file_format >= 4
- 10,11: Reserved
- N>=12 and even: BLOB with length (N-12)/2
- N>=13 and odd: TEXT with length (N-13)/2

**Implementation Steps:**
1. Calculate header size and data size
2. Allocate buffer (nVarint + nHdr + nData bytes)
3. Write header size varint
4. Write serial type varints for each field
5. Write data values

**Helper Functions:**
- `__vdbe_serial_type(pMem, file_format, &nZero)` - Determine serial type
- `__vdbe_varint_len(value)` - Calculate varint length
- `__vdbe_put_varint(buf, value)` - Write varint
- `__vdbe_serial_type_len(serial_type)` - Get data length
- `__vdbe_serial_put(buf, pMem, serial_type)` - Write data value

### 5. OP_IdxInsert (P1, P2, P3, P4, P5)
**Purpose:** Insert into index btree
- **P1:** Cursor number (must be index, not table)
- **P2:** Register with index key (MEM_Blob)
- **P3:** Unused in modern SQLite
- **P4:** Optional count of registers
- **P5:** Flags (NCHANGE, APPEND, SAVEPOSITION, USESEEKRESULT)

**Implementation:**
```c
sm_payload_t x;
x.pKey = key->z;     // Index keys are blobs
x.nKey = key->n;
x.pData = NULL;      // Indexes don't store separate data
x.nData = 0;
x.nZero = 0;
```

**Key Difference from OP_Insert:**
- Index cursors: Key is a blob (packed with MakeRecord)
- Table cursors: Key is an integer rowid

## Helper Functions Added

### Varint Encoding
```c
static u8 __vdbe_put_varint(unsigned char *p, u64 v)
```
- Encodes unsigned 64-bit integer as variable-length integer
- Returns number of bytes written (1-9)
- Format: 7 bits per byte, MSB indicates continuation

```c
static int __vdbe_varint_len(u64 v)
```
- Returns number of bytes needed to encode v
- Used for pre-calculating buffer sizes

### Serial Type System
```c
static u32 __vdbe_serial_type(Mem *pMem, int file_format, u8 *pnZero)
```
- Determines SQLite serial type for a Mem value
- Handles NULL, integers, floats, TEXT, BLOB
- Returns type code (0-7 for fixed types, 12+ for variable)

```c
static u32 __vdbe_serial_type_len(u32 serial_type)
```
- Returns data length for a serial type
- Used to calculate total record size

```c
static u32 __vdbe_serial_put(u8 *buf, Mem *pMem, u32 serial_type)
```
- Writes serialized value to buffer
- Handles endianness for integers
- Returns bytes written

## Integration Points

### Required Definitions
All required types and structures already exist:
- `Mem` / `mem_t` - Register value cell (vdbe_int.h:224)
- `VdbeCursor` - Register-based cursor (vdbe_int.h:103)
- `sm_payload_t` - SM layer payload structure (dbsql_int.in:481)
- `vdbe_t` / `vdbe` - VDBE instance (vdbe_int.h:452)

### Required SM Functions
All already implemented in src/sm.c:
- `__sm_insert_ex()` - Line 1966
- `__sm_delete_ex()` - Line 2062
- `__sm_integer_key()` - Line 1904
- `__sm_last_ex()` - Wrapper around __sm_last
- `__sm_table_moveto()` - Existing function

### Flag Constants
Need to add OPFLAG_* constants to header:
```c
#define OPFLAG_NCHANGE       0x01  /* Increment change counter */
#define OPFLAG_LASTROWID     0x02  /* Set lastRowid */
#define OPFLAG_USESEEKRESULT 0x10  /* Use cached seek result */
#define OPFLAG_APPEND        0x08  /* Hint: appending to end */
#define OPFLAG_SAVEPOSITION  0x02  /* Save cursor position */
#define OPFLAG_AUXDELETE     0x04  /* Auxiliary delete */
```

## File Modifications Required

### 1. src/vdbe.c
- **Line ~402:** Add helper functions before `__vdbe_boolean_value()`
  - `__vdbe_put_varint()`
  - `__vdbe_varint_len()`
  - `__vdbe_serial_type()`
  - `__vdbe_serial_type_len()`
  - `__vdbe_serial_put()`

- **Line ~3700:** Add 5 new opcode cases in `__vdbe_exec()` switch
  - After existing OP_Next/OP_Prev (line ~3690)
  - Before OP_IdxPut (line ~3703)

### 2. src/inc/dbsql_int.in or src/inc/vdbe.h
Add OPFLAG_* constants if not already present

## Testing Strategy

### Unit Tests Needed

1. **OP_NewRowid:**
   - Test sequential allocation (1, 2, 3, ...)
   - Test wraparound to random when MAX_ROWID reached
   - Test AUTOINCREMENT behavior
   - Test conflict detection in random mode

2. **OP_Insert:**
   - Test inserting with explicit rowid
   - Test with BLOB data
   - Test with MEM_Zero (trailing zeros)
   - Test USESEEKRESULT optimization
   - Test change counter updates

3. **OP_Delete:**
   - Test delete at current position
   - Test SAVEPOSITION behavior
   - Test change counter updates

4. **OP_MakeRecord:**
   - Test all serial types (NULL, INT, REAL, TEXT, BLOB)
   - Test special integer optimizations (0, 1)
   - Test variable-length encoding
   - Test record round-trip (pack/unpack)

5. **OP_IdxInsert:**
   - Test inserting into index
   - Test uniqueness checking
   - Test compound index keys

### Integration Tests

1. Full INSERT statement (uses NewRowid, MakeRecord, Insert)
2. Full DELETE statement (uses seek, Delete)
3. Full CREATE INDEX + INSERT (uses MakeRecord, IdxInsert)
4. Large table with 1M+ rows (test random rowid mode)

## Compatibility Notes

### Register-Based Architecture
These opcodes follow SQLite 3.52.0 register-based model:
- All data passed via registers (not stack)
- Cursors accessed via `p->apCsr[P1]` (pointer array)
- Memory cells use `Mem` structure with union for i64/double/string

### Differences from Old DBSQL Opcodes

| Old Opcode | New Opcode | Key Differences |
|------------|------------|-----------------|
| OP_NewRecno | OP_NewRowid | 64-bit rowids, register output |
| OP_PutIntKey | OP_Insert | Uses sm_payload_t, more flags |
| OP_Delete | OP_Delete | Similar but register-based |
| OP_MakeRecord | OP_MakeRecord | New serial type encoding, varint headers |
| OP_IdxPut | OP_IdxInsert | Uses sm_payload_t, unified with Insert |

### Backward Compatibility
- Old stack-based opcodes remain functional during transition
- Code generators updated separately to emit new opcodes
- Can run both old and new opcodes in same VDBE (for testing)

## Performance Considerations

### OP_NewRowid
- **Optimization:** Cache next rowid in VdbeCursor.nextRowid
- **Worst case:** Random mode requires 100 seeks (O(log N) each)
- **Best case:** Sequential allocation with cached value (O(1))

### OP_Insert
- **Optimization:** USESEEKRESULT avoids redundant btree seek
- **Optimization:** APPEND hint enables fast append path
- **Cost:** O(log N) for btree insert, plus record serialization

### OP_MakeRecord
- **Cost:** O(N) where N = number of fields
- **Memory:** Allocates nVarint + nHdr + nData bytes
- **Optimization:** Small integers (0, 1) encode as single varint

### OP_IdxInsert
- **Cost:** Same as OP_Insert but for index btrees
- **Note:** Indexes often smaller than tables (fewer levels)

## Known Limitations

1. **Affinity Handling:** OP_MakeRecord has simplified affinity application
   - Full implementation needs `__vdbe_apply_affinity()` function
   - Current code has placeholder comment

2. **Update Hooks:** OP_Insert and OP_Delete don't call update hooks yet
   - SQLite calls `db->xUpdateCallback()` after DML
   - DBSQL may not have hooks implemented

3. **Pre-Update Hooks:** Not implemented
   - SQLite 3.52.0 has pre-update hooks for replication
   - Can add later if needed

4. **Error Messages:** Basic error handling only
   - SQLite builds detailed error messages
   - DBSQL may want richer diagnostics

## References

- SQLite 3.52.0 source: `_/sqlite/src/vdbe.c`
- DBSQL SM layer: `src/sm.c`
- VDBE architecture doc: `vdbe_analysis_report.md`
- Type system update: `type_comparison_report.md`
- Task definition: Task #20 in project plan

## Completion Status

### Implemented ✓
- [x] Helper functions (varint, serial type)
- [x] OP_NewRowid (line-by-line from SQLite reference)
- [x] OP_Insert (adapted for SM layer)
- [x] OP_Delete (adapted for SM layer)
- [x] OP_MakeRecord (full serial type encoding)
- [x] OP_IdxInsert (index-specific payload)

### Needs Integration
- [ ] Add helper functions to vdbe.c (before line 424)
- [ ] Add opcode cases to __vdbe_exec() switch (after line 3690)
- [ ] Add OPFLAG_* constants to header
- [ ] Compile and test
- [ ] Update code generators to emit new opcodes

### Future Work
- [ ] Implement `__vdbe_apply_affinity()` for proper type coercion
- [ ] Add update hooks (if DBSQL supports them)
- [ ] Performance tuning and profiling
- [ ] Comprehensive test suite
