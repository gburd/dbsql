# dbsql

dbsql is the SQL language of [SQLite](https://sqlite.org) running on
[Berkeley DB (libdb)](https://github.com/berkeleydb/libdb) storage, written
in Berkeley DB's coding style.

- **SQL:** every SQL feature of SQLite 3.53.4, plus the FTS5 and ICU
  extensions and the full interactive shell, as `dbsql`.
- **Storage:** libdb v2026.10.5 provides storage, transactions, logging
  and crash recovery.  Each SQL database is one libdb file, with tables and
  indexes as sub-databases; the directory is the libdb environment.
- **Concurrency:** SQLite WAL-mode semantics on libdb MVCC: readers see a
  snapshot and never block a commit; one writer per database at a time.
- **API:** a Berkeley DB-style C API (`DBSQL` handles, `dbsql_*`
  functions, `DBSQL_*` constants).  There is no `sqlite3_*` API.

dbsql began in 2007 as a fork of SQLite 2.8 with the b-tree and pager
replaced by Berkeley DB.  Release 1.2.0 is a rebuild on SQLite 3.53.4: the
SQL layer is produced from the SQLite release by a repeatable transform
(`dist/transpile`), and the storage manager (`src/sm`) maps SQLite's
b-tree interface onto libdb's public API.

## Status

**1.2.0**, October 2026.  Qualified on Debian 13 amd64; see
`.agent/qualification/QUALIFICATION.md` for what was measured.
Differences from SQLite are listed in `.agent/steering/DIVERGENCES.md`:
there is no SQLite file format, rollback journal or WAL file, and
page-level pragmas report what libdb provides.  Known libdb defects that
affect dbsql are reported upstream with reproducers in
`.agent/libdb-bugs/`; dbsql carries source fixes for four of them (R7,
R9, R10, R11) in `deps/patches/`.

### Upgrading

1.2.0 keeps the libdb pin at v2026.10.5 and does not change dbsql's
on-disk format: it reads databases written by 1.0.0 and 1.1.0.  It adds
a VACUUM page-size fix and runs the SQLite suite per test case (1111 of
1181 files pass); there are no migration steps from 1.1.0.

1.1.0 moved the libdb pin from v2026.10.4 to v2026.10.5.  Your SQL
databases and their logs are **unchanged** -- the binaries read databases
written by 1.0.0.  libdb v2026.10.5 did change its internal *region*
format (the shared-memory `__db.NNN` files), so you cannot run a v2026.10.5
binary (1.1.0 or 1.2.0) against an environment left open/live by a
v2026.10.4 one (1.0.0) and vice versa: a mixed-version attach returns
`DB_VERSION_MISMATCH` (surfaced as "unable to open database file").  To
upgrade from 1.0.0 cleanly, with no dbsql process running against the
directory:

1. close every connection (or run `db_recover` with the old version);
2. delete the region files: `rm __db.0*` in the database directory;
3. reopen with the 1.1.0+ binary -- it recreates the regions and recovers
   from the (unchanged) log.

The region files are rebuilt automatically on the next open; only a
*concurrent* mixed-version attach is the error.

The previous code (the SQLite 2.8 derivative, and a 2026-03 hybrid) is
preserved at the git tags `archive/sqlite28-2024` and
`archive/hybrid-2026-03`.

## Building

Requires a C compiler, autoconf, `ed`, Tcl 8.6 (for tests), and
optionally ICU, zlib and readline/libedit.

```sh
git submodule update --init deps/libdb
sh deps/build_libdb.sh --prefix=$HOME/libdb && make -C deps/libdb/build_unix install
cd dist && sh s_config && cd ..        # generate dist/configure
cd build_unix
../dist/configure --with-berkeleydb=$HOME/libdb [--enable-test]
make -j
make check                             # needs --enable-test and the
                                       # SQLite 3.53.4 source tree in
                                       # ../sqlite-3.53.4 (SQLITE_SRC)
make install
```

`make check` runs the unit, API, shell and libdb (crash, recovery,
multi-process) tests and SQLite's own Tcl test suite, prepared from the
SQLite release; failures that follow from a listed divergence are in
`test/tcl/known-failures.txt`, each with its reason.

## Using it

```sh
$ dbsql ex1.db
dbsql version 1.2.0 (SQL 3.53.4, libdb storage)
dbsql> CREATE TABLE t(x); INSERT INTO t VALUES(1); SELECT * FROM t;
1
```

```c
#include <dbsql.h>

DBSQL *db;
dbsql_stmt_t *st;
dbsql_open("ex1.db", &db);
dbsql_prepare_v2(db, "SELECT x FROM t", -1, &st, NULL);
while (dbsql_step(st) == DBSQL_ROW)
	printf("%lld\n", (long long)dbsql_column_int64(st, 0));
dbsql_finalize(st);
dbsql_close(db);
```

Link with `pkg-config --cflags --libs dbsql`.  The API is SQLite's C API
with `sqlite3_` replaced by `dbsql_` and `SQLITE_` by `DBSQL_`; SQLite's
documentation describes it.

## License

dbsql is distributed under the ISC license; see `LICENSE`.   SQLite is in the public
domain.  libdb is distributed under the Sleepycat license; review its
terms before redistributing.
