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
 * Virtual Table Implementation for DBSQL
 *
 * This file implements the virtual table subsystem, allowing external modules
 * to provide table implementations accessed via SQL.
 *
 * Key components:
 *   - Module registration and management
 *   - Virtual table instance lifecycle (create/connect/disconnect/destroy)
 *   - Schema declaration parsing
 *   - Transaction coordination
 *   - Savepoint handling
 *
 * Virtual tables are stored in a hash table in the database connection,
 * keyed by module name. Table instances are tracked in the schema's
 * table hash and linked via vtable_t structures.
 */

#include "inc/dbsql_config.h"
#include "dbsql_int.h"
#include "inc/vtab.h"

/*
 * Register a virtual table module.
 */
int
__dbsql_create_module(
	DBSQL *db,
	const char *zName,
	const sqlite3_module *pModule,
	void *pAux,
	void (*xDestroy)(void *)
)
{
	module_t *pMod;
	module_t *pDel;
	char *zCopy;
	int nName;

	if (db == NULL || zName == NULL || pModule == NULL) {
		return DBSQL_ERROR;
	}

	/* Check module version */
	if (pModule->iVersion < 0 || pModule->iVersion > 3) {
		__str_append(&db->pErr, "unsupported module version", (char *)0);
		return DBSQL_ERROR;
	}

	/* Allocate module structure */
	nName = strlen(zName);
	pMod = __dbsql_calloc(db, 1, sizeof(module_t) + nName + 1);
	if (pMod == NULL) {
		if (xDestroy != NULL) {
			xDestroy(pAux);
		}
		return DBSQL_NOMEM;
	}

	/* Initialize module */
	zCopy = (char *)(&pMod[1]);
	strcpy(zCopy, zName);
	pMod->zName = zCopy;
	pMod->pModule = pModule;
	pMod->pAux = pAux;
	pMod->xDestroy = xDestroy;
	pMod->nRefModule = 1;
	pMod->pEpoTab = NULL;

	/* Add to module hash, replacing any existing module with same name */
	pDel = (module_t *)__hash_insert(&db->aModule, zCopy, pMod);
	if (pDel != NULL) {
		/* Existing module - decrement its reference count */
		if (pDel->xDestroy != NULL) {
			pDel->xDestroy(pDel->pAux);
		}
		__dbsql_free(db, pDel);
	}

	return DBSQL_SUCCESS;
}

/*
 * Helper: Parse a CREATE TABLE statement and extract column definitions.
 * Used by __dbsql_declare_vtab to parse the schema declaration.
 */
static int
vtab_parse_schema(DBSQL *db, const char *zSQL, table_t **ppTab, char **pzErr)
{
	parser_t *pParse;
	int rc;

	/* Create a parser context */
	pParse = __dbsql_calloc(db, 1, sizeof(parser_t));
	if (pParse == NULL) {
		return DBSQL_NOMEM;
	}

	pParse->db = db;
	pParse->declareVtab = 1;  /* Mark as virtual table declaration */

	/* Parse the SQL statement */
	__run_parser(pParse, zSQL, pzErr);
	rc = pParse->rc;

	/* Extract the table structure if successful */
	if (rc == DBSQL_SUCCESS && pParse->pNewTable != NULL) {
		*ppTab = pParse->pNewTable;
		pParse->pNewTable = NULL;  /* Transfer ownership */
	} else {
		rc = DBSQL_ERROR;
	}

	/* Cleanup parser */
	__parse_cleanup(pParse);
	__dbsql_free(db, pParse);

	return rc;
}

/*
 * Declare the schema for a virtual table.
 */
