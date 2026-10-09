/*
 * libdb repro R2: under DB_MULTIVERSION, a stream of small committed
 * snapshot write transactions fails with ENOMEM ("BDB4525 Unable to
 * allocate memory for transaction detail") after a few thousand commits,
 * although nothing is active: each committed writer keeps its txn detail
 * and mutex until its page copies leave the cache, and the default mutex
 * region does not grow to cover them.
 *
 * Variants (argv[1]):
 *   plain      DB_MULTIVERSION, no DB_TXN_SNAPSHOT     -- control
 *   snapshot   DB_TXN_SNAPSHOT writers                 -- fails
 *   ckpt       snapshot + txn_checkpoint every 100     -- does it help?
 *   mutexmax   snapshot + mutex_set_max(262144)        -- no effect
 *   txmax      snapshot + set_tx_max(40000)            -- documented knob
 *   nomvcc     no DB_MULTIVERSION, no snapshot         -- control
 *
 * build: cc r2_mvcc_enomem.c -I$LIBDB/include -L$LIBDB/lib -ldb-2026.0 \
 *            -Wl,-rpath,$LIBDB/lib -o r2 && for m in plain snapshot ckpt mutexmax; do ./r2 $m; done
 */
#include <db.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "snapshot";
	int snap = strcmp(mode, "plain") != 0 && strcmp(mode, "nomvcc") != 0;
	char h[] = "/tmp/r2XXXXXX";
	DB_ENV *e;
	DB *d;
	DB_TXN *t;
	DB_TXN_STAT *ts;
	DB_MUTEX_STAT *ms;
	DBT k, v;
	char buf[220];
	int i, r;

	mkdtemp(h);
	db_env_create(&e, 0);
	e->set_errfile(e, stderr);
	e->set_flags(e, DB_MULTIVERSION, 1);
	e->set_cachesize(e, 0, 32 << 20, 1);
	if (strcmp(mode, "mutexmax") == 0)
		e->mutex_set_max(e, 262144);
	if (strcmp(mode, "txmax") == 0)		/* documented sizing knob */
		e->set_tx_max(e, 40000);
	if (strcmp(mode, "nomvcc") == 0)	/* no DB_MULTIVERSION at all */
		e->set_flags(e, DB_MULTIVERSION, 0);
	if ((r = e->open(e, h, DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
	    DB_INIT_MPOOL | DB_INIT_TXN, 0644)) != 0) {
		printf("env open: %s\n", db_strerror(r));
		return (1);
	}
	db_create(&d, e, 0);
	d->set_pagesize(d, 1024);
	d->open(d, NULL, "r2.db", NULL, DB_BTREE, DB_CREATE | DB_AUTO_COMMIT |
	    (strcmp(mode, "nomvcc") == 0 ? 0 : DB_MULTIVERSION), 0644);
	memset(buf, 'x', sizeof buf);
	r = 0;
	for (i = 0; i < 20000; i++) {
		if ((r = e->txn_begin(e, NULL, &t,
		    snap ? DB_TXN_SNAPSHOT : 0)) != 0) {
			printf("%-9s txn_begin #%d: %s\n", mode, i,
			    db_strerror(r));
			break;
		}
		memset(&k, 0, sizeof k); memset(&v, 0, sizeof v);
		k.data = &i; k.size = sizeof i;
		v.data = buf; v.size = sizeof buf;
		if ((r = d->put(d, t, &k, &v, 0)) != 0) {
			printf("%-9s put #%d: %s\n", mode, i, db_strerror(r));
			t->abort(t);
			break;
		}
		if ((r = t->commit(t, DB_TXN_NOSYNC)) != 0) {
			printf("%-9s commit #%d: %s\n", mode, i,
			    db_strerror(r));
			break;
		}
		if (strcmp(mode, "ckpt") == 0 && i % 100 == 0)
			e->txn_checkpoint(e, 0, 0, DB_FORCE);
	}
	if (e->txn_stat(e, &ts, 0) == 0 && e->mutex_stat(e, &ms, 0) == 0) {
		printf("%-9s %5d/20000 commits; committed snapshot txn details"
		    " held: %u; mutexes in use %u of %u\n", mode, i,
		    ts->st_nsnapshot, ms->st_mutex_inuse, ms->st_mutex_cnt);
		free(ts); free(ms);
	}
	d->close(d, 0);
	e->close(e, 0);
	return (r != 0);
}
