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
 * sm_cursor.c --
 *	Cursors of the storage manager: SQLite's BtCursor semantics on
 *	libdb DBC cursors.
 *
 *	Each cursor holds the current entry in buffers it owns (key: the
 *	encoded rowid or the index record; data: the row record), so the
 *	pointers __sm_payload_fetch returns stay valid until the cursor
 *	moves.  Before a txn level ends, every DBC opened in it is closed
 *	and the cursor keeps its key (CURSOR_REQUIRESEEK); the next access
 *	reopens a DBC in the current txn and seeks back, as upstream's
 *	saveCursorPosition/restoreCursorPosition do.
 */

#include "dbsql_int.h"
#include "vdbe_int.h"
#include "sm_int.h"

#include <stdlib.h>

/*
 * __sm_cursor_size --
 *	PUBLIC: int __sm_cursor_size __P((void));
 */
int
__sm_cursor_size(void)
{
	return ((int)ROUND8(sizeof(sm_cursor_t)));
}

/*
 * __sm_cursor_zero --
 *	PUBLIC: void __sm_cursor_zero __P((sm_cursor_t *));
 */
void
__sm_cursor_zero(sm_cursor_t *cur)
{
	memset(cur, 0, sizeof(*cur));
}

/*
 * __sm_fake_valid_cursor --
 *	A cursor that always answers "not moved".
 *
 * PUBLIC: sm_cursor_t *__sm_fake_valid_cursor __P((void));
 */
sm_cursor_t *
__sm_fake_valid_cursor(void)
{
	static u_int8_t fake = CURSOR_VALID;

	return ((sm_cursor_t *)&fake);
}

/*
 * __sm_dbc_open --
 *	Make sure the cursor has a DBC in the database's current txn.
 */
static int
__sm_dbc_open(sm_cursor_t *cur)
{
	DB_TXN *txn;
	int ret;

	if (cur->empty) {
		/* The database was created since: open the real table. */
		if (cur->bt->meta == NULL)
			return (DBSQL_EMPTY);
		cur->empty = 0;
		cur->dbh = NULL;
	}
	txn = __sm_cur_txn(cur->bt);
	if (cur->dbc != NULL && cur->dbc_txn == txn)
		return (DBSQL_OK);
	if (cur->dbc != NULL) {
		(void)cur->dbc->close(cur->dbc);
		cur->dbc = NULL;
	}
	if (cur->dbh == NULL) {
		int rc = __sm_dbh_get(cur->bt, cur->pgno, cur->key_info, 0,
		    &cur->dbh);
		if (rc != DBSQL_OK)
			return (rc);
	}
	if ((ret = cur->dbh->dbp->cursor(cur->dbh->dbp, txn, &cur->dbc,
	    0)) != 0)
		return (__sm_errmap(ret));
	cur->dbc_txn = txn;
	return (DBSQL_OK);
}

/*
 * __sm_cursor_set_current --
 *	After a successful DBC get: cache the decoded rowid.
 */
static void
__sm_cursor_set_current(sm_cursor_t *cur)
{
	cur->state = CURSOR_VALID;
	cur->at_eof = 0;
	cur->saved = 0;
	if (cur->dbh->is_intkey && cur->key.size == SM_ROWID_KEYLEN)
		cur->rowid = __sm_key_to_rowid(cur->key.data);
}

/*
 * __sm_dbc_get --
 *	One DBC get into the cursor's buffers.  DB_NOTFOUND leaves the
 *	cursor CURSOR_INVALID and returns DBSQL_OK with *found = 0.
 */
static int
__sm_dbc_get(sm_cursor_t *cur, u_int32_t op, int *found)
{
	int ret;

	cur->key.flags = DB_DBT_REALLOC;
	cur->data.flags = DB_DBT_REALLOC;
	ret = cur->dbc->get(cur->dbc, &cur->key, &cur->data, op);
	if (ret == DB_NOTFOUND || ret == DB_KEYEMPTY) {
		cur->state = CURSOR_INVALID;
		*found = 0;
		return (DBSQL_OK);
	}
	if (ret != 0) {
		cur->state = CURSOR_FAULT;
		cur->fault = __sm_errmap(ret);
		return (cur->fault);
	}
	*found = 1;
	__sm_cursor_set_current(cur);
	return (DBSQL_OK);
}

