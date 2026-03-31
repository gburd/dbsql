# PRAGMA Commands Quick Reference

## Command Summary

| PRAGMA | Purpose | Read/Write | Default | Risk Level |
|--------|---------|------------|---------|------------|
| `integrity_check` | Validate database | Read | N/A | Safe |
| `optimize` | Analyze & optimize | Write | N/A | Safe |
| `analysis_limit` | Control ANALYZE scope | Both | 0 | Safe |
| `trusted_schema` | Schema trust flag | Both | 1 | Low |
| `writable_schema` | Allow schema edits | Both | 0 | CRITICAL |

## Quick Usage

### Check Database Integrity
```sql
PRAGMA integrity_check;
-- Returns: "ok" or list of errors
```

### Optimize Database
```sql
PRAGMA optimize;
-- Analyzes all tables, updates statistics
```

### Set Analysis Limit
```sql
PRAGMA analysis_limit = 1000;  -- Set limit
PRAGMA analysis_limit;          -- Check current value
```

### Control Schema Trust
```sql
PRAGMA trusted_schema = 0;  -- Distrust (secure)
PRAGMA trusted_schema = 1;  -- Trust (default)
PRAGMA trusted_schema;       -- Check current
```

### Enable Schema Editing (DANGEROUS!)
```sql
PRAGMA writable_schema = 1;  -- Enable (use with extreme caution!)
-- Make necessary repairs
UPDATE dbsql_master SET sql = '...' WHERE ...;
PRAGMA writable_schema = 0;  -- Disable immediately
PRAGMA integrity_check;       -- Verify repairs
```

## Common Scenarios

### Weekly Maintenance
```sql
-- Check integrity
PRAGMA integrity_check;

-- If OK, optimize
PRAGMA optimize;
```

### After Bulk Operations
```sql
-- After large INSERT/UPDATE/DELETE
PRAGMA optimize;
```

### Large Database Tuning
```sql
-- Limit analysis time
PRAGMA analysis_limit = 1000;
PRAGMA optimize;
```

### Untrusted Database
```sql
-- Disable schema trust
PRAGMA trusted_schema = 0;

-- Then use database
SELECT * FROM ...;
```

### Emergency Recovery
```sql
-- 1. Enable writable schema
PRAGMA writable_schema = 1;

-- 2. Fix the problem
UPDATE dbsql_master SET sql = 'CREATE TABLE ...' WHERE name = 'broken_table';

-- 3. IMMEDIATELY disable
PRAGMA writable_schema = 0;

-- 4. Verify repairs
PRAGMA integrity_check;
```

## Return Values

### integrity_check
- **Success:** `"ok"`
- **Errors:** One line per error found
  - "Cannot open cursor for table X"
  - "Error scanning table X"
  - "Error scanning index X"

### optimize
- **Success:** No output (silent success)
- **Errors:** SQL error message

### analysis_limit
- **Get:** Integer value (0 = unlimited)
- **Set:** No output

### trusted_schema
- **Get:** `0` or `1`
- **Set:** No output

### writable_schema
- **Get:** `0` or `1`
- **Set:** No output

## Error Handling

All PRAGMA commands return proper error codes:
- `DBSQL_SUCCESS` (0) - Operation succeeded
- `DBSQL_ERROR` - General error
- Other error codes as appropriate

## Performance Impact

| PRAGMA | Time Complexity | Disk I/O | Locks Required |
|--------|----------------|-----------|----------------|
| `integrity_check` | O(n) | High | Read locks |
| `optimize` | O(n log n) | Medium | Write locks |
| `analysis_limit` | O(1) | None | None |
| `trusted_schema` | O(1) | None | None |
| `writable_schema` | O(1) | None | None |

Where n = number of rows in database

## Safety Guidelines

### Always Safe
- `PRAGMA integrity_check` - Read-only
- `PRAGMA analysis_limit` - Configuration only
- `PRAGMA trusted_schema` - Minimal risk
- `PRAGMA optimize` - Safe but may take time

### Use with Caution
- `PRAGMA writable_schema = 1` - EXTREMELY DANGEROUS
  - Only for emergency recovery
  - Can corrupt database
  - Always disable immediately after use
  - Always verify with integrity_check

