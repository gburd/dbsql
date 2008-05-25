/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * There are special exceptions to the terms and conditions of the GPL as it
 * is applied to this software. View the full text of the exception in file
 * LICENSE_EXCEPTIONS in the directory of this software distribution.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * $Id: cg_attach.c 7 2007-02-03 13:34:17Z gburd $
 */

/*
 * This file contains C code routines that are called by the parser
 * to handle ATTACH and DETACH statements.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __attach --
 *	This routine is called by the parser to process an ATTACH statement:
 *
 *	     ATTACH DATABASE filename AS dbname
 *
 *	The 'file' and 'db' arguments are the tokens that define the
 *	'file' and 'db' in the ATTACH statement.
 *
 * PUBLIC: void __attach __P((parser_t *, token_t *, token_t *));
 */
void
__attach(parser, file, db)
	parser_t *parser;
	token_t *file;
	token_t *db;
{
	int rc, i;
	dbsql_db_t *new;
	char *filename, *databasename;
	DBSQL *dbp;
	vdbe_t *v;

	v = __parser_get_vdbe(parser);
	__vdbe_add_op(v, OP_Halt, 0, 0);
	if (parser->explain)
		return;
	dbp = parser->db;
	if (dbp->format_version < 1) {
		__error_msg(parser, "cannot attach auxiliary databases to an "
			    "older format master database", 0);
		parser->rc = DBSQL_ERROR;
		return;
	}
	if (dbp->nDb >= (MAX_ATTACHED + 2)) {
		__error_msg(parser, "too many attached databases - max %d", 
			    MAX_ATTACHED);
		parser->rc = DBSQL_ERROR;
		return;
	}

	filename = 0;
	__str_nappend(&filename, file->z, file->n, NULL);
	if (filename == 0)
		return;
	__str_unquote(filename);
#ifndef DBSQL_NO_AUTH
	if (__auth_check(parser, DBSQL_ATTACH, filename, 0, 0) !=
	    DBSQL_SUCCESS) {
		__dbsql_free(dbp, filename);
		return;
	}
#endif /* DBSQL_NO_AUTH */

	databasename = 0;
	__str_nappend(&databasename, db->z, db->n, NULL);
	if (databasename == 0)
		return;
	__str_unquote(databasename);
	for (i = 0; i < dbp->nDb; i++) {
		if (dbp->aDb[i].zName && strcasecmp(
			    dbp->aDb[i].zName, databasename) == 0) {
			__error_msg(parser, "database %z is already in use",
				    databasename);
			parser->rc = DBSQL_ERROR;
			__dbsql_free(dbp, filename);
			return;
		}
	}

	if (__dbsql_realloc(dbp, (sizeof(dbp->aDb[0]) *
				  (dbp->nDb + 1)), &dbp->aDb) == ENOMEM)
		return;
	new = &dbp->aDb[(dbp->nDb++)];
	memset(new, 0, sizeof(*new));
	if (__sqldb_init(new, dbp, filename, 0, 0, 1) != 0) {
		__error_msg(parser, "unable to open database: %s", filename);
	}
	new->zName = databasename;
	__dbsql_free(dbp, filename);
	dbp->flags &= ~DBSQL_Initialized;
	if (parser->nErr)
		return;
	if (rc == DBSQL_SUCCESS) {
		rc = __init_databases(parser->db, &parser->zErrMsg);
	}
	if (rc) {
		i = dbp->nDb - 1;
		DBSQL_ASSERT(i >= 2);
		if (dbp->aDb[i].pBt) {
			__sm_close_db(dbp->aDb[i].pBt);
			dbp->aDb[i].pBt = 0;
		}
		__reset_internal_schema(dbp, 0);
		parser->nErr++;
		parser->rc = DBSQL_ERROR;
	}
}

/*
 * __detach --
 *	This routine is called by the parser to process a DETACH statement:
 *
 *	    DETACH DATABASE db
 *
 *	The db argument is the name of the database in the DETACH statement.
 *
 * PUBLIC: void __detach __P((parser_t *, token_t *));
 */
