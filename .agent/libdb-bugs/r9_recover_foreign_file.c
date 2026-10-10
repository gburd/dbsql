/*
 * libdb repro R9: DB_REGISTER | DB_RECOVER.  Process A has the
 * environment open and writes f.db.  Child B opens it, starts a write
 * txn on f.db and is SIGKILLed.  A closes everything, deletes f.db and
 * creates an empty (zero-length) file of the same name.  A then reopens
 * the environment: the registry shows B dead, so open runs recovery,
 * which fails on the non-libdb f.db ("BDB0004 fop_read_meta: unexpected
 * file type or format", "BDB1521 Recovery function for LSN ... failed")
 * -- and instead of returning an error, __env_attach_regions panics and
 * the process aborts ("BDB0061 PANIC: Invalid argument").
 *
 * Variants (argv[1]):
 *   keep    A leaves f.db alone                  -- control
 *   delete  A deletes f.db                       -- control (recovers)
 *   empty   A replaces f.db with an empty file   -- aborts
 *
 * Expected: DB_ENV->open returns an error, never abort().
 *
 * build: cc r9_recover_foreign_file.c -I$L/include -L$L/lib \
 *            -ldb-2026.0 -Wl,-rpath,$L/lib -o r9 && ./r9 keep && ./r9 delete && ./r9 empty
 */
#include <db.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define	FLAGS	(DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_MPOOL | \
		DB_INIT_TXN | DB_THREAD | DB_REGISTER | DB_RECOVER)

static DB_ENV *
env_open(const char *h, int *rp)
{
	DB_ENV *e;

	db_env_create(&e, 0);
	e->set_errfile(e, stderr);
	if ((*rp = e->open(e, h, FLAGS, 0644)) != 0) {
		e->close(e, 0);
		return (NULL);
	}
	return (e);
}

static DB *
db_open(DB_ENV *e)
{
	DB *d;

	db_create(&d, e, 0);
	if (d->open(d, NULL, "f.db", "t", DB_BTREE,
	    DB_CREATE | DB_AUTO_COMMIT | DB_THREAD, 0644) != 0)
		return (NULL);
	return (d);
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "empty";
	int del = strcmp(mode, "keep") != 0;
	char h[] = "/tmp/r9XXXXXX", f[64];
	DB_ENV *e;
	DB *d;
	DBT k, v;
	pid_t p;
	int r, st;

	mkdtemp(h);
	if ((e = env_open(h, &r)) == NULL || (d = db_open(e)) == NULL)
		return (printf("A open failed\n"), 2);
	memset(&k, 0, sizeof(k));
	memset(&v, 0, sizeof(v));
	k.data = "k"; k.size = 1;
	v.data = "v"; v.size = 1;
	d->put(d, NULL, &k, &v, DB_AUTO_COMMIT);
	if ((p = fork()) == 0) {			/* B */
		DB_ENV *be = env_open(h, &r);
		DB *bd = be != NULL ? db_open(be) : NULL;
		DB_TXN *t;
		if (bd != NULL) {
			be->txn_begin(be, NULL, &t, 0);
			bd->put(bd, t, &k, &v, 0);
		}
		kill(getpid(), SIGKILL);
	}
	waitpid(p, &st, 0);
	d->close(d, 0);
	e->close(e, 0);
	if (del) {
		snprintf(f, sizeof(f), "%s/f.db", h);
		unlink(f);
		if (strcmp(mode, "empty") == 0)
			fclose(fopen(f, "w"));
	}
	printf("%s: reopening the environment...\n", argv[1]);
	fflush(stdout);
	e = env_open(h, &r);
	printf("%s: reopen: %s\n", argv[1], r ? db_strerror(r) : "ok");
	if (e != NULL)
		e->close(e, 0);
	return (0);
}
