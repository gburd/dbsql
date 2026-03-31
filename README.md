# DBSQL - A SQL Database Engine

DBSQL is a small SQL92 relational database layer built on top of BerkeleyDB. It provides a SQLite-compatible interface while leveraging BerkeleyDB's robust storage engine.

## Dependencies

DBSQL uses BerkeleyDB 5.3.21 as its storage backend. The source is included as a git submodule.

### Initial Setup

After cloning the repository, initialize and build BerkeleyDB:

```bash
# Initialize the submodule
git submodule update --init --recursive

# Build BerkeleyDB
./deps/build_libdb.sh
```

The build script will configure and compile BerkeleyDB in `deps/libdb/build_unix/`.

### Building DBSQL

```bash
# Create build directory and configure
mkdir -p build_unix
cd build_unix
../dist/s_config
../dist/configure

# Build
make -j$(nproc)

# Optional: Run tests
make test
```

The configure script will automatically detect and use the BerkeleyDB submodule. If you have BerkeleyDB installed elsewhere, you can specify its location:

```bash
../dist/configure --with-berkeleydb=/path/to/berkeleydb
```

### Build Requirements

- C compiler (gcc or clang)
- Make
- Autoconf/automake (for regenerating configure script)
- BerkeleyDB 5.3.21 (included as submodule)

## Project History

This work started from the SQLite project by Dr. Richard Hipp. DBSQL extends that foundation by integrating BerkeleyDB as the storage engine, providing features like:

- Multi-version concurrency control (MVCC)
- ACID transactions with fine-grained locking
- Proven reliability from BerkeleyDB's 25+ year history

## License

dbsql is distributed under the ISC license; see `LICENSE`.  See LICENSE for details.

BerkeleyDB is licensed under the Sleepycat Public License (compatible with GPL for open source use). Review both licenses before redistributing.


## Resources

- Website: http://dbsql.org
- Documentation: See `docs/index.html` in this distribution
- BerkeleyDB Documentation: https://docs.oracle.com/cd/E17276_01/html/index.html
