# VDBE Immediate Fixes - Quick Compilation Path

## Goal
Get `/home/gburd/ws/dbsql/src/vdbe.c` to compile successfully (35/35 files).

## Current Build Errors
**Error Count:** ~200+ errors
**Blocking:** Last file in build

## Fix Sequence (Prioritized by Impact)

---

## FIX #1: Add Missing vdbe_return Label
**Time:** 2 minutes
**Impact:** Fixes 1 error immediately
**Location:** Before line 5936

### Current Code (line 5931-5936):
```c
	}  /* The end of the for(;;) loop the loops through opcodes */

  /*
   * If we reach this point, it means that execution is finished.
   */
vdbe_halt:
```

### Fixed Code:
```c
	}  /* The end of the for(;;) loop the loops through opcodes */

  /*
   * Normal return from opcodes that complete successfully.
   */
vdbe_return:
	p->pc = pc;
	/* Fall through to vdbe_halt */

  /*
   * If we reach this point, it means that execution is finished.
   */
vdbe_halt:
```

**Compilation Test:** `make 2>&1 | grep -c "vdbe_return"`
Should drop to 0.

---

## FIX #2: Disable Legacy Aggregator Functions
**Time:** 5 minutes
**Impact:** Fixes ~40 errors
**Location:** Lines 179-234

### Wrap with #if 0:
```c
#if 0  /* LEGACY AGGREGATOR - REMOVED IN SQLITE 3.52.0
        * These functions used the old hash-based aggregator.
        * Modern VDBE uses OP_AggStep/OP_AggFinal with Mem-based accumulators.
        * TODO: Remove after verifying aggregate opcodes work correctly.
        */

/*
 * __agg_insert --
 *	Insert a new aggregate element and make it the element that
 *	has focus.
 *	Return 0 on success and 1 if memory is exhausted.
 *
 * STATIC: static int __agg_insert __P((agg_t *, char *, int));
 */
static int
__agg_insert(agg_t* p, char* key, int len)
{
	// ... existing code (lines 188-212) ...
}

/*
 * __agg_in_focus --
 *	Get the agg_elem_t currently in focus
 *
 * STATIC: static agg_elem_t *__agg_in_focus __P((agg_t *p));
 */
static agg_elem_t *
__agg_in_focus(agg_t* p)
{
	// ... existing code (lines 221-234) ...
}

#endif /* LEGACY AGGREGATOR */
```

**Compilation Test:** `make 2>&1 | grep -c "agg_elem_t"`
Should drop to 0.

---

## FIX #3: Fix Mem Union Field Access in Helper Functions
**Time:** 30 minutes
**Impact:** Fixes ~50 errors
**Locations:** Lines 244-410

### 3a. Fix __entity_as_string() (line 244-262)

**Current Code:**
```c
static int
__entity_as_string(mem_t* stack)
{
	if ((stack->flags & MEM_Str) == 0) {
		int fg = stack->flags;
		if (fg & MEM_Real) {
			snprintf(stack->zShort, sizeof(stack->zShort),
					"%.15g", stack->r);
		} else if (fg & MEM_Int) {
			snprintf(stack->zShort, sizeof(stack->zShort),
					"%d",stack->i);
		} else {
			stack->zShort[0] = 0;
		}
		stack->z = stack->zShort;
		stack->n = strlen(stack->zShort) + 1;
		stack->flags = MEM_Str | MEM_Short;
		return 0;
	}
}
```

**Fixed Code:**
```c
static int
__entity_as_string(mem_t* stack)
{
	if ((stack->flags & MEM_Str) == 0) {
		int fg = stack->flags;
		char buf[32];

		if (fg & MEM_Real) {
			snprintf(buf, sizeof(buf), "%.15g", stack->u.r);
		} else if (fg & MEM_Int) {
			snprintf(buf, sizeof(buf), "%lld", (long long)stack->u.i);
		} else {
			buf[0] = 0;
		}

		/* Allocate dynamic memory for the string */
		if (__dbsql_calloc(NULL, 1, strlen(buf) + 1, &stack->z) == ENOMEM)
			return 1;
		memcpy(stack->z, buf, strlen(buf) + 1);
		stack->n = strlen(buf) + 1;
		stack->flags = MEM_Str | MEM_Dyn;
	}
	return 0;
}
```