/*
 * __sm_cursors_save --
 *	Close the DBC of every cursor opened in 'txn' (which is about to
 *	end), keeping the position to restore later.
 *
 * PUBLIC: void __sm_cursors_save __P((sm_shared_t *, DB_TXN *));
 */
void
__sm_cursors_save(sm_shared_t *bt, DB_TXN *txn)
{
	sm_cursor_t *cur;

	for (cur = bt->cursors; cur != NULL; cur = cur->next) {
		if (cur->dbc == NULL || cur->dbc_txn != txn)
			continue;
		(void)cur->dbc->close(cur->dbc);
		cur->dbc = NULL;
		cur->dbc_txn = NULL;
		cur->dbh = NULL;	/* handle may be discarded too */
		if (cur->state == CURSOR_VALID) {
			cur->state = CURSOR_REQUIRESEEK;
			cur->saved = 1;
		}
	}
}

/*
 * __sm_cursors_invalidate_table --
 *	A table is being cleared: its cursors no longer point anywhere.
 *
 * PUBLIC: void __sm_cursors_invalidate_table __P((sm_shared_t *,
 * PUBLIC:     u_int32_t));
 */
void
__sm_cursors_invalidate_table(sm_shared_t *bt, u_int32_t pgno)
{
	sm_cursor_t *cur;

	for (cur = bt->cursors; cur != NULL; cur = cur->next)
		if (cur->pgno == pgno && cur->state != CURSOR_FAULT) {
			if (cur->dbc != NULL) {
				(void)cur->dbc->close(cur->dbc);
				cur->dbc = NULL;
			}
			cur->state = CURSOR_INVALID;
			cur->saved = 0;
		}
}

/*
 * __sm_cursor --
 *	Open a cursor on table i_table.
 *
 * PUBLIC: int __sm_cursor __P((sm_t *, pgno_t, int, struct key_info_t *,
 * PUBLIC:     sm_cursor_t *));
 */
int
__sm_cursor(sm_t *p, pgno_t i_table, int wr_flag, key_info_t *ki,
    sm_cursor_t *cur)
{
	sm_shared_t *bt;
	int rc;

	bt = p->bt;
	memset(cur, 0, sizeof(*cur));
	cur->btree = p;
	cur->bt = bt;
	cur->pgno = i_table;
	cur->wr_flag = wr_flag;
	cur->key_info = ki;
	cur->state = CURSOR_INVALID;
	rc = __sm_dbh_get(bt, i_table, ki, 0, &cur->dbh);
	if (rc == DBSQL_EMPTY) {
		/* Table of a not yet created database: an empty cursor. */
		cur->empty = 1;
		rc = DBSQL_OK;
	}
	if (rc != DBSQL_OK)
		return (rc);
	cur->next = bt->cursors;
	bt->cursors = cur;
	return (DBSQL_OK);
}

/*
 * __sm_close_cursor --
 *	PUBLIC: int __sm_close_cursor __P((sm_cursor_t *));
 */
int
__sm_close_cursor(sm_cursor_t *cur)
{
	sm_cursor_t **pp;

	if (cur->bt == NULL)
		return (DBSQL_OK);
	if (cur->dbc != NULL)
		(void)cur->dbc->close(cur->dbc);
	for (pp = &cur->bt->cursors; *pp != NULL; pp = &(*pp)->next)
		if (*pp == cur) {
			*pp = cur->next;
			break;
		}
	free(cur->key.data);
	free(cur->data.data);
	memset(cur, 0, sizeof(*cur));
	return (DBSQL_OK);
}

/*
 * __sm_closes_with_cursor --
 *	PUBLIC: int __sm_closes_with_cursor __P((sm_t *, sm_cursor_t *));
 */
int
__sm_closes_with_cursor(sm_t *p, sm_cursor_t *cur)
{
	sm_cursor_t *c;

	for (c = p->bt->cursors; c != NULL; c = c->next)
		if (c != cur)
			return (0);
	return (p->bt->cursors == cur);
}

/*
 * __sm_cursor_restore_position --
 *	Bring a REQUIRESEEK cursor back to its saved key (or the next one).
 *	*different is set when the exact entry no longer exists.
 */