void
__detach(parser, db)
	parser_t *parser;
	token_t *db;
{
	int i;
	DBSQL *dbp;
	vdbe_t *v = __parser_get_vdbe(parser);
	__vdbe_add_op(v, OP_Halt, 0, 0);
	if (parser->explain)
		return;
	dbp = parser->db;
	for (i = 0; i < dbp->nDb; i++) {
		if (dbp->aDb[i].pBt == 0 ||
		    dbp->aDb[i].zName == 0)
			continue;
		if (strlen(dbp->aDb[i].zName) != db->n)
			continue;
		if (strncasecmp(dbp->aDb[i].zName, db->z, db->n) == 0)
			break;
	}
	if (i >= dbp->nDb) {
		__error_msg(parser, "no such database: %T", db);
		return;
	}
	if (i < 2) {
		__error_msg(parser, "cannot detach database %T", db);
		return;
	}
#ifndef DBSQL_NO_AUTH
	if (__auth_check(parser, DBSQL_DETACH, dbp->aDb[i].zName, 0, 0) !=
	    DBSQL_SUCCESS) {
		return;
	}
#endif /* DBSQL_NO_AUTH */
	__sm_close_db(dbp->aDb[i].pBt);
	dbp->aDb[i].pBt = 0;
	__dbsql_free(dbp, dbp->aDb[i].zName);
	__reset_internal_schema(dbp, i);
	dbp->nDb--;
	if (i < dbp->nDb) {
		dbp->aDb[i] = dbp->aDb[dbp->nDb];
		memset(&dbp->aDb[dbp->nDb], 0, sizeof(dbp->aDb[0]));
		__reset_internal_schema(dbp, i);
	}
}

/*
 * __ref_normalizer_ctx_init --
 *	Initialize a ref_normalizer_ctx_t structure.  This routine must be
 *	called prior to passing the structure to one of the
 *	__ref_normalize_XXX() routines below.  The return value indicates
 *	whether or not normalization is required.  TRUE means we do need to
 *	fix the database references, FALSE means we do not.
 *
 * PUBLIC: int __ref_normalizer_ctx_init __P((ref_normalizer_ctx_t *,
 * PUBLIC:                               parser_t *, int, const char *,
 * PUBLIC:                               const token_t *));
 *
 * normctx			The normalizer to be initialized
 * parser			Error messages will be written here
 * dbnum			This is the database that must must be used
 * type				"view", "trigger", or "index"
 * name				Name of the view, trigger, or index
 */
int
__ref_normalizer_ctx_init(normctx, parser, dbnum, type, name)
	ref_normalizer_ctx_t *normctx;
	parser_t *parser;
	int dbnum;
	const char *type;
	const token_t *name;
{
	DBSQL *dbp;

	if (dbnum < 0 || dbnum == 1)
		return 0;
	dbp = parser->db;
	DBSQL_ASSERT(dbp->nDb > dbnum);
	normctx->pParse = parser;
	normctx->zDb = dbp->aDb[dbnum].zName;
	normctx->zType = type;
	normctx->pName = name;
	return 1;
}

/*
 * __ref_normalize_src_list --
 *	The following set of routines walk through the parse tree and assign
 *	a specific database to all table references where the database name
 *	was left unspecified in the original SQL statement.  The normctx
 *	structure must have been initialized by a prior call to
 *	__ref_normalizer_ctx_init().
 *
 *	These routines are used to make sure that an index, trigger, or
 *	view in one database does not refer to objects in a different database.
 *	(Exception: indices, triggers, and views in the TEMP database are
 *	allowed to refer to anything.)  If a reference is explicitly made
 *	to an object in a different database, an error message is added to
 *	parser->zErrMsg and these routines return non-zero.  If everything
 *	checks out, these routines return 0.
 *
 * PUBLIC: int __ref_normalize_src_list __P((ref_normalizer_ctx_t *,
 * PUBLIC:                              src_list_t *));
 *
 * normctx			Context of the normalization
 * src_list			The source list to check and modify
 */
