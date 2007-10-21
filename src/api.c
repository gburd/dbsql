/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  DBSQL Group, Inc - All rights reserved.
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
 * $Id: api.c 7 2007-02-03 13:34:17Z gburd $
 */

/*
 * Implementation of the programmer public API to the library.
 */

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <ctype.h>
#endif

#include "dbsql_int.h"

DBSQL_GLOBALS __dbsql_global_values;

/*
 * A pointer to this structure is used to communicate information
 * from __init_databases into the __init_callback.
 */
typedef struct {
	DBSQL  *db;          /* The database being initialized */
	char  **err_msgs;    /* Error message stored here */
} init_data_t;

/*
 * __corrupt_schema --
 *	Fill the init_data_t structure with an error message that
 *	indicates that the database is corrupt.
 *
 * STATIC: static void __corrupt_schema __P((init_data_t *));
 */
static void
__corrupt_schema(data)
     init_data_t *data;
{
	__str_append(data->err_msgs, "malformed database schema",
		     (char*)0);
}

/*
 * __init_callback --
 *	This is the callback routine for the code that initializes
 *	the database.  See __initXXX() below for additional information.
 *
 *	Each callback contains the following information:
 *
 *	  argv[0] = "file-format", "schema-cookie", "table", "index"
 *	  argv[1] = table or index name or meta statement type.
 *	  argv[2] = root page number for table or index.  NULL for meta.
 *	  argv[3] = SQL text for a CREATE TABLE or CREATE INDEX statement.
 *	  argv[4] = "1" for temporary files, "0" for main database,
 *	            "2" or more for auxiliary database files.
 *
 * STATIC: static int __init_callback __P((init_data_t *));
 */
static int
__init_callback(init, argc, argv, col_name)
	void *init;
	int argc;
	char **argv;
	char **col_name;
{
	init_data_t *data = (init_data_t*)init;
	parser_t parser;
	int nerr = 0;

	DBSQL_ASSERT(argc == 5);

	/* Might happen if EMPTY_RESULT_CALLBACKS are on */
	if (argv == 0)
		return 0;

	if (argv[0] == 0) {
		__corrupt_schema(data);
		return 1;
	}

	switch(argv[0][0]) {
	case 'v': /* FALLTHROUGH */
	case 'i': /* FALLTHROUGH */
	case 't':
                /* CREATE TABLE, CREATE INDEX, or CREATE VIEW statements */
		if (argv[2] == 0 || argv[4] == 0) {
			__corrupt_schema(data);
			return 1;
		}
		/* 
		 * Call the parser to process a CREATE TABLE, INDEX or VIEW.
		 * But because sParse.initFlag is set to 1, no VDBE code is
		 * generated or executed.  All the parser does is build the
		 * internal data structures that describe the table, index,
		 * or view.
		 */
		if(argv[3] && argv[3][0]) {
			memset(&parser, 0, sizeof(parser));
			parser.db = data->db;
			parser.initFlag = 1;
			parser.iDb = atoi(argv[4]);
			parser.newTnum = atoi(argv[2]);
			parser.useCallback = 1;
			__run_sql_parser(&parser, argv[3], data->err_msgs);
		} else {
			/*
			 * If the SQL column is blank it means this is an
			 * index that was created to be the PRIMARY KEY or
			 * to fulfill a UNIQUE constraint for a CREATE TABLE.
			 * The index should have already been created when
			 * we processed the CREATE TABLE.  All we have to do
			 * here is record the root page number for that index.
			 */
			int dbi;
			index_t *index;

			dbi = atoi(argv[4]);
			DBSQL_ASSERT(dbi >= 0 && dbi < data->db->nDb);
			index = __find_index(data->db, argv[1],
					     data->db->aDb[dbi].zName);
			if (index == 0 || index->tnum != 0) {
				/*
				 * This can occur if there exists an index
				 * on a TEMP table which has the same name
				 * as another index on a permanent index.
				 * Since the permanent table is hidden by
				 * the TEMP table, we can also safely ignore
				 * the index on the permanent table.
				 */
				/* FALLTHRUOUGH */;
			} else {
				index->tnum = atoi(argv[2]);
			}
		}
		break;
	default:
		/* This can not happen! */
		nerr = 1;
		DBSQL_ASSERT(nerr == 0); /* TODO create a __fatal() */
	}
	return nerr;
}

/*
 * __init_db_file --
 *	Attempt to read the database schema and initialize internal
 *	data structures for a single database file.  The index of the
 *	database file is given by 'dbi'.  dbi==0 is used for the main
 *	database.  idb==1 should never be used.  dbi>=2 is used for
 *	auxiliary databases.  Return one of the DBSQL_ error codes to
 *	indicate success or failure.
 *
 * STATIC: static int __init_db_file __P((DBSQL *, int, char **));
 */
