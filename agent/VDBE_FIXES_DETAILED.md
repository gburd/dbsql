# VDBE.C Detailed Fix Patterns

**Date:** 2026-03-20
**Task:** Document all systematic fixes applied to vdbe.c

## Fix Categories

### 1. Structure Definition Additions

#### agg_elem_t and agg_t (vdbe_int.h)

**Problem:** Forward-declared but never defined, causing ~200 "incomplete type" errors.

**Location:** /home/gburd/ws/dbsql/src/inc/vdbe_int.h, lines 334-349

**Solution:**
```c
/* Added after mem_t definition to ensure mem_t is complete */

struct agg_elem {
    char *zKey;              /* Hash key for this group */
    int nKey;                /* Length of the key */
    mem_t registers[];       /* Flexible array of register values for group */
};

struct agg {
    int nMem;                /* Number of registers per group */
    hash_t hash;             /* Hash table of agg_elem_t entries */
    agg_elem_t *pCurrent;    /* Current aggregate element */
};
```

**Rationale:**
- `agg_elem_t` uses flexible array member requiring complete mem_t type
- Must appear after mem_t definition (line 254) but before usage
- Placement after memIsValid macro (line 330) is optimal

### 2. mem_t Field Access Corrections

#### Pattern: Integer Field Access

**Error Pattern:**
```
error: 'mem_t' {aka 'struct mem'} has no member named 'i'
```

**Old Code:**
```c
stack->i = value;
pRec->i = 0;
pMem->i++;
```

**Fixed Code:**
```c
stack->u.i = value;
pRec->u.i = 0;
pMem->u.i++;
```

**Sed Transform:**
```bash
s/\([^u]\)->i\([^a-zA-Z0-9_]\)/\1->u.i\2/g
s/\([^u]\)\.i\([^a-zA-Z0-9_]\)/\1.u.i\2/g
```

**Explanation:**
- mem_t.i moved to mem_t.u.i (union MemValue)
- Negative lookbehind `[^u]` prevents double-substitution (->u.u.i)
- Character class boundary `[^a-zA-Z0-9_]` ensures word boundaries

**Instances Fixed:** ~40 occurrences

#### Pattern: Real/Double Field Access

**Error Pattern:**
```
error: 'mem_t' {aka 'struct mem'} has no member named 'r'
```

**Old Code:**
```c
stack->r = 3.14;
pRec->r = pRec->i;
value = pMem->r;
```

**Fixed Code:**
```c
stack->u.r = 3.14;
pRec->u.r = pRec->u.i;
value = pMem->u.r;
```

**Sed Transform:**
```bash
s/\([^u]\)->r\([^a-zA-Z0-9_]\)/\1->u.r\2/g
s/\([^u]\)\.r\([^a-zA-Z0-9_]\)/\1.u.r\2/g
```

**Instances Fixed:** ~30 occurrences

### 3. zShort Buffer Elimination

#### Pattern: zShort References

**Error Pattern:**
```
error: 'mem_t' {aka 'struct mem'} has no member named 'zShort'
```

**Background:**
- SQLite <= 3.5: mem_t had `char zShort[32]` inline buffer
- SQLite >= 3.52.0: Removed, use dynamic zMalloc instead
- DBSQL was using old model

**Old Code Patterns:**
```c
snprintf(pTos->zShort, sizeof(pTos->zShort), "%d", value);
pTos->z = pTos->zShort;
pTos->n = strlen(pTos->zShort);
if (pTos->z == pTos->zShort) { /* is inline */ }
```

**Fixed Code:**
```c
snprintf(buf, sizeof(buf), "%lld", (long long)value);
/* Use zMalloc for storage */
pTos->z = pTos->zMalloc ? pTos->zMalloc : "";
pTos->n = strlen(pTos->z);
/* zMalloc is always dynamic now */
```

**Sed Transform:**
```bash
s/pTos->zShort/pTos->z/g
s/pRec->zShort/pRec->z/g
s/pMem->zShort/pMem->z/g
s/ctx\.s\.zShort/ctx.s.z/g
s/->zShort/->z/g
```

**Special Case: __entity_as_string()**

**Old Implementation:**
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
                    "%d", stack->i);
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

**New Implementation:**
```c
static int
__entity_as_string(mem_t* stack)
{
    if ((stack->flags & MEM_Str) == 0) {
        int fg = stack->flags;
        char buf[32];
        int len;

        if (fg & MEM_Real) {
            snprintf(buf, sizeof(buf), "%.15g", stack->u.r);
        } else if (fg & MEM_Int) {
            snprintf(buf, sizeof(buf), "%lld", (long long)stack->u.i);
        } else {
            buf[0] = 0;
        }

        len = strlen(buf) + 1;
        if (len <= 32 && stack->szMalloc == 0) {
            if (__dbsql_calloc(stack->db, 1, 32, (void**)&stack->zMalloc) == ENOMEM)
                return ENOMEM;
            stack->szMalloc = 32;
        }
        if (stack->zMalloc) {
            memcpy(stack->zMalloc, buf, len);
            stack->z = stack->zMalloc;
            stack->n = len - 1;
            stack->flags = MEM_Str | MEM_Term;
        }
        return 0;
    }
    return 0;
}
```

