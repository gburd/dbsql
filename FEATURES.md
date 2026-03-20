# DBSQL Features

**Version**: 0.4.0+
**Last Updated**: March 2026
**SQLite Compatibility**: Based on SQLite 3.52.0 architecture

---

## SQL Standards Compliance

- **SQL-92 Core**: 90%+ compliant
- **SQLite 3.52.0 Compatibility**: Partial (core features implemented, modern features in progress)
- **Database Engine**: BerkeleyDB 5.3.21 with enhanced SM layer

---

## Supported SQL Statements

### Data Query Language (DQL)

**SELECT Statement**:
- ✅ Basic SELECT with column list or `*`
- ✅ WHERE clause with complex boolean expressions
- ✅ ORDER BY (ASC/DESC, multiple columns)
- ✅ GROUP BY with HAVING clause
- ✅ LIMIT and OFFSET
- ✅ DISTINCT
- ✅ Subqueries (scalar, row, table)
- ✅ UNION, EXCEPT, INTERSECT set operations

**Joins**:
- ✅ INNER JOIN
- ✅ LEFT OUTER JOIN
- ✅ CROSS JOIN
- ✅ NATURAL JOIN
- ✅ Join using ON and USING clauses

**Aggregate Functions**:
- ✅ COUNT(), SUM(), AVG(), MIN(), MAX()
- ✅ GROUP_CONCAT()
- ✅ TOTAL() (always returns REAL, never NULL)

### Data Manipulation Language (DML)

**INSERT Statement**:
- ✅ INSERT with VALUES clause
- ✅ INSERT with SELECT subquery
- ✅ Multi-row INSERT
- ✅ INSERT OR REPLACE
- ✅ INSERT OR IGNORE
- ✅ RETURNING clause (PostgreSQL-compatible) ← New in 0.4.0

**UPDATE Statement**:
- ✅ UPDATE with WHERE clause
- ✅ Multi-column updates
- ✅ Expressions in SET clause
- ✅ UPDATE OR REPLACE/IGNORE
- ✅ RETURNING clause ← New in 0.4.0

**DELETE Statement**:
- ✅ DELETE with WHERE clause
- ✅ DELETE without WHERE (truncate table)
- ✅ RETURNING clause ← New in 0.4.0

**Planned**:
- 📋 UPSERT (INSERT ON CONFLICT) - Fully planned (95-130 hour implementation)

### Data Definition Language (DDL)

**CREATE TABLE**:
- ✅ Column definitions with types (INTEGER, REAL, TEXT, BLOB)
- ✅ PRIMARY KEY constraints
- ✅ UNIQUE constraints
- ✅ NOT NULL constraints
- ✅ DEFAULT values
- ✅ CHECK constraints
- ✅ FOREIGN KEY constraints (defined, enforcement TBD)
- ✅ AUTOINCREMENT
- ✅ WITHOUT ROWID tables

**CREATE INDEX**:
- ✅ B-tree indexes
- ✅ Hash indexes (BerkeleyDB-specific)
- ✅ Multi-column indexes
- ✅ UNIQUE indexes
- ✅ Partial indexes (WHERE clause)

**Other DDL**:
- ✅ DROP TABLE
- ✅ DROP INDEX
- ⚠️ ALTER TABLE (limited - only RENAME TABLE currently)
- ✅ CREATE VIEW
- ✅ DROP VIEW
- ✅ CREATE TRIGGER
- ✅ DROP TRIGGER

**Planned**:
- 📋 ALTER TABLE ADD COLUMN
- 📋 ALTER TABLE DROP COLUMN
- 📋 ALTER TABLE RENAME COLUMN
- 📋 Generated columns (VIRTUAL/STORED)

### Transaction Control

**Transaction Statements**:
- ✅ BEGIN [DEFERRED|IMMEDIATE|EXCLUSIVE]
- ✅ COMMIT
- ✅ ROLLBACK
- ✅ ROLLBACK TO savepoint (limited)

**Transaction Features**:
- ✅ ACID guarantees (via BerkeleyDB)
- ✅ Multi-reader, single-writer concurrency (MVCC)
- ✅ Row-level locking
- ⚠️ Savepoints (partially implemented)

### Database Attachment

- ✅ ATTACH DATABASE
- ✅ DETACH DATABASE
- ✅ Multi-database queries (cross-database joins)

### Query Introspection

- ✅ EXPLAIN [QUERY PLAN]
- ✅ Query plan visualization
- ✅ Opcode listing

