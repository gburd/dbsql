# UPSERT AST Structure Design

**Date**: 2026-03-18
**Target**: DBSQL AST structures (src/inc/dbsql_int.in)
**Purpose**: Define upsert_t structure for ON CONFLICT clause

---

## Executive Summary

Define `upsert_t` structure matching SQLite's `Upsert`, adapted for DBSQL conventions:
- Snake_case naming
- _t suffix for types
- Clear memory ownership semantics
- Integration with existing expr_list_t and expr_t

**Estimated Effort**: 5-10 hours

---

## 1. Structure Definition

### 1.1 Main Structure

```c
/*
 * Represents an ON CONFLICT clause in INSERT statement.
 * Multiple upsert_t can be chained via 'next' pointer.
 *
 * Memory ownership:
 * - Parser allocates and initializes all fields
 * - __insert() takes ownership, must call __upsert_delete() on error
 * - Code generator uses fields, doesn't free them
 * - __insert() cleanup frees entire chain via __upsert_delete()
 *
 * (SQLite: struct Upsert, sqliteInt.h:3565)
 */
struct upsert {
    /* ========== Parse Tree (owned by upsert_t) ========== */

    expr_list_t *target;         /* Conflict target columns (e.g., (id, email)) */
    expr_t *target_where;        /* WHERE clause for partial index target */
    expr_list_t *set_list;       /* SET expressions for DO UPDATE (NULL = DO NOTHING) */
    expr_t *update_where;        /* WHERE clause for DO UPDATE */
    upsert_t *next;              /* Next ON CONFLICT clause (linked list) */

    u8 is_do_update;             /* 1 if DO UPDATE, 0 if DO NOTHING */
    u8 is_dup;                   /* 1 if duplicate clause (same index targeted) */

    /* ========== Analysis Results (set by __upsert_analyze_target) ========== */

    index_t *upsert_idx;         /* Which unique index/constraint this targets */
    void *to_free;               /* Auxiliary memory to free with upsert_t */

    /* ========== Code Generation State (set by __insert) ========== */

    src_list_t *upsert_src;      /* Table being inserted into */
    int reg_data;                /* First register holding VALUES data */
    int data_cursor;             /* Cursor number for table */
    int idx_cursor;              /* First cursor number for indexes */
};
```

### 1.2 Field Descriptions

| Field | Type | Purpose | When Set |
|-------|------|---------|----------|
| **target** | expr_list_t* | Column list for conflict detection | Parser |
| **target_where** | expr_t* | WHERE clause for partial index | Parser |
| **set_list** | expr_list_t* | SET expressions (NULL = DO NOTHING) | Parser |
| **update_where** | expr_t* | WHERE filter for DO UPDATE | Parser |
| **next** | upsert_t* | Next clause in linked list | Parser |
| **is_do_update** | u8 | Cached flag: set_list != NULL | Parser |
| **is_dup** | u8 | Redundant clause warning flag | Analysis |
| **upsert_idx** | index_t* | Resolved index this targets | Analysis |
| **to_free** | void* | Extra memory for cleanup | Analysis |
| **upsert_src** | src_list_t* | Source table | Code gen |
| **reg_data** | int | Base register for VALUES | Code gen |
| **data_cursor** | int | Table cursor number | Code gen |
| **idx_cursor** | int | First index cursor number | Code gen |

---

## 2. Constructor and Destructor

### 2.1 Constructor

```c
/*
 * __upsert_new --
 *     Allocate and initialize a new upsert_t structure.
 *
 * PUBLIC: upsert_t *__upsert_new __P((parser_t *, expr_list_t *, expr_t *,
 * PUBLIC:     expr_list_t *, expr_t *, upsert_t *));
 *
 * parser           Parser context (for error reporting and allocation)
 * target           Conflict target column list (NULL if implicit)
 * target_where     WHERE clause for partial index (NULL if none)
 * set_list         SET expressions for DO UPDATE (NULL for DO NOTHING)
 * update_where     WHERE clause for DO UPDATE (NULL if none)
 * next             Next ON CONFLICT clause (NULL if last)
 *
 * RETURNS: Newly allocated upsert_t*, or NULL on OOM
 *
 * On OOM, all arguments are freed before returning NULL.
 */
upsert_t *
__upsert_new(parser, target, target_where, set_list, update_where, next)
    parser_t *parser;
    expr_list_t *target;
    expr_t *target_where;
    expr_list_t *set_list;
    expr_t *update_where;
    upsert_t *next;
{
    upsert_t *upsert;
    DBSQL *dbp;

    dbp = parser->db;

    if (__dbsql_calloc(dbp, 1, sizeof(upsert_t), &upsert) != 0) {
        /* OOM - clean up arguments */
        __expr_list_delete(target);
        __expr_delete(target_where);
        __expr_list_delete(set_list);
        __expr_delete(update_where);
        __upsert_delete(next);
        return 0;
    }

    /* Initialize fields */
    upsert->target = target;
    upsert->target_where = target_where;
    upsert->set_list = set_list;
    upsert->update_where = update_where;
    upsert->next = next;
    upsert->is_do_update = (set_list != 0);
    upsert->is_dup = 0;

    /* Analysis and code gen fields initialized to NULL/0 */
    upsert->upsert_idx = 0;
    upsert->to_free = 0;
    upsert->upsert_src = 0;
    upsert->reg_data = 0;
    upsert->data_cursor = 0;
    upsert->idx_cursor = 0;

    return upsert;
}
```

