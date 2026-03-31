# DBSQL Implemented Features

This document describes the SQL features implemented in DBSQL, unimplemented features, VDBE opcode coverage, limitations, and development roadmap.

**Version**: 0.4
**Status**: Active development
**Last Updated**: 2026-03-31

---

## 1. Implemented SQL Features

### 1.1 Data Definition Language (DDL)

**CREATE TABLE**
- Syntax: `CREATE [TEMP] TABLE table_name (column_definitions)`
- Column types: INTEGER, TEXT, BLOB, REAL
- Column constraints: PRIMARY KEY, NOT NULL, UNIQUE, DEFAULT
- Table constraints: PRIMARY KEY, UNIQUE
- Status: Fully implemented

**DROP TABLE**
- Syntax: `DROP TABLE table_name`
- Status: Fully implemented

**CREATE INDEX**
- Syntax: `CREATE [UNIQUE] INDEX index_name ON table_name (column_list)`
- Supports single and multi-column indices
- Supports UNIQUE constraint
- Status: Fully implemented

**DROP INDEX**
- Syntax: `DROP INDEX index_name`
- Status: Fully implemented

**CREATE VIEW**
- Syntax: `CREATE [TEMP] VIEW view_name AS select_statement`
- Status: Parser implemented, execution support partial

**DROP VIEW**
- Syntax: `DROP VIEW view_name`
- Status: Implemented

### 1.2 Data Manipulation Language (DML)

**SELECT**
- Basic form: `SELECT column_list FROM table_list`
- WHERE clause with arbitrary expressions
- JOIN operations: INNER JOIN, LEFT OUTER JOIN, CROSS JOIN
- ORDER BY with ASC/DESC
- GROUP BY with HAVING clause
- DISTINCT keyword
- LIMIT and OFFSET
- Subqueries in FROM clause
- UNION, UNION ALL, INTERSECT, EXCEPT set operations
- CASE expressions
- Status: Fully implemented and tested

**INSERT**
- Syntax: `INSERT INTO table_name [(column_list)] VALUES (value_list)`
- Syntax: `INSERT INTO table_name [(column_list)] select_statement`
- REPLACE variant: `REPLACE INTO table_name ...`
- Conflict resolution: ON CONFLICT ROLLBACK/ABORT/FAIL/IGNORE/REPLACE
- Status: Fully implemented and tested

**DELETE**
- Syntax: `DELETE FROM table_name [WHERE condition]`
- Supports arbitrary WHERE expressions
- Status: Fully implemented and tested

**UPDATE**
- Syntax: `UPDATE table_name SET assignments [WHERE condition]`
- Status: STUBBED (parser implemented, code generation not implemented)
- Priority: HIGH - core SQL feature

### 1.3 Query Features

**WHERE Clause**
- All comparison operators: =, !=, <>, <, <=, >, >=
- Logical operators: AND, OR, NOT
- BETWEEN, IN, LIKE, GLOB patterns
- IS NULL, IS NOT NULL
- Subquery expressions
- Status: Fully implemented

**ORDER BY**
- Multiple columns
- ASC/DESC per column
- NULL ordering (NULLS FIRST/LAST not implemented)
- Status: Fully implemented using sorter

**GROUP BY**
- Multiple grouping columns
- HAVING clause for group filtering
- Status: Fully implemented

**Aggregate Functions**
- COUNT(*), COUNT(column)
- SUM(column), AVG(column), TOTAL(column)
- MIN(column), MAX(column)
- STDDEV(column) - standard deviation
- GROUP_CONCAT(column [, separator])
- Status: Fully implemented

**Built-in Functions** (from sql_fns.c)

Scalar Functions:
- **String**: length(), substr()/substring(), upper(), lower(), trim(), ltrim(), rtrim()
- **String (new)**: typeof(), instr(), replace(), hex(), unhex(), concat(), concat_ws(), unicode(), char()
- **Numeric**: abs(), round(), sign()
- **Comparison**: min(), max(), coalesce(), ifnull(), nullif()
- **Pattern**: like(), glob()
- **Utility**: quote(), random(), last_inserted_rowid(), dbsql_get_version()
- **Conditional**: CASE expressions

