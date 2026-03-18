# Type Definition Comparison: SQLite 3.52.0 vs DBSQL

## Overview

This report compares the type/struct definitions in SQLite 3.52.0
(`_/sqlite/src/sqliteInt.h`) against the current DBSQL definitions
(`src/inc/dbsql_int.in`) to identify what needs to be added, modified,
or removed to bring DBSQL up to parity with SQLite 3.52.0's SQL engine.

---

## A. New Structures (in SQLite 3.52.0, not in DBSQL)

These structures exist in SQLite 3.52.0 but have no equivalent in DBSQL.
They must be added to `dbsql_int.in`.

### A1. Schema (line 1501)
- **Purpose**: Represents a database schema (tables, indices, triggers, FKs).
  In SQLite, each Db points to a Schema. In DBSQL, schema info is inlined
  into `__dbsql_db`.
- **Key fields**: `schema_cookie`, `iGeneration`, `tblHash`, `idxHash`,
  `trigHash`, `fkeyHash`, `pSeqTab`, `file_format`, `enc`, `schemaFlags`,
  `cache_size`
- **Recommended DBSQL name**: `schema_t`
- **Risk**: HIGH -- currently DBSQL inlines schema data into `__dbsql_db`.
  Refactoring to use a separate schema object affects all code that
  accesses `aDb[i].tblHash`, `aDb[i].idxHash`, etc.

### A2. CollSeq (line 2304)
- **Purpose**: Defines a collating sequence (name + comparison function).
  DBSQL's old model used simple DBSQL_SO_TEXT/DBSQL_SO_NUM constants.
- **Key fields**: `zName`, `enc`, `pUser`, `xCmp`, `xDel`
- **Recommended DBSQL name**: `coll_seq_t`
- **Risk**: MEDIUM -- required for proper multi-encoding collation support.

### A3. KeyInfo (line 2667)
- **Purpose**: Comparison info for index keys; used by VDBE sort/compare ops.
- **Key fields**: `nRef`, `enc`, `nKeyField`, `nAllField`, `db`,
  `aSortFlags`, `aColl[]`
- **Recommended DBSQL name**: `key_info_t`
- **Risk**: HIGH -- pervasive in VDBE comparison code.

### A4. UnpackedRecord (line 2729)
- **Purpose**: A parsed record used for key comparisons in indexes.
- **Key fields**: `pKeyInfo`, `aMem`, `u` (union z/i), `n`, `nField`,
  `default_rc`, `errCode`, `r1`, `r2`, `eqSeen`
- **Recommended DBSQL name**: `unpacked_record_t`
- **Risk**: MEDIUM -- used in btree/index comparison paths.

### A5. Savepoint (line 2192)
- **Purpose**: Represents a SQL SAVEPOINT.
- **Key fields**: `zName`, `nDeferredCons`, `nDeferredImmCons`, `pNext`
- **Recommended DBSQL name**: `savepoint_t`
- **Risk**: LOW -- straightforward addition.

### A6. Module (line 2213)
- **Purpose**: Represents a virtual table module definition.
- **Key fields**: `pModule`, `zName`, `nRefModule`, `pAux`, `xDestroy`,
  `pEpoTab`
- **Recommended DBSQL name**: `module_t`
- **Risk**: LOW -- only needed if virtual table support is enabled.

### A7. VTable (line 2405)
- **Purpose**: Per-connection virtual table instance.
- **Key fields**: `db`, `pMod`, `pVtab`, `nRef`, `bConstraint`,
  `bAllSchemas`, `eVtabRisk`, `iSavepoint`, `pNext`
- **Recommended DBSQL name**: `vtable_t`
- **Risk**: LOW -- only needed if virtual table support is enabled.

### A8. NameContext (line 3501)
- **Purpose**: Context for resolving table/column names during parsing.
- **Key fields**: `pParse`, `pSrcList`, `uNC` (union: pEList/pAggInfo/
  pUpsert/iBaseReg), `pNext`, `nRef`, `nNcErr`, `ncFlags`,
  `nNestedSelect`, `pWinSelect`
- **Recommended DBSQL name**: `name_context_t`
- **Risk**: HIGH -- core to name resolution in the parser.

### A9. Walker (line 4374)
- **Purpose**: Tree-walking context for traversing parse trees.
- **Key fields**: `pParse`, `xExprCallback`, `xSelectCallback`,
  `xSelectCallback2`, `walkerDepth`, `eCode`, `mWFlags`, `u` (large union)
- **Recommended DBSQL name**: `walker_t`
- **Risk**: MEDIUM -- used in many optimization and transform passes.

### A10. DbFixer (line 4411)
- **Purpose**: Fix database references in parse tree (replaces DBSQL's
  `ref_normalizer_ctx`).
- **Key fields**: `pParse`, `w` (Walker), `pSchema`, `bTemp`, `zDb`,
  `zType`, `pName`
- **Recommended DBSQL name**: `db_fixer_t` (replaces `ref_normalizer_ctx_t`)
- **Risk**: LOW -- direct replacement of existing struct.

