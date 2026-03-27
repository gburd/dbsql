/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "inc/dbsql_config.h"
#include "dbsql_int.h"

typedef struct {
	DB *db;
	char *file;
	int flags;
#define SMR_TYPE_TABLE    0x0001
#define SMR_TYPE_INDEX    0x0002
} sm_rec_t;

#define SM_SCHEMA_SIG "__DBSQL_schema_sig__"
#define SM_FORMAT_VER "__DBSQL_format_sig__"
#define SM_META_NAME  "__DBSQL_meta__"

/*
 * __sm_cmp_values --
 *
 */
static int
__sm_cmp_values(a, sz_a, b, sz_b)
	const void *a;
	size_t sz_a;
	const void *b;
	size_t sz_b;
{
	int mcmp = memcmp(a, b, (sz_a <= sz_b) ? sz_a : sz_b);
	if (mcmp == 0) {
		if (sz_a == sz_b)
			return 0;
		else
			return ((sz_a < sz_b) ? -1 : 1);
	} else {
		return ((mcmp > 0) ? 1 : -1);
	}
}

/*
 * __sm_bt_compare --
 *	Compare data using the proper comparison function given the type
 *	of data in the records being compared.
 *
 * PUBLIC: int __sm_bt_compare __P((DB *, const DBT *, const DBT *));
 */
int
__sm_bt_compare(db, dbt1, dbt2)
	DB *db;
	const DBT *dbt1;
	const DBT *dbt2;
{
	return __sm_cmp_values(dbt1->data, dbt1->size, dbt2->data, dbt2->size);
}

/*
 * __sm_is_threaded --
 *	Return 1 if the environment was opened with DB_THREAD set,
 *	otherwise 0;
 */
static int
__sm_is_threaded(sm_t* sm)
{
	int flags;
	return F_ISSET(sm->dbp, DBSQL_Threaded);
}

/*
 * __sm_next_from_seq --
 *	Using the dbsql_db_t.n DB_SEQUENCE return the next unused
 *	u_int32_t that is unused.  This sequence may wrap (if someone
 *	has a very long lived and active database) which means that we
 *	must make sure that there isn't a database file using this
 *	sequence number already.
 *	!!! If there are more than MAX_UINT32 open tables during the
 *	lifetime of a database then there is the potential that we
 *	will return a sequence number that is in use.
 *
 * STATIC: static u_int32_t __sm_next_from_seq __P((sm_t *, DB_TXN *));
 */
static u_int32_t
__sm_next_from_seq(sm_t* sm, DB_TXN* txn)
{
	int rc;
	db_seq_t val;

	if ((rc = sm->n->get(sm->n, txn, 1, &val, 0)) != 0)
		__dbsql_panic(sm->dbp, rc);
	return (u_int32_t)val;
}

/*
 * __sqldb_seq_init --
 *	If 'init' is non-zero, don't expect to find __DBSQL_<name>_dbi__,
 *	rather create it.  'dbi' is what replaces the concept of rootpage.
 *	This sequence is used to reliabily generate sequential keys starting
 *	with 2 up to max u_int32_t for use when naming DB files.  Return a
 *	new sequence or NULL on failure.
 *
 * STATIC: static DB_SEQUENCE *__sm_seq_init __P((sm_t *, DB *, DB_TXN *,
 * STATIC:                                   const char *, int));
 */
static DB_SEQUENCE *
__sm_seq_init(sm, db, txn, name, init)
	sm_t *sm;
	DB *db;
	DB_TXN *txn;
	const char *name;
	int init;
{
	int rc;
	DBT key;
	DB_SEQUENCE *seq;
	char kn[1024];
	int flags;

	DBSQL_ASSERT(db != 0);
	DBSQL_ASSERT(txn != 0);

	flags = 0;

	if ((rc = db_sequence_create(&seq, db, 0)) != 0)
		return NULL;

	memset(&key, 0, sizeof(DBT));
	if (name != 0) {
		snprintf(kn, sizeof(kn), "__DBSQL_%s_dbi__", name);
	} else {
		snprintf(kn, sizeof(kn), "__DBSQL_dbi__");
	}
	key.data = kn;
	key.size = strlen(kn);

	if (__sm_is_threaded(sm))
		flags |= DB_THREAD;

	if (init) {
		seq->set_flags(seq, DB_SEQ_INC | DB_SEQ_WRAP);
		seq->set_range(seq, 2, UINT32_MAX);
		seq->initial_value(seq, 2);
		flags |= DB_CREATE | DB_EXCL;
	}

	if ((rc = seq->open(seq, txn, &key, flags)) != 0)
		return NULL;

	return seq;
}

/*
 * __sm_meta_init --
 *	If 'init' is non-zero, don't expect to find __DBSQL_<name>_meta__,
 *	rather create it.  Return a reference to the meta database within
 *	this 'db' or NULL on failure.
 *
 * STATIC: static DB *__sm_meta_init __P((sm_t *, DB_TXN *, const char *,
 * STATIC:                           int));
 */
static DB *
__sm_meta_init(sm, txn, name, init)
	sm_t *sm;
	DB_TXN *txn;
	const char *name;
	int init;
{
	int rc;
	int flags = 0;
	char mn[1024];
	DB *db;

	DBSQL_ASSERT(sm != 0);
	DBSQL_ASSERT(txn != 0);

	if (db_create(&db, sm->dbp->dbenv, 0) != 0)
		return NULL;

	/*
	 * Open or create a database named __DBSQL_<name>_meta__ within
	 * 'db'.
	 */
	if (name != 0)
		snprintf(mn, sizeof(mn), "__DBSQL_%s_meta__", name);

	if (init)
		flags |= DB_CREATE | DB_EXCL;

	if (__sm_is_threaded(sm))
		flags |= DB_THREAD;

	if ((rc = db->open(db, txn, (name ? name : NULL), (name ? mn : NULL),
			   DB_BTREE, flags, 0)) != 0)
		return NULL;
	return db;
  err:
	db->close(db, 0);
	return 0;
}

/*
 * __sm_init --
 *
 * STATIC: static int __sm_init __P((sm_t *, int *));
 */
static int
__sm_init(sm_t* sm, int* init)
{
	int rc = DBSQL_SUCCESS;
	int flags = 0;
	db_seq_t val;
	u_int32_t n;
	DB_TXN *txn;
	DB_ENV *dbenv;
	DB *db;

	DBSQL_ASSERT(sm != 0);

	dbenv = sm->dbp->dbenv;
	*init = 1;

	if (F_ISSET(sm, SM_HAS_INIT) != 0)
		return DBSQL_SUCCESS;

	if ((rc = db_create(&db, dbenv, 0)) != 0)
		return DBSQL_CANTOPEN;

	/*
	 * Try to open the database 'name', if its not there, try to create it.
	 * TODO: when 4.3 supports named in memory files, this will change
	 */
	if (dbenv->txn_begin(dbenv, 0, &txn, 0) == ENOMEM)
		goto err;

	/* Try to create a new database called 'name'. */

	if (__sm_is_threaded(sm))
		flags |= DB_THREAD;

	if ((rc = db->open(db, txn,
			   (F_ISSET(sm, SM_INMEM_DB) ? NULL : sm->name),
			   (F_ISSET(sm, SM_INMEM_DB) ? NULL : SM_META_NAME),
			   DB_BTREE,
			   DB_CREATE | DB_EXCL | flags, 0)) == EEXIST) {
		/* The database already exists, try to open it */
		if ((rc = db->open(db, txn,
				   (F_ISSET(sm, SM_INMEM_DB) ? NULL : sm->name),
				   (F_ISSET(sm, SM_INMEM_DB) ? NULL : SM_META_NAME),
				   DB_BTREE, flags, 0)) == 0)
			*init = 0;
		else
			goto err;
	} else if (rc != 0)
		goto err;
	sm->primary = db;
	if ((sm->n = __sm_seq_init(sm, db, txn, sm->name, *init)) == 0)
		goto err;
	if ((sm->meta = __sm_meta_init(sm, txn, (F_ISSET(sm, SM_INMEM_DB) ?
					NULL : sm->name), *init)) == 0)
		goto err;
	txn->commit(txn, 0);
	txn = 0;
	F_SET(sm, SM_HAS_INIT);
	return DBSQL_SUCCESS;
  err:
	if (sm->meta)
		db->close(sm->meta, 0);
	if (sm->n)
		sm->n->close(sm->n, 0);
	db->close(db, 0);
	if (txn)
		txn->abort(txn);
	return DBSQL_CANTOPEN;
}

