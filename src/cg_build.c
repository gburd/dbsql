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
 * This file contains C code routines that are called by the SQLite parser
 * when syntax rules are reduced.  The routines in this file handle the
 * following kinds of SQL syntax:
 *
 *     CREATE TABLE
 *     DROP TABLE
 *     CREATE INDEX
 *     DROP INDEX
 *     creating ID lists
 *     BEGIN TRANSACTION
 *     COMMIT
 *     ROLLBACK
 *     PRAGMA
 *
 */

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#endif

#include "dbsql_int.h"


/*
 * __parse_begin --
 *	This routine is called when a new SQL statement is beginning to
 *	be parsed.  Check to see if the schema for the database needs
 *	to be read from the DBSQL_MASTER and DBSQL_TEMP_MASTER tables.
 *	If it does, then read it.
 *
 * PUBLIC: void __parse_begin __P((parser_t *, int));
 */
void
__parse_begin(parser, explain_flag)
	parser_t *parser;
	int explain_flag;
{
	DBSQL *dbp = parser->db;
	int i;
	parser->explain = explain_flag;
	if ((dbp->flags & DBSQL_Initialized) == 0 && parser->initFlag == 0) {
		int rc = __init_databases(dbp, &parser->zErrMsg);
		if (rc != DBSQL_SUCCESS) {
			parser->rc = rc;
			parser->nErr++;
		}
	}
	for (i = 0; i < dbp->nDb; i++) {
		DB_PROPERTY_CLEAR(dbp, i, DBSQL_SCHEMA_LOCKED);
		if (!dbp->aDb[i].inTrans ) {
			DB_PROPERTY_CLEAR(dbp, i, DBSQL_COOKIE);
		}
	}
	parser->nVar = 0;
}

/*
 * __null_callback --
 *	This is a fake callback procedure used when dbsql_exec() is
 *	invoked with a NULL callback pointer.  If we pass a NULL callback
 *	pointer into __vdbe_exec() it will return at every OP_Callback,
 *	which we do not want it to do.  So we substitute a pointer to this
 *	procedure in place of the NULL.
 *
 * STATIC: static int __null_callback __P((void *, int , char **, char **));
 */
static int
__null_callback(not_used, n, a, b)
	void *not_used;
	int n;
	char **a;
	char **b;
{
	return 0;
}

/*
 * __parse_exec --
 *	This routine is called after a single SQL statement has been
 *	parsed and we want to execute the VDBE code to implement 
 *	that statement.  Prior action routines should have already
 *	constructed VDBE code to do the work of the SQL statement.
 *	This routine just has to execute the VDBE code.
 *	Note that if an error occurred, it might be the case that
 *	no VDBE code was generated.
 *
 * PUBLIC: void __parse_exec __P((parser_t *));
 */
void
__parse_exec(parser)
	parser_t *parser;
{
	int rc = DBSQL_SUCCESS;
	DBSQL *dbp = parser->db;
	vdbe_t *v = parser->pVdbe;
	int (*callback)(void*,int,char**,char**);

	if (parser->rc == ENOMEM)
		return;
	callback = parser->xCallback;
	if (callback == 0 && parser->useCallback)
		callback = __null_callback;
	if (v && parser->nErr==0) {
		FILE *trace;
		if ((dbp->flags & DBSQL_VdbeTrace) != 0) {
			trace = stdout;
		} else {
			trace = 0;
		}
		__vdbe_trace(v, trace);
		__vdbe_make_ready(v, parser->nVar, callback, parser->pArg,
				  parser->explain);
		if (parser->useCallback) {
			if (parser->explain) {
				rc = __vdbe_list(v);
				dbp->next_sig = dbp->aDb[0].schema_sig;
			} else {
				__vdbe_exec(v);
			}
			rc = __vdbe_finalize(v, &parser->zErrMsg);
			if (rc)
				parser->nErr++;
			parser->pVdbe = 0;
			parser->rc = rc;
			if (rc)
				parser->nErr++;
		} else {
			parser->rc = parser->nErr ? DBSQL_ERROR : DBSQL_DONE;
		}
		parser->colNamesSet = 0;
	} else if (parser->useCallback == 0) {
		parser->rc = DBSQL_ERROR;
	}
	parser->nTab = 0;
	parser->nMem = 0;
	parser->nSet = 0;
	parser->nAgg = 0;
	parser->nVar = 0;
}

/*
 * __find_table --
 *	Locate the in-memory structure that describes 
 *	a particular database table given the name
 *	of that table and (optionally) the name of the database
 *	containing the table.  Return NULL if not found.
 *	If 'database' is 0, all databases are searched for the
 *	table and the first matching table is returned.  (No checking
 *	for duplicate table names is done.)  The search order is
 *	TEMP first, then MAIN, then any auxiliary databases added
 *	using the ATTACH command.
 *	NOTE: See also __locate_table().
 *
 * PUBLIC: table_t *__find_table __P((DBSQL *, const char *,
 * PUBLIC:                       const char *));
 */
table_t *
__find_table(dbp, name, database)
	DBSQL *dbp;
	const char *name;
	const char *database;
{
	table_t *p = 0;
	int i;
	for (i = 0; i < dbp->nDb; i++) {
		int j = (i < 2) ? i^1 : i; /* Search TEMP before MAIN */
		if (database != 0 &&
		    strcasecmp(database, dbp->aDb[j].zName))
			continue;
		p = __hash_find(&dbp->aDb[j].tblHash, name, strlen(name) + 1);
		if (p)
			break;
	}
	return p;
}

/*
 * __locate_table --
 *	Locate the in-memory structure that describes 
 *	a particular database table given the name
 *	of that table and (optionally) the name of the database
 *	containing the table.  Return NULL if not found.
 *	Also leave an error message in parser->err_msgs.
 *	The difference between this routine and __find_table()
 *	is that this routine leaves an error message in parser->err_msgs
 *	where __find_table() does not.
 *
 * PUBLIC: table_t *__locate_table __P((parser_t *, const char *,
 * PUBLIC:                         const char *));
 */
table_t *
__locate_table(parser, name, database)
	parser_t *parser;
	const char *name;
	const char *database;
{
	table_t *p;

	p = __find_table(parser->db, name, database);
	if (p == 0) {
		if (database) {
			__error_msg(parser, "no such table: %s.%s",
				    database, name);
		} else if (__find_table(parser->db, name, 0) != 0) {
			__error_msg(parser, "table \"%s\" is not in "
				    "database \"%s\"", name, database);
		} else {
			__error_msg(parser, "no such table: %s", name);
		}
	}
	return p;
}

/*
 * __find_index --
 *	Locate the in-memory structure that describes 
 *	a particular index given the name of that index
 *	and the name of the database that contains the index.
 *	Return NULL if not found.
 *	If 'database' is 0, all databases are searched for the
 *	table and the first matching index is returned.  (No checking
 *	for duplicate index names is done.)  The search order is
 *	TEMP first, then MAIN, then any auxiliary databases added
 *	using the ATTACH command.
 *
 * PUBLIC: index_t *__find_index __P((DBSQL *, const char *,
 * PUBLIC:                       const char *));
 */
index_t *
__find_index(dbp, name, database)
	DBSQL *dbp;
	const char *name;
	const char *database;
{
	index_t *p = 0;
	int i;
	for (i = 0; i < dbp->nDb; i++) {
		int j = (i < 2) ? i^1 : i; /* Search TEMP before MAIN */
		if (database &&
		    strcasecmp(database, dbp->aDb[j].zName))
			continue;
		p = __hash_find(&dbp->aDb[j].idxHash, name, strlen(name)+1);
		if (p)
			break;
	}
	return p;
}

/*
 * __delete_index
 *	Remove the given index from the index hash table, and __dbsql_free
 *	its memory structures.
 *	The index is removed from the database hash tables but
 *	it is not unlinked from the table_t that it indexes.
 *	Unlinking from the table_t must be done by the calling function.
 *
 * STATIC: static void __delete_index __P((DBSQL *, index_t *));
 */
static void
__delete_index(dbp, index)
	DBSQL *dbp;
	index_t *index;
{
	index_t *old;

	DBSQL_ASSERT(dbp != 0 && index->zName != 0);
	old = __hash_insert(&dbp->aDb[index->iDb].idxHash, index->zName,
			    strlen(index->zName) + 1, 0);
	if (old != 0 && old != index) {
		__hash_insert(&dbp->aDb[index->iDb].idxHash, old->zName,
			      strlen(old->zName) + 1, old);
	}
	__dbsql_free(dbp, index);
}

/*
 * __unlink_and_delete_index --
 *	Unlink the given index from its table, then remove
 *	the index from the index hash table and __dbsql_free its memory
 *	structures.
 *
 * PUBLIC: void __unlink_and_delete_index __P((DBSQL *, index_t *));
 */
void
__unlink_and_delete_index(dbp, index)
	DBSQL *dbp;
	index_t *index;
{
	if (index->pTable->pIndex == index) {
		index->pTable->pIndex = index->pNext;
	} else {
		index_t *i = index->pTable->pIndex;
		while (i && i->pNext != index) {
			index = index->pNext;
		}
		if (i && i->pNext == index) {
			i->pNext = index->pNext;
		}
	}
	__delete_index(dbp, index);
}

/*
 * __reset_internal_schema --
 *	Erase all schema information from the in-memory hash tables of
 *	database connection.  This routine is called to reclaim memory
 *	before the connection closes.  It is also called during a rollback
 *	if there were schema changes during the transaction.
 *	If idb <= 0 then reset the internal schema tables for all database
 *	files.  If idb > =2 then reset the internal schema for only the
 *	single file indicated.
 *
 * PUBLIC: void __reset_internal_schema __P((DBSQL *, int));
 */
void
__reset_internal_schema(dbp, idb)
	DBSQL *dbp;
	int idb;
{
	hash_ele_t *ele;
	hash_t temp1;
	hash_t temp2;
	int i, j;

	DBSQL_ASSERT(idb >= 0 && idb < dbp->nDb);
	dbp->flags &= ~DBSQL_Initialized;
	for (i = idb; i < dbp->nDb; i++) {
		dbsql_db_t *d = &dbp->aDb[i];
		temp1 = d->tblHash;
		temp2 = d->trigHash;
		__hash_init(&d->trigHash, DBSQL_HASH_STRING, 0);
		__hash_clear(&d->aFKey);
		__hash_clear(&d->idxHash);
		for (ele = __hash_first(&temp2); ele; ele = __hash_next(ele)) {
			trigger_t *trigger = __hash_data(ele);
			__vdbe_delete_trigger(trigger);
		}
		__hash_clear(&temp2);
		__hash_init(&d->tblHash, DBSQL_HASH_STRING, 0);
		for (ele = __hash_first(&temp1); ele; ele = __hash_next(ele)) {
			table_t *t = __hash_data(ele);
			__vdbe_delete_table(dbp, t);
		}
		__hash_clear(&temp1);
		DB_PROPERTY_CLEAR(dbp, i, DBSQL_SCHEMA_LOADED);
		if (idb > 0)
			return;
	}
	DBSQL_ASSERT(idb == 0);
	dbp->flags &= ~DBSQL_InternChanges;

	/*
	 * If one or more of the auxiliary database files has been
	 * closed, then remove then from the auxiliary database
	 * list.  We take the opportunity to do this here since we
	 * have just deleted all of the schema hash tables and
	 * therefore do not have to make any changes to any of those
	 * tables.
	 */
	for (i = j = 2; i < dbp->nDb; i++) {
		if (dbp->aDb[i].pBt == 0) {
			__dbsql_free(dbp, dbp->aDb[i].zName);
			dbp->aDb[i].zName = 0;
			continue;
		}
		if (j < i) {
			dbp->aDb[j] = dbp->aDb[i];
		}
		j++;
	}
	memset(&dbp->aDb[j], 0, ((dbp->nDb - j) * sizeof(dbp->aDb[j])));
	dbp->nDb = j;
}

