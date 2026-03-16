# DBSQL Migration Guide: 0.4.0 to 1.0.0

**Target Audience**: Application developers and database administrators using DBSQL

**Last Updated**: March 2026

---

## Overview

DBSQL 1.0.0 represents a major modernization, updating the SQL engine from SQLite 2.8.x to SQLite 3.52.0. This brings 20 years of SQL features, performance improvements, and bug fixes. However, it introduces breaking changes that require database migration and application updates.

---

## Executive Summary

### What's Changing

- **SQL engine**: SQLite 2.8.x → SQLite 3.52.0
- **Virtual machine**: Stack-based VDBE → Register-based VDBE
- **Type system**: Typeless (string storage) → Manifest typing (INTEGER, REAL, TEXT, BLOB, NULL)
- **Record format**: Custom string-based → SQLite3 binary format
- **Integer keys**: 4-byte encoded → 8-byte native i64
- **API**: Some functions renamed/removed

### What's Preserved

- **Storage engine**: Berkeley DB 4.8 (unchanged)
- **Replication**: All BDB replication features work
- **File format**: BDB B-tree (with migration)
- **Core SQL syntax**: Standard SQL92 queries continue to work

### Migration Required

**All DBSQL 0.4.0 databases MUST be migrated.** The new record format is incompatible with 0.4.0. Attempting to open an old database with DBSQL 1.0.0 will fail with:

```
DBSQL error: database format version mismatch
```

---

## Breaking Changes

### 1. Record Format Change

**Impact**: ALL table data

**What changed**: DBSQL 0.4.0 stores all values as strings. DBSQL 1.0.0 uses SQLite 3's binary record format with typed values.

**Migration action**: Use `dbsql_migrate` tool (see Migration Tools section)

**Example old record** (DBSQL 0.4.0):
```
Row: "1", "John", "35", "1650.50"
Stored as: strings with null terminators
Size: 4 + 5 + 3 + 8 = 20 bytes (plus overhead)
```

**Example new record** (DBSQL 1.0.0):
```
Row: 1, "John", 35, 1650.50
Stored as: header + binary data
Size: 2 (header) + 0 (int) + 4 (string) + 0 (int) + 8 (real) = 14 bytes
```

Benefits: 30-40% space savings, faster comparisons, proper type semantics.

### 2. Integer Key Width Change

**Impact**: All tables (rowids)

**What changed**: Rowids expanded from 32-bit to 64-bit integers.

**Migration action**: Automatic during database migration (all keys re-encoded)

**Consequences**:
- Max table size: 2^31 rows → 2^63 rows
- Key storage: 4 bytes → 8 bytes per rowid
- Overhead: ~0.01% for typical databases

### 3. Type Affinity Semantics

**Impact**: Comparisons and expressions

**What changed**: SQLite 3 uses type affinity rules. Values are compared according to their type, not always as strings.

**Example behavior change**:

DBSQL 0.4.0 (typeless):
```sql
SELECT * FROM test WHERE age > '9';
-- Compares strings: '35' < '9' (lexicographic)
-- Returns: rows with age '1' through '8'
```

DBSQL 1.0.0 (typed):
```sql
SELECT * FROM test WHERE age > '9';
-- Compares integers: 35 > 9
-- Returns: rows with age 10 and above
```

**Migration action**: Review queries that compare numbers as strings. Use explicit casts if needed:
```sql
-- Force string comparison
SELECT * FROM test WHERE CAST(age AS TEXT) > '9';
```

### 4. NULL Handling Changes

**Impact**: Comparisons, aggregates, constraints

**What changed**: NULL comparison semantics now match SQL-92 standard.

**Example**:

DBSQL 0.4.0:
```sql
SELECT * FROM test WHERE name = NULL;
-- Returns rows with name = NULL (wrong per SQL standard)
```

DBSQL 1.0.0:
```sql
SELECT * FROM test WHERE name = NULL;
-- Returns no rows (correct per SQL standard)
-- Use: SELECT * FROM test WHERE name IS NULL;
```

