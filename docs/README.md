# DBSQL Documentation

Comprehensive documentation for DBSQL, a SQL database engine combining SQLite's architecture with BerkeleyDB storage.

## Getting Started

**New to DBSQL?** Start here:

1. [Build Guide](BUILD.md) - Install and build DBSQL
2. [SQL Reference](reference/SQL_REFERENCE.md) - Learn SQL syntax
3. [C API Reference](api/C_API.md) - Integrate into your application

## Documentation Structure

### 📚 Reference Documentation

Complete reference for DBSQL features:

- **[SQL Reference](reference/SQL_REFERENCE.md)** - Full SQL syntax guide
  - Data types and type affinity
  - SELECT, INSERT, UPDATE, DELETE statements
  - DDL (CREATE TABLE, CREATE INDEX, etc.)
  - Transactions and concurrency
  - Built-in functions
  - RETURNING clause (PostgreSQL-compatible)

### 🔧 API Documentation

Programming interfaces:

- **[C API Reference](api/C_API.md)** - Core C API (SQLite-compatible)
  - Connection management
  - Statement preparation and execution
  - Parameter binding
  - Result retrieval
  - Complete examples

- **[TCL API Reference](api/TCL_API.md)** - TCL extension interface
  - TCL commands and procedures
  - Test suite utilities
  - Scripting examples

### 📖 Guides

In-depth explanations:

- **[Storage Guide](guides/STORAGE.md)** - BerkeleyDB integration
  - Database file organization
  - B-tree storage structures
  - Transactions and MVCC
  - Concurrency model
  - Backup and recovery
  - Performance tuning
  - Replication setup

### 🏗️ Architecture Documentation

Understanding DBSQL internals:

- **[Architecture Overview](architecture/ARCHITECTURE.md)** - System design
  - Component architecture (API → Parser → VDBE → SM → BerkeleyDB)
  - Register-based VDBE (SQLite 3.52.0)
  - SQL parser and code generators
  - Storage manager layer
  - Data flow examples

### 🔨 Build & Test

Development workflows:

- **[Build Guide](BUILD.md)** - Building from source
  - Nix build (recommended)
  - Traditional autoconf build
  - Build artifacts and generated files
  - Troubleshooting
  - Cross-compilation

- **[Testing Guide](TESTING.md)** - Test suite
  - Running tests
  - Test directory structure
  - Writing new tests
  - Regression testing
  - Performance benchmarks

## Key Concepts

### DBSQL Uses BerkeleyDB for Storage

**Important**: DBSQL does NOT use SQLite's btree implementation. All storage is provided by BerkeleyDB 5.3.21.

**Why BerkeleyDB?**
- Master-replica replication
- MVCC concurrency control
- Hot backup and recovery
- Industrial-strength transactions
- Proven reliability (used in LDAP, Oracle products)

**Implications**:
- Multiple database files (one per table/index)
- BerkeleyDB's single-writer, multi-reader concurrency
- Different file format (not compatible with SQLite databases)
- Built-in replication support

### Register-Based VDBE

DBSQL uses SQLite 3.52.0's register-based Virtual Database Engine:

- Modern bytecode architecture (vs. legacy stack-based)
- 150+ opcodes (53 implemented as of March 2026)
- 64-bit integers and rowids
- Efficient expression evaluation

### SQL Compatibility

DBSQL implements SQL-92 with SQLite 3.52.0 syntax extensions:

- Full SELECT, INSERT, UPDATE, DELETE
- Joins, subqueries, aggregates
- Indexes, triggers, views
- Transactions (BEGIN, COMMIT, ROLLBACK)
- **RETURNING clause** (PostgreSQL-compatible) ✨

### Differences from SQLite

| Feature | SQLite | DBSQL |
|---------|--------|-------|
| Storage | Single file btree | BerkeleyDB (multiple files) |
| Concurrency | WAL mode or locking | BDB MVCC (always) |
| Replication | No | Yes (BDB replication) |
| File format | Proprietary | BerkeleyDB B-tree |
| License | Public domain | GPL v3 + commercial |

## Quick Reference

### Common Tasks

**Open database**:
```c
dbsql *db;
dbsql_open("myapp.db", &db);
```

**Execute SQL**:
```c
dbsql_exec(db, "CREATE TABLE users (id INTEGER, name TEXT)", NULL, NULL, NULL);
```

**Query with parameters**:
```c
dbsql_stmt *stmt;
dbsql_prepare(db, "SELECT * FROM users WHERE age > ?", -1, &stmt, NULL);
dbsql_bind_int(stmt, 1, 25);
while (dbsql_step(stmt) == DBSQL_ROW) {
    const char *name = dbsql_column_text(stmt, 1);
    printf("%s\n", name);
}
dbsql_finalize(stmt);
```

**Transaction**:
```c
dbsql_exec(db, "BEGIN", NULL, NULL, NULL);
dbsql_exec(db, "INSERT INTO ...", NULL, NULL, NULL);
dbsql_exec(db, "COMMIT", NULL, NULL, NULL);
```

**RETURNING clause**:
```sql
INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com') RETURNING id;
UPDATE users SET age = age + 1 WHERE id = 1 RETURNING age;
DELETE FROM users WHERE age < 18 RETURNING id, name;
```

## Project Status (March 2026)

### ✅ Completed

- Register-based VDBE architecture (SQLite 3.52.0)
- 64-bit integers and rowids
- All 12 code generators modernized
- RETURNING clause support
- BerkeleyDB 5.3.21 integration
- 23/39 files compiling (59%)

### 🔄 In Progress

- VDBE opcodes: 53/150+ implemented (36%)
- Full compilation: 16 utility files blocked on VDBE completion

### 📋 Planned

- UPSERT (INSERT ON CONFLICT)
- Window functions (ROW_NUMBER, RANK, etc.)
- Recursive CTEs (WITH RECURSIVE)
- JSON support (json1 extension)
- ALTER TABLE extensions

See [Project Status](../PROJECT_STATUS_UPDATE.md) for detailed progress.

## Additional Resources

### External Documentation

- [SQLite Documentation](https://www.sqlite.org/docs.html) - SQL syntax reference (DBSQL is compatible)
- [BerkeleyDB Docs](https://docs.oracle.com/cd/E17076_05/html/index.html) - Storage layer details
- [TCL Documentation](https://www.tcl.tk/man/) - TCL language reference

### Repository

- **Main README**: [../README.md](../README.md)
- **Feature List**: [../FEATURES.md](../FEATURES.md)
- **Compilation Status**: [../COMPILATION_STATUS.md](../COMPILATION_STATUS.md)
- **GitHub**: https://github.com/gburd/dbsql

### Community

- **Mailing List**: users@dbsql.org
- **IRC**: #dbsql on Libera.Chat
- **Issues**: https://github.com/gburd/dbsql/issues
- **Commercial Support**: support@dbsql.org

## Contributing

Documentation improvements welcome! See [Contributing Guide](../CONTRIBUTING.md) for guidelines.

**Documentation Style**:
- Clear, factual language (avoid "comprehensive", "robust", etc.)
- Code examples for every API function
- Emphasize BerkeleyDB storage (not SQLite btree)
- Include performance implications
- Link to related documentation

## License

Documentation: Copyright © 2004-2026 Greg Burd. Licensed under GPL v3.

Code examples: Public domain (use freely in your projects).

---

**Last Updated**: March 2026

For questions or corrections, contact: docs@dbsql.org
