/*
 * libdb repro R11: DB->compact(DB_FREE_SPACE) of a sub-database, inside
 * a txn, can move the sub-database's metadata page; __db_move_metadata
 * then takes a new handle lock with a blocking __lock_get.  If another
 * DB handle on the same sub-database is open (it holds a READ handle
 * lock under its own locker), the compact waits for it forever: the
 * environment has DB_TXN_NOWAIT set and the deadlock detector is on,
 * but the wait is not a txn lock wait and nobody will release the
 * other handle (it belongs to the same thread).
 *
 * dbsql path: auto_vacuum=FULL compacts every table at commit; a txn
 * that dropped a table then compacts the others (SQLite autovacuum.test
 * 2.5.4: DROP TABLE av1 with av2, av4 and their indexes present).
 *
 * Variants (argv[1]):
 *   noremove   as below, no dbremove (metadata pages do not move)  -- control
 *   remove     dbremove s1, then compact the others, while a second
 *              handle on s2 is open                                -- hangs
 *
 * A watchdog alarm(10) reports the hang.
 *
 * build: cc r11_compact_after_remove.c -I$L/include -L$L/lib \
 *            -ldb-2026.0 -Wl,-rpath,$L/lib -o r11
 *        ./r11 noremove && ./r11 remove
 */
#include <db.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
hung(int sig)
{
	(void)sig;
	printf("HUNG: compact did not return within 10 s\n");
	fflush(stdout);
	_exit(3);
}

static DB *
sub(DB_ENV *e, DB_TXN *t, const char *name)
{
	DB *d;
	int r;

	db_create(&d, e, 0);
	if ((r = d->open(d, t, "f.db", name, DB_BTREE,
	    DB_CREATE | DB_THREAD | DB_MULTIVERSION, 0644)) != 0) {
		printf("open %s: %s\n", name, db_strerror(r));
		exit(2);
	}
	return (d);
}

int
main(int argc, char **argv)
{
	int do_remove = argc > 1 && strcmp(argv[1], "remove") == 0;
	char h[] = "/tmp/r11XXXXXX", kb[32];
	static char val[600];
	DB_ENV *e;
	DB_TXN *t, *c;
	DB *s[4];
	DBT k, v;
	int i, j, r;

	mkdtemp(h);
	db_env_create(&e, 0);
	e->set_errfile(e, stderr);
	e->set_flags(e, DB_MULTIVERSION | DB_TXN_NOWAIT, 1);
	e->set_lk_detect(e, DB_LOCK_DEFAULT);
	r = e->open(e, h, DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
	    DB_INIT_MPOOL | DB_INIT_TXN | DB_THREAD, 0644);
	if (r != 0)
		return (printf("env open %s\n", db_strerror(r)), 2);
	/* Four sub-databases with some data, committed. */
	e->txn_begin(e, NULL, &t, DB_TXN_SNAPSHOT);
	for (i = 0; i < 4; i++) {
		char name[8];
		snprintf(name, sizeof(name), "s%d", i);
		s[i] = sub(e, t, name);
		/* One record each: compaction moves no data, only metadata
		 * pages, into the lowest free page -- s1's old metadata page,
		 * whose handle lock the removing txn holds (WRITE). */
		for (j = 0; j < 1; j++) {
			snprintf(kb, sizeof(kb), "%s-%03d", name, j);
			memset(&k, 0, sizeof(k));
			memset(&v, 0, sizeof(v));
			k.data = kb; k.size = (u_int32_t)strlen(kb);
			v.data = val; v.size = sizeof(val);
			s[i]->put(s[i], t, &k, &v, 0);
		}
	}
	t->commit(t, 0);
	s[1]->close(s[1], 0);
	/* A second, independent handle on s2 (another connection's view). */
	{
		DB_TXN *ot;
		DB *other;
		e->txn_begin(e, NULL, &ot, 0);
		other = sub(e, ot, "s2");
		ot->commit(ot, 0);
		memset(&k, 0, sizeof(k));
		memset(&v, 0, sizeof(v));
		k.data = "s2-001"; k.size = 6;
		v.flags = DB_DBT_MALLOC;
		r = other->get(other, NULL, &k, &v, 0);
		printf("%s: second handle on s2 reads %d\n", argv[1], r);
		free(v.data);
	}

	signal(SIGALRM, hung);
	alarm(10);
	e->txn_begin(e, NULL, &t, DB_TXN_SNAPSHOT);
	if (do_remove) {
		e->txn_begin(e, t, &c, DB_TXN_SNAPSHOT);
		r = e->dbremove(e, c, "f.db", "s1", 0);
		printf("%s: dbremove s1 %d\n", argv[1], r);
		c->commit(c, 0);
	}
	for (i = 0; i < 4; i++) {
		if (i == 1)
			continue;
		r = s[i]->compact(s[i], t, NULL, NULL, NULL, DB_FREE_SPACE,
		    NULL);
		printf("%s: compact s%d %d\n", argv[1], i, r);
		fflush(stdout);
	}
	r = t->commit(t, 0);
	printf("%s: commit %d\n", argv[1], r);
	return (0);
}
