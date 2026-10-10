/*
 * libdb repro R10: a log flush that needs to open a log file when the
 * process is out of file descriptors (EMFILE) panics the environment
 * (__log_name -> __env_panic, "BDB0061 PANIC: Too many open files")
 * instead of failing the commit with an error the application can
 * handle once descriptors are free again.
 *
 * Steps: open an environment and a database, put a record, use up every
 * free descriptor, then commit a write txn whose flush opens the log file
 * (the log handle is closed first by switching to a new log file).
 *
 * Variants (argv[1]): fdok (descriptors available) -- control; nofd.
 *
 * build: cc r10_log_emfile.c -I$L/include -L$L/lib -ldb-2026.0 \
 *            -Wl,-rpath,$L/lib -o r10 && ./r10 fdok && ./r10 nofd
 */
#include <db.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	int nofd = argc > 1 && strcmp(argv[1], "nofd") == 0;
	char h[] = "/tmp/r10XXXXXX";
	DB_ENV *e;
	DB *d;
	DB_TXN *t;
	DBT k, v;
	struct rlimit rl;
	int r, fds[4096], n = 0;

	mkdtemp(h);
	db_env_create(&e, 0);
	e->set_errfile(e, stderr);
	/* Small log files so the next commit has to open a new one. */
	e->set_lg_max(e, 64 * 1024);
	r = e->open(e, h, DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
	    DB_INIT_MPOOL | DB_INIT_TXN | DB_THREAD, 0644);
	if (r != 0)
		return (printf("env open %s\n", db_strerror(r)), 2);
	db_create(&d, e, 0);
	d->open(d, NULL, "f.db", NULL, DB_BTREE,
	    DB_CREATE | DB_AUTO_COMMIT | DB_THREAD, 0644);
	memset(&k, 0, sizeof(k));
	memset(&v, 0, sizeof(v));
	/*
	 * Fill the current log file to within a few hundred bytes of its
	 * end, so the next commit's flush must open log file 2.
	 */
	{
		static char big[1024];
		DB_LSN lsn;
		int i = 0;
		char kb[16];
		for (;;) {
			e->log_flush(e, NULL);
			memset(&lsn, 0, sizeof(lsn));
			{ DB_LOGC *lc; DBT rec; memset(&rec, 0, sizeof(rec));
			  e->log_cursor(e, &lc, 0); lc->get(lc, &lsn, &rec, DB_LAST); lc->close(lc, 0); }
			if (lsn.offset > 64 * 1024 - 1400)
				break;
			snprintf(kb, sizeof(kb), "f%d", i++);
			k.data = kb; k.size = (u_int32_t)strlen(kb);
			v.data = big; v.size = sizeof(big);
			d->put(d, NULL, &k, &v, DB_AUTO_COMMIT);
		}
		printf("log at file %u offset %u\n", lsn.file, lsn.offset);
	}
	if (nofd) {
		getrlimit(RLIMIT_NOFILE, &rl);
		rl.rlim_cur = 64;
		setrlimit(RLIMIT_NOFILE, &rl);
		while (n < 4096 && (fds[n] = open("/dev/null", O_RDONLY)) >= 0)
			n++;
		printf("nofd: %d descriptors used, errno %d\n", n, errno);
	}
	e->txn_begin(e, NULL, &t, 0);
	k.data = "k"; k.size = 1;
	{ static char next[2000]; v.data = next; v.size = sizeof(next); }
	r = d->put(d, t, &k, &v, 0);
	printf("%s: put %d\n", argv[1], r);
	fflush(stdout);
	r = t->commit(t, 0);
	printf("%s: commit %d (%s)\n", argv[1], r, r ? db_strerror(r) : "ok");
	while (n > 0)
		close(fds[--n]);
	return (0);
}
