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
 * sm.c --
 *	The storage manager: SQLite's btree.h interface (src/inc/sm.h) on
 *	libdb.  This file holds database open/close, the meta values,
 *	table create/drop/clear, and transactions and savepoints.  Cursors
 *	are in sm_cursor.c, the comparator in sm_cmp.c, environments in
 *	sm_env.c.  See .agent/steering/STORAGE.md.
 */

#include "dbsql_int.h"
#include "sm_int.h"

#include <unistd.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>

/*
 * __sm_subdb_name --
 *	The libdb sub-database name of SQL table number 'pgno'.
 */
static void
__sm_subdb_name(u_int32_t pgno, char *buf)
{
	snprintf(buf, SM_SUBDB_MAX, SM_SUBDB_FMT, (unsigned)pgno);
}

/*
 * __sm_cur_txn --
 *	The txn every libdb operation of this database runs in: the
 *	innermost child (libdb refuses writes and opens on a parent while a
 *	child is active), or NULL for non-transactional (ephemeral) dbs.
 *
 * PUBLIC: DB_TXN *__sm_cur_txn __P((sm_shared_t *));
 */
DB_TXN *
__sm_cur_txn(sm_shared_t *bt)
{
	if (bt->txn == NULL || bt->txn->depth == 0)
		return (NULL);
	return (bt->txn->stack[bt->txn->depth - 1]);
}

/*
 * __sm_txn_push --
 *	Begin a child of the innermost txn (or a new top-level txn).
 */
static int
__sm_txn_push(sm_txn_t *t, u_int32_t flags)
{
	DB_TXN *parent, *txn, **ns;
	int ret;

	if (t->depth == t->alloc) {
		ns = dbsql_realloc(t->stack,
		    (int)((size_t)(t->alloc + SM_TXN_INITIAL) * sizeof(*ns)));
		if (ns == NULL)
			return (DBSQL_NOMEM);
		t->stack = ns;
		t->alloc += SM_TXN_INITIAL;
	}
	parent = t->depth > 0 ? t->stack[t->depth - 1] : NULL;
	if ((ret = t->env->dbenv->txn_begin(t->env->dbenv, parent, &txn,
	    flags)) != 0)
		return (__sm_errmap(ret));
	t->stack[t->depth++] = txn;
	return (DBSQL_OK);
}

/*
 * __sm_dbh_free --
 *	Close and free a cached handle.  Cursors still pointing at it drop
 *	the pointer (and their DBC, which libdb requires closed first) and
 *	fetch a fresh handle when next used.
 */
static void
__sm_dbh_free(sm_shared_t *bt, sm_dbh_t *h)
{
	sm_cursor_t *cur;

	for (cur = bt->cursors; cur != NULL; cur = cur->next)
		if (cur->dbh == h) {
			if (cur->dbc != NULL) {
				(void)cur->dbc->close(cur->dbc);
				cur->dbc = NULL;
				cur->dbc_txn = NULL;
				if (cur->state == CURSOR_VALID) {
					cur->state = CURSOR_REQUIRESEEK;
					cur->saved = 1;
				}
			}
			cur->dbh = NULL;
		}
	(void)h->dbp->close(h->dbp, DB_NOSYNC);
	if (h->key_info != NULL)
		__key_info_unref(h->key_info);
	dbsql_free(h);
}

/*
 * __sm_dbh_discard_level --
 *	A txn at stack level 'level' (or deeper) is about to end; libdb
 *	invalidates a DB handle opened inside an aborted txn, so handles
 *	opened at that level are closed and will be reopened on demand.
 *	On commit the handles survive (they now belong to the parent).
 */
static void
__sm_dbh_discard_level(sm_shared_t *bt, int level, int aborting)
{
	sm_dbh_t **pp, *h;

	for (pp = &bt->handles; (h = *pp) != NULL;) {
		if (h->level >= level) {
			if (aborting) {
				*pp = h->next;
				__sm_dbh_free(bt, h);
				continue;
			}
			h->level = level > 0 ? level - 1 : 0;
		}
		pp = &h->next;
	}
}

/*
 * __sm_txn_pop --
 *	End the innermost txn: commit it into its parent, or abort it.
 *	Cursors with a DBC in that txn must already be saved (closed).
 */
static int
__sm_txn_pop(sm_shared_t *bt, int commit)
{
	sm_txn_t *t;
	DB_TXN *txn;
	int ret, level;

	t = bt->txn;
	level = t->depth - 1;
	txn = t->stack[level];
	__sm_cursors_save(bt, txn);
	__sm_dbh_discard_level(bt, level, !commit);
	if (commit)
		ret = txn->commit(txn, level == 0 ? t->commit_flags : 0);
	else
		ret = txn->abort(txn);
	t->depth--;
	return (__sm_errmap(ret));
}

/*
 * __sm_meta_get_raw --
 *	Read meta value 'idx' (0 if absent) within the current txn.
 */
/*
 * PUBLIC: int __sm_meta_get_raw __P((sm_shared_t *, int, u_int32_t *));
 */
int
__sm_meta_get_raw(sm_shared_t *bt, int idx, u_int32_t *value)
{
	DBT key, data;
	u_int8_t k[4], v[4];
	int ret;

	*value = 0;
	if (bt->meta == NULL)
		return (DBSQL_OK);
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	k[0] = 0; k[1] = 0; k[2] = 0; k[3] = (u_int8_t)idx;
	key.data = k;
	key.size = 4;
	data.data = v;
	data.ulen = 4;
	data.flags = DB_DBT_USERMEM;
	ret = bt->meta->get(bt->meta, __sm_cur_txn(bt), &key, &data, 0);
	if (ret == DB_NOTFOUND)
		return (DBSQL_OK);
	if (ret != 0)
		return (__sm_errmap(ret));
	*value = ((u_int32_t)v[0] << 24) | ((u_int32_t)v[1] << 16) |
	    ((u_int32_t)v[2] << 8) | v[3];
	return (DBSQL_OK);
}

/*
 * __sm_meta_put_raw --
 *	Write meta value 'idx' within the current txn.
 */
/*
 * PUBLIC: int __sm_meta_put_raw __P((sm_shared_t *, int, u_int32_t));
 */
int
__sm_meta_put_raw(sm_shared_t *bt, int idx, u_int32_t value)
{
	DBT key, data;
	u_int8_t k[4], v[4];

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	k[0] = 0; k[1] = 0; k[2] = 0; k[3] = (u_int8_t)idx;
	v[0] = (u_int8_t)(value >> 24); v[1] = (u_int8_t)(value >> 16);
	v[2] = (u_int8_t)(value >> 8); v[3] = (u_int8_t)value;
	key.data = k;
	key.size = 4;
	data.data = v;
	data.size = 4;
	return (__sm_errmap(bt->meta->put(bt->meta, __sm_cur_txn(bt),
	    &key, &data, 0)));
}

/*
 * __sm_db_create --
 *	Create and configure (but do not open) a DB handle for this file.
 */
static int
__sm_db_create(sm_shared_t *bt, DB **dbpp)
{
	DB *dbp;
	int ret;

	if ((ret = db_create(&dbp, bt->env->dbenv, 0)) != 0)
		return (__sm_errmap(ret));
	if (bt->page_size != 0)
		(void)dbp->set_pagesize(dbp, bt->page_size);
	if (bt->flags & SM_EPHEMERAL) {
		/* Never written to disk except as cache overflow. */
		DB_MPOOLFILE *mpf = dbp->get_mpf(dbp);
		(void)mpf->set_flags(mpf, DB_MPOOL_UNLINK, 1);
	}
	*dbpp = dbp;
	return (DBSQL_OK);
}

/*
 * __sm_db_open --
 *	Open sub-database 'name' of this file in the current txn.
 */
