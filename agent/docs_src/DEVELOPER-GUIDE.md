# DBSQL Developer Guide

**Target Audience**: Contributors to the DBSQL project

**Last Updated**: March 2026

---

## Current Project Status

**Version**: 0.4.0+ (SQLite 3.52.0 Modernization)

**Build Status**: 23/39 files compile (59%)
- ✅ All 12 code generators (100%)
- ✅ SQL parser and infrastructure (100%)
- 🔄 VDBE engine (36% - 53/148 opcodes)
- ⏳ Utility files (blocked by VDBE)

**Major Achievements**:
- Register-based VDBE architecture implemented
- 51 structures modernized to SQLite 3.52.0
- RETURNING clause fully implemented
- BerkeleyDB 5.3.21 integration complete
- 77.9% error reduction (1,700 → 376 errors)

**Current Focus**: Complete VDBE register-based conversion (Task #12)

See [COMPILATION_STATUS.md](../COMPILATION_STATUS.md) and [PROJECT_STATUS_UPDATE.md](../PROJECT_STATUS_UPDATE.md) for detailed status.

---

## Welcome

This guide helps developers understand DBSQL's architecture, build system, and development practices. Whether you're fixing bugs, adding features, or porting code from SQLite, this document provides the context you need.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Codebase Structure](#codebase-structure)
3. [Build System](#build-system)
4. [Development Workflow](#development-workflow)
5. [Testing](#testing)
6. [Berkeley DB Integration Patterns](#berkeley-db-integration-patterns)
7. [Porting from SQLite](#porting-from-sqlite)
8. [Code Style Guidelines](#code-style-guidelines)
9. [Debugging Techniques](#debugging-techniques)

---

## Architecture Overview

### High-Level Components

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│              (User code calling DBSQL API)              │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────┴──────────────────────────────────┐
│                     DBSQL API Layer                      │
│        (api.c, api_table.c - Public interfaces)         │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────┴──────────────────────────────────┐
│                   SQL Parser & Compiler                  │
│   ┌──────────────┐  ┌──────────────┐  ┌─────────────┐  │
│   │ Tokenizer    │→│ Parser       │→│ Code Gen     │  │
│   │ (tokenize.c) │  │ (parser.y)   │  │ (cg_*.c)     │  │
│   └──────────────┘  └──────────────┘  └─────────────┘  │
└──────────────────────┬──────────────────────────────────┘
                       │ Generates VDBE program
                       ↓
┌─────────────────────────────────────────────────────────┐
│          Virtual Database Engine (VDBE)                  │
│  Register-based VM executing bytecode instructions       │
│         (vdbe.c, vdbe_method.c, vdbe_int.h)             │
└──────────────────────┬──────────────────────────────────┘
                       │ Calls storage operations
                       ↓
┌─────────────────────────────────────────────────────────┐
│              Storage Manager (SM) Layer                  │
│   Adapter between VDBE and Berkeley DB                   │
│    (sm.c - wraps BDB with SQLite-like interface)        │
└──────────────────────┬──────────────────────────────────┘
                       │ Uses BDB API
                       ↓
┌─────────────────────────────────────────────────────────┐
│                    Berkeley DB                           │
│   B-tree storage, transactions, replication, recovery   │
│              (libdb-4.8 - external library)             │
└─────────────────────────────────────────────────────────┘
```

### Data Flow: Query Execution

```
1. SQL text         → "SELECT name FROM users WHERE id = 42"
                      ↓
2. Tokenize         → [SELECT, name, FROM, users, WHERE, id, =, 42]
                      ↓
3. Parse            → Abstract Syntax Tree (AST)
                      ↓
4. Code Generation  → VDBE bytecode program:
                        OpenRead 0 users
                        Integer 42 → reg[1]
                        SeekRowid 0 reg[1]
                        Column 0 1 → reg[2]    ; column "name"
                        ResultRow reg[2] 1
                        Halt
                      ↓
5. VDBE Execution   → Execute opcodes:
                        - Open cursor on "users" table
                        - Seek to rowid 42
                        - Extract column 1 into register 2
                        - Return register 2 as result row
                      ↓
6. Result           → "John"
```

### Key Concepts

**VDBE (Virtual Database Engine)**: A register-based virtual machine that executes bytecode programs. Similar to Java's JVM or Python's interpreter, but specialized for database operations.

**Register Machine**: Instead of a stack, the VDBE uses numbered registers (reg[0], reg[1], etc.) to store intermediate values. Opcodes specify which registers to read from and write to.

**Cursor**: A pointer into a table or index. Cursors support operations like seek, next, prev, read, write, delete. The VDBE opens cursors on tables and navigates them using opcodes.

**Storage Manager (SM)**: DBSQL's abstraction layer over Berkeley DB. Provides a SQLite-like interface (`__sm_*` functions) while internally using BDB APIs.

**Opcode**: A single instruction in the VDBE bytecode. Each opcode has an operation type (e.g., `OP_Integer`, `OP_Add`, `OP_SeekGE`) and up to 5 operands (P1, P2, P3, P4, P5).

---

## Codebase Structure

### Directory Layout

```
dbsql/
├── src/                      # Core source code
│   ├── inc/                  # Internal headers
│   │   ├── dbsql_int.in      # Main internal header (template)
│   │   ├── vdbe.h            # VDBE public interface
│   │   ├── vdbe_int.h        # VDBE internal structures
│   │   ├── hash.h            # Hash table implementation
│   │   ├── str.h             # String utilities
│   │   └── ...
│   ├── vdbe.c                # VDBE interpreter (opcode dispatch)
│   ├── vdbe_method.c         # VDBE support functions
│   ├── sm.c                  # Storage Manager (Berkeley DB wrapper)
│   ├── sql_parser.y          # Lemon grammar for SQL
│   ├── sql_tokenize.c        # SQL tokenizer
│   ├── cg_select.c           # SELECT code generation
│   ├── cg_expr.c             # Expression code generation
│   ├── cg_where.c            # WHERE clause code generation
│   ├── cg_build.c            # DDL (CREATE, DROP) code generation
│   ├── cg_insert.c           # INSERT code generation
│   ├── cg_update.c           # UPDATE code generation
│   ├── cg_delete.c           # DELETE code generation
│   ├── cg_trigger.c          # Trigger code generation
│   ├── sql_fns.c             # Built-in SQL functions
│   ├── api.c                 # Public API implementation
│   ├── api_table.c           # Table info API
│   └── ...
├── dist/                     # Build scripts and utilities
│   ├── s_*                   # Shell scripts for various tasks
│   └── ...
├── test/                     # Test suite
│   ├── scr001-scr030/        # Shell script tests (code quality)
│   └── scr050/tests/         # SQL functional tests
├── docs_src/                 # Documentation sources
│   ├── ADR-001-*.md          # Architectural Decision Records
│   ├── MIGRATION-GUIDE.md    # User migration guide
│   └── DEVELOPER-GUIDE.md    # This file
├── build_unix/               # Build artifacts (generated)
├── _/                        # External dependencies
│   └── sqlite/               # SQLite 3.52.0 reference source
└── README                    # Project overview
```

### Key Source Files

| File | Lines | Purpose |
|------|-------|---------|
| `vdbe.c` | ~5,000 | VDBE interpreter: dispatch loop for 190+ opcodes |
| `vdbe_method.c` | ~3,000 | VDBE support: memory management, cursor operations, result handling |
| `sm.c` | ~2,500 | Storage Manager: Berkeley DB integration |
| `cg_select.c` | ~3,000 | Code generation for SELECT statements |
| `cg_expr.c` | ~2,000 | Code generation for expressions (arithmetic, logic, functions) |
| `cg_where.c` | ~1,500 | Code generation for WHERE clauses and query optimization |
| `cg_build.c` | ~2,700 | Code generation for DDL (CREATE TABLE, CREATE INDEX, etc.) |
| `sql_parser.y` | ~900 | Lemon grammar: defines SQL syntax and builds AST |
| `sql_tokenize.c` | ~1,200 | Tokenizer: converts SQL text to token stream |
| `api.c` | ~2,000 | Public API: `dbsql_open`, `dbsql_prepare`, `dbsql_step`, etc. |

### Header File Organization

**Public headers** (installed with library):
- `dbsql.h` - Public API (generated from `dbsql_ext.h`)
- User code includes only this header

**Internal headers** (source tree only):
- `dbsql_int.h` - Main internal header (generated from `dbsql_int.in`)
- `vdbe.h` - VDBE public declarations (used by code generators)
- `vdbe_int.h` - VDBE internal structures (used only by `vdbe*.c`)
- `hash.h`, `str.h`, etc. - Utility headers

**Why `.in` files?**: Template files (`.in`) are processed by the build system to generate actual headers (`.h`). This allows:
- Version number injection
- Conditional compilation based on build options
- Platform-specific definitions

---

## Build System

### Prerequisites

- **C Compiler**: GCC 7+ or Clang 10+ with C99 support
- **BerkeleyDB**: Version 5.3.21 (included as git submodule in `deps/libdb/`)
- **Make**: GNU Make 3.81+
- **Git**: For submodule management
- **Lemon**: Parser generator (included in `dist/`)
- **Optional**: TCL 8.6+ (for test suite)

### Building from Source

```bash
# Clone with submodules
git clone --recursive https://github.com/gburd/dbsql.git
cd dbsql

# Build BerkeleyDB dependency
./deps/build_libdb.sh

# Standard build
cd build_unix
../dist/s_config
make -j$(nproc)

# Install to /usr/local
sudo make install

# Run tests (requires TCL)
make test
```

**Note**: DBSQL 0.4.0+ uses BerkeleyDB 5.3.21 included as a git submodule. The `deps/build_libdb.sh` script builds it automatically.

### Build Options

```bash
# Enable debugging symbols
../dist/configure --enable-debug

# Disable cryptography features
../dist/configure --disable-cryptography

# Specify Berkeley DB location
../dist/configure --with-berkeleydb=/opt/db-4.8

# Enable test coverage
../dist/configure --enable-coverage

# Custom installation prefix
../dist/configure --prefix=/usr/local/dbsql
```

### Build Targets

```bash
# Build everything
make all

# Build library only
make libdbsql.a libdbsql.so

# Build shell
make dbsql

# Run code quality checks
make check

# Run full test suite
make test

# Generate documentation
make docs

# Clean build artifacts
make clean

# Clean everything (including generated files)
make distclean
```

### Development Build (Recommended)

For active development, use a debug build:

```bash
cd build_unix
../dist/configure --enable-debug --enable-coverage CFLAGS="-O0 -g3 -Wall -Wextra"
make

# Run with debugger
gdb ./dbsql
```

### Incremental Builds

The build system tracks dependencies. After editing a source file:

```bash
# Rebuild only affected files
make

# If headers changed, force rebuild
make clean && make
```

### Common Build Issues

**Issue**: `BerkeleyDB submodule not initialized` or `cannot find -ldb-5.3`

**Solution**: Build BerkeleyDB from the included submodule:
```bash
# Automated build (recommended)
./deps/build_libdb.sh

# Or manual build
cd deps/libdb/build_unix
../dist/configure --enable-shared --disable-static
make -j$(nproc)
cd ../../..
```

**Issue**: `lemon: command not found`

**Solution**: Lemon is included in `dist/lemon`. Build system should find it automatically. If not:
```bash
cd dist
make lemon
cd ../build_unix
make
```

**Issue**: Linker errors about BDB symbols or version mismatch

**Solution**: Ensure you're using BerkeleyDB 5.3.21 from the submodule:
```bash
# Check that BDB was built correctly
ls -la deps/libdb/build_unix/.libs/libdb-5.3.so

# Verify DBSQL Makefile points to correct location
grep "libdb" build_unix/Makefile

# Rebuild BDB if necessary
cd deps/libdb/build_unix
make clean
../dist/configure --enable-shared --disable-static
make -j$(nproc)
```

---

## Development Workflow

### Setting Up Your Environment

1. **Clone repository**:
   ```bash
   git clone https://github.com/gburd/dbsql.git
   cd dbsql
   ```

2. **Create feature branch**:
   ```bash
   git checkout -b feature/my-feature
   ```

3. **Build and test**:
   ```bash
   cd build_unix
   ../dist/configure --enable-debug
   make
   make test
   ```

### Making Changes

1. **Edit source files**: Use your preferred editor

2. **Rebuild**:
   ```bash
   make
   ```

3. **Test your changes**:
   ```bash
   # Run specific test
   ./dbsql < ../test/scr050/tests/select1.test

   # Or run full suite
   make test
   ```

4. **Check code style**:
   ```bash
   make check
   ```

### Commit Guidelines

Follow these conventions:

**Commit message format**:
```
Short summary (≤72 chars)

Detailed description explaining:
- What changed
- Why it changed
- Any side effects or risks

Closes #123
```

**Examples**:

Good:
```
Fix segfault in VDBE when cursor is NULL

The OP_Column opcode didn't check if cursor was NULL before
dereferencing. This could happen if OP_OpenRead failed but
execution continued (e.g., in error recovery path).

Added NULL check and return DBSQL_ERROR if cursor is missing.

Closes #456
```

Bad:
```
fix bug
```

### Code Review Checklist

Before submitting a pull request:

- [ ] Code compiles without warnings
- [ ] All existing tests pass
- [ ] New tests added for new functionality
- [ ] Code follows project style (see Code Style Guidelines)
- [ ] No debug print statements left in code
- [ ] Documentation updated (if API changed)
- [ ] CHANGELOG updated

---

## Testing

### Test Suite Structure

DBSQL has multiple layers of tests:

**1. Shell Script Tests** (`test/scr001-scr030/`)
- Code quality checks (style, includes, defines, prototypes)
- Run with: `make check`

**2. SQL Functional Tests** (`test/scr050/tests/`)
- Derived from SQLite test suite
- Test SQL syntax, semantics, edge cases
- Run with: `make test` (requires TCL)

**3. Unit Tests** (if applicable)
- Test individual functions in isolation
- Located near source files

**4. Integration Tests**
- Test end-to-end scenarios
- Multiple tables, transactions, concurrent access

### Running Tests

```bash
# All tests
make test

# Code quality checks only
make check

# Specific test file
dbsql < test/scr050/tests/select1.test

# Verbose output
make test VERBOSE=1
```

### Writing Tests

**SQL functional test format** (`.test` files):

```tcl
# test/scr050/tests/mytest.test

# Test basic INSERT and SELECT
do_test mytest-1.1 {
  execsql {
    CREATE TABLE t1(a INTEGER, b TEXT);
    INSERT INTO t1 VALUES(1, 'hello');
    SELECT * FROM t1;
  }
} {1 hello}

# Test with NULL
do_test mytest-1.2 {
  execsql {
    INSERT INTO t1 VALUES(NULL, 'world');
    SELECT a FROM t1 WHERE a IS NULL;
  }
} {{}}

# Test error condition
do_test mytest-1.3 {
  catchsql {
    INSERT INTO t1 VALUES('not an int', 'text');
  }
} {1 {datatype mismatch}}
```

**Test naming convention**:
- `<filename>-<section>.<number>` (e.g., `mytest-1.1`)
- Group related tests in sections

**Test functions**:
- `do_test <name> <script> <expected>` - Run script, compare result
- `execsql <sql>` - Execute SQL, return results
- `catchsql <sql>` - Execute SQL, return {returncode result}

### Debugging Test Failures

When a test fails:

1. **Run test individually**:
   ```bash
   dbsql < test/scr050/tests/failing_test.test
   ```

2. **Enable VDBE trace**:
   ```bash
   # In dbsql shell
   .trace stdout
   # Then run failing query
   ```

3. **Use debugger**:
   ```bash
   gdb --args dbsql
   (gdb) run < test/scr050/tests/failing_test.test
   (gdb) break vdbe.c:1234
   (gdb) continue
   ```

4. **Check for known issues**: Search issue tracker

---

## Berkeley DB Integration Patterns

### Understanding the Storage Manager (SM)

The SM layer (`src/sm.c`) provides two interfaces:

1. **Internal SM API** (`__sm_*` functions): Direct BDB wrappers
2. **Shim layer** (`__sm_btree_*` functions): SQLite 3.52.0 Btree API adapters

### Internal SM API

**Transaction management**:
```c
sm_t *sm;  // Storage manager handle
DB_TXN *txn;

// Begin transaction
__sm_begin_txn(sm);

// Commit
__sm_commit_txn(sm);

// Rollback
__sm_abort_txn(sm);
```

**Cursor operations**:
```c
sm_cursor_t *cursor;
int result;

// Open cursor
__sm_cursor(sm, table_id, wrflag, &cursor);

// Seek to key
char key[8];
int key_len = 8;
__sm_moveto(cursor, key, key_len, &result);
// result: <0 (before), 0 (exact), >0 (after)

// Navigate
__sm_next(cursor, &result);  // result: 0 if found, 1 if EOF
__sm_prev(cursor, &result);
__sm_first(cursor, &result);
__sm_last(cursor, &result);

// Read data
int data_size;
__sm_data_size(cursor, &data_size);
char *data = malloc(data_size);
__sm_data(cursor, 0, data_size, data);

// Close
__sm_close_cursor(cursor);
```

**Insert/Delete**:
```c
// Insert
char key[8], data[100];
int key_len = 8, data_len = 100;
__sm_insert(cursor, key, key_len, data, data_len);

// Delete current row
__sm_delete(cursor);
```

### Key Encoding

Berkeley DB uses byte keys. SQLite uses native integers for rowids.

**Encoding i64 rowid to BDB key**:
```c
void encode_rowid(i64 rowid, unsigned char key[8]) {
    u64 tmp = (u64)rowid;
    tmp ^= 0x8000000000000000ULL;  // Flip sign bit for correct ordering
    key[0] = (tmp >> 56) & 0xff;
    key[1] = (tmp >> 48) & 0xff;
    key[2] = (tmp >> 40) & 0xff;
    key[3] = (tmp >> 32) & 0xff;
    key[4] = (tmp >> 24) & 0xff;
    key[5] = (tmp >> 16) & 0xff;
    key[6] = (tmp >> 8) & 0xff;
    key[7] = tmp & 0xff;
}
```

**Decoding BDB key to i64 rowid**:
```c
i64 decode_rowid(const unsigned char key[8]) {
    u64 tmp = 0;
    tmp |= ((u64)key[0]) << 56;
    tmp |= ((u64)key[1]) << 48;
    tmp |= ((u64)key[2]) << 40;
    tmp |= ((u64)key[3]) << 32;
    tmp |= ((u64)key[4]) << 24;
    tmp |= ((u64)key[5]) << 16;
    tmp |= ((u64)key[6]) << 8;
    tmp |= ((u64)key[7]);
    tmp ^= 0x8000000000000000ULL;  // Flip sign bit back
    return (i64)tmp;
}
```

**Why flip sign bit?**: BDB sorts keys as unsigned bytes. Negative integers have high bit set, so they'd sort after positive integers. Flipping makes negative integers sort before positive.

### Record Format

**Table records** (in BDB):
- **Key**: 8-byte encoded rowid
- **Data**: SQLite3 record format (header + typed values)

**Index records** (in BDB):
- **Key**: Encoded index columns + rowid
- **Data**: Empty (key contains all info)

### Shim Layer Patterns

When porting SQLite code that calls `sqlite3Btree*` functions:

**Pattern 1: Direct mapping**:
```c
// SQLite code
rc = sqlite3BtreeOpen(vfs, filename, db, &pBt, flags, vfsFlags);

// DBSQL equivalent
rc = __sm_btree_open(filename, db, &pSm, flags);
```

**Pattern 2: Cursor operations**:
```c
// SQLite code
rc = sqlite3BtreeCursor(pBt, table_id, wrFlag, pKeyInfo, &pCur);
rc = sqlite3BtreeFirst(pCur, &res);
while (res == 0) {
    rc = sqlite3BtreePayload(pCur, offset, amt, buf);
    rc = sqlite3BtreeNext(pCur, flags);
}

// DBSQL equivalent (shim handles translation)
rc = __sm_btree_cursor(pSm, table_id, wrFlag, pKeyInfo, &pCur);
rc = __sm_btree_first(pCur, &res);
while (res == 0) {
    rc = __sm_btree_payload(pCur, offset, amt, buf);
    rc = __sm_btree_next(pCur, flags);
}
```

**Pattern 3: Integer key optimization**:
```c
// SQLite code (direct i64 seek)
i64 rowid = 42;
rc = sqlite3BtreeTableMoveto(pCur, rowid, 0, &res);

// DBSQL shim (encodes rowid internally)
i64 rowid = 42;
rc = __sm_btree_table_moveto(pCur, rowid, 0, &res);
// Internally: encode_rowid(rowid, key), __sm_moveto(pCur, key, 8, &res)
```

### Common Pitfalls

**1. Forgetting to encode/decode rowids**

Wrong:
```c
i64 rowid = 42;
__sm_moveto(cursor, &rowid, sizeof(i64), &res);  // WRONG: needs encoding
```

Right:
```c
i64 rowid = 42;
unsigned char key[8];
encode_rowid(rowid, key);
__sm_moveto(cursor, key, 8, &res);
```

**2. Mixing key and data offsets**

Wrong:
```c
// Trying to read byte 100 of record
__sm_data(cursor, 100, 10, buf);  // WRONG: offset is data-only, not key+data
```

Right:
```c
// Use shim layer for unified payload
__sm_btree_payload(cursor, 100, 10, buf);  // Handles key vs. data offset
```

**3. Cursor invalidation after table modification**

Wrong:
```c
__sm_cursor(sm, table_id, 1, &cursor1);
__sm_cursor(sm, table_id, 1, &cursor2);
__sm_moveto(cursor1, key, 8, &res);
__sm_delete(cursor2);  // Invalidates cursor1!
__sm_next(cursor1, &res);  // WRONG: cursor1 may be invalid
```

Right:
```c
__sm_cursor(sm, table_id, 1, &cursor1);
__sm_cursor(sm, table_id, 1, &cursor2);
__sm_moveto(cursor1, key, 8, &res);
__sm_delete(cursor2);
if (__sm_cursor_has_moved(cursor1)) {
    __sm_cursor_restore(cursor1, &different);
}
__sm_next(cursor1, &res);  // Now safe
```

---

## Porting from SQLite

### When to Port vs. When to Rewrite

**Port** (copy SQLite code):
- Core algorithms (query optimization, B-tree algorithms)
- Built-in functions (date/time, string, math)
- SQL syntax and semantics

**Rewrite** (adapt for DBSQL):
- Storage layer interactions (replace `sqlite3Btree*` with `__sm_*`)
- Memory management (use BDB's allocators when appropriate)
- Platform-specific code (DBSQL may target different platforms)

### Porting Workflow

1. **Identify SQLite source**: Find relevant `.c` file in `_/sqlite/src/`

2. **Copy to DBSQL**: Create corresponding file in `src/`
   ```bash
   cp _/sqlite/src/func.c src/sql_fns.c
   ```

3. **Rename types**: Use DBSQL conventions
   ```c
   // SQLite
   sqlite3 *db;
   Vdbe *p;
   Mem *pMem;

   // DBSQL
   dbsql_t *db;
   vdbe_t *p;
   mem_t *pMem;
   ```

4. **Replace storage calls**: Convert `sqlite3Btree*` to `__sm_*`
   ```c
   // SQLite
   sqlite3BtreeBeginTrans(pBt, wrFlag, &cookie);

   // DBSQL
   __sm_begin_txn_ex(pSm, wrFlag, &cookie);
   ```

5. **Update includes**:
   ```c
   // SQLite
   #include "sqliteInt.h"

   // DBSQL
   #include "dbsql_config.h"
   #include "dbsql_int.h"
   ```

6. **Fix function names**: Add `__` prefix for internal functions
   ```c
   // SQLite
   int sqlite3BtreeInsert(...) { }

   // DBSQL
   int __sm_btree_insert(...) { }
   ```

7. **Test thoroughly**: Run test suite, add new tests if needed

### Common Translation Patterns

**Memory allocation**:
```c
// SQLite
void *p = sqlite3DbMalloc(db, size);
sqlite3DbFree(db, p);

// DBSQL
void *p = __mem_malloc(db->env, size);
__mem_free(db->env, p);
```

**Error handling**:
```c
// SQLite
return SQLITE_NOMEM;
return SQLITE_CORRUPT;

// DBSQL
return DBSQL_NOMEM;
return DBSQL_CORRUPT;
```

**String functions**:
```c
// SQLite
sqlite3_snprintf(sizeof(buf), buf, "%d", value);

// DBSQL
__str_nappend(buf, sizeof(buf), "%d", value);
```

**Assertions**:
```c
// SQLite
assert( x > 0 );

// DBSQL
ASSERT(x > 0);  // Note: uppercase
```

---

## Code Style Guidelines

### Naming Conventions

**Types**: Lowercase with underscores, `_t` suffix
```c
typedef struct vdbe_op vdbe_op_t;
typedef struct mem mem_t;
```

**Functions**:
- Internal: `__function_name()` (double underscore prefix)
- Public API: `dbsql_function_name()` (no prefix)

**Constants**: Uppercase with underscores
```c
#define DBSQL_MAX_LENGTH 1000000000
#define MEM_Int 0x0001
```

**Macros**: Uppercase, but use inline functions when possible
```c
#define MAX(a,b) ((a)>(b)?(a):(b))  // OK for simple cases

// Better: inline function
static inline int max(int a, int b) {
    return (a > b) ? a : b;
}
```

### Formatting

**Indentation**: Tabs (width 8)

**Braces**: K&R style
```c
if (condition) {
    // code
} else {
    // code
}

for (i = 0; i < n; i++) {
    // code
}
```

**Line length**: ≤100 characters

**Whitespace**:
```c
// Function calls
func(arg1, arg2);  // Space after comma

// Operators
x = a + b;  // Spaces around binary operators
x = -y;     // No space for unary operators
ptr->member;  // No spaces around -> or .

// Control structures
if (condition) { }    // Space after keyword
while (condition) { } // Space after keyword
```

**Comments**:
```c
// Single-line comments use //

/*
 * Multi-line comments use this format.
 * Each line starts with *.
 */

/**
 * Function documentation:
 * Brief description.
 *
 * @param arg1 Description of arg1
 * @param arg2 Description of arg2
 * @return Description of return value
 */
int function(int arg1, int arg2);
```

### Best Practices

**Error handling**:
```c
// Always check return values
int rc = function_that_can_fail();
if (rc != DBSQL_SUCCESS) {
    // Handle error
    return rc;
}

// Use goto for cleanup
int rc = DBSQL_SUCCESS;
void *p1 = NULL, *p2 = NULL;

p1 = __mem_malloc(env, size1);
if (p1 == NULL) {
    rc = DBSQL_NOMEM;
    goto cleanup;
}

p2 = __mem_malloc(env, size2);
if (p2 == NULL) {
    rc = DBSQL_NOMEM;
    goto cleanup;
}

// ... use p1 and p2 ...

cleanup:
    if (p1) __mem_free(env, p1);
    if (p2) __mem_free(env, p2);
    return rc;
```

**Assertions**:
```c
// Use assertions for invariants that should never fail
ASSERT(pCursor != NULL);
ASSERT(nReg > 0);

// Don't use assertions for user errors or runtime errors
if (pCursor == NULL) {  // Don't assert, handle error
    return DBSQL_ERROR;
}
```

**Magic numbers**:
```c
// Bad
if (type == 1) { }
if (size < 100) { }

// Good
#define TYPE_INTEGER 1
#define MAX_COLUMN_SIZE 100

if (type == TYPE_INTEGER) { }
if (size < MAX_COLUMN_SIZE) { }
```

---

## Debugging Techniques

### VDBE Tracing

Enable VDBE instruction trace:

```bash
# In dbsql shell
.trace stdout
SELECT * FROM users WHERE id = 42;
```

Output:
```
addr  opcode         p1    p2    p3             p4             p5  comment
----  -------------  ----  ----  -------------  -------------  --  -------
0     Init           0     10    0                             0   Start at 10
1     OpenRead       0     2     0     2                       0   users
2     Integer        42    1     0                             0
3     SeekRowid      0     9     1                             0
4     Column         0     1     2                             0   name
5     ResultRow      2     1     0                             0
6     Next           0     3     0                             0
7     Close          0     0     0                             0
8     Halt           0     0     0                             0
9     Transaction    0     0     0                             0
10    Goto           0     1     0                             0
```

### GDB Debugging

Set up GDB for DBSQL:

```bash
# Create .gdbinit in project root
cat > .gdbinit <<EOF
# Break on errors
break __error_handler
break __panic

# Print VDBE state
define vdbe_state
    if p
        print *p
        print p->aMem[0]@p->nMem
        print p->aOp[p->pc]
    end
end

# Continue past recoverable errors
handle SIGPIPE nostop

# Pretty-print structures
set print pretty on
EOF

# Run with GDB
gdb --args dbsql mydb.db
(gdb) break vdbe.c:1234
(gdb) run
(gdb) vdbe_state
```

Common breakpoints:
```gdb
# Break when executing specific opcode
break vdbe.c:2500 if pOp->opcode == 82  # OP_Column

# Break on specific table
break __sm_cursor if table_id == 2

# Conditional breakpoint
break vdbe.c:3000 if reg[1].flags & MEM_Int && reg[1].u.i == 42
```

### Memory Debugging

Use Valgrind to detect memory errors:

```bash
# Check for leaks and invalid access
valgrind --leak-check=full --track-origins=yes \
    ./dbsql mydb.db < test.sql

# Suppress known issues
valgrind --leak-check=full --suppressions=dbsql.supp \
    ./dbsql mydb.db
```

### Performance Profiling

Use `gprof` for profiling:

```bash
# Build with profiling
../dist/configure CFLAGS="-pg -O2"
make

# Run program (generates gmon.out)
./dbsql mydb.db < benchmark.sql

# Analyze
gprof ./dbsql gmon.out > profile.txt
less profile.txt
```

Or use `perf` (Linux):

```bash
# Record performance data
perf record -g ./dbsql mydb.db < benchmark.sql

# Analyze
perf report
```

### Logging

Add debug logging:

```c
#ifdef DBSQL_DEBUG
    #define DEBUG_LOG(fmt, ...) \
        fprintf(stderr, "[%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define DEBUG_LOG(fmt, ...) ((void)0)
#endif

// Usage
DEBUG_LOG("Executing opcode %d, reg[%d] = %d", opcode, reg_num, reg[reg_num].u.i);
```

### Assertion Debugging

When assertion fails:

```
dbsql: vdbe.c:1234: function_name: Assertion `pCursor != NULL' failed.
Aborted (core dumped)
```

Debug with GDB:
```bash
gdb ./dbsql core
(gdb) backtrace    # See call stack
(gdb) frame 3      # Go to frame where assertion failed
(gdb) info locals  # Inspect local variables
(gdb) print pCursor  # Check why it's NULL
```

---

## Resources

### Documentation

- **SQLite Documentation**: https://sqlite.org/docs.html
  - Understand SQL semantics and features
- **Berkeley DB Reference**: https://docs.oracle.com/cd/E17076_05/html/index.html
  - BDB API reference and programming guide
- **DBSQL Wiki**: https://dbsql.org/wiki/
  - Project-specific documentation

### Tools

- **GDB**: https://sourceware.org/gdb/documentation/
- **Valgrind**: https://valgrind.org/docs/manual/
- **Perf**: https://perf.wiki.kernel.org/

### Source Code References

- `_/sqlite/src/` - SQLite 3.52.0 reference implementation
- `src/vdbe.c` - DBSQL VDBE interpreter
- `src/sm.c` - Storage Manager (Berkeley DB integration)
- Architecture Decision Records in `docs_src/ADR-*.md`

---

## Getting Help

- **Mailing List**: dev@dbsql.org
- **IRC**: #dbsql on Libera.Chat
- **Bug Reports**: https://github.com/gburd/dbsql/issues
- **Code Review**: Submit pull requests on GitHub

---

## Current Modernization Status

### SQLite 3.52.0 Migration (2025-2026)

DBSQL is undergoing major modernization from SQLite 2.8.x to SQLite 3.52.0 architecture.

**Completed Phases**:

1. **Type System Modernization** ✅
   - 51 structures updated to SQLite 3.52.0 naming
   - mem_t, vdbe_t, parse_t, cursor_t, etc.
   - Field renames: `.i` → `.u.i`, `.r` → `.u.r`

2. **Code Generator Modernization** ✅
   - All 12 code generators converted and compiling
   - ~15,000 lines of SQL generation code updated
   - Register-based opcode generation

3. **BerkeleyDB Integration Enhancement** ✅
   - Upgraded to BDB 5.3.21
   - 10 new SM functions for SQLite 3.52.0 compatibility
   - Enhanced cursor and transaction management

4. **RETURNING Clause Implementation** ✅
   - INSERT/UPDATE/DELETE RETURNING support
   - Multi-row results
   - Expression evaluation in RETURNING

**In Progress**:

5. **VDBE Register-Based Conversion** 🔄 (Task #12)
   - 53+ opcodes implemented (36% complete)
   - 376 compilation errors remaining (all in vdbe.c)
   - Requires architectural conversion of `__vdbe_exec()` function
   - Estimated: 27-53 hours remaining

**Planned**:

6. **UPSERT Implementation** 📋
   - Fully planned (95-130 hour estimate)
   - 96 test cases defined
   - Parser rules designed

7. **Window Functions** 📋
   - 200-300 hour estimate
   - 10+ functions planned

8. **Recursive CTEs** 📋
   - 150-200 hour estimate
   - WITH RECURSIVE support

See [PROJECT_STATUS_UPDATE.md](../PROJECT_STATUS_UPDATE.md) for detailed progress tracking.

---

## Conclusion

DBSQL is a unique project that combines SQLite's excellent SQL engine with Berkeley DB's robust storage and replication features. Contributing requires understanding both systems and how they integrate.

The 2025-2026 modernization represents 20 years of SQL evolution, bringing SQLite 3.52.0's register-based architecture while preserving BerkeleyDB's enterprise features. This is an exciting time to contribute, as the foundation is solid and new features are being actively developed.

This guide provides the foundation. As you work on DBSQL, you'll develop deeper knowledge of the codebase. Don't hesitate to ask questions and contribute to the documentation when you learn something new.

Happy hacking!
