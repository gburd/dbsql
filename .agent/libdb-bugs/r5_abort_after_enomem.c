/*
 * libdb repro R5: a transaction that runs the environment out of lock
 * entries gets ENOMEM from DB->put (fine), but aborting that transaction
 * then panics the environment ("log undo failed ... Cannot allocate
 * memory" / DB_RUNRECOVERY) instead of rolling it back.
 *
 * Default environment sizing (no set_lk_max_*, set_lg_regionmax,
 * set_memory_max).  One txn inserts rows until a put fails, then aborts.
 *
 * Controls (argv[1]):
 *   commitsmall  1000 rows, commit                     -> ok
 *   abortsmall   1000 rows, abort                      -> ok
 *   abortbig     insert until a put fails, then abort  -> ok
 *   abortbig-nested       same through parent/child/grandchild txns
 *   abortbig-nested-mvcc  same, DB_MULTIVERSION + DB_TXN_SNAPSHOT -> PANIC
 *   abortbig-mvcc         one txn, DB_MULTIVERSION + DB_TXN_SNAPSHOT
 *
 * build: cc -g r5_abort_after_enomem.c -I$LIBDB/include -L$LIBDB/lib \
 *            -ldb-2026.0 -Wl,-rpath,$LIBDB/lib -o r5 &&
 *        for m in commitsmall abortsmall abortbig; do ./r5 $m; done
 */
#include <db.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "abortbig";
	int big = strncmp(mode, "abortbig", 8) == 0;
	int commit = strcmp(mode, "commitsmall") == 0;
	int nested = strstr(mode, "nested") != NULL;
	int mvcc = strstr(mode, "mvcc") != NULL;
	DB_TXN *top = NULL, *stmt = NULL;
	char h[] = "/tmp/r5XXXXXX";
	DB_ENV *e;
	DB *d;
	DB_TXN *t;
	DBT k, v;
	char buf[200];
	int i, r, pr = 0;

	mkdtemp(h);
	db_env_create(&e, 0);
	e->set_errfile(e, stderr);
	if (mvcc)
		e->set_flags(e, DB_MULTIVERSION, 1);
	if ((r = e->open(e, h, DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
	    DB_INIT_MPOOL | DB_INIT_TXN, 0644)) != 0) {
		printf("env open: %s\n", db_strerror(r));
		return (1);
	}
	db_create(&d, e, 0);
	d->set_pagesize(d, 1024);	/* many pages -> many page locks */
	d->open(d, NULL, "r5.db", NULL, DB_BTREE, DB_CREATE | DB_AUTO_COMMIT |
	    (mvcc ? DB_MULTIVERSION : 0), 0644);
	memset(buf, 'x', sizeof buf);
	if (nested || mvcc) {
		if (!nested) {
			e->txn_begin(e, NULL, &t, DB_TXN_SNAPSHOT);
			goto go;
		}
		/* app txn, a savepoint-like child, a per-statement grandchild */
		e->txn_begin(e, NULL, &top, mvcc ? DB_TXN_SNAPSHOT : 0);
		e->txn_begin(e, top, &t, mvcc ? DB_TXN_SNAPSHOT : 0);
		e->txn_begin(e, t, &stmt, mvcc ? DB_TXN_SNAPSHOT : 0);
	} else
		e->txn_begin(e, NULL, &t, 0);
go:
	for (i = 0; i < (big ? 10000000 : 1000); i++) {
		memset(&k, 0, sizeof k); memset(&v, 0, sizeof v);
		k.data = &i; k.size = sizeof i;
		v.data = buf; v.size = sizeof buf;
		if ((pr = d->put(d, stmt ? stmt : t, &k, &v, 0)) != 0)
			break;
	}
	printf("  %d puts, last put: %s; now aborting the statement txn\n",
	    i, pr ? db_strerror(pr) : "ok");
	fflush(stdout);
	if (stmt != NULL) {
		/* statement failed: roll back the statement, then the txn */
		r = stmt->abort(stmt);
		printf("  statement (grandchild) abort: %s\n",
		    r ? db_strerror(r) : "ok");
	}
	r = commit ? t->commit(t, 0) : t->abort(t);
	if (top != NULL && r == 0)
		r = commit ? top->commit(top, 0) : top->abort(top);
	printf("%s: %-11s %d puts (%s), %s: %s\n", db_version(NULL, NULL,
	    NULL), mode, i, pr ? db_strerror(pr) : "all ok",
	    commit ? "commit" : "abort", r ? db_strerror(r) : "ok");
	return (r != 0);
}
