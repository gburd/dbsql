# Task #21: Detailed Changes to cg_select.c

## File Statistics
- Original: 2,911 lines
- Modified: 2,951 lines
- Net change: +40 lines (added register allocation and comments)

## Function-by-Function Changes

### 1. `__select_inner_loop()` (Lines 504-709)

#### Variable Declarations Added
```c
int regResult;      /* Start of memory holding current results */
int nResultCol;     /* Number of result columns */
```

#### Register Allocation Logic
```c
if (num_cols > 0) {
    nResultCol = num_cols;
} else {
    nResultCol = elist->nExpr;
}

regResult = parser->nMem + 1;
parser->nMem += nResultCol;
```

#### Column Extraction Changes
**OLD (Stack-based)**:
```c
if (num_cols > 0) {
    for (i = 0; i < num_cols; i++) {
        __vdbe_add_op(v, OP_Column, src_table, i);  /* Pushes to stack */
    }
} else {
    for (i = 0; i < elist->nExpr; i++) {
        __expr_code(parser, elist->a[i].pExpr);     /* Pushes to stack */
    }
}
```

**NEW (Register-based)**:
```c
if (num_cols > 0) {
    for (i = 0; i < num_cols; i++) {
        __vdbe_add_op(v, OP_Column, src_table, i);
        __vdbe_add_op(v, OP_Copy, -1, regResult + i);  /* Copy to register */
        __vdbe_add_op(v, OP_Pop, 1, 0);                /* Clean stack */
    }
} else {
    for (i = 0; i < elist->nExpr; i++) {
        __expr_code_target(parser, elist->a[i].pExpr, regResult + i);
    }
}
```

#### Result Destination Changes

##### SRT_Callback
**OLD**:
```c
DBSQL_ASSERT(dest == SRT_Callback);
__vdbe_add_op(v, OP_Callback, num_cols, 0);
```

**NEW**:
```c
DBSQL_ASSERT(dest == SRT_Callback);
/* Phase 2: Use OP_ResultRow with register range */
__vdbe_add_op(v, OP_ResultRow, regResult, nResultCol);
```

##### SRT_Union
**OLD**: `__vdbe_add_op(v, OP_MakeRecord, num_cols, ...);`
**NEW**: `__vdbe_add_op(v, OP_MakeRecord, nResultCol, ...);`

##### SRT_Table/SRT_TempTable
**OLD**: `__vdbe_add_op(v, OP_MakeRecord, num_cols, 0);`
**NEW**: `__vdbe_add_op(v, OP_MakeRecord, nResultCol, 0);`

##### SRT_Except
**OLD**: `__vdbe_add_op(v, OP_MakeRecord, num_cols, ...);`
**NEW**: `__vdbe_add_op(v, OP_MakeRecord, nResultCol, ...);`

##### SRT_Set
**OLD**:
```c
DBSQL_ASSERT(num_cols == 1);
__vdbe_add_op(v, OP_NotNull, -1, (addr1 + 3));  /* Stack reference */
__vdbe_add_op(v, OP_Pop, 1, 0);
```

**NEW**:
```c
DBSQL_ASSERT(nResultCol == 1);
__vdbe_add_op(v, OP_NotNull, regResult, (addr1 + 2));  /* Register reference */
/* No Pop needed */
```

##### SRT_Mem
**OLD**:
```c
DBSQL_ASSERT(num_cols == 1);
__vdbe_add_op(v, OP_MemStore, param, 1);
```

**NEW**:
```c
DBSQL_ASSERT(nResultCol == 1);
/* Store result from register into memory cell */
__vdbe_add_op(v, OP_Copy, regResult, param);
```

##### SRT_Subroutine
**OLD**: `__vdbe_add_op(v, OP_MakeRecord, num_cols, 0);`
**NEW**: `__vdbe_add_op(v, OP_MakeRecord, nResultCol, 0);` + comment

##### SRT_Discard
**OLD**:
```c
DBSQL_ASSERT(dest == SRT_Discard);
__vdbe_add_op(v, OP_Pop, num_cols, 0);
```

**NEW**:
```c
DBSQL_ASSERT(dest == SRT_Discard);
/* No need to pop in register-based model - registers are reused */
```

### 2. `__compute_limit_registers()` (Lines 1452-1485)

#### Complete Rewrite

**OLD (Stack + Memory)**:
```c
if (select->nLimit >= 0) {
    mem = parser->nMem++;
    v = __parser_get_vdbe(parser);
    if (v == 0) return;
    __vdbe_add_op(v, OP_Integer, (-select->nLimit), 0);   /* Push */
    __vdbe_add_op(v, OP_MemStore, mem, 1);                /* Store */
    select->iLimit = mem;
}
```