static int
__sm_db_open(sm_shared_t *bt, DB *dbp, const char *name, u_int32_t flags)
{
	int ret;

	flags |= DB_THREAD;
	if (bt->txn == NULL)		/* ephemeral: unnamed, private */
		return (__sm_errmap(dbp->open(dbp, NULL, NULL, NULL,
		    DB_BTREE, flags, 0)));
	if (bt->flags & SM_MEMORY)
		/* :memory: sub-databases are unnamed-file (in-memory); a
		 * temp database's live in "temp.db" in its private home. */
		ret = dbp->open(dbp, __sm_cur_txn(bt),
		    bt->env->home != NULL ? SM_TEMP_FILE : NULL, name,
		    DB_BTREE, flags, 0600);
	else {
		if (bt->read_only)
			flags = (flags & ~DB_CREATE) | DB_RDONLY;
		ret = dbp->open(dbp, __sm_cur_txn(bt), bt->path, name,
		    DB_BTREE, flags | DB_MULTIVERSION, 0644);
	}
	return (__sm_errmap(ret));
}

/*
 * __sm_meta_open --
 *	Open (creating on first write) the metadata sub-database.  A new
 *	file gets the format record; an existing one must carry it.
 */
static int
__sm_meta_open(sm_shared_t *bt, int create)
{
	DB *dbp;
	DBT key, data;
	struct stat sb;
	char buf[32];
	int rc, ret;

	if (bt->meta != NULL)
		return (DBSQL_OK);
	/*
	 * __sm_open leaves a zero-length placeholder for a new database (the
	 * file exists from open, as upstream).  libdb cannot open that, so
	 * readers see an empty database and the first writer removes the
	 * placeholder and lets libdb create the file.
	 */
	if (!(bt->flags & (SM_MEMORY | SM_EPHEMERAL)) && bt->filename != NULL &&
	    stat(bt->filename, &sb) == 0 && sb.st_size == 0) {
		if (!create)
			return (DBSQL_EMPTY);
		(void)unlink(bt->filename);
	}
	if ((rc = __sm_db_create(bt, &dbp)) != DBSQL_OK)
		return (rc);
	rc = __sm_db_open(bt, dbp, SM_META_DB, create ? DB_CREATE : 0);
	if (rc != DBSQL_OK) {
		(void)dbp->close(dbp, DB_NOSYNC);
		/* A file without our meta is not a dbsql database. */
		return (rc == DBSQL_CANTOPEN && !create ? DBSQL_EMPTY : rc);
	}
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	key.data = SM_FORMAT_KEY;
	key.size = sizeof(SM_FORMAT_KEY) - 1;
	data.data = buf;
	data.ulen = sizeof(buf);
	data.flags = DB_DBT_USERMEM;
	ret = dbp->get(dbp, __sm_cur_txn(bt), &key, &data, 0);
	if (ret == DB_NOTFOUND && create) {
		data.data = SM_FORMAT_MAGIC;
		data.size = sizeof(SM_FORMAT_MAGIC) - 1;
		data.flags = 0;
		ret = dbp->put(dbp, __sm_cur_txn(bt), &key, &data, 0);
		if (ret == 0) {
			/* A new database: record its auto_vacuum mode. */
			bt->meta = dbp;
			if (__sm_meta_put_raw(bt, SM_META_AUTOVAC,
			    bt->auto_vacuum != 0) != DBSQL_OK ||
			    __sm_meta_put_raw(bt, BTREE_INCR_VACUUM,
			    bt->auto_vacuum == 2) != DBSQL_OK)
				ret = EIO;
			bt->meta = NULL;
		}
	} else if (ret == 0 && (data.size != sizeof(SM_FORMAT_MAGIC) - 1 ||
	    memcmp(buf, SM_FORMAT_MAGIC, data.size) != 0))
		ret = EINVAL;
	if (ret != 0) {
		(void)dbp->close(dbp, DB_NOSYNC);
		return (ret == EINVAL || ret == DB_NOTFOUND ? DBSQL_NOTADB :
		    __sm_errmap(ret));
	}
	bt->meta = dbp;
	bt->meta_level = bt->txn != NULL ? bt->txn->depth - 1 : 0;
	if (create) {
		/* Every database has the schema table, number 1. */
		sm_dbh_t *h;
		return (__sm_dbh_get(bt, 1, NULL, BTREE_INTKEY, &h));
	}
	return (DBSQL_OK);
}

/*
 * __sm_file_check --
 *	Before opening an environment: DBSQL_OK if 'path' is absent, empty or
 *	looks like a libdb file; DBSQL_NOTADB for any other file; and, as
 *	upstream, DBSQL_CANTOPEN for a file that exists but cannot be read.
 *	(__sm_open defers NOTADB to the first transaction, as upstream.)
 */
static int
__sm_file_check(const char *path)
{
	FILE *fp;
	unsigned char hdr[16];
	size_t n;

	if ((fp = fopen(path, "rb")) == NULL)
		return (errno == ENOENT ? DBSQL_OK : DBSQL_CANTOPEN);
	n = fread(hdr, 1, sizeof(hdr), fp);
	fclose(fp);
	if (n == 0)
		return (DBSQL_OK);	/* empty file: will be initialised */
	/* libdb btree metadata page: magic 0x053162 at offset 12. */
	if (n >= 16) {
		u_int32_t m = (u_int32_t)hdr[12] | ((u_int32_t)hdr[13] << 8) |
		    ((u_int32_t)hdr[14] << 16) | ((u_int32_t)hdr[15] << 24);
		u_int32_t mb = (u_int32_t)hdr[15] | ((u_int32_t)hdr[14] << 8) |
		    ((u_int32_t)hdr[13] << 16) | ((u_int32_t)hdr[12] << 24);
		if (m == 0x053162 || mb == 0x053162)
			return (DBSQL_OK);
	}
	return (DBSQL_NOTADB);
}

/*
 * __sm_txn_for --
 *	The connection's shared txn state for 'env', created on first use.
 */
static int
__sm_txn_for(DBSQL *db, sm_env_t *env, sm_txn_t **tp)
{
	sm_txn_t *t;

	for (t = db->sm_txns; t != NULL; t = t->next)
		if (t->env == env) {
			t->refs++;
			*tp = t;
			return (DBSQL_OK);
		}
	if ((t = dbsql_malloc(sizeof(*t))) == NULL)
		return (DBSQL_NOMEM);
	memset(t, 0, sizeof(*t));
	t->env = env;
	t->db = db;
	t->refs = 1;
	t->next = db->sm_txns;
	db->sm_txns = t;
	*tp = t;
	return (DBSQL_OK);
}

/*
 * __sm_txn_release --
 *	Drop an sm_t's reference on its connection txn state.
 */
static void
__sm_txn_release(DBSQL *db, sm_txn_t *t)
{
	sm_txn_t **pp;

	if (t == NULL || --t->refs > 0)
		return;
	while (t->depth > 0) {
		DB_TXN *txn = t->stack[--t->depth];
		(void)txn->abort(txn);
	}
	for (pp = &db->sm_txns; *pp != NULL; pp = &(*pp)->next)
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	dbsql_free(t->stack);
	dbsql_free(t);
}

/*
 * __sm_open --
 *	Open a database.  filename NULL or "" is an ephemeral or temp
 *	database; ":memory:" (or DBSQL_OPEN_MEMORY) is in-memory; anything
 *	else is a libdb file in the environment of its directory.
 *
 * PUBLIC: int __sm_open __P((dbsql_vfs_t *, const char *, DBSQL *,
 * PUBLIC:     sm_t **, int, int));
 */