/*
 * __rollback_internal_changes --
 *	This routine is called whenever a rollback occurs.  If there were
 *	schema changes during the transaction, then we have to reset the
 *	internal hash tables and reload them from disk.
 *
 * PUBLIC: void __rollback_internal_changes __P((DBSQL *));
 */
void
__rollback_internal_changes(dbp)
	DBSQL *dbp;
{
	if (dbp->flags & DBSQL_InternChanges) {
		__reset_internal_schema(dbp, 0);
	}
}

/*
 * __commit_internal_changes --
 *	This routine is called when a commit occurs.
 *
 * PUBLIC: void __commit_internal_changes __P((DBSQL *));
 */
void
__commit_internal_changes(dbp)
	DBSQL *dbp;
{
	dbp->aDb[0].schema_sig = dbp->next_sig;
	dbp->flags &= ~DBSQL_InternChanges;
}

/*
 * __vdbe_delete_table --
 *	Remove the memory data structures associated with the given
 *	table_t.  No changes are made to disk by this routine.
 *	This routine just deletes the data structure.  It does not unlink
 *	the table data structure from the hash table.  Nor does it remove
 *	foreign keys from the aFKey hash table.  But it does destroy
 *	memory structures of the indices and foreign keys associated with 
 *	the table.
 *	Indices associated with the table are unlinked from the "db"
 *	data structure if db!=NULL.  If db==NULL, indices attached to
 *	the table are deleted, but it is assumed they have already been
 *	unlinked.
 *
 * PUBLIC: void __vdbe_delete_table __P((DBSQL *, table_t *));
 */
void
__vdbe_delete_table(dbp, table)
	DBSQL *dbp;
	table_t *table;
{
	int i;
	index_t *index, *next;
	foreign_key_t *fkey, *next_fkey;

	if (table == 0)
		return;

	/*
	 * Delete all indices associated with this table.
	 */
	for (index = table->pIndex; index; index = next) {
		next = index->pNext;
		DBSQL_ASSERT(index->iDb == table->iDb ||
		       (table->iDb == 0 && index->iDb == 1));
		__delete_index(dbp, index);
	}

	/*
	 * Delete all foreign keys associated with this table.  The keys
	 * should have already been unlinked from the dbp->aFKey hash table 
	 */
	for (fkey = table->pFKey; fkey; fkey = next_fkey) {
		next_fkey = fkey->pNextFrom;
		DBSQL_ASSERT(table->iDb < dbp->nDb);
		DBSQL_ASSERT(__hash_find(&dbp->aDb[table->iDb].aFKey,
				   fkey->zTo, strlen(fkey->zTo) + 1) != fkey);
		__dbsql_free(dbp, fkey);
	}

	/*
	 * Delete the table_t structure itself.
	 */
	for (i = 0; i < table->nCol; i++) {
		__dbsql_free(dbp, table->aCol[i].zName);
		__dbsql_free(dbp, table->aCol[i].zDflt);
		__dbsql_free(dbp, table->aCol[i].zType);
	}
	__dbsql_free(dbp, table->zName);
	__dbsql_free(dbp, table->aCol);
	__select_delete(table->pSelect);
	__dbsql_free(dbp, table);
}

/*
 * __unlink_and_delete_table --
 *	Unlink the given table from the hash tables and the delete the
 *	table structure with all its indices and foreign keys.
 *
 * STATIC: static void __unlink_and_delete_table __P((DBSQL, table_t *));
 */
static void
__unlink_and_delete_table(dbp, table)
	DBSQL *dbp;
	table_t *table;
{
	table_t *old;
	foreign_key_t *f1, *f2;
	int i = table->iDb;
	DBSQL_ASSERT(dbp != 0);
	old = __hash_insert(&dbp->aDb[i].tblHash, table->zName,
			    strlen(table->zName) + 1, 0);
	DBSQL_ASSERT(old == 0 || old == table);
	for (f1 = table->pFKey; f1; f1 = f1->pNextFrom) {
		int n_to = strlen(f1->zTo) + 1;
		f2 = __hash_find(&dbp->aDb[i].aFKey, f1->zTo, n_to);
		if (f2 == f1) {
			__hash_insert(&dbp->aDb[i].aFKey, f1->zTo, n_to,
				      f1->pNextTo);
		} else {
			while (f2 && f2->pNextTo != f1) {
				f2 = f2->pNextTo;
			}
			if (f2) {
				f2->pNextTo = f1->pNextTo;
			}
		}
	}
	__vdbe_delete_table(dbp, table);
}

/*
 * __table_name_from_token --
 *	Construct the name of a user table or index from a token.
 *	Space to hold the name is obtained from __dbsql_calloc() and must
 *	be freed by the calling function.
 *
 * PUBLIC: char *__table_name_from_token __P((token_t *));
 */
char *
__table_name_from_token(name)
	token_t *name;
{
	char *n;
	__dbsql_strndup(NULL, name->z, &n, name->n);
	__str_unquote(n);
	return n;
}

/*
 * __open_master_table --
 *	Generate code to open the appropriate master table.  The table
 *	opened will be DBSQL_MASTER for persistent tables and 
 *	DBSQL_TEMP_MASTER for temporary tables.  The table is opened
 *	on cursor 0.
 *
 * PUBLIC: void __open_master_table __P((vdbe_t *, int));
 */
void
__open_master_table(vdbe_t *v, int isTemp){
	__vdbe_add_op(v, OP_Integer, isTemp, 0);
	__vdbe_add_op(v, OP_OpenWrite, 0, 2);
}

/*
 * __start_table --
 *	Begin constructing a new table representation in memory.  This is
 *	the first of several action routines that get called in response
 *	to a CREATE TABLE statement.  In particular, this routine is called
 *	after seeing tokens "CREATE" and "TABLE" and the table name.  The
 *	'start' token is the CREATE and 'name' is the table name.  The 'temp'
 *	flag is true if the table should be stored in the auxiliary database
 *	instead of in the main database.  This is normally the case
 *	when the "TEMP" or "TEMPORARY" keyword occurs in between
 *	CREATE and TABLE.
 *	The new table record is initialized and put in parser->pNewTable.
 *	As more of the CREATE TABLE statement is parsed, additional action
 *	routines will be called to add more information to this record.
 *	At the end of the CREATE TABLE statement, the
 *	__ending_create_table_paren() routine is called to complete the
 *	construction of the new table record.
 *
 * PUBLIC: void __start_table __P((parser_t *, token_t *, token_t *,
 * PUBLIC:                    int, int));
 *
 * parser			Parser context
 * start			The "CREATE" token
 * name				Name of table or view to create
 * temp				True if this is a TEMP table
 * view				True if this is a VIEW
 */
void __start_table(parser, start, name, temp, view)
	parser_t *parser;
	token_t *start;
	token_t *name;
	int temp;
	int view;
{
	table_t *table;
	index_t *idx;
	char *n;
	DBSQL *dbp = parser->db;
	vdbe_t *v;
	int idb;

	parser->sFirstToken = *start;
	n = __table_name_from_token(name);
	if (n == 0)
		return;
	if (parser->iDb == 1)
		temp = 1;
#ifndef DBSQL_NO_AUTH
	DBSQL_ASSERT((temp & 1) == temp);
	{
		int code;
		char *db_name = temp ? "temp" : "main";
		if (__auth_check(parser, DBSQL_INSERT, SCHEMA_TABLE(temp), 0,
				 db_name)) {
			__dbsql_free(dbp, n);
			return;
		}
		if (view) {
			if (temp) {
				code = DBSQL_CREATE_TEMP_VIEW;
			} else {
				code = DBSQL_CREATE_VIEW;
			}
		} else {
			if (temp) {
				code = DBSQL_CREATE_TEMP_TABLE;
			} else {
				code = DBSQL_CREATE_TABLE;
			}
		}
		if (__auth_check(parser, code, n, 0, db_name)) {
			__dbsql_free(dbp, n);
			return;
		}
	}
#endif
 

	/* 
	 * Before trying to create a temporary table, make sure the DB for
	 * holding temporary tables is open.
	 */
	if (temp && dbp->aDb[1].pBt == 0 && !parser->explain) {
		int rc = __sm_create(dbp, dbp->aDb[0].zName, 1,
				     (F_ISSET(dbp, DBSQL_DurableTemp) == 0),
				     &dbp->aDb[1].pBt);
		if (rc != DBSQL_SUCCESS) {
			__str_append(&parser->zErrMsg,
				     "unable to open a temporary database "
				     "file for storing temporary tables",
				     (char*)0);
			parser->nErr++;
			return;
		}
		if (dbp->flags & DBSQL_InTrans) { /* TODO */
			rc = __sm_begin_txn(dbp->aDb[1].pBt);
			if (rc != DBSQL_SUCCESS) {
				__str_nappend(&parser->zErrMsg,
					      "unable to get a write lock on "
					      "the temporary database file",
					      NULL);
				parser->nErr++;
				return;
			}
		}
	}

	/*
	 * Make sure the new table name does not collide with an existing
	 * index or table name.  Issue an error message if it does.
	 *
	 * If we are re-reading the master table because of a schema
	 * change and a new permanent table is found whose name collides with
	 * an existing temporary table, that is not an error.
	 */
	table = __find_table(dbp, n, 0);
	idb = temp ? 1 : parser->iDb;
	if (table != 0 && (table->iDb == idb || !parser->initFlag)) {
		__str_nappend(&parser->zErrMsg, "table ", 0, name->z,
			      name->n, " already exists", 0, NULL);
		__dbsql_free(dbp, n);
		parser->nErr++;
		return;
	}
	if ((idx = __find_index(dbp, n, 0)) != 0 &&
	    (idx->iDb == 0 || !parser->initFlag)) {
		__str_append(&parser->zErrMsg,
			     "there is already an index named ", 
			     n, (char*)0);
		__dbsql_free(dbp, n);
		parser->nErr++;
		return;
	}
	if (__dbsql_calloc(dbp, 1, sizeof(table_t), &table) == ENOMEM) {
		__dbsql_free(dbp, n);
		return;
	}
	table->zName = n;
	table->nCol = 0;
	table->aCol = 0;
	table->iPKey = -1;
	table->pIndex = 0;
	table->iDb = idb;
	if (parser->pNewTable)
		__vdbe_delete_table(dbp, parser->pNewTable);
	parser->pNewTable = table;

	/*
	 * Begin generating the code that will insert the table record into
	 * the DBSQL_MASTER table.  Note in particular that we must go ahead
	 * and allocate the record number for the table entry now.  Before any
	 * PRIMARY KEY or UNIQUE keywords are parsed.  Those keywords will
	 * cause indices to be created and the table record must come before
	 * the indices.  Hence, the record number for the table must be
	 * allocated now.
	 */
	if (!parser->initFlag && (v = __parser_get_vdbe(parser)) != 0) {
		__vdbe_prepare_write(parser, 0, temp);
		if (!temp) {
			__vdbe_add_op(v, OP_Integer, dbp->format_version, 0);
			__vdbe_add_op(v, OP_SetFormatVersion,
				      DBSQL_FORMAT_VERSION, 0);
		}
		__open_master_table(v, temp);
		__vdbe_add_op(v, OP_NewRecno, 0, 0);
		__vdbe_add_op(v, OP_Dup, 0, 0);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_add_op(v, OP_PutIntKey, 0, 0);
	}
}

