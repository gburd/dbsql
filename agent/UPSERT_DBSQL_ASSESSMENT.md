# UPSERT DBSQL Architecture Assessment

**Date**: 2026-03-18
**DBSQL Branch**: claude
**Purpose**: Assess DBSQL readiness for UPSERT implementation

---

## Executive Summary

DBSQL already has **strong foundation** for UPSERT:
- ✅ Conflict resolution framework (OE_* actions)
- ✅ Unique constraint checking infrastructure
- ✅ BerkeleyDB key existence checking
- ✅ Index maintenance machinery
- ⚠️ **Missing**: Parser support, excluded.* context, DO UPDATE code generation

**Readiness**: 60% - Core infrastructure exists, needs new code paths

---

## 1. Current Conflict Handling

### 1.1 Existing ON CONFLICT Resolution

DBSQL inherited SQLite's conflict resolution system:

```c
// src/inc/dbsql_int.in (conflict resolution modes)
#define OE_None     0   // No constraint (no-op)
#define OE_Rollback 1   // ROLLBACK transaction
#define OE_Abort    2   // ABORT statement (default)
#define OE_Fail     3   // FAIL (continue with next row)
#define OE_Ignore   4   // IGNORE (skip conflicting row)
#define OE_Replace  5   // REPLACE (delete old, insert new)
```

**Current SQL Support**:
```sql
-- These already work in DBSQL:
INSERT OR REPLACE INTO t VALUES (1, 'foo');
INSERT OR IGNORE INTO t VALUES (1, 'foo');
INSERT OR ABORT INTO t VALUES (1, 'foo');
INSERT OR FAIL INTO t VALUES (1, 'foo');
INSERT OR ROLLBACK INTO t VALUES (1, 'foo');

-- Column-level constraints:
CREATE TABLE t (
  id INTEGER PRIMARY KEY,
  email TEXT UNIQUE ON CONFLICT REPLACE,
  name TEXT NOT NULL ON CONFLICT ABORT
);
```

### 1.2 Gap Analysis

| Feature | Status | Notes |
|---------|--------|-------|
| INSERT OR REPLACE | ✅ Working | Deletes old row, inserts new |
| INSERT OR IGNORE | ✅ Working | Skips conflicting row |
| INSERT OR ABORT | ✅ Working | Stops on conflict |
| INSERT OR FAIL | ✅ Working | Continues with error |
| INSERT OR ROLLBACK | ✅ Working | Rolls back transaction |
| ON CONFLICT (cols) | ❌ Missing | No syntax for specifying columns |
| DO UPDATE | ❌ Missing | No UPDATE action on conflict |
| excluded.* | ❌ Missing | No pseudo-table for VALUES |

**Observation**: DBSQL has global conflict resolution (INSERT OR ...) but lacks targeted conflict resolution (ON CONFLICT (columns) DO ...).

---

## 2. Index Infrastructure

### 2.1 Unique Constraint Checking (cg_insert.c:888)

`__generate_constraint_checks()` already implements unique checking:

```c
// For INTEGER PRIMARY KEY:
if (will_recno_chng_p) {
    __vdbe_add_op(v, OP_Dup, num_col, 1);
    jmp_inst_2 = __vdbe_add_op(v, OP_NotExists, base, 0);
    // Conflict detected!
    switch(on_error) {
    case OE_Ignore:
        __vdbe_add_op(v, OP_Pop, (num_col + 1 + two_recnos_p), 0);
        __vdbe_add_op(v, OP_Goto, 0, ignore_dest);
        break;
    case OE_Replace:
        __generate_row_index_delete(...);
        // ... update logic
        break;
    // ... other cases
    }
}

// For unique indexes:
for (cur = 0, index = table->pIndex; index; index = index->pNext, cur++) {
    // Build index key
    __vdbe_add_op(v, OP_MakeIdxKey, index->nColumn, 0);

    // Check uniqueness
    jmp_inst_2 = __vdbe_add_op(v, OP_IsUnique, (base + cur + 1), 0);

    // Conflict detected!
    switch(on_error) {
    case OE_Ignore: /* skip row */ break;
    case OE_Replace: __generate_row_delete(...); break;
    // ... other cases
    }
}
```

**Analysis**: The infrastructure for detecting conflicts is **already there**. We just need to add new code paths for DO UPDATE and targeted conflict resolution.

### 2.2 Index Enumeration

```c
// DBSQL can iterate through all indexes on a table:
for (index = table->pIndex; index; index = index->pNext) {
    // index->nColumn = number of key columns
    // index->aiColumn[i] = column index for key column i
    // index->onError = conflict resolution (OE_None if not UNIQUE)
    // index->pPartIdxWhere = partial index WHERE clause (if any)
}
```

**Analysis**: DBSQL has full index metadata. Matching conflict targets to indexes is feasible.

