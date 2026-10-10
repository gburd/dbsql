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

static void __sm_invalidate_incrblob(sm_cursor_t *, int64_t, int);
static void __sm_cursors_save_others_keep_incrblob(sm_cursor_t *);

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
	cur->generation++;
	cur->state = CURSOR_VALID;
	cur->skip_next = 0;
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
		if (cur->state == CURSOR_VALID ||
		    cur->state == CURSOR_SKIPNEXT) {
			cur->state = CURSOR_REQUIRESEEK;
			cur->saved = 1;
		}
	}
}

/*
 * __sm_cursors_invalidate_table --
 *	A table is being cleared or written through another cursor: its
 *	positioned cursors are saved (CURSOR_REQUIRESEEK, key kept), as
 *	upstream's saveAllCursors does, so the VDBE re-seeks them; after a
 *	clear the re-seek finds nothing and they become invalid.
 *
 * PUBLIC: void __sm_cursors_invalidate_table __P((sm_shared_t *,
 * PUBLIC:     u_int32_t));
 */
void
__sm_cursors_invalidate_table(sm_shared_t *bt, u_int32_t pgno)
{
	sm_cursor_t *cur;

	for (cur = bt->cursors; cur != NULL; cur = cur->next) {
		if (cur->pgno != pgno || cur->state == CURSOR_FAULT)
			continue;
		/* DB->truncate refuses while any DBC is open on the table. */
		if (cur->dbc != NULL) {
			(void)cur->dbc->close(cur->dbc);
			cur->dbc = NULL;
			cur->dbc_txn = NULL;
		}
		if (cur->incrblob)
			cur->state = CURSOR_INVALID;	/* blob handle: gone */
		else if (cur->state == CURSOR_VALID ||
		    cur->state == CURSOR_SKIPNEXT) {
			cur->state = CURSOR_REQUIRESEEK;
			cur->saved = 1;
		}
	}
}

/*
 * __sm_cursors_save_others --
 *	A row is about to be deleted through 'cur': every other positioned
 *	cursor on the table is saved, so it reports "moved" and re-seeks
 *	(upstream's saveAllCursors).
 */
static int
__sm_cursors_check_pinned(sm_cursor_t *cur)
{
	sm_cursor_t *c;

	/*
	 * A write that would move a pinned cursor (OP_CursorLock: a REPLACE
	 * conflict resolution in progress) fails, as upstream's
	 * saveCursorPosition does.
	 */
	for (c = cur->bt->cursors; c != NULL; c = c->next)
		if (c != cur && c->pgno == cur->pgno && c->pinned &&
		    (c->state == CURSOR_VALID ||
		    c->state == CURSOR_REQUIRESEEK))
			return (DBSQL_CONSTRAINT_PINNED);
	return (DBSQL_OK);
}

/*
 * __sm_cursors_save_others_keep_incrblob --
 *	As __sm_cursors_save_others, for a blob write: no pin check, and
 *	every cursor is just released (blob handles re-read on use).
 */
static void
__sm_cursors_save_others_keep_incrblob(sm_cursor_t *cur)
{
	sm_cursor_t *c;

	for (c = cur->bt->cursors; c != NULL; c = c->next) {
		if (c == cur || c->pgno != cur->pgno || c->dbc == NULL)
			continue;
		(void)c->dbc->close(c->dbc);
		c->dbc = NULL;
		c->dbc_txn = NULL;
		if (c->state == CURSOR_VALID) {
			c->state = CURSOR_REQUIRESEEK;
			c->saved = 1;
		}
	}
}

static int
__sm_cursors_save_others(sm_cursor_t *cur)
{
	sm_cursor_t *c;
	int rc;

	if ((rc = __sm_cursors_check_pinned(cur)) != DBSQL_OK)
		return (rc);
	/*
	 * A write through 'cur' may change or remove the entries other
	 * cursors on the table are on: save their positions (upstream's
	 * saveAllCursors); they re-seek on next use.
	 */
	for (c = cur->bt->cursors; c != NULL; c = c->next) {
		if (c == cur || c->pgno != cur->pgno || c->dbc == NULL ||
		    c->state != CURSOR_VALID)
			continue;
		(void)c->dbc->close(c->dbc);
		c->dbc = NULL;
		c->dbc_txn = NULL;
		c->state = CURSOR_REQUIRESEEK;
		c->saved = 1;
	}
	return (DBSQL_OK);
}