/*
 * __add_column --
 *	Add a new column to the table currently being constructed.
 *	The parser calls this routine once for each column declaration
 *	in a CREATE TABLE statement.  __start_table() gets called
 *	first to get things going.  Then this routine is called for each
 *	column.
 *
 * PUBLIC: void __add_column __P((parser_t *, token_t *));
 */
void
__add_column(parser, name)
	parser_t *parser;
	token_t *name;
{
	table_t *table;
	int i;
	char *n = 0;
	column_t *col;
	DBSQL *dbp = parser->db;

	if ((table = parser->pNewTable) == 0)
		return;
	__str_nappend(&n, name->z, name->n, NULL);
	if (n == 0)
		return;
	__str_unquote(n);
	for (i = 0; i < table->nCol; i++) {
		if (strcasecmp(n, table->aCol[i].zName) == 0) {
			__str_append(&parser->zErrMsg,
				     "duplicate column name: ", n, (char*)0);
			parser->nErr++;
			__dbsql_free(dbp, n);
			return;
		}
	}
	if ((table->nCol & 0x7) == 0) { /* TODO ?? 0x7 ?? */
		if (__dbsql_realloc(dbp,
				 (table->nCol + 8) * sizeof(table->aCol[0]),
				 &table->aCol) == ENOMEM)
			return;
	}
	col = &table->aCol[table->nCol];
	memset(col, 0, sizeof(table->aCol[0]));
	col->zName = n;
	col->sortOrder = DBSQL_SO_NUM;
	table->nCol++;
}

/* __add_not_null --
 *	This routine is called by the parser while in the middle of
 *	parsing a CREATE TABLE statement.  A "NOT NULL" constraint has
 *	been seen on a column.  This routine sets the notNull flag on
 *	the column currently under construction.
 *
 * PUBLIC: void __add_not_null __P((parser_t *, int));
 */
void
__add_not_null(parser, on_error)
	parser_t *parser;
	int on_error;
{
	table_t *table;
	int i;
	if ((table = parser->pNewTable) == 0)
		return;
	i = table->nCol - 1;
	if (i >= 0)
		table->aCol[i].notNull = on_error;
}

/*
 * __add_colum_type --
 *	This routine is called by the parser while in the middle of
 *	parsing a CREATE TABLE statement.  The 'first' token is the first
 *	token in the sequence of tokens that describe the type of the
 *	column currently under construction.   pLast is the last token
 *	in the sequence.  Use this information to construct a string
 *	that contains the typename of the column and store that string
 *	in 'type'.
 *
 * PUBLIC: void __add_column_type __P((parser_t *, token_t *, token_t *));
 */ 
void
__add_column_type(parser, first, last)
	parser_t *parser;
	token_t *first;
	token_t *last;
{
	table_t *table;
	int i, j;
	int n;
	char *z, **pz;
	column_t *col;

	if ((table = parser->pNewTable) == 0)
		return;
	i = table->nCol - 1;
	if (i < 0)
		return;
	col = &table->aCol[i];
	pz = &col->zType;
	n = last->n + P_TO_UINT32(last->z) - P_TO_UINT32(first->z);
	__str_nappend(pz, first->z, n, NULL);
	z = *pz;
	if (z == 0)
		return;
	for(i = j = 0; z[i]; i++) {
		int c = z[i];
		if (isspace(c))
			continue;
		z[j++] = c;
	}
	z[j] = 0;
	col->sortOrder = __collate_type(z, n);
}

/*
 * __add_default_value --
 *	The given token is the default value for the last column added to
 *	the table currently under construction.  If 'minus' is true, it
 *	means the value token was preceded by a minus sign.
 *	This routine is called by the parser while in the middle of
 *	parsing a CREATE TABLE statement.
 *
 * PUBLIC: void __add_default_value __P((parser_t *, token_t *, int));
 */
void
__add_default_value(parser, val, minus)
	parser_t *parser;
	token_t *val;
	int minus;
{
	table_t *table;
	int i;
	char **pz;
	if ((table = parser->pNewTable) == 0)
		return;
	i = table->nCol - 1;
	if (i < 0)
		return;
	pz = &table->aCol[i].zDflt;
	if (minus) {
		__str_nappend(pz, "-", 1, val->z, val->n, NULL);
	} else {
		__str_nappend(pz, val->z, val->n, NULL);
	}
	__str_unquote(*pz);
}

/*
 * __add_primary_key --
 *
 *	Designate the PRIMARY KEY for the table.  'list' is a list of names 
 *	of columns that form the primary key.  If 'list' is NULL, then the
 *	most recently added column of the table is the primary key.
 *	A table can have at most one primary key.  If the table already has
 *	a primary key (and this is the second primary key) then create an
 *	error.
 *	If the PRIMARY KEY is on a single column whose datatype is INTEGER,
 *	then we will try to use that column as the row id.
 *	Set the table_t.iPKey field of the table under construction to be
 *	the index of the INTEGER PRIMARY KEY column.  table_t.iPKey is set
 *	to -1 if there is no INTEGER PRIMARY KEY.
 *	If the key is not an INTEGER PRIMARY KEY, then create a unique
 *	index for the key.  No index is created for INTEGER PRIMARY KEYs.
 * TODO REMOVE THIS Exception:
 * For backwards compatibility with older databases, do not do this
 * if the file format version number is less than 1.
 *
 * PUBLIC: void __add_primary_key __P((parser_t *, id_list_t *, int));
 */
void
__add_primary_key(parser, list, on_error)
	parser_t *parser;
	id_list_t *list;
	int on_error;
{
	table_t *table = parser->pNewTable;
	char *type = 0;
	int col = -1, i;

	if (table == 0)
		goto primary_key_exit;
	if (table->hasPrimKey) {
		__str_append(&parser->zErrMsg, "table \"",
			     table->zName, "\" has more than one primary key",
			     (char*)0);
		parser->nErr++;
		goto primary_key_exit;
	}
	table->hasPrimKey = 1;
	if (list == 0) {
		col = table->nCol - 1;
		table->aCol[col].isPrimKey = 1;
	} else {
		for (i = 0; i < list->nId; i++) {
			for (col = 0; col < table->nCol; col++) {
				if (strcasecmp(list->a[i].zName,
					  table->aCol[col].zName) == 0)
					break;
			}
			if (col < table->nCol)
				table->aCol[col].isPrimKey = 1;
		}
		if (list->nId > 1)
			col = -1;
	}
	if (col >= 0 && col < table->nCol) {
		type = table->aCol[col].zType;
	}
	if (type && strcasecmp(type, "INTEGER") == 0) {
		table->iPKey = col;
		table->keyConf = on_error;
	} else {
		__create_index(parser, 0, 0, list, on_error, 0, 0);
		list = 0;
	}

primary_key_exit:
	__id_list_delete(list);
	return;
}

/*
 * __collate_type --
 *	Return the appropriate collating type given a type name.
 *	The collation type is text (DBSQL_SO_TEXT) if the type
 *	name contains the character stream "text" or "blob" or
 *	"clob".  Any other type name is collated as numeric
 *	(DBSQL_SO_NUM).
 *
 * PUBLIC: int __collate_type __P((const char *, int));
 */
int
__collate_type(type, ntype)
	const char *type;
	int ntype;
{
	int i;
	for (i = 0; i < ntype - 1; i++) {
		switch(type[i]) {
		case 'b': /* FALLTHROUGH */
		case 'B':
			if (i < ntype - 3 &&
			    strncasecmp(&type[i], "blob", 4) == 0) {
				return DBSQL_SO_TEXT;
			}
			break;
		case 'c': /* FALLTHROUGH */
		case 'C':
			if (i < ntype - 3 &&
			    (strncasecmp(&type[i],"char",4) == 0 ||
			     strncasecmp(&type[i],"clob",4) == 0)) {
				return DBSQL_SO_TEXT;
			}
			break;
		case 'x': /* FALLTHROUGH */
		case 'X':
			if (i >= 2 &&
			    strncasecmp(&type[i - 2],
						   "text", 4) == 0) {
				return DBSQL_SO_TEXT;
			}
			break;
		default:
			break;
		}
	}
	return DBSQL_SO_NUM;
}

/*
 * __add_collate_type --
 *	This routine is called by the parser while in the middle of
 *	parsing a CREATE TABLE statement.  A "COLLATE" clause has
 *	been seen on a column.  This routine sets the column_t.sortOrder on
 *	the column currently under construction.
 *
 * PUBLIC: void __add_collate_type __P((parser_t *, int));
 */
void
__add_collate_type(parser, type)
	parser_t *parser;
	int type;
{
	table_t *table;
	int i;
	if ((table = parser->pNewTable) == 0)
		return;
	i = table->nCol - 1;
	if (i >= 0)
		table->aCol[i].sortOrder = type;
}

/*
 * __change_schema_signature
 *	Come up with a new random value for the schema signature.  Make sure
 *	the new value is different from the old.
 *	The schema signature is used to determine when the schema for the
 *	database changes.  After each schema change, the signature value
 *	changes.  When a process first reads the schema it records the
 *	signature.  Thereafter, whenever it goes to access the database,
 *	it checks the signature to make sure the schema has not changed
 *	since it was last read.
 *	!!!
 *	This plan is not completely bullet-proof.  It is possible for
 *	the schema to change multiple times and for the cookie to be
 *	set back to prior value.  But schema changes are infrequent
 *	and the probability of hitting the same cookie value is only
 *	1 chance in 2^32.  So we're safe enough.
 *
 * PUBLIC: void __change_schema_signature __P((DBSQL *, vdbe_t *));
 */
void __change_schema_signature(dbp, v)
	DBSQL *dbp;
	vdbe_t *v;
{
	static struct drand48_data rand;
	static int first_time = 1;
	if (first_time) {
		first_time = 0;
		srand48_r(1, &rand); /* XXX create a portable rand function */
	}
	if (dbp->next_sig == dbp->aDb[0].schema_sig) {
		u_int8_t n;
		rand8_r(&rand, &n);
		dbp->next_sig = dbp->aDb[0].schema_sig + n + 1;
		dbp->flags |= DBSQL_InternChanges;
		__vdbe_add_op(v, OP_Integer, dbp->next_sig, 0);
		__vdbe_add_op(v, OP_SetSchemaSignature, 0, 0);
	}
}

/*
 * __ident_length --
 *	Measure the number of characters needed to output the given
 *	identifier.  The number returned includes any quotes used
 *	but does not include the null terminator.
 *
 * PUBLIC static int __indent_length __P((const char *));
 */
static int
__ident_length(z)
	const char *z;
{
	int n;
	int need_quote = 0;
	for (n = 0; *z; n++, z++) {
		if (*z == '\'') {
			n++;
			need_quote = 1;
		}
	}
	return (n + (need_quote * 2));
}