### 2.3 Gap: Conflict Target Matching

**Missing**: Algorithm to match ON CONFLICT (col1, col2, ...) to a specific index.

**Required**:
- Column name to index mapping
- Expression matching (for expression indexes)
- Partial index WHERE clause matching
- Collation sequence matching

**Effort**: 10-15 hours to port SQLite's matching logic.

---

## 3. Code Generator Capabilities

### 3.1 Current INSERT Code Generator (cg_insert.c)

**Structure**:
```
__insert(parser, tlist, vlist, select, column, on_error)
  |
  +-- Open table and index cursors
  |
  +-- FOR each row:
      |
      +-- Generate row data on stack
      |
      +-- __generate_constraint_checks()
      |   |
      |   +-- NOT NULL checks
      |   |
      |   +-- INTEGER PRIMARY KEY uniqueness check
      |   |
      |   +-- Unique index checks
      |
      +-- __complete_insertion()
          |
          +-- Insert into table
          |
          +-- Insert into indexes
```

**Analysis**: Clean separation of concerns. UPSERT logic can be injected into `__generate_constraint_checks()` at the conflict detection points.

### 3.2 Existing Constraint Check Logic

```c
void __generate_constraint_checks(
    parser_t *parser,
    table_t *table,
    int base,                   // Table cursor
    char *used_indicies,
    int will_recno_chng_p,
    int update_p,
    int override_error,         // Conflict resolution mode
    int ignore_dest             // Jump target for OE_Ignore
)
```

**Key Parameters**:
- `override_error`: Already accepts conflict resolution mode
- `ignore_dest`: Already has jump target for skipping rows

**Analysis**: Signature is **compatible** with UPSERT needs. Can add `upsert_t *pUpsert` parameter.

### 3.3 Gap: excluded.* Context

**Missing**: Mechanism to reference "excluded" pseudo-table in expressions.

**Current Expression Evaluation**:
```c
// cg_expr.c
__expr_code(parser, expr);  // Evaluates expression, leaves result on stack
```

Expressions can reference:
- `old.*` - in triggers, via TK_TRIGGER
- `new.*` - in triggers, via TK_TRIGGER
- Table columns - via TK_COLUMN with iTable cursor number

**Required for UPSERT**:
- `excluded.*` - virtual table where columns map to registers holding VALUES

**Implementation Strategy**:
1. Allocate range of registers for VALUES
2. Create pseudo-cursor for "excluded" table
3. In expression resolver, recognize "excluded" table name
4. Map `excluded.col` to register holding that column's VALUE

**Effort**: 15-20 hours (requires expression resolver changes).

---

## 4. Storage Manager Interface

### 4.1 BerkeleyDB Conflict Detection

**Current Implementation** (src/sm.c):

```c
// Check if key exists in BerkeleyDB
int __sm_cursor_exists(sm_cursor_t *cursor, const void *key, int key_size)
{
    DBT dbt_key, dbt_data;
    int ret;

    memset(&dbt_key, 0, sizeof(DBT));
    memset(&dbt_data, 0, sizeof(DBT));

    dbt_key.data = (void *)key;
    dbt_key.size = key_size;
    dbt_data.flags = DB_DBT_USERMEM | DB_DBT_PARTIAL;
    dbt_data.dlen = 0;

    ret = cursor->dbc->get(cursor->dbc, &dbt_key, &dbt_data, DB_SET);

    return (ret == 0);  // 0 = exists, DB_NOTFOUND = doesn't exist
}
```

**Analysis**: Conflict detection is **already implemented** via `DBC->get()` with `DB_SET` flag.

### 4.2 BerkeleyDB Update-in-Place

**Current Implementation**:

```c
// Update existing record
int __sm_cursor_update(sm_cursor_t *cursor, const void *data, int data_size)
{
    DBT dbt_data;
    int ret;

    memset(&dbt_data, 0, sizeof(DBT));
    dbt_data.data = (void *)data;
    dbt_data.size = data_size;

    // Cursor must be positioned (via DB_SET or DB_NEXT)
    ret = cursor->dbc->put(cursor->dbc, NULL, &dbt_data, DB_CURRENT);

    return ret;
}
```

**Analysis**: Update-in-place is **already implemented** via `DBC->put()` with `DB_CURRENT` flag.

### 4.3 Gap: Transactional Semantics

**Question**: How to handle partial INSERT failures with UPSERT?

**SQLite Behavior**:
- UPSERT never "fails" - either inserts or updates
- No need for savepoints within UPSERT

**BerkeleyDB Behavior**:
- Transactions are all-or-nothing
- Child transactions can be used for partial rollback

**DBSQL Current Behavior**:
- Uses single transaction for entire INSERT statement
- On error, entire statement rolls back (if on_error = OE_Rollback/Abort)