Math Functions (if DBSQL_OMIT_MATH_FUNCTIONS not defined):
- **Basic**: ceil()/ceiling(), floor(), trunc(), sqrt()
- **Logarithmic**: ln(), log(), log10(), log2(), exp()
- **Power**: pow()/power(), mod()
- **Trigonometric**: sin(), cos(), tan(), asin(), acos(), atan(), atan2()
- **Hyperbolic**: sinh(), cosh(), tanh(), asinh(), acosh(), atanh()
- **Conversion**: radians(), degrees()
- **Constants**: pi()

Date/Time Functions (from cg_date.c, if DBSQL_OMIT_DATETIME_FUNCS not defined):
- date(), time(), datetime(), julianday(), strftime()
- Date arithmetic and formatting
- Timezone handling

### 1.4 Transactions

**BEGIN TRANSACTION**
- Syntax: `BEGIN [TRANSACTION]`
- Starts a transaction with specified isolation level
- Status: Fully implemented

**COMMIT**
- Syntax: `COMMIT [TRANSACTION]` or `END [TRANSACTION]`
- Commits current transaction
- Status: Fully implemented

**ROLLBACK**
- Syntax: `ROLLBACK [TRANSACTION]`
- Aborts current transaction
- Status: Fully implemented

### 1.5 Other Commands

**COPY**
- Syntax: `COPY table_name FROM file_name [USING DELIMITERS string]`
- Bulk data import from file
- Status: Implemented

**VACUUM**
- Syntax: `VACUUM [table_name]`
- Reclaims space from deleted records
- Status: Implemented

**PRAGMA**
- Various database control statements
- Examples: `PRAGMA cache_size = 2000;`
- Status: Implemented

**EXPLAIN**
- Syntax: `EXPLAIN query_statement`
- Shows VDBE bytecode for query
- Status: Implemented

---

## 2. Unimplemented Features

### 2.1 Core SQL (HIGH PRIORITY)

**UPDATE Statement**
- File: stubs.c, function: `__update()`
- Parser: Fully implemented in sql_parser.y
- Code generation: NOT IMPLEMENTED
- Impact: Cannot modify existing data
- Workaround: DELETE + INSERT
- Priority: CRITICAL - needed for core SQL compliance

### 2.2 Advanced Features (MEDIUM PRIORITY)

**TRIGGER Support**
- CREATE TRIGGER, DROP TRIGGER
- BEFORE/AFTER triggers
- INSERT/UPDATE/DELETE events
- Row-level triggers
- Files: 10 stub functions in stubs.c
- Status: Parser implemented, execution NOT IMPLEMENTED
- Impact: No automated data maintenance or validation
- Priority: MEDIUM - advanced database feature

**Advanced Constraint Checking**
- Function: `__generate_constraint_checks()`
- CHECK constraints (limited support)
- Foreign key cascade operations
- Advanced UNIQUE violation handling
- Status: Basic constraints work, advanced features NOT IMPLEMENTED
- Priority: MEDIUM - data integrity

### 2.3 Database Management (LOW PRIORITY)

**ATTACH/DETACH Database**
- ATTACH DATABASE file AS name
- DETACH DATABASE name
- Multi-database queries
- Files: 8 stub functions in stubs.c
- Status: NOT IMPLEMENTED
- Impact: Cannot work with multiple database files simultaneously
- Workaround: Use separate connections
- Priority: LOW - convenience feature

**ALTER TABLE**
- ADD COLUMN, DROP COLUMN
- RENAME TABLE, RENAME COLUMN
- Status: NOT IMPLEMENTED
- Impact: Schema changes require table rebuild
- Workaround: CREATE new table, COPY data, DROP old table