/*
 * __ident_put --
 *	Write an identifier onto the end of the given string.  Add
 *	quote characters as needed.
 *
 * STATIC: static void __ident_put __P((char *, int *, char *));
 */
static void
__ident_put(z, idx, ident)
	char *z;
	int *idx;
	char *ident;
{
	int i, j, need_quote;
	i = *idx;
	for (j = 0; ident[j]; j++) {
		if (!isalnum(ident[j]) && ident[j] != '_')
			break;
	}
	need_quote = (ident[j] != 0 ||
		      isdigit(ident[0]) ||
		      __get_keyword_code(ident, j) != TK_ID);
	if (need_quote)
		z[i++] = '\'';
	for (j = 0; ident[j]; j++) {
		z[i++] = ident[j];
		if (ident[j] == '\'')
			z[i++] = '\'';
	}
	if (need_quote)
		z[i++] = '\'';
	z[i] = 0;
	*idx = i;
}

/*
 * __gen_create_table_stmt --
 *	Generate a CREATE TABLE statement appropriate for the given
 *	table.  Memory to hold the text of the statement is obtained
 *	from __dbsql_calloc() and must be freed by the calling function.
 *
 * STATIC: static char *__gen_create_table_stmt __P((table_t *));
 */
static char *
__gen_create_table_stmt(table)
	table_t *table;
{
	int i, k, n;
	char *stmt;
	char *sep, *sep2, *end;

	n = 0;
	for (i = 0; i < table->nCol; i++) {
		n += __ident_length(table->aCol[i].zName);
	}
	n += __ident_length(table->zName);
	if (n < 40) {
		sep = "";
		sep2 = ",";
		end = ")";
	} else {
		sep = "\n  ";
		sep2 = ",\n  ";
		end = "\n)";
	}
	n += 35 + (6 * table->nCol);
	if (__dbsql_malloc(NULL, n, &stmt) == ENOMEM)
		return 0;
	strcpy(stmt, table->iDb == 1 ? "CREATE TEMP TABLE " : "CREATE TABLE ");
	k = strlen(stmt);
	__ident_put(stmt, &k, table->zName);
	stmt[k++] = '(';
	for (i = 0; i < table->nCol; i++) {
		strcpy(&stmt[k], sep);
		k += strlen(&stmt[k]);
		sep = sep2;
		__ident_put(stmt, &k, table->aCol[i].zName);
	}
	strcpy(&stmt[k], end);
	return stmt;
}

/*
 * __ending_create_table_paren --
 *	This routine is called to report the final ")" that terminates
 *	a CREATE TABLE statement.
 *	The table structure that other action routines have been building
 *	is added to the internal hash tables, assuming no errors have
 *	occurred.
 *	An entry for the table is made in the master table on disk,
 *	unless this is a temporary table or initFlag==1.  When initFlag==1,
 *	it means we are reading the master table because we just
 *	connected to the database or because the master table has
 *	recently changed, so the entry for this table already exists in
 *	the master table.  We do not want to create it again.
 *	If the 'select' argument is not NULL, it means that this routine
 *	was called to create a table generated from a 
 *	"CREATE TABLE ... AS SELECT ..." statement.  The column names of
 *	the new table will match the result set of the SELECT.
 *
 * PUBLIC: void __ending_create_table_paren __P((parser_t *, token_t *,
 * PUBLIC:                                  select_t *));
 */
void
__ending_create_table_paren(parser, end, select)
	parser_t *parser;
	token_t *end;
	select_t *select;
{
	table_t *table;
	DBSQL *dbp = parser->db;

	if ((end == 0 && select == 0) || parser->nErr || parser->rc == ENOMEM)
		return;

	table = parser->pNewTable;
	if (table == 0)
		return;

	/*
	 * If the table is generated from a SELECT, then construct the
	 * list of columns and the text of the table.
	 */
	if (select) {
		table_t *sel_table = __select_result_set(parser, 0, select);
		if (sel_table == 0)
			return;
		DBSQL_ASSERT(table->aCol == 0);
		table->nCol = sel_table->nCol;
		table->aCol = sel_table->aCol;
		sel_table->nCol = 0;
		sel_table->aCol = 0;
		__vdbe_delete_table(0, sel_table);
	}

	/*
	 * If the initFlag is 1 it means we are reading the SQL off the
	 * "master" or "temp_master" table on the disk.
	 * So do not write to the disk again.  Extract the root page number
	 * for the table from the parser->newTnum field.
	 * TODO: I couldn't find 'sqliteOpenCb' anywhere... cruft?
	 * (The page number should have been put there by the sqliteOpenCb
	 * routine.)
	 */
	if (parser->initFlag) {
		table->tnum = parser->newTnum;
	}

	/*
	 * If not initializing, then create a record for the new table
	 * in the DBSQL_MASTER table of the database.  The record number
	 * for the new table entry should already be on the stack.
	 * If this is a TEMPORARY table, write the entry into the auxiliary
	 * file instead of into the main database file.
	 */
	if (!parser->initFlag) {
		int n;
		vdbe_t *v;
		
		v = __parser_get_vdbe(parser);
		if (v == 0)
			return;
		if (table->pSelect == 0) {
			/* A regular table */
			__vdbe_add_op(v, OP_CreateTable, 0, table->iDb);
			__vdbe_change_p3(v, -1, (char *)&table->tnum,
					 P3_POINTER);
		} else {
			/* A view */
			__vdbe_add_op(v, OP_Integer, 0, 0);
		}
		table->tnum = 0;
		__vdbe_add_op(v, OP_Pull, 1, 0);
		__vdbe_add_op(v, OP_String, 0, 0);
		if (table->pSelect == 0) {
			__vdbe_change_p3(v, -1, "table", P3_STATIC);
		} else {
			__vdbe_change_p3(v, -1, "view", P3_STATIC);
		}
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p3(v, -1, table->zName, P3_STATIC);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p3(v, -1, table->zName, P3_STATIC);
		__vdbe_add_op(v, OP_Dup, 4, 0);
		__vdbe_add_op(v, OP_String, 0, 0);
		if (select) {
			char *z = __gen_create_table_stmt(table);
			n = z ? strlen(z) : 0;
			__vdbe_change_p3(v, -1, z, n);
			__dbsql_free(dbp, z);
		} else {
			DBSQL_ASSERT(end != 0);
			n = P_TO_UINT32(end->z) -
				P_TO_UINT32(parser->sFirstToken.z) + 1;
			__vdbe_change_p3(v, -1, parser->sFirstToken.z, n);
		}
		__vdbe_add_op(v, OP_MakeRecord, 5, 0);
		__vdbe_add_op(v, OP_PutIntKey, 0, 0);
		if (!table->iDb) {
			__change_schema_signature(dbp, v);
		}
		__vdbe_add_op(v, OP_Close, 0, 0);
		if (select) {
			__vdbe_add_op(v, OP_Integer,table->iDb, 0);
			__vdbe_add_op(v, OP_OpenWrite, 1, 0);
			parser->nTab = 2;
			__select(parser, select, SRT_Table, 1, 0, 0, 0);
		}
		__vdbe_conclude_write(parser);
	}

	/*
	 * Add the table to the in-memory representation of the database.
	 */
	if (parser->explain == 0 && parser->nErr == 0) {
		table_t *old;
		foreign_key_t *fkey;
		old = __hash_insert(&dbp->aDb[table->iDb].tblHash, 
                            table->zName, strlen(table->zName) + 1, table);
		if (old) {
			/* Malloc must have failed inside __hash_insert() */
			DBSQL_ASSERT(table == old);
			return;
		}
		for (fkey = table->pFKey; fkey; fkey = fkey->pNextFrom) {
			int nto = strlen(fkey->zTo) + 1;
			fkey->pNextTo =
				__hash_find(&dbp->aDb[table->iDb].aFKey,
						    fkey->zTo, nto);
			__hash_insert(&dbp->aDb[table->iDb].aFKey, fkey->zTo,
				      nto, fkey);
		}
		parser->pNewTable = 0;
		dbp->nTable++;
		dbp->flags |= DBSQL_InternChanges;
	}
}

/*
 * __create_view --
 *	The parser calls this routine in order to create a new VIEW
 *
 * PUBLIC: void __create_view __P((parser_t *, token_t *, token_t *,
 * PUBLIC:                    select_t *, int));
 *
 * parser			The parsing context
 * begin			The CREATE token that begins the statement
 * name				The token that holds the name of the view
 * select			A SELECT statement to produce the new view
 * temp				TRUE for a TEMPORARY view
 */
void __create_view(parser, begin, name, select, temp)
	parser_t *parser;
	token_t *begin;
	token_t *name;
	select_t *select;
	int temp;
{
	table_t *table;
	const char *z;
	token_t end;
	ref_normalizer_ctx_t normctx;

	__start_table(parser, begin, name, temp, 1);
	table = parser->pNewTable;
	if (table == 0 || parser->nErr) {
		__select_delete(select);
		return;
	}
	if (__ref_normalizer_ctx_init(&normctx, parser, table->iDb, "view",
				      name) &&
	    __ref_normalize_select(&normctx, select)) {
		__select_delete(select);
		return;
	}

	/*
	 * Make a copy of the entire SELECT statement that defines the view.
	 * This will force all the expr_t.token.z values to be dynamically
	 * allocated rather than point to the input string - which means that
	 * they will persist after the current dbsql_exec() call returns.
	 */
	table->pSelect = __select_dup(select);
	__select_delete(select);
	if (!parser->initFlag) {
		__view_get_column_names(parser, table);
	}

	/*
	 * Locate the end of the CREATE VIEW statement.  Make 'end' point to
	 * the end.
	 */
	end = parser->sLastToken;
	if (end.z[0] != 0 && end.z[0] != ';') {
		end.z += end.n;
	}
	end.n = 0;
	z = end.z;
	while (z-- >= begin->z) {
		if (*z == ';' || isspace(*z))
			break;
	}
	end.z = z;
	end.n = 1;

	/*
	 * Use __ending_create_table_paren() to add the view to the
	 * DBSQL_MASTER table
	 */
	__ending_create_table_paren(parser, &end, 0);
	return;
}

/*
 * __view_get_column_names --
 *	The table_t structure pTable is really a VIEW.  Fill in the names of
 *	the columns of the view in the pTable structure.  Return the number
 *	of errors.  If an error is seen leave an error message in
 *	parser->zErrMsg.
 *
 * PUBLIC: int __view_get_column_names __P((parser_t *, table_t *));
 */