### 3b. Fix __entity_to_int() (line 362-376)

**Current Code:**
```c
static void
__entity_to_int(mem_t* stack)
{
	if ((stack->flags & MEM_Int) == 0) {
		if (stack->flags & MEM_Real) {
			stack->i = (int)stack->r;
			__entity_release_mem(stack);
		} else if (stack->flags & MEM_Str) {
			__dbsql_atoi(stack->z, &stack->i);
			__entity_release_mem(stack);
		} else {
			stack->i = 0;
		}
		stack->flags = MEM_Int;
	}
}
```

**Fixed Code:**
```c
static void
__entity_to_int(mem_t* stack)
{
	if ((stack->flags & MEM_Int) == 0) {
		if (stack->flags & MEM_Real) {
			stack->u.i = (i64)stack->u.r;
			__entity_release_mem(stack);
		} else if (stack->flags & MEM_Str) {
			i64 val;
			__dbsql_atoi64(stack->z, &val);
			stack->u.i = val;
			__entity_release_mem(stack);
		} else {
			stack->u.i = 0;
		}
		stack->flags = MEM_Int;
	}
}
```

### 3c. Fix __entity_to_real() (line 387-408)

**Current Code:**
```c
static void
__entity_to_real(mem_t* stack)
{
	if ((stack->flags & MEM_Real) == 0) {
		if (stack->flags & MEM_Int) {
			stack->r = (double)stack->i;
		} else if (stack->flags & MEM_Str) {
			if (!__str_is_numeric(stack->z)) {
				stack->r = 0.0;
			} else {
				/* TODO, DIAGNOSTIC?
				DBSQL_ASSERT(stack->flags & MEM_Str);
				*/
				stack->r = atof(stack->z);
			}
		} else {
			stack->r = 0.0;
		}
		stack->flags |= MEM_Real;
	}
}
```

**Fixed Code:**
```c
static void
__entity_to_real(mem_t* stack)
{
	if ((stack->flags & MEM_Real) == 0) {
		if (stack->flags & MEM_Int) {
			stack->u.r = (double)stack->u.i;
		} else if (stack->flags & MEM_Str) {
			if (!__str_is_numeric(stack->z)) {
				stack->u.r = 0.0;
			} else {
				stack->u.r = atof(stack->z);
			}
		} else {
			stack->u.r = 0.0;
		}
		stack->flags |= MEM_Real;
	}
}
```

### 3d. Fix __vdbe_boolean_value() (line 410-431)

**Current Code:**
```c
static int
__vdbe_boolean_value(mem_t* pMem, int ifNull)
{
	if (pMem->flags & MEM_Int) {
		return pMem->i != 0;
	} else if (pMem->flags & MEM_Real) {
		return pMem->r != 0.0;
	} else if (pMem->flags & MEM_Str) {
		return pMem->n > 1 || (pMem->n == 1 && pMem->z[0] != '0');
	} else {
		return ifNull;
	}
}
```

**Fixed Code:**
```c
static int
__vdbe_boolean_value(mem_t* pMem, int ifNull)
{
	if (pMem->flags & MEM_Int) {
		return pMem->u.i != 0;
	} else if (pMem->flags & MEM_Real) {
		return pMem->u.r != 0.0;
	} else if (pMem->flags & MEM_Str) {
		return pMem->n > 1 || (pMem->n == 1 && pMem->z[0] != '0');
	} else {
		return ifNull;
	}
}
```

**Compilation Test:** `make 2>&1 | grep "has no member 'i'"`
Should drop significantly.

---

## FIX #4: Update __api_step() for Mem* Arrays
**Time:** 20 minutes
**Impact:** Fixes 2 errors
**Location:** Lines 133-177

