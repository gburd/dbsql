# UPSERT SQLite 3.52.0 Implementation Analysis

**Date**: 2026-03-18
**SQLite Version**: 3.52.0
**Purpose**: Detailed analysis of SQLite's UPSERT implementation for DBSQL port

---

## Executive Summary

SQLite 3.52.0 implements UPSERT (INSERT ON CONFLICT) using:
- **Parser**: Lemon grammar extensions for ON CONFLICT clause
- **AST**: `Upsert` structure linked list for multiple conflict clauses
- **Code Generation**: Integrated into insert.c with conflict detection logic
- **Runtime**: VDBE opcodes for index checking and conditional branching

**Complexity**: Medium-High (80-120 hours estimated implementation)

---

## 1. Parser Grammar Analysis

### 1.1 Grammar Rules (parse.y)

```y
// UPSERT clause attached to INSERT statement
insert_cmd ::= ... upsert(U).

// Multiple ON CONFLICT clauses can be chained
upsert ::= .
upsert ::= ON CONFLICT LP sortlist RP where_opt
           DO UPDATE SET setlist where_opt upsert.
upsert ::= ON CONFLICT LP sortlist RP where_opt
           DO NOTHING upsert.
upsert ::= ON CONFLICT DO NOTHING returning.
upsert ::= ON CONFLICT DO UPDATE SET setlist where_opt returning.
```

**Key Observations**:
1. **Recursive structure**: `upsert` can reference itself, allowing multiple ON CONFLICT clauses
2. **Conflict target**: `sortlist` + optional `where_opt` identifies which unique index/constraint
3. **Actions**: Either `DO NOTHING` or `DO UPDATE SET ...`
4. **Integration with RETURNING**: UPSERT can precede RETURNING clause
5. **No conflict target = implicit**: Matches INTEGER PRIMARY KEY or first unique index

### 1.2 Conflict Target Syntax

```sql
-- Conflict on specific column(s)
ON CONFLICT (id) DO UPDATE ...
ON CONFLICT (email, tenant_id) DO UPDATE ...

-- Conflict on partial index (with WHERE clause)
ON CONFLICT (email) WHERE active = 1 DO UPDATE ...

-- Conflict on any unique constraint (no target specified)
ON CONFLICT DO NOTHING
```

The `sortlist` in the grammar is actually a column list here, reusing existing grammar non-terminal.

---

## 2. AST Structure Analysis

### 2.1 Upsert Structure (sqliteInt.h:3565)

```c
struct Upsert {
  /* Parse tree for ON CONFLICT clause */
  ExprList *pUpsertTarget;      // Column list for conflict target
  Expr *pUpsertTargetWhere;     // WHERE clause for partial index
  ExprList *pUpsertSet;         // SET clause (NULL for DO NOTHING)
  Expr *pUpsertWhere;           // WHERE clause for UPDATE
  Upsert *pNextUpsert;          // Next ON CONFLICT clause
  u8 isDoUpdate;                // 1=DO UPDATE, 0=DO NOTHING
  u8 isDup;                     // True if redundant clause

  /* Intermediate data (freed by INSERT processing) */
  void *pToFree;                // Memory management

  /* Resolved at analysis time */
  Index *pUpsertIdx;            // Which index this targets
  SrcList *pUpsertSrc;          // Table being inserted
  int regData;                  // First register with VALUES
  int iDataCur;                 // Data table cursor
  int iIdxCur;                  // First index cursor
};
```

**Field Purposes**:
- **pUpsertTarget**: ExprList of columns identifying the unique constraint
- **pUpsertTargetWhere**: WHERE clause distinguishing partial indexes
- **pUpsertSet**: SET expressions for DO UPDATE (NULL means DO NOTHING)
- **pUpsertWhere**: WHERE filter for DO UPDATE action
- **pNextUpsert**: Linked list for multiple ON CONFLICT clauses
- **isDoUpdate**: Distinguishes UPDATE from NOTHING (derived from pUpsertSet)
- **isDup**: Marks redundant clauses (same index targeted twice)
- **pUpsertIdx**: Resolved to specific Index during analysis phase
- **regData/iDataCur/iIdxCur**: Code generation state passed down from INSERT