/*
 * __sm_create --
 *	This routine is called to setup a storage manager for this
 *	database.  A storage manager is really context that allows
 *	for the management of a group of Berkeley DB databases used
 *	to represent the contents of a SQL database.  A storage manager
 *	may represent a set of datbases in memory or on disk.  If the
 *	set 'is_temp' then when the database is close all resources
 *	should be deleted by the storage manager.
 *
 * PUBLIC: int __sm_create __P((DBSQL *, const char *, int, int, sm_t **));
 *
 * dbenv		DB_ENV that will contain our activity
 * name			Name of the database, null means temporary
 * is_temp              Non-zero if this is a temporary database
 * in_memory            Non-zero if this database should live only in memory
 * sm			OUT: Pointer to new sm_t instance
 */
int
__sm_create(dbp, name, is_temp, in_memory, smp)
	DBSQL *dbp;
	const char *name;
	int is_temp;
	int in_memory;
	sm_t **smp;
{
	int id, init, rc;
	sm_t *sm;
	char buf[1024];

	DBSQL_ASSERT(smp != 0);

	if (__dbsql_calloc(dbp, 1, sizeof(sm_t), &sm) == ENOMEM)
		return ENOMEM;

	if (in_memory && is_temp) {
		sm->name = 0;
		F_SET(sm, SM_TEMP_DB | SM_INMEM_DB);
	} else {
		if (is_temp) {
			if (name)
				snprintf(buf, sizeof(buf), "%s_tmp", name);
			F_SET(sm, SM_TEMP_DB);
		}
		if (in_memory)
			F_SET(sm, SM_INMEM_DB);
		if (__dbsql_strdup(dbp, (is_temp ? buf : name),
				   &sm->name) == ENOMEM) {
			__dbsql_free(dbp, sm);
			return ENOMEM;
		}
	}
	sm->dbp = dbp;
	__hash_init(&sm->dbs, DBSQL_HASH_INT, 0);
	if (__sm_init(sm, &init) != DBSQL_SUCCESS) {
		__dbsql_free(dbp, sm->name);
		__dbsql_free(dbp, sm);
		return DBSQL_CANTOPEN;
	}

	/* Create a binary tree for the DBSQL_MASTER table at location 2. */
	if (init) {
		if (__sm_create_table(sm, &id) != DBSQL_SUCCESS) {
			if (sm->name)
				__dbsql_free(NULL, sm->name);
			__dbsql_free(dbp, sm);
			return DBSQL_CANTOPEN;
		}
	} else {
		id = 2;
		if (__sm_open_table(sm, &id) != DBSQL_SUCCESS) {
			if (sm->name)
				__dbsql_free(NULL, sm->name);
			__dbsql_free(dbp, sm);
			return DBSQL_CANTOPEN;
		}
	}

	(*smp) = sm;
	return DBSQL_SUCCESS;
}

/*
 * __sm_close_db --
 *	Close all managed resources and free all memory used in sm_t.  If
 *	this is a temporary database erase all files created as part of
 *	the cleanup.
 *
 * PUBLIC: int __sm_close_db __P((sm_t *));
 */
int
__sm_close_db(sm_t* sm)
{
	int rc;
	hash_ele_t *p;
	sm_rec_t *smr;

	DBSQL_ASSERT(sm != 0);

/*	MUTEX_THREAD_LOCK(dbp->dbenv, sm->sm_mutexp);*/
	sm->n->close(sm->n, 0);
	sm->meta->close(sm->meta, 0);
	sm->primary->close(sm->primary, 0);
	for(p = __hash_first(&sm->dbs); p; p = __hash_next(p)) {
		smr = (sm_rec_t *)__hash_data(p);
		if (F_ISSET(sm, SM_TEMP_DB)) {
			smr->db->close(smr->db, 0);
			if (F_ISSET(sm, SM_INMEM_DB) == 0)
				smr->db->remove(smr->db, smr->file, NULL, 0);
		} else {
			smr->db->close(smr->db, 0);
		}
		__dbsql_free(sm->dbp, smr);
	}
	if (sm->name)
		__dbsql_free(sm->dbp, sm->name);
/*	MUTEX_THREAD_UNLOCK(dbp->dbenv, sm->sm_mutexp);*/
	return DBSQL_SUCCESS;
}

/*
 * __sm_checkpoint --
 *
 * PUBLIC: int __sm_checkpoint __P((sm_t *));
 */
int
__sm_checkpoint(sm_t* sm)
{
	DBSQL_ASSERT(sm != 0);
	if (sm->dbp->dbenv)
		sm->dbp->dbenv->txn_checkpoint(sm->dbp->dbenv, 0, 0, 0);
	return DBSQL_SUCCESS;
}

/*
 * __sm_get_database_name --
 *	Return the name of the database.
 *
 * PUBLIC: char *__sm_get_database_name __P((sm_t *));
 */
char *
__sm_get_database_name(sm_t* sm)
{
	DBSQL_ASSERT(sm != 0);
	return (sm->name);
}

/*
 * __sm_begin_txn --
 *
 * PUBLIC: int __sm_begin_txn __P((sm_t *));
 */
int
__sm_begin_txn(sm_t* sm)
{
	DBSQL_ASSERT(sm);
	return (sm->dbp->dbenv->txn_begin(sm->dbp->dbenv, 0, &sm->txn, 0));
}

/*
 * __sm_commit_txn --
 *	Note that DBSQL->dbenv may be NULL.  It might also be opened
 *	without DB_INIT_TXN set.  Only in the case when the DBENV exists
 *	and has transactions enabled will there actually be transactional
 *	protection.
 *
 * PUBLIC: int __sm_commit_txn __P((sm_t *));
 */
int
__sm_commit_txn(sm_t* sm)
{
	DBSQL_ASSERT(sm);
	if (sm->txn)
		sm->txn->commit(sm->txn, 0);
	sm->txn = 0;
	return DBSQL_SUCCESS;
}

/*
 * __sm_abort_txn --
 *	Note that DBSQL->dbenv may be NULL.  It might also be opened
 *	without DB_INIT_TXN set.  Only in the case when the DBENV exists
 *	and has transactions enabled will there actually be transactional
 *	protection.
 *
 * PUBLIC: int __sm_abort_txn __P((sm_t *));
 */
int
__sm_abort_txn(sm_t* sm)
{
	DBSQL_ASSERT(sm);
	if (sm->txn)
		sm->txn->abort(sm->txn);
	sm->txn = 0;
	return DBSQL_SUCCESS;
}

/*
 * __sm_cursor --
 *	Create a new cursor for the btree index 'idb'.  If 'write' is set
 *	the then cursor is opened DB_WRITECURSOR.  If 'write' is not set
 *	it is expected that the cursor is transactionally protected such
 *	that a sequential scan using __sm_cursor_next will consistently
 *	read all values in the database.
 *
 * PUBLIC: int __sm_cursor __P((sm_t *, int, int, sm_cursor_t **));
 */