int
__sm_open(dbsql_vfs_t *vfs, const char *filename, DBSQL *db, sm_t **btp,
    int flags, int vfs_flags)
{
	sm_t *p;
	sm_shared_t *bt;
	char *dir, *base, *full;
	int rc, is_temp, is_mem, missing;

	(void)vfs;
	*btp = NULL;
	is_temp = filename == NULL || filename[0] == '\0';
	is_mem = (filename != NULL && strcmp(filename, ":memory:") == 0) ||
	    (vfs_flags & DBSQL_OPEN_MEMORY) != 0 ||
	    (is_temp && (flags & BTREE_SINGLE) == 0);
	if ((p = dbsql_malloc(sizeof(*p))) == NULL)
		return (DBSQL_NOMEM);
	memset(p, 0, sizeof(*p));
	if ((bt = dbsql_malloc(sizeof(*bt))) == NULL) {
		dbsql_free(p);
		return (DBSQL_NOMEM);
	}
	memset(bt, 0, sizeof(*bt));
	p->db = db;
	p->bt = bt;
	bt->db = db;
	bt->page_size = DBSQL_DEFAULT_PAGE_SIZE;
	bt->pager_flags = PAGER_SYNCHRONOUS_FULL;
	if ((bt->filename = dbsql_mprintf("%s",
	    filename != NULL ? filename : "")) == NULL) {
		rc = DBSQL_NOMEM;
		goto err;
	}

	if (is_temp && (flags & BTREE_SINGLE)) {
		/* Ephemeral table: private, non-transactional cache. */
		bt->flags |= SM_EPHEMERAL;
		if ((rc = __sm_env_open_private(SM_PRIV_EPHEMERAL,
		    &bt->env)) != DBSQL_OK)
			goto err;
	} else if (is_mem) {
		/*
		 * :memory: lives entirely in memory; a temp database ("")
		 * gets a private on-disk environment so it can grow.  Both
		 * are SM_MEMORY: private, no write lock, no file name.
		 */
		bt->flags |= SM_MEMORY;
		if (vfs_flags & DBSQL_OPEN_READONLY)
			bt->read_only = 1;	/* pointless, but honoured */
		if ((rc = __sm_env_open_private(is_temp ? SM_PRIV_TEMP :
		    SM_PRIV_MEMORY, &bt->env)) != DBSQL_OK)
			goto err;
		if ((rc = __sm_txn_for(db, bt->env, &bt->txn)) != DBSQL_OK)
			goto err;
	} else {
		/*
		 * A foreign file opens, as upstream, and fails with
		 * DBSQL_NOTADB at the first transaction (the schema read).
		 */
		if ((rc = __sm_file_check(filename)) == DBSQL_NOTADB) {
			bt->foreign = 1;
			rc = DBSQL_OK;
		}
		if (rc != DBSQL_OK)
			goto err;
		/* Without DBSQL_OPEN_CREATE the file must exist. */
		missing = access(filename, F_OK) != 0;
		if (missing && (!(vfs_flags & DBSQL_OPEN_CREATE) ||
		    (vfs_flags & DBSQL_OPEN_READONLY))) {
			rc = DBSQL_CANTOPEN;
			goto err;
		}
		if ((rc = __sm_path_split(filename, &dir, &base)) != DBSQL_OK)
			goto err;
		full = dbsql_mprintf("%s/%s", dir, base);
		rc = full == NULL ? DBSQL_NOMEM :
		    __sm_env_open_persistent(dir, &bt->env, NULL);
		dbsql_free(dir);
		/*
		 * Create the file empty, as upstream does at open: libdb
		 * creates the real file only at the first write, and a
		 * database opened and closed unwritten must still exist.
		 * Only after the environment is open: its recovery must not
		 * meet the placeholder (it is not a libdb file).
		 */
		if (rc == DBSQL_OK && missing) {
			int fd;

			if ((fd = open(full, O_WRONLY | O_CREAT | O_EXCL,
			    0644)) >= 0)
				(void)close(fd);
			else if (errno != EEXIST)
				rc = DBSQL_CANTOPEN;
		}
		bt->path = base;
		if (rc != DBSQL_OK) {
			dbsql_free(full);
			goto err;
		}
		/* Write-lock object: the absolute path plus "\0wr". */
		bt->lock_obj_len = (u_int32_t)strlen(full);
		if (bt->lock_obj_len + 4 > sizeof(bt->lock_obj))
			bt->lock_obj_len = sizeof(bt->lock_obj) - 4;
		memcpy(bt->lock_obj, full, bt->lock_obj_len);
		memcpy(bt->lock_obj + bt->lock_obj_len, "\0wr", 3);
		bt->lock_obj_len += 3;
		/* Read-only when asked, or when the file is not writable. */
		if ((vfs_flags & DBSQL_OPEN_READONLY) ||
		    (access(full, F_OK) == 0 && access(full, W_OK) != 0))
			bt->read_only = 1;
		/* As upstream, the reported file name is the full path. */
		dbsql_free(bt->filename);
		bt->filename = full;
		if ((rc = __sm_txn_for(db, bt->env, &bt->txn)) != DBSQL_OK)
			goto err;
	}
	*btp = p;
	return (DBSQL_OK);

err:	__sm_txn_release(db, bt->txn);
	__sm_env_release(bt->env);
	dbsql_free(bt->path);
	dbsql_free(bt->filename);
	dbsql_free(bt);
	dbsql_free(p);
	return (rc);
}

/*
 * __sm_close --
 *	Close a database and every cursor and handle on it.
 *
 * PUBLIC: int __sm_close __P((sm_t *));
 */
int
__sm_close(sm_t *p)
{
	sm_shared_t *bt;
	sm_dbh_t *h;

	if (p == NULL)
		return (DBSQL_OK);
	bt = p->bt;
	while (bt->cursors != NULL)
		(void)__sm_close_cursor(bt->cursors);
	if (bt->txn != NULL && bt->in_trans != DBSQL_TXN_NONE)
		(void)__sm_rollback(p, DBSQL_OK, 0);
	while ((h = bt->handles) != NULL) {
		bt->handles = h->next;
		__sm_dbh_free(bt, h);
	}
	if (bt->meta != NULL)
		(void)bt->meta->close(bt->meta, DB_NOSYNC);
	if (bt->schema != NULL && bt->free_schema != NULL)
		bt->free_schema(bt->schema);
	dbsql_free(bt->schema);
	__sm_txn_release(p->db, bt->txn);
	__sm_env_release(bt->env);
	dbsql_free(bt->path);
	dbsql_free(bt->filename);
	dbsql_free(bt);
	dbsql_free(p);
	return (DBSQL_OK);
}

/*
 * __sm_wrlock --
 *	Take the database write lock under the top-level txn's locker.
 *	Released by libdb when that txn ends.
 */
static int
__sm_wrlock(sm_shared_t *bt)
{
	DB_ENV *dbenv;
	DB_LOCK lock;
	DBT obj;
	int i, ret;

	if ((bt->flags & (SM_WRLOCKED | SM_MEMORY | SM_EPHEMERAL)) != 0)
		return (DBSQL_OK);
	/*
	 * The same file attached twice in one connection has two sm_shared_t
	 * on the one shared txn: as upstream (WAL mode), only one of them may
	 * write in a transaction.  libdb would grant the second request
	 * (same locker), so check the connection's other handles.
	 */
	for (i = 0; i < bt->db->n_db; i++) {
		sm_t *q = bt->db->dbs[i].bt;
		if (q != NULL && q->bt != bt && q->bt->txn == bt->txn &&
		    (q->bt->flags & SM_WRLOCKED) &&
		    q->bt->lock_obj_len == bt->lock_obj_len &&
		    memcmp(q->bt->lock_obj, bt->lock_obj,
		    bt->lock_obj_len) == 0)
			return (DBSQL_BUSY);
	}
	dbenv = bt->env->dbenv;
	memset(&obj, 0, sizeof(obj));
	obj.data = bt->lock_obj;
	obj.size = bt->lock_obj_len;
	ret = dbenv->lock_get(dbenv, bt->txn->stack[0]->id(bt->txn->stack[0]),
	    DB_LOCK_NOWAIT, &obj, DB_LOCK_WRITE, &lock);
	if (ret != 0)
		return (__sm_errmap(ret));
	bt->flags |= SM_WRLOCKED;
	return (DBSQL_OK);
}

/*
 * __sm_begin_trans --
 *	Start a read (wrflag == 0) or write transaction, or upgrade a read
 *	transaction to write.  Conflicts return DBSQL_BUSY after the
 *	connection's busy handler gives up, as upstream.
 *
 * PUBLIC: int __sm_begin_trans __P((sm_t *, int, int *));
 */
