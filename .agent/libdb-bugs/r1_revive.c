/*
 * libdb repro R1: a cursor put of a key the comparator calls equal to a
 * just-deleted item stores the DELETED item's key bytes when another
 * cursor still references that item.
 *
 * Controls: the deleting cursor closed and no other cursor open -> the
 * new bytes are stored.  Fails both with another cursor referencing the
 * item and when the deleting cursor itself does the put.  Same result in
 * a transactional environment and with no environment.
 *
 * build: cc r1_revive.c -I$LIBDB/include -L$LIBDB/lib -ldb-2026.0 \
 *            -Wl,-rpath,$LIBDB/lib -o r1 && ./r1
 */
#include <db.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int
ci(DB *d, const DBT *a, const DBT *b)
{
	size_t n = a->size < b->size ? a->size : b->size;
	int r = strncasecmp(a->data, b->data, n);
	(void)d;
	return (r ? r : (int)a->size - (int)b->size);
}

/*
 * pin: 0 = control (cursor A deletes and is closed, cursor B puts);
 *      1 = another cursor references the item while A deletes and B puts;
 *      2 = the same cursor deletes and then puts.
 */
static void
run(int pin, int txn_env)
{
	DB_ENV *e = NULL;
	DB *d;
	DB_TXN *t = NULL;
	DBC *other = NULL, *a, *b;
	DBT k, v;
	char got[8] = "";
	char h[] = "/tmp/r1XXXXXX";

	if (txn_env) {
		mkdtemp(h);
		db_env_create(&e, 0);
		e->open(e, h, DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
		    DB_INIT_MPOOL | DB_INIT_TXN, 0644);
	}
	db_create(&d, e, 0);
	d->set_bt_compare(d, ci);
	if (e) e->txn_begin(e, NULL, &t, 0);
	d->open(d, t, txn_env ? "r1.db" : NULL, NULL, DB_BTREE, DB_CREATE, 0644);

	memset(&k, 0, sizeof k); memset(&v, 0, sizeof v);
	k.data = "abc"; k.size = 3;
	d->put(d, t, &k, &v, 0);

	if (pin == 1) {			/* a second cursor sitting on "abc" */
		d->cursor(d, t, &other, 0);
		memset(&k, 0, sizeof k);
		other->get(other, &k, &v, DB_FIRST);
	}
	d->cursor(d, t, &a, 0);		/* delete "abc" through cursor A */
	memset(&k, 0, sizeof k);
	a->get(a, &k, &v, DB_FIRST);
	a->del(a, 0);
	if (pin == 2)
		b = a;			/* ... and put through A as well */
	else {
		a->close(a);
		d->cursor(d, t, &b, 0);
	}
	memset(&k, 0, sizeof k); memset(&v, 0, sizeof v);
	k.data = "ABC"; k.size = 3;	/* insert "ABC" (equal under ci) */
	b->put(b, &k, &v, DB_KEYLAST);
	b->close(b);

	d->cursor(d, t, &b, 0);
	memset(&k, 0, sizeof k);
	if (b->get(b, &k, &v, DB_FIRST) == 0 && k.size < sizeof got)
		memcpy(got, k.data, k.size);
	b->close(b);
	printf("%-30s %-6s stored key: \"%s\"  (%s)\n",
	    pin == 1 ? "another cursor on the item" : pin == 2 ?
	    "same cursor deletes and puts" : "control (deleter closed)",
	    txn_env ? "txn" : "no-env", got,
	    strcmp(got, "ABC") == 0 ? "expected" : "UNEXPECTED: old bytes");
	if (other) other->close(other);
	if (t) t->commit(t, 0);
	d->close(d, 0);
	if (e) e->close(e, 0);
}

int
main(void)
{
	printf("%s\n", db_version(NULL, NULL, NULL));
	run(0, 0); run(1, 0); run(2, 0);
	run(0, 1); run(1, 1); run(2, 1);
	return (0);
}