**Key Changes:**
1. Local `buf[32]` for formatting (stack-allocated)
2. Allocate zMalloc if needed (32 bytes minimum)
3. Copy formatted string to zMalloc
4. Set MEM_Term flag (terminated string) not MEM_Short (removed)
5. Return ENOMEM on allocation failure
6. Add explicit return 0 at end

**Instances Fixed:** 15 occurrences

### 4. __dbsql_malloc Signature Updates

#### Pattern: Return Value vs Output Parameter

**Error Pattern:**
```
error: too few arguments to function '__dbsql_malloc'
```

**Signature:**
```c
int __dbsql_malloc(DBSQL *dbp, size_t size, void **ptr);
```

**Old Code:**
```c
colCache = (const char**)__dbsql_malloc(NULL, size);
```

**Fixed Code:**
```c
__dbsql_malloc(NULL, size, (void**)&colCache);
```

**Changes:**
1. Return value is `int` status code (0 = success, ENOMEM = failure)
2. Allocated pointer returned via third parameter (void **)
3. Cast required: `(void**)&variable`

**Instances Fixed:** 2 occurrences (lines 160, 187)

### 5. Type System Additions

#### Pattern: Forward Declarations

**Error Pattern:**
```
error: unknown type name 'sorter_t'
```

**File:** src/inc/dbsql_int.in

**Addition (line 131):**
```c
struct sorter;        typedef struct sorter sorter_t;
```

**Impact:**
- Allows forward references to sorter_t
- Does NOT define the structure (still incomplete)
- Sufficient for pointer usage: `sorter_t *p;`
- NOT sufficient for member access: `p->field`

**Note:** Full struct sorter definition needed for complete fix.

### 6. Forward Declaration Additions

#### Pattern: Function Declaration Order

**Error Pattern:**
```
error: static declaration of '__entity_as_string' follows non-static declaration
```

**Problem:**
- `__entity_as_string()` called at line 198
- Defined at line 281
- First use triggers implicit declaration (non-static)
- Later static definition conflicts

**Solution (line 88):**
```c
/* Forward declarations */
static int __entity_as_string(mem_t* stack);
```

**Instances Fixed:** 1 occurrence

### 7. Format String Corrections

#### Pattern: i64 Integer Formatting

**Error Pattern:**
```
warning: format '%d' expects argument of type 'int', but argument 4 has type 'i64' {aka 'long int'}
```

**Old Code:**
```c
snprintf(buf, sizeof(buf), "%d", stack->u.i);
```

**Fixed Code:**
```c
snprintf(buf, sizeof(buf), "%lld", (long long)stack->u.i);
```

**Explanation:**
- i64 is typedef for `long long int` (64-bit)
- %d format expects `int` (typically 32-bit)
- Correct format: %lld (long long decimal)
- Cast for portability: `(long long)`

**Instances Fixed:** 1 occurrence (line 294)

## Summary Statistics

| Fix Category | Errors Fixed | Method |
|-------------|--------------|---------|
| Structure definitions | ~200 | Manual addition to vdbe_int.h |
| mem_t.i field access | ~40 | Sed pattern replacement |
| mem_t.r field access | ~30 | Sed pattern replacement |
| zShort elimination | ~50 | Sed + manual __entity_as_string rewrite |
| __dbsql_malloc signature | 2 | Manual fix |
| Type forward declarations | ~10 | Manual addition to dbsql_int.in |
| Function forward declarations | 1 | Manual addition |
| Format strings | 1 | Sed replacement |
| **Total** | **~334** | **Mixed** |

## Files Modified Summary

1. **src/inc/vdbe_int.h**
   - Added agg_elem_t structure (3 members)
   - Added agg_t structure (3 members)
   - Total lines added: ~15

2. **src/vdbe.c**
   - Fixed mem_t field accesses (~70 locations)
   - Removed zShort references (~15 locations)
   - Rewrote __entity_as_string() (~30 lines)
   - Fixed __dbsql_malloc calls (2 locations)
   - Added forward declaration (1 line)
   - Fixed format string (1 line)
   - Total lines modified: ~120

3. **src/inc/dbsql_int.in**
   - Added sorter_t typedef (1 line)

## Verification Commands

```bash
# Count mem_t.u.i accesses (should be ~40)
grep -c '->u\.i\|\.u\.i' src/vdbe.c

# Count mem_t.u.r accesses (should be ~30)
grep -c '->u\.r\|\.u\.r' src/vdbe.c

# Verify no zShort in code (should only be in comments)
grep -n 'zShort' src/vdbe.c | grep -v '/\*\|//'

# Check structure definitions exist
grep -A3 "struct agg_elem\|struct agg" src/inc/vdbe_int.h
```

## Remaining Work

The 376 remaining errors fall into these categories:

1. **Incomplete Types:** cursor_t, sorter_t need full definitions
2. **Removed Members:** vdbe_t fields (azColName, xCallback, etc.)
3. **Stack Architecture:** pTos, pNos pointers don't exist in register model
4. **Opcode Changes:** ~150 opcode implementations use old architecture

These require architectural modernization, not simple pattern fixes.
