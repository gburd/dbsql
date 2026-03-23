# Task #5: Structure Modification Status Report

## Summary

This report documents the status of updating 16 existing structures in DBSQL to match SQLite 3.52.0 definitions. Based on analysis of `/home/gburd/ws/dbsql/src/inc/dbsql_int.in`, the following structures have been evaluated:

## Status Overview

| # | Structure | SQLite Line | DBSQL Line | Status | Risk | Notes |
|---|-----------|-------------|------------|--------|------|-------|
| 1 | Column | 2247 | 692 | ✅ COMPLETE | HIGH | Fully updated |
| 2 | Table | 2427 | 797 | ✅ COMPLETE | HIGH | Fully updated with union |
| 3 | FKey (foreign_key) | ~2570 | 894 | ✅ COMPLETE | MEDIUM | Fully updated |
| 4 | Index | ~2680 | 974 | ✅ COMPLETE | HIGH | Fully updated |
| 5 | Token | 2882 | 1044 | ✅ COMPLETE | LOW | Fully updated |
| 6 | Expr | 3034 | 1073 | ✅ COMPLETE | CRITICAL | Fully updated with unions |
| 7 | ExprList | 3234 | 1211 | ✅ COMPLETE | HIGH | Fully updated with FLEXARRAY |
| 8 | IdList | 3291 | 1273 | ✅ COMPLETE | LOW | Fully updated with FLEXARRAY |
| 9 | SrcList | 3413 | 1305 | ⚠️ PARTIAL | CRITICAL | Simplified inline version |
| 10 | Select | 3592 | 1395 | ✅ COMPLETE | HIGH | Fully updated |
| 11 | FuncDef | ~1974 | 644 | ✅ COMPLETE | HIGH | Fully updated |
| 12 | AggInfo | 2900 | 1485 | ✅ COMPLETE | HIGH | Fully updated, replaces old agg* |
| 13 | Parse (parser) | 3877 | 1556 | ❌ NOT UPDATED | CRITICAL | Still uses old DBSQL format |
| 14 | Trigger | ~4150 | 1622 | ✅ COMPLETE | MEDIUM | Fully updated |
| 15 | TriggerStep (trigger_step) | ~4125 | 1652 | ✅ COMPLETE | MEDIUM | Fully updated |
| 16 | Db (__dbsql_db) | ~1476 | 526 | ✅ COMPLETE | HIGH | Updated with pSchema |

## Detailed Analysis

### 1. Column (struct column) - ✅ COMPLETE

**Location**: Line 692 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- `zName` → `zCnName` (combined name allocation)
- `zDflt` removed (replaced by `iDflt` index)
- `zType` removed (replaced by `eCType` enum)
- `notNull` widened to 4-bit OE_ code
- `isPrimKey` removed (now in `colFlags`)
- `sortOrder` removed (now in Index.aSortOrder)
- New fields: `affinity`, `szEst`, `hName`, `iDflt`, `colFlags`, `eCType`

**Impact**: All column references in codebase affected.

---

### 2. Table (struct table) - ✅ COMPLETE

**Location**: Line 797 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format with discriminated union

**Key Changes Implemented**:
- `readOnly` → `TF_Readonly` in `tabFlags`
- `iDb` removed (now via `pSchema`)
- `isTransient` → `TF_Ephemeral` in `tabFlags`
- `hasPrimKey` → `TF_HasPrimaryKey` in `tabFlags`
- `pSelect` and `pFKey` moved into union `u`
- `nCol`/`iPKey` changed from `int` to `i16`
- `tnum` changed from `int` to `Pgno`
- New fields: `zColAff`, `pCheck`, `nTabRef`, `tabFlags`, `nNVCol`, `nRowLogEst`, `szTabRow`, `eTabType`, `u` (union), `pSchema`, `aHx[16]`

**Impact**: Fundamental structural change affecting all table operations.

---

### 3. FKey (struct foreign_key) - ✅ COMPLETE

**Location**: Line 894 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- New: `pPrevTo` (doubly-linked list)
- `updateConf`/`deleteConf`/`insertConf` → `aAction[2]`
- New: `apTrigger[2]` (FK enforcement triggers)
- `*aCol` → `aCol[FLEXARRAY]` (flexible array member)

**Impact**: Foreign key constraint handling updated.

---

### 4. Index (struct index) - ✅ COMPLETE

**Location**: Line 974 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- New: `aiRowLogEst`, `zColAff`, `pSchema`, `aSortOrder`, `azColl`
- New: `pPartIdxWhere`, `aColExpr`, `szIdxRow`, `nKeyCol`
- New bitfields: `idxType`, `bUnordered`, `uniqNotNull`, `isResized`, `isCovering`, `noSkipScan`, `hasStat1`, `bNoQuery`, `bAscKeyBug`, `bHasVCol`, `bHasExpr`
- New STAT4 fields: `nSample`, `mxSample`, `nSampleCol`, `aAvgEq`, `aSample`, `aiRowEst`, `nRowEst0`
- New: `colNotIdxed` bitmask
- `autoIndex` removed (replaced by `idxType`)
- `iDb` removed (now in Schema)