### 2.2 Lifecycle

1. **Parser**: Creates Upsert struct(s), links to Insert AST node
2. **Analysis**: `sqlite3UpsertAnalyzeTarget()` resolves pUpsertTarget to pUpsertIdx
3. **Code Generation**: `sqlite3GenerateConstraintChecks()` and `sqlite3UpsertDoUpdate()` emit VDBE
4. **Cleanup**: `sqlite3UpsertDelete()` frees memory

---

## 3. Conflict Resolution Analysis

### 3.1 Resolution Function (upsert.c:90)

`sqlite3UpsertAnalyzeTarget()` performs:

1. **Resolve symbolic names** in conflict target and WHERE clauses
2. **Match to table constraints**:
   - Check if target matches INTEGER PRIMARY KEY (rowid)
   - Iterate through unique indexes, comparing:
     - Number of columns matches
     - Column expressions match (accounting for collation)
     - Partial index WHERE clause matches (if present)
3. **Set pUpsertIdx** to matched Index*
4. **Detect duplicates**: Mark isDup if same index targeted by multiple clauses
5. **Error if no match**: "ON CONFLICT clause does not match any PRIMARY KEY or UNIQUE constraint"

**Complex Matching Logic**:
- Uses `sqlite3ExprCompare()` to match expressions (handles column order independence)
- Handles expression indexes (indexed expressions, not just column names)
- Handles collation sequences (must match index collation)
- Handles partial indexes (WHERE must match exactly)

### 3.2 Index Matching Algorithm

```
For each ON CONFLICT clause:
  For each UNIQUE index on table:
    IF index.nKeyCol != target.nExpr THEN continue

    IF index has partial WHERE clause:
      IF target has no WHERE THEN continue
      IF target WHERE != index WHERE THEN continue

    FOR each column in index:
      Find matching column in target (any order)
      IF no match found THEN break

    IF all columns matched:
      pUpsert->pUpsertIdx = index
      BREAK

  IF no index matched:
    ERROR "does not match any UNIQUE constraint"
```

---

## 4. Code Generation Strategy

### 4.1 Integration with INSERT (insert.c)

UPSERT code generation is interleaved with normal INSERT constraint checking:

```
1. Open table and index cursors

2. FOR each row to insert:

   a. Generate row data on stack

   b. Check NOT NULL constraints

   c. Check INTEGER PRIMARY KEY uniqueness:
      - IF conflict detected AND upsert applies:
        • DO NOTHING → skip row
        • DO UPDATE → call sqlite3UpsertDoUpdate()

   d. FOR each unique index:
      - Generate index key
      - Check uniqueness (OP_IsUnique)
      - IF conflict detected AND upsert applies to this index:
        • DO NOTHING → skip row
        • DO UPDATE → call sqlite3UpsertDoUpdate()

   e. IF no conflicts:
      - Insert row normally

3. Close cursors
```

### 4.2 DO UPDATE Implementation (upsert.c:267)

`sqlite3UpsertDoUpdate()` generates update code:

```
1. Position data cursor on conflicting row:
   - IF conflict on index: read rowid from index, seek to row
   - IF conflict on IPK: already positioned

2. Set up "excluded" pseudo-table:
   - excluded.* references the VALUES being inserted
   - Implemented via registers holding NEW values

3. Generate UPDATE code:
   - Evaluate SET expressions (can reference old.* and excluded.*)
   - Apply WHERE clause filter (if present)
   - Update row in place
   - Update all affected indexes
```

**Critical Detail**: The "excluded" table is not a real table. It's a virtual context where column references resolve to the registers holding the VALUES from the INSERT statement.

---

## 5. VDBE Opcode Sequences

### 5.1 DO NOTHING Example

```
# Check if rowid already exists
Dup                # Duplicate rowid
NotExists base     # Jump to label if rowid not found
  # Conflict detected
  Pop              # Remove rowid and row data
  Goto next_row    # Skip to next iteration
# label: no conflict, insert normally
PutIntKey base
```