### A11. SelectDest (line 3756)
- **Purpose**: Describes where SELECT results should be sent.
- **Key fields**: `eDest`, `iSDParm`, `iSDParm2`, `iSdst`, `nSdst`,
  `zAffSdst`, `pOrderBy`
- **Recommended DBSQL name**: `select_dest_t`
- **Risk**: MEDIUM -- used in SELECT code generation.

### A12. AutoincInfo (line 3775)
- **Purpose**: Tracks AUTOINCREMENT counters during INSERT code generation.
- **Key fields**: `pNext`, `pTab`, `iDb`, `regCtr`
- **Recommended DBSQL name**: `autoinc_info_t`
- **Risk**: LOW -- only needed for AUTOINCREMENT support.

### A13. Upsert (line 3565)
- **Purpose**: Represents ON CONFLICT DO UPDATE/NOTHING clauses.
- **Key fields**: `pUpsertTarget`, `pUpsertTargetWhere`, `pUpsertSet`,
  `pUpsertWhere`, `pNextUpsert`, `isDoUpdate`, `isDup`, `pToFree`,
  `pUpsertIdx`, `pUpsertSrc`, `regData`, `iDataCur`, `iIdxCur`
- **Recommended DBSQL name**: `upsert_t`
- **Risk**: MEDIUM -- new SQL feature (INSERT ... ON CONFLICT).

### A14. Cte (line 4456)
- **Purpose**: Represents a single Common Table Expression.
- **Key fields**: `zName`, `pCols`, `pSelect`, `zCteErr`, `pUse`, `eM10d`
- **Recommended DBSQL name**: `cte_t`
- **Risk**: MEDIUM -- new SQL feature (WITH clauses / CTEs).

### A15. CteUse (line 4498)
- **Purpose**: Usage info for a CTE that persists through code generation.
- **Key fields**: `nUse`, `addrM9e`, `regRtn`, `iCur`, `nRowEst`, `eM10d`
- **Recommended DBSQL name**: `cte_use_t`
- **Risk**: LOW -- accompanies CTE support.

### A16. With (line 4476)
- **Purpose**: Represents a WITH clause containing one or more CTEs.
- **Key fields**: `nCte`, `bView`, `pOuter`, `a[]` (Cte array)
- **Recommended DBSQL name**: `with_t`
- **Risk**: MEDIUM -- new SQL feature.

### A17. Window (line 4556)
- **Purpose**: Represents a window function definition (OVER clause).
- **Key fields**: `zName`, `zBase`, `pPartition`, `pOrderBy`, `eFrmType`,
  `eStart`, `eEnd`, `bImplicitFrame`, `eExclude`, `pStart`, `pEnd`,
  `ppThis`, `pNextWin`, `pFilter`, `pWFunc`, `iEphCsr`, `regAccum`,
  `regResult`, `csrApp`, `regApp`, `regPart`, `pOwner`
- **Recommended DBSQL name**: `window_t`
- **Risk**: HIGH -- window functions are a major new feature affecting
  SELECT processing and VDBE code generation.

### A18. TriggerPrg (line 3800)
- **Purpose**: Compiled trigger program for a specific ON CONFLICT policy.
- **Key fields**: `pTrigger`, `pNext`, `pProgram`, `orconf`, `aColmask[2]`
- **Recommended DBSQL name**: `trigger_prg_t`
- **Risk**: MEDIUM -- replaces DBSQL's `trigger_stack` approach.

### A19. Returning (line 4182)
- **Purpose**: Implements the RETURNING clause for DML statements.
- **Key fields**: `pParse`, `pReturnEL`, `retTrig`, `retTStep`, `iRetCur`,
  `nRetCol`, `iRetReg`, `zName[40]`
- **Recommended DBSQL name**: `returning_t`
- **Risk**: MEDIUM -- new SQL feature (RETURNING clause).

### A20. IndexedExpr (line 3837)
- **Purpose**: Tracks expressions in indexes for optimization.
- **Key fields**: `pExpr`, `iDataCur`, `iIdxCur`, `iIdxCol`,
  `bMaybeNullRow`, `aff`, `pIENext`, `zIdxName`
- **Recommended DBSQL name**: `indexed_expr_t`
- **Risk**: LOW -- optimization feature.

### A21. ParseCleanup (line 3855)
- **Purpose**: Deferred cleanup operations after parsing.
- **Key fields**: `pNext`, `pPtr`, `xCleanup`
- **Recommended DBSQL name**: `parse_cleanup_t`
- **Risk**: LOW -- memory management helper.

### A22. Subquery (line 3312)
- **Purpose**: Details of subquery implementation.
- **Key fields**: `pSelect`, `addrFillSub`, `regReturn`, `regResult`
- **Recommended DBSQL name**: `subquery_t`
- **Risk**: MEDIUM -- used in SrcItem for subquery handling.

### A23. SrcItem (line 3356)
- **Purpose**: Represents a single term in FROM clause. In DBSQL this is
  inlined as `src_list_item` inside `src_list`.
