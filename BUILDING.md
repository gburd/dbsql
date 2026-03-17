# Building DBSQL

This document describes how to build DBSQL from source.

## Prerequisites

- GCC or compatible C compiler
- Make
- Git (for submodule management)
- Autoconf/Automake (if regenerating build files)

## Quick Start

```bash
# 1. Clone repository with submodules
git clone --recursive <repository-url>
cd dbsql

# OR if already cloned without submodules:
git submodule update --init --recursive

# 2. Build BerkeleyDB dependency
./deps/build_libdb.sh

# 3. Configure and build DBSQL
cd build_unix
../dist/s_config
make

# 4. Test the build
./dbsql :memory: "SELECT 1;"
```

## Dependencies

### BerkeleyDB 5.3.21

DBSQL uses BerkeleyDB as its storage backend. The source is included as a git submodule at `deps/libdb/`.

**Why BerkeleyDB 5.3.21?**
- Last version with Sleepycat License (BSD-style, GPL-compatible)
- Proven stability (2012 release)
- Compatible with DBSQL's storage manager layer

**Submodule Setup:**

The BerkeleyDB submodule is configured in `.gitmodules`:
```
[submodule "deps/libdb"]
    path = deps/libdb
    url = https://github.com/berkeleydb/libdb.git
```

**Building BerkeleyDB:**

```bash
# Automated build (recommended):
./deps/build_libdb.sh

# OR manual build:
cd deps/libdb/build_unix
../dist/configure --enable-shared --disable-static
make -j$(nproc)
```

The compiled library will be at `deps/libdb/build_unix/.libs/libdb-5.3.so` (~2MB).

### Build Configuration

The DBSQL Makefile is configured to use the local BerkeleyDB build:

```makefile
CPPFLAGS = ... -I$(builddir)/../deps/libdb/build_unix
LIBSO_LIBS = -L$(builddir)/../deps/libdb/build_unix/.libs -ldb-5.3
```

## Build Process

### 1. Generate Build Files

```bash
cd build_unix
../dist/s_config
```

This runs the configure script and generates Makefiles.

### 2. Compile

```bash
make
```

This compiles:
- Core DBSQL library (`libdbsql-0.4.so`)
- Command-line utility (`dbsql`)
- All code generators and VDBE engine

### 3. Install (Optional)

```bash
sudo make install
```

Default install location: `/usr/local/DBSQL.0.4/`

## Build Output

Successful build produces:

```
build_unix/
├── libdbsql-0.4.la          # Libtool library
├── .libs/
│   └── libdbsql-0.4.so      # Shared library
└── dbsql                     # Command-line interface
```

## Troubleshooting

### Submodule Not Initialized

```
Error: BerkeleyDB submodule not initialized
```

**Fix:** Run `git submodule update --init --recursive`

### BerkeleyDB Not Built

```
/usr/bin/ld: cannot find -ldb-5.3
```

**Fix:** Run `./deps/build_libdb.sh`

### Makefile Regeneration

If build configuration changes aren't reflected:

```bash
# The Makefile is regenerated from dist/Makefile.in
# Modify dist/Makefile.in, then:
cd build_unix
rm Makefile
../dist/s_config
```

## Development Build

For development with debug symbols:

```bash
make CFLAGS="-g -O0 -DCONFIG_TEST"
```

## Clean Build

```bash
make clean          # Remove object files
make distclean      # Remove all generated files
```

## Current Build Status

**DBSQL 0.4.0+** (Post-SQLite 3.52.0 Modernization)

- ✅ All code generators compile (cg_insert, cg_delete, cg_update, cg_expr)
- ✅ SQL parser compiles
- ⚠️  VDBE engine requires modernization (stack→register conversion)
- 🔄 Build: 34/35 files compiling successfully

## Architecture

DBSQL architecture (post-modernization):

```
┌─────────────────────────────────────┐
│   SQL Parser (sql_parser.y)        │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│   Code Generators (cg_*.c)          │
│   - cg_select, cg_insert,           │
│   - cg_update, cg_delete, cg_expr   │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│   VDBE (Virtual Database Engine)    │
│   Register-based bytecode executor  │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│   Storage Manager (sm.c)            │
│   Abstraction over BerkeleyDB       │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│   BerkeleyDB 5.3.21                 │
│   B-tree storage, transactions      │
└─────────────────────────────────────┘
```

## License

DBSQL is licensed under GPL v3 with exceptions (see LICENSE_EXCEPTIONS).

BerkeleyDB 5.3.21 is licensed under the Sleepycat License (BSD-style, GPL-compatible).

## More Information

- Project website: http://dbsql.org
- BerkeleyDB docs: https://docs.oracle.com/cd/E17275_01/html/index.html
- SQLite compatibility: Based on SQLite 3.52.0 architecture
