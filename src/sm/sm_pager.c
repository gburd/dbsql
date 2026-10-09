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
 * sm_pager.c --
 *	The pager questions the SQL layer still asks (PRAGMA journal_mode,
 *	locking_mode, cache statistics, the file name...).  libdb is the
 *	pager here; these answer from the storage manager's state or report
 *	the fixed behaviour of a libdb database.  See DIVERGENCES.md.
 */

#include "dbsql_int.h"
#include "sm_int.h"

/*
 * A pager_t is the sm_shared_t itself: __sm_pager returns it cast, and
 * every function here casts it back.
 */
#define	BT(pager)	((sm_shared_t *)(void *)(pager))

/* PUBLIC: struct pager_t *__sm_pager __P((sm_t *)); */
struct pager_t *
__sm_pager(sm_t *p)
{
	return ((struct pager_t *)(void *)p->bt);
}

/*
 * There is no VFS file under a libdb database, but the API (and the
 * tests) can ask for one through DBSQL_FCNTL_FILE_POINTER and call its
 * methods.  This one answers the questions and refuses I/O.
 */
static int
__sm_nofile_ok(dbsql_file_t *f)
{
	(void)f;
	return (DBSQL_OK);
}
static int
__sm_nofile_read(dbsql_file_t *f, void *b, int n, int64_t o)
{
	(void)f; (void)b; (void)n; (void)o;
	return (DBSQL_IOERR_READ);
}
static int
__sm_nofile_write(dbsql_file_t *f, const void *b, int n, int64_t o)
{
	(void)f; (void)b; (void)n; (void)o;
	return (DBSQL_IOERR_WRITE);
}
static int
__sm_nofile_trunc(dbsql_file_t *f, int64_t sz)
{
	(void)f; (void)sz;
	return (DBSQL_IOERR_TRUNCATE);
}
static int
__sm_nofile_int(dbsql_file_t *f, int i)
{
	(void)f; (void)i;
	return (DBSQL_OK);
}
static int
__sm_nofile_size(dbsql_file_t *f, int64_t *sz)
{
	(void)f;
	*sz = 0;
	return (DBSQL_OK);
}
static int
__sm_nofile_reserved(dbsql_file_t *f, int *res)
{
	(void)f;
	*res = 0;
	return (DBSQL_OK);
}
static int
__sm_nofile_fcntl(dbsql_file_t *f, int op, void *arg)
{
	(void)f; (void)op; (void)arg;
	return (DBSQL_NOTFOUND);
}
static int
__sm_nofile_sector(dbsql_file_t *f)
{
	(void)f;
	return (4096);
}
static int
__sm_nofile_devchar(dbsql_file_t *f)
{
	(void)f;
	return (0);
}

static const dbsql_io_methods_t __sm_nofile_methods = {
	1,			/* i_version: no shm, no mmap */
	__sm_nofile_ok,		/* x_close */
	__sm_nofile_read,
	__sm_nofile_write,
	__sm_nofile_trunc,
	__sm_nofile_int,	/* x_sync */
	__sm_nofile_size,
	__sm_nofile_int,	/* lock */
	__sm_nofile_int,	/* unlock */
	__sm_nofile_reserved,
	__sm_nofile_fcntl,
	__sm_nofile_sector,
	__sm_nofile_devchar,
	NULL, NULL, NULL, NULL, NULL, NULL
};

/* PUBLIC: dbsql_file_t *__sm_pager_file __P((pager_t *)); */
dbsql_file_t *
__sm_pager_file(pager_t *pager)
{
	static dbsql_file_t none = { &__sm_nofile_methods };

	(void)pager;
	return (&none);
}

/* PUBLIC: dbsql_file_t *__sm_pager_jrnl_file __P((pager_t *)); */
dbsql_file_t *
__sm_pager_jrnl_file(pager_t *pager)
{
	(void)pager;
	return (NULL);
}

