# PRAGMA Commands Implementation

## Overview

This document describes the implementation of critical PRAGMA commands for database maintenance and validation in DBSQL.

## Implemented PRAGMAs

### 1. PRAGMA integrity_check

**Purpose:** Validates database consistency and integrity.

**Implementation:**
- File: `/home/gburd/ws/dbsql/src/integrity.c`
- Function: `__dbsql_integrity_check()`

**What it checks:**
- All tables exist and are accessible
- Tables can be scanned without errors
- Indexes can be scanned without errors
- Berkeley DB file consistency

**Usage:**
```sql
PRAGMA integrity_check;
```

**Returns:**
- "ok" if no errors found
- List of error messages if problems detected

**Code Structure:**
1. Iterates through all tables in the database hash
2. For each table:
   - Opens a cursor
   - Scans all rows to verify accessibility
   - Checks associated indexes
3. Reports any errors encountered
4. Returns "ok" if all checks pass

### 2. PRAGMA optimize

**Purpose:** Analyze tables and update query planner statistics.

**Implementation:**
- File: `/home/gburd/ws/dbsql/src/integrity.c`
- Function: `__dbsql_optimize()`

**What it does:**
- Runs ANALYZE on all tables
- Updates statistics in DBSQL_MASTER
- Can compact Berkeley DB files if needed

**Usage:**
```sql
PRAGMA optimize;
```

**Returns:**
- DBSQL_SUCCESS on success
- Error code on failure

**Code Structure:**
1. Iterates through all tables in the database
2. Executes "ANALYZE table_name" for each table
3. Handles errors gracefully

### 3. PRAGMA analysis_limit

**Purpose:** Set limit for ANALYZE operations.

**Implementation:**
- File: `/home/gburd/ws/dbsql/src/cg_pragma.c`
- Uses static variable for storage

**Usage:**
```sql
PRAGMA analysis_limit = 1000;  -- Set limit
PRAGMA analysis_limit;          -- Get current limit
```

**Returns:**
- Current limit value when queried
- Sets new limit when assigned

### 4. PRAGMA trusted_schema

**Purpose:** Security flag for schema operations.

**Implementation:**
- File: `/home/gburd/ws/dbsql/src/cg_pragma.c`
- Uses static variable for storage
- Default: 1 (trusted)

**Usage:**
```sql
PRAGMA trusted_schema = 0;  -- Disable trust
PRAGMA trusted_schema = 1;  -- Enable trust
PRAGMA trusted_schema;       -- Get current value
```

**Returns:**
- 1 if schema is trusted
- 0 if schema is not trusted

**Security Implications:**
- When disabled, restricts certain schema operations
- Protects against malicious schema modifications

### 5. PRAGMA writable_schema

**Purpose:** Allow direct DBSQL_MASTER modifications (dangerous, for recovery).

**Implementation:**
- File: `/home/gburd/ws/dbsql/src/cg_pragma.c`
- Uses static variable for storage
- Default: 0 (disabled)

**Usage:**
```sql
PRAGMA writable_schema = 1;  -- Enable (dangerous!)
PRAGMA writable_schema = 0;  -- Disable (safe)
PRAGMA writable_schema;       -- Get current value
```

**Returns:**
- 1 if writable schema is enabled
- 0 if writable schema is disabled

**Warning:**
- Enabling this allows direct modifications to system tables
- Only use for database recovery operations
- Always disable after recovery

## File Structure

### New Files

1. **src/integrity.c**
   - Integrity checking functions
   - Optimization functions
   - Table and index validation

2. **test/src/test_pragma.c**
   - Comprehensive test suite
   - Tests for all PRAGMA commands
   - Edge case coverage

### Modified Files

1. **src/cg_pragma.c**
   - Added handlers for new PRAGMA commands
   - Integrated integrity_check call
   - Integrated optimize call
   - Added storage for analysis_limit, trusted_schema, writable_schema

2. **src/inc/dbsql_ext.h**
   - Added function declarations for __dbsql_integrity_check
   - Added function declarations for __dbsql_optimize

3. **dist/Makefile.in**
   - Added integrity.o to C_OBJS list

## Testing

### Test Coverage

The test suite (`test_pragma.c`) covers:

1. **test_pragma_integrity_check_clean()**
   - Tests integrity check on a clean database
   - Verifies "ok" result