### 2.2 Destructor

```c
/*
 * __upsert_delete_internal --
 *     Recursive deletion of upsert_t chain.
 *     Static helper, not exported.
 */
static void
__upsert_delete_internal(dbp, upsert)
    DBSQL *dbp;
    upsert_t *upsert;
{
    upsert_t *next;

    while (upsert != 0) {
        next = upsert->next;

        /* Free parse tree components */
        __expr_list_delete(upsert->target);
        __expr_delete(upsert->target_where);
        __expr_list_delete(upsert->set_list);
        __expr_delete(upsert->update_where);

        /* Free auxiliary memory */
        if (upsert->to_free != 0)
            __dbsql_free(dbp, upsert->to_free);

        /* Note: upsert_src, upsert_idx not owned by upsert_t */

        /* Free the structure itself */
        __dbsql_free(dbp, upsert);

        upsert = next;
    }
}

/*
 * __upsert_delete --
 *     Free an upsert_t structure and all chained structures.
 *
 * PUBLIC: void __upsert_delete __P((upsert_t *));
 *
 * upsert       Upsert structure to free (may be NULL)
 *
 * Safe to call with NULL pointer (no-op).
 * Frees entire linked list recursively.
 */
void
__upsert_delete(upsert)
    upsert_t *upsert;
{
    if (upsert != 0) {
        DBSQL *dbp = upsert->target ? upsert->target->dbp : /* guess from context */;
        __upsert_delete_internal(dbp, upsert);
    }
}
```

**Issue with dbp**: We need DBSQL* for __dbsql_free(), but upsert_t doesn't store it.

**Solution**: Pass dbp explicitly to __upsert_delete():

```c
void __upsert_delete(DBSQL *dbp, upsert_t *upsert);
```

Or store dbp in upsert_t:

```c
struct upsert {
    DBSQL *dbp;  /* Database handle for memory management */
    /* ... other fields ... */
};
```

**Recommendation**: Store dbp in upsert_t for cleaner API.

---

## 3. Duplication Function

```c
/*
 * __upsert_dup --
 *     Deep copy an upsert_t structure chain.
 *
 * PUBLIC: upsert_t *__upsert_dup __P((DBSQL *, upsert_t *));
 *
 * dbp          Database handle for allocation
 * upsert       Upsert structure to duplicate
 *
 * RETURNS: Newly allocated copy, or NULL if input is NULL or on OOM
 *
 * Used when duplicating parse trees (e.g., for prepared statements).
 */
upsert_t *
__upsert_dup(dbp, upsert)
    DBSQL *dbp;
    upsert_t *upsert;
{
    if (upsert == 0)
        return 0;

    return __upsert_new(
        /* parser = */ 0,  /* Not called from parser context */
        __expr_list_dup(dbp, upsert->target, 0),
        __expr_dup(dbp, upsert->target_where, 0),
        __expr_list_dup(dbp, upsert->set_list, 0),
        __expr_dup(dbp, upsert->update_where, 0),
        __upsert_dup(dbp, upsert->next)
    );
}
```

**Issue**: __upsert_new() needs parser_t*, but we're not in parser context.

**Solution**: Create separate constructor for duplication:

```c
upsert_t *__upsert_new_from_parts(DBSQL *dbp, ...);  /* No parser needed */
upsert_t *__upsert_new(parser_t *parser, ...);       /* Wrapper for parser */
```

---

## 4. Integration with Existing Structures

