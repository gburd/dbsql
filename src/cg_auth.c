/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc. - All rights reserved.
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
 */

/*
 * This file contains code used to implement the __api_set_auth_callback()
 * API.  This facility is an optional feature of the library.  Embedded
 * systems that do not need this facility may omit it by reconfiguring
 * with '--disable-auth'.  Then recompile the entire library.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

#ifndef DBSQL_OMIT_AUTHORIZATION

/*
 *	Set or clear the access authorization function.
 *	The access authorization function is be called during the compilation
 *	phase to verify that the user has read and/or write access permission
 *	on various fields of the database.  The first argument to the auth
 *	function is a copy of the 3rd argument to this routine.  The second
 *	argument to the auth function is one of these constants:
 *
 *	      DBSQL_COPY
 *	      DBSQL_CREATE_INDEX
 *	      DBSQL_CREATE_TABLE
 *	      DBSQL_CREATE_TEMP_INDEX
 *	      DBSQL_CREATE_TEMP_TABLE
 *	      DBSQL_CREATE_TEMP_TRIGGER
 *	      DBSQL_CREATE_TEMP_VIEW
 *	      DBSQL_CREATE_TRIGGER
 *	      DBSQL_CREATE_VIEW
 *	      DBSQL_DELETE
 *	      DBSQL_DROP_INDEX
 *	      DBSQL_DROP_TABLE
 *	      DBSQL_DROP_TEMP_INDEX
 *	      DBSQL_DROP_TEMP_TABLE
 *	      DBSQL_DROP_TEMP_TRIGGER
 *	      DBSQL_DROP_TEMP_VIEW
 *	      DBSQL_DROP_TRIGGER
 *	      DBSQL_DROP_VIEW
 *	      DBSQL_INSERT
 *	      DBSQL_PRAGMA
 *	      DBSQL_READ
 *	      DBSQL_SELECT
 *	      DBSQL_TRANSACTION
 *	      DBSQL_UPDATE
 *
 *	The third and fourth arguments to the auth function are the name of
 *	the table and the column that are being accessed.  The auth function
 *	should return either DBSQL_SUCCESS, DBSQL_DENY, or DBSQL_IGNORE.  If
 *	DBSQL_SUCCESS is returned, it means that access is allowed.  DBSQL_DENY
 *	means that the SQL statement will never-run - the dbsql_exec() call
 *	will return with an error.  DBSQL_IGNORE means that the SQL statement
 *	should run but attempts to read the specified column will return NULL
 *	and attempts to write the column will be ignored.
 *
 *	Setting the auth function to NULL disables this hook.  The default
 *	setting of the auth function is NULL.
 *
 * PUBLIC: int __api_set_authorizer __P((DBSQL *,
 * PUBLIC:     int (*auth)(void*,int,const char*, const char*,const char*,
 * PUBLIC:     const char*), void *));
 */
int __api_set_authorizer(dbp, auth, arg)
	DBSQL *dbp;
	int (*auth)(void*,int,const char*,const char*,const char*,const char*);
	void *arg;
{
	dbp->auth = auth;
	dbp->pAuthArg = arg;
	return DBSQL_SUCCESS;
}

/*
 * __auth_bad_return_code --
 *	Write an error message into pParse->zErrMsg that explains that the
 *	user-supplied authorization function returned an illegal value.
 *
 * STATIC: static void __auth_bad_return_code __P((parser_t *, int));
 */
static void
__auth_bad_return_code(parser, rc)
	parser_t *parser;
	int rc;
{
	char buf[20];
	sprintf(buf, "(%d)", rc);
	__str_append(&parser->zErrMsg, "illegal return value ",
		     buf, " from the authorization function - ",
		     "should be DBSQL_SUCCESS, DBSQL_IGNORE, or DBSQL_DENY",
		     (char*)0);
	parser->nErr++;
	parser->rc = DBSQL_MISUSE;
}

/*
 * __auth_read --
 *	The 'expr' should be a TK_COLUMN expression.  The table referred to
 *	is in 'tab_list' or else it is the NEW or OLD table of a trigger.  
 *	Check to see if it is OK to read this particular column.
 *
 *	If the auth function returns DBSQL_IGNORE, change the TK_COLUMN 
 *	instruction into a TK_NULL.  If the auth function returns DBSQL_DENY,
 *	then generate an error.
 *
 * PUBLIC: void __auth_read __P((parser_t *, expr_t *, src_list_t *));
 *
 * parser			The parser context
 * expr				The expression to check authorization on
 * tab_list			All table that expr might refer to
 */
