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
 * sm_env.c --
 *	libdb environments for the storage manager: the process-wide
 *	registry of persistent environments (one per directory, shared by
 *	every connection that opens a database file there), the private
 *	environments a connection uses for :memory:, temp and ephemeral
 *	databases, and the mapping of libdb errors to DBSQL result codes.
 */

#include "dbsql_int.h"
#include "sm_int.h"

#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

/* Registry of persistent environments, guarded by a static mutex. */
static sm_env_t *sm_env_list;

/*
 * __sm_env_mutex --
 *	The registry mutex (a static DBSQL mutex: no allocation, valid for
 *	the life of the process once dbsql_initialize has run).
 */
static dbsql_mutex_t *
__sm_env_mutex(void)
{
	return (dbsql_mutex_alloc(DBSQL_MUTEX_STATIC_VFS1));
}

/*
 * __sm_errmap --
 *	Map a libdb or errno error to a DBSQL result code.
 *
 * PUBLIC: int __sm_errmap __P((int));
 */
int
__sm_errmap(int ret)
{
	switch (ret) {
	case 0:
		return (DBSQL_OK);
	case DB_LOCK_NOTGRANTED:
	case DB_LOCK_DEADLOCK:
		return (DBSQL_BUSY);
	case DB_SNAPSHOT_CONFLICT:
	case DB_SNAPSHOT_UNSAFE:
		return (DBSQL_BUSY_SNAPSHOT);
	case DB_NOTFOUND:
	case DB_KEYEMPTY:
		return (DBSQL_NOTFOUND);
	case DB_RUNRECOVERY:
	case DB_VERIFY_BAD:
		return (DBSQL_CORRUPT);
	case DB_VERSION_MISMATCH:
		return (DBSQL_CANTOPEN);
	case ENOMEM:
		return (DBSQL_NOMEM);
	case ENOENT:
		return (DBSQL_CANTOPEN);
	case EACCES:
	case EPERM:
	case EROFS:
		return (DBSQL_READONLY);
	case ENOSPC:
	case DB_LOG_BUFFER_FULL:
		return (DBSQL_FULL);
	case EIO:
		return (DBSQL_IOERR);
	default:
		return (DBSQL_ERROR);
	}
}

/*
 * __sm_errcall --
 *	libdb error callback: keep the most recent message per environment
 *	so that a failing open can report why (the connection's error
 *	message is set by the caller from __sm_env_errmsg).
 */
static void
__sm_errcall(const DB_ENV *dbenv, const char *pfx, const char *msg)
{
	sm_env_t *env;

	(void)pfx;
	if ((env = dbenv->app_private) == NULL)
		return;
	strncpy(env->errmsg, msg, sizeof(env->errmsg) - 1);
	env->errmsg[sizeof(env->errmsg) - 1] = '\0';
}

/*
 * __sm_env_configure --
 *	Settings common to every environment we create.
 */
static void
__sm_env_configure(sm_env_t *env)
{
	DB_ENV *dbenv;

	dbenv = env->dbenv;
	dbenv->app_private = env;
	dbenv->set_errcall(dbenv, __sm_errcall);
	/*
	 * Each table and index is a DB handle opened with DB_THREAD; on
	 * libdb 2026 a threaded handle costs 9 mutexes.  Grow the mutex
	 * region on demand instead of sizing it up front.
	 */
	(void)dbenv->mutex_set_increment(dbenv, 4096);
}


/*
 * __sm_env_size_regions --
 *	A transaction holds a lock on every page it touches until it ends,
 *	and SQL transactions can touch many pages (one statement can write
 *	a whole table).  libdb's defaults (1000 locks, small log region)
 *	are sized for short txns; raise them.  The regions grow on demand
 *	up to SM_ENV_REGION_MAX.
 */