/*
 * __sm_cursor --
 *	Open a cursor on table i_table.
 *
 * PUBLIC: int __sm_cursor __P((sm_t *, pgno_t, int, struct key_info_t *,
 * PUBLIC:     sm_cursor_t *));
 */
static int __sm_cursor_open(sm_t *, pgno_t, int, key_info_t *,
    sm_cursor_t *, int);

int
__sm_cursor(sm_t *p, pgno_t i_table, int wr_flag, key_info_t *ki,
    sm_cursor_t *cur)
{
	return (__sm_cursor_open(p, i_table, wr_flag, ki, cur, 1));
}

/*
 * __sm_cursor_open --
 *	Open a cursor; with check_kind, a key_info/table-kind mismatch is
 *	corruption (integrity_check opens every table without key_info).
 */
static int
__sm_cursor_open(sm_t *p, pgno_t i_table, int wr_flag, key_info_t *ki,
    sm_cursor_t *cur, int check_kind)
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
	/*
	 * An index cursor (key_info) on a rowid table or the reverse: the
	 * schema's rootpage is wrong (upstream sees the wrong page type).
	 */
	if (check_kind && cur->dbh != NULL &&
	    (ki == NULL) != (cur->dbh->is_intkey != 0)) {
		cur->bt = NULL;
		return (DBSQL_CORRUPT_BKPT);
	}
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
	sm_t *p;

	if (cur->bt == NULL)
		return (DBSQL_OK);
	if (cur->dbc != NULL)
		(void)cur->dbc->close(cur->dbc);
	for (pp = &cur->bt->cursors; *pp != NULL; pp = &(*pp)->next)
		if (*pp == cur) {
			*pp = cur->next;
			break;
		}
	/* An ephemeral btree goes away with its last cursor (upstream). */
	p = (cur->bt->flags & SM_EPHEMERAL) && cur->bt->cursors == NULL ?
	    cur->btree : NULL;
	free(cur->key.data);
	free(cur->data.data);
	free(cur->preformat);
	memset(cur, 0, sizeof(*cur));
	if (p != NULL)
		(void)__sm_close(p);
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
	u_int8_t *saved;
	u_int32_t n;
	int rc, found, skip;

	*different = 0;
	if (cur->state == CURSOR_FAULT)
		return (cur->fault);
	if (cur->state != CURSOR_REQUIRESEEK)
		return (DBSQL_OK);
	if ((rc = __sm_dbc_open(cur)) != DBSQL_OK)
		return (rc);
	/*
	 * Re-seek to the saved key (still in cur->key).  As upstream: on
	 * the same entry the cursor is valid; on the next larger one it is
	 * CURSOR_SKIPNEXT with skip_next 1 (the next __sm_next stays put);
	 * past the end it is on the last entry with skip_next -1.
	 */
	n = cur->key.size;
	if ((saved = malloc(n == 0 ? 1 : n)) == NULL)
		return (DBSQL_NOMEM);
	memcpy(saved, cur->key.data, n);
	skip = cur->skip_next;
	rc = __sm_dbc_get(cur, DB_SET_RANGE, &found);
	if (rc == DBSQL_OK && found) {
		if (cur->key.size != n || memcmp(cur->key.data, saved, n) != 0) {
			cur->state = CURSOR_SKIPNEXT;
			cur->skip_next = 1;
			*different = 1;
		} else if (skip != 0) {
			/* A txn boundary closed a SKIPNEXT cursor's DBC. */
			cur->state = CURSOR_SKIPNEXT;
			cur->skip_next = skip;
		}
	} else if (rc == DBSQL_OK) {
		*different = 1;
		if ((rc = __sm_dbc_get(cur, DB_LAST, &found)) == DBSQL_OK &&
		    found) {
			cur->state = CURSOR_SKIPNEXT;
			cur->skip_next = -1;
		}
	}
	free(saved);
	return (rc);
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

	if (cur->state == CURSOR_INVALID)
		return (DBSQL_DONE);
	if ((rc = __sm_ensure(cur)) != DBSQL_OK)
		return (rc);
	if (cur->state == CURSOR_SKIPNEXT) {
		/* Already on the entry to return next (see restore). */
		cur->state = CURSOR_VALID;
		if (cur->skip_next == (op == DB_NEXT ? 1 : -1)) {
			cur->skip_next = 0;
			return (DBSQL_OK);
		}
		cur->skip_next = 0;
	}
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
static int __sm_index_eq_before(sm_cursor_t *, unpacked_record_t *, int);

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
	/*
	 * The record compare sets un_key->eq_seen whenever an entry equals
	 * the probe on all its fields, and libdb's descent compared many
	 * entries.  Upstream's meaning (BTREE_SEEK_EQ seeks jump to "not
	 * found" without it) is "an equal entry exists", and DB_SET_RANGE
	 * lands on the first entry >= the probe, so the landing entry
	 * alone decides it.
	 */
	un_key->eq_seen = 0;
	if (ret != 0 && ret != DB_NOTFOUND) {
		cur->state = CURSOR_FAULT;
		cur->fault = __sm_errmap(ret);
		return (cur->fault);
	}
	if (ret == 0) {
		__sm_cursor_set_current(cur);
		*res = __vdbe_record_compare((int)cur->key.size,
		    cur->key.data, un_key);
	} else {
		if ((rc = __sm_dbc_get(cur, DB_LAST, &found)) != DBSQL_OK)
			return (rc);
		*res = -1;
		if (!found)
			return (DBSQL_OK);
	}
	/*
	 * An entry equal on the probe's fields compares as default_rc
	 * (never 0 for a seek with fewer fields than the index), so it is
	 * either the landing entry (compared above) or the one just before
	 * it.  Compare that one too, for eq_seen only.
	 */
	if (!un_key->eq_seen &&
	    (rc = __sm_index_eq_before(cur, un_key, ret == 0)) != DBSQL_OK)
		return (rc);
	return (DBSQL_OK);
}

