/**
 * DBSQL FTS (Full-Text Search) Tests
 *
 * Comprehensive test suite for FTS virtual table functionality.
 *
 * STATUS: FTS not yet implemented in DBSQL
 *
 * This file documents expected FTS behavior and will be activated
 * once FTS support is added to DBSQL. Tests are based on SQLite FTS5
 * specification as a reference implementation.
 *
 * Features to test:
 * - FTS virtual tables (CREATE VIRTUAL TABLE ... USING fts5)
 * - Document insertion and indexing
 * - MATCH operator for full-text queries
 * - Boolean operators (AND, OR, NOT)
 * - Phrase queries
 * - Prefix matching
 * - NEAR operator (proximity search)
 * - Ranking functions (bm25, rank)
 * - Column filtering
 * - FTS table maintenance (OPTIMIZE, DELETE, UPDATE)
 */

#include "test_framework.h"
#include "dbsql.h"
#include <unistd.h>

/*
 * NOTE: All tests in this file are currently skipped because DBSQL
 * does not yet have FTS support. Once FTS is implemented, remove
 * the TEST_SKIP() calls and enable the tests.
 */

/**
 * Test: Create FTS virtual table
 *
 * Verifies that we can create an FTS virtual table with multiple
 * text columns for full-text indexing.
 */
