# Transpilation Tool Examples

This document shows real examples of SQLite to DBSQL transpilation using the updated tools.

## Example 1: VdbeOp Structure Usage

### Before (SQLite 3.52.0):
```c
VdbeOp *pOp;
u8 flags;

pOp = sqliteVdbeGetOp(v, addr);
pOp->p5 = 0x01;
pOp->p4type = P4_INT32;
pOp->p4.i = 42;
pOp->comment = "Test opcode";
```

### After (DBSQL):
```c
vdbe_op_t *pOp;
u_int8_t flags;

pOp = __vdbe_get_op(v, addr);
pOp->p5 = 0x01;
pOp->p4type = P4_INT32;
pOp->p4.i = 42;
pOp->zComment = "Test opcode";
```

### Changes:
- `VdbeOp` → `vdbe_op_t` (type name)
- `u8` → `u_int8_t` (integer type)
- `sqliteVdbeGetOp` → `__vdbe_get_op` (function name)
- `comment` → `zComment` (special field rename)

## Example 2: Mem Structure Usage

### Before (SQLite 3.52.0):
```c
Mem *pMem;

pMem->szMalloc = 1024;
pMem->uTemp = 0;
pMem->eSubtype = 0;
pMem->u.i = 42;
pMem->u.r = 3.14;
```

### After (DBSQL):
```c
mem_t *pMem;

pMem->sz_malloc = 1024;
pMem->u_temp = 0;
pMem->e_subtype = 0;
pMem->u.i = 42;
pMem->u.r = 3.14;
```

### Changes:
- `Mem` → `mem_t` (type name)
- `szMalloc` → `sz_malloc` (CamelCase to snake_case)
- `uTemp` → `u_temp` (CamelCase to snake_case)
- `eSubtype` → `e_subtype` (CamelCase to snake_case)
- Union fields (u.i, u.r) preserved

## Example 3: Parser Structure Usage

### Before (SQLite 3.52.0):
```c
Parse *pParse;
u32 nReg;

pParse->nRangeReg = 10;
pParse->iRangeReg = pParse->nMem + 1;
pParse->nLabelAlloc = 100;
pParse->isMultiWrite = 1;
pParse->eTriggerOp = TK_UPDATE;
```

### After (DBSQL):
```c
parser_t *pParse;
u_int32_t nReg;

pParse->n_range_reg = 10;
pParse->i_range_reg = pParse->n_mem + 1;
pParse->n_label_alloc = 100;
pParse->is_multi_write = 1;
pParse->e_trigger_op = TK_UPDATE;
```

### Changes:
- `Parse` → `parser_t` (type name override)
- `u32` → `u_int32_t` (integer type)
- All CamelCase field names → snake_case
- `nMem` → `n_mem` (in nested access)

## Example 4: Expr Structure Usage

### Before (SQLite 3.52.0):
```c
Expr *pExpr;

if (pExpr->u.zToken != NULL) {
    pExpr->x.pList = sqliteExprListAppend(pParse, NULL, pExpr);
    pExpr->y.pTab = sqliteFindTable(pDb, zTab);
    pExpr->w.iJoin = iTable;
    pExpr->w.iOfst = 0;
}
```

### After (DBSQL):
```c
expr_t *pExpr;

if (pExpr->u.z_token != NULL) {
    pExpr->x.p_list = __expr_list_append(pParse, NULL, pExpr);
    pExpr->y.p_tab = __find_table(pDb, zTab);
    pExpr->w.i_join = iTable;
    pExpr->w.i_ofst = 0;
}
```

### Changes:
- `Expr` → `expr_t` (type name)
- `zToken` → `z_token` (CamelCase to snake_case)
- `pList` → `p_list` (CamelCase to snake_case)
- `pTab` → `p_tab` (CamelCase to snake_case)
- `iJoin` → `i_join` (CamelCase to snake_case)
- `iOfst` → `i_ofst` (CamelCase to snake_case)
- Function names converted to snake_case with __ prefix

## Example 5: Complete Function