static int
__sm_cursor_restore_position(sm_cursor_t *cur, int *different)
{
	int rc, found;

	*different = 0;
	if (cur->state == CURSOR_FAULT)
		return (cur->fault);
	if (cur->state != CURSOR_REQUIRESEEK)
		return (DBSQL_OK);
	if ((rc = __sm_dbc_open(cur)) != DBSQL_OK)
		return (rc);
	if ((rc = __sm_dbc_get(cur, DB_SET_RANGE, &found)) != DBSQL_OK)
		return (rc);
	if (!found) {
		cur->state = CURSOR_INVALID;
		*different = 1;
	}
	return (DBSQL_OK);
}

/*
 * __sm_cursor_restore --
 *	PUBLIC: int __sm_cursor_restore __P((sm_cursor_t *, int *));
 */
int
__sm_cursor_restore(sm_cursor_t *cur, int *different_row)
{
	return (__sm_cursor_restore_position(cur, different_row));
}

/*
 * __sm_cursor_has_moved --
 *	PUBLIC: int __sm_cursor_has_moved __P((sm_cursor_t *));
 */
int
__sm_cursor_has_moved(sm_cursor_t *cur)
{
	return (CURSOR_VALID != *(u_int8_t *)cur);
}

/*
 * __sm_ensure --
 *	Common prologue: restore a saved position and make sure a DBC
 *	exists in the current txn.
 */
static int
__sm_ensure(sm_cursor_t *cur)
{
	int rc, diff;

	if (cur->state == CURSOR_FAULT)
		return (cur->fault);
	if (cur->state == CURSOR_REQUIRESEEK &&
	    (rc = __sm_cursor_restore_position(cur, &diff)) != DBSQL_OK)
		return (rc);
	return (__sm_dbc_open(cur));
}

/*
 * __sm_first --
 *	PUBLIC: int __sm_first __P((sm_cursor_t *, int *));
 */
int
__sm_first(sm_cursor_t *cur, int *res)
{
	int rc, found;

	if ((rc = __sm_ensure(cur)) == DBSQL_EMPTY) {
		cur->state = CURSOR_INVALID;
		*res = 1;
		return (DBSQL_OK);
	}
	if (rc != DBSQL_OK)
		return (rc);
	rc = __sm_dbc_get(cur, DB_FIRST, &found);
	*res = !found;
	if (!found)
		cur->at_eof = 1;
	return (rc);
}

/*
 * __sm_last --
 *	PUBLIC: int __sm_last __P((sm_cursor_t *, int *));
 */
int
__sm_last(sm_cursor_t *cur, int *res)
{
	int rc, found;

	if ((rc = __sm_ensure(cur)) == DBSQL_EMPTY) {
		cur->state = CURSOR_INVALID;
		*res = 1;
		return (DBSQL_OK);
	}
	if (rc != DBSQL_OK)
		return (rc);
	rc = __sm_dbc_get(cur, DB_LAST, &found);
	*res = !found;
	return (rc);
}

/*
 * __sm_is_empty --
 *	PUBLIC: int __sm_is_empty __P((sm_cursor_t *, int *));
 */
int
__sm_is_empty(sm_cursor_t *cur, int *res)
{
	return (__sm_first(cur, res));
}

/*
 * __sm_step --
 *	Move to the next (DB_NEXT) or previous (DB_PREV) entry.  Returns
 *	DBSQL_DONE at the end, as upstream's __sm_next/__sm_previous.
 */
static int
__sm_step(sm_cursor_t *cur, u_int32_t op)
{
	int rc, found;

	if (cur->state == CURSOR_SKIPNEXT) {
		/* A delete left the cursor on the entry to return next. */
		cur->state = CURSOR_VALID;
		if (cur->skip_next == (op == DB_NEXT ? 1 : -1)) {
			cur->skip_next = 0;
			return (DBSQL_OK);
		}
		cur->skip_next = 0;
	}
	if (cur->state == CURSOR_INVALID)
		return (DBSQL_DONE);
	if ((rc = __sm_ensure(cur)) != DBSQL_OK)
		return (rc);
	if (cur->state == CURSOR_INVALID)
		return (DBSQL_DONE);
	if ((rc = __sm_dbc_get(cur, op, &found)) != DBSQL_OK)
		return (rc);
	if (!found) {
		cur->at_eof = 1;
		return (DBSQL_DONE);
	}
	return (DBSQL_OK);
}

/*
 * __sm_next --
 *	PUBLIC: int __sm_next __P((sm_cursor_t *, int));
 */
int
__sm_next(sm_cursor_t *cur, int flags)
{
	(void)flags;
	return (__sm_step(cur, DB_NEXT));
}