/*
 * __sm_pager_filename --
 *	The database file name ("" for in-memory and temp databases).
 *
 * PUBLIC: const char *__sm_pager_filename __P((const pager_t *, int));
 */
const char *
__sm_pager_filename(const pager_t *pager, int null_if_memdb)
{
	sm_shared_t *bt = BT(pager);

	if (bt->flags & (SM_MEMORY | SM_EPHEMERAL))
		return (null_if_memdb ? NULL : "");
	return (bt->filename);
}

/* PUBLIC: dbsql_vfs_t *__sm_pager_vfs __P((pager_t *)); */
dbsql_vfs_t *
__sm_pager_vfs(pager_t *pager)
{
	return (BT(pager)->db->vfs);
}

/* PUBLIC: int __sm_pager_is_memdb __P((pager_t *)); */
int
__sm_pager_is_memdb(pager_t *pager)
{
	return ((BT(pager)->flags & (SM_MEMORY | SM_EPHEMERAL)) != 0);
}

/*
 * Journal mode: libdb's write-ahead log replaces both journals.  dbsql
 * reports "delete" for files and "memory" for in-memory databases, and
 * accepts but ignores any change (DIVERGENCES.md).
 */
/* PUBLIC: int __sm_pager_get_journal_mode __P((pager_t *)); */
int
__sm_pager_get_journal_mode(pager_t *pager)
{
	return (__sm_pager_is_memdb(pager) ? PAGER_JOURNALMODE_MEMORY :
	    PAGER_JOURNALMODE_DELETE);
}

/* PUBLIC: int __sm_pager_set_journal_mode __P((pager_t *, int)); */
int
__sm_pager_set_journal_mode(pager_t *pager, int mode)
{
	(void)mode;
	return (__sm_pager_get_journal_mode(pager));
}

/* PUBLIC: int __sm_pager_ok_to_change_journal_mode __P((pager_t *)); */
int
__sm_pager_ok_to_change_journal_mode(pager_t *pager)
{
	(void)pager;
	return (1);
}

/* PUBLIC: int64_t __sm_pager_journal_size_limit __P((pager_t *, int64_t)); */
int64_t
__sm_pager_journal_size_limit(pager_t *pager, int64_t limit)
{
	(void)pager;
	(void)limit;
	return (-1);
}

/* PUBLIC: int __sm_pager_locking_mode __P((pager_t *, int)); */
int
__sm_pager_locking_mode(pager_t *pager, int mode)
{
	(void)pager;
	(void)mode;
	return (PAGER_LOCKINGMODE_NORMAL);
}

/* PUBLIC: int __sm_pager_exclusive_lock __P((pager_t *)); */
int
__sm_pager_exclusive_lock(pager_t *pager)
{
	(void)pager;
	return (DBSQL_OK);
}

/* PUBLIC: int __sm_pager_flush __P((pager_t *)); */
int
__sm_pager_flush(pager_t *pager)
{
	sm_shared_t *bt = BT(pager);

	if (bt->flags & (SM_MEMORY | SM_EPHEMERAL))
		return (DBSQL_OK);
	return (__sm_errmap(bt->env->dbenv->memp_sync(bt->env->dbenv, NULL)));
}

/* PUBLIC: void __sm_pager_shrink __P((pager_t *)); */
void
__sm_pager_shrink(pager_t *pager)
{
	(void)pager;
}

/* PUBLIC: int __sm_pager_mem_used __P((pager_t *)); */
int
__sm_pager_mem_used(pager_t *pager)
{
	(void)pager;
	return (0);
}

/*
 * __sm_pager_cache_stat --
 *	DBSQL_DBSTATUS_CACHE_HIT/MISS/WRITE/SPILL from libdb's mpool.
 *
 * PUBLIC: void __sm_pager_cache_stat __P((pager_t *, int, int,
 * PUBLIC:     u_int64_t *));
 */
