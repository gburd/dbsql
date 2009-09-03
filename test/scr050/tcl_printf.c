/*
 * Code for testing the xprintf() function.  This code is used for testing
 * only and will not be included when the library is built without
 * CONFIG_TEST set.
 */

#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <stdlib.h>
#include <string.h>
#endif

#include "dbsql_int.h"
#include "tcl.h"

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
 * test_dbsql_env_create --
 *	Usage:    dbsql_create name
 *	Returns:  The name of an open database.
 */
static int
test_dbsql_env_create(notused, interp, argc, argv)
	void *notused;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *db;
	char *err_msgs;
	char ptr[100];
	int rc;
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " FILENAME\"", 0);
		return TCL_ERROR;
	}

	if ((rc = dbsql_create_env(&db, argv[1], NULL, 0, DBSQL_THREAD))
	   != DBSQL_SUCCESS) {
		Tcl_AppendResult(interp, dbsql_strerror(rc), 0);
		return TCL_ERROR;
	}
	if (__encode_as_ptr(interp, ptr, db))
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
 * test_exec_printf --
 *	Usage:  dbsql_exec_printf  DBSQL  FORMAT  STRING
 *
 *	Invoke the dbsql_exec_printf() interface using the open database
 *	DB.  The SQL is the string FORMAT.  The format string should contain
 *	one %s or %q.  STRING is the value inserted into %s or %q.
 */
static int
test_exec_printf(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *dbp;
	Tcl_DString str;
	int rc;
	char *err;
	char buf[30];
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;

	err = 0;
	if (argc != 4) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " DB FORMAT STRING", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &dbp))
		return TCL_ERROR;
	Tcl_DStringInit(&str);
	rc = dbctx->dbp->printf(dbctx->dbp, argv[2], exec_printf_cb,
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
	char *result;
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;

	result = 0;
	for(i = 2; i < argc; i++) {
		result = xvprintf(dbctx->dbp, "%z%s%s",
				  result, argv[1], argv[i]);
	}
	Tcl_AppendResult(interp, result, 0);
	__dbsql_free(dbctx->dbp, result);
	return TCL_OK;
}

/*
 * test_get_table_printf --
 *	Usage:  dbsql_get_table_printf  DB  FORMAT  STRING
 *
 *	Invoke the dbsql_get_table_printf() interface using the open database
 *	DB.  The SQL is the string FORMAT.  The format string should contain
 *	one %s or %q.  STRING is the value inserted into %s or %q.
 */
static int
test_get_table_printf(notused, interp, argc, argv)
	void *notused;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *db;
	Tcl_DString str;
	int rc;
	char *err = 0;
	int nrow, ncol;
	char **result;
	int i;
	char buf[30];
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;

	if (argc != 4) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " DB FORMAT STRING", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &db))
		return TCL_ERROR;
	Tcl_DStringInit(&str);
	rc = dbsql_get_table_printf(db, argv[2], &result, &nrow, &ncol, 
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
	dbsql_free_table(result);
	if (err)
		free(err);
	return TCL_OK;
}


/*
 * test_last_rowid --
 *	Usage:  dbsql_last_inserted_rowid DB
 *
 *	Returns the integer ROWID of the most recent insert.
 */
static int
test_last_rowid(notused, interp, argc, argv)
	void *notused;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *db;
	char buf[30];
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " DB\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &db))
		return TCL_ERROR;
	sprintf(buf, "%d", dbsql_last_inserted_rowid(db));
	Tcl_AppendResult(interp, buf, 0);
	return DBSQL_SUCCESS;
}

/*
 * test_close --
 *	Usage:  dbsql_close DB
 *
 *	Closes the database.
 */
static int
test_close(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *db;
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " FILENAME\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &db))
		return TCL_ERROR;
	dbsql_close(db);
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
	struct dstr x;
	memset(&x, 0, sizeof(x));
	dbsql_exec((DBSQL*)dbsql_user_data(context), argv[0], 
		   exec_func_callback, &x, 0);
	dbsql_set_result_string(context, x.str, x.len);
	__dbsql_free(NULL, x.str);
}