int
__dbsql_declare_vtab(DBSQL *db, const char *zSQL)
{
	table_t *pTab;
	char *zErr = NULL;
	int rc;

	if (db == NULL || zSQL == NULL) {
		return DBSQL_ERROR;
	}

	/* Must be called from within xCreate or xConnect */
	if (db->pVtabCtx == NULL) {
		__str_append(&db->pErr,
		    "dbsql_declare_vtab() must be called from within "
		    "xCreate or xConnect", (char *)0);
		return DBSQL_MISUSE;
	}

	/* Parse the schema */
	rc = vtab_parse_schema(db, zSQL, &pTab, &zErr);
	if (rc != DBSQL_SUCCESS) {
		if (zErr != NULL) {
			__str_append(&db->pErr, zErr, (char *)0);
			__dbsql_free(db, zErr);
		}
		return rc;
	}

	/* Validate the parsed table */
	if (pTab == NULL || pTab->columns == NULL || pTab->nCol == 0) {
		__str_append(&db->pErr,
		    "invalid schema in dbsql_declare_vtab()", (char *)0);
		if (pTab != NULL) {
			__delete_table(db, pTab);
		}
		return DBSQL_ERROR;
	}

	/* Store the schema in the vtab context */
	if (db->pVtabCtx->pTab != NULL) {
		__delete_table(db, db->pVtabCtx->pTab);
	}
	db->pVtabCtx->pTab = pTab;

	return DBSQL_SUCCESS;
}

/*
 * Create or connect to a virtual table instance.
 */
int
__vtab_call_create(DBSQL *db, int iDb, const char *zTab, char **pzErr)
{
	table_t *pTab;
	module_t *pMod;
	vtable_t *pVTab;
	sqlite3_vtab *pVtab;
	const char **azArg;
	int nArg;
	int rc;
	int i;
	char *zModuleName;

	*pzErr = NULL;

	/* Find the table in the schema */
	pTab = __find_table(db, zTab, db->aDb[iDb].zName);
	if (pTab == NULL || !IsVirtual(pTab)) {
		*pzErr = __dbsql_mprintf(db, "table %s is not a virtual table", zTab);
		return DBSQL_ERROR;
	}

	/* Get the module */
	zModuleName = pTab->azModuleArg[0];
	pMod = (module_t *)__hash_find(&db->aModule, zModuleName, strlen(zModuleName));
	if (pMod == NULL) {
		*pzErr = __dbsql_mprintf(db, "no such module: %s", zModuleName);
		return DBSQL_ERROR;
	}

	/* Check if already connected */
	for (pVTab = db->pVTrans; pVTab != NULL; pVTab = pVTab->pNext) {
		if (pVTab->pMod == pMod &&
		    strcmp(pVTab->pMod->zName, zModuleName) == 0) {
			pVTab->nRef++;
			return DBSQL_SUCCESS;
		}
	}

	/* Prepare arguments for xCreate/xConnect */
	nArg = pTab->nModuleArg;
	azArg = (const char **)__dbsql_malloc(db, sizeof(char *) * nArg);
	if (azArg == NULL) {
		return DBSQL_NOMEM;
	}
	for (i = 0; i < nArg; i++) {
		azArg[i] = pTab->azModuleArg[i];
	}

	/* Create vtab context for xCreate/xConnect */
	db->pVtabCtx = __dbsql_malloc(db, sizeof(struct vtab_context));
	if (db->pVtabCtx == NULL) {
		__dbsql_free(db, (void *)azArg);
		return DBSQL_NOMEM;
	}
	db->pVtabCtx->pTab = NULL;

	/* Call xCreate or xConnect */
	pVtab = NULL;
	if (pTab->tabFlags & TF_Virtual) {
		const sqlite3_module *pModule = (const sqlite3_module *)pMod->pModule;
		if (pModule->xConnect != NULL) {
			rc = pModule->xConnect(db, pMod->pAux, nArg, azArg, &pVtab, pzErr);
		} else {
			rc = pModule->xCreate(db, pMod->pAux, nArg, azArg, &pVtab, pzErr);
		}
	} else {
		rc = DBSQL_ERROR;
	}

	/* Cleanup context */
	if (db->pVtabCtx->pTab != NULL) {
		/* Schema was declared - update table structure */
		__delete_table(db, pTab);
		pTab = db->pVtabCtx->pTab;
	}
	__dbsql_free(db, db->pVtabCtx);
	db->pVtabCtx = NULL;
	__dbsql_free(db, (void *)azArg);

	if (rc != DBSQL_SUCCESS) {
		if (pVtab != NULL && pVtab->zErrMsg != NULL) {
			*pzErr = __dbsql_strdup(db, pVtab->zErrMsg);
		}
		return rc;
	}

	/* Create vtable_t wrapper */
	pVTab = __dbsql_calloc(db, 1, sizeof(vtable_t));
	if (pVTab == NULL) {
		if (pVtab != NULL) {
			const sqlite3_module *pModule = (const sqlite3_module *)pMod->pModule;
			if (pModule->xDisconnect != NULL) {
				pModule->xDisconnect(pVtab);
			}
		}
		return DBSQL_NOMEM;
	}

	pVTab->db = db;
	pVTab->pMod = pMod;
	pVTab->pVtab = pVtab;
	pVTab->nRef = 1;
	pVTab->bConstraint = 1;
	pVTab->iSavepoint = 0;

	/* Add to virtual table list */
	pVTab->pNext = db->pVTrans;
	db->pVTrans = pVTab;
	pMod->nRefModule++;

	return DBSQL_SUCCESS;
}