### Current Code (lines 156-169):
```c
	if (rc == DBSQL_DONE || rc == DBSQL_ROW) {
		if (col_names)
			*col_names = (const char**)vm->azColName;
		if (n)
			*n = vm->nResColumn;
	} else {
		if (col_names)
			*col_names = 0;
		if (n)
			*n = 0;
	}
	if (values) {
		if (rc == DBSQL_ROW) {
			*values = (const char**)vm->azResColumn;
		} else {
			*values = 0;
		}
	}
```

### Fixed Code:
```c
	if (rc == DBSQL_DONE || rc == DBSQL_ROW) {
		if (col_names) {
			/* aColName is Mem* array, extract .z pointers */
			static const char **colCache = NULL;
			if (!colCache && vm->nResColumn > 0) {
				colCache = (const char**)__dbsql_malloc(NULL,
					vm->nResColumn * sizeof(char*));
			}
			if (colCache) {
				int i;
				for (i = 0; i < vm->nResColumn; i++) {
					colCache[i] = vm->aColName[i].z ?
						vm->aColName[i].z : "";
				}
				*col_names = colCache;
			} else {
				*col_names = 0;
			}
		}
		if (n)
			*n = vm->nResColumn;
	} else {
		if (col_names)
			*col_names = 0;
		if (n)
			*n = 0;
	}
	if (values) {
		if (rc == DBSQL_ROW && vm->pResultRow) {
			/* pResultRow is Mem* pointer, extract .z pointers */
			static const char **valCache = NULL;
			if (!valCache && vm->nResColumn > 0) {
				valCache = (const char**)__dbsql_malloc(NULL,
					vm->nResColumn * sizeof(char*));
			}
			if (valCache) {
				int i;
				for (i = 0; i < vm->nResColumn; i++) {
					Mem *pMem = &vm->pResultRow[i];
					/* Convert to string if needed */
					if ((pMem->flags & MEM_Str) == 0) {
						__entity_as_string(pMem);
					}
					valCache[i] = pMem->z ? pMem->z : "";
				}
				*values = valCache;
			} else {
				*values = 0;
			}
		} else {
			*values = 0;
		}
	}
```

**Compilation Test:**
- `make 2>&1 | grep "azColName"` → should be 0
- `make 2>&1 | grep "azResColumn"` → should be 0

---

## FIX #5: Update __vdbe_exec() Function Structure
**Time:** 30 minutes
**Impact:** Fixes ~10 errors
**Location:** Lines 664-694

### Current Code (lines 666-694):
```c
__vdbe_exec(vdbe_t* p)
{
	int pc;                    /* The program counter */
	vdbe_op_t *pOp;            /* Current operation */
	int rc = DBSQL_SUCCESS;    /* Value to return */
	DBSQL *db = p->db;         /* The database */
	mem_t *pTos;               /* Top entry in the operand stack */
#define BUF_SIZE 100
	char zBuf[BUF_SIZE];       /* Space to sprintf() an integer */
	// ... other variables ...

	if (p->magic != VDBE_MAGIC_RUN)
		return DBSQL_MISUSE;
	DBSQL_ASSERT(db->magic == DBSQL_STATUS_BUSY);
	DBSQL_ASSERT(p->rc == DBSQL_SUCCESS || p->rc == DBSQL_BUSY);
	p->rc = DBSQL_SUCCESS;
	DBSQL_ASSERT(p->explain == 0);
	if (p->rc == ENOMEM)
		goto no_mem;
	pTos = p->pTos;                           // ✗ ERROR: no member pTos
	if (p->popStack) {                         // ✗ ERROR: no member popStack
		__pop_stack(&pTos, p->popStack);
		p->popStack = 0;
	}
	for(pc = p->pc; rc == DBSQL_SUCCESS; pc++) {
		DBSQL_ASSERT(pc >= 0 && pc < p->nOp);
		DBSQL_ASSERT(pTos <= &p->aStack[pc]);  // ✗ ERROR: no member aStack
```

