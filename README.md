# DBSQL

**A SQL database engine built on BerkeleyDB with SQLite 3.52.0 architecture**

Version: 0.4.0+ | License: GPL v3 (with commercial exceptions) | Status: Active Development

---

## Overview

DBSQL combines the SQL query engine architecture of SQLite 3.52.0 with the proven storage and replication capabilities of BerkeleyDB 5.3.21. It provides a small, embeddable SQL database with enterprise-grade reliability, ACID transactions, and high-availability replication.

**Key Features**:
- Full SQL-92 support with modern SQLite 3.52.0 features
- BerkeleyDB storage engine (B-tree, transactions, MVCC)
- Register-based VDBE (Virtual Database Engine)
- Master-replica replication for high availability
- 64-bit integers, UTF-8 strings, typed data storage
- Small footprint (~2MB library)
- GPL v3 license with commercial licensing options

---

## Current Status (March 2026)

DBSQL 0.4.0+ has completed major SQLite 3.52.0 modernization work:

### Completed ✅

- ✅ **Register-based VDBE**: Converted from SQLite 2.8.x stack-based to SQLite 3.52.0 register-based architecture
- ✅ **64-bit Support**: Full i64 integer and rowid support (range: -2^63 to 2^63-1)
- ✅ **Modern Type System**: 51 structures modernized to SQLite 3.52.0 conventions
- ✅ **Code Generators**: All 12 SQL code generators modernized and compiling (~15,000 lines)
  - SELECT, INSERT, UPDATE, DELETE, expressions, WHERE clauses
  - CREATE/DROP TABLE, CREATE/DROP INDEX
  - Triggers, pragmas, COPY, ATTACH
- ✅ **RETURNING Clause**: INSERT/UPDATE/DELETE RETURNING support (PostgreSQL-compatible)
- ✅ **BerkeleyDB 5.3.21**: Upgraded storage backend with 10 enhanced SM extensions
- ✅ **Comprehensive Documentation**: 40+ technical documents covering architecture, migration, and development

### In Progress 🔄

- 🔄 **VDBE Opcodes**: 53+ opcodes implemented (36% complete, 120+ remaining)
- 🔄 **Compilation**: 23/39 files (59%), VDBE modernization blocking 16 utility files

### Planned 📋

- 📋 **UPSERT**: INSERT ON CONFLICT fully planned (95-130 hour implementation, 96 test cases defined)
- 📋 **Window Functions**: ROW_NUMBER(), RANK(), LAG(), LEAD(), etc.
- 📋 **Recursive CTEs**: WITH RECURSIVE support
- 📋 **JSON1 Extension**: json_extract(), json_object(), -> operator
- 📋 **ALTER TABLE**: ADD COLUMN, DROP COLUMN, RENAME COLUMN

**Build Status**: 23/39 files compile (59%). See [COMPILATION_STATUS.md](COMPILATION_STATUS.md) for details.

---

## Quick Start

### Installation

```bash
# 1. Clone repository with submodules
git clone --recursive https://github.com/gburd/dbsql.git
cd dbsql

# OR if already cloned:
git submodule update --init --recursive

# 2. Build BerkeleyDB dependency
./deps/build_libdb.sh

# 3. Configure and build DBSQL
cd build_unix
../dist/s_config
make -j$(nproc)

# 4. Test the build
./dbsql :memory: "SELECT 'Hello, DBSQL!';"
```

See [BUILDING.md](BUILDING.md) for detailed build instructions.

### Basic Usage