int
__sm_begin_trans(sm_t *p, int wrflag, int *schema_version)
{
	sm_shared_t *bt;
	sm_txn_t *t;
	u_int32_t v;
	int rc, created, meta_was_open;

	bt = p->bt;
	t = bt->txn;
	if (bt->foreign)
		return (DBSQL_NOTADB);
	if (wrflag && bt->read_only)
		return (DBSQL_READONLY);
	if (bt->flags & SM_EPHEMERAL) {
		bt->in_trans = p->in_trans = DBSQL_TXN_WRITE;
		return (DBSQL_OK);
	}
	do {
		rc = DBSQL_OK;
		created = 0;
		meta_was_open = bt->meta != NULL;
		if (t->depth == 0) {
			/* Top-level txn (snapshot reads) + level-0 child. */
			rc = __sm_txn_push(t, (bt->flags & SM_MEMORY) ?
			    0 : DB_TXN_SNAPSHOT);
			if (rc == DBSQL_OK) {
				rc = __sm_txn_push(t, (bt->flags & SM_MEMORY) ?
				    0 : DB_TXN_SNAPSHOT);
				created = 1;
			}
			t->state = DBSQL_TXN_READ;
		}
		/*
		 * A writer takes the database write lock before its first
		 * read: a snapshot txn fixes its snapshot at that read, so it
		 * then sees every commit made before the lock was granted.
		 * (Locking after the read let a commit slip in between, and
		 * the writer's first put failed with an update conflict.)
		 */
		if (rc == DBSQL_OK && wrflag)
			rc = __sm_wrlock(bt);
		if (rc == DBSQL_OK && bt->meta == NULL)
			rc = __sm_meta_open(bt, wrflag);
		if (rc == DBSQL_EMPTY)		/* new file, read only */
			rc = DBSQL_OK;
		if (rc == DBSQL_OK && wrflag && bt->meta == NULL)
			rc = __sm_meta_open(bt, 1);
		if (rc == DBSQL_OK && wrflag && bt->snapshot_stale)
			rc = DBSQL_BUSY_SNAPSHOT;
		if (rc != DBSQL_OK && created) {
			/*
			 * A handle opened in a txn that is aborted dies with
			 * it: close meta if this attempt opened it.
			 */
			if (!meta_was_open && bt->meta != NULL) {
				(void)bt->meta->close(bt->meta, DB_NOSYNC);
				bt->meta = NULL;
			}
			while (t->depth > 0)
				(void)__sm_txn_pop(bt, 0);
			t->state = DBSQL_TXN_NONE;
			bt->flags &= ~SM_WRLOCKED;
		}
	} while ((rc & 0xff) == DBSQL_BUSY && bt->in_trans == DBSQL_TXN_NONE &&
	    rc != DBSQL_BUSY_SNAPSHOT &&
	    __invoke_busy_handler(&p->db->busy_handler));
	if (rc == DBSQL_BUSY_SNAPSHOT && bt->in_trans == DBSQL_TXN_NONE)
		rc = DBSQL_BUSY;
	if (rc != DBSQL_OK)
		return (rc);

	if (wrflag) {
		bt->in_trans = DBSQL_TXN_WRITE;
		t->state = DBSQL_TXN_WRITE;
	} else if (bt->in_trans == DBSQL_TXN_NONE)
		bt->in_trans = DBSQL_TXN_READ;
	p->in_trans = (u_int8_t)bt->in_trans;
	if (wrflag && p->db->n_savepoint > 0)
		rc = __sm_open_savepoint(p, p->db->n_savepoint);
	if (rc == DBSQL_OK && schema_version != NULL) {
		rc = __sm_meta_get_raw(bt, BTREE_SCHEMA_VERSION, &v);
		*schema_version = (int)v;
	}
	if (rc == DBSQL_OK && !(bt->flags & (SM_MEMORY | SM_EPHEMERAL))) {
		(void)__sm_meta_get_raw(bt, SM_META_COMMITS, &bt->commits_seen);
		/* data_version counts from this connection's first txn. */
		if (!bt->commits_base_set) {
			bt->commits_base = bt->commits_seen;
			bt->commits_base_set = 1;
		}
	}
	return (rc);
}

/*
 * __sm_open_savepoint --
 *	Make sure savepoints 0..n-1 exist (child txns stack[2..n+1]).
 *
 * PUBLIC: int __sm_open_savepoint __P((sm_t *, int));
 */
int
__sm_open_savepoint(sm_t *p, int n)
{
	sm_shared_t *bt;
	int rc;

	bt = p->bt;
	if (bt->txn == NULL)
		return (DBSQL_OK);
	while (bt->txn->depth < n + 2)
		if ((rc = __sm_txn_push(bt->txn, (bt->flags & SM_MEMORY) ?
		    0 : DB_TXN_SNAPSHOT)) != DBSQL_OK)
			return (rc);
	return (DBSQL_OK);
}

/*
 * __sm_begin_stmt --
 *	Open a statement subtransaction: a savepoint numbered above every
 *	SQL savepoint.
 *
 * PUBLIC: int __sm_begin_stmt __P((sm_t *, int));
 */
int
__sm_begin_stmt(sm_t *p, int i_statement)
{
	return (__sm_open_savepoint(p, i_statement));
}

/*
 * __sm_savepoint --
 *	Release or roll back savepoint i (0-based); i == -1 with ROLLBACK
 *	rolls back the whole transaction but keeps it open.  As upstream,
 *	a rolled-back savepoint stays open (a fresh child replaces it).
 *
 * PUBLIC: int __sm_savepoint __P((sm_t *, int, int));
 */
int
__sm_savepoint(sm_t *p, int op, int i_savepoint)
{
	sm_shared_t *bt;
	sm_txn_t *t;
	int rc, keep, reopen;

	if (p == NULL)
		return (DBSQL_OK);
	bt = p->bt;
	t = bt->txn;
	if (t == NULL || bt->in_trans != DBSQL_TXN_WRITE)
		return (DBSQL_OK);
	/*
	 * Savepoint i lives at stack[i + 2], so "savepoints 0..i-1 open"
	 * is depth i + 2; i == -1 means the level-0 content child,
	 * stack[1] (depth 1 without it).
	 */
	keep = i_savepoint + 2;
	if (keep < 1)
		keep = 1;
	rc = DBSQL_OK;
	reopen = 0;
	if (op == SAVEPOINT_RELEASE) {
		/* Commit savepoint i and everything above into its parent. */
		while (rc == DBSQL_OK && t->depth > keep)
			rc = __sm_txn_pop(bt, 1);
		return (rc);
	}
	/*
	 * Roll back: abort savepoint i (stack[keep]) and everything above,
	 * then open it again, as upstream leaves a rolled-back savepoint
	 * open.
	 */
	if (bt->meta != NULL && bt->meta_level >= keep) {
		/* Meta opened in a level being aborted dies with it. */
		(void)bt->meta->close(bt->meta, DB_NOSYNC);
		bt->meta = NULL;
		reopen = 1;
	}
	while (rc == DBSQL_OK && t->depth > keep)
		rc = __sm_txn_pop(bt, 0);
	if (rc == DBSQL_OK)
		rc = __sm_txn_push(t, (bt->flags & SM_MEMORY) ?
		    0 : DB_TXN_SNAPSHOT);
	/*
	 * The transaction stays open, so no __sm_begin_trans will reopen
	 * meta: do it now.  Schema reads (LARGEST_ROOT_PAGE) and table
	 * opens in the rest of the transaction need it (savepoint7-2.2).
	 * A new database whose meta was created in the aborted level is
	 * empty again (DBSQL_EMPTY).
	 */
	if (rc == DBSQL_OK && reopen && (rc = __sm_meta_open(bt, 0)) ==
	    DBSQL_EMPTY)
		rc = DBSQL_OK;
	return (rc);
}

/*
 * __sm_commit_phase_one --
 *	libdb commits atomically in one step; nothing to do here.
 *
 * PUBLIC: int __sm_commit_phase_one __P((sm_t *, const char *));
 */
int
__sm_commit_phase_one(sm_t *p, const char *super)
{
	(void)p;
	(void)super;
	return (DBSQL_OK);
}

/*
 * __sm_end_trans --
 *	Commit or abort the connection txn shared by this environment and
 *	reset every sm_t of the connection that uses it.
 */