/*
 * Disconnect a virtual table.
 */
int
__vtab_call_disconnect(DBSQL *db, vtable_t *pVTab)
{
	const sqlite3_module *pModule;
	int rc = DBSQL_SUCCESS;

	if (pVTab == NULL) {
		return DBSQL_SUCCESS;
	}

	pVTab->nRef--;
	if (pVTab->nRef > 0) {
		return DBSQL_SUCCESS;
	}

	/* Remove from list */
	if (db->pVTrans == pVTab) {
		db->pVTrans = pVTab->pNext;
	} else {
		vtable_t *p;
		for (p = db->pVTrans; p != NULL && p->pNext != pVTab; p = p->pNext) {
			/* Find predecessor */
		}
		if (p != NULL) {
			p->pNext = pVTab->pNext;
		}
	}

	/* Call xDisconnect */
	pModule = (const sqlite3_module *)pVTab->pMod->pModule;
	if (pModule->xDisconnect != NULL) {
		rc = pModule->xDisconnect((sqlite3_vtab *)pVTab->pVtab);
	}

	/* Decrement module reference */
	pVTab->pMod->nRefModule--;
	if (pVTab->pMod->nRefModule == 0) {
		if (pVTab->pMod->xDestroy != NULL) {
			pVTab->pMod->xDestroy(pVTab->pMod->pAux);
		}
		__hash_remove(&db->aModule, pVTab->pMod->zName);
		__dbsql_free(db, pVTab->pMod);
	}

	__dbsql_free(db, pVTab);
	return rc;
}

/*
 * Destroy a virtual table.
 */
int
__vtab_call_destroy(DBSQL *db, int iDb, const char *zTab)
{
	table_t *pTab;
	module_t *pMod;
	const sqlite3_module *pModule;
	sqlite3_vtab *pVtab;
	const char **azArg;
	int nArg;
	int rc;
	int i;
	char *zErr = NULL;
	char *zModuleName;

	/* Find the table */
	pTab = __find_table(db, zTab, db->aDb[iDb].zName);
	if (pTab == NULL || !IsVirtual(pTab)) {
		return DBSQL_ERROR;
	}

	/* Get the module */
	zModuleName = pTab->azModuleArg[0];
	pMod = (module_t *)__hash_find(&db->aModule, zModuleName, strlen(zModuleName));
	if (pMod == NULL) {
		return DBSQL_ERROR;
	}

	pModule = (const sqlite3_module *)pMod->pModule;

	/* Prepare arguments */
	nArg = pTab->nModuleArg;
	azArg = (const char **)__dbsql_malloc(db, sizeof(char *) * nArg);
	if (azArg == NULL) {
		return DBSQL_NOMEM;
	}
	for (i = 0; i < nArg; i++) {
		azArg[i] = pTab->azModuleArg[i];
	}

	/* Call xDestroy if available, otherwise xDisconnect */
	if (pModule->xDestroy != NULL) {
		rc = pModule->xDestroy((sqlite3_vtab *)pTab->pVtab);
	} else if (pModule->xDisconnect != NULL) {
		rc = pModule->xDisconnect((sqlite3_vtab *)pTab->pVtab);
	} else {
		rc = DBSQL_SUCCESS;
	}

	__dbsql_free(db, (void *)azArg);

	return rc;
}

