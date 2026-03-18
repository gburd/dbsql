# Parse Struct Migration Plan: DBSQL → SQLite 3.52.0

## Overview

This document maps the transformation of the DBSQL `struct parser` (line 1556) to SQLite 3.52.0's `struct Parse` (sqliteInt.h line 3877).

**Current DBSQL**: 27 fields in flat structure
**Target SQLite 3.52.0**: 80+ fields organized in 3 memory sections

---

## Memory Section Organization

SQLite 3.52.0 divides Parse into three distinct regions for initialization efficiency:

### Section 1: Header (Always Zeroed)
Lines 3878-3938: Fields from `db` through `pCleanup`
- Zeroed on every Parse initialization
- Contains active parsing state

### Section 2: Non-Recursive Constants (aTempReg boundary)
Lines 3947-3962: Fields from `aTempReg[8]` through `u1` union
- Set once, persist across recursive parsing
- Not zeroed between recursions

### Section 3: Recursive (sLastToken boundary)
Lines 3971-3997: Fields from `sLastToken` through `pRename`
- Reset before and after each recursion level
- Contains per-statement state

---

## Field-by-Field Mapping

### SECTION 1: HEADER (Fields to zero on init)

#### Core Database & VDBE (KEEP)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `DBSQL *db` | `sqlite3 *db` | ✅ KEEP | Already correct |
| `vdbe_t *pVdbe` | `Vdbe *pVdbe` | ✅ KEEP | Already correct |
| `int rc` | `int rc` | ✅ KEEP | Already correct |
| `char *zErrMsg` | `char *zErrMsg` | ✅ KEEP | Already correct |

#### Query Optimization & Parsing State (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `LogEst nQueryLoop` | ➕ ADD | Estimated query loop iterations (10*log2(N)) |
| `u8 nested` | ➕ ADD | Nesting depth of parser/code generator |
| `u8 nTempReg` | ➕ ADD | Number of temp registers in aTempReg[] |
| `u8 isMultiWrite` | ➕ ADD | True if statement may modify multiple rows |
| `u8 disableLookaside` | ➕ ADD | Lookaside memory disable counter |
| `u8 prepFlags` | ➕ ADD | SQLITE_PREPARE_* flags |
| `u8 withinRJSubrtn` | ➕ ADD | Nesting level for RIGHT JOIN subroutines |
| `u8 mSubrtnSig` | ➕ ADD | Mini Bloom filter on available SubrtnSig.selId |

#### Trigger Compilation Context (ADD/MODIFY)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| N/A | `u8 eTriggerOp` | ➕ ADD | TK_UPDATE, TK_INSERT, or TK_DELETE |
| N/A | `u8 eOrconf` | ➕ ADD | Default ON CONFLICT policy for trigger steps |

#### Debug & Testing (CONDITIONAL ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `u8 earlyCleanup` | ➕ ADD | OOM inside sqlite3ParserAddCleanup() [DEBUG/COVERAGE] |
| `u8 ifNotExists` | ➕ ADD | Might be true if IF NOT EXISTS [DEBUG] |
| `u8 isCreate` | ➕ ADD | CREATE TABLE/INDEX/VIEW or ALTER TABLE ADD COLUMN [DEBUG] |

#### Statement Properties (ADD/CONVERT)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `u_int8_t colNamesSet` | `bft colNamesSet:1` | 🔄 CONVERT | Move to bitfield |
| `u_int8_t explain` | (moved to Section 3) | ⬇️ MOVE | Becomes `u8 explain` in recursive section |
| N/A | `bft disableTriggers:1` | ➕ ADD | True to disable triggers |
| N/A | `bft mayAbort:1` | ➕ ADD | Statement may throw ABORT exception |
| N/A | `bft hasCompound:1` | ➕ ADD | Need convertCompoundSelectToSubquery() |
| N/A | `bft bReturning:1` | ➕ ADD | Coding a RETURNING trigger |
| N/A | `bft bHasExists:1` | ➕ ADD | Has correlated "EXISTS (SELECT ...)" |
| N/A | `bft bHasWith:1` | ➕ ADD | Statement contains WITH clause |
| N/A | `bft okConstFactor:1` | ➕ ADD | OK to factor out constants |
| N/A | `bft checkSchema:1` | ➕ ADD | Force schema cookie check after error |