/*
 * __sm_index_eq_before --
 *	Set un_key->eq_seen if the entry before the cursor's landing entry
 *	(or the landing entry itself when the seek ran off the end) equals
 *	the probe on its fields.  The cursor's position is not changed.
 */
static int
__sm_index_eq_before(sm_cursor_t *cur, unpacked_record_t *un_key, int landed)
{
	DBC *dbc;
	DBT k, d;
	int ret;

	if (!landed) {
		(void)__vdbe_record_compare((int)cur->key.size, cur->key.data,
		    un_key);
		return (DBSQL_OK);
	}
	if ((ret = cur->dbc->dup(cur->dbc, &dbc, DB_POSITION)) != 0)
		return (__sm_errmap(ret));
	memset(&k, 0, sizeof(k));
	memset(&d, 0, sizeof(d));
	k.flags = d.flags = DB_DBT_REALLOC;
	if ((ret = dbc->get(dbc, &k, &d, DB_PREV)) == 0)
		(void)__vdbe_record_compare((int)k.size, k.data, un_key);
	(void)dbc->close(dbc);
	free(k.data);
	free(d.data);
	return (ret == 0 || ret == DB_NOTFOUND ? DBSQL_OK : __sm_errmap(ret));
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
 *	A pointer to the start of the current entry's payload.  Upstream
 *	returns only the part on the b-tree page, never more than 64 KiB,
 *	and the VDBE reads anything beyond *amt through __sm_payload; the
 *	same limit is kept here.
 *
 * PUBLIC: const void *__sm_payload_fetch __P((sm_cursor_t *, u_int32_t *));
 */
static const void *
__sm_payload_all(sm_cursor_t *cur, u_int32_t *amt)
{
	const DBT *d;

	d = cur->dbh->is_intkey ? &cur->data : &cur->key;
	*amt = d->size;
	return (d->data);
}

const void *
__sm_payload_fetch(sm_cursor_t *cur, u_int32_t *amt)
{
	const DBT *d;

	d = cur->dbh->is_intkey ? &cur->data : &cur->key;
	*amt = d->size > SM_PAYLOAD_LOCAL_MAX ? SM_PAYLOAD_LOCAL_MAX : d->size;
	return (d->data);
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

	p = __sm_payload_all(cur, &n);
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
	int rc, diff, found;

	if (cur->state == CURSOR_INVALID)
		return (DBSQL_ABORT);
	if (cur->state == CURSOR_REQUIRESEEK) {
		if ((rc = __sm_cursor_restore_position(cur, &diff)) != DBSQL_OK)
			return (rc);
		if (diff || cur->state != CURSOR_VALID)
			return (DBSQL_ABORT);
	}
	if (cur->state == CURSOR_FAULT)
		return (cur->fault);
	/*
	 * Blob reads re-read the row: another blob handle on the same row
	 * may have written it since this cursor copied it.
	 */
	if (cur->incrblob) {
		if ((rc = __sm_dbc_open(cur)) != DBSQL_OK)
			return (rc);
		if ((rc = __sm_dbc_get(cur, DB_CURRENT, &found)) != DBSQL_OK)
			return (rc);
		if (!found)
			return (DBSQL_ABORT);
	}
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
	int rc, ret, diff, found;

	if (cur->state == CURSOR_REQUIRESEEK) {
		if ((rc = __sm_cursor_restore_position(cur, &diff)) != DBSQL_OK)
			return (rc);
		if (diff)
			return (DBSQL_ABORT);
	}
	if (cur->state != CURSOR_VALID)
		return (DBSQL_ABORT);
	if (!cur->wr_flag)
		return (DBSQL_READONLY);
	if ((rc = __sm_dbc_open(cur)) != DBSQL_OK)
		return (rc);
	/* Start from the row as stored (another handle may have written). */
	if ((rc = __sm_dbc_get(cur, DB_CURRENT, &found)) != DBSQL_OK)
		return (rc);
	if (!found)
		return (DBSQL_ABORT);
	if ((u_int64_t)offset + amt > cur->data.size)
		return (DBSQL_CORRUPT_BKPT);
	/* Other blob handles on this row stay valid (upstream). */
	__sm_cursors_save_others_keep_incrblob(cur);
	memcpy((u_int8_t *)cur->data.data + offset, z, amt);
	cur->data.flags = 0;
	ret = cur->dbc->put(cur->dbc, &cur->key, &cur->data, DB_CURRENT);
	cur->data.flags = DB_DBT_REALLOC;
	return (__sm_errmap(ret));
}

/*
 * __sm_incrblob_cursor --
 *	Mark a cursor as a blob handle's: a write to its row through any
 *	other cursor (or clearing the table) invalidates it, and blob I/O on
 *	it then fails with DBSQL_ABORT.
 *
 * PUBLIC: void __sm_incrblob_cursor __P((sm_cursor_t *));
 */
void
__sm_incrblob_cursor(sm_cursor_t *cur)
{
	cur->incrblob = 1;
}

/*
 * __sm_invalidate_incrblob --
 *	A write is about to change row 'rowid' of table pgno (or every row,
 *	with all set): invalidate the blob handles on it.
 */
static void
__sm_invalidate_incrblob(sm_cursor_t *writer, int64_t rowid, int all)
{
	sm_cursor_t *c;

	for (c = writer->bt->cursors; c != NULL; c = c->next)
		if (c != writer && c->incrblob && c->pgno == writer->pgno &&
		    (all || c->rowid == rowid))
			c->state = CURSOR_INVALID;
}

/*
 * __sm_after_delete --
 *	The entry under the cursor was just deleted: keep its key as a
 *	saved position, so the next __sm_next/__sm_previous lands where it
 *	would have without the delete (upstream's BTREE_SAVEPOSITION), via
 *	__sm_cursor_restore_position.
 */
static void
__sm_after_delete(sm_cursor_t *cur)
{
	(void)cur->dbc->close(cur->dbc);
	cur->dbc = NULL;
	cur->dbc_txn = NULL;
	cur->state = CURSOR_REQUIRESEEK;
	cur->saved = 1;
}

/*
 * __sm_index_replace --
 *	Delete the entry whose first x->n_mem fields equal x->mems, if any.
 */
static int
__sm_index_replace(sm_cursor_t *cur, const sm_payload_t *x)
{
	unpacked_record_t r;
	int rc, res, ret;

	unpacked_record_t *rp;

	if (x->n_mem > 0) {
		memset(&r, 0, sizeof(r));
		r.key_info = cur->key_info;
		r.mems = x->mems;
		r.n_field = x->n_mem;
		rp = &r;
	} else {
		/* The whole record identifies the entry. */
		if ((rp = __vdbe_alloc_unpacked_record(cur->key_info)) == NULL)
			return (DBSQL_NOMEM);
		__vdbe_record_unpack((int)x->n_key, x->key, rp);
		rp->default_rc = 0;
	}
	rc = __sm_index_moveto(cur, rp, &res);
	if (rp != &r)
		__dbsql_db_free(cur->key_info->db, rp);
	if (rc != DBSQL_OK)
		return (rc);
	if (res != 0 || cur->state != CURSOR_VALID)
		return (DBSQL_OK);
	if ((rc = __sm_cursors_save_others(cur)) != DBSQL_OK)
		return (rc);
	if ((ret = cur->dbc->del(cur->dbc, 0)) != 0)
		return (__sm_errmap(ret));
	__sm_after_delete(cur);
	return (DBSQL_OK);
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
	if (flags & BTREE_PREFORMAT) {
		/* The payload was prepared by __sm_transfer_row. */
		if (cur->dbh->is_intkey) {
			__sm_rowid_to_key(x->n_key, kbuf);
			key.data = kbuf;
			key.size = SM_ROWID_KEYLEN;
			data.data = cur->preformat;
			data.size = cur->preformat_size;
		} else {
			key.data = cur->preformat;
			key.size = cur->preformat_size;
		}
	} else if (cur->dbh->is_intkey) {
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
		/*
		 * An existing entry that compares equal to the new one is
		 * replaced, as upstream overwrites the cell.  With x->n_mem
		 * only the first n_mem fields identify the entry (a WITHOUT
		 * ROWID primary key); libdb, comparing whole records, would
		 * keep both.  (libdb then stores the old item's key bytes when
		 * the deleted item is still referenced: a libdb bug, reported
		 * as .agent/libdb-bugs R1; NOCASE/WITHOUT ROWID key updates
		 * keep the old spelling until it is fixed.)
		 */
		if ((rc = __sm_index_replace(cur, x)) != DBSQL_OK ||
		    (rc = __sm_dbc_open(cur)) != DBSQL_OK)
			return (rc);
	}
	/*
	 * Other cursors on the table hold copies of the rows they are on;
	 * a write may replace one of them (a trigger updating the row its
	 * statement is on), so they are saved and re-read, as upstream's
	 * saveAllCursors.  Index inserts did this above.
	 */
	if (cur->dbh->is_intkey) {
		__sm_invalidate_incrblob(cur, x->n_key, 0);
		if ((rc = __sm_cursors_save_others(cur)) != DBSQL_OK) {
			free(zbuf);
			return (rc);
		}
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
	if (cur->dbh->is_intkey)
		__sm_invalidate_incrblob(cur, cur->rowid, 0);
	if ((rc = __sm_cursors_save_others(cur)) != DBSQL_OK)
		return (rc);
	if ((ret = cur->dbc->del(cur->dbc, 0)) != 0)
		return (__sm_errmap(ret));
	__sm_after_delete(cur);
	return (DBSQL_OK);
}

/*
 * __sm_transfer_row --
 *	OP_RowCell: prepare the current row of src for the BTREE_PREFORMAT
 *	insert that follows on dest (upstream builds the cell here and the
 *	insert writes it).  The payload is copied into dest's preformat
 *	buffer; for rowid tables the insert supplies the key (i_key, also
 *	the OP_Insert's rowid).
 *
 * PUBLIC: int __sm_transfer_row __P((sm_cursor_t *, sm_cursor_t *,
 * PUBLIC:     int64_t));
 */
int
__sm_transfer_row(sm_cursor_t *dest, sm_cursor_t *src, int64_t i_key)
{
	const void *p;
	void *buf;
	u_int32_t n;

	(void)i_key;
	p = __sm_payload_all(src, &n);
	if (n > dest->preformat_alloc) {
		if ((buf = realloc(dest->preformat, n)) == NULL)
			return (DBSQL_NOMEM);
		dest->preformat = buf;
		dest->preformat_alloc = n;
	}
	if (n > 0)
		memcpy(dest->preformat, p, n);
	dest->preformat_size = n;
	return (DBSQL_OK);
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
 *	Estimated rows (OP_IfSizeBetween, OP_Count with P3): as upstream,
 *	0 unless the cursor is on an entry, else a positive estimate that
 *	is only used as a log-estimate.  libdb's DB_FAST_STAT does not
 *	maintain a key count, so the estimate counts entries from the
 *	first one, stopping at SM_ROW_EST_MAX.
 *
 * PUBLIC: int64_t __sm_row_count_est __P((sm_cursor_t *));
 */
int64_t
__sm_row_count_est(sm_cursor_t *cur)
{
	DBC *dbc;
	DBT k, d;
	int64_t n;

	/* ponytail: capped walk; a libdb key count would make this O(1). */
	if (cur->state != CURSOR_VALID || cur->dbh == NULL)
		return (0);
	if (cur->dbh->dbp->cursor(cur->dbh->dbp, __sm_cur_txn(cur->bt),
	    &dbc, 0) != 0)
		return (1);
	memset(&k, 0, sizeof(k));
	memset(&d, 0, sizeof(d));
	k.flags = d.flags = DB_DBT_PARTIAL | DB_DBT_USERMEM;
	for (n = 0; n < SM_ROW_EST_MAX &&
	    dbc->get(dbc, &k, &d, DB_NEXT) == 0; n++)
		;
	(void)dbc->close(dbc);
	return (n > 0 ? n : 1);
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
	cur->pinned = 1;
}

/* PUBLIC: void __sm_cursor_unpin __P((sm_cursor_t *)); */
void
__sm_cursor_unpin(sm_cursor_t *cur)
{
	cur->pinned = 0;
}

/*
 * __sm_offset --
 *	Upstream returns the file offset of the cell; there is none in a
 *	libdb file.  The VDBE also uses the value to tell rows apart (its
 *	large-value column cache is keyed on it), so it is a counter that
 *	changes whenever the cursor's current entry changes.  sqlite_offset()
 *	is not meaningful on dbsql (DIVERGENCES.md).
 *
 * PUBLIC: int64_t __sm_offset __P((sm_cursor_t *));
 */
int64_t
__sm_offset(sm_cursor_t *cur)
{
	return ((int64_t)cur->generation);
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
		/* As upstream: every slot gets a count, 0 for root 0 (a
		 * virtual table's declared index has no b-tree). */
		__vdbe_mem_set_int64(&cnts[i], 0);
		if (roots[i] == 0)
			continue;
		if ((rc = __sm_cursor_open(p, roots[i], 0, NULL, &cur, 0)) !=
		    DBSQL_OK) {
			/*
			 * Out of memory is an error, not a finding: as
			 * upstream's __check_oom, return NOMEM with at least
			 * one error counted (the VDBE requires it).
			 */
			if ((rc & 0xff) == DBSQL_NOMEM) {
				dbsql_str_reset(&acc);
				*pn_err = nerr > 0 ? nerr : 1;
				*out = NULL;
				return (DBSQL_NOMEM);
			}
			dbsql_str_appendf(&acc, "%stable %u: cannot open",
			    nerr++ ? "\n" : "", (unsigned)roots[i]);
			continue;
		}
		n = 0;
		for (rc = __sm_first(&cur, &res); rc == DBSQL_OK && !res;
		    rc = __sm_next(&cur, 0)) {
			n++;
			rec = __sm_payload_all(&cur, &sz);
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
