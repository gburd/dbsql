# DBSQL Baseline Report

**Date**: March 5, 2026
**Git Tag**: `baseline-pre-retranspile` (commit `f0d3534` "K&R no more")
**Purpose**: Establish baseline state before SQLite 3.52.0 re-transpilation

---

## 1. Current DBSQL Version

- **Version**: DBSQL 0.4.0
- **Release Date**: January 14, 2026
- **Confirmed by**: `dbsql --version` output and `README`
- **Branch**: `claude`
- **Build**: Compiles and links against Berkeley DB 4.8 (`libdb-4.8.so`)

---

## 2. Old SQLite Version Identification

### Based on SQLite 2.8.x (the final 2.x release, circa 2003-2005)

DBSQL was transpiled from the **SQLite 2.8.x** codebase. This is determined through
multiple independent architectural indicators:

#### 2.1 Stack-Based VDBE Architecture

DBSQL uses a **stack machine** for its Virtual Database Engine. The VDBE source
(`src/vdbe.c`) explicitly states:

> "Computation results are stored on a stack. Each entry on the stack is either
> an integer, a null-terminated string, a floating point number, or the SQL
> 'NULL' value."

SQLite switched from a stack-based to a **register-based** VDBE in version 3.0.0
(June 2004). The register-based architecture is fundamentally different.

Stack manipulation opcodes present in DBSQL:
- `OP_Pop` (8), `OP_Dup` (9), `OP_Pull` (10), `OP_Push` (11)

These opcodes do not exist in any SQLite 3.x release.

#### 2.2 Three-Operand Instructions Only

DBSQL instructions have only **P1, P2, P3** operands:

> "Each operation has an opcode and 3 operands. Operands P1 and P2 are integers.
> Operand P3 is a null-terminated string."

SQLite 3.x added **P4** (a more flexible union type replacing the string-only P3)
and **P5** (an additional flags byte). DBSQL has neither.

#### 2.3 Legacy Opcodes

The following opcodes exist in DBSQL but were removed or replaced in SQLite 3.0:

| DBSQL Opcode | SQLite 3.x Replacement |
|--------------|----------------------|
| `OP_Callback` (13) | `OP_ResultRow` |
| `OP_SortCallback` (112) | Removed |
| `OP_SortPut/SortMakeRec/SortMakeKey` | `OP_SorterInsert`, `OP_SorterSort` |
| `OP_ListWrite/ListRead/ListRewind/ListReset` | Removed (subsumed by cursors) |
| `OP_ListPush/ListPop` | Removed |
| `OP_FileOpen/FileRead/FileColumn` | Removed (COPY FROM removed in 3.0) |
| `OP_AggReset/AggInit/AggFunc/AggFocus` | `OP_AggStep`, `OP_AggFinal` |
| `OP_AggSet/AggGet` | Replaced by register operations |
| `OP_SetInsert/SetFound/SetNotFound` | Replaced by `OP_RowSetAdd/RowSetRead` |
| `OP_PutIntKey/PutStrKey` | `OP_Insert` |
| `OP_NewRecno` | `OP_NewRowid` |
| `OP_MoveTo/MoveLt` | `OP_SeekGE/SeekGT/SeekLE/SeekLT` |
| `OP_KeyAsData` | Removed |
| `OP_ColumnName` | Removed |
| `OP_StrEq/StrNe/StrLt/StrLe/StrGt/StrGe` | Merged into `OP_Eq/Ne/Lt/Le/Gt/Ge` |
| `OP_MakeKey/MakeIdxKey` | `OP_MakeRecord` (unified) |
| `OP_MakeRecord` (52) | `OP_MakeRecord` (different semantics) |

**Total DBSQL opcodes**: 132 (SQLite 3.52.0 has 190+)

#### 2.4 No Type Affinity System

SQLite 2.x stored all data as strings (typeless). SQLite 3.0 introduced
**manifest typing** with type affinities (INTEGER, REAL, TEXT, BLOB, NULL).
DBSQL has no type affinity logic.

#### 2.5 Copyright and Date Evidence

- DBSQL source headers: Copyright 2007-2008, The DBSQL Group, Inc.
- The code acknowledges starting from SQLite public domain code
- SQLite 2.8.17 was the last 2.x release (December 2005)

### Architectural Gap Summary

| Feature | DBSQL (SQLite 2.8.x) | SQLite 3.52.0 |
|---------|----------------------|---------------|
| VDBE architecture | Stack-based | Register-based |
| Instruction operands | P1, P2, P3 | P1, P2, P3, P4, P5 |
| Opcode count | 132 | 190+ |
| Type system | Typeless (string storage) | Manifest typing with affinity |
| Record format | Custom string-based | SQLite3 record format |
| Storage engine | Berkeley DB 4.8 | B-tree (SQLite native) |
| Window functions | No | Yes |
| CTEs | No | Yes |
| JSON support | No | Yes |
| UPSERT | No | Yes |
| Generated columns | No | Yes |
| WAL mode | No | Yes |
| FTS5 | No | Yes |
| Partial indexes | No | Yes |