/*
 * Begin using a virtual table.
 */
int
__vtab_begin(DBSQL *db, vtable_t *pVTab)
{
	const sqlite3_module *pModule;

	if (pVTab == NULL || pVTab->pVtab == NULL) {
		return DBSQL_ERROR;
	}

	pModule = (const sqlite3_module *)pVTab->pMod->pModule;

	/* Call xBegin if available */
	if (pModule->iVersion >= 2 && pModule->xBegin != NULL) {
		return pModule->xBegin((sqlite3_vtab *)pVTab->pVtab);
	}

	return DBSQL_SUCCESS;
}

/*
 * Call savepoint methods on all virtual tables.
 */
int
__vtab_savepoint(DBSQL *db, int op, int iSavepoint)
{
	vtable_t *pVTab;
	int rc = DBSQL_SUCCESS;

	for (pVTab = db->pVTrans; pVTab != NULL; pVTab = pVTab->pNext) {
		const sqlite3_module *pModule = (const sqlite3_module *)pVTab->pMod->pModule;
		sqlite3_vtab *pVtab = (sqlite3_vtab *)pVTab->pVtab;

		if (pModule->iVersion < 2) {
			continue;
		}

		switch (op) {
		case SAVEPOINT_BEGIN:
			if (pModule->xSavepoint != NULL) {
				rc = pModule->xSavepoint(pVtab, iSavepoint);
			}
			break;
		case SAVEPOINT_RELEASE:
			if (pModule->xRelease != NULL) {
				rc = pModule->xRelease(pVtab, iSavepoint);
			}
			break;
		case SAVEPOINT_ROLLBACK:
			if (pModule->xRollbackTo != NULL) {
				rc = pModule->xRollbackTo(pVtab, iSavepoint);
			}
			break;
		}

		if (rc != DBSQL_SUCCESS) {
			break;
		}
	}

	return rc;
}

/*
 * Sync all virtual tables.
 */
int
__vtab_sync(DBSQL *db, int rc)
{
	vtable_t *pVTab;

	if (rc != DBSQL_SUCCESS) {
		return rc;
	}

	for (pVTab = db->pVTrans; pVTab != NULL; pVTab = pVTab->pNext) {
		const sqlite3_module *pModule = (const sqlite3_module *)pVTab->pMod->pModule;
		sqlite3_vtab *pVtab = (sqlite3_vtab *)pVTab->pVtab;

		if (pModule->xSync != NULL) {
			rc = pModule->xSync(pVtab);
			if (rc != DBSQL_SUCCESS) {
				return rc;
			}
		}
	}

	return DBSQL_SUCCESS;
}

/*
 * Commit all virtual tables.
 */
int
__vtab_commit(DBSQL *db)
{
	vtable_t *pVTab;
	int rc = DBSQL_SUCCESS;

	for (pVTab = db->pVTrans; pVTab != NULL; pVTab = pVTab->pNext) {
		const sqlite3_module *pModule = (const sqlite3_module *)pVTab->pMod->pModule;
		sqlite3_vtab *pVtab = (sqlite3_vtab *)pVTab->pVtab;

		if (pModule->xCommit != NULL) {
			rc = pModule->xCommit(pVtab);
			if (rc != DBSQL_SUCCESS) {
				return rc;
			}
		}
	}

	return DBSQL_SUCCESS;
}

/*
 * Rollback all virtual tables.
 */
int
__vtab_rollback(DBSQL *db)
{
	vtable_t *pVTab;
	int rc = DBSQL_SUCCESS;

	for (pVTab = db->pVTrans; pVTab != NULL; pVTab = pVTab->pNext) {
		const sqlite3_module *pModule = (const sqlite3_module *)pVTab->pMod->pModule;
		sqlite3_vtab *pVtab = (sqlite3_vtab *)pVTab->pVtab;

		if (pModule->xRollback != NULL) {
			int rc2 = pModule->xRollback(pVtab);
			if (rc == DBSQL_SUCCESS) {
				rc = rc2;
			}
		}
	}

	return rc;
}