### PRAGMA Commands

**Implemented**:
- ✅ `PRAGMA table_info(table)` - Column information
- ✅ `PRAGMA index_list(table)` - Index listing
- ✅ `PRAGMA database_list` - Attached databases
- ✅ `PRAGMA cache_size` - Memory cache configuration
- ✅ `PRAGMA page_size` - Database page size
- ✅ `PRAGMA synchronous` - Sync mode
- ✅ `PRAGMA foreign_keys` - Enable/disable FK checks
- ✅ `PRAGMA schema_version` - Schema version number

**Not Yet Implemented**:
- ⏳ `PRAGMA integrity_check` - Database validation
- ⏳ `PRAGMA foreign_key_check` - FK constraint validation
- ⏳ `PRAGMA optimize` - Index optimization

---

## Built-in Functions

### Scalar Functions (54+)

**String Functions**:
- `length(X)` - String length in characters
- `lower(X)` - Convert to lowercase
- `upper(X)` - Convert to uppercase
- `substr(X, Y, Z)` - Extract substring
- `trim(X)`, `ltrim(X)`, `rtrim(X)` - Remove whitespace
- `replace(X, Y, Z)` - Replace occurrences
- `like(X, Y)`, `glob(X, Y)` - Pattern matching
- `instr(X, Y)` - Find substring position
- `char(X1, X2, ...)` - Create string from character codes
- `coalesce(X, Y, ...)` - First non-NULL value
- `ifnull(X, Y)` - NULL replacement
- `nullif(X, Y)` - Return NULL if equal
- `quote(X)` - SQL literal quoting
- `soundex(X)` - Phonetic encoding
- `hex(X)` - Hexadecimal encoding
- `unicode(X)` - First character's Unicode code point
- `printf(FORMAT, ...)` - Formatted string

**Numeric Functions**:
- `abs(X)` - Absolute value
- `round(X, Y)` - Round to Y decimal places
- `random()` - Random integer
- `randomblob(N)` - Random blob of N bytes
- `min(X, Y, ...)` - Minimum value
- `max(X, Y, ...)` - Maximum value
- `sign(X)` - Sign (-1, 0, 1)
- `ceil(X)`, `floor(X)` - Ceiling and floor
- `typeof(X)` - Type name (null, integer, real, text, blob)
- `zeroblob(N)` - Zero-filled blob

**Date/Time Functions**:
- `date(timestring, modifier, ...)` - Date as YYYY-MM-DD
- `time(timestring, modifier, ...)` - Time as HH:MM:SS
- `datetime(timestring, modifier, ...)` - Date and time
- `julianday(timestring, modifier, ...)` - Julian day number
- `strftime(format, timestring, modifier, ...)` - Custom formatting
- `unixepoch(timestring, modifier, ...)` - Unix timestamp
- `timediff(t1, t2)` - Time difference

**Modifiers**: 'now', 'start of day', 'start of month', 'start of year', '+N days', '+N hours', '-N minutes', 'weekday N', 'localtime', 'utc'

**Type Conversion**:
- `cast(X AS type)` - Explicit type conversion
- `typeof(X)` - Get type name

**Aggregate Functions**:
- `count(*)`, `count(X)` - Count rows
- `sum(X)` - Sum of values
- `avg(X)` - Average value
- `min(X)` - Minimum value
- `max(X)` - Maximum value
- `group_concat(X, separator)` - Concatenate strings
- `total(X)` - Sum (never NULL, returns 0.0)

**System Functions**:
- `last_insert_rowid()` - Last inserted rowid
- `changes()` - Rows affected by last statement
- `total_changes()` - Total rows affected in connection
- `sqlite_version()` - Version string
- `dbsql_version()` - DBSQL version

---

## Storage Backend

### BerkeleyDB 5.3.21 Integration

**Features**:
- B-tree and Hash access methods
- ACID transaction support
- Multi-version concurrency control (MVCC)
- Row-level locking
- Write-ahead logging (WAL)
- Hot backups
- Replication (master/replica)
- Encryption support (optional)

**Storage Manager Layer**:
DBSQL provides an enhanced SM (Storage Manager) layer that adapts BerkeleyDB to SQLite 3.52.0 semantics:

**Enhanced SM Functions** (10 new functions for SQLite 3.52.0):
- `__sm_begin_txn_ex()` - Enhanced transaction with schema cookie
- `__sm_table_moveto()` - i64 integer key seek
- `__sm_index_moveto()` - UnpackedRecord key seek
- `__sm_payload()` - Unified payload access (key + data)
- `__sm_payload_size()` - Payload size query
- `__sm_integer_key()` - i64 rowid extraction
- `__sm_insert_ex()` - Enhanced insert with payload
- `__sm_delete_ex()` - Enhanced delete with flags
- `__sm_eof()` - Explicit EOF detection
- `__sm_create_btree()` - Unified table/index creation

**Benefits**:
- Proven stability (BDB 5.3.21 released 2012)
- Last version with Sleepycat License (BSD-style, GPL-compatible)
- Enterprise-grade replication and HA features
- Efficient B-tree storage with compression

---

## Architecture Highlights

### VDBE (Virtual Database Engine)

**Architecture**: Register-based virtual machine (SQLite 3.52.0 model)

**Features**:
- 190+ opcode instruction set (53+ implemented, 37% complete)
- Register-based execution model
- Query plan caching
- Expression evaluation
- Cursor management
- Sort and aggregation operations

**Register Model**:
```
Register array: aMem[0..nMem-1]
Each register stores a typed value (mem_t):
- INTEGER (i64)
- REAL (double)
- TEXT (UTF-8 string)
- BLOB (binary data)
- NULL
```

**Opcode Categories** (implemented):
- ✅ Control flow (Goto, If, IfNot, Halt)
- ✅ Arithmetic (Add, Subtract, Multiply, Divide, Remainder)
- ✅ Comparison (Eq, Ne, Lt, Le, Gt, Ge)
- ✅ Logical (And, Or, Not)
- ✅ Data access (Column, Rowid, Integer, String, Real, Null)
- ✅ Cursor operations (OpenRead, OpenWrite, Close, SeekGE, SeekGT, SeekLE, SeekLT)
- ✅ Result handling (ResultRow, MakeRecord)
- 🔄 Database modification (Insert, Delete, partial)
- ⏳ Aggregation (partial)
- ⏳ Sorting (partial)

### Type System

**Dynamic Typing** (SQLite manifest typing model):
- Five storage classes: NULL, INTEGER, REAL, TEXT, BLOB
- Type affinity per column: INTEGER, TEXT, NUMERIC, REAL, BLOB
- Automatic type conversion when appropriate
- Type preservation (integers stay integers, text stays text)

**Type Affinity Rules**:
- INTEGER affinity: Prefers integer storage
- TEXT affinity: Prefers text storage (even for numbers)
- NUMERIC affinity: Prefers integer or real
- REAL affinity: Prefers floating point
- BLOB affinity: No preference

**Data Representation**:
- Integers: 64-bit signed (i64) - range: -2^63 to 2^63-1
- Reals: IEEE 754 double precision (64-bit)
- Text: UTF-8 encoded strings (up to 1GB)
- Blobs: Raw binary data (up to 1GB)

### Record Format

**SQLite3 Binary Record Format**:
```
Header: Varint count + type codes
Body: Raw values (integers, reals, strings, blobs)
```

**Benefits**:
- 30-40% space savings vs. text-based storage
- Fast type identification
- Efficient sorting and comparison
- Zero-copy string/blob access

### Concurrency

**Multi-reader, Single-writer** (BerkeleyDB MVCC):
- Multiple concurrent readers (no blocking)
- Single writer at a time (exclusive lock)
- Row-level locking within writer
- Snapshot isolation for readers
- Deadlock detection and resolution

**Transaction Modes**:
- DEFERRED: Lock acquired on first read/write
- IMMEDIATE: Write lock acquired immediately
- EXCLUSIVE: Exclusive lock acquired immediately

---

## Planned Features

### Short Term (Next Release)

1. **Complete VDBE Modernization** (In Progress)
   - Convert remaining 120+ opcodes to register-based
   - Achieve 100% compilation
   - Full opcode test coverage

2. **Window Functions** (Planned)
   - ROW_NUMBER(), RANK(), DENSE_RANK()
   - LAG(), LEAD()
   - FIRST_VALUE(), LAST_VALUE(), NTH_VALUE()
   - Requires ~200-300 hours implementation

3. **ALTER TABLE ADD COLUMN** (Planned)
   - Schema evolution support
   - Default values
   - NOT NULL constraints with defaults

### Medium Term