static int
__init_db_file(dbp, dbi, err_msgs)
	DBSQL *dbp;
	int dbi;
	char **err_msgs;
{
	int rc;
	int size;
	table_t *table;
	char *args[6];
	char db_num[30];
	parser_t parser;
	init_data_t init_data;

	/*
	 * The master database table has a structure like this
	 */
	static char master_schema[] = 
		"CREATE TABLE " MASTER_NAME "(\n"
		"  type text,\n"
		"  name text,\n"
		"  tbl_name text,\n"
		"  rootpage integer,\n"
		"  sql text\n"
		")"
		;
	static char temp_master_schema[] = 
		"CREATE TEMP TABLE " TEMP_MASTER_NAME "(\n"
		"  type text,\n"
		"  name text,\n"
		"  tbl_name text,\n"
		"  rootpage integer,\n"
		"  sql text\n"
		")"
		;

	/*
	 * The following SQL will read the schema from the master tables.
	 * The rowid for new table entries (including entries in
	 * master is an increasing integer.  So we can play back master
	 * and all the CREATE statements will appear in the right order.
	 */
	static char init_script[] = 
	       "SELECT type, name, rootpage, sql, 1 FROM " TEMP_MASTER_NAME " "
	       "UNION ALL "
	       "SELECT type, name, rootpage, sql, 0 FROM " MASTER_NAME "";

	DBSQL_ASSERT(dbi >= 0 && dbi != 1 && dbi < dbp->nDb);

	/*
	 * Construct the schema tables: master and temp_master
	 */
	args[0] = "table";
	args[1] = MASTER_NAME;
	args[2] = "2";
	args[3] = master_schema;
	sprintf(db_num, "%d", dbi);
	args[4] = db_num;
	args[5] = 0;
	init_data.db = dbp;
	init_data.err_msgs = err_msgs;
	__init_callback(&init_data, 5, args, 0);
	table = __find_table(dbp, MASTER_NAME, "main");
	if (table) {
		table->readOnly = 1;
	}
	if (dbi == 0) {
		args[1] = TEMP_MASTER_NAME;
		args[3] = temp_master_schema;
		args[4] = "1";
		__init_callback(&init_data, 5, args, 0);
		table = __find_table(dbp, TEMP_MASTER_NAME, "temp");
		if(table){
			table->readOnly = 1;
		}
	}

	__sm_get_schema_sig(dbp->aDb[dbi].pBt, &dbp->aDb[dbi].schema_sig);

	if (dbi == 0) {
		dbp->next_sig = dbp->aDb[dbi].schema_sig;
		__sm_get_format_version(dbp->aDb[dbi].pBt,
					&dbp->format_version);
		if (dbp->format_version == 0) {
			/* New, empty database. */
			dbp->format_version = DBSQL_FORMAT_VERSION;
		}
	}

	/*
	 * Read the schema information out of the schema tables.
	 */
	memset(&parser, 0, sizeof(parser));
	parser.db = dbp;
	parser.xCallback = __init_callback;
	parser.pArg = (void*)&init_data;
	parser.initFlag = 1;
	parser.useCallback = 1;
	if(dbi == 0) {
		__run_sql_parser(&parser, init_script, err_msgs);
	} else {
		char *sql = 0;
		__str_append(&sql, "SELECT type, name, rootpage, sql, ",
			     db_num, " FROM \"",
			     dbp->aDb[dbi].zName, "\"." MASTER_NAME,
			     (char*)0);
		__run_sql_parser(&parser, sql, err_msgs);
		__dbsql_free(dbp, sql);
	}
	if (parser.rc == ENOMEM) {
		__str_append(err_msgs, "out of memory", (char*)0);
		parser.rc = DBSQL_NOMEM;
		__reset_internal_schema(dbp, 0);
	}
	if (parser.rc == DBSQL_SUCCESS) {
		DB_PROPERTY_SET(dbp, dbi, DBSQL_SCHEMA_LOADED);
		if (dbi == 0)
			DB_PROPERTY_SET(dbp, 1, DBSQL_SCHEMA_LOADED);
	} else {
		__reset_internal_schema(dbp, dbi);
	}
	return parser.rc;
}

/*
 * __init_databases --
 *	Initialize all database files - the main database file, the file
 *	used to store temporary tables, and any additional database files
 *	created using ATTACH statements.  Return a success code.  If an
 *	error occurs, write an error message into *pzErrMsg.
 *	
 *	After the database is initialized, the DBSQL_Initialized
 *	bit is set in the flags field of the dbsql_t structure.  An
 *	attempt is made to initialize the database as soon as it
 *	is opened.  If that fails (perhaps because another process
 *	has the master table locked) than another attempt
 *	is made the first time the database is accessed.
 *
 * PUBLIC: int __init_databases __P((DBSQL *, char**));
 */
int
__init_databases(dbp, err_msgs)
	DBSQL *dbp;
	char **err_msgs;
{
	int i = 0;
	int rc = DBSQL_SUCCESS;
  
	DBSQL_ASSERT((dbp->flags & DBSQL_Initialized) == 0);

	for(i = 0; rc == DBSQL_SUCCESS && i < dbp->nDb; i++) {
		if (DB_PROPERTY_HAS_VALUE(dbp, i, DBSQL_SCHEMA_LOADED))
			continue;
		DBSQL_ASSERT(i != 1);
		rc = __init_db_file(dbp, i, err_msgs);
	}
	if (rc == DBSQL_SUCCESS) {
		dbp->flags |= DBSQL_Initialized;
		__commit_internal_changes(dbp);
	} else {
		dbp->flags &= ~DBSQL_Initialized;
	}
	return rc;
}

/*
 * __sqldb_init --
 *
 * PUBLIC: int __sqldb_init __P((dbsql_db_t *, DBSQL *, const char *, int,
 * PUBLIC:                  int, int));
 */
int
__sqldb_init(p, dbp, name, temp, mem, init_sm)
	dbsql_db_t *p;
	DBSQL *dbp;
	const char *name;
	int temp;
	int mem;
	int init_sm;
{
	int rc = DBSQL_SUCCESS;

	DBSQL_ASSERT(p != 0);
	DBSQL_ASSERT(dbp != 0);

	p->dbp = dbp;

	__hash_init(&p->tblHash, DBSQL_HASH_STRING, 0);
	__hash_init(&p->idxHash, DBSQL_HASH_STRING, 0);
	__hash_init(&p->trigHash, DBSQL_HASH_STRING, 0);
	__hash_init(&p->aFKey, DBSQL_HASH_STRING, 1);

	__dbsql_strdup(NULL, name, &p->zName);

	if (init_sm) {
		/* Create a Berkeley DB storage manager. */
		if ((rc = __sm_create(dbp, name, temp, mem, &p->pBt))
		    != DBSQL_SUCCESS) {
			__hash_clear(&p->tblHash);
			__hash_clear(&p->idxHash);
			__hash_clear(&p->trigHash);
			__hash_clear(&p->aFKey);
			return rc;
		}
	}

	return DBSQL_SUCCESS;
}

/*
 * __api_open --
 *	An attempt is made to initialize the in-memory data structures that
 *	hold the database schema.  But if this fails (because the schema file
 *	is locked) then that step is deferred until the first call to
 *	DBSQL->exec().
 *
 * STATIC: int __api_open __P((DBSQL *, const char *, int, char **));
 */