int
__sm_end_trans(sm_t *p, int commit)
{
	sm_shared_t *bt;
	sm_txn_t *t;
	DBSQL *db;
	int i, rc, rc2;

	bt = p->bt;
	t = bt->txn;
	rc = DBSQL_OK;
	/* An interrupted nested reader must not end the outer reader's
	 * snapshot.  Test the shared txn, not just this database: another
	 * attached database in the environment may have written to it. */
	if (t != NULL && t->state == DBSQL_TXN_READ &&
	    p->db->n_vdbe_read > 1)
		return (DBSQL_OK);
	if (t != NULL && t->depth > 0) {
		/* Commit (or abort) every level, innermost first. */
		while (t->depth > 0) {
			rc2 = __sm_txn_pop(bt, commit && rc == DBSQL_OK);
			if (rc == DBSQL_OK)
				rc = rc2;
		}
		/* A commit that failed was aborted by libdb. */
		t->state = DBSQL_TXN_NONE;
		db = p->db;
		for (i = 0; i < db->n_db; i++) {
			sm_t *q = db->dbs[i].bt;
			if (q == NULL || q->bt->txn != t)
				continue;
			if (q->bt->in_trans == DBSQL_TXN_WRITE && commit &&
			    rc == DBSQL_OK) {
				q->bt->wrote = 1;
				/* Own commits do not change our data_version. */
				q->bt->own_seen++;
			}
			q->bt->in_trans = DBSQL_TXN_NONE;
			q->in_trans = DBSQL_TXN_NONE;
			q->bt->flags &= ~SM_WRLOCKED;
			q->bt->snapshot_stale = 0;
			if (q->bt->meta != NULL && !commit) {
				(void)q->bt->meta->close(q->bt->meta, DB_NOSYNC);
				q->bt->meta = NULL;
			}
		}
	}
	bt->in_trans = DBSQL_TXN_NONE;
	p->in_trans = DBSQL_TXN_NONE;
	bt->flags &= ~SM_WRLOCKED;
	/*
	 * A committed write is durable in the log; the database file
	 * catches up at a checkpoint.  Checkpoint once SM_CKPT_KBYTES of
	 * log has accumulated so the file (and its size) track the data,
	 * as tools that copy or measure the file expect.  (libdb does
	 * expensive work before testing the threshold: reported upstream,
	 * .agent/libdb-bugs R3.)
	 */
	if (bt->wrote && !(bt->flags & (SM_MEMORY | SM_EPHEMERAL))) {
		bt->wrote = 0;
		(void)bt->env->dbenv->txn_checkpoint(bt->env->dbenv,
		    SM_CKPT_KBYTES, 0, 0);
	}
	/* After a write rollback/commit, surviving readers need a new
	 * read txn and meta handles before their saved cursors can seek. */
	if (rc == DBSQL_OK && t != NULL && p->db->n_vdbe_read > 1) {
		db = p->db;
		for (i = 0; i < db->n_db && rc == DBSQL_OK; i++) {
			sm_t *q = db->dbs[i].bt;
			if (q != NULL && q->bt->txn == t &&
			    q->bt->cursors != NULL)
				rc = __sm_begin_trans(q, 0, NULL);
		}
	}
	return (rc);
}


/*
 * __sm_commit_phase_two --
 *	Commit.  Called once per database of the connection; the shared
 *	txn commits on the first call, the others find nothing to do.
 *
 * PUBLIC: int __sm_commit_phase_two __P((sm_t *, int));
 */
int
__sm_commit_phase_two(sm_t *p, int cleanup)
{
	(void)cleanup;
	if (p == NULL)
		return (DBSQL_OK);
	if (p->bt->flags & SM_EPHEMERAL) {
		p->bt->in_trans = p->in_trans = DBSQL_TXN_NONE;
		return (DBSQL_OK);
	}
	if (p->in_trans == DBSQL_TXN_NONE && p->bt->in_trans == DBSQL_TXN_NONE)
		return (DBSQL_OK);
	if (p->bt->in_trans == DBSQL_TXN_WRITE) {
		u_int32_t n;
		int rc;

		/* auto_vacuum=FULL: give free pages back before committing. */
		if (__sm_get_auto_vacuum(p) == BTREE_AUTOVACUUM_FULL)
			(void)__sm_compact(p);
		/*
		 * Count commits in meta (SM_META_COMMITS): other connections
		 * see PRAGMA data_version change (__sm_get_meta), and this
		 * one's DBSQL_FCNTL_DATA_VERSION counts its own commit.
		 */
		if (p->bt->meta != NULL &&
		    !(p->bt->flags & (SM_MEMORY | SM_EPHEMERAL))) {
			if ((rc = __sm_meta_get_raw(p->bt, SM_META_COMMITS,
			    &n)) != DBSQL_OK ||
			    (rc = __sm_meta_put_raw(p->bt, SM_META_COMMITS,
			    n + 1)) != DBSQL_OK) {
				(void)__sm_end_trans(p, 0);
				return (rc);
			}
			p->bt->commits_seen = n + 1;
		}
	}
	return (__sm_end_trans(p, 1));
}

/*
 * __sm_commit --
 *	Both commit phases.
 *
 * PUBLIC: int __sm_commit __P((sm_t *));
 */
int
__sm_commit(sm_t *p)
{
	int rc;

	if ((rc = __sm_commit_phase_one(p, 0)) == DBSQL_OK)
		rc = __sm_commit_phase_two(p, 0);
	return (rc);
}

/*
 * __sm_rollback --
 *	Roll back the transaction.  Cursors are tripped with trip_code (or
 *	saved, if trip_code is DBSQL_OK), as upstream.
 *
 * PUBLIC: int __sm_rollback __P((sm_t *, int, int));
 */
int
__sm_rollback(sm_t *p, int trip_code, int write_only)
{
	if (p == NULL)
		return (DBSQL_OK);
	if (trip_code != DBSQL_OK)
		(void)__sm_trip_all_cursors(p, trip_code, write_only);
	if (p->bt->flags & SM_EPHEMERAL) {
		p->bt->in_trans = p->in_trans = DBSQL_TXN_NONE;
		return (DBSQL_OK);
	}
	return (__sm_end_trans(p, 0));
}

/*
 * __sm_txn_state --
 *	DBSQL_TXN_NONE, _READ or _WRITE.
 *
 * PUBLIC: int __sm_txn_state __P((sm_t *));
 */
int
__sm_txn_state(sm_t *p)
{
	return (p != NULL ? p->bt->in_trans : DBSQL_TXN_NONE);
}

/*
 * __sm_is_in_backup --
 *	PUBLIC: int __sm_is_in_backup __P((sm_t *));
 */
int
__sm_is_in_backup(sm_t *p)
{
	return (p->bt->n_backup != 0);
}

/*
 * __sm_dbh_get --
 *	The cached DB handle for table 'pgno', opening it on first use.
 *	key_info (indexes only) is copied by reference into the handle so
 *	the comparator can find it.
 *
 * PUBLIC: int __sm_dbh_get __P((sm_shared_t *, u_int32_t, key_info_t *,
 * PUBLIC:     int, sm_dbh_t **));
 */
