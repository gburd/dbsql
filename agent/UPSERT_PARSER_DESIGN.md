# UPSERT Parser Grammar Design

**Date**: 2026-03-18
**Target**: DBSQL Lemon parser (src/sql_parser.y)
**Purpose**: Grammar extensions for INSERT ON CONFLICT clause

---

## Executive Summary

Add UPSERT syntax to DBSQL parser following SQL:2011 and SQLite 3.52.0:

```sql
INSERT INTO table VALUES (...) ON CONFLICT (columns) DO UPDATE SET ...;
INSERT INTO table VALUES (...) ON CONFLICT (columns) DO NOTHING;
INSERT INTO table VALUES (...) ON CONFLICT DO NOTHING;
```

**Complexity**: Low-Medium (grammar is straightforward, integration requires care)
**Estimated Effort**: 15-20 hours

---

## 1. Grammar Rules

### 1.1 Top-Level INSERT Statement

**Current** (sql_parser.y):
```y
cmd ::= insert_cmd.

insert_cmd ::= INSERT orconf(R) INTO fullname(X) inscollist_opt(F)
               VALUES LP exprlist(Y) RP.
               {
                   __insert(&parser, X, Y, 0, F, R);
               }

insert_cmd ::= INSERT orconf(R) INTO fullname(X) inscollist_opt(F)
               select(S).
               {
                   __insert(&parser, X, 0, S, F, R);
               }
```

**New** (with UPSERT):
```y
cmd ::= insert_cmd.

insert_cmd ::= INSERT orconf(R) INTO fullname(X) inscollist_opt(F)
               VALUES LP exprlist(Y) RP upsert(U).
               {
                   __insert(&parser, X, Y, 0, F, R, U);
               }

insert_cmd ::= INSERT orconf(R) INTO fullname(X) inscollist_opt(F)
               select(S) upsert(U).
               {
                   __insert(&parser, X, 0, S, F, R, U);
               }
```

**Changes**:
- Add `upsert(U)` non-terminal to end of INSERT statement
- Pass `U` (upsert_t*) to `__insert()` function
- Update `__insert()` signature to accept upsert parameter

### 1.2 UPSERT Non-Terminal

```y
%type upsert {upsert_t*}

// Base case: no UPSERT clause
upsert(A) ::= .
              {
                  A = 0;
              }

// ON CONFLICT with target and DO NOTHING
upsert(A) ::= ON CONFLICT LP sortlist(T) RP where_opt(TW)
              DO NOTHING upsert(N).
              {
                  A = __upsert_new(&parser, T, TW, 0, 0, N);
              }

// ON CONFLICT with target and DO UPDATE
upsert(A) ::= ON CONFLICT LP sortlist(T) RP where_opt(TW)
              DO UPDATE SET setlist(Z) where_opt(W) upsert(N).
              {
                  A = __upsert_new(&parser, T, TW, Z, W, N);
              }

// ON CONFLICT without target (implicit), DO NOTHING
upsert(A) ::= ON CONFLICT DO NOTHING upsert(N).
              {
                  A = __upsert_new(&parser, 0, 0, 0, 0, N);
              }

// ON CONFLICT without target (implicit), DO UPDATE
upsert(A) ::= ON CONFLICT DO UPDATE SET setlist(Z) where_opt(W) upsert(N).
              {
                  A = __upsert_new(&parser, 0, 0, Z, W, N);
              }
```

**Notes**:
1. **Recursive structure**: `upsert(N)` allows chaining multiple ON CONFLICT clauses
2. **sortlist(T)**: Reuses existing non-terminal for column list
3. **setlist(Z)**: Reuses existing non-terminal from UPDATE statement
4. **where_opt(TW)**: Optional WHERE on conflict target (partial index)
5. **where_opt(W)**: Optional WHERE on UPDATE action

### 1.3 RETURNING Clause Integration

SQLite allows RETURNING after UPSERT:

```y
// RETURNING after UPSERT
upsert(A) ::= RETURNING selcollist(X).
              {
                  A = 0;
                  __add_returning(&parser, X);
              }

// ON CONFLICT ... RETURNING
upsert(A) ::= ON CONFLICT DO NOTHING returning.
              {
                  A = __upsert_new(&parser, 0, 0, 0, 0, 0);
                  // RETURNING handled separately
              }

upsert(A) ::= ON CONFLICT DO UPDATE SET setlist(Z) where_opt(W) returning.
              {
                  A = __upsert_new(&parser, 0, 0, Z, W, 0);
                  // RETURNING handled separately
              }
```

**Decision for DBSQL**:
- Implement UPSERT first, add RETURNING later
- RETURNING is separate feature (Task #6)
- Can be implemented in parallel or after UPSERT

---

## 2. AST Constructor Function

### 2.1 Function Signature

```c
/*
 * __upsert_new --
 *     Create a new upsert_t structure for ON CONFLICT clause.
 *
 * PUBLIC: upsert_t *__upsert_new __P((parser_t *, expr_list_t *,
 * PUBLIC:     expr_t *, expr_list_t *, expr_t *, upsert_t *));
 *
 * parser       Parser context
 * target       Conflict target column list (NULL if no target specified)
 * target_where WHERE clause for partial index (NULL if none)
 * set_list     SET expressions for DO UPDATE (NULL for DO NOTHING)
 * update_where WHERE clause for DO UPDATE (NULL if none)
 * next         Next ON CONFLICT clause (NULL if last)
 *
 * RETURNS: Newly allocated upsert_t*, or NULL on OOM
 */
upsert_t *__upsert_new(
    parser_t *parser,
    expr_list_t *target,
    expr_t *target_where,
    expr_list_t *set_list,
    expr_t *update_where,
    upsert_t *next
);
```

### 2.2 Implementation

```c
upsert_t *__upsert_new(
    parser_t *parser,
    expr_list_t *target,
    expr_t *target_where,
    expr_list_t *set_list,
    expr_t *update_where,
    upsert_t *next
)
{
    upsert_t *upsert;
    DBSQL *dbp = parser->db;

    __dbsql_calloc(dbp, 1, sizeof(upsert_t), &upsert);
    if (upsert == 0) {
        // OOM - clean up arguments
        __expr_list_delete(target);
        __expr_delete(target_where);
        __expr_list_delete(set_list);
        __expr_delete(update_where);
        __upsert_delete(next);
        return 0;
    }

    upsert->target = target;
    upsert->target_where = target_where;
    upsert->set_list = set_list;
    upsert->update_where = update_where;
    upsert->next = next;
    upsert->is_do_update = (set_list != 0);

    return upsert;
}
```

---

## 3. Token Additions

### 3.1 New Keywords

Must add to token list (src/inc/sql_tokens.h):

```
TK_CONFLICT   - "CONFLICT" keyword
TK_EXCLUDED   - "excluded" pseudo-table name (for excluded.*)
```

**Note**: DO, NOTHING, UPDATE, SET, WHERE already exist as keywords.

### 3.2 Token Definitions

```c
// sql_tokenize.c keyword table
static const struct {
    const char *name;
    int token;
} keyword_table[] = {
    // ... existing keywords ...
    { "CONFLICT",  TK_CONFLICT },
    { "EXCLUDED",  TK_EXCLUDED },
    // ... rest of keywords ...
};
```

---

## 4. Integration with Existing Parser

### 4.1 orconf vs ON CONFLICT

DBSQL currently has two conflict resolution mechanisms:

**Global (orconf)**:
```sql
INSERT OR REPLACE INTO t VALUES (1, 'foo');
INSERT OR IGNORE INTO t VALUES (1, 'foo');
```

**Targeted (ON CONFLICT)**:
```sql
INSERT INTO t VALUES (1, 'foo') ON CONFLICT (id) DO UPDATE SET name='bar';
```

**Can they coexist?**

SQLite 3.52.0 allows both:
```sql
-- This is valid in SQLite:
INSERT OR ABORT INTO t VALUES (1, 'foo') ON CONFLICT (id) DO UPDATE ...;
```

The `orconf` applies to all constraints not covered by ON CONFLICT clauses.

**Recommendation for DBSQL**: Allow both, match SQLite behavior.

### 4.2 Grammar Precedence

Potential parsing ambiguity:
```sql
INSERT INTO tab SELECT * FROM aaa JOIN bbb ON ...
```

Is `ON` the start of `ON CONFLICT` or part of `JOIN ... ON`?

**SQLite Solution**: The ON CONFLICT comes after the entire SELECT, so no ambiguity:
```sql
INSERT INTO tab SELECT * FROM aaa JOIN bbb ON cond WHERE true
  ON CONFLICT (id) DO UPDATE ...;
```

**DBSQL Status**: Same structure, no ambiguity.

---

## 5. Parser Actions

### 5.1 Memory Management

```y
// Destructor for upsert (called on parse error)
%destructor upsert {
    __upsert_delete($$);
}
```

### 5.2 Error Handling

Parser must handle:
1. **Syntax errors** in ON CONFLICT clause
2. **Multiple DO UPDATE** without target (ambiguous)
3. **Empty SET list** (should be DO NOTHING instead)

Example error checks:
```c
// In __upsert_new():
if (set_list != 0 && set_list->nExpr == 0) {
    __error_msg(parser, "DO UPDATE requires at least one SET clause");
    parser->nErr++;
    __expr_list_delete(set_list);
    set_list = 0;  // Treat as DO NOTHING
}

if (target == 0 && next != 0 && next->target == 0) {
    __error_msg(parser, "multiple ON CONFLICT clauses without target");
    parser->nErr++;
}
```

---

## 6. Compatibility Considerations

### 6.1 Backward Compatibility

**Will existing queries break?**

No. The new grammar is additive:
- Existing INSERT statements work unchanged
- ON CONFLICT is optional
- If not present, parser matches base case `upsert ::= .`

### 6.2 Reserved Words

"CONFLICT" is not currently a reserved word in DBSQL. Adding it could break:
```sql
-- If someone named a column "conflict":
CREATE TABLE t (conflict TEXT);
INSERT INTO t VALUES ('foo');
```

**SQLite Approach**: CONFLICT is semi-reserved (contextual keyword).

**Recommendation**: Make CONFLICT contextual:
- Reserved in INSERT context after ON
- Not reserved elsewhere

Implementation:
```y
// Not as a reserved keyword, but recognized in context:
upsert ::= ON CONFLICT ...  // Parser recognizes "CONFLICT" here
```

Lemon parser handles this automatically via context.

---

## 7. Testing Strategy

### 7.1 Parser Tests

Test grammar recognition:

```sql
-- Basic DO NOTHING
INSERT INTO t VALUES (1) ON CONFLICT DO NOTHING;

-- Basic DO UPDATE
INSERT INTO t VALUES (1) ON CONFLICT (id) DO UPDATE SET x=1;

-- Multiple clauses
INSERT INTO t VALUES (1) ON CONFLICT (id) DO UPDATE SET x=1
                          ON CONFLICT (email) DO NOTHING;

-- With WHERE on target
INSERT INTO t VALUES (1) ON CONFLICT (email) WHERE active=1
                          DO UPDATE SET last_seen=now();

-- With WHERE on UPDATE
INSERT INTO t VALUES (1) ON CONFLICT (id)
                          DO UPDATE SET x=1 WHERE x<10;

-- Syntax errors (should fail)
INSERT INTO t VALUES (1) ON CONFLICT (id) DO;  -- incomplete
INSERT INTO t VALUES (1) ON CONFLICT;          -- incomplete
INSERT INTO t VALUES (1) ON CONFLICT DO UPDATE; -- no SET
```

### 7.2 AST Validation Tests

After parsing, validate AST structure:
- upsert_t* is non-NULL
- Fields are correctly populated
- Linked list is correct for multiple clauses

---

## 8. Implementation Checklist

### 8.1 Files to Modify

| File | Changes Required | Effort |
|------|------------------|--------|
| `src/sql_parser.y` | Add grammar rules | 3-4h |
| `src/sql_tokenize.c` | Add keywords | 1h |
| `src/inc/sql_tokens.h` | Token definitions | 1h |
| `src/cg_insert.c` | Add __upsert_new() | 2-3h |
| `src/cg_insert.c` | Update __insert() signature | 2h |
| `src/inc/dbsql_int.in` | Add upsert_t forward decl | 1h |
| **Total** | | **10-12h** |

### 8.2 Testing

| Test Type | Effort |
|-----------|--------|
| Grammar recognition tests | 2-3h |
| AST validation tests | 2-3h |
| Error handling tests | 1-2h |
| **Total** | **5-8h** |

---

## 9. Example Grammar Integration

### 9.1 Complete Grammar Fragment

```y
// =====================================================
// UPSERT: ON CONFLICT clause for INSERT
// =====================================================

%type upsert {upsert_t*}

upsert(A) ::= .
    { A = 0; }

upsert(A) ::= ON CONFLICT LP sortlist(T) RP where_opt(TW)
              DO NOTHING upsert(N).
    { A = __upsert_new(&parser, T, TW, 0, 0, N); }

upsert(A) ::= ON CONFLICT LP sortlist(T) RP where_opt(TW)
              DO UPDATE SET setlist(Z) where_opt(W) upsert(N).
    { A = __upsert_new(&parser, T, TW, Z, W, N); }

upsert(A) ::= ON CONFLICT DO NOTHING upsert(N).
    { A = __upsert_new(&parser, 0, 0, 0, 0, N); }

upsert(A) ::= ON CONFLICT DO UPDATE SET setlist(Z) where_opt(W) upsert(N).
    { A = __upsert_new(&parser, 0, 0, Z, W, N); }

%destructor upsert { __upsert_delete($$); }

// =====================================================
// INSERT statement (modified to accept upsert)
// =====================================================

cmd ::= insert_cmd.

insert_cmd ::= INSERT orconf(R) INTO fullname(X) inscollist_opt(F)
               VALUES LP exprlist(Y) RP upsert(U).
    { __insert(&parser, X, Y, 0, F, R, U); }

insert_cmd ::= INSERT orconf(R) INTO fullname(X) inscollist_opt(F)
               select(S) upsert(U).
    { __insert(&parser, X, 0, S, F, R, U); }
```

---

## 10. Potential Issues and Solutions

### 10.1 Issue: sortlist vs Column List

**Problem**: `sortlist` includes ORDER BY direction (ASC/DESC), but conflict target shouldn't.

**SQLite Solution**: Ignores ORDER BY direction in conflict target context.

**DBSQL Solution**: Same - accept sortlist grammar, ignore direction in analysis phase.

### 10.2 Issue: SET Clause Expressions

**Problem**: SET clause may reference `excluded.*`, which doesn't exist yet.

**Solution**: Parser doesn't validate table references. Analysis phase handles this.

### 10.3 Issue: Multiple ON CONFLICT Without Target

**Problem**:
```sql
INSERT INTO t VALUES (1)
  ON CONFLICT DO UPDATE SET x=1
  ON CONFLICT DO NOTHING;  -- Which constraint does this apply to?
```

**SQLite Behavior**: Error if ambiguous. Each ON CONFLICT must specify target, OR only one ON CONFLICT allowed without target.

**DBSQL Solution**: Match SQLite - check in analysis phase.

---

## 11. Next Steps

1. ✅ Complete grammar design
2. ⏸️ Implement `__upsert_new()` function
3. ⏸️ Add keywords to tokenizer
4. ⏸️ Modify INSERT grammar rules
5. ⏸️ Update `__insert()` signature
6. ⏸️ Write parser tests
7. ⏸️ Proceed to AST design phase

---

**Document Status**: ✅ COMPLETE
**Estimated Effort**: 15-20 hours
**Confidence**: HIGH
**Dependencies**: None (can start immediately)
