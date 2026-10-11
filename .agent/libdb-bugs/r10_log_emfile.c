/*
 * libdb repro R10: when a log write must open a log file and the open fails
 * with EMFILE/ENFILE (the process is out of file descriptors), __log_name
 * (src/log/log_put.c) treats any non-ENOENT open error as fatal and panics
 * the whole environment (-> __env_panic; in diagnostic builds, abort),
 * instead of returning the transient error so the caller can free
 * descriptors and retry.
 *
 * This repro injects the condition deterministically with
 * db_env_set_func_open: the installed open returns EMFILE for the log file
 * only (names matching "log.0*"), and behaves normally for everything else.
 * It forces a new log file to be created so a log open happens after the
 * environment is up.
 *
 * Variants (argv[1]):
 *   fdok   -- control: never inject; the archive/commit succeeds.
 *   nofd   -- inject EMFILE on the next log-file open.
 *
 * Expected after the fix: nofd returns a non-zero error (EMFILE) from the
 * operation that needed the log file, and the environment is NOT panicked
 * (a subsequent operation, once injection is turned off, still works).
 *
 * build: cc r10_log_emfile.c -I$L/include -L$L/lib -ldb-2026.0 \
 *            -Wl,-rpath,$L/lib -o r10 && ./r10 fdok && ./r10 nofd
 */
#include <db.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int inject;		/* when set, fail the next log open */
static volatile int injected;		/* how many log opens we failed */

static int
my_open(const char *name, int flags, ...)
{
	va_list ap;
	int mode;
	const char *base;

	mode = 0;
	va_start(ap, flags);
	mode = va_arg(ap, int);
	va_end(ap);

	base = strrchr(name, '/');
	base = base ? base + 1 : name;
	if (inject && strncmp(base, "log.", 4) == 0) {
		injected++;
		errno = EMFILE;
		return (-1);
	}
	return (open(name, flags, mode));
}

int
main(int argc, char **argv)
{
	int nofd = argc > 1 && strcmp(argv[1], "nofd") == 0;
	char h[] = "/tmp/r10XXXXXX", kb[16];
	static char big[4096];
	DB_ENV *e;
	DB *d;
	DBT k, v;
	int i, r, panicked;

	printf("%s\n", db_version(NULL, NULL, NULL));

	if (mkdtemp(h) == NULL)
		return (printf("mkdtemp\n"), 99);
	(void)db_env_set_func_open(my_open);

	if ((r = db_env_create(&e, 0)) != 0)
		return (printf("env_create %s\n", db_strerror(r)), 2);
	e->set_errfile(e, stderr);
	e->set_errpfx(e, argv[0]);
	(void)e->set_lg_max(e, 64 * 1024);	/* small logs */
	r = e->open(e, h, DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
	    DB_INIT_MPOOL | DB_INIT_TXN | DB_THREAD, 0644);
	if (r != 0)
		return (printf("env open %s\n", db_strerror(r)), 2);

	if ((r = db_create(&d, e, 0)) != 0)
		return (printf("db_create %s\n", db_strerror(r)), 2);
	if ((r = d->open(d, NULL, "f.db", NULL, DB_BTREE,
	    DB_CREATE | DB_AUTO_COMMIT | DB_THREAD, 0644)) != 0)
		return (printf("db open %s\n", db_strerror(r)), 2);

	memset(&k, 0, sizeof(k));
	memset(&v, 0, sizeof(v));

	/* Turn injection on just before the writes that roll the log. */
	inject = nofd;

	/*
	 * Write enough to roll past the 64K log file so the log subsystem
	 * must open a new log file.  With injection on, that open gets EMFILE.
	 */
	r = 0;
	for (i = 0; i < 200 && r == 0; i++) {
		snprintf(kb, sizeof(kb), "k%05d", i);
		k.data = kb;
		k.size = (u_int32_t)strlen(kb);
		v.data = big;
		v.size = sizeof(big);
		r = d->put(d, NULL, &k, &v, DB_AUTO_COMMIT);
	}
	printf("%s: writes stopped at i=%d r=%d (%s), log opens failed=%d\n",
	    argv[1], i, r, r ? db_strerror(r) : "ok", injected);

	/*
	 * Was the environment panicked?  Turn injection off and probe with a
	 * read.  If the env is dead, every call returns DB_RUNRECOVERY.
	 */
	inject = 0;
	panicked = 0;
	{
		DBT pk, pv;
		memset(&pk, 0, sizeof(pk));
		memset(&pv, 0, sizeof(pv));
		pk.data = "k00000";
		pk.size = 6;
		pv.flags = DB_DBT_MALLOC;
		r = d->get(d, NULL, &pk, &pv, 0);
		if (r == DB_RUNRECOVERY)
			panicked = 1;
		else if (r == 0)
			free(pv.data);
	}
	printf("%s: env after = %s\n",
	    argv[1], panicked ? "PANICKED" : "alive");

	(void)d->close(d, 0);
	(void)e->close(e, 0);

	if (nofd) {
		if (injected == 0)
			return (printf("  (log open was not forced; "
			    "inconclusive)\n"), 77);
		if (panicked)
			return (printf("  FAIL: EMFILE on log open "
			    "panicked the environment\n"), 1);
		printf("  OK: EMFILE on log open returned an error, "
		    "env survived\n");
	}
	return (0);
}
