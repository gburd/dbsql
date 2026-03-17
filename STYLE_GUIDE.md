# DBSQL Code Style Guide

This document defines the coding standards and naming conventions for the DBSQL project.

## Type Naming Conventions

DBSQL follows consistent naming conventions for all type definitions to maintain code clarity and reduce confusion between different naming styles.

### Standard: snake_case with _t suffix

All types (structs, unions, enums, and typedefs) use lowercase snake_case with a `_t` suffix to indicate "type":

**Correct:**
```c
typedef struct mem mem_t;
typedef struct vdbe vdbe_t;
typedef struct cursor cursor_t;
typedef struct vdbe_frame vdbe_frame_t;
typedef struct parser parser_t;
typedef struct expr expr_t;
typedef struct key_info key_info_t;
```

**Incorrect:**
```c
typedef struct mem Mem;        /* PascalCase - SQLite style */
typedef struct vdbe Vdbe;
typedef struct VdbeCursor {};  /* Bare PascalCase */
```

### Why this convention?

1. **Consistency**: All DBSQL and BerkeleyDB types use this standard
2. **Clarity**: The `_t` suffix immediately indicates a type definition
3. **Namespace safety**: Lowercase prevents conflicts with macros (which use UPPERCASE)
4. **Maintainability**: Easier to distinguish types from variables at a glance

### SQLite 3.52.0 Compatibility Note

SQLite uses PascalCase naming (`Mem`, `Vdbe`, `VdbeCursor`). When integrating SQLite 3.52.0 code:

- Update all typedef declarations to use DBSQL convention
- Replace struct field declarations using SQLite types with DBSQL equivalents
- Example transpilation:
  ```c
  /* SQLite style (before) */
  typedef struct Vdbe Vdbe;
  Mem *pValue;

  /* DBSQL style (after) */
  typedef struct vdbe vdbe_t;
  mem_t *pValue;
  ```

## Struct and Union Naming

- **Struct names**: Use lowercase snake_case without `_t` suffix
- **Struct typedefs**: Same name with `_t` suffix added
- **Union names**: Inline as `union { ... }` with typedef if needed

Example:
```c
struct mem {
    /* fields */
};
typedef struct mem mem_t;

struct vdbe {
    union {
        int i;
        double r;
    } u;
};
typedef struct vdbe vdbe_t;
```

## Enum Naming

- **Enum names**: Use lowercase snake_case
- **Enum values**: Use UPPERCASE with semantic prefixes
- **Enum typedefs**: Add `_t` suffix to the type name

Example:
```c
enum mem_flags {
    MEM_NULL = 0x0001,
    MEM_INT = 0x0004,
    MEM_REAL = 0x0008,
};
typedef enum mem_flags mem_flags_t;

enum cursor_type {
    CURTYPE_BTREE = 0,
    CURTYPE_SORTER = 1,
    CURTYPE_PSEUDO = 3,
};
```

## Macro Naming

- **Constants and flags**: All UPPERCASE with underscores
- **Type/struct macros**: UPPERCASE_SUFFIX pattern
- **Avoid PascalCase**: Never use PascalCase for macro names

Example:
```c
#define MEM_NULL        0x0001
#define MEM_INT         0x0004
#define VDBE_INIT_STATE 0
#define VDBE_READY_STATE 1

#define SZ_VDBECURSOR(N) (...)
#define MEMCELLSIZE offsetof(mem_t, db)
```

## Pointer Naming Conventions

- **Struct pointers**: Use `p` prefix followed by struct type name
- **Array pointers**: Use `a` prefix followed by element type
- **Other pointers**: Use semantic names

Example:
```c
mem_t *pMem;              /* pointer to mem_t */
vdbe_t *pVdbe;            /* pointer to vdbe_t */
mem_t *aMem;              /* array of mem_t elements */
mem_t **apArg;            /* array of pointers to mem_t */
cursor_t **apCsr;         /* array of pointers to cursor_t */
```

## Function Naming

- **Internal functions**: Use double-underscore prefix for scope limitation
- **Naming style**: lowercase snake_case
- **Verb-first pattern**: Action describes what function does

Example:
```c
/* Good: Clear action verbs */
__vdbe_mem_set_null(mem_t *p);
__vdbe_mem_release(mem_t *p);
__vdbe_mem_copy(mem_t *pTo, const mem_t *pFrom);

/* Avoid: Unclear names */
process_mem();           /* Too vague */
mem_process();          /* Wrong order */
MemSetNull();           /* Wrong case */
```

## Variable Naming

- **Local variables**: Use lowercase snake_case
- **Loop counters**: Single letters acceptable (i, j, k) or meaningful names
- **Status/flags**: Use descriptive names indicating boolean/flag state

Example:
```c
int i, j;               /* Loop counters */
int cursor_idx;         /* More descriptive cursor index */
bool_t is_cached;      /* Clear boolean state */
u32 cache_status;       /* More specific than 'status' */
```

## Comments for Type Definitions

Document non-trivial type definitions with:
1. What the type represents
2. Key fields or attributes
3. Any SQLite 3.52.0 mapping reference

Example:
```c
/*
 * mem_t -- register-based value cell
 *
 * Replaces the old stack-based memory representation. Key changes:
 *   - 64-bit integer support (i64 vs old 32-bit int)
 *   - Encoding field (enc) for UTF-8/UTF-16BE/UTF-16LE
 *   - BLOB support (MEM_Blob flag)
 *   - Destructor callback (xDel) for MEM_Dyn values
 *
 * (SQLite: struct sqlite3_value, vdbeInt.h:232)
 */
```

## File Organization

### Header Files (.h)
1. File header comment (copyright, license, purpose)
2. Include guards (`#ifndef _XXX_H_`)
3. Forward struct declarations
4. Type definitions (typedefs, enums)
5. Macro definitions
6. Function prototypes

### Implementation Files (.c)
1. File header comment
2. System includes
3. Local includes
4. Static function declarations
5. Implementation
6. Internal static functions

## Enforcement

These conventions are verified by:
- Pre-commit hooks that check naming patterns
- Type checker warnings (when enabled)
- Code review checklist
- Automated linting tools (when available)

Violations should be fixed proactively - they indicate either:
1. Incomplete modernization from SQLite
2. Code that needs review/refactoring
3. Documentation gaps

## References

- **Project**: DBSQL (Berkeley DB SQL engine)
- **Based on**: SQLite 3.52.0 architecture
- **Standards**: POSIX C conventions with DBSQL extensions
- **Related**: See POST_MODERNIZATION_PLAN_STATUS.md for naming convention migration phases
