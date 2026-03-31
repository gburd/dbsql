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

/*
 * CSV Virtual Table Module
 *
 * This module implements a simple CSV file reader as a virtual table.
 * It demonstrates the virtual table API and provides a practical example
 * for testing and reference.
 *
 * Usage:
 *   CREATE VIRTUAL TABLE temp.csv USING csv(
 *       filename = 'data.csv',
 *       header = yes,
 *       separator = ','
 *   );
 *
 * Features:
 *   - Read CSV files with configurable separator
 *   - Optional header row for column names
 *   - Automatic column count detection
 *   - Basic WHERE clause support (full table scan)
 *
 * Limitations:
 *   - Read-only (no INSERT/UPDATE/DELETE)
 *   - No quoting/escaping support (simple split on separator)
 *   - Entire file must fit in memory
 *   - No index support (always full scan)
 */

#include "inc/dbsql_config.h"
#include "dbsql_int.h"
#include "inc/vtab.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * CSV virtual table structure.
 */
typedef struct csv_vtab csv_vtab;
struct csv_vtab {
	sqlite3_vtab base;      /* Base class - must be first */
	char *zFilename;        /* CSV filename */
	char *zSeparator;       /* Column separator (default ",") */
	int hasHeader;          /* True if first row is header */
	int nColumn;            /* Number of columns */
	char **azColumn;        /* Column names */
	int nRow;               /* Number of data rows */
	char ***rows;           /* Array of rows, each row is array of strings */
};

/*
 * CSV cursor structure.
 */
typedef struct csv_cursor csv_cursor;
struct csv_cursor {
	sqlite3_vtab_cursor base;  /* Base class - must be first */
	int iRow;                  /* Current row number (0-based) */
	int isEof;                 /* True when cursor is exhausted */
};

/*
 * Forward declarations.
 */
static int csv_connect(DBSQL *db, void *pAux, int argc, const char *const *argv,
                       sqlite3_vtab **ppVtab, char **pzErr);
static int csv_disconnect(sqlite3_vtab *pVtab);
static int csv_best_index(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo);
static int csv_open(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor);
static int csv_close(sqlite3_vtab_cursor *pCursor);
static int csv_filter(sqlite3_vtab_cursor *pCursor, int idxNum,
                      const char *idxStr, int argc, mem_t **argv);
static int csv_next(sqlite3_vtab_cursor *pCursor);
static int csv_eof(sqlite3_vtab_cursor *pCursor);
static int csv_column(sqlite3_vtab_cursor *pCursor, func_context_t *pCtx, int iCol);
static int csv_rowid(sqlite3_vtab_cursor *pCursor, i64 *pRowid);

/*
 * CSV module definition.
 */
static sqlite3_module csv_module = {
	0,                  /* iVersion */
	csv_connect,        /* xCreate - use xConnect */
	csv_connect,        /* xConnect */
	csv_best_index,     /* xBestIndex */
	csv_disconnect,     /* xDisconnect */
	csv_disconnect,     /* xDestroy - use xDisconnect */
	csv_open,           /* xOpen */
	csv_close,          /* xClose */
	csv_filter,         /* xFilter */
	csv_next,           /* xNext */
	csv_eof,            /* xEof */
	csv_column,         /* xColumn */
	csv_rowid,          /* xRowid */
	NULL,               /* xUpdate */
	NULL,               /* xBegin */
	NULL,               /* xSync */
	NULL,               /* xCommit */
	NULL,               /* xRollback */
	NULL,               /* xFindFunction */
	NULL,               /* xRename */
};

/*
 * Helper: Parse CSV configuration from arguments.
 */
static int
csv_parse_args(DBSQL *db, int argc, const char *const *argv,
               char **pzFilename, char **pzSeparator, int *pHasHeader,
               char **pzErr)
{
	int i;

	*pzFilename = NULL;
	*pzSeparator = strdup(",");
	*pHasHeader = 1;

	/* argv[0] = module name ("csv")
	 * argv[1] = database name
	 * argv[2] = table name
	 * argv[3...] = arguments
	 */
	for (i = 3; i < argc; i++) {
		const char *zArg = argv[i];
		if (strncmp(zArg, "filename=", 9) == 0) {
			*pzFilename = strdup(zArg + 9);
		} else if (strncmp(zArg, "separator=", 10) == 0) {
			free(*pzSeparator);
			*pzSeparator = strdup(zArg + 10);
		} else if (strcmp(zArg, "header=yes") == 0) {
			*pHasHeader = 1;
		} else if (strcmp(zArg, "header=no") == 0) {
			*pHasHeader = 0;
		}
	}

	if (*pzFilename == NULL) {
		*pzErr = __dbsql_mprintf(db, "csv: filename parameter required");
		free(*pzSeparator);
		return DBSQL_ERROR;
	}

	return DBSQL_SUCCESS;
}