int
__sm_dbh_get(sm_shared_t *bt, u_int32_t pgno, key_info_t *ki, int create,
    sm_dbh_t **hp)
{
	sm_dbh_t *h;
	char name[SM_SUBDB_MAX];
	int rc;

	for (h = bt->handles; h != NULL; h = h->next)
		if (h->pgno == pgno) {
			if (h->key_info == NULL && ki != NULL) {
				h->key_info = __key_info_ref(ki);
			}
			*hp = h;
			return (DBSQL_OK);
		}
	if ((h = dbsql_malloc(sizeof(*h))) == NULL)
		return (DBSQL_NOMEM);
	memset(h, 0, sizeof(*h));
	h->pgno = pgno;
	if ((rc = __sm_db_create(bt, &h->dbp)) != DBSQL_OK) {
		dbsql_free(h);
		return (rc);
	}
	h->dbp->app_private = h;
	/*
	 * Whether a table is an index (record keys, our comparator) or a
	 * rowid table (8-byte keys, libdb's memcmp) is fixed at creation and
	 * recorded in meta; the caller says which via 'create' flags or we
	 * read it back.
	 */
	if (!create && bt->meta == NULL && !(bt->flags & SM_EPHEMERAL)) {
		/* New database, nothing written yet: every table is empty. */
		(void)h->dbp->close(h->dbp, DB_NOSYNC);
		dbsql_free(h);
		return (DBSQL_EMPTY);
	}
	if ((rc = __sm_table_kind(bt, pgno, create, &h->is_intkey)) != DBSQL_OK)
		goto err;
	if (!h->is_intkey)
		(void)h->dbp->set_bt_compare(h->dbp, __sm_rec_cmp);
	__sm_subdb_name(pgno, name);
	/* Ephemeral tables exist only in this handle: always create. */
	rc = __sm_db_open(bt, h->dbp, name,
	    (create || (bt->flags & SM_EPHEMERAL)) ? DB_CREATE : 0);
	if (rc != DBSQL_OK)
		goto err;
	if (ki != NULL)
		h->key_info = __key_info_ref(ki);
	h->level = bt->txn != NULL && bt->txn->depth > 0 ?
	    bt->txn->depth - 1 : 0;
	h->next = bt->handles;
	bt->handles = h;
	*hp = h;
	return (DBSQL_OK);

err:	(void)h->dbp->close(h->dbp, DB_NOSYNC);
	dbsql_free(h);
	return (rc == DBSQL_CANTOPEN ? DBSQL_CORRUPT : rc);
}

/*
 * __sm_table_kind --
 *	Record (create != 0: create is BTREE_INTKEY or BTREE_BLOBKEY) or
 *	look up whether table pgno is a rowid table.  Kinds are stored in
 *	the metadata sub-database under key "k<pgno>".  The schema table
 *	(1) is always a rowid table.
 *
 * PUBLIC: int __sm_table_kind __P((sm_shared_t *, u_int32_t, int, int *));
 */
int
__sm_table_kind(sm_shared_t *bt, u_int32_t pgno, int create, int *intkeyp)
{
	DBT key, data;
	char kbuf[SM_SUBDB_MAX];
	u_int8_t v;
	int ret;

	/*
	 * Table 1 is always a rowid table (the schema table, or an ephemeral
	 * btree's implicit table).  An ephemeral btree's table 2 is created
	 * by __sm_create_table with an explicit kind.
	 */
	if (pgno == 1) {
		*intkeyp = 1;
		return (DBSQL_OK);
	}
	if (bt->flags & SM_EPHEMERAL) {
		if (create)
			bt->eph_kind[pgno & 1] = (create & BTREE_INTKEY) != 0;
		*intkeyp = bt->eph_kind[pgno & 1];
		return (DBSQL_OK);
	}
	snprintf(kbuf, sizeof(kbuf), "k%u", (unsigned)pgno);
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	key.data = kbuf;
	key.size = (u_int32_t)strlen(kbuf);
	if (create) {
		v = (create & BTREE_INTKEY) ? 1 : 0;
		data.data = &v;
		data.size = 1;
		*intkeyp = v;
		return (__sm_errmap(bt->meta->put(bt->meta, __sm_cur_txn(bt),
		    &key, &data, 0)));
	}
	if (bt->meta == NULL)
		return (DBSQL_CORRUPT);
	data.data = &v;
	data.ulen = 1;
	data.flags = DB_DBT_USERMEM;
	ret = bt->meta->get(bt->meta, __sm_cur_txn(bt), &key, &data, 0);
	if (ret != 0)
		return (ret == DB_NOTFOUND ? DBSQL_CORRUPT : __sm_errmap(ret));
	*intkeyp = v;
	return (DBSQL_OK);
}

/*
 * __sm_create_table --
 *	Create a new table (BTREE_INTKEY) or index/WITHOUT ROWID table
 *	(BTREE_BLOBKEY) and return its number in *pgno.
 *
 * PUBLIC: int __sm_create_table __P((sm_t *, pgno_t *, int));
 */
int
__sm_create_table(sm_t *p, pgno_t *pgnop, int flags)
{
	sm_shared_t *bt;
	sm_dbh_t *h;
	u_int32_t largest;
	int rc;

	bt = p->bt;
	if (bt->flags & SM_EPHEMERAL) {
		/* Ephemeral btrees hold one table, number 2 (SCHEMA_ROOT+1). */
		*pgnop = 2;
		return (__sm_dbh_get(bt, 2, NULL, (flags & BTREE_INTKEY) ?
		    BTREE_INTKEY : BTREE_BLOBKEY, &h));
	}
	if ((rc = __sm_meta_get_raw(bt, BTREE_LARGEST_ROOT_PAGE, &largest))
	    != DBSQL_OK)
		return (rc);
	if (largest < 1)
		largest = 1;
	largest++;
	if ((rc = __sm_meta_put_raw(bt, BTREE_LARGEST_ROOT_PAGE, largest))
	    != DBSQL_OK)
		return (rc);
	if ((rc = __sm_dbh_get(bt, largest, NULL,
	    (flags & BTREE_INTKEY) ? BTREE_INTKEY : BTREE_BLOBKEY, &h))
	    != DBSQL_OK)
		return (rc);
	*pgnop = largest;
	return (DBSQL_OK);
}

/*
 * __sm_dbh_close_table --
 *	Close the cached handle on table pgno (dbremove needs that).
 */
static void
__sm_dbh_close_table(sm_shared_t *bt, u_int32_t pgno)
{
	sm_dbh_t **pp, *h;

	for (pp = &bt->handles; (h = *pp) != NULL; pp = &h->next)
		if (h->pgno == pgno) {
			*pp = h->next;
			__sm_dbh_free(bt, h);
			return;
		}
}

/*
 * __sm_drop_table --
 *	Remove table pgno.  dbsql never moves tables, so *moved is 0.
 *
 * PUBLIC: int __sm_drop_table __P((sm_t *, int, int *));
 */
static int __sm_subdb_remove(sm_shared_t *, const char *);

int
__sm_drop_table(sm_t *p, int pgno, int *movedp)
{
	sm_shared_t *bt;
	sm_dbh_t *h;
	DBT key;
	u_int32_t count;
	char name[SM_SUBDB_MAX], kbuf[SM_SUBDB_MAX];
	int rc, ret;

	bt = p->bt;
	*movedp = 0;
	if (bt->flags & SM_EPHEMERAL) {
		__sm_dbh_close_table(bt, (u_int32_t)pgno);
		return (DBSQL_OK);
	}
	/*
	 * dbremove needs every handle on the sub-database closed, and
	 * other connections keep theirs open (DB_LOCK_NOTGRANTED).  So the
	 * table is emptied with truncate, which other handles and their
	 * snapshots tolerate, and forgotten: its kind record goes and the
	 * number is never reused (LARGEST_ROOT_PAGE only grows).  The
	 * empty sub-database is removed now if nobody else has it open;
	 * otherwise it stays, empty, until a VACUUM rewrites the file
	 * (DIVERGENCES.md).
	 */
	if ((rc = __sm_dbh_get(bt, (u_int32_t)pgno, NULL, 0, &h)) == DBSQL_OK) {
		__sm_cursors_invalidate_table(bt, (u_int32_t)pgno);
		if ((ret = h->dbp->truncate(h->dbp, __sm_cur_txn(bt), &count,
		    0)) != 0)
			return (__sm_errmap(ret));
	} else if (rc != DBSQL_CANTOPEN)
		return (rc);
	__sm_dbh_close_table(bt, (u_int32_t)pgno);
	__sm_subdb_name((u_int32_t)pgno, name);
	(void)__sm_subdb_remove(bt, name);
	snprintf(kbuf, sizeof(kbuf), "k%u", (unsigned)pgno);
	memset(&key, 0, sizeof(key));
	key.data = kbuf;
	key.size = (u_int32_t)strlen(kbuf);
	ret = bt->meta->del(bt->meta, __sm_cur_txn(bt), &key, 0);
	return (ret == DB_NOTFOUND ? DBSQL_OK : __sm_errmap(ret));
}

/*
 * __sm_subdb_remove --
 *	dbremove sub-database 'name' in a child of the current txn, so a
 *	refusal (another handle open: DB_LOCK_NOTGRANTED) leaves the
 *	caller's txn untouched.  Returns the libdb error.
 */
