/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996-2004
 *	Sleepycat Software.  All rights reserved.
 */

/*******************************************************
 * Global variables.
 *
 * Held in a single structure to minimize the name-space
 * pollution.
 *******************************************************/
#ifdef HAVE_VXWORKS
#include "semLib.h"
#endif

typedef struct __dbsql_globals {
	const char *encoding;
	int dbseq_has_wrapped;
} DBSQL_GLOBALS;

extern	DBSQL_GLOBALS	__dbsql_global_values;

#define	DBSQL_GLOBAL(v)	__dbsql_global_values.v