**Full-Text Search (FTS)**
- FTS3/FTS4/FTS5 virtual tables
- Status: NOT IMPLEMENTED
- Impact: Text search requires LIKE queries (slow)
- Priority: LOW - advanced feature

---

## 3. VDBE Opcode Coverage

The VDBE (Virtual Database Engine) interprets bytecode opcodes. DBSQL defines 152 opcodes.

### 3.1 Core Opcodes (IMPLEMENTED)

**Program Control**
- OP_Goto, OP_Gosub, OP_Return, OP_Halt
- OP_If, OP_IfNot, OP_Noop

**Data Stack**
- OP_Integer, OP_String, OP_Null, OP_Variable
- OP_Pop, OP_Dup, OP_Pull, OP_Push, OP_Copy

**Results**
- OP_ResultRow, OP_NullCallback, OP_ColumnName, OP_Callback

**Transactions**
- OP_Transaction, OP_Commit, OP_Rollback
- OP_Checkpoint

**Cursors**
- OP_OpenRead, OP_OpenWrite, OP_OpenTemp, OP_OpenPseudo, OP_Close
- OP_Rewind, OP_Next, OP_Prev, OP_Last
- OP_SeekLT, OP_SeekLE, OP_SeekGE, OP_SeekGT, OP_SeekRowid
- OP_MoveLt, OP_MoveTo

**Data Access**
- OP_Column, OP_Rowid, OP_RowData, OP_RowKey, OP_FullKey
- OP_KeyAsData, OP_NullRow

**Data Modification**
- OP_MakeRecord, OP_Insert, OP_Delete
- OP_NewRecno, OP_Clear, OP_Destroy
- OP_CreateTable, OP_CreateIndex

**Arithmetic**
- OP_Add, OP_Subtract, OP_Multiply, OP_Divide, OP_Remainder
- OP_Negative, OP_AbsValue
- OP_AddImm, OP_ForceInt, OP_MustBeInt

**Bitwise**
- OP_BitAnd, OP_BitOr, OP_BitNot
- OP_ShiftLeft, OP_ShiftRight

**Comparison**
- OP_Eq, OP_Ne, OP_Lt, OP_Le, OP_Gt, OP_Ge
- OP_StrEq, OP_StrNe, OP_StrLt, OP_StrLe, OP_StrGt, OP_StrGe

**Logical**
- OP_And, OP_Or, OP_Not
- OP_IsNull, OP_NotNull

**Aggregates**
- OP_AggStep, OP_AggFinal, OP_AggValue

**Sorting**
- OP_SorterOpen, OP_SorterInsert, OP_SorterSort, OP_SorterData, OP_SorterNext

**String Operations**
- OP_Concat

**Search**
- OP_NotFound, OP_Found, OP_Distinct
- OP_NotExists, OP_IsUnique

**Memory**
- OP_MemLoad, OP_MemIncr

**Other**
- OP_Vacuum, OP_Sequence, OP_Function, OP_InitCoroutine

### 3.2 Index Opcodes (PARTIALLY IMPLEMENTED)

The following index opcodes have case statements in vdbe.c but may have limited or stubbed implementations:

- OP_IdxPut, OP_IdxDelete, OP_IdxRecno
- OP_IdxLT, OP_IdxGT, OP_IdxGE, OP_IdxIsNull
- OP_IdxInsert (implemented, wraps OP_Insert)
- OP_MakeKey, OP_MakeIdxKey, OP_IncrKey

Status: Basic index operations work for PRIMARY KEY and UNIQUE indices. Complex index operations may have issues.

### 3.3 Legacy/Unimplemented Opcodes

The following opcodes have stub implementations that return errors:

**List Operations** (legacy SQLite 2.x feature)
- OP_ListWrite, OP_ListRewind, OP_ListRead, OP_ListReset
- OP_ListPush, OP_ListPop
- Status: Intentionally unimplemented (replaced by sorter)

**Sort Operations** (legacy)
- OP_SortPut, OP_SortMakeRec, OP_SortMakeKey, OP_Sort, OP_SortNext, OP_SortCallback, OP_SortReset
- Status: Replaced by OP_Sorter* opcodes

