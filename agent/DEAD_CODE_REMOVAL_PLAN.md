# Dead Code Removal Plan for vdbe.c

## Strategy

For blocks with `#if 0 ... #else ... #endif`:
- Remove the `#if 0` section
- Remove the `#else` and `#endif` markers
- Keep the fallback handler

For blocks with only `#if 0 ... #endif`:
- Remove the entire block including markers

## Blocks to Remove

### Block 1: Lines 1003-1062 - Legacy callback opcodes
**Structure**: `#if 0 ... #endif` (no else)
**Contains**: OP_ColumnName, OP_Callback opcodes
**Action**: Remove entire block

### Block 2: Lines 1099-1119 - OP_NullCallback
**Structure**: `#if 0 ... #else ... #endif`
**Contains**: Legacy OP_NullCallback implementation with fallback
**Action**: Remove #if 0 section, keep #else fallback, remove markers

### Block 3: Line 1341+ - Legacy stack-based OP_Function
**Structure**: `#if 0 ... #endif`
**Action**: Remove entire block

### Block 4: Line 4392+ - keylistStack opcodes
**Structure**: `#if 0 ... #endif`
**Contains**: OP_ListWrite, OP_ListRewind, OP_ListRead
**Action**: Remove entire block

### Block 5: Line 4516+ - Legacy Sort opcodes
**Structure**: `#if 0 ... #endif`
**Contains**: OP_SortPut, OP_SortMakeRec, OP_SortMakeKey, OP_Sort, OP_SortNext, OP_SortCallback, OP_SortReset
**Action**: Remove entire block

### Block 6: Line 4720+ - Legacy callback (xCallback/azResColumn)
**Structure**: `#if 0 ... #endif`
**Action**: Remove entire block

### Block 7: Line 4767+ - Legacy File/COPY opcodes
**Structure**: `#if 0 ... #endif`
**Contains**: OP_FileOpen, OP_FileClose, OP_FileRead, OP_FileColumn, OP_MemStore, OP_MemLoad
**Action**: Remove entire block

### Block 8: Line 5056+ - Legacy aggregate opcodes
**Structure**: `#if 0 ... #endif`
**Contains**: OP_AggReset, OP_AggFocus, OP_AggIncr, OP_AggNext, OP_AggSet, OP_AggGet
**Action**: Remove entire block

### Block 9: Line 5079+ - Legacy stack-based OP_AggFunc
**Structure**: `#if 0 ... #endif`
**Action**: Remove entire block

### Block 10: Line 5709+ - Legacy Set opcodes (p->aSet removed)
**Structure**: `#if 0 ... #endif`
**Contains**: Multiple OP_Set* opcodes
**Action**: Remove entire block

### Blocks 11-14: Lines 5711, 5749, 5776, 5813 - Individual aSet opcodes
**Structure**: `#if 0 ... #endif` for each
**Contains**: OP_SetInsert, OP_SetFound, OP_SetNotFound, OP_SetClear
**Action**: Remove each block

## Blocks to KEEP

### Line 5896 - VDBE_PROFILE debug output
```c
#if 0
	    fprintf(stdout, "%10lld ", elapse);
	    __vdbe_print_op(stdout, origPc, &p->opcodes[origPc]);
#endif
```
**Reason**: Intentional debug output inside `#ifdef VDBE_PROFILE`
**Action**: KEEP - Document as intentional

## Implementation Notes

- Must preserve line structure and indentation
- Verify no opcode references remain after removal
- Check that case statements don't create unreachable code
- Ensure #else fallbacks are properly unwrapped
