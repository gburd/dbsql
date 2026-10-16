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
 * sm_cmp.c --
 *	The libdb B-tree comparison function for SQL indexes and WITHOUT
 *	ROWID tables, and the key encoding for rowid tables.
 *
 *	libdb calls the comparator from its lock-free (optimistic) descent
 *	on page bytes that have not been validated yet, so it must be a pure
 *	function of its two DBTs that never reads outside them (NORTH_STAR
 *	invariant 6).  Ordering must be exactly SQLite's record order, which
 *	the VDBE also relies on (OP_IdxGT and friends compare the payload a
 *	cursor returns), so the comparison itself is delegated to the
 *	transformed upstream record compare once the page-side record has
 *	been checked to be well formed within its bounds.
 */

#include "dbsql_int.h"
#include "vdbe_int.h"
#include "sm_int.h"

/*
 * __sm_varint_bounded --
 *	Decode a SQLite varint from [p, end).  Returns the number of bytes
 *	consumed (1..9) or 0 if the varint runs past 'end'.  Never reads at
 *	or beyond 'end'.
 */
static int
__sm_varint_bounded(const u_int8_t *p, const u_int8_t *end, u_int64_t *v)
{
	u_int64_t x;
	int i;

	x = 0;
	for (i = 0; i < 8; i++) {
		if (p + i >= end)
			return (0);
		x = (x << 7) | (p[i] & 0x7f);
		if ((p[i] & 0x80) == 0) {
			*v = x;
			return (i + 1);
		}
	}
	if (p + 8 >= end)
		return (0);
	*v = (x << 8) | p[8];
	return (9);
}

/*
 * __sm_serial_len --
 *	Byte length of the data of a record field with the given serial type.
 */
static u_int64_t
__sm_serial_len(u_int64_t t)
{
	static const u_int8_t fixed[] = { 0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0 };

	if (t >= 12)
		return ((t - 12) / 2);
	return (fixed[t]);
}

/*
 * __sm_rec_valid --
 *	Check that 'n' bytes at 'p' hold a well-formed SQLite record: a
 *	header-size varint, serial types that lie within the header, and
 *	field data whose total length does not exceed the record.  Serial
 *	types 10 and 11 are reserved and invalid.  Optionally return the
 *	number of fields.  Reads only within [p, p + n).
 *
 * PUBLIC: int __sm_rec_valid __P((const void *, u_int32_t, u_int32_t *));
 */
int
__sm_rec_valid(const void *data, u_int32_t n, u_int32_t *nfieldsp)
{
	const u_int8_t *p, *end, *hdr_end;
	u_int64_t hdr_size, t, body;
	u_int32_t nfields;
	int len;

	p = data;
	end = p + n;
	if (n == 0)
		return (0);
	if ((len = __sm_varint_bounded(p, end, &hdr_size)) == 0)
		return (0);
	if (hdr_size < (u_int64_t)len || hdr_size > n)
		return (0);
	hdr_end = p + hdr_size;
	p += len;
	body = 0;
	nfields = 0;
	while (p < hdr_end) {
		if ((len = __sm_varint_bounded(p, hdr_end, &t)) == 0)
			return (0);
		if (t == 10 || t == 11)
			return (0);
		body += __sm_serial_len(t);
		if (body > n - hdr_size)
			return (0);
		p += len;
		nfields++;
	}
	if (nfieldsp != NULL)
		*nfieldsp = nfields;
	return (1);
}

/*
 * __sm_rec_cmp_packed --
 *	Compare two well-formed packed records field by field with the
 *	index's collations and sort flags, exactly as upstream's record
 *	compare orders them.  Both records have already passed
 *	__sm_rec_valid.  Fields are compared up to the shorter record; a
 *	record that is a prefix of the other sorts first.
 */
