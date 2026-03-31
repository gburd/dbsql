/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _VTAB_H_
#define _VTAB_H_

/*
 * Virtual Table Interface for DBSQL
 *
 * This file defines the virtual table mechanism for DBSQL, compatible with
 * SQLite's virtual table API. Virtual tables allow application code to
 * provide table implementations that are queried like regular tables but
 * whose data comes from external sources (files, network, computation, etc.)
 *
 * Key use cases:
 *   - Full-text search (FTS5)
 *   - Spatial indexing (R-Tree)
 *   - CSV/JSON file readers
 *   - External data source adapters
 *
 * Architecture:
 *   1. Module: A registered virtual table implementation (sqlite3_module)
 *   2. Virtual Table Instance: Per-table instance (sqlite3_vtab)
 *   3. Cursor: Per-query cursor for iteration (sqlite3_vtab_cursor)
 *
 * Reference: SQLite virtual table documentation (sqlite.org/vtab.html)
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Forward declarations for types defined in dbsql_int.h
 */
struct DBSQL;
typedef struct DBSQL DBSQL;
struct mem;
typedef struct mem mem_t;
struct func_context;
typedef struct func_context func_context_t;

/*
 * Virtual table base structure.
 * Application code creates structures that include this as the first member.
 *
 * Example:
 *   struct my_vtab {
 *       sqlite3_vtab base;  // Must be first
 *       int custom_field;
 *   };
 *
 * (SQLite: struct sqlite3_vtab, sqlite3.h)
 */
typedef struct sqlite3_vtab sqlite3_vtab;
struct sqlite3_vtab {
	const struct sqlite3_module *pModule;  /* The module for this vtab */
	int nRef;                               /* Number of active cursors */
	char *zErrMsg;                          /* Error message from xCreate/xConnect */
};

/*
 * Virtual table cursor structure.
 * Application code creates structures that include this as the first member.
 *
 * Example:
 *   struct my_cursor {
 *       sqlite3_vtab_cursor base;  // Must be first
 *       int current_row;
 *   };
 *
 * (SQLite: struct sqlite3_vtab_cursor, sqlite3.h)
 */
typedef struct sqlite3_vtab_cursor sqlite3_vtab_cursor;
struct sqlite3_vtab_cursor {
	sqlite3_vtab *pVtab;  /* Virtual table of this cursor */
};

/*
 * Index constraint information passed to xBestIndex.
 * Describes a single constraint from the WHERE clause.
 *
 * (SQLite: struct sqlite3_index_constraint, sqlite3.h)
 */
struct sqlite3_index_constraint {
	int iColumn;              /* Column constrained (-1 for ROWID) */
	unsigned char op;         /* Constraint operator (see below) */
	unsigned char usable;     /* True if this constraint is usable */
	int iTermOffset;          /* Used internally - xBestIndex should ignore */
};

/*
 * Constraint usage information returned by xBestIndex.
 * Tells the query planner how to use each constraint.
 *
 * (SQLite: struct sqlite3_index_constraint_usage, sqlite3.h)
 */
struct sqlite3_index_constraint_usage {
	int argvIndex;           /* If >0, constraint is part of argv to xFilter */
	unsigned char omit;      /* If true, do not code a test for this constraint */
};

/*
 * ORDER BY clause information passed to xBestIndex.
 *
 * (SQLite: struct sqlite3_index_orderby, sqlite3.h)
 */
struct sqlite3_index_orderby {
	int iColumn;              /* Column number */
	unsigned char desc;       /* True for DESC, false for ASC */
};

/*
 * Index selection information for xBestIndex method.
 * The query planner fills in constraint/orderby arrays, and the virtual
 * table module fills in estimated cost and usage arrays.
 *
 * (SQLite: struct sqlite3_index_info, sqlite3.h)
 */
typedef struct sqlite3_index_info sqlite3_index_info;
struct sqlite3_index_info {
	/* Inputs */
	int nConstraint;           /* Number of entries in aConstraint */
	struct sqlite3_index_constraint *aConstraint;  /* Array of constraints */
	int nOrderBy;              /* Number of terms in the ORDER BY clause */
	struct sqlite3_index_orderby *aOrderBy;       /* Array of ORDER BY columns */