### 5.2 DO UPDATE Example (Simplified)

```
# Check if rowid already exists
Dup                # Duplicate rowid
NotExists base     # Jump if not found
  # Conflict detected - DO UPDATE

  # Position on conflicting row (already there for IPK)

  # Set up excluded.* context (data already in registers)

  # Evaluate SET expressions
  Column excluded 0  # Read from "excluded" pseudo-table
  # ... more SET expression evaluation

  # Generate new row
  MakeRecord

  # Update in place
  PutIntKey base

  Goto after_insert

# label: no conflict
MakeRecord
PutIntKey base

# after_insert:
```

**Opcodes Used**:
- **OP_NotExists**: Check if row exists by rowid
- **OP_IsUnique**: Check if index key is unique
- **OP_IdxRowid**: Extract rowid from index cursor
- **OP_SeekRowid**: Position table cursor by rowid
- **OP_Column**: Read column from cursor (for old.* or excluded.*)
- **OP_MakeRecord**: Build row record
- **OP_PutIntKey**: Insert/update row
- **OP_IdxInsert**: Insert into index

---

## 6. Edge Cases and Corner Cases

### 6.1 Multiple ON CONFLICT Clauses

```sql
INSERT INTO t VALUES (1, 'foo', 'bar')
  ON CONFLICT (id) DO UPDATE SET name = excluded.name
  ON CONFLICT (email) DO NOTHING;
```

SQLite allows multiple clauses, each targeting a different unique constraint. The first matching constraint wins.

**Implementation**:
- Linked list of Upsert structs
- Each constraint check consults its specific ON CONFLICT clause
- `sqlite3UpsertOfIndex()` finds the clause for a given Index

### 6.2 Partial Index Conflicts

```sql
CREATE UNIQUE INDEX idx_active_email ON users(email) WHERE active = 1;

INSERT INTO users VALUES (1, 'user@example.com', 1)
  ON CONFLICT (email) WHERE active = 1
  DO UPDATE SET updated_at = current_timestamp;
```

The WHERE clause must match the partial index WHERE clause exactly.

### 6.3 Expression Indexes

```sql
CREATE UNIQUE INDEX idx_lower_email ON users(lower(email));

INSERT INTO users VALUES (1, 'USER@EXAMPLE.COM')
  ON CONFLICT (lower(email)) DO UPDATE SET last_login = current_timestamp;
```

Target can reference expressions, not just column names.

### 6.4 WITHOUT ROWID Tables

For WITHOUT ROWID tables, the PRIMARY KEY is stored in the table itself, not as a separate rowid. Conflict detection uses the primary key index.

### 6.5 Triggers and UPSERT

- BEFORE INSERT triggers fire before conflict checking
- AFTER INSERT triggers fire only if row is actually inserted (not updated)
- UPDATE triggers fire if DO UPDATE executes
- Interaction is complex, requires careful ordering

---

## 7. BerkeleyDB Considerations for DBSQL

### 7.1 Unique Key Checking

SQLite b-tree has `sqlite3BtreeInsert()` which returns conflict status.

BerkeleyDB has:
- **DB->put() with DB_NOOVERWRITE**: Returns DB_KEYEXIST if key exists
- **DB->exists()**: Check existence without retrieving data
- **DBC->get() with DB_SET**: Position on key, returns DB_NOTFOUND if absent

**Strategy for DBSQL**:
- Use `DB->exists()` or `DBC->get()` for conflict detection
- On conflict, decide DO NOTHING vs DO UPDATE
- For DO UPDATE, use `DBC->put()` to update in place

### 7.2 Transaction Semantics

SQLite supports statement-level rollback (OP_Savepoint, OP_Statement).

BerkeleyDB supports nested transactions (DB_TXN child transactions).

**Strategy**:
- Use child transactions for statement-level rollback
- On conflict with DO UPDATE, don't roll back - just update
- On conflict with DO NOTHING, don't roll back - just skip

### 7.3 Index Maintenance

SQLite automatically maintains secondary indexes.

BerkeleyDB requires manual index maintenance in DBSQL's storage manager.