/*
 * __sm_previous --
 *	PUBLIC: int __sm_previous __P((sm_cursor_t *, int));
 */
int
__sm_previous(sm_cursor_t *cur, int flags)
{
	(void)flags;
	return (__sm_step(cur, DB_PREV));
}

/*
 * __sm_eof --
 *	PUBLIC: int __sm_eof __P((sm_cursor_t *));
 */
int
__sm_eof(sm_cursor_t *cur)
{
	return (cur->state != CURSOR_VALID);
}

/*
 * __sm_seek --
 *	Position on the smallest entry >= the search key (DB_SET_RANGE),
 *	or the last entry if there is none.  *found_any is 0 for an empty
 *	table.
 */
static int
__sm_seek(sm_cursor_t *cur, int *found_any, int *past_end)
{
	int rc, found;

	*past_end = 0;
	if ((rc = __sm_dbc_get(cur, DB_SET_RANGE, &found)) != DBSQL_OK)
		return (rc);
	if (found) {
		*found_any = 1;
		return (DBSQL_OK);
	}
	*past_end = 1;
	if ((rc = __sm_dbc_get(cur, DB_LAST, &found)) != DBSQL_OK)
		return (rc);
	*found_any = found;
	return (DBSQL_OK);
}

/*
 * __sm_table_moveto --
 *	Move a rowid-table cursor to rowid int_key.  *res < 0: the cursor
 *	is on a smaller rowid; 0: exact; > 0: on a larger rowid.  An empty
 *	table leaves the cursor invalid with *res = -1, as upstream.
 *
 * PUBLIC: int __sm_table_moveto __P((sm_cursor_t *, int64_t, int, int *));
 */
int
__sm_table_moveto(sm_cursor_t *cur, int64_t int_key, int bias, int *res)
{
	int rc, any, past;

	(void)bias;
	if (cur->state == CURSOR_VALID && cur->rowid == int_key &&
	    cur->dbc != NULL && cur->dbc_txn == __sm_cur_txn(cur->bt)) {
		*res = 0;
		return (DBSQL_OK);
	}
	if (cur->state == CURSOR_FAULT)
		return (cur->fault);
	cur->state = CURSOR_INVALID;
	if ((rc = __sm_dbc_open(cur)) == DBSQL_EMPTY) {
		*res = -1;
		return (DBSQL_OK);
	}
	if (rc != DBSQL_OK)
		return (rc);
	__sm_rowid_to_key(int_key, cur->rowid_key);
	if (cur->key.ulen < SM_ROWID_KEYLEN) {
		void *k = realloc(cur->key.data, SM_ROWID_KEYLEN);
		if (k == NULL)
			return (DBSQL_NOMEM);
		cur->key.data = k;
		cur->key.ulen = SM_ROWID_KEYLEN;
	}
	memcpy(cur->key.data, cur->rowid_key, SM_ROWID_KEYLEN);
	cur->key.size = SM_ROWID_KEYLEN;
	if ((rc = __sm_seek(cur, &any, &past)) != DBSQL_OK)
		return (rc);
	if (!any) {
		*res = -1;
		return (DBSQL_OK);
	}
	if (past)
		*res = -1;
	else
		*res = cur->rowid == int_key ? 0 : 1;
	return (DBSQL_OK);
}

/*
 * __sm_index_moveto --
 *	Move an index cursor to the probe record un_key.  The comparator
 *	sees the probe through key.app_data.  *res is the comparison of
 *	the entry the cursor lands on with the probe, as upstream.
 *
 * PUBLIC: int __sm_index_moveto __P((sm_cursor_t *, unpacked_record_t *,
 * PUBLIC:     int *));
 */
