# Testing and release qualification

A feature is not done until a test that would fail without it passes on
the build host under ASan and UBSan.  "It compiled" and "a status report
says so" are not evidence.

## Tiers (all run by `make check` in build_unix)

| Tier | Location | What it proves |
|---|---|---|
| unit | test/unit/*_test.c | sm/ against libdb directly: rowid key encoding, the comparator against upstream's record compare, and the comparator on random and truncated bytes (bounds, antisymmetry) |
| api | test/api/api_test.c | the public C API as an application sees it (only `dbsql.h`): open flags, prepare/bind/step/column, error codes and messages, `dbsql_errstr` for every code, changes/rowid, savepoints, two connections, reopen after commit, blob I/O, backup.  Every expectation is stock SQLite's |
| shell | test/shell/ | the `dbsql` CLI: dot-commands, output modes, import, ICU, FTS5 |
| bdb | test/bdb/ | what libdb changes: snapshot isolation and writer exclusion (checked against SQLite in WAL mode); SIGKILL during commits then recovery (no lost commit, no torn row, integrity ok); concurrent writer processes (no lost update) |
| sqlite-tcl | test/tcl/run.sh + SQLite 3.53.4's `test/*.test` (prepared from the pinned release by dist/transpile/tests, not vendored) | SQL semantics.  Every non-passing file must be in `test/tcl/known-failures.txt` with a reason; a listed file that passes is reported so the list only shrinks |
| fuzz | test/fuzz/ (run.sh) | libFuzzer over SQL on `:memory:` and on a file database, and over the comparator, against a coverage-instrumented ASan+UBSan library |

`make check` runs unit, api, shell, bdb and sqlite-tcl.  The fuzz tier
needs a clang build configured as in test/fuzz/run.sh's header.

## How a failure is classified

`dist/transpile/mk-known-failures RESULTS NATIVE CLASSIFIED` (CLASSIFIED
from `dist/transpile/classify.sh`) builds the
known-failures list.  NATIVE is the same suite on stock SQLite storage
(`dist/transpile/native-testfixture.sh`), the control: a file that fails
there too is `[native]` (platform or harness).  Otherwise a file is
tagged with a divergence from DIVERGENCES.md, a libdb defect
(`[libdb-Rn]`, .agent/libdb-bugs), or `[triage]`.  **A `[triage]` file
is a bug until shown otherwise; a release has none.**  A crash is never
a divergence: it is a dbsql bug or a reported libdb defect.

## Gates for a release

1. `make check` passes on the build host (Debian 13 amd64, clang 19), on
   the release build and on an `--enable-diagnostic` ASan+UBSan build.
2. Valgrind `--leak-check=full` reports no definite leaks and no errors
   on the api, shell and bdb tiers.
3. A coverage-guided fuzz run of at least 30 minutes finds no crash,
   sanitizer report or leak (other than the reported libdb leak R7,
   suppressed by test/fuzz/lsan.supp).
4. `dist/transpile/run --check` reproduces `src/` byte-for-byte, and
   `dist/transpile/check-names.sh src` reports 0.
5. No `sqlite3` identifier in installed headers or exported symbols
   (`nm -D`).  SQLite attribution remains in comments and docs.
6. The working tree is clean, extra worktrees are removed, and docs match
   measured results.

Results of each qualification run are kept in `.agent/qualification/`.

## Where things run

On the EC2 build host (Debian 13, `ssh dbsql-kiwi`, `~/w/dbsql`): the
hosts are short-lived burner-account instances, so everything that
matters is committed on the laptop, which also signs the commits.
FreeBSD 15.1 was the original host; the release is qualified on Debian
only (FreeBSD's disk filled during the rebuild and it was retired).
