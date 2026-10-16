/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (c) 2007-2026  The DBSQL Group, Inc.
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
 * sm_backup.c --
 *	The online backup API (dbsql_backup_*) and __sm_copy_file (VACUUM),
 *	as a logical copy: each table and index of the source is copied
 *	entry by entry into the destination inside the destination's write
 *	transaction, together with the meta values.  A "page" in the
 *	backup API's accounting is one sub-database (DIVERGENCES.md).
 */

#include "dbsql_int.h"
#include "sm_int.h"

#include <errno.h>
#include <stdlib.h>

struct dbsql_backup_t {
	DBSQL *dest_db;
	sm_t *dest;
	DBSQL *src_db;
	sm_t *src;
	u_int32_t *tables;		/* Source table numbers to copy. */
	int n_tables;
	int next;			/* Next table to copy. */
	int started;			/* Destination cleared. */
	int rc;
};

/*
 * __sm_copy_one --
 *	Copy every entry of source table pgno into the same-numbered
 *	destination table (created with the same kind if missing).
 */
static int
__sm_copy_one(sm_t *dest, sm_t *src, u_int32_t pgno)
{
	sm_dbh_t *sh, *dh;
	DBC *dbc;
	DBT k, d;
	int rc, ret;

	if ((rc = __sm_dbh_get(src->bt, pgno, NULL, 0, &sh)) != DBSQL_OK)
		return (rc);
	if ((rc = __sm_dbh_get(dest->bt, pgno, NULL,
	    sh->is_intkey ? BTREE_INTKEY : BTREE_BLOBKEY, &dh)) != DBSQL_OK)
		return (rc);
	if ((ret = sh->dbp->cursor(sh->dbp, __sm_cur_txn(src->bt), &dbc, 0))
	    != 0)
		return (__sm_errmap(ret));
	memset(&k, 0, sizeof(k));
	memset(&d, 0, sizeof(d));
	k.flags = d.flags = DB_DBT_REALLOC;
	while ((ret = dbc->get(dbc, &k, &d, DB_NEXT)) == 0)
		if ((ret = dh->dbp->put(dh->dbp, __sm_cur_txn(dest->bt), &k,
		    &d, 0)) != 0)
			break;
	(void)dbc->close(dbc);
	free(k.data);
	free(d.data);
	return (ret == DB_NOTFOUND ? DBSQL_OK : __sm_errmap(ret));
}

/*
 * __sm_table_list --
 *	All table numbers of a database: 1 plus every "k<N>" kind record.
 *	The array is dbsql_malloc'd.
 */
/* PUBLIC: int __sm_table_list __P((sm_t *, u_int32_t **, int *)); */
int
__sm_table_list(sm_t *p, u_int32_t **tablesp, int *np)
{
	sm_shared_t *bt;
	DBC *dbc;
	DBT k, d;
	u_int32_t *t, *nt;
	int n, alloc, ret;
	char buf[SM_SUBDB_MAX];

	bt = p->bt;
	*tablesp = NULL;
	*np = 0;
	if (bt->meta == NULL)
		return (DBSQL_OK);
	if ((t = dbsql_malloc(16 * sizeof(*t))) == NULL)
		return (DBSQL_NOMEM);
	alloc = 16;
	t[0] = 1;
	n = 1;
	if ((ret = bt->meta->cursor(bt->meta, __sm_cur_txn(bt), &dbc, 0)) != 0) {
		dbsql_free(t);
		return (__sm_errmap(ret));
	}
	memset(&k, 0, sizeof(k));
	memset(&d, 0, sizeof(d));
	k.flags = d.flags = DB_DBT_REALLOC;
	while ((ret = dbc->get(dbc, &k, &d, DB_NEXT)) == 0) {
		if (k.size < 2 || k.size >= sizeof(buf) ||
		    ((char *)k.data)[0] != 'k')
			continue;
		memcpy(buf, (char *)k.data + 1, k.size - 1);
		buf[k.size - 1] = '\0';
		if (n == alloc) {
			nt = dbsql_realloc(t, (int)(2 * (size_t)alloc *
			    sizeof(*t)));
			if (nt == NULL) {
				ret = ENOMEM;
				break;
			}
			t = nt;
			alloc *= 2;
		}
		t[n++] = (u_int32_t)strtoul(buf, NULL, 10);
	}
	(void)dbc->close(dbc);
	free(k.data);
	free(d.data);
	if (ret != DB_NOTFOUND) {
		dbsql_free(t);
		return (__sm_errmap(ret));
	}
	*tablesp = t;
	*np = n;
	return (DBSQL_OK);
}