---

## 3. DBSQL Source Structure

### Core Source Files

| File | Lines | SQLite 3.52.0 Equivalent |
|------|-------|--------------------------|
| `vdbe.c` | 4,987 | `vdbe.c` (9,318) |
| `vdbe_method.c` | - | `vdbeaux.c` + `vdbeapi.c` + `vdbemem.c` |
| `cg_select.c` | 2,911 | `select.c` (8,964) |
| `cg_expr.c` | 1,934 | `expr.c` (7,702) |
| `cg_where.c` | 1,435 | `where.c` + `wherecode.c` + `whereexpr.c` |
| `cg_build.c` | 2,715 | `build.c` (5,820) |
| `sql_parser.y` | 903 | `parse.y` (2,160) |
| `sql_tokenize.c` | - | `tokenize.c` |
| `sql_fns.c` | - | `func.c` |
| `cg_insert.c` | - | `insert.c` |
| `cg_delete.c` | - | `delete.c` |
| `cg_update.c` | - | `update.c` |
| `cg_trigger.c` | - | `trigger.c` |
| `cg_attach.c` | - | `attach.c` |
| `cg_auth.c` | - | `auth.c` |
| `cg_copy.c` | - | *(removed in 3.x)* |
| `cg_date.c` | - | `date.c` |
| `cg_pragma.c` | - | `pragma.c` |
| `cg_vacuum.c` | - | `vacuum.c` |
| `sm.c` | - | *(DBSQL-specific: Berkeley DB storage manager)* |
| `safety.c` | - | *(DBSQL-specific)* |
| `api.c` | - | `main.c` + `legacy.c` |
| `api_table.c` | - | `table.c` |

**Total DBSQL core source**: 34 `.c` files, ~34,494 lines
**Total DBSQL headers**: 15 files in `src/inc/`, ~2,894 lines

### Header Files

| File | Lines | Purpose |
|------|-------|---------|
| `dbsql_int.in` | 1,023 | Main internal header (template, generates `dbsql_int.h`) |
| `queue.h` | 564 | Queue/list macros (BSD-style) |
| `vdbe_int.h` | 303 | VDBE internals |
| `dbsql_ext.h` | 201 | External API declarations |
| `hash.h` | 189 | Hash table |
| `debug.h` | 130 | Debug macros |
| `str.h` | 111 | String utilities |
| `vdbe.h` | 88 | VDBE public interface |
| `db_int.h` | 54 | Database internal types |
| `xvprintf.h` | 54 | Printf variant |
| Others | ~127 | Various utility headers |

### DBSQL Naming Convention

- Functions: `__function_name()` (double-underscore prefix for internal)
- Types: `type_name_t` (underscore-separated with `_t` suffix)
- Macros: `DBSQL_*` prefix
- API: function pointers on `dbsql_t` and `dbsql_env_t` objects

---

## 4. Test Suite Results

### 4.1 Shell Script Tests (Code Quality Checks)

| Test | Script | Result | Notes |
|------|--------|--------|-------|
| scr003 | `chk.define` | FAIL | Picks up `_/sqlite/` files with indented #defines (false positive from new SQLite source) |
| scr005 | `chk.nl` | PASS | Newline check passes (compiler warning about missing ctype.h) |
| scr006 | `chk.offt` | PASS | Off_t usage check |
| scr007 | `chk.proto` | PASS | Prototype check |
| scr010 | `chk.str` | SKIP | Requires `spell` command not available |
| scr011 | `chk.tags` | PASS | Tags file check (minor informational diff: `_` dir not in tags) |
| scr014 | `chk.err` | PASS | Error message format check (informational diffs for `DBSQL_NOTFOUND`, `DBSQL_EMPTY`, `db_strerror`) |
| scr018 | `chk.comma` | FAIL | Test program compilation error (`isblank` declaration conflict) |
| scr019 | `chk.include` | FAIL | 7 extraneous system includes found in source |
| scr020 | `chk.inc` | FAIL | 4 files with includes before `dbsql_config.h` |
| scr026 | `chk.method` | INFO | Lists internal method calls (informational, not pass/fail) |
| scr030 | `chk.build` | FAIL | Build check fails in configure with `--disable-cryptography` |

**Summary**: 4 PASS, 4 FAIL, 1 SKIP, 1 INFO (out of 10 active tests)

### 4.2 Runtime Tests