- **Key fields**: `zName`, `zAlias`, `pSTab`, `fg` (bitfield struct),
  `iCursor`, `colUsed`, `u1`/`u2`/`u3`/`u4` (unions)
- **Recommended DBSQL name**: `src_item_t`
- **Risk**: HIGH -- massive restructuring from DBSQL's inline approach.

### A24. OnOrUsing (line 3408)
- **Purpose**: Holds either an ON clause or USING clause for a join.
- **Key fields**: `pOn`, `pUsing`
- **Recommended DBSQL name**: `on_or_using_t`
- **Risk**: LOW -- simple helper struct.

### A25. FuncDestructor (line 2000)
- **Purpose**: Reference-counted destructor for application-defined functions.
- **Key fields**: `nRef`, `xDestroy`, `pUserData`
- **Recommended DBSQL name**: `func_destructor_t`
- **Risk**: LOW

### A26. FuncDefHash (line 1632)
- **Purpose**: Hash table for built-in function definitions.
- **Key fields**: `a[SQLITE_FUNC_HASH_SZ]`
- **Recommended DBSQL name**: `func_def_hash_t`
- **Risk**: LOW

### A27. IndexSample (line 2858)
- **Purpose**: Stat4 sample data for query planning.
- **Key fields**: `p`, `n`, `anEq`, `anLt`, `anDLt`
- **Recommended DBSQL name**: `index_sample_t`
- **Risk**: LOW -- only needed with STAT4 support.

### A28. Lookaside / LookasideSlot (lines 1589, 1608)
- **Purpose**: Memory allocator for small transient objects.
- **Key fields**: (Lookaside) `bDisable`, `sz`, `szTrue`, `bMalloced`,
  `nSlot`, `anStat[3]`, `pInit`, `pFree`, `pStart`, `pEnd`, `pTrueEnd`
- **Recommended DBSQL name**: `lookaside_t` / `lookaside_slot_t`
- **Risk**: LOW -- DBSQL uses BerkeleyDB's memory management, may not need.

### A29. BusyHandler (line 1218)
- **Purpose**: Busy callback handler for locked databases.
- **Key fields**: `xBusyHandler`, `pBusyArg`, `nBusy`
- **Recommended DBSQL name**: `busy_handler_t`
- **Risk**: LOW

### A30. PrintfArguments (line 4827)
- **Purpose**: Arguments to printf-like functions.
- **Recommended DBSQL name**: `printf_arguments_t`
- **Risk**: LOW

### A31. FpDecode (line 4837)
- **Purpose**: Floating-point number decode structure.
- **Recommended DBSQL name**: `fp_decode_t`
- **Risk**: LOW

### A32. RCStr (line 4230)
- **Purpose**: Reference-counted string.
- **Key fields**: `nRCRef`
- **Recommended DBSQL name**: `rcstr_t`
- **Risk**: LOW

### A33. DbClientData (line 4511)
- **Purpose**: Client data for sqlite3_set_clientdata().
- **Key fields**: `pNext`, `pData`, `xDestructor`, `zName[]`
- **Recommended DBSQL name**: `db_client_data_t`
- **Risk**: LOW -- API layer only.

### A34. Sqlite3Config (line 4279)
- **Purpose**: Global configuration singleton.
- **Recommended DBSQL name**: `dbsql_config_t`
- **Risk**: MEDIUM -- many fields reference SQLite-specific subsystems.

### A35. TreeView (line 4527)
- **Purpose**: Debug tree printing. Only in SQLITE_DEBUG builds.
- **Recommended DBSQL name**: `tree_view_t`
- **Risk**: LOW -- debug only.

---

## B. Modified Structures (exist in both, with changes)

### B1. Column (`column` -> SQLite `Column`)

DBSQL (`struct column`):
```c
char *zName;           char *zDflt;           char *zType;
u_int8_t notNull;      u_int8_t isPrimKey;    u_int8_t sortOrder;
```

SQLite 3.52.0 (`struct Column`):
```c
char *zCnName;         /* Combined name/type/coll in single allocation */
unsigned notNull :4;   /* OE_ code (was bool) */
unsigned eCType :4;    /* Standard column type enum */
char affinity;         /* SQLITE_AFF_* value */
u8 szEst;              /* Estimated value size */
u8 hName;              /* Hash of column name */
u16 iDflt;             /* Index into defaults list (was inline string) */
u16 colFlags;          /* COLFLAG_* bitmask */
```

**Changes**:
- `zName` -> `zCnName` (combined name allocation)
- `zDflt` removed (replaced by index `iDflt` into ExprList)
- `zType` removed (replaced by `eCType` enum + COLFLAG_HASTYPE)
- `notNull` widened to 4-bit OE_ conflict resolution code
- `isPrimKey` removed (replaced by COLFLAG_PRIMKEY in colFlags)
- `sortOrder` removed (now stored in Index.aSortOrder)
- NEW: `affinity`, `szEst`, `hName`, `iDflt`, `colFlags`, `eCType`
- **Risk**: HIGH -- every column reference in the codebase is affected.

