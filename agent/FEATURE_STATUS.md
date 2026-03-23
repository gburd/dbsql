# DBSQL Feature Status - SQLite 3.52.0 Compliance

**Document Version**: 1.0
**Date**: 2025-03-17
**DBSQL Version**: 0.4.0+ (post-modernization)

## Executive Summary

DBSQL has completed core SQLite 3.52.0 modernization with register-based VDBE architecture. The implementation provides **strong foundation coverage** (68% SQL features, 93% functions) but is missing modern SQL features and extension mechanisms required for full SQLite 3.52.0 parity.

### Overall Feature Coverage

| Category | Implemented | Missing | Coverage | Status |
|----------|-------------|---------|----------|--------|
| SQL Statements | 11/14 | 3 | 79% | ✅ Good |
| Built-in Functions | 54/54+ | 0 | 100% | ✅ Complete |
| Date/Time Functions | 8/9 | 1 | 89% | ✅ Good |
| Aggregate Functions | 7/7 | 0 | 100% | ✅ Complete |
| JSON Support | 0/15+ | 15+ | 0% | ❌ None |
| FTS/Search | 0/3 | 3 | 0% | ❌ None |
| Virtual Tables | 0/1 | 1 | 0% | ❌ None |
| Window Functions | 0/10+ | 10+ | 0% | ❌ None |
| Recursive CTEs | 0/1 | 1 | 0% | ❌ None |
| **VDBE Opcodes** | **70+/190+** | **120+** | **37%** | ⚠️ Partial |

**Key Insight**: DBSQL excels at SQL92 fundamentals but lacks SQL:2011+ features (window functions, CTEs) and modern extensions (JSON, FTS).

---

## Critical Missing Features (Must-Have for SQL Compliance)

### 1. UPSERT (INSERT ON CONFLICT) ⚠️ CRITICAL

**Standard**: SQL:2011 MERGE statement (SQLite syntax variant)
**Status**: ❌ Tokens defined (`TK_CONFLICT`, `TK_ON`), no parser rules
**Priority**: CRITICAL - Common pattern in modern applications

**SQL Syntax**:
```sql
INSERT INTO users (id, name) VALUES (1, 'Alice')
  ON CONFLICT(id) DO UPDATE SET name = excluded.name;

INSERT INTO logs VALUES (1, 'info')
  ON CONFLICT DO NOTHING;
```

**Implementation Requirements**:
- Parser rules in `sql_parser.y` for `ON CONFLICT` clause
- Conflict detection logic in `cg_insert.c`
- New VDBE opcodes for conflict resolution
- Support for both `DO UPDATE` and `DO NOTHING` actions
- `excluded.*` pseudo-table support

**Files**: `sql_parser.y`, `cg_insert.c`, `vdbe.c`
**Estimated Effort**: 80-120 hours (Very High complexity)
**Risk**: High - Requires transaction-aware conflict detection

---

### 2. RETURNING Clause ⚠️ CRITICAL

**Standard**: PostgreSQL extension (widely adopted by SQLite, MySQL 8.0.21+)
**Status**: ❌ Token defined (`TK_RETURNING`), no implementation
**Priority**: CRITICAL - Essential for application frameworks (ORMs, APIs)

**SQL Syntax**:
```sql
INSERT INTO users (name) VALUES ('Bob')
  RETURNING id, created_at;

UPDATE users SET active = 1 WHERE id = 42
  RETURNING id, updated_at;

DELETE FROM sessions WHERE expired < NOW()
  RETURNING user_id, session_id;
```

**Implementation Requirements**:
- Parser rules in `sql_parser.y` for `RETURNING` clause
- Modify `cg_insert.c`, `cg_update.c`, `cg_delete.c` to emit result rows
- VDBE output handling (result set generation)
- Support for `RETURNING *` (all columns)

**Files**: `sql_parser.y`, `cg_insert.c`, `cg_update.c`, `cg_delete.c`, `vdbe.c`
**Estimated Effort**: 30-40 hours (Medium complexity)
**Risk**: Medium
**ROI**: 🌟 **Highest impact per effort** - Recommended first implementation