static void
__sm_env_size_regions(sm_env_t *env)
{
	DB_ENV *dbenv;

	dbenv = env->dbenv;
	(void)dbenv->set_memory_init(dbenv, DB_MEM_LOCK, SM_LOCKS_INIT);
	(void)dbenv->set_memory_init(dbenv, DB_MEM_LOCKOBJECT, SM_LOCKS_INIT);
	(void)dbenv->set_lk_max_locks(dbenv, SM_LOCKS_MAX);
	(void)dbenv->set_lk_max_objects(dbenv, SM_LOCKS_MAX);
	(void)dbenv->set_lk_tablesize(dbenv, SM_LOCKS_INIT);
	(void)dbenv->set_lg_regionmax(dbenv, SM_LOG_REGIONMAX);
	(void)dbenv->set_memory_max(dbenv, 0, SM_ENV_REGION_MAX);
	/*
	 * Under DB_MULTIVERSION a committed transaction is "retained until
	 * the last page it created is evicted from cache, so ... an
	 * additional transaction may be needed for each page in the cache"
	 * (libdb, transapp_read).  Size the transaction table for that, as
	 * the documentation says; the default fails after a few thousand
	 * small write transactions (ENOMEM, "Unable to allocate memory for
	 * transaction detail").
	 */
	(void)dbenv->set_tx_max(dbenv, SM_TX_MAX);
}

/*
 * __sm_env_open_persistent --
 *	Return (creating if needed) the shared environment for 'dir', a
 *	canonical absolute directory path.  The caller holds a reference
 *	on success and releases it with __sm_env_release.
 *
 * PUBLIC: int __sm_env_open_persistent __P((const char *, sm_env_t **,
 * PUBLIC:     char **));
 */
int
__sm_env_open_persistent(const char *dir, sm_env_t **envp, char **errp)
{
	sm_env_t *env;
	dbsql_mutex_t *mtx;
	u_int32_t flags;
	int ret, rc;

	*envp = NULL;
	mtx = __sm_env_mutex();
	dbsql_mutex_enter(mtx);
	for (env = sm_env_list; env != NULL; env = env->next)
		if (strcmp(env->home, dir) == 0) {
			env->refs++;
			*envp = env;
			dbsql_mutex_leave(mtx);
			return (DBSQL_OK);
		}

	rc = DBSQL_NOMEM;
	if ((env = dbsql_malloc(sizeof(*env))) == NULL)
		goto err;
	memset(env, 0, sizeof(*env));
	if ((env->home = dbsql_mprintf("%s", dir)) == NULL)
		goto err;
	if ((ret = db_env_create(&env->dbenv, 0)) != 0) {
		rc = __sm_errmap(ret);
		goto err;
	}
	__sm_env_configure(env);
	/*
	 * Snapshot (MVCC) reads, conflicts reported instead of waited on,
	 * and the deadlock detector run on every conflict.  See STORAGE.md.
	 */
	(void)env->dbenv->set_flags(env->dbenv,
	    DB_MULTIVERSION | DB_TXN_NOWAIT, 1);
	(void)env->dbenv->set_lk_detect(env->dbenv, DB_LOCK_DEFAULT);
	(void)env->dbenv->log_set_config(env->dbenv, DB_LOG_AUTO_REMOVE, 1);
	(void)env->dbenv->set_cachesize(env->dbenv, 0, SM_ENV_CACHE, 1);
	__sm_env_size_regions(env);
	/*
	 * Multi-process: DB_REGISTER | DB_RECOVER.  A process that exits
	 * without closing the environment makes the next opener recover it
	 * under every live process (.agent/libdb-bugs R6).  The documented
	 * alternative, DB_FAILCHK with an is-alive function, trips libdb
	 * diagnostic assertions in ordinary single-process use (R6 notes),
	 * so it is not used.
	 */
	flags = DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL |
	    DB_INIT_TXN | DB_THREAD | DB_REGISTER | DB_RECOVER;
	if ((ret = env->dbenv->open(env->dbenv, dir, flags, 0644)) != 0) {
		rc = ret == DB_VERSION_MISMATCH ? DBSQL_CANTOPEN :
		    __sm_errmap(ret);
		if (errp != NULL)
			*errp = dbsql_mprintf("cannot open libdb environment "
			    "in %s: %s%s%s", dir, db_strerror(ret),
			    env->errmsg[0] ? ": " : "", env->errmsg);
		(void)env->dbenv->close(env->dbenv, 0);
		env->dbenv = NULL;
		goto err;
	}
	env->refs = 1;
	env->next = sm_env_list;
	sm_env_list = env;
	dbsql_mutex_leave(mtx);
	*envp = env;
	return (DBSQL_OK);

err:	if (env != NULL) {
		dbsql_free(env->home);
		dbsql_free(env);
	}
	dbsql_mutex_leave(mtx);
	return (rc);
}