int
__view_get_column_names(parser, table)
	parser_t *parser;
	table_t *table;
{
	expr_list_t *elist;
	select_t *sel;
	table_t *sel_table;
	int nerr = 0;

	DBSQL_ASSERT(table);

	/*
	 * A positive nCol means the columns names for this view are
	 * already known.
	 */
	if (table->nCol > 0)
		return 0;

	/*
	 * A negative nCol is a special marker meaning that we are currently
	 * trying to compute the column names.  If we enter this routine with
	 * a negative nCol, it means two or more views form a loop, like this:
	 *
	 *     CREATE VIEW one AS SELECT * FROM two;
	 *     CREATE VIEW two AS SELECT * FROM one;
	 *
	 * Actually, this error is caught previously and so the following test
	 * should always fail.  But we will leave it in place just to be safe.
	 */
	if (table->nCol < 0) {
		__str_append(&parser->zErrMsg, "view ",
			     table->zName, " is circularly defined", (char*)0);
		parser->nErr++;
		return 1;
	}

	/*
	 * If we get this far, it means we need to compute the table names.
	 */
        /* If nCol == 0, then 'table' must be a VIEW */
	DBSQL_ASSERT(table->pSelect);
	sel = table->pSelect;

	/*
	 * Note that the call to __select_result_set() will expand any
	 * "*" elements in this list.  But we will need to restore the list
	 * back to its original configuration afterwards, so we save a copy of
	 * the original in elist.
	 */
	elist = sel->pEList;
	sel->pEList = __expr_list_dup(elist);
	if (sel->pEList == 0) {
		sel->pEList = elist;
		return 1;  /* Malloc failed */
	}
	table->nCol = -1;
	sel_table = __select_result_set(parser, 0, sel);
	if (sel_table) {
		DBSQL_ASSERT(table->aCol == 0);
		table->nCol = sel_table->nCol;
		table->aCol = sel_table->aCol;
		sel_table->nCol = 0;
		sel_table->aCol = 0;
		__vdbe_delete_table(0, sel_table);
		DB_PROPERTY_SET(parser->db, table->iDb, DBSQL_UNRESET_VIEWS);
	} else {
		table->nCol = 0;
		nerr++;
	}
	__select_unbind(sel);
	__expr_list_delete(sel->pEList);
	sel->pEList = elist;
	return nerr;  
}

/*
 * __view_reset_column_names --
 *
 *	Clear the column names from the VIEW 'table'.
 *	This routine is called whenever any other table or view is modified.
 *	The view passed into this routine might depend directly or indirectly
 *	on the modified or deleted table so we need to clear the old column
 *	names so that they will be recomputed.
 *
 * STATIC: static void __view_reset_column_names __P((table_t *));
 */
static void
__view_reset_column_names(table)
	table_t *table;
{
	int i;
	if (table == 0 || table->pSelect==0 ) return;
	if (table->nCol == 0)
		return;
	for (i = 0; i < table->nCol; i++) {
		__dbsql_free(NULL, table->aCol[i].zName);
		__dbsql_free(NULL, table->aCol[i].zDflt);
		__dbsql_free(NULL, table->aCol[i].zType);
	}
	__dbsql_free(NULL, table->aCol);
	table->aCol = 0;
	table->nCol = 0;
}

/*
 * __view_reset_all --
 *	Clear the column names from every VIEW in database idx.
 *
 * STATIC: static void __view_reset_all __P((DBSQL *, int));
 */
static void
__view_reset_all(dbp, idx)
	DBSQL *dbp;
	int idx;
{
	hash_ele_t *i;
	if (!DB_PROPERTY_HAS_VALUE(dbp, idx, DBSQL_UNRESET_VIEWS))
		return;
	for (i = __hash_first(&dbp->aDb[idx].tblHash); i;
	     i = __hash_next(i)) {
		table_t *t = __hash_data(i);
		if (t->pSelect ) {
			__view_reset_column_names(t);
		}
	}
	DB_PROPERTY_CLEAR(dbp, idx, DBSQL_UNRESET_VIEWS);
}

/*
 * __table_from_token --
 *	Given a token, look up a table with that name.  If not found, leave
 *	an error for the parser to find and return NULL.
 *
 * PUBLIC: table_t *__table_from_token __P((parser_t *, token_t *));
 */
table_t *
__table_from_token(parser, token)
	parser_t *parser;
	token_t *token;
{
	char *name;
	table_t *table;
	name = __table_name_from_token(token);
	if (name == 0)
		return 0;
	table = __find_table(parser->db, name, 0);
	__dbsql_free(parser->db, name);
	if (table == 0) {
		__str_nappend(&parser->zErrMsg, "no such table: ",
			      0, token->z, token->n, NULL);
		parser->nErr++;
	}
	return table;
}

/*
 * __drop_table --
 *	This routine is called to do the work of a DROP TABLE statement.
 *	pName is the name of the table to be dropped.
 *	TODO: Its likely that with DB we can do this much quiker as we'll
 *	likely have each sql database in its own DB database.
 *
 * PUBLIC: void __drop_table __P((parser_t *, token_t *name, int));
 */
void
__drop_table(parser, name, view)
	parser_t *parser;
	token_t *name;
	int view;
{
	table_t *table;
	vdbe_t *v;
	int base;
	DBSQL *dbp = parser->db;
	int idb;

	if (parser->nErr || parser->rc == ENOMEM)
		return;
	table = __table_from_token(parser, name);
	if (table == 0)
		return;
	idb = table->iDb;
	DBSQL_ASSERT(idb >= 0 && idb < dbp->nDb);
#ifndef DBSQL_NO_AUTH
	{
		int code;
		const char *auth_table = SCHEMA_TABLE(table->iDb);
		const char *auth_db_name = dbp->aDb[table->iDb].zName;
		if (__auth_check(parser, DBSQL_DELETE, auth_table, 0,
				 auth_db_name)) {
			return;
		}
		if (view) {
			if (idb == 1) {
				code = DBSQL_DROP_TEMP_VIEW;
			} else {
				code = DBSQL_DROP_VIEW;
			}
		} else {
			if (idb == 1) {
				code = DBSQL_DROP_TEMP_TABLE;
			} else {
				code = DBSQL_DROP_TABLE;
			}
		}
		if (__auth_check(parser, code, table->zName, 0,
				 auth_db_name)) {
			return;
		}
		if (__auth_check(parser, DBSQL_DELETE, table->zName, 0,
			    auth_db_name)) {
			return;
		}
	}
#endif
	if (table->readOnly) {
		__str_append(&parser->zErrMsg, "table ", table->zName, 
			     " may not be dropped", (char*)0);
		parser->nErr++;
		return;
	}
	if (view && table->pSelect == 0) {
		__str_append(&parser->zErrMsg,
			     "use DROP TABLE to delete table ", table->zName,
			     (char*)0);
		parser->nErr++;
		return;
	}
	if (!view && table->pSelect) {
		__str_append(&parser->zErrMsg,
			     "use DROP VIEW to delete view ", table->zName,
			     (char*)0);
		parser->nErr++;
		return;
	}

	/*
	 *  Generate code to remove the table from the master table
	 * on disk.
	 */
	v = __parser_get_vdbe(parser);
	if (v) {
		static vdbe_op_t drop_table[] = {
			{ OP_Rewind,     0, ADDR(8),  0},
			{ OP_String,     0, 0,        0}, /* 1 */
			{ OP_MemStore,   1, 1,        0},
			{ OP_MemLoad,    1, 0,        0}, /* 3 */
			{ OP_Column,     0, 2,        0},
			{ OP_Ne,         0, ADDR(7),  0},
			{ OP_Delete,     0, 0,        0},
			{ OP_Next,       0, ADDR(3),  0}, /* 7 */
		};
		index_t *idx;
		trigger_t *trigger;
		__vdbe_prepare_write(parser, 0, table->iDb);

		/*
		 * Drop all triggers associated with the table being dropped.
		 */
		trigger = table->pTrigger;
		while (trigger) {
			DBSQL_ASSERT(trigger->iDb == table->iDb ||
			       trigger->iDb == 1);
			__drop_trigger_ptr(parser, trigger, 1);
			if (parser->explain) {
				trigger = trigger->pNext;
			} else {
				trigger = table->pTrigger;
			}
		}

		/*
		 * Drop all DBSQL_MASTER entries that refer to the table.
		 */
		__open_master_table(v, table->iDb);
		base = __vdbe_add_op_list(v, ARRAY_SIZE(drop_table),
					  drop_table);
		__vdbe_change_p3(v, base + 1, table->zName, 0);

		/*
		 * Drop all DBSQL_TEMP_MASTER entries that refer to the table.
		 */
		if (table->iDb != 1) {
			__open_master_table(v, 1);
			base = __vdbe_add_op_list(v, ARRAY_SIZE(drop_table),
						  drop_table);
			__vdbe_change_p3(v, base + 1, table->zName, 0);
		}

		if (table->iDb == 0) {
			__change_schema_signature(dbp, v);
		}
		__vdbe_add_op(v, OP_Close, 0, 0);
		if (!view) {
			__vdbe_add_op(v, OP_Destroy, table->tnum, table->iDb);
			for (idx = table->pIndex; idx; idx = idx->pNext) {
				__vdbe_add_op(v, OP_Destroy, idx->tnum,
					      idx->iDb);
			}
		}
		__vdbe_conclude_write(parser);
	}

	/*
	 * Delete the in-memory description of the table.
	 * Exception: if the SQL statement began with the EXPLAIN keyword,
	 * then no changes should be made.
	 */
	if (!parser->explain) {
		__unlink_and_delete_table(dbp, table);
		dbp->flags |= DBSQL_InternChanges;
	}
	__view_reset_all(dbp, idb);
}

/*
 * __add_idx_key_type --
 *	This routine constructs a P3 string suitable for an OP_MakeIdxKey
 *	opcode and adds that P3 string to the most recently inserted
 *	instruction in the virtual machine.  The P3 string consists of a
 *	single character for each column in the index 'idx' of table
 *	'table'.  If the column uses a numeric sort order, then the P3
 *	string character corresponding to that column is 'n'.  If the column
 *	uses a text sort order, then the P3 string is 't'.  See the
 *	OP_MakeIdxKey opcode documentation for additional information.
 *	See also: __add_key_type()
 *
 * PUBLIC: void __add_idx_key_type __P((vdbe_t *, index_t *));
 */
void
__add_idx_key_type(v, idx)
	vdbe_t *v;
	index_t *idx;
{
	char *type;
	table_t *table;
	int i, n;

	DBSQL_ASSERT(idx != 0 && idx->pTable != 0);
	table = idx->pTable;
	n = idx->nColumn;
	if (__dbsql_malloc(NULL, n + 1, &type) == ENOMEM)
		return;
	for (i = 0; i < n; i++) {
		int col = idx->aiColumn[i];
		DBSQL_ASSERT(col >= 0 && col < table->nCol);
		if ((table->aCol[col].sortOrder & DBSQL_SO_TYPEMASK) ==
		    DBSQL_SO_TEXT) {
			type[i] = 't';
		} else {
			type[i] = 'n';
		}
	}
	type[n] = 0;
	__vdbe_change_p3(v, -1, type, n);
	__dbsql_free(NULL, type);
}

/*
 * __create_foreign_key --
 *	This routine is called to create a new foreign key on the table
 *	currently under construction.  'from_col' determines which columns
 *	in the current table point to the foreign key.  If 'from_col'==0 then
 *	connect the key to the last column inserted.  'to' is the name of
 *	the table referred to.  to_col is a list of tables in the other
 *	'to' table that the foreign key points to.  flags contains all
 *	information about the conflict resolution algorithms specified
 *	in the ON DELETE, ON UPDATE and ON INSERT clauses.
 *	An foreign_key_t structure is created and added to the table currently
 *	under construction in the parser->pNewTable field.  The new
 *	foreign_key_t is not linked into dbp->aFKey at this point - that
 *	does not happen until __ending_create_table_paren().
 *	The foreign key is set for IMMEDIATE processing.  A subsequent call
 *	to __defer_foreign_key() might change this to DEFERRED.
 *
 * PUBLIC: void __create_foreign_key __P((parser_t *, id_list_t *, token_t *,
 * PUBLIC:                           id_list_t *, int));
 *
 * parser			Parsing context
 * from_col			Columns in table that point to other table
 * to				Name of the other table
 * to_col			Columns in the other table
 * flags			Conflict resolution algorithms
 */