---

### 3. Window Functions ⚠️ HIGH

**Standard**: SQL:2011 (Core Feature F201)
**Status**: ❌ Tokens defined (`TK_OVER`, `TK_PARTITION`), no parser/execution
**Priority**: HIGH - Standard SQL feature, widely expected

**SQL Syntax**:
```sql
SELECT
  employee_id,
  salary,
  ROW_NUMBER() OVER (ORDER BY salary DESC) AS rank,
  AVG(salary) OVER (PARTITION BY department) AS dept_avg
FROM employees;
```

**Implementation Requirements**:
- Parser rules for `OVER` clause with `PARTITION BY` and `ORDER BY`
- Window frame specification (`ROWS`, `RANGE`, `GROUPS`)
- New file `cg_window.c` for window function code generation
- 15+ new VDBE opcodes: `OP_AggInverse`, `OP_AggValue`, frame control opcodes
- Window frame computation logic (sliding window, unbounded)
- 10+ window functions: `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, `NTILE()`, `LAG()`, `LEAD()`, `FIRST_VALUE()`, `LAST_VALUE()`, `NTH_VALUE()`, `PERCENT_RANK()`, `CUME_DIST()`

**Files**: New `cg_window.c`, `sql_parser.y`, `vdbe.c`, new `window.c` (frame logic)
**Estimated Effort**: 200-300 hours (Very High complexity)
**Risk**: High - Complex window frame semantics

---

### 4. Recursive CTEs (Common Table Expressions) ⚠️ HIGH

**Standard**: SQL:1999 (Feature T121)
**Status**: ❌ Tokens defined (`TK_WITH`, `TK_RECURSIVE`), no parser support
**Priority**: HIGH - Complex queries, organizational hierarchies

**SQL Syntax**:
```sql
WITH RECURSIVE employee_hierarchy(id, name, manager_id, level) AS (
  -- Anchor member
  SELECT id, name, manager_id, 0 AS level
  FROM employees
  WHERE manager_id IS NULL

  UNION ALL

  -- Recursive member
  SELECT e.id, e.name, e.manager_id, eh.level + 1
  FROM employees e
  JOIN employee_hierarchy eh ON e.manager_id = eh.id
)
SELECT * FROM employee_hierarchy ORDER BY level, name;
```

**Implementation Requirements**:
- Parser rules for `WITH RECURSIVE` clause
- New file `cg_cte.c` for CTE code generation
- Recursion control opcodes (depth limiting, cycle detection)
- Temporary table management for CTE results
- Union handling for anchor + recursive members

**Files**: New `cg_cte.c`, `sql_parser.y`, `vdbe.c`
**Estimated Effort**: 150-200 hours (Very High complexity)
**Risk**: High - Recursion depth control, cycle detection

---

### 5. ALTER TABLE Variants ⚠️ HIGH

**Standard**: SQL:1999 (Feature E111)
**Status**: ⚠️ Tokens defined, minimal parser support (only `RENAME TABLE` works)
**Priority**: HIGH - Schema evolution requirement

**SQL Syntax**:
```sql
ALTER TABLE users ADD COLUMN email TEXT;
ALTER TABLE users DROP COLUMN temp_field;
ALTER TABLE users RENAME TO customers;
ALTER TABLE users RENAME COLUMN old_name TO new_name;
```

**Current Support**:
- ✅ `RENAME TABLE` - Implemented
- ❌ `ADD COLUMN` - Not implemented
- ❌ `DROP COLUMN` - Not implemented
- ❌ `RENAME COLUMN` - Not implemented

**Implementation Requirements**:
- Parser rules in `sql_parser.y` for all ALTER TABLE variants
- Schema modification logic in `cg_build.c`
- Storage manager (`sm.c`) support for schema evolution
- Metadata table updates (`sqlite_master` equivalent)
- Data migration for `ADD COLUMN` (default values, NOT NULL constraints)

**Files**: `sql_parser.y`, `cg_build.c`, `sm.c`
**Estimated Effort**: 40-60 hours (High complexity)
**Risk**: Medium - Schema evolution requires careful metadata handling

---

## High Priority Features (Important for Applications)

### 6. JSON1 Extension (JSON Support) ⚠️ HIGH

**Standard**: Non-standard extension (SQLite-specific, inspired by PostgreSQL)
**Status**: ❌ Not implemented (0% coverage)
**Priority**: HIGH - Semi-structured data is ubiquitous

**Functions Needed** (15+ functions):
- `json_extract(json, path)` - Extract values by JSON path
- `json_object(key, value, ...)` - Create JSON object
- `json_array(value, ...)` - Create JSON array
- `json_type(json, path)` - Get type of JSON value
- `json_valid(json)` - Validate JSON syntax
- `json_insert()`, `json_set()`, `json_replace()` - Modify JSON
- `json_remove()` - Remove JSON elements
- `json_patch()`, `json_quote()`, `json_group_array()`, `json_group_object()`
- `->` operator - JSON extract (alias for `json_extract()`)
- `->>` operator - JSON extract as text

**Implementation Requirements**:
- New file `sql_json.c` with JSON parsing/manipulation functions
- Parser support for `->` and `->>` operators in `sql_parser.y`
- JSON validation and path parsing
- 5+ new VDBE opcodes for JSON operations
- Subtypes (OP_ClrSubtype, OP_GetSubtype, OP_SetSubtype) for JSON type tracking

**Files**: New `sql_json.c`, `sql_parser.y`, `vdbe.c`
**Estimated Effort**: 120-180 hours
**Risk**: Medium - JSON path parsing complexity

---

### 7. Generated Columns (VIRTUAL/STORED) ⚠️ MEDIUM

**Standard**: SQL:2003 (Feature F561)
**Status**: ❌ Tokens defined (`TK_GENERATED`, `TK_ALWAYS`, `TK_VIRTUAL`), no schema support
**Priority**: MEDIUM - Schema design flexibility

**SQL Syntax**:
```sql
CREATE TABLE products (
  price REAL,
  tax_rate REAL,
  total_price REAL GENERATED ALWAYS AS (price * (1 + tax_rate)) STORED
);