int
__api_open(dbp, filename, mode, err_msgs)
	DBSQL *dbp;
	const char *filename;
	int mode; /*TODO: argument not used*/
	char **err_msgs;
{
	int rc, i, mem;

	DBSQL_ASSERT(dbp);
	DBSQL_ASSERT(err_msgs);

	mem = 0;
	*err_msgs = 0;

	if (dbp == 0)
		return DBSQL_CANTOPEN;

	dbp->onError = OE_Default;
	dbp->priorNewRowid = 0;
	dbp->magic = DBSQL_STATUS_BUSY;
	dbp->nDb = 2;
	
	if (__dbsql_calloc(dbp, 2, sizeof(dbsql_db_t), &dbp->aDb) == ENOMEM)
		goto no_mem_on_open1;

	if (filename[0] == ':' && strcmp(filename, ":memory:") == 0)
		mem = 1;

	for (i = 0; i < dbp->nDb; i++) {
		rc = __sqldb_init(&dbp->aDb[i], dbp, filename, 0, mem,
				  (i ? 0 : 1));
		if (rc != DBSQL_SUCCESS) {
			__str_append(err_msgs, "unable to open database: ",
				     filename, (char*)0);
			__dbsql_free(dbp, dbp);
			__str_urealloc(err_msgs);
			return DBSQL_CANTOPEN;
		}
	}

	dbp->aDb[0].zName = "main";
	dbp->aDb[1].zName = "temp";

	/* Attempt to read the schema. */
	if (__dbsql_calloc(dbp, 1, sizeof(hash_t), &dbp->fns) == ENOMEM)
		goto no_mem_on_open2;
	__hash_init((hash_t*)dbp->fns, DBSQL_HASH_STRING, 1);
	__register_builtin_funcs(dbp);
	rc = __init_databases(dbp, err_msgs);
	dbp->magic = DBSQL_STATUS_OPEN;
	if (rc == ENOMEM) {
		dbp->close(dbp);
		goto no_mem_on_open2;
	} else if (rc != DBSQL_SUCCESS && rc != DBSQL_BUSY) {
		dbp->close(dbp);
		__str_urealloc(err_msgs);
		return DBSQL_CANTOPEN;
	} else if (*err_msgs) {
		__dbsql_free(dbp, *err_msgs);
		*err_msgs = 0;
	}

	/* Return a pointer to the newly opened database structure */
	return DBSQL_SUCCESS;

  no_mem_on_open2:
	__dbsql_free(NULL, dbp);
  no_mem_on_open1:
	__str_append(err_msgs, "out of memory", (char*)0);
	__str_urealloc(err_msgs);
	return DBSQL_NOMEM;
}

/*
 * __api_last_inserted_rowid --
 *	Return the ROWID of the most recent insert
 *
 * STATIC: static int __api_last_inserted_rowid __P((DBSQL *));
 */
int
__api_last_inserted_rowid(dbp)
	DBSQL *dbp;
{
	return dbp->lastRowid;
}

/*
 * __api_last_change_count --
 *	Return the number of changes in the most recent call to __api_exec().
 *
 * STATIC: static int __api_last_change_count __P((DBSQL *));
 */
int
__api_last_change_count(dbp)
	DBSQL *dbp;
{
	return dbp->_num_last_changes;
}

/*
 * __api_total_change_count --
 *	Return the total number of changes for all calls since database
 *	open.  Essentially the sum of all _num_last_changes.
 *
 * STATIC: static int __api_total_change_count __P((DBSQL *));
 */
int
__api_total_change_count(dbp)
	DBSQL *dbp;
{
	return dbp->_num_total_changes;
}

/*
 * __api_close --
 *	Close this database releasing all associated resources.
 *
 * STATIC: static int __api_close __P((DBSQL *));
 */
int
__api_close(dbp)
	DBSQL *dbp;
{
	hash_ele_t *i;
	int j;
	dbp->want_to_close = 1;

	if (__safety_check(dbp) || __safety_on(dbp)) {
		/* TODO printf("DID NOT CLOSE\n"); fflush(stdout); */
		return DBSQL_ERROR;
	}
	dbp->magic = DBSQL_STATUS_CLOSED;
	for ( j = 0; j < dbp->nDb; j++) {
		if (dbp->aDb[j].pBt) {
			__sm_close_db(dbp->aDb[j].pBt);
			dbp->aDb[j].pBt = 0;
		}
		if(j >= 2) {
			__dbsql_free(dbp, dbp->aDb[j].zName);
			dbp->aDb[j].zName = 0;
		}
	}
	__reset_internal_schema(dbp, 0);
	DBSQL_ASSERT(dbp->nDb <= 2);
	for (i = __hash_first((hash_t*)dbp->fns); i; i = __hash_next(i)){
		func_def_t *func, *next;
		for (func = (func_def_t*)__hash_data(i); func; func = next){
			next = func->pNext;
			__dbsql_free(dbp, func);
		}
	}
	if (dbp->dbsql_errpfx)
		__dbsql_free(dbp, dbp->dbsql_errpfx);
	__dbsql_free(dbp, dbp->aDb);
	__hash_clear((hash_t*)dbp->fns);
	__dbsql_free(dbp, dbp->fns);
	__dbsql_free(NULL, dbp);
	return DBSQL_SUCCESS;
}

/*
 * __process_sql --
 *	This routine does the work of either __api_exec() or __api_prepare().
 *	It works like __api_exec() if vm == NULL and it works like
 *	__api_prepare() otherwise.
 *
 * STATIC: static int __process_sql __P((DBSQL *, const char *,
 * STATIC:                          dbsql_callback, void *, const char **,
 * STATIC:                          dbsql_stmt_t, char **))
 *
 * dbp			The database on which the SQL executes
 * sql			The SQL to be executed
 * callback		Invoke this callback routine
 * arg			First argument to callback()
 * tail			OUT: Next statement after the first
 * vm			OUT: The virtual machine
 * err_msgs		OUT: Write error messages here
 */
