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
 * A TCL Shell, dbsql_tclsh, for testing DBSQL.
 */

#include <dbsql.h>
#include <dbsql_config.h>
#include <dbsql_int.h>
#include "inc/os_ext.h"

#include <tcl.h>

#include <stdlib.h>
#include <string.h>

extern void __tcl_sql_func_md5step(dbsql_func_t *, int, const char **);
extern void __tcl_sql_func_md5finalize(dbsql_func_t *);


/*
 * If TCL uses UTF-8 and DBSQL is configured to use iso8859, then we
 * have to do a translation when going between the two.  Set the
 * UTF_TRANSLATION_NEEDED macro to indicate that we need to do
 * this translation.
 */
#if defined(TCL_UTF_MAX) && !defined(DBSQL_UTF8_ENCODING)
#define UTF_TRANSLATION_NEEDED 1
#endif

/*
 * New SQL functions can be created as TCL scripts.  Each such function
 * is described by an instance of the following structure.
 */
typedef struct sql_func sql_func_t;
struct sql_func {
	sql_func_t *next;   /* Next function on the list of them all */
	Tcl_Interp *interp; /* The TCL interpret to execute the function */
	char *script;       /* The script to be run */
};

/*
 * There is one instance of this structure for each database
 * that has been opened by this inteface.
 */
typedef struct dbsql_ctx dbsql_ctx_t;
struct dbsql_ctx {
	DB_ENV *dbenv;        /* The Berkeley DB Environment */
	DBSQL *dbp;           /* The SQL datbase within the dbenv */
	Tcl_Interp *interp;   /* The interpreter used for this database */
	char *busy;           /* The busy callback routine */
	char *commit;         /* The commit hook callback routine */
	char *trace;          /* The trace callback routine */
	char *progress;       /* The progress callback routine */
	char *auth;           /* The authorization callback routine */
	int crypt;            /* Non-zero if environment is encrypted */
	sql_func_t *func;     /* List of SQL functions */
	int rc;               /* Return code of most recent dbsql_exec() */
};

/*
 * An instance of this structure passes information thru the internal
 * logic from the original TCL command into the callback routine.
*/
typedef struct callback_data callback_data_t;
struct callback_data {
	Tcl_Interp *interp;    /* The TCL interpreter */
	char *array;           /* The array into which data is written */
	Tcl_Obj *code;         /* The code to execute for each row */
	int once;              /* Set for first callback only */
	int tcl_rc;            /* Return code from TCL script */
	int ncols;             /* Number of entries in the col_names[] array */
	char **col_names;      /* Column names translated to UTF-8 */
};

/* __testset_1 --------------------------------------------------------------*/
#ifdef DB_WIN32
#define PTR_FMT "%x"
#else
#define PTR_FMT "%p"
#endif

/*
 * get_dbsql_from_ptr --
 *	Decode a pointer encoded in a string to an pointer to a structure.
 */
static int
get_dbsql_from_ptr(interp, args, dbsqlp)
	Tcl_Interp *interp;
	const char *args;
	DBSQL **dbsqlp;
{
	if (sscanf(args, PTR_FMT, (void**)dbsqlp) != 1 &&
	    (args[0] != '0' || args[1] != 'x' ||
	     sscanf(&args[2], PTR_FMT, (void**)dbsqlp) != 1)) {
		Tcl_AppendResult(interp,
			 "\"", args, "\" is not a valid pointer value", 0);
		return TCL_ERROR;
	}
	return TCL_OK;
}

/*
 * get_sqlvm_from_ptr --
 *	Decode a pointer to an sqlvm_t object.
 */
static int
get_sqlvm_from_ptr(interp, args, sqlvmp)
	Tcl_Interp *interp;
	const char *args;
	dbsql_stmt_t **sqlvmp;
{
	if (sscanf(args, PTR_FMT, (void**)sqlvmp) != 1) {
		Tcl_AppendResult(interp,
			    "\"", args, "\" is not a valid pointer value", 0);
		return TCL_ERROR;
	}
	return TCL_OK;
}

/*
 * __encode_as_ptr --
 *	Generate a text representation of a pointer that can be understood
 *	by the get_dbsql_from_ptr and get_sqlvm_from_ptr routines above.
 *
 *	The problem is, on some machines (Solaris) if you do a printf with
 *	"%p" you cannot turn around and do a scanf with the same "%p" and
 *	get your pointer back.  You have to prepend a "0x" before it will
 *	work.  But this behavior varies from machine to machine.  This
 *	work around tests the string after it is generated to see if it can be
 *	understood by scanf, and if not, try prepending an "0x" to see if
 *	that helps.  If nothing works, a fatal error is generated.
 */
static int
__encode_as_ptr(interp, ptr, p)
	Tcl_Interp *interp;
	char *ptr;
	void *p;
{
	void *p2;

	sprintf(ptr, PTR_FMT, p);
	if( sscanf(ptr, PTR_FMT, &p2) != 1 || p2 != p) {
		sprintf(ptr, "0x" PTR_FMT, p);
		if (sscanf(ptr, PTR_FMT, &p2) != 1 || p2 != p) {
			Tcl_AppendResult(interp,
	    "unable to convert a pointer to a string "
	    "in the file " __FILE__ " in function __encode_as_ptr().  Please "
            "report this problem to support@dbsql.org as a new "
            "report.  Please provide detailed information about how "
	    "you compiled DBSQL (include your config.log file) and what "
            "hardware and operating system you are using.", 0);
			return TCL_ERROR;
		}
	}
	return TCL_OK;
}

/*
 * t__dbsql_env_create --
 *	TCL usage:    dbsql_create name
 *	Returns:  The name of an open database.
 */
static int
t__dbsql_env_create(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	char *err_msgs;
	char ptr[100];
	int rc;

	COMPQUIET(_dbctx, NULL);

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " FILENAME\"", 0);
		return TCL_ERROR;
	}

	rc = dbsql_create_env(&dbp, argv[1], NULL, 0, DBSQL_THREAD);
	if (rc != DBSQL_SUCCESS) {
		Tcl_AppendResult(interp, dbsql_strerror(rc), 0);
		return TCL_ERROR;
	}
	if (__encode_as_ptr(interp, ptr, dbp))
		return TCL_ERROR;
	Tcl_AppendResult(interp, ptr, 0);
	return TCL_OK;
}

/*
 * exec_printf_cb --
 *	The callback routine for DBSQL->exec_printf().
 */
static int
exec_printf_cb(arg, argc, argv, name)
	void *arg;
	int argc;
	char **argv;
	char **name;
{
	Tcl_DString *str;
	int i;

	str = (Tcl_DString*)arg;
	if (Tcl_DStringLength(str) == 0) {
		for(i = 0; i < argc; i++) {
			Tcl_DStringAppendElement(str, name[i] ?
						 name[i] : "NULL");
		}
	}
	for(i = 0; i < argc; i++) {
		Tcl_DStringAppendElement(str, argv[i] ? argv[i] : "NULL");
	}
	return 0;
}

/*
 * t__exec_printf --
 *	TCL usage:  exec_printf  DBSQL  FORMAT  STRING
 *
 *	Invoke the dbsql_exec_printf() interface using the open database
 *	DB.  The SQL is the string FORMAT.  The format string should contain
 *	one %s or %q.  STRING is the value inserted into %s or %q.
 */
static int
t__exec_printf(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	Tcl_DString str;
	char *err = 0;
	char buf[30];
	int rc;

	COMPQUIET(_dbctx, NULL);

	if (argc != 4) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " DB FORMAT STRING", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	Tcl_DStringInit(&str);
	rc = dbp->exec_printf(dbp, argv[2], exec_printf_cb,
			      &str, &err, argv[3]);
	sprintf(buf, "%d", rc);
	Tcl_AppendElement(interp, buf);
	Tcl_AppendElement(interp, rc == DBSQL_SUCCESS ?
			  Tcl_DStringValue(&str) : err);
	Tcl_DStringFree(&str);
	if (err)
		free(err);
	return TCL_OK;
}

#if 0
FIXME
/*
 * test_xprintf --
 *	Usage:  xprintf  SEPARATOR  ARG0  ARG1 ...
 *
 *	Test the %z format of xprintf().  Use multiple xprintf() calls to
 *	concatenate arg0 through argn using separator as the separator.
 *	Return the result.
 */
static int
test_xprintf(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	int i;
	char *result = 0;

	COMPQUIET(_dbctx, NULL);

	if (get_dbsql_from_ptr(interp, argv[1], &db))
		return TCL_ERROR;

	for(i = 2; i < argc; i++) {
		result = exec_xprintf(dbctx->dbp, "%z%s%s",
				      result, argv[1], argv[i]);
	}
	Tcl_AppendResult(interp, result, 0);
	__dbsql_free(dbctx->dbp, result);
	return TCL_OK;
}
#endif

/*
 * t__get_table_printf --
 *	TCL usage:  dbsql_get_table_printf  DB  FORMAT  STRING
 *
 *	Invoke the dbsql_get_table_printf() interface using the open database
 *	DB.  The SQL is the string FORMAT.  The format string should contain
 *	one %s or %q.  STRING is the value inserted into %s or %q.
 */
static int
t__get_table_printf(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	Tcl_DString str;
	int rc;
	char *err = 0;
	int nrow, ncol;
	char **result;
	int i;
	char buf[30];

	COMPQUIET(_dbctx, NULL);

	if (argc != 4) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " DB FORMAT STRING", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	Tcl_DStringInit(&str);
	rc = dbp->exec_table_printf(dbp, argv[2], &result, &nrow, &ncol,
				    &err, argv[3]);
	sprintf(buf, "%d", rc);
	Tcl_AppendElement(interp, buf);
	if (rc == DBSQL_SUCCESS) {
		sprintf(buf, "%d", nrow);
		Tcl_AppendElement(interp, buf);
		sprintf(buf, "%d", ncol);
		Tcl_AppendElement(interp, buf);
		for (i = 0; i < (nrow + 1) * ncol; i++) {
			Tcl_AppendElement(interp, result[i] ?
					  result[i] : "NULL");
		}
	} else {
		Tcl_AppendElement(interp, err);
	}
	dbp->free_table(result);
	if (err)
		free(err);
	return TCL_OK;
}