CREATE TABLE users (
  first_name TEXT,
  last_name TEXT,
  full_name TEXT AS (first_name || ' ' || last_name) VIRTUAL
);
```

**Implementation Requirements**:
- Parser rules in `sql_parser.y` for `GENERATED` clause
- Schema storage for generated column expressions
- Code generation in `cg_build.c` for STORED columns (compute on INSERT/UPDATE)
- Code generation for VIRTUAL columns (compute on SELECT)
- Expression validation (no references to other generated columns)

**Files**: `sql_parser.y`, `cg_build.c`, `vdbe.c`
**Estimated Effort**: 50-80 hours
**Risk**: Medium

---

### 8. Virtual Tables Framework ⚠️ MEDIUM

**Standard**: SQLite-specific extension mechanism
**Status**: ❌ Not implemented (0% - blocks FTS5, R-Tree)
**Priority**: MEDIUM - Extension mechanism, required for FTS5/R-Tree

**Implementation Requirements**:
- 11 OP_V* opcodes: `OP_VBegin`, `OP_VCreate`, `OP_VDestroy`, `OP_VOpen`, `OP_VFilter`, `OP_VColumn`, `OP_VNext`, `OP_VUpdate`, `OP_VRename`, `OP_VFindFunction`
- `sqlite3_vtab` interface implementation
- Module registration (`sqlite3_create_module()`)
- Virtual table cursor management
- Virtual table transaction handling

**Files**: New `vtab.c`, `cg_build.c`, `vdbe.c`
**Estimated Effort**: 100-150 hours
**Risk**: High - Complex interface, enables entire extension ecosystem

---

### 9. Savepoints (Nested Transactions) ⚠️ MEDIUM

**Standard**: SQL:1999 (Feature F840)
**Status**: ❌ OP_Savepoint not implemented
**Priority**: MEDIUM - Transaction control

**SQL Syntax**:
```sql
BEGIN;
  INSERT INTO users VALUES (1, 'Alice');
  SAVEPOINT sp1;
    UPDATE users SET name = 'Alice Smith' WHERE id = 1;
    SAVEPOINT sp2;
      DELETE FROM users WHERE id = 1;
    ROLLBACK TO sp2;  -- Undo DELETE
  ROLLBACK TO sp1;    -- Undo UPDATE
