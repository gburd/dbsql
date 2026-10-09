/*
 * libdb repro R3: DB_ENV->txn_checkpoint(env, kbytes, 0, 0) with a kbytes
 * threshold that is NOT reached still costs ~200 us per call, because it
 * runs SIREAD-marker cleanup (__lock_sicleanup) before testing the
 * threshold.  An application that calls it after each commit (the usual
 * "checkpoint every N KB" idiom) pays ~40x per small write.
 *
 * Variants: none (no checkpoint call), kb=4096 (threshold never reached),
 * kb=0 (always checkpoints).  2000 one-row DB_TXN_SNAPSHOT writers.
 *
 * build: cc -O1 r3_ckpt_cost.c -I$LIBDB/include -L$LIBDB/lib -ldb-2026.0 \
 *            -Wl,-rpath,$LIBDB/lib -o r3 && ./r3
 */
#include <db.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec + t.tv_nsec / 1e9);
}

static void
run(int kb)
{
	char h[] = "/tmp/r3XXXXXX";
	DB_ENV *e;
	DB *d;
	DB_TXN *t;
	DBT k, v;
	char buf[220];
	double t0;
	int i;

	mkdtemp(h);
	db_env_create(&e, 0);
	e->set_flags(e, DB_MULTIVERSION, 1);
	e->set_tx_max(e, 100000);
	e->set_cachesize(e, 0, 32 << 20, 1);
	e->open(e, h, DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL |
	    DB_INIT_TXN, 0644);
	db_create(&d, e, 0);
	d->open(d, NULL, "r3.db", NULL, DB_BTREE,
	    DB_CREATE | DB_AUTO_COMMIT | DB_MULTIVERSION, 0644);
	memset(buf, 'x', sizeof buf);
	t0 = now();
	for (i = 0; i < 2000; i++) {
		e->txn_begin(e, NULL, &t, DB_TXN_SNAPSHOT);
		memset(&k, 0, sizeof k); memset(&v, 0, sizeof v);
		k.data = &i; k.size = sizeof i;
		v.data = buf; v.size = sizeof buf;
		d->put(d, t, &k, &v, 0);
		t->commit(t, DB_TXN_NOSYNC);
		if (kb >= 0)
			e->txn_checkpoint(e, (u_int32_t)kb, 0, 0);
	}
	printf("%-28s %7.1f us per write txn\n",
	    kb < 0 ? "no txn_checkpoint call" : kb == 0 ?
	    "txn_checkpoint(0) (always)" : "txn_checkpoint(4096) (never)",
	    (now() - t0) * 1e6 / 2000);
	d->close(d, 0);
	e->close(e, 0);
}

int
main(void)
{
	printf("%s\n", db_version(NULL, NULL, NULL));
	run(-1);
	run(4096);
	run(0);
	return (0);
}