int
__sm_index_moveto(sm_cursor_t *cur, unpacked_record_t *un_key, int *res)
{
	int rc, ret, found;

	if (cur->state == CURSOR_FAULT)
		return (cur->fault);
	cur->state = CURSOR_INVALID;
	if ((rc = __sm_dbc_open(cur)) == DBSQL_EMPTY) {
		*res = -1;
		return (DBSQL_OK);
	}
	if (rc != DBSQL_OK)
		return (rc);
	/*
	 * DB_SET_RANGE with the probe: the comparator orders by the
	 * unpacked record in app_data and never reads the probe's bytes.
	 * libdb overwrites the key DBT with the found key, so the probe is
	 * the cursor's own key buffer (DB_DBT_REALLOC), emptied first.
	 */
	cur->key.flags = DB_DBT_REALLOC;
	cur->data.flags = DB_DBT_REALLOC;
	if (cur->key.data == NULL) {
		if ((cur->key.data = malloc(SM_ROWID_KEYLEN)) == NULL)
			return (DBSQL_NOMEM);
		cur->key.ulen = SM_ROWID_KEYLEN;
	}
	((u_int8_t *)cur->key.data)[0] = 0;
	cur->key.size = 1;
	cur->key.app_data = un_key;
	ret = cur->dbc->get(cur->dbc, &cur->key, &cur->data, DB_SET_RANGE);
	cur->key.app_data = NULL;
	if (ret == 0) {
		__sm_cursor_set_current(cur);
		*res = __vdbe_record_compare((int)cur->key.size,
		    cur->key.data, un_key);
		return (DBSQL_OK);
	}
	if (ret != DB_NOTFOUND) {
		cur->state = CURSOR_FAULT;
		cur->fault = __sm_errmap(ret);
		return (cur->fault);
	}
	if ((rc = __sm_dbc_get(cur, DB_LAST, &found)) != DBSQL_OK)
		return (rc);
	*res = -1;
	return (DBSQL_OK);
}

/*
 * __sm_integer_key --
 *	PUBLIC: int64_t __sm_integer_key __P((sm_cursor_t *));
 */
int64_t
__sm_integer_key(sm_cursor_t *cur)
{
	return (cur->rowid);
}

/*
 * __sm_payload_size --
 *	Size of the row record (tables) or of the key record (indexes).
 *
 * PUBLIC: u_int32_t __sm_payload_size __P((sm_cursor_t *));
 */
u_int32_t
__sm_payload_size(sm_cursor_t *cur)
{
	if (cur->dbh == NULL)
		return (0);
	return (cur->dbh->is_intkey ? cur->data.size : cur->key.size);
}

/* PUBLIC: int64_t __sm_max_record_size __P((sm_cursor_t *)); */
int64_t
__sm_max_record_size(sm_cursor_t *cur)
{
	(void)cur;
	return ((int64_t)DBSQL_MAX_LENGTH * 2);
}

/*
 * __sm_payload_fetch --
 *	A pointer to the whole payload of the current entry.
 *
 * PUBLIC: const void *__sm_payload_fetch __P((sm_cursor_t *, u_int32_t *));
 */
const void *
__sm_payload_fetch(sm_cursor_t *cur, u_int32_t *amt)
{
	if (cur->dbh->is_intkey) {
		*amt = cur->data.size;
		return (cur->data.data);
	}
	*amt = cur->key.size;
	return (cur->key.data);
}

/*
 * __sm_payload --
 *	Copy amt bytes of the payload starting at offset.
 *
 * PUBLIC: int __sm_payload __P((sm_cursor_t *, u_int32_t, u_int32_t,
 * PUBLIC:     void *));
 */
int
__sm_payload(sm_cursor_t *cur, u_int32_t offset, u_int32_t amt, void *buf)
{
	const u_int8_t *p;
	u_int32_t n;

	p = __sm_payload_fetch(cur, &n);
	if ((u_int64_t)offset + amt > n)
		return (DBSQL_CORRUPT_BKPT);
	memcpy(buf, p + offset, amt);
	return (DBSQL_OK);
}

/*
 * __sm_payload_checked --
 *	As __sm_payload, restoring the cursor first (incremental blob I/O).
 *
 * PUBLIC: int __sm_payload_checked __P((sm_cursor_t *, u_int32_t,
 * PUBLIC:     u_int32_t, void *));
 */
int
__sm_payload_checked(sm_cursor_t *cur, u_int32_t offset, u_int32_t amt,
    void *buf)
{
	int rc, diff;

	if (cur->state == CURSOR_INVALID)
		return (DBSQL_ABORT);
	if (cur->state == CURSOR_REQUIRESEEK) {
		if ((rc = __sm_cursor_restore_position(cur, &diff)) != DBSQL_OK)
			return (rc);
		if (diff)
			return (DBSQL_ABORT);
	}
	if (cur->state == CURSOR_FAULT)
		return (cur->fault);
	return (__sm_payload(cur, offset, amt, buf));
}

/*
 * __sm_put_data --
 *	Incremental blob write: overwrite amt bytes of the current row's
 *	record in place (the size never changes).
 *
 * PUBLIC: int __sm_put_data __P((sm_cursor_t *, u_int32_t, u_int32_t,
 * PUBLIC:     void *));
 */