void
__auth_read(parser, expr, tab_list)
	parser_t *parser;
	expr_t *expr;
	src_list_t *tab_list;
{
	int rc;
	DBSQL *dbp = parser->db;
	table_t *table;     /* The table being read */
	const char *col;    /* Name of the column of the table */
	int src;            /* Index in tab_list->a[] of table being read */
	const char *databasename; /* Name of database being accessed */
	trigger_stack_t *stack;   /* The stack of current triggers */

	if (dbp->auth == 0)
		return;
	DBSQL_ASSERT(expr->op == TK_COLUMN);
	for (src = 0; src < tab_list->nSrc; src++) {
		if (expr->iTable == tab_list->a[src].iCursor)
			break;
	}
	if (src >= 0 && src < tab_list->nSrc) {
		table = tab_list->a[src].pTab;
	} else {
		/*
		 * This must be an attempt to read the NEW or OLD pseudo-tables
		 * of a trigger.
		 */
		stack = parser->trigStack;
		DBSQL_ASSERT(stack != 0);
		DBSQL_ASSERT(expr->iTable == stack->newIdx ||
		       expr->iTable == stack->oldIdx);
		table = stack->pTab;
	}
	if (table == 0)
		return;
	if (expr->iColumn >= 0) {
		DBSQL_ASSERT(expr->iColumn < table->nCol);
		col = table->aCol[expr->iColumn].zName;
	} else if (table->iPKey >= 0) {
		DBSQL_ASSERT(table->iPKey < table->nCol);
		col = table->aCol[table->iPKey].zName;
	} else {
		col = "ROWID";
	}
	DBSQL_ASSERT(expr->iDb < dbp->nDb);
	databasename = dbp->aDb[expr->iDb].zName;
	rc = dbp->auth(dbp->pAuthArg, DBSQL_READ, table->zName,
			 col, databasename, parser->zAuthContext);
	if (rc == DBSQL_IGNORE) {
		expr->op = TK_NULL;
	} else if (rc == DBSQL_DENY) {
		if (dbp->nDb > 2 || expr->iDb != 0) {
			__str_append(&parser->zErrMsg,"access to ",
				     databasename, ".", table->zName, ".",
				     col, " is prohibited", (char*)0);
		} else {
			__str_append(&parser->zErrMsg,"access to ",
				     table->zName, ".", col, " is prohibited",
				     (char*)0);
		}
		parser->nErr++;
		parser->rc = DBSQL_AUTH;
	} else if (rc != DBSQL_SUCCESS) {
		__auth_bad_return_code(parser, rc);
	}
}

/*
 * __auth_check --
 *	Do an authorization check using the code and arguments given.  Return
 *	either DBSQL_SUCCESS (zero) or DBSQL_IGNORE or DBSQL_DENY.  If
 *	DBSQL_DENY is returned, then the error count and error message in
 *	parser are modified appropriately.
 *
 * PUBLIC: int __auth_check __P((parser_t *, int, const char *, const char *,
 * PUBLIC:                  const char *));
 */
int
__auth_check(parser, code, arg1, arg2, arg3)
	parser_t *parser;
	int code;
	const char *arg1;
	const char *arg2;
	const char *arg3;
{
	int rc;
	DBSQL *dbp = parser->db;

	if (dbp->auth == 0) {
		return DBSQL_SUCCESS;
	}
	rc = dbp->auth(dbp->pAuthArg, code, arg1, arg2, arg3,
			 parser->zAuthContext);
	if (rc == DBSQL_DENY) {
		__str_append(&parser->zErrMsg, "not authorized",
			     (char*)0);
		parser->rc = DBSQL_AUTH;
		parser->nErr++;
	} else if (rc != DBSQL_SUCCESS && rc != DBSQL_IGNORE) {
		rc = DBSQL_DENY;
		__auth_bad_return_code(parser, rc);
	}
	return rc;
}

/*
 * __auth_context_push --
 *	Push an authorization context.  After this routine is called, the
 *	arg3 argument to authorization callbacks will be context until
 *	popped.  Or if parser==0, this routine is a no-op.
 *
 * PUBLIC: void __auth_context_push __P((parser_t *, auth_context_t *,
 * PUBLIC:                          const char *));
 */
void __auth_context_push(parser, authctx, context)
	parser_t *parser;
	auth_context_t *authctx;
	const char *context;
{
	authctx->pParse = parser;
	if (parser) {
		authctx->zAuthContext = parser->zAuthContext;
		parser->zAuthContext = context;
	}
}

/*
 * __auth_context_pop --
 *	Pop an authorization context that was previously pushed
 *	by __auth_context_push.
 *
 * PUBLIC: void __auth_context_pop __P((auth_context_t *));
 */
void
__auth_context_pop(authctx)
	auth_context_t *authctx;
{
	if (authctx->pParse) {
		authctx->pParse->zAuthContext = authctx->zAuthContext;
		authctx->pParse = 0;
	}
}

#else

/* TODO what is the difference between DBSQL_OMIT_AUTHORIZATION and
   DBSQL_NO_AUTH?  Which one should be used here? Setup a test case for this.*/

# define __auth_check(a,b,c,d,e)    DBSQL_SUCCESS

#endif /* !defined(DBSQL_OMIT_AUTHORIZATION) */
