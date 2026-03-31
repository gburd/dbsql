# UPSERT (INSERT ON CONFLICT) Implementation for DBSQL

## Overview

This document describes the implementation of SQL:2011 UPSERT (INSERT ON CONFLICT) support for DBSQL. The implementation follows the SQLite 3.52.0 model with adaptations for DBSQL's architecture.

## Syntax to Support

```sql
-- DO NOTHING variants
INSERT INTO t VALUES(1, 'x') ON CONFLICT DO NOTHING;
INSERT INTO t VALUES(1, 'x') ON CONFLICT(id) DO NOTHING;
INSERT INTO t VALUES(1, 'x') ON CONFLICT(id) WHERE condition DO NOTHING;

-- DO UPDATE variants
INSERT INTO t VALUES(1, 'x') ON CONFLICT(id) DO UPDATE SET name=excluded.name;
INSERT INTO t VALUES(1, 'x') ON CONFLICT(id) WHERE condition DO UPDATE SET name=excluded.name WHERE update_condition;
INSERT INTO t VALUES(1, 'x') ON CONFLICT DO UPDATE SET name=excluded.name;
```

## Implementation Components

### 1. Tokenizer Updates (COMPLETE)

**File:** `/home/gburd/ws/dbsql/src/sql_tokenize.c`
**File:** `/home/gburd/ws/dbsql/src/inc/sql_tokens.h`

- [x] Added TK_EXCLUDED token (value 232)
- [x] Added "EXCLUDED" keyword to sql_tokens_table
- [x] TK_DO and TK_NOTHING already exist

### 2. Parser Grammar Updates (PENDING)

**File:** `/home/gburd/ws/dbsql/src/sql_parser.y`

Need to add UPSERT grammar rules. The current INSERT commands have 7 parameters (the 7th is for RETURNING). We need to add UPSERT as an 8th parameter or restructure.

#### Current INSERT Signature
```c
void __insert(parser_t *parser, src_list_t *tlist, expr_list_t *vlist,
              select_t *select, id_list_t *column, int on_error,
              expr_list_t *returning);
```

#### Proposed Changes

**Option A: Add UPSERT as 8th parameter**
```c
void __insert(parser_t *parser, src_list_t *tlist, expr_list_t *vlist,
              select_t *select, id_list_t *column, int on_error,
              expr_list_t *returning, upsert_t *upsert);
```

**Option B: Use parser->pUpsert** (SQLite approach)
Store UPSERT in parser_t structure and access from __insert:
```c
struct parser {
    // ... existing fields ...
    upsert_t *pUpsert;  /* UPSERT clause for INSERT */
};
```

**Recommended: Option B** (matches SQLite pattern)

#### Grammar Rules to Add

```yacc
%type upsert {upsert_t*}
%destructor upsert {__upsert_delete(pParse->db, $$);}

/* Store upsert in parser for use by __insert */
upsert ::= .  { }
upsert ::= ON CONFLICT LP idxlist(T) RP where_opt(TW) DO UPDATE SET setlist(Z) where_opt(W). {
  pParse->pUpsert = __upsert_new(pParse->db, T, TW, Z, W, 1);
}
upsert ::= ON CONFLICT LP idxlist(T) RP where_opt(TW) DO NOTHING. {
  pParse->pUpsert = __upsert_new(pParse->db, T, TW, 0, 0, 0);
}
upsert ::= ON CONFLICT DO NOTHING. {
  pParse->pUpsert = __upsert_new(pParse->db, 0, 0, 0, 0, 0);
}

/* Update INSERT commands to include upsert */
cmd ::= insert_cmd(R) INTO nm(X) dbnm(D) inscollist_opt(F)
        VALUES LP itemlist(Y) RP upsert.
            {__insert(pParse, __src_list_append(0,&X,&D), Y, 0, F, R, 0);}
cmd ::= insert_cmd(R) INTO nm(X) dbnm(D) inscollist_opt(F) select(S) upsert.
            {__insert(pParse, __src_list_append(0,&X,&D), 0, S, F, R, 0);}
```