### B2. Table (`table` -> SQLite `Table`)

DBSQL (`struct table`):
```c
char *zName; int nCol; column_t *aCol; int iPKey; index_t *pIndex;
int tnum; select_t *pSelect; u_int8_t readOnly; u_int8_t iDb;
u_int8_t isTransient; u_int8_t hasPrimKey; u_int8_t keyConf;
trigger_t *pTrigger; foreign_key_t *pFKey;
```

SQLite 3.52.0 (`struct Table`):
```c
char *zName; Column *aCol; Index *pIndex; char *zColAff;
ExprList *pCheck; Pgno tnum; u32 nTabRef; u32 tabFlags;
i16 iPKey; i16 nCol; i16 nNVCol; LogEst nRowLogEst; LogEst szTabRow;
u8 keyConf; u8 eTabType;
union { struct { int addColOffset; FKey *pFKey; ExprList *pDfltList; } tab;
        struct { Select *pSelect; } view;
        struct { int nArg; char **azArg; VTable *p; } vtab; } u;
Trigger *pTrigger; Schema *pSchema; u8 aHx[16];
```

**New fields**: `zColAff`, `pCheck`, `nTabRef`, `tabFlags`, `nNVCol`,
`nRowLogEst`, `szTabRow`, `eTabType`, `u.tab.addColOffset`,
`u.tab.pDfltList`, `u.view.*`, `u.vtab.*`, `pSchema`, `aHx[16]`

**Removed fields**: `readOnly` (now TF_Readonly flag), `iDb` (now in
Schema), `isTransient` (now TF_Ephemeral flag), `hasPrimKey` (now
TF_HasPrimaryKey flag)

**Restructured**: `pSelect` and `pFKey` moved into union `u`
- **Risk**: HIGH -- fundamental structural change.

### B3. FKey (`foreign_key` -> SQLite `FKey`)

DBSQL (`struct foreign_key`):
```c
table_t *pFrom; foreign_key_t *pNextFrom; char *zTo;
foreign_key_t *pNextTo; int nCol;
struct col_map { int iFrom; char *zCol; } *aCol;
u_int8_t isDeferred; u_int8_t updateConf; u_int8_t deleteConf;
u_int8_t insertConf;
```

SQLite 3.52.0 (`struct FKey`):
```c
Table *pFrom; FKey *pNextFrom; char *zTo; FKey *pNextTo;
FKey *pPrevTo;        /* NEW: doubly-linked list */
int nCol;
u8 isDeferred;
u8 aAction[2];        /* Replaces updateConf/deleteConf/insertConf */
Trigger *apTrigger[2]; /* NEW: FK enforcement triggers */
struct sColMap { int iFrom; char *zCol; } aCol[FLEXARRAY];
```

**New fields**: `pPrevTo`, `aAction[2]`, `apTrigger[2]`
**Changed**: `updateConf`/`deleteConf`/`insertConf` -> `aAction[2]`
**Changed**: `*aCol` -> `aCol[FLEXARRAY]` (flexible array member)
- **Risk**: MEDIUM

### B4. Index (`index` -> SQLite `Index`)

DBSQL (`struct index`):
```c
char *zName; int nColumn; int *aiColumn; table_t *pTable;
int tnum; u_int8_t onError; u_int8_t autoIndex; u_int8_t iDb;
index_t *pNext;
```

SQLite 3.52.0 (`struct Index`):
```c
char *zName; i16 *aiColumn; LogEst *aiRowLogEst; Table *pTable;
char *zColAff; Index *pNext; Schema *pSchema; u8 *aSortOrder;
const char **azColl; Expr *pPartIdxWhere; ExprList *aColExpr;
Pgno tnum; LogEst szIdxRow; u16 nKeyCol; u16 nColumn;
u8 onError; unsigned idxType:2; unsigned bUnordered:1;
unsigned uniqNotNull:1; unsigned isResized:1; unsigned isCovering:1;
unsigned noSkipScan:1; unsigned hasStat1:1; unsigned bNoQuery:1;
unsigned bAscKeyBug:1; unsigned bHasVCol:1; unsigned bHasExpr:1;
#ifdef SQLITE_ENABLE_STAT4
int nSample; int mxSample; int nSampleCol; tRowcnt *aAvgEq;
IndexSample *aSample; tRowcnt *aiRowEst; tRowcnt nRowEst0;
#endif
Bitmask colNotIdxed;
```

**New fields**: `aiRowLogEst`, `zColAff`, `pSchema`, `aSortOrder`,
`azColl`, `pPartIdxWhere`, `aColExpr`, `szIdxRow`, `nKeyCol`,
`idxType`, `bUnordered`, `uniqNotNull`, `isResized`, `isCovering`,
`noSkipScan`, `hasStat1`, `bNoQuery`, `bAscKeyBug`, `bHasVCol`,
`bHasExpr`, STAT4 fields, `colNotIdxed`
**Removed**: `autoIndex` (replaced by `idxType`), `iDb` (in Schema)
- **Risk**: HIGH

### B5. Token (`token` -> SQLite `Token`)

