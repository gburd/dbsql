# Savepoint Implementation for DBSQL

## Overview

This document describes the implementation of SQL savepoint support for DBSQL, enabling nested transaction functionality using Berkeley DB's nested transaction feature.

## Features Implemented

### SQL Syntax Support

The following SQL commands are now supported:

```sql
-- Create a savepoint
SAVEPOINT sp1;

-- Rollback to a savepoint (multiple syntaxes)
ROLLBACK TO sp1;
ROLLBACK TO SAVEPOINT sp1;
ROLLBACK TRANSACTION TO sp1;
ROLLBACK TRANSACTION TO SAVEPOINT sp1;

-- Release a savepoint (commits and removes it)
RELEASE sp1;
RELEASE SAVEPOINT sp1;
```

## Architecture

### Components Modified

1. **Parser (src/sql_parser.y)**
   - Added grammar rules for SAVEPOINT, ROLLBACK TO, and RELEASE commands
   - Tokens TK_SAVEPOINT and TK_RELEASE were already defined in tokenizer

2. **Storage Manager (src/sm.c)**
   - Added savepoint stack management to `sm_t` structure
   - Implemented three new functions:
     - `__sm_savepoint_create()` - Creates a new nested transaction
     - `__sm_savepoint_rollback()` - Aborts nested transactions back to savepoint
     - `__sm_savepoint_release()` - Commits and removes savepoint

3. **VDBE (src/vdbe.c)**
   - Added `OP_Savepoint` opcode handler
   - Opcode parameters:
     - P1: Operation type (0=create, 1=rollback, 2=release)
     - P4: Savepoint name (string)

4. **Code Generator (src/cg_build.c)**
   - Added `__dbsql_savepoint()` function
   - Generates OP_Savepoint instructions
   - Includes authorization checks

5. **Headers**
   - `src/inc/savepoint.h` - Savepoint structure and constants
   - `build_unix.old/dbsql_int.h` - Updated sm_t structure
   - `src/inc/dbsql_ext.h` - Function declarations

## Implementation Details

### Savepoint Stack

Each storage manager (`sm_t`) maintains a stack of active savepoints:

```c
typedef struct sm_savepoint {
    char *name;                    /* Savepoint name */
    DB_TXN *txn;                   /* Berkeley DB nested transaction */
    struct sm_savepoint *next;     /* Next savepoint in stack */
} sm_savepoint_t;
```

### Berkeley DB Nested Transactions

Savepoints are implemented using Berkeley DB's nested transaction feature:

- **CREATE**: Calls `DB_ENV->txn_begin()` with parent transaction
- **ROLLBACK**: Calls `DB_TXN->abort()` on nested transaction and all children
- **RELEASE**: Calls `DB_TXN->commit()` on nested transaction and all children

### Operation Flow

#### SAVEPOINT sp1

1. Parser recognizes syntax and calls `__dbsql_savepoint(parser, 0, &name)`
2. Code generator creates OP_Savepoint instruction with P1=0
3. VDBE executes opcode, calling `__sm_savepoint_create()`
4. New nested transaction created as child of current transaction
5. Savepoint pushed onto stack

#### ROLLBACK TO sp1

1. Parser recognizes syntax and calls `__dbsql_savepoint(parser, 1, &name)`
2. Code generator creates OP_Savepoint instruction with P1=1
3. VDBE executes opcode, calling `__sm_savepoint_rollback()`
4. All savepoints up to and including sp1 are aborted
5. Savepoints removed from stack

#### RELEASE sp1

1. Parser recognizes syntax and calls `__dbsql_savepoint(parser, 2, &name)`
2. Code generator creates OP_Savepoint instruction with P1=2
3. VDBE executes opcode, calling `__sm_savepoint_release()`
4. All savepoints up to and including sp1 are committed
5. Savepoints removed from stack

## Testing

### Test Suite

A comprehensive test suite has been created in `test/src/test_savepoint.c`:

1. **test_basic_savepoint()** - Tests basic create/rollback functionality
2. **test_nested_savepoints()** - Tests multiple nested savepoints
3. **test_release_savepoint()** - Tests RELEASE command

### Building and Running Tests

```bash
cd test/src
gcc -o test_savepoint test_savepoint.c -I../../src -L../../build_unix.old \
    -ldbsql -ldb -lpthread
./test_savepoint
```

## Example Usage

