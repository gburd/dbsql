# Transform rules: SQLite 3.53.4 → dbsql

The SQLite-derived part of dbsql is **generated** by `dist/transpile`, a
deterministic transformer that runs over a pinned SQLite tag. Hand edits to
generated files go in the patch queue (`dist/transpile/patches/`) or into
hand-owned files, never into the output directly.

## Pipeline

```
fossil checkout version-3.53.4
  → select   (dist/transpile/manifest: which upstream files, under which dbsql names)
  → rename   (token-level, libclang-verified symbol inventory + names.map)
  → restyle  (comment blocks → `__func --` + PUBLIC:, BSD brace style)
  → patch    (dist/transpile/patches/*.patch, applied in order)
  → src/     (committed; the vendor branch `upstream/sqlite` holds the
              pure transform output, one commit per SQLite tag)
```

`dist/transpile/run --tag version-3.53.4 --out src/` must produce a
byte-identical tree on every run. CI checks this.

## Naming

These rules follow the 2007 dbsql conventions and libdb's own.

| SQLite | dbsql | Rule |
|---|---|---|
| `sqlite3` (connection) | `DBSQL` | Public handle, uppercase like `DB_ENV` |
| `sqlite3_stmt` | `DBSQL_STMT` | Public handle |
| `sqlite3_value` | `DBSQL_VALUE` | Public handle |
| `sqlite3_context` | `DBSQL_FUNC` | Public handle |
| `sqlite3_str` | `DBSQL_STR` | Public handle |
| `sqlite3_vtab`, `_vtab_cursor`, `_module`, `_index_info` | `DBSQL_VTAB`, `DBSQL_VTAB_CURSOR`, `DBSQL_MODULE`, `DBSQL_INDEX_INFO` | Public handles |
| `sqlite3_foo(db, ...)` method-like API | `dbp->foo(dbp, ...)` | Method table on the handle |
| `sqlite3_foo(...)` free function | `dbsql_foo(...)` | Global API |
| `SQLITE_FOO` constants | `DBSQL_FOO` | Error codes are negative, in libdb style (§Errors) |
| `sqlite3FooBar()` internal | `__foo_bar()` | snake_case, with a module prefix where SQLite's name lacks one |
| `static fooBar()` | `__foo_bar()` (static) | Same rule |
| `Table`, `Expr`, `Select`, `Parse`, `Vdbe`, … | `table_t`, `expr_t`, `select_t`, `parser_t`, `vdbe_t`, … | `struct name` + `name_t` typedef |
| `Mem` | `mem_t` | |
| `VdbeCursor` | `vdbe_cursor_t` | |
| `u8 u16 u32 i64 u64` | `u_int8_t u_int16_t u_int32_t int64_t u_int64_t` | libdb types |
| struct fields: `zName nCol aCol pTab iDb` | `name ncols cols table db_idx` | **Readable names** (§Fields) |
| locals: `pParse pTab zSql rc` | `parser table sql ret` | **Readable names** (§Locals) |
| `TK_*` `OP_*` `EP_*` `TF_*` `SF_*` `WHERE_*` `MEM_*` | unchanged | Internal enum/flag macros stay as-is: they are the language of the VDBE and parser, and keeping them makes upstream diffs readable |

### Fields and locals

The upstream Hungarian prefixes are rewritten to plain readable names:

- `z` (string) is dropped: `zName → name`, `zErrMsg → errmsg`.
- `n` (count) becomes a plural noun or `_count` suffix: `nCol → ncols`,
  `nExpr → nexprs`, `nMem → nmem`.
- `a` (array) becomes the plural: `aCol → cols`, `aDb → dbs`, `aOp → ops`.
- `p` (pointer) is dropped: `pTab → table`, `pParse → parser`,
  `pLeft → left`.
- `i` (index) becomes `_idx` or a descriptive noun: `iDb → db_idx`,
  `iCur → cursor`, `iTable → table_cursor`.
- `x` (callback) becomes the `_fn` or verb form: `xFunc → func`,
  `xStep → step`.
- `b` and `is` (boolean) become `is_` or `has_`.

Each rename lives in `dist/transpile/names.map`, with one scope per entry
(struct field or local, plus the enclosing function where needed). The tool
refuses to apply a mapping that collides with an existing identifier in the
same scope, and it fails if any upstream identifier is unmapped and still
looks Hungarian (the "unmapped Hungarian" gate).

The upstream comment text is kept, with identifiers inside comments renamed
using the same map.

## Errors

The public `DBSQL_*` result codes follow libdb's practice of claiming a
negative range that does not collide with errno or libdb's own codes:
`DBSQL_ERROR (-50999)` … as in 2007. `DBSQL_SUCCESS` is 0. The step results
`DBSQL_ROW` and `DBSQL_DONE` are also negative. Extended result codes are
`(base) - (n << 8)` within the reserved range, and `dbsql_strerror()` maps
every code.

## Files

| SQLite | dbsql |
|---|---|
| parse.y | src/sql_parser.y |
| tokenize.c, complete.c | src/sql_tokenize.c, src/sql_complete.c |
| prepare.c | src/sql_prepare.c |
| build.c | src/cg_build.c (`cg_` = code generator) |
| expr.c, select.c, insert.c, update.c, delete.c, upsert.c, trigger.c, alter.c, attach.c, auth.c, analyze.c, pragma.c, fkey.c, window.c, resolve.c, walker.c, where*.c, vacuum.c, callback.c | src/cg_*.c with the same stem |
| func.c, date.c, json.c | src/sql_fns.c, src/sql_date.c, src/sql_json.c |
| vdbe.c, vdbeaux.c, vdbeapi.c, vdbemem.c, vdbesort.c, vdbetrace.c, vdbevtab.c, vdbeblob.c, rowset.c | src/vdbe/vdbe.c, src/vdbe/vdbe_aux.c, … |
| main.c, legacy.c, table.c, loadext.c, global.c, status.c | src/api/*.c |
| printf.c, utf.c, util.c, hash.c, random.c, malloc.c, mutex*.c, threads.c | src/common/*.c |
| sqliteInt.h | src/inc/dbsql_int.h |
| sqlite.h.in | src/dbsql.in → dbsql.h |
| btree.c, pager.c, wal.c, pcache*.c, os_*.c, memdb.c, backup.c, dbpage.c, dbstat.c | **not shipped.** Replaced by the hand-written src/sm/ (storage manager, libdb) and src/os/ (time, sleep, randomness, temp files) |
| shell.c.in | src/shell/dbsql.c |
| tclsqlite.c, test*.c | src/tcl/, test/tcl/ (build-only testfixture) |
| ext/fts5, ext/icu | src/ext/fts5, src/ext/icu |
| tool/lemon.c, tool/lempar.c | src/lemon/ (upstream tool, renamed only at its output via `%name`/`%token_prefix`) |