DBSQL:
```c
const char *z; unsigned dyn:1; unsigned n:31;
```

SQLite 3.52.0:
```c
const char *z; unsigned int n;
```

**Changed**: `dyn` bitfield removed, `n` is now full `unsigned int`
- **Risk**: LOW -- straightforward.

### B6. Expr (`expr` -> SQLite `Expr`)

DBSQL:
```c
u_int8_t op; u_int8_t dataType; u_int8_t iDb; u_int8_t flags;
expr_t *pLeft, *pRight; expr_list_t *pList; token_t token; token_t span;
int iTable, iColumn; int iAgg; select_t *pSelect;
```

SQLite 3.52.0:
```c
u8 op; char affExpr; u8 op2; u32 flags;
union { char *zToken; int iValue; } u;
Expr *pLeft; Expr *pRight;
union { ExprList *pList; Select *pSelect; } x;
int nHeight; int iTable; ynVar iColumn; i16 iAgg;
union { int iJoin; int iOfst; } w;
AggInfo *pAggInfo;
union { Table *pTab; Window *pWin; int nReg;
        struct { int iAddr; int regReturn; } sub; } y;
```

**Major changes**:
- `dataType` -> `affExpr` (affinity)
- NEW: `op2`, many new EP_* flag bits, `u` union (zToken/iValue)
- `token`/`span` removed (replaced by `u.zToken` and `w.iOfst`)
- `pList`/`pSelect` moved into union `x`
- NEW: `w` union (iJoin/iOfst), `pAggInfo`, `y` union (pTab/pWin/sub)
- `iDb` removed (resolved through Table->pSchema)
- `flags` widened from u8 to u32
- `iColumn` changed from int to ynVar (i16)
- `iAgg` changed from int to i16
- **Risk**: CRITICAL -- every expression in every SQL statement is affected.

### B7. ExprList (`expr_list` -> SQLite `ExprList`)

DBSQL:
```c
int nExpr; int nAlloc;
struct expr_list_item { expr_t *pExpr; char *zName;
  u_int8_t sortOrder; u_int8_t isAgg; u_int8_t done; } *a;
```

SQLite 3.52.0:
```c
int nExpr; int nAlloc;
struct ExprList_item { Expr *pExpr; char *zEName;
  struct { u8 sortFlags; unsigned eEName:2; unsigned done:1;
    unsigned reusable:1; unsigned bSorterRef:1; unsigned bNulls:1;
    unsigned bUsed:1; unsigned bUsingTerm:1; unsigned bNoExpand:1; } fg;
  union { struct { u16 iOrderByCol; u16 iAlias; } x;
    int iConstExprReg; } u;
} a[FLEXARRAY];
```

**Changes**:
- `zName` -> `zEName` with `eEName` enum for purpose
- `sortOrder` -> `sortFlags` (KEYINFO_ORDER_* bitmask)
- `isAgg` removed (tracked in Expr flags)
- NEW: `fg` bitfield struct, `u` union, FLEXARRAY
- **Risk**: HIGH

### B8. IdList (`id_list` -> SQLite `IdList`)

DBSQL:
```c
int nId; int nAlloc;
struct id_list_item { char *zName; int idx; } *a;
```

SQLite 3.52.0:
```c
int nId;
struct IdList_item { char *zName; } a[FLEXARRAY];
```

**Changes**: `nAlloc` removed, `idx` removed from items,
array changed to FLEXARRAY
- **Risk**: LOW

### B9. SrcList (`src_list` -> SQLite `SrcList`)

DBSQL:
```c
u_int16_t nSrc; u_int16_t nAlloc;
struct src_list_item {
  char *zDatabase; char *zName; char *zAlias; table_t *pTab;
  select_t *pSelect; int jointype; int iCursor;
  expr_t *pOn; id_list_t *pUsing;
} a[1];
```

SQLite 3.52.0:
```c
int nSrc; u32 nAlloc;
SrcItem a[FLEXARRAY];
```

Where SrcItem is a much larger separate struct with:
- `zName`, `zAlias`, `pSTab`
- `fg` bitfield struct (19+ flags)
- `iCursor`, `colUsed` (Bitmask)
- `u1`/`u2`/`u3`/`u4` unions for various data
- `zDatabase` moved into `u4.zDatabase`
- `pSelect` moved into `u4.pSubq->pSelect`
- `pOn`/`pUsing` moved into `u3` union
- **Risk**: CRITICAL -- fundamental FROM clause representation changed.

### B10. Select (`select` -> SQLite `Select`)

DBSQL:
```c
expr_list_t *pEList; u_int8_t op; u_int8_t isDistinct;
src_list_t *pSrc; expr_t *pWhere; expr_list_t *pGroupBy;
expr_t *pHaving; expr_list_t *pOrderBy; select_t *pPrior;
int nLimit, nOffset; int iLimit, iOffset; char *zSelect;
```