/*
 * Helper: Read entire CSV file into memory.
 */
static int
csv_read_file(csv_vtab *pTab, char **pzErr)
{
	FILE *fp;
	char line[4096];
	int nRow = 0;
	int nRowAlloc = 0;
	char ***rows = NULL;

	fp = fopen(pTab->zFilename, "r");
	if (fp == NULL) {
		*pzErr = __dbsql_mprintf(NULL, "csv: cannot open file '%s'", pTab->zFilename);
		return DBSQL_ERROR;
	}

	/* Read first line to determine column count */
	if (fgets(line, sizeof(line), fp) == NULL) {
		fclose(fp);
		*pzErr = __dbsql_mprintf(NULL, "csv: empty file");
		return DBSQL_ERROR;
	}

	/* Count columns */
	pTab->nColumn = 1;
	for (char *p = line; *p; p++) {
		if (*p == pTab->zSeparator[0]) {
			pTab->nColumn++;
		}
	}

	/* Parse first line */
	if (pTab->hasHeader) {
		/* Extract column names */
		pTab->azColumn = malloc(sizeof(char *) * pTab->nColumn);
		char *token = strtok(line, pTab->zSeparator);
		int iCol = 0;
		while (token != NULL && iCol < pTab->nColumn) {
			/* Remove trailing newline */
			char *nl = strchr(token, '\n');
			if (nl) *nl = '\0';
			pTab->azColumn[iCol++] = strdup(token);
			token = strtok(NULL, pTab->zSeparator);
		}
	} else {
		/* Generate default column names */
		pTab->azColumn = malloc(sizeof(char *) * pTab->nColumn);
		for (int i = 0; i < pTab->nColumn; i++) {
			char buf[32];
			snprintf(buf, sizeof(buf), "c%d", i);
			pTab->azColumn[i] = strdup(buf);
		}
		/* Rewind to re-read first line as data */
		fseek(fp, 0, SEEK_SET);
	}

	/* Read data rows */
	while (fgets(line, sizeof(line), fp) != NULL) {
		/* Allocate more space if needed */
		if (nRow >= nRowAlloc) {
			nRowAlloc = nRowAlloc == 0 ? 16 : nRowAlloc * 2;
			rows = realloc(rows, sizeof(char **) * nRowAlloc);
		}

		/* Parse row */
		rows[nRow] = malloc(sizeof(char *) * pTab->nColumn);
		char *token = strtok(line, pTab->zSeparator);
		int iCol = 0;
		while (token != NULL && iCol < pTab->nColumn) {
			/* Remove trailing newline */
			char *nl = strchr(token, '\n');
			if (nl) *nl = '\0';
			rows[nRow][iCol++] = strdup(token);
			token = strtok(NULL, pTab->zSeparator);
		}
		/* Pad with NULLs if line has fewer columns */
		while (iCol < pTab->nColumn) {
			rows[nRow][iCol++] = NULL;
		}
		nRow++;
	}

	fclose(fp);
	pTab->nRow = nRow;
	pTab->rows = rows;
	return DBSQL_SUCCESS;
}

/*
 * Connect to (or create) a CSV virtual table.
 */
static int
csv_connect(DBSQL *db, void *pAux, int argc, const char *const *argv,
            sqlite3_vtab **ppVtab, char **pzErr)
{
	csv_vtab *pTab;
	char *zFilename, *zSeparator;
	int hasHeader;
	int rc;
	char *zSchema;
	int i;

	*ppVtab = NULL;

	/* Parse arguments */
	rc = csv_parse_args(db, argc, argv, &zFilename, &zSeparator, &hasHeader, pzErr);
	if (rc != DBSQL_SUCCESS) {
		return rc;
	}

	/* Allocate virtual table structure */
	pTab = calloc(1, sizeof(csv_vtab));
	if (pTab == NULL) {
		free(zFilename);
		free(zSeparator);
		return DBSQL_NOMEM;
	}

	pTab->zFilename = zFilename;
	pTab->zSeparator = zSeparator;
	pTab->hasHeader = hasHeader;

	/* Read CSV file */
	rc = csv_read_file(pTab, pzErr);
	if (rc != DBSQL_SUCCESS) {
		free(pTab->zFilename);
		free(pTab->zSeparator);
		free(pTab);
		return rc;
	}

	/* Declare schema */
	zSchema = malloc(1024);
	strcpy(zSchema, "CREATE TABLE x(");
	for (i = 0; i < pTab->nColumn; i++) {
		if (i > 0) strcat(zSchema, ", ");
		strcat(zSchema, pTab->azColumn[i]);
		strcat(zSchema, " TEXT");
	}
	strcat(zSchema, ")");

	rc = __dbsql_declare_vtab(db, zSchema);
	free(zSchema);

	if (rc != DBSQL_SUCCESS) {
		csv_disconnect((sqlite3_vtab *)pTab);
		return rc;
	}

	*ppVtab = (sqlite3_vtab *)pTab;
	return DBSQL_SUCCESS;
}

