# Testing and release qualification

A feature is not done until a test that would fail without it passes on the
FreeBSD build host under ASan and UBSan. "It compiled" and "a status report
says so" are not evidence.

## Tiers (all run by `gmake check` in build_unix)

| Tier | Location | What it proves |
|---|---|---|
| unit | test/unit/*.c (munit) | sm/ against libdb directly: key encoding, comparator (including torn/garbage bytes), txn/child txn/savepoint mapping, meta, sequence naming, drop/clear |
| api | test/api/*.c | the public C API, method tables, error codes, `dbsql_strerror` for every code |
| sqlite-tcl | test/tcl/ + SQLite's `test/*.test` (fetched from the pinned fossil tag, not vendored) | SQL semantics. Driven by `test/tcl/allowlist.txt`; known failures are listed in `test/tcl/known-failures.txt` with a reason each, and that list must only shrink |
| diff | test/diff/ | the same SQL corpus run through `dbsql` and stock `sqlite3` 3.53.4; outputs must match except for listed divergences |
| bdb | test/bdb/*.sh | dbsql-specific: kill -9 mid-write then reopen and verify; multi-process writers; deadlock → BUSY; SSI write skew; hot backup and restore; an environment from an older libdb gives a clean error |
| shell | test/shell/ | the `dbsql` CLI: dot-commands, output modes, import/export |
| fuzz | test/fuzz/ | libFuzzer over prepare + step (seeded from SQLite's test SQL); comparator fuzzing with random bytes |

## Gates for a release

1. `gmake check` is green on FreeBSD 15.1 amd64 (the build host), on both a
   release (`-O2`) build and an `--enable-debug --enable-diagnostic`
   ASan+UBSan build.
2. Valgrind `--leak-check=full` reports zero definite leaks on the api and
   shell tiers.
3. A fuzz run of at least 30 minutes finds no crashes.
4. `dist/transpile/run` reproduces `src/` byte-for-byte.
5. No `sqlite3` identifier appears in installed headers or exported symbols
   (`nm -D` gate). SQLite attribution remains in comments and docs.
6. The working tree is clean, extra worktrees are removed, and docs match
   measured results.

## Where things run

All builds and tests run on the FreeBSD 15.1 EC2 host (`ssh dbsql-fbsd`,
`~/w/dbsql`). The laptop checkout is for review and for signing commits.