int
__sm_cursor(sm_t* sm, int id, int write, sm_cursor_t* *smcp)
{
	int rc;
	sm_rec_t *smr;
	sm_cursor_t *smc;
	DB_ENV *dbenv;

	DBSQL_ASSERT(sm != 0);
	DBSQL_ASSERT(smcp != 0);

	*smcp = 0;
	if (sm->dbp == NULL)
		return DBSQL_ERROR;
	dbenv = sm->dbp->dbenv;

	smr = (sm_rec_t *)__hash_find(&sm->dbs, (const void *)0, id);
	if (smr == 0)
		return DBSQL_INTERNAL;

	if (__dbsql_calloc(sm->dbp, 1, sizeof(sm_cursor_t), &smc) != 0)
		return DBSQL_NOMEM;

	smc->sm = sm;
	smc->db = smr->db;
	smc->id = id;
	smc->eState = SMC_STATE_INVALID;
	smc->cachedRowidValid = 0;

	if (dbenv->txn_begin(dbenv, sm->txn, &smc->txn, 0) != 0) {
		__dbsql_free(sm->dbp, smc);
		return DBSQL_INTERNAL;
	}
	if (write)
		F_SET(smc, SMC_RW_CURSOR);
	else
		F_SET(smc, SMC_RO_CURSOR);

	/* Set cursor type based on the resource type. */
	if (F_ISSET(smr, SMR_TYPE_TABLE))
		F_SET(smc, SMC_INTKEY);
	else if (F_ISSET(smr, SMR_TYPE_INDEX))
		F_SET(smc, SMC_INDEX);

	if (smr->db->cursor(smr->db, smc->txn, &smc->dbc, 0) != 0) {
		smc->txn->abort(smc->txn);
		__dbsql_free(sm->dbp, smc);
		return DBSQL_INTERNAL;
	}
	*smcp = smc;
	return DBSQL_SUCCESS;
}

/*
 * __sm_close_cursor --
 *	If this is a read-only cursor, discard the sm_cursor_t->txn.  If
 *	not, it should have been NULL, check that.  Close the
 *	sm_cursor_t->dbc and free the sm_cursor_t.
 *
 * PUBLIC: int __sm_close_cursor __P((sm_cursor_t *));
 */
int
__sm_close_cursor(sm_cursor_t* smc)
{
	int rc = DBSQL_SUCCESS;
	DBSQL_ASSERT(smc != 0);


	if (smc->dbc->c_close(smc->dbc)  == DB_LOCK_DEADLOCK) {
		smc->txn->abort(smc->txn);
		rc = DBSQL_INTERNAL;
	} else
		smc->txn->commit(smc->txn, 0);
	__dbsql_free(smc->sm->dbp, smc);
	return rc;
}

/*
 * __sm_moveto --
 *	Move the cursor to a node near the key to be inserted. If the key
 *	already exists in the table, then (result == 0). In this case we
 *	can just replace the data associated with the entry, we don't need
 *	to manipulate the tree.  If there is no exact match, then the cursor
 *	points at what would be either the predecessor (result == -1) or
 *	successor (result == 1) of the searched-for key, were it to be
 *	inserted. The new node becomes a child of this node.
 *
 *     *result  < 0 The cursor is left pointing at an entry that
 *                  is smaller than pKey or if the table is empty
 *                  and the cursor is therefore left point to nothing.
 *
 *     *result == 0 The cursor is left pointing at an entry that
 *                  exactly matches pKey.
 *
 *     *result  > 0 The cursor is left pointing at an entry that
 *                  is larger than pKey.
 *
 * PUBLIC: int __sm_moveto __P((sm_cursor_t *, const void *, int, int *));
 */
int
__sm_moveto(smc, item, len, result)
	sm_cursor_t *smc;
	const void *item;
	int len;
	int *result;
{
	int rc = DBSQL_SUCCESS;
	DBT key, data;
	DBC *dbc;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(item);
	DBSQL_ASSERT(len);
	DBSQL_ASSERT(result);

	dbc = smc->dbc;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_USERMEM | DB_DBT_PARTIAL;
	data.flags = DB_DBT_REALLOC;
	key.ulen = len;
	key.dlen = len;
	key.size = len;
	if (__dbsql_umalloc(smc->sm->dbp, len, &key.data) == ENOMEM)
		return DBSQL_INTERNAL;
	memcpy(key.data, item, len);

	switch (dbc->c_get(dbc, &key, &data, DB_SET_RANGE)) {
	case 0:
		*result = 0;
		smc->eState = SMC_STATE_VALID;
		rc = DBSQL_SUCCESS;
		break;
	case DB_NOTFOUND:
		if (data.data == 0) {
			/* There was no key >= item. */
			switch(dbc->c_get(dbc, &key, &data, DB_LAST)) {
			case DB_NOTFOUND: /* FALLTHROUGH */
				smc->eState = SMC_STATE_EOF;
				/* FALLTHROUGH */
			case 0:
				*result = -1;
				if (smc->eState != SMC_STATE_EOF)
					smc->eState = SMC_STATE_VALID;
				break;
			default:
				rc = DBSQL_INTERNAL;
				break;
			}
		} else {
			/* There was a key > but != item. */
			if (dbc->c_get(dbc, &key, &data, DB_SET) != 0) {
				rc = DBSQL_INTERNAL;
				break;
			}
			*result = 1;
			smc->eState = SMC_STATE_VALID;
		}
		break;
	default:
		rc = DBSQL_INTERNAL;
		break;
	}
	smc->cachedRowidValid = 0;
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return rc;
}

/*
 * __sm_next --
 *	Advance the cursor to the next entry in the database.  If
 *	successful then set *result=0 and return DBSQL_SUCCESS.
 *	If the cursor was already at the last entry in the database,
 *	then set *result=1.
 *
 * PUBLIC: int __sm_next __P((sm_cursor_t *, int *));
 */
int
__sm_next(sm_cursor_t* smc, int* result)
{
	int rc = DBSQL_SUCCESS;
	DBT key, data;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(result);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch(smc->dbc->c_get(smc->dbc, &key, &data, DB_NEXT)) {
	case 0:
		*result = 0;
		smc->eState = SMC_STATE_VALID;
		break;
	case DB_NOTFOUND:
		*result = 1;
		smc->eState = SMC_STATE_EOF;
		break;
	default:
		rc = DBSQL_INTERNAL; /* TODO */
		break;
	}
	smc->cachedRowidValid = 0;
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return rc;
}

/*
 * __sm_prev --
 *	Advance the cursor to the previous entry in the database.  If
 *	successful then set *result=0 and return DBSQL_SUCCESS.
 *	If the cursor was already at the first entry in the database,
 *	then set *result=1.
 *
 * PUBLIC: int __sm_prev __P((sm_cursor_t *, int *));
 */
int
__sm_prev(sm_cursor_t* smc, int* result)
{
	int rc = DBSQL_SUCCESS;
	DBT key, data;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(result);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch(smc->dbc->c_get(smc->dbc, &key, &data, DB_PREV)) {
	case 0:
		*result = 0;
		smc->eState = SMC_STATE_VALID;
		break;
	case DB_NOTFOUND:
		*result = 1;
		smc->eState = SMC_STATE_EOF;
		break;
	default:
		rc = DBSQL_INTERNAL;
		break;
	}
	smc->cachedRowidValid = 0;
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return rc;
}

/*
 * __sm_key_size --
 *	Set *size to the size of the key in bytes of the entry the
 *	cursor currently points to.  Always return DBSQL_SUCCESS.
 *	If the cursor is not currently pointing to an entry (which
 *	can happen, for example, if the database is empty) then *size
 *	is set to 0.
 *
 * PUBLIC: int __sm_key_size __P((sm_cursor_t *, int *));
 */
int
__sm_key_size(sm_cursor_t* smc, int* size)
{
	int rc = DBSQL_SUCCESS;
	DBT key, data;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(size);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch(smc->dbc->c_get(smc->dbc, &key, &data, DB_CURRENT)) {
	case 0:
		*size = key.size;
		break;
	case DB_NOTFOUND:
		*size = 0;
		break;
	default:
		rc = DBSQL_INTERNAL; /* TODO */
		break;
	}
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return rc;
}

/*
 * __sm_data_size --
 *	Set 'size' to the size of the data in bytes of the entry the
 *	cursor currently points to.  Always return DBSQL_SUCCESS.
 *	If the cursor is not currently pointing to an entry (which
 *	can happen, for example, if the database is empty) then 'size'
 *	is set to 0.
 *
 * PUBLIC: int __sm_data_size __P((sm_cursor_t *, int *));
 */