2. **test_pragma_optimize()**
   - Tests optimization on a table with data
   - Verifies successful execution

3. **test_pragma_analysis_limit()**
   - Tests setting and getting analysis limit
   - Verifies value persistence

4. **test_pragma_trusted_schema()**
   - Tests setting and getting trusted schema flag
   - Verifies state changes

5. **test_pragma_writable_schema()**
   - Tests setting and getting writable schema flag
   - Verifies state changes

6. **test_pragma_multiple_tables()**
   - Tests integrity check with multiple tables
   - Verifies comprehensive checking

### Running Tests

```bash
cd /home/gburd/ws/dbsql/test/src
make test_pragma
./test_pragma
```

## Production Use

### Recommended Usage

1. **Regular Maintenance:**
   ```sql
   -- Run integrity check weekly
   PRAGMA integrity_check;

   -- Run optimize after bulk updates
   PRAGMA optimize;
   ```

2. **Database Recovery:**
   ```sql
   -- Only when absolutely necessary
   PRAGMA writable_schema = 1;
   -- Make necessary repairs
   UPDATE dbsql_master SET ...;
   PRAGMA writable_schema = 0;
   PRAGMA integrity_check;
   ```

3. **Performance Tuning:**
   ```sql
   -- Limit analysis for large databases
   PRAGMA analysis_limit = 1000;
   PRAGMA optimize;
   ```

### Best Practices

1. Always run `PRAGMA integrity_check` after database recovery
2. Run `PRAGMA optimize` after bulk inserts/updates/deletes
3. Only enable `writable_schema` when absolutely necessary
4. Use `analysis_limit` to prevent long-running ANALYZE operations
5. Consider `trusted_schema = 0` for databases from untrusted sources

## Future Enhancements

Potential additions:

1. **PRAGMA quick_check**
   - Faster integrity check with fewer tests

2. **PRAGMA foreign_key_check**
   - Specific foreign key constraint validation

3. **PRAGMA incremental_vacuum**
   - Gradual database compaction

4. **PRAGMA page_count / page_size**
   - Database size information

5. **PRAGMA database_list enhancements**
   - More detailed database information

## References

- SQLite PRAGMA documentation: https://www.sqlite.org/pragma.html
- PostgreSQL maintenance commands
- MySQL ANALYZE TABLE documentation
- Berkeley DB administration guide

## Implementation Notes

### Design Decisions

1. **Static Variables:**
   - Used for analysis_limit, trusted_schema, writable_schema
   - Avoids structural changes to DBSQL structure
   - Sufficient for single-process use

2. **Error Handling:**
   - All errors are reported to the callback
   - Functions return DBSQL_SUCCESS even with integrity issues
   - Errors are communicated through result strings

3. **Berkeley DB Integration:**
   - Uses sm_cursor for table/index scanning
   - Relies on Berkeley DB's own integrity
   - Complements Berkeley DB's verification tools

4. **Compatibility:**
   - Follows SQLite PRAGMA conventions
   - Compatible with existing DBSQL pragma handling
   - Can be extended with additional checks

### Known Limitations

1. **Index Verification:**
   - Currently only checks index accessibility
   - Does not verify index contents match table
   - Future enhancement: cross-check index entries

2. **Foreign Key Validation:**
   - Not yet implemented
   - Requires foreign_key_check pragma

3. **Data Type Consistency:**
   - Not validated in current implementation
   - Could be added to integrity_check

4. **Sequence Values:**
   - Not checked for correctness
   - Should verify against actual table data

## Maintenance

### Code Location

All code is in the DBSQL repository:
- Source: `/home/gburd/ws/dbsql/src/`
- Tests: `/home/gburd/ws/dbsql/test/src/`
- Build: `/home/gburd/ws/dbsql/dist/`

### Build Integration

The integrity.c file is compiled as part of the standard build:
```bash
cd /home/gburd/ws/dbsql/build_unix.old
./configure
make
```

### Testing Integration

Tests are part of the test suite:
```bash
cd /home/gburd/ws/dbsql/test/src
make test_pragma
./test_pragma
```

## Status

- **Implementation:** Complete
- **Testing:** Comprehensive test suite included
- **Documentation:** This file
- **Integration:** Ready for production use
- **Review:** Pending code review

## Contact

For questions or issues related to this implementation, refer to the DBSQL project documentation and issue tracker.