static int
__process_sql(dbp, sql, callback, arg, tail, vm, err_msgs)
	DBSQL *dbp;
	const char *sql;
	dbsql_callback callback;
	void *arg;
	const char **tail;
	dbsql_stmt_t **vm;
	char **err_msgs;
{
	parser_t parser;

	if (err_msgs)
		*err_msgs = 0;
	if (__safety_on(dbp))
		goto exec_misuse;
	if ((dbp->flags & DBSQL_Initialized) == 0) {
		int rc, i = 1;
		while ((rc = __init_databases(dbp, err_msgs)) == DBSQL_BUSY
		     && dbp->xBusyCallback
		     && dbp->xBusyCallback(dbp, dbp->pBusyArg, "", i++) != 0) {
			/* EMPTY */;
		}
		if (rc != DBSQL_SUCCESS) {
			__str_urealloc(err_msgs);
			__safety_off(dbp);
			return rc;
		}
		if (err_msgs) {
			__dbsql_free(dbp, *err_msgs);
			*err_msgs = 0;
		}
	}
        /* TODO: in the __meta subdatabase get:'format_version'
	if (dbp->file_format < 3) {
		__safety_off(dbp);
		__str_append(err_msgs, "obsolete database file format",
		             (char*)0);
		return DBSQL_ERROR;
	}
	*/
	if (dbp->pVdbe == 0)
		dbp->_num_last_changes = 0;
	memset(&parser, 0, sizeof(parser_t));
	parser.db = dbp;
	parser.xCallback = callback;
	parser.pArg = arg;
	parser.useCallback = (vm == 0);
	if (dbp->xTrace)
		dbp->xTrace(dbp->pTraceArg, sql);
#ifdef NDEBUG
	if (F_ISSET(dbp, DBSQL_VdbeTrace))
		parser.trace = stderr;
#endif
	__run_sql_parser(&parser, sql, err_msgs);
	if (parser.rc == ENOMEM) {
		__str_append(err_msgs, "out of memory", (char*)0);
		parser.rc = DBSQL_NOMEM;
		/* TODO __sm_abort_txn(dbp, ); do we need this? */
		__reset_internal_schema(dbp, 0);
		dbp->flags &= ~DBSQL_InTrans;
	}
	if (parser.rc == DBSQL_DONE)
		parser.rc = DBSQL_SUCCESS;
	if (parser.rc != DBSQL_SUCCESS && err_msgs && *err_msgs == 0) {
		__str_append(err_msgs, dbsql_strerror(parser.rc),
			     (char*)0);
	}
	__str_urealloc(err_msgs);
	if (parser.rc == DBSQL_SCHEMA) {
		__reset_internal_schema(dbp, 0);
	}
	if (parser.useCallback == 0) {
		DBSQL_ASSERT(vm);
		*vm = (dbsql_stmt_t*)parser.pVdbe;
		if (tail)
			*tail = parser.zTail;
	}
	if (__safety_off(dbp))
		goto exec_misuse;
	return parser.rc;

  exec_misuse:
	if (err_msgs) {
		*err_msgs = 0;
		__str_append(err_msgs, dbsql_strerror(DBSQL_MISUSE),
			     (char*)0);
		__str_urealloc(err_msgs);
	}
	return DBSQL_MISUSE;
}

/*
 * __api_exec --
 *	Execute SQL code.  Return one of the DBSQL_ success/failure
 *	codes.
 *	If the SQL is a query, then for each row in the query result
 *	the callback() function is called.  'arg' becomes the first
 *	argument to callback().  If callback is NULL then no callback
 *	is invoked, even for queries.
 *
 * STATIC: static int __api_exec __P((DBSQL *, const char *, dbsql_callback,
 * STATIC:                void *, char **));
 *
 * dbp			The database on which the SQL executes
 * sql			The SQL to be executed
 * callback		Invoke this callback routine
 * arg			First argument to callback()
 * err_msgs		Write error messages here
 */
int __api_exec(dbp, sql, callback, arg, err_msgs)
	DBSQL *dbp;
	const char *sql;
	dbsql_callback callback;
	void *arg;
	char **err_msgs;
{
	return __process_sql(dbp, sql, callback, arg, 0, 0, err_msgs);
}

/*
 * __api_prepare --
 *	Compile a single statement of SQL into a virtual machine.  Return one
 *	of the DBSQL_ success/failure codes.
 *
 * STATIC: static int __api_prepare __P((DBSQL *, const char *, const char **,
 * STATIC:                   dbsql_stmt_t **, char **));
 *
 * db			The database on which the SQL executes
 * sql			The SQL to be executed
 * tail			OUT: Next statement after the first
 * stmt			OUT: The virtual machine to run this SQL statement
 * err_msgs		OUT: Write error messages here
 */
int
__api_prepare(dbp, sql, tail, stmt, err_msgs)	
	DBSQL *dbp;
	const char *sql;
	const char **tail;
	dbsql_stmt_t **stmt;
	char **err_msgs;
{
	return __process_sql(dbp, sql, 0, 0, tail, stmt, err_msgs);
}


/*
 * __api_finalize --
 *	The following routine destroys a virtual machine that is created by
 *	the __api_prepare() routine.
 *	The integer returned is an DBSQL_ success/failure code that describes
 *	the result of executing the virtual machine.
 *
 * STATIC: static int __api_finalize __P((dbsql_stmt_t *, char **));
 *
 * stmt			The virtual machine for the statement to be destroyed
 * err_msgs		OUT: Write error messages here
 */
/*
 * TODO REMOVE THIS: An error message is
 * written into memory obtained from malloc and *pzErrMsg is made to
 * point to that error if pzErrMsg is not NULL.  The calling routine
 * should use __api_freemem() to delete the message when it has finished
 * with it.
 */
int
__api_finalize(stmt, err_msgs)
	dbsql_stmt_t *stmt;
	char **err_msgs;
{
	int rc = __vdbe_finalize((vdbe_t*)stmt, err_msgs);
	__str_urealloc(err_msgs);
	return rc;
}