**Impact**: Major impact on index operations and query planning.

---

### 5. Token (struct token) - ✅ COMPLETE

**Location**: Line 1044 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- `dyn` bitfield removed
- `n` changed from `unsigned:31` to `unsigned int`

**Impact**: Low - straightforward change.

---

### 6. Expr (struct expr) - ✅ COMPLETE

**Location**: Line 1073 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- `dataType` → `affExpr` (affinity)
- New: `op2`, `vvaFlags` (debug only)
- `flags` widened from `u8` to `u32`
- `token`/`span` removed, replaced by `u.zToken` and `w.iOfst`
- New: `u` union (zToken/iValue)
- `pList`/`pSelect` moved into union `x`
- New: `nHeight`, `w` union (iJoin/iOfst), `pAggInfo`, `y` union (pTab/pWin/nReg/sub)
- `iDb` removed (resolved through Table->pSchema)
- `iColumn` changed from `int` to `ynVar` (i16)
- `iAgg` changed from `int` to `i16`

**Impact**: CRITICAL - every expression in every SQL statement affected.

---

### 7. ExprList (struct expr_list) - ✅ COMPLETE

**Location**: Line 1211 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- `zName` → `zEName` with `eEName` enum
- `sortOrder` → `sortFlags` (KEYINFO_ORDER_* bitmask)
- `isAgg` removed (tracked in Expr flags)
- New: `fg` bitfield struct with 9 flags
- New: `u` union (x.iOrderByCol/iAlias vs iConstExprReg)
- Array changed to `FLEXARRAY`

**Impact**: High - affects all expression list processing.

---

### 8. IdList (struct id_list) - ✅ COMPLETE

**Location**: Line 1273 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- `nAlloc` removed
- `idx` removed from items
- Array changed to `FLEXARRAY`

**Impact**: Low - simple structural cleanup.

---

### 9. SrcList (struct src_list) - ⚠️ PARTIAL

**Location**: Line 1305 in dbsql_int.in

**Status**: Simplified inline version; full SrcItem struct deferred

**Current Implementation**:
- `nSrc` widened from `u16` to `int`
- `nAlloc` widened from `u16` to `u32`
- Array changed to `FLEXARRAY`
- Inline `src_list_item` with essential fields retained

**Not Yet Implemented**:
- Full separate `src_item_t` struct with:
  - `fg` bitfield struct (19+ flags)
  - `u1`/`u2`/`u3`/`u4` unions for various data
  - `zDatabase` moved into `u4.zDatabase`
  - `pSelect` moved into `u4.pSubq->pSelect`
  - `pOn`/`pUsing` moved into `u3` union

**Impact**: CRITICAL - fundamental FROM clause representation. Currently using simplified version for Phase 1/2 transition.

**Action Required**: Full SrcItem struct implementation needed in later phase.

---

### 10. Select (struct select) - ✅ COMPLETE

**Location**: Line 1395 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- `isDistinct` → `selFlags` bitmask (SF_Distinct)
- `nLimit`/`nOffset` removed (now in `pLimit` expr)
- `zSelect` removed
- New: `nSelectRow`, `selFlags`, `selId`, `pNext`, `pLimit` (Expr*), `pWith`, `pWin`, `pWinDefn`

**Impact**: High - affects all SELECT statement processing.

---

### 11. FuncDef (struct func_def) - ✅ COMPLETE

**Location**: Line 644 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- `xFunc`/`xStep` → single `xSFunc`
- Function signatures changed (sqlite3_context/sqlite3_value → dbsql_func_t)
- `dataType` removed (replaced by `funcFlags`)
- New: `funcFlags`, `xValue`, `xInverse` (window function support), `zName`, `u` union (pHash/pDestructor)
- `nArg` changed from `int` to `i16`

**Impact**: High - all function definitions must use new format.

---

### 12. AggInfo (struct agg_info) - ✅ COMPLETE

**Location**: Line 1485 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format, replaces old agg* structs

**Key Changes Implemented**:
- Complete restructure from separate `agg`, `agg_expr`, `agg_elem` structures
- New: `directMode`, `useSortingIdx`, `nSortingColumn`, `sortingIdx`, `sortingIdxPTab`, `iFirstReg`, `pGroupBy`, `nAccumulator`, `nFunc`, `selId`
- Inline sub-structs: `agg_info_col` and `agg_info_func`
- `agg_info_col`: `pTab`, `pCExpr`, `iTable`, `iColumn`, `iSorterColumn`
- `agg_info_func`: `pFExpr`, `pFunc`, `iDistinct`, `iDistAddr`, `iOBTab`, `bOBPayload`, `bOBUnique`, `bUseSubtype`