static int
__sm_subdb_remove(sm_shared_t *bt, const char *name)
{
	DB_ENV *dbenv;
	DB_TXN *child;
	int ret;

	dbenv = bt->env->dbenv;
	if ((ret = dbenv->txn_begin(dbenv, __sm_cur_txn(bt), &child,
	    DB_TXN_NOWAIT)) != 0)
		return (ret);
	ret = dbenv->dbremove(dbenv, child, (bt->flags & SM_MEMORY) ?
	    (bt->env->home != NULL ? SM_TEMP_FILE : NULL) : bt->path, name, 0);
	if (ret == 0 || ret == ENOENT)
		return (child->commit(child, 0) == 0 ? 0 : ret);
	(void)child->abort(child);
	return (ret);
}

/*
 * __sm_clear_table --
 *	Delete every entry of table pgno, adding the count to *changes.
 *
 * PUBLIC: int __sm_clear_table __P((sm_t *, int, int64_t *));
 */
int
__sm_clear_table(sm_t *p, int pgno, int64_t *changesp)
{
	sm_shared_t *bt;
	sm_dbh_t *h;
	u_int32_t count;
	int rc, ret;

	bt = p->bt;
	if ((rc = __sm_dbh_get(bt, (u_int32_t)pgno, NULL, 0, &h)) != DBSQL_OK)
		return (rc);
	__sm_cursors_invalidate_table(bt, (u_int32_t)pgno);
	count = 0;
	ret = h->dbp->truncate(h->dbp, __sm_cur_txn(bt), &count, 0);
	if (ret != 0)
		return (__sm_errmap(ret));
	if (changesp != NULL)
		*changesp += count;
	return (DBSQL_OK);
}

/*
 * __sm_clear_table_of_cursor --
 *	PUBLIC: int __sm_clear_table_of_cursor __P((sm_cursor_t *));
 */
int
__sm_clear_table_of_cursor(sm_cursor_t *cur)
{
	return (__sm_clear_table(cur->btree, (int)cur->pgno, 0));
}

/*
 * __sm_get_meta --
 *	Read meta value idx.  BTREE_DATA_VERSION is virtual: it changes
 *	when the database changes under this connection.  Meta 0 (free page
 *	count) is always 0: libdb manages free space.
 *
 * PUBLIC: void __sm_get_meta __P((sm_t *, int, u_int32_t *));
 */
void
__sm_get_meta(sm_t *p, int idx, u_int32_t *value)
{
	sm_shared_t *bt;

	bt = p->bt;
	if (idx == BTREE_DATA_VERSION) {
		/*
		 * Upstream: changes when another connection commits, not
		 * when this one does.  1 + commits visible in this snapshot
		 * (SM_META_COMMITS) - this connection's own commits.
		 */
		u_int32_t n;

		/* Private (memory, temp) databases have no other
		 * connections: always 1, as upstream. */
		if (bt->flags & (SM_MEMORY | SM_EPHEMERAL)) {
			*value = 1 + p->data_version_base;
			return;
		}
		if (__sm_meta_get_raw(bt, SM_META_COMMITS, &n) != DBSQL_OK)
			n = bt->commits_base + bt->own_seen;
		*value = 1 + (n - bt->commits_base - bt->own_seen) +
		    p->data_version_base;
		return;
	}
	if (idx == BTREE_LARGEST_ROOT_PAGE) {
		/* Upstream: the largest root page, or 0 when the database
		 * is not auto-vacuum (the PRAGMA tests it for that). */
		u_int32_t on;
		*value = 0;
		if (__sm_meta_get_raw(bt, SM_META_AUTOVAC, &on) == DBSQL_OK &&
		    on && __sm_meta_get_raw(bt, idx, value) == DBSQL_OK &&
		    *value < 1)
			*value = 1;
		return;
	}
	if (idx == BTREE_FREE_PAGE_COUNT) {
		*value = 0;
		return;
	}
	if (__sm_meta_get_raw(bt, idx, value) != DBSQL_OK)
		*value = 0;
}

/*
 * __sm_update_meta --
 *	Write meta value idx (within a write transaction).
 *
 * PUBLIC: int __sm_update_meta __P((sm_t *, int, u_int32_t));
 */
int
__sm_update_meta(sm_t *p, int idx, u_int32_t value)
{
	sm_shared_t *bt;

	bt = p->bt;
	if (bt->flags & SM_EPHEMERAL)
		return (DBSQL_OK);
	if (bt->meta == NULL)
		return (DBSQL_CORRUPT);
	return (__sm_meta_put_raw(bt, idx, value));
}

/*
 * __sm_new_db --
 *	Reset an empty database's meta (used by VACUUM's target).
 *
 * PUBLIC: int __sm_new_db __P((sm_t *));
 */
int
__sm_new_db(sm_t *p)
{
	(void)p;
	return (DBSQL_OK);
}

/*
 * __sm_schema --
 *	The schema blob shared by every user of this database.
 *
 * PUBLIC: void *__sm_schema __P((sm_t *, int, void (*)(void *)));
 */
void *
__sm_schema(sm_t *p, int n_bytes, void (*x_free)(void *))
{
	sm_shared_t *bt;

	bt = p->bt;
	if (bt->schema == NULL && n_bytes > 0) {
		bt->schema = __db_malloc_zero(0, (u_int64_t)n_bytes);
		bt->free_schema = x_free;
	}
	return (bt->schema);
}

/*
 * __sm_schema_locked --
 *	No shared cache: the schema is never locked by another connection.
 *
 * PUBLIC: int __sm_schema_locked __P((sm_t *));
 */
int
__sm_schema_locked(sm_t *p)
{
	(void)p;
	return (DBSQL_OK);
}

/*
 * Settings that SQLite keeps per b-tree.  libdb manages its own cache and
 * free space, so most are stored and reported back unchanged.
 */

/* PUBLIC: int __sm_set_cache_size __P((sm_t *, int)); */
int
__sm_set_cache_size(sm_t *p, int n)
{
	p->bt->cache_size = n;
	return (DBSQL_OK);
}

/* PUBLIC: int __sm_set_spill_size __P((sm_t *, int)); */
int
__sm_set_spill_size(sm_t *p, int n)
{
	(void)p;
	return (n);
}

/* PUBLIC: int __sm_set_mmap_limit __P((sm_t *, int64_t)); */
int
__sm_set_mmap_limit(sm_t *p, int64_t n)
{
	(void)p;
	(void)n;
	return (DBSQL_OK);
}

/*
 * __sm_set_pager_flags --
 *	PRAGMA synchronous: OFF -> DB_TXN_NOSYNC, NORMAL ->
 *	DB_TXN_WRITE_NOSYNC, FULL/EXTRA -> synchronous commit.
 *
 * PUBLIC: int __sm_set_pager_flags __P((sm_t *, unsigned));
 */
int
__sm_set_pager_flags(sm_t *p, unsigned flags)
{
	sm_shared_t *bt;

	bt = p->bt;
	bt->pager_flags = flags;
	if (bt->txn == NULL)
		return (DBSQL_OK);
	switch (flags & PAGER_SYNCHRONOUS_MASK) {
	case PAGER_SYNCHRONOUS_OFF:
		bt->txn->commit_flags = DB_TXN_NOSYNC;
		break;
	case PAGER_SYNCHRONOUS_NORMAL:
		bt->txn->commit_flags = DB_TXN_WRITE_NOSYNC;
		break;
	default:
		bt->txn->commit_flags = DB_TXN_SYNC;
		break;
	}
	return (DBSQL_OK);
}

/*
 * __sm_set_page_size --
 *	PRAGMA page_size: applies to tables created from now on.  Fixed
 *	(DBSQL_READONLY) once the database has tables, as upstream.
 *
 * PUBLIC: int __sm_set_page_size __P((sm_t *, int, int, int));
 */
