# DBSQL Architecture

DBSQL combines SQLite's SQL query engine architecture with BerkeleyDB's storage layer, creating a hybrid database system that provides SQL capabilities with enterprise-grade storage.

## Overview

**Key Architectural Principle**: DBSQL uses **BerkeleyDB for all storage operations**, not SQLite's btree implementation.

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│              (User code calling DBSQL API)              │
└──────────────────────┬──────────────────────────────────┘
                       │
                       │ dbsql_open(), dbsql_exec(), dbsql_prepare()
                       ↓
┌─────────────────────────────────────────────────────────┐
│                     DBSQL API Layer                      │
│        (api.c, api_table.c - Public interfaces)         │
│                                                          │
│  • Connection management                                │
│  • Statement preparation                                │
│  • Result set handling                                  │
│  • Transaction control                                  │
└──────────────────────┬──────────────────────────────────┘
                       │
                       │ Calls parser
                       ↓
┌─────────────────────────────────────────────────────────┐
│                   SQL Parser & Compiler                  │
│   ┌──────────────┐  ┌──────────────┐  ┌─────────────┐  │
│   │ Tokenizer    │→│ Lemon Parser │→│ Code Gen     │  │
│   │(tokenize.c)  │  │(sql_parser.y)│  │(cg_*.c)      │  │
│   └──────────────┘  └──────────────┘  └─────────────┘  │
│                                                          │
│  • Lexical analysis (token stream)                      │
│  • Syntax analysis (AST construction)                   │
│  • Semantic analysis (type checking, name resolution)   │
│  • VDBE bytecode generation                             │
└──────────────────────┬──────────────────────────────────┘
                       │
                       │ Generates VDBE program
                       ↓
┌─────────────────────────────────────────────────────────┐
│          Virtual Database Engine (VDBE)                  │
│        Register-based VM (SQLite 3.52.0 arch)           │
│         (vdbe.c, vdbe_method.c, vdbe_int.h)             │
│                                                          │
│  • Register-based bytecode interpreter                  │
│  • 150+ opcodes (53 implemented as of Mar 2026)         │
│  • Cursor management                                    │
│  • Expression evaluation                                │
│  • Row assembly and result set generation               │
└──────────────────────┬──────────────────────────────────┘
                       │
                       │ Calls storage APIs
                       ↓
┌─────────────────────────────────────────────────────────┐
│              Storage Manager (SM) Layer                  │
│         Adapter between VDBE and BerkeleyDB             │
│                      (sm.c)                             │
│                                                          │
│  • Table/index file management                          │
│  • Cursor operations (open, seek, next)                 │
│  • Key generation and formatting                        │
│  • Transaction coordination                             │
│  • Schema metadata storage                              │
└──────────────────────┬──────────────────────────────────┘
                       │
                       │ Uses BDB API: db_create(), cursor->get()
                       ↓
┌─────────────────────────────────────────────────────────┐
│                    BerkeleyDB 5.3.21                     │
│          B-tree Storage, ACID Transactions              │
│              (libdb-5.3 - external library)             │
│                                                          │
│  • B-tree and Hash index structures                     │
│  • MVCC (Multi-version concurrency control)             │
│  • Write-ahead logging (WAL)                            │
│  • Transaction management (BEGIN/COMMIT/ROLLBACK)       │
│  • Master-replica replication                           │
│  • Hot backup and recovery                              │
└─────────────────────────────────────────────────────────┘
                       │
                       │ File I/O
                       ↓
                 Disk Storage
         (*.db files, transaction logs)
```

## Components

### 1. API Layer

**Files**: `api.c`, `api_table.c`

Provides the public C API compatible with SQLite's interface:

```c
// Connection management
int dbsql_open(const char *filename, dbsql **ppDb);
int dbsql_close(dbsql *db);

// Statement lifecycle
int dbsql_prepare(dbsql *db, const char *zSql, int nByte,
                  dbsql_stmt **ppStmt, const char **pzTail);
int dbsql_step(dbsql_stmt *pStmt);
int dbsql_finalize(dbsql_stmt *pStmt);