/*
 * Disconnect from CSV virtual table.
 */
static int
csv_disconnect(sqlite3_vtab *pVtab)
{
	csv_vtab *pTab = (csv_vtab *)pVtab;
	int i, j;

	if (pTab->zFilename) free(pTab->zFilename);
	if (pTab->zSeparator) free(pTab->zSeparator);

	if (pTab->azColumn) {
		for (i = 0; i < pTab->nColumn; i++) {
			if (pTab->azColumn[i]) free(pTab->azColumn[i]);
		}
		free(pTab->azColumn);
	}

	if (pTab->rows) {
		for (i = 0; i < pTab->nRow; i++) {
			for (j = 0; j < pTab->nColumn; j++) {
				if (pTab->rows[i][j]) free(pTab->rows[i][j]);
			}
			free(pTab->rows[i]);
		}
		free(pTab->rows);
	}

	free(pTab);
	return DBSQL_SUCCESS;
}

/*
 * Determine the best query plan.
 * For this simple module, we always do a full table scan.
 */
static int
csv_best_index(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo)
{
	/* No indexes - full table scan */
	pInfo->estimatedCost = 100000.0;  /* High cost - no optimization */
	pInfo->estimatedRows = 100000;
	return DBSQL_SUCCESS;
}

/*
 * Open a cursor on the CSV table.
 */
static int
csv_open(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor)
{
	csv_cursor *pCur;

	pCur = calloc(1, sizeof(csv_cursor));
	if (pCur == NULL) {
		return DBSQL_NOMEM;
	}

	pCur->base.pVtab = pVtab;
	pCur->iRow = -1;
	pCur->isEof = 0;

	*ppCursor = (sqlite3_vtab_cursor *)pCur;
	return DBSQL_SUCCESS;
}

/*
 * Close a cursor.
 */
static int
csv_close(sqlite3_vtab_cursor *pCursor)
{
	csv_cursor *pCur = (csv_cursor *)pCursor;
	free(pCur);
	return DBSQL_SUCCESS;
}

/*
 * Begin a scan of the CSV table.
 */
static int
csv_filter(sqlite3_vtab_cursor *pCursor, int idxNum, const char *idxStr,
           int argc, mem_t **argv)
{
	csv_cursor *pCur = (csv_cursor *)pCursor;
	csv_vtab *pTab = (csv_vtab *)pCursor->pVtab;

	pCur->iRow = 0;
	pCur->isEof = (pTab->nRow == 0);

	return DBSQL_SUCCESS;
}

/*
 * Advance to the next row.
 */
static int
csv_next(sqlite3_vtab_cursor *pCursor)
{
	csv_cursor *pCur = (csv_cursor *)pCursor;
	csv_vtab *pTab = (csv_vtab *)pCursor->pVtab;

	pCur->iRow++;
	if (pCur->iRow >= pTab->nRow) {
		pCur->isEof = 1;
	}

	return DBSQL_SUCCESS;
}

/*
 * Return true if cursor is at EOF.
 */
static int
csv_eof(sqlite3_vtab_cursor *pCursor)
{
	csv_cursor *pCur = (csv_cursor *)pCursor;
	return pCur->isEof;
}

/*
 * Return a column value.
 */
static int
csv_column(sqlite3_vtab_cursor *pCursor, func_context_t *pCtx, int iCol)
{
	csv_cursor *pCur = (csv_cursor *)pCursor;
	csv_vtab *pTab = (csv_vtab *)pCursor->pVtab;
	const char *zValue;

	if (iCol < 0 || iCol >= pTab->nColumn || pCur->iRow >= pTab->nRow) {
		return DBSQL_ERROR;
	}

	zValue = pTab->rows[pCur->iRow][iCol];
	if (zValue == NULL) {
		__set_result_null(pCtx);
	} else {
		__set_result_text(pCtx, zValue, -1, NULL);
	}

	return DBSQL_SUCCESS;
}

/*
 * Return the rowid.
 */
static int
csv_rowid(sqlite3_vtab_cursor *pCursor, i64 *pRowid)
{
	csv_cursor *pCur = (csv_cursor *)pCursor;
	*pRowid = pCur->iRow;
	return DBSQL_SUCCESS;
}

/*
 * Register the CSV module with the database.
 */
int
__dbsql_register_csv_module(DBSQL *db)
{
	return __dbsql_create_module(db, "csv", &csv_module, NULL, NULL);
}