**Analysis**: No special transaction handling needed for UPSERT. Existing transaction model is sufficient.

---

## 5. Transaction Handling

### 5.1 Current Model

```c
// BEGIN transaction (if not already in one)
__vdbe_prepare_write(parser, select || row_triggers_p, table->iDb);

// ... INSERT logic ...

// COMMIT transaction
__vdbe_conclude_write(parser);
```

**Analysis**: DBSQL already has transaction begin/commit infrastructure. UPSERT fits within existing model.

### 5.2 Statement-Level Rollback

**SQLite**: Uses savepoints (OP_Savepoint, OP_Statement) for statement-level rollback.

**DBSQL**: Does not currently implement statement-level rollback.

**Gap Analysis**:
- UPSERT doesn't need statement-level rollback (never "fails")
- Multi-row INSERT with UPSERT processes each row independently
- IF row conflicts AND DO NOTHING → skip row, continue
- IF row conflicts AND DO UPDATE → update row, continue
- No partial rollback needed

**Conclusion**: No new transaction infrastructure required.

---

## 6. Expression Evaluation

### 6.1 Current Capabilities (cg_expr.c)

```c
// Evaluate expression, leave result on VDBE stack
void __expr_code(parser_t *parser, expr_t *expr);

// Supported expression types:
TK_COLUMN    // Column reference
TK_INTEGER   // Integer literal
TK_STRING    // String literal
TK_FUNCTION  // Function call
TK_PLUS, TK_MINUS, ...  // Arithmetic
// ... many more
```

**Analysis**: Rich expression evaluator. Can handle SET clause expressions.

### 6.2 Gap: excluded.* Resolution

**Required Changes**:

1. **Parser** (sql_parser.y):
   - Recognize "excluded" as table name in INSERT context
   - Set flag on expr_t to indicate excluded reference

2. **Name Resolution** (__expr_resolve_names):
   - Map `excluded.column` to register holding VALUE

3. **Code Generation** (__expr_code):
   - For TK_COLUMN with excluded flag, emit OP_Copy from VALUE register

**Complexity**: Medium (touches 3 subsystems).

**Effort**: 15-20 hours.

---

## 7. Multi-Row INSERT

### 7.1 Current Support

```sql
-- DBSQL supports multi-row VALUES:
INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c');

-- Also INSERT ... SELECT (multi-row):
INSERT INTO t SELECT * FROM source;
```

**Implementation**: Loop over rows, process each independently.

### 7.2 UPSERT Interaction

```sql
INSERT INTO t VALUES (1, 'a'), (2, 'b'), (1, 'c')  -- duplicate key 1
  ON CONFLICT (id) DO UPDATE SET name = excluded.name;

-- Expected behavior:
-- Row 1: Insert (1, 'a')
-- Row 2: Insert (2, 'b')
-- Row 3: Update (1, 'c')  -- conflict with row 1 OR existing data?
```

**SQLite Behavior**: Each row in VALUES is processed independently. Row 3 conflicts with existing data in table, not with row 1 in same INSERT.

**DBSQL Current Behavior**: Same - each row processed independently.

**Analysis**: No changes needed for multi-row INSERT. UPSERT logic applies to each row individually.

---

## 8. Trigger Integration

### 8.1 Current Trigger Support

DBSQL has full trigger support:
- BEFORE INSERT triggers (run before conflict checking)
- AFTER INSERT triggers (run after successful insert)
- BEFORE UPDATE triggers
- AFTER UPDATE triggers

### 8.2 UPSERT Trigger Interaction

**Expected Behavior**:
- BEFORE INSERT fires for every row (before conflict check)
- IF conflict AND DO NOTHING → no AFTER INSERT, no UPDATE triggers
- IF conflict AND DO UPDATE → UPDATE triggers fire, no AFTER INSERT
- IF no conflict → AFTER INSERT fires

**Complexity**: High (requires careful ordering).

**Strategy**: Implement basic UPSERT first, add trigger interaction later.

---

## 9. Test Infrastructure

### 9.1 Existing Test Framework

DBSQL has test suite in `test/`:
- TCL-based tests (inherited from SQLite)
- `conflict.test` already tests INSERT OR REPLACE/IGNORE

**Analysis**: Test infrastructure is ready for UPSERT tests.

### 9.2 Required Test Coverage

- Basic DO NOTHING
- Basic DO UPDATE
- Multi-row INSERT with UPSERT
- Conflict target matching
- excluded.* references
- WHERE clauses
- Partial indexes
- Error cases

**Effort**: 15-20 hours for comprehensive test suite.

---

## 10. Overall Readiness Assessment