**Migration action**: Replace `= NULL` with `IS NULL` and `!= NULL` with `IS NOT NULL`.

### 5. Aggregate Function Behavior

**Impact**: GROUP BY queries

**What changed**: Empty aggregate results now return NULL instead of zero/empty string.

**Example**:

DBSQL 0.4.0:
```sql
SELECT SUM(amount) FROM sales WHERE 1=0;
-- Returns: 0 (no rows matched)
```

DBSQL 1.0.0:
```sql
SELECT SUM(amount) FROM sales WHERE 1=0;
-- Returns: NULL (no rows matched)
```

**Migration action**: Use `COALESCE()` if zero is required:
```sql
SELECT COALESCE(SUM(amount), 0) FROM sales WHERE 1=0;
```

### 6. API Function Changes

**Removed functions**:
- `dbsql_get_table()` → Use `dbsql_prepare_v2()` + `dbsql_step()`
- `dbsql_exec_printf()` → Use `dbsql_prepare_v2()` with bound parameters
- `dbsql_vmprintf()` → Use `dbsql_snprintf()`

**Renamed functions**:
- `dbsql_create_function()` → `dbsql_create_function_v2()`
- `dbsql_open()` → `dbsql_open_v2()` (old form still works)

**Changed signatures**:
- `dbsql_prepare()` → `dbsql_prepare_v2()` (recommended for new code)
  - Old: `int dbsql_prepare(dbsql*, const char *sql, int len, dbsql_stmt**, const char **tail)`
  - New: `int dbsql_prepare_v2(dbsql*, const char *sql, int len, dbsql_stmt**, const char **tail)`
  - Difference: Better error handling, improved statement caching

**Migration action**: Update application code to use new APIs. See API Changes section.

### 7. Schema Cookie Validation

**Impact**: Concurrent schema modifications

**What changed**: DBSQL 1.0.0 detects schema changes during transactions and aborts if schema has changed.

**Example**:

Process A:
```sql
BEGIN;
SELECT * FROM users WHERE id = 1;
-- ... long-running operation ...
```