// Result access
const unsigned char *dbsql_column_text(dbsql_stmt*, int iCol);
int dbsql_column_int(dbsql_stmt*, int iCol);
```

### 2. SQL Parser

**Files**: `sql_tokenize.c`, `sql_parser.y` (generates `sql_parser.c`)

#### Tokenizer (`sql_tokenize.c`)

Converts SQL text into tokens:
- Keywords: `SELECT`, `FROM`, `WHERE`, `INSERT`, etc.
- Identifiers: table names, column names
- Literals: strings, numbers
- Operators: `=`, `<`, `>`, `+`, `-`, etc.

#### Parser (`sql_parser.y`)

Lemon-generated LALR(1) parser that builds an Abstract Syntax Tree (AST):
- Parses SQL-92 syntax + modern extensions
- Constructs in-memory representation of queries
- Handles expressions, subqueries, joins, aggregates
- Error reporting with line/column information

**Key structures**:
```c
typedef struct select select_t;     // SELECT statement
typedef struct expr expr_t;         // Expressions (WHERE, etc.)
typedef struct src_list src_list_t; // FROM clause tables
typedef struct id_list id_list_t;   // Column lists
```

### 3. Code Generators

**Files**: `cg_*.c` (15 modules)

Convert AST into VDBE bytecode:

| Module | Purpose |
|--------|---------|
| `cg_select.c` | SELECT statements |
| `cg_insert.c` | INSERT statements |
| `cg_update.c` | UPDATE statements |
| `cg_delete.c` | DELETE statements |
| `cg_expr.c` | Expressions (WHERE, computed columns) |
| `cg_where.c` | WHERE clause optimization |
| `cg_build.c` | CREATE/DROP TABLE/INDEX |
| `cg_trigger.c` | Trigger compilation |
| `cg_pragma.c` | PRAGMA statements |
| `cg_attach.c` | ATTACH/DETACH database |
| `cg_copy.c` | COPY statement |
| `cg_vacuum.c` | VACUUM (reorganize database) |

#### Example: SELECT Code Generation

For `SELECT name FROM users WHERE age > 25`:

1. **Table scan setup** (`cg_select.c`)
   - Emit `OP_OpenRead` to open cursor on "users" table
   - Allocate registers for row data

2. **WHERE clause** (`cg_where.c`)
   - Emit `OP_Rewind` to start at first row
   - Loop: `OP_Column` to load "age" column
   - `OP_Gt` to compare age > 25
   - `OP_IfNot` to skip non-matching rows

3. **Result row** (`cg_select.c`)
   - `OP_Column` to load "name" column
   - `OP_ResultRow` to output result

4. **Loop control**
   - `OP_Next` to advance to next row
   - Jump back to step 2

### 4. Virtual Database Engine (VDBE)

**Files**: `vdbe.c`, `vdbe_method.c`, `vdbe_int.h`

Register-based bytecode interpreter based on SQLite 3.52.0 architecture.

#### Register-Based Design

Unlike stack-based VMs, VDBE uses a register file:

```c
typedef struct vdbe vdbe_t;
struct vdbe {
    mem_t *a_mems;       // Array of registers
    int n_mems;          // Number of registers
    cursor_t **ap_csrs;  // Array of cursors
    int n_csrs;          // Number of cursors
    vdbe_op_t *a_ops;    // Array of opcodes
    int n_ops;           // Number of opcodes
    int pc;              // Program counter
};
```

**Advantages**:
- Fewer memory operations (no push/pop)
- Better register allocation
- Simpler optimization
- More efficient expression evaluation

#### VDBE Opcodes (Examples)

| Opcode | Description |
|--------|-------------|
| `OP_Init` | Initialize program, jump to main |
| `OP_OpenRead` | Open cursor for reading |
| `OP_OpenWrite` | Open cursor for writing |
| `OP_Rewind` | Move cursor to first row |
| `OP_Column` | Read column value into register |
| `OP_Integer` | Load integer literal into register |
| `OP_String8` | Load string literal into register |
| `OP_Add` | Add two registers |
| `OP_Eq` | Compare two registers for equality |
| `OP_IfNot` | Conditional jump if register is false |
| `OP_ResultRow` | Output current row to result set |
| `OP_Next` | Advance cursor to next row |
| `OP_Insert` | Insert row into table |
| `OP_Delete` | Delete current row |
| `OP_Halt` | Stop execution |

**Status**: 53/150+ opcodes implemented (March 2026)

#### Memory Management

VDBE uses typed memory cells:

```c
typedef struct mem mem_t;
struct mem {
    union {
        i64 i;          // INTEGER
        double r;       // REAL
        void *p;        // Pointer (TEXT, BLOB)
    } u;
    char *z;            // String data
    int n;              // Length
    u16 flags;          // Type flags
    void (*xDel)(void*); // Destructor
};
```

Flags:
- `MEM_Null` - NULL value
- `MEM_Int` - 64-bit integer
- `MEM_Real` - IEEE 754 double
- `MEM_Str` - UTF-8 string
- `MEM_Blob` - Binary data
- `MEM_Static` - Data is constant (don't free)
- `MEM_Dyn` - Data is dynamically allocated (call xDel)

### 5. Storage Manager (SM) Layer

**File**: `sm.c`

Adapter between VDBE and BerkeleyDB, translating SQLite-style storage operations to BDB API calls.

#### Key Responsibilities

1. **Database File Management**
   - Each SQL table → one BDB database file
   - Each SQL index → one BDB database file
   - Schema metadata stored in special `__DBSQL_meta__` database

2. **Cursor Operations**
   ```c
   // VDBE calls SM functions:
   __sm_table_open(dbsql_t *db, const char *table_name, cursor_t *cursor);
   __sm_table_moveto(cursor_t *cursor, i64 rowid);
   __sm_table_next(cursor_t *cursor);
   __sm_table_insert(cursor_t *cursor, i64 rowid, const void *data, int len);
   ```

3. **Key Generation**
   - SQL rowid (64-bit integer) → BDB key (8-byte big-endian)
   - Index keys: composite keys from multiple columns
   - Sequence generation for AUTOINCREMENT

4. **Transaction Coordination**
   - Maps SQL BEGIN/COMMIT/ROLLBACK to BDB transactions
   - Manages transaction nesting
   - Coordinates with BDB's MVCC

#### BerkeleyDB Integration Points

```c
// SM wraps BDB operations:
DB *db;                       // BDB database handle
DBC *dbc;                     // BDB cursor
DB_TXN *txn;                  // BDB transaction
DB_ENV *env;                  // BDB environment

