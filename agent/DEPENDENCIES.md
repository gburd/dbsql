# DBSQL Dependencies

**Version**: 0.4.0+
**Last Updated**: 2025-03-17

## Overview

DBSQL has minimal external dependencies by design. The primary dependency is **BerkeleyDB** as the storage backend, which provides ACID transactions, concurrent access, and data persistence.

---

## Core Dependencies

### BerkeleyDB 4.8.30

**Type**: Storage Backend (REQUIRED)
**Version**: 4.8.30
**License**: Sleepycat License (BSD-style, GPL-compatible)
**Website**: https://www.oracle.com/database/technologies/related/berkeleydb.html

#### Why BerkeleyDB?

DBSQL uses BerkeleyDB as its storage engine instead of implementing low-level storage primitives. This provides:

- **ACID Transactions** - Full transactional semantics with rollback support
- **Concurrent Access** - Multi-reader, single-writer (MVCC) for high concurrency
- **Crash Recovery** - Automatic recovery from system failures
- **Proven Reliability** - 25+ years of production use in mission-critical systems
- **Replication** - High availability through BDB's replication features (future work)

#### Integration Architecture

DBSQL implements a **Storage Manager (SM) layer** (`src/sm.c`) that wraps BerkeleyDB APIs:

| DBSQL Function | BerkeleyDB API | Purpose |
|----------------|----------------|---------|
| `__sm_open()` | `DB->open()` | Open database file |
| `__sm_cursor()` | `DB->cursor()` | Create cursor for iteration |
| `__sm_table_moveto()` | `DBC->get()` | Seek to table row by rowid |
| `__sm_index_moveto()` | `DBC->get()` | Seek to index entry by key |
| `__sm_payload()` | `DBC->get()` | Retrieve row data |
| `__sm_put()` | `DB->put()` | Insert/update row |
| `__sm_delete()` | `DB->del()` | Delete row |
| `__sm_begin_txn_ex()` | `DB_ENV->txn_begin()` | Start transaction |
| `__sm_commit()` | `DB_TXN->commit()` | Commit transaction |
| `__sm_abort()` | `DB_TXN->abort()` | Rollback transaction |

**Total SM Extensions**: 10 critical functions for SQLite 3.52.0 integration (see `src/sm.c`)

---

## Installation Methods

### Option 1: Nix Package Manager (Current Build System)

**Recommended for**: Development, reproducible builds

```bash
# DBSQL build system uses Nix-provided BDB automatically
cd /home/gburd/ws/dbsql/build_unix
../dist/s_config
make
```

**Current Configuration**:
- Include path: `/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/include`
- Library path: `/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib`
- Linked library: `-ldb-4.8`

### Option 2: System Package Manager

#### Debian/Ubuntu

```bash
sudo apt-get install libdb4.8-dev
```

#### RHEL/CentOS/Fedora

```bash
sudo yum install libdb-devel
# or
sudo dnf install libdb-devel
```

#### macOS (Homebrew)

```bash
brew install berkeley-db@4
```

**Note**: macOS Homebrew only provides BDB 18.1.x by default. For 4.8.30, use:

```bash
brew tap homebrew/versions
brew install berkeley-db4
```

#### FreeBSD

```bash
pkg install db48
```

### Option 3: Build from Source

**Download**: https://www.oracle.com/database/technologies/related/berkeleydb-downloads.html

```bash
# Download db-4.8.30.tar.gz from Oracle
wget https://download.oracle.com/berkeley-db/db-4.8.30.tar.gz
tar -xzf db-4.8.30.tar.gz
cd db-4.8.30/build_unix

# Configure and build
../dist/configure --prefix=/usr/local --enable-shared --disable-static
make -j$(nproc)
sudo make install

# Update DBSQL build configuration
cd /home/gburd/ws/dbsql/build_unix
# Edit Makefile to point to /usr/local/include and /usr/local/lib
```

---

## License Considerations

### BerkeleyDB Sleepycat License

BerkeleyDB 4.8.30 is licensed under the **Sleepycat License**, which is:

- **BSD-style** with GPL-compatible terms
- **Requires**: If you distribute BDB with your application and the application is not open source, you must obtain a commercial license from Oracle
- **Permits**: Free use in open source projects under GPL-compatible licenses

**Official License Text**: https://opensource.org/licenses/Sleepycat

### DBSQL License

DBSQL is licensed under the **GPL v2** (see `LICENSE` file).

### Combined Licensing for Commercial Use

If you wish to use DBSQL in a **closed-source commercial application**:

1. **DBSQL**: Purchase a commercial license from DBSQL project (see `http://dbsql.org/wiki/CommercialUse`)
2. **BerkeleyDB**: Purchase a commercial license from Oracle (see https://www.oracle.com/database/technologies/related/berkeleydb.html)

**For open source GPL projects**: No commercial licenses required.

---

## Version Compatibility

### Why BerkeleyDB 4.8.30?

DBSQL targets **BDB 4.8.30** (released 2010) for these reasons:

1. **Stable API** - Well-tested, no surprises
2. **Wide Availability** - Available in most package managers
3. **License Clarity** - Sleepycat License (pre-AGPL v3 transition)
4. **Known Good** - DBSQL SM layer developed against 4.8.x APIs

### BerkeleyDB Version History

| Version | Release | License | Notes |
|---------|---------|---------|-------|
| 4.8.30 | 2010 | Sleepycat | ✅ Current DBSQL target |
| 5.x | 2010-2012 | Sleepycat | Compatible, untested |
| 6.x | 2013-2017 | AGPL v3 | License change, API compatible |
| 18.1.x | 2018-present | AGPL v3 | Latest, API mostly compatible |

### Migration to BDB 18.1.x (Future Work)

**Status**: Not yet tested, planned for future release

**Benefits**:
- Active maintenance and bug fixes
- Performance improvements
- Modern platform support

**Risks**:
- API changes may require SM layer updates
- AGPL v3 license considerations for commercial use
- Behavior differences requiring testing

**Recommendation**: Upgrade to BDB 18.1.x in a future DBSQL release after thorough SM layer testing.

---

## Build Configuration

### Current Makefile Configuration

From `build_unix/Makefile`:

```makefile
CPPFLAGS= -I$(builddir) -I$(srcdir) \
          -I/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/include \
          -D_GNU_SOURCE -D_REENTRANT

LIBSO_LIBS= -L/nix/store/mlhib3c2ra8bj36vhxmwqhyxph8a8sgf-db-4.8.30/lib \
            -ldb-4.8
```

### Customizing for System BDB

If you have BDB installed in a non-standard location:

```bash
cd build_unix
../dist/s_config \
  --with-berkeleydb=/path/to/bdb \
  CPPFLAGS="-I/path/to/bdb/include" \
  LDFLAGS="-L/path/to/bdb/lib"
make
```

---

## Verification

### Check BDB Installation

```bash
# Check header files
ls -l /usr/include/db.h
# or
ls -l /usr/local/include/db.h

# Check library files
ls -l /usr/lib/libdb-4.8.so
# or
ls -l /usr/local/lib/libdb-4.8.so

# Check version
db_verify -V
```

### Verify DBSQL Linkage

After building DBSQL:

```bash
cd build_unix

# Check shared library dependencies
ldd ./dbsql | grep db
# Expected output: libdb-4.8.so => /path/to/libdb-4.8.so

# Check linked BDB version
strings ./dbsql | grep "Berkeley DB"
```

---

## Alternative Storage Backends (Future Work)

DBSQL's SM layer abstraction could theoretically support alternative backends:

| Backend | License | Pros | Cons | Status |
|---------|---------|------|------|--------|
| **BerkeleyDB 4.8.30** | Sleepycat | Proven, stable | Old (2010) | ✅ CURRENT |
| **BerkeleyDB 18.1.x** | AGPL v3 | Modern, maintained | License change | ⏸️ PLANNED |
| **LMDB** | OpenLDAP Public | Fast, simple | No write transactions | ❌ NOT IMPLEMENTED |
| **RocksDB** | Apache 2.0 | High performance | Complex | ❌ NOT IMPLEMENTED |
| **SQLite** | Public Domain | Self-contained | Circular dependency | ❌ NOT APPLICABLE |

**Current Recommendation**: Continue using BerkeleyDB for transaction semantics and proven reliability.

---

## Development Dependencies

### Build Tools

**Required**:
- GCC 4.8+ or Clang 3.4+
- GNU Make 3.81+
- Autoconf 2.69+
- Libtool 2.4+

**Optional**:
- Lemon parser generator (included in `src/lemon/`)
- Tcl 8.5+ (for test suite)
- Valgrind (for memory debugging)

### Testing

DBSQL includes a test suite adapted from SQLite:

```bash
cd build_unix
make test
```

**Test Requirements**:
- Tcl 8.5+ (`tclsh`)
- BerkeleyDB 4.8.30 installed
- ~50MB disk space for test databases

---

## Troubleshooting

### "db.h: No such file or directory"

**Cause**: BerkeleyDB headers not found

**Solution**:
```bash
# Find BDB installation
find /usr -name "db.h" 2>/dev/null

# Update Makefile CPPFLAGS with correct path
# or install BDB: apt-get install libdb-dev
```

### "undefined reference to `db_create`"

**Cause**: BerkeleyDB library not linked

**Solution**:
```bash
# Check library path
find /usr -name "libdb*.so" 2>/dev/null

# Update Makefile LIBSO_LIBS with correct path and library name
# Example: -L/usr/lib/x86_64-linux-gnu -ldb-4.8
```

### "BerkeleyDB version mismatch"

**Cause**: Multiple BDB versions installed

**Solution**:
```bash
# Check installed versions
dpkg -l | grep libdb  # Debian/Ubuntu
rpm -qa | grep db     # RHEL/CentOS

# Explicitly link to 4.8:
# Update Makefile: -ldb-4.8 (not -ldb)
```

---

## Future Work

### Git Submodule Integration

**Status**: Investigated, blocked

**Issue**: BDB 4.8.30 not available on GitHub (https://github.com/berkeleydb/libdb only has v5.3.x, v1.85)

**Alternative Approaches**:
1. **Vendor BDB 4.8.30 source** - Download from Oracle, commit to `deps/bdb/`
2. **Use Debian source package** - Import from Debian apt source
3. **Upgrade to BDB 18.1.x** - Use modern GitHub tags (requires SM layer testing)

**Recommendation**: Keep external dependency until build is fully stable, then reconsider vendoring.

---

## References

- **BerkeleyDB Documentation**: https://docs.oracle.com/cd/E17276_01/html/index.html
- **BerkeleyDB Downloads**: https://www.oracle.com/database/technologies/related/berkeleydb-downloads.html
- **Sleepycat License**: https://opensource.org/licenses/Sleepycat
- **DBSQL Storage Manager**: `src/sm.c` (10 SM extensions)
- **Commercial Licensing**: http://dbsql.org/wiki/CommercialUse

---

## Document Maintenance

**Created**: 2025-03-17
**Last Updated**: 2025-03-17
**Maintainer**: DBSQL Development Team

**Change Log**:
- 2025-03-17: Initial dependencies documentation created