void __create_foreign_key(parser, from_col, to, to_col, flags)
	parser_t *parser;
	id_list_t *from_col;
	token_t *to;
	id_list_t *to_col;
	int flags;
{
	table_t *table = parser->pNewTable;
	int bytes;
	int i;
	int ncol;
	char *z;
	foreign_key_t *fkey = 0;

	DBSQL_ASSERT(to != 0);
	if (table == 0 || parser->nErr)
		goto fk_end;
	if (from_col == 0) {
		int col = table->nCol - 1;
		if (col < 0)
			goto fk_end;
		if (to_col && to_col->nId!=1 ){
			__str_nappend(&parser->zErrMsg,
				      "foreign key on ", -1,
				      table->aCol[col].zName, -1, 
				      " should reference only one column "
				      "of table ", -1, to->z, to->n, NULL);
			parser->nErr++;
			goto fk_end;
		}
		ncol = 1;
	} else if (to_col && to_col->nId != from_col->nId) {
		__str_append(&parser->zErrMsg, 
			     "number of columns in foreign key does not "
			     "match the number of columns in the referenced "
			     "table", (char*)0);
		parser->nErr++;
		goto fk_end;
	} else {
		ncol = from_col->nId;
	}
	bytes = sizeof(*fkey) + ncol * sizeof(fkey->aCol[0]) + to->n + 1;
	if (to_col) {
		for (i = 0; i < to_col->nId; i++) {
			bytes += strlen(to_col->a[i].zName) + 1;
		}
	}
	if (__dbsql_calloc(NULL, 1, bytes, &fkey) == ENOMEM)
		goto fk_end;
	fkey->pFrom = table;
	fkey->pNextFrom = table->pFKey;
	z = (char*)&fkey[1];
	fkey->aCol = (struct col_map*)z;
	z += sizeof(struct col_map)*ncol;
	fkey->zTo = z;
	memcpy(z, to->z, to->n);
	z[to->n] = 0;
	z += to->n + 1;
	fkey->pNextTo = 0;
	fkey->nCol = ncol;
	if (from_col == 0) {
		fkey->aCol[0].iFrom = table->nCol - 1;
	} else {
		for(i = 0; i < ncol; i++) {
			int j;
			for(j = 0; j < table->nCol; j++) {
				if (strcasecmp(
					    table->aCol[j].zName,
					    from_col->a[i].zName) == 0) {
					fkey->aCol[i].iFrom = j;
					break;
				}
			}
			if (j >= table->nCol) {
				__str_append(&parser->zErrMsg,
					     "unknown column \"", 
					     from_col->a[i].zName,
					     "\" in foreign key definition",
					     (char*)0);
				parser->nErr++;
				goto fk_end;
			}
		}
	}
	if (to_col) {
		for (i = 0; i < ncol; i++) {
			int n = strlen(to_col->a[i].zName);
			fkey->aCol[i].zCol = z;
			memcpy(z, to_col->a[i].zName, n);
			z[n] = 0;
			z += n+1;
		}
	}
	fkey->isDeferred = 0;
	fkey->deleteConf = flags & 0xff;
	fkey->updateConf = (flags >> 8 ) & 0xff;
	fkey->insertConf = (flags >> 16 ) & 0xff;

	/*
	 * Link the foreign key to the table as the last step.
	 */
	table->pFKey = fkey;
	fkey = 0;

  fk_end:
	__dbsql_free(NULL, fkey);
	__id_list_delete(from_col);
	__id_list_delete(to_col);
}

/*
 * __defer_foreign_key --
 *	This routine is called when an INITIALLY IMMEDIATE or
 *	INITIALLY DEFERRED clause is seen as part of a foreign key
 *	definition.  The 'deferred' parameter is 1 for INITIALLY DEFERRED
 *	and 0 for INITIALLY IMMEDIATE.
 *	The behavior of the most recently created foreign key is adjusted
 *	accordingly.
 *
 * PUBLIC: void __defer_foreign_key __P((parser_t *, int));
 */
void
__defer_foreign_key(parser, deferred)
	parser_t *parser;
	int deferred;
{
	table_t *table;
	foreign_key_t *fkey;
	if ((table = parser->pNewTable) == 0 || (fkey = table->pFKey) == 0)
		return;
	fkey->isDeferred = deferred;
}

/*
 * __creat_index --
 *	 Create a new index for an SQL table.  'index' is the name of the
 *	index and pTable is the name of the table that is to be indexed.
 *	Both will be NULL for a primary key or an index that is created
 *	to satisfy a UNIQUE constraint.  If pTable and pIndex are NULL,
 *	use parser->pNewTable as the table to be indexed.  parser->pNewTable
 *	is a table that is currently being constructed by a CREATE TABLE
 *	statement.
 *	'list' is a list of columns to be indexed.  'list' will be NULL if
 *	this is a primary key or unique-constraint on the most recent column
 *	added to the table currently under construction.
 *
 * PUBLIC: void __create_index __P((parser_t *, token_t *, src_list_t *,
 * PUBLIC:                     id_list_t *, int, token_t *, token_t *));
 *
 * parser			All information about this parse.
 * token			Name of the index.  May be NULL.
 * sltable			Name of the table to index.  Use
 *				parser->pNewTable if 0
 * list				A list of columns to be indexed
 * on_error			OE_Abort, OE_Ignore, OE_Replace, or OE_None
 * start			The CREATE token that begins a CREATE TABLE
 *				statement.
 * end				The ")" that closes the CREATE INDEX statement.
 */
