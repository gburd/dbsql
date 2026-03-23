# Building DBSQL

This guide covers building DBSQL from source using both Nix and traditional autoconf methods.

## Prerequisites

DBSQL requires:
- C compiler (GCC 7+ or Clang 10+)
- BerkeleyDB 5.3.21 or compatible (included as submodule)
- TCL 8.5+ (optional, for test suite)
- GNU Make
- Autoconf/Automake (for traditional build)

## Quick Start with Nix (Recommended)

DBSQL includes a complete Nix flake for reproducible builds.

### Build with Nix

```bash
# Clone with submodules
git clone --recursive https://github.com/gburd/dbsql.git
cd dbsql

# Build using Nix
nix build

# Run the binary
./result/bin/dbsql :memory: "SELECT 'Hello, DBSQL!';"
```

### Development Shell

Enter a development environment with all dependencies:

```bash
nix develop

# Now you have access to all build tools
cd build_unix
../dist/s_config
make -j$(nproc)
```

### What Nix Provides

The Nix build:
- Automatically handles all dependencies (BerkeleyDB, TCL, build tools)
- Generates `opcodes.h` from `vdbe.c` during build
- Creates both static library (`libdbsql-0.4.a`) and shared library (`libdbsql-0.4.so`)
- Installs the `dbsql` command-line utility
- Ensures reproducible builds across systems

## Traditional Build (Autoconf)

### Step 1: Clone and Prepare

```bash
# Clone repository with submodules
git clone --recursive https://github.com/gburd/dbsql.git
cd dbsql

# OR if already cloned without submodules:
git submodule update --init --recursive
```

### Step 2: Build BerkeleyDB

DBSQL depends on BerkeleyDB 5.3.21, which must be built first:

```bash
./deps/build_libdb.sh
```

This builds BerkeleyDB and installs it to `deps/libdb/build_unix/.libs/`.

### Step 3: Configure DBSQL

```bash
cd build_unix
../dist/s_config
```

The `s_config` script:
- Generates `configure` from `configure.ac`
- Substitutes version placeholders in templates
- Creates `config.hin` header template

### Step 4: Run Configure

```bash
./configure \
  --with-berkeleydb=../deps/libdb/build_unix/.libs \
  --enable-tcl \
  --with-tcl=/usr/lib/tcl8.5
```

Common configure options:

| Option | Description |
|--------|-------------|
| `--prefix=DIR` | Install to DIR (default: `/usr/local`) |
| `--with-berkeleydb=DIR` | Path to BerkeleyDB installation |
| `--enable-tcl` | Build TCL extension (required for tests) |
| `--with-tcl=DIR` | Path to TCL library directory |
| `--enable-debug` | Build with debug symbols and no optimization |

### Step 5: Build

```bash
make -j$(nproc)
```

This builds:
- `libdbsql-0.4.a` - Static library
- `libdbsql-0.4.so` - Shared library
- `dbsql` - Command-line utility
- `lemon` - Parser generator (build tool)

### Step 6: Test

```bash
# Basic smoke test
./dbsql :memory: "SELECT 'Hello, DBSQL!';"

# Run full test suite (requires TCL)
make test
```

### Step 7: Install

```bash
sudo make install
```

This installs:
- Libraries to `$PREFIX/lib/`
- Headers to `$PREFIX/include/`
- Binary to `$PREFIX/bin/`

## Build Artifacts

### Generated Files

The build process generates several files:

```
build_unix/
├── opcodes.h          # VDBE opcode definitions (from vdbe.c)
├── opcodes.c          # VDBE opcode names array
├── sql_parser.c       # SQL parser (from sql_parser.y via lemon)
├── sql_parser.h       # Parser header
├── dbsql.h            # Public API header (from dbsql.in)
├── dbsql_int.h        # Internal header (from dbsql_int.in)
└── db_int.h           # BerkeleyDB integration header
```

### Libraries

- **Static**: `libdbsql-0.4.a`
  - Contains all DBSQL object files
  - Links BerkeleyDB statically
  - Use: `gcc myapp.c -ldbsql-0.4 -ldb-5.3`

- **Shared**: `libdbsql-0.4.so.0.4.0`
  - Dynamically linked library
  - Symlinks: `libdbsql.so` → `libdbsql-0.4.so` → `libdbsql-0.4.so.0.4.0`
  - Use: `gcc myapp.c -ldbsql`

## Compilation Status (March 2026)

Current build status: **23/39 files compile (59%)**

### Compiling Files ✅