/*
 * __sm_clear_all --
 *	Empty the destination: drop every table but the schema table, clear
 *	that one.
 */
static int
__sm_clear_all(sm_t *dest)
{
	u_int32_t *t;
	int n, i, rc, moved;

	if ((rc = __sm_table_list(dest, &t, &n)) != DBSQL_OK)
		return (rc);
	for (i = 0; i < n && rc == DBSQL_OK; i++)
		rc = t[i] == 1 ? __sm_clear_table(dest, 1, NULL) :
		    __sm_drop_table(dest, (int)t[i], &moved);
	dbsql_free(t);
	return (rc);
}

/*
 * __sm_copy_meta --
 *	Copy the meta values the SQL layer uses.
 */
static int
__sm_copy_meta(sm_t *dest, sm_t *src, int bump_schema)
{
	u_int32_t v, old;
	int idx, rc;

	/* Raw values: the table counter and the auto_vacuum mode too. */
	for (idx = 1; idx < DBSQL_N_BTREE_META; idx++) {
		if (idx == BTREE_DATA_VERSION)
			continue;
		if ((rc = __sm_meta_get_raw(src->bt, idx, &v)) != DBSQL_OK)
			return (rc);
		if (idx == BTREE_SCHEMA_VERSION && bump_schema) {
			if ((rc = __sm_meta_get_raw(dest->bt, idx, &old)) !=
			    DBSQL_OK)
				return (rc);
			if (v <= old)
				v = old + 1;
		}
		if ((rc = __sm_meta_put_raw(dest->bt, idx, v)) != DBSQL_OK)
			return (rc);
	}
	return (DBSQL_OK);
}

/*
 * __sm_copy_file --
 *	Replace the whole content of 'to' with that of 'from' (in-place
 *	VACUUM).  Both are in write/read transactions already.
 *
 * PUBLIC: int __sm_copy_file __P((sm_t *, sm_t *));
 */
int
__sm_copy_file(sm_t *to, sm_t *from)
{
	u_int32_t *t;
	int n, i, rc;

	if ((rc = __sm_clear_all(to)) != DBSQL_OK)
		return (rc);
	if ((rc = __sm_table_list(from, &t, &n)) != DBSQL_OK)
		return (rc);
	for (i = 0; i < n && rc == DBSQL_OK; i++)
		rc = __sm_copy_one(to, from, t[i]);
	dbsql_free(t);
	if (rc == DBSQL_OK)
		rc = __sm_copy_meta(to, from, 0);
	if (rc == DBSQL_OK)
		rc = __sm_compact(to);
	return (rc);
}

/*
 * __sm_find_btree --
 *	The sm_t of database 'name' of connection db, or NULL with an
 *	error left in 'err_db'.
 */
static sm_t *
__sm_find_btree(DBSQL *err_db, DBSQL *db, const char *name)
{
	int i;

	i = __find_db_name(db, name);
	if (i < 0) {
		__error_with_msg(err_db, DBSQL_ERROR, "unknown database %s", name);
		return (NULL);
	}
	return (db->dbs[i].bt);
}

/*
 * dbsql_backup_init --
 *	Start an online backup of source_db.source_name into
 *	dest_db.dest_name.
 *
 * EXTERN: dbsql_backup_t *dbsql_backup_init __P((DBSQL *, const char *,
 * EXTERN:     DBSQL *, const char *));
 */
dbsql_backup_t *
dbsql_backup_init(DBSQL *dest_db, const char *dest_name, DBSQL *src_db,
    const char *src_name)
{
	dbsql_backup_t *b;

	dbsql_mutex_enter(src_db->mutex);
	dbsql_mutex_enter(dest_db->mutex);
	b = NULL;
	if (src_db == dest_db) {
		__error_with_msg(dest_db, DBSQL_ERROR,
		    "source and destination must be distinct");
		goto out;
	}
	if ((b = dbsql_malloc(sizeof(*b))) == NULL) {
		__dbsql_error(dest_db, DBSQL_NOMEM);
		goto out;
	}
	memset(b, 0, sizeof(*b));
	b->dest_db = dest_db;
	b->src_db = src_db;
	b->dest = __sm_find_btree(dest_db, dest_db, dest_name);
	b->src = __sm_find_btree(dest_db, src_db, src_name);
	if (b->dest == NULL || b->src == NULL) {
		dbsql_free(b);
		b = NULL;
		goto out;
	}
	b->src->bt->n_backup++;
out:	dbsql_mutex_leave(dest_db->mutex);
	dbsql_mutex_leave(src_db->mutex);
	return (b);
}