/*
 * t__last_rowid --
 *	TCL usage:  t__last_inserted_rowid DB
 *
 *	Returns the integer ROWID of the most recent insert.
 */
static int
t__last_rowid(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	dbsql_ctx_t *dbctx = (dbsql_ctx_t *)_dbctx;
	char buf[30];

	COMPQUIET(_dbctx, NULL);

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " DB\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	sprintf(buf, "%d", dbp->rowid(dbp));
	Tcl_AppendResult(interp, buf, 0);
	return DBSQL_SUCCESS;
}

/*
 * t__test_close --
 *	TCL usage:  dbsql_close DB
 *
 *	Closes the database.
 */
static int
t__test_close(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;

	COMPQUIET(_dbctx, NULL);

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " FILENAME\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	dbp->close(dbp);
	return TCL_OK;
}

/*
 * ifnull_func --
 *	Implementation of the x_coalesce() function.
 *	Return the first argument non-NULL argument.
 */
static void
ifnull_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	int i;

	for(i = 0; i < argc; i++) {
		if (argv[i]) {
			dbsql_set_result_string(context, argv[i], -1);
			break;
		}
	}
}

/*
 * A structure into which to accumulate text.
 */
struct dstr {
	int size;	/* Space allocated */
	int len;	/* Space used */
	char *str;	/* The string */
};

/*
 * dstr_append --
 *	Append text to a dstr.
 */
static void
dstr_append(p, z, divider)
	struct dstr *p;
	const char *z;
	int divider;
{
	int n = strlen(z);
	if (p->len + n + 2 > p->size) {
		char *zNew;
		p->size = p->size * 2 + n + 200;
		if (__dbsql_realloc(NULL, p->size, &p->str) == ENOMEM)
			return;
	}
	if (divider && p->len > 0) {
		p->str[p->len++] = divider;
	}
	memcpy(&p->str[p->len], z, n + 1);
	p->len += n;
}

/*
 * exec_func_callback --
 *	Invoked for each callback from dbsql_exec
 */
static int
exec_func_callback(data, argc, argv, notused)
	void *data;
	int argc;
	char **argv;
	char **notused;
{
	struct dstr *p;
	int i;

	COMPQUIET(notused, NULL);
	p = (struct dstr*)data;

	for(i = 0; i < argc; i++) {
		if (argv[i] == 0) {
			dstr_append(p, "NULL", ' ');
		} else {
			dstr_append(p, argv[i], ' ');
		}
	}
	return 0;
}

/*
 * exec_func --
 *	Implementation of the x_dbsql_exec() function.  This function takes
 *	a single argument and attempts to execute that argument as SQL code.
 *	This is illegal and should set the DBSQL_MISUSE flag on the database.
 *	This routine simulates the effect of having two threads attempt to
 *	use the same database at the same time.
 */
static void
exec_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	DBSQL *dbp = (DBSQL*)dbsql_user_data(context);
	struct dstr x;
	memset(&x, 0, sizeof(x));
	dbp->exec(dbp, argv[0], exec_func_callback, &x, 0);
	dbsql_set_result_string(context, x.str, x.len);
	__dbsql_free(NULL, x.str);
}

/*
 * t__create_function --
 *	TCL usage:  dbsql_test_create_function DB
 *
 *	Call the dbsql_create_function API on the given database in order
 *	to create a function named "x_coalesce".  This function does the same
 *	thing as the "coalesce" function.  This function also registers an
 *	SQL function named "x_dbsql_exec" that invokes dbsql_exec().
 *	Invoking dbsql_exec() in this way is illegal recursion and should
 *	raise an DBSQL_MISUSE error.  The effect is similar to trying to use
 *	the same database connection from
 *	two threads at the same time.
 *
 *	The original motivation for this routine was to be able to call the
 *	dbsql_create_function function while a query is in progress in order
 *	to test the DBSQL_MISUSE detection logic.
 */
static int
t__create_function(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	extern void Md5_Register(DBSQL*);

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " FILENAME\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;

	dbp->create_function(dbp, "x_coalesce", -1, DBSQL_UTF8_ENCODED,
			     NULL, ifnull_func, NULL, NULL);
	dbp->create_function(dbp, "x_dbsql_exec", -1, DBSQL_UTF8_ENCODED,
			     NULL, exec_func, NULL, NULL);

	return TCL_OK;
}

/*
 * Routines to implement the x_count() aggregate function.
 */
typedef struct cnt {
	int n;
} cnt_t;

/*
 * count_step --
 *	
 */
static void
count_step(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	cnt_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	if ((argc == 0 || argv[0]) && p) {
		p->n++;
	}
}

/*
 * count_finalize --
 *	
 */
static void
count_finalize(context)
	dbsql_func_t *context;
{
	cnt_t *p;
	p = dbsql_aggregate_context(context, sizeof(*p));
	dbsql_set_result_int(context, p ? p->n : 0);
}

/*
 * t__create_aggregate --
 *	TCL usage:  dbsql_test_create_aggregate DB
 *
 *	Call the dbsql_create_function API on the given database in order
 *	to create a function named "x_count".  This function does the same
 *	thing as the "md5sum" function.
 *
 *	The original motivation for this routine was to be able to call the
 *	dbsql_create_aggregate function while a query is in progress in order
 *	to test the DBSQL_MISUSE detection logic.
 */
static int
t__create_aggregate(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;

	COMPQUIET(_dbctx, NULL);

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " FILENAME\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	dbp->create_function(dbp, "x_count", 0, DBSQL_UTF8_ENCODED,
			     NULL, NULL, count_step, count_finalize);

	return TCL_OK;
}

#if 0
TODO
/*
 * dbsql_mprintf_int --
 *	Usage:  dbsql_mprintf_int FORMAT INTEGER INTEGER INTEGER
 *
 *	Call mprintf with three integer arguments.
 */
static int
dbsql_mprintf_int(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	int a[3], i;
	char *buf;

	COMPQUIET(_dbctx, NULL);

	if (argc != 5) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " FORMAT INT INT INT\"", 0);
		return TCL_ERROR;
	}
	for(i = 2; i < 5; i++) {
		if (Tcl_GetInt(interp, argv[i], &a[i-2]))
			return TCL_ERROR;
	}
	buf = __mprintf(argv[1], a[0], a[1], a[2]);
	Tcl_AppendResult(interp, buf, 0);
	__dbsql_free(NULL, buf);
	return TCL_OK;
}

/*
 * dbsql_mprintf_str --
 *	Usage:  dbsql_mprintf_str FORMAT INTEGER INTEGER STRING
 *
 *	Call mprintf with two integer arguments and one string argument.
 */
static int
dbsql_mprintf_str(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	int a[3], i;
	char *buf;

	COMPQUIET(_dbctx, NULL);

	if (argc < 4 || argc > 5) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " FORMAT INT INT ?STRING?\"", 0);
		return TCL_ERROR;
	}
	for (i = 2; i < 4; i++) {
		if (Tcl_GetInt(interp, argv[i], &a[i-2]))
			return TCL_ERROR;
	}
	buf = __mprintf(argv[1], a[0], a[1], argc > 4 ? argv[4] : NULL);
	Tcl_AppendResult(interp, buf, 0);
	dbsql_free(buf);
	return TCL_OK;
}

/*
 * dbsql_mprintf_double --
 * Usage:  dbsql_mprintf_str FORMAT INTEGER INTEGER DOUBLE
 *
 *	Call mprintf with two integer arguments and one double argument
 */
static int
dbsql_mprintf_double(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	int a[3], i;
	double r;
	char *buf;

	COMPQUIET(_dbctx, NULL);

	if (argc != 5) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " FORMAT INT INT STRING\"", 0);
		return TCL_ERROR;
	}
	for (i = 2; i < 4; i++) {
		if (Tcl_GetInt(interp, argv[i], &a[i-2]))
			return TCL_ERROR;
	}
	if (Tcl_GetDouble(interp, argv[4], &r))
		return TCL_ERROR;
	buf = __mprintf(argv[1], a[0], a[1], r);
	Tcl_AppendResult(interp, buf, 0);
	dbsql_free(buf);
	return TCL_OK;
}
#endif

/*
 * dbsql_malloc_fail --
 *	Usage: dbsql_malloc_fail N
 *
 *	Rig __dbsql_calloc() to fail on the N-th call.  Turn off this mechanism
 *	and reset the dbsql_malloc_failed variable is N==0.
 */
#ifdef MEMORY_DEBUG
static int
dbsql_malloc_fail(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	int n;

	COMPQUIET(_dbctx, NULL);

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " N\"", 0);
		return TCL_ERROR;
	}
	if (Tcl_GetInt(interp, argv[1], &n))
		return TCL_ERROR;
	sqlite_iMallocFail = n;
	sqlite_malloc_failed = 0;
	return TCL_OK;
}
#endif

/*
 * dbsql_malloc_stat --
 *	Usage: dbsql_malloc_stat
 *
 *	Return the number of prior calls to __dbsql_calloc()
 *	and __dbsql_free().
 */
#ifdef MEMORY_DEBUG
static int
dbsql_malloc_stat(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	char buf[200];

	COMPQUIET(_dbctx, NULL);

	sprintf(buf, "%d %d %d", sqlite_nMalloc, sqlite_nFree,
		sqlite_iMallocFail);
	Tcl_AppendResult(interp, buf, 0);
	return TCL_OK;
}
#endif

/*
 * t__dbsql_abort --
 *	TCL usage:  dbsql_abort
 *
 *	Shutdown the process immediately.  This is not a clean shutdown.
 *	This command is used to test the recoverability of a database in
 *	the event of a program crash.
 */
static int
t__dbsql_abort(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	COMPQUIET(_dbctx, NULL);

	DBSQL_ASSERT(interp == 0);   /* This will always fail. */
	return TCL_OK;
}