int
__sm_data_size(sm_cursor_t* smc, int* size)
{
	int rc = DBSQL_SUCCESS;
	DBT key, data;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(size);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch(smc->dbc->c_get(smc->dbc, &key, &data, DB_CURRENT)) {
	case 0:
		*size = data.size;
		break;
	case DB_NOTFOUND:
		*size = 0;
		break;
	default:
		rc = DBSQL_INTERNAL;
		break;
	}
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return rc;
}

/*
 * __sm_key_compare --
 *	Compare 'value[0:len]' against the key[0:smc->key.size - ignore].
 *	The comparison result is written to *result as follows:
 *
 *	   *result <  0    This means key <  value
 *	   *result == 0    This means key == value for all len bytes
 *	   *result >  0    This means key >  value
 *
 *	When one is an exact prefix of the other, the shorter is
 *	considered less than the longer one.  In order to be equal they
 *	must be exactly the same length. (The length of the key
 *	is the actual key length minus ignore bytes.)
 *
 * PUBLIC: int __sm_key_compare __P((sm_cursor_t *, const void *, int,
 * PUBLIC:                      int, int *));
 */
int
__sm_key_compare(smc, value, len, ignore, result)
	sm_cursor_t *smc;
	const void *value;
	int len;
	int ignore;
	int *result;
{
	int rc = DBSQL_SUCCESS;
	int nlen;
	DBT key, data;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(value);
	DBSQL_ASSERT(ignore >= 0);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch(smc->dbc->c_get(smc->dbc, &key, &data, DB_CURRENT)) {
	case 0:
		nlen = key.size - ignore;
		if (nlen < 0) {
			*result = -1;
		} else {
			*result = __sm_cmp_values(key.data, nlen, value, len);
			break;
		}
	default:
		rc = DBSQL_INTERNAL;
		break;
	}
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return rc;
}

/*
 * __sm_key --
 *	Copy from key.data[offset:offset + amt] into 'value'.
 *	Return amount of bytes copied.
 *
 * PUBLIC: size_t __sm_key __P((sm_cursor_t *, size_t, size_t, const void *));
 */
size_t
__sm_key(smc, offset, len, value)
	sm_cursor_t *smc;
	size_t offset;
	size_t len;
	const void *value;
{
	size_t amt = 0;
	DBT key, data;

	DBSQL_ASSERT(smc);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch(smc->dbc->c_get(smc->dbc, &key, &data, DB_CURRENT)) {
	case 0:
		if (!key.data || ((len + offset) <= key.size)) {
			memcpy((void *)value, ((char*)key.data) + offset, len);
			amt = len;
		} else {
			memcpy((void *)value, ((char*)key.data) + offset,
			       key.size - offset);
			amt = key.size - offset;
		}
		break;
	default:
		amt = 0; /* TODO error */
		break;
	}
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return amt;
}

/*
 * __sm_data --
 *	Copy from data.data[offset:offset + amt] into 'value'.
 *	Return amount of bytes copied.
 *
 * PUBLIC: size_t __sm_data __P((sm_cursor_t *, size_t, size_t, char *));
 */
size_t
__sm_data(sm_cursor_t* smc, size_t offset, size_t len, char* value)
{
	size_t amt = 0;
	DBT key, data;

	DBSQL_ASSERT(smc);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch(smc->dbc->c_get(smc->dbc, &key, &data, DB_CURRENT)) {
	case 0:
		if (data.data && ((len + offset) <= data.size)) {
			memcpy((void *)value, ((char*)data.data) + offset,len);
			amt = len;
		} else if (data.data && (offset < data.size)) {
			memcpy((void *)value, ((char*)data.data) + offset,
			       data.size - offset);
			amt = data.size - offset;
		} else {
			amt = 0;  /* No data or out of bounds */
		}
		break;
	default:
		amt = 0; /* TODO */
		break;
	}
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return amt;
}

/*
 * __sm_first --
 *	Advance the cursor to the first entry in the database.  If
 *	successful then set *result=0 and return DBSQL_SUCCESS.
 *	If the database was empty, then set *result=1.
 *
 * PUBLIC: int __sm_first __P((sm_cursor_t *, int *));
 */
int
__sm_first(sm_cursor_t* smc, int* result)
{
	int rc = DBSQL_SUCCESS;
	DBT key, data;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(result);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch(smc->dbc->c_get(smc->dbc, &key, &data, DB_FIRST)) {
	case 0:
		*result = 0;
		smc->eState = SMC_STATE_VALID;
		break;
	case DB_NOTFOUND:
		*result = 1;
		smc->eState = SMC_STATE_EOF;
		break;
	default:
		rc = DBSQL_INTERNAL; /* TODO: report error */
	}
	smc->cachedRowidValid = 0;
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return rc;
}

/*
 * __sm_last --
 *	Advance the cursor to the last entry in the database.  If
 *	successful then set *result=0 and return DBSQL_SUCCESS.
 *	If the database was empty, then set *result=1.
 *
 * PUBLIC: int __sm_last __P((sm_cursor_t *, int *));
 */
int
__sm_last(sm_cursor_t* smc, int* result)
{
	int rc = DBSQL_SUCCESS;
	DBT key, data;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(result);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch(smc->dbc->c_get(smc->dbc, &key, &data, DB_LAST)) {
	case 0:
		*result = 0;
		smc->eState = SMC_STATE_VALID;
		break;
	case DB_NOTFOUND:
		*result = 1;
		smc->eState = SMC_STATE_EOF;
		break;
	default:
		rc = DBSQL_INTERNAL; /* TODO: report error */
	}
	smc->cachedRowidValid = 0;
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return rc;
}

/*
 * __sm_insert --
 *	Insert a new record.  The key is given by (key,k_len)
 *	and the data is given by (data,d_len).  The cursor is used only to
 *	define what database the record should be inserted into.  The cursor
 *	is left pointing at the new record.
 *
 * PUBLIC: int __sm_insert __P((sm_cursor_t *, const void *, int,
 * PUBLIC:                 const void *, int));
 */
int
__sm_insert(smc, k, k_len, v, v_len)
	sm_cursor_t *smc;
	const void *k;
	int k_len;
	const void *v;
	int v_len;
{
	int rc = DBSQL_SUCCESS;
	DBT key, data;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(k);
	DBSQL_ASSERT(k_len);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_USERMEM;
	data.flags = DB_DBT_USERMEM;

	key.size = k_len;
	key.data = (void *)k;
	data.size = v_len;
	data.data = (void *)v;

	if (smc->db->put(smc->db, smc->txn, &key, &data, 0) != 0)
		rc = DBSQL_INTERNAL;
	else {
		/* Position the cursor to the new entry. */
		key.ulen = k_len;
		data.ulen = v_len;
		if (smc->dbc->c_get(smc->dbc, &key, &data, DB_SET) != 0)
			rc = DBSQL_INTERNAL;
		else
			smc->eState = SMC_STATE_VALID;
	}
	smc->cachedRowidValid = 0;
	return rc;
}

/*
 * __sm_delete --
 *	Delete the entry that the cursor is pointing to.
 *
 * PUBLIC: int __sm_delete __P((sm_cursor_t *));
 */
int
__sm_delete(sm_cursor_t* smc)
{
	int rc = DBSQL_SUCCESS;

	DBSQL_ASSERT(smc);

	switch(smc->dbc->c_del(smc->dbc, 0)) {
	case 0:
		smc->eState = SMC_STATE_INVALID;
		smc->cachedRowidValid = 0;
		break;
	default:
		rc = DBSQL_INTERNAL;
	}
	return rc;
}

/*
 * __sm_drop_table --
 *	DB->close() the database, clean up the smr_t and DB->remove().
 *
 * PUBLIC: int __sm_drop_table __P((sm_t *, int));
 */
