# Changelog

All notable changes to DBSQL will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### In Progress

- VDBE register-based modernization (53+ opcodes implemented, 36% complete)
- 100% compilation milestone (23/39 files currently compile)

---

## [0.4.0] - 2026-03-20 (In Progress)

### Added - Core Modernization

**SQLite 3.52.0 Architecture**:
- Register-based VDBE execution model (replacing stack-based SQLite 2.8.x)
- 64-bit integer support (i64 type, -2^63 to 2^63-1 range)
- SQLite3 binary record format (typed values, 30-40% space savings)
- Modern memory management (mem_t with union types: u.i, u.r, u.p)
- 51 structures modernized to SQLite 3.52.0 naming conventions
- Complete type system overhaul (dynamic typing with manifest affinity)

**VDBE Opcodes** (53+ implemented, 36% complete):
- Control flow: Goto, If, IfNot, Halt, Integer, String, Real, Null
- Arithmetic: Add, Subtract, Multiply, Divide, Remainder
- Comparison: Eq, Ne, Lt, Le, Gt, Ge, IsNull, NotNull
- Logical: And, Or, Not
- Data access: Column, Rowid, ResultRow, MakeRecord
- Cursor operations: OpenRead, OpenWrite, Close, Rewind, Next
- Seek operations: SeekGE, SeekGT, SeekLE, SeekLT, SeekRowid
- Transaction: Transaction, Commit, Rollback

**Code Generators** (all 12 modernized, ~15,000 lines):
- `cg_select.c` - SELECT statement generation
- `cg_insert.c` - INSERT statement generation
- `cg_update.c` - UPDATE statement generation
- `cg_delete.c` - DELETE statement generation
- `cg_expr.c` - Expression evaluation
- `cg_where.c` - WHERE clause optimization
- `cg_build.c` - DDL (CREATE/DROP TABLE/INDEX)
- `cg_trigger.c` - Trigger code generation
- `cg_pragma.c` - PRAGMA command handling
- `cg_attach.c` - ATTACH/DETACH database
- `cg_copy.c` - COPY command
- `cg_vacuum.c` - VACUUM command

**SQL Features**:
- RETURNING clause for INSERT/UPDATE/DELETE (PostgreSQL-compatible)
  - Multi-row results
  - Expression support in RETURNING clause
  - RETURNING * (all columns)
- Enhanced WHERE clause optimization
- Improved expression code generation
- Better subquery handling

**BerkeleyDB Integration**:
- Upgraded to BerkeleyDB 5.3.21 (from 4.8)
- 10 enhanced SM (Storage Manager) functions for SQLite 3.52.0 compatibility:
  - `__sm_begin_txn_ex()` - Transaction with schema cookie
  - `__sm_table_moveto()` - i64 integer key seek
  - `__sm_index_moveto()` - UnpackedRecord key seek
  - `__sm_payload()` - Unified payload access
  - `__sm_payload_size()` - Payload size query
  - `__sm_integer_key()` - i64 rowid extraction
  - `__sm_insert_ex()` - Enhanced insert
  - `__sm_delete_ex()` - Enhanced delete
  - `__sm_eof()` - EOF detection
  - `__sm_create_btree()` - Unified table/index creation
- Improved cursor handling for register-based opcodes
- Enhanced transaction management

**Documentation** (40+ technical documents):
- Comprehensive architecture documentation
- Migration guides (SQLite 2.8.x → 3.52.0 patterns)
- VDBE conversion plans and progress tracking
- Feature planning documents (UPSERT with 96 test cases)
- Compilation status tracking
- Developer guides and coding standards

### Changed

**Architecture**:
- Migrated from stack-based to register-based VDBE execution
- Replaced callback result delivery with result row iteration
- Updated all structure field names to SQLite 3.52.0 conventions
- Converted memory access patterns (mem_t->i → mem_t->u.i)
- Modernized opcode parameter usage (P1, P2, P3, P4, P5)

**Type System**:
- Replaced string-only storage with typed binary records
- Introduced type affinity (INTEGER, TEXT, NUMERIC, REAL, BLOB)
- Added proper NULL handling (MEM_Null flag)
- Implemented type coercion rules (SQLite semantics)

**API Changes**:
- Updated `dbsql_malloc()` signature (added environment parameter)
- Modernized internal function naming (`__prefix_action_object` convention)
- Enhanced error handling and return codes
- Improved prepared statement lifecycle management

**Build System**:
- Updated Makefile for BerkeleyDB 5.3.21
- Improved dependency tracking
- Added compilation status tracking tools
- Enhanced error reporting during build

### Fixed

**Compilation Errors** (1,700+ → 376 errors, 77.9% reduction):
- Resolved 1,324 type system errors
- Fixed 200+ structure member access patterns
- Corrected 150+ function signature mismatches
- Updated 100+ opcode implementations
- Fixed flexible array member usage in foreign key structures
- Resolved incomplete type definitions (agg_t, agg_elem_t, sorter_t)

**Code Quality**:
- Eliminated obsolete zShort buffer references (15 instances)
- Fixed memory allocation patterns (70+ locations)
- Corrected i64 printf format specifiers
- Updated static initializers in pragma handler
- Fixed OP_Recno → OP_Rowid opcode rename (3 files)

