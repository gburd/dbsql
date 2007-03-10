/*
 * Implemntation of additional sql functions useful from the TCL
 * interface.  Most of these are for use during testing.
 */

#include "dbsql_config.h"
#include "dbsql_int.h"
#include "inc/os_ext.h"
#include "tcl.h"

#include <stdlib.h>
#include <string.h>

extern void __tcl_sql_func_randstr(dbsql_func_t *, int, const char **);
extern void __tcl_sql_func_md5step(dbsql_func_t *, int, const char **);
extern void __tcl_sql_func_md5finalize(dbsql_func_t *);

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
		dbsql_create_function(dbp, funcs[i].name,
				      funcs[i].args, funcs[i].fn, 0);
		if (funcs[i].fn) {
			dbsql_func_return_type(dbp, funcs[i].name,
					       funcs[i].type);
		}
	}

	for (i = 0; i < (sizeof(aggfns) / sizeof(aggfns[0])); i++) {
		dbsql_create_aggregate(dbp, aggfns[i].name,
				       aggfns[i].args, aggfns[i].step,
				       aggfns[i].finalize, 0);
		dbsql_func_return_type(dbp, aggfns[i].name,
				       aggfns[i].type);
	}
}