/*
 * __api_reset --
 *	Terminate the current execution of a virtual machine then
 *	reset the virtual machine back to its starting state so that it
 *	can be reused.
 *
 * STATIC: static int __api_reset __P((dbsql_stmt_t *, char **));
 *
 * stmt			The virtual machine for the statement to be reset
 * err_msgs		OUT: Write error messages here
 */
/* TODO REMOVE THIS: Any error message resulting from the prior execution
 * is written into *pzErrMsg.  A success code from the prior execution
 * is returned.
 */
int
__api_reset(stmt, err_msgs)
	dbsql_stmt_t *stmt;
	char **err_msgs;
{
	int rc = __vdbe_reset((vdbe_t*)stmt, err_msgs);
	__vdbe_make_ready((vdbe_t*)stmt, -1, 0, 0, 0);
	__str_urealloc(err_msgs);
	return rc;
}

/*
 * __default_busy_callback --
 *	This routine implements a busy callback that sleeps and tries
 *	again until a timeout value is reached.  The timeout value is
 *	an integer number of milliseconds passed in as the first
 *	argument.
 *
 * STATIC: static int __default_busy_callback __P((DBSQL *, void *,
 * STATIC:                                    const char *, int));
 *
 * timeout		Maximum amount of time to wait
 * not_used		The name of the table that is busy
 * count		Number of times table has been busy
 */
static int
__default_busy_callback(dbp, arg, not_used, count)
	DBSQL *dbp;
	void *arg;
	const char *not_used;
	int count;
{
#if defined(__LP64) || defined(__LP64__)
	u_int64_t timeout = (u_int64_t)arg;
#else
	u_int32_t timeout = (u_int32_t)arg;
#endif
#if DBSQL_MIN_SLEEP_MS==1
	static const char delays[] =
		{ 1, 2, 5, 10, 15, 20, 25, 25,  25,  50,  50,  50, 100};
	static const short int totals[] =
		{ 0, 1, 3,  8, 18, 33, 53, 78, 103, 128, 178, 228, 287};
# define NDELAY (sizeof(delays)/sizeof(delays[0]))
	u_int32_t delay, prior;

	if (count <= NDELAY) {
		delay = delays[count - 1];
		prior = totals[count - 1];
	} else {
		delay = delays[NDELAY - 1];
		prior = totals[NDELAY - 1] + delay * (count - NDELAY - 1);
	}
	if (prior + delay > timeout){
		delay = timeout - prior;
		if (delay <= 0)
			return 0;
	}
	__os_sleep(dbp, 0, delay);
	return 1;
#else
	if ((count + 1) * 1000 > timeout) {
		return 0;
	}
	__os_sleep(dbp, 1, 0);
	return 1;
#endif
}

/*
 * __api_set_busy_callback --
 *	This routine sets the busy callback for the database to the
 *	given callback function with the given argument.
 *
 * STATIC: static void __api_set_busy_callback __P((DBSQL *,
 * STATIC:      int (*)(DBSQL *, void*, const char*, int), void *));
 */
void
__api_set_busy_callback(dbp, busy, arg)
	DBSQL *dbp;
	int (*busy)(DBSQL *, void *, const char*, int);
	void *arg;
{
	dbp->xBusyCallback = busy;
	dbp->pBusyArg = arg;
}

#ifndef DBSQL_NO_PROGRESS
/*
 * __api_set_progress_callback --
 *	This routine sets the progress callback for a database to the
 *	given callback function with the given argument. The progress
 *	callback will be invoked every num_ops opcodes have been executed.
 *
 * STATIC: static void __api_set_progress_callback __P((DBSQL *, int,
 * STATIC:                                 int (*)(void*), void *));
 */
void
__api_set_progress_callback(dbp, num_ops, progress, arg)
	DBSQL *dbp;
	int num_ops;
	int (*progress)(void*);
	void *arg;
{
	if (num_ops > 0) {
		dbp->xProgress = progress;
		dbp->nProgressOps = num_ops;
		dbp->pProgressArg = arg;
	} else {
		dbp->xProgress = 0;
		dbp->nProgressOps = 0;
		dbp->pProgressArg = 0;
	}
}
#endif


/*
 * __api_set_busy_timeout --
 *	This routine installs a default busy handler that waits for the
 *	specified number of milliseconds before returning 0.
 *
 * STATIC: static void __api_set_busy_timeout __P((DBSQL *, int));
 */
void
__api_set_busy_timeout(dbp, ms)
	DBSQL *dbp;
	int ms;
{
#if defined(__LP64) || defined(__LP64__)
	u_int64_t delay = ms;
#else
	u_int32_t delay = ms;
#endif
	
	if (ms > 0)
		dbp->set_busycall(dbp, __default_busy_callback, (void*)delay);
	else
		dbp->set_busycall(dbp, 0, 0);
}

/*
 * __api_interrupt --
 *	Cause any pending operation to stop at its earliest opportunity.
 *
 * PUBLIC: void __api_interrupt __P((DBSQL *));
 */
void
__api_interrupt(dbp)
	DBSQL *dbp;
{
	dbp->flags |= DBSQL_Interrupt;
}

/*
 * dbsql_version --
 *	Return the version of the library.
 *
 * EXTERN: const char *dbsql_version __P((int *, int *, int *));
 */
const char *
dbsql_version(major, minor, patch)
	int *major;
	int *minor;
	int *patch;
{
	*major = DBSQL_VERSION_MAJOR;
	*minor = DBSQL_VERSION_MINOR;
	*patch = DBSQL_VERSION_PATCH;
	return DBSQL_VERSION_STRING;
}

/*
 * __api_get_encoding --
 *	Return the string encoding used by the library.
 *
 * STATIC: static const char * __api_get_encoding __P((void));
 */
const char *
__api_get_encoding()
{
	return DBSQL_GLOBAL(encoding);
}