/*
 * __sm_env_open_private --
 *	Create a connection-private environment:
 *	  SM_PRIV_MEMORY  :memory: -- transactional, everything in memory
 *	                  (log buffer and databases; bounded by the cache).
 *	  SM_PRIV_TEMP    a temp database ("" filename) -- transactional,
 *	                  backed by a private directory under the temp
 *	                  directory so it can outgrow memory; removed when
 *	                  the environment closes.
 *	  SM_PRIV_EPHEMERAL  non-transactional cache for ephemeral tables,
 *	                  paged to the temp directory when full.
 *
 * PUBLIC: int __sm_env_open_private __P((int, sm_env_t **));
 */
static char *__sm_mktempdir(void);
static void __sm_rmtempdir(char *);

int
__sm_env_open_private(int kind, sm_env_t **envp)
{
	sm_env_t *env;
	u_int32_t flags;
	int ret;
	char *home;

	*envp = NULL;
	home = NULL;
	if (kind == SM_PRIV_TEMP && (home = __sm_mktempdir()) == NULL)
		return (DBSQL_CANTOPEN);
	if ((env = dbsql_malloc(sizeof(*env))) == NULL) {
		__sm_rmtempdir(home);
		return (DBSQL_NOMEM);
	}
	memset(env, 0, sizeof(*env));
	env->home = home;
	if ((ret = db_env_create(&env->dbenv, 0)) != 0) {
		__sm_rmtempdir(home);
		dbsql_free(env);
		return (__sm_errmap(ret));
	}
	__sm_env_configure(env);
	env->flags = SM_ENV_PRIVATE;
	flags = DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL | DB_THREAD;
	if (kind == SM_PRIV_EPHEMERAL)
		env->flags |= SM_ENV_NOTXN;
	else {
		(void)env->dbenv->set_flags(env->dbenv, DB_TXN_NOWAIT, 1);
		if (kind == SM_PRIV_MEMORY) {
			(void)env->dbenv->log_set_config(env->dbenv,
			    DB_LOG_IN_MEMORY, 1);
			(void)env->dbenv->set_lg_bsize(env->dbenv,
			    SM_MEMLOG_SIZE);
		} else {
			(void)env->dbenv->log_set_config(env->dbenv,
			    DB_LOG_AUTO_REMOVE, 1);
			(void)env->dbenv->set_flags(env->dbenv,
			    DB_TXN_NOSYNC, 1);
		}
		flags |= DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN;
		__sm_env_size_regions(env);
	}
	(void)env->dbenv->set_cachesize(env->dbenv, 0, SM_PRIVATE_CACHE, 1);
	if (home != NULL)
		(void)env->dbenv->set_tmp_dir(env->dbenv, home);
	else if (dbsql_temp_directory != NULL)
		(void)env->dbenv->set_tmp_dir(env->dbenv,
		    dbsql_temp_directory);
	if ((ret = env->dbenv->open(env->dbenv, home, flags, 0600)) != 0) {
		(void)env->dbenv->close(env->dbenv, 0);
		__sm_rmtempdir(home);
		dbsql_free(env);
		return (__sm_errmap(ret));
	}
	env->refs = 1;
	*envp = env;
	return (DBSQL_OK);
}

/*
 * __sm_mktempdir --
 *	A fresh private directory for a temp database, under
 *	dbsql_temp_directory, $TMPDIR or /tmp.
 */