void __create_index(parser, token, sltable, list, on_error, start, end)
	parser_t *parser;
	token_t *token;
	src_list_t *sltable;
	id_list_t *list;
	int on_error;
	token_t *start;
	token_t *end;
{
	table_t *table;     /* Table to be indexed */
	index_t *index;     /* The index to be created */
	char *name = 0;
	int i, j;
	token_t null_id;    /* Fake token for an empty ID list */
	ref_normalizer_ctx_t normctx; /* For assigning database
                                         names to sltable */
	int temp;           /* True for a temporary index */
	DBSQL *dbp = parser->db;

	if (parser->nErr || parser->rc == ENOMEM)
		goto exit_create_index;
	if (parser->initFlag &&
	   __ref_normalizer_ctx_init(&normctx, parser, parser->iDb,
					"index", token) &&
	   __ref_normalize_src_list(&normctx, sltable)) {
		goto exit_create_index;
	}

	/*
	 * Find the table that is to be indexed.  Return early if not found.
	 */
	if (sltable != 0) {
		DBSQL_ASSERT(token != 0);
		DBSQL_ASSERT(sltable->nSrc == 1);
		table = __src_list_lookup(parser, sltable);
	} else {
		DBSQL_ASSERT(token == 0);
		table =  parser->pNewTable;
	}
	if (table == 0 || parser->nErr )
		goto exit_create_index;
	if (table->readOnly) {
		__str_append(&parser->zErrMsg, "table ", table->zName,
			     " may not be indexed", (char*)0);
		parser->nErr++;
		goto exit_create_index;
	}
	if (table->iDb >= 2 && parser->initFlag == 0) {
		__str_append(&parser->zErrMsg, "table ", table->zName, 
			     " may not have indices added", (char*)0);
		parser->nErr++;
		goto exit_create_index;
	}
	if (table->pSelect) {
		__str_append(&parser->zErrMsg, "views may not be indexed",
			     (char*)0);
		parser->nErr++;
		goto exit_create_index;
	}
	temp = (table->iDb == 1);

	/*
	 * Find the name of the index.  Make sure there is not already another
	 * index or table with the same name.  
	 *
	 * Exception:  If we are reading the names of permanent indices from
	 * the master table (because some other process changed the schema) and
	 ** one of the index names collides with the name of a temporary table
	 * or index, then we will continue to process this index.
	 *
	 * If token==0 it means that we are dealing with a primary key or
	 * UNIQUE constraint.  We have to invent our own name.
	 */
	if (token && !parser->initFlag) {
		index_t *idx_same_name; /* Another index with the same name */
		table_t *tbl_same_name; /* A table with same name as index */
		__dbsql_strndup(dbp, token->z, &name, token->n);
		if (name == 0)
			goto exit_create_index;
		if ((idx_same_name = __find_index(dbp, name, 0)) != 0) {
			__str_append(&parser->zErrMsg, "index ", name, 
				     " already exists", (char*)0);
			parser->nErr++;
			goto exit_create_index;
		}
		if ((tbl_same_name = __find_table(dbp, name, 0)) != 0) {
			__str_append(&parser->zErrMsg,
				     "there is already a table named ", name,
				     (char*)0);
			parser->nErr++;
			goto exit_create_index;
		}
	} else if (token == 0) {
		char buf[30]; /* TODO: static buffers are bad */
		int n = 1;
		index_t *loop = table->pIndex;
		while (loop) {
			loop = loop->pNext;
			n++;
		}
		sprintf(buf,"%d)", n);
		name = 0;
		__str_append(&name, "(", table->zName, " autoindex ", buf,
			     (char*)0);
		if (name == 0)
			goto exit_create_index;
	} else {
		__dbsql_strndup(dbp, token->z, &name, token->n);
	}

	/*
	 * Check for authorization to create an index.
	 */
#ifndef DBSQL_NO_AUTH
	{
		const char *db_name = dbp->aDb[table->iDb].zName;

		DBSQL_ASSERT(table->iDb == parser->iDb || temp);
		if (__auth_check(parser, DBSQL_INSERT, SCHEMA_TABLE(temp),
				 0, db_name)) {
			goto exit_create_index;
		}
		i = DBSQL_CREATE_INDEX;
		if (temp)
			i = DBSQL_CREATE_TEMP_INDEX;
		if (__auth_check(parser, i, name, table->zName, db_name)) {
			goto exit_create_index;
		}
	}
#endif

	/*
	 * If 'list'==0, it means this routine was called to make a primary
	 * key out of the last column added to the table under construction.
	 * So create a fake list to simulate this.
	 */
	if (list == 0) {
		null_id.z = table->aCol[table->nCol-1].zName;
		null_id.n = strlen(null_id.z);
		list = __id_list_append(0, &null_id);
		if (list == 0)
			goto exit_create_index;
	}

	/* 
	 * Allocate the index structure. 
	 */
	if (__dbsql_calloc(dbp, 1, sizeof(index_t) + (strlen(name) + 1) +
			(sizeof(int) * list->nId), &index) == ENOMEM)
		goto exit_create_index;
	index->aiColumn = (int*)&index[1];
	index->zName = (char*)&index->aiColumn[list->nId];
	strcpy(index->zName, name);
	index->pTable = table;
	index->nColumn = list->nId;
	index->onError = on_error;
	index->autoIndex = (token == 0);
	index->iDb = (temp ? 1 : parser->iDb);

	/*
	 * Scan the names of the columns of the table to be indexed and
	 * load the column indices into the index_t structure.  Report an error
	 * if any column is not found.
	 */
	for (i = 0; i < list->nId; i++) {
		for(j = 0; j < table->nCol; j++) {
			if (strcasecmp(list->a[i].zName,
						  table->aCol[j].zName) == 0) {
				break;
			}
		}
		if (j >= table->nCol) {
			__str_append(&parser->zErrMsg, "table ",
				     table->zName, " has no column named ",
				     list->a[i].zName, (char*)0);
			parser->nErr++;
			__dbsql_free(dbp, index);
			goto exit_create_index;
		}
		index->aiColumn[i] = j;
	}

	/* 
	 * Link the new index_t structure to its table and to the other
	 * in-memory database structures. 
	 */
	if (!parser->explain) {
		index_t *rindex;
		rindex = __hash_insert(&dbp->aDb[index->iDb].idxHash, 
                         index->zName, strlen(index->zName) + 1, index);
		if (rindex) {
                        /* Malloc must have failed */
			DBSQL_ASSERT(rindex == index);
			__dbsql_free(dbp, index);
			goto exit_create_index;
		}
		dbp->flags |= DBSQL_InternChanges;
	}

	/*
	 * When adding an index to the list of indices for a table, make
	 * sure all indices labeled OE_Replace come after all those labeled
	 * OE_Ignore.  This is necessary for the correct operation of UPDATE
	 * and INSERT.
	 */
	if (on_error != OE_Replace || table->pIndex == 0 ||
	    table->pIndex->onError == OE_Replace) {
		index->pNext = table->pIndex;
		table->pIndex = index;
	} else {
		index_t *other = table->pIndex;
		while (other->pNext && other->pNext->onError != OE_Replace) {
			other = other->pNext;
		}
		index->pNext = other->pNext;
		other->pNext = index;
	}

	if (parser->initFlag && sltable != 0) {
		/*
		 * If the initFlag is 1 it means we are reading the SQL off the
		 * "master" table on the disk.  So do not write to the disk
		 * again.  Extract the table number from the parser->newTnum
		 * field.
		 */
		index->tnum = parser->newTnum;
	} else if (parser->initFlag == 0) {
		/*
		 * If the initFlag is 0 then create the index on disk.  This
		 * involves writing the index into the master table and
		 * filling in the index with the current table contents.
		 *
		 * The initFlag is 0 when the user first enters a CREATE INDEX 
		 * command.  The initFlag is 1 when a database is opened and 
		 * CREATE INDEX statements are read out of the master table.
		 * In the latter case the index already exists on disk, which
		 * is why we don't want to recreate it.
		 *
		 * If sltable==0 it means this index is generated as a primary
		 * key or UNIQUE constraint of a CREATE TABLE statement.
		 * Since the table has just been created, it contains no data
		 * and the index initialization step can be skipped.
		 */
		int n;
		vdbe_t *v;
		int lbl1, lbl2;
		int i;
		int addr;

		v = __parser_get_vdbe(parser);
		if (v == 0)
			goto exit_create_index;
		if (sltable != 0) {
			__vdbe_prepare_write(parser, 0, temp);
			__open_master_table(v, temp);
		}
		__vdbe_add_op(v, OP_NewRecno, 0, 0);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p3(v, -1, "index", P3_STATIC);
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p3(v, -1, index->zName, strlen(index->zName));
		__vdbe_add_op(v, OP_String, 0, 0);
		__vdbe_change_p3(v, -1, table->zName, P3_STATIC);
		addr = __vdbe_add_op(v, OP_CreateIndex, 0, temp);
		__vdbe_change_p3(v, addr, (char*)&index->tnum, P3_POINTER);
		index->tnum = 0;
		if (sltable) {
			__vdbe_add_op(v, OP_Dup, 0, 0);
			__vdbe_add_op(v, OP_Integer, temp, 0);
			__vdbe_add_op(v, OP_OpenWrite, 1, 0);
		}
		addr = __vdbe_add_op(v, OP_String, 0, 0);
		if (start && end ){
			n = P_TO_UINT32(end->z) - P_TO_UINT32(start->z) + 1;
			__vdbe_change_p3(v, addr, start->z, n);
		}
		__vdbe_add_op(v, OP_MakeRecord, 5, 0);
		__vdbe_add_op(v, OP_PutIntKey, 0, 0);
		if (sltable) {
			__vdbe_add_op(v, OP_Integer, table->iDb, 0);
			__vdbe_add_op(v, OP_OpenRead, 2, table->tnum);
			__vdbe_change_p3(v, -1, table->zName, P3_STATIC);
			lbl2 = __vdbe_make_label(v);
			__vdbe_add_op(v, OP_Rewind, 2, lbl2);
			lbl1 = __vdbe_add_op(v, OP_Recno, 2, 0);
			for (i = 0; i < index->nColumn; i++) {
				int col = index->aiColumn[i];
				if (table->iPKey == col) {
					__vdbe_add_op(v, OP_Dup, i, 0);
				} else {
					__vdbe_add_op(v, OP_Column, 2, col);
				}
			}
			__vdbe_add_op(v, OP_MakeIdxKey, index->nColumn, 0);
			__add_idx_key_type(v, index);
			__vdbe_add_op(v, OP_IdxPut, 1,
				      index->onError != OE_None);
			__vdbe_change_p3(v, -1,
					 "indexed columns are not unique",
					 P3_STATIC);
			__vdbe_add_op(v, OP_Next, 2, lbl1);
			__vdbe_resolve_label(v, lbl2);
			__vdbe_add_op(v, OP_Close, 2, 0);
			__vdbe_add_op(v, OP_Close, 1, 0);
		}
		if (sltable != 0) {
			if (!temp) {
				__change_schema_signature(dbp, v);
			}
			__vdbe_add_op(v, OP_Close, 0, 0);
			__vdbe_conclude_write(parser);
		}
	}

	/* Clean up before exiting */
exit_create_index:
	__id_list_delete(list);
	__src_list_delete(sltable);
	__dbsql_free(dbp, name);
	return;
}

/*
 * __drop_index --
 *	This routine will drop an existing named index.  This routine
 *	implements the DROP INDEX statement.
 *
 * PUBILC: __drop_index __P((parser_t *, src_list_t *));
 */
void
__drop_index(parser, name)
	parser_t *parser;
	src_list_t *name;
{
	index_t *index;
	vdbe_t *v;
	DBSQL *dbp = parser->db;

	if (parser->nErr || parser->rc == ENOMEM)
		return;
	DBSQL_ASSERT(name->nSrc == 1);
	index = __find_index(dbp, name->a[0].zName, name->a[0].zDatabase);
	if (index == 0) {
		__error_msg(parser, "no such index: %S", name, 0);
		goto exit_drop_index;
	}
	if (index->autoIndex) {
		__error_msg(parser, "index associated with UNIQUE "
			    "or PRIMARY KEY constraint cannot be dropped", 0);
		goto exit_drop_index;
	}
	if (index->iDb > 1) {
		__error_msg(parser, "cannot alter schema of attached "
			    "databases", 0);
		goto exit_drop_index;
	}
#ifndef DBSQL_NO_AUTH
	{
		int code = DBSQL_DROP_INDEX;
		table_t *table = index->pTable;
		const char *db_name = dbp->aDb[index->iDb].zName;
		const char *table_name = SCHEMA_TABLE(index->iDb);
		if (__auth_check(parser, DBSQL_DELETE, table_name, 0,
				 db_name)) {
			goto exit_drop_index;
		}
		if (index->iDb)
			code = DBSQL_DROP_TEMP_INDEX;
		if (__auth_check(parser, code, index->zName, table->zName,
				 db_name)) {
			goto exit_drop_index;
		}
	}
#endif

	/*
	 * Generate code to remove the index and from the master table.
	 */
	v = __parser_get_vdbe(parser);
	if( v ){
		static vdbe_op_t drop_index[] = {
			{ OP_Rewind,     0, ADDR(9), 0}, 
			{ OP_String,     0, 0,       0}, /* 1 */
			{ OP_MemStore,   1, 1,       0},
			{ OP_MemLoad,    1, 0,       0}, /* 3 */
			{ OP_Column,     0, 1,       0},
			{ OP_Eq,         0, ADDR(8), 0},
			{ OP_Next,       0, ADDR(3), 0},
			{ OP_Goto,       0, ADDR(9), 0},
			{ OP_Delete,     0, 0,       0}, /* 8 */
		};
		int base;

		__vdbe_prepare_write(parser, 0, index->iDb);
		__open_master_table(v, index->iDb);
		base = __vdbe_add_op_list(v, ARRAY_SIZE(drop_index),
					  drop_index);
		__vdbe_change_p3(v, base + 1, index->zName, 0);
		if (index->iDb == 0) {
			__change_schema_signature(dbp, v);
		}
		__vdbe_add_op(v, OP_Close, 0, 0);
		__vdbe_add_op(v, OP_Destroy, index->tnum, index->iDb);
		__vdbe_conclude_write(parser);
	}

	/*
	 * Delete the in-memory description of this index.
	 */
	if (!parser->explain) {
		__unlink_and_delete_index(dbp, index);
		dbp->flags |= DBSQL_InternChanges;
	}

  exit_drop_index:
	__src_list_delete(name);
}

/*
 * __id_list_append --
 *	Append a new element to the given id_list_t.  Create a new
 *	id_list_t if need be.
 *	A new id_list_t is returned, or NULL if __dbsql_malloc() fails.
 *
 * PUBLIC: id_list_t *__id_list_append __P((id_list_t *, token_t *));
 */
id_list_t *
__id_list_append(list, token)
	id_list_t *list;
	token_t *token;
{
	if (list == 0) {
		if (__dbsql_calloc(NULL, 1, sizeof(id_list_t), &list) ==ENOMEM)
			return 0;
		list->nAlloc = 0;
	}
	if (list->nId >= list->nAlloc) {
		struct id_list_item *a;
		list->nAlloc = (list->nAlloc * 2) + 5;
		if (__dbsql_realloc(NULL, list->nAlloc * sizeof(list->a[0]),
				 &list->a) == ENOMEM) {
			__id_list_delete(list);
			return 0;
		}
	}
	memset(&list->a[list->nId], 0, sizeof(list->a[0]));
	if(token) {
		char **pz = &list->a[list->nId].zName;
		__str_nappend(pz, token->z, token->n, NULL);
		if (*pz == 0) {
			__id_list_delete(list);
			return 0;
		} else {
			__str_unquote(*pz);
		}
	}
	list->nId++;
	return list;
}

/*
 * __src_list_append --
 *	Append a new table name to the given src_list_t.  Create a new
 *	src_list_t if need be.  A new entry is created in the src_list_t
 *	even if 'token' is NULL.
 *	A new src_list_t is returned, or NULL if __dbsql_malloc() fails.
 *	If 'database' is not null, it means that the table has an optional
 *	database name prefix.  Like this:  "database.table".  The database
 *	points to the table name and the 'table' points to the database name.
 *	The src_list_t.a[].zName field is filled with the table name which
 *	might come from 'table' (if 'database' is NULL) or from 'database'.
 *	src_list_t.a[].zDatabase is filled with the database name from
 *	'table', or with NULL if no database is specified.
 *	In other words, if call like this:
 *	  __src_list_append(A,B,0);
 *	Then B is a table name and the database name is unspecified.  If
 *	called like this:
 *	  __src_list_append(A,B,C);
 *	Then C is the table name and B is the database name.
 *
 * PUBLIC: src_list_t *__src_list_append __P((src_list_t *, token_t *,
 * PUBLIC:                               token_t *));
 */
