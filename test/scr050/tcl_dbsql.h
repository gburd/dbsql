#ifndef	_DBSQL_TCL_H_
#define	_DBSQL_TCL_H_


#if defined(__cplusplus)
extern "C" {
#endif

typedef struct dbsqltcl_info {
	LIST_ENTRY(dbsqltcl_info) entries;
	Tcl_Interp *i_interp;
	char *i_name;
	DBSQL *i_dbp;
        FILE *i_err;
        const char *i_errpfx;
} DBSQLTCL_INFO;

typedef struct dbsqltcl_global {
	LIST_HEAD(infohead, dbsqltcl_info) infohead;
	int __debug_on, __debug_print, __debug_stop, __debug_test;
} DBSQLTCL_GLOBAL;
#define	__dbsql_infohead __dbsqltcl_global.infohead

extern DBSQLTCL_GLOBAL __dbsqltcl_global;

#define __debug_on    __dbsqltcl_global.__debug_on
#define __debug_print __dbsqltcl_global.__debug_print
#define __debug_stop  __dbsqltcl_global.__debug_stop
#define __debug_test  __dbsqltcl_global.__debug_test

/*
 * Tcl_NewStringObj takes an "int" length argument, when the typical use is to
 * call it with a size_t length (for example, returned by strlen).  Tcl is in
 * the wrong, but that doesn't help us much -- cast the argument.
 */
#define	NewStringObj(a, b)						\
	Tcl_NewStringObj(a, (int)b)

#define	NAME_TO_DBSQL(name) (DBSQL *)__dbsql_tcl_name_to_ptr((name))
#define NAME_TO_INFO(name) __dbsql_tcl_name_to_info((name))
#define SET_INFO_DBP(ip, dbp) (void)__dbsql_tcl_set_dbp((ip), (dbp))
#define DELETE_INFO(ip) (void)__dbsql_tcl_delete((ip))
#define RETURN_SETUP(interp, ret, ok, errmsg) \
	__dbsql_tcl_return_setup((interp), (ret), (ok), (errmsg))

/*
 * IS_HELP checks whether the arg we bombed on is -?, which is a help option.
 * If it is, we return TCL_OK (but leave the result set to whatever
 * Tcl_GetIndexFromObj says, which lists all the valid options.  Otherwise
 * return TCL_ERROR.
 */
#define	IS_HELP(s)						       \
    (strcmp(Tcl_GetStringFromObj(s,NULL), "-?") == 0) ? TCL_OK : TCL_ERROR

#include "tcl_ext.h"

#if defined(__cplusplus)
}
#endif
#endif /* !_DBSQL_TCL_H_ */
