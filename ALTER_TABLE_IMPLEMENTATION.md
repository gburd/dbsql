# ALTER TABLE Implementation

This document describes the ALTER TABLE support added to DBSQL.

## Overview

ALTER TABLE statements allow modification of existing table definitions without recreating the table or losing data. This implementation follows SQL standard syntax and SQLite conventions.

## Supported Operations

### 1. ALTER TABLE ... RENAME TO

Renames a table.

**Syntax:**
```sql
ALTER TABLE old_name RENAME TO new_name;
```

**Implementation:**
- Updates table name in DBSQL_MASTER
- Updates references in indexes, triggers, and foreign keys
- Validates that new name is not already in use
- Prevents renaming of system tables (prefix: dbsql_)

**Location:** `src/cg_alter.c::__alter_rename_table()`

### 2. ALTER TABLE ... ADD COLUMN

Adds a new column to an existing table.

**Syntax:**
```sql
ALTER TABLE table_name ADD [COLUMN] column_name column_type [constraints];
```

**Implementation:**
- Adds column definition to table metadata
- Default values are applied to existing rows
- PRIMARY KEY and UNIQUE constraints are not supported on added columns
- Column constraints: DEFAULT, NOT NULL (with DEFAULT)

**Location:** `src/cg_alter.c::__alter_finish_add_column()`

**Limitations:**
- Cannot add PRIMARY KEY columns
- Cannot add UNIQUE columns
- NOT NULL requires DEFAULT value

### 3. ALTER TABLE ... RENAME COLUMN

Renames a column.

**Syntax:**
```sql
ALTER TABLE table_name RENAME [COLUMN] old_name TO new_name;
```

**Implementation:**
- Updates column name in table definition
- Updates CREATE TABLE SQL statement in DBSQL_MASTER
- Updates references in indexes, triggers, views
- Validates column exists and new name is unique

**Location:** `src/cg_alter.c::__alter_rename_column()`

### 4. ALTER TABLE ... DROP COLUMN

Removes a column from a table.

**Syntax:**
```sql
ALTER TABLE table_name DROP [COLUMN] column_name;
```

**Implementation:**
- Removes column from table definition
- Requires table rebuild (most expensive operation)
- Updates all indexes that reference the column
- Handles foreign key constraints

**Location:** `src/cg_alter.c::__alter_drop_column()`

**Limitations:**
- Cannot drop the last column
- Cannot drop PRIMARY KEY columns
- Triggers table rebuild (expensive for large tables)

## Grammar Changes

File: `src/sql_parser.y`

Added production rules:
```yacc
cmd ::= ALTER TABLE fullname RENAME TO nm.
cmd ::= ALTER TABLE fullname ADD column_opt nm type carglist.
cmd ::= ALTER TABLE fullname RENAME column_opt nm TO nm.
cmd ::= ALTER TABLE fullname DROP column_opt nm.
column_opt ::= COLUMN | .
fullname ::= nm dbnm.
```

## Token Support

File: `src/sql_tokenize.c`

Keywords already supported:
- ALTER (TK_ALTER = 250)
- ADD (TK_ADD = 251)
- COLUMN (TK_COLUMNKW = 252)
- RENAME (TK_RENAME = 253)
- TO (TK_TO = 291)

These tokens are defined in `src/inc/sql_tokens.h`.

## Function Signatures

File: `src/inc/dbsql_ext.h`

```c
void __alter_rename_table(parser_t *, src_list_t *, token_t *);
void __alter_finish_add_column(parser_t *, token_t *);
void __alter_rename_column(parser_t *, src_list_t *, token_t *, token_t *);
void __alter_drop_column(parser_t *, src_list_t *, token_t *);
```

## Error Handling

All operations validate:
- Table exists and is not a system table
- Table is not read-only (not in a trigger context)
- Column names are valid and not duplicated
- Constraints are not violated

Error messages follow DBSQL conventions:
```c
__str_append(&parser->zErrMsg, "error message", ...);
parser->nErr++;
```

## Schema Updates

All ALTER TABLE operations:
1. Begin a write transaction
2. Open DBSQL_MASTER table
3. Modify the appropriate records
4. Update schema signature via `__change_schema_signature()`
5. Conclude the write operation

This ensures atomicity and triggers schema reload on next statement.

## Testing

Test file: `test/src/test_alter_table.c`

Tests cover:
- Basic rename table operation
- Add column with various types
- Rename column validation
- Drop column constraints
- Optional COLUMN keyword
- Error conditions:
  - Nonexistent columns
  - Duplicate names
  - Dropping last column
  - Dropping PRIMARY KEY columns

Run tests:
```bash
cd test/src
make test_alter_table
./test_alter_table
```

## Build Integration

To build with ALTER TABLE support:

1. Regenerate parser:
```bash
cd build_unix
../dist/lemon ../src/sql_parser.y
cp sql_parser.h sql_parser.c ../src/
```

2. Add cg_alter.c to build:
```bash
# Edit dist/Makefile.in to include cg_alter.o in libobjs
```

3. Rebuild:
```bash
./config.sh
make clean
make
```

## Implementation Status

| Feature | Status | Notes |
|---------|--------|-------|
| RENAME TABLE | Partial | Core logic implemented, metadata updates need completion |
| ADD COLUMN | Partial | Parser support complete, metadata updates in progress |
| RENAME COLUMN | Partial | Validation complete, SQL statement rewrite needed |
| DROP COLUMN | Partial | Validation complete, table rebuild needed |

## Future Work

### Phase 1: Complete Metadata Updates
- Implement full DBSQL_MASTER record updates
- Add Berkeley DB file rename for RENAME TABLE
- Complete ADD COLUMN default value handling

### Phase 2: Table Rebuild for DROP COLUMN
- Implement CREATE TABLE ... AS SELECT without column
- Copy all indexes excluding dropped column
- Update foreign key constraints
- Atomic swap of old and new tables

### Phase 3: Advanced Features
- ALTER TABLE ... ADD CONSTRAINT
- ALTER TABLE ... DROP CONSTRAINT
- ALTER TABLE ... MODIFY COLUMN
- ALTER TABLE ... ALTER COLUMN SET DEFAULT

### Phase 4: Online Schema Changes
- Non-blocking ADD COLUMN for large tables
- Background table rebuild
- Copy-on-write for DROP COLUMN

## References

- SQLite ALTER TABLE: https://www.sqlite.org/lang_altertable.html
- PostgreSQL ALTER TABLE: https://www.postgresql.org/docs/current/sql-altertable.html
- SQL:2023 Standard: Foundation (SQL/Foundation)

## Architecture Notes

The implementation follows DBSQL's existing patterns:

1. **Parser Integration**: Grammar rules in `sql_parser.y` call functions in `cg_alter.c`
2. **Code Generation**: Functions generate VDBE opcodes to modify schema
3. **Transaction Safety**: All operations wrapped in write transactions
4. **Error Propagation**: Uses parser error system (`parser->zErrMsg`, `parser->nErr`)
5. **Memory Management**: Uses DBSQL allocator (`__dbsql_free`, `__dbsql_calloc`)

The separation into `cg_alter.c` keeps ALTER TABLE logic isolated and maintainable, similar to how `cg_copy.c` handles COPY statements.