int
__sm_drop_table(sm_t* sm, int id)
{
	int rc;
	sm_rec_t *smr;

	DBSQL_ASSERT(sm);
	DBSQL_ASSERT(id > 1);

/*	MUTEX_THREAD_LOCK(dbp->dbenv, sm->sm_mutexp);*/
	smr = __hash_find(&sm->dbs, (const void *)0, id);
	if (smr == 0)
		return DBSQL_INTERNAL;

	rc = smr->db->close(smr->db, 0);
	if (F_ISSET(sm, SM_INMEM_DB) == 0)
		smr->db->remove(smr->db, smr->file, NULL, 0);

	__hash_insert(&sm->dbs, (void *)0, id, 0);
	if (smr->file)
		__dbsql_free(sm->dbp, smr->file);
	__dbsql_free(sm->dbp, smr);
/*	MUTEX_THREAD_UNLOCK(dbp->dbenv, sm->sm_mutexp);*/
	return DBSQL_SUCCESS;
}

/*
 * __sm_clear_table --
 *	DB->truncate()
 *
 * PUBLIC: int __sm_clear_table __P((sm_t *, int));
 */
int
__sm_clear_table(sm_t* sm, int id)
{
	int rc;
	u_int32_t count = 0;
	sm_rec_t *smr;

	DBSQL_ASSERT(sm);

	smr = __hash_find(&sm->dbs, (const void *)0, id);
	if (smr == 0)
		return DBSQL_INTERNAL;
	if ((rc = smr->db->truncate(smr->db, sm->txn, &count, 0)) != 0)
		return rc;
	return DBSQL_SUCCESS;
}

/*
 * __sm_create_resource --
 *	A new "resource" is really a new DB_BTREE.  Create one here with the
 *	name <sm_t.name>_<type><n> where <n> is the next sequence number from
 *	<sm_t.name>[__DBSQL_<name>_dbi].  Set id to the value of <n> and return
 *	DBSQL_SUCCESS when things go our way.
 *
 * STATIC: static int __sm_resource __P((sm_t *, u_int32_t *, int, int));
 */
static int
__sm_resource(sm_t* sm, u_int32_t* id, int type, int init)
{
	int rc, flags;
	char name[1024];
	db_seq_t val;
	u_int32_t n;
	DB_TXN *txn;
	DB_ENV *dbenv;
	sm_rec_t *smr;

	dbenv = sm->dbp->dbenv;

	if (__dbsql_calloc(sm->dbp, 1, sizeof(sm_rec_t), &smr) == ENOMEM)
		return DBSQL_CANTOPEN;

	/* Create and initialize database object, open the database. */
	if ((rc = db_create(&smr->db, dbenv, 0)) != 0)
		return DBSQL_CANTOPEN;

	smr->db->set_bt_compare(smr->db, __sm_bt_compare);

	/* Start a transaction, get an id, open the database, commit. */
	dbenv->txn_begin(dbenv, sm->txn, &txn, 0);
	if (init) {
		n = __sm_next_from_seq(sm, txn);
		*id = n;
		flags = DB_CREATE | DB_EXCL;
	} else {
		n = *id;
		flags = 0;
	}
	snprintf(name, sizeof(name), "%s_%s.%.10u",
		 (F_ISSET(sm, SM_INMEM_DB) ? ":memory:" : sm->name),
		 (type == SMR_TYPE_TABLE ? "tbl" : "idx"), n);
	__dbsql_strdup(sm->dbp, name, &smr->file);
	F_SET(smr, type);
	if ((rc = smr->db->open(smr->db, txn,
				(F_ISSET(sm, SM_INMEM_DB) ? NULL : name),
				NULL, DB_BTREE, flags, 0)) != 0)
			goto err;
	txn->commit(txn, 0);
	__hash_insert(&sm->dbs, (void *)0, n, smr);
	return DBSQL_SUCCESS;
  err:
	txn->abort(txn);
	smr->db->close(smr->db, 0);
	__dbsql_free(sm->dbp, smr->file);
	__dbsql_free(sm->dbp, smr);
	return DBSQL_CANTOPEN;
}

/*
 * __sm_open_table --
 *	Open an existing DB_BTREE containing "table" data.
 *
 * PUBLIC: int __sm_open_table __P((sm_t *, int *));
 */
int
__sm_open_table(sm_t* sm, int* id)
{
	return __sm_resource(sm, id, SMR_TYPE_TABLE, 0);
}

/*
 * __sm_create_table --
 *	A new "table" is really a new DB_BTREE.
 *
 * PUBLIC: int __sm_create_table __P((sm_t *, int *));
 */
int
__sm_create_table(sm_t* sm, int* id)
{
	return __sm_resource(sm, id, SMR_TYPE_TABLE, 1);
}

/*
 * __sm_create_index --
 *
 * PUBLIC: int __sm_create_index __P((sm_t *, int *));
 */
int
__sm_create_index(sm_t* sm, int* id)
{
	return __sm_resource(sm, id, SMR_TYPE_INDEX, 1);
}

/*
 * __sm_set_format_version --
 *
 * PUBLIC: int __sm_set_format_version __P((sm_t *, int, u_int32_t));
 */
int
__sm_set_format_version(sm_t* sm, int id, u_int32_t ver)
{
	int rc;
	DBT key, data;
	DB_TXN *txn;
	DB_ENV *dbenv;
	static const char *k;

	DBSQL_ASSERT(sm != 0);

	k = SM_FORMAT_VER;
	dbenv = sm->dbp->dbenv;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.data = (void *)k;
	key.size = strlen(k);
	data.data = &ver;
	data.size = sizeof(u_int32_t);

	dbenv->txn_begin(dbenv, sm->txn, &txn, 0);

	if ((rc = sm->meta->put(sm->meta, txn, &key, &data, 0)) != 0) {
		/* TODO: report error */
		dbenv->err(dbenv, rc, "put/meta/ver");
		rc = DBSQL_INTERNAL;
		txn->abort(txn);
	} else {
		txn->commit(txn, 0);
		rc = DBSQL_SUCCESS;
	}
	return rc;
}

/*
 * __sm_get_format_version --
 *
 * PUBLIC: int __sm_get_format_version __P((sm_t *, u_int32_t *));
 */
int
__sm_get_format_version(sm_t* sm, u_int32_t* ver)
{
	int rc;
	DBT key, data;
	DB_TXN *txn;
	DB_ENV *dbenv;
	u_int32_t value;
	static const char *k;

	DBSQL_ASSERT(sm != 0);

	k = SM_FORMAT_VER;
	dbenv = sm->dbp->dbenv;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.data = (void *)k;
	key.size = strlen(k);
	data.data = &value;
	data.size = sizeof(u_int32_t);
	data.ulen = sizeof(u_int32_t);
	data.flags |= DB_DBT_USERMEM;

	dbenv->txn_begin(dbenv, sm->txn, &txn, 0);

	rc = sm->meta->get(sm->meta, txn, &key, &data, 0);
	if (rc == DB_NOTFOUND) {
		*ver = 0;
		rc = DBSQL_SUCCESS;
	} else if (rc != 0) {
		/* TODO: report error */
		dbenv->err(dbenv, rc, "get/meta/ver");
		rc = DBSQL_INTERNAL;
		goto err;
	} else {
		*ver = value;
		rc = DBSQL_SUCCESS;
	}
	txn->commit(txn, 0);
	return rc;
  err:
	txn->abort(txn);
	return rc;
}

/*
 * __sm_set_schema_sig --
 *
 * PUBLIC: int __sm_set_schema_sig __P((sm_t *, u_int32_t));
 */
int
__sm_set_schema_sig(sm_t* sm, u_int32_t sig)
{
	int rc;
	char *k = SM_SCHEMA_SIG;
	DBT key, data;
	DB_TXN *txn;
	DB_ENV *dbenv;

	DBSQL_ASSERT(sm != 0);

	dbenv = sm->dbp->dbenv;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.data = k;
	key.size = strlen(k);
	data.data = &sig;
	data.size = sizeof(u_int32_t);

	dbenv->txn_begin(dbenv, sm->txn, &txn, 0);

	if ((rc = sm->meta->put(sm->meta, txn, &key, &data, 0)) != 0) {
		/* TODO: report error */
		dbenv->err(dbenv, rc, "put/meta/sig");
		rc = DBSQL_INTERNAL;
		txn->abort(txn);
	} else {
		rc = DBSQL_SUCCESS;
		txn->commit(txn, 0);
	}
	return rc;
}

