# Task #21: Convert cg_select.c to Register-Based Code Generation

## Summary

Converted `src/cg_select.c` (~2911 lines) from stack-based to register-based VDBE code generation, aligning with SQLite 3.52.0's Phase 2 architecture.

## Key Changes

### Core SELECT Processing (`__select_inner_loop`)
- **Before**: Results pushed to stack via `OP_Callback`
- **After**: Results stored in registers via `OP_ResultRow`
- Added register allocation: `regResult = parser->nMem + 1; parser->nMem += nResultCol;`
- Converted all result destinations (SRT_Callback, SRT_Table, SRT_Set, SRT_Mem, etc.)

### LIMIT/OFFSET (`__compute_limit_registers`)
- **Before**: `OP_Integer value, 0` (stack) + `OP_MemStore`
- **After**: `OP_Integer value, reg` (direct register storage)
- Registers used for countdown instead of memory cells

### Expression Evaluation
- **Before**: `__expr_code(parser, expr)` pushes to stack
- **After**: `__expr_code_target(parser, expr, reg)` stores to specific register
- Applied to: SELECT columns, GROUP BY expressions, aggregate parameters

### Aggregate Processing
- GROUP BY expressions now use register ranges
- Aggregate function indices stored in registers
- Database access integers use dedicated registers

### Operations Eliminated
- `OP_Pop` for result discarding (registers reused automatically)
- `OP_MemStore` for temporary storage (replaced with `OP_Copy`)
- Stack-based `OP_Integer` calls (converted to register-targeted)

## Files Changed

- `src/cg_select.c` - Main conversion (preserved backup as cg_select.c.backup)

## Testing Status

⚠️ **Build system issues prevent compilation testing**
- Missing `dbsql_config.h` and configuration scripts
- Need to fix build before verifying changes

## Integration Notes

- Depends on Task #5 (AggInfo structure) for complete aggregate processing
- Sorter operations retain some stack-based operations (complex internal state)
- Compound SELECT (UNION/INTERSECT/EXCEPT) basic structure updated

## Reference Implementation

Based on SQLite 3.52.0 `src/select.c`:
- `selectInnerLoop()` function
- `OP_ResultRow` opcode usage
- Register allocation patterns

## Next Steps

1. Fix build system
2. Test compilation
3. Run test suite
4. Integrate with Task #5 (AggInfo)
5. Verify compound SELECT operations

## Status

**85% Complete** - Core functionality converted, pending testing and integration.