/*
 * test_func --
 *	The following routine is a user-defined SQL function whose purpose
 *	is to test the dbsql_set_result_string() API.
 */
static void
test_func(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	while(argc >= 2) {
		if (argv[0] == 0) {
			dbsql_set_result_error(context,
			  "first argument to test function may "
                          "not be NULL", -1);
		} else if (strcasecmp(argv[0], "string") == 0) {
			dbsql_set_result_string(context, argv[1], -1);
		} else if (argv[1] == 0) {
			dbsql_set_result_error(context,
                          "2nd argument may not be NULL if the "
                          "first argument is not \"string\"", -1);
		} else if (strcasecmp(argv[0], "int") == 0) {
			dbsql_set_result_int(context, atoi(argv[1]));
		} else if (strcasecmp(argv[0], "double") == 0) {
			dbsql_set_result_double(context,
						__dbsql_atof(argv[1]));
		} else {
			dbsql_set_result_error(context,
			  "first argument should be one of: "
                          "string int double", -1);
		}
		argc -= 2;
		argv += 2;
	}
}

/*
 * t__register_func --
 *	TCL usage:   dbsql_register_test_function  DB  NAME
 *
 *	Register the test SQL function on the database DB under the name NAME.
 */
static int
t__register_func(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	int rc;

	COMPQUIET(_dbctx, NULL);

	if (argc != 3) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " DB FUNCTION-NAME", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	rc = dbp->create_function(dbp, argv[2], -1, DBSQL_UTF8_ENCODED,
				  NULL, test_func, NULL, NULL);
	if (rc != 0) {
		Tcl_AppendResult(interp, dbsql_strerror(rc), 0);
		return TCL_ERROR;
	}
	return TCL_OK;
}

/*
 * __remember_data_types --
 *	Usage:   This callback records the datatype of all columns.
 *
 *	Column names are inserted as the result of this interpreter.
 *	Return non-zero should cause the query to abort.
 */
static int
__remember_data_types(interp, cols, argv, colv)
	Tcl_Interp *interp;
	int cols;
	char **argv;
	char **colv;
{
	int i;
	Tcl_Obj *list, *elem;

	if (colv[cols + 1] == 0)
		return 1;
	list = Tcl_NewObj();
	for (i = 0; i < cols; i++) {
		elem = Tcl_NewStringObj(colv[i + cols] ? colv[i + cols] :
					"NULL", -1);
		Tcl_ListObjAppendElement(interp, list, elem);
	}
	Tcl_SetObjResult(interp, list);
	return 1;
}

/*
 * t__dbsql_datatypes --
 *	TCL usage:   This callback records the datatype of all columns.
 *
 *	Invoke an SQL statement but ignore all the data in the result.
 *	Instead, return a list that consists of the datatypes of the
 *	various columns.
 *
 *	This only works if "PRAGMA show_datatypes=on" has been executed
 *	against the database connection.
 */
static int
t__dbsql_datatypes(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	int rc;

	COMPQUIET(_dbctx, NULL);

	if (argc != 3) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " DB SQL", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	rc = dbp->exec(dbp, argv[2], __remember_data_types, interp, 0);
	if (rc != 0 && rc != DBSQL_ABORT) {
		Tcl_AppendResult(interp, dbsql_strerror(rc), 0);
		return TCL_ERROR;
	}
	return TCL_OK;
}

/*
 * t__dbsql_compile --
 *	TCL usage:  dbsql_compile  DB  SQL  ?TAILVAR?
 *
 *	Attempt to compile an SQL statement.  Return a pointer to the virtual
 *	machine used to execute that statement.  Unprocessed SQL is written
 *	into TAILVAR.
 */
static int
t__dbsql_compile(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	dbsql_stmt_t *vm;
	int rc;
	char *err = 0;
	const char *tail;
	char buf[50];

	COMPQUIET(_dbctx, NULL);

	if (argc != 3 && argc !=4) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " DB SQL TAILVAR", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	rc = dbp->prepare(dbp, argv[2], argc==4 ? &tail : 0, &vm, &err);
	if (argc == 4)
		Tcl_SetVar(interp, argv[3], tail, 0);
	if (rc) {
		DBSQL_ASSERT(vm == 0);
		sprintf(buf, "(%d) ", rc);
		Tcl_AppendResult(interp, buf, err, 0);
		free(err); /* TODO this was a dbsql_freemem call... */
		return TCL_ERROR;
	}
	if (vm) {
		if (__encode_as_ptr(interp, buf, vm))
			return TCL_ERROR;
		Tcl_AppendResult(interp, buf, 0);
	}
	return TCL_OK;
}

/*
 * t__dbsql_step --
 *	TCL usage:  dbsql_step  VM  ?NVAR?  ?VALUEVAR?  ?COLNAMEVAR?
 *
 *	Step a virtual machine.  Return a the result code as a string.
 *	Column results are written into three variables.
 */
static int
t__dbsql_step(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	dbsql_stmt_t *vm;
	int rc, i;
	const char **values = 0;
	const char **names = 0;
	int n = 0;
	char *result;
	char buf[50];

	COMPQUIET(_dbctx, NULL);

	if (argc < 2 || argc > 5) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " VM NVAR VALUEVAR COLNAMEVAR", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	if (get_sqlvm_from_ptr(interp, argv[1], &vm))
		return TCL_ERROR;
	rc = dbp->step(vm,
		       argc >= 3 ? &n : 0,
		       argc >= 4 ? &values : 0,
		       argc == 5 ? &names : 0);
	if (argc >= 3) {
		sprintf(buf, "%d", n);
		Tcl_SetVar(interp, argv[2], buf, 0);
	}
	if (argc >= 4) {
		Tcl_SetVar(interp, argv[3], "", 0);
		if (values) {
			for(i = 0; i < n; i++) {
				Tcl_SetVar(interp, argv[3],
					  values[i] ? values[i] : "",
					  TCL_APPEND_VALUE | TCL_LIST_ELEMENT);
			}
		}
	}
	if (argc == 5) {
		Tcl_SetVar(interp, argv[4], "", 0);
		if (names) {
			for(i = 0; i < n * 2; i++) {
				Tcl_SetVar(interp, argv[4],
					  names[i] ? names[i] : "",
					  TCL_APPEND_VALUE | TCL_LIST_ELEMENT);
			}
		}
	}
	switch(rc) {
	case DBSQL_DONE:   result = "DBSQL_DONE";    break;
	case DBSQL_BUSY:   result = "DBSQL_BUSY";    break;
	case DBSQL_ROW:    result = "DBSQL_ROW";     break;
	case DBSQL_ERROR:  result = "DBSQL_ERROR";   break;
	case DBSQL_MISUSE: result = "DBSQL_MISUSE";  break;
	default:           result = "unknown";       break;
	}
	Tcl_AppendResult(interp, result, 0);
	return TCL_OK;
}

/*
 * t__dbsql_finalize
 *	TCL usage:  dbsql_close_sqlvm  VM
 *
 *	Shutdown a virtual machine.
 */
static int
t__dbsql_finalize(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	dbsql_stmt_t *vm;
	int rc;
	char *err_msg = 0;
	char buf[50];

	COMPQUIET(_dbctx, NULL);

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " VM\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	if (get_sqlvm_from_ptr(interp, argv[1], &vm))
		return TCL_ERROR;
	rc = dbp->finalize(vm, &err_msg);
	if (rc) {
		sprintf(buf, "(%d) ", rc);
		Tcl_AppendResult(interp, buf, err_msg, 0);
		free(err_msg); /* TODO, who allocated this memory? */
		return TCL_ERROR;
	}
	return TCL_OK;
}

/*
 * t__dbsql_test --
 *	TCL usage:  dbsql_reset_sqlvm   VM
 *
 *	Reset a virtual machine and prepare it to be run again.
 */
static int
t__dbsql_reset(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	dbsql_stmt_t *vm;
	int rc;
	char *err_msg = 0;
	char buf[50];

	COMPQUIET(_dbctx, NULL);

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " VM\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	if (get_sqlvm_from_ptr(interp, argv[1], &vm))
		return TCL_ERROR;
	rc = dbp->reset(vm, &err_msg);
	if (rc) {
		sprintf(buf, "(%d) ", rc);
		Tcl_AppendResult(interp, buf, err_msg, 0);
		free(err_msg); /* TODO who allocated this memory? */
		return TCL_ERROR;
	}
	return TCL_OK;
}

/*
 * This is the "static_bind_value" that variables are bound to when
 * the FLAG option of dbsql_bind is "static"
 */
static char *dbsql_static_bind_value = 0;

/*
 * t__dbsql_bind --
 *	TCL usage:  dbsql_bind  VM  IDX  VALUE  FLAGS
 *
 *	Sets the value of the IDX-th occurance of "?" in the original SQL
 *	string.  VALUE is the new value.  If FLAGS=="null" then VALUE is
 *	ignored and the value is set to NULL.  If FLAGS=="static" then
 *	the value is set to the value of a static variable named
 *	"dbsql_static_bind_value".  If FLAGS=="normal" then a copy
 *	of the VALUE is made.
 */
static int
t__dbsql_bind(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	dbsql_stmt_t *vm;
	int rc;
	int idx;
	char buf[50];

	COMPQUIET(_dbctx, NULL);

	if (argc != 5) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " VM IDX VALUE (null|static|normal)\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	if (get_sqlvm_from_ptr(interp, argv[1], &vm))
		return TCL_ERROR;
	if (Tcl_GetInt(interp, argv[2], &idx))
		return TCL_ERROR;
	if (strcmp(argv[4],"null") == 0) {
		rc = dbp->bind(vm, idx, 0, 0, 0);
	} else if (strcmp(argv[4], "static") == 0) {
		rc = dbp->bind(vm, idx, dbsql_static_bind_value, -1, 0);
	} else if (strcmp(argv[4], "normal") == 0) {
		rc = dbp->bind(vm, idx, argv[3], -1, 1);
	} else {
		Tcl_AppendResult(interp, "4th argument should be "
				 "\"null\" or \"static\" or \"normal\"", 0);
		return TCL_ERROR;
	}
	if (rc) {
		sprintf(buf, "(%d) ", rc);
		Tcl_AppendResult(interp, buf, dbsql_strerror(rc), 0);
		return TCL_ERROR;
	}
	return TCL_OK;
}

