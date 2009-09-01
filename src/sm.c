/*-
 * DBSQL - A SQL database engine.
 *
 * Copyright (C) 2007-2008  The DBSQL Group, Inc. - All rights reserved.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * There are special exceptions to the terms and conditions of the GPL as it
 * is applied to this software. View the full text of the exception in file
 * LICENSE_EXCEPTIONS in the directory of this software distribution.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include "dbsql_config.h"
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
__sm_is_threaded(sm)
	sm_t *sm;
{
	int flags;
	return (sm->dbp->flags & DBSQL_Threaded ? 1 : 0);
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
__sm_next_from_seq(sm, txn)
	sm_t *sm;
	DB_TXN *txn;
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
__sm_init(sm, init)
	sm_t *sm;
	int *init;
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
		/* Try to open the databse, it already exists */
		if ((rc = db->open(db, txn, sm->name, SM_META_NAME, DB_BTREE,
				   flags, 0)) == 0) 
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
__sm_close_db(sm)
	sm_t *sm;
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
__sm_checkpoint(sm)
	sm_t *sm;
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
__sm_get_database_name(sm)
	sm_t *sm;
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
__sm_begin_txn(sm)
	sm_t *sm;
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
__sm_commit_txn(sm)
	sm_t *sm;
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
__sm_abort_txn(sm)
	sm_t *sm;
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
__sm_cursor(sm, id, write, smcp)
	sm_t *sm;
	int id;
	int write;
	sm_cursor_t **smcp;
{
	int rc;
	sm_rec_t *smr;
	sm_cursor_t *smc;
	DB_ENV *dbenv;

	DBSQL_ASSERT(sm != 0);
	DBSQL_ASSERT(smcp != 0);

	*smcp = 0;
	dbenv = sm->dbp->dbenv;

	smr = (sm_rec_t *)__hash_find(&sm->dbs, (const void *)0, id);
	if (smr == 0)
		return DBSQL_INTERNAL;

	if (__dbsql_calloc(sm->dbp, 1, sizeof(sm_cursor_t), &smc) == ENOMEM)
		return DBSQL_NOMEM;

	smc->sm = sm;
	smc->db = smr->db;
	smc->id = id;

	if (dbenv->txn_begin(dbenv, sm->txn, &smc->txn, 0) != 0) {
		__dbsql_free(sm->dbp, smc);
		return DBSQL_INTERNAL;
	}
	if (write)
		F_SET(smc, SMC_RW_CURSOR);
	else
		F_SET(smc, SMC_RO_CURSOR);

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
__sm_close_cursor(smc)
	sm_cursor_t *smc;
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
		rc = DBSQL_SUCCESS;
		break;
	case DB_NOTFOUND:
		if (data.data == 0) {
			/* There was no key >= item. */
			switch(dbc->c_get(dbc, &key, &data, DB_LAST)) {
			case DB_NOTFOUND: /* FALLTHROUGH */
			case 0:
				*result = -1;
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
		}
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
 * __sm_next --
 *	Advance the cursor to the next entry in the database.  If
 *	successful then set *result=0 and return DBSQL_SUCCESS.
 *	If the cursor was already at the last entry in the database,
 *	then set *result=1.
 *
 * PUBLIC: int __sm_next __P((sm_cursor_t *, int *));
 */
int
__sm_next(smc, result)
	sm_cursor_t *smc;
	int *result;
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
		break;
	case DB_NOTFOUND:
		*result = 1;
		break;
	default:
		rc = DBSQL_INTERNAL; /* TODO */
		break;
	}
	if (key.data)
		__dbsql_ufree(smc->sm->dbp, key.data);
	if (data.data)
		__dbsql_ufree(smc->sm->dbp, data.data);
	return DBSQL_SUCCESS;
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
__sm_prev(smc, result)
	sm_cursor_t *smc;
	int *result;
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
		break;
	case DB_NOTFOUND:
		*result = 1;
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
__sm_key_size(smc, size)
	sm_cursor_t *smc;
	int *size;
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
__sm_data_size(smc, size)
	sm_cursor_t *smc;
	int *size;
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
__sm_data(smc, offset, len, value)
	sm_cursor_t *smc;
	size_t offset;
	size_t len;
	char *value;
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
		if (!data.data || ((len + offset) <= data.size)) {
			memcpy((void *)value, ((char*)data.data) + offset,len);
			amt = len;
		} else {
			memcpy((void *)value, ((char*)data.data) + offset,
			       data.size - offset);
			amt = data.size - offset;
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
__sm_first(smc, result)
	sm_cursor_t *smc;
	int *result;
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
		break;
	case DB_NOTFOUND:
		*result = 1;
		break;
	default:
		rc = DBSQL_INTERNAL; /* TODO: report error */
	}
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
__sm_last(smc, result)
	sm_cursor_t *smc;
	int *result;
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
		break;
	case DB_NOTFOUND:
		*result = 1;
		break;
	default:
		rc = DBSQL_INTERNAL; /* TODO: report error */
	}
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
	}
	return rc;
}

/*
 * __sm_delete --
 *	Delete the entry that the cursor is pointing to.
 *
 * PUBLIC: int __sm_delete __P((sm_cursor_t *));
 */
int
__sm_delete(smc)
	sm_cursor_t *smc;
{
	int rc = DBSQL_SUCCESS;

	DBSQL_ASSERT(smc);

	switch(smc->dbc->c_del(smc->dbc, 0)) {
	case 0:
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
__sm_drop_table(sm, id)
	sm_t *sm;
	int id;
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
__sm_clear_table(sm, id)
	sm_t *sm;
	int id;
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
__sm_resource(sm, id, type, init)
	sm_t *sm;
	u_int32_t *id;
	int type;
	int init;
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
__sm_open_table(sm, id)
	sm_t *sm;
	int *id;
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
__sm_create_table(sm, id)
	sm_t *sm;
	int *id;
{
	return __sm_resource(sm, id, SMR_TYPE_TABLE, 1);
}

/*
 * __sm_create_index --
 *
 * PUBLIC: int __sm_create_index __P((sm_t *, int *));
 */
int
__sm_create_index(sm, id)
	sm_t *sm;
	int *id;
{
	return __sm_resource(sm, id, SMR_TYPE_INDEX, 1);
}

/*
 * __sm_set_format_version --
 *
 * PUBLIC: int __sm_set_format_version __P((sm_t *, int, u_int32_t));
 */
int
__sm_set_format_version(sm, id, ver)
	sm_t *sm;
	int id;
	u_int32_t ver;
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
__sm_get_format_version(sm, ver)
	sm_t *sm;
	u_int32_t *ver;
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
__sm_set_schema_sig(sm, sig)
	sm_t *sm;
	u_int32_t sig;
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
__sm_get_schema_sig(sm, sig)
	sm_t *sm;
	u_int32_t *sig;
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
