# dbsql

dbsql is the SQL language of [SQLite](https://sqlite.org) running on
[Berkeley DB (libdb)](https://github.com/berkeleydb/libdb) storage, written
in Berkeley DB's coding style.

It began in 2007 as a fork of SQLite 2.8 with the b-tree and pager replaced
by Berkeley DB. It is being rebuilt on **SQLite 3.53.4** and **libdb
v2026.10.2**, with these goals:

- Every SQL feature of SQLite 3.53.4, plus the FTS5 and ICU extensions and
  the full interactive shell (as `dbsql`).
- libdb for storage, transactions, logging and crash recovery. Each SQL
  database is one libdb file, with tables and indexes as sub-databases.
- A Berkeley DB-style C API (`DBSQL` handles with method tables, `dbsql_*`
  functions, `DBSQL_*` constants). There is no `sqlite3_*` API.

## Status

**Under reconstruction. Not yet usable.** The previous code (the SQLite 2.8
derivative, and a 2026-03 hybrid that does not build) is preserved at the
git tags `archive/sqlite28-2024` and `archive/hybrid-2026-03`. Progress is
tracked in `.agent/STEERING.md`.

## Building

```sh
git submodule update --init deps/libdb
cd build_unix
../dist/s_config            # regenerate dist/configure (needs autoconf)
../dist/configure
gmake -j
gmake check
```

## License

dbsql is distributed under the ISC license; see `LICENSE`.  SQLite is in the public domain.
libdb is distributed under the Sleepycat license. Review the libdb licence
terms before redistributing.
