# UPSERT Implementation - Next Steps Quick Guide

## What's Been Done

1. **Tokens Added** - TK_EXCLUDED token for `excluded.*` references
2. **Core Functions Implemented** - Full cg_upsert.c module with:
   - `__upsert_new()` - Create UPSERT structures
   - `__upsert_delete()` - Free UPSERT structures
   - `__upsert_resolve_target()` - Match conflict targets to indices
   - `__upsert_code_do_test()` - Generate conflict detection VDBE code
   - `__upsert_code_do_update()` - Generate UPDATE VDBE code
   - `__id_list_to_expr_list()` - Helper for column list conversion
3. **Headers Updated** - Function prototypes added to dbsql_ext.h
4. **Documentation Complete** - Full implementation guide and status tracking

## What's Next - Critical Path

### Step 1: Update Parser Structure (src/sql_parser.y)

Add `pUpsert` field to `parser_t` structure in dbsql_int.h:

```c
struct parser {
    // ... existing fields ...
    upsert_t *pUpsert;           /* UPSERT clause for current INSERT */
    // ... rest of fields ...
};
```

### Step 2: Add Grammar Rules (src/sql_parser.y)

Insert BEFORE the INSERT command section (around line 504):

```yacc
// UPSERT clause for INSERT statements
%type upsert {upsert_t*}
%destructor upsert {__upsert_delete(pParse->db, $$);}

upsert ::= .  {
    pParse->pUpsert = 0;
}

upsert ::= ON CONFLICT LP idxlist(T) RP where_opt(TW) DO UPDATE SET setlist(Z) where_opt(W). {
    pParse->pUpsert = __upsert_new(pParse->db, T, TW, Z, W, 1);
}

upsert ::= ON CONFLICT LP idxlist(T) RP where_opt(TW) DO NOTHING. {
    pParse->pUpsert = __upsert_new(pParse->db, T, TW, 0, 0, 0);
}

upsert ::= ON CONFLICT DO NOTHING. {
    pParse->pUpsert = __upsert_new(pParse->db, 0, 0, 0, 0, 0);
}
```

Update INSERT commands to include `upsert` non-terminal:

```yacc
cmd ::= insert_cmd(R) INTO nm(X) dbnm(D) inscollist_opt(F)
        VALUES LP itemlist(Y) RP upsert.
            {__insert(pParse, __src_list_append(0,&X,&D), Y, 0, F, R, 0);}
cmd ::= insert_cmd(R) INTO nm(X) dbnm(D) inscollist_opt(F) select(S) upsert.
            {__insert(pParse, __src_list_append(0,&X,&D), 0, S, F, R, 0);}
```

### Step 3: Integrate into INSERT (src/cg_insert.c)

#### 3a. Claim UPSERT at function start (after line 160):

```c
void __insert(parser, tlist, vlist, select, column, on_error, returning)
    // ... parameter declarations ...
{
    // ... existing variable declarations ...
    upsert_t *pUpsert;

    if (parser->nErr)
        goto insert_cleanup;

    dbp = parser->db;

    /* Claim ownership of UPSERT clause */
    pUpsert = parser->pUpsert;
    parser->pUpsert = NULL;

    // ... rest of function ...
}
```

#### 3b. Resolve UPSERT target (after table validation, around line 196):

```c
    if (table == 0)
        goto insert_cleanup;

    /* Resolve UPSERT conflict target */
    if (pUpsert) {
        if (__upsert_resolve_target(parser, table, pUpsert) != 0) {
            goto insert_cleanup;
        }
    }

    // ... continue with existing code ...
```

#### 3c. Add UPSERT code generation (replace lines 620-626):

REPLACE THIS:
```c
    __vdbe_add_op3(v, OP_MakeRecord, regRowid, table->nCol + 1, regRec);
    __vdbe_add_op3(v, OP_Insert, base, regRec, regRowid);
```

WITH THIS:
```c
    /* Handle UPSERT or normal INSERT */
    if (pUpsert) {
        if (!pUpsert->isDoUpdate) {
            /* DO NOTHING: skip insert on conflict */
            __upsert_code_do_test(parser, pUpsert, base, regData, regRowid, end_of_loop);
            __vdbe_add_op3(v, OP_MakeRecord, regRowid, table->nCol + 1, regRec);
            __vdbe_add_op3(v, OP_Insert, base, regRec, regRowid);
        } else {
            /* DO UPDATE: jump to update code on conflict */
            int lblUpdate = __vdbe_make_label(v);
            int lblContinue = __vdbe_make_label(v);

            __upsert_code_do_test(parser, pUpsert, base, regData, regRowid, lblUpdate);

            /* Normal insert path */
            __vdbe_add_op3(v, OP_MakeRecord, regRowid, table->nCol + 1, regRec);
            __vdbe_add_op3(v, OP_Insert, base, regRec, regRowid);
            __vdbe_add_op(v, OP_Goto, 0, lblContinue);

            /* Update path */
            __vdbe_resolve_label(v, lblUpdate);
            pUpsert->regData = regData;
            pUpsert->iDataCur = base;
            __upsert_code_do_update(parser, pUpsert, table, base, regData);

            __vdbe_resolve_label(v, lblContinue);
        }
    } else {
        /* Normal INSERT (no UPSERT) */
        __vdbe_add_op3(v, OP_MakeRecord, regRowid, table->nCol + 1, regRec);
        __vdbe_add_op3(v, OP_Insert, base, regRec, regRowid);
    }
```