**NEW (Direct Register)**:
```c
if (select->nLimit >= 0) {
    reg = parser->nMem++;
    v = __parser_get_vdbe(parser);
    if (v == 0) return;
    /* Store negative limit for countdown in register */
    __vdbe_add_op(v, OP_Integer, (-select->nLimit), reg);  /* Direct */
    select->iLimit = reg;
}
```

Added comment:
```c
/*
 * Phase 2: LIMIT/OFFSET now use registers with OP_Integer
 * to store countdown values, rather than OP_MemStore.
 */
```

Same pattern for OFFSET.

### 3. `__generate_sort_tail()` (Lines 708-761)

#### Minor Changes

Added comment to SRT_Callback case:
```c
case SRT_Callback:
    /* Phase 2: Use OP_SortCallback which internally uses registers */
    __vdbe_add_op(v, OP_SortCallback, num_cols, 0);
    break;
```

**Note**: Other cases retain stack-based operations due to sorter's complex internal state. This is intentional pending sorter modernization.

### 4. Aggregate Processing - GROUP BY (Lines 2779-2799)

#### GROUP BY Register Allocation

**OLD**:
```c
for (i = 0; i < groupby_clause->nExpr; i++) {
    __expr_code(parser, groupby_clause->a[i].pExpr);  /* Stack */
}
```

**NEW**:
```c
/* Allocate registers for GROUP BY expressions */
int regGroupBy = parser->nMem + 1;
parser->nMem += groupby_clause->nExpr;

for (i = 0; i < groupby_clause->nExpr; i++) {
    __expr_code_target(parser,
                       groupby_clause->a[i].pExpr,
                       regGroupBy + i);
}
```

### 5. Aggregate Functions (Lines 2800-2815)

#### Aggregate Index Register

**OLD**:
```c
__vdbe_add_op(v, OP_Integer, i, 0);  /* Push index to stack */
__vdbe_add_op(v, OP_AggFunc, 0, (e->pList ? e->pList->nExpr : 0));
```

**NEW**:
```c
/* Allocate register for aggregate index */
int regAggIdx = parser->nMem + 1;
parser->nMem++;
__vdbe_add_op(v, OP_Integer, i, regAggIdx);  /* Store in register */
__vdbe_add_op(v, OP_AggFunc, 0, (e->pList ? e->pList->nExpr : 0));
```

### 6. Min/Max Optimization (Lines 2266-2282)

#### Database Index Registers

**OLD (Table)**:
```c
__vdbe_add_op(v, OP_Integer, table->iDb, 0);  /* Push */
__vdbe_add_op(v, OP_OpenRead, base, table->tnum);
```

**NEW (Table)**:
```c
/* Allocate register for database index */
int regDbIdx = parser->nMem + 1;
parser->nMem++;
__vdbe_add_op(v, OP_Integer, table->iDb, regDbIdx);  /* Store */
__vdbe_add_op(v, OP_OpenRead, base, table->tnum);
```

**OLD (Index)**:
```c
__vdbe_add_op(v, OP_Integer, index->iDb, 0);  /* Push */
__vdbe_add_op(v, OP_OpenRead, (base + 1), index->tnum);
```

**NEW (Index)**:
```c
/* Allocate register for index database number */
int regIdxDb = parser->nMem + 1;
parser->nMem++;
__vdbe_add_op(v, OP_Integer, index->iDb, regIdxDb);  /* Store */
__vdbe_add_op(v, OP_OpenRead, (base + 1), index->tnum);
```

## Patterns Consistently Applied

### Register Allocation Pattern
```c
int reg<Purpose> = parser->nMem + 1;
parser->nMem += count;
```

Examples:
- `regResult` - Result columns
- `regGroupBy` - GROUP BY expressions
- `regAggIdx` - Aggregate function index
- `regDbIdx` - Database index
- `regIdxDb` - Index database number

### Expression Code Pattern
**Stack-based**: `__expr_code(parser, expr);`
**Register-based**: `__expr_code_target(parser, expr, target_reg);`

### Integer Constant Pattern
**Stack-based**: `__vdbe_add_op(v, OP_Integer, value, 0);`
**Register-based**: `__vdbe_add_op(v, OP_Integer, value, reg);`

### Result Output Pattern
**Stack-based**: `__vdbe_add_op(v, OP_Callback, n, 0);`
**Register-based**: `__vdbe_add_op(v, OP_ResultRow, reg, n);`

## Unchanged Areas (Intentional)

### 1. Aggregate Expression Evaluation
Lines 2796, 2808-2809: Still use `__expr_code()` in aggregate context
**Reason**: Awaiting Task #5 (AggInfo structure) integration

### 2. Sorter Operations
Lines 736-774: Retain stack-based operations in `__generate_sort_tail()`
**Reason**: Sorter has complex internal state management requiring coordinated update

