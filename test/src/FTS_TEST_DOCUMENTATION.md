# DBSQL FTS (Full-Text Search) Test Suite Documentation

## Overview

This document describes the comprehensive FTS test suite created for DBSQL. The test suite is designed to verify full-text search functionality once FTS support is implemented in DBSQL.

**Current Status**: FTS is **not yet implemented** in DBSQL. All tests are currently skipped with `TEST_SKIP()` and will be activated once FTS support is added.

## Test File

- **Location**: `/home/gburd/ws/dbsql/test/src/test_fts.c`
- **Framework**: Uses existing DBSQL test framework with TAP output
- **Total Tests**: 21 comprehensive FTS tests
- **Build Target**: `test_fts`

## Test Categories

### 1. Basic FTS Operations (3 tests)

#### test_fts_create_table
- Creates FTS virtual table with multiple columns
- Verifies table appears in master table
- Tests: `CREATE VIRTUAL TABLE docs USING fts5(title, body)`

#### test_fts_insert_documents
- Inserts text documents into FTS table
- Verifies automatic indexing
- Tests bulk insert of multiple documents

#### test_fts_simple_match
- Basic full-text search using MATCH operator
- Tests: `SELECT * FROM docs WHERE docs MATCH 'term'`
- Verifies case-insensitive matching

### 2. Search Features (5 tests)

#### test_fts_case_insensitive
- Verifies case-insensitive search behavior
- Tests that "SQL", "sql", and "Sql" match equivalently
- Ensures consistent search results

#### test_fts_phrase_query
- Tests exact phrase matching with quotes
- Verifies: `"query language"` matches exact sequence
- Tests word order sensitivity

#### test_fts_boolean_operators
- Tests AND operator (both terms required)
- Tests OR operator (either term matches)
- Tests NOT operator (exclusion)
- Example: `'SQL AND Python'`, `'programming NOT Python'`

#### test_fts_prefix_matching
- Tests prefix matching with asterisk (*)
- Example: `'program*'` matches "program", "programming", "programmer"
- Verifies partial word matching

#### test_fts_near_operator
- Tests proximity search (NEAR operator)
- Example: `'SQL NEAR/2 database'` finds terms within 2 words
- Verifies distance-based matching

### 3. Advanced Features (3 tests)

#### test_fts_column_filter
- Tests searching specific columns
- Syntax: `'title:SQL'`, `'author:Smith'`, `'body:SQL'`
- Verifies column-specific search filtering

#### test_fts_bm25_ranking
- Tests BM25 relevance scoring function
- Verifies: `SELECT *, bm25(docs) as score ... ORDER BY score`
- Tests result ordering by relevance

#### test_fts_rank_ordering
- Tests generic rank column
- Verifies documents ordered by relevance
- Tests: `ORDER BY rank`

### 4. FTS Table Maintenance (3 tests)

#### test_fts_update_document
- Tests UPDATE operations on FTS tables
- Verifies automatic index updates
- Ensures old content removed from index

#### test_fts_delete_document
- Tests DELETE operations on FTS tables
- Verifies documents removed from index
- Tests rowid-based deletion

#### test_fts_optimize_table
- Tests FTS index optimization
- Syntax: `INSERT INTO docs(docs) VALUES('optimize')`
- Verifies index merging for performance

### 5. Performance & Edge Cases (4 tests)

#### test_fts_large_documents
- Tests indexing of large documents (10KB+)
- Verifies performance with substantial text
- Tests search within large documents

#### test_fts_tokenization
- Tests word boundary detection
- Tests handling of: emails, URLs, apostrophes
- Verifies correct token extraction

#### test_fts_empty_table
- Tests queries on empty FTS tables
- Verifies no errors with zero results
- Tests baseline behavior

#### test_fts_multiple_tables
- Tests multiple FTS tables in same database
- Verifies independent operation
- Tests: articles and comments tables

### 6. Error Handling (3 tests)

#### test_fts_invalid_match_syntax
- Tests malformed query rejection
- Tests: unmatched quotes, invalid operators, empty queries
- Verifies appropriate error messages

#### test_fts_nonexistent_column
- Tests column filter for non-existent columns
- Example: `'author:Smith'` when no author column exists
- Verifies proper error handling

#### test_fts_transactions
- Tests FTS within transactions
- Verifies rollback removes documents from index
- Tests commit persists documents in index

## FTS Features Tested

### Core FTS Functionality
- ✓ Virtual table creation (`CREATE VIRTUAL TABLE ... USING fts5`)
- ✓ Document insertion and automatic indexing
- ✓ MATCH operator for full-text queries
- ✓ Case-insensitive search
- ✓ Tokenization and word boundary detection

### Query Operators
- ✓ Boolean AND (both terms required)
- ✓ Boolean OR (either term matches)
- ✓ Boolean NOT (term exclusion)
- ✓ Phrase queries ("exact phrase")
- ✓ Prefix matching (term*)
- ✓ NEAR operator (proximity search: NEAR/N)
- ✓ Column filtering (column:term)

### Ranking & Relevance
- ✓ BM25 ranking function
- ✓ Generic rank column
- ✓ ORDER BY relevance score

### Table Operations
- ✓ INSERT documents
- ✓ UPDATE documents with index refresh
- ✓ DELETE documents with index cleanup
- ✓ OPTIMIZE for index maintenance

### Error Handling
- ✓ Invalid MATCH syntax
- ✓ Non-existent column references
- ✓ Malformed queries
- ✓ Transaction rollback/commit