**Impact**: High - complete replacement of old aggregate structures.

**Note**: Legacy `struct agg_expr` retained at line 1545 for compatibility during transition.

---

### 13. Parse (struct parser) - ❌ NOT UPDATED

**Location**: Line 1556 in dbsql_int.in

**Status**: Still using old DBSQL format, needs complete overhaul

**Current DBSQL Fields**:
```c
DBSQL *db; int rc; dbsql_callback xCallback; void *pArg;
char *zErrMsg; token_t sErrToken; token_t sFirstToken; token_t sLastToken;
const char *zTail; table_t *pNewTable; vdbe_t *pVdbe;
u_int8_t colNamesSet; u_int8_t explain; u_int8_t initFlag;
u_int8_t nameClash; u_int8_t useAgg; u_int8_t iDb; u_int8_t useCallback;
int newTnum; int nErr; int nTab; int nMem; int nSet; int nAgg; int nVar;
agg_expr_t *aAgg; const char *zAuthContext;
trigger_t *pNewTrigger; trigger_stack_t *trigStack;
```

**Required SQLite 3.52.0 Fields** (partial list):
```c
sqlite3 *db; char *zErrMsg; Vdbe *pVdbe; int rc;
LogEst nQueryLoop; u8 nested; u8 nTempReg; u8 isMultiWrite;
u8 disableLookaside; u8 prepFlags; u8 withinRJSubrtn; u8 mSubrtnSig;
u8 eTriggerOp; u8 eOrconf; bft disableTriggers:1; bft mayAbort:1;
bft hasCompound:1; bft bReturning:1; bft bHasExists:1; bft colNamesSet:1;
bft bHasWith:1; bft okConstFactor:1; bft checkSchema:1;
int nRangeReg; int iRangeReg; int nErr; int nTab; int nMem;
int szOpAlloc; int iSelfTab; int nLabel; int nLabelAlloc; int *aLabel;
ExprList *pConstExpr; IndexedExpr *pIdxEpr; IndexedExpr *pIdxPartExpr;
yDbMask writeMask; yDbMask cookieMask; int nMaxArg; int nSelect;
AutoincInfo *pAinc; Parse *pToplevel; Table *pTriggerTab;
TriggerPrg *pTriggerPrg; ParseCleanup *pCleanup;
int aTempReg[8]; Parse *pOuterParse; Token sNameToken;
u32 oldmask; u32 newmask; union u1; Token sLastToken;
ynVar nVar; u8 iPkSortOrder; u8 explain; u8 eParseMode;
int nHeight; int addrExplain; VList *pVList; Trigger *pReprepare;
char *zTail; Table *pNewTable; Index *pNewIndex; Trigger *pNewTrigger;
const char *zAuthContext; With *pWith; RenameToken *pRename;
```

**Fields to Remove**:
- `xCallback`, `pArg`, `sErrToken`, `sFirstToken`
- `initFlag`, `nameClash`, `useAgg`, `useCallback`
- `newTnum`, `nSet`, `nAgg`, `aAgg`, `trigStack`

**Fields to Add** (60+ new fields):
- Organized into header section (zeroed on init)
- Non-recursive section (persists between recursions)
- Recursive section (reset per recursion)
- New tracking: `nQueryLoop`, `nested`, `nTempReg`, `isMultiWrite`, `disableLookaside`, `prepFlags`, `withinRJSubrtn`, `mSubrtnSig`, `eTriggerOp`, `eOrconf`
- Bitfields: `disableTriggers`, `mayAbort`, `hasCompound`, `bReturning`, `bHasExists`, `colNamesSet`, `bHasWith`, `okConstFactor`, `checkSchema`
- New registers: `nRangeReg`, `iRangeReg`, `szOpAlloc`, `iSelfTab`
- New label tracking: `nLabel`, `nLabelAlloc`, `*aLabel`
- New expressions: `pConstExpr`, `pIdxEpr`, `pIdxPartExpr`
- New masks: `writeMask`, `cookieMask`
- New metadata: `nMaxArg`, `nSelect`
- New structures: `pAinc`, `pToplevel`, `pTriggerTab`, `pTriggerPrg`, `pCleanup`
- Temporary registers: `aTempReg[8]`
- Parsing context: `pOuterParse`, `sNameToken`, `oldmask`, `newmask`, `u1` union
- And many more...

**Impact**: CRITICAL - the parser struct is the core of all SQL processing. This is a massive update affecting the entire codebase.