/*
 * test_create_function --
 *	Usage:  sqlite_test_create_function DB
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
test_create_function(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *db;
	extern void Md5_Register(DBSQL*);
	dbsql_ctx_t *dbctx = (dbsql_ctx_t*)_dbctx;

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " FILENAME\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &db))
		return TCL_ERROR;
	dbsql_create_function(db, "x_coalesce", -1, ifnull_func, 0);
	dbsql_create_function(db, "x_dbsql_exec", 1, exec_func, db);
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
 * test_create_aggregate --
 *	Usage:  sqlite_test_create_aggregate DB
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
test_create_aggregate(notused, interp, argc, argv)
	void *notused;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *db;

	COMPQUIET(notused, NULL);

	if (argc != 2) {
		Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0],
				 " FILENAME\"", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &db))
		return TCL_ERROR;
	dbsql_create_aggregate(db, "x_count", 0, count_step,
			       count_finalize, 0);
	dbsql_create_aggregate(db, "x_count", 1, count_step,
			       count_finalize, 0);
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

	COMPQUIET(notused, NULL);

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

	COMPQUIET(notused, NULL);

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
 *	and reset the sqlite_malloc_failed variable is N==0.
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

	COMPQUIET(notused, NULL);

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

	COMPQUIET(notused, NULL);

	sprintf(buf, "%d %d %d", sqlite_nMalloc, sqlite_nFree,
		sqlite_iMallocFail);
	Tcl_AppendResult(interp, buf, 0);
	return TCL_OK;
}
#endif

/*
 * dbsql_abort --
 *	Usage:  dbsql_abort
 *
 *	Shutdown the process immediately.  This is not a clean shutdown.
 *	This command is used to test the recoverability of a database in
 *	the event of a program crash.
 */
static int
dbsql_abort(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	COMPQUIET(notused, NULL);

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
 * test_register_func --
 *	Usage:   dbsql_register_test_function  DB  NAME
 *
 *	Register the test SQL function on the database DB under the name NAME.
 */
static int
test_register_func(_dbctx, interp, argc, argv)
	void *_dbctx;
	Tcl_Interp *interp;
	int argc;
	char **argv;
{
	DBSQL *db;
	int rc;

	COMPQUIET(notused, NULL);

	if (argc != 3) {
		Tcl_AppendResult(interp, "wrong # args: should be \"",
				 argv[0], " DB FUNCTION-NAME", 0);
		return TCL_ERROR;
	}
	if (get_dbsql_from_ptr(interp, argv[1], &db))
		return TCL_ERROR;
	rc = dbsql_create_function(db, argv[2], -1, test_func, 0);
	if (rc != 0) {
		Tcl_AppendResult(interp, dbsql_strerr(rc), 0);
		return TCL_ERROR;
	}
	return TCL_OK;
}

/*
** This SQLite callback records the datatype of all columns.
**
** The pArg argument is really a pointer to a TCL interpreter.  The
** column names are inserted as the result of this interpreter.
**
** This routine returns non-zero which causes the query to abort.
*/
static int rememberDataTypes(void *pArg, int nCol, char **argv, char **colv){
  int i;
  Tcl_Interp *interp = (Tcl_Interp*)pArg;
  Tcl_Obj *pList, *pElem;
  if( colv[nCol+1]==0 ){
    return 1;
  }
  pList = Tcl_NewObj();
  for(i=0; i<nCol; i++){
    pElem = Tcl_NewStringObj(colv[i+nCol] ? colv[i+nCol] : "NULL", -1);
    Tcl_ListObjAppendElement(interp, pList, pElem);
  }
  Tcl_SetObjResult(interp, pList);
  return 1;
}

/*
** Invoke an SQL statement but ignore all the data in the result.  Instead,
** return a list that consists of the datatypes of the various columns.
**
** This only works if "PRAGMA show_datatypes=on" has been executed against
** the database connection.
*/
static int dbsql_datatypes(
  void *_dbctx,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  char **argv            /* Text of each argument */
){
  DBSQL *db;
  int rc;
	COMPQUIET(_dbctx, NULL);
  if( argc!=3 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0], 
       " DB SQL", 0);
    return TCL_ERROR;
  }
  if( get_dbsql_from_ptr(interp, argv[1], &db) ) return TCL_ERROR;
  rc = dbsql_exec(db, argv[2], rememberDataTypes, interp, 0);
  if( rc!=0 && rc!=DBSQL_ABORT ){
    Tcl_AppendResult(interp, dbsql_strerr(rc), 0);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:  dbsql_compile  DB  SQL  ?TAILVAR?
**
** Attempt to compile an SQL statement.  Return a pointer to the virtual
** machine used to execute that statement.  Unprocessed SQL is written
** into TAILVAR.
*/
static int test_compile(
  void *_dbctx,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  char **argv            /* Text of each argument */
){
  DBSQL *db;
  dbsql_stmt_t *vm;
  int rc;
  char *zErr = 0;
  const char *zTail;
  char zBuf[50];
	COMPQUIET(notused, NULL);
  if( argc!=3 && argc!=4 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0], 
       " DB SQL TAILVAR", 0);
    return TCL_ERROR;
  }
  if( get_dbsql_from_ptr(interp, argv[1], &db) ) return TCL_ERROR;
  rc = dbsql_compile(db, argv[2], argc==4 ? &zTail : 0, &vm, &zErr);
  if( argc==4 ) Tcl_SetVar(interp, argv[3], zTail, 0);
  if( rc ){
    DBSQL_ASSERT( vm==0 );
    sprintf(zBuf, "(%d) ", rc);
    Tcl_AppendResult(interp, zBuf, zErr, 0);
    dbsql_freemem(zErr);
    return TCL_ERROR;
  }
  if( vm ){
    if( __encode_as_ptr(interp, zBuf, vm) ) return TCL_ERROR;
    Tcl_AppendResult(interp, zBuf, 0);
  }
  return TCL_OK;
}