| Component | Readiness | Gap Size | Effort |
|-----------|-----------|----------|--------|
| **Conflict Detection** | 90% | Small | 5h |
| **Index Matching** | 20% | Large | 10-15h |
| **Parser** | 0% | Large | 15-20h |
| **AST Structures** | 0% | Large | 5-10h |
| **Code Generation** | 50% | Medium | 20-25h |
| **excluded.* Context** | 0% | Large | 15-20h |
| **BDB Integration** | 80% | Small | 5-10h |
| **Testing** | 70% | Small | 15-20h |
| **Documentation** | 0% | Medium | 5-10h |

**Total Estimated Effort**: 95-160 hours

---

## 11. Architectural Strengths

### ✅ What DBSQL Does Well

1. **Modular design**: Clear separation between parser, AST, code gen, storage
2. **Existing conflict infrastructure**: OE_* system is well-tested
3. **BerkeleyDB integration**: Key existence checking is reliable
4. **Index metadata**: Rich index structure with full metadata
5. **Expression evaluator**: Flexible, supports complex expressions
6. **Transaction model**: Simple, correct, sufficient for UPSERT

### ⚠️ What Needs Enhancement

1. **Parser extensibility**: Need to add new grammar rules (manageable)
2. **Expression context**: Need excluded.* pseudo-table (new concept)
3. **Index matching**: Need SQLite's matching algorithm (port required)
4. **Code generator complexity**: INSERT path is already complex (careful refactoring)

---

## 12. Implementation Recommendations

### 12.1 Phased Approach

**Phase 1: Foundation** (30-40 hours)
- Add parser grammar for ON CONFLICT
- Add upsert_t AST structure
- Add conflict target matching algorithm
- Tests: Parse and analyze UPSERT statements

**Phase 2: DO NOTHING** (10-15 hours)
- Integrate with `__generate_constraint_checks()`
- Implement skip logic for conflicts
- Tests: Basic DO NOTHING cases

**Phase 3: DO UPDATE** (40-50 hours)
- Implement excluded.* pseudo-table context
- Implement UPDATE code generation
- Integrate with index maintenance
- Tests: Basic DO UPDATE cases

**Phase 4: Advanced** (15-25 hours)
- Partial index support
- Expression index support
- WHERE clauses
- Tests: Complex cases

### 12.2 Risk Mitigation

1. **Code Complexity**: INSERT code generator is already complex
   - Mitigation: Refactor for clarity before adding UPSERT
   - Extract helper functions for conflict handling

2. **BDB Behavior**: Ensure BDB conflict detection is consistent
   - Mitigation: Comprehensive unit tests for storage layer
   - Test edge cases (concurrent access, transaction isolation)

3. **excluded.* Context**: New concept, potential for bugs
   - Mitigation: Isolated testing of expression resolver changes
   - Validate with extensive expression tests

4. **Performance**: UPSERT adds branching to hot path
   - Mitigation: Profile before/after
   - Ensure fast path when pUpsert == NULL

---

## 13. Prerequisites

### 13.1 Must Complete First

1. ✅ Code generator modernization (Tasks 21-28) - **COMPLETED**
2. ✅ VDBE register-based architecture - **COMPLETED**
3. ✅ Build stability - **COMPLETED**

### 13.2 Nice to Have First

1. ⏸️ RETURNING clause implementation - **Can be parallel**
2. ⏸️ Window functions - **Independent**
3. ⏸️ Recursive CTEs - **Independent**

**Analysis**: UPSERT can begin immediately. Prerequisites are met.

---

## 14. Key Takeaways

### 14.1 Good News

- ✅ Core infrastructure exists
- ✅ Conflict detection works
- ✅ BDB integration is solid
- ✅ Expression evaluator is flexible
- ✅ Test framework is ready

### 14.2 Work Required

- 🔨 Parser grammar extension
- 🔨 AST structure addition
- 🔨 Index matching algorithm
- 🔨 excluded.* pseudo-table
- 🔨 Code generation integration
- 🔨 Comprehensive testing

### 14.3 Estimated Timeline

- **Research & Design**: 10-15 hours (this document + others)
- **Implementation**: 95-160 hours
- **Testing**: 15-20 hours (included in phases)
- **Total**: 120-195 hours

**Conservative Estimate**: 120-160 hours for full implementation

---

## 15. Next Steps

1. ✅ Complete this assessment
2. ⏸️ Design parser grammar (UPSERT_PARSER_DESIGN.md)
3. ⏸️ Design AST structures (UPSERT_AST_DESIGN.md)
4. ⏸️ Design code generation (UPSERT_CODEGEN_DESIGN.md)
5. ⏸️ Create implementation roadmap (UPSERT_IMPLEMENTATION_PLAN.md)
6. ⏸️ Begin Phase 1 implementation

---

**Document Status**: ✅ COMPLETE
**Confidence Level**: HIGH
**Recommendation**: PROCEED with UPSERT implementation