### 3. UPSERT Structure and Helper Functions

**File:** `/home/gburd/ws/dbsql/src/cg_upsert.c` (NEW)

#### Structure (already defined in dbsql_int.h)
```c
struct upsert {
    expr_list_t *pUpsertTarget;    /* Optional conflict target columns */
    expr_t *pUpsertTargetWhere;    /* WHERE for partial index */
    expr_list_t *pUpsertSet;       /* SET clause for DO UPDATE */
    expr_t *pUpsertWhere;          /* WHERE clause for UPDATE */
    upsert_t *pNextUpsert;         /* Chain for multiple ON CONFLICT */
    u8 isDoUpdate;                 /* 1=DO UPDATE, 0=DO NOTHING */
    /* Runtime fields: */
    index_t *pUpsertIdx;           /* Resolved target index */
    src_list_t *pUpsertSrc;        /* Table being updated */
    int regData;                   /* First data register */
    int iDataCur;                  /* Data cursor number */
    int iIdxCur;                   /* First index cursor */
};
```

#### Functions to Implement

```c
/* Create new UPSERT clause */
upsert_t* __upsert_new(DBSQL *dbp, id_list_t *pTarget, expr_t *pTargetWhere,
                       expr_list_t *pSet, expr_t *pWhere, int isDoUpdate);

/* Delete UPSERT clause */
void __upsert_delete(DBSQL *dbp, upsert_t *pUpsert);

/* Resolve UPSERT target to a specific UNIQUE index or PRIMARY KEY */
int __upsert_resolve_target(parser_t *pParse, table_t *pTab, upsert_t *pUpsert);

/* Generate VDBE code for UPSERT conflict handling */
void __upsert_code_do_test(parser_t *pParse, upsert_t *pUpsert, int iTab,
                           int regData, int regRowid, int onError);
void __upsert_code_do_update(parser_t *pParse, upsert_t *pUpsert, table_t *pTab,
                             int iDataCur, int regData);

/* Resolve "excluded.*" references in SET and WHERE clauses */
int __upsert_resolve_excluded(parser_t *pParse, expr_t *pExpr, upsert_t *pUpsert);
```

### 4. INSERT Code Generator Updates

**File:** `/home/gburd/ws/dbsql/src/cg_insert.c`

#### Key Changes Needed

1. **Check for UPSERT clause** at start of __insert:
```c
void __insert(parser, tlist, vlist, select, column, on_error, returning)
    parser_t *parser;
    // ... other params ...
{
    upsert_t *pUpsert = parser->pUpsert;
    parser->pUpsert = NULL;  /* Claim ownership */

    // ... existing code ...

    if (pUpsert) {
        /* Resolve UPSERT target index */
        if (__upsert_resolve_target(parser, table, pUpsert) != 0) {
            goto insert_cleanup;
        }
    }

    // ... continue ...
}
```

2. **Modify constraint checking** to handle conflicts:
```c
/* In the data insertion loop, before __generate_constraint_checks */
if (pUpsert && pUpsert->isDoUpdate == 0) {
    /* DO NOTHING: Skip on conflict */
    __upsert_code_do_test(parser, pUpsert, base, regData, regRowid, OE_Ignore);
} else if (pUpsert && pUpsert->isDoUpdate) {
    /* DO UPDATE: Jump to update code on conflict */
    int lblUpdate = __vdbe_make_label(v);
    __upsert_code_do_test(parser, pUpsert, base, regData, regRowid, lblUpdate);

    /* Normal insert code */
    __vdbe_add_op3(v, OP_MakeRecord, regRowid, table->nCol + 1, regRec);
    __vdbe_add_op3(v, OP_Insert, base, regRec, regRowid);

    /* Skip update */
    int lblContinue = __vdbe_make_label(v);
    __vdbe_add_op(v, OP_Goto, 0, lblContinue);

    /* Update code */
    __vdbe_resolve_label(v, lblUpdate);
    __upsert_code_do_update(parser, pUpsert, table, base, regData);

    __vdbe_resolve_label(v, lblContinue);
}
```

