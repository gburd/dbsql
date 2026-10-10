/*
 * libdb repro R6 (candidate): with DB_REGISTER | DB_RECOVER, a process
 * that opens the environment and exits without closing it makes the
 * next opener run recovery, which rebuilds the regions underneath a
 * third, still-running process that has the environment open.  That
 * process's next read fails.
 *
 * Steps (one directory):
 *   A: open env (REGISTER|RECOVER), create db, put a record, keep open.
 *   B: (child) open env, exit(0) without close.          [dead pid]
 *   C: (child) open env  -> sees the dead pid, recovers.
 *   A: get the record.
 *
 * Control: same, but B closes the env before exiting.
 * Documented behaviour (Reference Guide, "Architecting transactional
 * data store applications"): DB_REGISTER recovery is meant to run only
 * when no live process has the environment open; a live process must
 * see DB_RUNRECOVERY, not corrupt data.
 *
 * build: cc r6_register.c -I$L/include -L$L/lib -ldb-2026.0 \
 *            -Wl,-rpath,$L/lib -o r6 && ./r6 noclose && ./r6 close
 */
#include <db.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define	FLAGS	(DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL | \
		DB_INIT_TXN | DB_THREAD | DB_REGISTER | DB_RECOVER)
#include <errno.h>
#include <signal.h>
static int hybrid;

/* is-alive for DB_FAILCHK: process granularity (DB_MUTEX_PROCESS_ONLY). */
static int
is_alive(DB_ENV *e, pid_t pid, db_threadid_t tid, u_int32_t flags)
{
	(void)e; (void)tid; (void)flags;
	return (kill(pid, 0) == 0 || errno == EPERM);
}

static DB_ENV *
env_open(const char *h)
{
	DB_ENV *e;
	int r;

	db_env_create(&e, 0);
	e->set_errfile(e, stderr);
	if (hybrid) {
		e->set_thread_count(e, 64);
		e->set_isalive(e, is_alive);
	}
	if ((r = e->open(e, h, FLAGS | (hybrid ? DB_FAILCHK : 0), 0644)) != 0) {
		printf("  pid %d env open: %s\n", (int)getpid(), db_strerror(r));
		exit(2);
	}
	return (e);
}

static void
child(const char *h, int do_close)
{
	pid_t p;
	int st;

	if ((p = fork()) == 0) {
		DB_ENV *e = env_open(h);
		if (do_close)
			e->close(e, 0);
		_exit(0);
	}
	waitpid(p, &st, 0);
}

int
main(int argc, char **argv)
{
	int do_close = argc > 1 && strcmp(argv[1], "close") == 0;
	hybrid = argc > 2 && strcmp(argv[2], "hybrid") == 0;
	char h[] = "/tmp/r6XXXXXX";
	DB_ENV *e;
	DB *d;
	DBT k, v;
	int r;

	mkdtemp(h);
	e = env_open(h);
	db_create(&d, e, 0);
	r = d->open(d, NULL, "f.db", NULL, DB_BTREE,
	    DB_CREATE | DB_AUTO_COMMIT | DB_THREAD, 0644);
	memset(&k, 0, sizeof(k));
	memset(&v, 0, sizeof(v));
	k.data = "k"; k.size = 1;
	v.data = "v"; v.size = 1;
	r = d->put(d, NULL, &k, &v, DB_AUTO_COMMIT);
	printf("%s: A put %d\n", do_close ? "close" : "noclose", r);
	child(h, do_close);		/* B */
	child(h, 1);			/* C: recovers if B's pid is dead */
	memset(&v, 0, sizeof(v));
	v.flags = DB_DBT_MALLOC;
	r = d->get(d, NULL, &k, &v, 0);
	printf("%s: A get after B,C: %d (%s)\n", do_close ? "close" : "noclose",
	    r, r ? db_strerror(r) : "ok");
	return (r != 0);
}
