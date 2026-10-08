/*
 * test/unit/sm_cmp_test.c --
 *	Unit tests for the storage manager's key encoding and comparator
 *	(src/sm/sm_cmp.c).  Built against the dbsql library; run under
 *	ASan+UBSan.  Exit status is non-zero on any failure.
 *
 *	1. rowid key encoding: memcmp order == integer order, round trip.
 *	2. comparator vs upstream: for random valid records, __sm_rec_cmp
 *	   (packed vs packed, and probe vs packed) agrees in sign with
 *	   upstream's __vdbe_record_compare.
 *	3. comparator on garbage: random and truncated byte strings never
 *	   read out of bounds (ASan), and the result is antisymmetric.
 */
#include "dbsql_int.h"
#include "vdbe_int.h"
#include "sm_int.h"

static int failures;
#define	CHECK(c, ...) do {						\
	if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__,	\
	    __LINE__); printf(__VA_ARGS__); printf("\n"); }		\
} while (0)

static u_int64_t rng = 0x9e3779b97f4a7c15ULL;

static u_int64_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (rng);
}

static int
sgn(int x)
{
	return ((x > 0) - (x < 0));
}

static void
test_rowid_keys(void)
{
	static const int64_t v[] = { INT64_MIN, INT64_MIN + 1, -1000000, -2,
	    -1, 0, 1, 2, 255, 256, 1000000, INT64_MAX - 1, INT64_MAX };
	u_int8_t a[8], b[8];
	size_t i, j;

	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		__sm_rowid_to_key(v[i], a);
		CHECK(__sm_key_to_rowid(a) == v[i], "round trip %lld",
		    (long long)v[i]);
		for (j = 0; j < sizeof(v) / sizeof(v[0]); j++) {
			__sm_rowid_to_key(v[j], b);
			CHECK(sgn(memcmp(a, b, 8)) == sgn((v[i] > v[j]) -
			    (v[i] < v[j])), "order %lld %lld",
			    (long long)v[i], (long long)v[j]);
		}
	}
}

/*
 * make_record --
 *	Build a random record of 'nf' fields (NULL, integers of every width,
 *	reals, short text, short blobs) into buf; return its length.
 */
static int
make_record(u_int8_t *buf, int nf)
{
	u_int8_t hdr[64], body[512];
	int hn, bn, i, k, len;
	u_int64_t t, x;

	hn = 0;
	bn = 0;
	for (i = 0; i < nf; i++) {
		switch (rnd() % 6) {
		case 0:
			t = 0;
			break;
		case 1:		/* small integer, width 1..8 bytes */
			t = 1 + rnd() % 6;
			if (t == 5)
				t = 6;
			len = (int)__vdbe_serial_type_len((u_int32_t)t);
			x = rnd();
			for (k = 0; k < len; k++)
				body[bn++] = (u_int8_t)(x >> (8 * k));
			break;
		case 2:		/* constants 0 and 1 */
			t = 8 + rnd() % 2;
			break;
		case 3:		/* real */
			{
				double d = (double)(int64_t)(rnd() % 2000) - 1000;
				u_int64_t bits;

				d /= 7.0;
				memcpy(&bits, &d, 8);
				t = 7;
				for (k = 7; k >= 0; k--)
					body[bn++] = (u_int8_t)(bits >> (8 * k));
			}
			break;
		case 4:		/* text, 0..6 chars from a tiny alphabet */
			len = (int)(rnd() % 7);
			t = 13 + 2 * (u_int64_t)len;
			for (k = 0; k < len; k++)
				body[bn++] = (u_int8_t)('a' + rnd() % 3);
			break;
		default:	/* blob */
			len = (int)(rnd() % 5);
			t = 12 + 2 * (u_int64_t)len;
			for (k = 0; k < len; k++)
				body[bn++] = (u_int8_t)(rnd() % 4);
			break;
		}
		hdr[hn++] = (u_int8_t)t;	/* t < 128: one-byte varint */
	}
	buf[0] = (u_int8_t)(hn + 1);
	memcpy(buf + 1, hdr, (size_t)hn);
	memcpy(buf + 1 + hn, body, (size_t)bn);
	return (1 + hn + bn);
}