/*
 * __api_create_function --
 *	Create new user-defined functions.  This routine creates a
 *	regular scalar function or an aggregate function.
 *	The function pointers passed (func, step and finalize), implement
 *	the function or aggregate and how to dispose of its data.
 *	A scalar function should supply the func callback and NULL for
 *	step and finalize.  An aggregate function should supply step and
 *	finalize but not func by passing NULL for that parameter. To remove
 *	a function or aggregate, pass NULL for all three callbacks (func,
 *	step, and finalize).
 *	If 'num_arg' is -1 it means that this function will accept any number
 *	of arguments, including 0.
 *
 * STATIC: static int __api_create_function __P((DBSQL *, const char *, int,
 * STATIC:                       int, void *,
 * STATIC:                       void (*)(dbsql_func_t *, int, const char**),
 * STATIC:                       void (*)(dbsql_func_t *, int, const char**),
 * STATIC:                       void (*)(dbsql_func_t *)));
 *
 * dbp			Add the function to this database connection
 * name			Name of the function to add
 * num_arg		Number of arguments
 * encoding		The encoding expected by the functions
 * user_data		User data
 * func			The function's implementation
 * step			Step is used by aggregate functions
 * finalize	                When finished with 
 */
int
__api_create_function(dbp, name, num_arg, user_data, encoding, func,
		      step, finalize)
	DBSQL *dbp;
	const char *name;
	int num_arg;
	void *user_data;
	int encoding;/*TODO: not yet used*/
	void (*func)(dbsql_func_t*, int, const char**);
	void (*step)(dbsql_func_t*, int, const char**);
	void (*finalize)(dbsql_func_t*);
{
	func_def_t *p;
	int name_len;

	if (dbp == NULL || __safety_check(dbp))
		return DBSQL_ERROR;

	if((name == NULL) ||
	   (func && (finalize || step)) ||
	   (!func && (finalize && !step)) ||
	   (!func && (!finalize && step)) ||
	   (num_arg < -1 || num_arg > 127))
		return DBSQL_ERROR;

	name_len = strlen(name);
	if (name_len > 255)
		return DBSQL_ERROR;

	p = __find_function(dbp, name, name_len, num_arg, 1);
	if (p == 0)
		return DBSQL_ERROR;
	p->xFunc = func;
	p->xStep = step;
	p->xFinalize = finalize;
	p->pUserData = user_data;

	return DBSQL_SUCCESS;
}

/*
 * __api_exec_printf --
 *
 * STATIC: static int __api_exec_printf __P((DBSQL *, const char *,
 * STATIC:                              dbsql_callback, void *, char **, ...));
 * STATIC:      __attribute__ ((__format__ (__printf__, 2, 5)));
 *
 * dbp				The DBSQL database
 * fmt				Printf-style format string for the SQL
 * callback			Callback function
 * arg				1st argument to callback function
 * err_msgs			Error msg written here
 * ...				Arguments to the format string
 */
static int
#ifdef STDC_HEADERS
__api_exec_printf(DBSQL *dbp, const char *fmt, dbsql_callback callback,
		  void *arg, char **err_msgs, ...)
#else
__api_exec_printf(dbp, fmt, callback, arg, err_msgs, ...)
	DBSQL *dbp;
	const char *fmt;
	dbsql_callback callback;
	void *arg;
	char **err_msgs;
	va_dcl
#endif
{
	va_list ap;
	int rc;

	va_start(ap, err_msgs);
	rc = dbp->exec_vprintf(dbp, fmt, callback, arg, err_msgs, ap);
	va_end(ap);
	return rc;
}

/*
 * __api_exec_vprintf --
 *
 * STATIC: static int __api_exec_vprintf __P((DBSQL *, const char *,
 * STATIC:                        dbsql_callback, void *, char **, va_list));
 *
 * dbp				The DBSQL database
 * fmt				Printf-style format string for the SQL
 * callback			Callback function
 * arg				1st argument to callback function
 * err_msgs			Error msg written here
 * va_list			Args list
 */
static int
__api_exec_vprintf(dbp, fmt, callback, arg, err_msgs, ap)
	DBSQL *dbp;
	const char *fmt;
	dbsql_callback callback;
	void *arg;
	char **err_msgs;
	va_list ap;
{
	char *sql;
	int rc;

	sql = xvprintf(dbp, fmt, ap);
	rc = dbp->exec(dbp, sql, callback, arg, err_msgs);
	__dbsql_free(dbp, sql);
	return rc;
}

/*
 * __api_exec_table_printf --
 *
 * STATIC: static int __api_exec_table_printf __P((DBSQL *, const char *,
 * STATIC:            char ***, int *, int *, char **, ...));
 * STATIC:      __attribute__ ((__format__ (__printf__, 2, 7)));
 *
 * dbp				The DBSQL database
 * fmt				Printf-style format string for the SQL
 * results			The results as an array of arrays of strings.
 * num_rows			Number of rows returned
 * num_cols			Number of cols in each row
 * err_msgs			Error msg written here
 * ...				Arguments to the format string
 */
static int
#ifdef STDC_HEADERS
__api_exec_table_printf(DBSQL *dbp, const char *fmt, char ***results,
			int *num_rows, int *num_cols, char **err_msgs, ...)
#else
__api_exec_printf(dbp, fmt, results, num_rows, num_cols, err_msgs, ...)
	DBSQL *dbp;
	const char *fmt;
	char ***results;
	int *num_rows;
	int *num_cols;
	char **err_msgs;
	va_dcl
#endif
{
	va_list ap;
	int rc;

	va_start(ap, err_msgs);
	rc = dbp->exec_table_printf(dbp, fmt, results, num_rows, num_cols,
				    err_msgs, ap);
	va_end(ap);
	return rc;
}

/*
 * __api_exec_table_vprintf --
 *
 * STATIC: static int __api_exec_table_vprintf __P((DBSQL *, const char *,
 * STATIC:            char ***, int *, int *, char **, va_list));
 *
 * dbp				The DBSQL database
 * fmt				Printf-style format string for the SQL
 * results			The results as an array of arrays of strings.
 * num_rows			Number of rows returned
 * num_cols			Number of cols in each row
 * err_msgs			Error msg written here
 * ap				Arguments to the format string
 */