static char *
__sm_mktempdir(void)
{
	const char *base;
	char *dir;

	if ((base = dbsql_temp_directory) == NULL &&
	    (base = getenv("TMPDIR")) == NULL)
		base = "/tmp";
	if ((dir = dbsql_mprintf("%s/dbsql_tmp.XXXXXX", base)) == NULL)
		return (NULL);
	if (mkdtemp(dir) == NULL) {
		dbsql_free(dir);
		return (NULL);
	}
	return (dir);
}

/*
 * __sm_rmtempdir --
 *	Remove a temp database directory and the files libdb left in it.
 */
static void
__sm_rmtempdir(char *dir)
{
	DIR *d;
	struct dirent *e;
	char *f;

	if (dir == NULL)
		return;
	if ((d = opendir(dir)) != NULL) {
		while ((e = readdir(d)) != NULL) {
			if (strcmp(e->d_name, ".") == 0 ||
			    strcmp(e->d_name, "..") == 0)
				continue;
			if ((f = dbsql_mprintf("%s/%s", dir, e->d_name)) != NULL) {
				(void)unlink(f);
				dbsql_free(f);
			}
		}
		(void)closedir(d);
	}
	(void)rmdir(dir);
	dbsql_free(dir);
}

/*
 * __sm_env_release --
 *	Drop a reference; close the environment with the last one.  A
 *	persistent environment's regions stay on disk (DB_REGISTER) for
 *	the next opener.
 *
 * PUBLIC: void __sm_env_release __P((sm_env_t *));
 */
void
__sm_env_release(sm_env_t *env)
{
	sm_env_t **pp;
	dbsql_mutex_t *mtx;

	if (env == NULL)
		return;
	if (env->flags & SM_ENV_PRIVATE) {
		if (--env->refs == 0) {
			(void)env->dbenv->close(env->dbenv, 0);
			__sm_rmtempdir(env->home);
			dbsql_free(env);
		}
		return;
	}
	mtx = __sm_env_mutex();
	dbsql_mutex_enter(mtx);
	if (--env->refs > 0) {
		dbsql_mutex_leave(mtx);
		return;
	}
	for (pp = &sm_env_list; *pp != NULL; pp = &(*pp)->next)
		if (*pp == env) {
			*pp = env->next;
			break;
		}
	dbsql_mutex_leave(mtx);
	(void)env->dbenv->txn_checkpoint(env->dbenv, 0, 0, 0);
	(void)env->dbenv->close(env->dbenv, 0);
	dbsql_free(env->home);
	dbsql_free(env);
}

/*
 * __sm_path_split --
 *	Split a database file name into its canonical absolute directory
 *	(the environment home) and the file's base name (the libdb file
 *	name within that home).  Both results are dbsql_malloc'd.
 *
 * PUBLIC: int __sm_path_split __P((const char *, char **, char **));
 */
int
__sm_path_split(const char *filename, char **dirp, char **basep)
{
	char buf[PATH_MAX], *dir, *slash;
	const char *base;
	size_t n;

	*dirp = *basep = NULL;
	if ((slash = strrchr(filename, '/')) == NULL) {
		dir = ".";
		base = filename;
		n = 1;
	} else {
		dir = (char *)filename;
		base = slash + 1;
		n = (size_t)(slash - filename);
		if (n == 0)
			n = 1;	/* "/x.db" */
	}
	if (*base == '\0')
		return (DBSQL_CANTOPEN);
	if (n >= sizeof(buf))
		return (DBSQL_CANTOPEN);
	memcpy(buf, dir, n);
	buf[n] = '\0';
	if ((*dirp = dbsql_malloc(PATH_MAX)) == NULL)
		return (DBSQL_NOMEM);
	if (realpath(buf, *dirp) == NULL) {
		dbsql_free(*dirp);
		*dirp = NULL;
		return (DBSQL_CANTOPEN);
	}
	if ((*basep = dbsql_mprintf("%s", base)) == NULL) {
		dbsql_free(*dirp);
		*dirp = NULL;
		return (DBSQL_NOMEM);
	}
	return (DBSQL_OK);
}