	/* Outputs */
	struct sqlite3_index_constraint_usage *aConstraintUsage;
	int idxNum;                /* Number used to identify the index */
	char *idxStr;              /* String, possibly obtained from malloc */
	int needToFreeIdxStr;      /* Free idxStr using free() if true */
	int orderByConsumed;       /* True if output is already ordered */
	double estimatedCost;      /* Estimated cost of using this index */
	i64 estimatedRows;         /* Estimated number of rows returned */
	int idxFlags;              /* Mask of SQLITE_INDEX_SCAN_* flags */
	i64 colUsed;               /* Input: Mask of columns used by statement */
};

/*
 * Constraint operators for xBestIndex.
 *
 * (SQLite: SQLITE_INDEX_CONSTRAINT_* constants)
 */
#define SQLITE_INDEX_CONSTRAINT_EQ         2
#define SQLITE_INDEX_CONSTRAINT_GT         4
#define SQLITE_INDEX_CONSTRAINT_LE         8
#define SQLITE_INDEX_CONSTRAINT_LT        16
#define SQLITE_INDEX_CONSTRAINT_GE        32
#define SQLITE_INDEX_CONSTRAINT_MATCH     64
#define SQLITE_INDEX_CONSTRAINT_LIKE      65
#define SQLITE_INDEX_CONSTRAINT_GLOB      66
#define SQLITE_INDEX_CONSTRAINT_REGEXP    67
#define SQLITE_INDEX_CONSTRAINT_NE        68
#define SQLITE_INDEX_CONSTRAINT_ISNOT     69
#define SQLITE_INDEX_CONSTRAINT_ISNOTNULL 70
#define SQLITE_INDEX_CONSTRAINT_ISNULL    71
#define SQLITE_INDEX_CONSTRAINT_IS        72
#define SQLITE_INDEX_CONSTRAINT_LIMIT     73
#define SQLITE_INDEX_CONSTRAINT_OFFSET    74
#define SQLITE_INDEX_CONSTRAINT_FUNCTION 150

/*
 * Index scan flags for sqlite3_index_info.idxFlags.
 *
 * (SQLite: SQLITE_INDEX_SCAN_* constants)
 */
#define SQLITE_INDEX_SCAN_UNIQUE      1   /* Scan visits at most 1 row */

/*
 * Virtual table module structure.
 * Defines the methods that implement a virtual table.
 *
 * All methods return DBSQL_SUCCESS on success, error code on failure.
 * Methods can set vtab->zErrMsg to an error message string (will be freed).
 *
 * Required methods: xCreate/xConnect, xBestIndex, xDisconnect, xDestroy,
 *                   xOpen, xClose, xFilter, xNext, xEof, xColumn, xRowid
 *
 * Optional methods: xUpdate, xBegin, xSync, xCommit, xRollback, xFindFunction,
 *                   xRename, xSavepoint, xRelease, xRollbackTo, xShadowName
 *
 * (SQLite: struct sqlite3_module, sqlite3.h)
 */
typedef struct sqlite3_module sqlite3_module;
struct sqlite3_module {
	int iVersion;  /* Module version (0, 1, 2, or 3) */

	/*
	 * Create a new virtual table.
	 * Called when CREATE VIRTUAL TABLE is executed.
	 */
	int (*xCreate)(DBSQL *db, void *pAux, int argc, const char *const *argv,
	               sqlite3_vtab **ppVTab, char **pzErr);

	/*
	 * Connect to an existing virtual table.
	 * Called when opening a database containing a virtual table.
	 */
	int (*xConnect)(DBSQL *db, void *pAux, int argc, const char *const *argv,
	                sqlite3_vtab **ppVTab, char **pzErr);

	/*
	 * Determine the best way to access the virtual table.
	 * Called by query planner for each query involving this table.
	 */
	int (*xBestIndex)(sqlite3_vtab *pVTab, sqlite3_index_info *pInfo);

	/*
	 * Disconnect from virtual table.
	 * Called when closing a database connection.
	 */
	int (*xDisconnect)(sqlite3_vtab *pVTab);

	/*
	 * Destroy the virtual table.
	 * Called when DROP TABLE is executed.
	 */
	int (*xDestroy)(sqlite3_vtab *pVTab);

	/*
	 * Open a cursor for accessing the virtual table.
	 */
	int (*xOpen)(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCursor);

	/*
	 * Close a cursor.
	 */
	int (*xClose)(sqlite3_vtab_cursor *pCursor);

	/*
	 * Configure the cursor for a particular query.
	 * idxNum and idxStr come from xBestIndex output.
	 * argc and argv contain constraint values.
	 */
	int (*xFilter)(sqlite3_vtab_cursor *pCursor, int idxNum,
	               const char *idxStr, int argc, mem_t **argv);