COMMIT;
```

**Implementation Requirements**:
- `OP_Savepoint` opcode with create/release/rollback modes
- Storage manager (`sm.c`) nested transaction support via BerkeleyDB
- Savepoint stack management in VDBE

**Files**: `vdbe.c`, `sm.c`
**Estimated Effort**: 40-60 hours
**Risk**: Medium - BerkeleyDB nested transaction semantics

---

## Medium Priority Features (Nice-to-Have)

### 10. Full-Text Search (FTS5) ⚠️ MEDIUM

**Standard**: SQLite-specific extension
**Status**: ❌ Not implemented (requires virtual tables)
**Priority**: MEDIUM - Search capability, common requirement

**Requirements**:
- Virtual tables framework (Task #8) must be completed first
- FTS5 tokenizers (unicode61, porter, trigram)
- Ranking functions (`bm25()`, `highlight()`, `snippet()`)
- Index management and query optimization

**Files**: New `fts5/` subdirectory, virtual table integration
**Estimated Effort**: 300-500 hours (Very High - entire module)
**Risk**: Very High - Complex indexing and query optimization
**Dependency**: Requires Virtual Tables (Task #8)

---

### 11. R-Tree Spatial Index ⚠️ LOW

**Standard**: SQLite-specific extension
**Status**: ❌ Not implemented (requires virtual tables)
**Priority**: LOW - Geospatial applications only

**Requirements**:
- Virtual tables framework (Task #8) must be completed first
- R-Tree index structure for multi-dimensional data
- Spatial query operations (overlap, within, nearest)

**Files**: New `rtree/` subdirectory, virtual table integration
**Estimated Effort**: 150-250 hours
**Risk**: High
**Dependency**: Requires Virtual Tables (Task #8)

---

### 12. PRAGMA Extensions ⚠️ MEDIUM

**Standard**: SQLite-specific (pragmas are non-standard)
**Status**: ⚠️ Basic PRAGMAs work; missing advanced diagnostics
**Priority**: MEDIUM - Data validation and optimization

**Current Support**:
- ✅ `PRAGMA table_info(table)` - Column information
- ✅ `PRAGMA index_list(table)` - Index listing
- ✅ `PRAGMA database_list` - Database list
- ✅ `PRAGMA cache_size` - Cache configuration

**Missing PRAGMAs**:
- ❌ `PRAGMA integrity_check` - Database integrity validation
- ❌ `PRAGMA foreign_key_check` - Foreign key constraint validation
- ❌ `PRAGMA optimize` - Analyze and optimize indexes
- ❌ `PRAGMA analysis_limit` - Query optimizer limits
- ❌ `PRAGMA trusted_schema` - Security flag
- ❌ `PRAGMA writable_schema` - Schema modification flag

**Implementation Requirements**:
- New `integrity.c` for database validation logic
- Index statistics collection for `PRAGMA optimize`
- Foreign key constraint validation

**Files**: `cg_pragma.c`, new `integrity.c`
**Estimated Effort**: 60-80 hours total
**Risk**: Medium

---

## VDBE Opcode Gaps (120+ Missing)

DBSQL has implemented **70+ opcodes** of the full SQLite 3.52.0 set (**190+ opcodes**), achieving 37% coverage. The missing opcodes fall into these categories:

### Missing Opcode Categories

| Category | Count | Examples | Priority |
|----------|-------|----------|----------|
| **Virtual Tables** | 11 | VBegin, VCreate, VDestroy, VOpen, VFilter, VColumn, VNext, VUpdate, VRename, VFindFunction, VCheck | HIGH |
| **Window Functions** | 5+ | AggInverse, AggValue, WindowFrame control opcodes | HIGH |
| **Foreign Keys** | 3 | FkCheck, FkCounter, FkIfZero | MEDIUM |
| **Advanced Seek** | 5 | SeekEnd, SeekScan, SeekHit, IfNoHope, DeferredSeek | MEDIUM |
| **Savepoints** | 2 | Savepoint, ReadCookie/SetCookie | MEDIUM |
| **Subtypes (JSON)** | 3 | ClrSubtype, GetSubtype, SetSubtype | MEDIUM |
| **Optimization** | 5+ | Bloom filters, advanced column tracking | LOW |
| **Miscellaneous** | 86+ | Various query optimizer and edge case opcodes | LOW |

**Impact**: Missing opcodes limit feature implementation. Window functions, virtual tables, and JSON support all require their respective opcode categories.

---

## Implementation Roadmap

### Phase 1: Critical SQL Compliance (300-350 hours)

**Goal**: Achieve baseline SQL:2011 compliance for modern applications

1. ✅ **RETURNING clause** (30-40h) - Most impactful per effort
2. ⚠️ **UPSERT support** (80-120h) - Complex but essential
3. ⚠️ **ALTER TABLE ADD COLUMN** (40-60h) - Schema evolution
4. ⚠️ **Savepoints** (40-60h) - Transaction control
5. ⚠️ **PRAGMA integrity_check** (60-80h) - Data validation

**Success Criteria**: Application frameworks (ORMs) can use DBSQL without workarounds

---

### Phase 2: Modern SQL Features (500-650 hours)

**Goal**: Support SQL:2011 analytical queries and modern schema patterns

6. ⚠️ **Window Functions** (200-300h) - Major undertaking
7. ⚠️ **Recursive CTEs** (150-200h) - Complex queries
8. ⚠️ **Generated Columns** (50-80h) - Schema flexibility
9. ⚠️ **JSON1 functions** (120-180h) - Semi-structured data

**Success Criteria**: DBSQL supports analytical workloads and semi-structured data

---

### Phase 3: Extensions & Optimization (600-1000 hours)

**Goal**: Full SQLite 3.52.0 feature parity and extension ecosystem

10. ⚠️ **Virtual Tables framework** (100-150h) - Extension mechanism
11. ⚠️ **FTS5 module** (300-500h) - Full-text search
12. ⚠️ **R-Tree module** (150-250h) - Spatial indexing
13. ⚠️ **Additional opcodes** (50-100h) - Performance optimizations

**Success Criteria**: DBSQL achieves full SQLite 3.52.0 parity, supports all extensions

---

## Total Estimated Effort

**Full SQLite 3.52.0 Parity**: 1,400-2,000 hours

**Breakdown by Priority**:
- Critical (Phase 1): 300-350 hours
- High (Phase 2): 500-650 hours
- Medium/Low (Phase 3): 600-1000 hours

---

## Recommended Immediate Actions

1. **Document feature gaps** ✅ (This document)
2. **Prioritize Phase 1** - Start with RETURNING clause (highest ROI)
3. **Plan incrementally** - Each feature as a separate project/release
4. **Test each feature** - Add tests to the 92-test baseline for each addition
5. **Update documentation** - Maintain this document as features are implemented

---

## Feature Request Process

To request a missing feature:

1. Check this document for current implementation status
2. File an issue at https://github.com/gburd/dbsql/issues (if applicable)
3. Include use case justification and impact assessment
4. Reference this document section number

---

## Document Maintenance

**Last Updated**: 2025-03-17
**Next Review**: After each major feature addition
**Maintainer**: DBSQL Development Team

**Change Log**:
- 2025-03-17: Initial feature status document created (v1.0)