## Testing

Run the test suite:
```bash
cd /home/gburd/ws/dbsql/test/src
make test_pragma
./test_pragma
```

## Files Reference

- **Implementation:** `/home/gburd/ws/dbsql/src/integrity.c`
- **PRAGMA handlers:** `/home/gburd/ws/dbsql/src/cg_pragma.c`
- **Tests:** `/home/gburd/ws/dbsql/test/src/test_pragma.c`
- **Documentation:** `/home/gburd/ws/dbsql/test/src/PRAGMA_IMPLEMENTATION.md`

## Examples

### Example 1: Weekly Health Check
```sql
-- Run every week
PRAGMA integrity_check;
-- Expected: "ok"

-- If OK, optimize
PRAGMA optimize;
```

### Example 2: Performance Tuning
```sql
-- Before bulk operations
PRAGMA analysis_limit = 500;

-- Do bulk operations
INSERT INTO large_table SELECT * FROM import_table;

-- After bulk operations
PRAGMA optimize;
```

### Example 3: Database Recovery
```sql
-- Symptom: "table X not found" but you know it exists

-- 1. Check current state
PRAGMA integrity_check;
-- Shows: "Cannot open cursor for table X"

-- 2. Enable repairs (DANGEROUS!)
PRAGMA writable_schema = 1;

-- 3. Fix the master table
UPDATE dbsql_master
SET sql = 'CREATE TABLE X (id INTEGER, name TEXT)'
WHERE name = 'X' AND type = 'table';

-- 4. Immediately disable
PRAGMA writable_schema = 0;

-- 5. Verify the fix
PRAGMA integrity_check;
-- Should show: "ok"
```

### Example 4: Secure External Database
```sql
-- Loading database from untrusted source
PRAGMA trusted_schema = 0;

-- Use database normally
SELECT * FROM untrusted_table;
-- Schema operations will be restricted
```

## Troubleshooting

### Problem: integrity_check reports errors
**Solution:**
1. Note the specific errors
2. Backup database immediately
3. If table errors, consider recovery with writable_schema
4. If corruption, restore from backup

### Problem: optimize takes too long
**Solution:**
```sql
-- Reduce analysis scope
PRAGMA analysis_limit = 100;
PRAGMA optimize;
```

### Problem: writable_schema changes not working
**Solution:**
1. Ensure writable_schema = 1
2. Check your UPDATE syntax
3. Disable writable_schema after changes
4. Verify with integrity_check

### Problem: trusted_schema blocking operations
**Solution:**
```sql
-- Temporarily enable if you trust the database
PRAGMA trusted_schema = 1;
-- Perform operation
-- Consider setting back to 0
PRAGMA trusted_schema = 0;
```

## Best Practices

1. **Run integrity_check:**
   - Before important operations
   - After crashes or errors
   - Weekly for production databases

2. **Run optimize:**
   - After bulk inserts
   - After bulk updates
   - After bulk deletes
   - Not during peak usage

3. **Set analysis_limit:**
   - For databases > 1GB
   - On production systems
   - Before running optimize

4. **Use trusted_schema = 0:**
   - For external databases
   - For downloaded databases
   - For databases from untrusted sources

5. **NEVER use writable_schema:**
   - Unless you're a DBA
   - Unless it's an emergency
   - Unless you have backups
   - Without immediately disabling it

## SQLite Compatibility

These PRAGMAs follow SQLite conventions:
- Same syntax
- Similar behavior
- Compatible error handling
- Standard return values

Applications using SQLite PRAGMA commands can use DBSQL equivalents with minimal changes.

## Related Commands

### Berkeley DB Tools
```bash
# Berkeley DB verification (complementary)
db_verify database.db
```

### Other DBSQL PRAGMAs
```sql
PRAGMA table_info(table_name);    -- Table structure
PRAGMA index_list(table_name);    -- Table indexes
PRAGMA index_info(index_name);    -- Index columns
PRAGMA foreign_key_list(table);   -- Foreign keys
PRAGMA database_list;              -- All databases
```

## Support

For issues or questions:
1. Check this guide
2. Review full documentation in PRAGMA_IMPLEMENTATION.md
3. Run test suite for examples
4. Review source code in integrity.c
