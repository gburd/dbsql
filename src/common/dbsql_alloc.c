/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007  The DBSQL Group, Inc.
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
 *
 * $Id: dbsql_alloc.c 7 2007-02-03 13:34:17Z gburd $
 */

#include "dbsql_config.h"
#include "dbsql_int.h"

/*
 * __dbsql_umalloc --
 *	A malloc(3) function that will use use the _os_umalloc call of DB
 *	to obtain memory.
 *
 * PUBLIC: int __dbsql_umalloc __P((DBSQL *, size_t, void *));
 */
int
__dbsql_umalloc(dbp, size, storep)
	DBSQL *dbp;
	size_t size;
	void *storep;
{
	return (__os_umalloc((dbp ? dbp->dbenv : NULL), size, storep));
}

/*
 * __dbsql_urealloc --
 *	realloc(3) counterpart to __dbsql_umalloc.
 *
 * PUBLIC: int __dbsql_urealloc __P((DBSQL *, size_t, void *));
 */
int
__dbsql_urealloc(dbp, size, storep)
	DBSQL *dbp;
	size_t size;
	void *storep;
{
	return (__os_urealloc((dbp ? dbp->dbenv : NULL), size, storep));
}

/*
 * __dbsql_ufree --
 *	free(3) counterpart to __dbsql_umalloc.
 *
 * PUBLIC: void __dbsql_ufree __P((DBSQL *, void *));
 */
void
__dbsql_ufree(dbp, ptr)
	DBSQL *dbp;
	void *ptr;
{
	__os_ufree((dbp ? dbp->dbenv : NULL), ptr);
}

/*
 * __dbsql_strdup --
 *	The __dbsql_strdup(3) function for DBSQL.
 *
 * PUBLIC: int __dbsql_strdup __P((DBSQL *, const char *, void *));
 */
int
__dbsql_strdup(dbp, str, storep)
	DBSQL *dbp;
	const char *str;
	void *storep;
{
	return (__os_strdup((dbp ? dbp->dbenv : NULL), str, storep));
}

/*
 * __dbsql_strndup --
 *	The __dbsql_strndup(3) function for DBSQL.
 *
 * PUBLIC: int __dbsql_strndup __P((DBSQL *, const char *, void *, size_t));
 */
int
__dbsql_strndup(dbp, str, storep, len)
	DBSQL *dbp;
	const char *str;
	void *storep;
	size_t len;
{
	size_t size;
	int ret;
	void *p;
	DB_ENV *dbenv = (dbp->dbenv ? NULL : dbp->dbenv);

	*(void **)storep = NULL;

	if (len > strlen(str))
		size = strlen(str);
	else
		size = len;
	if ((ret = __os_calloc(dbenv, 1, size + 1, &p)) != 0)
		return (ret);

	memcpy(p, str, size);

	*(void **)storep = p;
	return (0);
}

/*
 * __dbsql_calloc --
 *	The calloc(3) function for DBSQL.
 *
 * PUBLIC: int __dbsql_calloc __P((DBSQL *, size_t, size_t, void *));
 */
int
__dbsql_calloc(dbp, num, size, storep)
	DBSQL *dbp;
	size_t num, size;
	void *storep;
{
	return (__os_calloc((dbp ? dbp->dbenv : NULL), num, size,
			storep));
}

/*
 * __dbsql_malloc --
 *	The malloc(3) function for DBSQL.
 *
 * PUBLIC: int __dbsql_malloc __P((DBSQL *, size_t, void *));
 */
int
__dbsql_malloc(dbp, size, storep)
	DBSQL *dbp;
	size_t size;
	void *storep;
{
	return (__os_malloc((dbp ? dbp->dbenv : NULL), size, storep));
}

/*
 * __dbsql_realloc --
 *	The realloc(3) function for DBSQL.
 *
 * PUBLIC: int __dbsql_realloc __P((DBSQL *, size_t, void *));
 */
int
__dbsql_realloc(dbp, size, storep)
	DBSQL *dbp;
	size_t size;
	void *storep;
{
	return (__os_realloc((dbp ? dbp->dbenv : NULL), size, storep));
}

/*
 * __dbsql_free --
 *	The free(3) function for DBSQL.
 *
 * PUBLIC: void __dbsql_free __P((DBSQL *, void *));
 */
void
__dbsql_free(dbp, ptr)
	DBSQL *dbp;
	void *ptr;
{
	__os_free((dbp ? dbp->dbenv : NULL), ptr);
}