3. **Handle `excluded.*` references**:
The SET clause expressions may reference `excluded.columnname`. These need to be resolved to the new values being inserted.

```c
/* In __upsert_code_do_update, for each column in SET clause: */
for (i = 0; i < pUpsert->pUpsertSet->nExpr; i++) {
    expr_t *pExpr = pUpsert->pUpsertSet->a[i].pExpr;
    if (__upsert_resolve_excluded(parser, pExpr, pUpsert) != 0) {
        return;  /* Error resolving excluded reference */
    }
    /* Generate code to evaluate expression and update column */
}
```

### 5. VDBE Opcode Strategy

The existing opcodes should be sufficient:

- **OP_SeekRowid**: Check if rowid exists (for PRIMARY KEY conflicts)
- **OP_NotExists**: Check if index entry exists (for UNIQUE conflicts)
- **OP_NotFound**: Similar to NotExists for index lookups
- **OP_IsUnique**: Check uniqueness of index entry
- **OP_Found**: Jump if key found in index
- **OP_Insert**: Insert record (used in both paths)
- **OP_Update**: Not currently defined - may need to add or use Delete+Insert pattern

#### Recommended Flow for DO UPDATE

```
1. Try to insert normally
2. If conflict detected:
   a. Seek to existing row (OP_SeekRowid or OP_Found)
   b. Read existing row data (OP_Column for needed columns)
   c. Evaluate UPDATE SET expressions with "excluded.*" bound to new values
   d. Generate updated record (OP_MakeRecord)
   e. Replace existing row (OP_Insert with same rowid, or OP_Delete + OP_Insert)
   f. Update all indices
```

#### Recommended Flow for DO NOTHING

```
1. Before insert, check for conflict:
   a. Use OP_NotFound on target index
   b. If found, skip insert (OP_Goto to end_of_loop)
   c. If not found, proceed with insert
```

### 6. Expression Resolution for `excluded.*`

**File:** `/home/gburd/ws/dbsql/src/cg_expr.c`

Need to handle TK_DOT expressions where left side is "excluded":

```c
/* In __expr_code or __expr_resolve_ids: */
case TK_DOT: {
    expr_t *pLeft = pExpr->left;
    expr_t *pRight = pExpr->right;

    if (pLeft && pLeft->op == TK_ID &&
        strcasecmp(pLeft->token.z, "excluded") == 0) {
        /* This is "excluded.column" */
        /* Resolve pRight to column in UPSERT values */
        int iCol = __resolve_column_name(parser, pTab, pRight->token.z);
        if (iCol < 0) {
            __error_msg(parser, "no such column: %s", pRight->token.z);
            return;
        }
        /* Generate code to load from pUpsert->regData + iCol */
        __vdbe_add_op(v, OP_Copy, pUpsert->regData + iCol, target_reg);
        return;
    }

    /* Normal DOT handling ... */
}
```

### 7. Test Cases

**File:** `/home/gburd/ws/dbsql/test/src/test_upsert.c` (NEW)