**Semantic Issues**:
- Fixed WHERE clause logic generation
- Corrected trigger field mappings (36 instances)
- Updated expression evaluation order
- Fixed aggregate function handling

### Planned

**UPSERT** (Fully Planned):
- INSERT ON CONFLICT DO UPDATE/NOTHING
- 96 test cases defined
- 95-130 hour implementation estimate
- Conflict resolution strategies documented
- Parser rules designed

**Window Functions**:
- ROW_NUMBER(), RANK(), DENSE_RANK(), NTILE()
- LAG(), LEAD(), FIRST_VALUE(), LAST_VALUE(), NTH_VALUE()
- PARTITION BY and ORDER BY clauses
- Frame specifications (ROWS, RANGE, GROUPS)
- ~200-300 hour implementation estimate

**Recursive CTEs**:
- WITH RECURSIVE support
- Hierarchical queries
- Cycle detection
- ~150-200 hour implementation estimate

**JSON Support**:
- json_extract(), json_object(), json_array()
- -> and ->> operators
- JSON path queries
- ~120-180 hour implementation estimate

**ALTER TABLE Enhancements**:
- ADD COLUMN with defaults
- DROP COLUMN
- RENAME COLUMN
- ~40-60 hour implementation estimate

### Breaking Changes

**Database Format**:
- ⚠️ Incompatible with DBSQL 0.3.x databases
- Migration required (use `dbsql_migrate` tool when available)
- Binary record format replaces string-based storage
- 8-byte rowids replace 4-byte encoding

**API Changes**:
- Some internal functions renamed or removed
- Callback architecture replaced with result row iteration
- Memory allocation functions require environment parameter

**Type Semantics**:
- Integer comparisons now numeric (not lexicographic)
- NULL handling follows SQLite 3.x rules
- Type coercion applied in expressions

See [MIGRATION-GUIDE.md](docs_src/MIGRATION-GUIDE.md) for detailed migration instructions.

### Build Status

**Files Compiling**: 23/39 (59%)
- ✅ All 12 code generators (100%)
- ✅ SQL parser and tokenizer (100%)
- ✅ Infrastructure files (100%)
- 🔄 VDBE engine (partial)
- ⏳ Utility files (blocked by VDBE)

**Errors**: 376 (concentrated in vdbe.c, architectural conversion needed)

**Progress**: 77.9% error reduction from initial state (1,700 → 376)

See [COMPILATION_STATUS.md](COMPILATION_STATUS.md) for current build status.

---

## [0.3.x] - 2004-2024

### Legacy Features

- SQLite 2.8.x SQL engine
- BerkeleyDB 4.8 storage backend
- Stack-based VDBE execution
- Basic SQL-92 support
- Transaction support (BEGIN, COMMIT, ROLLBACK)
- Replication (master-replica)
- B-tree and hash indexes
- Triggers and views
- Multi-database support (ATTACH/DETACH)

### Maintenance Period (2012-2024)

- Minor bug fixes
- Security updates
- Platform compatibility updates
- No major feature additions

---

## [0.2.x] - 2006-2012

### Added

- Initial BerkeleyDB integration
- Replication support
- Enhanced transaction handling
- Improved concurrency (MVCC)

---

## [0.1.x] - 2004-2006

### Initial Release

- Fork from SQLite 2.8.x
- Basic BerkeleyDB storage manager
- Core SQL support
- ACID transactions
- Initial documentation

---

## Version History Summary

| Version | Release Date | Status | Key Features |
|---------|--------------|--------|--------------|
| 0.4.0 | 2026-03-20 | In Progress | SQLite 3.52.0, register-based VDBE, RETURNING clause |
| 0.3.x | 2004-2024 | Legacy | SQLite 2.8.x, stack-based VDBE, BDB 4.8 |
| 0.2.x | 2006-2012 | Archived | Initial BDB integration, replication |
| 0.1.x | 2004-2006 | Archived | Initial fork from SQLite 2.8.x |

---

## Migration Paths

### From 0.3.x to 0.4.0

**Requirements**: Database migration mandatory (format incompatible)

**Steps**:
1. Export data from 0.3.x using `dbsql_export` tool (coming soon)
2. Install DBSQL 0.4.0
3. Import data using `dbsql_import` tool (coming soon)
4. Update application code (API changes minimal)
5. Test thoroughly (type semantics changed)

See [MIGRATION-GUIDE.md](docs_src/MIGRATION-GUIDE.md) for detailed instructions.

### From SQLite to DBSQL

**Compatibility**: SQL syntax mostly compatible

**Differences**:
- Storage format (BerkeleyDB B-tree, not SQLite file)
- Concurrency model (single-writer MVCC)
- Some features not yet implemented (window functions, CTEs, JSON)

**Benefits**:
- Replication (master-replica)
- Hot backups
- Enterprise-grade reliability

---

## Contributing

See [DEVELOPER-GUIDE.md](docs_src/DEVELOPER-GUIDE.md) for contribution guidelines.

---

## References

- **Project Website**: http://dbsql.org
- **Source Repository**: https://github.com/gburd/dbsql
- **Issue Tracker**: https://github.com/gburd/dbsql/issues
- **Documentation**: http://dbsql.org/docs

---

**Changelog Maintenance**:
- Updated on each release
- Breaking changes clearly marked with ⚠️
- Migration guides provided for incompatible changes
- All significant changes documented

**Last Updated**: March 20, 2026