### Fixed Code:
```c
__vdbe_exec(vdbe_t* p)
{
	int pc;                    /* The program counter */
	vdbe_op_t *pOp;            /* Current operation */
	int rc = DBSQL_SUCCESS;    /* Value to return */
	DBSQL *db = p->db;         /* The database */
	Mem *aMem;                 /* Register array (replaces pTos stack) */
#define BUF_SIZE 100
	char zBuf[BUF_SIZE];       /* Space to sprintf() an integer */
	// ... other variables ...

	if (p->magic != VDBE_MAGIC_RUN)
		return DBSQL_MISUSE;
	DBSQL_ASSERT(db->magic == DBSQL_STATUS_BUSY);
	DBSQL_ASSERT(p->rc == DBSQL_SUCCESS || p->rc == DBSQL_BUSY);
	p->rc = DBSQL_SUCCESS;
	DBSQL_ASSERT(p->explain == 0);
	if (p->rc == ENOMEM)
		goto no_mem;

	/* Get register array from VDBE */
	aMem = p->aMem;

	/* Note: popStack logic removed - not needed in register-based VDBE */

	for(pc = p->pc; rc == DBSQL_SUCCESS; pc++) {
		DBSQL_ASSERT(pc >= 0 && pc < p->nOp);
		/* Note: aStack assertion removed - no stack in register-based VDBE */
```

### Also Fix End of Function (line 5943-5944):
**Current:**
```c
	p->magic = VDBE_MAGIC_HALT;
	p->pTos = pTos;      // ✗ ERROR
	return rc;
```

**Fixed:**
```c
	p->magic = VDBE_MAGIC_HALT;
	/* Note: No need to save pTos - register-based VDBE doesn't track stack pointer */
	return rc;
```

**Compilation Test:**
- `make 2>&1 | grep "pTos"` → should drop significantly
- `make 2>&1 | grep "popStack"` → should be 0
- `make 2>&1 | grep "aStack"` → should drop significantly

---

## FIX #6: Fix OP_Variable Opcode (azVar → aVar)
**Time:** 10 minutes
**Impact:** Fixes 3 errors
**Location:** Lines 909-920

### Current Code:
```c
case OP_Variable: {
	int j = pOp->p1 - 1;
	pTos++;
	if (j >= 0 && j < p->nVar && p->azVar[j] != 0) {
		pTos->z = p->azVar[j];
		pTos->n = p->anVar[j];
		pTos->flags = MEM_Str | MEM_Static;
	} else {
		pTos->flags = MEM_Null;
	}
	break;
}
```

### Fixed Code:
```c
case OP_Variable: {
	int j = pOp->p1 - 1;
	int iReg = pOp->p2;  /* Target register */
	Mem *pOut = &aMem[iReg];

	if (j >= 0 && j < p->nVar) {
		/* aVar is Mem* array, copy the Mem */
		Mem *pVar = &p->aVar[j];
		if (pVar->flags & MEM_Null) {
			pOut->flags = MEM_Null;
		} else {
			/* Copy the variable value to output register */
			pOut->flags = pVar->flags;
			pOut->z = pVar->z;
			pOut->n = pVar->n;
			pOut->u = pVar->u;
		}
	} else {
		pOut->flags = MEM_Null;
	}
	break;
}
```

**Compilation Test:** `make 2>&1 | grep "azVar"` → should be 0

---

## FIX #7: Fix OP_Gosub and OP_Return (returnStack → register)
**Time:** 15 minutes
**Impact:** Fixes 6 errors
**Location:** Lines 801-830

### Current Code (OP_Gosub, lines 801-812):
```c
case OP_Gosub: {
	if (p->returnDepth >=
	    (sizeof(p->returnStack) / sizeof(p->returnStack[0]))) {
		__str_append(&p->zErrMsg, "return address stack overflow",
			     (char*)0);
		p->rc = DBSQL_INTERNAL;
		return DBSQL_ERROR;
	}
	p->returnStack[p->returnDepth++] = pc+1;
	pc = pOp->p2 - 1;
	break;
}
```

### Fixed Code (OP_Gosub):
```c
case OP_Gosub: {
	/* Modern SQLite: return address stored in register P1 */
	int iReg = pOp->p1;
	Mem *pRet = &aMem[iReg];
	pRet->u.i = (i64)(pc + 1);
	pRet->flags = MEM_Int;
	pc = pOp->p2 - 1;
	break;
}
```