int
__sm_put_data(sm_cursor_t *cur, u_int32_t offset, u_int32_t amt, void *z)
{
	int rc, ret, diff;

	if (cur->state == CURSOR_REQUIRESEEK &&
	    (rc = __sm_cursor_restore_position(cur, &diff)) != DBSQL_OK)
		return (rc);
	if (cur->state != CURSOR_VALID)
		return (DBSQL_ABORT);
	if (!cur->wr_flag)
		return (DBSQL_READONLY);
	if ((u_int64_t)offset + amt > cur->data.size)
		return (DBSQL_CORRUPT_BKPT);
	if ((rc = __sm_dbc_open(cur)) != DBSQL_OK)
		return (rc);
	memcpy((u_int8_t *)cur->data.data + offset, z, amt);
	cur->data.flags = 0;
	ret = cur->dbc->put(cur->dbc, &cur->key, &cur->data, DB_CURRENT);
	cur->data.flags = DB_DBT_REALLOC;
	return (__sm_errmap(ret));
}

/* PUBLIC: void __sm_incrblob_cursor __P((sm_cursor_t *)); */
void
__sm_incrblob_cursor(sm_cursor_t *cur)
{
	(void)cur;
}

/*
 * __sm_insert --
 *	Insert or replace an entry.  Tables: key = rowid payload->n_key,
 *	data = payload->pData (+ n_zero zero bytes).  Indexes: key =
 *	payload->key (n_key bytes), no data.  The cursor is left on the new
 *	entry.
 *
 * PUBLIC: int __sm_insert __P((sm_cursor_t *, const sm_payload_t *, int,
 * PUBLIC:     int));
 */
int
__sm_insert(sm_cursor_t *cur, const sm_payload_t *x, int flags,
    int seek_result)
{
	DBT key, data;
	u_int8_t kbuf[SM_ROWID_KEYLEN], *zbuf;
	int rc, ret, found;

	(void)seek_result;
	if (cur->state == CURSOR_FAULT)
		return (cur->fault);
	if ((rc = __sm_dbc_open(cur)) != DBSQL_OK)
		return (rc);
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	zbuf = NULL;
	if (cur->dbh->is_intkey) {
		__sm_rowid_to_key(x->n_key, kbuf);
		key.data = kbuf;
		key.size = SM_ROWID_KEYLEN;
		if (x->n_zero > 0) {
			zbuf = malloc((size_t)x->n_data + (size_t)x->n_zero);
			if (zbuf == NULL)
				return (DBSQL_NOMEM);
			if (x->n_data > 0)
				memcpy(zbuf, x->data, (size_t)x->n_data);
			memset(zbuf + x->n_data, 0, (size_t)x->n_zero);
			data.data = zbuf;
			data.size = (u_int32_t)(x->n_data + x->n_zero);
		} else {
			data.data = (void *)x->data;
			data.size = (u_int32_t)x->n_data;
		}
	} else {
		key.data = (void *)x->key;
		key.size = (u_int32_t)x->n_key;
	}
	ret = cur->dbc->put(cur->dbc, &key, &data, DB_KEYLAST);
	free(zbuf);
	if (ret != 0)
		return (__sm_errmap(ret));
	if (flags & BTREE_SAVEPOSITION) {
		/* Re-read so the cursor holds the new entry. */
		return (__sm_dbc_get(cur, DB_CURRENT, &found));
	}
	return (__sm_dbc_get(cur, DB_CURRENT, &found));
}

/*
 * __sm_delete --
 *	Delete the entry the cursor is on.  With BTREE_SAVEPOSITION the next
 *	__sm_next/__sm_previous lands where it would have without the
 *	delete; libdb leaves a DBC on the deleted slot, so DB_NEXT/DB_PREV
 *	already do that and the cursor stays CURSOR_VALID.
 *
 * PUBLIC: int __sm_delete __P((sm_cursor_t *, u_int8_t));
 */
int
__sm_delete(sm_cursor_t *cur, u_int8_t flags)
{
	int rc, ret, diff;

	(void)flags;
	if (cur->state == CURSOR_REQUIRESEEK &&
	    (rc = __sm_cursor_restore_position(cur, &diff)) != DBSQL_OK)
		return (rc);
	if (cur->state != CURSOR_VALID)
		return (cur->state == CURSOR_FAULT ? cur->fault : DBSQL_OK);
	if ((rc = __sm_dbc_open(cur)) != DBSQL_OK)
		return (rc);
	if ((ret = cur->dbc->del(cur->dbc, 0)) != 0)
		return (__sm_errmap(ret));
	return (DBSQL_OK);
}

