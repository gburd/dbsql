/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "inc/dbsql_config.h"
#include "dbsql_int.h"
#include "db_int.h"

#include <sys/stat.h>

/*
 * __dbsql_exists --
 *	Returns if file exists using the __os_exists call of DB.
 *
 * PUBLIC: int __dbsql_exists __P((DBSQL *, const char *, int *));
 */
int
__dbsql_exists(dbp, path, isdirp)
	DBSQL *dbp;
	const char *path;
	int *isdirp;
{
	return (__os_exists((dbp ? dbp->dbenv : NULL), path, isdirp));
}

/*
 * __dbsql_mkdir --
 *	Create a directory using the __os_mkdir call of DB.
 *
 * PUBLIC: int __dbsql_mkdir __P((DBSQL *, const char *, int));
 */
int
__dbsql_mkdir(dbp, path, mode)
	DBSQL *dbp;
	const char *path;
	int mode;
{
	return (__os_mkdir((dbp ? dbp->dbenv : NULL), path, mode));
}

/*
 * __dbsql_omode --
 *      Convert a file mode from a string to an int.
 *
 * PUBLIC: int  __dbsql_omode __P((DBSQL *, const char *));
 */
int
__dbsql_omode(dbp, mode)
        DBSQL *dbp;
        const char *mode;
{
        u_int t;
        int ret;


#define __SETMODE(offset, valid_ch, mask) {                             \
        if (mode[offset] == (valid_ch))                                 \
                t |= (mask);                                            \
        else if (mode[offset] != '-')                                   \
                goto format_err;                                        \
}
        t = 0;
        __SETMODE(0, 'r', S_IRUSR);
        __SETMODE(1, 'w', S_IWUSR);
        __SETMODE(2, 'x', S_IXUSR);
        __SETMODE(3, 'r', S_IRGRP);
        __SETMODE(4, 'w', S_IWGRP);
        __SETMODE(5, 'x', S_IXGRP);
        __SETMODE(6, 'r', S_IROTH);
        __SETMODE(7, 'w', S_IWOTH);
        __SETMODE(8, 'x', S_IXOTH);
        if (mode[9] != '\0' || t == 0) {
                /*
                 * We disallow modes of 0 -- we use 0 to decide the application
                 * never configured intermediate directory permissions, and we
                 * shouldn't create intermediate directories.  Besides, setting
                 * the permissions to 0 makes no sense.
                 */

format_err:     __dbsql_err(dbp, "illegal mode \"%s\"", mode);
                return (EINVAL);
        }

        return t;
}