/*
 * t__dbsql_breakpoint --
 *	TCL usage:    breakpoint
 *
 *	This routine exists for one purpose - to provide a place to put a
 *	breakpoint with GDB that can be triggered using TCL code.  The use
 *	for this is when a particular test fails on (say) the 1485th iteration.
 *	In the TCL test script, we can add code like this:
 *
 *	if {$i==1485} breakpoint
 *
 *	Then run testfixture in the debugger and wait for the breakpoint to
 *	fire.  Then additional breakpoints can be set to trace down the bug.
 */
static int
t__dbsql_breakpoint(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	COMPQUIET(_dbctx, NULL);

	return TCL_OK; /* Do nothing */
}

/*
 *	Register commands with the TCL interpreter.
 */
int
__testset_1_init(interp)
	Tcl_Interp *interp;
{
	extern int dbsql_search_count;
	extern int dbsql_interrupt_count;
	extern int dbsql_open_file_count;
	extern int _fake_current_time;
	int i;
	static struct {
		char *name;
		Tcl_CmdProc *proc;
	} cmds[] = {
#if 0
TODO
     { "dbsql_mprintf_int",             (Tcl_CmdProc*)dbsql_mprintf_int    },
     { "dbsql_mprintf_str",             (Tcl_CmdProc*)dbsql_mprintf_str    },
     { "dbsql_mprintf_double",          (Tcl_CmdProc*)dbsql_mprintf_double },
     { "__mprintf_z_test",              (Tcl_CmdProc*)test_mprintf_z       },
#endif
     { "dbsql_env_create",              (Tcl_CmdProc*)t__dbsql_env_create  },
     { "dbsql_last_inserted_rowid",     (Tcl_CmdProc*)t__last_rowid        },
     { "dbsql_exec_printf",             (Tcl_CmdProc*)t__exec_printf       },
     { "dbsql_get_table_printf",        (Tcl_CmdProc*)t__get_table_printf  },
     { "dbsql_close",                   (Tcl_CmdProc*)t__test_close        },
     { "dbsql_create_function",         (Tcl_CmdProc*)t__create_function   },
     { "dbsql_create_aggregate",        (Tcl_CmdProc*)t__create_aggregate  },
     { "dbsql_register_test_function",  (Tcl_CmdProc*)t__register_func     },
     { "dbsql_abort",                   (Tcl_CmdProc*)t__dbsql_abort       },
     { "dbsql_datatypes",               (Tcl_CmdProc*)t__dbsql_datatypes   },
#ifdef MEMORY_DEBUG
     { "dbsql_malloc_fail",             (Tcl_CmdProc*)dbsql_malloc_fail    },
     { "dbsql_malloc_stat",             (Tcl_CmdProc*)dbsql_malloc_stat    },
#endif
     { "dbsql_compile",                 (Tcl_CmdProc*)t__dbsql_compile     },
     { "dbsql_step",                    (Tcl_CmdProc*)t__dbsql_step        },
     { "dbsql_close_sqlvm",             (Tcl_CmdProc*)t__dbsql_finalize    },
     { "dbsql_bind",                    (Tcl_CmdProc*)t__dbsql_bind        },
     { "dbsql_reset_sqlvm",             (Tcl_CmdProc*)t__dbsql_reset       },
     { "breakpoint",                    (Tcl_CmdProc*)t__dbsql_breakpoint  },
  };

	for(i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		Tcl_CreateCommand(interp, cmds[i].name,
				  cmds[i].proc, 0, 0);
	}
	Tcl_LinkVar(interp, "dbsql_search_count",
		    (char*)&dbsql_search_count, TCL_LINK_INT);
	Tcl_LinkVar(interp, "dbsql_interrupt_count",
		    (char*)&dbsql_interrupt_count, TCL_LINK_INT);
#if 0
FIXME
	Tcl_LinkVar(interp, "dbsql_open_file_count",
		    (char*)&dbsql_open_file_count, TCL_LINK_INT);
#endif
	Tcl_LinkVar(interp, "dbsql_current_time",
		    (char*)&_fake_current_time, TCL_LINK_INT);
	Tcl_LinkVar(interp, "dbsql_static_bind_value",
		    (char*)&dbsql_static_bind_value, TCL_LINK_STRING);
	return TCL_OK;
}
/* end of __testset_1 -------------------------------------------------------*/

#ifdef UTF_TRANSLATION_NEEDED
/*
 * __tcl_process_row --
 *	Called for each row of the result.
 *	This version is used when TCL expects UTF-8 data but the database
 *	uses the ISO-8859 format.  A translation must occur from ISO-8859 into
 *	UTF-8.
 *
 * _data			An instance of callback_data_t
 * ncol				Number of columns in the result
 * col				Data for each column
 * col_names			Name for each column
 */
static int
__tcl_process_row(_data, ncol, col, col_names)
	void *_data;
	int ncol;
	char **col;
	char **col_names;
{
	callback_data_t *data = (callback_data_t*)_data;
	int i, rc;
	Tcl_DString tcol;
	Tcl_DStringInit(&tcol);
	if (data->col_names == 0) {
		DBSQL_ASSERT(data->once);
		data->once = 0;
		if (data->array[0]) {
			Tcl_SetVar2(data->interp, data->array, "*", "", 0);
		}
		data->col_names = malloc(ncol * sizeof(char*));
		if (data->col_names == 0) {
			return 1;
		}
		data->ncols = ncol;
		for (i = 0; i < ncol; i++) {
			Tcl_ExternalToUtfDString(NULL, col_names[i],
						 -1, &tcol);
			data->col_names[i] =malloc(Tcl_DStringLength(&tcol)+1);
			if (data->col_names[i]) {
				strcpy(data->col_names[i],
				       Tcl_DStringValue(&tcol));
			} else {
				return 1;
			}
			if (data->array[0]) {
				Tcl_SetVar2(data->interp, data->array, "*",
					    Tcl_DStringValue(&tcol),
					    TCL_LIST_ELEMENT |
					    TCL_APPEND_VALUE);
				if (col_names[ncol] != 0) {
					Tcl_DString type;
					Tcl_DStringInit(&type);
					Tcl_DStringAppend(&type,"typeof:", -1);
					Tcl_DStringAppend(&type,
						 Tcl_DStringValue(&tcol), -1);
					Tcl_DStringFree(&tcol);
					Tcl_ExternalToUtfDString(NULL,
						 col_names[i+ncol], -1, &tcol);
					Tcl_SetVar2(data->interp, data->array, 
						    Tcl_DStringValue(&type),
						    Tcl_DStringValue(&tcol),
						    TCL_LIST_ELEMENT |
						    TCL_APPEND_VALUE);
					Tcl_DStringFree(&type);
				}
			}
			Tcl_DStringFree(&tcol);
		}
	}
	if (col != 0) {
		if (data->array[0]) {
			for (i = 0; i < ncol; i++) {
				char *z = col[i];
				if (z == 0)
					z = "";
				Tcl_DStringInit(&tcol);
				Tcl_ExternalToUtfDString(NULL, z, -1, &tcol);
				Tcl_SetVar2(data->interp, data->array,
					    data->col_names[i], 
					    Tcl_DStringValue(&tcol), 0);
				Tcl_DStringFree(&tcol);
			}
		} else {
			for (i = 0; i < ncol; i++) {
				char *z = col[i];
				if (z == 0)
					z = "";
				Tcl_DStringInit(&tcol);
				Tcl_ExternalToUtfDString(NULL, z, -1, &tcol);
				Tcl_SetVar(data->interp, data->col_names[i],
					   Tcl_DStringValue(&tcol), 0);
				Tcl_DStringFree(&tcol);
			}
		}
	}
	rc = Tcl_EvalObj(data->interp, data->code);
	if (rc == TCL_CONTINUE)
		rc = TCL_OK;
	data->tcl_rc = rc;
	return (rc != TCL_OK);
}
#endif /* UTF_TRANSLATION_NEEDED */

#ifndef UTF_TRANSLATION_NEEDED
/*
 * __tcl_process_row --
 *	Called for each row of the result.
 *	This version is used when either of the following is true:
 *	(1) This version of TCL uses UTF-8 and the data in the
 *	    database is already in the UTF-8 format.
 *	(2) This version of TCL uses ISO-8859 and the data in the
 *	    database is already in the ISO-8859 format.
 *
 * _data			An instance of callback_data_t
 * ncol				Number of columns in the result
 * col				Data for each column
 * col_names			Name for each column
 */
static int
__tcl_process_row(_data, ncol, col, col_names)
	void *_data;
	int ncol;
	char** col;
	char **col_names;
{
	callback_data_t *data = (callback_data_t*)_data;
	int i, rc;
	if (col == 0 || (data->once && data->array[0])) {
		Tcl_SetVar2(data->interp, data->array, "*", "", 0);
		for (i = 0; i < ncol; i++) {
			Tcl_SetVar2(data->interp, data->array, "*",
				    col_names[i],
				    TCL_LIST_ELEMENT | TCL_APPEND_VALUE);
			if (col_names[ncol]) {
				char *z;
				__dbsql_calloc(NULL, 7 + strlen(col_names[i]),
					    sizeof(char), &z);
				sprintf(z, "typeof:%s", col_names[i]);
				Tcl_SetVar2(data->interp, data->array, z,
					    col_names[i + ncol],
					    TCL_LIST_ELEMENT |
					    TCL_APPEND_VALUE);
				__dbsql_free(NULL, z);
			}
		}
		data->once = 0;
	}
	if (col != 0) {
		if (data->array[0]) {
			for (i = 0; i < ncol; i++) {
				char *z = col[i];
				if (z == 0)
					z = "";
				Tcl_SetVar2(data->interp, data->array,
					    col_names[i], z, 0);
			}
		} else {
			for (i = 0; i < ncol; i++) {
				char *z = col[i];
				if (z == 0)
					z = "";
				Tcl_SetVar(data->interp, col_names[i], z, 0);
			}
		}
	}
	rc = Tcl_EvalObj(data->interp, data->code);
	if (rc == TCL_CONTINUE)
		rc = TCL_OK;
	data->tcl_rc = rc;
	return rc!=TCL_OK;
}
#endif