### Before (SQLite 3.52.0):
```c
/*
 * sqliteVdbeAllocRegisters --
 *   Allocate a range of temporary registers.
 */
int
sqliteVdbeAllocRegisters(Parse *pParse, int nReg)
{
    Vdbe *v;
    u32 iBase;

    v = sqliteGetVdbe(pParse);
    if (v == NULL)
        return SQLITE_ERROR;

    pParse->nRangeReg = nReg;
    pParse->iRangeReg = pParse->nMem + 1;
    pParse->nMem += nReg;

    if (pParse->isMultiWrite) {
        /* Mark as unsafe for triggers */
        v->pFrame->flags |= SQLITE_TRIGGER_UNSAFE;
    }

    return pParse->iRangeReg;
}
```

### After (DBSQL):
```c
/*
 * __vdbe_alloc_registers --
 *   Allocate a range of temporary registers.
 */
int
__vdbe_alloc_registers(parser_t *pParse, int nReg)
{
    vdbe_t *v;
    u_int32_t iBase;

    v = __get_vdbe(pParse);
    if (v == NULL)
        return DBSQL_ERROR;

    pParse->n_range_reg = nReg;
    pParse->i_range_reg = pParse->n_mem + 1;
    pParse->n_mem += nReg;

    if (pParse->is_multi_write) {
        /* Mark as unsafe for triggers */
        v->p_frame->flags |= DBSQL_TRIGGER_UNSAFE;
    }

    return pParse->i_range_reg;
}
```

### Changes:
- Function name: `sqliteVdbeAllocRegisters` → `__vdbe_alloc_registers`
- Type names: `Parse` → `parser_t`, `Vdbe` → `vdbe_t`
- Integer type: `u32` → `u_int32_t`
- Field names: All CamelCase → snake_case
- Constants: `SQLITE_ERROR` → `DBSQL_ERROR`
- Nested field access: `pFrame` → `p_frame`

## Validation Examples

### Example 6: P5 Flag Validation

The validator checks P5 values are within u16 range:

```c
pOp->p5 = 0x10000;  // ❌ FAIL: P5 value out of range (u16)
pOp->p5 = 0xFFFF;   // ✅ PASS: Maximum valid u16 value
pOp->p5 = 0x01;     // ✅ PASS: Normal flag value
```

### Example 7: MEM Flag Validation

The validator checks for invalid flag combinations:

```c
// ❌ FAIL: Invalid MEM flag combination (Int+Real)
pMem->flags = MEM_Int | MEM_Real;

// ❌ FAIL: Invalid MEM flag combination (Str+Blob)
pMem->flags = MEM_Str | MEM_Blob;

// ✅ PASS: Valid flag combinations
pMem->flags = MEM_Int;
pMem->flags = MEM_Str | MEM_Dyn;
pMem->flags = MEM_Blob | MEM_Static;
```

### Example 8: Register Addressing Validation

The validator detects suspicious 1-indexed patterns:

```c
// ❌ FAIL: Suspicious register pattern
int iReg = 1;  // first register

// ✅ PASS: Correct 0-indexed addressing
int iReg = 0;  // first register
```

## Running the Tools

### Transpile a file:
```bash
cd dist/transpile
./sqlite_to_dbsql.py -i sqlite_code.c -o dbsql_code.c
```

### Verify transpiled code:
```bash
./verify_transpilation.py dbsql_code.c
```

### Run self-tests:
```bash
./sqlite_to_dbsql.py --test
```

## Field Name Conversion Rules

| SQLite Pattern | DBSQL Pattern | Example |
|----------------|---------------|---------|
| camelCase | camel_case | szMalloc → sz_malloc |
| CamelCase | camel_case | ExprList → expr_list |
| nPrefix | n_prefix | nRangeReg → n_range_reg |
| iPrefix | i_prefix | iRangeReg → i_range_reg |
| ePrefix | e_prefix | eSubtype → e_subtype |
| pPrefix | p_prefix | pList → p_list |
| Special | Manual | comment → zComment |

## Type Name Conversion Rules

| SQLite Type | DBSQL Type | Override |
|-------------|------------|----------|
| Parse | parser_t | Yes |
| Expr | expr_t | No |
| Vdbe | vdbe_t | No |
| Mem | mem_t | No |
| VdbeOp | vdbe_op_t | No |
| FKey | foreign_key_t | Yes |

## Function Name Conversion Rules

| SQLite Pattern | DBSQL Pattern | Example |
|----------------|---------------|---------|
| sqliteCamelCase | __camel_case | sqliteVdbeExec → __vdbe_exec |
| sqlite3CamelCase | __camel_case | sqlite3GetVdbe → __get_vdbe |
| dbsql_* | dbsql_* | (public API, unchanged) |