#### 3d. Add cleanup (in insert_cleanup label, around line 697):

```c
  insert_cleanup:
    if (pUpsert) __upsert_delete(dbp, pUpsert);
    if (aRegIdx) __dbsql_free(dbp, aRegIdx);
    // ... rest of cleanup ...
```

### Step 4: Build and Test

```bash
cd /home/gburd/ws/dbsql
cd build_unix.old

# Rebuild parser
../dist/lemon ../src/sql_parser.y

# Rebuild
make

# Simple test (after implementing test):
./test_upsert
```

## Minimal Test to Start

Create `/home/gburd/ws/dbsql/test/src/test_upsert_minimal.c`:

```c
#include <stdio.h>
#include <assert.h>
#include "dbsql.h"

int main(void) {
    DBSQL *dbp;
    char *errmsg = NULL;
    int rc;

    /* Create database */
    rc = dbsql_create(&dbp, 0);
    assert(rc == 0);

    /* Create table */
    rc = dbsql_exec(dbp, "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)", NULL, NULL, &errmsg);
    if (rc != 0) {
        printf("CREATE TABLE failed: %s\n", errmsg);
        return 1;
    }

    /* Test 1: Normal insert */
    rc = dbsql_exec(dbp, "INSERT INTO t VALUES (1, 'Alice')", NULL, NULL, &errmsg);
    if (rc != 0) {
        printf("INSERT failed: %s\n", errmsg);
        return 1;
    }

    /* Test 2: UPSERT DO NOTHING (should parse but may not work yet) */
    rc = dbsql_exec(dbp, "INSERT INTO t VALUES (1, 'Bob') ON CONFLICT DO NOTHING",
                    NULL, NULL, &errmsg);
    if (rc != 0) {
        printf("UPSERT DO NOTHING failed (expected at this stage): %s\n", errmsg);
        /* Don't return 1 yet - parsing is the first milestone */
    } else {
        printf("UPSERT DO NOTHING parsed successfully!\n");
    }

    dbsql_close(dbp);
    printf("Basic UPSERT parsing test complete\n");
    return 0;
}
```

Compile:
```bash
gcc -I../../src/inc -I.. -o test_upsert_minimal test_upsert_minimal.c \
    ../cg_upsert.o ../*.o -ldb
```

## Key Files Reference

| File | Purpose |
|------|---------|
| `/home/gburd/ws/dbsql/src/sql_parser.y` | Grammar rules |
| `/home/gburd/ws/dbsql/src/cg_insert.c` | INSERT code generator |
| `/home/gburd/ws/dbsql/src/cg_upsert.c` | UPSERT support (DONE) |
| `/home/gburd/ws/dbsql/src/cg_expr.c` | Expression eval (for excluded.*) |
| `/home/gburd/ws/dbsql/build_unix.old/dbsql_int.h` | Add pUpsert to parser_t |

## Common Issues to Watch For

1. **Parser Conflicts** - May need to adjust precedence for ON CONFLICT
2. **Token Order** - Make sure TK_DO and TK_NOTHING are recognized
3. **Memory Leaks** - Ensure pUpsert is freed in all error paths
4. **Register Allocation** - Verify regData points to correct registers
5. **Index Cursor Numbers** - Ensure iIdxCur doesn't conflict with base cursors

## Verification Checklist

- [ ] Parser compiles without conflicts
- [ ] Simple INSERT still works
- [ ] INSERT ... ON CONFLICT DO NOTHING parses
- [ ] INSERT ... ON CONFLICT(col) DO NOTHING parses
- [ ] INSERT ... ON CONFLICT DO UPDATE parses
- [ ] Actual conflict detection works
- [ ] DO NOTHING skips duplicate insert
- [ ] DO UPDATE updates existing row
- [ ] excluded.* references work

## Success Criteria

**Milestone 1 (Parsing):** All UPSERT syntax parses without errors
**Milestone 2 (DO NOTHING):** Duplicate inserts are silently skipped
**Milestone 3 (DO UPDATE):** Conflicts trigger updates correctly
**Milestone 4 (excluded.*):** Can reference new values in UPDATE

## Time Estimates

- Grammar changes: 30 minutes
- INSERT integration: 1-2 hours
- Testing and debugging: 2-3 hours
- **Total for basic DO NOTHING: 4-6 hours**

Add 4-6 hours more for DO UPDATE with excluded.* support.

## Getting Help

If stuck, check:
1. `/home/gburd/ws/dbsql/test/src/UPSERT_IMPLEMENTATION.md` - Full design
2. `/home/gburd/ws/dbsql/test/src/UPSERT_STATUS.md` - Current status
3. SQLite 3.52.0 source code - Reference implementation
4. Recent git history - Pattern for register-based VDBE conversions

---

**Remember:** Start small. Get parsing working first, then conflict detection, then DO NOTHING, then DO UPDATE, then excluded.*.

**Next Session:** Focus on Step 1-3 (grammar and basic integration). Don't worry about excluded.* yet.