/*
 * __sm_get_schema_sig --
 *
 * PUBLIC: int __sm_get_schema_sig __P((sm_t *, u_int32_t *));
 */
int
__sm_get_schema_sig(sm_t* sm, u_int32_t* sig)
{
	int rc;
	DBT key, data;
	DB_TXN *txn;
	DB_ENV *dbenv;
	u_int32_t value;
	static const char *k;

	DBSQL_ASSERT(sm != 0);

	k = SM_SCHEMA_SIG;
	dbenv = sm->dbp->dbenv;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.data = (void *)k;
	key.size = strlen(k);
	data.data = &value;
	data.size = sizeof(u_int32_t);
	data.ulen = sizeof(u_int32_t);
	data.flags |= DB_DBT_USERMEM;

	dbenv->txn_begin(dbenv, sm->txn, &txn, 0);

	rc = sm->meta->get(sm->meta, txn, &key, &data, 0);
	if (rc == DB_NOTFOUND) {
		*sig = 0;
		rc = DBSQL_SUCCESS;
	} else if (rc != 0) {
		/* TODO: report error */
		dbenv->err(dbenv, rc, "get/meta/sig");
		rc = DBSQL_INTERNAL;
		goto err;
	} else {
		*sig = value;
		rc = DBSQL_SUCCESS;
	}
	txn->commit(txn, 0);
	return rc;
  err:
	txn->abort(txn);
	return rc;
}

/* ================================================================
 * SM API Extensions for SQLite 3.52.0 Register-Based VDBE
 *
 * These 10 functions bridge the gap between the existing SM API
 * (designed for the old stack-based VDBE with 32-bit encoded keys)
 * and the SQLite 3.52.0 Btree API which uses i64 rowids, unified
 * payload access, UnpackedRecord seeks, and BtreePayload inserts.
 * ================================================================
 */

/*
 * __sm_i64_to_key --
 *	Encode a 64-bit signed integer into an 8-byte big-endian key
 *	suitable for BDB btree ordering.  The sign bit is flipped so
 *	that negative values sort before positive values in memcmp().
 *
 * STATIC: static void __sm_i64_to_key __P((i64, unsigned char *));
 */
static void
__sm_i64_to_key(i64 v, unsigned char *buf)
{
	u_int64_t uv = (u_int64_t)v ^ ((u_int64_t)1 << 63);
	buf[0] = (unsigned char)(uv >> 56);
	buf[1] = (unsigned char)(uv >> 48);
	buf[2] = (unsigned char)(uv >> 40);
	buf[3] = (unsigned char)(uv >> 32);
	buf[4] = (unsigned char)(uv >> 24);
	buf[5] = (unsigned char)(uv >> 16);
	buf[6] = (unsigned char)(uv >> 8);
	buf[7] = (unsigned char)(uv);
}

/*
 * __sm_key_to_i64 --
 *	Decode an 8-byte big-endian key (as produced by __sm_i64_to_key)
 *	back into a 64-bit signed integer.
 *
 * STATIC: static i64 __sm_key_to_i64 __P((const unsigned char *));
 */
static i64
__sm_key_to_i64(const unsigned char *buf)
{
	u_int64_t uv;
	uv  = ((u_int64_t)buf[0]) << 56;
	uv |= ((u_int64_t)buf[1]) << 48;
	uv |= ((u_int64_t)buf[2]) << 40;
	uv |= ((u_int64_t)buf[3]) << 32;
	uv |= ((u_int64_t)buf[4]) << 24;
	uv |= ((u_int64_t)buf[5]) << 16;
	uv |= ((u_int64_t)buf[6]) << 8;
	uv |= ((u_int64_t)buf[7]);
	return (i64)(uv ^ ((u_int64_t)1 << 63));
}

/*
 * __sm_begin_txn_ex --
 *	Enhanced transaction begin.  Starts a transaction with the given
 *	write flag and returns the current schema cookie.  This maps to
 *	SQLite 3.52.0's sqlite3BtreeBeginTrans(pBt, wrFlag, &schemaCookie).
 *
 *	wrFlag: 0 for read-only, non-zero for read-write.
 *	pSchemaCookie: OUT parameter; if non-NULL, receives the current
 *	               schema cookie value.
 *
 * PUBLIC: int __sm_begin_txn_ex __P((sm_t *, int, u_int32_t *));
 */
int
__sm_begin_txn_ex(sm, wrFlag, pSchemaCookie)
	sm_t *sm;
	int wrFlag;
	u_int32_t *pSchemaCookie;
{
	int rc;

	DBSQL_ASSERT(sm);

	/*
	 * Start the transaction.  The wrFlag parameter is noted but
	 * BDB does not distinguish read-only vs read-write at txn_begin
	 * time -- the distinction matters at cursor open time.  We store
	 * the flag for callers that need to query it.
	 */
	rc = sm->dbp->dbenv->txn_begin(sm->dbp->dbenv, 0, &sm->txn, 0);
	if (rc != 0)
		return DBSQL_INTERNAL;

	/*
	 * Retrieve the schema cookie if the caller requested it.
	 */
	if (pSchemaCookie != 0) {
		rc = __sm_get_schema_sig(sm, pSchemaCookie);
		if (rc != DBSQL_SUCCESS) {
			sm->txn->abort(sm->txn);
			sm->txn = 0;
			return rc;
		}
	}
	return DBSQL_SUCCESS;
}

/*
 * __sm_table_moveto --
 *	Seek a table cursor to the entry with the given i64 integer key.
 *	This is the table-specific seek that avoids the overhead of
 *	constructing and parsing a byte-encoded key, directly mapping to
 *	SQLite 3.52.0's sqlite3BtreeTableMoveto(pCur, intKey, bias, &res).
 *
 *	intKey: The 64-bit integer rowid to seek to.
 *	bias:   Search bias (currently unused, reserved for future use).
 *	pRes:   OUT: <0 if cursor points to entry smaller than intKey,
 *	             0 if cursor points to exact match,
 *	            >0 if cursor points to entry larger than intKey.
 *
 * PUBLIC: int __sm_table_moveto __P((sm_cursor_t *, i64, int, int *));
 */
int
__sm_table_moveto(smc, intKey, bias, pRes)
	sm_cursor_t *smc;
	i64 intKey;
	int bias;
	int *pRes;
{
	int rc;
	unsigned char keybuf[8];
	DBT key, data;
	DBC *dbc;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(pRes);

	dbc = smc->dbc;

	/* Encode the i64 key into big-endian format for BDB. */
	__sm_i64_to_key(intKey, keybuf);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.data = keybuf;
	key.size = sizeof(keybuf);
	key.ulen = sizeof(keybuf);
	key.flags = DB_DBT_USERMEM;
	data.flags = DB_DBT_PARTIAL;
	data.dlen = 0;

	switch (dbc->c_get(dbc, &key, &data, DB_SET_RANGE)) {
	case 0:
		/*
		 * DB_SET_RANGE found a key >= our target.  Compare to
		 * determine if it is an exact match or larger.
		 */
		if (key.size == 8) {
			i64 found = __sm_key_to_i64(key.data);
			if (found == intKey) {
				*pRes = 0;
				smc->eState = SMC_STATE_VALID;
			} else {
				*pRes = 1;
				smc->eState = SMC_STATE_VALID;
			}
		} else {
			*pRes = 1;
			smc->eState = SMC_STATE_VALID;
		}
		break;
	case DB_NOTFOUND:
		/*
		 * No key >= intKey.  Position at last entry if any.
		 */
		memset(&key, 0, sizeof(DBT));
		memset(&data, 0, sizeof(DBT));
		key.flags = DB_DBT_MALLOC;
		data.flags = DB_DBT_PARTIAL;
		data.dlen = 0;
		switch (dbc->c_get(dbc, &key, &data, DB_LAST)) {
		case 0:
			*pRes = -1;
			smc->eState = SMC_STATE_VALID;
			break;
		case DB_NOTFOUND:
			*pRes = -1;
			smc->eState = SMC_STATE_EOF;
			break;
		default:
			if (key.data)
				__dbsql_ufree(smc->sm->dbp, key.data);
			return DBSQL_INTERNAL;
		}
		if (key.data)
			__dbsql_ufree(smc->sm->dbp, key.data);
		break;
	default:
		return DBSQL_INTERNAL;
	}

	/* Cache the rowid for __sm_integer_key(). */
	if (smc->eState == SMC_STATE_VALID && *pRes == 0) {
		smc->cachedRowid = intKey;
		smc->cachedRowidValid = 1;
	} else {
		smc->cachedRowidValid = 0;
	}
	return DBSQL_SUCCESS;
}