src_list_t *
__src_list_append(list, table, database)
	src_list_t *list;
	token_t *table;
	token_t *database;
{
	if (list == 0) {
		if (__dbsql_calloc(NULL, 1, sizeof(src_list_t), &list)==ENOMEM)
			return 0;
		list->nAlloc = 1;
	}
	if (list->nSrc >= list->nAlloc) {
		list->nAlloc *= 2;
		if (__dbsql_realloc(NULL, sizeof(*list) + (list->nAlloc - 1) *
				 sizeof(list->a[0]), &list) == ENOMEM) {
			__src_list_delete(list);
			return 0;
		}
	}
	memset(&list->a[list->nSrc], 0, sizeof(list->a[0]));
	if (database && database->z == 0) {
		database = 0;
	}
	if (database && table) {
		token_t *temp = database;
		database = table;
		table = temp;
	}
	if (table) {
		char **pz = &list->a[list->nSrc].zName;
		__str_nappend(pz, table->z, table->n, NULL);
		if (*pz == 0) {
			__src_list_delete(list);
			return 0;
		} else {
			__str_unquote(*pz);
		}
	}
	if (database) {
		char **pz = &list->a[list->nSrc].zDatabase;
		__str_nappend(pz, database->z, database->n, NULL);
		if (*pz == 0) {
			__src_list_delete(list);
			return 0;
		} else {
			__str_unquote(*pz);
		}
	}
	list->a[list->nSrc].iCursor = -1;
	list->nSrc++;
	return list;
}

/*
 * __src_list_assign_cursors --
 *	Assign cursors to all tables in a src_list_t.
 *
 * __src_list_assign_cursors __P((parser_t *, src_list_t *));
 */
void
__src_list_assign_cursors(parser, list)
	parser_t *parser;
	src_list_t *list;
{
	int i;
	for (i = 0; i < list->nSrc; i++) {
		if (list->a[i].iCursor < 0) {
			list->a[i].iCursor = parser->nTab++;
		}
	}
}

/*
 * __src_list_add_alias --
 *	Add an alias to the last identifier on the given identifier list.
 *
 * PUBLIC: void __src_list_add_alias __P((src_list_t *, token_t *));
 */
void
__src_list_add_alias(list, token)
	src_list_t *list;
	token_t *token;
{
	if (list && list->nSrc > 0) {
		int i = list->nSrc - 1;
		__str_nappend(NULL, &list->a[i].zAlias, token->z,
			      token->n, NULL);
		__str_unquote(list->a[i].zAlias);
	}
}

/*
 * __id_list_delete --
 *	Delete an id_list_t.
 *
 * PUBLIC: void __id_list_delete __P((id_list_t *));
 */
void
__id_list_delete(list)
	id_list_t *list;
{
	int i;
	if (list == 0)
		return;
	for (i = 0; i < list->nId; i++) {
		__dbsql_free(NULL, list->a[i].zName);
	}
	__dbsql_free(NULL, list->a);
	__dbsql_free(NULL, list);
}

/*
 * __id_list_index --
 *	Return the index in list of the identifier named zId.  Return -1
 *	if not found.
 *
 * PUBLIC: int __id_list_index __P((id_list_t *, const char *));
 */
int
__id_list_index(list, name)
	id_list_t *list;
	const char *name;
{
	int i;
	if (list == 0)
		return -1;
	for (i = 0; i < list->nId; i++) {
		if (strcasecmp(list->a[i].zName, name) == 0)
			return i;
	}
	return -1;
}

/*
 * __src_list_delete --
 *	Delete an entire src_list_t including all its substructure.
 *
 * PUBLIC: void __src_list_delete __P((src_list_t *));
 */
void
__src_list_delete(list)
	src_list_t *list;
{
	int i;
	if (list == 0)
		return;
	for (i = 0; i < list->nSrc; i++) {
		__dbsql_free(NULL, list->a[i].zDatabase);
		__dbsql_free(NULL, list->a[i].zName);
		__dbsql_free(NULL, list->a[i].zAlias);
		if (list->a[i].pTab && list->a[i].pTab->isTransient) {
			__vdbe_delete_table(0, list->a[i].pTab);
		}
		__select_delete(list->a[i].pSelect);
		__expr_delete(list->a[i].pOn);
		__id_list_delete(list->a[i].pUsing);
	}
	__dbsql_free(NULL, list);
}

/*
 * __dbsql_txn_begin --
 *	Begin a transaction
 *
 * PUBLIC: void __dbsql_txn_begin __P((parser_t *, int));
 */
void
__dbsql_txn_begin(parser, on_error)
	parser_t *parser;
	int on_error;
{
	DBSQL *dbp;

	if (parser == 0)
		return;
	dbp = parser->db;
	if (dbp == 0 || dbp->aDb[0].pBt == 0)
		return;
	if (parser->nErr || parser->rc == ENOMEM)
		return;
	if (__auth_check(parser, DBSQL_TRANSACTION, "BEGIN", 0, 0))
		return;
	if (dbp->flags & DBSQL_InTrans) {
		__error_msg(parser,
			    "cannot start a transaction within a transaction");
		return;
	}
	__vdbe_prepare_write(parser, 0, 0);
	dbp->flags |= DBSQL_InTrans;
	dbp->onError = on_error;
}

/*
 * __dbsql_txn_commit --
 *	Commit a transaction.
 *
 * PUBLIC: void __dbsql_txn_commit __P((parser_t *));
 */
void
__dbsql_txn_commit(parser)
	parser_t *parser;
{
	DBSQL *dbp;

	if (parser == 0)
		return;
	dbp = parser->db;
	if (dbp == 0 || dbp->aDb[0].pBt==0 )
		return;
	if (parser->nErr || parser->rc == ENOMEM)
		return;
	if (__auth_check(parser, DBSQL_TRANSACTION, "COMMIT", 0, 0))
		return;
	if ((dbp->flags & DBSQL_InTrans) == 0) {
		__error_msg(parser,
			    "cannot commit - no transaction is active");
		return;
	}
	dbp->flags &= ~DBSQL_InTrans;
	__vdbe_conclude_write(parser);
	dbp->onError = OE_Default;
}

/*
 * __dbsql_txn_abort --
 *	Rollback (aka abort) a transaction.
 *
 * PUBLIC: void __dbsql_txn_abort __P((parser_t *));
 */
void
__dbsql_txn_abort(parser)
	parser_t *parser;
{
	DBSQL *dbp;
	vdbe_t *v;

	if (parser == 0)
		return;
	dbp = parser->db;
	if (dbp == 0 || dbp->aDb[0].pBt == 0)
		return;
	if (parser->nErr || parser->rc == ENOMEM)
		return;
	if (__auth_check(parser, DBSQL_TRANSACTION, "ROLLBACK", 0, 0))
		return;
	if ((dbp->flags & DBSQL_InTrans) == 0) {
		__error_msg(parser,
			    "cannot rollback - no transaction is active");
		return; 
	}
	v = __parser_get_vdbe(parser);
	if (v) {
		__vdbe_add_op(v, OP_Rollback, 0, 0);
	}
	dbp->flags &= ~DBSQL_InTrans;
	dbp->onError = OE_Default;
}

/*
 * __code_verify_schema --
 *	Generate VDBE code that will verify the schema signature for all
 *	named database files.
 *
 * PUBLIC: void __code_verify_schema __P((parser_t *, int));
 */
void __code_verify_schema(parser, idb)
	parser_t *parser;
	int idb;
{
	DBSQL *dbp = parser->db;
	vdbe_t *v = __parser_get_vdbe(parser);
	DBSQL_ASSERT(idb >= 0 && idb < dbp->nDb);
	DBSQL_ASSERT(dbp->aDb[idb].pBt != 0);
	if (idb != 1 && !DB_PROPERTY_HAS_VALUE(dbp, idb, DBSQL_COOKIE)) {
		__vdbe_add_op(v, OP_VerifySchemaSignature, idb,
			      dbp->aDb[idb].schema_sig);
		DB_PROPERTY_SET(dbp, idb, DBSQL_COOKIE);
	}
}

/*
 * __vdbe_prepare_write --
 *	Generate VDBE code that prepares for doing an operation that
 *	might change the database.
 *	This routine starts a new transaction if we are not already within
 *	a transaction.  If we are already within a transaction, then a
 *	checkpoint is set if the 'checkpoint' parameter is true.  A
 *	checkpoint should be set for operations that might fail (due to a
 *	constraint) part of the way through and which will need to undo
 *	some writes without having to rollback the whole transaction.  For
 *	operations where all constraints can be checked before any changes
 *	are made to the database, it is never necessary to undo a write and
 *	the checkpoint should not be set.
 *	Only database 'idb' and the temp database are made writable by this
 *	call.
 *	If idb==0, then the main and temp databases are made writable. If
 *	idb==1 then only the temp database is made writable.  If idb>1 then
 *	the specified auxiliary database and the temp database are made
 *	writable.
 *
 * PUBLIC: void __vdbe_prepare_write __P((parser_t*, int, int));
 */
void
__vdbe_prepare_write(parser, checkpoint, idb)
	parser_t *parser;
	int checkpoint;
	int idb;
{
	vdbe_t *v;
	DBSQL *dbp = parser->db;
	if (DB_PROPERTY_HAS_VALUE(dbp, idb, DBSQL_SCHEMA_LOCKED))
		return;
	v = __parser_get_vdbe(parser);
	if (v == 0)
		return;
	if (!dbp->aDb[idb].inTrans) {
		__vdbe_add_op(v, OP_Transaction, 0, 0);
		DB_PROPERTY_SET(dbp, idb, DBSQL_SCHEMA_LOCKED);
		__code_verify_schema(parser, idb);
		if (idb != 1) {
			__vdbe_prepare_write(parser, checkpoint, 1);
		}
	} else if (checkpoint) {
		__vdbe_add_op(v, OP_Checkpoint, idb, 0);
		DB_PROPERTY_SET(dbp, idb, DBSQL_SCHEMA_LOCKED); /*TODO why?*/
	}
}

/*
 * __vdbe_conclude_write --
 *	Generate code that concludes an operation that may have changed
 *	the database.  If a statement transaction was started, then emit
 *	an OP_Commit that will cause the changes to be committed to disk.
 *	Note that checkpoints are automatically committed at the end of
 *	a statement.  Note also that there can be multiple calls to 
 *	__vdbe_prepare_write() but there should only be a single
 *	call to __vdbe_conclude_write() at the conclusion of the statement.
 *
 * PUBLIC: void __vdbe_conclude_write __P((parser_t *));
 */
void
__vdbe_conclude_write(parser)
	parser_t *parser;
{
	vdbe_t *v;
	DBSQL *dbp = parser->db;
	if (parser->trigStack)
		return; /* if this is in a trigger */
	v = __parser_get_vdbe(parser);
	if (v == 0)
		return;
	if (dbp->flags & DBSQL_InTrans) {
		/* 
		 * A BEGIN has executed.  Do not commit until we see an
		 * explicit COMMIT statement.
		 */
	} else {
		__vdbe_add_op(v, OP_Commit, 0, 0);
	}
}