/*
 * __tcl_process_row2 --
 *	This is an alternative callback for database queries.  Instead
 *	of invoking a TCL script to handle the result, this callback just
 *	appends each column of the result to a list.  After the query
 *	is complete, the list is returned.
 *
 * _data			An instance of callback_data_t
 * ncol				Number of columns in the result
 * col				Data for each column
 * col_names			Name for each column
 */
static int
__tcl_process_row2(_data, ncol, col, col_names)
	void *_data;
	int ncol;
	char ** col;
	char ** col_names;
{
	int i;
	Tcl_Obj *elem;
	Tcl_Obj *list = (Tcl_Obj*)_data;
	if (col == 0)
		return 0;
	for (i = 0; i < ncol; i++) {
		if (col[i] && *col[i]) {
#ifdef UTF_TRANSLATION_NEEDED
			Tcl_DString tcol;
			Tcl_DStringInit(&tcol);
			Tcl_ExternalToUtfDString(NULL, col[i], -1, &tcol);
			elem = Tcl_NewStringObj(Tcl_DStringValue(&tcol), -1);
			Tcl_DStringFree(&tcol);
#else
			elem = Tcl_NewStringObj(col[i], -1);
#endif
		} else {
			elem = Tcl_NewObj();
		}
		Tcl_ListObjAppendElement(0, list, elem);
	}
	return 0;
}

/*
 * __tcl_process_row3 --
 *	This is a second alternative callback for database queries.  A the
 *	first column of the first row of the result is made the TCL result.
 *
 * _data			An instance of callback_data_t
 * ncol				Number of columns in the result
 * col				Data for each column
 * col_names			Name for each column
 */
static int
__tcl_process_row3(_data, ncol, col, col_names)
	void *_data;
	int ncol;
	char **col;
	char **col_names;
{
	Tcl_Interp *interp = (Tcl_Interp*)_data;
	Tcl_Obj *elem;
	if (col == 0)
		return 1;
	if (ncol == 0)
		return 1;
#ifdef UTF_TRANSLATION_NEEDED
	{
	Tcl_DString tcol;
	Tcl_DStringInit(&tcol);
	Tcl_ExternalToUtfDString(NULL, col[0], -1, &tcol);
	elem = Tcl_NewStringObj(Tcl_DStringValue(&tcol), -1);
	Tcl_DStringFree(&tcol);
	}
#else
	elem = Tcl_NewStringObj(col[0], -1);
#endif
	Tcl_SetObjResult(interp, elem);
	return 1;
}

/*
 * __tcl_delete_cmd
 *	Called when the command is deleted.
 */
static void
__tcl_delete_cmd(_dbctx)
	void *_dbctx;
{
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;
	dbctx->dbp->close(dbctx->dbp);
	while (dbctx->func) {
		sql_func_t *func = dbctx->func;
		dbctx->func = func->next;
		Tcl_Free((char*)func);
	}
	if (dbctx->busy) {
		Tcl_Free(dbctx->busy);
	}
	if (dbctx->trace) {
		Tcl_Free(dbctx->trace);
	}
	if (dbctx->auth) {
		Tcl_Free(dbctx->auth);
	}
	Tcl_Free((char*)dbctx);
}

/*
 * __tcl_busy_handler
 *	This routine is called when a database file is locked while trying
 *	to execute SQL.
 */
static int
__tcl_busy_handler(cd, table, tries)
	void *cd;
	const char *table;
	int tries;
{
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)cd;
	int rc;
	char val[30];
	char *command;
	Tcl_DString cmd;

	Tcl_DStringInit(&cmd);
	Tcl_DStringAppend(&cmd, dbctx->busy, -1);
	Tcl_DStringAppendElement(&cmd, table);
	sprintf(val, " %d", tries);
	Tcl_DStringAppend(&cmd, val, -1);
	command = Tcl_DStringValue(&cmd);
	rc = Tcl_Eval(dbctx->interp, command);
	Tcl_DStringFree(&cmd);
	if (rc != TCL_OK || atoi(Tcl_GetStringResult(dbctx->interp))) {
		return 0;
	}
	return 1;
}

/*
 * __tcl_progress_handler --
 *	This routine is invoked as the 'progress callback' for the database.
 */
static int
__tcl_progress_handler(cd)
	void *cd;
{
	int rc;
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)cd;

	DBSQL_ASSERT(dbctx->progress);
	rc = Tcl_Eval(dbctx->interp, dbctx->progress);
	if (rc != TCL_OK || atoi(Tcl_GetStringResult(dbctx->interp))) {
		return 1;
	}
	return 0;
}

/*
 * __tcl_trace_handler --
 *	This routine is called by the DBSQL trace handler whenever a new
 *	block of SQL is executed.  The TCL script in dbctx->trace is executed.
 */
static void
__tcl_trace_handler(_dbctx, sql)
	void *_dbctx;
	const char *sql;
{
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;
	Tcl_DString str;

	Tcl_DStringInit(&str);
	Tcl_DStringAppend(&str, dbctx->trace, -1);
	Tcl_DStringAppendElement(&str, sql);
	Tcl_Eval(dbctx->interp, Tcl_DStringValue(&str));
	Tcl_DStringFree(&str);
	Tcl_ResetResult(dbctx->interp);
}

/*
 * __tcl_commit_handler --
 *	This routine is called when a transaction is committed.  The
 *	TCL script in dbctx->commit is executed.  If it returns non-zero or
 *	if it throws an exception, the transaction is rolled back instead
 *	of being committed.
 */
static int
__tcl_commit_handler(_dbctx)
	void *_dbctx;
{
	int rc;
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;

	rc = Tcl_Eval(dbctx->interp, dbctx->commit);
	if (rc != TCL_OK || atoi(Tcl_GetStringResult(dbctx->interp))) {
		return 1;
	}
	return 0;
}

/*
 * __tcl_eval_sql_fn
 *	This routine is called to evaluate a SQL function implemented
 *	using TCL script.
 */
static void
__tcl_eval_sql_fn(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	int i;
	int rc;
	sql_func_t *p = dbsql_user_data(context);
	Tcl_DString cmd;

	Tcl_DStringInit(&cmd);
	Tcl_DStringAppend(&cmd, p->script, -1);
	for (i = 0; i < argc; i++) {
		Tcl_DStringAppendElement(&cmd, argv[i] ? argv[i] : "");
	}
	rc = Tcl_Eval(p->interp, Tcl_DStringValue(&cmd));
	if (rc) {
		dbsql_set_result_error(context,
					Tcl_GetStringResult(p->interp), -1); 
	} else {
		dbsql_set_result_string(context,
					 Tcl_GetStringResult(p->interp), -1);
	}
}

#ifndef DBSQL_NO_AUTH
/*
** This is the authentication function.  It appends the authentication
** type code and the two arguments to cmd[] then invokes the result
** on the interpreter.  The reply is examined to determine if the
** authentication fails or succeeds.
*/
static int
__tcl_auth_callback(_dbctx, c, arg1, arg2, arg3, arg4)
	void *_dbctx;
	int c;
	const char *arg1;
	const char *arg2;
	const char *arg3;
	const char *arg4;
{
	int rc;
	char *code;
	Tcl_DString str;
	const char *reply;
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;

	switch(c) {
	case DBSQL_COPY              : code="DBSQL_COPY"; break;
	case DBSQL_CREATE_INDEX      : code="DBSQL_CREATE_INDEX"; break;
	case DBSQL_CREATE_TABLE      : code="DBSQL_CREATE_TABLE"; break;
	case DBSQL_CREATE_TEMP_INDEX : code="DBSQL_CREATE_TEMP_INDEX"; break;
	case DBSQL_CREATE_TEMP_TABLE : code="DBSQL_CREATE_TEMP_TABLE"; break;
	case DBSQL_CREATE_TEMP_TRIGGER:code="DBSQL_CREATE_TEMP_TRIGGER"; break;
	case DBSQL_CREATE_TEMP_VIEW  : code="DBSQL_CREATE_TEMP_VIEW"; break;
	case DBSQL_CREATE_TRIGGER    : code="DBSQL_CREATE_TRIGGER"; break;
	case DBSQL_CREATE_VIEW       : code="DBSQL_CREATE_VIEW"; break;
	case DBSQL_DELETE            : code="DBSQL_DELETE"; break;
	case DBSQL_DROP_INDEX        : code="DBSQL_DROP_INDEX"; break;
	case DBSQL_DROP_TABLE        : code="DBSQL_DROP_TABLE"; break;
	case DBSQL_DROP_TEMP_INDEX   : code="DBSQL_DROP_TEMP_INDEX"; break;
	case DBSQL_DROP_TEMP_TABLE   : code="DBSQL_DROP_TEMP_TABLE"; break;
	case DBSQL_DROP_TEMP_TRIGGER : code="DBSQL_DROP_TEMP_TRIGGER"; break;
	case DBSQL_DROP_TEMP_VIEW    : code="DBSQL_DROP_TEMP_VIEW"; break;
	case DBSQL_DROP_TRIGGER      : code="DBSQL_DROP_TRIGGER"; break;
	case DBSQL_DROP_VIEW         : code="DBSQL_DROP_VIEW"; break;
	case DBSQL_INSERT            : code="DBSQL_INSERT"; break;
	case DBSQL_PRAGMA            : code="DBSQL_PRAGMA"; break;
	case DBSQL_READ              : code="DBSQL_READ"; break;
	case DBSQL_SELECT            : code="DBSQL_SELECT"; break;
	case DBSQL_TRANSACTION       : code="DBSQL_TRANSACTION"; break;
	case DBSQL_UPDATE            : code="DBSQL_UPDATE"; break;
	case DBSQL_ATTACH            : code="DBSQL_ATTACH"; break;
	case DBSQL_DETACH            : code="DBSQL_DETACH"; break;
	default                      : code="????"; break;
	}
	Tcl_DStringInit(&str);
	Tcl_DStringAppend(&str, dbctx->auth, -1);
	Tcl_DStringAppendElement(&str, code);
	Tcl_DStringAppendElement(&str, arg1 ? arg1 : "");
	Tcl_DStringAppendElement(&str, arg2 ? arg2 : "");
	Tcl_DStringAppendElement(&str, arg3 ? arg3 : "");
	Tcl_DStringAppendElement(&str, arg4 ? arg4 : "");
	rc = Tcl_GlobalEval(dbctx->interp, Tcl_DStringValue(&str));
	Tcl_DStringFree(&str);
	reply = Tcl_GetStringResult(dbctx->interp);
	if (strcmp(reply,"DBSQL_SUCCESS") == 0) {
		rc = DBSQL_SUCCESS;
	} else if (strcmp(reply,"DBSQL_DENY") == 0) {
		rc = DBSQL_DENY;
	} else if (strcmp(reply,"DBSQL_IGNORE") == 0) {
		rc = DBSQL_IGNORE;
	} else {
		rc = 999;
	}
	return rc;
}
#endif /* DBSQL_NO_AUTH */