```c
#include "test_framework.h"
#include "dbsql.h"

/* Test DO NOTHING */
void test_upsert_do_nothing_simple(void) {
    DBSQL *dbp;
    assert(dbsql_create(&dbp, 0) == 0);

    exec_ok(dbp, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");
    exec_ok(dbp, "INSERT INTO t VALUES (1, 'Alice')");

    /* Should silently ignore conflict */
    exec_ok(dbp, "INSERT INTO t VALUES (1, 'Bob') ON CONFLICT DO NOTHING");

    /* Verify original value remains */
    assert_query_result(dbp, "SELECT name FROM t WHERE id=1", "Alice");

    dbsql_close(dbp);
}

/* Test DO NOTHING with conflict target */
void test_upsert_do_nothing_with_target(void) {
    DBSQL *dbp;
    assert(dbsql_create(&dbp, 0) == 0);

    exec_ok(dbp, "CREATE TABLE t (id INTEGER, name TEXT UNIQUE)");
    exec_ok(dbp, "INSERT INTO t VALUES (1, 'Alice')");

    /* Conflict on name column */
    exec_ok(dbp, "INSERT INTO t VALUES (2, 'Alice') ON CONFLICT(name) DO NOTHING");

    /* Only original row should exist */
    assert_query_count(dbp, "SELECT COUNT(*) FROM t", 1);

    dbsql_close(dbp);
}

/* Test DO UPDATE simple */
void test_upsert_do_update_simple(void) {
    DBSQL *dbp;
    assert(dbsql_create(&dbp, 0) == 0);

    exec_ok(dbp, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, count INTEGER)");
    exec_ok(dbp, "INSERT INTO t VALUES (1, 'Alice', 1)");

    /* Update on conflict */
    exec_ok(dbp, "INSERT INTO t VALUES (1, 'Alice', 1) "
                 "ON CONFLICT(id) DO UPDATE SET count = count + 1");

    assert_query_result(dbp, "SELECT count FROM t WHERE id=1", "2");

    dbsql_close(dbp);
}

/* Test excluded pseudo-table */
void test_upsert_excluded_reference(void) {
    DBSQL *dbp;
    assert(dbsql_create(&dbp, 0) == 0);

    exec_ok(dbp, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, value INTEGER)");
    exec_ok(dbp, "INSERT INTO t VALUES (1, 'Alice', 10)");

    /* Use excluded.* to reference new values */
    exec_ok(dbp, "INSERT INTO t VALUES (1, 'Bob', 20) "
                 "ON CONFLICT(id) DO UPDATE SET name = excluded.name, "
                 "value = t.value + excluded.value");

    assert_query_result(dbp, "SELECT name FROM t WHERE id=1", "Bob");
    assert_query_result(dbp, "SELECT value FROM t WHERE id=1", "30");

    dbsql_close(dbp);
}

/* Test WHERE clause in DO UPDATE */
void test_upsert_update_with_where(void) {
    DBSQL *dbp;
    assert(dbsql_create(&dbp, 0) == 0);

    exec_ok(dbp, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, value INTEGER)");
    exec_ok(dbp, "INSERT INTO t VALUES (1, 'Alice', 10)");

    /* Only update if condition is met */
    exec_ok(dbp, "INSERT INTO t VALUES (1, 'Bob', 5) "
                 "ON CONFLICT(id) DO UPDATE SET name = excluded.name "
                 "WHERE excluded.value > t.value");

    /* Should NOT update (5 is not > 10) */
    assert_query_result(dbp, "SELECT name FROM t WHERE id=1", "Alice");

    /* Now with condition that passes */
    exec_ok(dbp, "INSERT INTO t VALUES (1, 'Bob', 20) "
                 "ON CONFLICT(id) DO UPDATE SET name = excluded.name "
                 "WHERE excluded.value > t.value");

    /* Should update (20 > 10) */
    assert_query_result(dbp, "SELECT name FROM t WHERE id=1", "Bob");

    dbsql_close(dbp);
}

/* Test with SELECT source */
void test_upsert_from_select(void) {
    DBSQL *dbp;
    assert(dbsql_create(&dbp, 0) == 0);

    exec_ok(dbp, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)");
    exec_ok(dbp, "CREATE TABLE source (id INTEGER, name TEXT)");
    exec_ok(dbp, "INSERT INTO source VALUES (1, 'Alice'), (2, 'Bob'), (1, 'Charlie')");

    /* Upsert from SELECT */
    exec_ok(dbp, "INSERT INTO t SELECT * FROM source "
                 "ON CONFLICT(id) DO UPDATE SET name = excluded.name");

    /* id=1 should have 'Charlie' (last value), id=2 should have 'Bob' */
    assert_query_result(dbp, "SELECT name FROM t WHERE id=1", "Charlie");
    assert_query_result(dbp, "SELECT name FROM t WHERE id=2", "Bob");

    dbsql_close(dbp);
}

int main(void) {
    test_upsert_do_nothing_simple();
    test_upsert_do_nothing_with_target();
    test_upsert_do_update_simple();
    test_upsert_excluded_reference();
    test_upsert_update_with_where();
    test_upsert_from_select();

    printf("All UPSERT tests passed!\n");
    return 0;
}
```