/*
 * dbsql_backup_step --
 *	Copy up to n_page tables (all if n_page < 0).  Returns DBSQL_DONE
 *	when everything is copied and committed.
 *
 * EXTERN: int dbsql_backup_step __P((dbsql_backup_t *, int));
 */
int
dbsql_backup_step(dbsql_backup_t *b, int n_page)
{
	int rc, n;

	if (b == NULL)
		return (DBSQL_MISUSE_BKPT);
	if (b->rc != DBSQL_OK)
		return (b->rc);
	dbsql_mutex_enter(b->src_db->mutex);
	dbsql_mutex_enter(b->dest_db->mutex);
	rc = DBSQL_OK;
	if (!b->started) {
		if ((rc = __sm_begin_trans(b->src, 0, 0)) == DBSQL_OK &&
		    (rc = __sm_begin_trans(b->dest, 1, 0)) == DBSQL_OK &&
		    (rc = __sm_table_list(b->src, &b->tables, &b->n_tables)) ==
		    DBSQL_OK)
			rc = __sm_clear_all(b->dest);
		if (rc == DBSQL_OK)
			b->started = 1;
	}
	for (n = 0; rc == DBSQL_OK && b->next < b->n_tables &&
	    (n_page < 0 || n < n_page); n++)
		rc = __sm_copy_one(b->dest, b->src, b->tables[b->next++]);
	if (rc == DBSQL_OK && b->next >= b->n_tables) {
		if ((rc = __sm_copy_meta(b->dest, b->src, 1)) == DBSQL_OK &&
		    (rc = __sm_commit(b->dest)) == DBSQL_OK)
			rc = DBSQL_DONE;
		(void)__sm_commit(b->src);
		__reset_all_schemas_of_connection(b->dest_db);
	}
	if (rc != DBSQL_OK && rc != DBSQL_DONE && (rc & 0xff) != DBSQL_BUSY) {
		b->rc = rc;
		(void)__sm_rollback(b->dest, rc, 0);
	}
	dbsql_mutex_leave(b->dest_db->mutex);
	dbsql_mutex_leave(b->src_db->mutex);
	return (rc);
}

/*
 * dbsql_backup_finish --
 *	EXTERN: int dbsql_backup_finish __P((dbsql_backup_t *));
 */
int
dbsql_backup_finish(dbsql_backup_t *b)
{
	int rc;

	if (b == NULL)
		return (DBSQL_OK);
	dbsql_mutex_enter(b->src_db->mutex);
	b->src->bt->n_backup--;
	if (b->started && b->next < b->n_tables)
		(void)__sm_rollback(b->dest, DBSQL_OK, 0);
	rc = b->rc == DBSQL_DONE ? DBSQL_OK : b->rc;
	dbsql_mutex_leave(b->src_db->mutex);
	dbsql_free(b->tables);
	dbsql_free(b);
	return (rc);
}

/* EXTERN: int dbsql_backup_remaining __P((dbsql_backup_t *)); */
int
dbsql_backup_remaining(dbsql_backup_t *b)
{
	return (b == NULL ? 0 : b->n_tables - b->next);
}

/* EXTERN: int dbsql_backup_pagecount __P((dbsql_backup_t *)); */
int
dbsql_backup_pagecount(dbsql_backup_t *b)
{
	return (b == NULL ? 0 : b->n_tables);
}

/*
 * Hooks upstream's pager calls to keep backups current; a logical
 * backup re-reads from a snapshot instead.
 */
/* PUBLIC: void __backup_restart __P((dbsql_backup_t *)); */
void
__backup_restart(dbsql_backup_t *b)
{
	(void)b;
}

/* PUBLIC: void __backup_update __P((dbsql_backup_t *, pgno_t,
 * PUBLIC:     const u_int8_t *)); */
void
__backup_update(dbsql_backup_t *b, pgno_t pgno, const u_int8_t *data)
{
	(void)b;
	(void)pgno;
	(void)data;
}