#### Register & Memory Management (ADD/MODIFY)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `int nMem` | `int nMem` | ✅ KEEP | Memory cells used so far |
| `int nTab` | `int nTab` | ✅ KEEP | Previously allocated VDBE cursors |
| N/A | `int nRangeReg` | ➕ ADD | Size of temporary register block |
| N/A | `int iRangeReg` | ➕ ADD | First register in temp register block |
| N/A | `int szOpAlloc` | ➕ ADD | Bytes allocated for Vdbe.aOp[] |
| N/A | `int iSelfTab` | ➕ ADD | Table w/ index on expr, or neg base reg during CHECK |

#### Error Handling (MODIFY/REMOVE)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `int nErr` | `int nErr` | ✅ KEEP | Number of errors seen |
| `token_t sErrToken` | (removed) | ❌ REMOVE | No longer tracked |

#### Label Management (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `int nLabel` | ➕ ADD | *Negative* of number of labels used |
| `int nLabelAlloc` | ➕ ADD | Slots allocated in aLabel |
| `int *aLabel` | ➕ ADD | Label tracking array |

#### Expression & Index Optimization (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `ExprList *pConstExpr` | ➕ ADD | Constant expressions |
| `IndexedExpr *pIdxEpr` | ➕ ADD | Expressions used by active indexes |
| `IndexedExpr *pIdxPartExpr` | ➕ ADD | Expressions constrained by index WHERE clauses |

#### Database Write Tracking (ADD)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `u_int8_t iDb` | (moved to Section 3) | ⬇️ MOVE | Becomes per-statement in recursive section |
| N/A | `yDbMask writeMask` | ➕ ADD | Bitmask: start write transaction on these DBs |
| N/A | `yDbMask cookieMask` | ➕ ADD | Bitmask: schema verified databases |

#### Virtual Table Support (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `int nMaxArg` | ➕ ADD | Max args to xUpdate/xFilter vtab methods |

#### SELECT Statement Tracking (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `int nSelect` | ➕ ADD | Number of SELECT stmts (counter for Select.selId) |

#### Progress Callback (CONDITIONAL ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `u32 nProgressSteps` | ➕ ADD | xProgress steps during sqlite3_prepare() [!OMIT_PROGRESS_CALLBACK] |

#### Shared Cache Locking (CONDITIONAL ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `int nTableLock` | ➕ ADD | Number of locks in aTableLock [!OMIT_SHARED_CACHE] |
| `TableLock *aTableLock` | ➕ ADD | Required table locks [!OMIT_SHARED_CACHE] |

#### AUTOINCREMENT Support (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `AutoincInfo *pAinc` | ➕ ADD | AUTOINCREMENT counter information |

#### Nested Parsing Context (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `Parse *pToplevel` | ➕ ADD | Parse structure for main program (NULL if this is top) |

#### Trigger Compilation (MODIFY)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `trigger_t *pNewTrigger` | (moved to Section 3) | ⬇️ MOVE | Becomes `Trigger *pNewTrigger` in recursive |
| N/A | `Table *pTriggerTab` | ➕ ADD | Table for which triggers are being coded |
| N/A | `TriggerPrg *pTriggerPrg` | ➕ ADD | Linked list of coded triggers |
| `trigger_stack_t *trigStack` | (replaced) | ❌ REMOVE | Replaced by pToplevel mechanism |

#### Cleanup & Resource Management (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `ParseCleanup *pCleanup` | ➕ ADD | List of cleanup operations after parse |

---

### SECTION 2: NON-RECURSIVE (Persist across recursions)

#### Temporary Register Cache
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `int aTempReg[8]` | ➕ ADD | Holding area for temporary registers |

#### Outer Parse Context
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `Parse *pOuterParse` | ➕ ADD | Outer Parse when nested |

#### Schema Object Name
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `Token sNameToken` | ➕ ADD | Token with unqualified schema object name |

#### Trigger OLD/NEW Column Tracking
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `u32 oldmask` | ➕ ADD | Mask of old.* columns referenced |
| `u32 newmask` | ➕ ADD | Mask of new.* columns referenced |