/*
 * __tcl_dbsql_cmd_impl --
 *	The "dbsql" command below creates a new Tcl command for each
 *	connection it opens to a DBSQL database.  This routine is invoked
 *	whenever one of those connection-specific commands is executed
 *	in Tcl.  For example, if you run Tcl code like this:
 *
 *	dbsql db1  "my_database"
 *	db1 close
 *
 *	The first command opens a connection to the "my_database" database
 *	and calls that connection "db1".  The second command causes this
 *	subroutine to be invoked.
 */
static int
__tcl_dbsql_cmd_impl(_dbctx, interp, objc, objv)
	void *_dbctx;
	Tcl_Interp *interp;
	int objc;
	Tcl_Obj * const *objv;
{
	int n, complete, nscript, rowid, nchanges, choice, len, rc = TCL_OK;
	int nkey, ms;
	char *script, *name, *auth, *busy, *progress, *commit, *err_msgs, *sql;
	char *trace;
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;
	Tcl_Obj *result;
	callback_data_t data;
	sql_func_t *func;
	void *key;
#ifdef UTF_TRANSLATION_NEEDED
	Tcl_DString dsql;
	int i;
#endif
	static const char *DBSQL_strs[] = {
		"authorizer",           "busy",              "changes",
		"close",                "commit_hook",       "complete",
		"errorcode",            "eval",              "function",
		"last_inserted_rowid",  "onecolumn",         "progress",
		"timeout",              "trace",             NULL };
	enum DBSQL_enum {
	DBSQL__AUTHORIZER,        DBSQL__BUSY,             DBSQL__CHANGES,
	DBSQL__CLOSE,             DBSQL__COMMIT_HOOK,      DBSQL__COMPLETE,
	DBSQL__ERRORCODE,         DBSQL__EVAL,             DBSQL__FUNCTION,
	DBSQL__LAST_INSERT_ROWID, DBSQL__ONECOLUMN,        DBSQL__PROGRESS,
	DBSQL__TIMEOUT,           DBSQL__TRACE
	};

	if (objc < 2) {
		Tcl_WrongNumArgs(interp, 1, objv, "SUBCOMMAND ...");
		return TCL_ERROR;
	}
	if (Tcl_GetIndexFromObj(interp, objv[1], DBSQL_strs, "option",
				0, &choice)) {
		return TCL_ERROR;
	}

	switch ((enum DBSQL_enum)choice) {

	case DBSQL__AUTHORIZER:
		/*
		 *    $db authorizer ?CALLBACK?
		 *
		 * Invoke the given callback to authorize each SQL operation
		 * as it is compiled.  5 arguments are appended to the
		 * callback before it is invoked:
		 *
		 *   (1) The authorization type (ex: DBSQL_CREATE_TABLE,
		 *       DBSQL_INSERT, ...)
		 *   (2) First descriptive name (depends on authorization type)
		 *   (3) Second descriptive name
		 *   (4) Name of the database (ex: "main", "temp")
		 *   (5) Name of trigger that is doing the access
		 *
		 * The callback should return on of the following
		 * strings: DBSQL_SUCCESS, DBSQL_IGNORE, or DBSQL_DENY.  Any
		 * other return value is an error.
		 *
		 * If this method is invoked with no arguments, the current
		 * authorization* callback string is returned.
		 */
		if (objc > 3) {
			Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
		} else if (objc == 2) {
			if (dbctx->auth) {
				Tcl_AppendResult(interp, dbctx->auth, 0);
			}
		} else {
			if (dbctx->auth) {
				Tcl_Free(dbctx->auth);
			}
			auth = Tcl_GetStringFromObj(objv[2], &len);
			if (auth && len > 0) {
				dbctx->auth = Tcl_Alloc(len + 1);
				strcpy(dbctx->auth, auth);
			} else {
				dbctx->auth = 0;
			}
#ifndef DBSQL_NO_AUTH
			if (dbctx->auth) {
				dbctx->interp = interp;
				dbctx->dbp->set_authorizer(dbctx->dbp,
							   __tcl_auth_callback,
							   dbctx);
			} else {
				dbctx->dbp->set_authorizer(dbctx->dbp, 0, 0);
			}
#endif
		}
		break;

	case DBSQL__BUSY:
		/*
		 * $db busy ?CALLBACK?
		 *
		 * Invoke the given callback if an SQL statement attempts
		 * to open a locked database file.
		 */
		if (objc > 3) {
			Tcl_WrongNumArgs(interp, 2, objv, "CALLBACK");
			return TCL_ERROR;
		} else if (objc == 2) {
			if (dbctx->busy) {
				Tcl_AppendResult(interp, dbctx->busy, 0);
			}
		} else {
			if (dbctx->busy) {
				Tcl_Free(dbctx->busy);
			}
			busy = Tcl_GetStringFromObj(objv[2], &len);
			if (busy && len > 0) {
				dbctx->busy = Tcl_Alloc(len + 1);
				strcpy(dbctx->busy, busy);
			} else {
				dbctx->busy = 0;
			}
			if (dbctx->busy) {
				dbctx->interp = interp;
				dbctx->dbp->set_busycall(dbctx->dbp,
							 __tcl_busy_handler,
							 dbctx);
			} else {
				dbctx->dbp->set_busycall(dbctx->dbp, 0, 0);
			}
		}
		break;
	case DBSQL__PROGRESS:
		/*
		 *    $db progress ?N CALLBACK?
		 * 
		 * Invoke the given callback every N virtual machine
		 * opcodes while executing queries.
		 */
		if (objc == 2) {
			if (dbctx->progress) {
				Tcl_AppendResult(interp, dbctx->progress, 0);
			}
		} else if (objc == 4) {
			if (TCL_OK!=Tcl_GetIntFromObj(interp, objv[2], &n)) {
				return TCL_ERROR;
			}
			if (dbctx->progress) {
				Tcl_Free(dbctx->progress);
			}
			progress = Tcl_GetStringFromObj(objv[3], &len);
			if (progress && len > 0) {
				dbctx->progress = Tcl_Alloc(len + 1);
				strcpy(dbctx->progress, progress);
			} else {
				dbctx->progress = 0;
			}
#ifndef DBSQL_NO_PROGRESS
			if (dbctx->progress) {
				dbctx->interp = interp;
				dbctx->dbp->set_progresscall(dbctx->dbp, n,
							__tcl_progress_handler,
							dbctx);
			} else {
				dbctx->dbp->set_progresscall(dbctx->dbp,0,0,0);
			}
#endif
		} else {
			Tcl_WrongNumArgs(interp, 2, objv, "N CALLBACK");
			return TCL_ERROR;
		}
		break;
	case DBSQL__CHANGES:
		/*
		*     $db changes
		*
		* Return the number of rows that were modified, inserted,
		* or deleted by the most recent "eval".
		*/
		if (objc != 2) {
			Tcl_WrongNumArgs(interp, 2, objv, "");
			return TCL_ERROR;
		}
		nchanges = dbctx->dbp->last_change_count(dbctx->dbp);
		result = Tcl_GetObjResult(interp);
		Tcl_SetIntObj(result, nchanges);
		break;
	case DBSQL__CLOSE:
		/*
		 *    $db close
		 *
		 * Shutdown the database.
		 */
		dbctx->dbp->close(dbctx->dbp);
		dbctx->dbenv->close(dbctx->dbenv, 0);
		Tcl_DeleteCommand(interp, Tcl_GetStringFromObj(objv[0], 0));
		break;
	case DBSQL__COMMIT_HOOK:
		/*
		 *    $db commit_hook ?CALLBACK?
		 *
		 * Invoke the given callback just before committing every
		 * SQL transaction.  If the callback throws an exception
		 * or returns non-zero, then the transaction is aborted.  If
		 * CALLBACK is an empty string, the callback is disabled.
		 */
		if (objc > 3) {
			Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
		} else if (objc == 2) {
			if (dbctx->commit) {
				Tcl_AppendResult(interp, dbctx->commit, 0);
			}
		} else {
			if (dbctx->commit) {
				Tcl_Free(dbctx->commit);
			}
			commit = Tcl_GetStringFromObj(objv[2], &len);
			if (commit && len > 0) {
				dbctx->commit = Tcl_Alloc(len + 1);
				strcpy(dbctx->commit, commit);
			} else {
				dbctx->commit = 0;
			}
			if (dbctx->commit) {
				dbctx->interp = interp;
				dbctx->dbp->set_commitcall(dbctx->dbp,
							 __tcl_commit_handler,
							 dbctx);
			} else {
				dbctx->dbp->set_commitcall(dbctx->dbp, 0, 0);
			}
		}
		break;
	case DBSQL__COMPLETE:
		/*
		 *    $db complete SQL
		 *
		 * Return TRUE if SQL is a complete SQL statement.  Return
		 * FALSE if additional lines of input are needed.  This is
		 * similar to the built-in "info complete" command of Tcl.
		 */
		if (objc != 3) {
			Tcl_WrongNumArgs(interp, 2, objv, "SQL");
			return TCL_ERROR;
		}
		complete = dbsql_complete_stmt(Tcl_GetStringFromObj(objv[2],
								    0));
		result = Tcl_GetObjResult(interp);
		Tcl_SetBooleanObj(result, complete);
		break;
	case DBSQL__ERRORCODE:
		/*
		 *    $db errorcode
		 *
		 * Return the numeric error code that was returned by the
		 * most recent call to dbsql_exec().
		 */
		Tcl_SetObjResult(interp, Tcl_NewIntObj(dbctx->rc));
		break;
	case DBSQL__EVAL:
		/*
		*    $db eval $sql ?array {  ...code... }?
		*
		* The SQL statement in $sql is evaluated.  For each row, the
		* values are placed in elements of the array named "array"
		* and ...code... is executed.  If "array" and "code" are
		* omitted, then no callback is every invoked.  If "array" is
		* an empty string, then the values are placed in variables
		* that have the same name as the fields extracted by the query.
		*/
		if (objc != 5 && objc != 3) {
			Tcl_WrongNumArgs(interp, 2, objv,
					 "SQL ?ARRAY-NAME CODE?");
			return TCL_ERROR;
		}
		dbctx->interp = interp;
		sql = Tcl_GetStringFromObj(objv[2], 0);
#ifdef UTF_TRANSLATION_NEEDED
		Tcl_DStringInit(&dsql);
		Tcl_UtfToExternalDString(NULL, sql, -1, &dsql);
		sql = Tcl_DStringValue(&dsql);
#endif
		Tcl_IncrRefCount(objv[2]);
		if (objc == 5) {
			data.interp = interp;
			data.once = 1;
			data.array = Tcl_GetStringFromObj(objv[3], 0);
			data.code = objv[4];
			data.tcl_rc = TCL_OK;
			data.ncols = 0;
			data.col_names = 0;
			err_msgs = 0;
			Tcl_IncrRefCount(objv[3]);
			Tcl_IncrRefCount(objv[4]);
			rc = dbctx->dbp->exec(dbctx->dbp, sql,
					__tcl_process_row, &data, &err_msgs);
			Tcl_DecrRefCount(objv[4]);
			Tcl_DecrRefCount(objv[3]);
			if (data.tcl_rc == TCL_BREAK) {
				data.tcl_rc = TCL_OK;
			}
		} else {
			Tcl_Obj *list = Tcl_NewObj();
			data.tcl_rc = TCL_OK;
			rc = dbctx->dbp->exec(dbctx->dbp, sql,
					__tcl_process_row2, list, &err_msgs);
			Tcl_SetObjResult(interp, list);
		}
		dbctx->rc = rc;
		if (rc == DBSQL_ABORT) {
			if (err_msgs)
				free(err_msgs);
			rc = data.tcl_rc;
		} else if (err_msgs) {
			Tcl_SetResult(interp, err_msgs, TCL_VOLATILE);
			free(err_msgs);
			rc = TCL_ERROR;
		} else if (rc != DBSQL_SUCCESS) {
			Tcl_AppendResult(interp, dbsql_strerror(rc), 0);
			rc = TCL_ERROR;
		} else {
		}
		Tcl_DecrRefCount(objv[2]);
#ifdef UTF_TRANSLATION_NEEDED
		Tcl_DStringFree(&dsql);
		if (objc == 5 && data.col_names) {
			for (i = 0; i < data.ncols; i++) {
				if (data.col_names[i])
					free(data.col_names[i]);
			}
			free(data.col_names);
			data.col_names = 0;
		}
#endif
		break;
	case DBSQL__FUNCTION:
		/*
		 *     $db function NAME SCRIPT
		 *
		 * Create a new SQL function called NAME.  Whenever that
		 * function is called, invoke SCRIPT to evaluate the function.
		 */
		if (objc != 4) {
		  Tcl_WrongNumArgs(interp, 2, objv, "NAME SCRIPT");
		  return TCL_ERROR;
		}
		name = Tcl_GetStringFromObj(objv[2], 0);
		script = Tcl_GetStringFromObj(objv[3], &nscript);
		func = (sql_func_t*)Tcl_Alloc(sizeof(*func) + nscript + 1);
		if (func == 0)
			return TCL_ERROR;
		func->interp = interp;
		func->next = dbctx->func;
		func->script = (char*)&func[1];
		strcpy(func->script, script);
		dbctx->dbp->create_function(dbctx->dbp, name, -1,
					    DBSQL_UTF8_ENCODED, func,
					    __tcl_eval_sql_fn,
					    NULL, NULL);
		dbctx->dbp->func_return_type(dbctx->dbp, name, DBSQL_NUMERIC);
		break;
	case DBSQL__LAST_INSERT_ROWID:
		/*
		*     $db last_inserted_rowid
		*
		* Return an integer which is the ROWID for the most
		* recent insert.
		*/
		if (objc != 2) {
			Tcl_WrongNumArgs(interp, 2, objv, "");
			return TCL_ERROR;
		}
		rowid = dbctx->dbp->rowid(dbctx->dbp);
		result = Tcl_GetObjResult(interp);
		Tcl_SetIntObj(result, rowid);
		break;
	case DBSQL__ONECOLUMN:
		/*
		 *     $db onecolumn SQL
		 *
		 * Return a single column from a single row of the given
		 * SQL query.
		 */
		if (objc != 3) {
			Tcl_WrongNumArgs(interp, 2, objv, "SQL");
			return TCL_ERROR;
		}
		sql = Tcl_GetStringFromObj(objv[2], 0);
		rc = dbctx->dbp->exec(dbctx->dbp, sql, __tcl_process_row3,
				      interp, &err_msgs);
		if (rc == DBSQL_ABORT) {
			rc = DBSQL_SUCCESS;
		} else if (err_msgs) {
			Tcl_SetResult(interp, err_msgs, TCL_VOLATILE);
			free(err_msgs);
			rc = TCL_ERROR;
		} else if (rc != DBSQL_SUCCESS) {
			Tcl_AppendResult(interp, dbsql_strerror(rc), 0);
			rc = TCL_ERROR;
		}
		break;
	case DBSQL__TIMEOUT:
		/*
		 *     $db timeout MILLESECONDS
		 *
		 * Delay for the number of milliseconds specified when a
		 * file is locked.
		 */
		if (objc != 3) {
			Tcl_WrongNumArgs(interp, 2, objv, "MILLISECONDS");
			return TCL_ERROR;
		}
		if (Tcl_GetIntFromObj(interp, objv[2], &ms))
			return TCL_ERROR;
		dbctx->dbp->set_timeout(dbctx->dbp, ms);
		break;
	case DBSQL__TRACE:
		/*
		 *    $db trace ?CALLBACK?
		 *
		 * Make arrangements to invoke the CALLBACK routine for
		 * each SQL statement that is executed.  The text of the
		 * SQL is appended to CALLBACK before it is executed.
		 */
		if (objc > 3) {
			Tcl_WrongNumArgs(interp, 2, objv, "?CALLBACK?");
		} else if (objc == 2) {
			if (dbctx->trace) {
				Tcl_AppendResult(interp, dbctx->trace, 0);
			}
		} else {
			if(dbctx->trace) {
				Tcl_Free(dbctx->trace);
			}
			trace = Tcl_GetStringFromObj(objv[2], &len);
			if (trace && len > 0) {
				dbctx->trace = Tcl_Alloc(len + 1);
				strcpy(dbctx->trace, trace);
			} else {
				dbctx->trace = 0;
			}
			if (dbctx->trace) {
				dbctx->interp = interp;
				dbctx->dbp->set_tracecall(dbctx->dbp,
							  __tcl_trace_handler,
							  dbctx);
			} else {
				dbctx->dbp->set_tracecall(dbctx->dbp, 0, 0);
			}
		}
		break;
	}
	return rc;
}