// Example: Table scan
db->cursor(db, txn, &dbc, 0);
while (dbc->get(dbc, &key, &data, DB_NEXT) == 0) {
    // Process row
}
dbc->close(dbc);
```

### 6. BerkeleyDB Storage Layer

**Library**: `libdb-5.3.so` (external dependency)

DBSQL relies on BerkeleyDB for all physical storage:

#### Storage Structures

- **B-tree**: Primary storage for tables and indexes
  - Sorted by key
  - Efficient range scans
  - Supports duplicates (for indexes with non-unique keys)

- **Hash**: Optional for exact-match indexes
  - Faster point lookups
  - No range scan support

#### ACID Properties

1. **Atomicity**: All-or-nothing transactions via write-ahead log
2. **Consistency**: Constraints enforced at SQL layer
3. **Isolation**: MVCC provides snapshot isolation
4. **Durability**: WAL flushed before commit acknowledgment

#### Concurrency Model

- **Multi-reader**: Unlimited concurrent readers via MVCC
- **Single-writer**: One writer at a time per database
- **Row-level locking**: Minimal contention

#### Files on Disk

For database `myapp.db`:
```
myapp.db                    # Main database file
  ├── __DBSQL_meta__        # Schema metadata
  ├── __DBSQL_users_dbi__   # Sequence for "users" table
  ├── table_2.db            # "users" table data
  ├── table_3.db            # "products" table data
  └── index_4.db            # Index on users(email)