### Edge Cases
- ✓ Empty FTS tables
- ✓ Large documents (10KB+)
- ✓ Multiple FTS tables
- ✓ Complex tokenization (emails, URLs, apostrophes)

## FTS Features NOT Yet Tested

The following FTS5 features are not covered by this test suite and should be added when DBSQL implements them:

### Advanced Features
- **Custom tokenizers**: Unicode61, Porter stemming, ASCII
- **FTS table options**: prefix index configuration, content tables
- **Auxiliary functions**: highlight(), snippet(), matchinfo()
- **External content tables**: Separate storage from index
- **Content-less tables**: Index-only without document storage

### Performance Features
- **Automerge configuration**: Control index segment merging
- **Crisismerge**: Emergency index consolidation
- **Pgsz**: Page size configuration for FTS tables
- **Rank**: Custom ranking weights

### Special Queries
- **Column weights**: Different importance per column
- **Token queries**: Advanced token-level matching
- **Initial token**: ^term (matches start of document)

### Advanced Operators
- **Nested operators**: Complex boolean expressions
- **Precedence**: Operator evaluation order
- **Grouping**: Parenthetical expressions

## Building and Running Tests

### Build FTS Tests
```bash
cd /home/gburd/ws/dbsql/test/src
make test_fts
```

### Run FTS Tests
```bash
cd /home/gburd/ws/dbsql/test/src
make test  # Runs all tests including FTS
# OR
LD_LIBRARY_PATH=../../build_unix.old/.libs:$LD_LIBRARY_PATH ./test_fts
```

### Expected Output (Current)
All 21 tests will be skipped with messages like:
```
ok 1 # SKIP FTS not yet implemented in DBSQL
ok 2 # SKIP FTS not yet implemented in DBSQL
...
1..21
# Tests: 21
# Passed: 0
# Failed: 0
# Skipped: 21
```

### Expected Output (After FTS Implementation)
Once FTS is implemented, remove the `TEST_SKIP()` calls from each test and expect:
```
ok 1 - test_fts_create_table
ok 2 - test_fts_insert_documents
...
1..21
# Tests: 21
# Passed: 21
# Failed: 0
# Skipped: 0
```

## Activating Tests

To activate tests once FTS is implemented:

1. **Remove TEST_SKIP() calls**: Delete or comment out the first line of each test:
   ```c
   TEST(fts_create_table) {
       // TEST_SKIP("FTS not yet implemented in DBSQL");  // Remove this line
       ...
   }
   ```

2. **Rebuild tests**: `make clean && make test_fts`

3. **Run tests**: `make test`

4. **Fix failures**: Address any API differences between SQLite FTS5 and DBSQL's implementation

## Test Framework Integration

The FTS tests follow the same pattern as existing DBSQL tests:

- **Test macro**: `TEST(name)` for automatic registration
- **Assertions**: `ASSERT_EQ()`, `ASSERT_NE()`, `ASSERT_TRUE()`, etc.
- **TAP output**: Test Anything Protocol for CI integration
- **Cleanup**: Each test creates/removes its own database file
- **Error handling**: Tests check return codes and error messages

## Implementation Notes

### SQLite FTS5 Compatibility

These tests are based on SQLite FTS5 specification and should be compatible once DBSQL implements FTS. Key considerations:

1. **Virtual table syntax**: `CREATE VIRTUAL TABLE ... USING fts5(...)`
2. **MATCH operator**: `WHERE table MATCH 'query'`
3. **Ranking functions**: `bm25()` or `rank` column
4. **FTS commands**: `INSERT INTO table(table) VALUES('optimize')`

### Berkeley DB Integration

DBSQL's FTS implementation will likely need to:

1. **Store inverted index** in Berkeley DB tables
2. **Manage posting lists** efficiently
3. **Support transactions** for index updates
4. **Handle concurrent access** with proper locking

### Performance Considerations

The test suite includes:
- Large document tests (10KB)
- Bulk insert tests (100 documents)
- Optimization tests
- Multiple table tests

These should help identify performance bottlenecks during FTS development.

## Coverage Summary

### Test Distribution
- Basic operations: 3 tests (14%)
- Search features: 5 tests (24%)
- Advanced features: 3 tests (14%)
- Maintenance: 3 tests (14%)
- Performance/edge cases: 4 tests (19%)
- Error handling: 3 tests (14%)

### Feature Coverage
- **High coverage**: Basic MATCH queries, boolean operators, ranking
- **Medium coverage**: Tokenization, column filtering, transactions
- **Low coverage**: Custom tokenizers, external content, advanced configuration

### Recommended Additions

When FTS is implemented, consider adding:
1. Benchmark tests for large datasets (100K+ documents)
2. Concurrency tests (multiple writers/readers)
3. Unicode tests (multi-language content)
4. Custom tokenizer tests (if supported)
5. Integration tests with regular SQL tables

## References

- **SQLite FTS5**: https://www.sqlite.org/fts5.html
- **DBSQL Test Framework**: `/home/gburd/ws/dbsql/test/src/test_framework.h`
- **Example Tests**: `/home/gburd/ws/dbsql/test/src/test_api.c`

## Conclusion

This comprehensive FTS test suite provides:
- **21 tests** covering core FTS functionality
- **6 test categories** for organized coverage
- **Ready to activate** once FTS is implemented
- **Foundation** for future FTS development
- **Specification** documenting expected behavior

The tests are currently skipped but compile successfully and integrate with the existing test infrastructure. Once DBSQL implements FTS support, simply remove the `TEST_SKIP()` calls to activate full FTS testing.