static void
test_against_upstream(DBSQL *db)
{
	u_int8_t ra[640], rb[640];
	key_info_t *ki;
	unpacked_record_t *up;
	sm_dbh_t dbh;
	DB fake;
	DBT a, b;
	int i, n, na, nb, mine, theirs, nf, nlt, ngt, neq;

	nf = 3;
	nlt = ngt = neq = 0;
	ki = __key_info_alloc(db, nf, 0);
	CHECK(ki != NULL, "key_info_alloc");
	if (ki == NULL)
		return;
	for (i = 0; i < nf; i++) {
		ki->colls[i] = db->dflt_coll;
		ki->sort_flags[i] = (u_int8_t)(i == 1 ? KEYINFO_ORDER_DESC : 0);
	}
	memset(&dbh, 0, sizeof(dbh));
	dbh.key_info = ki;
	memset(&fake, 0, sizeof(fake));
	fake.app_private = &dbh;
	up = __vdbe_alloc_unpacked_record(ki);
	CHECK(up != NULL, "alloc unpacked");
	for (n = 0; n < 200000 && up != NULL; n++) {
		na = make_record(ra, nf);
		nb = make_record(rb, nf);
		memset(&a, 0, sizeof(a));
		memset(&b, 0, sizeof(b));
		a.data = ra;
		a.size = (u_int32_t)na;
		b.data = rb;
		b.size = (u_int32_t)nb;

		__vdbe_record_unpack(nb, rb, up);
		up->default_rc = 0;
		theirs = sgn(__vdbe_record_compare(na, ra, up));

		mine = sgn(__sm_rec_cmp(&fake, &a, &b));	/* packed */
		CHECK(mine == theirs, "packed vs upstream %d %d (n=%d)",
		    mine, theirs, n);
		b.app_data = up;				/* probe */
		mine = sgn(__sm_rec_cmp(&fake, &a, &b));
		CHECK(mine == theirs, "probe vs upstream %d %d", mine, theirs);
		b.app_data = NULL;
		a.app_data = up;
		/* a carries the probe of rb; compares to b = ra */
		b.data = ra;
		b.size = (u_int32_t)na;
		mine = sgn(__sm_rec_cmp(&fake, &a, &b));
		CHECK(mine == -theirs, "reverse probe %d %d", mine, theirs);
		if (failures > 20)
			break;
		CHECK(__sm_rec_valid(ra, (u_int32_t)na, NULL),
		    "generated record is valid");
		nlt += theirs < 0;
		ngt += theirs > 0;
		neq += theirs == 0;
	}
	printf("upstream comparisons: %d (lt=%d eq=%d gt=%d)\n", n, nlt,
	    neq, ngt);
	CHECK(nlt > 1000 && ngt > 1000 && neq > 10, "comparison mix");
	__dbsql_db_free(db, up);
	__key_info_unref(ki);
}

static void
test_garbage(void)
{
	u_int8_t *pa, *pb;
	sm_dbh_t dbh;
	DB fake;
	DBT a, b;
	int n, k, la, lb, ab, ba, nvalid = 0;

	memset(&dbh, 0, sizeof(dbh));
	memset(&fake, 0, sizeof(fake));
	fake.app_private = &dbh;
	for (n = 0; n < 1000000; n++) {
		la = (int)(rnd() % 24);
		lb = (int)(rnd() % 24);
		/* Exact-size heap blocks: ASan flags any read past the end. */
		pa = malloc((size_t)(la ? la : 1));
		pb = malloc((size_t)(lb ? lb : 1));
		for (k = 0; k < la; k++)
			pa[k] = (u_int8_t)rnd();
		for (k = 0; k < lb; k++)
			pb[k] = (u_int8_t)rnd();
		if (la > 0 && rnd() % 2)
			pa[0] = (u_int8_t)(1 + rnd() % (u_int64_t)la);
		memset(&a, 0, sizeof(a));
		memset(&b, 0, sizeof(b));
		a.data = pa;
		a.size = (u_int32_t)la;
		b.data = pb;
		b.size = (u_int32_t)lb;
		ab = sgn(__sm_rec_cmp(&fake, &a, &b));
		ba = sgn(__sm_rec_cmp(&fake, &b, &a));
		CHECK(ab == -ba, "antisymmetry on garbage %d %d", ab, ba);
		nvalid += __sm_rec_valid(a.data, a.size, NULL);
		free(pa);
		free(pb);
		if (failures > 20)
			break;
	}
	printf("garbage comparisons: %d (valid-looking: %d)\n", n, nvalid);
}

int
main(void)
{
	DBSQL *db;

	if (dbsql_open(":memory:", &db) != DBSQL_OK) {
		printf("FAIL open\n");
		return (1);
	}
	test_rowid_keys();
	dbsql_mutex_enter(db->mutex);	/* internal allocators assert it */
	test_against_upstream(db);
	dbsql_mutex_leave(db->mutex);
	test_garbage();
	dbsql_close(db);
	printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
	return (failures != 0);
}