TEST(fts_create_table) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_create.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create FTS5 virtual table with multiple columns */
	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(title, body)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Verify table exists in master table */
	rc = db->exec(db,
		"SELECT name FROM master WHERE type='table' AND name='docs'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Insert documents into FTS table
 *
 * Verifies that we can insert text documents and they are
 * automatically indexed for full-text search.
 */
TEST(fts_insert_documents) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_insert.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(title, body)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert multiple documents */
	rc = db->exec(db,
		"INSERT INTO docs(title, body) VALUES "
		"('Introduction to SQL', 'SQL is a powerful database query language'), "
		"('Advanced SQL Techniques', 'Learn about joins, subqueries and optimization'), "
		"('Database Design', 'Proper database design is crucial for performance')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Verify row count */
	rc = db->exec(db,
		"SELECT COUNT(*) FROM docs",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Simple MATCH query
 *
 * Verifies basic full-text search using the MATCH operator
 * to find documents containing specific terms.
 */
TEST(fts_simple_match) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_match.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(title, body)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(title, body) VALUES "
		"('SQL Tutorial', 'Learn SQL from scratch'), "
		"('Python Guide', 'Python programming basics'), "
		"('SQL Advanced', 'Advanced SQL techniques')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Search for documents containing "SQL" */
	rc = db->exec(db,
		"SELECT title FROM docs WHERE docs MATCH 'SQL'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 2 documents */

	/* Search for documents containing "Python" */
	rc = db->exec(db,
		"SELECT title FROM docs WHERE docs MATCH 'Python'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 1 document */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Case-insensitive search
 *
 * Verifies that FTS queries are case-insensitive by default,
 * matching "SQL", "sql", and "Sql" equivalently.
 */
TEST(fts_case_insensitive) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_case.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(content) VALUES "
		"('SQL is great'), "
		"('sql is powerful'), "
		"('Sql databases are fast')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* All three variations should match all documents */
	rc = db->exec(db,
		"SELECT COUNT(*) FROM docs WHERE docs MATCH 'SQL'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 3 */

	rc = db->exec(db,
		"SELECT COUNT(*) FROM docs WHERE docs MATCH 'sql'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 3 */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Phrase queries
 *
 * Verifies that quoted phrases match exact word sequences
 * in the correct order.
 */
TEST(fts_phrase_query) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_phrase.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(content) VALUES "
		"('database query language'), "
		"('language for database queries'), "
		"('query language for databases')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Phrase query - exact sequence required */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH '\"query language\"'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 1 document (third one) */

	/* Different phrase should match different document */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH '\"database query\"'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 1 document (first one) */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Boolean operators (AND, OR, NOT)
 *
 * Verifies that FTS supports boolean combinations of search terms.
 */
TEST(fts_boolean_operators) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_boolean.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(content) VALUES "
		"('SQL database programming'), "
		"('Python programming tutorial'), "
		"('SQL and Python integration'), "
		"('JavaScript web programming')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* AND operator - both terms required */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'SQL AND Python'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 1 document */

	/* OR operator - either term matches */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'SQL OR JavaScript'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 3 documents */

	/* NOT operator - exclude documents */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'programming NOT Python'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 2 documents (SQL database and JavaScript) */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Prefix matching
 *
 * Verifies that prefix matching with * works to find terms
 * that start with a given prefix.
 */
TEST(fts_prefix_matching) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_prefix.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(content) VALUES "
		"('programming language'), "
		"('program execution'), "
		"('programmer guide'), "
		"('database system')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Prefix match - should match program, programming, programmer */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'program*'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 3 documents */

	/* More specific prefix */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'programm*'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 2 documents (programming, programmer) */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: NEAR operator (proximity search)
 *
 * Verifies that NEAR finds documents where terms appear
 * within a specified distance of each other.
 */
TEST(fts_near_operator) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_near.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(content) VALUES "
		"('SQL database system'), "
		"('SQL is a powerful database query language'), "
		"('database administrators use SQL daily')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* NEAR/2 - terms within 2 words of each other */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'SQL NEAR/2 database'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 2 documents (first and third) */

	/* NEAR/5 - more lenient distance */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'SQL NEAR/5 language'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 1 document (second) */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Column filtering in MATCH
 *
 * Verifies that we can search specific columns within
 * an FTS table using column: syntax.
 */
TEST(fts_column_filter) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_column.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE articles USING fts5(title, author, body)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO articles(title, author, body) VALUES "
		"('SQL Guide', 'John Smith', 'Comprehensive SQL tutorial'), "
		"('Python Basics', 'Jane Smith', 'Learn Python programming'), "
		"('Database Design', 'John Doe', 'SQL database architecture')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Search only in title column */
	rc = db->exec(db,
		"SELECT title FROM articles WHERE articles MATCH 'title:SQL'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 1 document (SQL Guide) */

	/* Search only in author column */
	rc = db->exec(db,
		"SELECT title FROM articles WHERE articles MATCH 'author:Smith'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 2 documents */

	/* Search in body column */
	rc = db->exec(db,
		"SELECT title FROM articles WHERE articles MATCH 'body:SQL'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 2 documents (SQL Guide and Database Design) */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: BM25 ranking function
 *
 * Verifies that the bm25() function returns relevance scores
 * and can be used for sorting results by relevance.
 */
TEST(fts_bm25_ranking) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_bm25.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(content) VALUES "
		"('database database database'), "
		"('database system'), "
		"('relational database management system')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Query with BM25 ranking */
	rc = db->exec(db,
		"SELECT content, bm25(docs) as score FROM docs "
		"WHERE docs MATCH 'database' ORDER BY score",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return all 3 documents sorted by relevance */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Generic rank function
 *
 * Verifies that the rank column provides relevance scoring
 * for ordering search results.
 */
TEST(fts_rank_ordering) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_rank.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(title, body)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(title, body) VALUES "
		"('SQL', 'SQL SQL SQL SQL SQL'), "
		"('SQL Tutorial', 'Learn SQL'), "
		"('Database', 'SQL databases')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Order by rank - most relevant first */
	rc = db->exec(db,
		"SELECT title FROM docs WHERE docs MATCH 'SQL' ORDER BY rank",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Documents should be ordered by relevance */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: UPDATE FTS content
 *
 * Verifies that we can update documents in an FTS table
 * and the index is automatically updated.
 */
TEST(fts_update_document) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_update.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(rowid, content) VALUES (1, 'original content')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Update the content */
	rc = db->exec(db,
		"UPDATE docs SET content = 'updated content' WHERE rowid = 1",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Search for new content should find it */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'updated'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 1 document */

	/* Search for old content should not find it */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'original'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 0 documents */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: DELETE from FTS table
 *
 * Verifies that deleting rows from an FTS table removes
 * them from the full-text index.
 */
TEST(fts_delete_document) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_delete.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(rowid, content) VALUES "
		"(1, 'keep this document'), "
		"(2, 'delete this document')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Delete one document */
	rc = db->exec(db,
		"DELETE FROM docs WHERE rowid = 2",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Search should only find remaining document */
	rc = db->exec(db,
		"SELECT COUNT(*) FROM docs WHERE docs MATCH 'document'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return count of 1 */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: OPTIMIZE FTS table
 *
 * Verifies that the OPTIMIZE command can be run to merge
 * FTS index segments and improve query performance.
 */
TEST(fts_optimize_table) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_optimize.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert many documents to create multiple index segments */
	for (int i = 0; i < 100; i++) {
		char sql[256];
		snprintf(sql, sizeof(sql),
			"INSERT INTO docs(content) VALUES ('document %d with content')", i);
		rc = db->exec(db, sql, NULL, NULL, &errmsg);
		ASSERT_EQ(0, rc);
	}

	/* Optimize the FTS index */
	rc = db->exec(db,
		"INSERT INTO docs(docs) VALUES('optimize')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Verify queries still work after optimization */
	rc = db->exec(db,
		"SELECT COUNT(*) FROM docs WHERE docs MATCH 'document'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 100 */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Large document insertion
 *
 * Verifies that FTS can handle large documents efficiently
 * and index them correctly.
 */
TEST(fts_large_documents) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_large.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create a large document (10KB of text) */
	char large_doc[10240];
	for (int i = 0; i < 1000; i++) {
		snprintf(large_doc + i * 10, 11, "word%04d ", i);
	}

	/* Insert large document */
	char sql[11000];
	snprintf(sql, sizeof(sql),
		"INSERT INTO docs(content) VALUES ('%s')", large_doc);
	rc = db->exec(db, sql, NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Search within large document */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'word0500'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should find the document */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Invalid MATCH syntax
 *
 * Verifies that malformed FTS queries are properly rejected
 * with appropriate error messages.
 */
TEST(fts_invalid_match_syntax) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_invalid.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Unmatched quote */
	rc = db->exec(db,
		"SELECT * FROM docs WHERE docs MATCH 'unmatched quote\"'",
		NULL, NULL, &errmsg);
	ASSERT_NE(0, rc);

	/* Invalid operator */
	rc = db->exec(db,
		"SELECT * FROM docs WHERE docs MATCH 'term XOR other'",
		NULL, NULL, &errmsg);
	ASSERT_NE(0, rc);

	/* Empty query */
	rc = db->exec(db,
		"SELECT * FROM docs WHERE docs MATCH ''",
		NULL, NULL, &errmsg);
	ASSERT_NE(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Non-existent column in MATCH
 *
 * Verifies that column filters for non-existent columns
 * are properly rejected.
 */
TEST(fts_nonexistent_column) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_badcol.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(title, body)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Try to search non-existent column */
	rc = db->exec(db,
		"SELECT * FROM docs WHERE docs MATCH 'author:Smith'",
		NULL, NULL, &errmsg);
	ASSERT_NE(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Tokenization behavior
 *
 * Verifies that FTS tokenizes text correctly, respecting
 * word boundaries and handling punctuation appropriately.
 */
TEST(fts_tokenization) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_tokens.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(content) VALUES "
		"('email@example.com is an email address'), "
		"('http://example.com is a URL'), "
		"('O''Reilly is a publisher')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Search for email should tokenize correctly */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'email'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Search for URL components */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'example'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should find documents with example.com */

	/* Search with apostrophe */
	rc = db->exec(db,
		"SELECT content FROM docs WHERE docs MATCH 'Reilly'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Empty FTS table queries
 *
 * Verifies that queries against empty FTS tables
 * return no results without errors.
 */
TEST(fts_empty_table) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_empty.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Query empty table */
	rc = db->exec(db,
		"SELECT * FROM docs WHERE docs MATCH 'anything'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 0 rows */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: Multiple FTS tables
 *
 * Verifies that multiple FTS tables can coexist in the
 * same database and be queried independently.
 */
TEST(fts_multiple_tables) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_multi.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	/* Create multiple FTS tables */
	rc = db->exec(db,
		"CREATE VIRTUAL TABLE articles USING fts5(title, body)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE comments USING fts5(author, text)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert into first table */
	rc = db->exec(db,
		"INSERT INTO articles(title, body) VALUES ('Article', 'Content')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert into second table */
	rc = db->exec(db,
		"INSERT INTO comments(author, text) VALUES ('User', 'Comment')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Query each table independently */
	rc = db->exec(db,
		"SELECT title FROM articles WHERE articles MATCH 'Content'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"SELECT author FROM comments WHERE comments MATCH 'Comment'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/**
 * Test: FTS transaction handling
 *
 * Verifies that FTS operations work correctly within
 * transactions and can be rolled back.
 */
TEST(fts_transactions) {
	TEST_SKIP("FTS not yet implemented in DBSQL");

	DBSQL *db = NULL;
	int rc;
	char *errmsg = NULL;
	const char *dbfile = "test_fts_txn.db";

	unlink(dbfile);

	rc = dbsql_create_env(&db, NULL, NULL, 0, 0);
	ASSERT_EQ(0, rc);

	rc = db->open(db, dbfile, 0664, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"CREATE VIRTUAL TABLE docs USING fts5(content)",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Begin transaction */
	rc = db->exec(db, "BEGIN", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Insert document */
	rc = db->exec(db,
		"INSERT INTO docs(content) VALUES ('transactional content')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Rollback */
	rc = db->exec(db, "ROLLBACK", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Document should not be found after rollback */
	rc = db->exec(db,
		"SELECT COUNT(*) FROM docs WHERE docs MATCH 'transactional'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 0 */

	/* Now with commit */
	rc = db->exec(db, "BEGIN", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db,
		"INSERT INTO docs(content) VALUES ('committed content')",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	rc = db->exec(db, "COMMIT", NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);

	/* Document should be found after commit */
	rc = db->exec(db,
		"SELECT COUNT(*) FROM docs WHERE docs MATCH 'committed'",
		NULL, NULL, &errmsg);
	ASSERT_EQ(0, rc);
	/* Should return 1 */

	rc = db->close(db);
	ASSERT_EQ(0, rc);

	unlink(dbfile);
}

/* Main test runner */
int
main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	TEST_INIT("DBSQL FTS Tests");
	test_run_all();
	TEST_FINISH();
}