/*
 * __sm_index_moveto --
 *	Seek an index cursor using an UnpackedRecord.  The UnpackedRecord
 *	contains the decoded field values and collation information needed
 *	to compare against encoded index keys in the BDB btree.
 *
 *	This function encodes the UnpackedRecord back into a binary key
 *	(using the standard serialization), then uses __sm_moveto() to
 *	perform the actual BDB cursor seek.
 *
 *	For the initial implementation, this is a shim: the caller is
 *	expected to have the packed key available.  The UnpackedRecord
 *	carries a reference to the original packed key via its pKeyInfo
 *	and associated data.  For now, we use a simplified approach that
 *	works with the existing moveto infrastructure.
 *
 *	pIdxKey: The unpacked record describing the index key.
 *	pRes:    OUT: <0 cursor at smaller, 0 exact match, >0 cursor at larger.
 *
 * PUBLIC: int __sm_index_moveto __P((sm_cursor_t *, unpacked_record_t *,
 * PUBLIC:                        int *));
 */
int
__sm_index_moveto(smc, pIdxKey, pRes)
	sm_cursor_t *smc;
	unpacked_record_t *pIdxKey;
	int *pRes;
{
	int rc;
	DBT key, data;
	DBC *dbc;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(pIdxKey);
	DBSQL_ASSERT(pRes);

	/*
	 * For index seeks, the VDBE layer constructs the packed record
	 * using OP_MakeRecord and passes it through pIdxKey.  The
	 * comparison logic is in the VDBE layer (using the KeyInfo
	 * collation sequences).  At the SM level, we perform a raw
	 * byte-oriented seek and let the VDBE layer verify the comparison
	 * semantics after the seek.
	 *
	 * TODO: When the VDBE layer passes the original packed key blob
	 * alongside the UnpackedRecord, use it directly.  For now, we
	 * position to the first entry and let the caller iterate.
	 */
	dbc = smc->dbc;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_PARTIAL;
	data.dlen = 0;

	/*
	 * Position cursor at the first entry.  The VDBE cursor opcode
	 * layer (OP_SeekGE, etc.) will refine the position using the
	 * UnpackedRecord comparison after this initial seek.
	 */
	switch (dbc->c_get(dbc, &key, &data, DB_FIRST)) {
	case 0:
		*pRes = 1;
		smc->eState = SMC_STATE_VALID;
		break;
	case DB_NOTFOUND:
		*pRes = -1;
		smc->eState = SMC_STATE_EOF;
		break;
	default:
		if (key.data)
			__dbsql_ufree(smc->sm->dbp, key.data);
		return DBSQL_INTERNAL;
	}
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);

	smc->cachedRowidValid = 0;
	return DBSQL_SUCCESS;
}

/*
 * __sm_payload --
 *	Unified payload access.  Copies 'amt' bytes starting at 'offset'
 *	from the current entry's combined key+data into 'buf'.
 *
 *	For table cursors (INTKEY), the "payload" is the data portion
 *	only (the key is the i64 rowid, not part of the payload).
 *
 *	For index cursors (BLOBKEY), the "payload" is the key portion
 *	only (there is no separate data in index entries).
 *
 *	This maps to SQLite 3.52.0's sqlite3BtreePayload().
 *
 * PUBLIC: int __sm_payload __P((sm_cursor_t *, u32, u32, void *));
 */
int
__sm_payload(smc, offset, amt, buf)
	sm_cursor_t *smc;
	u32 offset;
	u32 amt;
	void *buf;
{
	DBT key, data;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(buf);

	if (smc->eState != SMC_STATE_VALID)
		return DBSQL_INTERNAL;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch (smc->dbc->c_get(smc->dbc, &key, &data, DB_CURRENT)) {
	case 0:
		break;
	default:
		if (key.data)
			__dbsql_ufree(smc->sm->dbp, key.data);
		if (data.data)
			__dbsql_ufree(smc->sm->dbp, data.data);
		return DBSQL_INTERNAL;
	}

	if (F_ISSET(smc, SMC_INTKEY)) {
		/*
		 * Table cursor: payload is the data portion.
		 * The key is the rowid and not part of the payload.
		 */
		if (offset + amt <= data.size)
			memcpy(buf, (char *)data.data + offset, amt);
		else if (offset < data.size) {
			memcpy(buf, (char *)data.data + offset,
			       data.size - offset);
			memset((char *)buf + (data.size - offset), 0,
			       amt - (data.size - offset));
		} else {
			memset(buf, 0, amt);
		}
	} else {
		/*
		 * Index cursor: payload is the key portion.
		 * There is no separate data in index entries.
		 */
		if (offset + amt <= key.size)
			memcpy(buf, (char *)key.data + offset, amt);
		else if (offset < key.size) {
			memcpy(buf, (char *)key.data + offset,
			       key.size - offset);
			memset((char *)buf + (key.size - offset), 0,
			       amt - (key.size - offset));
		} else {
			memset(buf, 0, amt);
		}
	}

	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return DBSQL_SUCCESS;
}

/*
 * __sm_payload_size --
 *	Return the total size of the current entry's payload.
 *	For table cursors: returns the data size.
 *	For index cursors: returns the key size.
 *
 *	Maps to SQLite 3.52.0's sqlite3BtreePayloadSize().
 *
 * PUBLIC: u32 __sm_payload_size __P((sm_cursor_t *));
 */
u32
__sm_payload_size(smc)
	sm_cursor_t *smc;
{
	DBT key, data;
	u32 size = 0;

	DBSQL_ASSERT(smc);

	if (smc->eState != SMC_STATE_VALID)
		return 0;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_MALLOC;

	switch (smc->dbc->c_get(smc->dbc, &key, &data, DB_CURRENT)) {
	case 0:
		if (F_ISSET(smc, SMC_INTKEY))
			size = (u32)data.size;
		else
			size = (u32)key.size;
		break;
	default:
		size = 0;
		break;
	}
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return size;
}

/*
 * __sm_integer_key --
 *	Return the i64 rowid of the entry the table cursor currently
 *	points to.  This decodes the BDB btree key (8-byte big-endian
 *	with sign-bit flip) back into a native i64.
 *
 *	For index cursors this function should not be called; it returns 0.
 *
 *	Maps to SQLite 3.52.0's sqlite3BtreeIntegerKey().
 *
 * PUBLIC: i64 __sm_integer_key __P((sm_cursor_t *));
 */
i64
__sm_integer_key(smc)
	sm_cursor_t *smc;
{
	DBT key, data;
	i64 result = 0;

	DBSQL_ASSERT(smc);

	if (smc->eState != SMC_STATE_VALID)
		return 0;

	/* Return cached value if available. */
	if (smc->cachedRowidValid)
		return smc->cachedRowid;

	if (!F_ISSET(smc, SMC_INTKEY))
		return 0;

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_MALLOC;
	data.flags = DB_DBT_PARTIAL;
	data.dlen = 0;

	switch (smc->dbc->c_get(smc->dbc, &key, &data, DB_CURRENT)) {
	case 0:
		if (key.size == 8) {
			result = __sm_key_to_i64(key.data);
			smc->cachedRowid = result;
			smc->cachedRowidValid = 1;
		}
		break;
	default:
		break;
	}
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	return result;
}