### 3. ORDER BY in `__push_onto_sorter`
Line 438: Uses `__expr_code()` for ORDER BY expressions
**Reason**: Sorter interface not yet updated to register-based

## Verification Checklist

- [x] All `OP_Callback` converted to `OP_ResultRow`
- [x] All direct `OP_Integer X, 0` converted to `OP_Integer X, reg`
- [x] Register allocation follows consistent pattern
- [x] `parser->nMem` updated for all allocations
- [x] Comments added explaining Phase 2 changes
- [ ] Compilation tested (blocked by build system)
- [ ] Runtime tested (blocked by build system)
- [ ] Performance benchmarked (pending compilation)

## Dependencies

### Upstream (Required)
- None - self-contained changes

### Downstream (Enables)
- Task #5 aggregate modernization integration
- Full Phase 2 VDBE completion

### Related
- Task #7 VDBE architecture analysis (reference)
- Task #10 SQL tokenizer (already complete)
- Task #12 Built-in functions (already complete)

## Risk Assessment

### Low Risk
✅ Core SELECT processing - well-tested pattern from SQLite 3.52.0
✅ LIMIT/OFFSET - simple register substitution
✅ Basic expression evaluation - straightforward conversion

### Medium Risk
⚠️ Aggregate processing - partial conversion pending AggInfo
⚠️ Compound SELECT - multi-query register coordination
⚠️ Subqueries - register scope management

### High Risk
⚠️ Sorter operations - complex internal state
⚠️ JOIN processing - register allocation across tables

### Mitigation
- Comprehensive test suite before production use
- Gradual integration with Task #5
- Performance baseline comparison

## Testing Strategy

### Phase 1: Basic Functionality
1. Simple SELECT: `SELECT * FROM table`
2. WHERE clause: `SELECT * FROM table WHERE id = 1`
3. Column selection: `SELECT col1, col2 FROM table`
4. LIMIT: `SELECT * FROM table LIMIT 10`
5. OFFSET: `SELECT * FROM table LIMIT 10 OFFSET 5`

### Phase 2: Advanced Features
6. ORDER BY: `SELECT * FROM table ORDER BY col`
7. DISTINCT: `SELECT DISTINCT col FROM table`
8. Aggregates: `SELECT COUNT(*) FROM table`
9. GROUP BY: `SELECT col, COUNT(*) FROM table GROUP BY col`
10. HAVING: `SELECT col, COUNT(*) FROM table GROUP BY col HAVING COUNT(*) > 1`

### Phase 3: Complex Queries
11. Subqueries: `SELECT * FROM (SELECT * FROM table)`
12. JOIN: `SELECT * FROM t1 JOIN t2 ON t1.id = t2.id`
13. UNION: `SELECT * FROM t1 UNION SELECT * FROM t2`
14. Nested aggregates: `SELECT MAX(COUNT(*)) FROM table GROUP BY col`

### Phase 4: Edge Cases
15. Empty result sets
16. NULL handling
17. Large result sets (memory management)
18. Complex expressions in SELECT list

## Performance Expectations

### Expected Improvements
- Reduced stack manipulation overhead
- Better register locality
- More efficient memory access patterns

### Expected Neutral
- Simple queries (overhead dominated by I/O)
- Cached result sets

### Potential Regressions
- Sorter operations (mixed register/stack model)
- Very deep subqueries (register allocation overhead)

### Measurement Plan
1. Baseline: Measure original stack-based version
2. Measure: New register-based version
3. Compare: Query execution time, memory usage
4. Profile: Identify hotspots if regressions found

## Documentation Updates Needed

### Code Comments
- [x] Added Phase 2 markers in code
- [x] Explained register allocation
- [x] Noted intentional incompletions

### Developer Documentation
- [x] Created task21_conversion_notes.md
- [x] Created TASK21_COMPLETE_REPORT.md
- [x] Created TASK21_SUMMARY.md
- [x] Created TASK21_CHANGES_DETAILED.md (this file)

### User-Facing Documentation
- [ ] Update SQL reference if behavioral changes
- [ ] Update performance tuning guide
- [ ] Note any incompatibilities

## Conclusion

Task #21 represents a fundamental architectural shift from stack-based to register-based code generation for SELECT statements. The conversion is comprehensive for core functionality, with intentional deferrals for complex subsystems (aggregates, sorter) pending coordinated updates.

**Next owner**: Should fix build system, test compilation, and begin integration testing.

---

**Conversion Date**: 2026-03-16
**Line Count**: Original 2,911 → Modified 2,951 (+40 lines)
**Functions Modified**: 6 major functions
**Opcodes Changed**: OP_Callback, OP_Integer, OP_MemStore, OP_Pop
**New Opcodes Used**: OP_ResultRow, OP_Copy (in new contexts)
**Register Allocations Added**: 6 distinct register allocation sites
