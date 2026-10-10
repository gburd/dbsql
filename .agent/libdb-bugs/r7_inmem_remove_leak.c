/*
 * libdb repro R7 (candidate): DB_ENV->dbremove of an in-memory named
 * database inside a child transaction leaks the txn event that
 * __db_inmem_remove queues (__txn_remevent), when the parent is then
 * committed or aborted.  Run under ASan/LSan or valgrind.
 *
 * Variants (argv[1]):
 *   child-commit   dbremove in a child, commit child, commit parent
 *   child-abort    dbremove in a child, abort the parent
 *   top            dbremove in the top-level txn (control)
 *
 * build: cc -g -fsanitize=address r7_inmem_remove_leak.c -I$L/include \
 *            -L$L/lib -ldb-2026.0 -Wl,-rpath,$L/lib -o r7
 *        for m in top child-commit child-abort; do ./r7 $m; done
 */
#include <db.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "child-commit";
	DB_ENV *e;
	DB_TXN *top, *child, *t;
	DB *d;
	int r;

	db_env_create(&e, 0);
	e->set_errfile(e, stderr);
	e->log_set_config(e, DB_LOG_IN_MEMORY, 1);
	e->set_lg_bsize(e, 4 * 1024 * 1024);
	r = e->open(e, NULL, DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL |
	    DB_INIT_LOCK | DB_INIT_LOG | DB_INIT_TXN, 0);
	if (r != 0)
		return (printf("env open %s\n", db_strerror(r)), 2);
	/* Create the in-memory named database and commit it. */
	e->txn_begin(e, NULL, &top, 0);
	db_create(&d, e, 0);
	r = d->open(d, top, NULL, "sub", DB_BTREE, DB_CREATE, 0);
	d->close(d, 0);
	top->commit(top, 0);
	if (r != 0)
		return (printf("db open %s\n", db_strerror(r)), 2);

	e->txn_begin(e, NULL, &top, 0);
	child = NULL;
	if (strcmp(mode, "top") != 0)
		e->txn_begin(e, top, &child, 0);
	t = child != NULL ? child : top;
	r = e->dbremove(e, t, NULL, "sub", 0);
	printf("%s: dbremove %d\n", mode, r);
	if (strcmp(mode, "child-abort") == 0)
		r = top->abort(top);
	else {
		if (child != NULL)
			child->commit(child, 0);
		r = top->commit(top, 0);
	}
	printf("%s: end %d\n", mode, r);
	e->close(e, 0);
	return (0);
}