Core functionality (23 files):
- API layer: `api.c`, `api_table.c`
- Code generators: `cg_attach.c`, `cg_auth.c`, `cg_build.c`, `cg_copy.c`, `cg_date.c`, `cg_delete.c`, `cg_expr.c`, `cg_insert.c`, `cg_pragma.c`, `cg_select.c`, `cg_trigger.c`, `cg_update.c`, `cg_where.c`, `cg_vacuum.c`
- Storage: `sm.c`
- Common: `hash.c`, `str.c`, `dbsql_atoi.c`, `dbsql_atof.c`, `dbsql_fop.c`, `dbsql_alloc.c`, `dbsql_err.c`

### In Progress 🔄

VDBE modernization (16 files blocked):
- `vdbe.c` - 53/150+ opcodes implemented
- `vdbe_method.c` - Partially modernized
- Utility files: `sql_fns.c`, `sql_tokenize.c`, `safety.c`, `util.c`

See [COMPILATION_STATUS.md](../COMPILATION_STATUS.md) for detailed progress.

## Troubleshooting

### BerkeleyDB Not Found

**Error**: `configure: error: Cannot find BerkeleyDB library`

**Solution**:
```bash
# Build BerkeleyDB first
./deps/build_libdb.sh

# Then configure with explicit path
./configure --with-berkeleydb=$(pwd)/../deps/libdb/build_unix/.libs
```

### Missing opcodes.h

**Error**: `fatal error: opcodes.h: No such file or directory`

**Cause**: The Makefile generates `opcodes.h` from `vdbe.c` during build.

**Solution**:
```bash
# Ensure you're in build_unix directory
cd build_unix

# Manually generate opcodes.h
grep '^case OP_' ../src/vdbe.c | \
  sed -e 's/://' | \
  awk '{printf "#define %-30s %3d\n", $2, ++cnt}' > opcodes.h
```

### TCL Test Suite Fails

**Error**: `test.tcl: command not found`

**Solution**: Ensure TCL is enabled:
```bash
./configure --enable-tcl --with-tcl=/usr/lib/tcl8.5
make test
```

### Linker Errors with -ldb

**Error**: `undefined reference to 'db_create'`

**Solution**: Link both DBSQL and BerkeleyDB:
```bash
gcc -o myapp myapp.c -ldbsql-0.4 -ldb-5.3
```

### Version Mismatch

**Error**: `incompatible library version`

**Cause**: Multiple DBSQL versions installed.

**Solution**:
```bash
# Check installed versions
ls -l /usr/local/lib/libdbsql*

# Remove old versions
sudo rm /usr/local/lib/libdbsql*

# Reinstall
make clean
make install
```

## Build Configuration

### Debug Build

```bash
./configure --enable-debug
make CFLAGS="-g -O0 -DDBSQL_DEBUG"
```

### Optimized Release Build

```bash
./configure
make CFLAGS="-O3 -DNDEBUG -march=native"
```

### Static Binary

Build `dbsql` with all libraries statically linked:

```bash
make LDFLAGS="-static" dbsql
```

### Cross-Compilation

```bash
./configure --host=aarch64-linux-gnu \
  CC=aarch64-linux-gnu-gcc \
  --with-berkeleydb=/path/to/arm64/libdb
make
```

## Continuous Integration

DBSQL uses Nix for reproducible CI builds:

```yaml
# GitHub Actions example
- name: Build with Nix
  run: nix build --show-trace

- name: Run tests
  run: nix develop -c make test
```

## Build System Architecture

DBSQL uses a hybrid build system:

1. **Nix Flake** (`flake.nix`)
   - Top-level reproducible build
   - Dependency management
   - Development shells

2. **Autoconf** (`configure.ac` in `dist/`)
   - Traditional Unix build
   - Platform detection
   - Library discovery

3. **GNU Make** (`Makefile.in` in `dist/`)
   - Compilation rules
   - Code generation (parser, opcodes)
   - Installation targets

4. **Lemon Parser Generator** (`src/lemon/`)
   - Generates SQL parser from grammar
   - Similar to Yacc/Bison but faster

## Performance Tuning

### Compiler Optimization Levels

| Level | Use Case | Flags |
|-------|----------|-------|
| Debug | Development | `-g -O0` |
| Default | General use | `-O2` |
| Release | Production | `-O3 -DNDEBUG -flto` |
| Size | Embedded | `-Os -DNDEBUG` |

### BerkeleyDB Tuning

Configure BerkeleyDB cache size at runtime:

```c
// 64MB cache
db_env->set_cachesize(db_env, 0, 64 * 1024 * 1024, 1);
```

See [Storage Guide](guides/STORAGE.md) for more tuning options.

## Next Steps

- [API Documentation](api/C_API.md) - Using DBSQL in your application
- [Architecture Overview](architecture/ARCHITECTURE.md) - Understanding DBSQL internals
- [Storage Guide](guides/STORAGE.md) - BerkeleyDB integration details
- [Testing Guide](TESTING.md) - Running and writing tests