/*
 * This function generates a string of random characters.  Used for
 * generating test data.
 */
void
__tcl_sql_func_randstr(context, argc, argv)
	dbsql_func_t *context;
	int argc;
	const char **argv;
{
	static const char src[] =
		"abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"0123456789"
		".-!,:*^+=_|?/<> ";
	int min, max, n, r, i;
	char buf[1000];

	if (argc >= 1) {
		min = atoi(argv[0]);
		if (min <0)
			min = 0;
		if (min >= sizeof(buf))
			min = sizeof(buf) - 1;
	} else {
		min = 1;
	}
	if (argc >= 2) {
		max = atoi(argv[1]);
		if (max < min)
			max = min;
		if (max >= sizeof(buf))
			max = sizeof(buf) - 1;
	} else {
		max = 50;
	}
	n = min;
	if (max > min) {
		r = rand() & 0x7fffffff;
		n += r % (max + 1 - min);
	}
	DBSQL_ASSERT(n < sizeof(buf));
	r = 0;
	for (i = 0; i < n; i++) {
		uint8_t rb = (uint8_t) (rand() % 256);
		r = (r + rb) % (sizeof(src) - 1);
		buf[i] = src[r];
	}
	buf[n] = 0;
	dbsql_set_result_string(context, buf, n);
}

/*
 * __register_tcl_sql_funcs --
 *	This function registered all of the above C functions as SQL
 *	functions.  This should be the only routine in this file with
 *	external linkage.
 */