| Test | Result | Notes |
|------|--------|-------|
| `dbsql --version` | PASS | Returns "DBSQL 0.4.0: (January 14, 2026)" |
| `dbsql --help` | PASS | Help text displays correctly |
| Smoke test (SQL) | FAIL | Binary segfaults on database open |
| `dbsql <db> ".tables"` | FAIL | Segfault (exit code 139) |
| `dbsql <db> "select 1"` | FAIL | Segfault |

The binary links correctly against `libdb-4.8.so` (Berkeley DB) but crashes
with a segmentation fault when attempting any database operation. The `--version`
and `--help` flags work because they exit before opening a database.

### 4.3 SQLite-Derived Functional Tests

68 `.test` files exist in `test/scr050/tests/` covering:
- Core SQL: `select1-6.test`, `insert.test`, `insert2.test`, `update.test`, `delete.test`
- Expressions: `expr.test`, `in.test`, `null.test`
- Schema: `table.test`, `index.test`, `view.test`, `trigger1-4.test`
- Integrity: `conflict.test`, `unique.test`, `notnull.test`, `fkey1.test`
- Functions: `func.test`, `date.test`, `printf.test`
- Joins: `join.test`, `join2.test`
- Transactions: `trans.test`, `lock.test`
- Other: `pragma.test`, `vacuum.test`, `sort.test`, `limit.test`, `subselect.test`

**Status**: NOT RUN -- requires `dbsql_tclsh` (TCL-enabled DBSQL shell) which is
not built in the current configuration, and the base binary segfaults on DB open.

---

## 5. SQLite 3.52.0 Source Verification

### Location: `_/sqlite/`

### Version Confirmation

- **VERSION file**: `3.52.0`
- **Manifest UUID**: `f41b9ad98c474755c6701eb6ab3dc9864ace9b52a81581f73ca0912cedbc3c37`

### Key Source Files Verified Present

| File | Present | Lines |
|------|---------|-------|
| `src/vdbe.c` | Yes | 9,318 |
| `src/vdbeaux.c` | Yes | - |
| `src/vdbeapi.c` | Yes | - |
| `src/vdbemem.c` | Yes | - |
| `src/vdbesort.c` | Yes | - |
| `src/select.c` | Yes | 8,964 |
| `src/expr.c` | Yes | 7,702 |
| `src/where.c` | Yes | 7,883 |
| `src/wherecode.c` | Yes | - |
| `src/whereexpr.c` | Yes | - |
| `src/build.c` | Yes | 5,820 |
| `src/parse.y` | Yes | 2,160 |
| `src/tokenize.c` | Yes | - |
| `src/func.c` | Yes | - |
| `src/insert.c` | Yes | - |
| `src/delete.c` | Yes | - |
| `src/update.c` | Yes | - |
| `src/trigger.c` | Yes | - |
| `src/attach.c` | Yes | - |
| `src/window.c` | Yes | - |
| `src/json.c` | Yes | - |
| `src/upsert.c` | Yes | - |
| `src/sqliteInt.h` | Yes | - |
| `src/vdbeInt.h` | Yes | - |

### Source Completeness

- **C source files**: 120+ in `src/`
- **Header files**: 24 in `src/`
- **Grammar**: `src/parse.y`
- **Total core source lines**: ~165,101 (non-test `.c` files)
- **Test suite**: 1,276 files in `test/`
- **Extensions**: `ext/` directory (ICU, recover, rtree, session, FTS, etc.)
- **Build system**: `configure`, `Makefile.in`, `main.mk`, `auto.def`, `autosetup/`

The SQLite 3.52.0 source tree is **complete and intact**.

---

## 6. Git Tag Confirmation

```
Tag:    baseline-pre-retranspile
Commit: f0d3534 "K&R no more"
Branch: claude
```

The tag marks the exact state of the DBSQL codebase before any re-transpilation
work begins. All subsequent changes can be diffed against this tag.

---

## 7. Key Observations and Risks

### Critical

1. **Massive architectural gap**: SQLite 2.8.x to 3.52.0 spans ~20 years of
   development. The stack-to-register VDBE transition fundamentally changes
   every code generator and the VDBE interpreter.

2. **Runtime instability**: The current binary segfaults on database open,
   meaning the existing DBSQL code has pre-existing runtime issues.

3. **Storage manager adaptation**: The Berkeley DB storage manager (`sm.c`)
   must be adapted to the new VDBE cursor interface without breaking BDB
   integration.

### Important

4. **Code size growth**: SQLite 3.52.0 core source (~165K lines) is roughly
   5x larger than current DBSQL (~34K lines), reflecting 20 years of features.

5. **New subsystems**: Several SQLite 3.x subsystems have no DBSQL equivalent
   and will need to be transpiled from scratch: window functions, CTEs,
   UPSERT, JSON, WAL, FTS5, etc.

6. **Test coverage gap**: The 68 SQLite-derived test files test only basic
   SQL92 functionality. Modern SQL features will need new tests.