void
__sm_pager_cache_stat(pager_t *pager, int type, int reset, u_int64_t *value)
{
	sm_shared_t *bt = BT(pager);
	DB_MPOOL_STAT *gsp;

	if (bt->env == NULL || bt->env->dbenv->memp_stat(bt->env->dbenv,
	    &gsp, NULL, reset ? DB_STAT_CLEAR : 0) != 0)
		return;
	switch (type) {
	case DBSQL_DBSTATUS_CACHE_HIT:
		*value += gsp->st_cache_hit;
		break;
	case DBSQL_DBSTATUS_CACHE_MISS:
		*value += gsp->st_cache_miss;
		break;
	case DBSQL_DBSTATUS_CACHE_WRITE:
		*value += gsp->st_page_out;
		break;
	default:
		break;
	}
	free(gsp);
}

/* PUBLIC: u_int32_t __sm_pager_data_version __P((pager_t *)); */
u_int32_t
__sm_pager_data_version(pager_t *pager)
{
	/*
	 * DBSQL_FCNTL_DATA_VERSION: as upstream's pager, counts every
	 * commit, ours included, as of this connection's most recent
	 * transaction (another connection's commit shows after we next
	 * start one).
	 */
	return (1 + BT(pager)->commits_seen - BT(pager)->commits_base);
}

/* PUBLIC: int __sm_pager_wal_system_errno __P((pager_t *)); */
int
__sm_pager_wal_system_errno(pager_t *pager)
{
	(void)pager;
	return (0);
}

/* PUBLIC: int __sm_pager_wal_callback __P((pager_t *)); */
int
__sm_pager_wal_callback(pager_t *pager)
{
	(void)pager;
	return (0);
}

/* PUBLIC: int __sm_pager_wal_supported __P((pager_t *)); */
int
__sm_pager_wal_supported(pager_t *pager)
{
	(void)pager;
	return (0);
}

/* PUBLIC: int __sm_pager_close_wal __P((pager_t *, DBSQL *)); */
int
__sm_pager_close_wal(pager_t *pager, DBSQL *db)
{
	(void)pager;
	(void)db;
	return (DBSQL_OK);
}

/*
 * WAL snapshots (dbsql_snapshot_*) are not available: libdb's MVCC gives
 * every read transaction a snapshot, but there is no API to name one.
 */
/* PUBLIC: int __sm_pager_snapshot_get __P((pager_t *,
 * PUBLIC:     dbsql_snapshot_t **)); */
int
__sm_pager_snapshot_get(pager_t *pager, dbsql_snapshot_t **snapshot)
{
	(void)pager;
	*snapshot = NULL;
	return (DBSQL_ERROR);
}

/* PUBLIC: int __sm_pager_snapshot_open __P((pager_t *,
 * PUBLIC:     dbsql_snapshot_t *)); */
int
__sm_pager_snapshot_open(pager_t *pager, dbsql_snapshot_t *snapshot)
{
	(void)pager;
	(void)snapshot;
	return (DBSQL_ERROR);
}

/* PUBLIC: int __sm_pager_snapshot_recover __P((pager_t *)); */
int
__sm_pager_snapshot_recover(pager_t *pager)
{
	(void)pager;
	return (DBSQL_ERROR);
}

/* PUBLIC: int __sm_pager_snapshot_check __P((pager_t *,
 * PUBLIC:     dbsql_snapshot_t *)); */
int
__sm_pager_snapshot_check(pager_t *pager, dbsql_snapshot_t *snapshot)
{
	(void)pager;
	(void)snapshot;
	return (DBSQL_ERROR);
}

/* PUBLIC: void __sm_pager_snapshot_unlock __P((pager_t *)); */
void
__sm_pager_snapshot_unlock(pager_t *pager)
{
	(void)pager;
}

/*
 * __sm_pager_stats --
 *	Test-only pager counters (test1.c): all zero.
 *
 * PUBLIC: int *__sm_pager_stats __P((pager_t *));
 */
int *
__sm_pager_stats(pager_t *pager)
{
	static int zero[11];

	(void)pager;
	return (zero);
}