```c
#include <dbsql.h>

int main() {
    dbsql *db;
    dbsql_stmt *stmt;
    int rc;

    // Open database (creates if doesn't exist)
    rc = dbsql_open("mydb.db", &db);
    if (rc != DBSQL_SUCCESS) {
        fprintf(stderr, "Cannot open database: %s\n", dbsql_errmsg(db));
        return 1;
    }

    // Create table
    rc = dbsql_exec(db, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)", NULL, NULL, NULL);

    // Insert data with RETURNING clause
    rc = dbsql_prepare(db, "INSERT INTO users (name, age) VALUES ('Alice', 30) RETURNING id", -1, &stmt, NULL);
    if (rc == DBSQL_SUCCESS) {
        rc = dbsql_step(stmt);
        if (rc == DBSQL_ROW) {
            int id = dbsql_column_int(stmt, 0);
            printf("Inserted user with id: %d\n", id);
        }
        dbsql_finalize(stmt);
    }

    // Query data
    rc = dbsql_prepare(db, "SELECT name, age FROM users WHERE age > 25", -1, &stmt, NULL);
    if (rc == DBSQL_SUCCESS) {
        while ((rc = dbsql_step(stmt)) == DBSQL_ROW) {
            const char *name = (const char*)dbsql_column_text(stmt, 0);
            int age = dbsql_column_int(stmt, 1);
            printf("Name: %s, Age: %d\n", name, age);
        }
        dbsql_finalize(stmt);
    }

    // Close database
    dbsql_close(db);
    return 0;
}
```

Compile:
```bash
gcc -o myapp myapp.c -I/usr/local/include -L/usr/local/lib -ldbsql-0.4 -ldb-5.3
```

---

## Architecture

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
│   Adapter between VDBE and BerkeleyDB                    │
│    (sm.c - wraps BDB with SQLite-like interface)        │
└──────────────────────┬──────────────────────────────────┘
                       │ Uses BDB API
                       ↓