	/*
	 * Advance cursor to next row.
	 */
	int (*xNext)(sqlite3_vtab_cursor *pCursor);

	/*
	 * Return true if cursor is at end of data.
	 */
	int (*xEof)(sqlite3_vtab_cursor *pCursor);

	/*
	 * Return column value at current cursor position.
	 * Use __set_result_*() functions to set the result.
	 */
	int (*xColumn)(sqlite3_vtab_cursor *pCursor, func_context_t *pCtx, int iCol);

	/*
	 * Return rowid of current row.
	 */
	int (*xRowid)(sqlite3_vtab_cursor *pCursor, i64 *pRowid);

	/*
	 * Update/insert/delete row (optional).
	 * If argc == 1: delete row with rowid argv[0]
	 * If argc > 1 && argv[0] == NULL: insert new row
	 * If argc > 1 && argv[0] != NULL: update existing row
	 */
	int (*xUpdate)(sqlite3_vtab *pVTab, int argc, mem_t **argv, i64 *pRowid);

	/*
	 * Begin a transaction (optional).
	 */
	int (*xBegin)(sqlite3_vtab *pVTab);

	/*
	 * Sync transaction (optional).
	 */
	int (*xSync)(sqlite3_vtab *pVTab);

	/*
	 * Commit transaction (optional).
	 */
	int (*xCommit)(sqlite3_vtab *pVTab);

	/*
	 * Rollback transaction (optional).
	 */
	int (*xRollback)(sqlite3_vtab *pVTab);

	/*
	 * Find a custom function (optional).
	 * Allows virtual table to override SQL functions.
	 */
	int (*xFindFunction)(sqlite3_vtab *pVTab, int nArg, const char *zName,
	                     void (**pxFunc)(func_context_t *, int, mem_t **),
	                     void **ppArg);

	/*
	 * Rename the virtual table (optional).
	 */
	int (*xRename)(sqlite3_vtab *pVTab, const char *zNew);

	/* Methods added in version 2 and later */
	int (*xSavepoint)(sqlite3_vtab *pVTab, int iSavepoint);
	int (*xRelease)(sqlite3_vtab *pVTab, int iSavepoint);
	int (*xRollbackTo)(sqlite3_vtab *pVTab, int iSavepoint);

	/* Methods added in version 3 */
	int (*xShadowName)(const char *zName);  /* Is this a shadow table name? */
};

/*
 * Function declarations for virtual table management.
 */

/*
 * Register a virtual table module.
 *
 * db        - Database connection
 * zName     - Name of the module
 * pModule   - Module implementation
 * pAux      - Auxiliary data passed to xCreate/xConnect
 * xDestroy  - Destructor for pAux (optional)
 *
 * Returns DBSQL_SUCCESS on success, error code on failure.
 */
int __dbsql_create_module(
	DBSQL *db,
	const char *zName,
	const sqlite3_module *pModule,
	void *pAux,
	void (*xDestroy)(void *)
);

/*
 * Declare the schema for a virtual table.
 * Called from within xCreate/xConnect to define table columns.
 *
 * db   - Database connection
 * zSQL - CREATE TABLE statement defining the schema
 *
 * Example:
 *   __dbsql_declare_vtab(db, "CREATE TABLE x(a, b, c)");
 *
 * Returns DBSQL_SUCCESS on success, error code on failure.
 */
int __dbsql_declare_vtab(DBSQL *db, const char *zSQL);

/*
 * Internal function: Create or connect to a virtual table instance.
 * Called during table lookup/creation.
 */
int __vtab_call_create(DBSQL *db, int iDb, const char *zTab,
                       char **pzErr);

/*
 * Internal function: Disconnect a virtual table.
 */
int __vtab_call_disconnect(DBSQL *db, vtable_t *pVTab);

/*
 * Internal function: Destroy a virtual table.
 */
int __vtab_call_destroy(DBSQL *db, int iDb, const char *zTab);

/*
 * Internal function: Begin using a virtual table.
 * Ensures the vtab is connected and ready for use.
 */
int __vtab_begin(DBSQL *db, vtable_t *pVTab);

/*
 * Internal function: Call savepoint methods on all virtual tables.
 */
int __vtab_savepoint(DBSQL *db, int op, int iSavepoint);

/*
 * Internal function: Call transaction methods on all virtual tables.
 */
int __vtab_sync(DBSQL *db, int rc);
int __vtab_commit(DBSQL *db);
int __vtab_rollback(DBSQL *db);

#if defined(__cplusplus)
}
#endif

#endif /* _VTAB_H_ */