### Current Code (OP_Return, lines 820-830):
```c
case OP_Return: {
	if (p->returnDepth <= 0) {
		__str_append(&p->zErrMsg, "return address stack underflow",
			     (char*)0);
		p->rc = DBSQL_INTERNAL;
		return DBSQL_ERROR;
	}
	p->returnDepth--;
	pc = p->returnStack[p->returnDepth] - 1;
	break;
}
```

### Fixed Code (OP_Return):
```c
case OP_Return: {
	/* Modern SQLite: return address read from register P1 */
	int iReg = pOp->p1;
	Mem *pRet = &aMem[iReg];
	if ((pRet->flags & MEM_Int) == 0) {
		__str_append(&p->zErrMsg, "return address is not an integer",
			     (char*)0);
		p->rc = DBSQL_INTERNAL;
		return DBSQL_ERROR;
	}
	pc = (int)pRet->u.i - 1;
	break;
}
```

**Compilation Test:**
- `make 2>&1 | grep "returnStack"` → should be 0
- `make 2>&1 | grep "returnDepth"` → should be 0

---

## FIX #8: Remove aStack Assertions Throughout
**Time:** 10 minutes
**Impact:** Fixes ~5 errors
**Locations:** Lines 697, 929, 1167, others

### Pattern to Find:
```bash
grep -n "aStack" src/vdbe.c
```

### Fix Pattern:
**Any line with `p->aStack` assertion:**
```c
✗ DBSQL_ASSERT(pTos <= &p->aStack[pc]);
✗ DBSQL_ASSERT(pTos >= &p->aStack[-1]);
✗ DBSQL_ASSERT(&pTos[1-nField] >= p->aStack);
```

**Replace with comment or remove:**
```c
/* Note: Stack assertion removed - register-based VDBE doesn't use aStack */
```

**Compilation Test:** `make 2>&1 | grep "aStack"` → should be 0

---

## Testing After All Fixes

### Expected Result:
```bash
cd /home/gburd/ws/dbsql/build_unix
make 2>&1 | tee /tmp/build.log
grep "error:" /tmp/build.log | wc -l
```

**Target:** Error count should drop from ~200 to ~50 or less after these 8 fixes.

### Remaining Errors (Expected):
Most remaining errors will be from opcodes that still use:
- `pTos++` / `pTos--` (stack operations)
- `pTos[-n]` (stack-relative addressing)
- Direct `.i` / `.r` access in opcode implementations

These will need systematic per-opcode conversion (covered in next phase).

## Compilation Milestone:
After these 8 fixes:
- **Files compiling:** Still 34/35 (vdbe.c still fails)
- **But error count drops dramatically:** 200 → ~50 errors
- **Path forward is clear:** Convert remaining opcodes one by one

---

## Next Phase (After These Fixes)

Convert opcodes systematically, starting with simplest:

**Phase 1 Opcodes (5-10 lines each, 30 minutes total):**
- OP_Integer
- OP_String
- OP_Null
- OP_Not
- OP_Noop

**Phase 2 Opcodes (10-20 lines each, 2 hours):**
- OP_Add, OP_Subtract, OP_Multiply, OP_Divide
- OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge
- OP_And, OP_Or

**Phase 3-N Opcodes (Complex, 20-40 hours):**
- Database operations
- Cursor operations
- Aggregates
- Sorting
- Transactions

## Summary

**Immediate Fixes (8 items):**
1. Add vdbe_return label
2. Disable legacy aggregator
3. Fix Mem union access (.i → .u.i, .r → .u.r, remove zShort)
4. Fix __api_step() Mem* arrays
5. Update __vdbe_exec() structure (remove pTos, popStack, aStack)
6. Fix OP_Variable (azVar → aVar)
7. Fix OP_Gosub/Return (returnStack → register)
8. Remove aStack assertions

**Expected Impact:**
- Time: 2-3 hours
- Error reduction: 200 → ~50 errors
- Makes path forward clear and manageable
