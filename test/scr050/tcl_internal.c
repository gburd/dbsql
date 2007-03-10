#include "dbsql_config.h"

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <stdlib.h>
#include <string.h>
#include <tcl.h>
#endif

#include "dbsql_int.h"
#include "tcl_dbsql.h"

/*
 *
 * tcl_internal.c --
 *
 *	This file contains internal functions we need to maintain
 *	state for our Tcl interface.
 */


/*
 * PUBLIC: DBSQLTCL_INFO *__dbsql_tcl_new_info __P((Tcl_Interp *,
 * PUBLIC:                                                           DBSQL *, char *));
 *
 * __dbsql_tcl_new_info --
 *
 *	This function will create a new info structure and fill it in
 *	with the name and pointer, id and type.
 */
DBSQLTCL_INFO *
__dbsql_tcl_new_info(interp, dbp, name)
	Tcl_Interp *interp;
	DBSQL *dbp;
	char *name;
{
	DBSQLTCL_INFO *p;
	int i, ret;

	if ((ret = __os_malloc(NULL, sizeof(DBSQLTCL_INFO), &p)) != 0) {
		Tcl_SetResult(interp, dbsql_strerror(ret), TCL_STATIC);
		return (NULL);
	}

	if ((ret = __os_strdup(NULL, name, &p->i_name)) != 0) {
		Tcl_SetResult(interp, dbsql_strerror(ret), TCL_STATIC);
		__os_free(NULL, p);
		return (NULL);
	}
	p->i_interp = interp;
	p->i_err = stderr;
	p->i_errpfx = NULL;
/*TODO	for (i = 0; i < MAX_ID; i++)
		p->i_otherid[i] = 0;*/

	LIST_INSERT_HEAD(&__dbsql_infohead, p, entries);
	return (p);
}

/*
 * PUBLIC: DBSQL *__dbsql_tcl_name_to_ptr __P((CONST char *));
 */
DBSQL *
__dbsql_tcl_name_to_ptr(name)
	CONST char *name;
{
	DBSQLTCL_INFO *p;

	for (p = LIST_FIRST(&__dbsql_infohead); p != NULL;
	     p = LIST_NEXT(p, entries))
		if (strcmp(name, p->i_name) == 0)
			return (p->i_dbp);
	return (NULL);
}

/*
 * PUBLIC: DBSQLTCL_INFO *__dbsql_tcl_ptr_to_info __P((CONST DBSQL *));
 */
DBSQLTCL_INFO *
__dbsql_tcl_ptr_to_info(dbp)
	CONST DBSQL *dbp;
{
	DBSQLTCL_INFO *p;

	for (p = LIST_FIRST(&__dbsql_infohead); p != NULL;
	    p = LIST_NEXT(p, entries))
		if (p->i_dbp == dbp)
			return (p);
	return (NULL);
}

/*
 * PUBLIC: DBSQLTCL_INFO *__dbsql_tcl_name_to_info __P((CONST char *));
 */
DBSQLTCL_INFO *
__dbsql_tcl_name_to_info(name)
	CONST char *name;
{
	DBSQLTCL_INFO *p;

	for (p = LIST_FIRST(&__dbsql_infohead); p != NULL;
	    p = LIST_NEXT(p, entries))
		if (strcmp(name, p->i_name) == 0)
			return (p);
	return (NULL);
}

/*
 * PUBLIC: void  __dbsql_tcl_delete_info __P((DBSQLTCL_INFO *));
 */
void
__dbsql_tcl_delete_info(p)
	DBSQLTCL_INFO *p;
{
	if (p == NULL)
		return;
	LIST_REMOVE(p, entries);
	/* p->i_dbp should have been DBSQL->close'd by this point */
	if (p->i_err != NULL && p->i_err != stderr) {
		fclose(p->i_err);
		p->i_err = NULL;
	}
	if (p->i_errpfx != NULL)
		__os_free(NULL, p->i_errpfx);
	__os_free(NULL, p->i_name);
	__os_free(NULL, p);

	return;
}

/*
 * PUBLIC: void  __dbsql_tcl_set_info_dbp __P((DBSQLTCL_INFO *, void *));
 */
void
__dbsql_tcl_set_info_dbp(p, data)
	DBSQLTCL_INFO *p;
	void *data;
{
	if (p == NULL)
		return;
	p->i_dbp = data;
	return;
}

/*
 * PUBLIC: int __dbsql_tcl_return_setup __P((Tcl_Interp *, int, int, char *));
 */
int
__dbsql_tcl_return_setup(interp, ret, ok, errmsg)
	Tcl_Interp *interp;
	int ret, ok;
	char *errmsg;
{
	char *msg;

	if (ret > 0)
		return (__dbsql_tcl_error_setup(interp, ret, errmsg));

	/*
	 * We either have success or a DBSQL error.  If a DBSQL error, set up
	 * the string.  We return an error if not one of the errors we catch.
	 * If anyone wants to reset the result to return anything different,
	 * then the calling function is responsible for doing so via
	 * Tcl_ResetResult or another Tcl_SetObjResult.
	 */
	if (ret == 0) {
		Tcl_SetResult(interp, "0", TCL_STATIC);
		return (TCL_OK);
	}

	msg = dbsql_strerror(ret);
	Tcl_AppendResult(interp, msg, NULL);

	if (ok)
		return (TCL_OK);
	else {
		Tcl_SetErrorCode(interp, "DBSQL", msg, NULL);
		return (TCL_ERROR);
	}
}

/*
 * PUBLIC: int __dbsql_tcl_error_setup __P((Tcl_Interp *, int, char *));
 */
int
__dbsql_tcl_error_setup(interp, ret, errmsg)
	Tcl_Interp *interp;
	int ret;
	char *errmsg;
{
	Tcl_SetErrno(ret);
	Tcl_AppendResult(interp, errmsg, ":", Tcl_PosixError(interp), NULL);
	return (TCL_ERROR);
}

/*
 * PUBLIC: void __dbsql_tcl_error_func __P((const DBSQL *, CONST char *,
 * PUBLIC:      const char *));
 */
void
__dbsql_tcl_error_func(dbp, pfx, msg)
	const DBSQL *dbp;
	CONST char *pfx;
	const char *msg;
{
	DBSQLTCL_INFO *p;
	Tcl_Interp *interp;
	int size;
	char *err;

	COMPQUIET(dbp, NULL);

	p = NAME_TO_INFO(pfx);
	if (p == NULL)
		return;
	interp = p->i_interp;

	size = strlen(pfx) + strlen(msg) + 4;
	/*
	 * If we cannot allocate enough to put together the prefix
	 * and message then give them just the message.
	 */
	if (__os_malloc(NULL, size, &err) != 0) {
		Tcl_AddErrorInfo(interp, msg);
		Tcl_AppendResult(interp, msg, "\n", NULL);
		return;
	}
	snprintf(err, size, "%s: %s", pfx, msg);
	Tcl_AddErrorInfo(interp, err);
	Tcl_AppendResult(interp, err, "\n", NULL);
	__os_free(NULL, err);
	return;
}