**File Operations**
- OP_FileOpen, OP_FileRead, OP_FileColumn
- Status: Unimplemented (unclear if needed)

**Set Operations**
- OP_SetInsert, OP_SetFound, OP_SetNotFound, OP_SetFirst, OP_SetNext
- Status: Unimplemented

**Memory Store**
- OP_MemStore
- Status: Stub (OP_MemLoad is implemented)

**Old Aggregate Opcodes**
- OP_AggReset, OP_AggInit, OP_AggFunc, OP_AggFocus, OP_AggSet, OP_AggGet, OP_AggNext
- Status: Replaced by OP_AggStep/OP_AggFinal

**Update Opcodes**
- OP_PutIntKey, OP_PutStrKey
- Status: May be legacy (OP_Insert is used instead)

### 3.4 Opcode Implementation Verification

The VDBE default case handler (vdbe.c lines 5237-5242) catches any unimplemented opcode and returns:
```
ERROR: unknown opcode <N>
```

All generated bytecode has been tested with:
- CREATE TABLE, DROP TABLE
- INSERT, SELECT, DELETE
- WHERE, ORDER BY, GROUP BY
- Aggregates, JOINs
- Transactions

---

## 4. Known Limitations

### 4.1 Performance

**Index Usage**
- Indices are created but may not be optimally used by query planner
- Complex queries may perform full table scans
- No query cost estimation

**Sorter Memory**
- Large ORDER BY operations use in-memory sorter
- May cause high memory usage on large result sets
- No disk-based external sort

**Aggregate Performance**
- Aggregates process all rows in memory
- No incremental aggregate computation

### 4.2 Concurrency

**Transaction Isolation**
- Uses Berkeley DB's transaction isolation
- Default: READ COMMITTED
- Concurrent writes may block

**Lock Granularity**
- Table-level locking (via Berkeley DB)
- No row-level locking

### 4.3 Storage

**Data Types**
- Dynamic typing like SQLite (TEXT, INTEGER, REAL, BLOB)
- No strict type enforcement
- Type affinity rules apply

**NULL Handling**
- NULL propagates through expressions
- NULL != NULL (standard SQL behavior)
- ORDER BY: NULLs sort first

**String Encoding**
- UTF-8 encoding if DBSQL_UTF8_ENCODING defined
- ASCII otherwise
- Limited Unicode normalization

### 4.4 SQL Compatibility

**Not Implemented from Standard SQL**
- Window functions (OVER clause)
- Common Table Expressions (WITH clause)
- MERGE statement
- Recursive queries
- UNION column naming differs from PostgreSQL

**SQLite Compatibility**
- Based on SQLite 2.x architecture
- Many SQLite 3.x features added (functions, opcodes)
- Not 100% compatible with modern SQLite

### 4.5 Error Handling

**Error Messages**
- Basic error reporting
- Limited context in some error messages
- No error codes for all error types

**Recovery**
- Transaction rollback on error
- No partial transaction recovery
- Database may need repair after crashes

---

## 5. Roadmap

### 5.1 High Priority

**Phase 1: Core SQL Completion**
- [ ] Implement UPDATE statement (cg_update.c)
- [ ] Full index operation verification
- [ ] Comprehensive error handling
- [ ] Memory leak fixes
- [ ] Test suite at >95% coverage

**Timeline**: 1-2 weeks
**Impact**: CRITICAL - Core SQL compliance

### 5.2 Medium Priority

**Phase 2: Advanced Features**
- [ ] Implement TRIGGER support
- [ ] Advanced constraint checking (CHECK, FK cascades)
- [ ] Window functions
- [ ] Common Table Expressions (WITH)
- [ ] ALTER TABLE support
- [ ] Query optimization improvements

**Timeline**: 1-2 months
**Impact**: HIGH - Advanced SQL features

### 5.3 Low Priority

