# VDBE Current State Analysis

## File Status
**Location:** `/home/gburd/ws/dbsql/src/vdbe.c`
**Lines:** 5,988
**Compilation Status:** FAILS (multiple errors)
**Blocking:** Last file preventing full build (34/35 compile successfully)

## Partial Modernization Detected

The file shows MIXED architecture - some parts converted to register-based, others still stack-based:

### Already Modernized (Line 1103-1107):
```c
case OP_ResultRow: {
    // ... code ...
    /* Point to first result register */
    p->pResultRow = &aMem[pOp->p1];  // ✓ REGISTER-BASED
    p->cacheCtr = (p->cacheCtr + 2) | 1;
    p->pc = (int)(pc + 1);
    rc = DBSQL_ROW;
    goto vdbe_return;  // ✓ Modern control flow
}
```

### Still Legacy Stack-Based:

**Lines 670-694 (__vdbe_exec function):**
```c
mem_t *pTos;               // ✗ Stack pointer (should be removed)
pTos = p->pTos;            // ✗ Restore stack (line 690)
if (p->popStack) {         // ✗ Stack pop logic (line 691-693)
    __pop_stack(&pTos, p->popStack);
    p->popStack = 0;
}
DBSQL_ASSERT(pTos <= &p->aStack[pc]);  // ✗ Stack assertion (line 697)
p->pTos = pTos;            // ✗ Save stack (line 5944)
```

**Lines 870-920 (Stack-based opcodes):**
```c
case OP_Integer: {
    pTos++;                // ✗ Stack push
    pTos->i = pOp->p1;     // ✗ Direct .i access (should be .u.i)
    pTos->flags = MEM_Int;
    break;
}

case OP_String: {
    pTos++;                // ✗ Stack push
    pTos->z = z;
    break;
}

case OP_Variable: {
    pTos++;                // ✗ Stack push
    if (j >= 0 && j < p->nVar && p->azVar[j] != 0) {  // ✗ azVar (should be aVar)
        pTos->z = p->azVar[j];
        pTos->n = p->anVar[j];  // ✗ anVar array doesn't exist
    }
    break;
}

case OP_Pop: {
    __pop_stack(&pTos, pOp->p1);  // ✗ Stack pop
    DBSQL_ASSERT(pTos >= &p->aStack[-1]);  // ✗ Stack assertion
    break;
}
```

## Critical Build Errors (Prioritized)

### ERROR 1: Missing vdbe_return Label (CRITICAL)
**Location:** Line 1107 has `goto vdbe_return;` but label doesn't exist
**Impact:** Immediate compile failure
**Fix:** Add label before vdbe_halt (line ~5935)
```c
vdbe_return:
    p->pc = (int)(pc + 1);
    /* Fall through to vdbe_halt for cleanup */

vdbe_halt:
    // existing code
```

### ERROR 2: Structure Member Mismatches (HIGH)
**Line 157:**
```c
✗ *col_names = (const char**)vm->azColName;  // No member 'azColName'
✓ *col_names = (const char**)vm->aColName;   // WRONG - aColName is Mem*, not char**
```
Requires wrapper to extract .z from each Mem element.

**Line 168:**
```c
✗ *values = (const char**)vm->azResColumn;   // No member 'azResColumn'
✓ *values = (const char**)vm->pResultRow;    // WRONG - pResultRow is Mem*, not char**
```
Requires wrapper to extract .z from each Mem element.

**Lines 690, 5944:**
```c
✗ pTos = p->pTos;   // No member 'pTos'
✗ p->pTos = pTos;   // No member 'pTos'
```
Remove entirely - register-based doesn't track stack pointer.

**Lines 691-693:**
```c
✗ if (p->popStack) {  // No member 'popStack'
      __pop_stack(&pTos, p->popStack);
      p->popStack = 0;
  }
```
Remove entirely - no stack pop mechanism in register-based.

**Lines 697, 929, 1167:**
```c
✗ DBSQL_ASSERT(pTos <= &p->aStack[pc]);     // No member 'aStack'
✗ DBSQL_ASSERT(pTos >= &p->aStack[-1]);     // No member 'aStack'
✗ DBSQL_ASSERT(&pTos[1-nField] >= p->aStack); // No member 'aStack'
```
Remove entirely - no aStack in register-based.

**Lines 802-809 (OP_Gosub):**
```c
✗ if (p->returnDepth >= (sizeof(p->returnStack) / sizeof(p->returnStack[0]))) {
      // error
  }
  p->returnStack[p->returnDepth++] = pc+1;  // No member 'returnStack', 'returnDepth'
```
Register-based uses register to store return address.

**Lines 821-828 (OP_Return):**
```c
✗ if (p->returnDepth <= 0) {  // No member 'returnDepth'
      // error
  }
  p->returnDepth--;
  pc = p->returnStack[p->returnDepth] - 1;  // No member 'returnStack'
```
Register-based reads return address from register.

**Lines 912-914 (OP_Variable):**
```c
✗ if (j >= 0 && j < p->nVar && p->azVar[j] != 0) {  // No member 'azVar'
      pTos->z = p->azVar[j];
      pTos->n = p->anVar[j];  // No member 'anVar'
  }
```
Should use `p->aVar[j]` which is Mem* array.

### ERROR 3: Legacy Aggregator Code (HIGH)
**Lines 188-234:**
- `__agg_insert()` - Uses undefined `agg_t`, `agg_elem_t`
- `__agg_in_focus()` - Uses undefined `agg_t`, `hash_ele_t`

**Impact:** 40+ compilation errors
**Status:** These structures don't exist in vdbe_int.h (already removed)
**Fix:** Remove functions entirely OR mark as disabled with `#if 0`

