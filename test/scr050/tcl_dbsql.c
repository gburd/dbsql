/*
 * A TCL harness for executing the Sqlite test suite.
 */

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <stdlib.h>
#include <string.h>
#endif

#include "dbsql_int.h"
#include "tcl.h"

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
	Tcl_Interp *interp;   /* The TCL interpret to execute the function */
	char *script;         /* The script to be run */
	sql_func_t *next;     /* Next function on the list of them all */
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
static int __tcl_process_row(_data, ncol, col, col_names)
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
		"timeout",              "trace",
		0                    
	};
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
				dbsql_set_authorizer(dbctx->dbp,
						      __tcl_auth_callback,
						      dbctx);
			} else {
				dbsql_set_authorizer(dbctx->dbp, 0, 0);
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
					    DBSQL_UTF8_ENCODED, func, __tcl_eval_sql_fn,
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
		rowid = dbsql_last_inserted_rowid(dbctx->dbp);
		result = Tcl_GetObjResult(interp);
		Tcl_SetIntObj(result, rowid);
		break;
	case DBSQL__ONECOLUMN:
		/*
		 *     $db onecolumn SQL
		 *
		 * Return a single column from a single row of the given
		 *SQL query.
		 */
		if (objc != 3) {
			Tcl_WrongNumArgs(interp, 2, objv, "SQL");
			return TCL_ERROR;
		}
		sql = Tcl_GetStringFromObj(objv[2], 0);
		rc = dbsql_exec(dbctx->dbp, sql, __tcl_process_row3,
				interp, &err_msgs);
		if (rc == DBSQL_ABORT) {
			rc = DBSQL_SUCCESS;
		} else if (err_msgs) {
			Tcl_SetResult(interp, err_msgs, TCL_VOLATILE);
			free(err_msgs);
			rc = TCL_ERROR;
		} else if (rc != DBSQL_SUCCESS) {
			Tcl_AppendResult(interp, dbsql_strerr(rc), 0);
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
		dbsql_set_busy_timeout(dbctx->dbp, ms);
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
				dbsql_trace(dbctx->dbp,
					    __tcl_trace_handler, dbctx);
			} else {
				dbsql_trace(dbctx->dbp, 0, 0);
			}
		}
		break;
	}
	return rc;
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
			Tcl_AppendResult(interp, dbsql_encoding, 0);
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
	if ((rc = dbctx->dbenv->set_cachesize(dbctx->dbenv, 0, 64 * 1024,
					      0)) != 0) {
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
 * Provide a dummy Tcl_InitStubs if we are using this as a static
 * library.
 */
#ifndef USE_TCL_STUBS
#undef  Tcl_InitStubs
#define Tcl_InitStubs(a,b,c)
#endif

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
