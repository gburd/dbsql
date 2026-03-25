# DBSQL Type System Documentation

## Overview

This document describes the DBSQL type naming conventions and the relationship between DBSQL types and their SQLite origins. DBSQL follows a consistent snake_case_t naming convention, while SQLite uses PascalCase.

## Primary VDBE Types

### Core Types (snake_case_t)

| DBSQL Type | Description | Source File |
|-----------|-------------|-------------|
| `mem_t` | Register-based value cell (64-bit int, double, BLOB, string) | src/inc/vdbe_int.h:224-254 |
| `vdbe_t` | Virtual database engine instance (register-based VM) | src/inc/vdbe_int.h:452-529 |
| `vdbe_op_t` | VDBE opcode instruction | src/inc/vdbe.h |
| `vdbe_frame_t` | Sub-program frame for triggers (OP_Program) | src/inc/vdbe_int.h:341-363 |
| `vdbe_sorter_t` | Opaque sorter used by ORDER BY operations | src/inc/vdbe_int.h:74 |
| `vdbe_txt_blb_cache_t` | Cache for large TEXT/BLOB values | src/inc/vdbe_int.h:190-196 |

### Exception: VdbeCursor (PascalCase)

`VdbeCursor` is intentionally kept in PascalCase for historical and API compatibility reasons:

```c
typedef struct VdbeCursor VdbeCursor;
struct VdbeCursor {
    u8 eCurType;           /* CURTYPE_BTREE, SORTER, VTAB, PSEUDO */
    i8 iDb;                /* Database index */
    u8 nullRow;            /* True if pointing to row with no data */
    u8 deferredMoveto;     /* Deferred __sm_moveto() call needed */
    ...
};
```

**Why PascalCase is preserved:**
- Deep integration with Berkeley DB SM (storage manager) layer
- Used extensively in opcode handlers throughout src/vdbe.c
- Changing to `cursor_t` would require updating 65+ occurrences
- The name `VdbeCursor` clearly indicates VDBE-specific cursor (vs sm_cursor_t)

**Public API:** External code should never see `VdbeCursor` directly. It is an internal VDBE type defined in vdbe_int.h.

## Deprecated Types

### cursor_t (Scheduled for Removal)

```c
// src/inc/dbsql_int.in:129
struct cursor; typedef struct cursor cursor_t;
```

**Status:** Forward declaration only, no actual definition exists.

**Replacement:** All code uses `VdbeCursor` directly.

**Migration notes:**
- The forward declaration exists for compatibility during transition
- No code actively uses `cursor_t`
- Will be removed in future cleanup

### Mem (Alias for mem_t)

**Status:** No typedef exists. All code uses `mem_t`.

**Usage in comments:** Comments may reference "Mem" when explaining SQLite origins:
- "mem_t (Mem)" indicates DBSQL type with SQLite equivalent
- Example: vdbe_int.h:34 "mem_t (Mem): 64-bit integers, encoding, BLOB..."

## Type Mapping Table

### DBSQL to SQLite Type Correspondence

| DBSQL Type | SQLite Type | Purpose |
|-----------|------------|---------|
| `mem_t` | `struct Mem` / `sqlite3_value` | Register value cell |
| `vdbe_t` | `struct Vdbe` | Virtual machine instance |
| `VdbeCursor` | `struct VdbeCursor` | VDBE cursor |
| `vdbe_op_t` | `struct VdbeOp` | VDBE instruction |
| `vdbe_frame_t` | `struct VdbeFrame` | Trigger/sub-program frame |
| `key_info_t` | `struct KeyInfo` | Index key comparison info |
| `unpacked_record_t` | `struct UnpackedRecord` | Unpacked index key |
| `func_def_t` | `struct FuncDef` | SQL function definition |
| `coll_seq_t` | `struct CollSeq` | Collation sequence |

### Storage Manager Types

| DBSQL Type | Berkeley DB Type | Purpose |
|-----------|-----------------|---------|
| `sm_t` | `DB` | Database handle |
| `sm_cursor_t` | `DBC` | Berkeley DB cursor |
| `sm_txn_t` | `DB_TXN` | Transaction handle |
| `sm_env_t` | `DB_ENV` | Environment handle |

## Naming Convention Rules

### General Rules