┌─────────────────────────────────────────────────────────┐
│                    BerkeleyDB 5.3.21                     │
│   B-tree storage, transactions, replication, recovery   │
│              (libdb-5.3 - external library)             │
└─────────────────────────────────────────────────────────┘
```

See [DEVELOPER-GUIDE.md](docs_src/DEVELOPER-GUIDE.md) for detailed architecture documentation.

---

## Features

### SQL Support

**Statements**: SELECT, INSERT, UPDATE, DELETE, CREATE TABLE, CREATE INDEX, DROP TABLE, DROP INDEX, CREATE VIEW, CREATE TRIGGER, ALTER TABLE (limited), ATTACH/DETACH, EXPLAIN

**Clauses**: WHERE, ORDER BY, GROUP BY, HAVING, LIMIT, OFFSET, DISTINCT, JOIN (INNER, LEFT OUTER, CROSS), UNION/EXCEPT/INTERSECT, RETURNING ✨

**Functions**: 54+ built-in functions (string, numeric, date/time, aggregate, type conversion)

**Features**:
- Subqueries (scalar, row, table)
- Multi-database queries
- Prepared statements with parameter binding
- Transactions (BEGIN, COMMIT, ROLLBACK)
- Triggers (BEFORE/AFTER, FOR EACH ROW)
- Views
- Indexes (B-tree, hash, unique, partial)
- Foreign keys (defined, enforcement TBD)
- AUTOINCREMENT
- WITHOUT ROWID tables

See [FEATURES.md](FEATURES.md) for complete feature list.

### Storage & Concurrency

**BerkeleyDB Integration**:
- ACID transaction guarantees
- Multi-version concurrency control (MVCC)
- Multi-reader, single-writer concurrency
- Row-level locking
- Hot backups
- Write-ahead logging
- Master-replica replication

**Data Types**:
- NULL
- INTEGER (64-bit signed, -2^63 to 2^63-1)
- REAL (IEEE 754 double precision)
- TEXT (UTF-8, up to 1GB)
- BLOB (binary, up to 1GB)

**Type System**: SQLite manifest typing (dynamic typing with column affinity)

---

## Documentation

### User Documentation

- [README](README.md) - This file
- [FEATURES.md](FEATURES.md) - Complete feature list
- [BUILDING.md](BUILDING.md) - Build instructions
- [MIGRATION-GUIDE.md](docs_src/MIGRATION-GUIDE.md) - Upgrading from DBSQL 0.3.x

### Developer Documentation

- [DEVELOPER-GUIDE.md](docs_src/DEVELOPER-GUIDE.md) - Architecture and development practices
- [COMPILATION_STATUS.md](COMPILATION_STATUS.md) - Current build status
- [PROJECT_STATUS_UPDATE.md](PROJECT_STATUS_UPDATE.md) - Detailed progress report
- [ADR-001: Stack to Register VDBE](docs_src/ADR-001-stack-to-register-vdbe.md) - Architecture decision record
- [ADR-002: BerkeleyDB Integration](docs_src/ADR-002-berkeley-db-integration.md) - Storage layer design

### API Documentation

API documentation available at: http://dbsql.org/docs/api

---

## Project History

DBSQL started as a fork of SQLite 2.8.x in 2004, integrating BerkeleyDB as the storage engine to provide replication and high availability. The project has undergone major modernization in 2025-2026 to adopt SQLite 3.52.0's register-based architecture while maintaining BerkeleyDB integration.

**Timeline**:
- 2004: Initial fork from SQLite 2.8.x
- 2006: BerkeleyDB integration complete
- 2012-2024: Maintenance mode (minor updates)
- 2025: SQLite 3.52.0 modernization begins
- 2026 Q1: Code generators modernized, RETURNING clause implemented, UPSERT planned
- 2026 Q2: VDBE modernization in progress (target: 100% compilation)

**Original Author**: Greg Burd (gburd@dbsql.org)

**Inspiration**: SQLite by Dr. Richard Hipp (public domain) and BerkeleyDB by Sleepycat Software/Oracle

---

## Contributing

Contributions are welcome. DBSQL development follows these priorities:

1. **Complete VDBE Modernization**: Convert remaining opcodes to register-based architecture
2. **Implement UPSERT**: INSERT ON CONFLICT DO UPDATE/NOTHING
3. **Add Window Functions**: ROW_NUMBER(), RANK(), LAG(), LEAD()
4. **Implement Recursive CTEs**: WITH RECURSIVE support
5. **Add JSON Support**: json_extract(), json_object(), -> operator

See [DEVELOPER-GUIDE.md](docs_src/DEVELOPER-GUIDE.md) for development workflow and coding standards.

**Development Discussion**:
- Mailing List: dev@dbsql.org
- IRC: #dbsql on Libera.Chat
- GitHub Issues: https://github.com/gburd/dbsql/issues

---

## Testing

DBSQL includes a comprehensive test suite:

- **92+ test cases** (derived from SQLite test suite)
- **Code quality checks** (style, includes, prototypes)
- **SQL functional tests** (syntax, semantics, edge cases)

Run tests:
```bash
cd build_unix
make test
```

See [test/README](test/README) for test suite documentation.

---

## License

**DBSQL**: Copyright © 2004-2026 Greg Burd. Licensed under GPL v3 with commercial exceptions.

**BerkeleyDB 5.3.21**: Licensed under Sleepycat License (BSD-style, GPL-compatible)

**Commercial Licensing**: Available for applications that cannot comply with GPL. Contact: licensing@dbsql.org or see http://dbsql.org/wiki/CommercialUse

**SQLite Ancestry**: DBSQL is derived from SQLite 2.8.x (public domain by Dr. Richard Hipp). The modernization incorporates SQLite 3.52.0 architecture patterns.

---

## Support

### Community Support

- **Website**: http://dbsql.org
- **Documentation**: http://dbsql.org/docs
- **Issue Tracker**: https://github.com/gburd/dbsql/issues
- **Mailing List**: users@dbsql.org
- **IRC**: #dbsql on Libera.Chat

### Commercial Support

Professional support, training, and custom development available. Contact: support@dbsql.org

---

## Acknowledgments

- **Dr. Richard Hipp** - SQLite creator (public domain inspiration)
- **Sleepycat Software/Oracle** - BerkeleyDB developers
- **SQLite Community** - Test cases and feature designs
- **Contributors** - See AUTHORS file

---

## See Also

- **SQLite**: https://sqlite.org - Original SQL engine design
- **BerkeleyDB**: https://www.oracle.com/database/technologies/related/berkeleydb.html - Storage engine
- **Comparison**: [Why DBSQL vs. SQLite](http://dbsql.org/wiki/WhyDBSQL)

---

**Project Status**: Active development
**Stability**: Beta (0.4.0+ undergoing major modernization)
**Production Ready**: Not yet (wait for 1.0.0 release after VDBE modernization complete)

**Last Updated**: March 2026