SQLite 3.52.0:
```c
u8 op; LogEst nSelectRow; u32 selFlags;
int iLimit, iOffset; u32 selId;
ExprList *pEList; SrcList *pSrc; Expr *pWhere;
ExprList *pGroupBy; Expr *pHaving; ExprList *pOrderBy;
Select *pPrior; Select *pNext; Expr *pLimit;
With *pWith; Window *pWin; Window *pWinDefn;
```

**Changes**:
- `isDistinct` -> `selFlags` bitmask (SF_Distinct)
- `nLimit`/`nOffset` removed (now in pLimit expr)
- `zSelect` removed
- NEW: `nSelectRow`, `selFlags`, `selId`, `pNext`, `pLimit` (Expr*),
  `pWith`, `pWin`, `pWinDefn`
- **Risk**: HIGH

### B11. FuncDef (`func_def` -> SQLite `FuncDef`)

DBSQL:
```c
void (*xFunc)(dbsql_func_t*,int,const char**);
void (*xStep)(dbsql_func_t*,int,const char**);
void (*xFinalize)(dbsql_func_t*);
int nArg; int dataType; void *pUserData; func_def_t *pNext;
```

SQLite 3.52.0:
```c
i16 nArg; u32 funcFlags; void *pUserData; FuncDef *pNext;
void (*xSFunc)(sqlite3_context*,int,sqlite3_value**);
void (*xFinalize)(sqlite3_context*);
void (*xValue)(sqlite3_context*);
void (*xInverse)(sqlite3_context*,int,sqlite3_value**);
const char *zName;
union { FuncDef *pHash; FuncDestructor *pDestructor; } u;
```

**Changes**:
- `xFunc`/`xStep` -> single `xSFunc`
- Function signature changed (sqlite3_context/sqlite3_value)
- `dataType` removed (replaced by `funcFlags`)
- NEW: `funcFlags`, `xValue`, `xInverse` (window function support),
  `zName`, `u` union
- **Risk**: HIGH -- all function definitions must be updated.

### B12. AggInfo (`agg`/`agg_expr` -> SQLite `AggInfo`)

DBSQL has separate `struct agg`, `struct agg_expr`, `struct agg_elem`.

SQLite 3.52.0 has a single `struct AggInfo` with inline sub-structs
`AggInfo_col` and `AggInfo_func`.

**Changes**: Complete restructure.
- NEW: `directMode`, `useSortingIdx`, `nSortingColumn`, `sortingIdx`,
  `sortingIdxPTab`, `iFirstReg`, `pGroupBy`, `nAccumulator`,
  `nFunc`, `selId`
- `AggInfo_col`: `pTab`, `pCExpr`, `iTable`, `iColumn`, `iSorterColumn`
- `AggInfo_func`: `pFExpr`, `pFunc`, `iDistinct`, `iDistAddr`,
  `iOBTab`, `bOBPayload`, `bOBUnique`, `bUseSubtype`
- **Risk**: HIGH -- DBSQL's agg_* structs must be replaced entirely.

### B13. Parse (`parser` -> SQLite `Parse`)

DBSQL (`struct parser`):
```c
DBSQL *db; int rc; dbsql_callback xCallback; void *pArg;
char *zErrMsg; token_t sErrToken; token_t sFirstToken;
token_t sLastToken; const char *zTail; table_t *pNewTable;
vdbe_t *pVdbe; u_int8_t colNamesSet; u_int8_t explain;
u_int8_t initFlag; u_int8_t nameClash; u_int8_t useAgg;
u_int8_t iDb; u_int8_t useCallback; int newTnum; int nErr;
int nTab; int nMem; int nSet; int nAgg; int nVar;
agg_expr_t *aAgg; const char *zAuthContext;
trigger_t *pNewTrigger; trigger_stack_t *trigStack;
```

SQLite 3.52.0 (`struct Parse`):
Massively expanded -- ~80 fields organized into:
- Header section (zeroed on init)
- Non-recursive section (persists between recursions)
- Recursive section (reset per recursion)

**Major new fields**: `nQueryLoop`, `nested`, `nTempReg`, `isMultiWrite`,
`disableLookaside`, `prepFlags`, `withinRJSubrtn`, `mSubrtnSig`,
`eTriggerOp`, `eOrconf`, `disableTriggers`, `mayAbort`, `hasCompound`,
`bReturning`, `bHasExists`, `bHasWith`, `okConstFactor`, `checkSchema`,
`nRangeReg`, `iRangeReg`, `szOpAlloc`, `iSelfTab`, `nLabel`,
`nLabelAlloc`, `aLabel`, `pConstExpr`, `pIdxEpr`, `pIdxPartExpr`,
`writeMask`, `cookieMask`, `nMaxArg`, `nSelect`, `nTableLock`,
`aTableLock`, `pAinc`, `pToplevel`, `pTriggerTab`, `pTriggerPrg`,
`pCleanup`, `aTempReg[8]`, `pOuterParse`, `sNameToken`, `oldmask`,
`newmask`, `u1` union, `sLastToken`, `nVar`, `iPkSortOrder`, `explain`,
`eParseMode`, `nHeight`, `addrExplain`, `pVList`, `pReprepare`,
`zTail`, `pNewTable`, `pNewIndex`, `pNewTrigger`, `zAuthContext`,
`pWith`, `pRename`