```sql
BEGIN TRANSACTION;

CREATE TABLE accounts (id INT, balance INT);
INSERT INTO accounts VALUES (1, 1000);
INSERT INTO accounts VALUES (2, 2000);

-- Create first savepoint
SAVEPOINT before_transfer;

UPDATE accounts SET balance = balance - 100 WHERE id = 1;
UPDATE accounts SET balance = balance + 100 WHERE id = 2;

-- Create nested savepoint
SAVEPOINT after_transfer;

-- Try something risky
UPDATE accounts SET balance = balance * 2 WHERE id = 1;

-- Rollback just the risky operation
ROLLBACK TO after_transfer;

-- Release the transfer savepoint (commit it)
RELEASE before_transfer;

COMMIT;
```

## Error Handling

The implementation handles several error cases:

1. **No active transaction** - Returns DBSQL_ERROR
2. **Savepoint not found** - Returns DBSQL_ERROR during rollback/release
3. **Memory allocation failure** - Returns ENOMEM
4. **Berkeley DB errors** - Returns DBSQL_INTERNAL

## Limitations

1. **Cursor interactions** - Cursors may need to be repositioned after rollback
2. **Schema changes** - DDL operations within savepoints are supported but must
   be used carefully as they may affect the entire transaction
3. **Authorization** - Uses DBSQL_SAVEPOINT authorization code

## Performance Considerations

1. **Memory overhead** - Each savepoint allocates a small structure (~32 bytes)
2. **Berkeley DB overhead** - Nested transactions have some overhead
3. **Rollback cost** - Rolling back to a savepoint aborts all nested transactions

## Future Enhancements

Potential improvements for future versions:

1. **Named savepoint reuse** - Currently requires unique names
2. **Cursor savepoints** - Automatic cursor repositioning
3. **Savepoint introspection** - Query active savepoints
4. **Savepoint limits** - Configurable maximum nesting depth
5. **Implicit savepoints** - Automatic savepoints for long transactions

## SQL Standard Compliance

This implementation follows the SQL:2003 standard for savepoint syntax
and semantics:

- CREATE: `SAVEPOINT <savepoint_name>`
- ROLLBACK: `ROLLBACK TO [SAVEPOINT] <savepoint_name>`
- RELEASE: `RELEASE [SAVEPOINT] <savepoint_name>`

Additional Oracle-compatible syntax is supported:
- `ROLLBACK [TRANSACTION] TO [SAVEPOINT] <name>`

## References

- Berkeley DB Nested Transactions: https://docs.oracle.com/cd/E17276_01/html/programmer_reference/transapp_nested.html
- SQL:2003 Standard, Part 2 (Foundation), Section 14.1
- PostgreSQL SAVEPOINT Documentation: https://www.postgresql.org/docs/current/sql-savepoint.html
- Oracle PL/SQL SAVEPOINT: https://docs.oracle.com/en/database/oracle/oracle-database/

## Files Modified/Created

### Created Files
- `src/inc/savepoint.h` - Savepoint structure definitions
- `test/src/test_savepoint.c` - Test suite
- `SAVEPOINT_IMPLEMENTATION.md` - This documentation

### Modified Files
- `src/sql_parser.y` - Added grammar rules
- `src/sm.c` - Added savepoint management functions
- `src/vdbe.c` - Added OP_Savepoint opcode handler
- `src/cg_build.c` - Added code generation function
- `build_unix.old/dbsql_int.h` - Updated sm_t structure
- `src/inc/dbsql_ext.h` - Added function declarations
- `build_unix.old/opcodes.h` - Will be regenerated to include OP_Savepoint

## Build Instructions

1. **Regenerate parser:**
   ```bash
   cd build_unix.old
   make sql_parser.c
   ```

2. **Regenerate opcodes.h:**
   ```bash
   make opcodes.h
   ```

3. **Rebuild library:**
   ```bash
   make clean
   make
   ```

4. **Run tests:**
   ```bash
   cd ../test/src
   make test_savepoint
   ./test_savepoint
   ```

## Integration Checklist

- [x] Parser grammar updated
- [x] Tokenizer (already had tokens)
- [x] Storage manager functions implemented
- [x] VDBE opcode handler added
- [x] Code generation function added
- [x] Header files updated
- [x] Test suite created
- [x] Documentation written
- [ ] Build and test
- [ ] Integration testing
- [ ] Performance benchmarks
- [ ] Documentation review