/*
** Usage:  dbsql_step  VM  ?NVAR?  ?VALUEVAR?  ?COLNAMEVAR?
**
** Step a virtual machine.  Return a the result code as a string.
** Column results are written into three variables.
*/
static int test_step(
  void *_dbctx,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  char **argv            /* Text of each argument */
){
  dbsql_stmt_t *vm;
  int rc, i;
  const char **azValue = 0;
  const char **azColName = 0;
  int N = 0;
  char *zRc;
  char zBuf[50];
	COMPQUIET(notused, NULL);
  if( argc<2 || argc>5 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0], 
       " VM NVAR VALUEVAR COLNAMEVAR", 0);
    return TCL_ERROR;
  }
  if( get_sqlvm_from_ptr(interp, argv[1], &vm) ) return TCL_ERROR;
  rc = dbsql_step(vm, argc>=3?&N:0, argc>=4?&azValue:0, argc==5?&azColName:0);
  if( argc>=3 ){
    sprintf(zBuf, "%d", N);
    Tcl_SetVar(interp, argv[2], zBuf, 0);
  }
  if( argc>=4 ){
    Tcl_SetVar(interp, argv[3], "", 0);
    if( azValue ){
      for(i=0; i<N; i++){
        Tcl_SetVar(interp, argv[3], azValue[i] ? azValue[i] : "",
            TCL_APPEND_VALUE | TCL_LIST_ELEMENT);
      }
    }
  }
  if( argc==5 ){
    Tcl_SetVar(interp, argv[4], "", 0);
    if( azColName ){
      for(i=0; i<N*2; i++){
        Tcl_SetVar(interp, argv[4], azColName[i] ? azColName[i] : "",
            TCL_APPEND_VALUE | TCL_LIST_ELEMENT);
      }
    }
  }
  switch( rc ){
    case DBSQL_DONE:   zRc = "DBSQL_DONE";    break;
    case DBSQL_BUSY:   zRc = "DBSQL_BUSY";    break;
    case DBSQL_ROW:    zRc = "DBSQL_ROW";     break;
    case DBSQL_ERROR:  zRc = "DBSQL_ERROR";   break;
    case DBSQL_MISUSE: zRc = "DBSQL_MISUSE";  break;
    default:            zRc = "unknown";        break;
  }
  Tcl_AppendResult(interp, zRc, 0);
  return TCL_OK;
}