**Removed**: `xCallback`, `pArg`, `sErrToken`, `sFirstToken`,
`initFlag`, `nameClash`, `useAgg`, `useCallback`, `newTnum`, `nSet`,
`nAgg`, `aAgg`, `trigStack`
- **Risk**: CRITICAL -- the parser struct is the core of all SQL processing.

### B14. Trigger (`trigger` -> SQLite `Trigger`)

DBSQL:
```c
char *name; char *table; u_int8_t iDb; u_int8_t iTabDb;
u_int8_t op; u_int8_t tr_tm; expr_t *pWhen; id_list_t *pColumns;
int foreach; token_t nameToken; trigger_step_t *step_list;
trigger_t *pNext;
```

SQLite 3.52.0:
```c
char *zName; char *table; u8 op; u8 tr_tm; u8 bReturning;
Expr *pWhen; IdList *pColumns; Schema *pSchema; Schema *pTabSchema;
TriggerStep *step_list; Trigger *pNext;
```

**Changes**:
- `name` -> `zName`
- `iDb`/`iTabDb` -> `pSchema`/`pTabSchema`
- `foreach` removed
- `nameToken` removed
- NEW: `bReturning`
- **Risk**: MEDIUM

### B15. TriggerStep (`trigger_step` -> SQLite `TriggerStep`)

DBSQL:
```c
int op; int orconf; trigger_t *pTrig; select_t *pSelect;
token_t target; expr_t *pWhere; expr_list_t *pExprList;
id_list_t *pIdList; trigger_step_t *pNext;
```

SQLite 3.52.0:
```c
u8 op; u8 orconf; Trigger *pTrig; Select *pSelect;
SrcList *pSrc; Expr *pWhere; ExprList *pExprList;
IdList *pIdList; Upsert *pUpsert; char *zSpan;
TriggerStep *pNext; TriggerStep *pLast;
```

**Changes**:
- `target` (Token) -> `pSrc` (SrcList*)
- NEW: `pUpsert`, `zSpan`, `pLast`
- `op`/`orconf` type changed from int to u8
- **Risk**: MEDIUM

### B16. Db (`__dbsql_db` -> SQLite `Db`)

DBSQL (`struct __dbsql_db`):
```c
char *zName; sm_t *pBt; u_int32_t schema_sig;
hash_t tblHash; hash_t idxHash; hash_t trigHash; hash_t aFKey;
int inTrans; DBSQL *dbp; int flags;
```

SQLite 3.52.0 (`struct Db`):
```c
char *zDbSName; Btree *pBt; u8 safety_level; u8 bSyncSet;
Schema *pSchema;
```

**Major change**: Schema info (hash tables, flags) moved to separate
`Schema` struct. DBSQL's `dbp` pointer and `inTrans` not present.
- `zName` -> `zDbSName`
- All hash tables moved to Schema
- `schema_sig` -> `Schema.schema_cookie`
- `flags` -> `Schema.schemaFlags`
- NEW: `safety_level`, `bSyncSet`
- **Risk**: HIGH -- requires Schema extraction.

### B17. AuthContext (`auth_context` -> SQLite `AuthContext`)

No changes. Both have `zAuthContext` and `pParse`.
- **Risk**: NONE

---

## C. Removed / Renamed Structures (in DBSQL but not in SQLite 3.52.0)

### C1. `struct agg` / `struct agg_expr` / `struct agg_elem`
- **Status**: Replaced by unified `AggInfo` struct.
- DBSQL's separate aggregate structures must be removed.

### C2. `struct keylist`
- **Status**: Not present in SQLite 3.52.0. Was used for key storage in
  old VDBE. Replaced by ephemeral tables (OP_OpenEphemeral).

### C3. `struct cursor`
- **Status**: Not in sqliteInt.h (cursor handling moved to VdbeCursor in
  vdbeInt.h).

### C4. `struct trigger_stack`
- **Status**: Replaced by `TriggerPrg` approach. SQLite no longer uses
  a stack-based trigger execution model.

### C5. `struct where_level`
- **Status**: Still exists in SQLite but defined in whereInt.h, not
  sqliteInt.h. Now internal to the WHERE clause optimizer.

### C6. `struct ref_normalizer_ctx`
- **Status**: Replaced by `DbFixer` struct.

---

## D. Special Considerations