**Phase 3: Convenience & Performance**
- [ ] ATTACH/DETACH database
- [ ] Full-Text Search (FTS3/FTS4)
- [ ] External sort for large ORDER BY
- [ ] Query cost estimation
- [ ] Statistics collection (ANALYZE)
- [ ] JSON functions
- [ ] Additional math functions

**Timeline**: 3-6 months
**Impact**: MEDIUM - Performance and convenience

### 5.4 Future Enhancements

**Long-term Goals**
- [ ] Incremental backup/restore
- [ ] Replication support (via Berkeley DB HA)
- [ ] Stored procedures
- [ ] Compiled query plans
- [ ] Better Unicode support
- [ ] EXPLAIN QUERY PLAN improvements

**Timeline**: 6-12+ months
**Impact**: LOW - Nice-to-have features

---

## 6. Testing Status

### 6.1 Verified Working Features

The following have been tested and verified working:

**Basic Operations**
- CREATE TABLE with various column types and constraints
- INSERT with explicit and implicit columns
- SELECT with WHERE, ORDER BY, GROUP BY
- DELETE with and without WHERE clause
- CREATE INDEX, DROP INDEX
- Transactions: BEGIN, COMMIT, ROLLBACK

**Complex Queries**
- JOINs (INNER, LEFT OUTER)
- Subqueries in SELECT and WHERE
- UNION, UNION ALL
- Aggregate functions with GROUP BY
- CASE expressions
- String and math functions

**Test Coverage**
- Core API functions: >90% coverage
- VDBE interpreter: ~85% coverage
- Code generators: ~80% coverage
- Overall: ~85% code coverage

See test/src/ for test suite details.

### 6.2 Known Issues

**UPDATE Not Working**
- Stubbed out, returns no error but does nothing
- Requires code generation implementation

**Some Index Operations**
- Complex multi-column index lookups may not optimize correctly
- Index usage in JOINs needs verification

**Error Messages**
- Some error conditions produce generic "DBSQL_INTERNAL" errors
- Need more specific error codes and messages

---

## 7. Contributing

### 7.1 Before Adding Features

1. Check this document to see if feature is already stubbed
2. Check CODE_AUDIT_REPORT.md for cleanup priorities
3. Review existing code style in similar files
4. Add test cases before implementing

### 7.2 Priority Areas

**Immediate Needs**
1. UPDATE statement implementation
2. Memory leak fixes
3. Error message improvements
4. Test coverage >95%

**Next Steps**
1. TRIGGER support
2. Advanced constraints
3. Query optimization

### 7.3 Code Style

- Follow existing naming conventions (no Hungarian notation)
- Functions: `__function_name()` for internal, `dbsql_function()` for API
- Maximum 100 characters per line
- Use tabs for indentation
- Add comprehensive comments

---

## 8. Version History

**0.4** (2026-03-31)
- Fixed OP_Column crash
- Cleaned up debug fprintf statements
- Documented all features and limitations
- Test coverage at ~85%

**0.3** (Previous)
- Implemented ORDER BY with sorter
- Fixed aggregate functions
- Added many SQLite 3.52 functions
- Fixed memory allocation bugs

**0.2** (Previous)
- Ported to register-based VDBE
- Implemented basic CRUD operations
- Added transaction support

**0.1** (Initial)
- Fork from SQLite 2.x
- Berkeley DB storage backend

---

## 9. References

**Documentation**
- README.md - Building and installing
- CODE_AUDIT_REPORT.md - Code quality audit
- Test suite in test/src/

**Source Files**
- src/sql_parser.y - SQL grammar
- src/vdbe.c - Virtual machine interpreter
- src/cg_*.c - Code generators for SQL statements
- src/sql_fns.c - Built-in SQL functions
- src/stubs.c - Unimplemented feature stubs

**External Dependencies**
- Berkeley DB (libdb) - Storage engine
- Standard C library

---

**Document maintained by**: DBSQL Development Team
**Questions/Issues**: See project repository
