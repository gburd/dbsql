# Features we don't test for, but want the #defines to exist for
# other ports.
AH_TEMPLATE(DBSQL_WIN32,
    [We use DBSQL_WIN32 much as one would use _WIN32 -- to specify that
    we're using an operating system environment that supports Win32
    calls and semantics.  We don't use _WIN32 because Cygwin/GCC also
    defines _WIN32, even though Cygwin/GCC closely emulates the Unix
    environment.])

AH_TEMPLATE(HAVE_FILESYSTEM_NOTZERO,
    [Define to 1 if allocated filesystem blocks are not zeroed.])

AH_TEMPLATE(HAVE_UNLINK_WITH_OPEN_FAILURE,
    [Define to 1 if unlink of file with open file descriptors will fail.])

AH_BOTTOM([/*
 * Exit success/failure macros.
 */
#ifndef	HAVE_EXIT_SUCCESS
#define	EXIT_FAILURE	1
#define	EXIT_SUCCESS	0
#endif

#ifdef DBSQL_WIN32
#include "win_dbsql.h"
#endif])