/*
 * __sm_transfer_row --
 *	Copy the current row of src into dest with rowid i_key (used by
 *	INSERT INTO ... SELECT transfer optimisation).
 *
 * PUBLIC: int __sm_transfer_row __P((sm_cursor_t *, sm_cursor_t *,
 * PUBLIC:     int64_t));
 */
int
__sm_transfer_row(sm_cursor_t *dest, sm_cursor_t *src, int64_t i_key)
{
	sm_payload_t x;
	u_int32_t n;

	memset(&x, 0, sizeof(x));
	if (dest->dbh->is_intkey) {
		x.n_key = i_key;
		x.data = __sm_payload_fetch(src, &n);
		x.n_data = (int)n;
	} else {
		x.key = __sm_payload_fetch(src, &n);
		x.n_key = n;
	}
	return (__sm_insert(dest, &x, 0, 0));
}

/*
 * __sm_count --
 *	Exact number of entries, by a cursor walk under the snapshot.
 *
 * PUBLIC: int __sm_count __P((DBSQL *, sm_cursor_t *, int64_t *));
 */
int
__sm_count(DBSQL *db, sm_cursor_t *cur, int64_t *n)
{
	DBC *dbc;
	DBT k, d;
	int ret;

	(void)db;
	*n = 0;
	if (cur->empty)
		return (DBSQL_OK);
	if (cur->dbh == NULL &&
	    __sm_dbh_get(cur->bt, cur->pgno, cur->key_info, 0, &cur->dbh) != 0)
		return (DBSQL_CORRUPT);
	if ((ret = cur->dbh->dbp->cursor(cur->dbh->dbp, __sm_cur_txn(cur->bt),
	    &dbc, 0)) != 0)
		return (__sm_errmap(ret));
	memset(&k, 0, sizeof(k));
	memset(&d, 0, sizeof(d));
	k.flags = d.flags = DB_DBT_PARTIAL | DB_DBT_USERMEM;
	while ((ret = dbc->get(dbc, &k, &d, DB_NEXT)) == 0)
		(*n)++;
	(void)dbc->close(dbc);
	return (ret == DB_NOTFOUND ? DBSQL_OK : __sm_errmap(ret));
}

/*
 * __sm_row_count_est --
 *	Estimated rows, for the query planner (log-estimate is enough).
 *
 * PUBLIC: int64_t __sm_row_count_est __P((sm_cursor_t *));
 */
int64_t
__sm_row_count_est(sm_cursor_t *cur)
{
	DB_BTREE_STAT *st;
	int64_t n;

	n = -1;
	if (cur->dbh != NULL && cur->dbh->dbp->stat(cur->dbh->dbp,
	    __sm_cur_txn(cur->bt), &st, DB_FAST_STAT) == 0) {
		n = st->bt_nkeys > 0 ? (int64_t)st->bt_nkeys : -1;
		free(st);
	}
	return (n);
}

/*
 * __sm_trip_all_cursors --
 *	Fault every (write, if write_only) cursor of this database with
 *	err; or, with err == DBSQL_OK, just save positions.
 *
 * PUBLIC: int __sm_trip_all_cursors __P((sm_t *, int, int));
 */
int
__sm_trip_all_cursors(sm_t *p, int err, int write_only)
{
	sm_cursor_t *cur;

	if (p == NULL)
		return (DBSQL_OK);
	for (cur = p->bt->cursors; cur != NULL; cur = cur->next) {
		if (err == DBSQL_OK || (write_only && !cur->wr_flag)) {
			if (cur->dbc != NULL) {
				(void)cur->dbc->close(cur->dbc);
				cur->dbc = NULL;
				if (cur->state == CURSOR_VALID)
					cur->state = CURSOR_REQUIRESEEK;
			}
			continue;
		}
		if (cur->dbc != NULL) {
			(void)cur->dbc->close(cur->dbc);
			cur->dbc = NULL;
		}
		cur->state = CURSOR_FAULT;
		cur->fault = err;
	}
	return (DBSQL_OK);
}