int
__sm_set_page_size(sm_t *p, int page_size, int n_reserve, int fix)
{
	sm_shared_t *bt;
	u_int32_t largest;

	(void)n_reserve;
	bt = p->bt;
	/* As upstream: no change requested is not an error when fixed. */
	if (page_size <= 0 || (u_int32_t)page_size == bt->page_size)
		return (DBSQL_OK);
	if (bt->page_size_fixed)
		return (DBSQL_READONLY);
	if (page_size >= 512 && page_size <= 65536 &&
	    (page_size & (page_size - 1)) == 0) {
		largest = 0;
		if (bt->meta != NULL)
			(void)__sm_meta_get_raw(bt, BTREE_LARGEST_ROOT_PAGE,
			    &largest);
		if (largest <= 1)
			bt->page_size = (u_int32_t)page_size;
	}
	if (fix)
		bt->page_size_fixed = 1;
	return (DBSQL_OK);
}

/* PUBLIC: int __sm_get_page_size __P((sm_t *)); */
int
__sm_get_page_size(sm_t *p)
{
	return ((int)p->bt->page_size);
}

/* PUBLIC: int __sm_get_requested_reserve __P((sm_t *)); */
int
__sm_get_requested_reserve(sm_t *p)
{
	(void)p;
	return (0);
}

/* PUBLIC: int __sm_get_reserve_no_mutex __P((sm_t *)); */
int
__sm_get_reserve_no_mutex(sm_t *p)
{
	(void)p;
	return (0);
}

/* PUBLIC: pgno_t __sm_max_page_count __P((sm_t *, pgno_t)); */
pgno_t
__sm_max_page_count(sm_t *p, pgno_t n)
{
	if (n > 0)
		p->bt->max_page_count = n;
	return (p->bt->max_page_count ? p->bt->max_page_count : 0xfffffffe);
}

/*
 * __sm_last_page --
 *	The largest valid "root page" number.  Upstream's schema loader
 *	rejects rootpage values above it as corruption, so it is the
 *	largest table number ever allocated (meta LARGEST_ROOT_PAGE, at
 *	least 1), not a page count.
 *
 * PUBLIC: pgno_t __sm_last_page __P((sm_t *));
 */
pgno_t
__sm_last_page(sm_t *p)
{
	u_int32_t largest;

	if (p->bt->flags & SM_EPHEMERAL)
		return (2);
	if (__sm_meta_get_raw(p->bt, BTREE_LARGEST_ROOT_PAGE, &largest) !=
	    DBSQL_OK || largest < 1)
		largest = 1;
	return ((pgno_t)largest);
}

/* PUBLIC: int __sm_secure_delete __P((sm_t *, int)); */
int
__sm_secure_delete(sm_t *p, int on)
{
	if (p == NULL)
		return (0);
	if (on >= 0)
		p->bt->secure_delete = on;
	return (p->bt->secure_delete);
}

/*
 * __sm_set_auto_vacuum --
 *	PRAGMA auto_vacuum = NONE(0) | FULL(1) | INCREMENTAL(2).  As
 *	upstream: whether auto-vacuum is on is fixed when the database is
 *	created (SM_META_AUTOVAC); changing it afterwards returns
 *	DBSQL_READONLY (VACUUM changes it by rebuilding).  FULL versus
 *	INCREMENTAL is meta[BTREE_INCR_VACUUM], which the PRAGMA's program
 *	writes.  On libdb FULL compacts at each commit and INCREMENTAL on
 *	PRAGMA incremental_vacuum (DIVERGENCES.md).
 *
 * PUBLIC: int __sm_set_auto_vacuum __P((sm_t *, int));
 */
int
__sm_set_auto_vacuum(sm_t *p, int v)
{
	sm_shared_t *bt;
	u_int32_t on;

	bt = p->bt;
	if (v < 0 || v > 2)
		return (DBSQL_OK);
	if (bt->meta != NULL &&
	    __sm_meta_get_raw(bt, SM_META_AUTOVAC, &on) == DBSQL_OK &&
	    (on != 0) != (v != 0))
		return (DBSQL_READONLY);
	bt->auto_vacuum = v;
	return (DBSQL_OK);
}

/*
 * __sm_get_auto_vacuum --
 *	The database's mode, or the requested one before it is created.
 *
 * PUBLIC: int __sm_get_auto_vacuum __P((sm_t *));
 */
int
__sm_get_auto_vacuum(sm_t *p)
{
	sm_shared_t *bt;
	u_int32_t on, incr;

	bt = p->bt;
	if (bt->meta == NULL ||
	    __sm_meta_get_raw(bt, SM_META_AUTOVAC, &on) != DBSQL_OK ||
	    __sm_meta_get_raw(bt, BTREE_INCR_VACUUM, &incr) != DBSQL_OK)
		return (bt->auto_vacuum);
	return (!on ? BTREE_AUTOVACUUM_NONE :
	    incr ? BTREE_AUTOVACUUM_INCR : BTREE_AUTOVACUUM_FULL);
}

/*
 * __sm_incr_vacuum --
 *	PRAGMA incremental_vacuum: compact every table, then report done.
 *
 * PUBLIC: int __sm_incr_vacuum __P((sm_t *));
 */
int
__sm_incr_vacuum(sm_t *p)
{
	(void)__sm_compact(p);
	return (DBSQL_DONE);
}

/*
 * __sm_compact --
 *	Compact every table and the meta sub-database, returning free
 *	pages at the end of the file to the filesystem (DB_FREE_SPACE).
 *	VACUUM and PRAGMA incremental_vacuum.
 *
 * PUBLIC: int __sm_compact __P((sm_t *));
 */
int
__sm_compact(sm_t *p)
{
	sm_shared_t *bt;
	sm_dbh_t *h;
	u_int32_t *t;
	int i, n, rc;

	bt = p->bt;
	if (bt->flags & (SM_EPHEMERAL | SM_MEMORY) || bt->meta == NULL)
		return (DBSQL_OK);
	if ((rc = __sm_table_list(p, &t, &n)) != DBSQL_OK)
		return (rc);
	for (i = 0; i < n; i++)
		if (__sm_dbh_get(bt, t[i], NULL, 0, &h) == DBSQL_OK)
			(void)h->dbp->compact(h->dbp, __sm_cur_txn(bt), NULL,
			    NULL, NULL, DB_FREE_SPACE, NULL);
	(void)bt->meta->compact(bt->meta, __sm_cur_txn(bt), NULL, NULL, NULL,
	    DB_FREE_SPACE, NULL);
	dbsql_free(t);
	return (DBSQL_OK);
}

/* PUBLIC: const char *__sm_get_filename __P((sm_t *)); */
const char *
__sm_get_filename(sm_t *p)
{
	return (p->bt->filename);
}

/*
 * __sm_get_journalname --
 *	There is no rollback journal file (libdb's log replaces it); NULL
 *	makes the multi-database commit leave this database out of the
 *	super-journal.
 *
 * PUBLIC: const char *__sm_get_journalname __P((sm_t *));
 */
const char *
__sm_get_journalname(sm_t *p)
{
	(void)p;
	return (NULL);
}

/* PUBLIC: int __sm_is_readonly __P((sm_t *)); */
int
__sm_is_readonly(sm_t *p)
{
	return (p->bt->read_only);
}

/* PUBLIC: int __sm_set_version __P((sm_t *, int)); */
int
__sm_set_version(sm_t *p, int v)
{
	(void)p;
	(void)v;
	return (DBSQL_OK);
}

/* PUBLIC: int __header_size_btree __P((void)); */
int
__header_size_btree(void)
{
	return (0);
}

/*
 * __sm_checkpoint --
 *	PRAGMA wal_checkpoint maps to a libdb checkpoint.
 *
 * PUBLIC: int __sm_checkpoint __P((sm_t *, int, int *, int *));
 */
int
__sm_checkpoint(sm_t *p, int mode, int *n_log, int *n_ckpt)
{
	(void)mode;
	if (n_log != NULL)
		*n_log = 0;
	if (n_ckpt != NULL)
		*n_ckpt = 0;
	if (p->bt->flags & (SM_MEMORY | SM_EPHEMERAL))
		return (DBSQL_OK);
	return (__sm_errmap(p->bt->env->dbenv->txn_checkpoint(
	    p->bt->env->dbenv, 0, 0, 0)));
}

/* PUBLIC: void __sm_clear_cache __P((sm_t *)); */
void
__sm_clear_cache(sm_t *p)
{
	(void)p;
}