void
__register_tcl_sql_funcs(dbp)
	DBSQL *dbp;
{
	static struct {
		char *name;
		int args;
		int type;
		void (*fn)(dbsql_func_t *, int, const char**);
	} funcs[] = {
#ifdef CONFIG_TEST
		{ "randstr", 2, DBSQL_TEXT, __tcl_sql_func_randstr },
#endif
	};
	static struct {
		char *name;
		int args;
		int type;
		void (*step)(dbsql_func_t *, int, const char**);
		void (*finalize)(dbsql_func_t *);
	} aggfns[] = {
#ifdef CONFIG_TEST
		{ "md5sum", -1, DBSQL_TEXT,
		  __tcl_sql_func_md5step, __tcl_sql_func_md5finalize },
#endif
	};
	int i;

	for (i = 0; i < (sizeof(funcs) / sizeof(funcs[0])); i++) {
		dbp->create_function(dbp, funcs[i].name,
				     funcs[i].args,
				     DBSQL_ASCII_ENCODED,/* FIXME: not used */
				     NULL,
				     funcs[i].fn,
				     NULL,
				     NULL);
		if (funcs[i].fn) {
			dbp->func_return_type(dbp, funcs[i].name,
					      funcs[i].type);
		}
	}

	for (i = 0; i < (sizeof(aggfns) / sizeof(aggfns[0])); i++) {
		dbp->create_function(dbp, aggfns[i].name,
				     aggfns[i].args,
				     DBSQL_ASCII_ENCODED,/* FIXME: not used */
				     NULL,
				     NULL,
				     aggfns[i].step,
				     aggfns[i].finalize);
		dbp->func_return_type(dbp, aggfns[i].name, aggfns[i].type);
	}
}

/*
 * __tcl_dbsql_impl --
 *	dbsql DBNAME FILENAME ?MODE? ?-key KEY?
 *
 *	This is the main Tcl command.  When the "dbsql" Tcl command is
 *	invoked, this routine runs to process that command.
 *
 * DBNAME
 *          An arbitrary name for a new database connection.  This
 *          command creates a new command named DBNAME that is used
 *          to control that connection.  The database connection is
 *          deleted when the DBNAME command is deleted.
 *
 * FILENAME
 *          The name of the directory that contains the database that
 *          is to be accessed.
 *
 * ?MODE?
 *          The mode of the database to be created.
 *
 * ?-key KEY?
 *
 *
 * -encoding
 *          Return the encoding used by LIKE and GLOB operators.  Choices
 *          are UTF-8 and iso8859.
 *
 * -version
 *          Return the version number of the library.
 *
 * -tcl-uses-utf
 *          Return "1" if compiled with a Tcl uses UTF-8.  Return "0" if
 *          not.  Used by tests to make sure the library was compiled
 *          correctly.
 */
static int
__tcl_dbsql_impl(_dbctx, interp, objc, objv)
	void *_dbctx;
	Tcl_Interp *interp;
	int objc;
	Tcl_Obj *const*objv;
{
	int rc;
	int mode;
	dbsql_ctx_t *dbctx;
	void *key = 0;
	int nkey = 0;
	const char *args;
	char *err_msgs;
	const char *filename;
	char buf[80];
	const char *ver;

	if (objc == 2) {
		args = Tcl_GetStringFromObj(objv[1], 0);
		if (strcmp(args, "-encoding") == 0) {
			Tcl_AppendResult(interp, dbctx->dbp->encoding(), 0);
			return TCL_OK;
		}
		if (strcmp(args, "-version") == 0) {
			int major, minor, patch;
			ver = dbsql_version(&major, &minor, &patch);
			Tcl_AppendResult(interp, ver, 0);
			return TCL_OK;
		}
		if (strcmp(args, "-has-crypto") == 0) {
			Tcl_AppendResult(interp,"1",0);
			return TCL_OK;
		}
		if (strcmp(args,"-tcl-uses-utf") == 0) {
#ifdef TCL_UTF_MAX
			Tcl_AppendResult(interp,"1",0);
#else
			Tcl_AppendResult(interp,"0",0);
#endif
			return TCL_OK;
		}
	}
	if (objc == 5 || objc == 6) {
		args = Tcl_GetStringFromObj(objv[objc-2], 0);
		if (strcmp(args, "-key") == 0){
			key = Tcl_GetByteArrayFromObj(objv[objc-1], &nkey);
			objc -= 2;
		}
	}
	if (objc != 3 && objc != 4) {
		Tcl_WrongNumArgs(interp, 1, objv,
				 "HANDLE FILENAME ?MODE? ?-key CRYPTOKEY?");
		return TCL_ERROR;
	}
	if (objc == 3) {
		mode = 0666;
	} else if (Tcl_GetIntFromObj(interp, objv[3], &mode) != TCL_OK) {
		return TCL_ERROR;
	}
	err_msgs = 0;
	dbctx = (dbsql_ctx_t*)Tcl_Alloc(sizeof(*dbctx));
	if (dbctx == 0) {
		Tcl_SetResult(interp, "malloc failed", TCL_STATIC);
		return TCL_ERROR;
	}
	memset(dbctx, 0, sizeof(*dbctx));
	filename = Tcl_GetStringFromObj(objv[2], 0);

	/* First setup the DB_ENV */
	if ((rc = db_env_create(&dbctx->dbenv, 0)) != 0) {
		Tcl_SetResult(interp, db_strerror(rc), TCL_STATIC);
		return TCL_ERROR;
	}
	dbctx->dbenv->set_errfile(dbctx->dbenv, stderr);
	dbctx->dbenv->set_lk_detect(dbctx->dbenv, DB_LOCK_DEFAULT);
	rc = dbctx->dbenv->set_cachesize(dbctx->dbenv, 0, 64 * 1024 * 1024, 0);
	if (rc) {
		Tcl_SetResult(interp, db_strerror(rc), TCL_STATIC);
		dbctx->dbenv->close(dbctx->dbenv, 0);
		return TCL_ERROR;
	}
	if (nkey > 0) {
		dbctx->crypt = (nkey ? 1 : 0);
		if ((rc = dbctx->dbenv->set_encrypt(dbctx->dbenv, key,
					      DB_ENCRYPT_AES)) != 0) {
			Tcl_SetResult(interp, db_strerror(rc), TCL_STATIC);
			dbctx->dbenv->close(dbctx->dbenv, 0);
			return TCL_ERROR;
		}
	}
	if ((rc = dbctx->dbenv->open(dbctx->dbenv, filename,
				     DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
				     DB_INIT_MPOOL | DB_INIT_TXN, 0)) != 0) {
		Tcl_SetResult(interp, db_strerror(rc), TCL_STATIC);
		dbctx->dbenv->close(dbctx->dbenv, 0);
	}
	if (dbsql_create(&dbctx->dbp, dbctx->dbenv, mode) != 0) {
		Tcl_SetResult(interp, err_msgs, TCL_VOLATILE);
		Tcl_Free((char*)dbctx);
		free(err_msgs);
		return TCL_ERROR;
	}
	args = Tcl_GetStringFromObj(objv[1], 0);
	Tcl_CreateObjCommand(interp, args, __tcl_dbsql_cmd_impl,
			     (char*)dbctx, __tcl_delete_cmd);

	/*
	 * The return value is the value of the DBSQL* pointer.
	 */
	sprintf(buf, "%p", dbctx->dbp);
	if (strncmp(buf,"0x",2)) {
		sprintf(buf, "0x%p", dbctx->dbp);
	}
	Tcl_AppendResult(interp, buf, 0);

	/*
	 * If compiled with CONFIG_TEST turned on, then register the "md5sum"
	 * SQL function.
	 */
	__register_tcl_sql_funcs(dbctx->dbp);
	return TCL_OK;
}

/*
 * dbsql_init_tcl_interface --
 *	Initialize this module.
 *	This Tcl module contains only a single new Tcl command named "dbsql".
 *	By using this single name, there are no Tcl namespace issues.
 */
int
dbsql_init_tcl_interface(interp)
	Tcl_Interp *interp;
{
	const char *cmd = "dbsql";

	Tcl_InitStubs(interp, "8.0", 0);
	Tcl_CreateObjCommand(interp, cmd, (Tcl_ObjCmdProc*)__tcl_dbsql_impl,
			     0, 0);
	Tcl_PkgProvide(interp, cmd, "2.0");
	return TCL_OK;
}

static char main_loop[] =
  "set line {}\n"
  "while {![eof stdin]} {\n"
    "if {$line!=\"\"} {\n"
      "puts -nonewline \"> \"\n"
    "} else {\n"
      "puts -nonewline \"% \"\n"
    "}\n"
    "flush stdout\n"
    "append line [gets stdin]\n"
    "if {[info complete $line]} {\n"
      "if {[catch {uplevel #0 $line} result]} {\n"
	"puts stderr \"Error: $result\"\n"
      "} elseif {$result!=\"\"} {\n"
	"puts $result\n"
      "}\n"
      "set line {}\n"
    "} else {\n"
      "append line \\n\n"
    "}\n"
  "}\n"
;

/*
 * main --
 */
int
main(argc, argv)
	int argc;
	char **argv;
{
	int i;
	const char *info;
	Tcl_Interp *interp;
	Tcl_FindExecutable(argv[0]);
	interp = Tcl_CreateInterp();
	dbsql_init_tcl_interface(interp);
#ifdef CONFIG_TEST
	__testset_1_init(interp);
/* FIXME	extern int __testset_4_init(Tcl_Interp*);
        __testset_4_init(interp); */
	extern int __testset_MD5_init(Tcl_Interp*);
	__testset_MD5_init(interp);
#endif
	if (argc >= 2) {
		Tcl_SetVar(interp, "argv0", argv[1], TCL_GLOBAL_ONLY);
		Tcl_SetVar(interp, "argv", "", TCL_GLOBAL_ONLY);
		for (i = 2; i < argc; i++) {
			Tcl_SetVar(interp, "argv", argv[i],
				   TCL_GLOBAL_ONLY | TCL_LIST_ELEMENT |
				   TCL_APPEND_VALUE);
		}
		if (Tcl_EvalFile(interp, argv[1]) != TCL_OK) {
			info = Tcl_GetVar(interp, "errorInfo",
					  TCL_GLOBAL_ONLY);
			if (info == 0)
				info = interp->result;
			fprintf(stderr, "%s: %s\n", *argv, info);
			return 1;
		}
	} else {
		Tcl_GlobalEval(interp, main_loop);
	}
	return 0;
}