/*
** Usage:  dbsql_close_sqlvm  VM 
**
** Shutdown a virtual machine.
*/
static int test_finalize(
  void *_dbctx,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  char **argv            /* Text of each argument */
){
  dbsql_stmt_t *vm;
  int rc;
  char *zErrMsg = 0;
	COMPQUIET(notused, NULL);
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0], 
       " VM\"", 0);
    return TCL_ERROR;
  }
  if( get_sqlvm_from_ptr(interp, argv[1], &vm) ) return TCL_ERROR;
  rc = dbsql_close_sqlvm(vm, &zErrMsg);
  if( rc ){
    char zBuf[50];
    sprintf(zBuf, "(%d) ", rc);
    Tcl_AppendResult(interp, zBuf, zErrMsg, 0);
    dbsql_freemem(zErrMsg);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:  dbsql_reset_sqlvm   VM 
**
** Reset a virtual machine and prepare it to be run again.
*/
static int test_reset(
  void *_dbctx,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  char **argv            /* Text of each argument */
){
  dbsql_stmt_t *vm;
  int rc;
  char *zErrMsg = 0;
	COMPQUIET(notused, NULL);
  if( argc!=2 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0], 
       " VM\"", 0);
    return TCL_ERROR;
  }
  if( get_sqlvm_from_ptr(interp, argv[1], &vm) ) return TCL_ERROR;
  rc = dbsql_reset_sqlvm(vm, &zErrMsg);
  if( rc ){
    char zBuf[50];
    sprintf(zBuf, "(%d) ", rc);
    Tcl_AppendResult(interp, zBuf, zErrMsg, 0);
    dbsql_freemem(zErrMsg);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** This is the "static_bind_value" that variables are bound to when
** the FLAG option of dbsql_bind is "static"
*/
static char *dbsql_static_bind_value = 0;

/*
** Usage:  dbsql_bind  VM  IDX  VALUE  FLAGS
**
** Sets the value of the IDX-th occurance of "?" in the original SQL
** string.  VALUE is the new value.  If FLAGS=="null" then VALUE is
** ignored and the value is set to NULL.  If FLAGS=="static" then
** the value is set to the value of a static variable named
** "dbsql_static_bind_value".  If FLAGS=="normal" then a copy
** of the VALUE is made.
*/
static int test_bind(
  void *_dbctx,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  char **argv            /* Text of each argument */
){
  dbsql_stmt_t *vm;
  int rc;
  int idx;
	COMPQUIET(notused, NULL);
  if( argc!=5 ){
    Tcl_AppendResult(interp, "wrong # args: should be \"", argv[0], 
       " VM IDX VALUE (null|static|normal)\"", 0);
    return TCL_ERROR;
  }
  if( get_sqlvm_from_ptr(interp, argv[1], &vm) ) return TCL_ERROR;
  if( Tcl_GetInt(interp, argv[2], &idx) ) return TCL_ERROR;
  if( strcmp(argv[4],"null")==0 ){
    rc = dbsql_bind(vm, idx, 0, 0, 0);
  }else if( strcmp(argv[4],"static")==0 ){
    rc = dbsql_bind(vm, idx, dbsql_static_bind_value, -1, 0);
  }else if( strcmp(argv[4],"normal")==0 ){
    rc = dbsql_bind(vm, idx, argv[3], -1, 1);
  }else{
    Tcl_AppendResult(interp, "4th argument should be "
        "\"null\" or \"static\" or \"normal\"", 0);
    return TCL_ERROR;
  }
  if( rc ){
    char zBuf[50];
    sprintf(zBuf, "(%d) ", rc);
    Tcl_AppendResult(interp, zBuf, dbsql_strerr(rc), 0);
    return TCL_ERROR;
  }
  return TCL_OK;
}

/*
** Usage:    breakpoint
**
** This routine exists for one purpose - to provide a place to put a
** breakpoint with GDB that can be triggered using TCL code.  The use
** for this is when a particular test fails on (say) the 1485th iteration.
** In the TCL test script, we can add code like this:
**
**     if {$i==1485} breakpoint
**
** Then run testfixture in the debugger and wait for the breakpoint to
** fire.  Then additional breakpoints can be set to trace down the bug.
*/
static int test_breakpoint(
  void *_dbctx,
  Tcl_Interp *interp,    /* The TCL interpreter that invoked this command */
  int argc,              /* Number of arguments */
  char **argv            /* Text of each argument */
){
	COMPQUIET(notused, NULL);
  return TCL_OK;         /* Do nothing */
}

/*
 *	Register commands with the TCL interpreter.
 */
int
__testset_1_Init(interp)
	Tcl_Interp *interp;
{
	extern int dbsql_search_count;
	extern int dbsql_interrupt_count;
	extern int dbsql_open_file_count;
	extern int _fake_current_time;
	int i;
	static struct {
		char *zName;
		Tcl_CmdProc *xProc;
	} cmds[] = {
#if 0
TODO
     { "dbsql_mprintf_int",             (Tcl_CmdProc*)dbsql_mprintf_int    },
     { "dbsql_mprintf_str",             (Tcl_CmdProc*)dbsql_mprintf_str    },
     { "dbsql_mprintf_double",          (Tcl_CmdProc*)dbsql_mprintf_double },
     { "__mprintf_z_test",              (Tcl_CmdProc*)test_mprintf_z        },
#endif
     { "dbsql_env_create",              (Tcl_CmdProc*)test_dbsql_env_create },
     { "dbsql_last_inserted_rowid",     (Tcl_CmdProc*)test_last_rowid       },
     { "dbsql_exec_printf",             (Tcl_CmdProc*)test_exec_printf      },
     { "dbsql_get_table_printf",        (Tcl_CmdProc*)test_get_table_printf },
     { "dbsql_close",                   (Tcl_CmdProc*)test_close     },
     { "dbsql_create_function",         (Tcl_CmdProc*)test_create_function  },
     { "dbsql_create_aggregate",        (Tcl_CmdProc*)test_create_aggregate },
     { "dbsql_register_test_function",  (Tcl_CmdProc*)test_register_func    },
     { "dbsql_abort",                   (Tcl_CmdProc*)dbsql_abort          },
     { "dbsql_datatypes",               (Tcl_CmdProc*)dbsql_datatypes      },
#ifdef MEMORY_DEBUG
     { "dbsql_malloc_fail",             (Tcl_CmdProc*)dbsql_malloc_fail    },
     { "dbsql_malloc_stat",             (Tcl_CmdProc*)dbsql_malloc_stat    },
#endif
     { "dbsql_compile",                 (Tcl_CmdProc*)test_compile          },
     { "dbsql_step",                    (Tcl_CmdProc*)test_step             },
     { "dbsql_close_sqlvm",             (Tcl_CmdProc*)test_finalize         },
     { "dbsql_bind",                    (Tcl_CmdProc*)test_bind             },
     { "dbsql_reset_sqlvm",             (Tcl_CmdProc*)test_reset            },
     { "breakpoint",                    (Tcl_CmdProc*)test_breakpoint       },
  };

	for(i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		Tcl_CreateCommand(interp, aCmd[i].zName, aCmd[i].xProc, 0, 0);
	}
	Tcl_LinkVar(interp, "dbsql_search_count",
		    (char*)&dbsql_search_count, TCL_LINK_INT);
	Tcl_LinkVar(interp, "dbsql_interrupt_count",
		    (char*)&dbsql_interrupt_count, TCL_LINK_INT);
	Tcl_LinkVar(interp, "dbsql_open_file_count",
		    (char*)&dbsql_open_file_count, TCL_LINK_INT);
	Tcl_LinkVar(interp, "dbsql_current_time",
		    (char*)&_fake_current_time, TCL_LINK_INT);
	Tcl_LinkVar(interp, "dbsql_static_bind_value",
		    (char*)&dbsql_static_bind_value, TCL_LINK_STRING);
	return TCL_OK;
}
/* end of __testset_1 -------------------------------------------------------*/