**Action Required**: Complete rewrite of parser struct needed. This is the most complex structure update.

---

### 14. Trigger (struct trigger) - ✅ COMPLETE

**Location**: Line 1622 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- `name` → `zName`
- `iDb`/`iTabDb` → `pSchema`/`pTabSchema`
- `foreach` removed
- `nameToken` removed
- New: `bReturning`

**Impact**: Medium - trigger processing updated.

---

### 15. TriggerStep (struct trigger_step) - ✅ COMPLETE

**Location**: Line 1652 in dbsql_int.in

**Status**: Fully updated to SQLite 3.52.0 format

**Key Changes Implemented**:
- `target` (Token) → `pSrc` (SrcList*)
- `op`/`orconf` type changed from `int` to `u8`
- New: `pUpsert`, `zSpan`, `pLast`

**Impact**: Medium - trigger step processing updated.

---

### 16. Db (struct __dbsql_db) - ✅ COMPLETE

**Location**: Line 526 in dbsql_int.in

**Status**: Updated with pSchema pointer; transitional fields marked as deprecated

**Key Changes Implemented**:
- `zName` → maintains `zName` (compatible)
- New: `safety_level`, `bSyncSet`
- New: `pSchema` pointer (main change)
- Preserved BDB-specific: `inTrans`, `dbp`
- Marked as deprecated: `schema_sig`, `tblHash`, `idxHash`, `trigHash`, `aFKey`, `flags`

**Schema Migration Strategy**:
- Old code can still use `aDb[i].tblHash` etc.
- New code should use `aDb[i].pSchema->tblHash`
- Gradual migration path enabled

**Impact**: High - requires Schema extraction, but transition path provided.

---

## Overall Status Summary

### Completed: 14 of 16 structures (87.5%)

**Fully Updated**:
1. ✅ Column
2. ✅ Table
3. ✅ FKey
4. ✅ Index
5. ✅ Token
6. ✅ Expr
7. ✅ ExprList
8. ✅ IdList
10. ✅ Select
11. ✅ FuncDef
12. ✅ AggInfo
14. ✅ Trigger
15. ✅ TriggerStep
16. ✅ Db

**Needs Work**:
9. ⚠️ SrcList - Simplified version in place, full SrcItem struct deferred
13. ❌ Parse - Not updated, requires complete overhaul

## Risk Assessment

### CRITICAL Priority (Must Address):
- **Parse (struct parser)** - Core of all SQL processing, affects entire codebase
- **SrcList/SrcItem** - Full implementation deferred but will be needed

### Notes on Parser Update

The parser struct update is the most complex and invasive change. It requires:

1. Field reorganization into three sections:
   - Header (zeroed on init)
   - Non-recursive (persists)
   - Recursive (reset per recursion)

2. Adding 60+ new fields for:
   - Query optimization tracking
   - Temporary register management
   - Label management
   - Expression caching
   - Trigger context
   - Nested parsing support
   - Window function support
   - CTE support
   - RETURNING clause support

3. Removing legacy fields:
   - Old callback mechanism
   - Old aggregate tracking
   - Old trigger stack

4. Code impact:
   - Every parser initialization
   - Every code generator
   - Every SQL statement processor
   - All trigger handling
   - All aggregate processing

This update should be coordinated with Tasks #2 (VDBE infrastructure), #6 (VDBE interpreter), and #7 (code generators).

## Recommendations

1. **Task #5 Status**: Mark as INCOMPLETE
   - 14 of 16 structures complete
   - Parse struct requires dedicated effort
   - SrcList needs full SrcItem implementation

2. **Parse Struct Update Strategy**:
   - Create separate task for Parse struct overhaul
   - Coordinate with VDBE tasks
   - Phase the update:
     a. Add new fields alongside old
     b. Update code generators to use new fields
     c. Remove old fields when no longer referenced

3. **SrcList/SrcItem Strategy**:
   - Current simplified version sufficient for Phase 1/2
   - Full SrcItem implementation can wait until Phase 3
   - Document the limitation

## Files Modified

- `/home/gburd/ws/dbsql/src/inc/dbsql_int.in` - All structure definitions updated

## Dependencies

- Schema struct (from Task #4) - ✅ Available
- Window struct (from Task #4) - ✅ Available for conditional compilation
- With/Cte structs (from Task #4) - ✅ Available for conditional compilation
- Upsert struct (from Task #4) - ✅ Available

## Conclusion

Task #5 is 87.5% complete. The Parse struct update is a major undertaking that should be treated as a separate sub-task or coordinated with other VDBE-related tasks. The SrcList simplification is acceptable for the current phase.

The completion of 14 out of 16 structures represents substantial progress toward SQLite 3.52.0 compatibility. The remaining work on Parse and SrcItem is significant but well-defined.