static int
__api_exec_table_vprintf(dbp, fmt, results, num_rows, num_cols, err_msgs, ap)
	DBSQL *dbp;
	const char *fmt;
	char ***results;
	int *num_rows;
	int *num_cols;
	char **err_msgs;
	va_list ap;
{
	char *sql;
	int rc;

	sql = xvprintf(dbp, fmt, ap);
	rc = dbp->get_table(dbp, sql, results, num_rows, num_cols, err_msgs);
	__dbsql_free(dbp, sql);
	return rc;
}

/*
 * __api_func_return_type --
 *	Change the datatype for all functions with a given name.  See the
 *	header comment for the prototype of this function in dbsql.h for
 *	additional information.
 *
 * STATIC: static int __api_func_return_type __P((DBSQL *, const char *, int));
 */
int
__api_func_return_type(dbp, name, data_type)
	DBSQL *dbp;
	const char *name;
	int data_type;
{
	func_def_t *p = (func_def_t*)__hash_find((hash_t*)dbp->fns, name,
						 strlen(name));
	while(p) {
		p->dataType = data_type; 
		p = p->pNext;
	}
	return DBSQL_SUCCESS;
}

/*
 * __api_set_trace_callback --
 *	Register a trace function.  The 'arg' from the previously
 *	registered trace is returned.  
 *	A NULL trace function means that no tracing is executes.  A non-NULL
 *	trace is a pointer to a function that is invoked at the start of each
 *	__api_exec().
 *
 * STATIC: static void *__api_set_trace_callback __P((DBSQL *,
 * STATIC:                   void (*trace)(void*, const char *), void *));
 */
void *
__api_set_trace_callback(dbp, trace, arg)
	DBSQL *dbp;
	void (*trace)(void*,const char*);
	void *arg;
{
	void *old = dbp->pTraceArg;
	dbp->xTrace = trace;
	dbp->pTraceArg = arg;
	return old;
}

/*
 * __api_set_commit_callback --
 *	Register a function to be invoked when a transaction comments.
 *	If either function returns non-zero, then the commit becomes a
 *	rollback.
 *
 * STATIC: static void *__api_set_commit_callback __P((DBSQL *,
 * STATIC:              int (*)(void *), void *));
 *
 * dbp			Attach the hook to this database
 * callback		Function to invoke on each commit
 * arg			Argument to the function
 */
void *
__api_set_commit_callback(dbp, callback, arg)
	DBSQL *dbp;
	int (*callback)(void*);
	void *arg;
{
	void *old = dbp->pCommitArg;
	dbp->xCommitCallback = callback;
	dbp->pCommitArg = arg;
	return old;
}

/*
 * __api_get_dbenv --
 *
 * STATIC: static DB_ENV *__api_get_dbenv __P((DBSQL *));
 */
static DB_ENV *
__api_get_dbenv(dbp)
	DBSQL *dbp;
{
	return dbp->dbenv;
}

/*
 * __api_set_errcall --
 *	Register a function when an error occurs.
 *
 * STATIC: static void __api_set_errcall __P((DBSQL *,
 * STATIC:              void (*)(const char *, char *)));
 *
 * dbp			Attach the hook to this database
 * callback		Function to invoke on each commit
 */
void
__api_set_errcall(dbp, callback)
	DBSQL *dbp;
	void (*callback)(const char *, char *);
{
	dbp->dbsql_errcall = callback;
}

/*
 * __api_set_errfile --
 *	Register a FILE* for use when logging error messages.
 *
 * STATIC: static void __api_set_errfile __P((DBSQL *, FILE *));
 *
 * dbp			Attach the hook to this database
 * file			Open file stream for suitable for writing
 */
void
__api_set_errfile(dbp, file)
	DBSQL *dbp;
	FILE *file;
{
	dbp->dbsql_errfile = file;
}

/*
 * __api_get_errfile --
 *	Get the FILE* registered for error messages.
 *
 * STATIC: static void __api_get_errfile __P((DBSQL *, FILE **));
 *
 * dbp			Attach the hook to this database
 * file			OUT: The file used for error messages
 */
void
__api_get_errfile(dbp, file)
	DBSQL *dbp;
	FILE **file;
{
	*file = dbp->dbsql_errfile;
}

/*
 * __api_set_errpfx --
 *	Set a prefix for use when writting error messages.
 *
 * STATIC: static void __api_set_errpfx __P((DBSQL *, const char *));
 *
 * dbp			Attach the hook to this database
 * prefix		A prefix string
 */
void
__api_set_errpfx(dbp, prefix)
	DBSQL *dbp;
	const char *prefix;
{
	__dbsql_strdup(dbp, prefix, &dbp->dbsql_errpfx);
}

/*
 * __api_get_errpfx --
 *	Get the error prefix.
 *
 * STATIC: static void __api_get_errpfx __P((DBSQL *, char **));
 *
 * dbp			Attach the hook to this database
 * prefix		OUT: The prefix string
 */
void
__api_get_errpfx(dbp, prefix)
	DBSQL *dbp;
	const char **prefix;
{
	*prefix = dbp->dbsql_errpfx;
}

/*
 * dbsql_create_env --
 *	Create a basic DB_ENV and then a DBSQL manager within it.
 *
 * EXTERN: int dbsql_create_env __P((DBSQL **dbp, const char *,
 * EXTERN:                      const char *, int, u_int32_t flags));
 */