1. **snake_case_t suffix:** All DBSQL types use lowercase with underscores and `_t` suffix
2. **No dual typedefs:** One canonical name per type (no Mem/mem_t aliases)
3. **Forward declarations:** Match definition exactly (no PascalCase forward, snake_case definition)

### Exceptions

**PascalCase allowed when:**
1. Direct Berkeley DB integration makes snake_case impractical
2. Type is purely internal (never exposed in public API)
3. Renaming would break deep integrations (VdbeCursor case)

**Current exceptions:**
- `VdbeCursor` - VDBE internal cursor structure (explained above)

### Avoiding SQLite Type Names

Do not use these SQLite type names in new code:

| Avoid | Use Instead | Reason |
|-------|-------------|--------|
| `Mem` | `mem_t` | DBSQL convention |
| `Vdbe` | `vdbe_t` | DBSQL convention |
| `VdbeOp` | `vdbe_op_t` | DBSQL convention |
| `VdbeFrame` | `vdbe_frame_t` | DBSQL convention |
| `cursor` | `VdbeCursor` | Avoid confusion with deprecated cursor_t |
| `BtCursor` | `sm_cursor_t` | Use storage manager abstraction |
| `Btree` | `sm_t` | Use storage manager abstraction |

## Type Usage Guidelines

### When to Use mem_t

```c
mem_t *pMem;                    /* Register value */
mem_t registers[100];           /* Register array */
pMem->flags = MEM_Int;          /* Set type flags */
pMem->u.i = 42;                 /* 64-bit integer */
```

**Common operations:**
- `__vdbe_mem_release(pMem)` - Free memory
- `__vdbe_mem_set_int(pMem, i64 val)` - Set integer
- `__vdbe_mem_set_str(pMem, const char *z, int n, u8 enc, void (*xDel)(void*))` - Set string
- `__vdbe_mem_make_writable(pMem)` - Ensure writeable buffer

### When to Use VdbeCursor

```c
VdbeCursor *pCur;              /* Internal VDBE cursor */
pCur = vdbe->cursors[iCur];    /* Get from cursor array */
pCur->eCurType = CURTYPE_BTREE; /* Set cursor type */
pCur->uc.pCursor = pBtCur;     /* Storage manager cursor */
```

**Common operations:**
- `__vdbe_cursor_moveto(pCur, &res)` - Position cursor
- `__vdbe_cursor_restore(pCur)` - Restore after moveto
- `OP_Column` opcode - Read column from cursor

### When to Use vdbe_t

```c
vdbe_t *v;                     /* VDBE instance */
v->registers[iReg]             /* Access register */
v->cursors[iCur]               /* Access cursor */
v->pc                          /* Program counter */
```

**Lifecycle:**
1. `__vdbe_create(db)` - Create VDBE
2. `__vdbe_add_op3(v, opcode, p1, p2, p3)` - Add instructions
3. `__vdbe_reset(v)` - Reset for next execution
4. `__vdbe_finalize(v)` - Destroy VDBE

## Memory Cell (mem_t) Flag Reference

### Type Flags (Affinity)

```c
#define MEM_Undefined 0x0000   /* Uninitialized */
#define MEM_Null      0x0001   /* NULL value */
#define MEM_Str       0x0002   /* String */
#define MEM_Int       0x0004   /* 64-bit integer */
#define MEM_Real      0x0008   /* Double */
#define MEM_Blob      0x0010   /* Binary data */
#define MEM_IntReal   0x0020   /* Int that stringifies as Real */
```

### Modifier Flags

```c
#define MEM_FromBind  0x0040   /* From SQL parameter binding */
#define MEM_Cleared   0x0100   /* NULL from OP_Null */
#define MEM_Term      0x0200   /* String is zero-terminated */
#define MEM_Zero      0x0400   /* Blob with u.nZero trailing zeros */
#define MEM_Subtype   0x0800   /* eSubtype is valid (JSON, etc.) */
```

### Storage Flags

```c
#define MEM_Dyn       0x1000   /* Call xDel() on z */
#define MEM_Static    0x2000   /* z points to static string */
#define MEM_Ephem     0x4000   /* z points to ephemeral string */
#define MEM_Agg       0x8000   /* z is aggregate function context */
```

### Helper Macros