static int
__sm_rec_cmp_packed(key_info_t *ki,
    const u_int8_t *a, u_int32_t na, const u_int8_t *b, u_int32_t nb)
{
	mem_t ma, mb;
	u_int64_t ha, hb, ta, tb;
	u_int32_t ia, ib, da, db_, i, nk;
	int la, lb, rc;
	u_int8_t flags;

	la = __sm_varint_bounded(a, a + na, &ha);
	lb = __sm_varint_bounded(b, b + nb, &hb);
	ia = (u_int32_t)la;
	ib = (u_int32_t)lb;
	da = (u_int32_t)ha;
	db_ = (u_int32_t)hb;
	nk = ki != NULL ? ki->n_all_field : 0;
	for (i = 0; ia < ha && ib < hb; i++) {
		ia += (u_int32_t)__sm_varint_bounded(a + ia, a + ha, &ta);
		ib += (u_int32_t)__sm_varint_bounded(b + ib, b + hb, &tb);
		memset(&ma, 0, sizeof(ma));
		memset(&mb, 0, sizeof(mb));
		ma.enc = mb.enc = ki != NULL ? ki->enc : DBSQL_UTF8;
		ma.db = mb.db = ki != NULL ? ki->db : NULL;
		__vdbe_serial_get(a + da, (u_int32_t)ta, &ma);
		__vdbe_serial_get(b + db_, (u_int32_t)tb, &mb);
		da += (u_int32_t)__sm_serial_len(ta);
		db_ += (u_int32_t)__sm_serial_len(tb);
		rc = __mem_compare(&ma, &mb,
		    (ki != NULL && i < nk) ? ki->colls[i] : NULL);
		if (rc != 0) {
			flags = (ki != NULL && i < nk) ? ki->sort_flags[i] : 0;
			if ((flags & KEYINFO_ORDER_BIGNULL) &&
			    ((ma.flags & MEM_Null) || (mb.flags & MEM_Null)))
				rc = -rc;
			if (flags & KEYINFO_ORDER_DESC)
				rc = -rc;
			return (rc);
		}
	}
	if (ia < ha)
		return (1);
	if (ib < hb)
		return (-1);
	return (0);
}

/*
 * __sm_rec_cmp --
 *	The libdb bt_compare callback for SQL indexes.  The handle's
 *	app_private is the sm_dbh_t that owns the (immutable) key_info_t.
 *	When either DBT carries an unpacked probe in app_data (every VDBE
 *	seek and insert sets one), the comparison is upstream's record
 *	compare against that probe; otherwise the two packed records are
 *	compared field by field.  A malformed record sorts after every
 *	well-formed one and reads nothing outside its DBT.
 *
 * PUBLIC: int __sm_rec_cmp __P((DB *, const DBT *, const DBT *));
 */
int
__sm_rec_cmp(DB *dbp, const DBT *a, const DBT *b)
{
	sm_dbh_t *dbh;
	unpacked_record_t *probe;
	int va, vb, rc;

	dbh = dbp->app_private;
	if ((probe = a->app_data) != NULL) {
		if (!__sm_rec_valid(b->data, b->size, NULL))
			return (-1);
		/* a is the probe: compare(b, probe) has the opposite sign. */
		rc = __vdbe_record_compare((int)b->size, b->data, probe);
		return (-rc);
	}
	if ((probe = b->app_data) != NULL) {
		if (!__sm_rec_valid(a->data, a->size, NULL))
			return (1);
		return (__vdbe_record_compare((int)a->size, a->data, probe));
	}
	va = __sm_rec_valid(a->data, a->size, NULL);
	vb = __sm_rec_valid(b->data, b->size, NULL);
	if (!va || !vb)
		return (va - vb == 0 ? 0 : (va ? -1 : 1));
	return (__sm_rec_cmp_packed(dbh != NULL ? dbh->key_info : NULL,
	    a->data, a->size, b->data, b->size));
}

/*
 * __sm_rowid_to_key --
 *	Encode a rowid as an 8-byte big-endian key with the sign bit
 *	flipped, so that memcmp() order is integer order.
 *
 * PUBLIC: void __sm_rowid_to_key __P((int64_t, u_int8_t *));
 */
void
__sm_rowid_to_key(int64_t rowid, u_int8_t *buf)
{
	u_int64_t v;
	int i;

	v = (u_int64_t)rowid ^ ((u_int64_t)1 << 63);
	for (i = 7; i >= 0; i--) {
		buf[i] = (u_int8_t)(v & 0xff);
		v >>= 8;
	}
}

/*
 * __sm_key_to_rowid --
 *	Decode a key produced by __sm_rowid_to_key.
 *
 * PUBLIC: int64_t __sm_key_to_rowid __P((const u_int8_t *));
 */
int64_t
__sm_key_to_rowid(const u_int8_t *buf)
{
	u_int64_t v;
	int i;

	v = 0;
	for (i = 0; i < 8; i++)
		v = (v << 8) | buf[i];
	return ((int64_t)(v ^ ((u_int64_t)1 << 63)));
}