### 4.1 Forward Declaration (dbsql_int.in)

```c
/* Add to forward declarations section */
struct upsert;        typedef struct upsert upsert_t;
```

Already present in current dbsql_int.in (line 89).

### 4.2 Usage in insert_stmt

```c
/* Hypothetical insert_stmt structure (if we had one) */
struct insert_stmt {
    src_list_t *table;       /* Table to insert into */
    expr_list_t *columns;    /* Column names (optional) */
    expr_list_t *values;     /* VALUES expressions */
    select_t *select;        /* SELECT for INSERT ... SELECT */
    int on_error;            /* Conflict resolution (OE_*) */
    upsert_t *upsert;        /* ON CONFLICT clauses */
};
```

**Current DBSQL**: Doesn't have insert_stmt structure; __insert() takes bare arguments.

**Decision**: Pass upsert_t* as additional parameter to __insert(), no structural changes needed.

---

## 5. Memory Management Patterns

### 5.1 Allocation Pattern

```
Parser allocates:
  upsert = __upsert_new(...)
    |
    +-- Allocates upsert_t
    |
    +-- Takes ownership of expr_list_t* and expr_t* arguments
    |
    +-- Returns upsert_t* to grammar action

Grammar action:
  __insert(... upsert)
    |
    +-- Takes ownership of upsert_t*
    |
    +-- On success: Keeps upsert for code generation, frees in cleanup
    |
    +-- On error: Calls __upsert_delete() before returning
```

### 5.2 Error Handling Pattern

```c
/* In __insert() */
void
__insert(parser, tlist, vlist, select, column, on_error, upsert)
    /* ... */
    upsert_t *upsert;
{
    /* ... parsing and setup ... */

    if (parser->nErr)
        goto insert_cleanup;

    /* ... code generation ... */

    if (some_error) {
        parser->nErr++;
        goto insert_cleanup;
    }

    /* ... successful completion ... */

insert_cleanup:
    __src_list_delete(tlist);
    if (vlist)
        __expr_list_delete(vlist);
    if (select)
        __select_delete(select);
    __id_list_delete(column);
    __upsert_delete(parser->db, upsert);  /* Free upsert on any exit */
}
```

---

## 6. Linked List Operations

### 6.1 Traversal

```c
/* Iterate through all ON CONFLICT clauses */
upsert_t *u;
for (u = upsert; u != 0; u = u->next) {
    if (u->target == 0) {
        /* Implicit target (no column list) */
    } else {
        /* Explicit target */
        for (i = 0; i < u->target->nExpr; i++) {
            expr_t *col = u->target->a[i].pExpr;
            /* Process column expression */
        }
    }

    if (u->is_do_update) {
        /* DO UPDATE clause */
    } else {
        /* DO NOTHING clause */
    }
}
```

### 6.2 Finding Clause for Index

```c
/*
 * __upsert_of_index --
 *     Find the ON CONFLICT clause that targets a specific index.
 *
 * PUBLIC: upsert_t *__upsert_of_index __P((upsert_t *, index_t *));
 *
 * upsert       Head of upsert_t linked list
 * idx          Index to search for
 *
 * RETURNS: Matching upsert_t*, or NULL if no clause targets this index
 */
upsert_t *
__upsert_of_index(upsert, idx)
    upsert_t *upsert;
    index_t *idx;
{
    while (upsert != 0 &&
           upsert->target != 0 &&
           upsert->upsert_idx != idx) {
        upsert = upsert->next;
    }
    return upsert;
}
```

---

## 7. Debugging and Introspection

### 7.1 Debug Print Function

```c
#ifdef DBSQL_DEBUG
/*
 * __upsert_print --
 *     Print upsert_t structure for debugging.
 */
void
__upsert_print(upsert, indent)
    upsert_t *upsert;
    int indent;
{
    while (upsert != 0) {
        printf("%*sON CONFLICT ", indent, "");

        if (upsert->target) {
            printf("(");
            __expr_list_print(upsert->target);
            printf(")");
        }

        if (upsert->target_where) {
            printf(" WHERE ");
            __expr_print(upsert->target_where);
        }

        if (upsert->is_do_update) {
            printf(" DO UPDATE SET ");
            __expr_list_print(upsert->set_list);
            if (upsert->update_where) {
                printf(" WHERE ");
                __expr_print(upsert->update_where);
            }
        } else {
            printf(" DO NOTHING");
        }

        if (upsert->upsert_idx) {
            printf(" [targets index: %s]", upsert->upsert_idx->zName);
        }

        printf("\n");

        upsert = upsert->next;
    }
}
#endif
```

