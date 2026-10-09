/*
 * libdb repro R4: aborting a transaction that ran DB->compact
 * (DB_FREE_SPACE) on a sub-database, after another sub-database of the
 * same file was dbremove'd in that transaction, fails an assertion /
 * panics the environment while undoing the compaction:
 *
 *   BDB0059 assert failure: ../src/lock/lock.c/2669:
 *       "old_lp->gen == old_lock->gen"
 *   __lock_change <- __db_merge_recover <- __db_dispatch
 *       <- __txn_dispatch_undo <- __txn_undo <- __txn_abort
 *
 * (--enable-diagnostic build; a release build panics in the same undo.)
 *
 * Steps: file f.db with sub-databases a (big) and b; in txn T: dbremove
 * b, then compact a with DB_FREE_SPACE (which moves a's meta page into
 * the space b freed), close a, abort T.
 *
 * Controls (argv[1]):
 *   commit     same steps, T committed                -> ok
 *   nocompact  dbremove b, no compact, abort          -> ok
 *   noremove   compact a, no dbremove, abort          -> ok
 *   full       dbremove b, compact a, abort           -> assertion
 *
 * build: cc -g r4_compact_abort.c -I$LIBDB/include -L$LIBDB/lib \
 *            -ldb-2026.0 -Wl,-rpath,$LIBDB/lib -o r4 &&
 *        for m in commit nocompact noremove full; do ./r4 $m; done
 */
#include <db.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
check(int r, const char *what)
{
	if (r != 0) {
		printf("  %s: %s\n", what, db_strerror(r));
		exit(1);
	}
}

static void
fill(DB_ENV *e, const char *sub, int n, DB **dp)
{
	DB_TXN *t;
	DB *d;
	DBT k, v;
	char buf[100];
	int i;

	memset(buf, 'x', sizeof buf);
	check(e->txn_begin(e, NULL, &t, 0), "txn_begin");
	check(db_create(&d, e, 0), "db_create");
	check(d->open(d, t, "f.db", sub, DB_BTREE, DB_CREATE, 0644), "open");
	for (i = 0; i < n; i++) {
		memset(&k, 0, sizeof k); memset(&v, 0, sizeof v);
		k.data = &i; k.size = sizeof i;
		v.data = buf; v.size = sizeof buf;
		check(d->put(d, t, &k, &v, 0), "put");
	}
	check(t->commit(t, 0), "commit");
	*dp = d;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "full";
	int do_remove = strcmp(mode, "noremove") != 0;
	int do_compact = strcmp(mode, "nocompact") != 0;
	int do_abort = strcmp(mode, "commit") != 0;
	char h[] = "/tmp/r4XXXXXX";
	DB_ENV *e;
	DB *a, *b;
	DB_TXN *t;
	int r;

	mkdtemp(h);
	db_env_create(&e, 0);
	e->set_errfile(e, stderr);
	check(e->open(e, h, DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
	    DB_INIT_MPOOL | DB_INIT_TXN, 0644), "env open");
	fill(e, "b", 4000, &b);		/* b first: low pages */
	fill(e, "a", 4000, &a);
	check(b->close(b, 0), "close b");

	check(e->txn_begin(e, NULL, &t, 0), "txn_begin T");
	if (do_remove)
		check(e->dbremove(e, t, "f.db", "b", 0), "dbremove b");
	if (do_compact)
		check(a->compact(a, t, NULL, NULL, NULL, DB_FREE_SPACE, NULL),
		    "compact a");
	check(a->close(a, 0), "close a");
	r = do_abort ? t->abort(t) : t->commit(t, 0);
	printf("%s: %-9s %s: %s\n", db_version(NULL, NULL, NULL), mode,
	    do_abort ? "abort" : "commit", r ? db_strerror(r) : "ok");
	return (r != 0);
}