log.0000000001              # Transaction log
log.0000000002              # Transaction log (rotated)
__db.001                    # BDB metadata
```

## Data Flow Examples

### SELECT Query

```sql
SELECT name, email FROM users WHERE age > 25 ORDER BY name;
```

1. **Parser** tokenizes and parses SQL
2. **Code Generator** (`cg_select.c`):
   - Allocates registers: r1=rowid, r2=name, r3=email, r4=age
   - Emits opcodes:
     ```
     0: OpenRead cursor=0 table=users
     1: Rewind cursor=0 goto=10
     2: Column cursor=0 col=3 reg=4   # Load age
     3: Integer literal=25 reg=5
     4: Gt reg=4 reg=5 jump=9         # if age > 25
     5: Column cursor=0 col=1 reg=2   # Load name
     6: Column cursor=0 col=2 reg=3   # Load email
     7: ResultRow reg=2 cols=2         # Output (name, email)
     9: Next cursor=0 goto=2
    10: Close cursor=0
    11: Halt
     ```
3. **VDBE** executes bytecode:
   - `OP_OpenRead` calls `__sm_table_open()`
   - `OP_Column` calls `__sm_table_data()` to read column
   - `OP_ResultRow` returns row to API layer
4. **Storage Manager** translates to BDB:
   - Opens BDB cursor: `db->cursor(db, txn, &dbc, 0)`
   - Scans rows: `dbc->get(dbc, &key, &data, DB_NEXT)`
   - Extracts column from BDB data buffer
5. **BerkeleyDB** reads from B-tree on disk

### INSERT Query

```sql
INSERT INTO users (name, email, age) VALUES ('Alice', 'alice@example.com', 30);
```

1. **Parser** parses INSERT statement
2. **Code Generator** (`cg_insert.c`):
   ```
   0: OpenWrite cursor=0 table=users
   1: NewRowid cursor=0 reg=1        # Allocate new rowid
   2: String8 'Alice' reg=2
   3: String8 'alice@example.com' reg=3
   4: Integer 30 reg=4
   5: MakeRecord reg=2 cols=3 reg=5   # Pack into row format
   6: Insert cursor=0 key=r1 data=r5
   7: Close cursor=0
   8: Halt
   ```
3. **VDBE** executes
4. **Storage Manager**:
   - `__sm_get_rowid()` generates next rowid from BDB sequence
   - `__sm_table_insert()` inserts row
5. **BerkeleyDB**:
   - `db->put(db, txn, &key, &data, 0)` writes to B-tree

## Architecture Decisions

### Why BerkeleyDB Instead of SQLite's btree?

**Advantages**:
1. **Replication**: BDB provides built-in master-replica replication
2. **Proven Reliability**: Used in Oracle products, LDAP servers, etc.
3. **Concurrent Access**: MVCC allows many readers without blocking
4. **Transaction Management**: Industrial-strength WAL and recovery
5. **Licensing**: BSD-style license compatible with GPL

**Tradeoffs**:
- Larger footprint than SQLite's btree
- External dependency (libdb-5.3)
- Different concurrency model (single writer)

### Why SQLite 3.52.0 VDBE Architecture?

**Advantages**:
1. **Performance**: Register-based is faster than stack-based
2. **Modern Features**: Support for CTEs, window functions, UPSERT
3. **Maturity**: SQLite 3.x is battle-tested
4. **64-bit Support**: Native i64 integers and rowids

**Migration Path**:
- DBSQL started as SQLite 2.8.x fork (2004)
- Modernization to 3.52.0 began in 2025
- Maintains BDB storage integration throughout

### Single Writer Model

DBSQL inherits BDB's single-writer concurrency model:
- One transaction can write at a time
- Unlimited concurrent readers via MVCC
- Writers don't block readers (snapshot isolation)

**Rationale**: Simplifies consistency guarantees while providing excellent read scalability.

## Performance Characteristics

### Memory Usage

Typical memory per connection:
- Parser state: ~16 KB
- VDBE registers: 1 KB per 100 registers
- Cursor buffers: 4-16 KB per open cursor
- BDB cache: Configurable (default: 256 KB per database)

### Disk Usage

- Tables: Variable row size (8-byte rowid + data)
- Indexes: Key size + 8-byte rowid pointer
- Overhead: ~2% for B-tree structure
- Transaction logs: Rotate at 10 MB by default

### Latency

Typical operation latencies (uncached):
- Simple SELECT: 100-200 µs
- INSERT: 200-500 µs (includes WAL flush)
- UPDATE: 300-600 µs
- Complex JOIN: 1-10 ms

Cached operations are 10-100x faster.

## Future Architecture

Planned enhancements:
1. **Multi-writer support** via BDB's multi-process locking
2. **Query optimizer** based on SQLite 3.x query planner
3. **Window functions** (requires VDBE opcode additions)
4. **JSON support** (json1 extension)
5. **Full-text search** via FTS5 extension

## Related Documentation

- [Storage Guide](../guides/STORAGE.md) - BerkeleyDB integration details
- [VDBE Internals](VDBE.md) - Virtual machine implementation
- [Code Generators](CODE_GENERATORS.md) - SQL-to-bytecode compilation
- [API Reference](../api/C_API.md) - Public API documentation