```c
#define VdbeMemDynamic(X)  (((X)->flags & (MEM_Agg|MEM_Dyn)) != 0)
#define MemSetTypeFlag(p, f) \
    ((p)->flags = ((p)->flags & ~(MEM_TypeMask|MEM_Zero)) | f)
#define memIsValid(M) (((M)->flags & MEM_AffMask) != 0)  /* Debug only */
```

## Cursor Type Reference

### Cursor Type Constants

```c
#define CURTYPE_BTREE   0    /* Berkeley DB btree cursor (sm_cursor_t) */
#define CURTYPE_SORTER  1    /* External sorter (vdbe_sorter_t) */
#define CURTYPE_VTAB    2    /* Virtual table cursor (future) */
#define CURTYPE_PSEUDO  3    /* Pseudo-cursor (single register) */
```

### Type-Specific Union

```c
union {
    sm_cursor_t *pCursor;    /* CURTYPE_BTREE or PSEUDO */
    vdbe_sorter_t *pSorter;  /* CURTYPE_SORTER */
} uc;
```

## Integer Type Aliases

DBSQL uses compact type names from SQLite for brevity in structures:

```c
typedef u_int8_t   u8;     /* Unsigned 8-bit */
typedef u_int16_t  u16;    /* Unsigned 16-bit */
typedef u_int32_t  u32;    /* Unsigned 32-bit */
typedef u_int64_t  u64;    /* Unsigned 64-bit */
typedef int8_t     i8;     /* Signed 8-bit */
typedef int16_t    i16;    /* Signed 16-bit */
typedef int64_t    i64;    /* Signed 64-bit (register value) */
```

## Special Types

### Page Numbers and Identifiers

```c
typedef u32 Pgno;          /* Page number (or table/index ID) */
typedef i16 ynVar;         /* SQL parameter number (?NNN) */
typedef u64 tRowcnt;       /* Row count for statistics */
typedef i16 LogEst;        /* Log-base-2 estimate (10*log2(X)) */
typedef u64 Bitmask;       /* Column bitmask (64 columns max) */
```

## Version History

### Register-Based VDBE (SQLite 3.52.0 Architecture)

**Major changes from stack-based VDBE:**

1. **mem_t (Mem) changes:**
   - Added 64-bit integer support (i64 vs old 32-bit int)
   - Added encoding field (enc) for UTF-8/UTF-16
   - Added destructor callback (xDel) for MEM_Dyn values
   - Added subtype field for extension types (JSON)
   - Removed inline zShort buffer (replaced by zMalloc)

2. **VdbeCursor changes:**
   - Added eCurType field (btree, sorter, vtab, pseudo)
   - Added union uc for type-specific pointers
   - Added column cache fields (cacheStatus, colCache)
   - Added 64-bit movetoTarget for i64 rowid seeks

3. **vdbe_t changes:**
   - Removed: aStack, pTos (stack machine eliminated)
   - Removed: pSort, pFile, nField (COPY command removed)
   - Removed: agg, nSet, aSet (legacy aggregator removed)
   - Added: registers array (mem_t *) replaces stack
   - Added: cursors array (VdbeCursor **) pointer-based
   - Added: cacheCtr for column cache invalidation
   - Added: nChange for DML change counting
   - Added: pFrame/pDelFrame for triggers

### Transition Notes

Code referencing old types should migrate:

- `Mem` comments → `mem_t` in new code
- `cursor_t` usage → `VdbeCursor` (or remove if deprecated)
- Stack references → register array references
- `Agg`/`AggElem` → `agg_info_t` unified structure

## References

**Source files:**
- `src/inc/vdbe_int.h` - VDBE internal structures
- `src/inc/vdbe.h` - VDBE public API
- `src/inc/dbsql_int.in` - Main internal header
- `src/vdbe.c` - VDBE opcode implementation
- `src/vdbe_method.c` - VDBE helper functions

**SQLite documentation:**
- SQLite VDBE architecture: https://www.sqlite.org/opcode.html
- Register-based design: SQLite 3.x architecture docs

**Berkeley DB integration:**
- Storage manager abstraction (sm_t, sm_cursor_t)
- See `src/inc/sm.h` for storage layer API

---

**Document version:** 1.0
**Last updated:** 2026-03-25
**Status:** Complete type system documentation post-Phase 2 cleanup