**Strategy**:
- On DO UPDATE, delete old index entries, insert new ones
- Reuse existing `__generate_row_index_delete()` and index insertion logic
- Ensure index updates are transactional

---

## 8. Performance Considerations

### 8.1 Overhead on Normal INSERT

UPSERT adds conditional branches to INSERT path:
- Check if ON CONFLICT clause applies
- If not, follow normal path (minimal overhead)

**Mitigation**: Fast path when pUpsert == NULL.

### 8.2 Conflict Detection Cost

Checking for conflicts requires index lookups:
- INTEGER PRIMARY KEY: OP_NotExists (btree lookup)
- Unique indexes: OP_IsUnique (btree lookup)

These are the same lookups normal INSERT does for constraint enforcement, so no extra cost.

### 8.3 DO UPDATE Cost

Updating in place requires:
- Reading existing row data (for old.* references)
- Evaluating SET expressions
- Updating row and indexes

This is essentially an UPDATE operation, same cost as regular UPDATE.

---

## 9. Implementation Complexity Assessment

### 9.1 Difficulty Factors

| Component | Complexity | Reason |
|-----------|-----------|--------|
| **Parser** | Low | Grammar extension is straightforward |
| **AST** | Low | Simple linked list structure |
| **Analysis** | Medium | Index matching logic is intricate |
| **Code Generation** | High | Interleaving with INSERT constraint checks |
| **excluded.* Context** | Medium | Virtual table context for expressions |
| **Multi-clause** | Medium | Managing multiple ON CONFLICT clauses |
| **BDB Integration** | Medium | Adapting to BDB's key existence checking |

### 9.2 Estimated Effort Breakdown

| Phase | Hours | Details |
|-------|-------|---------|
| Parser + AST | 15-20 | Grammar, structures, memory management |
| Analysis | 10-15 | Index matching, constraint resolution |
| Basic DO NOTHING | 10-15 | Conflict detection, skip logic |
| Basic DO UPDATE | 20-25 | Update code gen, excluded.* context |
| Conflict Target | 15-20 | Partial indexes, expressions, WHERE |
| Multi-clause | 10-15 | Linked list handling, clause selection |
| Testing | 15-20 | Comprehensive test suite |
| **Total** | **95-130** | Conservative estimate |

---

## 10. Key Takeaways for DBSQL Implementation

### 10.1 Must-Have Features

1. **Parser grammar extension** for ON CONFLICT clause
2. **Upsert AST structure** with linked list for multiple clauses
3. **Index matching algorithm** to resolve conflict targets
4. **Conflict detection** integrated into constraint checking
5. **DO NOTHING** implementation (simplest case)
6. **DO UPDATE** with excluded.* pseudo-table context
7. **BerkeleyDB integration** for conflict detection

### 10.2 Nice-to-Have Features

1. **Partial index support** (WHERE clause on conflict target)
2. **Expression index support** (indexed expressions in target)
3. **Multiple ON CONFLICT clauses** (different constraints)
4. **WHERE clause on DO UPDATE** (conditional updates)

### 10.3 Can Defer

1. **WITHOUT ROWID table support** (DBSQL doesn't have this yet)
2. **Trigger interaction** (get basic case working first)
3. **Optimization** (focus on correctness first)

---

## 11. Reference Files in SQLite Source

| File | Purpose |
|------|---------|
| `src/parse.y` | Grammar for ON CONFLICT (lines 1091-1108) |
| `src/sqliteInt.h` | Upsert struct definition (lines 3565-3586) |
| `src/upsert.c` | Upsert analysis and DO UPDATE code gen |
| `src/insert.c` | UPSERT integration with INSERT (lines 900+) |
| `test/upsert*.test` | Comprehensive test cases |

---

## 12. Next Steps

1. Review this analysis with implementation team
2. Create detailed design documents for each phase
3. Set up test infrastructure
4. Begin with parser + AST implementation
5. Proceed through phases incrementally

---

**Document Status**: ✅ COMPLETE
**Review Date**: 2026-03-18
**Next Review**: Before implementation kickoff