#### CREATE vs Other Statement Data (Union)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `union u1` | ➕ ADD | Discriminated union for statement-specific data |
| `u1.cr.addrCrTab` | ➕ ADD | Address of OP_CreateBtree (CREATE) |
| `u1.cr.regRowid` | ➕ ADD | Register holding rowid (CREATE) |
| `u1.cr.regRoot` | ➕ ADD | Register holding root page (CREATE) |
| `u1.cr.constraintName` | ➕ ADD | Name of constraint being parsed (CREATE) |
| `u1.d.pReturning` | ➕ ADD | The RETURNING clause (other statements) |

---

### SECTION 3: RECURSIVE (Reset per recursion)

#### Token Tracking (MODIFY)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `token_t sFirstToken` | (removed) | ❌ REMOVE | Not used in 3.52.0 |
| `token_t sLastToken` | `Token sLastToken` | ✅ KEEP | Last token parsed |

#### Variable Binding (MODIFY)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `int nVar` | `ynVar nVar` | 🔄 CONVERT | Change type from int to ynVar (i16) |

#### PRIMARY KEY Sort Order (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `u8 iPkSortOrder` | ➕ ADD | ASC or DESC for INTEGER PRIMARY KEY |

#### EXPLAIN Flag (MOVE)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `u_int8_t explain` | `u8 explain` | ⬇️ MOVE | Moved from Section 1, type changed |

#### Parse Mode (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `u8 eParseMode` | ➕ ADD | PARSE_MODE_NORMAL/DECLARE_VTAB/RENAME/UNMAP |

#### Virtual Table Locking (CONDITIONAL ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `int nVtabLock` | ➕ ADD | Number of virtual tables to lock [!OMIT_VIRTUALTABLE] |
| `Table **apVtabLock` | ➕ ADD | Pointer to virtual tables needing locking [!OMIT_VIRTUALTABLE] |
| `Token sArg` | ➕ ADD | Complete text of module argument [!OMIT_VIRTUALTABLE] |

#### Expression Tree Height (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `int nHeight` | ➕ ADD | Expression tree height of current sub-select |

#### EXPLAIN Query Plan (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `int addrExplain` | ➕ ADD | Address of current OP_Explain opcode |

#### Variable Name Mapping (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `VList *pVList` | ➕ ADD | Mapping between variable names and numbers |

#### Reprepare Context (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `Vdbe *pReprepare` | ➕ ADD | VM being reprepared (sqlite3Reprepare()) |

#### SQL Text Tail (KEEP)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `const char *zTail` | `const char *zTail` | ✅ KEEP | SQL text past last semicolon |

#### Schema Objects Under Construction (KEEP/ADD)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `table_t *pNewTable` | `Table *pNewTable` | ✅ KEEP | Table under CREATE TABLE |
| N/A | `Index *pNewIndex` | ➕ ADD | Index under CREATE INDEX (also UNIQUE during RENAME) |
| `trigger_t *pNewTrigger` | `Trigger *pNewTrigger` | ⬆️ MOVE | Moved from Section 1 |

#### Authorization Context (KEEP)
| DBSQL Current | SQLite 3.52.0 | Action | Notes |
|---------------|---------------|--------|-------|
| `const char *zAuthContext` | `const char *zAuthContext` | ✅ KEEP | 6th parameter to db->xAuth callbacks |

#### WITH Clause Support (ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `With *pWith` | ➕ ADD | Current WITH clause (NULL if none) |

#### ALTER TABLE Support (CONDITIONAL ADD)
| SQLite 3.52.0 | Action | Purpose |
|---------------|--------|---------|
| `RenameToken *pRename` | ➕ ADD | Tokens subject to renaming [!OMIT_ALTERTABLE] |

---

## Fields to REMOVE (Legacy DBSQL)

| Field | Reason |
|-------|--------|
| `dbsql_callback xCallback` | Removed callback mechanism |
| `void *pArg` | Removed callback mechanism |
| `token_t sErrToken` | Error tracking simplified |
| `token_t sFirstToken` | Not used in SQLite 3.52.0 |
| `u_int8_t initFlag` | Replaced by eParseMode |
| `u_int8_t nameClash` | Not used in SQLite 3.52.0 |
| `u_int8_t useAgg` | Aggregate tracking redesigned |
| `u_int8_t useCallback` | Removed callback mechanism |
| `int newTnum` | Not used in SQLite 3.52.0 |
| `int nSet` | Not used in SQLite 3.52.0 |
| `int nAgg` | Replaced by AggInfo tracking |
| `agg_expr_t *aAgg` | Replaced by AggInfo tracking |
| `trigger_stack_t *trigStack` | Replaced by pToplevel/pOuterParse |