/* PUBLIC: void __sm_clear_cursor __P((sm_cursor_t *)); */
void
__sm_clear_cursor(sm_cursor_t *cur)
{
	cur->state = CURSOR_INVALID;
	cur->saved = 0;
}

/* PUBLIC: int __sm_cursor_is_valid_nn __P((sm_cursor_t *)); */
int
__sm_cursor_is_valid_nn(sm_cursor_t *cur)
{
	return (cur->state == CURSOR_VALID);
}

/* PUBLIC: int __sm_cursor_is_valid __P((sm_cursor_t *)); */
int
__sm_cursor_is_valid(sm_cursor_t *cur)
{
	return (cur != NULL && cur->state == CURSOR_VALID);
}

/* PUBLIC: void __sm_cursor_hint_flags __P((sm_cursor_t *, unsigned)); */
void
__sm_cursor_hint_flags(sm_cursor_t *cur, unsigned mask)
{
	cur->hints = (u_int8_t)mask;
}

/* PUBLIC: int __sm_cursor_has_hint __P((sm_cursor_t *, unsigned int)); */
int
__sm_cursor_has_hint(sm_cursor_t *cur, unsigned int mask)
{
	return ((cur->hints & mask) != 0);
}

/* PUBLIC: void __sm_cursor_pin __P((sm_cursor_t *)); */
void
__sm_cursor_pin(sm_cursor_t *cur)
{
	(void)cur;
}

/* PUBLIC: void __sm_cursor_unpin __P((sm_cursor_t *)); */
void
__sm_cursor_unpin(sm_cursor_t *cur)
{
	(void)cur;
}

/*
 * __sm_offset --
 *	sqlite_offset(): there is no file offset in a libdb file.
 *
 * PUBLIC: int64_t __sm_offset __P((sm_cursor_t *));
 */
int64_t
__sm_offset(sm_cursor_t *cur)
{
	(void)cur;
	return (0);
}

/*
 * __sm_integrity_check --
 *	PRAGMA integrity_check, logically: every entry of every listed
 *	table is a well-formed record, keys of rowid tables decode, and
 *	index keys are in strictly increasing order under the comparator.
 *	Entry counts go to cnts[] (compared by upstream against indexes).
 *
 * PUBLIC: int __sm_integrity_check __P((DBSQL *, sm_t *, pgno_t *,
 * PUBLIC:     dbsql_value_t *, int, int, int *, char **));
 */
int
__sm_integrity_check(DBSQL *db, sm_t *p, pgno_t *roots, dbsql_value_t *cnts,
    int n_root, int mx_err, int *pn_err, char **out)
{
	sm_cursor_t cur;
	dbsql_str_t acc;
	int i, rc, res, nerr;
	int64_t n;
	u_int32_t sz;
	const void *rec;

	nerr = 0;
	__str_accum_init(&acc, 0, 0, 0, DBSQL_MAX_LENGTH);
	for (i = 0; i < n_root && nerr < mx_err; i++) {
		if (roots[i] == 0)
			continue;
		if ((rc = __sm_cursor(p, roots[i], 0, NULL, &cur)) != DBSQL_OK) {
			dbsql_str_appendf(&acc, "%stable %u: cannot open",
			    nerr++ ? "\n" : "", (unsigned)roots[i]);
			continue;
		}
		n = 0;
		for (rc = __sm_first(&cur, &res); rc == DBSQL_OK && !res;
		    rc = __sm_next(&cur, 0)) {
			n++;
			rec = __sm_payload_fetch(&cur, &sz);
			if (sz > 0 && !__sm_rec_valid(rec, sz, NULL) &&
			    nerr < mx_err)
				dbsql_str_appendf(&acc,
				    "%sTree %u entry %lld: malformed record",
				    nerr++ ? "\n" : "", (unsigned)roots[i],
				    (long long)n);
		}
		if (rc != DBSQL_OK && rc != DBSQL_DONE && nerr < mx_err)
			dbsql_str_appendf(&acc, "%sTree %u: %s",
			    nerr++ ? "\n" : "", (unsigned)roots[i],
			    dbsql_errstr(rc));
		__vdbe_mem_set_int64(&cnts[i], n);
		(void)__sm_close_cursor(&cur);
	}
	*pn_err = nerr;
	*out = nerr ? __str_accum_finish(&acc) : NULL;
	if (!nerr)
		dbsql_str_reset(&acc);
	(void)db;
	return (DBSQL_OK);
}