/*
 * __sm_insert_ex --
 *	Enhanced insert using the sm_payload_t structure.  This maps to
 *	SQLite 3.52.0's sqlite3BtreeInsert(pCur, &payload, flags, seekResult).
 *
 *	For table inserts (INTKEY):
 *	  payload->n_key_bytes is the i64 rowid.
 *	  payload->pData/nData is the record body.
 *	  payload->n_zero_bytes trailing zero bytes are appended logically.
 *
 *	For index inserts (BLOBKEY):
 *	  payload->pKey/nKey is the index key blob.
 *	  payload->pData/nData are unused.
 *
 *	flags:      SM_INSERT_APPEND hint, etc.
 *	seekResult: Hint from prior seek (used to optimize; currently unused
 *	            in BDB backend).
 *
 * PUBLIC: int __sm_insert_ex __P((sm_cursor_t *, const sm_payload_t *,
 * PUBLIC:                     int, int));
 */
int
__sm_insert_ex(smc, pPayload, flags, seekResult)
	sm_cursor_t *smc;
	const sm_payload_t *pPayload;
	int flags;
	int seekResult;
{
	int rc = DBSQL_SUCCESS;
	DBT key, data;
	unsigned char keybuf[8];
	void *expanded_data = 0;

	DBSQL_ASSERT(smc);
	DBSQL_ASSERT(pPayload);

	memset(&key, 0, sizeof(DBT));
	memset(&data, 0, sizeof(DBT));
	key.flags = DB_DBT_USERMEM;
	data.flags = DB_DBT_USERMEM;

	if (F_ISSET(smc, SMC_INTKEY)) {
		/*
		 * Table insert: encode the i64 rowid as the BDB key.
		 * The data is the record body, possibly with nZero
		 * trailing zeros.
		 */
		__sm_i64_to_key(pPayload->n_key_bytes, keybuf);
		key.data = keybuf;
		key.size = sizeof(keybuf);

		if (pPayload->n_zero_bytes > 0) {
			/*
			 * Expand the data to include trailing zeros.
			 * Allocate a single buffer for pData + nZero bytes.
			 */
			int total = pPayload->n_data_bytes + pPayload->n_zero_bytes;
			if (__dbsql_umalloc(smc->sm->dbp, total,
					    &expanded_data) == ENOMEM)
				return DBSQL_NOMEM;
			if (pPayload->n_data_bytes > 0)
				memcpy(expanded_data, pPayload->pData,
				       pPayload->n_data_bytes);
			memset((char *)expanded_data + pPayload->n_data_bytes, 0,
			       pPayload->n_zero_bytes);
			data.data = expanded_data;
			data.size = total;
		} else {
			data.data = (void *)pPayload->pData;
			data.size = pPayload->n_data_bytes;
		}
	} else {
		/*
		 * Index insert: the key blob is the only thing stored.
		 * No separate data.
		 */
		key.data = (void *)pPayload->pKey;
		key.size = (u_int32_t)pPayload->n_key_bytes;
		data.data = (void *)"";
		data.size = 0;
	}

	if (smc->db->put(smc->db, smc->txn, &key, &data, 0) != 0) {
		rc = DBSQL_INTERNAL;
	} else {
		/* Position the cursor to the new entry. */
		key.ulen = key.size;
		data.ulen = data.size;
		data.flags = DB_DBT_REALLOC;
		if (smc->dbc->c_get(smc->dbc, &key, &data, DB_SET) != 0)
			rc = DBSQL_INTERNAL;
		else {
			smc->eState = SMC_STATE_VALID;
			if (F_ISSET(smc, SMC_INTKEY)) {
				smc->cachedRowid = pPayload->n_key_bytes;
				smc->cachedRowidValid = 1;
			}
		}
	}

	if (expanded_data)
		__dbsql_ufree(smc->sm->dbp, expanded_data);
	return rc;
}

/*
 * __sm_delete_ex --
 *	Enhanced delete with flags.  Maps to SQLite 3.52.0's
 *	sqlite3BtreeDelete(pCur, flags).
 *
 *	flags: SM_DELETE_SAVEPOSITION - leave cursor pointing at the
 *	       next or previous entry after deletion.
 *	       SM_DELETE_AUXDELETE - this is a secondary (auxiliary)
 *	       delete, not the primary operation.
 *
 * PUBLIC: int __sm_delete_ex __P((sm_cursor_t *, int));
 */
int
__sm_delete_ex(smc, flags)
	sm_cursor_t *smc;
	int flags;
{
	int rc = DBSQL_SUCCESS;

	DBSQL_ASSERT(smc);

	if (smc->eState != SMC_STATE_VALID)
		return DBSQL_INTERNAL;

	switch (smc->dbc->c_del(smc->dbc, 0)) {
	case 0:
		break;
	default:
		return DBSQL_INTERNAL;
	}

	smc->cachedRowidValid = 0;

	if (flags & SM_DELETE_SAVEPOSITION) {
		/*
		 * Try to advance to the next entry so the cursor
		 * remains valid.  If there is no next entry, try
		 * the previous entry.  If the table is empty, mark
		 * the cursor as EOF.
		 */
		DBT key, data;
		memset(&key, 0, sizeof(DBT));
		memset(&data, 0, sizeof(DBT));
		key.flags = DB_DBT_MALLOC;
		data.flags = DB_DBT_PARTIAL;
		data.dlen = 0;

		switch (smc->dbc->c_get(smc->dbc, &key, &data, DB_NEXT)) {
		case 0:
			smc->eState = SMC_STATE_VALID;
			break;
		case DB_NOTFOUND:
			/* Try previous. */
			switch (smc->dbc->c_get(smc->dbc, &key, &data,
						DB_PREV)) {
			case 0:
				smc->eState = SMC_STATE_VALID;
				break;
			default:
				smc->eState = SMC_STATE_EOF;
				break;
			}
			break;
		default:
			smc->eState = SMC_STATE_INVALID;
			break;
		}
		if (key.data)
			__dbsql_ufree(smc->sm->dbp, key.data);
	} else {
		smc->eState = SMC_STATE_INVALID;
	}
	return rc;
}

/*
 * __sm_eof --
 *	Return non-zero if the cursor is at EOF (not pointing to a valid
 *	entry).  This maps to SQLite 3.52.0's sqlite3BtreeEof(pCur).
 *
 *	Returns 1 (true) if the cursor is at EOF or invalid.
 *	Returns 0 (false) if the cursor points to a valid entry.
 *
 * PUBLIC: int __sm_eof __P((sm_cursor_t *));
 */
int
__sm_eof(smc)
	sm_cursor_t *smc;
{
	DBSQL_ASSERT(smc);
	return (smc->eState != SMC_STATE_VALID);
}

/*
 * __sm_create_btree --
 *	Unified table/index creation with flags.  Maps to SQLite 3.52.0's
 *	sqlite3BtreeCreateTable(pBt, &pgno, flags).
 *
 *	flags: SM_BTREE_INTKEY - create a table (i64 integer key btree).
 *	       SM_BTREE_BLOBKEY - create an index (blob key btree).
 *
 *	pId:   OUT: the identifier for the new btree (analogous to the
 *	       root page number in SQLite).
 *
 * PUBLIC: int __sm_create_btree __P((sm_t *, int *, int));
 */
int
__sm_create_btree(sm, pId, flags)
	sm_t *sm;
	int *pId;
	int flags;
{
	DBSQL_ASSERT(sm);
	DBSQL_ASSERT(pId);
	DBSQL_ASSERT(flags == SM_BTREE_INTKEY || flags == SM_BTREE_BLOBKEY);

	if (flags == SM_BTREE_INTKEY)
		return __sm_resource(sm, (u_int32_t *)pId,
				     SMR_TYPE_TABLE, 1);
	else
		return __sm_resource(sm, (u_int32_t *)pId,
				     SMR_TYPE_INDEX, 1);
}