4. **UPSERT (INSERT ON CONFLICT)** (Fully Planned)
   - DO UPDATE and DO NOTHING actions
   - 96 test cases defined
   - 95-130 hour implementation estimated

5. **Recursive CTEs (WITH RECURSIVE)** (Planned)
   - Hierarchical queries
   - Graph traversal
   - Organizational charts
   - Requires ~150-200 hours implementation

6. **JSON1 Extension** (Planned)
   - json_extract(), json_object(), json_array()
   - JSON path queries
   - -> and ->> operators
   - Requires ~120-180 hours implementation

### Long Term

7. **Full-Text Search (FTS5)** (Planned)
   - Requires virtual tables framework
   - Unicode tokenization
   - Ranking (BM25)
   - Snippet/highlight functions
   - Requires ~300-500 hours implementation

8. **R-Tree Spatial Indexing** (Planned)
   - Requires virtual tables framework
   - Geospatial queries
   - Bounding box searches
   - Requires ~150-250 hours implementation

9. **Additional PRAGMA Commands** (Planned)
   - integrity_check
   - foreign_key_check
   - optimize

---

## Limitations

### Not Supported (SQLite 2.x Removals)

- ❌ Stack-based VDBE (replaced with register-based)
- ❌ Callback result delivery (replaced with result row iteration)
- ❌ 32-bit-only builds (64-bit i64 required)
- ❌ String-based storage (replaced with typed binary records)

### Not Yet Implemented (SQLite 3.52.0 Features)

- ⏳ Virtual tables framework
- ⏳ Window functions
- ⏳ UPSERT (INSERT ON CONFLICT)
- ⏳ Generated columns (VIRTUAL/STORED)
- ⏳ Recursive CTEs (WITH RECURSIVE)
- ⏳ JSON functions
- ⏳ ALTER TABLE variants (ADD/DROP/RENAME COLUMN)
- ⏳ PRAGMA integrity_check/foreign_key_check/optimize
- ⏳ Full FTS5 support
- ⏳ R-Tree spatial indexing

### Performance Characteristics

**Strengths**:
- Excellent single-writer performance
- Efficient B-tree storage
- Fast integer key lookups
- Good read concurrency (MVCC)

**Limitations**:
- Single writer (no concurrent writes)
- No write-write concurrency within transaction
- BerkeleyDB overhead vs. pure in-memory SQLite

---

## Comparison with SQLite

### Advantages over SQLite

1. **Replication**: BerkeleyDB provides master-replica replication for high availability
2. **Enterprise Features**: Hot backups, encryption, advanced recovery
3. **Mature Storage Engine**: 25+ years of production use
4. **License Flexibility**: GPL + commercial licensing options

### Differences from SQLite

1. **Storage Format**: BerkeleyDB B-tree (not SQLite file format)
2. **No Write-Ahead Logging**: Uses BDB's logging mechanism instead
3. **Concurrency Model**: Single-writer MVCC (SQLite supports concurrent writes with WAL)
4. **Feature Set**: SQL-92 core + SQLite 3.52.0 (partial), missing some modern SQLite features

---

## System Requirements

### Minimum Requirements

- **OS**: Linux, macOS, Unix (POSIX-compliant)
- **CPU**: x86_64 or ARM64 (64-bit required)
- **RAM**: 64MB minimum, 256MB+ recommended
- **Disk**: ~10MB for binaries, variable for databases
- **Compiler**: GCC 7+ or Clang 10+ with C99 support

### Recommended Configuration

- **RAM**: 512MB+ for medium databases (10K-1M rows)
- **Disk**: SSD for better transaction performance
- **Page Size**: 8KB (default) or 16KB for large records

---

## License

**DBSQL**: GPL v3 with commercial licensing exceptions

**BerkeleyDB 5.3.21**: Sleepycat License (BSD-style, GPL-compatible)

For commercial licensing without GPL restrictions, see:
http://dbsql.org/wiki/CommercialUse

---

## Resources

- **Project Website**: http://dbsql.org
- **Documentation**: http://dbsql.org/docs
- **Source Code**: https://github.com/gburd/dbsql
- **Issue Tracker**: https://github.com/gburd/dbsql/issues
- **SQLite Compatibility**: Based on SQLite 3.52.0 (January 2024)
- **BerkeleyDB Docs**: https://docs.oracle.com/cd/E17275_01/html/index.html

---

**Last Updated**: March 2026
**Document Version**: 1.0
**DBSQL Version**: 0.4.0+