---

## 8. DBSQL Naming Conventions Compliance

### 8.1 Structure Naming

| SQLite | DBSQL | Rationale |
|--------|-------|-----------|
| `Upsert` | `upsert_t` | snake_case + _t suffix |
| `pUpsertTarget` | `target` | Drop Hungarian notation |
| `pUpsertTargetWhere` | `target_where` | snake_case |
| `pUpsertSet` | `set_list` | snake_case + clarity |
| `pUpsertWhere` | `update_where` | snake_case + clarity |
| `pNextUpsert` | `next` | Drop "p" prefix |
| `isDoUpdate` | `is_do_update` | snake_case |
| `isDup` | `is_dup` | snake_case |
| `pUpsertIdx` | `upsert_idx` | Drop "p" prefix + snake_case |
| `pToFree` | `to_free` | Drop "p" prefix + snake_case |
| `pUpsertSrc` | `upsert_src` | Drop "p" prefix + snake_case |
| `regData` | `reg_data` | snake_case |
| `iDataCur` | `data_cursor` | snake_case + expand abbreviation |
| `iIdxCur` | `idx_cursor` | snake_case + expand abbreviation |

### 8.2 Function Naming

| SQLite | DBSQL | Rationale |
|--------|-------|-----------|
| `sqlite3UpsertNew` | `__upsert_new` | snake_case + __ prefix |
| `sqlite3UpsertDelete` | `__upsert_delete` | snake_case + __ prefix |
| `sqlite3UpsertDup` | `__upsert_dup` | snake_case + __ prefix |
| `sqlite3UpsertAnalyzeTarget` | `__upsert_analyze_target` | snake_case + __ prefix |
| `sqlite3UpsertDoUpdate` | `__upsert_do_update` | snake_case + __ prefix |
| `sqlite3UpsertOfIndex` | `__upsert_of_index` | snake_case + __ prefix |

---

## 9. Implementation Checklist

| Task | File | Effort |
|------|------|--------|
| Add upsert_t definition | src/inc/dbsql_int.in | 1h |
| Implement __upsert_new() | src/cg_insert.c | 2h |
| Implement __upsert_delete() | src/cg_insert.c | 1h |
| Implement __upsert_dup() | src/cg_insert.c | 1h |
| Implement __upsert_of_index() | src/cg_insert.c | 1h |
| Debug print function | src/cg_insert.c | 1h |
| Unit tests | test/upsert_ast.test | 2-3h |
| **Total** | | **9-11h** |

---

## 10. Unit Testing Strategy

### 10.1 Constructor Tests

```c
/* Test basic allocation */
upsert_t *u = __upsert_new(parser, NULL, NULL, NULL, NULL, NULL);
ASSERT(u != NULL);
ASSERT(u->is_do_update == 0);  /* DO NOTHING */
__upsert_delete(dbp, u);

/* Test DO UPDATE */
expr_list_t *set = make_set_list("x=1");
upsert_t *u2 = __upsert_new(parser, NULL, NULL, set, NULL, NULL);
ASSERT(u2->is_do_update == 1);
__upsert_delete(dbp, u2);

/* Test linked list */
upsert_t *u3 = __upsert_new(parser, NULL, NULL, NULL, NULL,
                            __upsert_new(parser, NULL, NULL, NULL, NULL, NULL));
ASSERT(u3->next != NULL);
ASSERT(u3->next->next == NULL);
__upsert_delete(dbp, u3);  /* Should free entire chain */
```

### 10.2 Ownership Tests

```c
/* Test OOM handling */
expr_list_t *target = make_expr_list("id");
set_oom_mode(1);  /* Simulate OOM */
upsert_t *u = __upsert_new(parser, target, NULL, NULL, NULL, NULL);
ASSERT(u == NULL);
/* target should be freed by __upsert_new() */
```

---

## 11. Next Steps

1. ✅ Complete AST design
2. ⏸️ Implement upsert_t structure in dbsql_int.in
3. ⏸️ Implement constructor/destructor functions
4. ⏸️ Implement utility functions (__upsert_of_index, etc.)
5. ⏸️ Write unit tests
6. ⏸️ Proceed to code generation design

---

**Document Status**: ✅ COMPLETE
**Estimated Effort**: 5-10 hours
**Confidence**: HIGH
**Dependencies**: None (parallel with parser design)