### 8. Build Integration

Add to Makefile:
```make
UPSERT_OBJS = cg_upsert.o
test_upsert: test_upsert.o $(LIB_OBJS) $(UPSERT_OBJS)
    $(CC) $(CFLAGS) -o $@ $^ $(LIBS)
```

## Implementation Phases

### Phase 1: DO NOTHING (Simple)
1. [x] Add tokens (EXCLUDED)
2. [ ] Add parser grammar for `ON CONFLICT DO NOTHING`
3. [ ] Implement __upsert_new, __upsert_delete
4. [ ] Modify __insert to check for conflicts before insert
5. [ ] Test simple DO NOTHING cases

### Phase 2: DO NOTHING (With Target)
1. [ ] Implement __upsert_resolve_target
2. [ ] Handle conflict target column list
3. [ ] Map to specific UNIQUE index
4. [ ] Test DO NOTHING with specific columns

### Phase 3: DO UPDATE (Simple)
1. [ ] Add SET clause parsing to grammar
2. [ ] Implement __upsert_code_do_update
3. [ ] Generate update code on conflict
4. [ ] Test basic DO UPDATE

### Phase 4: EXCLUDED References
1. [ ] Implement __upsert_resolve_excluded
2. [ ] Handle `excluded.*` in expressions
3. [ ] Load new values into registers
4. [ ] Test excluded references

### Phase 5: WHERE Clauses
1. [ ] Handle WHERE in conflict target (partial indexes)
2. [ ] Handle WHERE in UPDATE clause
3. [ ] Test conditional updates

### Phase 6: Advanced Features
1. [ ] Support multiple ON CONFLICT clauses
2. [ ] Integration with SELECT sources
3. [ ] Integration with triggers
4. [ ] Comprehensive test suite

## SQLite Reference Implementation

Key files in SQLite 3.52.0:
- `parse.y`: Grammar for ON CONFLICT (lines ~1500-1550)
- `insert.c`: __upsert_* functions
- `build.c`: Constraint resolution
- `expr.c`: Expression handling for "excluded"
- `vdbe.c`: Opcode execution

## Notes

- UPSERT is a compile-time feature (code generation), not runtime
- The EXCLUDED pseudo-table is implemented through register mapping
- Multiple ON CONFLICT clauses are chained via pNextUpsert
- Partial index support requires WHERE clause evaluation
- The feature must work with both VALUES and SELECT sources
- Triggers should fire correctly (BEFORE/AFTER, INSERT vs UPDATE semantics)

## Testing Strategy

1. Unit tests for each component (parser, resolver, codegen)
2. Integration tests for complete UPSERT statements
3. Edge cases: multiple columns, partial indexes, NULL values
4. Performance tests: bulk UPSERT operations
5. Compatibility tests: compare with SQLite behavior

## Current Status

- **Tokenizer**: COMPLETE (EXCLUDED added)
- **Parser Grammar**: PENDING (needs grammar rules)
- **UPSERT Functions**: NOT STARTED (cg_upsert.c to be created)
- **INSERT Integration**: NOT STARTED (cg_insert.c modifications)
- **Expression Handling**: NOT STARTED (excluded.* resolution)
- **Tests**: NOT STARTED (test_upsert.c to be created)

**Next Step**: Add parser grammar rules for ON CONFLICT clauses.