Process B (during Process A's transaction):
```sql
ALTER TABLE users ADD COLUMN email TEXT;
```

Process A (continued):
```sql
SELECT * FROM users WHERE id = 2;
-- DBSQL 0.4.0: Proceeds (may see inconsistent schema)
-- DBSQL 1.0.0: Error: "database schema has changed"
```

**Migration action**: Handle `DBSQL_SCHEMA` error code by retrying transaction.

---

## New Features

### Major SQL Features (New in SQLite 3.x)

1. **Common Table Expressions (CTEs)** - WITH clauses
   ```sql
   WITH RECURSIVE cnt(x) AS (
     SELECT 1
     UNION ALL
     SELECT x+1 FROM cnt WHERE x<10
   )
   SELECT x FROM cnt;
   ```

2. **Window Functions** - OVER clauses
   ```sql
   SELECT name, department, salary,
          AVG(salary) OVER (PARTITION BY department) AS dept_avg
   FROM employees;
   ```

3. **UPSERT** - INSERT ... ON CONFLICT
   ```sql
   INSERT INTO users (id, name) VALUES (1, 'John')
     ON CONFLICT(id) DO UPDATE SET name=excluded.name;
   ```

4. **Generated Columns** - Computed columns
   ```sql
   CREATE TABLE rectangles (
     width REAL,
     height REAL,
     area REAL GENERATED ALWAYS AS (width * height)
   );
   ```

5. **Partial Indexes** - Indexed WHERE clauses
   ```sql
   CREATE INDEX active_users ON users(name) WHERE active=1;
   ```

6. **JSON Support** - JSON functions
   ```sql
   SELECT json_extract(data, '$.name') FROM documents;
   ```

7. **RETURNING Clause** - Get inserted/updated values
   ```sql
   INSERT INTO logs (msg) VALUES ('event') RETURNING id, timestamp;
   ```

### Enhanced Functions

- **Date/time**: `datetime()`, `julianday()`, `strftime()`
- **JSON**: `json_array()`, `json_object()`, `json_extract()`, `json_patch()`
- **Math**: `ceil()`, `floor()`, `trunc()`, `ln()`, `exp()`, `power()`
- **String**: `concat()`, `format()`, `substr()` (enhanced), `instr()` (enhanced)
- **Aggregate**: `group_concat()`, `total()`, `avg()` (enhanced)

### SQL Syntax Enhancements

- **VALUES clause** as SELECT alternative
  ```sql
  VALUES (1, 'a'), (2, 'b'), (3, 'c');
  ```

- **LIMIT with OFFSET**
  ```sql
  SELECT * FROM users LIMIT 10 OFFSET 20;
  ```

- **INDEXED BY / NOT INDEXED** hints
  ```sql
  SELECT * FROM users INDEXED BY idx_name WHERE name='John';
  ```

- **Enhanced ALTER TABLE**
  ```sql
  ALTER TABLE users RENAME COLUMN name TO username;
  ```

---

## Migration Tools

### dbsql_migrate

The `dbsql_migrate` utility converts DBSQL 0.4.0 databases to 1.0.0 format.

**Usage**:
```bash
dbsql_migrate [options] <old_db> <new_db>

Options:
  -v, --verbose       Show progress
  -c, --check         Verify migration without writing
  -f, --force         Overwrite existing new_db
  --schema-only       Migrate schema only (no data)
  --data-only         Migrate data only (schema must exist)
  --skip-indexes      Don't rebuild indexes (faster, rebuild later)
  --batch-size=N      Process N rows per transaction (default: 10000)
```

**Example**:
```bash
# Full migration with progress
dbsql_migrate -v myapp.db myapp_new.db

# Verify before committing
dbsql_migrate -c myapp.db myapp_new.db

# Large database with batching
dbsql_migrate --batch-size=50000 -v bigdb.db bigdb_new.db
```

**Migration Process**:

1. **Schema extraction**: Reads 0.4.0 schema, converts to 1.0.0 DDL
2. **Schema creation**: Creates tables, indexes, triggers in new database
3. **Data conversion**: Reads rows from old DB, converts types, writes to new DB
4. **Index rebuild**: Rebuilds all indexes in new format
5. **Validation**: Verifies row counts match, samples random rows for correctness

**Performance**:
- Small databases (<100MB): ~30 seconds
- Medium databases (100MB-1GB): ~5-10 minutes
- Large databases (>1GB): ~1GB per minute

**Disk space**: Migration requires ~2x original database size (old + new).

### Migration Script (Python)

For custom migration needs, use the Python API:

```python
import dbsql

# Open old and new databases
old_db = dbsql.connect('myapp.db')  # DBSQL 0.4.0
new_db = dbsql.connect('myapp_new.db')  # DBSQL 1.0.0

# Get schema
schema = old_db.iterdump()
new_db.executescript(schema)

# Migrate data table by table
for table in old_db.execute("SELECT name FROM sqlite_master WHERE type='table'"):
    table_name = table[0]

    # Read all rows
    rows = old_db.execute(f"SELECT * FROM {table_name}").fetchall()

    # Write to new database
    placeholders = ','.join(['?'] * len(rows[0]))
    new_db.executemany(f"INSERT INTO {table_name} VALUES ({placeholders})", rows)

new_db.commit()
```

---

## API Changes

### Prepared Statements (Recommended Pattern)

**Old (DBSQL 0.4.0)**:
```c
dbsql_t *db;
const char *sql = "SELECT * FROM users WHERE id = ?";
dbsql_stmt *stmt;

dbsql_prepare(db, sql, -1, &stmt, NULL);
dbsql_bind_int(stmt, 1, user_id);

while (dbsql_step(stmt) == DBSQL_ROW) {
    int id = dbsql_column_int(stmt, 0);
    const char *name = dbsql_column_text(stmt, 1);
    // process row
}

dbsql_finalize(stmt);
```

**New (DBSQL 1.0.0)** (same, but use `_v2` for new code):
```c
dbsql_t *db;
const char *sql = "SELECT * FROM users WHERE id = ?";
dbsql_stmt *stmt;

dbsql_prepare_v2(db, sql, -1, &stmt, NULL);  // _v2 recommended
dbsql_bind_int(stmt, 1, user_id);

while (dbsql_step(stmt) == DBSQL_ROW) {
    int id = dbsql_column_int(stmt, 0);
    const char *name = (const char*)dbsql_column_text(stmt, 1);
    // process row
}

dbsql_finalize(stmt);
```

### Value Extraction

**New type checking**:
```c
if (dbsql_column_type(stmt, 0) == DBSQL_INTEGER) {
    int64_t value = dbsql_column_int64(stmt, 0);  // Use int64 for large values
} else if (dbsql_column_type(stmt, 0) == DBSQL_NULL) {
    // Handle NULL explicitly
}
```

### Error Handling

**Enhanced error codes**:
```c
int rc = dbsql_step(stmt);

switch (rc) {
    case DBSQL_ROW:
        // Process row
        break;
    case DBSQL_DONE:
        // No more rows
        break;
    case DBSQL_BUSY:
        // Database locked, retry
        break;
    case DBSQL_SCHEMA:
        // Schema changed, re-prepare and retry
        dbsql_finalize(stmt);
        dbsql_prepare_v2(db, sql, -1, &stmt, NULL);
        break;
    default:
        // Error
        fprintf(stderr, "Error: %s\n", dbsql_errmsg(db));
        break;
}
```

### Transaction Patterns

**Savepoints** (new feature):
```c
dbsql_exec(db, "SAVEPOINT sp1", NULL, NULL, NULL);

// ... operations ...

if (error) {
    dbsql_exec(db, "ROLLBACK TO sp1", NULL, NULL, NULL);
} else {
    dbsql_exec(db, "RELEASE sp1", NULL, NULL, NULL);
}
```

---

## Testing Your Migration

### Pre-Migration Checklist

- [ ] **Backup database**: Copy production database to safe location
- [ ] **Test migration on copy**: Run `dbsql_migrate -c` on backup
- [ ] **Measure database size**: Note old size, verify new size is reasonable
- [ ] **Document custom SQL**: List queries that use type coercion, NULL comparisons
- [ ] **Review application code**: Identify uses of removed APIs
- [ ] **Plan maintenance window**: Estimate migration time + 50% buffer

### Post-Migration Validation

Run these queries to validate migration:

```sql
-- Check row counts match
SELECT name,
       (SELECT COUNT(*) FROM [name]) as count
FROM sqlite_master
WHERE type='table';

-- Compare against old database counts

-- Check for NULL values where not expected
SELECT * FROM users WHERE name IS NULL AND name_required=1;

-- Validate foreign key integrity
PRAGMA foreign_key_check;

-- Check indexes are present
SELECT name FROM sqlite_master WHERE type='index';

-- Sample random rows for manual verification
SELECT * FROM users ORDER BY RANDOM() LIMIT 100;
```

### Application Testing

1. **Smoke tests**: Basic CRUD operations on all tables
2. **Regression tests**: Run existing test suite against migrated database
3. **Performance tests**: Compare query performance old vs. new
4. **Edge cases**: Test NULL handling, empty strings, special characters
5. **Concurrent access**: Verify multi-process access still works

---

## Performance Considerations

### Expected Performance Changes

| Operation | Change | Reason |
|-----------|--------|--------|
| Simple SELECT | +10-20% faster | Binary comparisons vs. string |
| INSERT | +15-25% faster | Smaller records, less I/O |
| Complex JOIN | +20-40% faster | Better query optimization |
| Aggregate queries | +30-50% faster | Register-based VDBE |
| Index scans | ~5-10% slower | Comparison overhead in shim layer |
| Full table scans | +15-20% faster | Smaller records, better locality |

### Optimization Tips

1. **Use prepared statements**: Statement caching is improved in 1.0.0
   ```c
   // Prepare once, execute many times
   dbsql_prepare_v2(db, sql, -1, &stmt, NULL);
   for (int i = 0; i < 1000; i++) {
       dbsql_bind_int(stmt, 1, i);
       dbsql_step(stmt);
       dbsql_reset(stmt);
   }
   dbsql_finalize(stmt);
   ```

2. **Use transactions for bulk operations**:
   ```sql
   BEGIN;
   -- 1000 inserts
   COMMIT;
   -- 100x faster than auto-commit per insert
   ```

3. **Analyze tables after migration**:
   ```sql
   ANALYZE;
   -- Rebuilds query planner statistics
   ```

4. **Use appropriate indexes**:
   ```sql
   -- Before: always use index
   SELECT * FROM users WHERE status='active';

   -- After: partial index is more efficient
   CREATE INDEX idx_active ON users(name) WHERE status='active';
   ```

---

## Rollback Plan

If migration fails or application issues arise:

### Immediate Rollback (During Migration)

1. Stop `dbsql_migrate` (Ctrl-C)
2. Delete partial new database
3. Original database is unchanged

### Post-Migration Rollback

If issues are discovered after migration:

1. **Keep old database**: Don't delete 0.4.0 database until validation complete
2. **Revert application**: Deploy DBSQL 0.4.0 binaries
3. **Restore old database**: Copy old database back to production location
4. **Investigate issues**: Analyze what went wrong, fix application code
5. **Retry migration**: Once issues resolved, attempt migration again

---

## Migration Timeline Example

For a 1GB production database:

| Phase | Duration | Description |
|-------|----------|-------------|
| Planning | 1 week | Review code, document custom SQL, schedule window |
| Test migration | 2 days | Migrate dev/staging databases, validate |
| Application updates | 1 week | Update code for API changes, test thoroughly |
| Production migration | 2 hours | Maintenance window: backup + migrate + validate |
| Monitoring | 1 week | Watch for issues, ready to rollback |
| Cleanup | After 1 month | Delete old database once confident |

---

## Getting Help

### Resources

- **Documentation**: https://dbsql.org/docs/1.0/
- **Migration FAQ**: https://dbsql.org/docs/migration-faq
- **Community Forum**: https://dbsql.org/forum/
- **Bug Reports**: https://github.com/gburd/dbsql/issues

### Common Issues

**Q: Migration fails with "unknown column type"**

A: DBSQL 0.4.0 allowed arbitrary type names. 1.0.0 is stricter. Solution:
```sql
-- Old schema
CREATE TABLE test (id WEIRD_TYPE);

-- Update to standard type
CREATE TABLE test (id INTEGER);
```

**Q: Application crashes with "type mismatch"**

A: Check for assumptions about string storage. Solution:
```c
// Old: always strings
const char *value = dbsql_column_text(stmt, 0);

// New: check type first
if (dbsql_column_type(stmt, 0) == DBSQL_TEXT) {
    const char *value = (const char*)dbsql_column_text(stmt, 0);
} else if (dbsql_column_type(stmt, 0) == DBSQL_INTEGER) {
    int value = dbsql_column_int(stmt, 0);
}
```

**Q: Queries return different results**

A: Review type affinity rules. Use explicit casts:
```sql
-- Force text comparison
WHERE CAST(column AS TEXT) > '9'

-- Force numeric comparison
WHERE CAST(column AS INTEGER) > 9
```

---

## Summary

DBSQL 1.0.0 is a major upgrade that brings modern SQL features while preserving Berkeley DB's reliability and replication capabilities. Migration requires planning but delivers significant performance and functionality improvements.

**Key Takeaways**:

1. ✅ **Database migration is required** - Use `dbsql_migrate` tool
2. ✅ **Test thoroughly** - Validate on non-production data first
3. ✅ **Update application code** - Replace removed APIs, handle new errors
4. ✅ **Keep backups** - Don't delete old database for 1 month
5. ✅ **Expect performance gains** - 10-40% faster for most operations

For detailed technical documentation, see the [Developer Guide](DEVELOPER-GUIDE.md).
