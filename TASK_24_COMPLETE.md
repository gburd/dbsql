# Task #24 Complete: Convert Expression Code Generator to Register-Based

## Summary

Successfully converted the expression code generator from stack-based to register-based architecture, following SQLite 3.52.0 patterns.

## Files Modified

### 1. `/home/gburd/ws/dbsql/src/cg_expr.c`

#### Main Changes:

**New Function: `__expr_code_target(parser_t* parser, expr_t* expr, int target)`**
- Primary register-based expression code generator
- Takes a target register parameter where result is stored
- Returns the register containing the result
- Replaces stack-based operations with register operations

**Updated Function: `__expr_code(parser_t* parser, expr_t* expr)`**
- Now a wrapper that allocates a temporary register and calls `__expr_code_target`
- Maintains backward compatibility with existing code

**New Function: `__expr_code_temp(parser_t* parser, expr_t* expr, int* reg_ptr)`**
- Generates code to evaluate expression into a temporary register
- Returns register number and optionally stores it in reg_ptr for later release
- Essential for managing temporary registers in complex expressions

**Helper Functions Added:**
- `__get_temp_reg()` - Allocates a temporary register
- `__release_temp_reg()` - Releases a temporary register
- `__get_temp_range()` - Allocates consecutive registers
- `__release_temp_range()` - Releases a range of registers

#### Expression Type Conversions:

**TK_COLUMN:**
- Old: Push column value to stack
- New: Load column value into target register using OP_Column with P3 as target

**TK_INTEGER/TK_STRING/TK_FLOAT:**
- Old: Push literal to stack via OP_String/OP_Integer
- New: Load literal into target register via OP_Integer/OP_String8 with target as P2

**TK_NULL:**
- Old: Push null to stack
- New: Load null into target register via OP_Null

**TK_VARIABLE:**
- Old: Push variable value to stack
- New: Load variable into target register via OP_Variable with target as P2

**Binary Operators (TK_PLUS, TK_MINUS, TK_EQ, etc.):**
- Old: Recursively push left operand, push right operand, apply operator to top 2 stack items
- New: Evaluate left to temp register r1, evaluate right to temp register r2,
       apply operator with OP_xxx(r2, r1, target), release temp registers

**Unary Operators (TK_NOT, TK_BITNOT, TK_UMINUS):**
- Old: Push operand, apply operator to top stack item
- New: Evaluate operand to temp register r1, apply operator with OP_xxx(r1, target),
       release temp register

**TK_ISNULL/TK_NOTNULL:**
- Old: Push 1, push operand, test, decrement
- New: Load 1 to target, evaluate operand to r1, test r1, set target to 0 if test fails

**TK_FUNCTION:**
- Old: Push arguments to stack, call OP_Function
- New: Allocate consecutive registers r1..r1+n-1, evaluate arguments into registers,
       call OP_Function with P4=func_def, P5=nargs, result in target

**TK_IN:**
- Old: Complex stack manipulation with Push/Pop
- New: Evaluate left operand to r1, test membership with OP_Found/OP_SetFound,
       result in target

**TK_BETWEEN:**
- Old: Dup, compare, Pull, compare, And
- New: Evaluate operands to registers, compare with OP_Ge/OP_Le, combine with OP_And,
       result in target

**TK_CASE:**
- Old: Stack-based condition/result evaluation
- New: Store CASE expression in register if present, evaluate conditions to temp registers,
       evaluate results directly into target, use jumps to skip non-matching branches

**TK_SELECT (subquery):**
- Old: OP_MemLoad pushes to stack
- New: OP_MemLoad loads into target register

### 2. `/home/gburd/ws/dbsql/src/vdbe_method.c`

Added convenience wrapper functions:

**`__vdbe_add_op1(vdbe_t* vm, int op, int p1)`**
- Adds instruction with 1 operand
- Calls `__vdbe_add_op3(vm, op, p1, 0, 0)`

**`__vdbe_add_op2(vdbe_t* vm, int op, int p1, int p2)`**
- Adds instruction with 2 operands
- Calls `__vdbe_add_op3(vm, op, p1, p2, 0)`

**`__vdbe_jump_here(vdbe_t* vm, int addr)`**
- Sets P2 of instruction at addr to current address
- Used for resolving forward jumps
- Calls `__vdbe_change_p2(vm, addr, vm->nOp)`

### 3. `/home/gburd/ws/dbsql/src/inc/dbsql_ext.h`

Added function declarations:

```c
int __expr_code_target __P((parser_t *, expr_t *, int));
int __expr_code_temp __P((parser_t *, expr_t *, int *));
int __get_temp_reg __P((parser_t *));
void __release_temp_reg __P((parser_t *, int));
int __get_temp_range __P((parser_t *, int));
void __release_temp_range __P((parser_t *, int, int));
int __vdbe_add_op1 __P((vdbe_t *, int, int));
int __vdbe_add_op2 __P((vdbe_t *, int, int, int));
void __vdbe_jump_here __P((vdbe_t *, int));
```

## Key Architectural Changes

### Register-Based vs Stack-Based

**Old Stack-Based Model:**
```
Expression: a + b * c
Code:
  Push a
  Push b
  Push c
  Multiply (pops 2, pushes 1)
  Add (pops 2, pushes 1)
```

**New Register-Based Model:**
```
Expression: a + b * c
Code:
  r1 = Column(a)
  r2 = Column(b)
  r3 = Column(c)
  r4 = Multiply(r2, r3)
  r5 = Add(r1, r4)
```

### Benefits:

1. **No implicit stack**: All data flow is explicit through register numbers
2. **Better optimization**: Register allocation can be optimized by future passes
3. **Clearer semantics**: Each instruction specifies exactly where inputs come from and output goes
4. **SQLite compatibility**: Matches SQLite 3.52.0 architecture for easier future updates
5. **Temporary management**: Explicit temporary register allocation/deallocation

### VDBE Instruction Format:

**Old (Stack-based):**
- Opcode, P1 (int), P2 (int), P3 (string)
- Implicit: operates on top of stack

**New (Register-based):**
- Opcode, P1 (source reg/cursor), P2 (jump dest), P3 (dest reg), P4 (union), P5 (flags)
- Explicit: specifies source and destination registers

## Example Transformations

### Binary Expression: `column1 + column2`

**Old:**
```c
__expr_code(parser, expr->pLeft);   // Push column1
__expr_code(parser, expr->pRight);  // Push column2
__vdbe_add_op(v, OP_Add, 0, 0);     // Add top 2 stack items
```

**New:**
```c
r1 = __expr_code_temp(parser, expr->pLeft, &regFree1);    // column1 -> r1
r2 = __expr_code_temp(parser, expr->pRight, &regFree2);   // column2 -> r2
__vdbe_add_op3(v, OP_Add, r2, r1, target);                // r1 + r2 -> target
__release_temp_reg(parser, regFree1);
__release_temp_reg(parser, regFree2);
```

### Function Call: `upper(name)`

**Old:**
```c
for (i = 0; i < nexpr; i++) {
    __expr_code(parser, list->a[i].pExpr);  // Push arg
}
__vdbe_add_op(v, OP_Function, nexpr, 0);
__vdbe_change_p3(v, -1, (char*)def, P3_POINTER);
```

**New:**
```c
if (nexpr > 0) {
    r1 = __get_temp_range(parser, nexpr);      // Allocate r1..r1+n-1
    for (i = 0; i < nexpr; i++) {
        __expr_code_target(parser, list->a[i].pExpr, r1 + i);
    }
} else {
    r1 = 0;
}
__vdbe_add_op4(v, OP_Function, 0, r1, target,
               (char*)def, P4_FUNCDEF);
__vdbe_change_p5(v, (u8)nexpr);
if (nexpr > 0) {
    __release_temp_range(parser, r1, nexpr);
}
```

## Testing Status

The conversion compiles successfully. There are compilation errors in other files that use:
- Old parser structure members (useAgg, aAgg, nAgg, trigStack)
- Old expression structure members (pList, token)
- Old function structure members (xFunc, xStep, dataType)

These errors are expected and will be resolved in subsequent tasks that update those structures.

## Next Steps

1. **Task #25**: Update aggregate function handling to use registers
2. **Task #26**: Convert WHERE clause code generator to register-based
3. **Task #27**: Convert SELECT result handling to register-based
4. **Update parser structure**: Add register tracking fields
5. **Testing**: Write comprehensive tests for expression evaluation

## References

- SQLite 3.52.0: `_/sqlite/src/expr.c` (`sqlite3ExprCodeTarget`)
- VDBE Architecture: `src/inc/vdbe.h`, `src/inc/vdbe_int.h`
- Original stack-based: Previous version of `__expr_code()`

## Notes

- Register management is currently simple (sequential allocation)
- Future optimization: Implement register reuse/free list
- Temporary register tracking uses regFree1/regFree2 pattern
- All expression evaluations now return the result register