Modern SQLite uses OP_AggStep/OP_AggFinal with Mem-based accumulators, not the legacy hash-based aggregator.

### ERROR 4: Mem Union Field Access (MEDIUM)
**Lines 249-253, 366, 389+:**
```c
✗ stack->r          // Should be stack->u.r
✗ stack->i          // Should be stack->u.i
✗ pTos->r           // Should be pTos->u.r
✗ pTos->i           // Should be pTos->u.i
```

**Occurrences:** ~50+ instances throughout file

### ERROR 5: zShort Buffer (MEDIUM)
**Lines 249-259:**
```c
✗ snprintf(stack->zShort, sizeof(stack->zShort), "%.15g", stack->r);
✗ stack->z = stack->zShort;
✗ stack->flags = MEM_Str | MEM_Short;  // MEM_Short flag removed
```

**Fix:** SQLite 3.52.0 removed inline zShort buffer. Use dynamic allocation:
```c
✓ char buf[32];
  snprintf(buf, sizeof(buf), "%.15g", stack->u.r);
  if (__vdbe_mem_set_str(stack, buf, -1, DBSQL_UTF8_ENCODED, DBSQL_TRANSIENT) != 0)
      goto no_mem;
```

## Quantified Remaining Work

### Stack Operations to Convert:
- `pTos++` occurrences: **~80** (estimated from 148 opcodes)
- `pTos--` occurrences: **22**
- `pTos[-n]` array accesses: **~30**
- `__pop_stack()` calls: **10+**

### Field Access to Fix:
- `.i` → `.u.i`: **~100**
- `.r` → `.u.r`: **~50**
- `zShort` references: **34**

### Opcodes Requiring Conversion:
- **Total opcodes:** 148
- **Already modernized:** ~5 (OP_ResultRow, possibly others)
- **Partially modernized:** ~10 (mixed stack/register)
- **Fully stack-based:** ~133
- **Estimated effort:** 2-5 minutes per opcode = 11-44 hours

## Immediate Action Plan (Get to Compilation)

### Step 1: Add Missing vdbe_return Label (5 minutes)
Insert before line 5936 (vdbe_halt label):
```c
vdbe_return:
    /* Normal return path for opcodes that complete successfully */
    p->pc = pc;  // Save program counter if needed
    /* Fall through to vdbe_halt */

vdbe_halt:
    // existing code...
```

### Step 2: Disable Legacy Aggregator (10 minutes)
Wrap lines 188-234 with:
```c
#if 0  /* LEGACY AGGREGATOR - DISABLED DURING MODERNIZATION */
static int
__agg_insert(agg_t* p, char* key, int len)
{
    // existing code
}

static agg_elem_t *
__agg_in_focus(agg_t* p)
{
    // existing code
}
#endif /* LEGACY AGGREGATOR */
```

### Step 3: Fix __api_step() API Compatibility (30 minutes)
Lines 133-177, convert aColName/pResultRow Mem* to char** for legacy API.

### Step 4: Update __vdbe_exec() Structure (1 hour)
- Remove pTos variable
- Remove popStack logic
- Remove aStack assertions
- Add aMem local variable
- Fix returnStack references

### Step 5: Fix Mem Union Access in Helper Functions (2 hours)
- __entity_as_string() (line 244)
- __entity_to_int() (line 362)
- __entity_to_real() (line 387)
- __vdbe_boolean_value() (line 410)
- __sorted_merge() (line 434)

### Step 6: Convert Core Opcodes (20-40 hours)
Convert opcodes group by group:
1. Data movement (OP_Integer, OP_String, OP_Variable, etc.)
2. Arithmetic (OP_Add, OP_Subtract, etc.)
3. Comparison (OP_Eq, OP_Lt, etc.)
4. Database operations (OP_Column, OP_Insert, etc.)
5. Control flow (OP_Gosub, OP_Return, etc.)

## Expected Compilation Milestone Sequence

**Milestone 1 (Steps 1-2): ~10 errors**
- vdbe_return label added
- Legacy aggregator disabled
- Errors drop from ~200 to ~10

**Milestone 2 (Step 3): ~5 errors**
- API function fixed
- azColName/azResColumn errors resolved

**Milestone 3 (Step 4): ~3 errors**
- __vdbe_exec structure updated
- pTos/popStack/aStack errors resolved

**Milestone 4 (Step 5): ~20 errors**
- Helper functions use .u.i/.u.r
- zShort removed
- Mem access consistent

**Milestone 5 (Step 6): 0 errors (GOAL)**
- All opcodes converted
- Full build succeeds (35/35 files)

## Comparison with Similar Conversions

**Previous successful conversions in this codebase:**
- `cg_expr.c` (Task #24) - Converted expression code generator
- `cg_insert.c` (Task #23) - Converted insert code generator
- `cg_select.c` - Partially converted (has register-based generation)

These files already generate register-based VDBE opcodes. The vdbe.c file is the executor that needs to match that architecture.

## Key Insight

**The code generator files were converted FIRST**, which is why vdbe.c compilation fails - it's receiving register-based opcodes but still executing with stack-based logic. This is actually GOOD - it means:

1. The hard architectural decisions were already made in code generation
2. The opcode parameter meanings are already defined
3. We just need to make vdbe.c execute what the generators produce
4. Test framework expects register-based behavior

**This makes the conversion more mechanical and less risky** - we're aligning the executor to an already-working generator layer.

## Success Criteria

- [ ] File compiles without errors
- [ ] Full build succeeds (35/35 files)
- [ ] Test suite passes
- [ ] No memory leaks (valgrind clean)
- [ ] Behavior matches SQLite 3.52.0