### D1. BerkeleyDB Type Dependencies
The following DBSQL structures reference BerkeleyDB types and must
be preserved or adapted:
- `sm_t` -- uses `DB*`, `DB_SEQUENCE*`, `DB_TXN*`, `DBSQL*`
- `sm_cursor_t` -- uses `DB*`, `DBC*`, `DB_TXN*`
- `struct __dbsql_db` -- uses `sm_t*` (replaces SQLite's `Btree*`)
- Any reference to `Btree*` in SQLite must map to `sm_t*` in DBSQL.

### D2. Storage Layer (SM) Compatibility
- SQLite's `Btree*`/`BtCursor*` -> DBSQL's `sm_t*`/`sm_cursor_t*`
- SQLite's `Pgno` type (page number) may not apply; DBSQL uses BDB
  record numbers differently.
- `Index.tnum` and `Table.tnum` semantics differ.
- SQLite's `Schema` stores `cache_size` -- not applicable to BDB.

### D3. Window Function Support
- Requires: `Window`, `Cte`, `CteUse`, `With` structs
- Affects: `Select` (new pWin/pWinDefn fields), `Expr` (y.pWin),
  `FuncDef` (xValue/xInverse), `Parse` (various)
- This is a major feature addition.

### D4. CTE (Common Table Expression) Support
- Requires: `Cte`, `CteUse`, `With` structs
- Affects: `Select` (pWith field), `SrcItem` (isCte flag, pCteUse),
  `Parse` (pWith field)

### D5. RETURNING Clause Support
- Requires: `Returning` struct
- Affects: `Parse` (bReturning flag, u1.d.pReturning), `Trigger`
  (bReturning flag)

### D6. UPSERT Support
- Requires: `Upsert` struct
- Affects: `TriggerStep` (pUpsert field)

### D7. Type System Changes
- SQLite now uses `affinity` (SQLITE_AFF_*) throughout
- DBSQL's old `DBSQL_SO_TEXT`/`DBSQL_SO_NUM` system is replaced
- Column types now use `eCType` enum
- The DBSQL_SO_* defines should be replaced with SQLITE_AFF_* equivalents

### D8. FLEXARRAY Usage
- SQLite 3.52.0 uses FLEXARRAY (C99 flexible array members) in:
  ExprList, IdList, SrcList, FKey, KeyInfo, With
- DBSQL currently uses `[1]` or `*` for these arrays.
- Recommendation: Define `FLEXARRAY` macro and adopt.

---

## E. Summary and Recommendations

### Counts
- **New structures to add**: 35
- **Structures to modify**: 16
- **Structures to remove/replace**: 6

### Risk Assessment

| Risk Level | Structures |
|-----------|------------|
| CRITICAL  | Expr, SrcList/SrcItem, Parse |
| HIGH      | Column, Table, Index, AggInfo, FuncDef, Db/Schema, KeyInfo, NameContext, Window |
| MEDIUM    | Select, FKey, TriggerStep, Trigger, Walker, SelectDest, Upsert, Cte/With, TriggerPrg, Returning, CollSeq, UnpackedRecord, Subquery, ExprList |
| LOW       | Token, IdList, AuthContext, Savepoint, Module, VTable, AutoincInfo, Lookaside, BusyHandler, IndexedExpr, ParseCleanup, OnOrUsing, FuncDestructor, FuncDefHash, IndexSample, RCStr, DbClientData, TreeView, FpDecode, PrintfArguments |

### Recommended Order of Implementation
1. Define helper types first: `Bitmask`, `LogEst`, `Pgno`, `ynVar`, `FLEXARRAY`
2. Add simple new structs: `Savepoint`, `BusyHandler`, `CollSeq`, `OnOrUsing`
3. Add `Schema` struct and refactor `__dbsql_db` (Db)
4. Update `Column`, then `Table` (depends on Column)
5. Update `Index` (depends on Table, CollSeq)
6. Update `Token`, then `Expr` (depends on Token changes)
7. Update `ExprList`, `IdList` (depends on Expr)
8. Add `SrcItem`/`Subquery`, update `SrcList` (depends on Expr, Table)
9. Add `NameContext`, `Walker`, `DbFixer` (depends on SrcList)
10. Update `FuncDef`, add `FuncDestructor`, `FuncDefHash`
11. Replace `agg*` with `AggInfo`
12. Update `Select`, add `SelectDest`, `With`/`Cte`/`CteUse`
13. Add `Window` (depends on Select, FuncDef, Expr)
14. Update `FKey`
15. Update `Trigger`, `TriggerStep`, add `TriggerPrg`, `Returning`
16. Add `Upsert`
17. Add `KeyInfo`, `UnpackedRecord`
18. Update `Parse` (depends on nearly everything above)
19. Add remaining utility structs

### Dependencies Between Structures

```
Schema <-- Db, Table, Index, Trigger
Column <-- Table
Table  <-- Index, FKey, Select, SrcItem, Trigger, AggInfo, Parse, Expr
Index  <-- Table, Schema, KeyInfo
Expr   <-- ExprList, Select, Trigger, TriggerStep, Window, Upsert,
           NameContext, Walker, IndexedExpr
ExprList <-- Select, Table, Upsert, Cte, ExprList_item
SrcList/SrcItem <-- Select, NameContext, Parse, Walker
Select <-- Table, SrcList, Expr, ExprList, With, Window
FuncDef <-- AggInfo, Window
Parse  <-- nearly all structures
CollSeq <-- KeyInfo, sqlite3 (DBSQL main struct)
KeyInfo <-- UnpackedRecord, Index comparisons
Window <-- Expr, Select, FuncDef
```