int
dbsql_create_env(dbpp, dir, crypt, mode, flags)
	DBSQL **dbpp;
	const char *dir;
	const char *crypt;
	int mode;
	u_int32_t flags;
{
	int rc;
	DB_ENV *dbenv;
	int dir_p = 0;
	int file_p = 0;
	int env_open_flags = DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL |
		DB_INIT_TXN | DB_CREATE;

        /* Setup the DB_ENV with that directory as DB_HOME */
	if ((rc = db_env_create(&dbenv, 0)) != 0) {
		__dbsql_err(NULL, db_strerror(rc));
		return DBSQL_CANTOPEN;
	}

	/* Check for a directory to house the database. */
	if (dir == 0 || dir[0] == '\0') {
		/* When dir is NULL, place all resources in memory. */
		env_open_flags |=  DB_PRIVATE;
		dbenv->set_flags(dbenv, DB_LOG_INMEMORY, 1);
                /* Specify the size of the in-memory log buffer. */
		if ((rc = dbenv->set_lg_bsize(dbenv, 10 * 1024 * 1024)) != 0) {
			__dbsql_err(NULL, db_strerror(rc));
			return DBSQL_CANTOPEN;
		}
	} else {
		if (__os_exists(dir, &dir_p) == 0) {
			if (dir_p) {
				char buf[1024];
				snprintf(buf, 1024, "%s%s%s", dir,
					 PATH_SEPARATOR, dir);
				if (__os_exists(buf, &dir_p) == 0)
					env_open_flags = DB_JOINENV;
			} else {
				__dbsql_err(NULL,
				       "Environment must be a directory.");
				return DBSQL_INVALID_NAME;
			}
		} else { /* TODO __db_omode("rwxrwxrwx"):mode) != 0) */
			if (mkdir(dir, mode == 0 ? 0777 : mode) != 0)
				return errno;
		}
	}

	if (LF_ISSET(DBSQL_THREAD))
		env_open_flags |= DB_THREAD;

	if ((rc = dbenv->set_lk_detect(dbenv, DB_LOCK_DEFAULT)) != 0) {
		__dbsql_err(NULL, db_strerror(rc));
		dbenv->close(dbenv, 0);
		return DBSQL_CANTOPEN;
	}

	if ((rc = dbenv->set_cachesize(dbenv, 0, 1 * 1024 * 1024, 1)) != 0) {
		__dbsql_err(NULL, db_strerror(rc));
		dbenv->close(dbenv, 0);
		return DBSQL_CANTOPEN;
	}

	if (crypt && crypt[0]) {
		if ((rc = dbenv->set_encrypt(dbenv, crypt,
					     DB_ENCRYPT_AES)) != 0) {
			__dbsql_err(NULL, db_strerror(rc));
			dbenv->close(dbenv, 0);
			return DBSQL_CANTOPEN;
		}
	}

	if ((rc = dbenv->open(dbenv, dir, env_open_flags, mode)) != 0) {
		__dbsql_err(NULL, db_strerror(rc));
		dbenv->close(dbenv, 0);
		return DBSQL_CANTOPEN;
	}

#ifdef DEBUG
	dbenv->set_verbose(dbenv, DB_VERB_WAITSFOR |
			   DB_VERB_DEADLOCK | DB_VERB_RECOVERY, 1);
#endif

	return dbsql_create(dbpp, dbenv, flags);
}

/*
 * dbsql_create --
 *	Create a new sql database.  Construct a DBSQL structure to hold
 *	the state of this database and return a pointer.
 *
 * EXTERN: int dbsql_create __P((DBSQL **, DB_ENV *, u_int32_t));
 */
int
dbsql_create(dbpp, dbenv, flags)
	DBSQL **dbpp;
	DB_ENV *dbenv;
	u_int32_t flags;
{
	DBSQL *dbp;
	DBSQL_ASSERT(dbpp != 0);
	
	if (dbenv == NULL)
		return EINVAL; /* TODO better error message */

	/*
	 * Does the library expect data to be encoded as UTF-8
	 * or iso8859?  The following global constant always
	 * lets us know.
	 * TODO: Make this configurable as a flag and part of the meta
	 * database.
	 */
#if DBSQL_UTF8_ENCODING
	DBSQL_GLOBAL(encoding) = "UTF-8";
#else
	DBSQL_GLOBAL(encoding) = "iso8859";
#endif
	
	if (__dbsql_calloc(NULL, 1, sizeof(DBSQL), &dbp) == ENOMEM)
		return DBSQL_NOMEM;

	if (LF_ISSET(DBSQL_THREAD))
		F_SET(dbp, DBSQL_Threaded);
	if (LF_ISSET(DBSQL_DURABLE_TEMP))
		F_SET(dbp, DBSQL_DurableTemp);

	dbp->dbenv = dbenv;
	dbp->encoding = __api_get_encoding;
	dbp->open = __api_open;
	dbp->close = __api_close;
	dbp->rowid = __api_last_inserted_rowid;
	dbp->last_change_count = __api_last_change_count;
	dbp->total_change_count = __api_total_change_count;
	dbp->interrupt = __api_interrupt;
	dbp->set_errcall = __api_set_errcall;
	dbp->set_errfile = __api_set_errfile;
	dbp->get_errfile = __api_get_errfile;
	dbp->set_errpfx = __api_set_errpfx;
	dbp->get_errpfx = __api_get_errpfx;
	dbp->get_dbenv = __api_get_dbenv;
	dbp->set_tracecall = __api_set_trace_callback;
#ifndef DBSQL_NO_PROGRESS
	dbp->set_progresscall = __api_set_progress_callback;
#endif
	dbp->set_commitcall = __api_set_commit_callback;
	dbp->set_busycall = __api_set_busy_callback;
	dbp->set_timeout = __api_set_busy_timeout;
	dbp->get_table = __api_get_table;
	dbp->free_table = __api_free_table;
	dbp->exec_printf = __api_exec_printf;
	dbp->exec_vprintf = __api_exec_vprintf;
	dbp->exec_table_printf = __api_exec_table_printf;
	dbp->exec_table_vprintf = __api_exec_table_vprintf;
	dbp->exec = __api_exec;
	dbp->create_function = __api_create_function;
	dbp->func_return_type = __api_func_return_type;
	dbp->set_authorizer = __api_set_authorizer;
	dbp->prepare = __api_prepare;
	dbp->step = __api_step;
	dbp->finalize = __api_finalize;
	dbp->reset = __api_reset;
	dbp->bind = __api_bind;
	*dbpp = dbp;
	return DBSQL_SUCCESS;
}
