# Task #21: Convert cg_select.c to Register-Based Code Generation

## Conversion Status: IN PROGRESS

This document tracks the conversion of `src/cg_select.c` from stack-based to register-based VDBE code generation, aligning with SQLite 3.52.0's Phase 2 architecture.

## Key Changes Made

### 1. __select_inner_loop() - Core Result Generation

**Location**: Lines 504-709

**Changes**:
- Added register allocation variables `regResult` and `nResultCol` at function start
- Replaced stack-based column pulling with register-targeted operations:
  - `OP_Column` now stores to specific registers via `OP_Copy`
  - `__expr_code()` calls replaced with `__expr_code_target(parser, expr, regResult + i)`
- Updated all result destinations to use register ranges:
  - `SRT_Callback`: Now uses `OP_ResultRow regResult, nResultCol` instead of `OP_Callback num_cols, 0`
  - `SRT_Union`: Updated to use `nResultCol`
  - `SRT_Table/SRT_TempTable`: Updated to use `nResultCol`
  - `SRT_Except`: Updated to use `nResultCol`
  - `SRT_Set`: Uses register-based `OP_NotNull regResult` instead of stack-based
  - `SRT_Mem`: Uses `OP_Copy regResult, param` instead of `OP_MemStore`
  - `SRT_Subroutine`: Results already in registers
  - `SRT_Discard`: No longer needs `OP_Pop` (registers are reused)

### 2. __compute_limit_registers() - LIMIT/OFFSET Handling

**Location**: Lines 1452-1485

**Changes**:
- Converted from `OP_MemStore` to register-based storage
- OLD: `__vdbe_add_op(v, OP_Integer, value, 0); __vdbe_add_op(v, OP_MemStore, mem, 1);`
- NEW: `__vdbe_add_op(v, OP_Integer, value, reg);`
- Registers are allocated via `reg = parser->nMem++`
- LIMIT/OFFSET values stored directly in registers for countdown

### 3. __generate_sort_tail() - Sorted Results

**Location**: Lines 708-761

**Changes**:
- Added comment noting `OP_SortCallback` is Phase 2 register-based
- No functional change needed as OP_SortCallback internally uses registers

### 4. Aggregate Processing - GROUP BY

**Location**: Lines 2779-2797

**Changes**:
- Added register allocation for GROUP BY expressions:
  ```c
  int regGroupBy = parser->nMem + 1;
  parser->nMem += groupby_clause->nExpr;
  ```
- Replaced `__expr_code()` with `__expr_code_target()` for GROUP BY expressions
- Each GROUP BY expression stores to consecutive registers

### 5. Aggregate Functions - __select() Main Function

**Location**: Lines 2800-2807

**Changes**:
- Converted `OP_Integer i, 0` (stack push) to register-based:
  ```c
  int regAggIdx = parser->nMem + 1;
  parser->nMem++;
  __vdbe_add_op(v, OP_Integer, i, regAggIdx);
  ```

### 6. Min/Max Optimization - Database Access

**Location**: Lines 2266-2282

**Changes**:
- Converted database index integers from stack-based to register-based:
  - Table database index: `__vdbe_add_op(v, OP_Integer, table->iDb, regDbIdx);`
  - Index database index: `__vdbe_add_op(v, OP_Integer, index->iDb, regIdxDb);`
- Allocate dedicated registers for these values

## Remaining Work

### High Priority
1. **Test compilation** - Need to fix build system issues to verify syntax
2. **JOIN processing** - Review JOIN code generation for register usage
3. **Subquery generation** - Ensure nested SELECTs properly allocate registers
4. **Compound SELECT** - Verify UNION/INTERSECT/EXCEPT use registers
5. **ORDER BY optimization** - Check if sorter references use registers

### Medium Priority
1. **Integration with AggInfo** - Task #5's new aggregate structure not yet integrated
2. **DISTINCT processing** - Verify DISTINCT logic uses register-based operations
3. **Window functions** - If present, convert to register-based
4. **CTE (WITH clause)** - Verify Common Table Expression handling

### Low Priority
1. **Error handling** - Ensure error paths don't leak registers
2. **Memory cleanup** - Verify register allocation doesn't overflow
3. **Performance testing** - Compare against stack-based version

## Pattern Reference

### Stack-Based (OLD)
```c
__vdbe_add_op(v, OP_Integer, value, 0);  /* pushes to stack */
__expr_code(parser, expr);                /* pushes result */
__vdbe_add_op(v, OP_Callback, n, 0);     /* pops n items */
__vdbe_add_op(v, OP_Pop, n, 0);          /* discard n items */
```

### Register-Based (NEW)
```c
int reg = parser->nMem + 1;
parser->nMem++;
__vdbe_add_op(v, OP_Integer, value, reg);     /* stores to register */
__expr_code_target(parser, expr, reg);        /* stores to register */
__vdbe_add_op(v, OP_ResultRow, regStart, n); /* outputs n registers */
/* no OP_Pop needed - registers reused */
```

## Key Opcodes Changed

| Old Opcode | New Opcode | Notes |
|------------|------------|-------|
| OP_Callback | OP_ResultRow | Core result output |
| OP_Integer X, 0 | OP_Integer X, R | Direct to register |
| OP_MemStore | OP_Copy | Register-to-register |
| OP_Pop | (removed) | Registers auto-reused |

## SQLite 3.52.0 Reference

The conversion follows patterns from `_/sqlite/src/select.c`, specifically:
- `selectInnerLoop()` function (lines 1139-1515)
- Use of `pDest->iSdst` and `pDest->nSdst` for result registers
- `SelectDest` structure for destination handling
- `OP_ResultRow` for result output (line 1451)

## Testing Notes

Once compilation succeeds:
1. Run test suite: `make test`
2. Verify SELECT queries return correct results
3. Check LIMIT/OFFSET behavior
4. Test aggregate functions (COUNT, SUM, AVG, MIN, MAX)
5. Verify GROUP BY and HAVING clauses
6. Test ORDER BY with and without LIMIT
7. Verify compound SELECT (UNION, INTERSECT, EXCEPT)
8. Test nested subqueries

## Files Modified

- `src/cg_select.c` - Main conversion file
- `src/cg_select.c.backup` - Backup of original file

## References

- Task #21 description in project documentation
- SQLite 3.52.0 source: `_/sqlite/src/select.c`
- VDBE architecture doc: `vdbe_analysis_report.md`
- Type system modernization: Phase 2 notes
