/*
 * Probe libdb behaviours the dbsql storage manager design depends on.
 * Each check prints PASS/FAIL; any FAIL means STORAGE.md must change.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <db.h>

static int fails;
#define CHECK(c, msg) do { if (c) printf("PASS %s\n", msg); \
	else { printf("FAIL %s\n", msg); fails++; } } while (0)

static DBT
dbt(const char *s)
{
	DBT d;
	memset(&d, 0, sizeof(d));
	d.data = (void *)s;
	d.size = (u_int32_t)strlen(s);
	return (d);
}

int
main(int argc, char *argv[])
{
	DB_ENV *env, *penv;
	DB *db, *mem, *eph;
	DB_TXN *t1, *t2, *c1, *r;
	DBT k, v;
	DB_LOCK lk;
	int ret;
	const char *home = argc > 1 ? argv[1] : "/tmp/probe-env";
	char cmd[512];

	snprintf(cmd, sizeof(cmd), "mkdir -p %s", home);
	if (system(cmd) != 0)
		return (2);

	/* 1. MVCC + NOWAIT env opens; snapshot reader sees old value. */
	db_env_create(&env, 0);
	env->set_flags(env, DB_MULTIVERSION | DB_TXN_NOWAIT, 1);
	env->set_lk_detect(env, DB_LOCK_DEFAULT);
	ret = env->open(env, home, DB_CREATE | DB_INIT_LOCK | DB_INIT_LOG |
	    DB_INIT_MPOOL | DB_INIT_TXN | DB_THREAD | DB_REGISTER | DB_RECOVER,
	    0644);
	CHECK(ret == 0, "env open MVCC|NOWAIT|REGISTER|RECOVER");
	if (ret)
		return (1);

	db_create(&db, env, 0);
	ret = db->open(db, NULL, "probe.db", "t1", DB_BTREE,
	    DB_CREATE | DB_AUTO_COMMIT | DB_THREAD | DB_MULTIVERSION, 0644);
	CHECK(ret == 0, "open sub-database t1 in file probe.db");
	k = dbt("k"); v = dbt("old");
	ret = db->put(db, NULL, &k, &v, DB_AUTO_COMMIT);
	CHECK(ret == 0, "autocommit put");

	env->txn_begin(env, NULL, &r, DB_TXN_SNAPSHOT);
	/* libdb fixes a snapshot txn's read LSN at its first read, not at
	 * txn_begin: an SQL read txn always reads (the schema cookie) first. */
	memset(&v, 0, sizeof(v));
	v.flags = DB_DBT_MALLOC;
	ret = db->get(db, r, &k, &v, 0);
	CHECK(ret == 0, "snapshot reader first read");
	free(v.data);
	env->txn_begin(env, NULL, &t1, 0);
	v = dbt("new");
	ret = db->put(db, t1, &k, &v, 0);
	CHECK(ret == 0, "writer put while snapshot reader open");
	ret = t1->commit(t1, 0);
	CHECK(ret == 0, "writer commit");
	memset(&v, 0, sizeof(v));
	v.flags = DB_DBT_MALLOC;
	ret = db->get(db, r, &k, &v, 0);
	CHECK(ret == 0 && v.size == 3 && memcmp(v.data, "old", 3) == 0,
	    "snapshot reader still sees old value after commit");
	free(v.data);

	/* 2. Stale snapshot txn writing: what error? (BUSY_SNAPSHOT map) */
	v = dbt("from-stale");
	ret = db->put(db, r, &k, &v, 0);
	printf("INFO stale snapshot write ret=%d (%s)\n", ret,
	    ret ? db_strerror(ret) : "ok");
	CHECK(ret != 0, "stale snapshot writer is refused");
	r->abort(r);

	/* 3. Database write lock via lock_get under txn locker, NOWAIT. */
	{
		DBT obj;
		u_int32_t id1, id2;
		DB_LOCK lk2;
		obj.data = "probe.db\0wr"; obj.size = 11; obj.flags = 0;
		env->txn_begin(env, NULL, &t1, 0);
		env->txn_begin(env, NULL, &t2, 0);
		id1 = t1->id(t1); id2 = t2->id(t2);
		ret = env->lock_get(env, id1, DB_LOCK_NOWAIT, &obj,
		    DB_LOCK_WRITE, &lk);
		CHECK(ret == 0, "txn1 takes db write lock");
		ret = env->lock_get(env, id2, DB_LOCK_NOWAIT, &obj,
		    DB_LOCK_WRITE, &lk2);
		CHECK(ret == DB_LOCK_NOTGRANTED, "txn2 write lock NOTGRANTED");
		ret = t1->commit(t1, 0);
		CHECK(ret == 0, "txn1 commit releases lock (txn locker)");
		ret = env->lock_get(env, id2, DB_LOCK_NOWAIT, &obj,
		    DB_LOCK_WRITE, &lk2);
		CHECK(ret == 0, "txn2 gets write lock after txn1 commit");
		t2->abort(t2);
	}

	/* 4. Child txns: abort child undoes only child work; snapshot
	 * parent with child. */
	env->txn_begin(env, NULL, &t1, DB_TXN_SNAPSHOT);
	k = dbt("p"); v = dbt("parent");
	db->put(db, t1, &k, &v, 0);
	env->txn_begin(env, t1, &c1, DB_TXN_SNAPSHOT);
	k = dbt("c"); v = dbt("child");
	ret = db->put(db, c1, &k, &v, 0);
	CHECK(ret == 0, "child txn put");
	c1->abort(c1);
	k = dbt("c"); memset(&v, 0, sizeof(v)); v.flags = DB_DBT_MALLOC;
	ret = db->get(db, t1, &k, &v, 0);
	CHECK(ret == DB_NOTFOUND, "aborted child write is gone");
	k = dbt("p");
	ret = db->get(db, t1, &k, &v, 0);
	CHECK(ret == 0, "parent write survives child abort");
	if (ret == 0) free(v.data);
	t1->commit(t1, 0);

	/* 5. Private env, named in-memory db, transactional abort. */
	db_env_create(&penv, 0);
	penv->set_flags(penv, DB_TXN_NOWAIT, 1);
	penv->log_set_config(penv, DB_LOG_IN_MEMORY, 1);
	penv->set_lg_bsize(penv, 1024 * 1024);
	ret = penv->open(penv, NULL, DB_CREATE | DB_PRIVATE | DB_INIT_LOCK |
	    DB_INIT_LOG | DB_INIT_MPOOL | DB_INIT_TXN | DB_THREAD, 0);
	CHECK(ret == 0, "private in-memory env open");
	db_create(&mem, penv, 0);
	ret = mem->open(mem, NULL, NULL, "t1", DB_BTREE,
	    DB_CREATE | DB_AUTO_COMMIT | DB_THREAD, 0);
	CHECK(ret == 0, "named in-memory database open");
	penv->txn_begin(penv, NULL, &t1, 0);
	k = dbt("x"); v = dbt("1");
	mem->put(mem, t1, &k, &v, 0);
	t1->abort(t1);
	memset(&v, 0, sizeof(v)); v.flags = DB_DBT_MALLOC;
	ret = mem->get(mem, NULL, &k, &v, 0);
	CHECK(ret == DB_NOTFOUND, "in-memory db honours txn abort");

	/* 6. Unnamed temp db (ephemeral) with tiny cache spills to disk. */
	{
		DB_ENV *eenv;
		int i, ok = 1;
		char kb[32], vb[512];
		db_env_create(&eenv, 0);
		eenv->set_cachesize(eenv, 0, 256 * 1024, 1);
		eenv->set_tmp_dir(eenv, "/tmp");
		ret = eenv->open(eenv, NULL, DB_CREATE | DB_PRIVATE |
		    DB_INIT_MPOOL | DB_THREAD, 0);
		CHECK(ret == 0, "ephemeral env (mpool only) open");
		db_create(&eph, eenv, 0);
		ret = eph->open(eph, NULL, NULL, NULL, DB_BTREE,
		    DB_CREATE | DB_THREAD, 0);
		CHECK(ret == 0, "unnamed temp db open");
		memset(vb, 'v', sizeof(vb));
		for (i = 0; i < 20000 && ok; i++) {
			snprintf(kb, sizeof(kb), "%08d", i);
			k = dbt(kb); v.data = vb; v.size = sizeof(vb); v.flags = 0;
			if ((ret = eph->put(eph, NULL, &k, &v, 0)) != 0) {
				printf("INFO eph put %d failed: %s\n", i,
				    db_strerror(ret));
				ok = 0;
			}
		}
		CHECK(ok, "10 MB into 256 KB cache unnamed temp db (spill)");
		eph->close(eph, 0);
		eenv->close(eenv, 0);
	}

	mem->close(mem, 0);
	penv->close(penv, 0);
	db->close(db, 0);
	env->close(env, 0);
	printf("%d failures\n", fails);
	return (fails != 0);
}