---

## Impact on Code Generators

### Affected Code Generators
1. **SELECT code generator** - Uses Parse for register allocation, expression tracking
2. **INSERT code generator** - Uses Parse for table context, conflict resolution
3. **UPDATE code generator** - Uses Parse for SET clause, WHERE clause, triggers
4. **DELETE code generator** - Uses Parse for WHERE clause, triggers
5. **CREATE TABLE code generator** - Uses u1.cr union fields
6. **CREATE INDEX code generator** - Uses pNewIndex
7. **Trigger code generator** - Uses eTriggerOp, eOrconf, pTriggerTab

### Key Dependencies
- **Register allocation**: nMem, nRangeReg, iRangeReg, aTempReg[]
- **Label management**: nLabel, nLabelAlloc, aLabel
- **Expression optimization**: pConstExpr, pIdxEpr, pIdxPartExpr
- **Error handling**: nErr, zErrMsg
- **Database context**: writeMask, cookieMask
- **Trigger context**: eTriggerOp, eOrconf, pTriggerTab, pTriggerPrg

---

## Implementation Strategy

### Phase 1: Add New Fields (Backward Compatible)
1. Add all new fields to struct
2. Initialize new fields to zero/NULL
3. Keep old fields temporarily
4. Code continues using old fields

### Phase 2: Migrate Code Generators
1. Update each generator to use new fields
2. Test each generator independently
3. Verify existing tests pass

### Phase 3: Remove Old Fields
1. Remove unused old fields
2. Update any remaining references
3. Final testing

---

## Summary Statistics

### Field Count
- **Current DBSQL**: 27 fields
- **SQLite 3.52.0**: 80+ fields
- **Fields to ADD**: 63+ new fields
- **Fields to REMOVE**: 12 legacy fields
- **Fields to KEEP**: 15 fields
- **Fields to MODIFY**: 5 fields (type/location changes)

### Complexity Breakdown
- **Section 1 (Header)**: ~50 fields
- **Section 2 (Non-recursive)**: ~8 fields
- **Section 3 (Recursive)**: ~22 fields

### Conditional Compilation
- `SQLITE_DEBUG`: 3 fields
- `SQLITE_COVERAGE_TEST`: 1 field
- `!SQLITE_OMIT_PROGRESS_CALLBACK`: 1 field
- `!SQLITE_OMIT_SHARED_CACHE`: 2 fields
- `!SQLITE_OMIT_VIRTUALTABLE`: 3 fields
- `!SQLITE_OMIT_ALTERTABLE`: 1 field

---

## Testing Requirements

### Unit Tests Needed
1. Parse struct initialization (all three sections)
2. Recursive parsing (section boundary preservation)
3. Register allocation (nMem, nRangeReg, iRangeReg)
4. Label allocation (nLabel, aLabel)
5. Trigger context (eTriggerOp, eOrconf, pTriggerTab)

### Integration Tests Needed
1. SELECT with optimization (pConstExpr, pIdxEpr)
2. INSERT with conflict resolution (eOrconf)
3. UPDATE with triggers (eTriggerOp, pTriggerTab)
4. DELETE with triggers
5. CREATE TABLE with constraints (u1.cr fields)
6. WITH clause (pWith, bHasWith)
7. RETURNING clause (bReturning, u1.d.pReturning)

---

## Dependencies

### Required Typedefs (Must exist)
- `LogEst` - Logarithmic estimation type
- `bft` - Bitfield type (unsigned int)
- `yDbMask` - Database bitmask type
- `ynVar` - Variable number type (i16)
- `IndexedExpr` - Indexed expression tracking
- `ParseCleanup` - Cleanup operation list
- `AutoincInfo` - AUTOINCREMENT tracking
- `TriggerPrg` - Compiled trigger program
- `VList` - Variable list
- `Returning` - RETURNING clause data
- `RenameToken` - ALTER TABLE rename tracking

### Forward Declarations Needed
- All struct typedefs already in dbsql_int.in
- TableLock (for shared cache)

---

## References

- **SQLite 3.52.0 Source**: `_/sqlite/src/sqliteInt.h` lines 3877-3998
- **Current DBSQL**: `src/inc/dbsql_int.in` lines 1556-1598
- **Task Documentation**: `task5_structure_update_status.md` lines 251-315
