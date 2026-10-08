/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2026  The DBSQL Group, Inc.
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

/*
 * sm_pcache.c --
 *	The page-cache entry points the library's initialisation and
 *	configuration code calls (dbsql_initialize, DBSQL_CONFIG_PAGECACHE,
 *	dbsql_status).  libdb's mpool is the cache, so these do nothing
 *	beyond keeping the API's contract: configuration succeeds and the
 *	statistics are zero.
 */

#include "dbsql_int.h"
#include "sm_int.h"

/* PUBLIC: int __pcache_initialize __P((void)); */
int
__pcache_initialize(void)
{
	return (DBSQL_OK);
}

/* PUBLIC: void __pcache_shutdown __P((void)); */
void
__pcache_shutdown(void)
{
}

/* PUBLIC: void __p_cache_buffer_setup __P((void *, int, int)); */
void
__p_cache_buffer_setup(void *buf, int sz, int n)
{
	(void)buf;
	(void)sz;
	(void)n;
}

/* PUBLIC: void __p_cache_set_default __P((void)); */
void
__p_cache_set_default(void)
{
}

/* PUBLIC: void __pcache_stats __P((int *, int *, int *, int *)); */
void
__pcache_stats(int *current, int *max, int *min, int *recyclable)
{
	*current = *max = *min = *recyclable = 0;
}

/* PUBLIC: int __header_size_pcache __P((void)); */
int
__header_size_pcache(void)
{
	return (0);
}

/* PUBLIC: int __header_size_pcache1 __P((void)); */
int
__header_size_pcache1(void)
{
	return (0);
}

/* PUBLIC: dbsql_mutex_t *__pcache1_mutex __P((void)); */
dbsql_mutex_t *
__pcache1_mutex(void)
{
	return (NULL);
}

/*
 * dbsql_database_file_object --
 *	There is no VFS file object behind a libdb database.
 *
 * EXTERN: dbsql_file_t *dbsql_database_file_object __P((const char *));
 */
dbsql_file_t *
dbsql_database_file_object(const char *name)
{
	(void)name;
	return (NULL);
}

/*
 * __dbstat_register --
 *	The dbstat virtual table reads b-tree pages; it is not built.
 *
 * PUBLIC: int __dbstat_register __P((DBSQL *));
 */
int
__dbstat_register(DBSQL *db)
{
	(void)db;
	return (DBSQL_OK);
}

#ifdef DBSQL_TEST
/*
 * Counters and switches the test harness links against.  The pager and
 * os_unix.c test builds maintain them; with libdb doing the I/O they stay
 * zero and the switches have nothing to switch.
 */
int dbsql_opentemp_count;
int dbsql_pager_readdb_count;
int dbsql_pager_writedb_count;
int dbsql_pager_writej_count;

void
disable_simulated_io_errors(void)
{
}

void
enable_simulated_io_errors(void)
{
}

/* PUBLIC: u_int64_t __sm_seek_count __P((sm_t *)); */
u_int64_t
__sm_seek_count(sm_t *p)
{
	(void)p;
	return (0);
}
#endif