int __ref_normalize_src_list(normctx, src_list)
	ref_normalizer_ctx_t *normctx;
	src_list_t *src_list;
{
	int i;
	const char *db;
	DBSQL *dbp = normctx->pParse->db;

	if (src_list == 0)
		return 0;
	db = normctx->zDb;
	for (i = 0; i < src_list->nSrc; i++) {
		if (src_list->a[i].zDatabase == 0) {
			__dbsql_strdup(dbp, db, &src_list->a[i].zDatabase);
		} else if (strcasecmp(src_list->a[i].zDatabase, db) != 0) {
			char *name;
			__dbsql_strndup(dbp, normctx->pName->z, &name,
				     normctx->pName->n);
			__error_msg(normctx->pParse, "%s %z cannot reference "
				    "objects in database %s", normctx->zType,
				    name, src_list->a[i].zDatabase);
			return 1;
		}
		if (__ref_normalize_select(normctx, src_list->a[i].pSelect))
			return 1;
		if (__ref_normalize_expr(normctx, src_list->a[i].pOn))
			return 1;
	}
	return 0;
}

/*
 * __ref_normalize_select --
 *
 * PUBLIC: int __ref_normalize_select __P((ref_normalizer_ctx_t *,
 * PUBLIC:                            select_t *));
 *
 * normctx			Context of the normalization
 * select			The SELECT statement to be fixed to one
 *                              database
 */
int
__ref_normalize_select(normctx, select)
	ref_normalizer_ctx_t *normctx;
	select_t *select;
{
	while (select) {
		if (__ref_normalize_expr_list(normctx, select->pEList)) {
			return 1;
		}
		if (__ref_normalize_src_list(normctx, select->pSrc)) {
			return 1;
		}
		if (__ref_normalize_expr(normctx, select->pWhere)) {
			return 1;
		}
		if (__ref_normalize_expr(normctx, select->pHaving)) {
			return 1;
		}
		select = select->pPrior;
	}
	return 0;
}


/*
 * __ref_normalize_expr --
 *
 * PUBLIC: int __ref_normalize_expr __P((ref_normalizer_ctx_t *,
 * PUBLIC:                          expr_t *));
 *
 * normctx			Context of the normalization
 * expr				The expr_t to be fixed to one database
 */
int __ref_normalize_expr(normctx, expr)
	ref_normalizer_ctx_t *normctx;
	expr_t *expr;
{
	while (expr) {
		if (__ref_normalize_select(normctx, expr->pSelect)) {
			return 1;
		}
		if (__ref_normalize_expr_list(normctx, expr->pList)) {
			return 1;
		}
		if (__ref_normalize_expr(normctx, expr->pRight)) {
			return 1;
		}
		expr = expr->pLeft;
	}
	return 0;
}

/*
 * __ref_normalize_expr_list --
 *
 * PUBLIC: int __ref_normalize_expr_list __P((ref_normalizer_ctx_t *,
 * PUBLIC:                               expr_list_t *));
 *
 * normctx			Context of the normalization
 * expr				The expression to be fixed to one database
 */
int
__ref_normalize_expr_list(normctx, list)
	ref_normalizer_ctx_t *normctx;
	expr_list_t *list;
{
	int i;
	if (list == 0)
		return 0;
	for (i = 0; i < list->nExpr; i++) {
		if (__ref_normalize_expr(normctx, list->a[i].pExpr)) {
			return 1;
		}
	}
	return 0;
}

/*
 * __ref_normalize_trigger_step --
 *
 * PUBLIC: int __ref_normalize_trigger_step __P((ref_normalizer_ctx_t *,
 * PUBLIC:                               trigger_step_t *));
 *
 * normctx			Context of the normalization
 * expr				The trigger step to be fixed to one database
 */
int __ref_normalize_trigger_step(normctx, step)
	ref_normalizer_ctx_t *normctx;
	trigger_step_t *step;
{
	while (step) {
		if (__ref_normalize_select(normctx, step->pSelect)) {
			return 1;
		}
		if (__ref_normalize_expr(normctx, step->pWhere)) {
			return 1;
		}
		if (__ref_normalize_expr_list(normctx, step->pExprList)) {
			return 1;
		}
		step = step->pNext;
	}
	return 0;
}
